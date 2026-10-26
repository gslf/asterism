# Test targets are named asngn_test_<name>; CTest names are asngn.<name>.
# Models are scripted fakes (fakes.c); tools run through the real astools
# registry with the asngn_fake_tool protocol fixture.

add_executable(asngn_fake_tool tools/fake_tool.c)
target_include_directories(asngn_fake_tool PRIVATE ${ASTERISM_XCDN_SRC})
target_link_libraries(asngn_fake_tool PRIVATE xcdn)
asterism_strict(asngn_fake_tool)

set(ASNGN_UNIT_TESTS
  scheduler util stream config redact verification
  workspace_tree retrieval workspace_git work work_replay steps grammar route
  detail session operations consumption ledger context_golden request_trace
  cache toolcache telemetry)
set(ASNGN_INTEGRATION_TESTS
  task_record action_events engine_turns engine_guards tool_selection
  native_actions approvals approval_replay evidence)

foreach(name ${ASNGN_UNIT_TESTS} ${ASNGN_INTEGRATION_TESTS})
  if(name IN_LIST ASNGN_UNIT_TESTS)
    set(source unit/test_${name}.c)
  else()
    set(source integration/test_${name}.c fixtures.c)
  endif()
  add_executable(asngn_test_${name} ${source} fakes.c)
  target_include_directories(asngn_test_${name} PRIVATE . .. ${ASTERISM_XCDN_SRC})
  target_link_libraries(asngn_test_${name} PRIVATE asngn_lib core_test_harness)
  target_compile_definitions(asngn_test_${name} PRIVATE
    "ASNGN_TEST_TOOL_PATH=\"$<TARGET_FILE:asngn_fake_tool>\"")
  add_dependencies(asngn_test_${name} asngn_fake_tool)
  asterism_strict(asngn_test_${name})
  add_test(NAME asngn.${name} COMMAND asngn_test_${name})
  set_tests_properties(asngn.${name} PROPERTIES TIMEOUT 120)
endforeach()

# Extra sources: write-ahead fixtures and the MCP task layer.
foreach(name stream operations native_actions)
  target_sources(asngn_test_${name} PRIVATE wal_fixture.c)
endforeach()
target_sources(asngn_test_engine_turns PRIVATE ../mcp/tasks.c ../mcp/work.c)
target_sources(asngn_test_work PRIVATE ../mcp/work.c)
target_sources(asngn_test_task_record PRIVATE ../mcp/recovery.c ../mcp/work.c)
target_compile_definitions(asngn_test_config PRIVATE
  "ASNGN_TEST_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
if(ASTERISM_WITH_LLAMA)
  # Validate generated grammars with llama.cpp's own parser.
  enable_language(CXX)
  target_sources(asngn_test_grammar PRIVATE grammar_validate.cpp)
  target_include_directories(asngn_test_grammar PRIVATE ${PROJECT_SOURCE_DIR}/third_party/llama.cpp/src)
  target_compile_definitions(asngn_test_grammar PRIVATE ASNGN_TEST_LLAMA)
  target_link_libraries(asngn_test_grammar PRIVATE llama)
endif()
if(ASTERISM_SANITIZERS)
  # Full-capacity journal replay is slow under sanitizer allocators.
  set_tests_properties(asngn.consumption PROPERTIES TIMEOUT 300)
endif()

add_test(NAME asngn.model_manifest
  COMMAND ${CMAKE_COMMAND} "-DMANIFEST=${PROJECT_SOURCE_DIR}/scripts/models.manifest.tsv"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/model_manifest_smoke.cmake)

if(TARGET asterism)
  add_test(NAME asngn.mcp_protocol
    COMMAND ${CMAKE_COMMAND} "-DASTERISM=$<TARGET_FILE:asterism>"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/mcp_smoke.cmake)
  set_tests_properties(asngn.mcp_protocol PROPERTIES TIMEOUT 60)
endif()

if(ASTERISM_ACP)
  add_executable(asngn_test_acp_io acp/io.c)
  target_include_directories(asngn_test_acp_io PRIVATE .)
  target_link_libraries(asngn_test_acp_io PRIVATE asngn_frontends core_test_harness)
  asterism_strict(asngn_test_acp_io)
  add_test(NAME asngn.acp_io COMMAND asngn_test_acp_io)
  set_tests_properties(asngn.acp_io PROPERTIES TIMEOUT 20)
endif()

if(NOT Python3_FOUND)
  return()
endif()

# ── Python-driven end-to-end checks ─────────────────────────────────────────
function(asngn_py_test name script timeout)
  add_test(NAME asngn.${name}
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/${script} ${ARGN})
  set_tests_properties(asngn.${name} PROPERTIES TIMEOUT ${timeout})
endfunction()

add_executable(asngn_workspace_probe workspace_probe.c)
target_link_libraries(asngn_workspace_probe PRIVATE asngn_lib)
asngn_py_test(workspace_git_live workspace_git.py 120 $<TARGET_FILE:asngn_workspace_probe>)
set_tests_properties(asngn.workspace_git PROPERTIES TIMEOUT 120)

add_test(NAME asngn.quality_oracle
  COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/quality -p "test_*.py")
set_tests_properties(asngn.quality_oracle PROPERTIES TIMEOUT 120)

if(TARGET asterism)
  asngn_py_test(confirmation_tui confirmation_tui.py 60 $<TARGET_FILE:asterism>)
endif()

if(TARGET asterism)
  asngn_py_test(approval_mcp approval_mcp.py 60 $<TARGET_FILE:asterism>)
  asngn_py_test(work_mcp work_smoke.py 60 $<TARGET_FILE:asterism>)
  get_target_property(ASNGN_MODEL_LINKS asmodel INTERFACE_LINK_LIBRARIES)
  if(WIN32 OR "CURL::libcurl" IN_LIST ASNGN_MODEL_LINKS)
    asngn_py_test(native_wire native_wire.py 60 $<TARGET_FILE:asterism> $<TARGET_FILE:asngn_fake_tool>)
  endif()
endif()

if(ASTERISM_ACP)
  add_executable(asngn_acp_test_driver acp/driver.c fakes.c fixtures.c)
  target_include_directories(asngn_acp_test_driver PRIVATE . ${ASTERISM_XCDN_SRC})
  target_link_libraries(asngn_acp_test_driver PRIVATE asngn_frontends core_test_harness)
  target_compile_definitions(asngn_acp_test_driver PRIVATE
    "ASNGN_TEST_TOOL_PATH=\"$<TARGET_FILE:asngn_fake_tool>\"")
  add_dependencies(asngn_acp_test_driver asngn_fake_tool)
  asterism_strict(asngn_acp_test_driver)
  asngn_py_test(acp_protocol acp/protocol.py 90 $<TARGET_FILE:asterism> $<TARGET_FILE:asngn_acp_test_driver>)
  asngn_py_test(acp_lifecycle acp/lifecycle.py 90 $<TARGET_FILE:asngn_acp_test_driver>)
endif()
