#ifndef TURBOWASM_COMPONENT_STRING_H
#define TURBOWASM_COMPONENT_STRING_H

#include "component_canonical.h"

bool turbowasm_component_string_encoding_valid(turbowasm_component_string_encoding encoding);
turbowasm_status turbowasm_component_string_validate(const turbowasm_component_owned_string *string);
turbowasm_status turbowasm_component_string_lift(
    const turbowasm_component_canonical_memory *memory, uint64_t pointer,
    uint64_t tagged_length, turbowasm_component_owned_string *out);
turbowasm_status turbowasm_component_string_lower(
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_component_owned_string *string, uint64_t *out_pointer,
    uint64_t *out_length);

#endif
