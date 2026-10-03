add_executable(turbowasm_module_test module_test.c)
target_link_libraries(turbowasm_module_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_module_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_module_test COMMAND turbowasm_module_test)

add_executable(turbowasm_artifact_envelope_test
  artifact_envelope_test.c)
target_link_libraries(turbowasm_artifact_envelope_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_artifact_envelope_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_artifact_envelope_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_artifact_envelope_test
         COMMAND turbowasm_artifact_envelope_test)

add_executable(turbowasm_artifact_restore_test
  artifact_restore_test.c)
target_link_libraries(turbowasm_artifact_restore_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_artifact_restore_test
  PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(turbowasm_artifact_restore_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_artifact_restore_test
         COMMAND turbowasm_artifact_restore_test)

if(TURBOWASM_ENABLE_COMPONENT)
  add_executable(turbowasm_component_public_hardening_test
    component_public_hardening_test.c)
  target_link_libraries(turbowasm_component_public_hardening_test
    PRIVATE TurboWasm::Component)
  set_target_properties(turbowasm_component_public_hardening_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_component_public_hardening_test
           COMMAND turbowasm_component_public_hardening_test)
endif()

add_executable(turbowasm_component_nested_resource_test
  component_nested_resource_test.c)
target_link_libraries(turbowasm_component_nested_resource_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_nested_resource_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_nested_resource_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_nested_resource_test
         COMMAND turbowasm_component_nested_resource_test)

add_executable(turbowasm_component_resource_call_test
  component_resource_call_test.c)
target_link_libraries(turbowasm_component_resource_call_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_resource_call_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_resource_call_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_resource_call_test
         COMMAND turbowasm_component_resource_call_test)

add_executable(turbowasm_component_resource_test
  component_resource_test.c)
target_link_libraries(turbowasm_component_resource_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_resource_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_resource_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_resource_test
         COMMAND turbowasm_component_resource_test)

add_executable(turbowasm_component_instance_exec_test
  component_instance_exec_test.c)
target_link_libraries(turbowasm_component_instance_exec_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_instance_exec_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_instance_exec_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_instance_exec_test
         COMMAND turbowasm_component_instance_exec_test)

add_executable(turbowasm_component_resource_builtin_exec_test
  component_resource_builtin_exec_test.c)
target_link_libraries(turbowasm_component_resource_builtin_exec_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_resource_builtin_exec_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(
  turbowasm_component_resource_builtin_exec_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_resource_builtin_exec_test
         COMMAND turbowasm_component_resource_builtin_exec_test)

add_executable(turbowasm_component_exec_options_test
  component_exec_options_test.c)
target_link_libraries(turbowasm_component_exec_options_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_exec_options_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_exec_options_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_exec_options_test
         COMMAND turbowasm_component_exec_options_test)

add_executable(turbowasm_component_exec_test
  component_exec_test.c)
target_link_libraries(turbowasm_component_exec_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_exec_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_exec_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_exec_test
         COMMAND turbowasm_component_exec_test)

add_executable(turbowasm_component_multi_import_test
  component_multi_import_test.c)
target_link_libraries(turbowasm_component_multi_import_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_multi_import_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_multi_import_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_multi_import_test
         COMMAND turbowasm_component_multi_import_test)

add_executable(turbowasm_component_core_call_test
  component_core_call_test.c)
target_link_libraries(turbowasm_component_core_call_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_core_call_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_core_call_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_core_call_test
         COMMAND turbowasm_component_core_call_test)

add_executable(turbowasm_component_canonical_memory_test
  component_canonical_memory_test.c)
target_link_libraries(turbowasm_component_canonical_memory_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_canonical_memory_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_canonical_memory_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_canonical_memory_test
         COMMAND turbowasm_component_canonical_memory_test)

add_executable(turbowasm_component_canonical_test
  component_canonical_test.c)
target_link_libraries(turbowasm_component_canonical_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_canonical_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_canonical_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_canonical_test
         COMMAND turbowasm_component_canonical_test)

add_executable(turbowasm_component_binary_test
  component_binary_test.c)
target_link_libraries(turbowasm_component_binary_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_binary_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_binary_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_binary_test
         COMMAND turbowasm_component_binary_test)

add_executable(turbowasm_component_type_graph_test
  component_type_graph_test.c)
target_link_libraries(turbowasm_component_type_graph_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_component_type_graph_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_component_type_graph_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_component_type_graph_test
         COMMAND turbowasm_component_type_graph_test)

if(TARGET turbowasm_wasi02_core)
  add_executable(turbowasm_wasi02_descriptor_test
    wasi02_descriptor_test.c)
  target_link_libraries(turbowasm_wasi02_descriptor_test
    PRIVATE turbowasm_wasi02_core)
  target_include_directories(turbowasm_wasi02_descriptor_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(turbowasm_wasi02_descriptor_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_descriptor_test
           COMMAND turbowasm_wasi02_descriptor_test)

  add_executable(turbowasm_wasi02_socket_descriptor_test
    wasi02_socket_descriptor_test.c)
  target_link_libraries(turbowasm_wasi02_socket_descriptor_test
    PRIVATE turbowasm_wasi02_core)
  target_include_directories(turbowasm_wasi02_socket_descriptor_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(turbowasm_wasi02_socket_descriptor_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_socket_descriptor_test
           COMMAND turbowasm_wasi02_socket_descriptor_test)

  add_executable(turbowasm_wasi02_socket_u16_test
    wasi02_socket_u16_test.c)
  target_link_libraries(turbowasm_wasi02_socket_u16_test
    PRIVATE
      turbowasm_wasi02_core
      TurboWasm::Runtime)
  target_include_directories(turbowasm_wasi02_socket_u16_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_socket_u16_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_socket_u16_test
           COMMAND turbowasm_wasi02_socket_u16_test)

  add_executable(turbowasm_wasi02_provider_test
    wasi02_provider_test.c)
  target_link_libraries(turbowasm_wasi02_provider_test
    PRIVATE
      turbowasm_wasi02_core
      TurboWasm::Runtime)
  target_include_directories(turbowasm_wasi02_provider_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_provider_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_provider_test
           COMMAND turbowasm_wasi02_provider_test)

  add_executable(turbowasm_wasi02_component_exec_test
    wasi02_component_exec_test.c)
  target_link_libraries(turbowasm_wasi02_component_exec_test
    PRIVATE
      turbowasm_wasi02_core
      TurboWasm::Runtime)
  target_include_directories(turbowasm_wasi02_component_exec_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_component_exec_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_component_exec_test
           COMMAND turbowasm_wasi02_component_exec_test)
endif()

if(TARGET turbowasm_wasi02_sockets_core)
  add_executable(turbowasm_wasi02_socket_component_bridge_test
    wasi02_socket_component_bridge_test.c)
  target_link_libraries(turbowasm_wasi02_socket_component_bridge_test
    PRIVATE
      turbowasm_wasi02_sockets_core
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_poll_core
      turbowasm_wasi02_core
      TurboWasm::Component
      TurboWasm::Runtime)
  target_include_directories(turbowasm_wasi02_socket_component_bridge_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(
    turbowasm_wasi02_socket_component_bridge_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_socket_component_bridge_test
           COMMAND turbowasm_wasi02_socket_component_bridge_test)

  add_executable(turbowasm_wasi02_tcp_state_test
    wasi02_tcp_state_test.c)
  target_link_libraries(turbowasm_wasi02_tcp_state_test
    PRIVATE
      turbowasm_wasi02_sockets_core
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_poll_core
      TurboWasm::Runtime)
  target_include_directories(turbowasm_wasi02_tcp_state_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_tcp_state_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_tcp_state_test
           COMMAND turbowasm_wasi02_tcp_state_test)
endif()

if(TARGET turbowasm_wasi02_cnet_core)
  add_executable(turbowasm_wasi02_cnet_control_test
    wasi02_cnet_control_test.c)
  target_link_libraries(turbowasm_wasi02_cnet_control_test
    PRIVATE
      turbowasm_wasi02_cnet_core
      Salts::CNet)
  target_include_directories(turbowasm_wasi02_cnet_control_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_cnet_control_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_cnet_control_test
           COMMAND turbowasm_wasi02_cnet_control_test)

  add_executable(turbowasm_wasi02_cnet_external_progress_test
    wasi02_cnet_external_progress_test.c)
  target_link_libraries(turbowasm_wasi02_cnet_external_progress_test
    PRIVATE
      turbowasm_wasi02_cnet_core
      Salts::CNet
      Salts::NativeIO)
  target_include_directories(turbowasm_wasi02_cnet_external_progress_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(
    turbowasm_wasi02_cnet_external_progress_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_cnet_external_progress_test
           COMMAND turbowasm_wasi02_cnet_external_progress_test)

  add_executable(turbowasm_wasi02_cnet_listener_progress_test
    wasi02_cnet_listener_progress_test.c)
  target_link_libraries(turbowasm_wasi02_cnet_listener_progress_test
    PRIVATE
      turbowasm_wasi02_cnet_core
      Salts::CNet
      Salts::NativeIO)
  target_include_directories(turbowasm_wasi02_cnet_listener_progress_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(
    turbowasm_wasi02_cnet_listener_progress_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_cnet_listener_progress_test
           COMMAND turbowasm_wasi02_cnet_listener_progress_test)

  set(_turbowasm_wasi02_cnet_tests
    turbowasm_wasi02_cnet_control_test
    turbowasm_wasi02_cnet_external_progress_test
    turbowasm_wasi02_cnet_listener_progress_test)

  add_executable(turbowasm_wasi02_cnet_connect_stream_test
    wasi02_cnet_connect_stream_test.c)
  target_link_libraries(turbowasm_wasi02_cnet_connect_stream_test
    PRIVATE
      turbowasm_wasi02_cnet_core
      turbowasm_wasi02_streams_core
      TurboWasm::Runtime
      Salts::CNet
      Salts::NativeIO)
  target_include_directories(turbowasm_wasi02_cnet_connect_stream_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(
    turbowasm_wasi02_cnet_connect_stream_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_cnet_connect_stream_test
           COMMAND turbowasm_wasi02_cnet_connect_stream_test)
  list(APPEND _turbowasm_wasi02_cnet_tests
    turbowasm_wasi02_cnet_connect_stream_test)

  if(TARGET turbowasm_wasi02_cnet_poll_native_io_core)
    add_executable(turbowasm_wasi02_cnet_dynamic_poll_test
      wasi02_cnet_dynamic_poll_test.c)
    target_link_libraries(turbowasm_wasi02_cnet_dynamic_poll_test
      PRIVATE
        turbowasm_wasi02_cnet_poll_native_io_core
        turbowasm_wasi02_cnet_core
        turbowasm_wasi02_poll_native_io_core
        turbowasm_wasi02_poll_core
        TurboWasm::Runtime
        Salts::CNet
        Salts::NativeIO)
    target_include_directories(turbowasm_wasi02_cnet_dynamic_poll_test
      PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/include")
    set_target_properties(
      turbowasm_wasi02_cnet_dynamic_poll_test PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF)
    add_test(NAME turbowasm_wasi02_cnet_dynamic_poll_test
             COMMAND turbowasm_wasi02_cnet_dynamic_poll_test)
    list(APPEND _turbowasm_wasi02_cnet_tests
      turbowasm_wasi02_cnet_dynamic_poll_test)
  endif()

  # Salts::CNet is a released shared SDK dependency. Keep its runtime payload
  # explicit for CTest instead of relying on a system-wide Salts install.
  if(WIN32)
    set_tests_properties(${_turbowasm_wasi02_cnet_tests} PROPERTIES
      ENVIRONMENT
        "PATH=$<TARGET_FILE_DIR:Salts::CNet>$<SEMICOLON>${SALTS_ROOT_PATH}/bin$<SEMICOLON>${SALTS_ROOT_PATH}/lib$<SEMICOLON>$ENV{PATH}")
  elseif(APPLE)
    set_tests_properties(${_turbowasm_wasi02_cnet_tests} PROPERTIES
      ENVIRONMENT
        "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:Salts::CNet>:${SALTS_ROOT_PATH}/lib:$ENV{DYLD_LIBRARY_PATH}")
  else()
    set_tests_properties(${_turbowasm_wasi02_cnet_tests} PROPERTIES
      ENVIRONMENT
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Salts::CNet>:${SALTS_ROOT_PATH}/lib:$ENV{LD_LIBRARY_PATH}")
  endif()
  unset(_turbowasm_wasi02_cnet_tests)
endif()

set(_turbowasm_wasi02_fixture_dir
  "${PROJECT_SOURCE_DIR}/tests/wasi02/fixtures/generated")
if(TARGET turbowasm_wasi02)
  add_executable(turbowasm_wasi02_toolchain_fixture_test
    wasi02_toolchain_fixture_test.c)
  target_link_libraries(turbowasm_wasi02_toolchain_fixture_test
    PRIVATE TurboWasm::WASI02)
  target_include_directories(turbowasm_wasi02_toolchain_fixture_test
    PRIVATE "${_turbowasm_wasi02_fixture_dir}")
  set_target_properties(
    turbowasm_wasi02_toolchain_fixture_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_toolchain_fixture_test
           COMMAND turbowasm_wasi02_toolchain_fixture_test)
endif()

if(TARGET turbowasm_wasi02)
  add_executable(turbowasm_wasi02_public_test
    wasi02_public_test.c)
  target_link_libraries(turbowasm_wasi02_public_test
    PRIVATE TurboWasm::WASI02)
  set_target_properties(turbowasm_wasi02_public_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_public_test
           COMMAND turbowasm_wasi02_public_test)
endif()

if(TARGET turbowasm_wasi02)
  add_executable(turbowasm_wasi02_toolchain_resume_test
    wasi02_toolchain_resume_test.c)
  target_link_libraries(turbowasm_wasi02_toolchain_resume_test
    PRIVATE TurboWasm::WASI02)
  target_include_directories(turbowasm_wasi02_toolchain_resume_test
    PRIVATE "${_turbowasm_wasi02_fixture_dir}")
  set_target_properties(
    turbowasm_wasi02_toolchain_resume_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_toolchain_resume_test
           COMMAND turbowasm_wasi02_toolchain_resume_test)
endif()

if(TARGET turbowasm_wasi02_exec_core)
  add_executable(turbowasm_wasi02_exec_test
    wasi02_exec_test.c)
  target_link_libraries(turbowasm_wasi02_exec_test
    PRIVATE
      turbowasm_wasi02_exec_core
      turbowasm_wasi02_core
      turbowasm_wasi02_poll_core
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_fs_core
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_exec_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_exec_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_exec_test
           COMMAND turbowasm_wasi02_exec_test)
endif()

if(TARGET turbowasm_wasi02_streams_core)
  add_executable(turbowasm_wasi02_stdio_stream_test
    wasi02_stdio_stream_test.c)
  target_link_libraries(turbowasm_wasi02_stdio_stream_test
    PRIVATE
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_core
      turbowasm_wasi02_poll_core
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_stdio_stream_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_stdio_stream_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_stdio_stream_test
           COMMAND turbowasm_wasi02_stdio_stream_test)

  add_executable(turbowasm_wasi02_stream_component_bridge_test
    wasi02_stream_component_bridge_test.c)
  target_link_libraries(turbowasm_wasi02_stream_component_bridge_test
    PRIVATE
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_poll_core
      turbowasm_wasi02_core
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_stream_component_bridge_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(
    turbowasm_wasi02_stream_component_bridge_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_stream_component_bridge_test
           COMMAND turbowasm_wasi02_stream_component_bridge_test)

  add_executable(turbowasm_wasi02_streams_test
    wasi02_streams_test.c)
  target_link_libraries(turbowasm_wasi02_streams_test
    PRIVATE
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_poll_core
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_streams_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_streams_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_streams_test
           COMMAND turbowasm_wasi02_streams_test)

  add_executable(turbowasm_wasi02_stream_blocking_test
    wasi02_stream_blocking_test.c)
  target_link_libraries(turbowasm_wasi02_stream_blocking_test
    PRIVATE
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_poll_core
      TurboWasm::Runtime
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_stream_blocking_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_stream_blocking_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_stream_blocking_test
           COMMAND turbowasm_wasi02_stream_blocking_test)

  add_executable(turbowasm_wasi02_stream_output_blocking_test
    wasi02_stream_output_blocking_test.c)
  target_link_libraries(turbowasm_wasi02_stream_output_blocking_test
    PRIVATE
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_poll_core
      TurboWasm::Runtime
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_stream_output_blocking_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_stream_output_blocking_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_stream_output_blocking_test
           COMMAND turbowasm_wasi02_stream_output_blocking_test)

  add_executable(turbowasm_wasi02_stream_splice_test
    wasi02_stream_splice_test.c)
  target_link_libraries(turbowasm_wasi02_stream_splice_test
    PRIVATE
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_poll_core
      TurboWasm::Runtime
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_stream_splice_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_stream_splice_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_stream_splice_test
           COMMAND turbowasm_wasi02_stream_splice_test)
endif()

if(TARGET turbowasm_wasi02_poll_core)
  add_executable(turbowasm_wasi02_poll_test
    wasi02_poll_test.c)
  target_link_libraries(turbowasm_wasi02_poll_test
    PRIVATE
      turbowasm_wasi02_poll_core
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_poll_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_poll_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_poll_test
           COMMAND turbowasm_wasi02_poll_test)

  add_executable(turbowasm_wasi02_poll_wait_test
    wasi02_poll_wait_test.c)
  target_link_libraries(turbowasm_wasi02_poll_wait_test
    PRIVATE
      turbowasm_wasi02_poll_core
      TurboWasm::Runtime
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_poll_wait_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_poll_wait_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_poll_wait_test
           COMMAND turbowasm_wasi02_poll_wait_test)
endif()

if(TARGET turbowasm_wasi02_poll_native_io_core)
  add_executable(turbowasm_wasi02_poll_native_io_test
    wasi02_poll_native_io_test.c)
  target_link_libraries(turbowasm_wasi02_poll_native_io_test
    PRIVATE
      turbowasm_wasi02_poll_native_io_core
      turbowasm_wasi02_poll_core
      TurboWasm::NativeIO
      TurboWasm::Runtime)
  target_include_directories(turbowasm_wasi02_poll_native_io_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_poll_native_io_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_poll_native_io_test
           COMMAND turbowasm_wasi02_poll_native_io_test)
endif()

if(TARGET turbowasm_wasi02_poll_native_io_core AND
   TARGET turbowasm_wasi02_sockets_core AND
   TARGET turbowasm_wasi02_streams_core)
  add_executable(turbowasm_wasi02_socket_native_io_test
    wasi02_socket_native_io_test.c)
  target_link_libraries(turbowasm_wasi02_socket_native_io_test
    PRIVATE
      turbowasm_wasi02_sockets_core
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_poll_native_io_core
      turbowasm_wasi02_poll_core
      TurboWasm::NativeIO
      TurboWasm::Component
      TurboWasm::Runtime)
  target_include_directories(turbowasm_wasi02_socket_native_io_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_socket_native_io_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_socket_native_io_test
           COMMAND turbowasm_wasi02_socket_native_io_test)
endif()

if(TARGET turbowasm_wasi02_fs_core)
  add_executable(turbowasm_wasi02_filesystem_test
    wasi02_filesystem_test.c)
  target_link_libraries(turbowasm_wasi02_filesystem_test
    PRIVATE
      turbowasm_wasi02_fs_core
      TurboWasm::WASI
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_filesystem_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_filesystem_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_filesystem_test
           COMMAND turbowasm_wasi02_filesystem_test)

  add_executable(turbowasm_wasi02_filesystem_ops_test
    wasi02_filesystem_ops_test.c)
  target_link_libraries(turbowasm_wasi02_filesystem_ops_test
    PRIVATE
      turbowasm_wasi02_fs_core
      TurboWasm::WASI
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_filesystem_ops_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_filesystem_ops_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_filesystem_ops_test
           COMMAND turbowasm_wasi02_filesystem_ops_test)

  add_executable(turbowasm_wasi02_filesystem_component_exec_test
    wasi02_filesystem_component_exec_test.c)
  target_link_libraries(turbowasm_wasi02_filesystem_component_exec_test
    PRIVATE
      turbowasm_wasi02_fs_core
      TurboWasm::WASI
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_filesystem_component_exec_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(turbowasm_wasi02_filesystem_component_exec_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_filesystem_component_exec_test
           COMMAND turbowasm_wasi02_filesystem_component_exec_test)

  add_executable(turbowasm_wasi02_filesystem_method_exec_test
    wasi02_filesystem_method_exec_test.c)
  target_link_libraries(turbowasm_wasi02_filesystem_method_exec_test
    PRIVATE
      turbowasm_wasi02_fs_core
      TurboWasm::WASI
      TurboWasm::Component)
  target_include_directories(turbowasm_wasi02_filesystem_method_exec_test
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/include")
  set_target_properties(
    turbowasm_wasi02_filesystem_method_exec_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi02_filesystem_method_exec_test
           COMMAND turbowasm_wasi02_filesystem_method_exec_test)
endif()

add_executable(turbowasm_runtime_config_test runtime_config_test.c)
target_link_libraries(turbowasm_runtime_config_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_runtime_config_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_runtime_config_test COMMAND turbowasm_runtime_config_test)

add_executable(turbowasm_simd_test simd_test.c)
target_link_libraries(turbowasm_simd_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_simd_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_test COMMAND turbowasm_simd_test)

add_executable(turbowasm_header_cpp_test header_cpp_test.cpp)
target_link_libraries(turbowasm_header_cpp_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_header_cpp_test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
add_test(NAME turbowasm_header_cpp_test COMMAND turbowasm_header_cpp_test)

add_executable(turbowasm_semantic_type_test
  semantic_type_test.c)
target_link_libraries(turbowasm_semantic_type_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_semantic_type_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_semantic_type_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_semantic_type_test
         COMMAND turbowasm_semantic_type_test)

add_executable(turbowasm_validation_test validation_test.c)
target_link_libraries(turbowasm_validation_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_validation_test COMMAND turbowasm_validation_test)

add_executable(turbowasm_linkage_validation_test linkage_validation_test.c)
target_link_libraries(turbowasm_linkage_validation_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_linkage_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_linkage_validation_test
         COMMAND turbowasm_linkage_validation_test)

add_executable(turbowasm_tag_validation_test tag_validation_test.c)
target_link_libraries(turbowasm_tag_validation_test PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_tag_validation_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_tag_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_tag_validation_test
         COMMAND turbowasm_tag_validation_test)

add_executable(turbowasm_eh_runtime_test eh_runtime_test.c)
target_link_libraries(turbowasm_eh_runtime_test PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_eh_runtime_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_eh_runtime_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_eh_runtime_test
         COMMAND turbowasm_eh_runtime_test)

add_executable(turbowasm_eh_cross_frame_test eh_cross_frame_test.c)
target_link_libraries(turbowasm_eh_cross_frame_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_eh_cross_frame_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_eh_cross_frame_test
         COMMAND turbowasm_eh_cross_frame_test)

add_executable(turbowasm_eh_validation_test eh_validation_test.c)
target_link_libraries(turbowasm_eh_validation_test PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_eh_validation_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_eh_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_eh_validation_test
         COMMAND turbowasm_eh_validation_test)

add_executable(turbowasm_linked_tag_test linked_tag_test.c)
target_link_libraries(turbowasm_linked_tag_test PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_linked_tag_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_linked_tag_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_linked_tag_test
         COMMAND turbowasm_linked_tag_test)

add_executable(turbowasm_data_validation_test data_validation_test.c)
target_link_libraries(turbowasm_data_validation_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_data_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_data_validation_test
         COMMAND turbowasm_data_validation_test)

add_executable(turbowasm_element_validation_test element_validation_test.c)
target_link_libraries(turbowasm_element_validation_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_element_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_element_validation_test
         COMMAND turbowasm_element_validation_test)

add_executable(turbowasm_instruction_validation_test instruction_validation_test.c)
target_link_libraries(turbowasm_instruction_validation_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_instruction_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_instruction_validation_test
         COMMAND turbowasm_instruction_validation_test)

add_executable(turbowasm_structured_control_validation_test
  structured_control_validation_test.c)
target_link_libraries(turbowasm_structured_control_validation_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_structured_control_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_structured_control_validation_test
         COMMAND turbowasm_structured_control_validation_test)

add_executable(turbowasm_memory_numeric_validation_test
  memory_numeric_validation_test.c)
target_link_libraries(turbowasm_memory_numeric_validation_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_memory_numeric_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_memory_numeric_validation_test
         COMMAND turbowasm_memory_numeric_validation_test)

add_executable(turbowasm_memory64_validation_test
  memory64_validation_test.c)
target_link_libraries(turbowasm_memory64_validation_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_memory64_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_memory64_validation_test
         COMMAND turbowasm_memory64_validation_test)

add_executable(turbowasm_memory64_runtime_test
  memory64_runtime_test.c)
target_link_libraries(turbowasm_memory64_runtime_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_memory64_runtime_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_memory64_runtime_test
         COMMAND turbowasm_memory64_runtime_test)

add_executable(turbowasm_memory64_host_link_test
  memory64_host_link_test.c)
target_link_libraries(turbowasm_memory64_host_link_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_memory64_host_link_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_memory64_host_link_test
         COMMAND turbowasm_memory64_host_link_test)

add_executable(turbowasm_multi_memory_scalar_test
  multi_memory_scalar_test.c)
target_link_libraries(turbowasm_multi_memory_scalar_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_multi_memory_scalar_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_multi_memory_scalar_test
         COMMAND turbowasm_multi_memory_scalar_test)

add_executable(turbowasm_custom_page_size_test
  custom_page_size_test.c)
target_link_libraries(turbowasm_custom_page_size_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_custom_page_size_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_custom_page_size_test
         COMMAND turbowasm_custom_page_size_test)

add_executable(turbowasm_bulk_validation_test
  bulk_validation_test.c)
target_link_libraries(turbowasm_bulk_validation_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_bulk_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_bulk_validation_test
         COMMAND turbowasm_bulk_validation_test)

add_executable(turbowasm_code_metadata_test
  code_metadata_test.c)
target_link_libraries(turbowasm_code_metadata_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_code_metadata_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_code_metadata_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_code_metadata_test
         COMMAND turbowasm_code_metadata_test)


add_executable(turbowasm_control_signature_metadata_test
  control_signature_metadata_test.c)
target_link_libraries(turbowasm_control_signature_metadata_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_control_signature_metadata_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_control_signature_metadata_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_control_signature_metadata_test
         COMMAND turbowasm_control_signature_metadata_test)

add_executable(turbowasm_instance_test
  instance_test.c)
target_link_libraries(turbowasm_instance_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_instance_test
         COMMAND turbowasm_instance_test)


add_executable(turbowasm_scalar_numeric_runtime_test
  scalar_numeric_runtime_test.c)
target_link_libraries(turbowasm_scalar_numeric_runtime_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_scalar_numeric_runtime_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_scalar_numeric_runtime_test
         COMMAND turbowasm_scalar_numeric_runtime_test)


add_executable(turbowasm_linker_test
  linker_test.c)
target_link_libraries(turbowasm_linker_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_linker_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_linker_test
         COMMAND turbowasm_linker_test)

add_executable(turbowasm_shared_memory_access_test
  shared_memory_access_test.c)
target_link_libraries(turbowasm_shared_memory_access_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_shared_memory_access_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_shared_memory_access_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_shared_memory_access_test
         COMMAND turbowasm_shared_memory_access_test)

add_executable(turbowasm_host_function_test
  host_function_test.c)
target_link_libraries(turbowasm_host_function_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_host_function_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_host_function_test
         COMMAND turbowasm_host_function_test)

add_executable(turbowasm_host_memory_test
  host_memory_test.c)
target_link_libraries(turbowasm_host_memory_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_host_memory_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_host_memory_test
         COMMAND turbowasm_host_memory_test)

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_preview1_test
    wasi_preview1_test.c)
  target_link_libraries(turbowasm_wasi_preview1_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_preview1_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_preview1_test
           COMMAND turbowasm_wasi_preview1_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_clock_random_test
    wasi_clock_random_test.c)
  target_link_libraries(turbowasm_wasi_clock_random_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_clock_random_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_clock_random_test
           COMMAND turbowasm_wasi_clock_random_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_fd_io_test
    wasi_fd_io_test.c)
  target_link_libraries(turbowasm_wasi_fd_io_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_fd_io_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fd_io_test
           COMMAND turbowasm_wasi_fd_io_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_fs_test
    wasi_fs_test.c)
  target_link_libraries(turbowasm_wasi_fs_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_fs_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fs_test
           COMMAND turbowasm_wasi_fs_test)
endif()

add_executable(turbowasm_host_wait_test
  host_wait_test.c)
target_link_libraries(turbowasm_host_wait_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_host_wait_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_host_wait_test
         COMMAND turbowasm_host_wait_test)


add_executable(turbowasm_linked_table_test
  linked_table_test.c)
target_link_libraries(turbowasm_linked_table_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_linked_table_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_linked_table_test
         COMMAND turbowasm_linked_table_test)

add_executable(turbowasm_structured_instance_test
  structured_instance_test.c)
target_link_libraries(turbowasm_structured_instance_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_structured_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_structured_instance_test
         COMMAND turbowasm_structured_instance_test)

add_executable(turbowasm_retained_instance_metadata_test
  retained_instance_metadata_test.c)
target_link_libraries(turbowasm_retained_instance_metadata_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_retained_instance_metadata_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_retained_instance_metadata_test
         COMMAND turbowasm_retained_instance_metadata_test)

add_executable(turbowasm_retained_segment_metadata_test
  retained_segment_metadata_test.c)
target_link_libraries(turbowasm_retained_segment_metadata_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_retained_segment_metadata_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_retained_segment_metadata_test
         COMMAND turbowasm_retained_segment_metadata_test)

add_executable(turbowasm_instance_state_test
  instance_state_test.c)
target_link_libraries(turbowasm_instance_state_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_instance_state_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_instance_state_test
         COMMAND turbowasm_instance_state_test)

add_executable(turbowasm_reference_table_test
  reference_table_test.c)
target_link_libraries(turbowasm_reference_table_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_reference_table_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_reference_table_test
         COMMAND turbowasm_reference_table_test)


add_executable(turbowasm_funcref_identity_test
  funcref_identity_test.c)
target_link_libraries(turbowasm_funcref_identity_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_funcref_identity_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_funcref_identity_test
         COMMAND turbowasm_funcref_identity_test)

add_executable(turbowasm_bulk_runtime_test
  bulk_runtime_test.c)
target_link_libraries(turbowasm_bulk_runtime_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_bulk_runtime_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_bulk_runtime_test
         COMMAND turbowasm_bulk_runtime_test)

add_executable(turbowasm_simd_descriptor_validation_test
  simd_descriptor_validation_test.c)
target_link_libraries(turbowasm_simd_descriptor_validation_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_simd_descriptor_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_descriptor_validation_test
         COMMAND turbowasm_simd_descriptor_validation_test)

add_executable(turbowasm_simd_memory_validation_test
  simd_memory_validation_test.c)
target_link_libraries(turbowasm_simd_memory_validation_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_simd_memory_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_memory_validation_test
         COMMAND turbowasm_simd_memory_validation_test)

add_executable(turbowasm_simd_instance_test
  simd_instance_test.c)
target_link_libraries(turbowasm_simd_instance_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_simd_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_instance_test
         COMMAND turbowasm_simd_instance_test)

add_executable(turbowasm_simd_exec_table_test
  simd_exec_table_test.c)
target_link_libraries(turbowasm_simd_exec_table_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_simd_exec_table_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_simd_exec_table_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_exec_table_test
         COMMAND turbowasm_simd_exec_table_test)

add_executable(turbowasm_simd_memory_instance_test
  simd_memory_instance_test.c)
target_link_libraries(turbowasm_simd_memory_instance_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_simd_memory_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_memory_instance_test
         COMMAND turbowasm_simd_memory_instance_test)

add_executable(turbowasm_simd_lane_instance_test
  simd_lane_instance_test.c)
target_link_libraries(turbowasm_simd_lane_instance_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_simd_lane_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_lane_instance_test
         COMMAND turbowasm_simd_lane_instance_test)


add_executable(turbowasm_simd_advanced_instance_test
  simd_advanced_instance_test.c)
target_link_libraries(turbowasm_simd_advanced_instance_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_simd_advanced_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_advanced_instance_test
         COMMAND turbowasm_simd_advanced_instance_test)


add_executable(turbowasm_simd_widen_instance_test
  simd_widen_instance_test.c)
target_link_libraries(turbowasm_simd_widen_instance_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_simd_widen_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_widen_instance_test
         COMMAND turbowasm_simd_widen_instance_test)


add_executable(turbowasm_simd_conversion_instance_test
  simd_conversion_instance_test.c)
target_link_libraries(turbowasm_simd_conversion_instance_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_simd_conversion_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_conversion_instance_test
         COMMAND turbowasm_simd_conversion_instance_test)


add_executable(turbowasm_execution_control_test
  execution_control_test.c)
target_link_libraries(turbowasm_execution_control_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_execution_control_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_execution_control_test
         COMMAND turbowasm_execution_control_test)

add_executable(turbowasm_resumable_execution_test
  resumable_execution_test.c)
target_link_libraries(turbowasm_resumable_execution_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_resumable_execution_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_resumable_execution_test
         COMMAND turbowasm_resumable_execution_test)


add_executable(turbowasm_jit_backend_contract_test
  jit_backend_contract_test.c)
target_link_libraries(turbowasm_jit_backend_contract_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_jit_backend_contract_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_jit_backend_contract_test
         COMMAND turbowasm_jit_backend_contract_test)


add_executable(turbowasm_jit_direct_call_helper_test
  jit_direct_call_helper_test.c)
target_link_libraries(turbowasm_jit_direct_call_helper_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_jit_direct_call_helper_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_jit_direct_call_helper_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_jit_direct_call_helper_test
         COMMAND turbowasm_jit_direct_call_helper_test)


add_executable(turbowasm_jit_tiering_test
  jit_tiering_test.c)
target_link_libraries(turbowasm_jit_tiering_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_jit_tiering_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_jit_tiering_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_jit_tiering_test
         COMMAND turbowasm_jit_tiering_test)


add_executable(turbowasm_jit_artifact_cache_test
  jit_artifact_cache_test.c)
target_link_libraries(turbowasm_jit_artifact_cache_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_jit_artifact_cache_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_jit_artifact_cache_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_jit_artifact_cache_test
         COMMAND turbowasm_jit_artifact_cache_test)


if(TURBOWASM_ENABLE_MIR_JIT)
  add_executable(turbowasm_mir_backend_test
    mir_backend_test.c)
  target_link_libraries(turbowasm_mir_backend_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  set_target_properties(turbowasm_mir_backend_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_backend_test
           COMMAND turbowasm_mir_backend_test)

  add_executable(turbowasm_mir_scalar_lowering_test
    mir_scalar_lowering_test.c)
  target_link_libraries(turbowasm_mir_scalar_lowering_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  set_target_properties(turbowasm_mir_scalar_lowering_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_scalar_lowering_test
           COMMAND turbowasm_mir_scalar_lowering_test)

  add_executable(turbowasm_mir_integer_locals_test
    mir_integer_locals_test.c)
  target_link_libraries(turbowasm_mir_integer_locals_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  set_target_properties(turbowasm_mir_integer_locals_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_integer_locals_test
           COMMAND turbowasm_mir_integer_locals_test)

  add_executable(turbowasm_mir_float_lowering_test
    mir_float_lowering_test.c)
  target_link_libraries(turbowasm_mir_float_lowering_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  set_target_properties(turbowasm_mir_float_lowering_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_float_lowering_test
           COMMAND turbowasm_mir_float_lowering_test)

  add_executable(turbowasm_mir_direct_call_test
    mir_direct_call_test.c)
  target_link_libraries(turbowasm_mir_direct_call_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  set_target_properties(turbowasm_mir_direct_call_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_direct_call_test
           COMMAND turbowasm_mir_direct_call_test)

  add_executable(turbowasm_mir_differential_safety_test
    mir_differential_safety_test.c)
  target_link_libraries(turbowasm_mir_differential_safety_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  target_include_directories(turbowasm_mir_differential_safety_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(turbowasm_mir_differential_safety_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_differential_safety_test
           COMMAND turbowasm_mir_differential_safety_test)

  add_executable(turbowasm_mir_native_safe_point_test
    mir_native_safe_point_test.c)
  target_link_libraries(turbowasm_mir_native_safe_point_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  target_include_directories(turbowasm_mir_native_safe_point_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(turbowasm_mir_native_safe_point_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_native_safe_point_test
           COMMAND turbowasm_mir_native_safe_point_test)

  add_executable(turbowasm_mir_structured_control_test
    mir_structured_control_test.c)
  target_link_libraries(turbowasm_mir_structured_control_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  target_include_directories(turbowasm_mir_structured_control_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(turbowasm_mir_structured_control_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_structured_control_test
           COMMAND turbowasm_mir_structured_control_test)

  add_executable(turbowasm_mir_tiering_test
    mir_tiering_test.c)
  target_link_libraries(turbowasm_mir_tiering_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  target_include_directories(turbowasm_mir_tiering_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(turbowasm_mir_tiering_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_tiering_test
           COMMAND turbowasm_mir_tiering_test)

  add_executable(turbowasm_mir_tail_call_test
    mir_tail_call_test.c)
  target_link_libraries(turbowasm_mir_tail_call_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  target_include_directories(turbowasm_mir_tail_call_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(turbowasm_mir_tail_call_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_tail_call_test
           COMMAND turbowasm_mir_tail_call_test)

  add_executable(turbowasm_mir_simd_helper_lowering_test
    mir_simd_helper_lowering_test.c)
  target_link_libraries(turbowasm_mir_simd_helper_lowering_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  target_include_directories(turbowasm_mir_simd_helper_lowering_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(turbowasm_mir_simd_helper_lowering_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_simd_helper_lowering_test
           COMMAND turbowasm_mir_simd_helper_lowering_test)

  add_executable(turbowasm_mir_simd_structured_test
    mir_simd_structured_test.c)
  target_link_libraries(turbowasm_mir_simd_structured_test
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  target_include_directories(turbowasm_mir_simd_structured_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(turbowasm_mir_simd_structured_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_mir_simd_structured_test
           COMMAND turbowasm_mir_simd_structured_test)
endif()

add_executable(turbowasm_jit_simd_helper_test
  jit_simd_helper_test.c)
target_link_libraries(turbowasm_jit_simd_helper_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_jit_simd_helper_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_jit_simd_helper_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_jit_simd_helper_test
         COMMAND turbowasm_jit_simd_helper_test)


if(TURBOWASM_ENABLE_WASI_NATIVE_IO_ADAPTER)
  add_executable(turbowasm_wasi_native_io_test
    wasi_native_io_test.c)
  target_link_libraries(turbowasm_wasi_native_io_test
    PRIVATE
      TurboWasm::WASI
      Salts::NativeIO)
  set_target_properties(turbowasm_wasi_native_io_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_native_io_test
           COMMAND turbowasm_wasi_native_io_test)
endif()

if(TURBOWASM_ENABLE_NATIVE_IO_ADAPTER)
  add_executable(turbowasm_native_io_adapter_test
    native_io_adapter_test.c)
  target_link_libraries(turbowasm_native_io_adapter_test
    PRIVATE TurboWasm::NativeIO)
  set_target_properties(turbowasm_native_io_adapter_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_native_io_adapter_test
           COMMAND turbowasm_native_io_adapter_test)

  add_executable(turbowasm_native_io_deterministic_test
    native_io_deterministic_test.c)
  target_link_libraries(turbowasm_native_io_deterministic_test
    PRIVATE
      TurboWasm::Runtime
      Salts::NativeIO)
  set_target_properties(turbowasm_native_io_deterministic_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_native_io_deterministic_test
           COMMAND turbowasm_native_io_deterministic_test)
endif()

if(TURBOWASM_ENABLE_CFLOW_ADAPTER)
  add_executable(turbowasm_cflow_adapter_test
    cflow_adapter_test.c)
  target_link_libraries(turbowasm_cflow_adapter_test
    PRIVATE TurboWasm::CFlow)
  set_target_properties(turbowasm_cflow_adapter_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_cflow_adapter_test
           COMMAND turbowasm_cflow_adapter_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_smoke_corpus_test
    wasi_smoke_corpus_test.c)
  target_link_libraries(turbowasm_wasi_smoke_corpus_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_smoke_corpus_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_smoke_corpus_test
           COMMAND turbowasm_wasi_smoke_corpus_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_cmeta_manifest_test
    wasi_cmeta_manifest_test.c)
  target_link_libraries(turbowasm_wasi_cmeta_manifest_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_cmeta_manifest_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_cmeta_manifest_test
           COMMAND turbowasm_wasi_cmeta_manifest_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_adapter_plan_test
    wasi_adapter_plan_test.c)
  target_include_directories(turbowasm_wasi_adapter_plan_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(turbowasm_wasi_adapter_plan_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_adapter_plan_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_adapter_plan_test
           COMMAND turbowasm_wasi_adapter_plan_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_fs_seek_stat_test
    wasi_fs_seek_stat_test.c)
  target_link_libraries(turbowasm_wasi_fs_seek_stat_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_fs_seek_stat_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fs_seek_stat_test
           COMMAND turbowasm_wasi_fs_seek_stat_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_fs_path_open_test
    wasi_fs_path_open_test.c)
  target_link_libraries(turbowasm_wasi_fs_path_open_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_fs_path_open_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fs_path_open_test
           COMMAND turbowasm_wasi_fs_path_open_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_fs_path_stat_test
    wasi_fs_path_stat_test.c)
  target_link_libraries(turbowasm_wasi_fs_path_stat_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_fs_path_stat_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fs_path_stat_test
           COMMAND turbowasm_wasi_fs_path_stat_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_fs_path_mutation_test
    wasi_fs_path_mutation_test.c)
  target_link_libraries(turbowasm_wasi_fs_path_mutation_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_fs_path_mutation_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fs_path_mutation_test
           COMMAND turbowasm_wasi_fs_path_mutation_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_fs_readdir_test
    wasi_fs_readdir_test.c)
  target_link_libraries(turbowasm_wasi_fs_readdir_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_fs_readdir_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fs_readdir_test
           COMMAND turbowasm_wasi_fs_readdir_test)
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_executable(turbowasm_wasi_fs_contract_fake_test
    wasi_fs_contract.c
    wasi_fs_contract_fake_test.c)
  target_link_libraries(turbowasm_wasi_fs_contract_fake_test
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_wasi_fs_contract_fake_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fs_contract_fake_test
           COMMAND turbowasm_wasi_fs_contract_fake_test)
endif()

if(TURBOWASM_ENABLE_WASI_HOST_FS_ADAPTER)
  add_executable(turbowasm_wasi_fs_contract_host_test
    wasi_fs_contract.c
    wasi02_fs_contract.c
    wasi_fs_contract_host_test.c)
  target_include_directories(turbowasm_wasi_fs_contract_host_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(turbowasm_wasi_fs_contract_host_test
    PRIVATE
      TurboWasm::WASIHostFS
      turbowasm_wasi02_fs_core
      turbowasm_wasi02_streams_core
      TurboWasm::Component)
  set_target_properties(
    turbowasm_wasi_fs_contract_host_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fs_contract_host_test
           COMMAND turbowasm_wasi_fs_contract_host_test)

  add_executable(turbowasm_wasi_fs_host_security_test
    wasi_fs_host_security_test.c)
  target_link_libraries(turbowasm_wasi_fs_host_security_test
    PRIVATE TurboWasm::WASIHostFS)
  set_target_properties(
    turbowasm_wasi_fs_host_security_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fs_host_security_test
           COMMAND turbowasm_wasi_fs_host_security_test)
endif()

if(TURBOWASM_ENABLE_WASI_LITTLEFS_ADAPTER)
  add_executable(turbowasm_wasi_fs_contract_littlefs_test
    wasi_fs_contract.c
    wasi02_fs_contract.c
    wasi_fs_contract_littlefs_test.c)
  target_include_directories(turbowasm_wasi_fs_contract_littlefs_test
    PRIVATE
      "${TURBOWASM_LITTLEFS_SOURCE_DIR}"
      "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(turbowasm_wasi_fs_contract_littlefs_test
    PRIVATE
      TurboWasm::WASILittleFS
      turbowasm_wasi02_fs_core
      turbowasm_wasi02_streams_core
      TurboWasm::Component)
  set_target_properties(
    turbowasm_wasi_fs_contract_littlefs_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_fs_contract_littlefs_test
           COMMAND turbowasm_wasi_fs_contract_littlefs_test)
endif()

add_executable(turbowasm_shared_memory_path_test
  shared_memory_path_test.c)
target_link_libraries(turbowasm_shared_memory_path_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_shared_memory_path_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_shared_memory_path_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_shared_memory_path_test
         COMMAND turbowasm_shared_memory_path_test)

add_executable(turbowasm_shared_memory_instance_test
  shared_memory_instance_test.c)
target_link_libraries(turbowasm_shared_memory_instance_test
  PRIVATE TurboWasm::Runtime Salts::Platform)
target_include_directories(turbowasm_shared_memory_instance_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_shared_memory_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_shared_memory_instance_test
         COMMAND turbowasm_shared_memory_instance_test)

add_executable(turbowasm_atomic_descriptor_test
  atomic_descriptor_test.c)
target_link_libraries(turbowasm_atomic_descriptor_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_atomic_descriptor_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_atomic_descriptor_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_atomic_descriptor_test
         COMMAND turbowasm_atomic_descriptor_test)

add_executable(turbowasm_atomic_validation_test
  atomic_validation_test.c)
target_link_libraries(turbowasm_atomic_validation_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_atomic_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_atomic_validation_test
         COMMAND turbowasm_atomic_validation_test)

add_executable(turbowasm_atomic_runtime_test
  atomic_runtime_test.c)
target_link_libraries(turbowasm_atomic_runtime_test
  PRIVATE TurboWasm::Runtime Salts::Platform)
target_include_directories(turbowasm_atomic_runtime_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_atomic_runtime_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_atomic_runtime_test
         COMMAND turbowasm_atomic_runtime_test)

add_executable(turbowasm_wait_notify_test
  wait_notify_test.c)
target_link_libraries(turbowasm_wait_notify_test
  PRIVATE TurboWasm::Runtime Salts::Platform)
target_include_directories(turbowasm_wait_notify_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_wait_notify_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_wait_notify_test
         COMMAND turbowasm_wait_notify_test)
set_tests_properties(turbowasm_wait_notify_test PROPERTIES
  TIMEOUT 20)


if(TURBOWASM_ENABLE_WASI_THREADS_ADAPTER)
  add_executable(turbowasm_wasi_threads_test
    wasi_threads_test.c)
  target_link_libraries(turbowasm_wasi_threads_test
    PRIVATE TurboWasm::WASIThreads Salts::Platform)
  set_target_properties(turbowasm_wasi_threads_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_threads_test
           COMMAND turbowasm_wasi_threads_test)
  set_tests_properties(turbowasm_wasi_threads_test PROPERTIES
    TIMEOUT 20)
  add_executable(turbowasm_wasi_threads_group_test
    wasi_threads_group_test.c)
  target_link_libraries(turbowasm_wasi_threads_group_test
    PRIVATE TurboWasm::WASIThreads Salts::Platform)
  set_target_properties(turbowasm_wasi_threads_group_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_threads_group_test
           COMMAND turbowasm_wasi_threads_group_test)
  set_tests_properties(turbowasm_wasi_threads_group_test PROPERTIES
    TIMEOUT 20)

  add_executable(turbowasm_wasi_threads_header_cpp_test
    wasi_threads_header_cpp_test.cpp)
  target_link_libraries(turbowasm_wasi_threads_header_cpp_test
    PRIVATE TurboWasm::WASIThreads)
  set_target_properties(turbowasm_wasi_threads_header_cpp_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(NAME turbowasm_wasi_threads_header_cpp_test
           COMMAND turbowasm_wasi_threads_header_cpp_test)

  if(TURBOWASM_ENABLE_WASI_ADAPTER)
    add_executable(turbowasm_wasi_threads_proc_exit_test
      wasi_threads_proc_exit_test.c)
    target_link_libraries(turbowasm_wasi_threads_proc_exit_test
      PRIVATE
        TurboWasm::WASIThreads
        TurboWasm::WASI
        Salts::Platform)
    set_target_properties(turbowasm_wasi_threads_proc_exit_test PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF)
    add_test(NAME turbowasm_wasi_threads_proc_exit_test
             COMMAND turbowasm_wasi_threads_proc_exit_test)
    set_tests_properties(turbowasm_wasi_threads_proc_exit_test PROPERTIES
      TIMEOUT 20)
  endif()
endif()
