# How asterism is built

asterism is one C program, `asterism`, built with CMake. Nothing else needs to
be installed or kept running next to it.

## Folders

| Folder | What is in it |
|---|---|
| `asngn/` | the application: the agent, the terminal chat, the Telegram daemon, the MCP server, setup |
| `modules/core/` | basics everything shares: files, threads, hashing, logs |
| `modules/asmodel/` | runs the models: local files through llama.cpp, or ChatGPT |
| `modules/asper/` | the agent's memory |
| `modules/astools/` | the tools, and the sandbox they run in |
| `modules/aslink/` | Telegram |
| `modules/astime/` | repeated and scheduled tasks |
| `modules/asvault/` | encrypted passwords and tokens |
| `sdk/` | Python and JavaScript clients |
| `third_party/` | llama.cpp and xCDN-C, unchanged (git submodules) |

Each module is a separate library with its own header and its own tests.
Modules never depend on the application.

## Where the data lives

Everything is in one folder, `~/asterism` by default:

| Path | What |
|---|---|
| `config.xcdn` | which models to use, and the settings |
| `models/` | model files |
| `tools/`, `astools.xcdn` | tools, and what they may do ([TOOLS.md](TOOLS.md)) |
| `memory/` | what the agent remembers ([MEMORY.md](MEMORY.md)) |
| `sessions/` | conversations; each has its own work folder |
| `projects/`, `projects.xcdn` | projects |
| `vault/` | encrypted secrets |
| `serve.xcdn`, `serve/` | Telegram settings and state |

The files are plain text, written so that a crash never leaves one half
written.

## Rules the code follows

- **Local first.** Small local models do the routine work. A subscription is
  used only where `config.xcdn` says so.
- **Tools are separate programs.** Each one gets only the permissions it
  declares and you allow. A broken tool cannot crash the agent.
- **It asks before acting.** Changing files, running commands or using the
  internet needs your OK. Text from outside (forwarded messages, web pages,
  other agents) is marked as untrusted; after reading it, the agent asks for
  every such action, with no exceptions.
- **Nothing listens on the network.** The agent fetches its Telegram messages
  itself; no port is opened.
- **The model never sees secrets.** It sees `{{secret:NAME}}`; only the tools
  allowed to use a secret receive its value.
- **Subscriptions only through official programs.** Codex and Claude Code run
  as their own programs, with your own login.
- **Projects keep work in one place.** A session works in its own folder, or
  in a project's folder when you choose one. One task at a time per project.
- **Tests need no models and no internet.** Models are faked and test servers
  run on the same computer. `ctest --test-dir build` runs everything in
  seconds.
