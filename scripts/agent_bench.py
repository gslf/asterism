#!/usr/bin/env python3
"""Run a fixed set of agent tasks against a real model server and measure them.

Each task is a short conversation in its own session, with a check on the
answer or on the files it leaves. A small proxy sits between asterism and the
model server and records, for every request, how many prompt tokens were
read and how many were reused, and how long reading and writing took.

usage: agent_bench.py --exe build/bin/asterism --packages build/packages
                      --models ~/asterism/models [--server http://127.0.0.1:8080]
                      [--config new|old] [--tasks a,b,...] [--out result.json]

--config new: no classifier, a fixed short tool list.
--config old: the classifier in front and every tool in every request.
The model server must already be running.
"""
import argparse
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CALC_C = """#include <stdio.h>
int add(int a, int b) { return a - b; }
int main(void) { printf("%d\\n", add(2, 3)); return 0; }
"""


def has(*words):
    return lambda answer, ws: all(w.lower() in answer.lower() for w in words)


def file_has(name, *words):
    def check(answer, ws):
        path = os.path.join(ws, name)
        if not os.path.exists(path):
            return False
        text = open(path, errors="replace").read()
        return all(w in text for w in words)
    return check


def calc_fixed(answer, ws):
    path = os.path.join(ws, "calc.c")
    if not os.path.exists(path):
        return False
    exe = os.path.join(ws, "calc_check")
    if subprocess.run(["cc", "-o", exe, path], capture_output=True).returncode != 0:
        return False
    return subprocess.run([exe], capture_output=True, text=True).stdout.strip() == "5"


# name, files to place in the workspace, the user's messages, the check
TASKS = [
    ("chat-en", {}, ["In one sentence: what is the capital of Australia?"], has("Canberra")),
    ("chat-it", {}, ["Rispondi in una frase: qual e' la capitale del Canada?"], has("Ottawa")),
    ("list-en", {"a.txt": "one\n", "b.txt": "two\n", "c.md": "# three\n"},
     ["How many files are in the workspace folder, and what are their names?"], has("a.txt", "b.txt", "c.md")),
    ("read-it", {"nota.txt": "Il codice segreto e' 7341.\n"},
     ["Leggi nota.txt e dimmi qual e' il codice segreto."], has("7341")),
    ("write-en", {}, ["Create a file hello.txt containing exactly the line: hello world"],
     file_has("hello.txt", "hello world")),
    ("edit-it", {"config.ini": "[server]\nport = 8080\nhost = localhost\n"},
     ["Nel file config.ini cambia la porta da 8080 a 9090, senza toccare il resto."],
     file_has("config.ini", "9090", "host = localhost")),
    ("grep-en", {"src/a.c": "int alpha(void) { return 1; }\n", "src/b.c": "int beta(void) { return 2; }\n",
                 "src/c.c": "int gamma_fn(void) { return beta() + 1; }\n"},
     ["Which file defines the function beta, and which other file calls it?"], has("b.c", "c.c")),
    ("debug-en", {"calc.c": CALC_C},
     ["calc.c should print 5 but prints -1. Find the bug, fix the file, compile and run it to confirm."], calc_fixed),
    ("follow-it", {"dati.csv": "nome,eta\nAnna,31\nLuca,45\nSara,28\n"},
     ["Leggi dati.csv e dimmi chi e' la persona piu' giovane.",
      "E la piu' anziana? Rispondi senza rileggere il file se non serve."], has("Luca")),
    ("multi-en", {"notes.md": "# Notes\n\n- buy milk\n- call Bob\n"},
     ["Add the item 'pay rent' to the list in notes.md.",
      "Now tell me how many items the list has."], lambda a, ws: file_has("notes.md", "pay rent")(a, ws) and
     ("3" in a or "three" in a.lower())),
]


