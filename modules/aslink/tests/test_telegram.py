"""Scripted Bot API server for aslink: argv[1] is telegram_driver.

The server listens on 127.0.0.1 (ephemeral port), checks every request the
driver makes (method, token, exact JSON or multipart body) and answers like
api.telegram.org would. The token in the path selects the behaviour.
"""

import email.parser
import email.policy
import json
import re
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

GOOD = "123456:TEST-token_abc"
RATE = "222222:RATE-limited"
BAD = "333333:WRONG-token"
HANG = "444444:HANG-forever"
SLOW = "555555:SLOW-server"
JUNK = "666666:JUNK-replies"

STOP = threading.Event()
SEEN = []
SEEN_LOCK = threading.Lock()
ALLOWED = ["message", "callback_query", "stopped_message_generation"]
PRIVATE = {"id": 5, "first_name": "Ann", "type": "private"}
ANN = {"id": 5, "is_bot": False, "first_name": "Ann", "username": "ann"}
BOT = {"id": 999, "is_bot": True, "first_name": "Asterism", "username": "asterism_test_bot"}


def message(message_id, **extra):
    m = {"message_id": message_id, "from": BOT, "chat": PRIVATE, "date": 1793001600}
    m.update(extra)
    return m


UPDATES = [
    {"update_id": 100, "message": {"message_id": 1, "from": ANN, "chat": PRIVATE,
                                   "date": 1793001600, "text": "/status",
                                   "entities": [{"offset": 0, "length": 6, "type": "bot_command"}]}},
    {"update_id": 101, "callback_query": {"id": "cbq-1", "from": ANN,
                                          "message": message(500, text="Approve?"),
                                          "chat_instance": "-42", "data": "ap:1:ok"}},
    {"update_id": 102, "stopped_message_generation": {"chat": PRIVATE, "draft_id": 77}},
]


