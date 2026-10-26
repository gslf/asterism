# Test targets are named asmodel_test_<name>; CTest names are asmodel.<name>.
set(ASMODEL_TESTS tools input embedding decode manager)
if(NOT WIN32 AND NOT ASTERISM_NO_THREADS)
  list(APPEND ASMODEL_TESTS queue)
endif()
add_executable(asmodel_test_json test_json.c)
target_link_libraries(asmodel_test_json PRIVATE asmodel_json core core_test_harness)
asterism_strict(asmodel_test_json)
add_test(NAME asmodel.json COMMAND asmodel_test_json)

foreach(name ${ASMODEL_TESTS})
  add_executable(asmodel_test_${name} test_${name}.c)
  target_include_directories(asmodel_test_${name} PRIVATE ../src)
  target_link_libraries(asmodel_test_${name} PRIVATE asmodel)
  add_test(NAME asmodel.${name} COMMAND asmodel_test_${name})
  set_tests_properties(asmodel.${name} PROPERTIES TIMEOUT 30)
endforeach()

# HTTP contract suite against a scripted local server.
get_target_property(ASMODEL_LINKS asmodel INTERFACE_LINK_LIBRARIES)
if(Python3_FOUND AND (WIN32 OR "CURL::libcurl" IN_LIST ASMODEL_LINKS))
  add_executable(asmodel_test_openai test_openai.c test_messages_wire.c test_tools_wire.c)
  target_link_libraries(asmodel_test_openai PRIVATE asmodel)
  add_test(NAME asmodel.openai
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_openai.py
            $<TARGET_FILE:asmodel_test_openai>)
  set_tests_properties(asmodel.openai PROPERTIES TIMEOUT 60)
endif()
