turbowasm_add_c_guest(guest_c11 "${CMAKE_CURRENT_SOURCE_DIR}/c11.c")
turbowasm_add_c_guest(guest_numeric "${CMAKE_CURRENT_SOURCE_DIR}/numeric.c")
turbowasm_add_c_guest(guest_buffering "${CMAKE_CURRENT_SOURCE_DIR}/buffering.c")
turbowasm_add_c_guest(guest_stdio_faults "${CMAKE_CURRENT_SOURCE_DIR}/stdio_faults.c")
add_executable(turbowasm_guest_test guest_test.c)
target_link_libraries(turbowasm_guest_test PRIVATE turbowasm_command Salts::TinyTest Salts::Core)
target_compile_definitions(turbowasm_guest_test PRIVATE GUEST_C11_PATH="${guest_c11_WASM}")
target_compile_definitions(turbowasm_guest_test PRIVATE GUEST_NUMERIC_PATH="${guest_numeric_WASM}")
target_compile_definitions(turbowasm_guest_test PRIVATE GUEST_BUFFERING_PATH="${guest_buffering_WASM}")
target_compile_definitions(turbowasm_guest_test PRIVATE GUEST_STDIO_FAULTS_PATH="${guest_stdio_faults_WASM}")
target_compile_features(turbowasm_guest_test PRIVATE c_std_11)
set_target_properties(turbowasm_guest_test PROPERTIES FOLDER "tests/guest")
add_dependencies(turbowasm_guest_test guest_c11 guest_numeric guest_buffering guest_stdio_faults)
add_test(NAME turbowasm_guest_test COMMAND turbowasm_guest_test)
set_tests_properties(turbowasm_guest_test PROPERTIES TIMEOUT 120)
add_test(NAME turbowasm_guest_cli COMMAND turbowasm_run "${guest_hello_WASM}" demo)
set_tests_properties(turbowasm_guest_cli PROPERTIES PASS_REGULAR_EXPRESSION "guest cleanup complete" TIMEOUT 30)

turbowasm_add_c_guest_library(guest_reactor_library SOURCES reactor_library.c)
turbowasm_add_c_reactor(guest_reactor SOURCES reactor.c
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}" LIBRARIES guest_reactor_library
  EXPORTS step initialized ask_host allocation_cycle finish fail terminate forever)
turbowasm_add_c_reactor(guest_reactor_consumer SOURCES reactor_consumer.c EXPORTS use_provider)
add_executable(turbowasm_guest_reactor_test reactor_test.c
  "${PROJECT_SOURCE_DIR}/examples/guest/reactor_session.c")
target_include_directories(turbowasm_guest_reactor_test PRIVATE "${PROJECT_SOURCE_DIR}/examples/guest")
target_link_libraries(turbowasm_guest_reactor_test PRIVATE TurboWasm::WASI Salts::TinyTest Salts::Platform)
target_compile_features(turbowasm_guest_reactor_test PRIVATE c_std_11)
target_compile_options(turbowasm_guest_reactor_test PRIVATE $<$<COMPILE_LANG_AND_ID:C,MSVC>:-experimental:c11atomics>)
target_compile_definitions(turbowasm_guest_reactor_test PRIVATE
  GUEST_REACTOR_PATH="${guest_reactor_WASM}" GUEST_REACTOR_CONSUMER_PATH="${guest_reactor_consumer_WASM}")
set_target_properties(turbowasm_guest_reactor_test PROPERTIES FOLDER "tests/guest")
add_dependencies(turbowasm_guest_reactor_test guest_reactor guest_reactor_consumer)
add_test(NAME turbowasm_guest_reactor_test COMMAND turbowasm_guest_reactor_test)
set_tests_properties(turbowasm_guest_reactor_test PROPERTIES TIMEOUT 60)
add_test(NAME turbowasm_guest_reactor_example COMMAND turbowasm_reactor_example "${guest_counter_WASM}")
set_tests_properties(turbowasm_guest_reactor_example PROPERTIES TIMEOUT 30)
if(TARGET turbowasm_guest_cmeta)
  turbowasm_add_c_reactor(guest_cmeta SOURCES cmeta.c cmeta_peer.c
    INCLUDE_DIRECTORIES "${TURBOWASM_GUEST_CMETA_SOURCE_DIR}/include"
    LIBRARIES turbowasm_guest_cmeta
    EXPORTS cmeta_metadata cmeta_calls cmeta_object_open cmeta_object_read cmeta_object_close cmeta_alignment)
  add_dependencies(turbowasm_guest_reactor_test guest_cmeta)
  target_compile_definitions(turbowasm_guest_reactor_test PRIVATE GUEST_CMETA_PATH="${guest_cmeta_WASM}")
endif()

foreach(mode IN ITEMS optimized unoptimized)
  set(sjlj_options)
  if(mode STREQUAL "unoptimized")
    set(sjlj_options -O0 -fno-lto)
  endif()
  turbowasm_add_c_reactor(guest_sjlj_${mode} SOURCES sjlj.c sjlj_leaf.c
    COMPILE_OPTIONS ${sjlj_options}
    EXPORTS sjlj_values sjlj_nested sjlj_repeated sjlj_restore_stack sjlj_callback sjlj_trap sjlj_fuel sjlj_leaf)
  add_dependencies(turbowasm_guest_reactor_test guest_sjlj_${mode})
endforeach()
target_compile_definitions(turbowasm_guest_reactor_test PRIVATE
  GUEST_SJLJ_OPTIMIZED_PATH="${guest_sjlj_optimized_WASM}"
  GUEST_SJLJ_UNOPTIMIZED_PATH="${guest_sjlj_unoptimized_WASM}")
if(TURBOWASM_ENABLE_MIR_JIT)
  foreach(tier IN ITEMS mir mixed)
    add_executable(turbowasm_guest_sjlj_${tier}_test reactor_test.c
      "${PROJECT_SOURCE_DIR}/examples/guest/reactor_session.c")
    target_include_directories(turbowasm_guest_sjlj_${tier}_test PRIVATE
      "${PROJECT_SOURCE_DIR}/examples/guest" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(turbowasm_guest_sjlj_${tier}_test PRIVATE
      TurboWasm::WASI Salts::TinyTest Salts::Platform turbowasm_mir_backend)
    target_compile_features(turbowasm_guest_sjlj_${tier}_test PRIVATE c_std_11)
    target_compile_definitions(turbowasm_guest_sjlj_${tier}_test PRIVATE
      TURBOWASM_TEST_MIR=1 $<TARGET_PROPERTY:turbowasm_guest_reactor_test,COMPILE_DEFINITIONS>)
    if(tier STREQUAL "mixed")
      target_compile_definitions(turbowasm_guest_sjlj_${tier}_test PRIVATE TURBOWASM_TEST_MIXED=1)
    endif()
    add_dependencies(turbowasm_guest_sjlj_${tier}_test guest_reactor guest_reactor_consumer
      guest_sjlj_optimized guest_sjlj_unoptimized)
    set_target_properties(turbowasm_guest_sjlj_${tier}_test PROPERTIES FOLDER "tests/guest")
    add_test(NAME turbowasm_guest_sjlj_${tier}_test COMMAND turbowasm_guest_sjlj_${tier}_test)
    set_tests_properties(turbowasm_guest_sjlj_${tier}_test PROPERTIES TIMEOUT 120)
  endforeach()
endif()
