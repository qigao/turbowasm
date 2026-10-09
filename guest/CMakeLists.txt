if(NOT TARGET TurboWasm::WASIHostFS OR NOT TURBOWASM_BUILD_TOOLS)
  message(FATAL_ERROR "Metallic guests require tools and the WASI HostFS adapter")
endif()
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES Salts::Core)
unset(TURBOWASM_HAVE_GUEST_FS CACHE)
check_c_source_compiles("#include <cmeta_fs.h>
#if !defined(SALTS_FS_ROOT_MUTATION_VERSION)
#error Metallic guests require secure rename, exclusive create and append mutation
#endif
int main(void) { return cmeta_fs_root_file_set_append(0, 0); }" TURBOWASM_HAVE_GUEST_FS)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT TURBOWASM_HAVE_GUEST_FS)
  message(FATAL_ERROR "Metallic guests require a Salts SDK with SALTS_FS_ROOT_MUTATION_VERSION")
endif()
find_program(TURBOWASM_GUEST_CLANG NAMES clang REQUIRED)
find_program(TURBOWASM_GUEST_AR NAMES llvm-ar REQUIRED)
if(NOT IS_ABSOLUTE "${TURBOWASM_METALLIC_SOURCE_DIR}" OR
   NOT EXISTS "${TURBOWASM_METALLIC_SOURCE_DIR}/src/crt/crt1.c" OR
   NOT EXISTS "${TURBOWASM_METALLIC_SOURCE_DIR}/include/stdio.h" OR
   NOT EXISTS "${TURBOWASM_METALLIC_SOURCE_DIR}/LICENSE")
  message(FATAL_ERROR "TURBOWASM_METALLIC_SOURCE_DIR must point to a local Metallic source checkout")
endif()
set(metallic_source "${TURBOWASM_METALLIC_SOURCE_DIR}")
set(TURBOWASM_GUEST_INCLUDE "${metallic_source}/include")
include(${CMAKE_CURRENT_SOURCE_DIR}/TurboWasmGuest.cmake)
# Match upstream's explicit two-depth source selection, restricted to vendor code.
file(GLOB metallic_sources RELATIVE "${metallic_source}" "${metallic_source}/src/*/*.c" "${metallic_source}/src/*/*/*.c")
set(metallic_objects)
set(metallic_variants single)
if(TURBOWASM_BUILD_METALLIC_THREADS OR (TURBOWASM_BUILD_TESTS AND TARGET TurboWasm::WASIThreads))
  list(APPEND metallic_variants threaded)
endif()
foreach(variant IN LISTS metallic_variants)
set(variant_objects)
set(variant_sources ${metallic_sources})
set(variant_directory objects)
set(variant_options -D__STDC_NO_THREADS__=1)
if(variant STREQUAL "threaded")
  set(variant_directory thread-objects)
  set(variant_options -D__METALLIC_THREADS__=1 -matomics -pthread -ftls-model=local-exec
    -I "${metallic_source}/threaded/include")
  list(APPEND variant_sources threaded/src/threads.c threaded/src/sync.c threaded/src/entry.S
    threaded/src/stdio_lock.c threaded/src/crt.c)
endif()
foreach(source IN LISTS variant_sources)
  set(metallic_unit_options)
  if(source MATCHES "^src/setjmp/")
    # LLVM creates these helper declarations during late SJLJ lowering. Keep
    # their definitions outside that LTO module to avoid renamed duplicates.
    set(metallic_unit_options -fno-lto)
  endif()
  set(object "${CMAKE_CURRENT_BINARY_DIR}/${variant_directory}/${source}.o")
  get_filename_component(object_dir "${object}" DIRECTORY)
  add_custom_command(OUTPUT "${object}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${object_dir}"
    COMMAND "${TURBOWASM_GUEST_CLANG}" --target=wasm32-unknown-unknown -mbulk-memory
      -O2 -flto -std=gnu11 ${variant_options} ${TURBOWASM_GUEST_SJLJ_OPTIONS} ${metallic_unit_options} -I "${metallic_source}/include"
      -MMD -MF "${object}.d" -MT "${object}"
      -c "${metallic_source}/${source}" -o "${object}"
    DEPENDS "${metallic_source}/${source}" DEPFILE "${object}.d" VERBATIM)
  if(source STREQUAL "src/crt/crt1.c")
    set(metallic_${variant}_crt "${object}")
  elseif(source STREQUAL "src/crt/crt1-reactor.c")
    set(metallic_${variant}_reactor_crt "${object}")
  else()
    list(APPEND variant_objects "${object}")
  endif()
endforeach()
set(metallic_${variant}_objects ${variant_objects})
endforeach()
set(metallic_objects ${metallic_single_objects})
set(TURBOWASM_GUEST_CRT "${metallic_single_crt}")
set(TURBOWASM_GUEST_REACTOR_CRT "${metallic_single_reactor_crt}")
set(TURBOWASM_GUEST_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/metallic.a")
set(metallic_response "")
foreach(object IN LISTS metallic_objects)
  string(APPEND metallic_response "\"${object}\"\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/metallic-objects.rsp" CONTENT "${metallic_response}")
add_custom_command(OUTPUT "${TURBOWASM_GUEST_LIBRARY}"
  COMMAND "${CMAKE_COMMAND}" -E rm -f "${TURBOWASM_GUEST_LIBRARY}"
  COMMAND "${TURBOWASM_GUEST_AR}" rcs "${TURBOWASM_GUEST_LIBRARY}" "@${CMAKE_CURRENT_BINARY_DIR}/metallic-objects.rsp"
  DEPENDS ${metallic_objects} "${CMAKE_CURRENT_BINARY_DIR}/metallic-objects.rsp" VERBATIM)
add_custom_target(turbowasm_guest_sdk ALL DEPENDS "${TURBOWASM_GUEST_LIBRARY}" "${TURBOWASM_GUEST_CRT}" "${TURBOWASM_GUEST_REACTOR_CRT}")
if(metallic_threaded_objects)
  set(TURBOWASM_GUEST_THREADED_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/metallic-threaded.a")
  set(threaded_response "")
  foreach(object IN LISTS metallic_threaded_objects)
    string(APPEND threaded_response "\"${object}\"\n")
  endforeach()
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/metallic-threaded-objects.rsp" CONTENT "${threaded_response}")
  add_custom_command(OUTPUT "${TURBOWASM_GUEST_THREADED_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${TURBOWASM_GUEST_THREADED_LIBRARY}"
    COMMAND "${TURBOWASM_GUEST_AR}" rcs "${TURBOWASM_GUEST_THREADED_LIBRARY}" "@${CMAKE_CURRENT_BINARY_DIR}/metallic-threaded-objects.rsp"
    DEPENDS ${metallic_threaded_objects} "${CMAKE_CURRENT_BINARY_DIR}/metallic-threaded-objects.rsp" VERBATIM)
  add_custom_target(turbowasm_guest_threaded_sdk ALL DEPENDS "${TURBOWASM_GUEST_THREADED_LIBRARY}"
    "${metallic_threaded_crt}" "${metallic_threaded_reactor_crt}")
  set_target_properties(turbowasm_guest_threaded_sdk PROPERTIES FOLDER "guest")
endif()
if(TURBOWASM_BUILD_METALLIC_THREADS)
  set(TURBOWASM_GUEST_THREADED_PROFILE "metallic-wasm32-threads-v1")
  set(TURBOWASM_GUEST_THREADED_INCLUDE "${metallic_source}/threaded/include")
  set(TURBOWASM_GUEST_THREADED_CRT "${metallic_threaded_crt}")
  set(TURBOWASM_GUEST_THREADED_REACTOR_CRT "${metallic_threaded_reactor_crt}")
endif()
set_target_properties(turbowasm_guest_sdk PROPERTIES FOLDER "guest")
set(TURBOWASM_GUEST_INCLUDE "${metallic_source}/include")
include(${CMAKE_CURRENT_SOURCE_DIR}/TurboWasmGuest.cmake)
if(TURBOWASM_GUEST_CMETA_SOURCE_DIR)
  include(${CMAKE_CURRENT_SOURCE_DIR}/TurboWasmGuestCMeta.cmake)
  turbowasm_add_cmeta_guest_library(turbowasm_guest_cmeta "${TURBOWASM_GUEST_CMETA_SOURCE_DIR}")
endif()
turbowasm_add_c_guest(guest_hello "${PROJECT_SOURCE_DIR}/examples/guest/hello.c")
turbowasm_add_c_reactor(guest_counter SOURCES "${PROJECT_SOURCE_DIR}/examples/guest/counter.c"
  EXPORTS counter_add counter_initializations counter_close)
add_executable(turbowasm_reactor_example
  "${PROJECT_SOURCE_DIR}/examples/guest/reactor_host.c"
  "${PROJECT_SOURCE_DIR}/examples/guest/reactor_session.c")
target_link_libraries(turbowasm_reactor_example PRIVATE TurboWasm::Runtime)
target_compile_features(turbowasm_reactor_example PRIVATE c_std_11)
target_compile_options(turbowasm_reactor_example PRIVATE $<$<COMPILE_LANG_AND_ID:C,MSVC>:-experimental:c11atomics>)
set_target_properties(turbowasm_reactor_example PROPERTIES FOLDER "examples/guest")
add_dependencies(turbowasm_reactor_example guest_counter)
if(TURBOWASM_BUILD_METALLIC_THREADS)
  turbowasm_add_c_reactor(guest_threaded_counter THREADS
    SOURCES "${PROJECT_SOURCE_DIR}/examples/guest/threaded_counter.c" EXPORTS counter_add counter_close)
  add_executable(turbowasm_threaded_reactor_example
    "${PROJECT_SOURCE_DIR}/examples/guest/threaded_host.c"
    "${PROJECT_SOURCE_DIR}/examples/guest/threaded_session.c")
  target_link_libraries(turbowasm_threaded_reactor_example PRIVATE TurboWasm::WASIThreads TurboWasm::WASI)
  target_compile_features(turbowasm_threaded_reactor_example PRIVATE c_std_11)
  target_compile_options(turbowasm_threaded_reactor_example PRIVATE $<$<COMPILE_LANG_AND_ID:C,MSVC>:-experimental:c11atomics>)
  set_target_properties(turbowasm_threaded_reactor_example PROPERTIES FOLDER "examples/guest")
  add_dependencies(turbowasm_threaded_reactor_example guest_threaded_counter)
  if(TURBOWASM_BUILD_TESTS)
    add_test(NAME turbowasm_guest_threaded_reactor_example
      COMMAND turbowasm_threaded_reactor_example "${guest_threaded_counter_WASM}")
    set_tests_properties(turbowasm_guest_threaded_reactor_example PROPERTIES TIMEOUT 30)
  endif()
endif()
if(TURBOWASM_BUILD_TESTS)
  add_subdirectory(${PROJECT_SOURCE_DIR}/tests/guest ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()
install(DIRECTORY "${metallic_source}/include/" DESTINATION share/turbowasm/guest/include)
install(FILES "${TURBOWASM_GUEST_LIBRARY}" DESTINATION share/turbowasm/guest/lib)
install(FILES "${TURBOWASM_GUEST_CRT}" DESTINATION share/turbowasm/guest/lib RENAME crt1.o)
install(FILES "${TURBOWASM_GUEST_REACTOR_CRT}" DESTINATION share/turbowasm/guest/lib RENAME crt1-reactor.o)
install(FILES TurboWasmGuest.cmake TurboWasmGuestCMeta.cmake DESTINATION share/turbowasm/guest)
if(TURBOWASM_BUILD_METALLIC_THREADS)
  install(DIRECTORY "${metallic_source}/threaded/include/" DESTINATION share/turbowasm/guest/threaded/include)
  install(FILES "${TURBOWASM_GUEST_THREADED_LIBRARY}" DESTINATION share/turbowasm/guest/threaded/lib)
  install(FILES "${TURBOWASM_GUEST_THREADED_CRT}" DESTINATION share/turbowasm/guest/threaded/lib RENAME crt1.o)
  install(FILES "${TURBOWASM_GUEST_THREADED_REACTOR_CRT}" DESTINATION share/turbowasm/guest/threaded/lib RENAME crt1-reactor.o)
  install(FILES TurboWasmGuestThreads.cmake DESTINATION share/turbowasm/guest/threaded)
endif()
install(FILES "${TURBOWASM_METALLIC_SOURCE_DIR}/LICENSE" DESTINATION share/turbowasm/guest/licenses RENAME Metallic.txt)
