# asngn — the engine (libasngn) and its front-ends: TUI/CLI, MCP, ACP.

set(ASNGN_SOURCES
  # text helpers, log, durable logs
  src/util.c src/log.c src/xutil.c src/wal.c src/stream.c
  # configuration, sessions, workspace
  src/config.c src/doctor.c src/session.c src/turn_journal.c
  src/turn_recovery.c src/task_record.c
  src/workspace.c src/workspace_git.c src/workspace_git_paths.c
  src/workspace_read.c src/workspace_tree.c src/workspace_fingerprint.c
  # accounting
  src/ledger.c src/operation_replay.c src/operation_codec.c src/operations.c
  src/consumption.c src/tokens.c
  # models
  src/models.c src/models_provider.c src/models_shared.c
  # context and retrieval
  src/context.c src/context_trace.c src/request_trace.c src/context_budget.c
  src/context_history.c src/retrieval.c src/retrieval_index.c
  src/retrieval_select.c src/retrieval_terms.c src/digest.c src/excerpt.c
  src/blob.c src/embedcache.c src/cache.c src/toolcache.c
  # turn loop
  src/route.c src/detail.c src/grammar.c src/protocol_schema.c
  src/protocol_value.c src/steps.c src/verification.c
  src/work_definition.c src/work_codec.c src/work_store.c src/work_replay.c
  src/work_api.c src/work_observe.c
  src/loop.c src/turn_support.c src/generation.c src/action_state.c
  src/draft.c src/tool_outcome.c src/confirmation.c
  src/approval_codec.c src/approval_store.c src/approval_replay.c
  src/approval_request.c
  src/tool_action.c src/action_instruction.c src/action_loop.c
  src/native_contract.c src/native_history.c src/native_loop.c
  src/action_apply.c src/action_event.c src/answer.c src/tools.c
  # safety, telemetry, modules, public API
  src/safety.c src/redact.c src/judge.c src/telemetry.c src/modules.c
  src/worker.c src/api.c src/introspect.c)
if(WIN32)
  list(APPEND ASNGN_SOURCES src/workspace_tree_win32.c)
else()
  list(APPEND ASNGN_SOURCES src/workspace_tree_posix.c)
endif()

add_library(asngn_lib STATIC ${ASNGN_SOURCES})
set_target_properties(asngn_lib PROPERTIES OUTPUT_NAME asngn)
target_include_directories(asngn_lib PUBLIC include src)
target_link_libraries(asngn_lib PUBLIC core asmodel asper astools xcdn)
asterism_strict(asngn_lib)
if(NOT WIN32)
  target_link_libraries(asngn_lib PUBLIC ${CMAKE_DL_LIBS} m)
endif()

if(ASTERISM_FRONTENDS)
  # The modes of the one executable: terminal UI, MCP server, editor host.
  set(ASNGN_FRONTEND_SOURCES
    tui/main.c tui/term.c tui/draw.c tui/theme.c tui/input.c tui/panes.c
    tui/modal.c tui/confirmation.c tui/chat.c
    mcp/main.c mcp/tasks.c mcp/work.c mcp/approval.c mcp/recovery.c
    mcp/consumption.c)
  if(ASTERISM_ACP)
    list(APPEND ASNGN_FRONTEND_SOURCES
      acp/main.c acp/io.c acp/protocol.c acp/dispatch.c acp/session.c
      acp/stream.c acp/events.c acp/permission.c acp/run.c)
  endif()
  add_library(asngn_frontends STATIC ${ASNGN_FRONTEND_SOURCES})
  target_include_directories(asngn_frontends PUBLIC . tui mcp acp)
  target_link_libraries(asngn_frontends PUBLIC asngn_lib)
  asterism_strict(asngn_frontends)

  # asterism: the only executable. main.c picks the mode.
  add_executable(asterism main.c)
  target_link_libraries(asterism PRIVATE asngn_frontends)
  if(ASTERISM_ACP)
    target_compile_definitions(asterism PRIVATE ASTERISM_ACP)
  endif()
  asterism_strict(asterism)
endif()

if(ASTERISM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
