# Installed only with the opt-in threaded SDK. All objects use this profile's
# native TLS, imported shared memory and LLVM standard EH/SJLJ ABI together.
set(TURBOWASM_GUEST_THREADED_PROFILE "metallic-wasm32-threads-v1")
set(TURBOWASM_GUEST_THREADED_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/include")
set(TURBOWASM_GUEST_THREADED_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/lib/metallic-threaded.a")
set(TURBOWASM_GUEST_THREADED_CRT "${CMAKE_CURRENT_LIST_DIR}/lib/crt1.o")
set(TURBOWASM_GUEST_THREADED_REACTOR_CRT "${CMAKE_CURRENT_LIST_DIR}/lib/crt1-reactor.o")
