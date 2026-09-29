#include <turbowasm/turbowasm.h>

#include "artifact.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t module_bytes[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,

    /* type0: () -> i32 */
    0x01,0x05,0x01,0x60,0x00,0x01,0x7f,

    /* function0 type0 */
    0x03,0x02,0x01,0x00,

    /* memory0 min=1 */
    0x05,0x03,0x01,0x00,0x01,

    /*
     * function0:
     *   i32.const 0
     *   i32.load8_u
     */
    0x0a,0x09,0x01,0x07,
    0x00,0x41,0x00,0x2d,0x00,0x00,0x0b,

    /* active data0: memory0[0] = 42 */
    0x0b,0x07,0x01,
    0x00,0x41,0x00,0x0b,
    0x01,0x2a
};

static int32_t invoke_value(turbowasm_instance *instance) {
    turbowasm_value result={0};
    size_t result_count=0u;
    turbowasm_trap trap=TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance,0u,
               NULL,0u,
               &result,1u,
               &result_count,&trap)==TURBOWASM_OK);
    assert(trap==TURBOWASM_TRAP_NONE);
    assert(result_count==1u);
    assert(result.kind==TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void test_fresh_and_restored_behavior_match(void) {
    turbowasm_module fresh={0};
    turbowasm_module restored={0};
    turbowasm_instance fresh_instance={0};
    turbowasm_instance restored_instance={0};
    turbowasm_module_summary fresh_summary={0};
    turbowasm_module_summary restored_summary={0};
    turbowasm_function_signature signature={0};
    turbowasm_memory_desc memory={0};
    uint8_t artifact[4096]={0};
    size_t artifact_size=0u;

    assert(turbowasm_module_load_borrowed(
               &fresh,module_bytes,sizeof(module_bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_write(
               &fresh,artifact,sizeof(artifact),&artifact_size)==TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed_from_artifact(
               &restored,
               module_bytes,sizeof(module_bytes),
               artifact,artifact_size)==TURBOWASM_OK);

    assert(turbowasm_module_summary_get(&fresh,&fresh_summary));
    assert(turbowasm_module_summary_get(&restored,&restored_summary));
    assert(fresh_summary.type_count==restored_summary.type_count);
    assert(fresh_summary.function_count==restored_summary.function_count);
    assert(fresh_summary.memory_count==restored_summary.memory_count);
    assert(fresh_summary.data_segment_count==
           restored_summary.data_segment_count);

    assert(turbowasm_module_function_signature_get(
               &restored,0u,&signature));
    assert(signature.param_count==0u);
    assert(signature.result_count==1u);
    assert(turbowasm_module_memory_at(&restored,0u,&memory));
    assert(!memory.memory64);
    assert(memory.minimum64==1u);

    assert(turbowasm_instance_create(
               &fresh_instance,&fresh)==TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &restored_instance,&restored)==TURBOWASM_OK);
    assert(invoke_value(&fresh_instance)==42);
    assert(invoke_value(&restored_instance)==42);

    turbowasm_instance_destroy(&restored_instance);
    turbowasm_instance_destroy(&fresh_instance);
    turbowasm_module_destroy(&restored);
    turbowasm_module_destroy(&fresh);
}

static void test_restore_rejects_wrong_source_and_truncation(void) {
    turbowasm_module fresh={0};
    turbowasm_module restored={0};
    uint8_t artifact[4096]={0};
    uint8_t wrong_source[sizeof(module_bytes)];
    size_t artifact_size=0u;

    assert(turbowasm_module_load_borrowed(
               &fresh,module_bytes,sizeof(module_bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_write(
               &fresh,artifact,sizeof(artifact),&artifact_size)==TURBOWASM_OK);

    memcpy(wrong_source,module_bytes,sizeof(wrong_source));
    wrong_source[sizeof(wrong_source)-1u]^=1u;
    assert(turbowasm_module_load_borrowed_from_artifact(
               &restored,
               wrong_source,sizeof(wrong_source),
               artifact,artifact_size)==TURBOWASM_TYPE_MISMATCH);
    assert(restored.impl==NULL);

    assert(artifact_size>1u);
    assert(turbowasm_module_load_borrowed_from_artifact(
               &restored,
               module_bytes,sizeof(module_bytes),
               artifact,artifact_size-1u)!=TURBOWASM_OK);
    assert(restored.impl==NULL);

    turbowasm_module_destroy(&fresh);
}

static void test_restore_respects_runtime_limits(void) {
    turbowasm_module fresh={0};
    turbowasm_module restored={0};
    turbowasm_runtime_config config;
    uint8_t artifact[4096]={0};
    size_t artifact_size=0u;

    assert(turbowasm_module_load_borrowed(
               &fresh,module_bytes,sizeof(module_bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_write(
               &fresh,artifact,sizeof(artifact),&artifact_size)==TURBOWASM_OK);

    turbowasm_runtime_config_init(&config);
    config.limits.max_allocation_bytes=1u;
    assert(turbowasm_module_load_borrowed_from_artifact_with_config(
               &restored,
               module_bytes,sizeof(module_bytes),
               artifact,artifact_size,
               &config)==TURBOWASM_OUT_OF_MEMORY);
    assert(restored.impl==NULL);

    turbowasm_module_destroy(&fresh);
}

int main(void) {
    test_fresh_and_restored_behavior_match();
    test_restore_rejects_wrong_source_and_truncation();
    test_restore_respects_runtime_limits();
    return 0;
}
