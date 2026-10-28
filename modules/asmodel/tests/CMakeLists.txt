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

# Prompt reuse against a real model; skipped unless ASMODEL_TEST_GGUF is set.
if(ASTERISM_WITH_LLAMA)
  add_executable(asmodel_test_llama_reuse test_llama_reuse.c)
  target_link_libraries(asmodel_test_llama_reuse PRIVATE asmodel)
  asterism_strict(asmodel_test_llama_reuse)
  add_test(NAME asmodel.llama_reuse COMMAND asmodel_test_llama_reuse)
  set_tests_properties(asmodel.llama_reuse PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

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

# ChatGPT plan provider: OAuth (PKCE, rotation, loopback/paste login) and the
# Responses stream against a fake OpenAI server. POSIX: the driver uses threads.
if(Python3_FOUND AND NOT WIN32 AND NOT ASTERISM_NO_THREADS AND "CURL::libcurl" IN_LIST ASMODEL_LINKS)
  add_executable(asmodel_test_chatgpt test_chatgpt.c)
  target_link_libraries(asmodel_test_chatgpt PRIVATE asmodel)
  asterism_strict(asmodel_test_chatgpt)
  add_test(NAME asmodel.chatgpt
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_chatgpt.py
            $<TARGET_FILE:asmodel_test_chatgpt>)
  set_tests_properties(asmodel.chatgpt PROPERTIES TIMEOUT 120)
endif()
