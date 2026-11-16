# Tools

Tools are how the agent does things: read and change files, search, build and
test, use git, pass work to Codex or Claude Code. They are the `astools`
module; they live in `~/asterism/tools/`.

## The standard tools

| Tool | What it does |
|---|---|
| `fs` | read, write, list, copy, move and delete files |
| `edit` | change part of a file |
| `grep` | search text in files |
| `code` | read pieces of code, find names, apply patches |
| `git` | status, diff, log, commit |
| `project` | build, test, check and format the project |
| `proc` | run a program |
| `env` | read the environment variables you allow |
| `sys` | report the system: OS, processor, memory |
| `delegate` | hand a coding task to Codex or Claude Code |

## Editing by line anchor

The most reliable way for a model to change a file:

- `fs.read` with `anchors: true` shows every line as `LINE:TAG|text`, for
  example `42:ab3|int x = 1;`. The tag is three characters made from the
  line's content (spaces at the end of the line do not count).
  `code.read-range` and `grep.search` take `anchors: true` too.
- `edit.lines` takes a file and a list of changes. Each change names a line
  (`start: "42:ab3"`) or a range (`start` and `end`) and an action:
  `replace`, `delete`, `insert_before` or `insert_after`, with the new `text`.
  The model never has to repeat the old text.
- If a line number or tag no longer matches the file, nothing is changed and
  the error shows the line as it is now; the agent reads again and retries.
- Two changes in one call may not touch the same lines.

`edit.replace`, `edit.insert` and `edit.patch` still work as before.

## Where they work

The tools work only inside the work folder: the conversation's own folder,
or the project's folder when you choose one (see Projects in the README).

## What needs your OK

- Reading and searching: nothing to approve.
- Changing files, running programs, using the internet: the agent asks
  first. On Telegram you answer with buttons; "Always" stops the question for
  that action in that folder.
- After the agent has read something from outside (a forwarded message, a web
  page, another agent's answer), it asks every time and "Always" is not
  offered.

## The permissions file

`~/asterism/astools.xcdn`, written by `asterism setup`, says what the tools may
do:

- `workspace_access: "read-write"` lets them change files in the work folder;
  `"read-only"` forbids it;
- `grants` says which tools may start programs (`proc: true`) or use the
  internet (`net: true`);
- `executable_paths` lists the folders they may start programs from. Setup
  adds Codex and Claude Code there when it finds them, so install those first
  and then run `asterism setup` again.

After changing it, run `asterism stop` and `asterism start`. In the terminal
chat, `/tools` lists the tools and `/perms` turns them on or off.

## Safety

- Every tool call runs as a separate program, with a time limit, a cap on its
  output and a clean environment. A tool that crashes or hangs cannot take the
  agent down.
- On Linux, `sandbox: { default_level: "strict" }` in the permissions file
  also has the system lock each tool inside the work folder and cut its
  internet unless it is allowed.
- A password reaches a tool only if you stored it for that tool (see the
  README).

## Add your own tool

A tool is a folder with a `manifest.xcdn`, which describes its commands and
what they need, and a program written in any language.

```bash
asterism tools check ./mytool      # is the manifest valid?
cp -r ./mytool ~/asterism/tools/
```

Then run `asterism stop` and `asterism start`.
