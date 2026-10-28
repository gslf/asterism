"""delegate.* end to end: `asterism tools invoke` -> runtime -> astools-std ->
fake `claude` / `codex` executables found through sandbox.executable_paths.

The fakes (tools/fake_agent.py) record their argv, environment, cwd and
stdin, so the test checks the exact flags, that the account's HOME reaches
the CLI, and that no credential file, API key or bypass flag ever does.
"""
import argparse
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import subprocess
import sys
import tempfile

CREDENTIAL_MARKERS = (".credentials.json", ".claude.json", "auth.json")
FORBIDDEN_FLAGS = {"--bare", "--dangerously-skip-permissions",
                   "--allow-dangerously-skip-permissions", "bypassPermissions",
                   "--dangerously-bypass-approvals-and-sandbox", "danger-full-access",
                   "--yolo", "--full-auto"}
FORBIDDEN_ENV = ("ANTHROPIC_", "OPENAI_", "CLAUDE_CODE_", "CODEX_API_KEY")
SECRETS = {"ANTHROPIC_API_KEY": "sk-ant-test-must-not-pass",
           "CLAUDE_CODE_OAUTH_TOKEN": "oauth-test-must-not-pass",
           "OPENAI_API_KEY": "sk-test-must-not-pass",
           "CODEX_API_KEY": "codex-test-must-not-pass"}
CLAUDE_PLAN = ["--output-format", "stream-json", "--verbose", "--permission-mode", "plan",
               "--tools", "Read,Glob,Grep", "--disallowedTools", "mcp__*",
               "--strict-mcp-config", "--setting-sources", "user", "--append-system-prompt"]


# ---- minimal xCDN reader for `tools invoke` output -------------------------

TOKEN = re.compile(r'\s*(?:(#[A-Za-z_][A-Za-z0-9_]*)|([{}\[\]:,])|([rtubd]?"(?:[^"\\]|\\.)*")|'
                   r'(-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)|([A-Za-z_][A-Za-z0-9_]*))', re.S)


def parse_xcdn(text):
    tokens = []
    pos, text = 0, text.strip()
    while pos < len(text):
        m = TOKEN.match(text, pos)
        assert m and m.end() > pos, ("unexpected xCDN text", text[pos:pos + 40])
        pos = m.end()
        tag, punct, string, number, ident = m.groups()
        if tag:
            continue  # tags carry no data the test needs
        if punct:
            tokens.append(punct)
        elif string is not None:
            body = string[1:] if string[0] != '"' else string
            tokens.append(("s", json.loads(body)))
        elif number is not None:
            tokens.append(("n", float(number) if re.search(r"[.eE]", number) else int(number)))
        else:
            tokens.append(("i", ident))
    value, rest = _value(tokens, 0)
    assert rest == len(tokens), "trailing xCDN tokens"
    return value


def _value(tokens, i):
    t = tokens[i]
    if t == "{":
        obj, i = {}, i + 1
        while tokens[i] != "}":
            key = tokens[i][1]
            assert tokens[i + 1] == ":"
            obj[key], i = _value(tokens, i + 2)
            if tokens[i] == ",":
                i += 1
        return obj, i + 1
    if t == "[":
        arr, i = [], i + 1
        while tokens[i] != "]":
            v, i = _value(tokens, i)
            arr.append(v)
            if tokens[i] == ",":
                i += 1
        return arr, i + 1
    kind, v = t
    if kind == "i":
        return {"true": True, "false": False, "null": None}.get(v, v), i + 1
    return v, i + 1


# ---- host --------------------------------------------------------------------

