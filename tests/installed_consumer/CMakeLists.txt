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


get_target_property(_turbowasm_runtime_links
  TurboWasm::Runtime INTERFACE_LINK_LIBRARIES)
if("Salts::CFlow" IN_LIST _turbowasm_runtime_links)
  message(FATAL_ERROR
    "TurboWasm::Runtime must remain CFlow-free")
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