def variants():
    """Five kinds of request, four wordings each: three to learn from (a, b, c) and one held out (t)."""
    out = []
    caps = [("France", "Paris", "In one sentence: what is the capital of France?"),
            ("Giappone", "Tokyo", "Rispondi in una frase: qual e' la capitale del Giappone?"),
            ("Spain", "Madrid", "Tell me in one sentence which city is the capital of Spain."),
            ("Portogallo", "Lisbon", "In una frase: qual e' la capitale del Portogallo?")]
    lists = [({"x.txt": "1\n", "y.txt": "2\n"}, "How many files are in the workspace folder, and what are they called?", ("x.txt", "y.txt")),
             ({"uno.md": "a\n", "due.md": "b\n", "tre.md": "c\n"}, "Quanti file ci sono nella cartella di lavoro e come si chiamano?", ("uno.md", "due.md", "tre.md")),
             ({"main.c": "int x;\n", "util.c": "int y;\n"}, "List the files in the workspace and tell me their names.", ("main.c", "util.c")),
             ({"red.txt": "r\n", "blue.txt": "b\n", "green.txt": "g\n"}, "Which files does the workspace folder contain? Give me the names.", ("red.txt", "blue.txt", "green.txt"))]
    reads = [("memo.txt", "The door code is 4821.\n", "Read memo.txt and tell me the door code.", "4821"),
             ("appunti.txt", "Il numero dell'ordine e' 77310.\n", "Leggi appunti.txt e dimmi il numero dell'ordine.", "77310"),
             ("key.txt", "The license key is QX-9917.\n", "Open key.txt and tell me the license key.", "QX-9917"),
             ("pin.txt", "Il pin della scheda e' 6054.\n", "Leggi pin.txt e dimmi qual e' il pin.", "6054")]
    edits = [("app.ini", "[net]\nport = 3000\nhost = local\n", "In app.ini change the port from 3000 to 3100 and leave the rest alone.", ("3100", "host = local")),
             ("srv.ini", "[web]\nport = 80\nname = demo\n", "Nel file srv.ini cambia la porta da 80 a 8088, senza toccare il resto.", ("8088", "name = demo")),
             ("db.ini", "[db]\ntimeout = 30\nuser = admin\n", "In db.ini set timeout to 45 instead of 30; change nothing else.", ("45", "user = admin")),
             ("log.ini", "[log]\nlevel = info\npath = out.log\n", "Nel file log.ini cambia il livello da info a debug, senza toccare altro.", ("debug", "path = out.log"))]
    greps = [({"lib/a.c": "int foo(void) { return 1; }\n", "lib/b.c": "int bar(void) { return foo(); }\n"}, "Which file defines the function foo?", ("a.c",)),
             ({"src/x.py": "def carica():\n    return 1\n", "src/y.py": "def salva():\n    return 2\n"}, "In quale file e' definita la funzione salva?", ("y.py",)),
             ({"m/one.c": "int init(void) { return 0; }\n", "m/two.c": "int stop(void) { return 0; }\n"}, "Find the file that defines the function stop.", ("two.c",)),
             ({"p/k.c": "int open_db(void) { return 0; }\n", "p/l.c": "int close_db(void) { return 0; }\n"}, "Which file contains the definition of close_db?", ("l.c",))]
    for i, tag in enumerate("abct"):
        c = caps[i]
        out.append((f"v-chat-{tag}", {}, [c[2]], has(c[1]) if c[1] != "Lisbon" else (lambda a, ws: "lisb" in a.lower())))
        l = lists[i]
        out.append((f"v-list-{tag}", l[0], [l[1]], has(*l[2])))
        r = reads[i]
        out.append((f"v-read-{tag}", {r[0]: r[1]}, [r[2]], has(r[3])))
        e = edits[i]
        out.append((f"v-edit-{tag}", {e[0]: e[1]}, [e[2]], file_has(e[0], *e[3])))
        g = greps[i]
        out.append((f"v-grep-{tag}", g[0], [g[1]], has(*g[2])))
    return out


