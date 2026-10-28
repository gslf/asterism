# Example settings

`asterism setup` writes the settings you need. These files are for when you
want something different: copy one into `~/asterism/` under the name shown.

| File | Save as | What it is |
|---|---|---|
| `base.xcdn`, `standard.xcdn`, `plus.xcdn` | `config.xcdn` | the three model sizes |
| `chatgpt.xcdn` | `config.xcdn` | ChatGPT writes the answers, a local model does the rest |
| `lmstudio.xcdn` | `config.xcdn` | use a model server (LM Studio, llama-server) instead of model files |
| `embedded.xcdn` | `config.xcdn` | local model files, set up for programming work |
| `serve.xcdn` | `serve.xcdn` | every Telegram setting |
| `astools.xcdn` | `astools.xcdn` | which tools run and what they may touch |
| `asper.xcdn` | `asper.xcdn` | memory settings |

After changing a file, run `asterism stop` and `asterism start`.
