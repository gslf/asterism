"""Projects from the MCP server and the headless TUI: register, bind, unbind."""
import json
import os
import subprocess
import sys
import tempfile

ASTERISM = sys.argv[1]


def same(a, b):
    """One path, whatever the separators and, on Windows, the case."""
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


def mcp(root, calls):
    messages = [dict(jsonrpc="2.0", id=1, method="initialize", params={
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "projects-test", "version": "1"}})]
    messages += [dict(jsonrpc="2.0", id=i + 2, method="tools/call",
                      params={"name": name, "arguments": args})
                 for i, (name, args) in enumerate(calls)]
    proc = subprocess.run([ASTERISM, "mcp", "--root", root],
                          input="".join(json.dumps(m) + "\n" for m in messages),
                          capture_output=True, text=True, encoding="utf-8",
                          timeout=60, check=True)
    replies = [json.loads(line) for line in proc.stdout.splitlines()][1:]
    assert len(replies) == len(calls), proc.stdout
    return [(r["result"]["isError"], json.loads(r["result"]["content"][0]["text"]))
            for r in replies]


with tempfile.TemporaryDirectory(prefix="asngn-projects-") as tmp:
    root = os.path.join(tmp, "root")
    ext = os.path.realpath(os.path.join(tmp, "ext"))
    os.makedirs(root)
    os.makedirs(ext)
    demo = os.path.join(os.path.realpath(root), "projects", "demo")

    r = mcp(root, [
        ("project_create", {"name": "demo"}),
        ("project_create", {"name": "ext", "path": ext}),
        ("project_create", {"name": "etc", "path": "/etc" if os.name != "nt" else os.environ["SystemRoot"]}),
        ("project_create", {"name": "Bad Name"}),
        ("project_list", {}),
        ("project_bind", {"project": "demo", "session": "s1"}),
        ("workspace_info", {"session": "s1"}),
        ("project_bind", {"project": None, "session": "s1"}),
        ("workspace_info", {"session": "s1"}),
        ("project_remove", {"name": "ext"}),
        ("project_list", {}),
    ])
    assert r[0][0] is False and r[0][1]["name"] == "demo" and same(r[0][1]["path"], demo), r[0]
    assert os.path.isdir(demo)
    assert same(r[1][1]["path"], ext), r[1]
    assert r[2][0] and r[2][1]["error"] == "ASNGN_ERR_DENIED", r[2]
    assert r[3][0], r[3]
    assert [p["name"] for p in r[4][1]["projects"]] == ["demo", "ext"], r[4]
    assert r[5][0] is False and same(r[5][1]["workspace"], demo), r[5]
    assert (r[5][1]["project"], r[5][1]["mode"], r[5][1]["security_profile"]) == \
        ("demo", "coding", "coding-sandboxed"), r[5]
    assert r[6][1]["project"] == "demo" and same(r[6][1]["canonical_root"], demo), r[6]
    assert r[7][1]["project"] is None and not same(r[7][1]["workspace"], demo), r[7]
    assert r[8][1]["project"] is None and "sessions" in r[8][1]["canonical_root"], r[8]
    assert r[9][1]["name"] == "ext" and os.path.isdir(ext), r[9]
    assert [p["name"] for p in r[10][1]["projects"]] == ["demo"], r[10]

    # The headless TUI binds before the turn, which fails here (no model);
    # the session defaults to the project's name and keeps the binding.
    proc = subprocess.run([ASTERISM, "--root", root, "--project", "demo", "--once", "hi"],
                          capture_output=True, text=True, encoding="utf-8", timeout=60)
    with open(os.path.join(root, "sessions", "demo", "session.xcdn"), encoding="utf-8") as f:
        manifest = f.read()
    assert 'project: "demo"' in manifest, manifest
    assert demo in manifest or demo.replace("\\", "/") in manifest, manifest
    assert not os.path.exists(os.path.join(demo, ".astools")), "tool scratch inside the project"

    # A fixed workspace has no per-session folders, so binding is refused.
    proc = subprocess.run([ASTERISM, "--root", root, "--workspace", ext, "--session", "fixed",
                           "--project", "demo", "--once", "hi"],
                          capture_output=True, text=True, encoding="utf-8", timeout=60)
    assert proc.returncode == 1 and "projects need session workspaces" in proc.stderr, proc.stderr
