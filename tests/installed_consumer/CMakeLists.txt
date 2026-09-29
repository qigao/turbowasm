cmake_minimum_required(VERSION 3.20)
project(TurboWasmInstalledConsumer LANGUAGES C CXX)

include(CTest)

find_package(TurboWasm CONFIG REQUIRED)

add_executable(turbowasm_installed_consumer main.c)
target_link_libraries(turbowasm_installed_consumer PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_installed_consumer PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

add_test(NAME turbowasm_installed_consumer
         COMMAND turbowasm_installed_consumer)

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
