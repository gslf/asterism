# The harness shared by every module's tests.
add_library(core_test_harness INTERFACE)
target_include_directories(core_test_harness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT WIN32)
  target_link_libraries(core_test_harness INTERFACE m)
endif()

# Test targets are named core_test_<name>; CTest names are core.<name>.
foreach(name util time sha_uuid log)
  add_executable(core_test_${name} test_${name}.c)
  target_link_libraries(core_test_${name} PRIVATE core core_test_harness)
  asterism_strict(core_test_${name})
  add_test(NAME core.${name} COMMAND core_test_${name})
  set_tests_properties(core.${name} PROPERTIES TIMEOUT 60)
endforeach()
