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
option(TURBOWASM_BUILD_CONFORMANCE_RUNNER
  "Build the non-installed WebAssembly spec conformance runner" OFF)
option(TURBOWASM_ENABLE_MIR_JIT "Enable optional MIR JIT backend" OFF)
option(TURBOWASM_ENABLE_CFLOW_ADAPTER "Build optional CFlow execution adapter" ON)
option(TURBOWASM_ENABLE_NATIVE_IO_ADAPTER
  "Build optional Salts NativeIO host-wait adapter" ON)
option(TURBOWASM_ENABLE_WASI_ADAPTER
  "Build optional WASI Preview1 capability adapter" ON)
option(TURBOWASM_ENABLE_WASI_THREADS_ADAPTER
  "Build optional legacy WASI Preview1 threads adapter" ON)
option(TURBOWASM_ENABLE_WASI_NATIVE_IO_ADAPTER
  "Build optional WASI Preview1 NativeIO fd adapter" ON)

if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_ROOT must point at an installed Salts SDK")
endif()

file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" SALTS_ROOT_PATH)
find_package(Salts 1.8.3 EXACT CONFIG REQUIRED
  PATHS "${SALTS_ROOT_PATH}"
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH)

if(NOT TARGET Salts::CMeta)
  message(FATAL_ERROR
    "TurboWasm requires Salts 1.8.3 with the canonical Salts::CMeta target")
endif()

if(NOT TARGET Salts::SIMD)
  message(FATAL_ERROR
    "TurboWasm requires Salts 1.8.3 with the canonical Salts::SIMD target")
endif()

if(NOT TARGET Salts::Coroutine)
  message(FATAL_ERROR
    "TurboWasm requires Salts 1.8.3 with the canonical Salts::Coroutine target")
endif()

if(NOT TARGET Salts::Platform)
  message(FATAL_ERROR
    "TurboWasm requires Salts 1.8.3 with the canonical Salts::Platform target")
endif()

add_library(turbowasm
  src/atomic.c
  src/execution.c
  src/instance.c
  src/instance_state.c
  src/jit_simd_helper.c
  src/link.c
  src/module.c
  src/reader.c
  src/relaxed_simd.c
  src/simd.c
  src/simd_exec_table.c
  src/status.c
  src/value.c
  src/validation_context.c
  src/validate.c
  src/validate_data.c
  src/validate_element.c
  src/validate_instr.c
  src/validate_simd_table.c
  src/validate_linkage.c
  src/validate_type.c)

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
target_link_libraries(turbowasm
  PUBLIC Salts::CMeta Salts::SIMD
  PRIVATE Salts::Coroutine Salts::Platform)
if(UNIX)
  target_link_libraries(turbowasm PRIVATE m)
endif()