class Host:
    def __init__(self, cli, packages, root):
        self.cli, self.packages, self.root = cli, packages, root

    def config(self, name, workspace, level="basic", exec_paths=(), grant=True):
        tools = []
        if grant:
            tools = [{"tool": "delegate", "proc": True, "net": True,
                      "env": ["HTTPS_PROXY", "LANG"] + list(SECRETS)}]
        cfg = {"registry": {"watch": "off", "pinning": "off"},
               "workspace": {"root": str(workspace)},
               "sandbox": {"default_level": level, "strict_fallback": "reject",
                           "executable_paths": [str(p) for p in exec_paths]},
               "grants": {"workspace_access": "read-write", "tools": tools},
               "invocation": {"result_validation": "enforce"}}
        path = self.root / (name + ".xcdn")
        path.write_text("#astools_config " + json.dumps(cfg), encoding="utf-8")
        return path

    def invoke(self, config, target, args):
        env = {**os.environ, **SECRETS, "HTTPS_PROXY": "http://proxy.test:3128"}
        run = subprocess.run([self.cli, "invoke", target, args, "--root", str(self.packages),
                              "--config", str(config)],
                             text=True, capture_output=True, timeout=120, env=env)
        value = parse_xcdn(run.stdout) if run.stdout.strip() else None
        return run.returncode, value, run.stderr


def record(bin_dir, name):
    path = bin_dir / ("record-%s.json" % name)
    data = json.loads(path.read_text(encoding="utf-8"))
    path.unlink()
    return data


def check_invariants(rec, bin_dir, workspace, prompt_index=None):
    """What must hold for every CLI start, whatever the command."""
    argv, env = rec["argv"], rec["env"]
    home = pwd.getpwuid(os.getuid()).pw_dir
    assert env.get("HOME") == home, ("account HOME not passed", env.get("HOME"))
    assert str(bin_dir) in env.get("PATH", "").split(":"), env.get("PATH")
    assert env.get("TERM") == "dumb" and env.get("NO_COLOR") == "1", env
    assert env.get("HTTPS_PROXY") == "http://proxy.test:3128", "granted proxy lost"
    assert os.path.realpath(rec["cwd"]) == os.path.realpath(workspace), rec["cwd"]
    for name in env:
        assert not name.startswith(FORBIDDEN_ENV), ("credential variable reached the CLI", name)
        assert not name.startswith("ASTOOLS_"), ("runtime control variable leaked", name)
    for secret in SECRETS.values():
        assert secret not in json.dumps(rec), "a secret value reached the CLI"
    for i, arg in enumerate(argv[1:], start=1):
        assert arg not in FORBIDDEN_FLAGS, ("forbidden flag", arg)
        if i != prompt_index:
            assert not any(m in arg for m in CREDENTIAL_MARKERS), ("credential path in argv", arg)
    for name, value in env.items():
        assert not any(m in value for m in CREDENTIAL_MARKERS), ("credential path in env", name)


