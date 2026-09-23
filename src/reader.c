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
