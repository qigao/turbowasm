#ifndef TURBOWASM_VALIDATE_H
#define TURBOWASM_VALIDATE_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>

#include "reader.h"
#include "validation_context.h"

turbowasm_status turbowasm_validate_sections(
    turbowasm_reader *reader,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context);

#endif /* TURBOWASM_VALIDATE_H */
