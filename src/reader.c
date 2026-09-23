#include "reader.h"

#include <limits.h>

void turbowasm_reader_init(turbowasm_reader *reader,
                           const uint8_t *bytes,
                           size_t size) {
    if (reader == NULL) return;
    reader->cursor = bytes;
    reader->end = bytes == NULL ? NULL : bytes + size;
}

size_t turbowasm_reader_remaining(const turbowasm_reader *reader) {
    if (reader == NULL || reader->cursor == NULL || reader->end == NULL ||
        reader->cursor > reader->end)
        return 0u;
    return (size_t)(reader->end - reader->cursor);
}

bool turbowasm_reader_u8(turbowasm_reader *reader, uint8_t *out) {
    if (reader == NULL || out == NULL || turbowasm_reader_remaining(reader) < 1u)
        return false;
    *out = *reader->cursor++;
    return true;
}

bool turbowasm_reader_u32le(turbowasm_reader *reader, uint32_t *out) {
    const uint8_t *p;
    if (reader == NULL || out == NULL || turbowasm_reader_remaining(reader) < 4u)
        return false;
    p = reader->cursor;
    *out = (uint32_t)p[0] |
           ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
    reader->cursor += 4u;
    return true;
}

bool turbowasm_reader_uleb32(turbowasm_reader *reader, uint32_t *out) {
    uint32_t value = 0u;
    unsigned shift = 0u;
    unsigned count = 0u;
    uint8_t byte = 0u;

    if (reader == NULL || out == NULL) return false;

    do {
        if (count == 5u || !turbowasm_reader_u8(reader, &byte))
            return false;
        if (shift == 28u && (byte & 0xf0u) != 0u)
            return false;
        value |= (uint32_t)(byte & 0x7fu) << shift;
        shift += 7u;
        ++count;
    } while ((byte & 0x80u) != 0u);

    *out = value;
    return true;
}

bool turbowasm_reader_slice(turbowasm_reader *reader,
                            size_t size,
                            turbowasm_reader *out) {
    if (reader == NULL || out == NULL ||
        turbowasm_reader_remaining(reader) < size)
        return false;
    out->cursor = reader->cursor;
    out->end = reader->cursor + size;
    reader->cursor += size;
    return true;
}

bool turbowasm_reader_sleb32(turbowasm_reader *reader, int32_t *out) {
    uint64_t value = 0u;
    unsigned shift = 0u;
    unsigned count;

    if (reader == NULL || out == NULL) return false;

    for (count = 0u; count < 5u; ++count) {
        uint8_t byte;
        uint8_t payload;

        if (!turbowasm_reader_u8(reader, &byte))
            return false;
        payload = (uint8_t)(byte & 0x7fu);

        if (count == 4u) {
            const uint8_t unused = (uint8_t)(payload & 0x70u);
            if (unused != 0x00u && unused != 0x70u)
                return false;
        }

        value |= (uint64_t)payload << shift;

        if ((byte & 0x80u) == 0u) {
            if (count < 4u && (byte & 0x40u) != 0u)
                value |= UINT64_MAX << (shift + 7u);
            *out = (int32_t)(uint32_t)value;
            return true;
        }

        shift += 7u;
    }

    return false;
}

bool turbowasm_reader_sleb64(turbowasm_reader *reader, int64_t *out) {
    uint64_t value = 0u;
    unsigned shift = 0u;
    unsigned count;

    if (reader == NULL || out == NULL) return false;

    for (count = 0u; count < 10u; ++count) {
        uint8_t byte;
        uint8_t payload;

        if (!turbowasm_reader_u8(reader, &byte))
            return false;
        payload = (uint8_t)(byte & 0x7fu);

        if (count == 9u) {
            const uint8_t unused = (uint8_t)(payload & 0x7eu);
            if (unused != 0x00u && unused != 0x7eu)
                return false;
        }

        value |= (uint64_t)payload << shift;

        if ((byte & 0x80u) == 0u) {
            if (count < 9u && (byte & 0x40u) != 0u)
                value |= UINT64_MAX << (shift + 7u);
            *out = (int64_t)value;
            return true;
        }

        shift += 7u;
    }

    return false;
}
