#include "component_binary.h"

#include <turbowasm/module.h>

#include "reader.h"
#include "runtime_alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    TW_COMPONENT_SECTION_CUSTOM = 0u,
    TW_COMPONENT_SECTION_CORE_MODULE = 1u,
    TW_COMPONENT_SECTION_MAX = 12u
};

static bool utf8_cont(uint8_t byte) {
    return (byte & UINT8_C(0xc0)) == UINT8_C(0x80);
}

static bool utf8_valid(const uint8_t *bytes, size_t size) {
    size_t i = 0u;

    if (size != 0u && bytes == NULL)
        return false;

    while (i < size) {
        uint8_t a = bytes[i++];

        if (a < UINT8_C(0x80))
            continue;

        if (a >= UINT8_C(0xc2) && a <= UINT8_C(0xdf)) {
            if (i >= size || !utf8_cont(bytes[i]))
                return false;
            ++i;
            continue;
        }

        if (a == UINT8_C(0xe0)) {
            if (i + 1u >= size ||
                bytes[i] < UINT8_C(0xa0) ||
                bytes[i] > UINT8_C(0xbf) ||
                !utf8_cont(bytes[i + 1u]))
                return false;
            i += 2u;
            continue;
        }

        if ((a >= UINT8_C(0xe1) && a <= UINT8_C(0xec)) ||
            (a >= UINT8_C(0xee) && a <= UINT8_C(0xef))) {
            if (i + 1u >= size ||
                !utf8_cont(bytes[i]) ||
                !utf8_cont(bytes[i + 1u]))
                return false;
            i += 2u;
            continue;
        }

        if (a == UINT8_C(0xed)) {
            if (i + 1u >= size ||
                bytes[i] < UINT8_C(0x80) ||
                bytes[i] > UINT8_C(0x9f) ||
                !utf8_cont(bytes[i + 1u]))
                return false;
            i += 2u;
            continue;
        }

        if (a == UINT8_C(0xf0)) {
            if (i + 2u >= size ||
                bytes[i] < UINT8_C(0x90) ||
                bytes[i] > UINT8_C(0xbf) ||
                !utf8_cont(bytes[i + 1u]) ||
                !utf8_cont(bytes[i + 2u]))
                return false;
            i += 3u;
            continue;
        }

        if (a >= UINT8_C(0xf1) && a <= UINT8_C(0xf3)) {
            if (i + 2u >= size ||
                !utf8_cont(bytes[i]) ||
                !utf8_cont(bytes[i + 1u]) ||
                !utf8_cont(bytes[i + 2u]))
                return false;
            i += 3u;
            continue;
        }

        if (a == UINT8_C(0xf4)) {
            if (i + 2u >= size ||
                bytes[i] < UINT8_C(0x80) ||
                bytes[i] > UINT8_C(0x8f) ||
                !utf8_cont(bytes[i + 1u]) ||
                !utf8_cont(bytes[i + 2u]))
                return false;
            i += 3u;
            continue;
        }

        return false;
    }

    return true;
}

static bool component_preamble(
    turbowasm_reader *reader) {
    uint32_t magic;
    uint8_t version_lo;
    uint8_t version_hi;
    uint8_t layer_lo;
    uint8_t layer_hi;

    return reader != NULL &&
           turbowasm_reader_u32le(reader, &magic) &&
           magic == UINT32_C(0x6d736100) &&
           turbowasm_reader_u8(reader, &version_lo) &&
           turbowasm_reader_u8(reader, &version_hi) &&
           turbowasm_reader_u8(reader, &layer_lo) &&
           turbowasm_reader_u8(reader, &layer_hi) &&
           version_lo == UINT8_C(0x0d) &&
           version_hi == 0u &&
           layer_lo == UINT8_C(0x01) &&
           layer_hi == 0u;
}

static bool reserve_array(
    void **items,
    uint32_t *capacity,
    uint32_t required,
    size_t item_size) {
    uint32_t next;
    void *grown;

    if (items == NULL || capacity == NULL || item_size == 0u)
        return false;
    if (required <= *capacity)
        return true;

    next = *capacity == 0u ? 4u : *capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }

    if ((size_t)next > SIZE_MAX / item_size)
        return false;

    grown = turbowasm_rt_realloc(
        *items, (size_t)next * item_size);
    if (grown == NULL)
        return false;

    *items = grown;
    *capacity = next;
    return true;
}

static bool append_section(
    turbowasm_component_binary *component,
    uint8_t id,
    const uint8_t *payload,
    uint32_t size) {
    turbowasm_component_section *section;
    uint32_t required;

    if (component == NULL ||
        component->section_count == UINT32_MAX)
        return false;

    required = component->section_count + 1u;
    if (!reserve_array(
            (void **)&component->sections,
            &component->section_capacity,
            required,
            sizeof(*component->sections)))
        return false;

    section = &component->sections[component->section_count++];
    section->id = id;
    section->payload = payload;
    section->size = size;
    return true;
}

