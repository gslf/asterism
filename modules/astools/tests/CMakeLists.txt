# Test targets are named astools_test_<name>; CTest names are astools.<name>.
# astools_tool_* are scripted tool fixtures spawned by the runtime under test.

function(astools_test name)
  cmake_parse_arguments(T "" "TIMEOUT" "SOURCES" ${ARGN})
  add_executable(astools_test_${name} test_${name}.c ${T_SOURCES})
  target_include_directories(astools_test_${name} PRIVATE
    . ../include ../src ../std ${ASTERISM_XCDN_SRC})
  target_link_libraries(astools_test_${name} PRIVATE astools core_test_harness)
  asterism_strict(astools_test_${name})
  add_test(NAME astools.${name} COMMAND astools_test_${name})
  if(NOT T_TIMEOUT)
    set(T_TIMEOUT 60)
  endif()
  set_tests_properties(astools.${name} PROPERTIES TIMEOUT ${T_TIMEOUT}
    ENVIRONMENT "${ASTOOLS_TEST_ENV}")
endfunction()

function(astools_tool name)
  add_executable(astools_tool_${name} tools/tool_${name}.c)
  target_include_directories(astools_tool_${name} PRIVATE ../include ../src ${ASTERISM_XCDN_SRC})
  asterism_strict(astools_tool_${name})
endfunction()

astools_tool(fake)
target_link_libraries(astools_tool_fake PRIVATE xcdn)

# The product modes "asterism jail" and "asterism tools", built standalone.
astools_tool(cli)
target_link_libraries(astools_tool_cli PRIVATE astools)
set(ASTOOLS_TEST_ENV "ASTOOLS_TEST_TOOL=$<TARGET_FILE:astools_tool_fake>")
if(NOT WIN32)
  astools_tool(jail)
  target_link_libraries(astools_tool_jail PRIVATE astools)
  list(APPEND ASTOOLS_TEST_ENV "ASTOOLS_JAIL=$<TARGET_FILE:astools_tool_jail>")
endif()

foreach(name duration semver types manifest path callline jscm
             process_frame lsp_files mcp_contract sandbox)
  astools_test(${name})
endforeach()
foreach(name policy lockfile catalog discovery invoke escape)
  astools_test(${name} SOURCES fakes.c)
endforeach()
astools_test(regex SOURCES ../std/regex.c)
astools_test(project_receipt SOURCES ../std/project_receipt.c)
foreach(name invoke escape)
  target_compile_definitions(astools_test_${name} PRIVATE
    "ASTOOLS_TEST_TOOL_PATH=\"$<TARGET_FILE:astools_tool_fake>\"")
  add_dependencies(astools_test_${name} astools_tool_fake)
endforeach()

# Standard tools, run from the assembled packages.
get_filename_component(ASTOOLS_CMAKE_BIN "${CMAKE_COMMAND}" DIRECTORY)
astools_test(std_semantic)
target_compile_definitions(astools_test_std_semantic PRIVATE
  "ASTOOLS_STD_PACKAGES=\"${CMAKE_BINARY_DIR}/packages\""
  "ASTOOLS_TEST_CMAKE_BIN=\"${ASTOOLS_CMAKE_BIN}\"")
add_dependencies(astools_test_std_semantic astools_packages)
if(NOT WIN32)
  astools_test(std_containment)
  target_compile_definitions(astools_test_std_containment PRIVATE
    "ASTOOLS_STD_PACKAGES=\"${CMAKE_BINARY_DIR}/packages\"")
  add_dependencies(astools_test_std_containment astools_packages)

  add_library(astools_tool_library SHARED tools/tool_library.c)
  target_include_directories(astools_tool_library PRIVATE ../include)
  asterism_strict(astools_tool_library)
  astools_test(library SOURCES fakes.c TIMEOUT 20)
  target_compile_definitions(astools_test_library PRIVATE
    "ASTOOLS_TEST_LIBRARY_PATH=\"$<TARGET_FILE:astools_tool_library>\"")
  add_dependencies(astools_test_library astools_tool_library)
endif()

# Persistent runtimes: process pool, LSP and MCP clients.
if(NOT WIN32 AND NOT ASTERISM_NO_THREADS)
  astools_tool(persistent)
  target_link_libraries(astools_tool_persistent PRIVATE xcdn)
  astools_test(persistent SOURCES fakes.c TIMEOUT 30)
  target_compile_definitions(astools_test_persistent PRIVATE
    "ASTOOLS_TEST_PERSISTENT_PATH=\"$<TARGET_FILE:astools_tool_persistent>\"")
  add_dependencies(astools_test_persistent astools_tool_persistent)

  astools_tool(lsp)
  target_link_libraries(astools_tool_lsp PRIVATE astools)
  astools_test(lsp SOURCES fakes.c TIMEOUT 30)
  target_compile_definitions(astools_test_lsp PRIVATE
    "ASTOOLS_TEST_LSP_PATH=\"$<TARGET_FILE:astools_tool_lsp>\"")
  add_dependencies(astools_test_lsp astools_tool_lsp)

  astools_tool(mcp)
  target_link_libraries(astools_tool_mcp PRIVATE astools)
  astools_test(mcp_client SOURCES fakes.c TIMEOUT 30)
  target_compile_definitions(astools_test_mcp_client PRIVATE
    "ASTOOLS_TEST_MCP_PATH=\"$<TARGET_FILE:astools_tool_mcp>\"")
  add_dependencies(astools_test_mcp_client astools_tool_mcp)

  # Real LSP conformance when clangd is installed.
  find_program(ASTOOLS_CLANGD NAMES clangd)
  if(APPLE AND ASTOOLS_CLANGD STREQUAL "/usr/bin/clangd")
    # /usr/bin holds Xcode dispatch shims; Seatbelt admits only the real binary.
    execute_process(COMMAND xcrun --find clangd OUTPUT_VARIABLE ASTOOLS_CLANGD
      RESULT_VARIABLE status TIMEOUT 15 OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT status EQUAL 0 OR ASTOOLS_CLANGD STREQUAL "/usr/bin/clangd")
      set(ASTOOLS_CLANGD "")
    endif()
  endif()
  if(ASTOOLS_CLANGD)
    astools_test(lsp_clangd SOURCES fakes.c TIMEOUT 45)
    target_compile_definitions(astools_test_lsp_clangd PRIVATE
      "ASTOOLS_CLANGD_PATH=\"${ASTOOLS_CLANGD}\"")
  endif()
endif()

# Packaging scripts (scripts/package_*.py), end to end through "tools".
if(Python3_FOUND AND NOT WIN32 AND NOT ASTERISM_NO_THREADS)
  add_test(NAME astools.mcp_package
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_package.py
            --cli $<TARGET_FILE:astools_tool_cli> --server $<TARGET_FILE:astools_tool_mcp>)
  set_tests_properties(astools.mcp_package PROPERTIES TIMEOUT 60 ENVIRONMENT "${ASTOOLS_TEST_ENV}")
  if(ASTOOLS_CLANGD)
    add_test(NAME astools.lsp_package
      COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_lsp_package.py
              --cli $<TARGET_FILE:astools_tool_cli> --clangd ${ASTOOLS_CLANGD})
    set_tests_properties(astools.lsp_package PROPERTIES TIMEOUT 45 ENVIRONMENT "${ASTOOLS_TEST_ENV}")
  endif()
endif()
