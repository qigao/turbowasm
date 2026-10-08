cmake_minimum_required(VERSION 3.20)
project(TurboWasmExamples LANGUAGES C)
find_package(TurboWasm CONFIG REQUIRED)
add_executable(turbowasm_gc_example gc.c)
target_link_libraries(turbowasm_gc_example PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_gc_example PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
