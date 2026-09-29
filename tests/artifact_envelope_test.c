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
    assert(info.flags==0u);
    assert(info.feature_fingerprint==
           turbowasm_artifact_current_feature_fingerprint());
    assert(info.source_size==sizeof(module_bytes));
    assert(info.section_count==1u);
    assert(info.summary.memory_count==1u);
    assert(info.summary.export_count==1u);

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
    test_capacity_contract();
    return 0;
}
