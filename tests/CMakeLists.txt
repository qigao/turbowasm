add_executable(turbowasm_module_test module_test.c)
target_link_libraries(turbowasm_module_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_module_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_module_test COMMAND turbowasm_module_test)

add_executable(turbowasm_simd_test simd_test.c)
target_link_libraries(turbowasm_simd_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_simd_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_simd_test COMMAND turbowasm_simd_test)

add_executable(turbowasm_header_cpp_test header_cpp_test.cpp)
target_link_libraries(turbowasm_header_cpp_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_header_cpp_test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
add_test(NAME turbowasm_header_cpp_test COMMAND turbowasm_header_cpp_test)

add_executable(turbowasm_validation_test validation_test.c)
target_link_libraries(turbowasm_validation_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_validation_test COMMAND turbowasm_validation_test)

add_executable(turbowasm_linkage_validation_test linkage_validation_test.c)
target_link_libraries(turbowasm_linkage_validation_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_linkage_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_linkage_validation_test
         COMMAND turbowasm_linkage_validation_test)

add_executable(turbowasm_data_validation_test data_validation_test.c)
target_link_libraries(turbowasm_data_validation_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_data_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_data_validation_test
         COMMAND turbowasm_data_validation_test)

add_executable(turbowasm_element_validation_test element_validation_test.c)
target_link_libraries(turbowasm_element_validation_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_element_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_element_validation_test
         COMMAND turbowasm_element_validation_test)

add_executable(turbowasm_instruction_validation_test instruction_validation_test.c)
target_link_libraries(turbowasm_instruction_validation_test PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_instruction_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_instruction_validation_test
         COMMAND turbowasm_instruction_validation_test)

add_executable(turbowasm_structured_control_validation_test
  structured_control_validation_test.c)
target_link_libraries(turbowasm_structured_control_validation_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_structured_control_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_structured_control_validation_test
         COMMAND turbowasm_structured_control_validation_test)

add_executable(turbowasm_memory_numeric_validation_test
  memory_numeric_validation_test.c)
target_link_libraries(turbowasm_memory_numeric_validation_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_memory_numeric_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_memory_numeric_validation_test
         COMMAND turbowasm_memory_numeric_validation_test)

add_executable(turbowasm_bulk_validation_test
  bulk_validation_test.c)
target_link_libraries(turbowasm_bulk_validation_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_bulk_validation_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_bulk_validation_test
         COMMAND turbowasm_bulk_validation_test)

add_executable(turbowasm_code_metadata_test
  code_metadata_test.c)
target_link_libraries(turbowasm_code_metadata_test
  PRIVATE TurboWasm::Runtime)
target_include_directories(turbowasm_code_metadata_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(turbowasm_code_metadata_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_code_metadata_test
         COMMAND turbowasm_code_metadata_test)

add_executable(turbowasm_instance_test
  instance_test.c)
target_link_libraries(turbowasm_instance_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_instance_test
         COMMAND turbowasm_instance_test)

add_executable(turbowasm_structured_instance_test
  structured_instance_test.c)
target_link_libraries(turbowasm_structured_instance_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_structured_instance_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_structured_instance_test
         COMMAND turbowasm_structured_instance_test)

add_executable(turbowasm_retained_instance_metadata_test
  retained_instance_metadata_test.c)
target_link_libraries(turbowasm_retained_instance_metadata_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_retained_instance_metadata_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_retained_instance_metadata_test
         COMMAND turbowasm_retained_instance_metadata_test)

add_executable(turbowasm_retained_segment_metadata_test
  retained_segment_metadata_test.c)
target_link_libraries(turbowasm_retained_segment_metadata_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_retained_segment_metadata_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_retained_segment_metadata_test
         COMMAND turbowasm_retained_segment_metadata_test)

add_executable(turbowasm_instance_state_test
  instance_state_test.c)
target_link_libraries(turbowasm_instance_state_test
  PRIVATE TurboWasm::Runtime)
set_target_properties(turbowasm_instance_state_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME turbowasm_instance_state_test
         COMMAND turbowasm_instance_state_test)
