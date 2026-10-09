#include <tinytest.h>
#include <turbowasm/turbowasm.h>
#include <string.h>

typedef struct copy_probe { unsigned calls; uint64_t size; bool shared; } copy_probe;
static turbowasm_name name(const char *s) { return (turbowasm_name){(const uint8_t *)s, (uint32_t)strlen(s)}; }

static turbowasm_status copy(void *context, turbowasm_host_call *call,
    const turbowasm_value *arguments, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)arguments; (void)results; (void)capacity;
    copy_probe *p = context;
    check_equal(argc, (size_t)0);
    uint8_t bytes[4] = {0};
    const uint8_t marker[] = {0x12, 0x34, 0x56, 0x78};
    check_equal(turbowasm_host_call_memory_read64(call, 0, 10, bytes, 4, trap), TURBOWASM_OK);
    if (p->calls) check_equal(memcmp(bytes, marker, 4), 0);
    else check_equal(bytes[0], (uint8_t)0);
    check_equal(turbowasm_host_call_memory_check64(call, 0, 10, 4, trap), TURBOWASM_OK);
    check_equal(turbowasm_host_call_memory_write64(call, 0, 10, marker, 4, trap), TURBOWASM_OK);
    check_equal(turbowasm_host_call_memory_read64(call, 0, 10, bytes, 4, trap), TURBOWASM_OK);
    check_equal(memcmp(bytes, marker, 4), 0);

    memset(bytes, 0x5a, sizeof(bytes));
    check_equal(turbowasm_host_call_memory_read64(call, 0, p->size - 2, bytes, 4, trap), TURBOWASM_TRAPPED);
    check_equal(*trap, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
    for (size_t i = 0; i < sizeof(bytes); ++i) check_equal(bytes[i], (uint8_t)0x5a);
    check_equal(turbowasm_host_call_memory_write64(call, 0, p->size - 2, marker, 4, trap), TURBOWASM_TRAPPED);
    check_equal(turbowasm_host_call_memory_read64(call, 0, p->size - 2, bytes, 2, trap), TURBOWASM_OK);
    check_equal(bytes[0], (uint8_t)0); check_equal(bytes[1], (uint8_t)0);
    check_equal(*trap, TURBOWASM_TRAP_NONE);
    check_equal(turbowasm_host_call_memory_check64(call, 0, 1, UINT64_MAX, trap), TURBOWASM_TRAPPED);
    check_equal(turbowasm_host_call_memory_check64(call, 0, UINT64_MAX, 2, trap), TURBOWASM_TRAPPED);
    check_equal(turbowasm_host_call_memory_read64(call, 0, UINT64_C(0x10000000a), bytes, 1, trap), TURBOWASM_TRAPPED);
    check_equal(turbowasm_host_call_memory_check64(call, 0, p->size, 0, trap), TURBOWASM_OK);
    check_equal(turbowasm_host_call_memory_read64(call, 0, p->size, NULL, 0, trap), TURBOWASM_OK);
    check_equal(turbowasm_host_call_memory_write64(call, 0, p->size, NULL, 0, trap), TURBOWASM_OK);
    check_equal(turbowasm_host_call_memory_check64(call, 0, p->size + 1, 0, trap), TURBOWASM_TRAPPED);
    check_equal(turbowasm_host_call_memory_read64(call, 0, 0, NULL, 1, trap), TURBOWASM_INVALID_ARGUMENT);
    check_equal(*trap, TURBOWASM_TRAP_NONE);
    check_equal(turbowasm_host_call_memory_write64(call, 0, 0, NULL, 1, trap), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_host_call_memory_check64(call, 1, 0, 0, trap), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_host_call_memory_read64(call, 1, 0, bytes, 1, trap), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_host_call_memory_write64(call, 1, 0, bytes, 1, trap), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_host_call_memory_check64(call, 0, 0, 0, NULL), TURBOWASM_INVALID_ARGUMENT);
    turbowasm_host_memory_span span = {0};
    check_equal(turbowasm_host_call_memory_span64(call, 0, 10, 4, &span, trap),
        p->shared ? TURBOWASM_UNSUPPORTED : TURBOWASM_OK);
    ++p->calls; *count = 0; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static void exercise(bool shared, bool memory64) {
    uint8_t flags = (uint8_t)(1u | (shared ? 2u : 0u) | (memory64 ? 4u : 0u));
    uint8_t type = memory64 ? 0x7e : 0x7f, constant = memory64 ? 0x42 : 0x41;
    const uint8_t provider_bytes[] = {
        0,97,115,109,1,0,0,0,
        1,5,1,0x60,0,1,type, 3,2,1,0,
        5,4,1,flags,1,2, 7,10,1,6,'m','e','m','o','r','y',2,0,
        10,8,1,6,0,constant,1,0x40,0,0x0b
    };
    const uint8_t consumer_bytes[] = {
        0,97,115,109,1,0,0,0, 1,4,1,0x60,0,0,
        2,23,2, 1,'h',4,'c','o','p','y',0,0,
        1,'p',6,'m','e','m','o','r','y',2,flags,1,2
    };
    turbowasm_module pm = {0}, cm = {0};
    turbowasm_instance provider = {0}, first = {0}, second = {0};
    turbowasm_linker linker = {0};
    turbowasm_host_function_type function = {0};
    copy_probe probe = {0, 65536, shared};
    check_equal(turbowasm_module_load_borrowed(&pm, provider_bytes, sizeof(provider_bytes)), TURBOWASM_OK);
    check_equal(turbowasm_module_load_borrowed(&cm, consumer_bytes, sizeof(consumer_bytes)), TURBOWASM_OK);
    check_equal(turbowasm_instance_create(&provider, &pm), TURBOWASM_OK);
    check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
    check_equal(turbowasm_linker_define_instance(&linker, name("p"), &provider), TURBOWASM_OK);
    check_equal(turbowasm_linker_define_host_function(&linker, name("h"), name("copy"), &function, copy, &probe), TURBOWASM_OK);
    check_equal(turbowasm_instance_create_linked(&first, &cm, &linker), TURBOWASM_OK);
    check_equal(turbowasm_instance_create_linked(&second, &cm, &linker), TURBOWASM_OK);
    size_t count = 0; turbowasm_trap trap = TURBOWASM_TRAP_NONE; turbowasm_value result = {0};
    check_equal(turbowasm_instance_invoke(&first, 0, NULL, 0, NULL, 0, &count, &trap), TURBOWASM_OK);
    check_equal(turbowasm_instance_invoke(&second, 0, NULL, 0, NULL, 0, &count, &trap), TURBOWASM_OK);
    check_equal(turbowasm_instance_invoke(&provider, 0, NULL, 0, &result, 1, &count, &trap), TURBOWASM_OK);
    check_equal(memory64 ? result.as.i64 : result.as.i32, 1);
    probe.size *= 2;
    check_equal(turbowasm_instance_invoke(&first, 0, NULL, 0, NULL, 0, &count, &trap), TURBOWASM_OK);
    check_equal(probe.calls, 3u);
    turbowasm_instance_destroy(&second); turbowasm_instance_destroy(&first);
    turbowasm_linker_destroy(&linker); turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&cm); turbowasm_module_destroy(&pm);
}

spec("Protected host memory copies") {
    it("copies shared memory32 across imported instances and growth") { exercise(true, false); }
    it("copies shared memory64 without truncating addresses") { exercise(true, true); }
    it("preserves memory32 span compatibility") { exercise(false, false); }
    it("preserves memory64 span compatibility") { exercise(false, true); }
    it("rejects absent host calls and clears stale traps") {
        turbowasm_trap trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        check_equal(turbowasm_host_call_memory_check64(NULL, 0, 0, 0, &trap), TURBOWASM_INVALID_ARGUMENT);
        check_equal(trap, TURBOWASM_TRAP_NONE);
        check_equal(turbowasm_host_call_memory_read64(NULL, 0, 0, NULL, 0, &trap), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_host_call_memory_write64(NULL, 0, 0, NULL, 0, &trap), TURBOWASM_INVALID_ARGUMENT);
    }
}