static bool append_core_module(
    turbowasm_component_binary *component,
    const uint8_t *bytes,
    uint32_t size) {
    turbowasm_component_core_module *module;
    uint32_t required;

    if (component == NULL ||
        component->core_module_count == UINT32_MAX)
        return false;

    required = component->core_module_count + 1u;
    if (!reserve_array(
            (void **)&component->core_modules,
            &component->core_module_capacity,
            required,
            sizeof(*component->core_modules)))
        return false;

    module =
        &component->core_modules[component->core_module_count++];
    module->bytes = bytes;
    module->size = size;
    return true;
}

static turbowasm_status validate_custom_section(
    turbowasm_reader section) {
    uint32_t name_size;
    turbowasm_reader name;

    if (!turbowasm_reader_uleb32(&section, &name_size) ||
        !turbowasm_reader_slice(&section, (size_t)name_size, &name))
        return TURBOWASM_MALFORMED_MODULE;

    if (!utf8_valid(name.cursor, turbowasm_reader_remaining(&name)))
        return TURBOWASM_MALFORMED_MODULE;

    return TURBOWASM_OK;
}

static turbowasm_status validate_core_module(
    const uint8_t *bytes,
    uint32_t size,
    const turbowasm_runtime_config *config) {
    turbowasm_module module = {0};
    turbowasm_status status;

    status = turbowasm_module_load_borrowed_with_config(
        &module, bytes, (size_t)size, config);
    if (status == TURBOWASM_OK)
        turbowasm_module_destroy(&module);
    return status;
}

turbowasm_status turbowasm_component_binary_load(
    turbowasm_component_binary *component,
    const uint8_t *bytes,
    size_t size) {
    return turbowasm_component_binary_load_with_config(
        component, bytes, size, NULL);
}

turbowasm_status turbowasm_component_binary_load_with_config(
    turbowasm_component_binary *component,
    const uint8_t *bytes,
    size_t size,
    const turbowasm_runtime_config *config) {
    turbowasm_runtime_config normalized;
    turbowasm_runtime_scope scope;
    turbowasm_reader reader;
    turbowasm_status status = TURBOWASM_OK;

    if (component == NULL || bytes == NULL ||
        component->bytes != NULL ||
        component->sections != NULL ||
        component->core_modules != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_runtime_config_normalize(config, &normalized))
        return TURBOWASM_INVALID_ARGUMENT;

    scope = turbowasm_runtime_scope_enter(&normalized);
    turbowasm_reader_init(&reader, bytes, size);

    if (!component_preamble(&reader)) {
        status = TURBOWASM_MALFORMED_MODULE;
        goto fail;
    }

    component->bytes = bytes;
    component->size = size;
    component->config = normalized;

    while (turbowasm_reader_remaining(&reader) != 0u) {
        uint8_t id;
        uint32_t payload_size;
        turbowasm_reader payload;

        if (!turbowasm_reader_u8(&reader, &id) ||
            id > TW_COMPONENT_SECTION_MAX ||
            !turbowasm_reader_uleb32(&reader, &payload_size) ||
            !turbowasm_reader_slice(
                &reader, (size_t)payload_size, &payload)) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        if (!append_section(
                component, id, payload.cursor, payload_size)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }

        if (id == TW_COMPONENT_SECTION_CUSTOM) {
            status = validate_custom_section(payload);
            if (status != TURBOWASM_OK)
                goto fail;
        } else if (id == TW_COMPONENT_SECTION_CORE_MODULE) {
            status = validate_core_module(
                payload.cursor, payload_size, &normalized);
            if (status != TURBOWASM_OK)
                goto fail;
            if (!append_core_module(
                    component, payload.cursor, payload_size)) {
                status = TURBOWASM_OUT_OF_MEMORY;
                goto fail;
            }
        } else if (id == 4u) {
            /*
             * C2a only establishes the nested-component boundary. Full nested
             * section validation is part of later Component decoding.
             */
            turbowasm_reader nested = payload;
            if (!component_preamble(&nested)) {
                status = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }
        }
    }

    turbowasm_runtime_scope_leave(scope);
    return TURBOWASM_OK;

fail:
    turbowasm_rt_free(component->sections);
    turbowasm_rt_free(component->core_modules);
    memset(component, 0, sizeof(*component));
    turbowasm_runtime_scope_leave(scope);
    return status;
}

void turbowasm_component_binary_destroy(
    turbowasm_component_binary *component) {
    turbowasm_runtime_scope scope;

    if (component == NULL)
        return;

    scope = turbowasm_runtime_scope_enter(&component->config);
    turbowasm_rt_free(component->sections);
    turbowasm_rt_free(component->core_modules);
    memset(component, 0, sizeof(*component));
    turbowasm_runtime_scope_leave(scope);
}

const turbowasm_component_section *
turbowasm_component_binary_section_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL || index >= component->section_count)
        return NULL;
    return &component->sections[index];
}

const turbowasm_component_core_module *
turbowasm_component_binary_core_module_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL || index >= component->core_module_count)
        return NULL;
    return &component->core_modules[index];
}
