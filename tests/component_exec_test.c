#include "../src/component_binary.h"
#include "../src/component_exec.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

static const uint8_t executable_component[] = {
    /* Component preamble */
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,

    /* section 1: embedded Core module exporting answer() -> i32 = 42 */
    0x01,0x27,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x07,0x0a,0x01,0x06,'a','n','s','w','e','r',0x00,0x00,
      0x0a,0x06,0x01,0x04,0x00,0x41,0x2a,0x0b,

    /* section 2: core instance 0 = instantiate core module 0, no args */
    0x02,0x04,0x01,0x00,0x00,0x00,

    /* section 6: core func 0 = alias core export instance0 "answer" */
    0x06,0x0c,0x01,
      0x00,0x00, /* sort = core func */
      0x01,      /* core export alias */
      0x00,      /* core instance index */
      0x06,'a','n','s','w','e','r',

    /* section 7: type0 = func() -> u32 */
    0x07,0x05,0x01,0x40,0x00,0x00,0x79,

    /* section 8: component func0 = canon lift corefunc0, no opts, type0 */
    0x08,0x06,0x01,0x00,0x00,0x00,0x00,0x00,

    /* section 11: export "answer" func0 */
    0x0b,0x0c,0x01,
      0x00,0x06,'a','n','s','w','e','r',
      0x01,0x00,0x00
};

static void test_decode_and_execute(void) {
    turbowasm_component_binary component = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    const turbowasm_component_core_instance_def *instance_def;
    const turbowasm_component_core_function_alias *alias;
    const turbowasm_component_canon_lift *lift;

    assert(turbowasm_component_binary_load(
               &component,
               executable_component,
               sizeof(executable_component)) == TURBOWASM_OK);

    assert(component.core_module_count == 1u);
    assert(component.core_instance_count == 1u);
    assert(component.core_function_alias_count == 1u);
    assert(component.canon_lift_count == 1u);
    assert(component.type_graph.count == 1u);
    assert(component.export_count == 1u);

    instance_def = turbowasm_component_binary_core_instance_at(
        &component, 0u);
    assert(instance_def != NULL);
    assert(instance_def->module_index == 0u);

    alias = turbowasm_component_binary_core_function_alias_at(
        &component, 0u);
    assert(alias != NULL);
    assert(alias->core_function_index == 0u);
    assert(alias->instance_index == 0u);
    assert(alias->name.size == 6u);
    assert(memcmp(alias->name.bytes, "answer", 6u) == 0);

    lift = turbowasm_component_binary_canon_lift_at(
        &component, 0u);
    assert(lift != NULL);
    assert(lift->component_function_index == 0u);
    assert(lift->core_function_index == 0u);
    assert(lift->type_index == 0u);

    assert(turbowasm_component_exec_init(
               &exec, &component) == TURBOWASM_OK);

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

static void test_unresolved_core_alias_fails_at_composition(void) {
    uint8_t bytes[sizeof(executable_component)];
    turbowasm_component_binary component = {0};
    turbowasm_component_exec exec = {0};

    memcpy(bytes, executable_component, sizeof(bytes));
    /* Alias name occurrence begins at byte 63; keep valid UTF-8/length. */
    bytes[63] = (uint8_t)'x';

    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_component_exec_init(
               &exec, &component) == TURBOWASM_MALFORMED_MODULE);
    assert(!exec.initialized);

    turbowasm_component_binary_destroy(&component);
}

static void test_non_c5c1_forms_fail_closed(void) {
    uint8_t bytes[sizeof(executable_component)];
    turbowasm_component_binary component = {0};

    memcpy(bytes, executable_component, sizeof(bytes));
    /* section2 argument-count byte without the required arg payload */
    bytes[54] = 1u;
    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) ==
           TURBOWASM_MALFORMED_MODULE);

    memcpy(bytes, executable_component, sizeof(bytes));
    /* section8 canon opts vector count */
    bytes[82] = 1u;
    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) == TURBOWASM_MALFORMED_MODULE);
}

int main(void) {
    test_decode_and_execute();
    test_unresolved_core_alias_fails_at_composition();
    test_non_c5c1_forms_fail_closed();
    return 0;
}
