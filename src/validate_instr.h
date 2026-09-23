#ifndef TURBOWASM_VALIDATE_INSTR_H
#define TURBOWASM_VALIDATE_INSTR_H

#include <turbowasm/status.h>

#include "reader.h"
#include "validation_context.h"

turbowasm_status turbowasm_validate_function_body(
    turbowasm_reader *body,
    turbowasm_validation_context *context,
    uint32_t function_index);

#endif /* TURBOWASM_VALIDATE_INSTR_H */
