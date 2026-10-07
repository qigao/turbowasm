#include <turbowasm/component.h>
#include "component_string.h"
#include "component_exec.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include "fixtures/component_encodings.h"
#include "fixtures/component_encoding_imports.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { BUFFER_CAPACITY = 256, MAX_REALLOCS = 16, MEMORY_SIZE = 65536, HEAP_START = 1024, TUPLE_ADDRESS = 32, MAX_RESUMES = 256 };
static const uint8_t memory32[] = {0,0x61,0x73,0x6d,1,0,0,0,5,3,1,0,1};
static const uint8_t memory64[] = {0,0x61,0x73,0x6d,1,0,0,0,5,3,1,4,1};
static uint8_t ascii[] = {'A',0,'Z'};
static uint8_t latin[] = {'A',0xc3,0xa9,0,'Z'};
static uint8_t mixed[] = {'A',0xc3,0xa9,0xe4,0xb8,0xad,0xf0,0x9f,0x98,0x80};
static const uint8_t mixed_utf16[] = {0x41,0,0xe9,0,0x2d,0x4e,0x3d,0xd8,0,0xde};
static turbowasm_module module;
static turbowasm_instance instance;
static turbowasm_component_canonical_memory memory;
static turbowasm_component_type_graph graph;
static turbowasm_component component;
static turbowasm_component_instance public_instance;
static turbowasm_component_call call;
static turbowasm_component_binary provider_binary, consumer_binary;
static turbowasm_component_exec provider_exec, consumer_exec;
static const char *provider_export;
static struct { uint64_t old, old_size, alignment, size, result; } requests[MAX_REALLOCS];
static struct { size_t count, fail_at, bad_at; uint64_t cursor, bad_pointer; } guest;
static struct { size_t live, attempts, fail_at; } allocations;

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (++allocations.attempts == allocations.fail_at) return NULL;
    p = malloc(size); if (p != NULL) ++allocations.live; return p;
}
static void deallocate(void *context, void *p) {
    (void)context;
    if (p != NULL) { check_true(allocations.live != 0); --allocations.live; }
    free(p);
}
static turbowasm_status realloc_guest(void *context, uint64_t old, uint64_t old_size,
    uint64_t alignment, uint64_t size, uint64_t *out) {
    uint8_t saved[BUFFER_CAPACITY];
    uint64_t pointer, copied = old_size < size ? old_size : size;
    size_t index = guest.count++;
    (void)context;
    check_true(index < MAX_REALLOCS); check_true(size <= BUFFER_CAPACITY);
    requests[index].old = old; requests[index].old_size = old_size;
    requests[index].alignment = alignment; requests[index].size = size;
    if (guest.count == guest.fail_at) return TURBOWASM_OUT_OF_MEMORY;
    if (guest.count == guest.bad_at) { *out = guest.bad_pointer; return TURBOWASM_OK; }
    pointer = (guest.cursor + alignment - 1) & ~(alignment - 1);
    if (copied != 0) {
        check_equal(turbowasm_instance_memory_read_bytes(instance.impl, 0, old, 0, saved, (size_t)copied), TURBOWASM_OK);
        check_equal(turbowasm_instance_memory_write_bytes(instance.impl, 0, pointer, 0, saved, (size_t)copied), TURBOWASM_OK);
    }
    guest.cursor = pointer + size + BUFFER_CAPACITY;
    requests[index].result = pointer; *out = pointer; return TURBOWASM_OK;
}
static void reset_guest(void) {
    memset(&guest, 0, sizeof(guest)); memset(requests, 0, sizeof(requests)); guest.cursor = HEAP_START;
}
static void setup(bool wide) {
    turbowasm_runtime_config config;
    turbowasm_runtime_config_init(&config);
    config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
    check_equal(turbowasm_module_load_borrowed_with_config(&module, wide ? memory64 : memory32,
        sizeof(memory32), &config), TURBOWASM_OK);
    check_equal(turbowasm_instance_create(&instance, &module), TURBOWASM_OK);
    memory = (turbowasm_component_canonical_memory){0}; memory.instance = &instance;
    memory.pointer_type = wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
    memory.guest_realloc = realloc_guest; reset_guest();
}
static void close_memory(void) { turbowasm_instance_destroy(&instance); turbowasm_module_destroy(&module); }
static turbowasm_component_value value(uint8_t *data, size_t size, turbowasm_component_string_origin origin) {
    turbowasm_component_value v = {0}; v.kind = TURBOWASM_COMPONENT_TYPE_STRING;
    v.as.string = (turbowasm_component_owned_string){data, size, origin}; return v;
}
static void roundtrip(turbowasm_component_value input) {
    turbowasm_component_value result = {0};
    turbowasm_component_type_ref type = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_STRING);
    check_equal(turbowasm_component_canonical_lower_value(&graph, type, &memory, TUPLE_ADDRESS, &input), TURBOWASM_OK);
    check_equal(turbowasm_component_canonical_lift_value(&graph, type, &memory, TUPLE_ADDRESS, &result), TURBOWASM_OK);
    check_equal(result.as.string.size, input.as.string.size);
    if (input.as.string.size != 0) check_equal(memcmp(result.as.string.data, input.as.string.data, input.as.string.size), 0);
    check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
}
static void expect_request(size_t at, uint64_t old_size, uint64_t alignment, uint64_t size) {
    check_true(at < guest.count);
    check_equal(requests[at].old_size, old_size); check_equal(requests[at].alignment, alignment); check_equal(requests[at].size, size);
    check_equal(requests[at].old, at == 0 ? 0u : requests[at - 1].result);
}
static void public_setup(void) {
    check_equal(turbowasm_component_load_borrowed(&component, component_encodings_bytes,
        sizeof(component_encodings_bytes)), TURBOWASM_OK);
    check_equal(turbowasm_component_instance_create(&public_instance, &component), TURBOWASM_OK);
}
static turbowasm_component_host_value invoke(const char *name, const turbowasm_component_host_value *input) {
    turbowasm_component_host_value out = {0}; size_t count = 0; turbowasm_trap trap;
    check_equal(turbowasm_component_instance_invoke(&public_instance,
        (turbowasm_name){(const uint8_t *)name, (uint32_t)strlen(name)}, input, 1, &out, 1, &count, &trap), TURBOWASM_OK);
    check_equal(count, 1u); check_equal(trap, TURBOWASM_TRAP_NONE); return out;
}
static bool can_bind(void *context, turbowasm_component_name instance_name,
    turbowasm_component_name function_name, const turbowasm_component_type_graph *types,
    turbowasm_component_type_id function_type) {
    const turbowasm_component_type *type = turbowasm_component_type_graph_get(types, function_type);
    (void)context;
    return instance_name.size == 4 && memcmp(instance_name.bytes, "host", 4) == 0 &&
        function_name.size == 4 && memcmp(function_name.bytes, "echo", 4) == 0 &&
        type != NULL && type->kind == TURBOWASM_COMPONENT_TYPE_FUNCTION &&
        type->as.function.param_count == 1 && type->as.function.has_result;
}
static turbowasm_status forward(void *context, turbowasm_host_call *host_call,
    turbowasm_component_name instance_name, turbowasm_component_name function_name,
    const turbowasm_component_type_graph *types, turbowasm_component_type_id function_type,
    const turbowasm_component_value *arguments, size_t count,
    turbowasm_component_value *out, turbowasm_trap *trap) {
    (void)host_call;
    check_true(can_bind(context, instance_name, function_name, types, function_type));
    check_equal(count, 1u);
    check_not_equal(arguments[0].as.string.origin, TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8);
    return turbowasm_component_exec_invoke_export(&provider_exec,
        (const uint8_t *)provider_export, (uint32_t)strlen(provider_export), arguments, count, out, trap);
}
spec("canonical string encodings") {
    before_each() { memset(&allocations, 0, sizeof(allocations)); }
    after_each() {
        allocations.fail_at = 0; turbowasm_component_call_destroy(&call);
        turbowasm_component_exec_destroy(&consumer_exec); turbowasm_component_binary_destroy(&consumer_binary);
        turbowasm_component_exec_destroy(&provider_exec); turbowasm_component_binary_destroy(&provider_binary);
        turbowasm_component_instance_destroy(&public_instance); turbowasm_component_destroy(&component);
        close_memory(); check_equal(allocations.live, 0u);
    }
    it("round trips all source and destination encodings on memory32 and memory64") {
        unsigned wide, origin, encoding, sample;
        turbowasm_component_value inputs[] = {value(NULL,0,0),value(ascii,sizeof(ascii),0),value(latin,sizeof(latin),0),value(mixed,sizeof(mixed),0)};
        for (wide = 0; wide < 2; ++wide) {
            setup(wide != 0);
            for (origin = 0; origin <= TURBOWASM_COMPONENT_STRING_ORIGIN_COMPACT_UTF16; ++origin)
                for (encoding = 0; encoding <= TURBOWASM_COMPONENT_STRING_LATIN1_UTF16; ++encoding)
                    for (sample = 0; sample < sizeof(inputs) / sizeof(*inputs); ++sample) {
                        if (origin == TURBOWASM_COMPONENT_STRING_ORIGIN_LATIN1 && sample == 3) continue;
                        memory.string_encoding = (turbowasm_component_string_encoding)encoding;
                        inputs[sample].as.string.origin = (turbowasm_component_string_origin)origin;
                        reset_guest(); roundtrip(inputs[sample]);
                    }
            close_memory();
        }
    }
    it("preserves the specified realloc sequence and prefix data across moving allocations") {
        turbowasm_component_value input = value(mixed,sizeof(mixed),TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8);
        uint64_t pointer, length; uint8_t bytes[sizeof(mixed_utf16)];
        setup(false); memory.string_encoding = TURBOWASM_COMPONENT_STRING_UTF16;
        check_equal(turbowasm_component_string_lower(&memory, &input.as.string, &pointer, &length), TURBOWASM_OK);
        check_equal(length, 5u); check_equal(guest.count, 2u);
        expect_request(0,0,2,20); expect_request(1,20,2,10);
        check_equal(turbowasm_instance_memory_read_bytes(instance.impl,0,pointer,0,bytes,sizeof(bytes)), TURBOWASM_OK);
        check_equal(memcmp(bytes,mixed_utf16,sizeof(bytes)),0);
        memory.string_encoding = TURBOWASM_COMPONENT_STRING_LATIN1_UTF16; reset_guest();
        roundtrip(input); check_equal(guest.count,3u);
        expect_request(0,0,2,10); expect_request(1,10,2,20); expect_request(2,20,2,10);
        memory.string_encoding = TURBOWASM_COMPONENT_STRING_UTF8; input.as.string.origin = TURBOWASM_COMPONENT_STRING_ORIGIN_UTF16;
        reset_guest(); roundtrip(input); check_equal(guest.count,3u);
        expect_request(0,0,1,5); expect_request(1,5,1,15); expect_request(2,15,1,10);
        input = value(latin,sizeof(latin),TURBOWASM_COMPONENT_STRING_ORIGIN_COMPACT_UTF16);
        memory.string_encoding = TURBOWASM_COMPONENT_STRING_LATIN1_UTF16;
        reset_guest(); roundtrip(input); check_equal(guest.count,2u);
        expect_request(0,0,2,8); expect_request(1,8,1,4);
    }
    it("uses the pointer-width tag and validates every Unicode scalar boundary") {
        static uint8_t boundaries[] = {0,0x7f,0xc2,0x80,0xdf,0xbf,0xe0,0xa0,0x80,
            0xed,0x9f,0xbf,0xee,0x80,0x80,0xef,0xbf,0xbf,0xf0,0x90,0x80,0x80,0xf4,0x8f,0xbf,0xbf};
        unsigned wide;
        for (wide=0;wide<2;++wide) {
            turbowasm_component_value input = value(boundaries,sizeof(boundaries),0), result = {0};
            uint64_t pointer,length;
            setup(wide != 0); memory.string_encoding = TURBOWASM_COMPONENT_STRING_LATIN1_UTF16;
            check_equal(turbowasm_component_string_lower(&memory,&input.as.string,&pointer,&length),TURBOWASM_OK);
            check_equal(length,(UINT64_C(1) << (wide ? 63u : 31u)) | 12u);
            check_equal(turbowasm_component_string_lift(&memory,pointer,length,&result.as.string),TURBOWASM_OK);
            result.kind = TURBOWASM_COMPONENT_TYPE_STRING;
            check_equal(result.as.string.origin,TURBOWASM_COMPONENT_STRING_ORIGIN_COMPACT_UTF16);
            check_equal(result.as.string.size,sizeof(boundaries)); check_equal(memcmp(result.as.string.data,boundaries,sizeof(boundaries)),0);
            check_equal(turbowasm_component_value_destroy(&result),TURBOWASM_OK); close_memory();
        }
    }
    it("rejects malformed Unicode without publishing a converted value") {
        static const uint8_t bad16[][4] = {{0,0xdc,0,0},{0,0xd8,0,0},{0,0xd8,0,0xd8}};
        static uint8_t bad8[][4] = {{0xc0,0x80},{0xed,0xa0,0x80},{0xf4,0x90,0x80,0x80},{0xe2,0x82},{0x80}};
        static const size_t sizes[] = {2,3,4,2,1};
        size_t i;
        setup(false); memory.string_encoding = TURBOWASM_COMPONENT_STRING_UTF16;
        for (i=0;i<sizeof(bad16)/sizeof(*bad16);++i) {
            turbowasm_component_owned_string out = {0};
            check_equal(turbowasm_instance_memory_write_bytes(instance.impl,0,HEAP_START,0,bad16[i],sizeof(*bad16)),TURBOWASM_OK);
            check_equal(turbowasm_component_string_lift(&memory,HEAP_START,i==0?1:2,&out),TURBOWASM_TRAPPED);
            check_null(out.data);
        }
        for (i=0;i<sizeof(sizes)/sizeof(*sizes);++i) {
            turbowasm_component_owned_string input = {bad8[i],sizes[i],0}; uint64_t pointer=UINT64_MAX,length=UINT64_MAX;
            check_equal(turbowasm_component_string_lower(&memory,&input,&pointer,&length),TURBOWASM_INVALID_ARGUMENT);
            check_equal(pointer,UINT64_MAX); check_equal(length,UINT64_MAX);
        }
        check_equal(guest.count,0u);
    }
    it("traps tagged-length overflow misalignment bounds and invalid allocator pointers") {
        unsigned wide, encoding;
        turbowasm_component_owned_string out = {0};
        turbowasm_component_value input = value(mixed,sizeof(mixed),0);
        for (wide=0;wide<2;++wide) {
            setup(wide != 0);
            for (encoding=1;encoding<=2;++encoding) {
                memory.string_encoding=(turbowasm_component_string_encoding)encoding;
                check_equal(turbowasm_component_string_lift(&memory,HEAP_START+1,0,&out),TURBOWASM_TRAPPED);
                check_equal(turbowasm_component_string_lift(&memory,0,UINT64_MAX,&out),TURBOWASM_TRAPPED);
                check_equal(turbowasm_component_string_lift(&memory,MEMORY_SIZE,1,&out),TURBOWASM_TRAPPED);
            }
            for (guest.bad_at=1;guest.bad_at<=3;++guest.bad_at) {
                uint64_t pointer=UINT64_MAX,length=UINT64_MAX;
                guest.count=0; guest.cursor=HEAP_START; guest.bad_pointer=HEAP_START+1;
                check_equal(turbowasm_component_string_lower(&memory,&input.as.string,&pointer,&length),TURBOWASM_TRAPPED);
                check_equal(pointer,UINT64_MAX); check_equal(length,UINT64_MAX);
            }
            close_memory();
        }
    }
    it("propagates each guest realloc failure without a partial output tuple") {
        turbowasm_component_value input=value(mixed,sizeof(mixed),0);
        size_t point; setup(false); memory.string_encoding=TURBOWASM_COMPONENT_STRING_LATIN1_UTF16;
        for (point=1;point<=3;++point) {
            uint64_t pointer=UINT64_MAX,length=UINT64_MAX; reset_guest(); guest.fail_at=point;
            check_equal(turbowasm_component_string_lower(&memory,&input.as.string,&pointer,&length),TURBOWASM_OUT_OF_MEMORY);
            check_equal(guest.count,point); check_equal(pointer,UINT64_MAX); check_equal(length,UINT64_MAX);
        }
        reset_guest(); roundtrip(input);
    }
    it("performs the canonical empty allocation and rejects oversized input before decoding") {
        turbowasm_component_value input=value(NULL,0,0);
        uint64_t pointer,length;
        setup(false); memory.string_encoding=TURBOWASM_COMPONENT_STRING_UTF16;
        check_equal(turbowasm_component_string_lower(&memory,&input.as.string,&pointer,&length),TURBOWASM_OK);
        check_equal(length,0u); check_equal(guest.count,1u); expect_request(0,0,2,0);
        input.as.string.data=ascii; input.as.string.size=(size_t)1u<<28u;
        reset_guest();
        check_equal(turbowasm_component_string_lower(&memory,&input.as.string,&pointer,&length),TURBOWASM_INVALID_ARGUMENT);
        check_equal(guest.count,0u);
    }
    it("frees both owned conversion buffers at every host allocation failure") {
        turbowasm_runtime_config config; turbowasm_runtime_scope scope;
        size_t point, baseline;
        setup(false); memory.string_encoding=TURBOWASM_COMPONENT_STRING_UTF16;
        check_equal(turbowasm_instance_memory_write_bytes(instance.impl,0,HEAP_START,0,mixed_utf16,sizeof(mixed_utf16)),TURBOWASM_OK);
        turbowasm_runtime_config_init(&config); config.allocator.allocate=allocate; config.allocator.deallocate=deallocate;
        baseline=allocations.live;
        for(point=1;point<=2;++point) {
            turbowasm_component_owned_string out={0}; turbowasm_status status;
            allocations.attempts=0; allocations.fail_at=point; scope=turbowasm_runtime_scope_enter(&config);
            status=turbowasm_component_string_lift(&memory,HEAP_START,5,&out);
            turbowasm_runtime_scope_leave(scope); allocations.fail_at=0;
            check_equal(status,TURBOWASM_OUT_OF_MEMORY); check_null(out.data); check_equal(allocations.live,baseline);
        }
    }
    it("loads both encoding options and converts nested host values") {
        const char *names[]={"utf16","compact"}; size_t i;
        turbowasm_component_host_value input={0},out;
        public_setup(); input.kind=TURBOWASM_COMPONENT_HOST_STRING; input.as.string.data=mixed; input.as.string.size=sizeof(mixed);
        for(i=0;i<sizeof(names)/sizeof(*names);++i) {
            out=invoke(names[i],&input); check_equal(out.kind,TURBOWASM_COMPONENT_HOST_STRING);
            check_equal(out.as.string.size,sizeof(mixed)); check_equal(memcmp(out.as.string.data,mixed,sizeof(mixed)),0);
            check_equal(turbowasm_component_host_value_destroy(&out),TURBOWASM_OK);
        }
        out=invoke("utf16-length",&input); check_equal(out.as.u32,5u);
        check_equal(turbowasm_component_host_value_destroy(&out),TURBOWASM_OK);
        out=invoke("compact-length",&input); check_equal(out.as.u32,UINT32_C(0x80000005));
        check_equal(turbowasm_component_host_value_destroy(&out),TURBOWASM_OK);
        {
            turbowasm_component_host_value list={0}; list.kind=TURBOWASM_COMPONENT_HOST_LIST; list.as.list.items=&input; list.as.list.count=1;
            out=invoke("utf16-list",&list); check_equal(out.as.list.count,1u);
            check_equal(out.as.list.items[0].as.string.size,sizeof(mixed));
            check_equal(memcmp(out.as.list.items[0].as.string.data,mixed,sizeof(mixed)),0);
            check_equal(turbowasm_component_host_value_destroy(&out),TURBOWASM_OK);
        }
    }
    it("forwards values between separately instantiated components through canon lower") {
        turbowasm_component_exec_imports imports = {0};
        const char *names[] = {"utf16", "compact"};
        turbowasm_component_value samples[] = {value(ascii,sizeof(ascii),0), value(latin,sizeof(latin),0), value(mixed,sizeof(mixed),0)};
        size_t source, target, sample;
        check_equal(turbowasm_component_binary_load(&provider_binary, component_encodings_bytes, sizeof(component_encodings_bytes)), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_init(&provider_exec, &provider_binary), TURBOWASM_OK);
        check_equal(turbowasm_component_binary_load(&consumer_binary, component_encoding_imports_bytes, sizeof(component_encoding_imports_bytes)), TURBOWASM_OK);
        imports.can_bind = can_bind; imports.invoke = forward;
        check_equal(turbowasm_component_exec_init_with_import_sets(&consumer_exec, &consumer_binary, &imports, 1), TURBOWASM_OK);
        for (source=0;source<2;++source) for (target=0;target<2;++target) for (sample=0;sample<3;++sample) {
            turbowasm_component_value out = {0}; turbowasm_trap trap;
            provider_export = names[target];
            check_equal(turbowasm_component_exec_invoke_export(&consumer_exec, (const uint8_t *)names[source],
                (uint32_t)strlen(names[source]), &samples[sample], 1, &out, &trap), TURBOWASM_OK);
            check_equal(out.as.string.size,samples[sample].as.string.size);
            check_equal(memcmp(out.as.string.data,samples[sample].as.string.data,out.as.string.size),0);
            check_equal(turbowasm_component_value_destroy(&out),TURBOWASM_OK);
        }
    }
    it("retains admitted encoded arguments across fuel yields and releases public owners safely") {
        turbowasm_component_host_value input={0}, out={0};
        turbowasm_execution_options options={0}; turbowasm_status status; unsigned resumes=0;
        uint8_t original[sizeof(mixed)]; memcpy(original,mixed,sizeof(original));
        public_setup(); input.kind=TURBOWASM_COMPONENT_HOST_STRING;
        input.as.string.data=original; input.as.string.size=sizeof(original);
        check_equal(turbowasm_component_call_create(&call,&public_instance,
            (turbowasm_name){(const uint8_t *)"utf16",5},&input,1),TURBOWASM_OK);
        memset(original,0,sizeof(original));
        turbowasm_component_instance_destroy(&public_instance); turbowasm_component_destroy(&component);
        options.has_fuel_limit=true; options.fuel=1;
        do {
            status=turbowasm_component_call_resume(&call,&options); check_true(++resumes<MAX_RESUMES);
        } while(status==TURBOWASM_YIELDED);
        check_equal(status,TURBOWASM_OK); check_true(resumes>1);
        check_equal(turbowasm_component_call_take_result(&call,&out),TURBOWASM_OK);
        check_equal(out.as.string.size,sizeof(mixed)); check_equal(memcmp(out.as.string.data,mixed,sizeof(mixed)),0);
        check_equal(turbowasm_component_host_value_destroy(&out),TURBOWASM_OK);
    }
}
