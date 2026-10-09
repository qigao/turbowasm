include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/TurboWasmGuest.cmake")

# Consume explicit local source; never use the host Salts::CMeta binary. This
# list is the portable CMeta core, excluding architecture-specific native thunks.
function(_turbowasm_cmeta_guest_sources name source_dir out_sources)
  if(NOT IS_ABSOLUTE "${source_dir}" OR NOT EXISTS "${source_dir}/include/cmeta/cmeta.h")
    message(FATAL_ERROR "${name}: expected an absolute local Salts cmeta source directory")
  endif()
  set(sources)
  foreach(unit IN ITEMS abi cmeta component container_type data declared_type enum
      fingerprint manifest_view entry function fixed_array interface operation
      object infer invokable type_identity vector)
    list(APPEND sources "${source_dir}/src/${unit}.c")
  endforeach()
  set(${out_sources} "${sources}" PARENT_SCOPE)
endfunction()

function(turbowasm_add_cmeta_guest_library name source_dir)
  cmake_parse_arguments(PARSE_ARGV 2 arg "THREADS" "" "")
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "${name}: expected only an optional THREADS flag")
  endif()
  set(mode_options)
  if(arg_THREADS)
    set(mode_options THREADS)
  endif()
  _turbowasm_cmeta_guest_sources(${name} "${source_dir}" sources)
  turbowasm_add_c_guest_library(${name} ${mode_options} SOURCES ${sources}
    INCLUDE_DIRECTORIES "${source_dir}/include")
  set(${name}_ARCHIVE "${${name}_ARCHIVE}" PARENT_SCOPE)
endfunction()
