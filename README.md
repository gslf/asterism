# ⁂ asterism

A personal AI agent that runs on your own computer. You talk to it from
Telegram or from the terminal. It uses small local models, so running it costs
nothing, and it can pass bigger jobs to your ChatGPT or Claude subscription.

## Install

Linux and macOS:

```bash
curl -fsSL https://raw.githubusercontent.com/gslf/asterism/main/install.sh | sh
```

Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/gslf/asterism/main/install.ps1 | iex
```

The installer asks a few things:

1. the model size for your computer (it suggests one);
2. your Telegram bot token: in Telegram, open [@BotFather](https://t.me/BotFather),
   send `/newbot` and copy the token it gives you;
3. to open a link on your phone, which connects your Telegram account;
4. whether to sign in with ChatGPT (you can skip it);
5. whether the agent should start by itself when you log in.

Then it downloads the models (about 4 GB for the smallest size). If the
download stops, run `asterism setup` and it continues where it left off.

## Start and stop

```bash
asterism start     # run the agent in the background
asterism stop      # stop it and free the memory it uses
asterism status    # is it running?
```

`asterism setup --manual` makes it run only when you start it;
`asterism setup --autostart` makes it start by itself when you log in.
To watch it work, run `asterism serve` in a terminal instead.

## Use it

On Telegram, write to your bot: text, voice notes, photos, files. Before it
changes a file or uses the internet, it asks you with Yes/No buttons.

| Command | What it does |
|---|---|
| `/new` | start a new task in its own thread |
| `/stop` | stop the current task |
| `/status` | show what is running |
| `/project` | work in a folder (see below) |
| `/routine` | repeat a task, e.g. `/routine add every day at 8 \| summarize my mail` |
| `/help` | all the commands |

In the terminal, `asterism` opens a chat and `asterism --once "your question"`
answers one question.

## Projects

A project is a folder the agent works in.

```
/project new myapp               creates ~/asterism/projects/myapp
/project new site ~/code/site    uses a folder you already have
```

Each project gets its own Telegram thread. There the agent works only in that
folder and asks before every change. `/project` lists your projects,
`/project off` leaves one, `/project rm myapp` removes it from the list (the
folder stays). From the terminal: `asterism --project myapp`.

## Change the model

There are three sizes:

| Size | For | Download |
|---|---|---|
| `base` | 8 GB of memory, no graphics card | 3.9 GB |
| `standard` | a graphics card with 8 GB | 6.8 GB |
| `plus` | a graphics card with 16 GB | 11.4 GB |

```bash
asterism setup --tier standard
```

This downloads the new models and saves your old settings as
`~/asterism/config.xcdn.bak`. If the agent is running, it restarts it.

The ready-made downloads use the processor, or the graphics chip on Apple
computers. To use an NVIDIA card, install CUDA and run the installer with
`--from-source`.

To use a model file of your choice (GGUF), put it in `~/asterism/models/` and
change the `path` of the model called `std` in `~/asterism/config.xcdn`.

## Use your subscriptions

**ChatGPT Plus or Pro.**

1. Run `asterism login chatgpt` and approve in the browser. It then lists the
   models your plan offers.
2. In `~/asterism/config.xcdn`, add this line inside `pool: [ ... ]`, with one
   of those models:

   ```
   { id: "plan", provider: "chatgpt", model: "MODEL-FROM-THE-LIST", ctx: 131072 },
   ```

   and in `roles` set `planner: "plan"` and `generator: "plan"`.
3. Run `asterism stop`, then `asterism start`.

ChatGPT now writes the answers; the local models keep doing the small jobs.

**Codex and Claude Code.** Install the program (`codex` or `claude`), log in
with it (`codex login`, or `claude` and then `/login`), then run
`asterism setup` again so the agent may start it. The agent can then hand big
coding tasks to it, and asks you before each one. asterism never reads their
login files.

## Passwords for tools

```bash
asterism vault set mail-password --tool imap --domain imap.example.com
```

It asks for the password without showing it, and stores it encrypted. The model only sees
`{{secret:mail-password}}`; only the tool you name gets the real value.

## Update, check, remove

Run the install command again to update. `asterism doctor` checks that
everything is in place. To remove asterism (your data in `~/asterism` stays):

```bash
curl -fsSL https://raw.githubusercontent.com/gslf/asterism/main/install.sh | sh -s -- --uninstall
```

## Build from source

```bash
git clone --recursive https://github.com/gslf/asterism
cd asterism
cmake -S . -B build && cmake --build build -j
ctest --test-dir build
```

Python and JavaScript clients: [sdk/](sdk).

## Learn more

- [Memory](docs/MEMORY.md): what the agent remembers, and how.
- [Tools](docs/TOOLS.md): what the agent can do, and what needs your OK.
- [Architecture](docs/ARCHITECTURE.md): how the code is organized.

## License

MIT, see [LICENSE](LICENSE).
