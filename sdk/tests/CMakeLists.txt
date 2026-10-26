# SDK checks against the real asngn-mcp. Configuring never downloads packages.
# CTest names are sdk.<name>.

function(sdk_test name timeout)
  add_test(NAME sdk.${name} COMMAND ${ARGN})
  set_tests_properties(sdk.${name} PROPERTIES TIMEOUT ${timeout})
endfunction()

find_program(ASTERISM_NODE NAMES node)
if(ASTERISM_NODE)
  execute_process(COMMAND ${ASTERISM_NODE} -p process.versions.node
    OUTPUT_VARIABLE node_version OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE node_result)
  if(NOT node_result EQUAL 0 OR node_version VERSION_LESS 22)
    message(STATUS "SDK JavaScript checks need Node 22+; found ${node_version}")
    set(ASTERISM_NODE "")
  endif()
endif()

set(PY ${Python3_EXECUTABLE} -B)
set(DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(MCP $<TARGET_FILE:asngn-mcp>)
set(TOOL $<TARGET_FILE:asngn_fake_tool>)
get_target_property(model_links asmodel INTERFACE_LINK_LIBRARIES)
set(HTTP OFF)
if(WIN32 OR "CURL::libcurl" IN_LIST model_links)
  set(HTTP ON)
endif()

sdk_test(python 60 ${PY} ${DIR}/test_python.py)
sdk_test(consumption 60 ${PY} ${DIR}/consumption_live.py ${MCP} ${ASTERISM_NODE})
sdk_test(recovery 60 ${PY} ${DIR}/recovery_live.py ${MCP} ${ASTERISM_NODE})
if(HTTP)
  sdk_test(python_live 60 ${PY} ${DIR}/python_live.py ${MCP} ${TOOL})
endif()

if(ASTERISM_NODE)
  sdk_test(javascript 60 ${CMAKE_COMMAND} -E env "ASTERISM_TEST_PYTHON=${Python3_EXECUTABLE}"
           ${ASTERISM_NODE} ${DIR}/test_javascript.mjs)
  if(HTTP)
    sdk_test(javascript_live 60 ${PY} ${DIR}/javascript_live.py ${MCP} ${ASTERISM_NODE} ${TOOL})
  endif()
  # The TypeScript check runs once the sdk dev dependencies are installed.
  set(tsc "${DIR}/../node_modules/typescript/bin/tsc")
  if(EXISTS "${tsc}")
    sdk_test(types 60 ${ASTERISM_NODE} ${tsc} --noEmit --strict --module nodenext
             --target es2022 --lib es2022,dom,esnext.disposable ${DIR}/types.mts)
  endif()
endif()
