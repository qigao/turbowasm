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
