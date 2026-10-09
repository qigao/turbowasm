cmake_minimum_required(VERSION 3.20)
project(TurboWasmInstalledConsumer LANGUAGES C CXX)

include(CTest)
include(../../cmake/SaltsCompilerOptions.cmake)

foreach(_dependency IN ITEMS Salts TurboWasm)
  if(_dependency STREQUAL "Salts")
    set(_root "$ENV{SALTS_ROOT}")
  else()
    set(_root "$ENV{TURBOWASM_ROOT}")
  endif()
  if(NOT IS_DIRECTORY "${_root}")
    message(FATAL_ERROR "${_dependency} SDK root is not a directory: ${_root}")
  endif()
  unset(${_dependency}_DIR CACHE)
  unset(${_dependency}_DIR)
  file(TO_CMAKE_PATH "${_root}" _root)
  find_package(${_dependency} CONFIG REQUIRED
    PATHS "${_root}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endforeach()

turbowasm_normalize_salts_c11_atomics()

if(TurboWasm_HAS_METALLIC_GUEST_SDK)
  include("${TurboWasm_GUEST_SDK_DIR}/TurboWasmGuest.cmake")
  turbowasm_add_c_guest(turbowasm_installed_c_guest "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/guest/hello.c")
  find_program(TURBOWASM_INSTALLED_RUNNER NAMES turbowasm-run
    PATHS "$ENV{TURBOWASM_ROOT}/bin" NO_DEFAULT_PATH REQUIRED)
  add_test(NAME turbowasm_installed_c_guest
    COMMAND "${TURBOWASM_INSTALLED_RUNNER}" "${turbowasm_installed_c_guest_WASM}")
  set_tests_properties(turbowasm_installed_c_guest PROPERTIES
    PASS_REGULAR_EXPRESSION "guest cleanup complete" TIMEOUT 30)
endif()

add_executable(turbowasm_installed_consumer main.c)
target_link_libraries(turbowasm_installed_consumer PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_installed_consumer PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

add_test(NAME turbowasm_installed_consumer
         COMMAND turbowasm_installed_consumer)

add_executable(turbowasm_installed_gc_consumer ../../examples/gc.c)
target_link_libraries(turbowasm_installed_gc_consumer PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_installed_gc_consumer PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME turbowasm_installed_gc_consumer COMMAND turbowasm_installed_gc_consumer)

add_executable(turbowasm_installed_artifact_cpp_consumer
  artifact_cpp_main.cpp)
target_link_libraries(turbowasm_installed_artifact_cpp_consumer
  PRIVATE TurboWasm::Runtime)
set_target_properties(
  turbowasm_installed_artifact_cpp_consumer PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
add_test(NAME turbowasm_installed_artifact_cpp_consumer
         COMMAND turbowasm_installed_artifact_cpp_consumer)


add_executable(turbowasm_installed_linker_consumer link_main.c)
target_link_libraries(turbowasm_installed_linker_consumer
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_installed_linker_consumer PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_installed_linker_consumer
         COMMAND turbowasm_installed_linker_consumer)


get_target_property(_turbowasm_runtime_links
  TurboWasm::Runtime INTERFACE_LINK_LIBRARIES)
if(NOT "Salts::CMeta" IN_LIST _turbowasm_runtime_links)
  message(FATAL_ERROR
    "TurboWasm::Runtime must expose its canonical Salts::CMeta dependency")
endif()
if("Salts::CFlow" IN_LIST _turbowasm_runtime_links)
  message(FATAL_ERROR
    "TurboWasm::Runtime must remain CFlow-free")
endif()
foreach(_turbowasm_forbidden_tooling_target IN ITEMS
    Salts::DataBindProducer
    Salts::DataBind
    Salts::DataBindCore)
  if("${_turbowasm_forbidden_tooling_target}" IN_LIST _turbowasm_runtime_links)
    message(FATAL_ERROR
      "TurboWasm::Runtime must remain free of ${_turbowasm_forbidden_tooling_target}")
  endif()
endforeach()

if("Salts::NativeIO" IN_LIST _turbowasm_runtime_links)
  message(FATAL_ERROR
    "TurboWasm::Runtime must remain NativeIO-free")
endif()

if("TurboWasm::Component" IN_LIST _turbowasm_runtime_links)
  message(FATAL_ERROR
    "TurboWasm::Runtime must remain Component-facade-free")
endif()

if(TARGET TurboWasm::Component)
  get_target_property(_turbowasm_component_links
    TurboWasm::Component INTERFACE_LINK_LIBRARIES)
  if(NOT "TurboWasm::Runtime" IN_LIST _turbowasm_component_links)
    message(FATAL_ERROR
      "TurboWasm::Component must depend on TurboWasm::Runtime")
  endif()
  if("Salts::CFlow" IN_LIST _turbowasm_component_links OR
     "Salts::NativeIO" IN_LIST _turbowasm_component_links OR
     "TurboWasm::WASI" IN_LIST _turbowasm_component_links)
    message(FATAL_ERROR
      "TurboWasm::Component must remain independent of CFlow/NativeIO/WASI")
  endif()

  add_executable(turbowasm_installed_component_consumer
    component_main.c)
  target_link_libraries(turbowasm_installed_component_consumer
    PRIVATE TurboWasm::Component)
  set_target_properties(
    turbowasm_installed_component_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_component_consumer
           COMMAND turbowasm_installed_component_consumer)

  add_executable(turbowasm_installed_component_cpp_consumer
    component_cpp_main.cpp)
  target_link_libraries(turbowasm_installed_component_cpp_consumer
    PRIVATE TurboWasm::Component)
  set_target_properties(
    turbowasm_installed_component_cpp_consumer PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_component_cpp_consumer
           COMMAND turbowasm_installed_component_cpp_consumer)
endif()

if(TARGET TurboWasm::WASI02)
  get_target_property(_turbowasm_wasi02_links
    TurboWasm::WASI02 INTERFACE_LINK_LIBRARIES)
  if(NOT "TurboWasm::Component" IN_LIST _turbowasm_wasi02_links)
    message(FATAL_ERROR
      "TurboWasm::WASI02 must expose TurboWasm::Component")
  endif()
  if("Salts::CFlow" IN_LIST _turbowasm_wasi02_links OR
     "Salts::NativeIO" IN_LIST _turbowasm_wasi02_links OR
     "TurboWasm::NativeIO" IN_LIST _turbowasm_wasi02_links)
    message(FATAL_ERROR
      "TurboWasm::WASI02 public link interface must remain CFlow/NativeIO-free")
  endif()

  add_executable(turbowasm_installed_wasi02_consumer
    wasi02_main.c)
  target_link_libraries(turbowasm_installed_wasi02_consumer
    PRIVATE TurboWasm::WASI02)
  set_target_properties(
    turbowasm_installed_wasi02_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi02_consumer
           COMMAND turbowasm_installed_wasi02_consumer)

  add_executable(turbowasm_installed_wasi02_corpus_consumer
    wasi02_corpus_main.c)
  target_link_libraries(turbowasm_installed_wasi02_corpus_consumer
    PRIVATE TurboWasm::WASI02)
  target_include_directories(turbowasm_installed_wasi02_corpus_consumer
    PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/../wasi02/fixtures/generated")
  set_target_properties(
    turbowasm_installed_wasi02_corpus_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi02_corpus_consumer
           COMMAND turbowasm_installed_wasi02_corpus_consumer)

  add_executable(turbowasm_installed_wasi02_cpp_consumer
    wasi02_cpp_main.cpp)
  target_link_libraries(turbowasm_installed_wasi02_cpp_consumer
    PRIVATE TurboWasm::WASI02)
  set_target_properties(
    turbowasm_installed_wasi02_cpp_consumer PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi02_cpp_consumer
           COMMAND turbowasm_installed_wasi02_cpp_consumer)
endif()

if(TARGET TurboWasm::WASI02IO)
  add_executable(turbowasm_installed_wasi02_io_cpp_consumer wasi02_io_cpp_main.cpp)
  target_link_libraries(turbowasm_installed_wasi02_io_cpp_consumer PRIVATE TurboWasm::WASI02IO)
  set_target_properties(turbowasm_installed_wasi02_io_cpp_consumer PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi02_io_cpp_consumer COMMAND turbowasm_installed_wasi02_io_cpp_consumer)
endif()

if(TARGET TurboWasm::WASI02CNet)
  add_executable(turbowasm_installed_wasi02_cnet_cpp_consumer wasi02_cnet_cpp_main.cpp)
  target_link_libraries(turbowasm_installed_wasi02_cnet_cpp_consumer PRIVATE TurboWasm::WASI02CNet)
  set_target_properties(turbowasm_installed_wasi02_cnet_cpp_consumer PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi02_cnet_cpp_consumer COMMAND turbowasm_installed_wasi02_cnet_cpp_consumer)
  add_executable(turbowasm_installed_wasi02_network_consumer ../wasi02_cnet_network_test.c)
  target_compile_definitions(turbowasm_installed_wasi02_network_consumer PRIVATE TW_NETWORK_PUBLIC_ONLY=1)
  target_link_libraries(turbowasm_installed_wasi02_network_consumer PRIVATE TurboWasm::WASI02CNet Salts::TinyTest)
  set_target_properties(turbowasm_installed_wasi02_network_consumer PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi02_network_consumer COMMAND turbowasm_installed_wasi02_network_consumer)
  add_executable(turbowasm_installed_wasi02_tcp_consumer ../wasi02_cnet_tcp_test.c)
  target_link_libraries(turbowasm_installed_wasi02_tcp_consumer PRIVATE TurboWasm::WASI02CNet Salts::TinyTest)
  set_target_properties(turbowasm_installed_wasi02_tcp_consumer PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi02_tcp_consumer COMMAND turbowasm_installed_wasi02_tcp_consumer)
endif()

if(TARGET TurboWasm::CFlow)
  add_executable(turbowasm_installed_cflow_consumer cflow_main.c)
  target_link_libraries(turbowasm_installed_cflow_consumer
    PRIVATE TurboWasm::CFlow)
  set_target_properties(turbowasm_installed_cflow_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_cflow_consumer
           COMMAND turbowasm_installed_cflow_consumer)
endif()

if(TARGET TurboWasm::NativeIO)
  add_executable(turbowasm_installed_native_io_consumer native_io_main.c)
  target_link_libraries(turbowasm_installed_native_io_consumer
    PRIVATE TurboWasm::NativeIO)
  set_target_properties(turbowasm_installed_native_io_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_native_io_consumer
           COMMAND turbowasm_installed_native_io_consumer)
endif()

if(TARGET TurboWasm::WASI)
  get_target_property(_turbowasm_wasi_links
    TurboWasm::WASI INTERFACE_LINK_LIBRARIES)
  if("Salts::NativeIO" IN_LIST _turbowasm_wasi_links OR
     "TurboWasm::NativeIO" IN_LIST _turbowasm_wasi_links)
    message(FATAL_ERROR
      "TurboWasm::WASI must remain NativeIO-free")
  endif()
  if("TurboWasm::WASILittleFS" IN_LIST _turbowasm_wasi_links)
    message(FATAL_ERROR
      "TurboWasm::WASI must remain littlefs-adapter-free")
  endif()
  if("TurboWasm::WASIHostFS" IN_LIST _turbowasm_wasi_links)
    message(FATAL_ERROR
      "TurboWasm::WASI must remain host-filesystem-adapter-free")
  endif()
  foreach(_turbowasm_forbidden_tooling_target IN ITEMS
      Salts::DataBindProducer
      Salts::DataBind
      Salts::DataBindCore)
    if("${_turbowasm_forbidden_tooling_target}" IN_LIST _turbowasm_wasi_links)
      message(FATAL_ERROR
        "TurboWasm::WASI must remain free of ${_turbowasm_forbidden_tooling_target}")
    endif()
  endforeach()
endif()

if(TARGET TurboWasm::WASINativeIO)
  add_executable(turbowasm_installed_wasi_native_io_consumer
    wasi_native_io_main.c)
  target_link_libraries(turbowasm_installed_wasi_native_io_consumer
    PRIVATE TurboWasm::WASINativeIO)
  set_target_properties(
    turbowasm_installed_wasi_native_io_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi_native_io_consumer
           COMMAND turbowasm_installed_wasi_native_io_consumer)
endif()

if(TARGET TurboWasm::WASI)
  add_executable(turbowasm_installed_wasi_consumer wasi_main.c)
  target_link_libraries(turbowasm_installed_wasi_consumer
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_installed_wasi_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi_consumer
           COMMAND turbowasm_installed_wasi_consumer)
endif()

if(TARGET TurboWasm::WASI)
  add_executable(turbowasm_installed_wasi_preview1_sockets_consumer ../wasi_preview1_sockets_test.c)
  target_link_libraries(turbowasm_installed_wasi_preview1_sockets_consumer PRIVATE TurboWasm::WASI Salts::TinyTest)
  set_target_properties(turbowasm_installed_wasi_preview1_sockets_consumer PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi_preview1_sockets_consumer COMMAND turbowasm_installed_wasi_preview1_sockets_consumer)
  if(TARGET TurboWasm::WASICNet)
    add_executable(turbowasm_installed_wasi_preview1_cnet_consumer ../wasi_preview1_cnet_test.c)
    target_link_libraries(turbowasm_installed_wasi_preview1_cnet_consumer PRIVATE TurboWasm::WASICNet Salts::TinyTest)
    set_target_properties(turbowasm_installed_wasi_preview1_cnet_consumer PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME turbowasm_installed_wasi_preview1_cnet_consumer COMMAND turbowasm_installed_wasi_preview1_cnet_consumer)
    set_tests_properties(turbowasm_installed_wasi_preview1_cnet_consumer PROPERTIES TIMEOUT 30)
    add_executable(turbowasm_installed_wasi_preview1_cnet_cpp_consumer wasi_preview1_cnet_cpp_main.cpp)
    target_link_libraries(turbowasm_installed_wasi_preview1_cnet_cpp_consumer PRIVATE TurboWasm::WASICNet)
    set_target_properties(turbowasm_installed_wasi_preview1_cnet_cpp_consumer PROPERTIES
      CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME turbowasm_installed_wasi_preview1_cnet_cpp_consumer COMMAND turbowasm_installed_wasi_preview1_cnet_cpp_consumer)
  endif()
endif()

if(TARGET TurboWasm::WASIThreads)
  add_executable(turbowasm_installed_wasi_threads_consumer
    wasi_threads_main.c)
  target_link_libraries(turbowasm_installed_wasi_threads_consumer
    PRIVATE TurboWasm::WASIThreads)
  set_target_properties(turbowasm_installed_wasi_threads_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi_threads_consumer
           COMMAND turbowasm_installed_wasi_threads_consumer)
endif()

if(TARGET TurboWasm::WASI)
  add_executable(turbowasm_installed_wasi_fs_consumer wasi_fs_main.c)
  target_link_libraries(turbowasm_installed_wasi_fs_consumer
    PRIVATE TurboWasm::WASI)
  set_target_properties(turbowasm_installed_wasi_fs_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi_fs_consumer
           COMMAND turbowasm_installed_wasi_fs_consumer)
endif()

if(TARGET TurboWasm::WASIHostFS)
  get_target_property(_turbowasm_wasi_host_fs_links
    TurboWasm::WASIHostFS INTERFACE_LINK_LIBRARIES)
  if(NOT "TurboWasm::WASI" IN_LIST _turbowasm_wasi_host_fs_links)
    message(FATAL_ERROR
      "TurboWasm::WASIHostFS must depend on TurboWasm::WASI")
  endif()

  add_executable(turbowasm_installed_wasi_host_fs_consumer
    wasi_host_fs_main.c)
  target_link_libraries(turbowasm_installed_wasi_host_fs_consumer
    PRIVATE TurboWasm::WASIHostFS)
  set_target_properties(
    turbowasm_installed_wasi_host_fs_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi_host_fs_consumer
           COMMAND turbowasm_installed_wasi_host_fs_consumer)
endif()

if(TARGET TurboWasm::WASILittleFS)
  get_target_property(_turbowasm_wasi_littlefs_links
    TurboWasm::WASILittleFS INTERFACE_LINK_LIBRARIES)
  if(NOT "TurboWasm::WASI" IN_LIST _turbowasm_wasi_littlefs_links)
    message(FATAL_ERROR
      "TurboWasm::WASILittleFS must depend on TurboWasm::WASI")
  endif()

  add_executable(turbowasm_installed_wasi_littlefs_consumer
    wasi_littlefs_main.c)
  target_link_libraries(turbowasm_installed_wasi_littlefs_consumer
    PRIVATE TurboWasm::WASILittleFS)
  set_target_properties(
    turbowasm_installed_wasi_littlefs_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME turbowasm_installed_wasi_littlefs_consumer
           COMMAND turbowasm_installed_wasi_littlefs_consumer)
endif()