if(TURBOWASM_ENABLE_CFLOW_ADAPTER)
  if(NOT TARGET Salts::CFlow)
    message(FATAL_ERROR
      "TurboWasm CFlow adapter requires a Salts SDK that exports Salts::CFlow")
  endif()

  add_library(turbowasm_cflow
    src/cflow_adapter.c)
  add_library(TurboWasm::CFlow ALIAS turbowasm_cflow)
  set_target_properties(turbowasm_cflow PROPERTIES
    EXPORT_NAME CFlow
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
  target_compile_features(turbowasm_cflow PUBLIC c_std_11)
  target_include_directories(turbowasm_cflow
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)
  target_link_libraries(turbowasm_cflow
    PUBLIC
      TurboWasm::Runtime
      Salts::CFlow)

  if(MSVC)
    target_compile_options(turbowasm_cflow PRIVATE /W4)
  else()
    target_compile_options(turbowasm_cflow PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_NATIVE_IO_ADAPTER)
  if(NOT TARGET Salts::NativeIO)
    message(FATAL_ERROR
      "TurboWasm NativeIO adapter requires Salts 1.8.3 Salts::NativeIO")
  endif()

  add_library(turbowasm_native_io
    src/native_io_adapter.c)
  add_library(TurboWasm::NativeIO ALIAS turbowasm_native_io)
  set_target_properties(turbowasm_native_io PROPERTIES
    EXPORT_NAME NativeIO
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
  target_compile_features(turbowasm_native_io PUBLIC c_std_11)
  target_include_directories(turbowasm_native_io
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)
  target_link_libraries(turbowasm_native_io
    PUBLIC
      TurboWasm::Runtime
      Salts::NativeIO)

  if(MSVC)
    target_compile_options(turbowasm_native_io PRIVATE /W4)
  else()
    target_compile_options(turbowasm_native_io PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  add_library(turbowasm_wasi
    src/wasi_preview1.c
    src/wasi_fs.c)
  add_library(TurboWasm::WASI ALIAS turbowasm_wasi)
  set_target_properties(turbowasm_wasi PROPERTIES
    EXPORT_NAME WASI
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
  target_compile_features(turbowasm_wasi PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)
  target_link_libraries(turbowasm_wasi
    PUBLIC TurboWasm::Runtime)
  if(MSVC)
    target_compile_options(turbowasm_wasi PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_WASI_NATIVE_IO_ADAPTER)
  if(NOT TURBOWASM_ENABLE_WASI_ADAPTER OR
     NOT TURBOWASM_ENABLE_NATIVE_IO_ADAPTER)
    message(FATAL_ERROR
      "TurboWasm WASINativeIO requires both WASI and NativeIO adapters")
  endif()

  add_library(turbowasm_wasi_native_io
    src/wasi_native_io.c)
  add_library(TurboWasm::WASINativeIO ALIAS turbowasm_wasi_native_io)
  set_target_properties(turbowasm_wasi_native_io PROPERTIES
    EXPORT_NAME WASINativeIO
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
  target_compile_features(turbowasm_wasi_native_io PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi_native_io
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)
  target_link_libraries(turbowasm_wasi_native_io
    PUBLIC
      TurboWasm::WASI
      TurboWasm::NativeIO)

  if(MSVC)
    target_compile_options(turbowasm_wasi_native_io PRIVATE /W4)
  else()
    target_compile_options(turbowasm_wasi_native_io PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_WASI_THREADS_ADAPTER)
  if(NOT TARGET Salts::CFlow)
    message(FATAL_ERROR
      "TurboWasm WASIThreads adapter requires Salts 1.8.3 Salts::CFlow")
  endif()

  add_library(turbowasm_wasi_threads
    src/wasi_threads.c)
  add_library(TurboWasm::WASIThreads ALIAS turbowasm_wasi_threads)
  set_target_properties(turbowasm_wasi_threads PROPERTIES
    EXPORT_NAME WASIThreads
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
  target_compile_features(turbowasm_wasi_threads PUBLIC c_std_11)
  target_include_directories(turbowasm_wasi_threads
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(turbowasm_wasi_threads
    PUBLIC
      TurboWasm::Runtime
      Salts::CFlow
    PRIVATE
      Salts::Platform)

  if(MSVC)
    target_compile_options(turbowasm_wasi_threads PRIVATE
      /W4 /experimental:c11atomics)
  else()
    target_compile_options(turbowasm_wasi_threads PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TURBOWASM_ENABLE_MIR_JIT)
  find_package(MIR CONFIG REQUIRED)

  # Keep MIR out of the installed TurboWasm::Runtime link interface.
  # The backend is an internal, non-installed component until the tiering
  # integration defines its explicit runtime/plugin ownership.
  add_library(turbowasm_mir_backend STATIC
    src/jit/mir_backend.c)
  target_compile_features(turbowasm_mir_backend PRIVATE c_std_11)
  target_include_directories(turbowasm_mir_backend
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(turbowasm_mir_backend
    PRIVATE
      Salts::SIMD
      MIR::MIR)
  target_compile_definitions(turbowasm_mir_backend PRIVATE
    TURBOWASM_ENABLE_MIR_JIT=1)

  if(MSVC)
    target_compile_options(turbowasm_mir_backend PRIVATE /W4)
  else()
    target_compile_options(turbowasm_mir_backend PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

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

if(TURBOWASM_BUILD_CONFORMANCE_RUNNER)
  add_subdirectory(tests/conformance)
endif()

install(
  TARGETS turbowasm
  EXPORT TurboWasmTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TURBOWASM_ENABLE_CFLOW_ADAPTER)
  install(
    TARGETS turbowasm_cflow
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBOWASM_ENABLE_NATIVE_IO_ADAPTER)
  install(
    TARGETS turbowasm_native_io
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBOWASM_ENABLE_WASI_ADAPTER)
  install(
    TARGETS turbowasm_wasi
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBOWASM_ENABLE_WASI_NATIVE_IO_ADAPTER)
  install(
    TARGETS turbowasm_wasi_native_io
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBOWASM_ENABLE_WASI_THREADS_ADAPTER)
  install(
    TARGETS turbowasm_wasi_threads
    EXPORT TurboWasmTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

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
