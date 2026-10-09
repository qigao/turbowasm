# Include this file from an installed SDK to build C11 command guests with LLVM.
find_program(TURBOWASM_GUEST_CLANG NAMES clang REQUIRED)
if(NOT DEFINED TURBOWASM_GUEST_INCLUDE)
  set(TURBOWASM_GUEST_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/include")
  set(TURBOWASM_GUEST_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/lib/metallic.a")
  set(TURBOWASM_GUEST_CRT "${CMAKE_CURRENT_LIST_DIR}/lib/crt1.o")
endif()
function(turbowasm_add_c_guest name source)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}.wasm")
  add_custom_command(OUTPUT "${output}"
    COMMAND "${TURBOWASM_GUEST_CLANG}" --target=wasm32-unknown-unknown -mbulk-memory
      -std=c11 -O2 -flto -D__STDC_NO_THREADS__=1 -I "${TURBOWASM_GUEST_INCLUDE}"
      -MMD -MF "${output}.d" -MT "${output}"
      -nostdlib -Wl,--stack-first -Wl,-z,stack-size=262144 -Wl,--max-memory=16777216
      "${source}" "${TURBOWASM_GUEST_CRT}" "${TURBOWASM_GUEST_LIBRARY}" -o "${output}"
    DEPENDS "${source}" "${TURBOWASM_GUEST_CRT}" "${TURBOWASM_GUEST_LIBRARY}"
    DEPFILE "${output}.d" VERBATIM)
  add_custom_target(${name} ALL DEPENDS "${output}")
  set_target_properties(${name} PROPERTIES FOLDER "guest/programs")
  set(${name}_WASM "${output}" PARENT_SCOPE)
endfunction()
