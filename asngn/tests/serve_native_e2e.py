"""Telegram approvals with native tool calls (routing.native_actions), against
the fake Telegram of serve_e2e.py and a fake OpenAI-compatible server that
answers with tool calls. The default star, with no mode set, has tools. The
owner allows a write and the file appears; a denied write leaves nothing; an
approval request Telegram refuses to show is denied, so the turn ends instead
of waiting for buttons nobody sees; a read of a missing file goes back to the
model as a rejected call instead of failing the turn.

usage: serve_native_e2e.py <asterism> <packages-dir>
"""
import io
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler

import serve_e2e as base
from serve_e2e import OWNER, TG, TOKEN, callback, message, sent_text, serve_http


class Model:
    """One reply per request: a class line for the router, a tool call while
    tools are offered, then the final answer."""

    def __init__(self):
        self.lock = threading.Lock()
        self.calls = []       # next proposals: (tool, arguments)
        self.answers = []
        self.requests = []
        self.ids = 0

    def reply(self, request):
        """(text, tool_call) for one request."""
        with self.lock:
            self.requests.append(request)
            grammar = request.get("grammar") or json.dumps(request.get("response_format") or "")
            if "CLASS" in grammar:
                return "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK EDIT\n", None
            if grammar.strip('"'):
                return "NONE\n", None
            tools = [t["function"] for t in request.get("tools") or []]
            if not tools:
                return (self.answers.pop(0) if self.answers else "ok"), None
            self.ids += 1
            call_id = f"call_{self.ids}"
            # Earlier turns stay in the request as native messages: look only at this turn.
            asked = max((i for i, m in enumerate(request["messages"]) if m.get("role") == "user"
                         and "[asterism runtime state" not in str(m.get("content"))), default=-1)
            if any(m.get("role") == "tool" for m in request["messages"][asked + 1:]):  # this turn has a result
                return "", {"id": call_id, "name": "asterism_finish", "arguments": "{}"}
            assert self.calls, "no scripted call left"
            tool, args = self.calls.pop(0)
            match = [t for t in tools if t["description"].startswith(tool + ":")]
            assert match, f"{tool} was not offered: " + ", ".join(t["name"] for t in tools)
            return "", {"id": call_id, "name": match[0]["name"], "arguments": json.dumps(args)}


MODEL = Model()


class ModelHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("content-length", "0")))
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
        events.append({"choices": [{"index": 0, "delta": {},
                                    "finish_reason": "tool_calls" if call else "stop"}],
                       "usage": {"prompt_tokens": 10, "completion_tokens": 5}})
        data = ("".join("data: " + json.dumps(e) + "\n\n" for e in events) + "data: [DONE]\n\n").encode()
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class Telegram(base.TgHandler):
    """serve_e2e's fake Telegram, which can refuse approval messages."""
    refuse = 0

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("content-length", "0")))
        if self.path.endswith("/sendMessage") and Telegram.refuse and b"Approval needed" in raw:
            Telegram.refuse -= 1
            TG.record("refused", json.loads(raw))
            self.answer({}, status=500)
            return
        self.rfile = io.BytesIO(raw)
        super().do_POST()


def approval(mark):
    _, review = TG.wait_for(lambda m, p: m == "sendMessage" and "reply_markup" in p
                            and "Approval needed" in p.get("text", ""), start=mark)
    assert "fs.write" in review["text"], review["text"]
    return [b["callback_data"] for row in review["reply_markup"]["inline_keyboard"] for b in row]