def wait(seconds):
    """Sleep like a long poll; return early when the test ends."""
    STOP.wait(seconds)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def reply(self, status, payload, content_type="application/json", length=True):
        body = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        try:
            self.send_response(status)
            self.send_header("content-type", content_type)
            if length:
                self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def ok(self, result):
        self.reply(200, {"ok": True, "result": result})

    def fail(self, code, description, **params):
        payload = {"ok": False, "error_code": code, "description": description}
        if params:
            payload["parameters"] = params
        self.reply(code, payload)

    # ── generic pages for aslink_http_get ───────────────────────────────
    def do_GET(self):
        with SEEN_LOCK:
            SEEN.append(("GET", self.path))
        assert self.headers.get("user-agent", "").startswith("asterism/")
        m = re.fullmatch(r"/file/bot([^/]+)/(.+)", self.path)
        if m:
            assert m.group(1) == GOOD, m.group(1)
            return self.file(m.group(2))
        hop = re.fullmatch(r"/page/hop/(\d+)", self.path)
        if self.path == "/page/redirect":
            return self.redirect("/page/final")
        if self.path == "/page/final":
            return self.reply(200, b"hello page", "text/plain")
        if hop:
            n = int(hop.group(1))
            if n == 0:
                return self.reply(200, b"landed", "text/plain")
            return self.redirect(f"/page/hop/{n - 1}")
        if self.path == "/page/big":
            return self.reply(200, b"x" * 100_000, "text/plain")
        if self.path == "/page/big-stream":
            return self.reply(200, b"y" * 100_000, "text/plain", length=False)
        if self.path == "/page/to-file":
            return self.redirect("file:///etc/passwd")
        return self.reply(404, b"no such page", "text/plain")

    def redirect(self, location):
        self.send_response(302)
        self.send_header("location", location)
        self.send_header("content-length", "0")
        self.end_headers()

    def file(self, path):
        if path == "documents/file_1.txt":
            return self.reply(200, b"hello world", "application/octet-stream")
        if path == "documents/file_2.bin":
            return self.reply(200, b"\0" * 5000, "application/octet-stream")
        if path == "documents/file_3.bin":
            # HTTP/1.0 without Content-Length: close-delimited body
            return self.reply(200, b"\1" * 5000, "application/octet-stream", length=False)
        if path == "documents/missing.txt":
            return self.fail(404, "Not Found")
        raise AssertionError(f"unexpected download of {path}")

    # ── Bot API methods ─────────────────────────────────────────────────
    def do_POST(self):
        m = re.fullmatch(r"/bot([^/]+)/([A-Za-z]+)", self.path)
        assert m, self.path
        token, method = m.group(1), m.group(2)
        raw = self.rfile.read(int(self.headers.get("content-length", "0")))
        ctype = self.headers.get("content-type", "")
        assert self.headers.get("expect") is None
        with SEEN_LOCK:
            SEEN.append((method, token))
        if method == "sendDocument":
            return self.send_document(token, ctype, raw)
        assert ctype == "application/json", ctype
        request = json.loads(raw)
        if token == RATE:
            return self.fail(429, "Too Many Requests: retry after 7", retry_after=7)
        if token == BAD:
            # Echo the token: aslink must redact it from its status message.
            return self.fail(401, f"Unauthorized: bot{BAD} and {BAD} rejected")
        if token == SLOW:
            wait(2)
            return self.ok(BOT)
        if token == JUNK:
            if method == "getMe":
                return self.reply(200, b"not json", "text/plain")
            return self.reply(502, b"<html>502 Bad Gateway</html>", "text/html")
        if token == HANG:
            assert method == "getUpdates"
            wait(30)
            return self.ok([])
        assert token == GOOD, token
        getattr(self, "m_" + method)(request)

    def m_getMe(self, request):
        assert request == {}
        self.ok(dict(BOT, can_join_groups=False, has_topics_enabled=True,
                     allows_users_to_create_topics=False))

    def m_getUpdates(self, request):
        assert request["allowed_updates"] == ALLOWED, request
        offset = request.get("offset", 0)
        if offset == 0:
            assert request["timeout"] == 0
            return self.ok(UPDATES)
        assert offset in (103, 200), offset
        wait(request["timeout"])
        self.ok([])

    def m_sendMessage(self, request):
        text = request["text"]
        if text.startswith("concurrent "):
            assert request == {"chat_id": 5, "text": text}
            return self.ok(message(700 + int(text.split()[1]), text=text))
        assert request == {
            "chat_id": 5,
            "message_thread_id": 9,
            "text": "<b>Approve</b> sending &lt;report&gt; &amp; co?",
            "parse_mode": "HTML",
            "link_preview_options": {"is_disabled": True},
            "disable_notification": True,
            "reply_parameters": {"message_id": 42, "allow_sending_without_reply": True},
            "reply_markup": {"inline_keyboard": [
                [{"text": "Yes ✓", "callback_data": "ap:1:ok"},
                 {"text": "No", "callback_data": "ap:1:no"}],
                [{"text": "Evidenza", "url": "https://example.org/e"}],
            ]},
        }, request
        self.ok(message(500, message_thread_id=9, is_topic_message=True, text="Approve"))

    def m_editMessageText(self, request):
        assert request == {"chat_id": 5, "message_id": 500, "text": "Approvato ✅"}, request
        self.ok(message(500, text="Approvato ✅", edit_date=1793001700))

    def m_editMessageReplyMarkup(self, request):
        assert request == {"chat_id": 5, "message_id": 500}, request
        self.ok(message(500, text="Approvato ✅"))

    def m_sendMessageDraft(self, request):
        assert request == {"chat_id": 5, "message_thread_id": 9, "draft_id": 77,
                           "text": "Sto scrivendo", "can_stop": True}, request
        self.ok(True)

    def m_sendChatAction(self, request):
        assert request == {"chat_id": 5, "message_thread_id": 9, "action": "typing"}, request
        self.ok(True)

    def m_answerCallbackQuery(self, request):
        assert request == {"callback_query_id": "cbq-1", "text": "Fatto"}, request
        self.ok(True)

    def m_createForumTopic(self, request):
        assert request == {"chat_id": 5, "name": "Task: report"}, request
        self.ok({"message_thread_id": 321, "name": "Task: report", "icon_color": 7322096})

    def m_setMyCommands(self, request):
        assert request == {"commands": [
            {"command": "status", "description": "Task status"},
            {"command": "stop", "description": "Stop"},
        ]}, request
        self.ok(True)

    def m_getFile(self, request):
        file_id = request["file_id"]
        files = {
            "doc-ok": {"file_size": 11, "file_path": "documents/file_1.txt"},
            "doc-big": {"file_size": 5000, "file_path": "documents/file_big.bin"},
            "doc-liar": {"file_path": "documents/file_2.bin"},
            "doc-stream": {"file_path": "documents/file_3.bin"},
            "doc-gone": {"file_size": 3, "file_path": "documents/missing.txt"},
            "doc-abs": {"file_path": "/var/lib/telegram-bot-api/documents/file_9.txt"},
        }
        if file_id not in files:
            return self.fail(400, "Bad Request: invalid file_id")
        self.ok(dict(files[file_id], file_id=file_id, file_unique_id="u-" + file_id))

    def send_document(self, token, ctype, raw):
        assert token == GOOD, token
        assert ctype.startswith("multipart/form-data; boundary="), ctype
        msg = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(
            b"Content-Type: " + ctype.encode() + b"\r\n\r\n" + raw)
        assert msg.is_multipart()
        fields, files = {}, {}
        for part in msg.iter_parts():
            name = part.get_param("name", header="content-disposition")
            data = part.get_payload(decode=True)
            if part.get_filename() is not None:
                files[name] = (part.get_filename(), data)
            else:
                fields[name] = data.decode("utf-8")
        assert fields == {"chat_id": "5", "message_thread_id": "9",
                          "caption": "The report is ready ✓"}, fields
        assert files == {"document": (
            "report.txt", "line one\nline ✓\n".encode() + b"\x00\x01\x02 binary")}, files
        self.ok(message(600, document={"file_id": "BQAC-new", "file_unique_id": "u",
                                       "file_name": "report.txt"}))


class CheckedServer(ThreadingHTTPServer):
    daemon_threads = True
    errors = []

    def handle_error(self, _request, _address):
        self.errors.append(traceback.format_exc())


server = CheckedServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory() as scratch:
        started = time.monotonic()
        result = subprocess.call([sys.argv[1], f"http://127.0.0.1:{server.server_port}", scratch])
        print(f"driver finished in {time.monotonic() - started:.2f}s", file=sys.stderr)
finally:
    STOP.set()
    server.shutdown()
    thread.join()
    server.server_close()

assert not server.errors, "\n".join(server.errors)
# Refused locally or over the cap: never downloaded.
assert ("GET", "/file/bot" + GOOD + "/documents/file_big.bin") not in SEEN
# The empty text was rejected before reaching the server.
assert sum(1 for s in SEEN if s == ("sendMessage", GOOD)) == 6, SEEN
if result:
    print(f"aslink telegram driver failed with exit code {result}", file=sys.stderr)
raise SystemExit(result)
