#include "component_string.h"
#include "instance_internal.h"
#include "runtime_alloc.h"

#include <string.h>

#define TW_STRING_MAX_BYTES ((UINT64_C(1) << 28u) - UINT64_C(1))
enum {
    TW_ASCII_END = 0x80, TW_LATIN1_END = 0x100,
    TW_UTF8_TWO_END = 0x800, TW_BMP_END = 0x10000, TW_UNICODE_END = 0x110000,
    TW_HIGH_SURROGATE = 0xd800, TW_LOW_SURROGATE = 0xdc00, TW_SURROGATE_END = 0xe000,
    TW_SURROGATE_BITS = 10, TW_SURROGATE_MASK = 0x3ff,
    TW_UTF8_CONT = 0x80, TW_UTF8_CONT_MASK = 0x3f, TW_UTF8_CONT_BITS = 6,
    TW_UTF8_TWO = 0xc0, TW_UTF8_THREE = 0xe0, TW_UTF8_FOUR = 0xf0,
    TW_UTF8_MAX_BYTES = 4, TW_UTF16_WIDTH = 2
};
typedef struct string_measure {
    size_t scalars, utf16_units, ascii_prefix, latin1_prefix, latin1_prefix_bytes;
    bool latin1;
} string_measure;

bool turbowasm_component_string_encoding_valid(turbowasm_component_string_encoding encoding) {
    return encoding == TURBOWASM_COMPONENT_STRING_UTF8 ||
        encoding == TURBOWASM_COMPONENT_STRING_UTF16 ||
        encoding == TURBOWASM_COMPONENT_STRING_LATIN1_UTF16;
}
static bool memory_valid(const turbowasm_component_canonical_memory *memory) {
    return memory != NULL && memory->instance != NULL && memory->instance->impl != NULL &&
        (memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I32 ||
         memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I64) &&
        turbowasm_component_string_encoding_valid(memory->string_encoding);
}
static uint64_t utf16_tag(const turbowasm_component_canonical_memory *memory) {
    return memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I64 ?
        (UINT64_C(1) << 63u) : (UINT64_C(1) << 31u);
}
static bool scalar_valid(uint32_t scalar) {
    return scalar < TW_UNICODE_END &&
        (scalar < TW_HIGH_SURROGATE || scalar >= TW_SURROGATE_END);
}
static uint32_t read_utf16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u);
}

