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
