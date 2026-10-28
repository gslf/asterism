# aslink — channels of the always-on agent: Telegram Bot API (outbound only).

# One HTTP backend: libcurl when found, else WinHTTP on Windows, else none
# (network calls report ASLINK_ERR_UNSUPPORTED). ASLINK_TRANSPORT forces one.
set(ASLINK_TRANSPORT "auto" CACHE STRING "aslink HTTP backend: auto, curl, winhttp or none")
set_property(CACHE ASLINK_TRANSPORT PROPERTY STRINGS auto curl winhttp none)
set(ASLINK_BACKEND "${ASLINK_TRANSPORT}")
if(ASLINK_BACKEND STREQUAL "auto")
  find_package(CURL QUIET)
  if(CURL_FOUND)
    set(ASLINK_BACKEND curl)
  elseif(WIN32)
    set(ASLINK_BACKEND winhttp)
  else()
    set(ASLINK_BACKEND none)
  endif()
elseif(ASLINK_BACKEND STREQUAL "curl")
  find_package(CURL REQUIRED)
elseif(ASLINK_BACKEND STREQUAL "winhttp")
  if(NOT WIN32)
    message(FATAL_ERROR "ASLINK_TRANSPORT=winhttp needs Windows")
  endif()
elseif(NOT ASLINK_BACKEND STREQUAL "none")
  message(FATAL_ERROR "ASLINK_TRANSPORT must be auto, curl, winhttp or none")
endif()
message(STATUS "aslink HTTP backend: ${ASLINK_BACKEND}")

set(ASLINK_SOURCES
  src/api.c src/text.c src/telegram_decode.c src/telegram_request.c
  src/transport.c src/telegram.c src/http.c)
# Absolute paths for the transport-less test build in tests/.
list(TRANSFORM ASLINK_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
     OUTPUT_VARIABLE ASLINK_SOURCES_ABS)

add_library(aslink STATIC ${ASLINK_SOURCES} src/transport_${ASLINK_BACKEND}.c)
target_include_directories(aslink PUBLIC include PRIVATE src)
target_link_libraries(aslink PUBLIC core asmodel_json)
asterism_strict(aslink)
if(ASLINK_BACKEND STREQUAL "curl")
  target_link_libraries(aslink PUBLIC CURL::libcurl)
elseif(ASLINK_BACKEND STREQUAL "winhttp")
  target_link_libraries(aslink PUBLIC winhttp)
endif()

if(ASTERISM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
