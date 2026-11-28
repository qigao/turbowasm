cmake_minimum_required(VERSION 3.25)



project(TurboWasm LANGUAGES C CXX)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
include(CMakeOptions.cmake)
include(cmake/Sanitizers.cmake)
find_package(SaltsBase CONFIG REQUIRED PATHS "$ENV{SALTS_BASE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(CMeta CONFIG REQUIRED PATHS "$ENV{CMETA_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsCNet CONFIG REQUIRED PATHS "$ENV{SALTSCORE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils CONFIG REQUIRED PATHS "$ENV{SALTS_UTILS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(Chttp CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
include(cmake/SaltsCompilerOptions.cmake)
turbowasm_normalize_salts_c11_atomics()

# Mirror the namespaced dependency headers used by the public ABI.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdk_include/salts")
foreach(header simd.h native_io.h)
  configure_file("$ENV{SALTS_BASE_ROOT}/include/${header}"
    "${CMAKE_CURRENT_BINARY_DIR}/sdk_include/salts/${header}" COPYONLY)
endforeach()

add_library(turbowasm
  wasm/src/store.c
  wasm/src/gc_exec.c
  wasm/src/artifact.c
  wasm/src/atomic.c
  wasm/src/component_binary.c
  wasm/src/component_async_state.c
  wasm/src/component_task.c
  wasm/src/component_subtask.c
  wasm/src/component_async_call.c
  wasm/src/component_task_builtin.c
  wasm/src/component_waitable.c
  wasm/src/component_endpoint.c
  wasm/src/component_endpoint_builtin.c
  wasm/src/component_buffer.c
  wasm/src/component_canonical.c
  wasm/src/component_string.c
  wasm/src/component_core_call.c
  wasm/src/component_exec.c
  wasm/src/component_exec_async.c
  wasm/src/component_exec_async_resource.c
  wasm/src/component_resource.c
  wasm/src/component_resource_binding.c
  wasm/src/component_type_graph.c
  wasm/src/component_type_view.c
  wasm/src/execution.c
  wasm/src/instance.c
  wasm/src/instance_state.c
  wasm/src/jit_reference_helper.c
  wasm/src/jit_gc_helper.c
  wasm/src/jit_table_helper.c
  wasm/src/jit_memory_helper.c
  wasm/src/jit_simd_helper.c
  wasm/src/link.c
  wasm/src/module.c
  wasm/src/reader.c
  wasm/src/relaxed_simd.c
  wasm/src/runtime_alloc.c
  wasm/src/sha256.c
  wasm/src/simd.c
  wasm/src/simd_exec_table.c
  wasm/src/status.c
  wasm/src/value.c
  wasm/src/validation_context.c
  wasm/src/validate.c
  wasm/src/validate_composite.c
  wasm/src/validate_data.c
  wasm/src/validate_element.c
  wasm/src/validate_instr.c
  wasm/src/validate_simd_table.c
  wasm/src/validate_linkage.c
  wasm/src/validate_type.c)

add_library(TurboWasm::Runtime ALIAS turbowasm)

set_target_properties(turbowasm PROPERTIES
  EXPORT_NAME Runtime
  POSITION_INDEPENDENT_CODE ON)

target_compile_features(turbowasm PUBLIC c_std_11)
target_compile_definitions(turbowasm PRIVATE
  TURBOWASM_TYPE_DEPENDENCY_LIMIT=${TURBOWASM_TYPE_DEPENDENCY_LIMIT})
target_include_directories(turbowasm
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wasm/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/sdk_include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(turbowasm
  PUBLIC CMeta::CMeta Salts::SIMD
  PRIVATE Salts::Coroutine Salts::Core)
if(UNIX)
  target_link_libraries(turbowasm PRIVATE m)
endif()

if(TURBOWASM_ENABLE_COMPONENT)
  add_library(turbowasm_component
    wasm/src/component_api.c
    wasm/src/component_async_api.c
    wasm/src/component_host_task.c
    wasm/src/component_host_shutdown.c
    wasm/src/component_host_transfer.c
    wasm/src/component_host_endpoint.c)
  add_library(TurboWasm::Component ALIAS turbowasm_component)
  set_target_properties(turbowasm_component PROPERTIES
    EXPORT_NAME Component)
  target_compile_features(turbowasm_component PUBLIC c_std_11)
  target_include_directories(turbowasm_component
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wasm/include>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src)
  target_link_libraries(turbowasm_component
    PUBLIC TurboWasm::Runtime)

  if(MSVC)
    target_compile_options(turbowasm_component PRIVATE /W4)
  else()
    target_compile_options(turbowasm_component PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_WASI02_ADAPTER AND
   TARGET TurboWasm::Component)
  # W1 stays internal until the descriptor/execution ABI is qualified.
  add_library(turbowasm_wasi02_core STATIC
    wasm/src/wasi02_component.c
    wasm/src/wasi02_descriptor.c
    wasm/src/wasi02_provider.c)
  target_compile_features(turbowasm_wasi02_core PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02_core
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src)
  target_link_libraries(turbowasm_wasi02_core
    PRIVATE TurboWasm::Component)
  if(MSVC)
    target_compile_options(turbowasm_wasi02_core PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi02_core PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TARGET turbowasm_wasi02_core AND TARGET TurboWasm::Component)
  # W4 pollable capability stays internal until stream/poll qualification
  # completes. It depends only on the typed WASI 0.2 core + Component layer.
  add_library(turbowasm_wasi02_poll_core STATIC
    wasm/src/wasi02_poll.c)
  target_compile_features(turbowasm_wasi02_poll_core PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02_poll_core
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/include)
  target_link_libraries(turbowasm_wasi02_poll_core
    PRIVATE
      turbowasm_wasi02_core
      TurboWasm::Component)
  if(MSVC)
    target_compile_options(turbowasm_wasi02_poll_core PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi02_poll_core PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_CFLOW_ADAPTER)
  if(NOT TARGET CMeta::CFlow)
    message(FATAL_ERROR
      "TurboWasm CFlow adapter requires a Salts SDK that exports CMeta::CFlow")
  endif()

  add_library(turbowasm_cflow
    wasm/src/cflow_adapter.c)
  add_library(TurboWasm::CFlow ALIAS turbowasm_cflow)
  set_target_properties(turbowasm_cflow PROPERTIES
    EXPORT_NAME CFlow)
  target_compile_features(turbowasm_cflow PUBLIC c_std_11)
  target_include_directories(turbowasm_cflow
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wasm/include>
      $<INSTALL_INTERFACE:include>)
  target_link_libraries(turbowasm_cflow
    PUBLIC
      TurboWasm::Runtime
      CMeta::CFlow)

  if(MSVC)
    target_compile_options(turbowasm_cflow PRIVATE /W4)
  else()
    target_compile_options(turbowasm_cflow PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_NATIVE_IO_ADAPTER)
  if(NOT TARGET Salts::NativeIO)
    message(FATAL_ERROR
      "TurboWasm NativeIO adapter requires a Salts SDK that exports Salts::NativeIO")
  endif()

  add_library(turbowasm_native_io
    wasm/src/native_io_adapter.c)
  add_library(TurboWasm::NativeIO ALIAS turbowasm_native_io)
  set_target_properties(turbowasm_native_io PROPERTIES
    EXPORT_NAME NativeIO)
  target_compile_features(turbowasm_native_io PUBLIC c_std_11)
  target_include_directories(turbowasm_native_io
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wasm/include>
      $<INSTALL_INTERFACE:include>)
  target_link_libraries(turbowasm_native_io
    PUBLIC
      TurboWasm::Runtime
      Salts::NativeIO)

  if(MSVC)
    target_compile_options(turbowasm_native_io PRIVATE /W4)
  else()
    target_compile_options(turbowasm_native_io PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TARGET turbowasm_wasi02_core AND
   TARGET turbowasm_wasi02_poll_core AND
   TARGET TurboWasm::Component)
  # W4c stream provider bridge remains internal until Component execution,
  # pollable subscription, and blocking semantics qualify.
  add_library(turbowasm_wasi02_streams_core STATIC
    wasm/src/wasi02_streams.c)
  target_compile_features(turbowasm_wasi02_streams_core PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02_streams_core
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/include)
  target_link_libraries(turbowasm_wasi02_streams_core
    PRIVATE
      turbowasm_wasi02_core
      turbowasm_wasi02_poll_core
      TurboWasm::Component)
  if(MSVC)
    target_compile_options(turbowasm_wasi02_streams_core PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi02_streams_core PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TARGET turbowasm_wasi02_core AND
   TARGET turbowasm_wasi02_poll_core AND
   TARGET turbowasm_wasi02_streams_core)
  # S2 typed sockets state/provider contract. Keep this backend-neutral:
  # CNet/NativeIO integration belongs to #393.
  add_library(turbowasm_wasi02_sockets_core STATIC
    wasm/src/wasi02_sockets.c
    wasm/src/wasi02_network.c)
  target_compile_features(turbowasm_wasi02_sockets_core PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02_sockets_core
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/include)
  target_link_libraries(turbowasm_wasi02_sockets_core
    PRIVATE
      turbowasm_wasi02_core
      turbowasm_wasi02_poll_core
      turbowasm_wasi02_streams_core
      TurboWasm::Component)
  if(MSVC)
    target_compile_options(turbowasm_wasi02_sockets_core PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi02_sockets_core PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_WASI02_CNET_ADAPTER AND
   TARGET turbowasm_wasi02_sockets_core)
  if(NOT TARGET Salts::CNet)
    message(FATAL_ERROR
      "TurboWasm WASI02 CNet adapter requires a released Salts SDK exporting Salts::CNet")
  endif()

  # #393 S3 concrete provider stays separate from the provider-neutral
  # TurboWasm::WASI02 target. This slice owns only the CNet TCP control plane;
  # data-plane streams/pollables are added separately.
  add_library(turbowasm_wasi02_cnet_core STATIC
    wasm/src/wasi02_cnet.c)
  target_compile_features(turbowasm_wasi02_cnet_core PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02_cnet_core
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/include)
  target_link_libraries(turbowasm_wasi02_cnet_core
    PRIVATE
      turbowasm_wasi02_sockets_core
      Salts::CNet)
  if(MSVC)
    target_compile_options(turbowasm_wasi02_cnet_core PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi02_cnet_core PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TARGET turbowasm_wasi02_poll_core AND TARGET TurboWasm::NativeIO)
  # W4b3 NativeIO completion routing remains internal until stream/poll
  # qualification is complete. It never submits I/O or exposes native handles.
  add_library(turbowasm_wasi02_poll_native_io_core STATIC
    wasm/src/wasi02_poll_native_io.c)
  target_compile_features(turbowasm_wasi02_poll_native_io_core PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02_poll_native_io_core
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/include)
  target_link_libraries(turbowasm_wasi02_poll_native_io_core
    PRIVATE
      turbowasm_wasi02_poll_core
      TurboWasm::NativeIO)
  if(MSVC)
    target_compile_options(turbowasm_wasi02_poll_native_io_core PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi02_poll_native_io_core PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_library(turbowasm_wasi
    wasm/src/wasi_preview1.c
    wasm/src/wasi_dispatch.c
    wasm/src/wasi_provider.c
    wasm/src/wasi_fs.c)
  add_library(TurboWasm::WASI ALIAS turbowasm_wasi)
  set_target_properties(turbowasm_wasi PROPERTIES
    EXPORT_NAME WASI)
  target_compile_features(turbowasm_wasi PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wasm/include>
      $<INSTALL_INTERFACE:include>)
  target_link_libraries(turbowasm_wasi
    PUBLIC TurboWasm::Runtime
    PRIVATE Salts::Core Salts::Core)
  if(MSVC)
    target_compile_options(turbowasm_wasi PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TARGET turbowasm_wasi02_core AND TARGET TurboWasm::WASI)
  # W3 filesystem capability bridge stays internal until resource/package
  # qualification is complete. Keep the W1/W2 core free of Preview1/WASI deps.
  add_library(turbowasm_wasi02_fs_core STATIC
    wasm/src/wasi02_filesystem.c)
  target_compile_features(turbowasm_wasi02_fs_core PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02_fs_core
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/include)
  target_link_libraries(turbowasm_wasi02_fs_core
    PRIVATE
      turbowasm_wasi02_core
      turbowasm_wasi02_streams_core
      TurboWasm::WASI
      TurboWasm::Component)
  if(MSVC)
    target_compile_options(turbowasm_wasi02_fs_core PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi02_fs_core PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TARGET turbowasm_wasi02_core AND
   TARGET turbowasm_wasi02_fs_core AND
   TARGET turbowasm_wasi02_poll_core AND
   TARGET turbowasm_wasi02_streams_core AND
   TARGET turbowasm_wasi02_sockets_core)
  # Internal aggregate import dispatcher used to qualify one Component exec
  # across the W2/W3/W4 capability families before the public W5 façade.
  add_library(turbowasm_wasi02_exec_core STATIC
    wasm/src/wasi02_exec.c)
  target_compile_features(turbowasm_wasi02_exec_core PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02_exec_core
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/include)
  target_link_libraries(turbowasm_wasi02_exec_core
    PRIVATE
      turbowasm_wasi02_core
      turbowasm_wasi02_fs_core
      turbowasm_wasi02_poll_core
      turbowasm_wasi02_streams_core
      turbowasm_wasi02_sockets_core
      TurboWasm::Component)
  if(MSVC)
    target_compile_options(turbowasm_wasi02_exec_core PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi02_exec_core PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()


if(TURBOWASM_ENABLE_WASI02_ADAPTER AND
   TARGET TurboWasm::Component)
  set(_turbowasm_wasi02_public_sources
    wasm/src/wasi02_api.c
    wasm/src/wasi02_component.c
    wasm/src/wasi02_descriptor.c
    wasm/src/wasi02_provider.c
    wasm/src/wasi02_poll.c
    wasm/src/wasi02_streams.c
    wasm/src/wasi02_sockets.c
    wasm/src/wasi02_network.c
    wasm/src/wasi02_exec.c)

  if(TARGET TurboWasm::WASI)
    list(APPEND _turbowasm_wasi02_public_sources
      wasm/src/wasi02_filesystem.c)
  endif()

  add_library(turbowasm_wasi02
    ${_turbowasm_wasi02_public_sources})
  add_library(TurboWasm::WASI02 ALIAS turbowasm_wasi02)
  set_target_properties(turbowasm_wasi02 PROPERTIES
    EXPORT_NAME WASI02)
  target_compile_features(turbowasm_wasi02 PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wasm/include>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src)
  target_link_libraries(turbowasm_wasi02
    PUBLIC TurboWasm::Component)

  if(TARGET TurboWasm::WASI)
    target_compile_definitions(turbowasm_wasi02 PRIVATE
      TURBOWASM_WASI02_HAS_FILESYSTEM=1)
    target_link_libraries(turbowasm_wasi02
      PRIVATE TurboWasm::WASI)
  else()
    target_compile_definitions(turbowasm_wasi02 PRIVATE
      TURBOWASM_WASI02_HAS_FILESYSTEM=0)
  endif()

  if(MSVC)
    target_compile_options(turbowasm_wasi02 PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi02 PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TARGET turbowasm_wasi02)
  add_library(turbowasm_wasi02_io STATIC wasm/src/wasi02_io.c)
  add_library(TurboWasm::WASI02IO ALIAS turbowasm_wasi02_io)
  set_target_properties(turbowasm_wasi02_io PROPERTIES EXPORT_NAME WASI02IO)
  target_compile_features(turbowasm_wasi02_io PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02_io PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/wasm/src" "${CMAKE_CURRENT_SOURCE_DIR}/wasm/include")
  target_link_libraries(turbowasm_wasi02_io PUBLIC turbowasm_wasi02 PRIVATE Salts::Core)
  if(TARGET turbowasm_wasi02_cnet_core)
    target_link_libraries(turbowasm_wasi02_cnet_core PRIVATE turbowasm_wasi02_io)
  endif()
endif()

if(TURBOWASM_ENABLE_WASI02_SOCKET_BACKEND)
  if(NOT TARGET TurboWasm::WASI02IO OR NOT TARGET Salts::CNet)
    message(FATAL_ERROR "WASI02 socket backend requires WASI02IO and Salts::CNet")
  endif()
  add_library(turbowasm_wasi02_cnet STATIC wasm/src/wasi02_cnet.c)
  add_library(TurboWasm::WASI02CNet ALIAS turbowasm_wasi02_cnet)
  set_target_properties(turbowasm_wasi02_cnet PROPERTIES EXPORT_NAME WASI02CNet)
  target_compile_definitions(turbowasm_wasi02_cnet PRIVATE TURBOWASM_WASI02_NATIVE_TCP=1)
  target_compile_features(turbowasm_wasi02_cnet PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi02_cnet PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/wasm/src")
  target_link_libraries(turbowasm_wasi02_cnet PUBLIC TurboWasm::WASI02IO Salts::CNet PRIVATE Salts::Unicode)
endif()

if(TARGET turbowasm_wasi AND TARGET turbowasm_wasi02_cnet)
  add_library(turbowasm_wasi_cnet INTERFACE)
  add_library(TurboWasm::WASICNet ALIAS turbowasm_wasi_cnet)
  set_target_properties(turbowasm_wasi_cnet PROPERTIES EXPORT_NAME WASICNet)
  target_link_libraries(turbowasm_wasi_cnet INTERFACE TurboWasm::WASI TurboWasm::WASI02CNet)
  install(TARGETS turbowasm_wasi_cnet EXPORT TurboWasmTargets)
endif()

if(TURBOWASM_ENABLE_WASI_HOST_FS_ADAPTER)
  if(NOT TURBOWASM_ENABLE_WASI_ADAPTER)
    message(FATAL_ERROR
      "TurboWasm WASIHostFS requires the WASI adapter")
  endif()
  if(NOT TARGET Salts::Core)
    message(FATAL_ERROR
      "TurboWasm WASIHostFS requires a Salts SDK that exports Salts::Core")
  endif()

  add_library(turbowasm_wasi_host_fs
    wasm/src/wasi_host_fs.c)
  add_library(TurboWasm::WASIHostFS ALIAS turbowasm_wasi_host_fs)
  set_target_properties(turbowasm_wasi_host_fs PROPERTIES
    EXPORT_NAME WASIHostFS)
  target_compile_features(turbowasm_wasi_host_fs PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi_host_fs
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wasm/include>
      $<INSTALL_INTERFACE:include>)
  target_link_libraries(turbowasm_wasi_host_fs
    PUBLIC TurboWasm::WASI
    PRIVATE Salts::Core)

  if(MSVC)
    target_compile_options(turbowasm_wasi_host_fs PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi_host_fs PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()
if(TURBOWASM_ENABLE_WASI_LITTLEFS_ADAPTER)
  if(NOT TURBOWASM_ENABLE_WASI_ADAPTER)
    message(FATAL_ERROR
      "TurboWasm WASILittleFS requires the WASI adapter")
  endif()
  if(TURBOWASM_LITTLEFS_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR
      "TURBOWASM_LITTLEFS_SOURCE_DIR must point at an externally acquired littlefs checkout")
  endif()
  foreach(_turbowasm_littlefs_file lfs.c lfs_util.c lfs.h lfs_util.h)
    if(NOT EXISTS
       "${TURBOWASM_LITTLEFS_SOURCE_DIR}/${_turbowasm_littlefs_file}")
      message(FATAL_ERROR
        "Missing littlefs source file: ${TURBOWASM_LITTLEFS_SOURCE_DIR}/${_turbowasm_littlefs_file}")
    endif()
  endforeach()

  add_library(turbowasm_wasi_littlefs
    wasm/src/wasi_littlefs.c
    "${TURBOWASM_LITTLEFS_SOURCE_DIR}/lfs.c"
    "${TURBOWASM_LITTLEFS_SOURCE_DIR}/lfs_util.c")
  add_library(TurboWasm::WASILittleFS ALIAS turbowasm_wasi_littlefs)
  set_target_properties(turbowasm_wasi_littlefs PROPERTIES
    EXPORT_NAME WASILittleFS)
  target_compile_features(turbowasm_wasi_littlefs PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi_littlefs
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wasm/include>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      "${TURBOWASM_LITTLEFS_SOURCE_DIR}")
  target_link_libraries(turbowasm_wasi_littlefs
    PUBLIC TurboWasm::WASI)
endif()

if(TURBOWASM_ENABLE_WASI_NATIVE_IO_ADAPTER)
  if(NOT TURBOWASM_ENABLE_WASI_ADAPTER OR
     NOT TURBOWASM_ENABLE_NATIVE_IO_ADAPTER)
    message(FATAL_ERROR
      "TurboWasm WASINativeIO requires both WASI and NativeIO adapters")
  endif()

  add_library(turbowasm_wasi_native_io
    wasm/src/wasi_native_io.c)
  add_library(TurboWasm::WASINativeIO ALIAS turbowasm_wasi_native_io)
  set_target_properties(turbowasm_wasi_native_io PROPERTIES
    EXPORT_NAME WASINativeIO)
  target_compile_features(turbowasm_wasi_native_io PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi_native_io
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wasm/include>
      $<INSTALL_INTERFACE:include>)
  target_link_libraries(turbowasm_wasi_native_io
    PUBLIC
      TurboWasm::WASI
      TurboWasm::NativeIO)

  if(MSVC)
    target_compile_options(turbowasm_wasi_native_io PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi_native_io PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_WASI_THREADS_ADAPTER)
  if(NOT TARGET CMeta::CFlow)
    message(FATAL_ERROR
      "TurboWasm WASIThreads adapter requires a Salts SDK that exports CMeta::CFlow")
  endif()

  add_library(turbowasm_wasi_threads
    wasm/src/wasi_threads.c)
  add_library(TurboWasm::WASIThreads ALIAS turbowasm_wasi_threads)
  set_target_properties(turbowasm_wasi_threads PROPERTIES
    EXPORT_NAME WASIThreads)
  target_compile_features(turbowasm_wasi_threads PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi_threads
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wasm/include>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src)
  target_link_libraries(turbowasm_wasi_threads
    PUBLIC
      TurboWasm::Runtime
      CMeta::CFlow
    PRIVATE
      Salts::Core)

  if(MSVC)
    target_compile_options(turbowasm_wasi_threads PRIVATE
      /W4 -experimental:c11atomics)
  else()
    target_compile_options(turbowasm_wasi_threads PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_MIR_JIT)
  find_package(MIR CONFIG REQUIRED PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

  # Keep MIR out of the installed TurboWasm::Runtime link interface.
  # The backend is an internal, non-installed component until the tiering
  # integration defines its explicit runtime/plugin ownership.
  add_library(turbowasm_mir_backend STATIC
    wasm/src/jit/mir_backend.c)
  target_compile_features(turbowasm_mir_backend PRIVATE c_std_11)
  target_include_directories(turbowasm_mir_backend
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/include
      ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src)
  target_link_libraries(turbowasm_mir_backend
    PRIVATE
      TurboWasm::Runtime Salts::SIMD
      MIR::MIR)
  target_compile_definitions(turbowasm_mir_backend PRIVATE
    TURBOWASM_ENABLE_MIR_JIT=1)

  if(MSVC)
    target_compile_options(turbowasm_mir_backend PRIVATE /W4)
  else()
    target_compile_options(turbowasm_mir_backend PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(MSVC)
  target_compile_options(turbowasm PRIVATE /W4)
else()
  target_compile_options(turbowasm PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TURBOWASM_BUILD_TOOLS)
  add_executable(turbowasm_cli tools/turbowasm.c)
  set_target_properties(turbowasm_cli PROPERTIES OUTPUT_NAME turbowasm)
  target_link_libraries(turbowasm_cli PRIVATE TurboWasm::Runtime)
  if(TARGET TurboWasm::WASIHostFS)
    add_library(turbowasm_command STATIC tools/command.c)
    target_compile_features(turbowasm_command PRIVATE c_std_11)
    target_link_libraries(turbowasm_command PUBLIC TurboWasm::WASIHostFS)
    target_include_directories(turbowasm_command PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tools")
    add_executable(turbowasm_run tools/turbowasm_run.c)
    target_link_libraries(turbowasm_run PRIVATE turbowasm_command Salts::Core)
    set_target_properties(turbowasm_run PROPERTIES OUTPUT_NAME turbowasm-run)
    set_target_properties(turbowasm_run turbowasm_command PROPERTIES FOLDER "tools")
    install(TARGETS turbowasm_run RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()

if(TURBOWASM_BUILD_METALLIC_GUESTS)
  add_subdirectory(guest)
endif()

if(TURBOWASM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(wasm/tests)
endif()

if(TURBOWASM_BUILD_CONFORMANCE_RUNNER)
  add_subdirectory(wasm/tests/conformance)
endif()

install(
  TARGETS turbowasm
  EXPORT TurboWasmTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TURBOWASM_ENABLE_COMPONENT)
  install(
    TARGETS turbowasm_component
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TARGET turbowasm_wasi02)
  install(
    TARGETS turbowasm_wasi02
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBOWASM_ENABLE_CFLOW_ADAPTER)
  install(
    TARGETS turbowasm_cflow
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TARGET turbowasm_wasi02_io)
  install(TARGETS turbowasm_wasi02_io EXPORT TurboWasmTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(TARGET turbowasm_wasi02_cnet)
  install(TARGETS turbowasm_wasi02_cnet EXPORT TurboWasmTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(TURBOWASM_ENABLE_NATIVE_IO_ADAPTER)
  install(
    TARGETS turbowasm_native_io
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  install(
    TARGETS turbowasm_wasi
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBOWASM_ENABLE_WASI_HOST_FS_ADAPTER)
  install(
    TARGETS turbowasm_wasi_host_fs
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TURBOWASM_ENABLE_WASI_LITTLEFS_ADAPTER)
  install(
    TARGETS turbowasm_wasi_littlefs
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBOWASM_ENABLE_WASI_NATIVE_IO_ADAPTER)
  install(
    TARGETS turbowasm_wasi_native_io
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBOWASM_ENABLE_WASI_THREADS_ADAPTER)
  install(
    TARGETS turbowasm_wasi_threads
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBOWASM_BUILD_TOOLS)
  install(TARGETS turbowasm_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(
  DIRECTORY wasm/include/
  DESTINATION include
  FILES_MATCHING PATTERN "*.h")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboWasmConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/TurboWasmConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboWasm")

install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/TurboWasmConfig.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboWasm")

install(
  EXPORT TurboWasmTargets
  FILE TurboWasmTargets.cmake
  NAMESPACE TurboWasm::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboWasm")

install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdk_include/salts/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/salts")
