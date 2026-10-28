# asmodel — shared inference runtime: model pool, budgets, providers, JSON.

find_package(CURL QUIET)

# The strict JSON codec, libc only: shared with astools without pulling in
# the providers (libcurl, llama.cpp).
add_library(asmodel_json STATIC
  src/json_support.c src/json_value.c src/json_parse.c src/json_write.c)
target_include_directories(asmodel_json PUBLIC include)
asterism_strict(asmodel_json)

add_library(asmodel STATIC
  src/asmodel.c
  src/input.c
  src/input_json.c
  src/tools.c
  src/tool_decode.c
  src/openai.c
  src/openai_decode.c
  src/http.c
  src/chatgpt.c
  src/chatgpt_decode.c
  src/chatgpt_auth.c
  src/chatgpt_login.c
  src/llama.c
  src/tokens.c
  src/pipeline.c)
target_include_directories(asmodel PUBLIC include PRIVATE src)
target_link_libraries(asmodel PUBLIC asmodel_json core)
asterism_strict(asmodel)
if(CURL_FOUND)
  target_compile_definitions(asmodel PRIVATE ASMODEL_WITH_CURL)
  target_link_libraries(asmodel PUBLIC CURL::libcurl)
endif()
if(WIN32)
  # ws2_32: the transient loopback listener of the ChatGPT login flow.
  target_link_libraries(asmodel PUBLIC winhttp ws2_32)
else()
  target_link_libraries(asmodel PUBLIC m)
endif()
if(ASTERISM_WITH_LLAMA)
  # The guard is the one C++ seam: it stops C++ exceptions from crossing the
  # extern "C" llama API into C callers (see src/llama_guard.h).
  target_sources(asmodel PRIVATE src/llama_guard.cpp)
  target_link_libraries(asmodel PRIVATE llama)
  if(MSVC)
    # /EHs, not /EHsc: under /EHsc MSVC assumes extern "C" callees never
    # throw and elides exactly the handlers the guard exists for.
    set_source_files_properties(src/llama_guard.cpp PROPERTIES COMPILE_OPTIONS "/EHs")
    target_compile_options(asmodel PRIVATE /wd4201) # llama.h nameless unions
  endif()
endif()

if(ASTERISM_BUILD_FUZZERS)
  foreach(target asmodel asmodel_json)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
  endforeach()
  target_link_options(asmodel_json INTERFACE -fsanitize=address,undefined)
  foreach(name json provider)
    add_executable(asmodel-fuzz-${name} fuzz/${name}.c)
    target_include_directories(asmodel-fuzz-${name} PRIVATE src)
    target_link_libraries(asmodel-fuzz-${name} PRIVATE asmodel)
    asterism_strict(asmodel-fuzz-${name})
    target_compile_options(asmodel-fuzz-${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(asmodel-fuzz-${name} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
endif()

if(ASTERISM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
