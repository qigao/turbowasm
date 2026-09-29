cmake_minimum_required(VERSION 3.20)
project(TurboWasmInstalledConsumer LANGUAGES C)

include(CTest)

find_package(TurboWasm 0.1 CONFIG REQUIRED)

add_executable(turbowasm_installed_consumer main.c)
target_link_libraries(turbowasm_installed_consumer PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_installed_consumer PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

add_test(NAME turbowasm_installed_consumer
         COMMAND turbowasm_installed_consumer)


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
