#ifndef TURBOWASM_READER_H
#define TURBOWASM_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct turbowasm_reader {
    const uint8_t *cursor;
    const uint8_t *end;
} turbowasm_reader;

void turbowasm_reader_init(turbowasm_reader *reader,
                           const uint8_t *bytes,
                           size_t size);
size_t turbowasm_reader_remaining(const turbowasm_reader *reader);
bool turbowasm_reader_u8(turbowasm_reader *reader, uint8_t *out);
bool turbowasm_reader_u32le(turbowasm_reader *reader, uint32_t *out);
bool turbowasm_reader_uleb32(turbowasm_reader *reader, uint32_t *out);

#endif /* TURBOWASM_READER_H */
