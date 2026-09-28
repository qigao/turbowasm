add_executable(turbowasm_spec_runner spec_runner.c)
target_link_libraries(turbowasm_spec_runner PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_spec_runner
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
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


if(TURBOWASM_ENABLE_MIR_JIT)
  add_executable(turbowasm_spec_runner_mir spec_runner.c)
  target_compile_definitions(turbowasm_spec_runner_mir
    PRIVATE TURBOWASM_SPEC_ENABLE_MIR=1)
  target_include_directories(turbowasm_spec_runner_mir
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(turbowasm_spec_runner_mir
    PRIVATE
      turbowasm_mir_backend
      TurboWasm::Runtime)
  set_target_properties(turbowasm_spec_runner_mir PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

  if(MSVC)
    target_compile_options(turbowasm_spec_runner_mir PRIVATE /W4)
  else()
    target_compile_options(turbowasm_spec_runner_mir PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()
