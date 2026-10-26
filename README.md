# ⁂ asterism

An agent harness that turns language models, small and local ones included,
into tools that complete real workflows, with evidence-driven execution.

| Part | Role |
|---|---|
| [`asngn/`](asngn) | The application: the engine and its front-ends `asngn` (TUI/CLI), `asngn-mcp`, `asngn-acp` |
| [`modules/core/`](modules/core) | Platform layer and utilities shared by every module |
| [`modules/asmodel/`](modules/asmodel) | Inference runtime: model pool, budgets, HTTP and llama.cpp providers |
| [`modules/asper/`](modules/asper) | Memory: exact events, semantic records, bounded context |
| [`modules/astools/`](modules/astools) | Tools: registry, policy, sandboxed execution, standard tools |
| [`sdk/`](sdk) | Python and JavaScript clients for `asngn-mcp` |

Why it is built this way: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build

Needs git, CMake ≥ 3.16 and a C99 compiler (plus C++ for embedded llama.cpp).
libcurl enables remote model providers on Linux and macOS.

```bash
git submodule update --init      # third_party/: xCDN-C, llama.cpp
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```

Executables land in `build/bin/`, tool packages in `build/packages/`.
On Windows (MSVC) add `--config Release` to the build and `-C Release` to ctest.

| Option | Default | |
|---|---|---|
| `ASTERISM_WITH_LLAMA` | ON | Embedded llama.cpp inference. OFF builds a remote-only, C-only engine |
| `ASTERISM_BUILD_TESTS` | ON | Test suite (no model weights or GPU needed) |
| `ASTERISM_SANITIZERS` | OFF | ASan + UBSan |
| `ASTERISM_NO_THREADS` | OFF | Single-threaded library (`asngn_tick`); no front-ends |
| `ASTERISM_BUILD_FUZZERS` | OFF | libFuzzer parser targets (Clang) |
| `GGML_CUDA` / `GGML_METAL` | OFF | GPU backend for llama.cpp |

## Run

The engine keeps all state in one engine root (default `~/asngn`). The
built-in defaults expect the reference models and the standard tools there:

```bash
scripts/fetch-models.sh ~/asngn --install-tools   # verified GGUF models + build/packages
```

(`scripts/fetch-models.ps1` on Windows.)

```bash
build/bin/asngn                        # terminal UI (F1: keys and commands)
build/bin/asngn --once "Summarize this workspace"
build/bin/asngn --doctor               # check configuration and models
build/bin/asngn-mcp --root ~/asngn     # MCP server over stdio
build/bin/asngn-acp --workspace /path/to/repo --config ~/asngn/config.xcdn
```

`<engine-root>/config.xcdn` is optional. [`examples/`](examples) holds starter
configurations, including one for an OpenAI-compatible server (llama-server,
LM Studio, vLLM) instead of local weights.

## License

MIT, see [LICENSE](LICENSE).
