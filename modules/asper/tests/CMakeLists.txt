# Test targets are named asper_test_<name>; CTest names are asper.<name>.

function(asper_executable target)
  add_executable(${target} ${ARGN} fakes.c)
  target_include_directories(${target} PRIVATE . ../include ../src ${ASTERISM_XCDN_SRC})
  target_link_libraries(${target} PRIVATE asper core_test_harness)
  asterism_strict(${target})
endfunction()

foreach(name os_files storage_durability knowledge hybrid_evidence names store
             journal decay_scoring index inject_golden curator_cycles
             curation_receipts curation_input review_recall config
             stats_projects source source_context source_pending)
  asper_executable(asper_test_${name} test_${name}.c)
  add_test(NAME asper.${name} COMMAND asper_test_${name})
  set_tests_properties(asper.${name} PROPERTIES TIMEOUT 120)
endforeach()
target_compile_definitions(asper_test_config PRIVATE
  "ASPER_TEST_EXAMPLE_CONFIG=\"${PROJECT_SOURCE_DIR}/examples/asper.xcdn\"")

# Offline store maintenance (scripts/store.py) against a live store host.
if(Python3_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  asper_executable(asper_store_host store_host.c)
  asper_executable(asper_curation_fixture curation_fixture.c)
  asper_executable(asper_deferral_fixture deferral_fixture.c)
  foreach(name store curation deferral)
    add_test(NAME asper.admin_${name}
      COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_${name}_admin.py
              --host $<TARGET_FILE:asper_store_host>
              --curation-fixture $<TARGET_FILE:asper_curation_fixture>
              --deferral-fixture $<TARGET_FILE:asper_deferral_fixture>)
    set_tests_properties(asper.admin_${name} PROPERTIES TIMEOUT 120)
  endforeach()
endif()
