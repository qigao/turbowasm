cmake_minimum_required(VERSION 3.20)

project(
  TurboWasm
  VERSION 0.1.0
  LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(TURBOWASM_BUILD_TOOLS "Build TurboWasm command-line tools" ON)
option(TURBOWASM_BUILD_TESTS "Build TurboWasm tests" ON)

if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_ROOT must point at an installed Salts SDK")
endif()

file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" SALTS_ROOT_PATH)
find_package(Salts 1.3.0 CONFIG REQUIRED
  PATHS "${SALTS_ROOT_PATH}"
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH)

if(NOT TARGET Salts::SIMD)
  message(FATAL_ERROR
    "TurboWasm requires a Salts SDK that exports Salts::SIMD (qigao/salts#343)")
endif()

add_library(turbowasm
  src/instance.c
  src/instance_state.c
  src/module.c
  src/reader.c
  src/simd.c
  src/status.c
  src/value.c
  src/validation_context.c
  src/validate.c
  src/validate_data.c
  src/validate_element.c
  src/validate_instr.c
  src/validate_simd_table.c
  src/validate_linkage.c)

add_library(TurboWasm::Runtime ALIAS turbowasm)

set_target_properties(turbowasm PROPERTIES
  EXPORT_NAME Runtime
  VERSION ${PROJECT_VERSION}
  SOVERSION 0)

target_compile_features(turbowasm PUBLIC c_std_11)
target_include_directories(turbowasm
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(turbowasm PUBLIC Salts::SIMD)

if(MSVC)
  target_compile_options(turbowasm PRIVATE /W4)
else()
  target_compile_options(turbowasm PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TURBOWASM_BUILD_TOOLS)
  add_executable(turbowasm_cli tools/turbowasm.c)
  set_target_properties(turbowasm_cli PROPERTIES OUTPUT_NAME turbowasm)
  target_link_libraries(turbowasm_cli PRIVATE TurboWasm::Runtime)
endif()

if(TURBOWASM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(
  TARGETS turbowasm
  EXPORT TurboWasmTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TURBOWASM_BUILD_TOOLS)
  install(TARGETS turbowasm_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(
  DIRECTORY include/turbowasm
  DESTINATION include
  FILES_MATCHING PATTERN "*.h")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboWasmConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/TurboWasmConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboWasm")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/TurboWasmConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/TurboWasmConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/TurboWasmConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboWasm")

install(
  EXPORT TurboWasmTargets
  FILE TurboWasmTargets.cmake
  NAMESPACE TurboWasm::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboWasm")
