"""ChatGPT plan provider contract against a fake OpenAI auth + Responses server.

The fake enforces what the SIWC docs require of a client: PKCE S256, the
exact scopes/resource/loopback redirect, dynamic registration, single-use
rotating refresh tokens, store:false + stream:true, no system-role items and
none of the fields the preview route rejects. The C driver (test_chatgpt.c)
runs one scenario per process; this script plays the browser and the user.
"""
import base64
import hashlib
import http.client
import json
import os
import re
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SCOPES = "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"
RESOURCE = "https://api.openai.com/v1"
REJECTED_FIELDS = [
    "background", "conversation", "max_output_tokens", "max_tool_calls", "metadata",
    "moderation", "multi_agent", "prompt", "prompt_cache_retention", "safety_identifier",
    "temperature", "top_logprobs", "top_p", "truncation", "user", "previous_response_id",
]
SCHEMA = {"type": "object", "properties": {"answer": {"type": "string"}},
          "required": ["answer"], "additionalProperties": False}
ARGS = {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"],
        "additionalProperties": False}
ONEOF = {"oneOf": [  # asngn's action-step shape: discriminated by const fields
    {"type": "object", "properties": {"action": {"type": "string", "const": "answer"},
                                      "input": {"type": "string"}},
     "required": ["action", "input"], "additionalProperties": False},
    {"type": "object", "properties": {"action": {"type": "string", "const": "call"},
                                      "tool": {"type": "string", "const": "fs.read"},
                                      "arguments": ARGS},
     "required": ["action", "tool", "arguments"], "additionalProperties": False},
    {"type": "object", "properties": {"action": {"type": "string", "const": "call"},
                                      "tool": {"type": "string", "const": "fs.list"},
                                      "arguments": ARGS},
     "required": ["action", "tool", "arguments"], "additionalProperties": False}]}
OVERLAPPING = {"oneOf": [
    {"type": "object", "properties": {"a": {"type": "string"}}, "required": ["a"]},
    {"type": "object", "properties": {"b": {"type": "string"}}, "required": ["b"]}]}
TOOL_PARAMETERS = {"type": "object", "properties": {"path": {"type": "string"},
                   "limit": {"type": "integer"}}, "required": ["path"]}


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.issuer = ""
        self.codes = {}
        self.clients = {}
        self.refresh_valid = {}
        self.refresh_used = set()
        self.access_valid = set()
        self.rejected_once = set()
        self.refreshes = 0
        self.responses = 0
        self.authorize = []
        self.revoked = []
        self.secrets = set()
        self.grant_plan = True
        self.deny = False
        self.refresh_delay = 0.0
        self.counter = 0


S = State()


def issue(client_id, nonce, scope):
    S.counter += 1
    n = S.counter
    access = f"at-{n}-" + b64url(os.urandom(12))
    refresh = f"rt-{n}-" + b64url(os.urandom(12))
    S.access_valid.add(access)
    S.refresh_valid[refresh] = (client_id, scope)
    now = int(time.time())
    claims = {"iss": S.issuer, "aud": client_id, "sub": "user-123", "email": "user@example.com",
              "exp": now + 3600, "iat": now,
              "https://api.openai.com/auth": {"chatgpt_plan_type": "plus"}}
    if nonce:
        claims["nonce"] = nonce
    id_token = ".".join([b64url(json.dumps({"alg": "RS256", "kid": "fake"}).encode()),
                         b64url(json.dumps(claims).encode()), b64url(b"not-verified")])
    S.secrets.update([access, refresh])  # ID tokens may appear in the shown URL (id_token_hint)
    return {"access_token": access, "refresh_token": refresh, "id_token": id_token,
            "token_type": "Bearer", "expires_in": 3600, "scope": scope,
            "earliest_refresh_at": now}


def event(kind, **fields):
    fields["type"] = kind
    return fields


def usage(inp=21, out=5, reasoning=0, cached=7):
    return {"input_tokens": inp, "output_tokens": out, "total_tokens": inp + out,
            "input_tokens_details": {"cached_tokens": cached},
            "output_tokens_details": {"reasoning_tokens": reasoning}}


