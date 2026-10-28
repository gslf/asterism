# astime — routines: cron, triggers, state, scheduling, dry-run, lint and
# natural schedule parsing. Depends on core, xcdn and libc only.

set(ASTIME_SOURCES
  src/util.c src/tz.c src/cron.c src/routine.c src/serialize.c
  src/schedule.c src/store.c src/render.c src/natural.c)

add_library(astime STATIC ${ASTIME_SOURCES})
target_include_directories(astime PUBLIC include PRIVATE src ${ASTERISM_XCDN_SRC})
target_link_libraries(astime PUBLIC core PRIVATE xcdn)
asterism_strict(astime)

if(ASTERISM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
