#ifndef TURBOWASM_VALIDATE_TYPE_H
#define TURBOWASM_VALIDATE_TYPE_H

#include <turbowasm/status.h>

#include "reader.h"
#include "validation_context.h"

#include <stdbool.h>

turbowasm_status turbowasm_validation_read_reftype(
    turbowasm_reader *reader,
    turbowasm_validation_value_type *out,
    bool *out_generalized);

turbowasm_status turbowasm_validation_read_valtype(
    turbowasm_reader *reader,
    turbowasm_validation_value_type *out,
    bool *out_generalized);

#endif /* TURBOWASM_VALIDATE_TYPE_H */
