# ⁂ asterism

An agent harness that turns language models, small and local ones included,
into tools that complete real workflows, with evidence-driven execution.

| Part | Role |
|---|---|
| [`asngn/`](asngn) | The application: the engine and its modes (terminal UI, MCP, ACP) in the one `asterism` executable |
| [`modules/core/`](modules/core) | Platform layer and utilities shared by every module |
| [`modules/asmodel/`](modules/asmodel) | Inference runtime: model pool, budgets, HTTP and llama.cpp providers |
| [`modules/asper/`](modules/asper) | Memory: exact events, semantic records, bounded context |
| [`modules/astools/`](modules/astools) | Tools: registry, policy, sandboxed execution, standard tools |
| [`sdk/`](sdk) | Python and JavaScript clients for `asterism mcp` |

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

The executable is `build/bin/asterism`; the standard tool packages land in
`build/packages/`.
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

The engine keeps all state in one engine root (default `~/asterism`). The
built-in defaults expect the reference models and the standard tools there:

```bash
scripts/fetch-models.sh ~/asterism --install-tools   # verified GGUF models + build/packages
```

(`scripts/fetch-models.ps1` on Windows.)

```bash
asterism                                    # terminal UI (F1: keys and commands)
asterism --once "Summarize this workspace"  # one headless turn
asterism doctor                             # check configuration and models
asterism mcp                                # MCP server on stdio
asterism acp --workspace /path/to/repo      # editor host on stdio (ACP)
asterism tools check path/to/package        # tool authors: check, catalog, invoke
```

One executable, the mode is the first parameter; `asterism <mode> --help`
lists its options. The engine modes take `--root <dir>` and `--config <file>`.

`<engine-root>/config.xcdn` is optional. [`examples/`](examples) holds starter
configurations, including one for an OpenAI-compatible server (llama-server,
LM Studio, vLLM) instead of local weights.

## License

MIT, see [LICENSE](LICENSE).