def text_stream(chunks, use=None, extra_output=None):
    text = "".join(chunks)
    item = {"type": "message", "id": "msg_1", "role": "assistant", "status": "completed",
            "content": [{"type": "output_text", "text": text, "annotations": []}]}
    events = [event("response.created", response={"id": "resp_1", "status": "in_progress"}),
              event("response.in_progress", response={"id": "resp_1", "status": "in_progress"}),
              event("response.output_item.added", output_index=0, item=dict(item, content=[]))]
    events += [event("response.output_text.delta", item_id="msg_1", output_index=0,
                     content_index=0, delta=c) for c in chunks]
    events += [event("response.output_text.done", item_id="msg_1", output_index=0,
                     content_index=0, text=text),
               event("response.output_item.done", output_index=0, item=item),
               event("response.completed", response={
                   "id": "resp_1", "status": "completed",
                   "output": [item] + (extra_output or []),
                   "usage": use or usage()})]
    return events


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def reply(self, status, body, headers=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def sse(self, events, delay=0.0, pause_after=None, pause=0.0, terminal=True):
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("x-request-id", "req_stream")
        self.end_headers()
        try:
            self.wfile.write(b": keep-alive\n\n")
            for i, e in enumerate(events):
                if not terminal and e["type"].startswith("response.completed"):
                    break
                self.wfile.write(f"event: {e['type']}\ndata: {json.dumps(e)}\n\n".encode())
                self.wfile.flush()
                if delay:
                    time.sleep(delay)
                if pause_after is not None and i == pause_after:
                    time.sleep(pause)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True

    def bearer(self):
        auth = self.headers.get("authorization", "")
        if not auth.startswith("Bearer "):
            return None
        token = auth[7:]
        with S.lock:
            return token if token in S.access_valid else None

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        if url.path == "/api/accounts/authorize":
            return self.serve_authorize(url.query)
        if url.path == "/.well-known/openid-configuration":
            return self.reply(200, {"issuer": S.issuer,
                                    "revocation_endpoint": S.issuer + "/oauth/revoke"})
        if url.path == "/v1/models":
            if not self.bearer():
                return self.reply(401, {"error": {"code": "invalid_api_key", "message": "no"}})
            return self.reply(200, {"models": [
                {"slug": "gpt-test", "display_name": "GPT Test", "visibility": "list"},
                {"slug": "gpt-hidden", "display_name": "Hidden", "visibility": "hide"}]})
        self.send_error(404)

    def serve_authorize(self, query):
        q = urllib.parse.parse_qs(query, keep_blank_values=True, strict_parsing=True)
        assert all(len(v) == 1 for v in q.values()), q
        q = {k: v[0] for k, v in q.items()}
        S.authorize.append(q)
        assert q["response_type"] == "code"
        assert q["scope"] == SCOPES, q["scope"]
        assert q["resource"] == RESOURCE
        assert q["code_challenge_method"] == "S256"
        assert re.fullmatch(r"[A-Za-z0-9_-]{43}", q["code_challenge"])
        assert len(q["state"]) >= 43 and len(q["nonce"]) >= 43
        assert re.fullmatch(r"urn:uuid:[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}",
                            q["ext_agent_host_id"]), q["ext_agent_host_id"]
        assert re.fullmatch(r"http://127\.0\.0\.1:[0-9]+/auth/callback", q["redirect_uri"])
        registering = q["client_id"] == "dynamic_agent_client"
        if registering:
            assert q["agent_name_hint"] == "asterism"
            assert "id_token_hint" not in q and "login_hint" not in q
        else:
            assert q["client_id"] in S.clients
            assert "agent_name_hint" not in q
        params = {"state": q["state"]}
        if S.deny:
            params.update(error="access_denied", error_description="The user declined")
        else:
            with S.lock:
                client = q["client_id"]
                if registering:
                    client = f"oaiapp_test{len(S.clients) + 1}"
                    S.clients[client] = q["ext_agent_host_id"]
                code = "code-" + b64url(os.urandom(12))
                scope = SCOPES if S.grant_plan else "email openid profile offline_access"
                S.codes[code] = {"client": client, "challenge": q["code_challenge"],
                                 "nonce": q["nonce"], "redirect": q["redirect_uri"],
                                 "scope": scope, "used": False}
            params.update(code=code, scope=scope)
            if registering:
                params["client_id"] = client
        location = q["redirect_uri"] + "?" + urllib.parse.urlencode(params)
        self.send_response(302)
        self.send_header("location", location)
        self.send_header("content-length", "0")
        self.end_headers()

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("content-length", "0")))
        url = urllib.parse.urlsplit(self.path)
        if url.path == "/api/accounts/oauth/token":
            return self.serve_token(raw)
        if url.path == "/oauth/revoke":
            form = {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode()).items()}
            assert form["token_type_hint"] == "refresh_token" and form["client_id"] in S.clients
            with S.lock:
                S.refresh_valid.pop(form["token"], None)
                S.revoked.append(form["token"])
            self.send_response(200)
            self.send_header("content-length", "0")
            self.end_headers()
            return
        if url.path == "/v1/responses":
            return self.serve_responses(raw)
        self.send_error(404)

    def serve_token(self, raw):
        assert self.headers["content-type"] == "application/x-www-form-urlencoded"
        form = urllib.parse.parse_qs(raw.decode(), strict_parsing=True)
        assert all(len(v) == 1 for v in form.values())
        form = {k: v[0] for k, v in form.items()}
        assert form["resource"] == RESOURCE
        assert "client_secret" not in form
        if form["grant_type"] == "authorization_code":
            with S.lock:
                entry = S.codes.get(form["code"])
                if not entry or entry["used"]:
                    return self.reply(400, {"error": "invalid_grant"})
                entry["used"] = True
                assert form["client_id"] == entry["client"] != "dynamic_agent_client"
                assert form["redirect_uri"] == entry["redirect"]
                verifier = form["code_verifier"]
                assert 43 <= len(verifier) <= 128
                assert b64url(hashlib.sha256(verifier.encode()).digest()) == entry["challenge"], "PKCE"
                return self.reply(200, issue(entry["client"], entry["nonce"], entry["scope"]))
        assert form["grant_type"] == "refresh_token"
        assert "scope" not in form
        time.sleep(S.refresh_delay)
        with S.lock:
            token = form["refresh_token"]
            if token in S.refresh_used:
                return self.reply(400, {"error": "invalid_grant",
                                        "error_description": "refresh_token_reused"})
            if token not in S.refresh_valid:
                return self.reply(400, {"error": "invalid_grant",
                                        "error_description": "unknown refresh token"})
            client, scope = S.refresh_valid.pop(token)
            assert form["client_id"] == client
            S.refresh_used.add(token)
            S.refreshes += 1
            return self.reply(200, issue(client, None, scope))

    def serve_responses(self, raw):
        assert b"host-only-correlation" not in raw
        body = json.loads(raw)
        with S.lock:
            S.responses += 1
        token = self.bearer()
        model = body["model"]
        if model == "gpt-401-once" and token and not S.rejected_once:
            with S.lock:  # the access token expired server-side: reject it once
                S.rejected_once.add(token)
                S.access_valid.discard(token)
            token = None
        if model == "gpt-401-always" or not token:
            return self.reply(401, {"error": {"code": "invalid_api_key",
                                              "message": "token expired"}},
                              {"x-request-id": "req_401"})
        assert body["store"] is False and body["stream"] is True
        for field in REJECTED_FIELDS:
            assert field not in body, field
        assert self.headers["accept"] == "text/event-stream"
        items = body["input"]
        assert all(item.get("role") != "system" for item in items), items
        assert body["instructions"] == "You are terse."
        assert items[0] == {"role": "developer", "content": "Prefer JSON."}
        assert items[1] == {"role": "user", "content": "Say hello with a star ★"}
        reasoning = body.get("reasoning")
        if model == "gpt-text":
            assert "text" not in body and "tools" not in body and reasoning is None
            return self.sse(text_stream(["Ciao", " \U0001f31f"],
                                        usage(21, 5, reasoning=2, cached=7)))
        if model in ("gpt-reasoning-off", "gpt-reasoning-violation"):
            assert reasoning == {"effort": "none"}
            spent = 12 if model == "gpt-reasoning-violation" else 0
            return self.sse(text_stream(["ok"], usage(9, 1, reasoning=spent, cached=0)))
        if model == "gpt-schema":
            assert body["text"] == {"format": {"type": "json_schema", "name": "asmodel_output",
                                               "strict": True, "schema": SCHEMA}}
            return self.sse(text_stream(['{"answer":', '"hello"}']))
        if model == "gpt-oneof":
            assert body["text"]["format"]["schema"] == {
                "type": "object", "properties": {"value": {"anyOf": ONEOF["oneOf"]}},
                "required": ["value"], "additionalProperties": False}, body["text"]
            return self.sse(text_stream(['{"value":{"action":"call","tool":"fs.read",',
                                         '"arguments":{"path":"a.c"}}}']))
        if model == "gpt-badschema":
            return self.reply(400, {"error": {
                "type": "invalid_request_error", "code": "invalid_json_schema",
                "param": "text.format.schema", "message": "Invalid schema for response_format"}})
        if model == "gpt-tools":
            assert body["tools"] == [{"type": "function", "name": "read",
                                      "description": "Read a file",
                                      "parameters": TOOL_PARAMETERS, "strict": False}]
            assert body["tool_choice"] == "required"
            call = {"type": "function_call", "id": "fc_1", "call_id": "call_abc",
                    "name": "read", "arguments": '{"path":"a.c"}', "status": "completed"}
            events = [event("response.created", response={"id": "r", "status": "in_progress"}),
                      event("response.output_item.added", output_index=0,
                            item=dict(call, arguments="")),
                      event("response.function_call_arguments.delta", item_id="fc_1",
                            output_index=0, delta='{"path":'),
                      event("response.function_call_arguments.delta", item_id="fc_1",
                            output_index=0, delta='"a.c"}'),
                      event("response.output_item.done", output_index=0, item=call),
                      event("response.completed", response={
                          "id": "r", "status": "completed", "output": [call],
                          "usage": usage(40, 12)})]
            return self.sse(events)
        if model == "gpt-quota":
            return self.reply(429, {"error": {
                "code": "subscription_sharing_usage_limit_exceeded",
                "message": "ChatGPT plan usage limit reached", "param": None,
                "type": "usage_limit"}}, {"retry-after": "120", "x-request-id": "req_quota"})
        if model == "gpt-quota-stream":
            return self.sse([event("response.created", response={"id": "r"}),
                             event("response.output_text.delta", delta="par"),
                             event("response.failed", response={
                                 "id": "r", "status": "failed", "usage": None,
                                 "error": {"code": "subscription_sharing_usage_limit_exceeded",
                                           "message": "limit"}})])
        if model == "gpt-unsupported":
            return self.reply(400, {"error": {
                "code": "subscription_sharing_unsupported_capability",
                "param": "text.format", "message": "not available"}})
        if model == "gpt-incomplete":
            return self.sse([event("response.output_text.delta", delta="trunc"),
                             event("response.incomplete", response={
                                 "id": "r", "status": "incomplete",
                                 "incomplete_details": {"reason": "max_output_tokens"},
                                 "usage": usage(30, 64)})])
        if model == "gpt-long":
            return self.sse(text_stream(["abcdefghij"] * 50))
        if model == "gpt-nocompleted":
            return self.sse(text_stream(["half"]), terminal=False)
        if model == "gpt-slow":
            return self.sse(text_stream(["partial", " rest"]), pause_after=3, pause=3.0)
        if model == "gpt-401-once":
            return self.sse(text_stream(["after refresh"]))
        raise AssertionError(model)


