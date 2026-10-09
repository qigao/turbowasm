option(TURBOWASM_BUILD_TOOLS "Build TurboWasm command-line tools" ON)
option(TURBOWASM_BUILD_METALLIC_GUESTS "Build the local Metallic wasm32 guest SDK and C11 guests" OFF)
set(TURBOWASM_METALLIC_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/guest/metallic" CACHE PATH
  "Absolute path to the local Metallic sources")
set(TURBOWASM_GUEST_CMETA_SOURCE_DIR "" CACHE PATH
  "Optional local Salts cmeta source directory to cross-compile and qualify with Metallic")
option(TURBOWASM_BUILD_TESTS "Build TurboWasm tests" ON)
option(TURBOWASM_BUILD_CONFORMANCE_RUNNER
  "Build the non-installed WebAssembly spec conformance runner" OFF)
option(TURBOWASM_ENABLE_MIR_JIT "Enable optional MIR JIT backend" OFF)
option(TURBOWASM_ENABLE_COMPONENT
  "Build installed synchronous Component Model façade" ON)
option(TURBOWASM_ENABLE_WASI02_ADAPTER
  "Build experimental WASI 0.2 typed Component capability layer" ON)
option(TURBOWASM_ENABLE_WASI02_CNET_ADAPTER
  "Build optional released-Salts CNet WASI 0.2 TCP control-plane provider" ON)
option(TURBOWASM_ENABLE_WASI02_SOCKET_BACKEND
  "Build installed WASI 0.2 native TCP backend (requires directional CNet EOF)" OFF)
option(TURBOWASM_ENABLE_CFLOW_ADAPTER "Build optional CFlow execution adapter" ON)
option(TURBOWASM_ENABLE_NATIVE_IO_ADAPTER
  "Build optional Salts NativeIO host-wait adapter" ON)
option(TURBOWASM_ENABLE_WASI_ADAPTER
  "Build optional WASI Preview1 capability adapter" ON)
option(TURBOWASM_QUALIFY_WASI_ADAPTER_PLAN
  "Regenerate the checked Preview1 adapter plan with SaltsUtils tooling" OFF)
option(TURBOWASM_ENABLE_WASI_THREADS_ADAPTER
  "Build optional legacy WASI Preview1 threads adapter" ON)
option(TURBOWASM_ENABLE_WASI_NATIVE_IO_ADAPTER
  "Build optional WASI Preview1 NativeIO fd adapter" ON)
option(TURBOWASM_ENABLE_WASI_HOST_FS_ADAPTER
  "Build optional Salts-backed WASI filesystem provider" ON)
option(TURBOWASM_ENABLE_WASI_LITTLEFS_ADAPTER
  "Build optional littlefs-backed WASI filesystem provider" OFF)
set(TURBOWASM_LITTLEFS_SOURCE_DIR "" CACHE PATH
  "Path to an externally acquired littlefs source checkout")

option(ENABLE_SANITIZER_ADDRESS "Enable AddressSanitizer" OFF)
set(TURBOWASM_SANITIZER_STACK_BYTES "8388608" CACHE STRING
  "Native executable/coroutine stack budget for instrumented builds")
if(NOT TURBOWASM_SANITIZER_STACK_BYTES MATCHES "^[1-9][0-9]*$" OR
   TURBOWASM_SANITIZER_STACK_BYTES LESS 524288 OR
   TURBOWASM_SANITIZER_STACK_BYTES GREATER 1073741824)
  message(FATAL_ERROR "TURBOWASM_SANITIZER_STACK_BYTES must be between 524288 and 1073741824")
endif()
set(TURBOWASM_TYPE_DEPENDENCY_LIMIT "256" CACHE STRING
  "Maximum recursive type-group dependency depth")
if(NOT TURBOWASM_TYPE_DEPENDENCY_LIMIT MATCHES "^[1-9][0-9]*$" OR
   TURBOWASM_TYPE_DEPENDENCY_LIMIT GREATER 1024)
  message(FATAL_ERROR "TURBOWASM_TYPE_DEPENDENCY_LIMIT must be between 1 and 1024")
endif()
set(TURBOWASM_SPEC_ROOT "" CACHE PATH "Pinned WebAssembly spec checkout for conformance tests")
set(TURBOWASM_CORE3_TOOLS "" CACHE FILEPATH "wasm-tools executable for the full Core 3.0 test suite")

# The vcpkg toolchain consumes features during project().
set(VCPKG_MANIFEST_FEATURES "")
if(TURBOWASM_ENABLE_MIR_JIT)
  list(APPEND VCPKG_MANIFEST_FEATURES mir)
endif()
if(TURBOWASM_BUILD_CONFORMANCE_RUNNER AND NOT TURBOWASM_CORE3_TOOLS)
  list(APPEND VCPKG_MANIFEST_FEATURES spec-tools)
endif()
