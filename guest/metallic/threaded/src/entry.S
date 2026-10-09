    .text
    .globaltype __stack_pointer, i32
    .globaltype __tls_size, i32, immutable
    .globaltype __tls_align, i32, immutable
    .functype __wasm_init_tls (i32) -> ()
    .functype __metallic_thread_run (i32, i32) -> ()
    .globl wasi_thread_start
    .type wasi_thread_start,@function
wasi_thread_start:
    .functype wasi_thread_start (i32, i32) -> ()
    local.get 1
    i32.load 0
    global.set __stack_pointer
    local.get 1
    i32.load 4
    call __wasm_init_tls
    local.get 0
    local.get 1
    call __metallic_thread_run
    # No stack or TLS access after publishing terminal: a joiner may free both.
    local.get 1
    i32.const 1
    i32.atomic.store 8
    local.get 1
    i32.const 8
    i32.add
    i32.const -1
    memory.atomic.notify 0
    drop
    end_function
    .globl __metallic_tls_size
    .type __metallic_tls_size,@function
__metallic_tls_size:
    .functype __metallic_tls_size () -> (i32)
    global.get __tls_size
    end_function
    .globl __metallic_tls_align
    .type __metallic_tls_align,@function
__metallic_tls_align:
    .functype __metallic_tls_align () -> (i32)
    global.get __tls_align
    end_function
