cmake_minimum_required(VERSION 3.20)
project(TurboWasmInstalledConsumer LANGUAGES C)

find_package(TurboWasm 0.1 CONFIG REQUIRED)

add_executable(turbowasm_installed_consumer main.c)
target_link_libraries(turbowasm_installed_consumer PRIVATE TurboWasm::Runtime)
target_compile_features(turbowasm_installed_consumer PRIVATE c_std_11)
