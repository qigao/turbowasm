add_executable(turbowasm_spec_runner spec_runner.c)
target_link_libraries(turbowasm_spec_runner PRIVATE TurboWasm::Runtime Salts::Platform)
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
      TurboWasm::Runtime
      Salts::Platform)
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

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME turbowasm_conformance_driver
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_run_core.py")

if(TURBOWASM_CORE3_TOOLS)
  if(NOT EXISTS "${TURBOWASM_CORE3_TOOLS}" OR
      NOT IS_DIRECTORY "${TURBOWASM_SPEC_ROOT}/test/core")
    message(FATAL_ERROR "Core 3.0 conformance requires wasm-tools and the pinned spec checkout")
  endif()
  add_test(NAME turbowasm_conformance_core3
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_core.py"
      --wasm-tools "${TURBOWASM_CORE3_TOOLS}"
      --runner $<TARGET_FILE:turbowasm_spec_runner>
      --core-dir "${TURBOWASM_SPEC_ROOT}/test/core"
      --all-core --maximum-unsupported 0)
  set_tests_properties(turbowasm_conformance_core3 PROPERTIES
    LABELS conformance TIMEOUT 600)
  add_test(NAME turbowasm_conformance_core3_references
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_core.py"
      --wasm-tools "${TURBOWASM_CORE3_TOOLS}"
      --runner $<TARGET_FILE:turbowasm_spec_runner>
      --core-dir "${TURBOWASM_SPEC_ROOT}/test/core"
      --suite "${CMAKE_CURRENT_SOURCE_DIR}/core3-reference-regression.txt"
      --maximum-unsupported 0)
  set_tests_properties(turbowasm_conformance_core3_references PROPERTIES
    LABELS conformance TIMEOUT 180)
  if(TURBOWASM_ENABLE_MIR_JIT)
    add_test(NAME turbowasm_conformance_core3-mir
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_core.py"
        --wasm-tools "${TURBOWASM_CORE3_TOOLS}"
        --runner $<TARGET_FILE:turbowasm_spec_runner>
        --compare-runner $<TARGET_FILE:turbowasm_spec_runner_mir>
        --core-dir "${TURBOWASM_SPEC_ROOT}/test/core/memory64"
        --all-core --maximum-unsupported 0)
    set_tests_properties(turbowasm_conformance_core3-mir PROPERTIES
      LABELS conformance TIMEOUT 600
      FAIL_REGULAR_EXPRESSION "DIFFERENTIAL_FAIL;MIR_REPLAY compiled=0 ")
  endif()
  return()
endif()

find_program(_turbowasm_wast2json NAMES wast2json
  PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/wabt"
  NO_DEFAULT_PATH REQUIRED)

add_test(NAME turbowasm_conformance_harness
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_core.py"
    --wast2json "${_turbowasm_wast2json}"
    --runner $<TARGET_FILE:turbowasm_spec_runner>
    --core-dir "${CMAKE_CURRENT_SOURCE_DIR}"
    --suite "${CMAKE_CURRENT_SOURCE_DIR}/result-set-harness-smoke.txt")

if(NOT IS_DIRECTORY "${TURBOWASM_SPEC_ROOT}/test/core")
  message(FATAL_ERROR "TURBOWASM_SPEC_ROOT must contain the pinned WebAssembly test/core suite")
endif()
add_test(NAME turbowasm_conformance_core
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_core.py"
    --wast2json "${_turbowasm_wast2json}"
    --runner $<TARGET_FILE:turbowasm_spec_runner>
    --core-dir "${TURBOWASM_SPEC_ROOT}/test/core"
    --suite "${CMAKE_CURRENT_SOURCE_DIR}/core-smoke.txt")

if(TURBOWASM_ENABLE_MIR_JIT)
  add_test(NAME turbowasm_conformance_mir
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_core.py"
      --wast2json "${_turbowasm_wast2json}"
      --runner $<TARGET_FILE:turbowasm_spec_runner>
      --compare-runner $<TARGET_FILE:turbowasm_spec_runner_mir>
      --core-dir "${TURBOWASM_SPEC_ROOT}/test/core"
      --suite "${CMAKE_CURRENT_SOURCE_DIR}/core-smoke.txt")
  set_tests_properties(turbowasm_conformance_mir PROPERTIES
    FAIL_REGULAR_EXPRESSION "SPEC_RUNTIME_UNSUPPORTED_DETAIL;DIFFERENTIAL_FAIL;MIR_REPLAY compiled=0 ")
endif()

foreach(_proposal IN ITEMS exception-handling threads tail-call custom-page-sizes
    memory64 extended-const multi-memory relaxed-simd)
  add_test(NAME turbowasm_conformance_${_proposal}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_proposal.py"
      --selector "${_proposal}"
      --wast2json "${_turbowasm_wast2json}"
      --runner $<TARGET_FILE:turbowasm_spec_runner>
      --work-dir "${CMAKE_CURRENT_BINARY_DIR}/proposals/${_proposal}")
  set_tests_properties(turbowasm_conformance_${_proposal} PROPERTIES
    LABELS conformance
    FAIL_REGULAR_EXPRESSION "SPEC_RUNTIME_UNSUPPORTED_DETAIL")
endforeach()
