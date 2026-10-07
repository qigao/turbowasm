# Apply the requested sanitizer once, after compiler detection.
if(ENABLE_SANITIZER_ADDRESS)
  # Instrumentation enlarges interpreter frames. Keep the Wasm call-depth
  # trap reachable before exhausting the native stack used by the test host.
  add_compile_definitions(TURBOWASM_RESUMABLE_STACK_SIZE=${TURBOWASM_SANITIZER_STACK_BYTES})
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO /STACK:${TURBOWASM_SANITIZER_STACK_BYTES})
  elseif(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  else()
    message(FATAL_ERROR "AddressSanitizer is unsupported by this compiler")
  endif()
endif()
