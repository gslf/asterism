# asper — durable memory: exact events, semantic records, bounded context.

set(ASPER_SOURCES
  src/log.c src/config.c src/manifest.c
  src/store.c src/store_files.c src/store_transaction.c src/store_compact.c
  src/record_table.c src/record_encode.c src/record_decode.c src/record_strings.c
  src/source_paths.c src/source_view.c src/source_pins.c src/source_events.c
  src/source_objects.c src/source_checkpoint.c src/source_context.c
  src/source_history.c src/source_replay.c src/source_pending.c
  src/source_deferred.c
  src/turn_queue.c
  src/curation_codec.c src/curation_input.c src/curation_receipt.c
  src/knowledge_codec.c src/knowledge_store.c src/knowledge_validate.c
  src/knowledge_status.c src/knowledge_api.c src/knowledge_history.c
  src/event_frame.c src/event_index.c src/event_log.c src/journal.c
  src/cache.c src/index.c src/decay.c src/retrieve.c src/hybrid.c src/inject.c
  src/grammar.c src/output_contract.c src/curator.c src/worker.c src/api.c
  src/models.c src/os_blob.c src/os_stamp.c)

add_library(asper STATIC ${ASPER_SOURCES})
target_include_directories(asper PUBLIC include PRIVATE src)
target_link_libraries(asper PUBLIC core xcdn asmodel)
asterism_strict(asper)
if(NOT WIN32)
  target_link_libraries(asper PUBLIC m)
endif()

if(ASTERISM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
