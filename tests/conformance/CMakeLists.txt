add_executable(turbowasm_spec_runner spec_runner.c)
target_link_libraries(turbowasm_spec_runner PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_spec_runner PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(turbowasm_spec_runner PRIVATE /W4)
else()
  target_compile_options(turbowasm_spec_runner PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
