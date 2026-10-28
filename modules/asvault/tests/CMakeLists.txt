# Test targets are named asvault_test_<name>; CTest names are asvault.<name>.

function(asvault_test name)
  add_executable(asvault_test_${name} test_${name}.c)
  target_include_directories(asvault_test_${name} PRIVATE . ../include ../src)
  target_link_libraries(asvault_test_${name} PRIVATE asvault core_test_harness)
  asterism_strict(asvault_test_${name})
  add_test(NAME asvault.${name} COMMAND asvault_test_${name})
  set_tests_properties(asvault.${name} PROPERTIES TIMEOUT 120)
endfunction()

foreach(name crypto store policy redact)
  asvault_test(${name})
endforeach()

# "asterism vault" built standalone: tool_cli <vault-dir> vault <command>...
add_executable(asvault_tool_cli tools/tool_cli.c)
target_link_libraries(asvault_tool_cli PRIVATE asvault)
asterism_strict(asvault_tool_cli)

if(Python3_FOUND AND NOT WIN32)
  add_test(NAME asvault.cli
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py
            --cli $<TARGET_FILE:asvault_tool_cli>)
  set_tests_properties(asvault.cli PROPERTIES TIMEOUT 120)
endif()
