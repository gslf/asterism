cmake_minimum_required(VERSION 3.16)
project(asterism VERSION 0.1.0 LANGUAGES C)

# ── Options ─────────────────────────────────────────────────────────────────
option(ASTERISM_WITH_LLAMA    "Embedded llama.cpp inference"              ON)
option(ASTERISM_NO_THREADS    "Single-threaded library build (asngn_tick)" OFF)
option(ASTERISM_BUILD_TESTS   "Unit and integration tests"                ON)
option(ASTERISM_SANITIZERS    "ASan + UBSan"                              OFF)
option(ASTERISM_BUILD_FUZZERS "Clang libFuzzer parser targets"            OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Front-ends need threads; the editor host also needs POSIX.
set(ASTERISM_FRONTENDS ON)
if(ASTERISM_NO_THREADS)
  set(ASTERISM_FRONTENDS OFF)
endif()
set(ASTERISM_ACP OFF)
if(UNIX AND ASTERISM_FRONTENDS)
  set(ASTERISM_ACP ON)
endif()

if(MSVC)
  add_compile_options(/utf-8)
endif()
# One version for the whole project.
add_compile_definitions(ASTERISM_VERSION="${PROJECT_VERSION}")
if(ASTERISM_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
  add_compile_definitions(ASTERISM_SANITIZERS)
endif()
if(ASTERISM_NO_THREADS)
  add_compile_definitions(ASTERISM_NO_THREADS)
endif()
if(ASTERISM_WITH_LLAMA)
  add_compile_definitions(ASTERISM_WITH_LLAMA)
endif()
if(ASTERISM_BUILD_FUZZERS AND (NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR WIN32))
  message(FATAL_ERROR "ASTERISM_BUILD_FUZZERS requires Clang on a POSIX host")
endif()

# Strict warnings for every first-party target. Third-party code keeps its own.
function(asterism_strict target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

# ── Third-party ─────────────────────────────────────────────────────────────
foreach(dep xcdn-c llama.cpp)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dep}/CMakeLists.txt")
    message(FATAL_ERROR "third_party/${dep} missing — run: git submodule update --init")
  endif()
endforeach()

# xCDN: built from its sources; its own project file also adds tests and examples.
set(ASTERISM_XCDN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xcdn-c/src")
add_library(xcdn STATIC
  ${ASTERISM_XCDN_SRC}/error.c ${ASTERISM_XCDN_SRC}/ast.c ${ASTERISM_XCDN_SRC}/lexer.c
  ${ASTERISM_XCDN_SRC}/parser.c ${ASTERISM_XCDN_SRC}/ser.c)
target_include_directories(xcdn PUBLIC ${ASTERISM_XCDN_SRC})
set_target_properties(xcdn PROPERTIES C_STANDARD 11)
if(MSVC)
  target_compile_definitions(xcdn PRIVATE strtok_r=strtok_s)
else()
  target_compile_definitions(xcdn PRIVATE _POSIX_C_SOURCE=200809L)
endif()

if(ASTERISM_WITH_LLAMA)
  enable_language(CXX)
  # One static copy, linked into every executable: no runtime library paths.
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
  set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
  find_program(ASTERISM_CCACHE NAMES ccache sccache)
  if(NOT ASTERISM_CCACHE)
    set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
  endif()
  if(APPLE AND NOT DEFINED GGML_METAL)
    set(GGML_METAL OFF CACHE BOOL "Use Metal for llama.cpp")
  endif()
  add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
endif()

if(ASTERISM_BUILD_TESTS)
  enable_testing()
  find_package(Python3 3.11 COMPONENTS Interpreter)
endif()

# ── Modules, then the application ───────────────────────────────────────────
add_subdirectory(modules/core)
add_subdirectory(modules/asmodel)
add_subdirectory(modules/astools)
add_subdirectory(modules/asper)
add_subdirectory(modules/aslink)
add_subdirectory(modules/astime)
add_subdirectory(modules/asvault)
add_subdirectory(asngn)

# The one executable lands in build/bin, as installed.
if(TARGET asterism)
  set_target_properties(asterism PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

if(ASTERISM_BUILD_TESTS AND Python3_FOUND AND TARGET asterism)
  add_subdirectory(sdk/tests)
endif()

# ── Install / package ───────────────────────────────────────────────────────
include(GNUInstallDirs)
if(TARGET asterism)
  install(TARGETS asterism RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY "${CMAKE_BINARY_DIR}/packages/"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/asterism/tools USE_SOURCE_PERMISSIONS)
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DATADIR}/asterism/examples)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/asterism)

# Release archives carry a stable name per platform (install.sh and
# install.ps1 fetch releases/latest/download/asterism-<platform>.<ext>), with
# the cmake --install layout inside: bin/, share/asterism/{tools,examples}.
set(ASTERISM_PACKAGE_PLATFORM "" CACHE STRING "Release archive platform, e.g. linux-x86_64")
set(CPACK_PACKAGE_NAME asterism)
if(WIN32)
  set(CPACK_GENERATOR ZIP)
else()
  set(CPACK_GENERATOR TGZ)
endif()
if(ASTERISM_PACKAGE_PLATFORM)
  set(CPACK_PACKAGE_FILE_NAME "asterism-${ASTERISM_PACKAGE_PLATFORM}")
endif()
set(CPACK_PACKAGE_CHECKSUM SHA256)
include(CPack)