class Proxy:
    """Forwards to the model server and keeps each response's usage and timings."""

    def __init__(self, upstream, force_effort=None):
        self.force_effort = force_effort
        u = urllib.parse.urlparse(upstream)
        self.host, self.port = u.hostname, u.port or 80
        self.records = []
        self.lock = threading.Lock()
        proxy = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def forward(self, body):
                started = time.time()
                conn = http.client.HTTPConnection(proxy.host, proxy.port, timeout=3600)
                headers = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "connection", "content-length")}
                conn.request(self.command, self.path, body=body, headers=headers)
                resp = conn.getresponse()
                data = resp.read()
                self.send_response(resp.status)
                for k, v in resp.getheaders():
                    if k.lower() not in ("transfer-encoding", "content-length", "connection"):
                        self.send_header(k, v)
                self.send_header("content-length", str(len(data)))
                self.send_header("connection", "close")
                self.end_headers()
                self.wfile.write(data)
                conn.close()
                if self.path.endswith("/chat/completions"):
                    proxy.note(body, data, time.time() - started)

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("content-length", "0")))
                if proxy.force_effort and self.path.endswith("/chat/completions"):
                    request = json.loads(body)
                    request["reasoning_effort"] = proxy.force_effort
                    body = json.dumps(request).encode()
                self.forward(body)

            def do_GET(self):
                self.forward(None)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}/v1"

    def note(self, body, data, seconds):
        usage, timings = {}, {}
        chunks = []
        text = data.decode(errors="replace")
        if text.lstrip().startswith("data:"):
            for line in text.splitlines():
                if line.startswith("data: ") and line != "data: [DONE]":
                    try:
                        chunks.append(json.loads(line[6:]))
                    except ValueError:
                        pass
        else:
            try:
                chunks.append(json.loads(text))
            except ValueError:
                pass
        for c in chunks:
            usage = c.get("usage") or usage
            timings = c.get("timings") or timings
        request = json.loads(body)
        cached = timings.get("prompt_cached")
        if cached is None:
            cached = (usage.get("prompt_tokens_details") or {}).get("cached_tokens", 0)
        with self.lock:
            self.records.append({
                "seconds": round(seconds, 2),
                "prompt_tokens": usage.get("prompt_tokens", 0),
                "cached_tokens": cached or 0,
                "completion_tokens": usage.get("completion_tokens", 0),
                "prompt_ms": timings.get("prompt_ms"),
                "predicted_ms": timings.get("predicted_ms"),
                "tools": len(request.get("tools") or []),
                "effort": request.get("reasoning_effort"),
            })

    def take(self):
        with self.lock:
            out, self.records = self.records, []
        return out


