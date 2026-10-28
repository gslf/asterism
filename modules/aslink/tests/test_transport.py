"""Two local HTTP servers for aslink's transport: argv[1] is transport_driver.

Server a and server b listen on 127.0.0.1 (ephemeral ports, so two hosts as
far as a redirect is concerned). /blob honours "Range: bytes=N-"; the other
paths ignore it, answer the wrong range, or redirect from one server to the
other. Every request is logged and checked after the driver exits.
"""

import re
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BLOB = bytes((i * 7 + 3) % 251 for i in range(50000))
PART = 20000
SEEN = []
SEEN_LOCK = threading.Lock()
URLS = {}


class Handler(BaseHTTPRequestHandler):
    name = "?"

    def log_message(self, *_args):
        pass

    def reply(self, status, body, headers=None):
        try:
            self.send_response(status)
            self.send_header("content-type", "application/octet-stream")
            self.send_header("content-length", str(len(body)))
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # the client stops reading a 200 to a ranged download

    def redirect(self, location):
        self.send_response(302)
        self.send_header("location", location)
        self.send_header("content-length", "0")
        self.end_headers()

    def start(self):
        """The N of "bytes=N-", or None without a Range."""
        value = self.headers.get("range")
        if value is None:
            return None
        m = re.fullmatch(r"bytes=(\d+)-", value)
        assert m, value
        return int(m.group(1))

    def ranged(self):
        start = self.start()
        if start is None:
            return self.reply(200, BLOB)
        if start >= len(BLOB):
            return self.reply(416, b"range not satisfiable",
                              {"content-range": f"bytes */{len(BLOB)}"})
        self.reply(206, BLOB[start:],
                   {"content-range": f"bytes {start}-{len(BLOB) - 1}/{len(BLOB)}"})

    def do_GET(self):
        with SEEN_LOCK:
            SEEN.append((self.name, self.path, self.headers.get("range")))
        assert self.headers.get("user-agent", "").startswith("asterism/")
        getattr(self, "get_" + self.name)()

    def get_a(self):
        start = self.start()
        if self.path == "/blob":
            return self.ranged()
        if self.path == "/blob-norange":
            return self.reply(200, BLOB)
        if self.path == "/blob-wrong":
            return self.reply(206, BLOB[start + 1:],
                              {"content-range": f"bytes {start + 1}-{len(BLOB) - 1}/{len(BLOB)}"})
        if self.path == "/blob-bare-206":
            return self.reply(206, BLOB[start:])
        if self.path == "/moved/blob":
            return self.redirect(URLS["b"] + "/blob")
        if self.path == "/moved/page":
            return self.redirect(URLS["b"] + "/page")
        if self.path == "/moved/round-trip":
            return self.redirect(URLS["b"] + "/back")
        if self.path == "/home":
            return self.reply(200, b"back home")
        self.reply(404, b"no such path on a")

    def get_b(self):
        if self.path == "/blob":
            return self.ranged()
        if self.path == "/page":
            return self.reply(200, b"second host")
        if self.path == "/back":
            return self.redirect(URLS["a"] + "/home")
        self.reply(404, b"no such path on b")


class CheckedServer(ThreadingHTTPServer):
    daemon_threads = True
    errors = []

    def handle_error(self, _request, _address):
        self.errors.append(traceback.format_exc())


servers = {}
for name in ("a", "b"):
    servers[name] = CheckedServer(("127.0.0.1", 0), type("Handler_" + name, (Handler,),
                                                         {"name": name}))
    URLS[name] = f"http://127.0.0.1:{servers[name].server_port}"
threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in servers.values()]
for t in threads:
    t.start()
try:
    with tempfile.TemporaryDirectory() as scratch:
        started = time.monotonic()
        result = subprocess.call([sys.argv[1], URLS["a"], URLS["b"], scratch])
        print(f"driver finished in {time.monotonic() - started:.2f}s", file=sys.stderr)
finally:
    for s in servers.values():
        s.shutdown()
        s.server_close()
    for t in threads:
        t.join()

assert not CheckedServer.errors, "\n".join(CheckedServer.errors)
if result == 0:
    ranged = f"bytes={PART}-"
    # The Range went along with the redirect to the second host.
    assert ("a", "/moved/blob", ranged) in SEEN, SEEN
    assert ("b", "/blob", ranged) in SEEN, SEEN
    # Not followed with a limit of 0; over a limit of 1 the second hop is
    # refused or never made.
    assert sum(1 for s in SEEN if s == ("b", "/blob", ranged)) == 1, SEEN
    assert sum(1 for s in SEEN if s[1] == "/home") == 1, SEEN
    # A request without range_from sends no Range.
    assert ("a", "/blob", None) in SEEN and ("a", "/blob-norange", None) in SEEN, SEEN
else:
    print(f"aslink transport driver failed with exit code {result}", file=sys.stderr)
raise SystemExit(result)
