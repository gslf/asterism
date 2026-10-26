# core — platform layer and utilities shared by every module.

set(CORE_SOURCES
  src/util.c src/sha256.c src/uuid.c src/time.c src/log.c src/os_common.c)
if(WIN32)
  list(APPEND CORE_SOURCES src/os_win32.c)
else()
  list(APPEND CORE_SOURCES src/os_posix.c)
endif()

add_library(core STATIC ${CORE_SOURCES})
target_include_directories(core PUBLIC include)
asterism_strict(core)
# One POSIX feature level for every module (core.h exposes pthread types).
# glibc fixes visibility at the first system header, so it is a command-line
# definition; Apple keeps its full namespace.
if(NOT WIN32)
  target_compile_definitions(core PUBLIC _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64)
  if(APPLE)
    target_compile_definitions(core PUBLIC _DARWIN_C_SOURCE)
  endif()
endif()
if(NOT WIN32 AND NOT ASTERISM_NO_THREADS)
  find_package(Threads REQUIRED)
  target_link_libraries(core PUBLIC Threads::Threads)
endif()

if(ASTERISM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
