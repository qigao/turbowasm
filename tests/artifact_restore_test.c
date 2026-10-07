#include <turbowasm/turbowasm.h>

#include "artifact.h"
#include "module_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t memory64_module_bytes[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    /* type0: () -> i64 */
    0x01,0x05,0x01,0x60,0x00,0x01,0x7e,
    /* function0 type0 */
    0x03,0x02,0x01,0x00,
    /* memory64 min=1 */
    0x05,0x03,0x01,0x04,0x01,
    /* function0: memory.size 0 */
    0x0a,0x06,0x01,0x04,0x00,0x3f,0x00,0x0b
};

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

static int64_t invoke_i64(turbowasm_instance *instance) {
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
    assert(result.kind==TURBOWASM_VALUE_I64);
    return result.as.i64;
}

static void write_u32le(uint8_t *p,uint32_t value) {
    p[0]=(uint8_t)value;
    p[1]=(uint8_t)(value>>8u);
    p[2]=(uint8_t)(value>>16u);
    p[3]=(uint8_t)(value>>24u);
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

static void test_memory64_fresh_and_restored_match(void) {
    turbowasm_module fresh={0};
    turbowasm_module restored={0};
    turbowasm_instance fresh_instance={0};
    turbowasm_instance restored_instance={0};
    uint8_t artifact[4096]={0};
    size_t artifact_size=0u;

    assert(turbowasm_module_load_borrowed(
               &fresh,
               memory64_module_bytes,
               sizeof(memory64_module_bytes))==TURBOWASM_OK);
    assert(turbowasm_module_artifact_write(
               &fresh,artifact,sizeof(artifact),&artifact_size)==
           TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed_from_artifact(
               &restored,
               memory64_module_bytes,
               sizeof(memory64_module_bytes),
               artifact,artifact_size)==TURBOWASM_OK);

    assert(turbowasm_instance_create(
               &fresh_instance,&fresh)==TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &restored_instance,&restored)==TURBOWASM_OK);
    assert(invoke_i64(&fresh_instance)==1);
    assert(invoke_i64(&restored_instance)==1);

    turbowasm_instance_destroy(&restored_instance);
    turbowasm_instance_destroy(&fresh_instance);
    turbowasm_module_destroy(&restored);
    turbowasm_module_destroy(&fresh);
}

static void test_integrity_rejects_single_byte_mutations(void) {
    turbowasm_module fresh={0};
    turbowasm_module restored={0};
    uint8_t artifact[4096]={0};
    uint8_t mutated[4096]={0};
    size_t artifact_size=0u;
    size_t index;

    assert(turbowasm_module_load_borrowed(
               &fresh,module_bytes,sizeof(module_bytes))==TURBOWASM_OK);
    assert(turbowasm_module_artifact_write(
               &fresh,artifact,sizeof(artifact),&artifact_size)==
           TURBOWASM_OK);
    assert(artifact_size<=sizeof(mutated));

    for(index=0u;index<artifact_size;++index) {
        memcpy(mutated,artifact,artifact_size);
        mutated[index]^=1u;
        assert(turbowasm_module_load_borrowed_from_artifact(
                   &restored,
                   module_bytes,sizeof(module_bytes),
                   mutated,artifact_size)!=TURBOWASM_OK);
        assert(restored.impl==NULL);
    }

    turbowasm_module_destroy(&fresh);
}

static void test_complete_artifact_without_integrity_remains_readable(void) {
    turbowasm_module fresh={0};
    turbowasm_module restored={0};
    turbowasm_instance instance={0};
    uint8_t artifact[4096]={0};
    size_t artifact_size=0u;

    assert(turbowasm_module_load_borrowed(
               &fresh,module_bytes,sizeof(module_bytes))==TURBOWASM_OK);
    assert(turbowasm_module_artifact_write(
               &fresh,artifact,sizeof(artifact),&artifact_size)==
           TURBOWASM_OK);
    assert(artifact_size>40u);

    /* The current schema permits COMPLETE metadata without the digest section. */
    write_u32le(artifact+12u,TURBOWASM_ARTIFACT_FLAG_COMPLETE_METADATA);
    write_u32le(artifact+64u,3u);
    artifact_size-=40u;

    assert(turbowasm_module_load_borrowed_from_artifact(
               &restored,
               module_bytes,sizeof(module_bytes),
               artifact,artifact_size)==TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance,&restored)==TURBOWASM_OK);
    assert(invoke_value(&instance)==42);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&restored);
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

static void test_bottom_reference_semantics_survive_restore(void) {
    static const uint8_t bytes[] = {
        0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
        0x01,0x05,0x01,0x60,0x00,0x01,0x73,
        0x03,0x02,0x01,0x00,
        0x0a,0x06,0x01,0x04,0x00,0xd0,0x73,0x0b
    };
    enum { ARTIFACT_CAPACITY = 4096 };
    uint8_t artifact[ARTIFACT_CAPACITY];
    size_t artifact_size = 0u, result_count = 0u;
    turbowasm_module source = {0}, restored = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_module_load_borrowed(&source, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_artifact_write(&source, artifact, sizeof(artifact), &artifact_size) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed_from_artifact(
        &restored, bytes, sizeof(bytes), artifact, artifact_size) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &restored) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(&instance, 0u, NULL, 0u,
        &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 1u && trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_VALUE_FUNCREF && result.as.funcref.is_null);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&restored);
    turbowasm_module_destroy(&source);
}

