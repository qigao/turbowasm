# C11 command/Reactor guests and static libraries using Metallic. Guest targets
# use the guest compiler, not the embedding project's native C compiler.
include_guard(GLOBAL)
find_program(TURBOWASM_GUEST_CLANG NAMES clang REQUIRED)
find_program(TURBOWASM_GUEST_AR NAMES llvm-ar REQUIRED)
execute_process(COMMAND "${TURBOWASM_GUEST_CLANG}" --version
  RESULT_VARIABLE guest_clang_status OUTPUT_VARIABLE guest_clang_version)
if(NOT guest_clang_status EQUAL 0 OR NOT guest_clang_version MATCHES "clang version ([0-9]+)")
  message(FATAL_ERROR "Cannot identify the Metallic guest Clang toolchain")
endif()
if(CMAKE_MATCH_1 LESS 20)
  message(FATAL_ERROR "Metallic guests require LLVM 20 or newer for standard Wasm EH and SJLJ; selected ${TURBOWASM_GUEST_CLANG} (LLVM ${CMAKE_MATCH_1})")
endif()
set(TURBOWASM_GUEST_SJLJ_OPTIONS -mexception-handling
  -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false)
if(NOT DEFINED TURBOWASM_GUEST_INCLUDE)
  set(TURBOWASM_GUEST_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/include")
  set(TURBOWASM_GUEST_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/lib/metallic.a")
  set(TURBOWASM_GUEST_CRT "${CMAKE_CURRENT_LIST_DIR}/lib/crt1.o")
  set(TURBOWASM_GUEST_REACTOR_CRT "${CMAKE_CURRENT_LIST_DIR}/lib/crt1-reactor.o")
endif()

function(_turbowasm_guest_objects name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;INCLUDE_DIRECTORIES;COMPILE_OPTIONS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "${name}: expected SOURCES and optional INCLUDE_DIRECTORIES/COMPILE_OPTIONS")
  endif()
  set(includes -I "${TURBOWASM_GUEST_INCLUDE}")
  foreach(dir IN LISTS arg_INCLUDE_DIRECTORIES)
    get_filename_component(dir "${dir}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND includes -I "${dir}")
  endforeach()
  set(objects)
  set(index 0)
  foreach(source IN LISTS arg_SOURCES)
    get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(stem "${source}" NAME)
    set(object "${CMAKE_CURRENT_BINARY_DIR}/${name}.objects/${index}-${stem}.o")
    add_custom_command(OUTPUT "${object}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/${name}.objects"
      COMMAND "${TURBOWASM_GUEST_CLANG}" --target=wasm32-unknown-unknown -mbulk-memory
        -std=c11 -O2 -flto -D__STDC_NO_THREADS__=1 ${TURBOWASM_GUEST_SJLJ_OPTIONS} ${includes} ${arg_COMPILE_OPTIONS}
        -MMD -MF "${object}.d" -MT "${object}" -c "${source}" -o "${object}"
      DEPENDS "${source}" DEPFILE "${object}.d" VERBATIM COMMAND_EXPAND_LISTS)
    list(APPEND objects "${object}")
    math(EXPR index "${index} + 1")
  endforeach()
  set(${name}_OBJECTS "${objects}" PARENT_SCOPE)
endfunction()

function(turbowasm_add_c_guest_library name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;INCLUDE_DIRECTORIES;COMPILE_OPTIONS")
  if(arg_UNPARSED_ARGUMENTS OR arg_KEYWORDS_MISSING_VALUES OR NOT arg_SOURCES)
    message(FATAL_ERROR "${name}: expected SOURCES and optional INCLUDE_DIRECTORIES/COMPILE_OPTIONS")
  endif()
  _turbowasm_guest_objects(${name} SOURCES ${arg_SOURCES}
    INCLUDE_DIRECTORIES ${arg_INCLUDE_DIRECTORIES} COMPILE_OPTIONS ${arg_COMPILE_OPTIONS})
  set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}.a")
  set(response "")
  foreach(object IN LISTS ${name}_OBJECTS)
    string(APPEND response "\"${object}\"\n")
  endforeach()
  file(GENERATE OUTPUT "${output}.rsp" CONTENT "${response}")
  add_custom_command(OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${output}"
    COMMAND "${TURBOWASM_GUEST_AR}" rcs "${output}" "@${output}.rsp"
    DEPENDS ${${name}_OBJECTS} "${output}.rsp" VERBATIM)
  add_custom_target(${name} ALL DEPENDS "${output}")
  set_target_properties(${name} PROPERTIES FOLDER "guest/libraries" TURBOWASM_GUEST_ARCHIVE "${output}")
  set(${name}_ARCHIVE "${output}" PARENT_SCOPE)
endfunction()

function(_turbowasm_guest_program name mode)
  cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "SOURCES;INCLUDE_DIRECTORIES;LIBRARIES;EXPORTS;COMPILE_OPTIONS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "${name}: invalid or missing guest program arguments")
  endif()
  _turbowasm_guest_objects(${name} SOURCES ${arg_SOURCES}
    INCLUDE_DIRECTORIES ${arg_INCLUDE_DIRECTORIES} COMPILE_OPTIONS ${arg_COMPILE_OPTIONS})
  set(libraries)
  set(library_targets)
  foreach(library IN LISTS arg_LIBRARIES)
    if(TARGET "${library}")
      get_target_property(archive "${library}" TURBOWASM_GUEST_ARCHIVE)
      if(NOT archive)
        message(FATAL_ERROR "${name}: ${library} is not a Wasm guest library target")
      endif()
      list(APPEND libraries "${archive}")
      list(APPEND library_targets "${library}")
    elseif(IS_ABSOLUTE "${library}")
      list(APPEND libraries "${library}")
    else()
      message(FATAL_ERROR "${name}: guest libraries must be guest targets or absolute archive paths")
    endif()
  endforeach()
  set(exports)
  foreach(symbol IN LISTS arg_EXPORTS)
    list(APPEND exports "-Wl,--export=${symbol}")
  endforeach()
  set(crt "${TURBOWASM_GUEST_CRT}")
  if(mode STREQUAL "REACTOR")
    set(crt "${TURBOWASM_GUEST_REACTOR_CRT}")
    if(NOT crt)
      message(FATAL_ERROR "${name}: Metallic Reactor CRT is not configured")
    endif()
    list(APPEND exports -Wl,--no-entry -Wl,--export=_initialize)
  endif()
  set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}.wasm")
  add_custom_command(OUTPUT "${output}"
    COMMAND "${TURBOWASM_GUEST_CLANG}" --target=wasm32-unknown-unknown -mbulk-memory
      -O2 -flto -nostdlib -mexception-handling
      -Wl,-mllvm,-wasm-enable-sjlj,-mllvm,-wasm-use-legacy-eh=false,-mllvm,-exception-model=wasm
      -Wl,-u,__wasm_setjmp,-u,__wasm_setjmp_test,-u,__wasm_longjmp
      -Wl,--stack-first -Wl,-z,stack-size=262144 -Wl,--max-memory=16777216
      ${${name}_OBJECTS} "${crt}" ${libraries} "${TURBOWASM_GUEST_LIBRARY}" ${exports} -o "${output}"
    DEPENDS ${${name}_OBJECTS} "${crt}" ${libraries} "${TURBOWASM_GUEST_LIBRARY}"
    VERBATIM COMMAND_EXPAND_LISTS)
  add_custom_target(${name} ALL DEPENDS "${output}")
  if(library_targets)
    add_dependencies(${name} ${library_targets})
  endif()
  set_target_properties(${name} PROPERTIES FOLDER "guest/programs")
  set(${name}_WASM "${output}" PARENT_SCOPE)
endfunction()

function(turbowasm_add_c_guest name source)
  _turbowasm_guest_program(${name} COMMAND SOURCES "${source}" ${ARGN})
  set(${name}_WASM "${${name}_WASM}" PARENT_SCOPE)
endfunction()

function(turbowasm_add_c_reactor name)
  _turbowasm_guest_program(${name} REACTOR ${ARGN})
  set(${name}_WASM "${${name}_WASM}" PARENT_SCOPE)
endfunction()