def git(*args, cwd):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args],
                   cwd=cwd, check=True, capture_output=True, timeout=30)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--packages", required=True, type=Path)
    parser.add_argument("--fake", required=True, type=Path)
    a = parser.parse_args()

    check = subprocess.run([a.cli, "check", str(a.packages / "delegate")], text=True,
                           capture_output=True, timeout=30)
    assert check.returncode == 0, (check.stdout, check.stderr)
    assert check.stdout.strip() == "ok: delegate 1.0.0, 4 commands", check.stdout

    with tempfile.TemporaryDirectory(prefix="astools-delegate-") as directory:
        root = Path(directory).resolve()
        bin_dir, ws_git, ws_plain = root / "bin", root / "repo", root / "plain"
        for d in (bin_dir, ws_git, ws_plain):
            d.mkdir()
        source = a.fake.read_text(encoding="utf-8")
        for name in ("claude", "codex"):
            exe = bin_dir / name
            exe.write_text("#!%s -I\n%s" % (sys.executable, source), encoding="utf-8")
            exe.chmod(0o755)
        for fixture in ("claude_not_logged_in.jsonl", "codex_unauthorized.jsonl"):
            shutil.copy(a.fake.parent / fixture, bin_dir / fixture)
        (ws_git / "src.txt").write_text("src\n", encoding="utf-8")
        have_git = shutil.which("git") is not None
        if have_git:
            git("init", "-q", cwd=ws_git)
            git("add", "src.txt", cwd=ws_git)
            git("commit", "-q", "-m", "init", cwd=ws_git)
        host = Host(a.cli, a.packages, root)
        cfg_git = host.config("git", ws_git, exec_paths=[bin_dir])
        cfg_plain = host.config("plain", ws_plain, exec_paths=[bin_dir])

        # Catalog: four commands, read-only versus destructive markers.
        cat = subprocess.run([a.cli, "catalog", "--root", str(a.packages), "--config",
                              str(cfg_plain), "--level", "index"],
                             text=True, capture_output=True, timeout=30)
        assert cat.returncode == 0, cat.stderr
        for line in ("- delegate.claude {task, max_turns?, model?}  [read-only] [long-running]",
                     "- delegate.claude-edit {task, max_turns?, model?}  [destructive] [long-running]",
                     "- delegate.codex {task, model?}  [read-only] [long-running]",
                     "- delegate.codex-edit {task, model?}  [destructive] [long-running]"):
            assert line in cat.stdout, (line, cat.stdout)

        # claude (plan): exact flags, parsed stream, usage and identity.
        task = "scenario:success explain the module"
        rc, res, err = host.invoke(cfg_git, "delegate.claude", json.dumps({"task": task}))
        assert rc == 0, err
        rec = record(bin_dir, "claude")
        check_invariants(rec, bin_dir, ws_git, prompt_index=2)
        assert rec["argv"][1:] == ["-p", task] + CLAUDE_PLAN + [rec["argv"][-1]], rec["argv"]
        assert "non-interactively" in rec["argv"][-1] and "read-only" in rec["argv"][-1]
        assert rec["stdin"] == "", "claude reads its task from argv only"
        assert res["ok"] is True and res["agent"] == "claude-code" and res["mode"] == "plan", res
        assert res["final"] == "The answer is 42." and res["final_truncated"] is False, res
        assert res["turns"] == 3 and res["exit_code"] == 0 and res["timed_out"] is False, res
        assert res["usage"] == {"input_tokens": 525, "output_tokens": 50,
                                "cached_input_tokens": 400}, res["usage"]
        assert abs(res["cost_usd"] - 0.0123) < 1e-9, res
        assert res["session_id"] == "6f0c2d41-8b1e-4c3a-9d2e-3c1a5b7e9f02", res
        assert res["model"] == "claude-sonnet-5" and "limit" not in res, res
        assert res["malformed_lines"] == 0 and res["oversized_lines"] == 0, res
        assert res["events"] == 8, res  # unknown event types count, are ignored
        assert "error" not in res and "stderr_tail" not in res, res
        if have_git:
            assert res["changed_files"] == [] and res["changed_files_truncated"] is False, res

        # Optional knobs, and a task that starts like a flag.
        rc, res, err = host.invoke(cfg_git, "delegate.claude",
                                   '{task: "-scenario:success dash", model: "opus[1m]", max_turns: 7}')
        assert rc == 0 and res["ok"], (err, res)
        rec = record(bin_dir, "claude")
        check_invariants(rec, bin_dir, ws_git, prompt_index=2)
        assert rec["argv"][2] == " -scenario:success dash", rec["argv"]
        assert rec["argv"][-4:] == ["--max-turns", "7", "--model", "opus[1m]"], rec["argv"]

        # claude-edit: acceptEdits, write tools only; git evidence, scratch excluded.
        if have_git:
            (ws_git / "src.txt").write_text("src\ndirty before the run\n", encoding="utf-8")
        rc, res, err = host.invoke(cfg_git, "delegate.claude-edit",
                                   '{task: "scenario:edit add new.txt"}')
        assert rc == 0 and res["ok"] and res["mode"] == "edit", (err, res)
        rec = record(bin_dir, "claude")
        check_invariants(rec, bin_dir, ws_git, prompt_index=2)
        argv = rec["argv"]
        assert argv[argv.index("--permission-mode") + 1] == "acceptEdits", argv
        assert argv[argv.index("--tools") + 1] == "Read,Edit,Write,Glob,Grep", argv
        assert "read-only" not in argv[-1], argv[-1]
        if have_git:
            # src.txt was already modified: only its content hash shows the edit.
            assert res["changed_files"] == [{"path": "new.txt", "status": "??"},
                                            {"path": "src.txt", "status": "M"}], res
            git("checkout", "--", "src.txt", cwd=ws_git)
            (ws_git / "new.txt").unlink()

        # Usage limit: reported, structured, not retried.
        rc, res, err = host.invoke(cfg_git, "delegate.claude", '{task: "scenario:limit"}')
        assert rc == 0, err
        record(bin_dir, "claude")
        assert res["ok"] is False and res["exit_code"] == 1, res
        assert res["limit"]["hit"] is True, res
        assert "usage limit" in res["limit"]["message"], res
        assert res["limit"]["resets_at"].startswith("2026-"), res
        assert "do not retry" in res["error"], res

        # Login problems name the fix without touching the login.
        rc, res, err = host.invoke(cfg_git, "delegate.claude", '{task: "scenario:auth"}')
        record(bin_dir, "claude")
        assert rc == 0 and res["ok"] is False, (err, res)
        assert "/login" in res["error"] and "running `claude` once" in res["error"], res
        assert "limit" not in res, res

        # The real CLI, not logged in (recorded from Claude Code 2.1.310).
        rc, res, err = host.invoke(cfg_plain, "delegate.claude", '{task: "scenario:replay"}')
        record(bin_dir, "claude")
        assert rc == 0 and res["ok"] is False and res["exit_code"] == 1, (err, res)
        assert res["error"].startswith("Not logged in \u00b7 Please run /login"), res
        assert "running `claude` once" in res["error"] and "limit" not in res, res
        assert res["model"] == "claude-sonnet-5-5" and res["events"] == 3, res

        # Non-JSON lines interleaved with events are counted and ignored.
        rc, res, err = host.invoke(cfg_git, "delegate.claude", '{task: "scenario:malformed"}')
        record(bin_dir, "claude")
        assert rc == 0 and res["ok"] and res["malformed_lines"] == 4, (err, res)
        assert res["final"] == "The answer is 42.", res

        # Non-zero exit without a result: stderr tail and status in the error.
        rc, res, err = host.invoke(cfg_git, "delegate.claude", '{task: "scenario:exitfail"}')
        record(bin_dir, "claude")
        assert rc == 0 and res["ok"] is False and res["exit_code"] == 3, (err, res)
        assert "exited with status 3" in res["error"], res
        assert "fatal: the fake CLI crashed" in res["stderr_tail"], res
        assert res["final"] == "Starting.", res

        # An older CLI rejecting a flag gets an update hint.
        rc, res, err = host.invoke(cfg_git, "delegate.claude", '{task: "scenario:unknown-option"}')
        record(bin_dir, "claude")
        assert rc == 0 and res["ok"] is False and "update it" in res["error"], (err, res)

        # Exit 0 but no result event is not success; final falls back to the tail.
        rc, res, err = host.invoke(cfg_git, "delegate.claude", '{task: "scenario:noresult"}')
        record(bin_dir, "claude")
        assert rc == 0 and res["ok"] is False, (err, res)
        assert "without a final result" in res["error"], res
        assert res["final"] == "Looking at the code.", res  # subagent text excluded

        # Very large output: bounded line cap, final capped on a UTF-8 boundary.
        rc, res, err = host.invoke(cfg_git, "delegate.claude", '{task: "scenario:big"}')
        record(bin_dir, "claude")
        assert rc == 0 and res["ok"], err
        assert res["oversized_lines"] == 1 and res["events"] > 3000, res
        assert res["final_truncated"] is True and len(res["final"]) == 65536, len(res["final"])

        # codex (read-only), outside a git repository: no changed_files.
        task = "scenario:success why does it crash"
        rc, res, err = host.invoke(cfg_plain, "delegate.codex", json.dumps({"task": task}))
        assert rc == 0, err
        rec = record(bin_dir, "codex")
        check_invariants(rec, bin_dir, ws_plain)
        assert rec["argv"][1:] == ["exec", "--json", "--color", "never", "--sandbox", "read-only",
                                   "--skip-git-repo-check", "--cd", str(ws_plain), "-"], rec["argv"]
        assert rec["stdin"] == task, "codex reads its task from stdin"
        assert res["ok"] is True and res["agent"] == "codex" and res["mode"] == "read-only", res
        assert res["final"] == "Codex final answer." and res["turns"] == 1, res
        assert res["thread_id"] == "0199a213-81c0-7800-8aa1-bbab2a035a53", res
        assert res["usage"] == {"input_tokens": 24763, "output_tokens": 122,
                                "cached_input_tokens": 24448}, res
        assert "session_id" not in res and "cost_usd" not in res, res
        assert "changed_files" not in res and "error" not in res, res

        # codex-edit: workspace-write, network off for its commands, model.
        rc, res, err = host.invoke(cfg_git, "delegate.codex-edit",
                                   '{task: "scenario:edit fix it", model: "gpt-5.1-codex"}')
        assert rc == 0 and res["ok"] and res["mode"] == "edit", (err, res)
        rec = record(bin_dir, "codex")
        check_invariants(rec, bin_dir, ws_git)
        assert rec["argv"][1:] == ["exec", "--json", "--color", "never", "--sandbox",
                                   "workspace-write", "--skip-git-repo-check", "--cd", str(ws_git),
                                   "-c", "sandbox_workspace_write.network_access=false",
                                   "--model", "gpt-5.1-codex", "-"], rec["argv"]
        if have_git:
            assert res["changed_files"] == [{"path": "new.txt", "status": "??"},
                                            {"path": "src.txt", "status": "M"}], res
            git("checkout", "--", "src.txt", cwd=ws_git)
            (ws_git / "new.txt").unlink()

        # codex usage limit.
        rc, res, err = host.invoke(cfg_plain, "delegate.codex", '{task: "scenario:limit"}')
        record(bin_dir, "codex")
        assert rc == 0 and res["ok"] is False and res["limit"]["hit"] is True, (err, res)
        assert "usage limit" in res["limit"]["message"] and "do not retry" in res["error"], res

        # The real codex, not logged in (recorded from codex-cli 0.164.0): its
        # transient "Reconnecting..." error events are not the failure.
        rc, res, err = host.invoke(cfg_plain, "delegate.codex", '{task: "scenario:replay"}')
        record(bin_dir, "codex")
        assert rc == 0 and res["ok"] is False and res["exit_code"] == 1, (err, res)
        assert res["error"].startswith("unexpected status 401 Unauthorized"), res
        assert "codex login" in res["error"] and "limit" not in res, res
        assert res["thread_id"] == "01a17a0a-6d84-7050-8e20-b56c207cfe88", res
        assert res["events"] == 15 and res["final"] == "", res

        # Input validation happens before anything is started.
        rc, res, err = host.invoke(cfg_plain, "delegate.codex", '{task: "x", model: "--bare"}')
        assert rc == 1 and "delegate/bad-input" in err, (rc, err)
        assert not (bin_dir / "record-codex.json").exists(), "CLI started on bad input"

        # Missing CLI: an actionable error naming install, login and executable_paths.
        cfg_none = host.config("none", ws_plain)
        rc, res, err = host.invoke(cfg_none, "delegate.claude", '{task: "x"}')
        assert rc == 1 and "delegate/not-installed" in err, (rc, err)
        assert "sandbox.executable_paths" in err and "log in" in err, err

        # Deny-by-default: no proc/net grant, no spawn.
        cfg_denied = host.config("denied", ws_plain, exec_paths=[bin_dir], grant=False)
        rc, res, err = host.invoke(cfg_denied, "delegate.codex", '{task: "x"}')
        assert rc == 2 and "proc grant" in err, (rc, err)
        assert not (bin_dir / "record-codex.json").exists(), "CLI started without grants"

        # Strict level: the jail hides the account's home; stop before spawning.
        cfg_strict = host.config("strict", ws_plain, level="strict", exec_paths=[bin_dir])
        rc, res, err = host.invoke(cfg_strict, "delegate.claude", '{task: "x"}')
        if "strict unavailable" in err:
            print("delegate: strict sandbox unavailable here; strict case skipped")
        else:
            assert rc == 1 and "delegate/sandbox" in err and '"basic"' in err, (rc, err)
            assert not (bin_dir / "record-claude.json").exists(), "CLI started under strict"
    print("delegate: flags, env hygiene, stream parsing, limits, caps, git evidence, "
          "denials and strict refusal passed")


if __name__ == "__main__":
    main()
