"""End-to-end check of `asterism setup` against a fake Telegram Bot API and a
local model server: config for the tier, tool packages, model downloads
verified by SHA-256 (one resumed with an HTTP Range), the bot token stored in
the vault and nowhere else, pairing claimed by setup itself, a second run
that keeps everything, doctor's serve checks, and an old serve.env token
moved into the vault, a tier switch, and start/status without a service. No
network beyond loopback, no service installed.

usage: setup_e2e.py <asterism>
"""
import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = "123456:TESTtoken_abc-XYZ"
OWNER = 4242
BLOBS = {"alpha.gguf": os.urandom(300_000), "beta.gguf": os.urandom(200_000)}
PART = 100_000
SEEN = []          # (path, range header)
SENT = []          # (method, payload)
UPDATES = queue.Queue()


class ModelHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        name = self.path.lstrip("/")
        SEEN.append((name, self.headers.get("range")))
        blob = BLOBS.get(name)
        if blob is None:
            self.send_error(404)
            return
        start = 0
        m = re.fullmatch(r"bytes=(\d+)-", self.headers.get("range") or "")
        if m:
            start = int(m.group(1))
            self.send_response(206)
            self.send_header("content-range", f"bytes {start}-{len(blob) - 1}/{len(blob)}")
        else:
            self.send_response(200)
        self.send_header("content-length", str(len(blob) - start))
        self.end_headers()
        self.wfile.write(blob[start:])


class TgHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def answer(self, result):
        data = json.dumps({"ok": True, "result": result}).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        prefix = f"/bot{TOKEN}/"
        if not self.path.startswith(prefix):
            data = json.dumps({"ok": False, "error_code": 401, "description": "Unauthorized"}).encode()
            self.send_response(401)
            self.send_header("content-length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        method = self.path[len(prefix):]
        raw = self.rfile.read(int(self.headers.get("content-length", "0")))
        payload = json.loads(raw) if raw else {}
        if method == "getMe":
            self.answer({"id": 1, "is_bot": True, "first_name": "asterism",
                         "username": "asterism_test_bot"})
        elif method == "getUpdates":
            out = []
            try:
                out.append(UPDATES.get(timeout=min(int(payload.get("timeout", 0)), 1)))
            except queue.Empty:
                pass
            self.answer(out)
        elif method == "sendMessage":
            SENT.append((method, payload))
            self.answer({"message_id": len(SENT), "date": int(time.time()),
                         "chat": {"id": payload.get("chat_id"), "type": "private"},
                         "text": payload.get("text", "")})
        else:
            SENT.append((method, payload))
            self.answer(True)


def serve(handler):
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return f"http://127.0.0.1:{server.server_address[1]}"


def start_message(text, update_id):
    return {"update_id": update_id, "message": {
        "message_id": update_id, "date": int(time.time()),
        "chat": {"id": OWNER, "type": "private"},
        "from": {"id": OWNER, "is_bot": False, "first_name": "Owner"}, "text": text}}


def run_setup(exe, args, stdin_text=None, pair=False, timeout=90, env=None):
    """Runs setup; with pair, answers the /start code it prints."""
    proc = subprocess.Popen([exe, "setup"] + args, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", env=env)
    if stdin_text is not None:
        proc.stdin.write(stdin_text)
    proc.stdin.close()
    lines = []
    deadline = time.time() + timeout
    for line in proc.stdout:
        lines.append(line)
        m = re.search(r"/start (\d{8})", line)
        if pair and m:
            UPDATES.put(start_message("/start 00000000", 1))   # a wrong code first
            UPDATES.put(start_message(f"/start {m.group(1)}", 2))
            pair = False
        assert time.time() < deadline, "".join(lines)
    proc.wait(timeout=30)
    return proc.returncode, "".join(lines)


def files_under(root):
    for base, _dirs, names in os.walk(root):
        for name in names:
            yield os.path.join(base, name)


def main():
    exe = sys.argv[1]
    models_url, tg_url = serve(ModelHandler), serve(TgHandler)
    tmp = tempfile.mkdtemp(prefix="asterism-setup-")
    try:
        manifest = os.path.join(tmp, "manifest.tsv")
        with open(manifest, "w", encoding="utf-8") as f:
            f.write("# test manifest\n")
            for name, blob in BLOBS.items():
                f.write(f"base\t{name}\t{len(blob)}\t{hashlib.sha256(blob).hexdigest()}\t"
                        f"{models_url}/{name}\t-\n")
        root = os.path.join(tmp, "root")
        os.makedirs(os.path.join(root, "models"))
        with open(os.path.join(root, "models", "alpha.gguf.part"), "wb") as f:
            f.write(BLOBS["alpha.gguf"][:PART])
        common = ["--root", root, "--tier", "base", "--no-service", "--manifest", manifest,
                  "--api-base", tg_url, "--pair-timeout", "30"]

        # First run: everything from scratch, token on stdin, pairing answered.
        rc, out = run_setup(exe, common + ["--token-stdin"], TOKEN + "\n", pair=True)
        assert rc == 0, out
        for name, blob in BLOBS.items():
            with open(os.path.join(root, "models", name), "rb") as f:
                assert f.read() == blob, name
        assert ("alpha.gguf", f"bytes={PART}-") in SEEN, SEEN
        assert ("beta.gguf", None) in SEEN, SEEN
        assert not [p for p in files_under(root) if p.endswith(".part")]
        with open(os.path.join(root, "config.xcdn"), encoding="utf-8") as f:
            config = f.read()
        assert "qwen3.5-4b-q4_k_m.gguf" in config and '"astools.xcdn"' in config, config
        assert os.listdir(os.path.join(root, "tools")), "no tool packages copied"
        with open(os.path.join(root, "astools.xcdn"), encoding="utf-8") as f:
            grants = f.read()
        assert grants.startswith("// Written by asterism setup") and '"read-write"' in grants, grants
        with open(os.path.join(root, "serve.xcdn"), encoding="utf-8") as f:
            serve_cfg = f.read()
        assert 'token_secret: "telegram-token"' in serve_cfg, serve_cfg
        for path in files_under(root):
            with open(path, "rb") as f:
                assert TOKEN.encode() not in f.read(), f"token in plain text: {path}"
        assert TOKEN not in out
        texts = [p.get("text", "") for m, p in SENT if m == "sendMessage"]
        assert any(t.startswith("Not paired yet") for t in texts), texts
        assert any(t.startswith("Paired.") for t in texts), texts
        assert "@asterism_test_bot" in out and "paired: the agent answers only your account" in out, out

        # Second run: nothing to ask, nothing to download.
        before = len(SEEN)
        rc, out = run_setup(exe, common + ["--yes"])
        assert rc == 0, out
        assert len(SEEN) == before, SEEN[before:]
        assert "config.xcdn: kept" in out and "token: in the vault" in out, out
        assert "paired with your Telegram account" in out and "alpha.gguf: checking" in out, out

        # Claude Code installed later: its folder joins the tool permissions.
        if os.name != "nt":
            cli = os.path.join(tmp, "cli")
            os.makedirs(cli)
            with open(os.path.join(cli, "claude"), "w", encoding="utf-8") as f:
                f.write("#!/bin/sh\nexit 0\n")
            os.chmod(os.path.join(cli, "claude"), 0o755)
            env = dict(os.environ, PATH=cli + os.pathsep + os.environ.get("PATH", ""))
            rc, out = run_setup(exe, common + ["--yes"], env=env)
            assert rc == 0 and f"the agent may start programs from {cli}" in out, out
            with open(os.path.join(root, "astools.xcdn"), encoding="utf-8") as f:
                grants = f.read()
            assert f'"{cli}"' in grants and '"read-write"' in grants, grants
            rc, out = run_setup(exe, common + ["--yes"], env=env)
            assert "may start programs" not in out, out

        # A corrupt model is fetched again.
        with open(os.path.join(root, "models", "beta.gguf"), "r+b") as f:
            f.write(b"x")
        rc, out = run_setup(exe, common + ["--yes"])
        assert rc == 0 and "beta.gguf: checking" in out and "wrong checksum" in out, out
        with open(os.path.join(root, "models", "beta.gguf"), "rb") as f:
            assert f.read() == BLOBS["beta.gguf"]

        # doctor reports the serve side.
        doctor = subprocess.run([exe, "doctor", "--root", root], capture_output=True, text=True,
                                encoding="utf-8", timeout=60)
        assert "telegram token: in the vault" in doctor.stdout, doctor.stdout
        assert "telegram pairing: paired" in doctor.stdout, doctor.stdout
        assert re.search(r"tools: [1-9]\d* packages", doctor.stdout), doctor.stdout

        # Switching tier rewrites config.xcdn and keeps the old one.
        rc, out = run_setup(exe, ["--root", root, "--tier", "standard", "--yes", "--no-models",
                                  "--no-telegram", "--no-service"])
        assert rc == 0 and "config.xcdn: written for tier standard" in out, out
        with open(os.path.join(root, "config.xcdn"), encoding="utf-8") as f:
            assert "qwen3.5-9b-q4_k_m.gguf" in f.read()
        with open(os.path.join(root, "config.xcdn.bak"), encoding="utf-8") as f:
            assert "qwen3.5-4b-q4_k_m.gguf" in f.read()

        # Without a background service, start and status say what to do.
        status = subprocess.run([exe, "status", "--root", root], capture_output=True, text=True,
                                encoding="utf-8", timeout=30)
        start = subprocess.run([exe, "start", "--root", root], capture_output=True, text=True,
                               encoding="utf-8", timeout=30)
        if "not set up to run in the background" in status.stdout:
            assert status.returncode == 0 and start.returncode == 1, (status, start)
            assert "asterism serve" in start.stdout, start.stdout

        # An install from before the vault: serve.env moves into it.
        old = os.path.join(tmp, "old")
        os.makedirs(old)
        with open(os.path.join(old, "serve.xcdn"), "w", encoding="utf-8") as f:
            f.write('#serve {\n  telegram: { token_env: "ASTERISM_TELEGRAM_TOKEN", topics: true },\n}\n')
        with open(os.path.join(old, "serve.env"), "w", encoding="utf-8") as f:
            f.write(f"ASTERISM_TELEGRAM_TOKEN={TOKEN}\n")
        env = dict(os.environ)
        env.pop("ASTERISM_TELEGRAM_TOKEN", None)
        rc, out = run_setup(exe, ["--root", old, "--yes", "--no-models", "--no-service",
                                  "--api-base", tg_url, "--pair-timeout", "2"])
        assert "serve.env removed" in out, out
        assert not os.path.exists(os.path.join(old, "serve.env"))
        with open(os.path.join(old, "serve.xcdn"), encoding="utf-8") as f:
            text = f.read()
        assert 'token_secret: "telegram-token"' in text and "token_env" not in text, text
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