def main():
    exe, packages = sys.argv[1], sys.argv[2]
    model = serve_http(ModelHandler)
    tg = serve_http(Telegram)
    root = tempfile.mkdtemp(prefix="asterism-native-")
    proc = None
    try:
        shutil.copytree(packages, os.path.join(root, "tools"))
        with open(os.path.join(root, "astools.xcdn"), "w") as f:
            f.write('#astools_config { registry: { paths: [ { path: "tools", trust: "standard" } ],'
                    ' watch: "off", pinning: "off" }, workspace: { root: "." },'
                    ' grants: { workspace_access: "read-write" } }\n')
        with open(os.path.join(root, "config.xcdn"), "w") as f:
            f.write(f'''#asngn_config {{
  models: {{
    max_resident: 1,
    pool: [ {{ id: "m", backend: "openai", base_url: "http://127.0.0.1:{model.server_address[1]}/v1",
              model: "fake", provider: "llama-server", ctx: 131072 }} ],
    roles: {{ router: "m", planner: "m", generator: "m", compressor: "m",
              adapter: "m", judge: "m", embedder: "" }},
  }},
  routing: {{ classifier: "model", max_escalations: 0, native_actions: true }},
  validation: {{ judge: "off" }},
  cache: {{ enable: false, tool_cache: false }},
  safety: {{ stall_timeout: "PT0S", turn_deadline: "PT0S" }},
  integration: {{
    asper: {{ enable: true, root: "memory" }},
    astools: {{ enable: true, root: "tools", config: "astools.xcdn", workspace: "session" }},
  }},
}}
''')
        with open(os.path.join(root, "serve.xcdn"), "w") as f:
            f.write(f'''#serve {{
  telegram: {{ token_env: "TEST_BOT_TOKEN", api_base: "http://127.0.0.1:{tg.server_address[1]}",
              poll_timeout: 1, topics: true }},
  stars: [ {{ name: "default" }} ],
}}
''')
        pair = subprocess.run([exe, "serve", "pair", "--root", root], capture_output=True, text=True, timeout=30)
        assert pair.returncode == 0, pair.stderr
        code = pair.stdout.split("/start ")[1].split()[0]
        proc = subprocess.Popen([exe, "serve", "--root", root, "--allow-degraded"],
                                env=dict(os.environ, TEST_BOT_TOKEN=TOKEN), stderr=subprocess.PIPE, text=True)
        TG.wait_for(lambda m, p: m == "setMyCommands")
        mark = TG.mark()
        TG.push(message(f"/start {code}"))
        TG.wait_for(lambda m, p: sent_text(m, p, "Paired."), start=mark)
        workspace = os.path.join(root, "sessions", f"tg-{OWNER}-0", "workspace")

        # Allowed: the proposed write runs.
        mark = TG.mark()
        MODEL.calls.append(("fs.write", {"path": "note.txt", "content": "hello"}))
        MODEL.answers.append("Done: note.txt written.")
        TG.push(message("write hello to note.txt"))
        buttons = approval(mark)
        assert len(buttons) == 3, buttons  # allow, deny, always
        TG.push(callback(buttons[0], 101))
        TG.wait_for(lambda m, p: sent_text(m, p, "Done: note.txt written."), start=mark)
        with open(os.path.join(workspace, "note.txt")) as f:
            assert f.read() == "hello"

        # Denied: nothing is written, and the turn still answers.
        mark = TG.mark()
        MODEL.calls.append(("fs.write", {"path": "other.txt", "content": "bye"}))
        MODEL.answers.append("I did not write other.txt.")
        TG.push(message("write bye to other.txt"))
        TG.push(callback(approval(mark)[1], 102))
        TG.wait_for(lambda m, p: sent_text(m, p, "I did not write other.txt."), start=mark)
        assert not os.path.exists(os.path.join(workspace, "other.txt"))

        # Not shown: Telegram refuses every attempt, the request is denied.
        mark = TG.mark()
        Telegram.refuse = 4
        MODEL.calls.append(("fs.write", {"path": "lost.txt", "content": "x"}))
        MODEL.answers.append("The write was not approved.")
        TG.push(message("write x to lost.txt"))
        TG.wait_for(lambda m, p: sent_text(m, p, "could not be shown"), timeout=40, start=mark)
        TG.wait_for(lambda m, p: sent_text(m, p, "The write was not approved."), start=mark)
        assert not os.path.exists(os.path.join(workspace, "lost.txt"))
        assert Telegram.refuse == 0

        # A rejected call is an observation, not the end of the turn.
        mark = TG.mark()
        MODEL.calls.append(("fs.read", {"path": "missing.txt"}))
        MODEL.answers.append("missing.txt does not exist.")
        TG.push(message("read missing.txt"))
        TG.wait_for(lambda m, p: sent_text(m, p, "missing.txt does not exist."), start=mark)
        results = [m for r in MODEL.requests for m in r.get("messages", []) if m.get("role") == "tool"]
        assert any("rejected before execution" in str(m.get("content")) for m in results), results[-1:]

        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=30)
        assert proc.returncode == 0, proc.stderr.read()
        proc = None
        print("serve native e2e: ok")
    except BaseException:
        for request in MODEL.requests[-6:]:
            last = request.get("messages", [{}])[-1]
            print(f"--- {json.dumps(last)[-600:]}", file=sys.stderr)
        raise
    finally:
        if proc:
            proc.kill()
            print(proc.stderr.read(), file=sys.stderr)
        model.shutdown()
        tg.shutdown()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
