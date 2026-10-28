# asvault — the local secret vault: encrypted store, {{secret:NAME}}
# surrogates resolved at the tool boundary, redaction of tool output.

set(ASVAULT_SOURCES
  src/crypto.c src/policy.c src/store.c src/placeholder.c src/redact.c
  src/api.c src/cli.c src/os_private.c)

add_library(asvault STATIC ${ASVAULT_SOURCES})
target_include_directories(asvault PUBLIC include PRIVATE src)
target_link_libraries(asvault PUBLIC core PRIVATE xcdn)
asterism_strict(asvault)

if(ASTERISM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
