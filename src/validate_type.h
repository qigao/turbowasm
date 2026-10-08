#ifndef TURBOWASM_VALIDATE_TYPE_H
#define TURBOWASM_VALIDATE_TYPE_H

#include <turbowasm/status.h>

#include "reader.h"
#include "validation_context.h"

#include <stdbool.h>

turbowasm_status turbowasm_validation_read_heaptype(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_validation_value_type *out);

turbowasm_status turbowasm_validation_read_reftype(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_validation_value_type *out,
    bool *out_generalized);

turbowasm_status turbowasm_validation_read_valtype(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_validation_value_type *out,
    bool *out_generalized);

turbowasm_status turbowasm_validation_read_globaltype(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_validation_value_type *out_type,
    bool *out_mutable);

turbowasm_status turbowasm_validate_composite_types(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context);

turbowasm_status turbowasm_validation_types_finalize(
    turbowasm_validation_context *context);

bool turbowasm_validation_bind_value(
    const turbowasm_validation_context *context,
    turbowasm_validation_value_type *value);

turbowasm_status turbowasm_validation_clone_types(
    const turbowasm_validation_context *source,
    turbowasm_validation_context *target);
bool turbowasm_validation_types_size(
    const turbowasm_validation_context *source,size_t *out);

#endif /* TURBOWASM_VALIDATE_TYPE_H */
