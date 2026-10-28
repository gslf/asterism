# Memory

asterism remembers what you tell it, from one conversation to the next and
after a restart. The memory is the `asper` module; it lives in
`~/asterism/memory/`.

## How it works

1. **Everything is kept.** Every message, answer and tool result is saved
   word for word. Nothing in this record is summarized away.
2. **Notes are written in the background.** When the agent is idle, a local
   model reads the latest conversation and writes short notes: who you are,
   what you prefer, what you are working on.
3. **Only the useful notes reach the model.** Before each answer the agent
   picks the notes that match your request, up to a fixed size, so a small
   model is not flooded.

## Three kinds of notes

| Kind | What | Example |
|---|---|---|
| About you | who you are, how you like things done | "prefers answers in Italian" |
| General | facts that matter across topics | "the NAS is at 192.168.1.10" |
| Project | facts about one project | "myapp builds with cmake" |

Project notes stay with their project: what the agent learns about `myapp`
does not show up while you work on `site`. A note about you changes only when
the same thing comes up twice within a week, so one odd remark does not
rewrite it.

## What it does not do

- Text you forward, web pages and answers from other agents are kept in the
  record but never turned into notes.
- Notes that are never used fade over time and are eventually deleted.
- Passwords never get there: the model only sees `{{secret:NAME}}`.
- There is no command yet to delete a single note.

Changing the model size keeps the memory: all sizes use the same search model.

## Use it

- On Telegram: `/memory where is the NAS?` answers from the memory.
- In the terminal chat: `/memory <question>` does the same, and `/pin` keeps
  the last exchange in view even when the conversation gets long.

## Settings

The defaults suit most uses. To change them (how much memory goes into each
answer, how fast notes fade), copy `examples/asper.xcdn` to
`~/asterism/asper.xcdn`, and in `config.xcdn` add `asper: { config:
"asper.xcdn" },` inside `integration`. Then run `asterism stop` and
`asterism start`.
