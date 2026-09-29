#include <turbowasm/turbowasm.h>

#include "artifact.h"
#include "sha256.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t module_bytes[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    /* memory64 min=1 max=2 */
    0x05,0x04,0x01,0x05,0x01,0x02,
    /* export memory as "mem" */
    0x07,0x07,0x01,0x03,0x6d,0x65,0x6d,0x02,0x00
};

static const uint8_t metadata_module_bytes[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,

    /* type0: (i32) -> i32 */
    0x01,0x06,0x01,0x60,0x01,0x7f,0x01,0x7f,

    /* import env.inc func type0 */
    0x02,0x0b,0x01,
    0x03,0x65,0x6e,0x76,
    0x03,0x69,0x6e,0x63,
    0x00,0x00,

    /* one defined function type0 */
    0x03,0x02,0x01,0x00,

    /* memory64 min=1 max=2 */
    0x05,0x04,0x01,0x05,0x01,0x02,

    /* export run=function1 and mem=memory0 */
    0x07,0x0d,0x02,
    0x03,0x72,0x75,0x6e,0x00,0x01,
    0x03,0x6d,0x65,0x6d,0x02,0x00,

    /* body: block(result i32) { local.get 0; call 0 } */
    0x0a,0x0b,0x01,0x09,
    0x00,
    0x02,0x7f,
    0x20,0x00,
    0x10,0x00,
    0x0b,
    0x0b
};

static const uint8_t state_module_bytes[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,

    /* type0: () -> () */
    0x01,0x04,0x01,0x60,0x00,0x00,

    /* one function type0 */
    0x03,0x02,0x01,0x00,

    /* table0 funcref min=1 */
    0x04,0x04,0x01,0x70,0x00,0x01,

    /* memory0 min=1 */
    0x05,0x03,0x01,0x00,0x01,

    /* immutable i32 global = 7 */
    0x06,0x06,0x01,0x7f,0x00,0x41,0x07,0x0b,

    /* active element segment: table0[0] = func0 */
    0x09,0x07,0x01,0x00,0x41,0x00,0x0b,0x01,0x00,

    /* empty function body */
    0x0a,0x04,0x01,0x02,0x00,0x0b,

    /* active data segment: memory0[0] = 'x' */
    0x0b,0x07,0x01,0x00,0x41,0x00,0x0b,0x01,0x78
};

static const uint8_t tag_module_bytes[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,

    /* type0: (i32, i64) -> (); type1: () -> () */
    0x01,0x09,0x02,
    0x60,0x02,0x7f,0x7e,0x00,
    0x60,0x00,0x00,

    /* one function of type1 */
    0x03,0x02,0x01,0x01,

    /* tag0: type0 */
    0x0d,0x03,0x01,0x00,0x00,

    /* i32.const 1; i64.const 2; throw tag0 */
    0x0a,0x0a,0x01,0x08,0x00,
    0x41,0x01,0x42,0x02,0x08,0x00,0x0b
};

static void test_sha256_known_vector(void) {
    static const uint8_t expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    static const uint8_t input[] = {'a','b','c'};
    uint8_t digest[32] = {0};

    turbowasm_sha256(input,sizeof(input),digest);
    assert(memcmp(digest,expected,sizeof(expected))==0);
}

