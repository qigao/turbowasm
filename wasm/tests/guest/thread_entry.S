/* Private ABI qualification trampoline. No C frame may precede stack/TLS
 * installation, or follow terminal publication. See #426 architecture. */
    .text
    .globaltype __stack_pointer, i32
    .globaltype __tls_size, i32, immutable
    .globaltype __tls_align, i32, immutable
    .functype __wasm_init_tls (i32) -> ()
    .functype fixture_child (i32, i32) -> ()

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
    call fixture_child
    local.get 1
    i32.const 1
    i32.atomic.store 8
    local.get 1
    i32.const 8
    i32.add
    i32.const 1
    memory.atomic.notify 0
    drop
    end_function

    .globl fixture_tls_size
    .type fixture_tls_size,@function
fixture_tls_size:
    .functype fixture_tls_size () -> (i32)
    global.get __tls_size
    end_function

    .globl fixture_tls_align
    .type fixture_tls_align,@function
fixture_tls_align:
    .functype fixture_tls_align () -> (i32)
    global.get __tls_align
    end_function