def config(kind, proxy_url, models, ctx):
    pool = f'''
      {{ id: "std", path: "{models}/qwen3.5-4b-q4_k_m.gguf", ctx: 32768, threads: 8, gpu_layers: 0{', tier: 1' if kind == 'new' else ''} }},
      {{ id: "nano", path: "{models}/qwen3.5-0.8b-q8_0.gguf", ctx: 8192, threads: 4, gpu_layers: 0{', tier: 0' if kind == 'new' else ''} }},
      {{ id: "inz", backend: "openai", provider: "generic", base_url: "{proxy_url}",
        model: "qwen3.8-flash-next", ctx: {ctx}{', tier: 2, bytes_per_token: 3.0' if kind == 'new' else ''} }},
      {{ id: "embed", path: "{models}/embeddinggemma-300m-q8_0.gguf", ctx: 2048, threads: 4, gpu_layers: 0,
        embedding: true, dim: 768, query_prefix: "task: search result | query: ",
        document_prefix: "title: none | text: " }},'''
    if kind == "new":
        routing = 'routing: { native_actions: true, classifier: "off" },'
        core = ', core: "fs.read fs.list fs.write edit.lines edit.replace grep.search proc.run git.status git.diff"'
        safety = 'safety: { max_steps: 48, max_tool_calls: 40, stall_timeout: "PT0S", turn_deadline: "PT0S" },'
    else:
        routing = 'routing: { native_actions: true },'
        core = ""
        safety = 'safety: { stall_timeout: "PT0S", turn_deadline: "PT0S" },'
    return f'''#asngn_config {{
  models: {{
    max_resident: 4,
    pool: [{pool}
    ],
    roles: {{ router: "nano", planner: "inz", generator: "inz", compressor: "std",
              adapter: "std", judge: "std", embedder: "embed" }},
    sampling: {{ decide: {{ temp: 1.0, top_p: 0.95, max_tokens: 8192 }}, answer: {{ temp: 1.0, top_p: 0.95 }} }},
  }},
  {routing}
  {safety}
  integration: {{
    asper: {{ enable: true, root: "memory" }},
    astools: {{ enable: true, root: "tools", config: "astools.xcdn", workspace: "session",
               tool_limit: 59, tool_schema_bytes: 131072{core} }},
  }},
}}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--packages", required=True)
    ap.add_argument("--models", default=os.path.expanduser("~/asterism/models"))
    ap.add_argument("--server", default="http://127.0.0.1:8080")
    ap.add_argument("--config", choices=("new", "old"), default="new")
    ap.add_argument("--ctx", type=int, default=65536)
    ap.add_argument("--tasks", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--timeout", type=int, default=2400)
    ap.add_argument("--variants", default=None, help="run the wording variants with these tags, e.g. abc or t")
    ap.add_argument("--root", default=None, help="keep the engine root here across runs (what the agent learned stays)")
    ap.add_argument("--suffix", default="", help="added to session names, so a kept root gets fresh sessions")
    ap.add_argument("--force-effort", default=None, help="send this reasoning_effort whatever asterism asks")
    args = ap.parse_args()

    wanted = set(args.tasks.split(",")) if args.tasks else None
    proxy = Proxy(args.server, args.force_effort)
    root = args.root or tempfile.mkdtemp(prefix="asterism-bench-")
    os.makedirs(root, exist_ok=True)
    tasks = TASKS
    if args.variants:
        tasks = [t for t in variants() if t[0][-1] in args.variants]
    results = []
    try:
        if not os.path.exists(os.path.join(root, "tools")):
            shutil.copytree(args.packages, os.path.join(root, "tools"))
        with open(os.path.join(root, "astools.xcdn"), "w") as f:
            f.write('#astools_config { registry: { paths: [ { path: "tools", trust: "standard" } ],'
                    ' watch: "off", pinning: "off" }, workspace: { root: "." },'
                    ' grants: { workspace_access: "read-write" } }\n')
        with open(os.path.join(root, "config.xcdn"), "w") as f:
            f.write(config(args.config, proxy.url, os.path.abspath(args.models), args.ctx))
        for name, files, turns, check in tasks:
            if wanted and name not in wanted:
                continue
            name += args.suffix
            ws = os.path.join(root, "sessions", name, "workspace")
            os.makedirs(ws, exist_ok=True)
            for rel, text in files.items():
                os.makedirs(os.path.dirname(os.path.join(ws, rel)) or ws, exist_ok=True)
                with open(os.path.join(ws, rel), "w") as f:
                    f.write(text)
            answer, error, started, per_turn = "", None, time.time(), []
            for text in turns:
                t0 = time.time()
                try:
                    run = subprocess.run([args.exe, "--root", root, "--session", name, "--confirm", "allow",
                                          "--once", text], capture_output=True, text=True, timeout=args.timeout)
                    answer = run.stdout
                    if run.returncode != 0:
                        error = run.stderr.strip()[-400:]
                except subprocess.TimeoutExpired:
                    error = "timeout"
                per_turn.append(round(time.time() - t0, 1))
                if error:
                    break
            reqs = proxy.take()
            try:
                ok = error is None and bool(check(answer, ws))
            except Exception as ex:  # a check must never stop the run
                ok, error = False, f"check failed: {ex}"
            prompt = sum(r["prompt_tokens"] for r in reqs)
            cached = sum(r["cached_tokens"] for r in reqs)
            row = {"task": name, "ok": ok, "seconds": round(time.time() - started, 1), "turn_seconds": per_turn,
                   "requests": len(reqs), "prompt_tokens": prompt, "cached_tokens": cached,
                   "read_tokens": prompt - cached,
                   "completion_tokens": sum(r["completion_tokens"] for r in reqs),
                   "model_seconds": round(sum(r["seconds"] for r in reqs), 1),
                   "efforts": [r["effort"] for r in reqs], "error": error, "answer": answer.strip()[-300:]}
            results.append(row)
            print(f"{name:10} {'ok  ' if ok else 'FAIL'} {row['seconds']:7.1f}s  model {row['model_seconds']:6.1f}s  requests {row['requests']:2}  "
                  f"read {row['read_tokens']:6} of {prompt:6} prompt tokens  wrote {row['completion_tokens']:5}"
                  + (f"  [{error[:80]}]" if error else ""), flush=True)
        done = [r for r in results]
        if done:
            print(f"\n{args.config}: {sum(r['ok'] for r in done)}/{len(done)} tasks ok, "
                  f"{sum(r['seconds'] for r in done):.0f}s total, "
                  f"{sum(r['read_tokens'] for r in done)} prompt tokens read of "
                  f"{sum(r['prompt_tokens'] for r in done)}")
        if args.out:
            with open(args.out, "w") as f:
                json.dump({"config": args.config, "results": results}, f, indent=1)
    finally:
        proxy.server.shutdown()
        if not args.root:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