static void test_envelope_round_trip(void) {
    turbowasm_module module={0};
    turbowasm_artifact_info info={0};
    uint8_t artifact[512]={0};
    uint8_t second[512]={0};
    size_t measured=0u;
    size_t written=0u;
    size_t written2=0u;

    assert(turbowasm_module_load_borrowed(
               &module,module_bytes,sizeof(module_bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_measure(&module,&measured)==TURBOWASM_OK);
    assert(measured <= sizeof(artifact));

    assert(turbowasm_artifact_write(
               &module,artifact,sizeof(artifact),&written)==TURBOWASM_OK);
    assert(written==measured);
    assert(turbowasm_artifact_write(
               &module,second,sizeof(second),&written2)==TURBOWASM_OK);
    assert(written2==written);
    assert(memcmp(artifact,second,written)==0);

    assert(turbowasm_artifact_inspect(
               artifact,written,module_bytes,sizeof(module_bytes),&info)==
           TURBOWASM_OK);
    assert(info.schema_version==TURBOWASM_ARTIFACT_SCHEMA_VERSION);
    assert(info.flags==
           (TURBOWASM_ARTIFACT_FLAG_COMPLETE_METADATA |
            TURBOWASM_ARTIFACT_FLAG_INTEGRITY_SHA256));
    assert(info.feature_fingerprint==
           turbowasm_artifact_current_feature_fingerprint());
    assert(info.source_size==sizeof(module_bytes));
    assert(info.section_count==4u);
    assert(info.summary.memory_count==1u);
    assert(info.summary.export_count==1u);
    assert(info.has_core_metadata);
    assert(info.metadata_type_count==0u);
    assert(info.metadata_function_count==0u);
    assert(info.metadata_import_count==0u);
    assert(info.metadata_export_count==1u);
    assert(info.metadata_memory_count==1u);
    assert(info.has_state_metadata);
    assert(info.metadata_global_count==0u);
    assert(info.metadata_table_count==0u);
    assert(info.metadata_tag_count==0u);
    assert(info.metadata_data_segment_count==0u);
    assert(info.metadata_element_segment_count==0u);
    assert(info.metadata_declared_ref_count==0u);
    assert(info.has_integrity_sha256);

    turbowasm_module_destroy(&module);
}

static void test_source_identity_and_corruption(void) {
    turbowasm_module module={0};
    turbowasm_artifact_info info={0};
    uint8_t artifact[512]={0};
    uint8_t corrupt[512]={0};
    uint8_t source[sizeof(module_bytes)];
    size_t written=0u;
    size_t i;

    assert(turbowasm_module_load_borrowed(
               &module,module_bytes,sizeof(module_bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_write(
               &module,artifact,sizeof(artifact),&written)==TURBOWASM_OK);

    memcpy(source,module_bytes,sizeof(source));
    source[sizeof(source)-1u]^=1u;
    assert(turbowasm_artifact_inspect(
               artifact,written,source,sizeof(source),&info)==
           TURBOWASM_TYPE_MISMATCH);

    assert(turbowasm_artifact_inspect(
               artifact,written,module_bytes,sizeof(module_bytes)-1u,&info)==
           TURBOWASM_UNSUPPORTED);

    memcpy(corrupt,artifact,written);
    corrupt[0]^=1u;
    assert(turbowasm_artifact_inspect(
               corrupt,written,module_bytes,sizeof(module_bytes),&info)==
           TURBOWASM_MALFORMED_MODULE);

    memcpy(corrupt,artifact,written);
    corrupt[16]^=1u;
    assert(turbowasm_artifact_inspect(
               corrupt,written,module_bytes,sizeof(module_bytes),&info)==
           TURBOWASM_UNSUPPORTED);

    for(i=0u;i<written;++i) {
        assert(turbowasm_artifact_inspect(
                   artifact,i,module_bytes,sizeof(module_bytes),&info)!=
               TURBOWASM_OK);
    }

    turbowasm_module_destroy(&module);
}

static void test_core_metadata_round_trip(void) {
    turbowasm_module module={0};
    turbowasm_artifact_info info={0};
    uint8_t artifact[2048]={0};
    uint8_t corrupt[2048]={0};
    size_t written=0u;

    assert(turbowasm_module_load_borrowed(
               &module,
               metadata_module_bytes,
               sizeof(metadata_module_bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_write(
               &module,artifact,sizeof(artifact),&written)==TURBOWASM_OK);
    assert(turbowasm_artifact_inspect(
               artifact,written,
               metadata_module_bytes,sizeof(metadata_module_bytes),
               &info)==TURBOWASM_OK);

    assert(info.section_count==4u);
    assert(info.flags==
           (TURBOWASM_ARTIFACT_FLAG_COMPLETE_METADATA |
            TURBOWASM_ARTIFACT_FLAG_INTEGRITY_SHA256));
    assert(info.has_core_metadata);
    assert(info.has_state_metadata);
    assert(info.metadata_type_count==1u);
    assert(info.metadata_function_count==2u);
    assert(info.metadata_import_count==1u);
    assert(info.metadata_export_count==2u);
    assert(info.metadata_memory_count==1u);
    assert(info.has_integrity_sha256);

    /*
     * v1 writes summary first. Core metadata payload begins after:
     * header(72) + summary section header(8) + summary(88) +
     * core metadata section header(8) = 176.
     * Corrupt its type count so retained metadata no longer matches summary.
     */
    assert(written>176u);
    memcpy(corrupt,artifact,written);
    corrupt[176u]=2u;
    assert(turbowasm_artifact_inspect(
               corrupt,written,
               metadata_module_bytes,sizeof(metadata_module_bytes),
               &info)==TURBOWASM_MALFORMED_MODULE);

    turbowasm_module_destroy(&module);
}

static void test_state_metadata_round_trip(void) {
    turbowasm_module module={0};
    turbowasm_artifact_info info={0};
    uint8_t artifact[2048]={0};
    size_t written=0u;

    assert(turbowasm_module_load_borrowed(
               &module,
               state_module_bytes,
               sizeof(state_module_bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_write(
               &module,artifact,sizeof(artifact),&written)==TURBOWASM_OK);
    assert(turbowasm_artifact_inspect(
               artifact,written,
               state_module_bytes,sizeof(state_module_bytes),
               &info)==TURBOWASM_OK);

    assert(info.flags==
           (TURBOWASM_ARTIFACT_FLAG_COMPLETE_METADATA |
            TURBOWASM_ARTIFACT_FLAG_INTEGRITY_SHA256));
    assert(info.section_count==4u);
    assert(info.has_core_metadata);
    assert(info.has_state_metadata);
    assert(info.metadata_function_count==1u);
    assert(info.metadata_memory_count==1u);
    assert(info.metadata_global_count==1u);
    assert(info.metadata_table_count==1u);
    assert(info.metadata_tag_count==0u);
    assert(info.metadata_data_segment_count==1u);
    assert(info.metadata_element_segment_count==1u);
    assert(info.metadata_declared_ref_count==1u);
    assert(info.has_integrity_sha256);

    turbowasm_module_destroy(&module);
}

static void test_tag_state_metadata_round_trip(void) {
    turbowasm_module module={0};
    turbowasm_artifact_info info={0};
    uint8_t artifact[2048]={0};
    size_t written=0u;

    assert(turbowasm_module_load_borrowed(
               &module,
               tag_module_bytes,
               sizeof(tag_module_bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_write(
               &module,artifact,sizeof(artifact),&written)==TURBOWASM_OK);
    assert(turbowasm_artifact_inspect(
               artifact,written,
               tag_module_bytes,sizeof(tag_module_bytes),
               &info)==TURBOWASM_OK);

    assert(info.has_state_metadata);
    assert(info.metadata_tag_count==1u);
    assert(info.metadata_function_count==1u);
    assert(info.metadata_type_count==2u);

    turbowasm_module_destroy(&module);
}

static void test_capacity_contract(void) {
    turbowasm_module module={0};
    uint8_t byte=0u;
    size_t required=0u;

    assert(turbowasm_module_load_borrowed(
               &module,module_bytes,sizeof(module_bytes))==TURBOWASM_OK);
    assert(turbowasm_artifact_measure(&module,&required)==TURBOWASM_OK);
    assert(required>1u);
    assert(turbowasm_artifact_write(
               &module,&byte,1u,&required)==TURBOWASM_OUT_OF_MEMORY);
    assert(required>1u);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_sha256_known_vector();
    test_envelope_round_trip();
    test_source_identity_and_corruption();
    test_core_metadata_round_trip();
    test_state_metadata_round_trip();
    test_tag_state_metadata_round_trip();
    test_capacity_contract();
    return 0;
}