class CheckedServer(ThreadingHTTPServer):
    daemon_threads = True
    errors = []

    def handle_error(self, _request, _address):
        self.errors.append(traceback.format_exc())


server = CheckedServer(("127.0.0.1", 0), Handler)
S.issuer = f"http://127.0.0.1:{server.server_port}"
BASE = S.issuer + "/v1"
DRIVER = sys.argv[1]
OUTPUTS = []
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()


def fail(message):
    raise AssertionError(message + ("\nserver errors:\n" + "\n".join(server.errors)
                                    if server.errors else ""))


def check(condition, message):
    if not condition:
        fail(message)


def run(cmd, auth, *args, timeout=60):
    proc = subprocess.run([DRIVER, cmd, BASE, S.issuer, auth, *args], capture_output=True,
                          text=True, timeout=timeout)
    OUTPUTS.append(proc.stdout + proc.stderr)
    check(proc.returncode == 0, f"{cmd} {args} exited {proc.returncode}: {proc.stdout}{proc.stderr}")
    return proc.stdout


def gen(auth, model, *flags):
    out = run("generate", auth, model, *flags)
    results = []
    for block in out.split("GEN ")[1:]:
        lines = block.splitlines()
        head = lines[0].split()
        fields = dict(f.split("=", 1) for f in head[1:])
        fields["rc"] = head[0]
        fields["text"] = next(l[5:] for l in lines if l.startswith("TEXT "))
        fields["error"] = next(l[6:] for l in lines if l.startswith("ERROR "))
        fields["calls"] = [l[5:] for l in lines if l.startswith("CALL ")]
        results.append(fields)
    return results if len(results) > 1 else results[0]


