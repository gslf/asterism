# Starter configurations

Copy what you need into the engine root (default `~/asterism`), never use this
directory as one. Relative paths resolve against the engine root.

| File | Copy to | Purpose |
|---|---|---|
| `embedded.xcdn` | `config.xcdn` | Local GGUF models under `models/` |
| `lmstudio.xcdn` | `config.xcdn` | OpenAI-compatible server; replace the `REPLACE_WITH_…` markers |
| `astools.xcdn` | `astools.xcdn` | Tool registry and sandbox policy |
| `asper.xcdn` | `asper.xcdn` | Memory settings; enable with `integration: { asper: { config: "asper.xcdn" } }` |

Copy the standard tools from `build/packages/` into `<engine-root>/tools/`.
