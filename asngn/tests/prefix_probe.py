"""How much of each model request repeats the one before it.

A local model server keeps the state of the last prompt and only reads what
is new, so a harness is fast when every request starts with the previous one.
This probe runs a few turns of one session against a fake OpenAI-compatible
server that answers with a tool call and then a text, records every request
with tools, and prints, for each, the share of it that is a prefix shared
with the previous request.

usage: prefix_probe.py <asterism> <packages-dir> [--classifier off|model]
                       [--min-turn-reuse F] [--dump DIR]

--min-turn-reuse makes it a test: it fails when the first request of a turn
shares less than that share with the last request of the turn before.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TURNS = [
    "List the files in the workspace and tell me how many there are.",
    "Now check whether notes.txt exists.",
    "Elenca di nuovo i file e dimmi se qualcosa e' cambiato.",
    "Thanks. In one sentence, what did you find overall?",
]


def rendered(request):
    """The request as one text, in the order a chat template lays it out:
    system text, the tools, then every later message."""
    messages = request.get("messages", [])
    parts = []
    rest = messages
    if messages and messages[0].get("role") in ("system", "developer"):
        parts.append("<system>" + json.dumps(messages[0].get("content")))
        rest = messages[1:]
    parts.append("<tools>" + json.dumps(request.get("tools") or []))
    for m in rest:
        parts.append("<" + m.get("role", "?") + ">" + json.dumps(
            {k: m[k] for k in ("content", "tool_calls", "tool_call_id") if k in m}, sort_keys=True))
    return "\n".join(parts)


def shared(a, b):
    n = min(len(a), len(b))
    i = 0
    while i < n and a[i] == b[i]:
        i += 1
    return i


class Model:
    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []   # (turn, request) for requests that offer tools or answer
        self.turn = 0
        self.ids = 0

    def reply(self, request):
        with self.lock:
            grammar = request.get("grammar") or json.dumps(request.get("response_format") or "")
            if "CLASS" in grammar:
                return "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK LOOKUP\n", None
            if grammar.strip('"'):
                return "NONE\n", None
            self.requests.append((self.turn, request))
            tools = [t["function"] for t in request.get("tools") or []]
            last_user = max((i for i, m in enumerate(request["messages"]) if m.get("role") == "user"
                             and "[asterism runtime state" not in str(m.get("content"))), default=-1)
            called = any(m.get("role") == "tool" for m in request["messages"][last_user + 1:])
            lister = [t for t in tools if t["description"].startswith("fs.list:")]
            if lister and not called:
                self.ids += 1
                return "", {"id": f"call_{self.ids}", "name": lister[0]["name"],
                            "arguments": json.dumps({"path": "."})}
            return f"Answer for turn {self.turn + 1}: one entry.", None


MODEL = Model()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("content-length", "0")))
        if self.path == "/tokenize":
            body = json.loads(raw)
            text = body.get("content") or body.get("text") or ""
            data = json.dumps({"tokens": [0] * (len(text.encode()) // 4 + 1)}).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return
        text, call = MODEL.reply(json.loads(raw))
        events = []
        if call:
            events.append({"choices": [{"index": 0, "delta": {"role": "assistant", "tool_calls": [
                {"index": 0, "id": call["id"], "type": "function",
                 "function": {"name": call["name"], "arguments": call["arguments"]}}]}}]})
        else:
            events += [{"choices": [{"index": 0, "delta": {"content": text[i:i + 16]}}]}
                       for i in range(0, len(text), 16)]
        events.append({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls" if call else "stop"}],
                       "usage": {"prompt_tokens": 10, "completion_tokens": 5}})
        data = ("".join("data: " + json.dumps(e) + "\n\n" for e in events) + "data: [DONE]\n\n").encode()
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("packages")
    ap.add_argument("--classifier", default="model")
    ap.add_argument("--min-turn-reuse", type=float, default=None)
    ap.add_argument("--dump", default=None)
    ap.add_argument("--core", default=None, help="integration.astools.core: tools always offered")
    args = ap.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    root = tempfile.mkdtemp(prefix="asterism-prefix-")
    try:
        shutil.copytree(args.packages, os.path.join(root, "tools"))
        with open(os.path.join(root, "astools.xcdn"), "w") as f:
            f.write('#astools_config { registry: { paths: [ { path: "tools", trust: "standard" } ],'
                    ' watch: "off", pinning: "off" }, workspace: { root: "." },'
                    ' grants: { workspace_access: "read-write" } }\n')
        core = f', core: "{args.core}"' if args.core else ""
        with open(os.path.join(root, "config.xcdn"), "w") as f:
            f.write(f'''#asngn_config {{
  models: {{
    max_resident: 1,
    pool: [ {{ id: "m", backend: "openai", base_url: "http://127.0.0.1:{server.server_address[1]}/v1",
              model: "fake", provider: "llama-server", ctx: 262144 }} ],
    roles: {{ router: "m", planner: "m", generator: "m", compressor: "m",
              adapter: "m", judge: "m", embedder: "" }},
  }},
  routing: {{ classifier: "{args.classifier}", max_escalations: 0, native_actions: true }},
  validation: {{ judge: "off" }},
  cache: {{ enable: false, tool_cache: false }},
  safety: {{ stall_timeout: "PT0S", turn_deadline: "PT0S" }},
  integration: {{
    asper: {{ enable: true, root: "memory" }},
    astools: {{ enable: true, root: "tools", config: "astools.xcdn", workspace: "session",
               tool_limit: 59, tool_schema_bytes: 131072{core} }},
  }},
}}
''')
        for i, text in enumerate(TURNS):
            MODEL.turn = i
            run = subprocess.run([args.exe, "--root", root, "--allow-degraded", "--session", "probe",
                                  "--confirm", "allow", "--once", text],
                                 capture_output=True, text=True, timeout=120)
            assert run.returncode == 0, f"turn {i + 1} failed: {run.stderr[-2000:]}"

        prev = None
        worst_turn = 1.0
        total = reused = 0
        seen_turn = -1
        print(f"{'turn':>4} {'req':>3} {'chars':>7} {'shared':>7} {'reuse':>6}")
        for n, (turn, request) in enumerate(MODEL.requests):
            text = rendered(request)
            if args.dump:
                os.makedirs(args.dump, exist_ok=True)
                with open(os.path.join(args.dump, f"req-{n:02d}-turn{turn + 1}.txt"), "w") as f:
                    f.write(text)
            if prev is not None:
                k = shared(prev, text)
                share = k / len(prev)
                total += len(text)
                reused += k
                first = turn != seen_turn
                if first:
                    worst_turn = min(worst_turn, share)
                print(f"{turn + 1:>4} {n:>3} {len(text):>7} {k:>7} {share:>6.2f}{'  <- new turn' if first else ''}")
            else:
                print(f"{turn + 1:>4} {n:>3} {len(text):>7} {'-':>7} {'-':>6}")
            seen_turn = turn
            prev = text
        offered = [len(r.get("tools") or []) for _, r in MODEL.requests]
        print(f"functions offered per request: {min(offered)}..{max(offered)}")
        print(f"requests: {len(MODEL.requests)}; characters to read again: "
              f"{total - reused} of {total} ({100.0 * (total - reused) / max(total, 1):.0f}%)")
        print(f"lowest share of the previous request kept at a new turn: {worst_turn:.2f}")
        if args.min_turn_reuse is not None:
            assert worst_turn >= args.min_turn_reuse, (
                f"a new turn kept only {worst_turn:.2f} of the previous request "
                f"(wanted {args.min_turn_reuse:.2f}): the prompt was rewritten instead of extended")
            print("prefix probe: ok")
    finally:
        server.shutdown()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