def status(auth):
    out = run("status", auth).split()
    return dict([("rc", out[1])] + [f.split("=", 1) for f in out[2:]])


def http_get(url):
    parts = urllib.parse.urlsplit(url)
    conn = http.client.HTTPConnection(parts.hostname, parts.port, timeout=10)
    conn.request("GET", parts.path + ("?" + parts.query if parts.query else ""))
    response = conn.getresponse()
    data = response.read()
    conn.close()
    return response.status, response.getheader("location"), data.decode(errors="replace")


def start_login(cmd, auth):
    proc = subprocess.Popen([DRIVER, cmd, BASE, S.issuer, auth], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    first = proc.stdout.readline()
    if not first.startswith("URL "):
        proc.kill()
        fail(f"login did not start: {first}{proc.stderr.read()}")
    redirect = proc.stdout.readline().split(" ", 1)[1].strip()
    return proc, first[4:].strip(), redirect


def finish_login(proc):
    out, err = proc.communicate("\n", timeout=30)
    OUTPUTS.append(out + err)
    check(proc.returncode == 0, f"login driver exited {proc.returncode}: {err}")
    return out


def browser_login(auth, cmd="login-browser"):
    proc, url, redirect = start_login(cmd, auth)
    port = urllib.parse.urlsplit(redirect).port
    # Unrelated and forged requests are answered and ignored.
    check(http_get(f"http://127.0.0.1:{port}/favicon.ico")[0] == 404, "favicon not 404")
    check(http_get(f"{redirect}?code=forged&state=wrong")[0] == 400, "forged state accepted")
    status_code, location, _ = http_get(url)
    check(status_code == 302 and location.startswith(redirect + "?"), f"authorize: {status_code}")
    code, _, page = http_get(location)
    result = proc.stdout.readline()
    # The listener is closed as soon as the login finished.
    with socket.socket() as probe:
        probe.settimeout(2)
        check(probe.connect_ex(("127.0.0.1", port)) != 0, "listener still open after login")
    finish_login(proc)
    return result.split(" ", 2), code, page, url


def mode(path):
    return stat.S_IMODE(os.stat(path).st_mode)


def expire(auth, refresh_token=None):
    with open(auth) as f:
        record = json.load(f)
    record["expires_at"] = int(time.time()) - 100
    if refresh_token is not None:
        record["refresh_token"] = refresh_token
    with open(auth, "w") as f:
        json.dump(record, f)


def main():
    tmp = tempfile.mkdtemp(prefix="asmodel-chatgpt-")
    auth = os.path.join(tmp, "auth", "chatgpt.json")

    caps = run("caps", auth).split()
    check(caps[1:4] == ["0", "chatgpt", "siwc-responses-v1"] and caps[5] == "embed=-1", str(caps))

    # Not logged in: a clear AUTH error, nothing dispatched.
    r = gen(auth, "gpt-text")
    check(r["rc"] == "ASMODEL_ERR_AUTH" and "asterism login chatgpt" in r["error"] and
          r["usage_known"] == "1" and S.responses == 0, str(r))

    # First sign-in: dynamic registration through the loopback listener.
    (tag, rc, msg), code, page, url = browser_login(auth)
    check(rc == "ASMODEL_OK" and code == 200 and "Signed in" in page, f"{rc} {msg} {page}")
    first = S.authorize[-1]
    check(first["client_id"] == "dynamic_agent_client", str(first))
    for path in (auth, auth + ".host", auth + ".lock"):
        check(mode(path) == 0o600, f"{path} mode {oct(mode(path))}")
    with open(auth + ".host") as f:
        host_id = f.read().strip()
    check(host_id == first["ext_agent_host_id"], "host id not persisted")
    record = json.load(open(auth))
    check(record["client_id"] == "oaiapp_test1" and record["subject"] == "user-123" and
          "chatgpt.tokens.use.direct" in record["scopes"] and record["refresh_token"], str(record.keys()))
    st = status(auth)
    check(st["rc"] == "ASMODEL_OK" and st["logged_in"] == "1" and st["plan_usage"] == "1" and
          st["email"] == "user@example.com" and st["plan"] == "plus" and
          st["client_id"] == "oaiapp_test1" and st["host_id"] == host_id and
          int(st["expires_at"]) > time.time() + 3000, str(st))

    out = run("models", auth)
    check(out.startswith("MODELS ASMODEL_OK ") and
          json.loads(out.split(" ", 2)[2]) == [{"slug": "gpt-test", "display_name": "GPT Test"}], out)

    # Streaming text with usage (cached + reasoning tokens).
    r = gen(auth, "gpt-text")
    check(r["rc"] == "ASMODEL_OK" and r["text"] == "Ciao \U0001f31f" and r["finish"] == "stop" and
          r["usage_known"] == "1" and r["in"] == "21" and r["out"] == "5" and
          r["reasoning"] == "2" and r["cached"] == "7" and int(r["heartbeats"]) >= 1 and
          r["outputs"] == "1", str(r))

    # Reasoning off is requested and verified from reported usage.
    r = gen(auth, "gpt-reasoning-off", "--reasoning-off")
    check(r["rc"] == "ASMODEL_OK" and r["reasoning"] == "0", str(r))
    r = gen(auth, "gpt-reasoning-violation", "--reasoning-off")
    check(r["rc"] == "ASMODEL_ERR_UNSUPPORTED" and "reasoning-off" in r["error"], str(r))
    before = S.responses
    r = gen(auth, "gpt-text", "--reasoning-budget")
    check(r["rc"] == "ASMODEL_ERR_UNSUPPORTED" and S.responses == before, str(r))

    # Structured output through text.format; GBNF-only fails before dispatch.
    r = gen(auth, "gpt-schema", "--schema", json.dumps(SCHEMA))
    check(r["rc"] == "ASMODEL_OK" and json.loads(r["text"]) == {"answer": "hello"} and
          r["json"] == "1" and int(r["applied"]) & 1, str(r))
    before = S.responses
    r = gen(auth, "gpt-schema", "--grammar-only")
    check(r["rc"] == "ASMODEL_ERR_UNSUPPORTED" and S.responses == before and
          r["usage_known"] == "1", str(r))

    # A discriminated root oneOf is wrapped for OpenAI's strict mode and the
    # caller gets back exactly its own shape; ambiguous unions never dispatch.
    r = gen(auth, "gpt-oneof", "--schema", json.dumps(ONEOF))
    check(r["rc"] == "ASMODEL_OK" and json.loads(r["text"]) ==
          {"action": "call", "tool": "fs.read", "arguments": {"path": "a.c"}}, str(r))
    before = S.responses
    r = gen(auth, "gpt-oneof", "--schema", json.dumps(OVERLAPPING))
    check(r["rc"] == "ASMODEL_ERR_UNSUPPORTED" and S.responses == before, str(r))
    r = gen(auth, "gpt-text", "--schema", json.dumps(OVERLAPPING), "--optional")
    check(r["rc"] == "ASMODEL_OK" and r["json"] == "0" and not int(r["applied"]) & 1, str(r))
    r = gen(auth, "gpt-badschema", "--schema", json.dumps(SCHEMA))
    check(r["rc"] == "ASMODEL_ERR_UNSUPPORTED" and "invalid_json_schema" in r["error"], str(r))

    # Native function tools.
    r = gen(auth, "gpt-tools", "--tools")
    check(r["rc"] == "ASMODEL_OK" and r["finish"] == "tool_calls" and
          r["calls"] == ['call_abc read {"path":"a.c"}'], str(r))

    # Plan limits are a distinct error, never retried.
    before = S.responses
    r = gen(auth, "gpt-quota")
    check(r["rc"] == "ASMODEL_ERR_QUOTA" and r["retry_ms"] == "120000" and
          r["code"] == "subscription_sharing_usage_limit_exceeded" and
          "chatgpt.com/settings/usage" in r["error"] and "req_quota" in r["error"] and
          S.responses == before + 1, str(r))
    r = gen(auth, "gpt-quota-stream")
    check(r["rc"] == "ASMODEL_ERR_QUOTA" and r["usage_known"] == "0" and r["text"] == "par", str(r))
    r = gen(auth, "gpt-unsupported")
    check(r["rc"] == "ASMODEL_ERR_UNSUPPORTED" and "text.format" in r["error"], str(r))

    # Length: server-side incomplete, client-side budget, missing terminal event.
    r = gen(auth, "gpt-incomplete")
    check(r["rc"] == "ASMODEL_ERR_LIMIT" and r["finish"] == "length" and r["usage_known"] == "1" and
          r["out"] == "64" and r["text"] == "trunc", str(r))
    r = gen(auth, "gpt-long", "--max", "16")
    check(r["rc"] == "ASMODEL_ERR_LIMIT" and r["finish"] == "length" and r["usage_known"] == "0" and
          64 < len(r["text"]) <= 80, str(r))
    r = gen(auth, "gpt-nocompleted")
    check(r["rc"] == "ASMODEL_ERR_BACKEND" and "response.completed" in r["error"] and
          r["usage_known"] == "0" and r["text"] == "half", str(r))

    # 401: exactly one refresh and one resend.
    refreshes, before = S.refreshes, S.responses
    r = gen(auth, "gpt-401-once")
    check(r["rc"] == "ASMODEL_OK" and r["text"] == "after refresh" and
          S.refreshes == refreshes + 1 and S.responses == before + 2, str(r))
    refreshes, before = S.refreshes, S.responses
    r = gen(auth, "gpt-401-always")
    check(r["rc"] == "ASMODEL_ERR_AUTH" and "asterism login chatgpt" in r["error"] and
          S.refreshes == refreshes + 1 and S.responses == before + 2, str(r))

    # Cancellation and deadline while streaming.
    started = time.time()
    r = gen(auth, "gpt-slow", "--cancel")
    check(r["rc"] == "ASMODEL_ERR_CANCELLED" and r["finish"] == "cancelled" and
          r["usage_known"] == "0" and time.time() - started < 2.5, str(r))
    r = gen(auth, "gpt-slow", "--deadline", "400")
    check(r["rc"] == "ASMODEL_ERR_TIMEOUT" and r["usage_known"] == "0" and
          r["text"] == "partial", str(r))

    # Proactive refresh: concurrent requests (threads, then processes) spend
    # the single-use refresh token exactly once.
    S.refresh_delay = 0.3
    expire(auth)
    refreshes = S.refreshes
    rs = gen(auth, "gpt-text", "--threads", "4")
    check(all(x["rc"] == "ASMODEL_OK" for x in rs) and S.refreshes == refreshes + 1,
          f"threads: {S.refreshes - refreshes} refreshes {rs}")
    check(mode(auth) == 0o600, "mode after refresh")
    expire(auth)
    refreshes = S.refreshes
    procs = [subprocess.Popen([DRIVER, "generate", BASE, S.issuer, auth, "gpt-text"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
             for _ in range(3)]
    outs = [p.communicate(timeout=60) for p in procs]
    OUTPUTS.extend(o + e for o, e in outs)
    check(all(p.returncode == 0 for p in procs), f"race process failed: {outs}")
    check(all(o.startswith("GEN ASMODEL_OK") for o, _ in outs) and S.refreshes == refreshes + 1,
          f"processes: {S.refreshes - refreshes} refreshes {outs}")
    S.refresh_delay = 0.0

    # Reauthorization reuses the issued client, the host id and the hints.
    (tag, rc, msg), code, page, url = browser_login(auth)
    again = S.authorize[-1]
    check(rc == "ASMODEL_OK" and again["client_id"] == "oaiapp_test1" and
          again["ext_agent_host_id"] == host_id and "id_token_hint" in again and
          again["login_hint"] == "user@example.com", f"{rc} {msg} {again}")
    check(len(S.clients) == 1, "reauthorization registered a new client")

    # A spent refresh token: login expired, tokens cleared, registration kept.
    expire(auth, refresh_token="rt-bogus")
    r = gen(auth, "gpt-text")
    check(r["rc"] == "ASMODEL_ERR_AUTH" and "login expired" in r["error"], str(r))
    st = status(auth)
    check(st["logged_in"] == "0" and st["client_id"] == "oaiapp_test1", str(st))
    (tag, rc, msg), code, page, url = browser_login(auth)
    check(rc == "ASMODEL_OK" and "id_token_hint" not in S.authorize[-1], f"{rc} {msg}")

    # Logout revokes the refresh token and keeps the registration.
    refresh = json.load(open(auth))["refresh_token"]
    out = run("logout", auth)
    check(out.startswith("LOGOUT ASMODEL_OK revoked=1") and refresh in S.revoked, out)
    st = status(auth)
    check(st["logged_in"] == "0" and st["client_id"] == "oaiapp_test1" and
          st["host_id"] == host_id, str(st))
    r = gen(auth, "gpt-text")
    check(r["rc"] == "ASMODEL_ERR_AUTH" and "not logged in" in r["error"], str(r))

    # Headless: the user pastes the redirected address from another device.
    paste = os.path.join(tmp, "headless.json")
    proc, url, redirect = start_login("login-paste", paste)
    _, location, _ = http_get(url)
    wrong_state = re.sub(r"state=[^&]+", "state=" + "x" * 43, location)
    wrong_port = location.replace(redirect, "http://127.0.0.1:1/auth/callback")
    out = finish_login_with(proc, ["abc123", wrong_state, wrong_port, "  " + location + "  "])
    lines = [l for l in out.splitlines() if l.startswith("COMPLETE")]
    check([l.split()[1] for l in lines] == ["ASMODEL_ERR_INVALID"] * 3 + ["ASMODEL_OK"] and
          "bare code" in lines[0] and "state mismatch" in lines[1], out)
    check(mode(paste) == 0o600 and status(paste)["plan_usage"] == "1", "headless record")
    check(gen(paste, "gpt-text")["rc"] == "ASMODEL_OK", "headless generate")

    # Signed in without plan usage: retained, but inference refused up front.
    S.grant_plan = False
    noplan = os.path.join(tmp, "noplan.json")
    (tag, rc, msg), code, page, url = browser_login(noplan)
    check(rc == "ASMODEL_ERR_UNSUPPORTED" and "plan usage was not granted" in msg, f"{rc} {msg}")
    check(status(noplan)["plan_usage"] == "0", "plan flag")
    before = S.responses
    r = gen(noplan, "gpt-text")
    check(r["rc"] == "ASMODEL_ERR_UNSUPPORTED" and S.responses == before, str(r))
    S.grant_plan = True

    # Consent declined.
    S.deny = True
    denied = os.path.join(tmp, "denied.json")
    (tag, rc, msg), code, page, url = browser_login(denied)
    check(rc == "ASMODEL_ERR_AUTH" and "declined" in msg and code == 400, f"{rc} {msg}")
    S.deny = False

    # Credentials never reach any output.
    joined = "\n".join(OUTPUTS)
    for secret in S.secrets:
        check(secret not in joined, "a credential leaked into driver output")
    check(not server.errors, "server errors")


def finish_login_with(proc, lines):
    out, err = proc.communicate("\n".join(lines) + "\n", timeout=30)
    OUTPUTS.append(out + err)
    check(proc.returncode == 0, f"login driver exited {proc.returncode}: {err}")
    return out


try:
    main()
except BaseException:
    if server.errors:
        print("server errors:\n" + "\n".join(server.errors), file=sys.stderr)
    raise
finally:
    server.shutdown()
    thread.join()
if server.errors:
    print("\n".join(server.errors), file=sys.stderr)
    raise SystemExit(1)
print("chatgpt provider contract: ok")
