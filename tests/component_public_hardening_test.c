#include <turbowasm/component.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

/*
 * Pinned upstream Component Model binary fixtures:
 * WebAssembly/component-model@a25fc0b372dd21f07f0242c46e98bd0f1ea0c0e1
 * test/binary/binary.wast blob 3766e0180176889c6eb22198e26ca095c21425c8
 */
static const uint8_t upstream_empty_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00
};

static const uint8_t upstream_custom_hi[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,
    0x00,0x03,0x02,'h','i'
};

static const uint8_t upstream_bad_version[] = {
    0x00,0x61,0x73,0x6d,0x0c,0x00,0x01,0x00
};

static const uint8_t upstream_bad_layer[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x02,0x00
};

static const uint8_t upstream_bad_custom_utf8[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,
    0x00,0x03,0x02,0xff,0xfe
};

static const uint8_t executable_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,
    0x01,0x27,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x07,0x0a,0x01,0x06,'a','n','s','w','e','r',0x00,0x00,
      0x0a,0x06,0x01,0x04,0x00,0x41,0x2a,0x0b,
    0x02,0x04,0x01,0x00,0x00,0x00,
    0x06,0x0c,0x01,
      0x00,0x00,0x01,0x00,
      0x06,'a','n','s','w','e','r',
    0x07,0x05,0x01,0x40,0x00,0x00,0x79,
    0x08,0x06,0x01,0x00,0x00,0x00,0x00,0x00,
    0x0b,0x0c,0x01,
      0x00,0x06,'a','n','s','w','e','r',
      0x01,0x00,0x00
};

static const uint8_t canonical_options_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,
    0x01,0x38,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x09,0x01,0x60,0x04,0x7f,0x7f,0x7f,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x05,0x03,0x01,0x00,0x01,
      0x07,0x11,0x02,
        0x03,'m','e','m',0x02,0x00,
        0x07,'r','e','a','l','l','o','c',0x00,0x00,
      0x0a,0x07,0x01,0x05,0x00,0x41,0x80,0x02,0x0b,
    0x01,0x26,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x07,0x07,0x01,0x03,'l','e','n',0x00,0x00,
      0x0a,0x06,0x01,0x04,0x00,0x20,0x01,0x0b,
    0x02,0x07,0x02,
      0x00,0x00,0x00,
      0x00,0x01,0x00,
    0x06,0x1d,0x03,
      0x00,0x02,0x01,0x00,0x03,'m','e','m',
      0x00,0x00,0x01,0x00,0x07,'r','e','a','l','l','o','c',
      0x00,0x00,0x01,0x01,0x03,'l','e','n',
    0x07,0x08,0x01,
      0x40,0x01,0x01,'s',0x73,0x00,0x79,
    0x08,0x0b,0x01,
      0x00,0x00,0x01,
      0x03,0x00,0x03,0x00,0x04,0x00,
      0x00,
    0x0b,0x09,0x01,
      0x00,0x03,'l','e','n',
      0x01,0x00,0x00
};

static const uint8_t recursive_list_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,
    0x01,0x38,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x09,0x01,0x60,0x04,0x7f,0x7f,0x7f,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x05,0x03,0x01,0x00,0x01,
      0x07,0x11,0x02,
        0x03,'m','e','m',0x02,0x00,
        0x07,'r','e','a','l','l','o','c',0x00,0x00,
      0x0a,0x07,0x01,0x05,0x00,0x41,0x80,0x02,0x0b,
    0x01,0x26,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x07,0x07,0x01,0x03,'l','e','n',0x00,0x00,
      0x0a,0x06,0x01,0x04,0x00,0x20,0x01,0x0b,
    0x02,0x07,0x02,
      0x00,0x00,0x00,
      0x00,0x01,0x00,
    0x06,0x1d,0x03,
      0x00,0x02,0x01,0x00,0x03,'m','e','m',
      0x00,0x00,0x01,0x00,0x07,'r','e','a','l','l','o','c',
      0x00,0x00,0x01,0x01,0x03,'l','e','n',

    /*
     * type0 = list<u32>
     * type1 = list<type0>
     * type2 = func(xs:type1)->u32
     */
    0x07,0x0d,0x03,
      0x70,0x79,
      0x70,0x00,
      0x40,0x01,0x02,'x','s',0x01,0x00,0x79,

    0x08,0x0b,0x01,
      0x00,0x00,0x01,
      0x03,
      0x00,
      0x03,0x00,
      0x04,0x00,
      0x02,

    0x0b,0x09,0x01,
      0x00,0x03,'l','e','n',
      0x01,0x00,0x00
};


static turbowasm_name name_span(const char *text, uint32_t size) {
    turbowasm_name name = {(const uint8_t *)text, size};
    return name;
}

