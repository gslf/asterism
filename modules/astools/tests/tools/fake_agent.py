"""Scripted stand-in for the `claude` and `codex` CLIs (delegate tests).

test_delegate.py installs this file under both names in a temporary
directory that the host adds to sandbox.executable_paths. It records how it
was started (argv, environment, cwd, stdin) next to itself, then plays the
event stream named by a "scenario:<name>" word in the task; "replay" plays
a stream recorded from the real CLI when it is not logged in. The streams
follow the documented shapes of `claude -p --output-format stream-json
--verbose` and `codex exec --json`; nothing here talks to a network.
"""
import json
import os
from pathlib import Path
import sys

NAME = Path(sys.argv[0]).name
HERE = Path(__file__).resolve().parent
SESSION = "6f0c2d41-8b1e-4c3a-9d2e-3c1a5b7e9f02"
THREAD = "0199a213-81c0-7800-8aa1-bbab2a035a53"


def emit(event):
    sys.stdout.write(json.dumps(event) + "\n")
    sys.stdout.flush()


def raw(text):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def edit_workspace():
    with open("src.txt", "a", encoding="utf-8") as f:
        f.write("edited by the fake agent\n")
    Path("new.txt").write_text("new\n", encoding="utf-8")
    # CLI temp files land in TMPDIR (the invocation scratch): not evidence.
    Path(os.environ["TMPDIR"], "cli-temp.txt").write_text("tmp\n", encoding="utf-8")


def assistant(text, parent=None):
    emit({"type": "assistant", "session_id": SESSION, "parent_tool_use_id": parent,
          "message": {"role": "assistant", "content": [{"type": "text", "text": text}]}})


def claude_result(text, is_error=False, turns=3, subtype="success"):
    emit({"type": "result", "subtype": subtype, "is_error": is_error, "result": text,
          "num_turns": turns, "duration_ms": 1234, "duration_api_ms": 1000,
          "total_cost_usd": 0.0123, "session_id": SESSION, "permission_denials": [],
          "usage": {"input_tokens": 100, "output_tokens": 50,
                    "cache_read_input_tokens": 400, "cache_creation_input_tokens": 25}})


def replay(fixture):
    """Real streams recorded from unauthenticated CLIs (sanitized)."""
    sys.stdout.write((HERE / fixture).read_text(encoding="utf-8"))
    sys.stdout.flush()
    return 1


def claude(scenario):
    if scenario == "replay":
        return replay("claude_not_logged_in.jsonl")
    if scenario == "unknown-option":
        sys.stderr.write("error: unknown option '--setting-sources'\n")
        return 1
    emit({"type": "system", "subtype": "init", "session_id": SESSION,
          "model": "claude-sonnet-5", "tools": ["Read", "Glob", "Grep"],
          "permissionMode": "plan", "cwd": os.getcwd(), "apiKeySource": "none",
          "claude_code_version": "2.1.310", "mcp_servers": []})
    if scenario == "limit":
        emit({"type": "rate_limit_event", "session_id": SESSION,
              "rate_limit_info": {"status": "rejected", "resetsAt": 1793000000}})
        emit({"type": "assistant", "session_id": SESSION, "parent_tool_use_id": None,
              "error": "rate_limit",
              "message": {"content": [{"type": "text",
                                       "text": "Claude AI usage limit reached|1793000000"}]}})
        claude_result("Claude AI usage limit reached|1793000000", is_error=True, turns=1)
        return 1
    if scenario == "auth":
        claude_result("Invalid API key · Please run /login", is_error=True, turns=0)
        return 1
    if scenario == "exitfail":
        assistant("Starting.")
        sys.stderr.write("fatal: the fake CLI crashed\n")
        return 3
    if scenario == "malformed":
        raw("Warning: this line is not JSON")
        raw("{broken")
        raw("[1, 2, 3]")
        raw("")
        raw('{"no_type": true}')
    if scenario == "edit":
        edit_workspace()
    final = "The answer is 42."
    if scenario == "big":
        for i in range(3000):
            assistant("progress %d %s" % (i, "x" * 100))
        # One tool result over the 8 MiB line cap: skipped, not parsed.
        raw(json.dumps({"type": "user", "message": {"content": [
            {"type": "tool_result", "tool_use_id": "toolu_1", "content": "y" * (9 << 20)}]}}))
        final = "z" * (200 * 1024)
    assistant("Looking at the code.")
    emit({"type": "assistant", "session_id": SESSION, "parent_tool_use_id": None,
          "message": {"content": [{"type": "tool_use", "id": "toolu_1", "name": "Read",
                                   "input": {"file_path": "src.txt"}}]}})
    emit({"type": "user", "session_id": SESSION, "parent_tool_use_id": None,
          "message": {"content": [{"type": "tool_result", "tool_use_id": "toolu_1",
                                   "content": "src\n"}]}})
    assistant("subagent chatter", parent="toolu_9")
    emit({"type": "system", "subtype": "api_retry", "attempt": 1, "max_retries": 10,
          "retry_delay_ms": 500, "error_status": 529, "error": "overloaded",
          "session_id": SESSION})
    emit({"type": "event_from_the_future", "payload": {"x": 1}})
    if scenario == "noresult":
        return 0
    claude_result(final)
    return 0


def codex(scenario):
    if scenario == "replay":
        return replay("codex_unauthorized.jsonl")
    emit({"type": "thread.started", "thread_id": THREAD})
    emit({"type": "turn.started"})
    if scenario == "limit":
        msg = ("You've hit your usage limit. Upgrade to Pro (https://chatgpt.com/explore/pro), "
               "or try again in 2 hours.")
        emit({"type": "error", "message": msg})
        emit({"type": "turn.failed", "error": {"message": msg}})
        return 1
    if scenario == "edit":
        edit_workspace()
    emit({"type": "item.started", "item": {"id": "item_1", "type": "command_execution",
                                           "command": "bash -lc ls", "status": "in_progress"}})
    emit({"type": "item.completed", "item": {"id": "item_1", "type": "command_execution",
                                             "command": "bash -lc ls",
                                             "aggregated_output": "src.txt\n",
                                             "exit_code": 0, "status": "completed"}})
    emit({"type": "item.completed", "item": {"id": "item_2", "type": "reasoning",
                                             "text": "Thinking about it."}})
    emit({"type": "item.completed", "item": {"id": "item_3", "type": "error",
                                             "message": "a non-fatal warning"}})
    emit({"type": "item.completed", "item": {"id": "item_4", "type": "agent_message",
                                             "text": "Codex final answer."}})
    emit({"type": "turn.completed", "usage": {"input_tokens": 24763, "cached_input_tokens": 24448,
                                              "output_tokens": 122,
                                              "reasoning_output_tokens": 0}})
    return 0


def main():
    stdin = sys.stdin.read()
    argv = sys.argv[1:]
    if NAME == "claude":
        task = argv[argv.index("-p") + 1] if "-p" in argv else ""
    else:
        task = stdin
    record = {"argv": sys.argv, "env": dict(os.environ), "cwd": os.getcwd(), "stdin": stdin}
    (HERE / ("record-%s.json" % NAME)).write_text(json.dumps(record), encoding="utf-8")
    scenario = "success"
    for word in task.split():
        if word.startswith("scenario:"):
            scenario = word[len("scenario:"):]
    return claude(scenario) if NAME == "claude" else codex(scenario)


if __name__ == "__main__":
    sys.exit(main())
