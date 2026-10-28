# astools — tool registry, manifests, policy and supervised execution.

if(MSVC)
  # int64 narrowing is range-checked at the call sites; local shadowing is
  # not part of the GCC/Clang warning set either.
  add_compile_options(/wd4244 /wd4456)
  add_compile_definitions(_CRT_NONSTDC_NO_WARNINGS)
else()
  # pthread rwlocks, realpath, strtok_r.
  add_compile_definitions(_XOPEN_SOURCE=700)
endif()

# Package slug for the current platform (registry entry selection).
if(WIN32)
  set(ASTOOLS_PLAT_OS windows)
elseif(APPLE)
  set(ASTOOLS_PLAT_OS macos)
else()
  set(ASTOOLS_PLAT_OS linux)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  set(ASTOOLS_PLAT "${ASTOOLS_PLAT_OS}-arm64")
else()
  set(ASTOOLS_PLAT "${ASTOOLS_PLAT_OS}-x86_64")
endif()

# ── library ─────────────────────────────────────────────────────────────────
set(ASTOOLS_SOURCES
  src/duration.c src/log.c
  src/types.c src/manifest.c
  src/semver.c src/registry.c src/lockfile.c src/config.c src/path.c
  src/policy.c src/proto.c src/invoke.c src/worker.c
  src/execution_common.c src/execution_oneshot.c src/execution_library.c
  src/execution_queue.c
  src/process_pool.c src/process_io.c src/process_frame.c
  src/process_start.c src/process_exchange.c
  src/rpc.c
  src/lsp_manifest.c src/lsp_uri.c src/lsp_file.c src/lsp_rpc.c
  src/lsp_initialize.c src/lsp_results.c src/lsp_query.c
  src/mcp_contract.c src/mcp_request.c src/mcp_schema_profile.c
  src/mcp_schema_validate.c src/mcp_initialize.c src/mcp_discovery.c
  src/mcp_invoke.c
  src/catalog.c src/selection.c src/discovery.c src/discovery_view.c
  src/command_schema.c src/gbnf.c src/callline.c src/jscm.c
  src/api.c src/cli.c)
if(WIN32)
  list(APPEND ASTOOLS_SOURCES src/os_proc_win32.c src/sandbox_win32.c)
else()
  list(APPEND ASTOOLS_SOURCES src/os_proc_posix.c src/sandbox_posix.c src/jail.c)
endif()

add_library(astools STATIC ${ASTOOLS_SOURCES})
target_include_directories(astools PUBLIC include PRIVATE src ${ASTERISM_XCDN_SRC})
target_link_libraries(astools PUBLIC asmodel_json core xcdn)
asterism_strict(astools)
if(NOT WIN32)
  target_link_libraries(astools PUBLIC ${CMAKE_DL_LIBS} m)
endif()

# ── astools-std: the standard tools, one busybox-style binary ───────────────
set(ASTOOLS_STD_TOOLS fs grep edit sys env proc git code project delegate)
set(ASTOOLS_STD_SOURCES
  std/main.c std/version.c std/sdk.c std/regex.c
  std/tool_fs.c std/tool_grep.c std/tool_edit.c std/tool_sys.c std/tool_env.c
  std/tool_proc.c std/process_capture.c std/process_output.c std/tool_git.c
  std/tool_code.c std/tool_project.c std/project_receipt.c
  std/tool_delegate.c std/delegate_events.c std/delegate_run.c
  src/duration.c)
if(WIN32)
  list(APPEND ASTOOLS_STD_SOURCES std/compat_win32.c)
endif()
add_executable(astools-std ${ASTOOLS_STD_SOURCES})
target_include_directories(astools-std PRIVATE src std ${ASTERISM_XCDN_SRC})
# asmodel_json: the delegate parses the agent CLIs' JSON event streams.
target_link_libraries(astools-std PRIVATE core xcdn asmodel_json)
asterism_strict(astools-std)

# Ready-to-copy registry: <build>/packages/<tool>/{manifest.xcdn,bin/<plat>/}.
set(ASTOOLS_PKG_DIR ${CMAKE_BINARY_DIR}/packages)
set(ASTOOLS_PKG_STAMPS)
foreach(tool ${ASTOOLS_STD_TOOLS})
  set(manifest ${CMAKE_CURRENT_SOURCE_DIR}/packages/${tool}/manifest.xcdn)
  add_custom_command(
    OUTPUT ${ASTOOLS_PKG_DIR}/${tool}/manifest.xcdn
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ASTOOLS_PKG_DIR}/${tool}/bin/${ASTOOLS_PLAT}
    COMMAND ${CMAKE_COMMAND} -E copy ${manifest} ${ASTOOLS_PKG_DIR}/${tool}/manifest.xcdn
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:astools-std> ${ASTOOLS_PKG_DIR}/${tool}/bin/${ASTOOLS_PLAT}/
    DEPENDS astools-std ${manifest}
    COMMENT "Assembling tool package ${tool}")
  list(APPEND ASTOOLS_PKG_STAMPS ${ASTOOLS_PKG_DIR}/${tool}/manifest.xcdn)
endforeach()
add_custom_target(astools_packages ALL DEPENDS ${ASTOOLS_PKG_STAMPS})

if(ASTERISM_BUILD_FUZZERS)
  foreach(target astools asmodel_json xcdn)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
  endforeach()
  target_compile_definitions(astools PRIVATE ASTERISM_SANITIZERS)
  target_link_options(xcdn INTERFACE -fsanitize=address,undefined)
  foreach(name manifest schema)
    add_executable(astools-fuzz-${name} fuzz/${name}.c)
    target_include_directories(astools-fuzz-${name} PRIVATE src)
    target_link_libraries(astools-fuzz-${name} PRIVATE astools)
    asterism_strict(astools-fuzz-${name})
    target_compile_options(astools-fuzz-${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(astools-fuzz-${name} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
endif()

if(ASTERISM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