static void test_upstream_binary_subset(void) {
    turbowasm_component component = {0};

    assert(turbowasm_component_load_borrowed(
               &component,
               upstream_empty_component,
               sizeof(upstream_empty_component)) == TURBOWASM_OK);
    turbowasm_component_destroy(&component);

    assert(turbowasm_component_load_borrowed(
               &component,
               upstream_custom_hi,
               sizeof(upstream_custom_hi)) == TURBOWASM_OK);
    turbowasm_component_destroy(&component);

    assert(turbowasm_component_load_borrowed(
               &component,
               upstream_bad_version,
               sizeof(upstream_bad_version)) == TURBOWASM_MALFORMED_MODULE);
    assert(component.impl == NULL);

    assert(turbowasm_component_load_borrowed(
               &component,
               upstream_bad_layer,
               sizeof(upstream_bad_layer)) == TURBOWASM_MALFORMED_MODULE);
    assert(component.impl == NULL);

    assert(turbowasm_component_load_borrowed(
               &component,
               upstream_bad_custom_utf8,
               sizeof(upstream_bad_custom_utf8)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(component.impl == NULL);
}

static void test_public_resource_limits(void) {
    turbowasm_component component = {0};
    turbowasm_runtime_config config;

    turbowasm_runtime_config_init(&config);
    config.limits.max_module_bytes =
        sizeof(executable_component) - 1u;
    assert(turbowasm_component_load_borrowed_with_config(
               &component,
               executable_component,
               sizeof(executable_component),
               &config) == TURBOWASM_OUT_OF_MEMORY);
    assert(component.impl == NULL);

    turbowasm_runtime_config_init(&config);
    config.limits.max_allocation_bytes = 1u;
    assert(turbowasm_component_load_borrowed_with_config(
               &component,
               upstream_empty_component,
               sizeof(upstream_empty_component),
               &config) == TURBOWASM_OUT_OF_MEMORY);
    assert(component.impl == NULL);
}

static void test_public_invoke_contract(void) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_component_host_value result = {0};
    size_t result_count = 99u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_component_load_borrowed(
               &component,
               executable_component,
               sizeof(executable_component)) == TURBOWASM_OK);
    assert(turbowasm_component_instance_create(
               &instance, &component) == TURBOWASM_OK);

    assert(turbowasm_component_instance_invoke(
               &instance,
               name_span("answer", 6u),
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap) == TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_component_instance_invoke(
               &instance,
               name_span("missing", 7u),
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_UNSUPPORTED);

    assert(turbowasm_component_instance_invoke(
               &instance,
               name_span("answer", 6u),
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_HOST_U32);
    assert(result.as.u32 == 42u);

    turbowasm_component_host_value_destroy(&result);
    turbowasm_component_instance_destroy(&instance);
    turbowasm_component_destroy(&component);

    /* Destructors are idempotent for zeroed/cleared public owners. */
    turbowasm_component_instance_destroy(&instance);
    turbowasm_component_destroy(&component);
}

static void test_public_utf8_string_path(void) {
    static uint8_t hello[] = {'h','e','l','l','o'};
    static uint8_t invalid_utf8[] = {0xffu};
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_component_host_value argument = {0};
    turbowasm_component_host_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_component_load_borrowed(
               &component,
               canonical_options_component,
               sizeof(canonical_options_component)) == TURBOWASM_OK);
    assert(turbowasm_component_instance_create(
               &instance, &component) == TURBOWASM_OK);

    argument.kind = TURBOWASM_COMPONENT_HOST_STRING;
    argument.as.string.data = hello;
    argument.as.string.size = sizeof(hello);

    assert(turbowasm_component_instance_invoke(
               &instance,
               name_span("len", 3u),
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_COMPONENT_HOST_U32);
    assert(result.as.u32 == sizeof(hello));
    turbowasm_component_host_value_destroy(&result);

    argument.as.string.data = invalid_utf8;
    argument.as.string.size = sizeof(invalid_utf8);
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_component_instance_invoke(
               &instance,
               name_span("len", 3u),
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_INVALID_ARGUMENT);

    turbowasm_component_instance_destroy(&instance);
    turbowasm_component_destroy(&component);
}

static void test_public_recursive_list_path(void) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_component_host_value inner_items[2] = {{0}};
    turbowasm_component_host_value outer_items[1] = {{0}};
    turbowasm_component_host_value argument = {0};
    turbowasm_component_host_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    inner_items[0].kind = TURBOWASM_COMPONENT_HOST_U32;
    inner_items[0].as.u32 = 10u;
    inner_items[1].kind = TURBOWASM_COMPONENT_HOST_U32;
    inner_items[1].as.u32 = 20u;

    outer_items[0].kind = TURBOWASM_COMPONENT_HOST_LIST;
    outer_items[0].as.list.items = inner_items;
    outer_items[0].as.list.count = 2u;

    argument.kind = TURBOWASM_COMPONENT_HOST_LIST;
    argument.as.list.items = outer_items;
    argument.as.list.count = 1u;

    assert(turbowasm_component_load_borrowed(
               &component,
               recursive_list_component,
               sizeof(recursive_list_component)) == TURBOWASM_OK);
    assert(turbowasm_component_instance_create(
               &instance, &component) == TURBOWASM_OK);

    assert(turbowasm_component_instance_invoke(
               &instance,
               name_span("len", 3u),
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_COMPONENT_HOST_U32);
    assert(result.as.u32 == 1u);

    turbowasm_component_host_value_destroy(&result);
    turbowasm_component_instance_destroy(&instance);
    turbowasm_component_destroy(&component);
}

int main(void) {
    test_upstream_binary_subset();
    test_public_resource_limits();
    test_public_invoke_contract();
    test_public_utf8_string_path();
    test_public_recursive_list_path();
    return 0;
}
