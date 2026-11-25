#include "../src/component_binary.h"
#include "../src/component_exec.h"
#include "../src/instance_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

static const uint8_t linked_instance_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,

    /* provider core module: export f() -> i32 = 7 */
    0x01,0x22,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x07,0x05,0x01,0x01,'f',0x00,0x00,
      0x0a,0x06,0x01,0x04,0x00,0x41,0x07,0x0b,

    /* consumer core module: import p.f and export it as call */
    0x01,0x22,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
      0x02,0x07,0x01,0x01,'p',0x01,'f',0x00,0x00,
      0x07,0x08,0x01,0x04,'c','a','l','l',0x00,0x00,

    /* instances: provider, then consumer with p -> instance0 */
    0x02,0x0b,0x02,
      0x00,0x00,0x00,
      0x00,0x01,0x01,0x01,'p',0x12,0x00,

    /* alias consumer.call as core func0 */
    0x06,0x0a,0x01,
      0x00,0x00,0x01,0x01,0x04,'c','a','l','l',

    /* component type0: () -> u32 */
    0x07,0x05,0x01,0x40,0x00,0x00,0x79,

    /* canon lift corefunc0, no opts, type0 */
    0x08,0x06,0x01,0x00,0x00,0x00,0x00,0x00,

    /* export call func0 */
    0x0b,0x0a,0x01,
      0x00,0x04,'c','a','l','l',
      0x01,0x00,0x00
};

static const uint8_t canonical_options_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,

    /*
     * allocator core module:
     *   memory export "mem"
     *   realloc(i32,i32,i32,i32)->i32 always returns 256
     */
    0x01,0x38,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x09,0x01,0x60,0x04,0x7f,0x7f,0x7f,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x05,0x03,0x01,0x00,0x01,
      0x07,0x11,0x02,
        0x03,'m','e','m',0x02,0x00,
        0x07,'r','e','a','l','l','o','c',0x00,0x00,
      0x0a,0x07,0x01,0x05,0x00,0x41,0x80,0x02,0x0b,

    /* callee core module: len(ptr,len)->len */
    0x01,0x26,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x07,0x07,0x01,0x03,'l','e','n',0x00,0x00,
      0x0a,0x06,0x01,0x04,0x00,0x20,0x01,0x0b,

    /* instantiate both modules without args */
    0x02,0x07,0x02,
      0x00,0x00,0x00,
      0x00,0x01,0x00,

    /*
     * aliases:
     *   core memory0 = instance0.mem
     *   core func0   = instance0.realloc
     *   core func1   = instance1.len
     */
    0x06,0x1d,0x03,
      0x00,0x02,0x01,0x00,0x03,'m','e','m',
      0x00,0x00,0x01,0x00,0x07,'r','e','a','l','l','o','c',
      0x00,0x00,0x01,0x01,0x03,'l','e','n',

    /* component type0: (s:string) -> u32 */
    0x07,0x08,0x01,
      0x40,0x01,0x01,'s',0x73,0x00,0x79,

    /*
     * canon lift corefunc1:
     * utf8, memory0, realloc corefunc0, type0
     */
    0x08,0x0b,0x01,
      0x00,0x00,0x01,
      0x03,
      0x00,
      0x03,0x00,
      0x04,0x00,
      0x00,

    /* export len func0 */
    0x0b,0x09,0x01,
      0x00,0x03,'l','e','n',
      0x01,0x00,0x00
};

static void test_linked_core_instance_argument(void) {
    turbowasm_component_binary component = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    const turbowasm_component_core_instance_def *consumer;

    assert(turbowasm_component_binary_load(
               &component,
               linked_instance_component,
               sizeof(linked_instance_component)) == TURBOWASM_OK);
    assert(component.core_instance_count == 2u);

    consumer = turbowasm_component_binary_core_instance_at(
        &component, 1u);
    assert(consumer != NULL);
    assert(consumer->module_index == 1u);
    assert(consumer->argument_count == 1u);
    assert(consumer->arguments[0].instance_index == 0u);
    assert(consumer->arguments[0].name.size == 1u);
    assert(consumer->arguments[0].name.bytes[0] == (uint8_t)'p');

    assert(turbowasm_component_exec_init(
               &exec, &component) == TURBOWASM_OK);
    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"call", 4u,
               NULL, 0u,
               &result, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == 7u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_component_binary_destroy(&component);
}

static void test_memory_and_realloc_options_cross_instance(void) {
    static const uint8_t hello[] = {'h','e','l','l','o'};
    turbowasm_component_binary component = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_component_value argument = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    const turbowasm_component_canon_lift *lift;
    const turbowasm_component_core_memory_alias *memory_alias;
    uint8_t bytes[sizeof(hello)] = {0};
    turbowasm_instance_impl *memory_instance;

    assert(turbowasm_component_binary_load(
               &component,
               canonical_options_component,
               sizeof(canonical_options_component)) == TURBOWASM_OK);

    assert(component.core_memory_alias_count == 1u);
    assert(component.core_function_alias_count == 2u);
    assert(component.canon_lift_count == 1u);

    memory_alias = turbowasm_component_binary_core_memory_alias_at(
        &component, 0u);
    assert(memory_alias != NULL);
    assert(memory_alias->core_memory_index == 0u);
    assert(memory_alias->instance_index == 0u);

    lift = turbowasm_component_binary_canon_lift_at(
        &component, 0u);
    assert(lift != NULL);
    assert(lift->core_function_index == 1u);
    assert(lift->has_memory);
    assert(lift->memory_index == 0u);
    assert(lift->has_realloc);
    assert(lift->realloc_function_index == 0u);
    assert(lift->string_encoding == TURBOWASM_COMPONENT_STRING_UTF8);

    assert(turbowasm_component_exec_init(
               &exec, &component) == TURBOWASM_OK);

    argument.kind = TURBOWASM_COMPONENT_TYPE_STRING;
    argument.as.string.data = (uint8_t *)hello;
    argument.as.string.size = sizeof(hello);

    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"len", 3u,
               &argument, 1u,
               &result, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == sizeof(hello));

    /* Proves canonical memory belongs to allocator instance, not callee. */
    memory_instance =
        (turbowasm_instance_impl *)exec.core_instances[0].impl;
    assert(memory_instance != NULL);
    assert(turbowasm_instance_memory_read_bytes(
               memory_instance, 0u, 256u, 0u,
               bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(memcmp(bytes, hello, sizeof(hello)) == 0);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_component_binary_destroy(&component);
}

static void test_realloc_signature_mismatch_fails_closed(void) {
    uint8_t bytes[sizeof(canonical_options_component)];
    turbowasm_component_binary component = {0};
    turbowasm_component_exec exec = {0};

    memcpy(bytes, canonical_options_component, sizeof(bytes));

    /*
     * Section 8 payload starts at byte 158. Change realloc option's core func
     * index from 0 (realloc) to 1 (len), whose signature is incompatible.
     */
    bytes[167] = 1u;

    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_component_exec_init(
               &exec, &component) == TURBOWASM_TYPE_MISMATCH);
    assert(!exec.initialized);

    turbowasm_component_binary_destroy(&component);
}

int main(void) {
    test_linked_core_instance_argument();
    test_memory_and_realloc_options_cross_instance();
    test_realloc_signature_mismatch_fails_closed();
    return 0;
}
