#include "../src/component_binary.h"
#include "../src/component_exec.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

static const uint8_t component_instance_alias_component[] = {
    /* Component preamble */
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,

    /* embedded Core module exporting answer() -> i32 = 42 */
    0x01,0x27,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x07,0x0a,0x01,
        0x06,'a','n','s','w','e','r',0x00,0x00,
      0x0a,0x06,0x01,0x04,0x00,0x41,0x2a,0x0b,

    /* core instance0 = instantiate module0 */
    0x02,0x04,0x01,0x00,0x00,0x00,

    /* core func0 = alias core export instance0 "answer" */
    0x06,0x0c,0x01,
      0x00,0x00,0x01,0x00,
      0x06,'a','n','s','w','e','r',

    /* type0 = func() -> u32 */
    0x07,0x05,0x01,0x40,0x00,0x00,0x79,

    /* component func0 = canon lift corefunc0, no opts, type0 */
    0x08,0x06,0x01,0x00,0x00,0x00,0x00,0x00,

    /*
     * component instance0 = inline exports:
     *   "inner" -> component func0
     */
    0x05,0x0c,0x01,
      0x01,0x01,
        0x00,0x05,'i','n','n','e','r',
        0x01,0x00,

    /* component func1 = alias export instance0 "inner" (func) */
    0x06,0x0a,0x01,
      0x01,0x00,0x00,
      0x05,'i','n','n','e','r',

    /* export "answer" -> component func1 */
    0x0b,0x0c,0x01,
      0x00,0x06,'a','n','s','w','e','r',
      0x01,0x01,0x00
};

static void test_inline_component_instance_alias_executes(void) {
    turbowasm_component_binary component = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    const turbowasm_component_instance_def *instance;
    const turbowasm_component_function_alias *alias;

    assert(turbowasm_component_binary_load(
               &component,
               component_instance_alias_component,
               sizeof(component_instance_alias_component)) ==
           TURBOWASM_OK);

    assert(component.canon_lift_count == 1u);
    assert(component.component_instance_count == 1u);
    assert(component.component_function_alias_count == 1u);
    assert(component.component_function_count == 2u);

    instance =
        turbowasm_component_binary_component_instance_at(
            &component, 0u);
    assert(instance != NULL);
    assert(instance->export_count == 1u);
    assert(instance->exports[0].kind ==
           TURBOWASM_COMPONENT_EXTERN_FUNCTION);
    assert(instance->exports[0].item_index == 0u);

    alias =
        turbowasm_component_binary_component_function_alias_at(
            &component, 0u);
    assert(alias != NULL);
    assert(alias->component_function_index == 1u);
    assert(alias->instance_index == 0u);

    assert(turbowasm_component_exec_init(
               &exec, &component) == TURBOWASM_OK);
    assert(exec.adapter_count == 1u);
    assert(exec.function_count == 2u);
    assert(exec.function_adapter_indices[0] == 0u);
    assert(exec.function_adapter_indices[1] == 0u);

    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"answer", 6u,
               NULL, 0u,
               &result, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == 42u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_component_binary_destroy(&component);
}

static void test_nested_component_instantiation_remains_deferred(void) {
    static const uint8_t bytes[] = {
        0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,
        /* section5: vec1, instanceexpr instantiate component0 */
        0x05,0x02,0x01,0x00
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) ==
           TURBOWASM_UNSUPPORTED);
}

static void test_inline_instance_cannot_export_future_function(void) {
    static const uint8_t bytes[] = {
        0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,
        /* no functions exist; inline instance exports func0 */
        0x05,0x08,0x01,
          0x01,0x01,
            0x00,0x01,'f',
            0x01,0x00
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) ==
           TURBOWASM_MALFORMED_MODULE);
}

int main(void) {
    test_inline_component_instance_alias_executes();
    test_nested_component_instantiation_remains_deferred();
    test_inline_instance_cannot_export_future_function();
    return 0;
}
