# Architecture

One C99 project: an application, four modules and two third-party
libraries, built by one CMake tree into self-contained executables.

```
asngn/           application
  src/           engine (libasngn): sessions, turn loop, context, safety, accounting
  tui/           asngn      terminal UI and one-shot CLI
  mcp/           asngn-mcp  MCP server (stdio)
  acp/           asngn-acp  editor host, Agent Client Protocol (POSIX)
modules/
  core/          platform and utilities: threads, files, clocks, buffers, hashes, log
  asmodel/       inference runtime: model pool, budgets, providers (HTTP, llama.cpp),
                 and the JSON codec (asmodel_json)
  asper/         memory: exact events, semantic records, bounded context
  astools/       tools: registry, manifests, policy, sandbox, standard tools
sdk/             Python and JavaScript clients of asngn-mcp
third_party/     xCDN-C, llama.cpp (git submodules)
```

```
asngn ──► asper ──► asmodel ──► core
  ├─────► astools ────────────► core
  │          └────► asmodel_json
  └─────► asmodel
```

Every module depends on `core`; nothing depends on the application.

## Decisions

**One repository, one build, one version.**
A change that crosses a module boundary lands in one commit and is tested as a
whole. There are no pins, ABI checks or release manifests to keep in sync.

**One application, independent modules.**
`asngn` is the only application. Each module is a static library with its own
public header (`core.h`, `asmodel.h`, `asper.h`, `astools.h`), error codes and
tests, and knows nothing about the application. Dependencies point one
way. Every module can be built and tested alone; policy is composed in one place.

**One platform layer.**
Threads, files, clocks, buffers, SHA-256, UUIDs, time formats and the log sink
exist once, in `core`, behind one header (`core.h`). Fallible core calls return
`core_err`; each module maps it to its own error codes in one place (its
`os.h` and internal header), so module code keeps a single error type.
Platform code that only one module needs (process supervision in astools,
private files in asper) stays in that module. The modules' tests share one
harness (`core_test.h`).

**One JSON codec.**
MCP, LSP and provider wire formats share one strict RFC 8259 codec,
`asmodel_json`. It depends only on libc and is a library of its own, so
astools speaks MCP and LSP without linking the model providers.

**One process, static linking.**
The engine calls modules in-process and the front-ends embed all project code
and llama.cpp. No IPC between components, nothing to version separately.
Product executables share `bin/`, because the strict sandbox expects
`astools-jail` next to the host.

**One MCP server.**
Only `asngn-mcp` is exposed. Memory and tools are reached through the engine,
which applies approvals, budgets, redaction and safety gates. A server per
module would bypass that policy and repeat the same protocol code.

**One model runtime.**
`asmodel` holds every provider: OpenAI-compatible HTTP and embedded llama.cpp.
The engine owns the pool and lends it to `asper`: curation uses the
`compressor` role, retrieval the `embedder` role. `asper` never loads a model
itself. Weights load once, under one RAM/VRAM budget and one consumption
ledger, and embedded GGUF models and remote endpoints mix freely in one pool.

**One engine root.**
All state lives under one directory: `config.xcdn`, `memory/`, `tools/`,
`sessions/`, `cache/`, `telemetry/`. Module settings stay in their own files
(`asper.xcdn`, `astools.xcdn`), referenced from `config.xcdn`, because each
module validates its own schema.

**Constrained decisions.**
The model answers one action per step (`call`, `discover`, `recall`, `open`,
`think`, `clarify`, `answer`), enforced by a GBNF grammar or JSON Schema.
Small local models stay on protocol, and every step is validated before it runs.

**Exact memory first.**
`asper` keeps every event verbatim in an append-only log. Semantic records are
derived by a background curator. Each prompt receives a bounded view with
explicit token accounting. Nothing is lost to summarisation; context size is a
budget, not a hope.

**Tools run out of process.**
A tool is a directory: a `manifest.xcdn` (typed contract plus the text the
model reads) and a binary. Permissions are deny-by-default: the manifest
requests, the host grants, the effective set is the intersection. Each call
runs in a child process with a scrubbed environment, a deadline, output caps
and, at the strict level, kernel confinement where the platform provides it.
A failing or hostile tool cannot crash the engine or exceed its grants, and
tools can be written in any language. `astools-check` validates, previews and
invokes packages for tool authors.

**Inspectable, crash-safe state.**
State is xCDN text or append-only logs, written with atomic replace under a
single-writer lock per store. A crash loses at most an uncommitted tail, which
is detected on load.

**Local front-ends only.**
TUI, MCP and ACP are thin layers over `libasngn`; the SDKs drive `asngn-mcp`
over stdio. There is no network listener: the operating-system process is the
trust boundary.

**Third-party code is never patched.**
xCDN-C and llama.cpp are submodules under `third_party/`, built unmodified.
llama.cpp is optional (`ASTERISM_WITH_LLAMA=OFF` gives a remote-only, C-only
build). Only `asmodel` touches it, and C++ exceptions stop at the C boundary
(`llama_guard.cpp`). Upgrading a dependency is a submodule bump.

**Tests need no weights.**
Models are scripted fakes; tools run through the real registry with fixture
tools. Python drives the checks where a process boundary is the subject (MCP,
ACP, SDKs, sandbox, offline store maintenance). CTest names are
`<module>.<test>`. The whole suite runs anywhere in seconds.
