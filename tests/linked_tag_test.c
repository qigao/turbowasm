#include "../src/instance_internal.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static const uint8_t provider_bytes[] = {
    WASM_HEADER,

    /* type0: (i32) -> () */
    0x01, 0x05,
    0x01, 0x60, 0x01, 0x7f, 0x00,

    /* two distinct defined tags with the same signature */
    0x0d, 0x05,
    0x02,
    0x00, 0x00,
    0x00, 0x00,

    /* export tag0 as "a", tag1 as "b" */
    0x07, 0x09,
    0x02,
    0x01, 0x61, 0x04, 0x00,
    0x01, 0x62, 0x04, 0x01
};

static const uint8_t middle_bytes[] = {
    WASM_HEADER,

    /* type0: (i32) -> () */
    0x01, 0x05,
    0x01, 0x60, 0x01, 0x7f, 0x00,

    /* import p.a as tag0 */
    0x02, 0x08,
    0x01,
    0x01, 0x70,
    0x01, 0x61,
    0x04, 0x00, 0x00,

    /* re-export imported tag0 as "x" */
    0x07, 0x05,
    0x01,
    0x01, 0x78, 0x04, 0x00
};

static const uint8_t leaf_bytes[] = {
    WASM_HEADER,

    /* type0: (i32) -> () */
    0x01, 0x05,
    0x01, 0x60, 0x01, 0x7f, 0x00,

    /* import m.x as tag0 */
    0x02, 0x08,
    0x01,
    0x01, 0x6d,
    0x01, 0x78,
    0x04, 0x00, 0x00
};

static const uint8_t mismatch_bytes[] = {
    WASM_HEADER,

    /* type0: (i64) -> () */
    0x01, 0x05,
    0x01, 0x60, 0x01, 0x7e, 0x00,

    /* import p.a as tag0 */
    0x02, 0x08,
    0x01,
    0x01, 0x70,
    0x01, 0x61,
    0x04, 0x00, 0x00
};

static const uint8_t missing_export_bytes[] = {
    WASM_HEADER,

    /* type0: (i32) -> () */
    0x01, 0x05,
    0x01, 0x60, 0x01, 0x7f, 0x00,

    /* import p.z as tag0 */
    0x02, 0x08,
    0x01,
    0x01, 0x70,
    0x01, 0x7a,
    0x04, 0x00, 0x00
};

static turbowasm_tag_identity identity_at(
    const turbowasm_instance *instance,
    uint32_t tag_index) {
    turbowasm_tag_identity identity = {0};
    const turbowasm_instance_impl *impl =
        (const turbowasm_instance_impl *)instance->impl;

    assert(impl != NULL);
    assert(turbowasm_instance_tag_identity(
               impl, tag_index, &identity) == TURBOWASM_OK);
    assert(identity.owner != NULL);
    return identity;
}

static void test_defined_tag_identity_is_stable_and_distinct(void) {
    turbowasm_module module = {0};
    turbowasm_instance first = {0};
    turbowasm_instance second = {0};
    turbowasm_tag_identity first_a;
    turbowasm_tag_identity first_a_again;
    turbowasm_tag_identity first_b;
    turbowasm_tag_identity second_a;

    assert(turbowasm_module_load_borrowed(
               &module, provider_bytes, sizeof(provider_bytes)) ==
           TURBOWASM_OK);
    assert(turbowasm_instance_create(&first, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&second, &module) == TURBOWASM_OK);

    first_a = identity_at(&first, 0u);
    first_a_again = identity_at(&first, 0u);
    first_b = identity_at(&first, 1u);
    second_a = identity_at(&second, 0u);

    assert(first_a.owner == first.impl);
    assert(first_a.tag_index == 0u);
    assert(first_a_again.owner == first_a.owner);
    assert(first_a_again.tag_index == first_a.tag_index);

    /* Same signature does not collapse two different defined tags. */
    assert(first_b.owner == first.impl);
    assert(first_b.tag_index == 1u);
    assert(first_b.tag_index != first_a.tag_index);

    /* Instantiating the same module creates fresh runtime tag identity. */
    assert(second_a.owner == second.impl);
    assert(second_a.owner != first_a.owner);
    assert(second_a.tag_index == 0u);

    turbowasm_instance_destroy(&second);
    turbowasm_instance_destroy(&first);
    turbowasm_module_destroy(&module);
}

static void test_import_and_reexport_preserve_provider_identity(void) {
    static const uint8_t p_name_bytes[] = {(uint8_t)'p'};
    static const uint8_t m_name_bytes[] = {(uint8_t)'m'};
    const turbowasm_name p_name = {p_name_bytes, 1u};
    const turbowasm_name m_name = {m_name_bytes, 1u};
    turbowasm_module provider_module = {0};
    turbowasm_module middle_module = {0};
    turbowasm_module leaf_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance middle = {0};
    turbowasm_instance leaf = {0};
    turbowasm_linker provider_linker = {0};
    turbowasm_linker middle_linker = {0};
    turbowasm_tag_identity provider_identity;
    turbowasm_tag_identity middle_identity;
    turbowasm_tag_identity leaf_identity;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes, sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &middle_module,
               middle_bytes, sizeof(middle_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &leaf_module,
               leaf_bytes, sizeof(leaf_bytes)) == TURBOWASM_OK);

    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&provider_linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &provider_linker, p_name, &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &middle, &middle_module, &provider_linker) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&middle_linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &middle_linker, m_name, &middle) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &leaf, &leaf_module, &middle_linker) == TURBOWASM_OK);

    provider_identity = identity_at(&provider, 0u);
    middle_identity = identity_at(&middle, 0u);
    leaf_identity = identity_at(&leaf, 0u);

    assert(middle_identity.owner == provider_identity.owner);
    assert(middle_identity.tag_index == provider_identity.tag_index);
    assert(leaf_identity.owner == provider_identity.owner);
    assert(leaf_identity.tag_index == provider_identity.tag_index);

    /* Bindings are normalized, not merely chained through the middle. */
    {
        const turbowasm_instance_impl *leaf_impl =
            (const turbowasm_instance_impl *)leaf.impl;
        assert(leaf_impl->linked_tag_count == 1u);
        assert(leaf_impl->linked_tags[0].provider == provider.impl);
        assert(leaf_impl->linked_tags[0].tag_index == 0u);
    }

    turbowasm_linker_destroy(&middle_linker);
    turbowasm_linker_destroy(&provider_linker);
    turbowasm_instance_destroy(&leaf);
    turbowasm_instance_destroy(&middle);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&leaf_module);
    turbowasm_module_destroy(&middle_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_tag_signature_mismatch_is_rejected(void) {
    static const uint8_t p_name_bytes[] = {(uint8_t)'p'};
    const turbowasm_name p_name = {p_name_bytes, 1u};
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes, sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               mismatch_bytes, sizeof(mismatch_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker, p_name, &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer, &consumer_module, &linker) ==
           TURBOWASM_TYPE_MISMATCH);
    assert(consumer.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_unresolved_tag_import_is_link_error(void) {
    static const uint8_t p_name_bytes[] = {(uint8_t)'p'};
    const turbowasm_name p_name = {p_name_bytes, 1u};
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes, sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               missing_export_bytes,
               sizeof(missing_export_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker, p_name, &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer, &consumer_module, &linker) ==
           TURBOWASM_LINK_ERROR);
    assert(consumer.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_defined_tag_identity_is_stable_and_distinct();
    test_import_and_reexport_preserve_provider_identity();
    test_tag_signature_mismatch_is_rejected();
    test_unresolved_tag_import_is_link_error();
    return 0;
}
