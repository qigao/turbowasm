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

if(TARGET TurboWasm::WASIThreads)
  _turbowasm_guest_objects(guest_c11_threads SOURCES c11_threads.c c11_stdio.c c11_libc.c
    INCLUDE_DIRECTORIES "${metallic_source}/threaded/include" "${metallic_source}/threaded/src"
    COMPILE_OPTIONS -U__STDC_NO_THREADS__ -D__METALLIC_THREADS__=1 -matomics -pthread -ftls-model=local-exec)
  set(c11_threads_wasm "${CMAKE_CURRENT_BINARY_DIR}/guest_c11_threads.wasm")
  add_custom_command(OUTPUT "${c11_threads_wasm}"
    COMMAND "${TURBOWASM_GUEST_CLANG}" --target=wasm32-unknown-unknown
      -matomics -mbulk-memory -pthread -nostdlib -flto -mexception-handling
      -Wl,-mllvm,-wasm-enable-sjlj,-mllvm,-wasm-use-legacy-eh=false,-mllvm,-exception-model=wasm
      -Wl,-u,__wasm_setjmp,-u,__wasm_setjmp_test,-u,__wasm_longjmp
      -Wl,--no-entry,--shared-memory,--import-memory,--stack-first
      -Wl,-z,stack-size=262144 -Wl,--initial-memory=1048576,--max-memory=16777216
      -Wl,--export=wasi_thread_start,--export=initialize,--export=lifecycle,--export=synchronization
      -Wl,--export=conditions,--export=thread_specific,--export=allocation,--export=capacity
      -Wl,--export=shared_io,--export=drain_join,--export=abnormal_join
      -Wl,--export=stdio_records,--export=stdio_progress,--export=stdio_close_flush,--export=stdio_error_release
      -Wl,--export=stdio_reopen_flush
      -Wl,--export=libc_state,--export=libc_exit_callbacks,--export=libc_signals,--export=libc_abort_default
      -Wl,--export=libc_preopens,--export=libc_preopen_denied,--export=libc_preopen_large,--export=libc_preopen_invalid
      ${guest_c11_threads_OBJECTS} "${TURBOWASM_GUEST_THREADED_LIBRARY}" -o "${c11_threads_wasm}"
    DEPENDS ${guest_c11_threads_OBJECTS} "${TURBOWASM_GUEST_THREADED_LIBRARY}" VERBATIM COMMAND_EXPAND_LISTS)
  add_custom_target(guest_c11_threads ALL DEPENDS "${c11_threads_wasm}")
  set_target_properties(guest_c11_threads PROPERTIES FOLDER "tests/guest")
  add_executable(turbowasm_guest_c11_threads_test c11_threads_test.c)
  target_link_libraries(turbowasm_guest_c11_threads_test PRIVATE TurboWasm::WASIThreads TurboWasm::WASI Salts::TinyTest Salts::Platform)
  target_compile_features(turbowasm_guest_c11_threads_test PRIVATE c_std_11)
  target_compile_definitions(turbowasm_guest_c11_threads_test PRIVATE GUEST_C11_THREADS_PATH="${c11_threads_wasm}")
  set_target_properties(turbowasm_guest_c11_threads_test PROPERTIES FOLDER "tests/guest")
  add_dependencies(turbowasm_guest_c11_threads_test guest_c11_threads)
  add_test(NAME turbowasm_guest_c11_threads_test COMMAND turbowasm_guest_c11_threads_test)
  set_tests_properties(turbowasm_guest_c11_threads_test PROPERTIES TIMEOUT 90)
  # Private #426 startup qualification, not an installed partial C11 profile.
  # Compile errno and SJLJ with native Wasm TLS; the single-thread SDK archive
  # cannot be linked into a shared-memory program.
  _turbowasm_guest_objects(guest_thread_abi SOURCES thread_abi.c thread_entry.S sjlj_leaf.c
    "${metallic_source}/src/errno/errno.c"
    "${metallic_source}/src/setjmp/setjmp.c"
    "${metallic_source}/src/setjmp/longjmp.c"
    "${metallic_source}/src/wchar/mbrtowc.c"
    "${metallic_source}/src/wchar/wcrtomb.c"
    "${metallic_source}/src/uchar/mbrtoc16.c"
    "${metallic_source}/src/uchar/mbrtoc32.c"
    "${metallic_source}/src/uchar/c16rtomb.c"
    COMPILE_OPTIONS -matomics -pthread -ftls-model=local-exec -fno-lto)
  set(thread_abi_wasm "${CMAKE_CURRENT_BINARY_DIR}/guest_thread_abi.wasm")
  add_custom_command(OUTPUT "${thread_abi_wasm}"
    COMMAND "${TURBOWASM_GUEST_CLANG}" --target=wasm32-unknown-unknown
      -matomics -mbulk-memory -pthread -nostdlib -mexception-handling
      -Wl,--no-entry,--shared-memory,--import-memory,--stack-first
      -Wl,-z,stack-size=262144 -Wl,--initial-memory=1048576,--max-memory=1048576
      -Wl,--export=wasi_thread_start,--export=setup,--export=spawn,--export=root_intact,--export=results,--export=root_conversions
      ${guest_thread_abi_OBJECTS} -o "${thread_abi_wasm}"
    DEPENDS ${guest_thread_abi_OBJECTS} VERBATIM COMMAND_EXPAND_LISTS)
  add_custom_target(guest_thread_abi ALL DEPENDS "${thread_abi_wasm}")
  set_target_properties(guest_thread_abi PROPERTIES FOLDER "tests/guest")
  add_executable(turbowasm_guest_thread_abi_test thread_abi_test.c)
  target_link_libraries(turbowasm_guest_thread_abi_test PRIVATE TurboWasm::WASIThreads Salts::TinyTest Salts::Platform)
  target_compile_features(turbowasm_guest_thread_abi_test PRIVATE c_std_11)
  target_compile_definitions(turbowasm_guest_thread_abi_test PRIVATE GUEST_THREAD_ABI_PATH="${thread_abi_wasm}")
  set_target_properties(turbowasm_guest_thread_abi_test PROPERTIES FOLDER "tests/guest")
  add_dependencies(turbowasm_guest_thread_abi_test guest_thread_abi)
  add_test(NAME turbowasm_guest_thread_abi_test COMMAND turbowasm_guest_thread_abi_test)
  set_tests_properties(turbowasm_guest_thread_abi_test PROPERTIES TIMEOUT 45)
endif()

foreach(mode IN ITEMS optimized unoptimized)
  set(sjlj_options)
  if(mode STREQUAL "unoptimized")
    set(sjlj_options -O0 -fno-lto)
  endif()
  turbowasm_add_c_reactor(guest_sjlj_${mode} SOURCES sjlj.c sjlj_leaf.c
    COMPILE_OPTIONS ${sjlj_options}
    EXPORTS sjlj_values sjlj_nested sjlj_repeated sjlj_restore_stack sjlj_callback sjlj_trap sjlj_fuel sjlj_leaf sjlj_unrelated_exception sjlj_cancel sjlj_exit)
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
