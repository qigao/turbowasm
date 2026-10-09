# The same formal fixtures qualify the build tree and installed guest SDK.
# The caller supplies a target prefix and optional local CMeta source directory.
set(profile_sources "${CMAKE_CURRENT_LIST_DIR}/threaded_profile.c")
set(profile_libraries)
set(profile_exports profile_begin profile_read profile_close profile_constructor_count profile_spin profile_clock)
if(threaded_cmeta_source)
  turbowasm_add_cmeta_guest_library(${threaded_profile_prefix}_cmeta "${threaded_cmeta_source}" THREADS)
  turbowasm_add_c_guest_library(${threaded_profile_prefix}_cmeta_fixture THREADS
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/c11_cmeta.c" "${CMAKE_CURRENT_LIST_DIR}/cmeta.c"
      "${CMAKE_CURRENT_LIST_DIR}/cmeta_peer.c"
    INCLUDE_DIRECTORIES "${threaded_cmeta_source}/include"
    COMPILE_OPTIONS -DGUEST_CMETA_THREADED=1)
  set(profile_libraries ${threaded_profile_prefix}_cmeta_fixture ${threaded_profile_prefix}_cmeta)
  list(APPEND profile_exports cmeta_concurrent)
endif()
turbowasm_add_c_reactor(${threaded_profile_prefix}_reactor THREADS SOURCES "${profile_sources}"
  EXPORTS ${profile_exports} LIBRARIES ${profile_libraries})
turbowasm_add_c_reactor(${threaded_profile_prefix}_recursive THREADS SOURCES "${profile_sources}"
  COMPILE_OPTIONS -DGUEST_RECURSIVE_CRT=1)
turbowasm_add_c_guest(${threaded_profile_prefix}_command "${profile_sources}" THREADS
  COMPILE_OPTIONS -DGUEST_PROFILE_COMMAND=1)
turbowasm_add_c_guest(${threaded_profile_prefix}_exit "${profile_sources}" THREADS
  COMPILE_OPTIONS -DGUEST_PROFILE_COMMAND=1 -DGUEST_COMMAND_EXIT_LIVE=1)
turbowasm_add_c_reactor(${threaded_profile_prefix}_tls_size THREADS SOURCES "${profile_sources}"
  COMPILE_OPTIONS -DGUEST_TLS_BYTES=65536 -DGUEST_TLS_ALIGNMENT=16 EXPORTS profile_tls_limit profile_close)
turbowasm_add_c_reactor(${threaded_profile_prefix}_tls_alignment THREADS SOURCES "${profile_sources}"
  COMPILE_OPTIONS -DGUEST_TLS_BYTES=1 -DGUEST_TLS_ALIGNMENT=131072 EXPORTS profile_tls_limit profile_close)
add_executable(${threaded_profile_prefix}_test
  "${CMAKE_CURRENT_LIST_DIR}/threaded_profile_test.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../examples/guest/threaded_session.c")
target_link_libraries(${threaded_profile_prefix}_test PRIVATE TurboWasm::WASIThreads TurboWasm::WASI Salts::TinyTest)
target_include_directories(${threaded_profile_prefix}_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../examples/guest")
target_compile_features(${threaded_profile_prefix}_test PRIVATE c_std_11)
target_compile_options(${threaded_profile_prefix}_test PRIVATE $<$<COMPILE_LANG_AND_ID:C,MSVC>:-experimental:c11atomics>)
target_compile_definitions(${threaded_profile_prefix}_test PRIVATE
  GUEST_THREADED_REACTOR_PATH="${${threaded_profile_prefix}_reactor_WASM}"
  GUEST_THREADED_COMMAND_PATH="${${threaded_profile_prefix}_command_WASM}"
  GUEST_THREADED_EXIT_PATH="${${threaded_profile_prefix}_exit_WASM}"
  GUEST_THREADED_RECURSIVE_PATH="${${threaded_profile_prefix}_recursive_WASM}"
  GUEST_THREADED_TLS_SIZE_PATH="${${threaded_profile_prefix}_tls_size_WASM}"
  GUEST_THREADED_TLS_ALIGNMENT_PATH="${${threaded_profile_prefix}_tls_alignment_WASM}")
if(threaded_cmeta_source)
  target_compile_definitions(${threaded_profile_prefix}_test PRIVATE GUEST_PROFILE_CMETA=1)
endif()
add_dependencies(${threaded_profile_prefix}_test ${threaded_profile_prefix}_reactor
  ${threaded_profile_prefix}_recursive ${threaded_profile_prefix}_command ${threaded_profile_prefix}_exit
  ${threaded_profile_prefix}_tls_size ${threaded_profile_prefix}_tls_alignment)
set_target_properties(${threaded_profile_prefix}_test PROPERTIES FOLDER "tests/guest")
add_test(NAME ${threaded_profile_prefix}_test COMMAND ${threaded_profile_prefix}_test)
set_tests_properties(${threaded_profile_prefix}_test PROPERTIES TIMEOUT 60)
