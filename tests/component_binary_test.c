#include "../src/component_binary.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

#define COMPONENT_HEADER     0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00

static void test_empty_component(void) {
    static const uint8_t bytes[] = { COMPONENT_HEADER };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(component.bytes == bytes);
    assert(component.size == sizeof(bytes));
    assert(component.section_count == 0u);
    assert(component.core_module_count == 0u);
    turbowasm_component_binary_destroy(&component);
}

static void test_preamble_rejection(void) {
    static const uint8_t core_module[] = {
        0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00
    };
    static const uint8_t bad_version[] = {
        0x00,0x61,0x73,0x6d,0x0c,0x00,0x01,0x00
    };
    static const uint8_t bad_layer[] = {
        0x00,0x61,0x73,0x6d,0x0d,0x00,0x00,0x00
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component,
               core_module,
               sizeof(core_module)) == TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,
               bad_version,
               sizeof(bad_version)) == TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,
               bad_layer,
               sizeof(bad_layer)) == TURBOWASM_MALFORMED_MODULE);
}

static void test_custom_section_framing(void) {
    static const uint8_t valid[] = {
        COMPONENT_HEADER,
        0x00,0x03,0x02,'h','i'
    };
    static const uint8_t bad_name_length[] = {
        COMPONENT_HEADER,
        0x00,0x03,0x05,'a','b'
    };
    static const uint8_t bad_utf8[] = {
        COMPONENT_HEADER,
        0x00,0x03,0x02,0xff,0xfe
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component,valid,sizeof(valid)) == TURBOWASM_OK);
    assert(component.section_count == 1u);
    assert(turbowasm_component_binary_section_at(
               &component,0u)->id == 0u);
    turbowasm_component_binary_destroy(&component);

    assert(turbowasm_component_binary_load(
               &component,
               bad_name_length,
               sizeof(bad_name_length)) == TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,
               bad_utf8,
               sizeof(bad_utf8)) == TURBOWASM_MALFORMED_MODULE);
}

static void test_section_bounds(void) {
    static const uint8_t invalid_id[] = {
        COMPONENT_HEADER,
        0x0d,0x00
    };
    static const uint8_t truncated_payload[] = {
        COMPONENT_HEADER,
        0x07,0x03,0x00
    };
    static const uint8_t missing_size[] = {
        COMPONENT_HEADER,
        0x00
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component,
               invalid_id,
               sizeof(invalid_id)) == TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,
               truncated_payload,
               sizeof(truncated_payload)) == TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,
               missing_size,
               sizeof(missing_size)) == TURBOWASM_MALFORMED_MODULE);
}

static void test_embedded_core_module_record(void) {
    static const uint8_t bytes[] = {
        COMPONENT_HEADER,
        0x01,0x08,
        0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
        0x07,0x01,0x00
    };
    static const uint8_t wrong_layer[] = {
        COMPONENT_HEADER,
        0x01,0x08,
        COMPONENT_HEADER
    };
    turbowasm_component_binary component = {0};
    const turbowasm_component_core_module *module;

    assert(turbowasm_component_binary_load(
               &component,bytes,sizeof(bytes)) == TURBOWASM_OK);
    assert(component.section_count == 2u);
    assert(component.core_module_count == 1u);

    module = turbowasm_component_binary_core_module_at(
        &component,0u);
    assert(module != NULL);
    assert(module->size == 8u);
    assert(module->bytes[0] == 0u);
    assert(module->bytes[1] == (uint8_t)'a');
    assert(turbowasm_component_binary_core_module_at(
               &component,1u) == NULL);

    turbowasm_component_binary_destroy(&component);

    assert(turbowasm_component_binary_load(
               &component,
               wrong_layer,
               sizeof(wrong_layer)) != TURBOWASM_OK);
}

static void test_nested_component_boundary(void) {
    static const uint8_t valid[] = {
        COMPONENT_HEADER,
        0x04,0x08,
        COMPONENT_HEADER
    };
    static const uint8_t bad_version[] = {
        COMPONENT_HEADER,
        0x04,0x08,
        0x00,0x61,0x73,0x6d,0x0c,0x00,0x01,0x00
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component,valid,sizeof(valid)) == TURBOWASM_OK);
    assert(component.section_count == 1u);
    turbowasm_component_binary_destroy(&component);

    assert(turbowasm_component_binary_load(
               &component,
               bad_version,
               sizeof(bad_version)) == TURBOWASM_MALFORMED_MODULE);
}

static void test_runtime_allocation_limit(void) {
    static const uint8_t bytes[] = {
        COMPONENT_HEADER,
        0x07,0x01,0x00
    };
    turbowasm_component_binary component = {0};
    turbowasm_runtime_config config;

    turbowasm_runtime_config_init(&config);
    config.limits.max_allocation_bytes = 1u;

    assert(turbowasm_component_binary_load_with_config(
               &component,
               bytes,
               sizeof(bytes),
               &config) == TURBOWASM_OUT_OF_MEMORY);
    assert(component.bytes == NULL);
}

int main(void) {
    test_empty_component();
    test_preamble_rejection();
    test_custom_section_framing();
    test_section_bounds();
    test_embedded_core_module_record();
    test_nested_component_boundary();
    test_runtime_allocation_limit();
    return 0;
}
