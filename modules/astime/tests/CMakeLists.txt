# Test targets are named astime_test_<name>; CTest names are astime.<name>.

foreach(name cron routine state due render natural)
  add_executable(astime_test_${name} test_${name}.c)
  target_include_directories(astime_test_${name} PRIVATE . ../include)
  target_link_libraries(astime_test_${name} PRIVATE astime core_test_harness)
  asterism_strict(astime_test_${name})
  add_test(NAME astime.${name} COMMAND astime_test_${name})
  set_tests_properties(astime.${name} PROPERTIES TIMEOUT 60)
endforeach()
