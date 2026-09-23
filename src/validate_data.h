#ifndef TURBOWASM_VALIDATE_DATA_H
#define TURBOWASM_VALIDATE_DATA_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>

#include "reader.h"
#include "validation_context.h"

turbowasm_status turbowasm_validate_const_expr(
    turbowasm_reader *reader,
    turbowasm_validation_context *context,
    uint8_t *out_type);

turbowasm_status turbowasm_validate_global_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context);
turbowasm_status turbowasm_validate_data_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context);

#endif /* TURBOWASM_VALIDATE_DATA_H */