/* Strict decoding of one canonical code point: no replacement or normalization. */
static bool next_scalar(const uint8_t *bytes, size_t size, size_t *offset,
    turbowasm_component_string_origin origin, uint32_t *out) {
    size_t cursor = *offset;
    uint32_t scalar, minimum = 0;
    unsigned trailing = 0, i;
    if (bytes == NULL || cursor >= size) return false;
    if (origin == TURBOWASM_COMPONENT_STRING_ORIGIN_LATIN1) {
        *out = bytes[cursor]; *offset = cursor + 1; return true;
    }
    if (origin != TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8) {
        if (size - cursor < TW_UTF16_WIDTH) return false;
        scalar = read_utf16(bytes + cursor); cursor += TW_UTF16_WIDTH;
        if (scalar >= TW_HIGH_SURROGATE && scalar < TW_LOW_SURROGATE) {
            uint32_t low;
            if (size - cursor < TW_UTF16_WIDTH) return false;
            low = read_utf16(bytes + cursor); cursor += TW_UTF16_WIDTH;
            if (low < TW_LOW_SURROGATE || low >= TW_SURROGATE_END) return false;
            scalar = TW_BMP_END + ((scalar - TW_HIGH_SURROGATE) << TW_SURROGATE_BITS) +
                (low - TW_LOW_SURROGATE);
        }
    } else {
        scalar = bytes[cursor++];
        if (scalar >= TW_ASCII_END) {
            if (scalar >= TW_UTF8_TWO && scalar < TW_UTF8_THREE) {
                trailing = 1; minimum = TW_ASCII_END; scalar &= 0x1fu;
            } else if (scalar >= TW_UTF8_THREE && scalar < TW_UTF8_FOUR) {
                trailing = 2; minimum = TW_UTF8_TWO_END; scalar &= 0x0fu;
            } else if (scalar >= TW_UTF8_FOUR && scalar < 0xf8u) {
                trailing = 3; minimum = TW_BMP_END; scalar &= 0x07u;
            } else return false;
            if (size - cursor < trailing) return false;
            for (i = 0; i < trailing; ++i) {
                uint8_t byte = bytes[cursor++];
                if ((byte & TW_UTF8_TWO) != TW_UTF8_CONT) return false;
                scalar = (scalar << TW_UTF8_CONT_BITS) | (byte & TW_UTF8_CONT_MASK);
            }
            if (scalar < minimum) return false;
        }
    }
    if (!scalar_valid(scalar)) return false;
    *out = scalar; *offset = cursor; return true;
}
static size_t encode_utf8(uint32_t scalar, uint8_t bytes[TW_UTF8_MAX_BYTES]) {
    unsigned count, i;
    if (scalar < TW_ASCII_END) { bytes[0] = (uint8_t)scalar; return 1; }
    count = scalar < TW_UTF8_TWO_END ? 2 : scalar < TW_BMP_END ? 3 : 4;
    for (i = count - 1; i != 0; --i) {
        bytes[i] = (uint8_t)(TW_UTF8_CONT | (scalar & TW_UTF8_CONT_MASK));
        scalar >>= TW_UTF8_CONT_BITS;
    }
    bytes[0] = (uint8_t)((count == 2 ? TW_UTF8_TWO : count == 3 ? TW_UTF8_THREE : TW_UTF8_FOUR) | scalar);
    return count;
}
static size_t encode_utf16(uint32_t scalar, uint8_t bytes[TW_UTF8_MAX_BYTES]) {
    uint32_t high, low;
    if (scalar < TW_BMP_END) {
        bytes[0] = (uint8_t)scalar; bytes[1] = (uint8_t)(scalar >> 8u); return TW_UTF16_WIDTH;
    }
    scalar -= TW_BMP_END;
    high = TW_HIGH_SURROGATE + (scalar >> TW_SURROGATE_BITS);
    low = TW_LOW_SURROGATE + (scalar & TW_SURROGATE_MASK);
    bytes[0] = (uint8_t)high; bytes[1] = (uint8_t)(high >> 8u);
    bytes[2] = (uint8_t)low; bytes[3] = (uint8_t)(low >> 8u); return TW_UTF8_MAX_BYTES;
}
static turbowasm_status measure(const turbowasm_component_owned_string *string, string_measure *out) {
    size_t cursor = 0;
    uint64_t source_bytes, maximum_utf8_bytes;
    string_measure result = {0};
    if (string == NULL || (unsigned)string->origin > TURBOWASM_COMPONENT_STRING_ORIGIN_COMPACT_UTF16 ||
        (string->size != 0 && string->data == NULL)) return TURBOWASM_INVALID_ARGUMENT;
    maximum_utf8_bytes = string->origin == TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8 ? TW_STRING_MAX_BYTES :
        string->origin == TURBOWASM_COMPONENT_STRING_ORIGIN_LATIN1 ? TW_STRING_MAX_BYTES * 2u :
        (TW_STRING_MAX_BYTES / TW_UTF16_WIDTH) * 3u;
    if (string->size > maximum_utf8_bytes) return TURBOWASM_INVALID_ARGUMENT;
    result.ascii_prefix = string->size; result.latin1 = true;
    while (cursor < string->size) {
        size_t before = cursor;
        uint32_t scalar;
        if (!next_scalar(string->data, string->size, &cursor, TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8, &scalar))
            return TURBOWASM_INVALID_ARGUMENT;
        if (scalar >= TW_ASCII_END && result.ascii_prefix == string->size) result.ascii_prefix = before;
        if (result.latin1 && scalar >= TW_LATIN1_END) {
            result.latin1 = false; result.latin1_prefix = result.scalars; result.latin1_prefix_bytes = before;
        }
        ++result.scalars; result.utf16_units += scalar >= TW_BMP_END ? 2u : 1u;
    }
    if (result.latin1) { result.latin1_prefix = result.scalars; result.latin1_prefix_bytes = string->size; }
    if (string->origin == TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8) source_bytes = string->size;
    else if (string->origin == TURBOWASM_COMPONENT_STRING_ORIGIN_LATIN1) {
        if (!result.latin1) return TURBOWASM_INVALID_ARGUMENT;
        source_bytes = result.scalars;
    } else source_bytes = (uint64_t)result.utf16_units * TW_UTF16_WIDTH;
    if (source_bytes > TW_STRING_MAX_BYTES) return TURBOWASM_INVALID_ARGUMENT;
    *out = result; return TURBOWASM_OK;
}
turbowasm_status turbowasm_component_string_validate(const turbowasm_component_owned_string *string) {
    string_measure ignored;
    return measure(string, &ignored);
}
static turbowasm_status bounds(const turbowasm_component_canonical_memory *memory,
    uint64_t pointer, uint64_t size, uint64_t alignment) {
    uint8_t *ignored = NULL;
    if (pointer % alignment != 0 || size > SIZE_MAX ||
        (memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I32 && pointer > UINT32_MAX))
        return TURBOWASM_TRAPPED;
    return turbowasm_instance_memory_bounds(memory->instance->impl, memory->memory_index,
        pointer, 0, (size_t)size, &ignored);
}
turbowasm_status turbowasm_component_string_lift(const turbowasm_component_canonical_memory *memory,
    uint64_t pointer, uint64_t tagged_length, turbowasm_component_owned_string *out) {
    uint64_t length = tagged_length, width = 1, alignment = 1;
    turbowasm_component_string_origin origin = TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8;
    uint8_t *raw = NULL, *encoded = NULL, bytes[TW_UTF8_MAX_BYTES];
    size_t cursor = 0, size = 0, output_cursor = 0;
    turbowasm_status status;
    if (!memory_valid(memory) || out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (memory->string_encoding == TURBOWASM_COMPONENT_STRING_UTF16) {
        origin = TURBOWASM_COMPONENT_STRING_ORIGIN_UTF16; width = alignment = TW_UTF16_WIDTH;
    } else if (memory->string_encoding == TURBOWASM_COMPONENT_STRING_LATIN1_UTF16) {
        alignment = TW_UTF16_WIDTH;
        if ((length & utf16_tag(memory)) != 0) {
            length ^= utf16_tag(memory); width = TW_UTF16_WIDTH;
            origin = TURBOWASM_COMPONENT_STRING_ORIGIN_COMPACT_UTF16;
        } else origin = TURBOWASM_COMPONENT_STRING_ORIGIN_LATIN1;
    }
    if (length > TW_STRING_MAX_BYTES / width) return TURBOWASM_TRAPPED;
    length *= width;
    status = bounds(memory, pointer, length, alignment);
    if (status != TURBOWASM_OK) return status;
    if (length != 0) {
        raw = turbowasm_rt_malloc((size_t)length);
        if (raw == NULL) return TURBOWASM_OUT_OF_MEMORY;
        status = turbowasm_instance_memory_read_bytes(memory->instance->impl, memory->memory_index,
            pointer, 0, raw, (size_t)length);
        if (status != TURBOWASM_OK) goto done;
    }
    while (cursor < length) {
        uint32_t scalar;
        if (!next_scalar(raw, (size_t)length, &cursor, origin, &scalar)) { status = TURBOWASM_TRAPPED; goto done; }
        size += encode_utf8(scalar, bytes);
    }
    if (origin == TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8) { encoded = raw; raw = NULL; }
    else if (size != 0) {
        encoded = turbowasm_rt_malloc(size);
        if (encoded == NULL) { status = TURBOWASM_OUT_OF_MEMORY; goto done; }
        cursor = 0;
        while (cursor < length) {
            uint32_t scalar; size_t count;
            if (!next_scalar(raw, (size_t)length, &cursor, origin, &scalar)) { status = TURBOWASM_TRAPPED; goto done; }
            count = encode_utf8(scalar, bytes); memcpy(encoded + output_cursor, bytes, count); output_cursor += count;
        }
    }
    *out = (turbowasm_component_owned_string){encoded, size, origin}; encoded = NULL;
    status = TURBOWASM_OK;
done:
    turbowasm_rt_free(encoded); turbowasm_rt_free(raw); return status;
}

typedef struct guest_string {
    const turbowasm_component_canonical_memory *memory;
    uint64_t pointer, capacity;
} guest_string;
static turbowasm_status resize(guest_string *guest, uint64_t alignment, uint64_t size) {
    uint64_t pointer;
    turbowasm_status status;
    if (size > UINT32_MAX) return TURBOWASM_TRAPPED;
    if (guest->memory->guest_realloc == NULL) return TURBOWASM_UNSUPPORTED;
    status = guest->memory->guest_realloc(guest->memory->realloc_context, guest->pointer,
        guest->capacity, alignment, size, &pointer);
    if (status != TURBOWASM_OK) return status;
    status = bounds(guest->memory, pointer, size, alignment);
    if (status != TURBOWASM_OK) return status;
    guest->pointer = pointer; guest->capacity = size; return TURBOWASM_OK;
}
static turbowasm_status write_bytes(const guest_string *guest, size_t offset, const uint8_t *bytes, size_t count) {
    if (offset > guest->capacity || count > guest->capacity - offset) return TURBOWASM_TRAPPED;
    return turbowasm_instance_memory_write_bytes(guest->memory->instance->impl,
        guest->memory->memory_index, guest->pointer, offset, bytes, count);
}
static turbowasm_status write_scalars(const guest_string *guest, const turbowasm_component_owned_string *string,
    size_t source_begin, size_t source_end, size_t destination, bool utf16) {
    size_t cursor = source_begin;
    while (cursor < source_end) {
        uint32_t scalar; uint8_t bytes[TW_UTF8_MAX_BYTES]; size_t count;
        turbowasm_status status;
        if (!next_scalar(string->data, source_end, &cursor, TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8, &scalar))
            return TURBOWASM_INVALID_ARGUMENT;
        if (utf16) count = encode_utf16(scalar, bytes);
        else { if (scalar >= TW_LATIN1_END) return TURBOWASM_INVALID_ARGUMENT; bytes[0] = (uint8_t)scalar; count = 1; }
        status = write_bytes(guest, destination, bytes, count);
        if (status != TURBOWASM_OK) return status;
        destination += count;
    }
    return TURBOWASM_OK;
}
static turbowasm_status adjust_latin1_prefix(const guest_string *guest, size_t count, bool inflate) {
    size_t index;
    for (index = 0; index < count; ++index) {
        size_t at = inflate ? count - index - 1 : index;
        uint8_t bytes[TW_UTF16_WIDTH] = {0};
        turbowasm_status status = turbowasm_instance_memory_read_bytes(guest->memory->instance->impl,
            guest->memory->memory_index, guest->pointer, inflate ? at : at * TW_UTF16_WIDTH, bytes, 1);
        if (status != TURBOWASM_OK) return status;
        status = write_bytes(guest, inflate ? at * TW_UTF16_WIDTH : at, bytes, inflate ? TW_UTF16_WIDTH : 1);
        if (status != TURBOWASM_OK) return status;
    }
    return TURBOWASM_OK;
}
static turbowasm_status lower_utf8(guest_string *guest, const turbowasm_component_owned_string *string,
    const string_measure *measured, size_t source_units, uint64_t *length) {
    bool source_utf8 = string->origin == TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8;
    size_t prefix = source_utf8 ? string->size : measured->ascii_prefix;
    turbowasm_status status = resize(guest, 1, source_units);
    if (status != TURBOWASM_OK) return status;
    if (prefix != 0) { status = write_bytes(guest, 0, string->data, prefix); if (status != TURBOWASM_OK) return status; }
    if (prefix != string->size) {
        uint64_t factor = string->origin == TURBOWASM_COMPONENT_STRING_ORIGIN_LATIN1 ? 2u : 3u;
        status = resize(guest, 1, source_units * factor);
        if (status != TURBOWASM_OK) return status;
        status = write_bytes(guest, prefix, string->data + prefix, string->size - prefix);
        if (status != TURBOWASM_OK) return status;
        if (guest->capacity > string->size) { status = resize(guest, 1, string->size); if (status != TURBOWASM_OK) return status; }
    }
    *length = string->size; return TURBOWASM_OK;
}
static turbowasm_status lower_utf16(guest_string *guest, const turbowasm_component_owned_string *string,
    const string_measure *measured, size_t source_units, uint64_t *length) {
    turbowasm_status status = resize(guest, TW_UTF16_WIDTH, (uint64_t)source_units * TW_UTF16_WIDTH);
    uint64_t bytes = (uint64_t)measured->utf16_units * TW_UTF16_WIDTH;
    if (status != TURBOWASM_OK) return status;
    status = write_scalars(guest, string, 0, string->size, 0, true);
    if (status != TURBOWASM_OK) return status;
    if (guest->capacity > bytes) { status = resize(guest, TW_UTF16_WIDTH, bytes); if (status != TURBOWASM_OK) return status; }
    *length = measured->utf16_units; return TURBOWASM_OK;
}
static turbowasm_status lower_compact(guest_string *guest, const turbowasm_component_owned_string *string,
    const string_measure *measured, size_t source_units, uint64_t *length) {
    turbowasm_status status;
    uint64_t utf16_bytes = (uint64_t)measured->utf16_units * TW_UTF16_WIDTH;
    if (string->origin == TURBOWASM_COMPONENT_STRING_ORIGIN_COMPACT_UTF16) {
        status = lower_utf16(guest, string, measured, source_units, length);
        if (status != TURBOWASM_OK) return status;
        if (!measured->latin1) { *length |= utf16_tag(guest->memory); return TURBOWASM_OK; }
        status = adjust_latin1_prefix(guest, measured->scalars, false);
        if (status != TURBOWASM_OK) return status;
        status = resize(guest, 1, measured->scalars);
        if (status != TURBOWASM_OK) return status;
    } else {
        status = resize(guest, TW_UTF16_WIDTH, source_units);
        if (status != TURBOWASM_OK) return status;
        status = write_scalars(guest, string, 0, measured->latin1_prefix_bytes, 0, false);
        if (status != TURBOWASM_OK) return status;
        if (!measured->latin1) {
            status = resize(guest, TW_UTF16_WIDTH, (uint64_t)source_units * TW_UTF16_WIDTH);
            if (status != TURBOWASM_OK) return status;
            status = adjust_latin1_prefix(guest, measured->latin1_prefix, true);
            if (status != TURBOWASM_OK) return status;
            status = write_scalars(guest, string, measured->latin1_prefix_bytes, string->size,
                measured->latin1_prefix * TW_UTF16_WIDTH, true);
            if (status != TURBOWASM_OK) return status;
            if (guest->capacity > utf16_bytes) {
                status = resize(guest, TW_UTF16_WIDTH, utf16_bytes); if (status != TURBOWASM_OK) return status;
            }
            *length = measured->utf16_units | utf16_tag(guest->memory); return TURBOWASM_OK;
        }
        if (guest->capacity > measured->scalars) {
            status = resize(guest, TW_UTF16_WIDTH, measured->scalars); if (status != TURBOWASM_OK) return status;
        }
    }
    *length = measured->scalars; return TURBOWASM_OK;
}
turbowasm_status turbowasm_component_string_lower(const turbowasm_component_canonical_memory *memory,
    const turbowasm_component_owned_string *string, uint64_t *out_pointer, uint64_t *out_length) {
    string_measure measured;
    guest_string guest = {memory, 0, 0};
    size_t source_units;
    uint64_t length = 0;
    turbowasm_status status;
    if (!memory_valid(memory) || out_pointer == NULL || out_length == NULL) return TURBOWASM_INVALID_ARGUMENT;
    status = measure(string, &measured); if (status != TURBOWASM_OK) return status;
    source_units = string->origin == TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8 ? string->size :
        string->origin == TURBOWASM_COMPONENT_STRING_ORIGIN_LATIN1 ? measured.scalars : measured.utf16_units;
    switch (memory->string_encoding) {
    case TURBOWASM_COMPONENT_STRING_UTF8: status = lower_utf8(&guest, string, &measured, source_units, &length); break;
    case TURBOWASM_COMPONENT_STRING_UTF16: status = lower_utf16(&guest, string, &measured, source_units, &length); break;
    case TURBOWASM_COMPONENT_STRING_LATIN1_UTF16: status = lower_compact(&guest, string, &measured, source_units, &length); break;
    default: return TURBOWASM_INVALID_ARGUMENT;
    }
    if (status != TURBOWASM_OK) return status;
    *out_pointer = guest.pointer; *out_length = length; return TURBOWASM_OK;
}