static void test_recursive_composite_types_survive_restore(void) {
    /* Recursive node/array group followed by a declared node subtype. */
    static const uint8_t bytes[] = {
        0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x1c,
        0x02,0x4e,0x02,0x50,0x00,0x5f,0x02,0x63,0x00,0x01,0x78,0x00,
        0x5e,0x63,0x00,0x01,0x50,0x01,0x00,0x5f,0x03,0x63,0x00,0x01,
        0x78,0x00,0x7e,0x00
    };
    enum { ARTIFACT_CAPACITY = 4096 };
    uint8_t artifact[ARTIFACT_CAPACITY];
    size_t size=0u;
    turbowasm_module source={0}, restored={0};
    const turbowasm_validation_context *fresh, *cached;
    assert(turbowasm_module_load_borrowed(&source,bytes,sizeof(bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_write(&source,artifact,sizeof(artifact),&size)==TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed_from_artifact(
        &restored,bytes,sizeof(bytes),artifact,size)==TURBOWASM_OK);
    fresh=&turbowasm_module_impl_get(&source)->validation;
    cached=&turbowasm_module_impl_get(&restored)->validation;
    assert(cached->type_count==3u);
    assert(fresh->requires_store && cached->requires_store);
    assert(turbowasm_validation_defined_type_equal(&fresh->types[0],&cached->types[0]));
    assert(turbowasm_validation_defined_type_equal(&fresh->types[1],&cached->types[1]));
    assert(turbowasm_validation_defined_type_matches(&cached->types[2],&fresh->types[0]));
    assert(!turbowasm_validation_defined_type_matches(&cached->types[0],&fresh->types[2]));
    assert(cached->types[0].fields[0].type.definition==&cached->types[0]);
    assert(cached->types[1].fields[0].type.definition==&cached->types[0]);
    assert(cached->types[0].fields[1].packed_bits==8u);
    assert(cached->types[2].super==&cached->types[0]);
    turbowasm_module_destroy(&source);
    assert(turbowasm_validation_defined_type_matches(&cached->types[2],&cached->types[0]));
    turbowasm_module_destroy(&restored);
}

static void test_table64_public_access_and_restore(void) {
    /* table i64 1 4294967296 externref */
    static const uint8_t bytes[]={
        0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
        0x04,0x09,0x01,0x6f,0x05,0x01,0x80,0x80,0x80,0x80,0x10
    };
    enum { ARTIFACT_CAPACITY=4096, TABLE_LIMIT=4, TOKEN=42 };
    uint8_t artifact[ARTIFACT_CAPACITY];
    size_t size=0u;
    uint64_t previous=0u,length=0u;
    turbowasm_module source={0},restored={0};
    turbowasm_instance instance={0};
    turbowasm_runtime_config config;
    turbowasm_table_desc desc;
    turbowasm_value value={0},read={0};
    turbowasm_runtime_config_init(&config);
    config.limits.max_table_elements=TABLE_LIMIT;
    assert(turbowasm_module_load_borrowed(&source,bytes,sizeof(bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_write(&source,artifact,sizeof(artifact),&size)==TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed_from_artifact_with_config(
        &restored,bytes,sizeof(bytes),artifact,size,&config)==TURBOWASM_OK);
    assert(turbowasm_module_table_at(&restored,0u,&desc));
    assert(desc.table64 && desc.minimum==1u && desc.has_maximum && desc.maximum==(UINT64_C(1)<<32u));
    assert(turbowasm_instance_create(&instance,&restored)==TURBOWASM_OK);
    value.kind=TURBOWASM_VALUE_EXTERNREF;
    value.as.externref.token=TOKEN;
    assert(turbowasm_instance_table_set64(&instance,0u,0u,value)==TURBOWASM_OK);
    assert(turbowasm_instance_table_get64(&instance,0u,UINT64_C(1)<<32u,&read)==TURBOWASM_TRAPPED);
    assert(turbowasm_instance_table_set64(&instance,0u,UINT64_MAX,value)==TURBOWASM_TRAPPED);
    assert(turbowasm_instance_table_get64(&instance,0u,0u,&read)==TURBOWASM_OK);
    assert(read.as.externref.token==TOKEN);
    assert(turbowasm_instance_table_grow64(&instance,0u,value,UINT64_MAX,&previous)==TURBOWASM_OK);
    assert(previous==UINT64_MAX);
    assert(turbowasm_instance_table_size64(&instance,0u,&length)==TURBOWASM_OK && length==1u);
    assert(turbowasm_instance_table_grow64(&instance,0u,value,1u,&previous)==TURBOWASM_OK && previous==1u);
    assert(turbowasm_instance_table_get64(&instance,0u,1u,&read)==TURBOWASM_OK && read.as.externref.token==TOKEN);
    assert(turbowasm_instance_table_grow64(&instance,0u,value,TABLE_LIMIT,&previous)==TURBOWASM_OK && previous==UINT64_MAX);
    assert(turbowasm_instance_table_size64(&instance,0u,&length)==TURBOWASM_OK && length==2u);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&restored);
    turbowasm_module_destroy(&source);
}

int main(void) {
    test_table64_public_access_and_restore();
    test_recursive_composite_types_survive_restore();
    test_bottom_reference_semantics_survive_restore();
    test_fresh_and_restored_behavior_match();
    test_memory64_fresh_and_restored_match();
    test_integrity_rejects_single_byte_mutations();
    test_complete_artifact_without_integrity_remains_readable();
    test_restore_rejects_wrong_source_and_truncation();
    test_restore_respects_runtime_limits();
    return 0;
}
