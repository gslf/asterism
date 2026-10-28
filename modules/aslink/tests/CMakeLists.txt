# Test targets are named aslink_test_<name>; CTest names are aslink.<name>.

foreach(name decode text request)
  add_executable(aslink_test_${name} test_${name}.c)
  target_include_directories(aslink_test_${name} PRIVATE ../src)
  target_link_libraries(aslink_test_${name} PRIVATE aslink core_test_harness)
  asterism_strict(aslink_test_${name})
  add_test(NAME aslink.${name} COMMAND aslink_test_${name})
  set_tests_properties(aslink.${name} PROPERTIES TIMEOUT 30)
endforeach()
target_compile_definitions(aslink_test_decode PRIVATE
  "ASLINK_TEST_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# The same sources without a transport, as on a host with neither libcurl
# nor WinHTTP: network calls must report UNSUPPORTED while the helpers keep
# working.
add_library(aslink_nocurl STATIC ${ASLINK_SOURCES_ABS}
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/transport_none.c)
target_include_directories(aslink_nocurl PUBLIC ../include PRIVATE ../src)
target_link_libraries(aslink_nocurl PUBLIC core asmodel_json)
asterism_strict(aslink_nocurl)
add_executable(aslink_test_nocurl test_nocurl.c)
target_include_directories(aslink_test_nocurl PRIVATE ../src)
target_link_libraries(aslink_test_nocurl PRIVATE aslink_nocurl core_test_harness)
asterism_strict(aslink_test_nocurl)
add_test(NAME aslink.nocurl COMMAND aslink_test_nocurl)
set_tests_properties(aslink.nocurl PROPERTIES TIMEOUT 30)

# Bot API contract suite against a scripted local server (127.0.0.1), on
# whichever backend was built.
if(Python3_FOUND AND NOT ASTERISM_NO_THREADS AND NOT ASLINK_BACKEND STREQUAL "none")
  add_executable(aslink_test_telegram_driver telegram_driver.c)
  target_include_directories(aslink_test_telegram_driver PRIVATE ../src)
  target_link_libraries(aslink_test_telegram_driver PRIVATE aslink)
  asterism_strict(aslink_test_telegram_driver)
  add_test(NAME aslink.telegram
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_telegram.py
            $<TARGET_FILE:aslink_test_telegram_driver>)
  set_tests_properties(aslink.telegram PROPERTIES TIMEOUT 120)
endif()

# Transport contract: Range (206 resume, 200 restart), redirects to a second
# local server, caps and timeouts, on the backend that was built.
if(Python3_FOUND AND NOT ASLINK_BACKEND STREQUAL "none")
  add_executable(aslink_test_transport_driver transport_driver.c)
  target_include_directories(aslink_test_transport_driver PRIVATE ../src)
  target_link_libraries(aslink_test_transport_driver PRIVATE aslink)
  asterism_strict(aslink_test_transport_driver)
  add_test(NAME aslink.transport
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_transport.py
            $<TARGET_FILE:aslink_test_transport_driver>)
  set_tests_properties(aslink.transport PROPERTIES TIMEOUT 120)
endif()
