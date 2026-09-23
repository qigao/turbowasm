#ifndef TURBOWASM_VALIDATE_ELEMENT_H
#define TURBOWASM_VALIDATE_ELEMENT_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>

#include "reader.h"
#include "validation_context.h"

turbowasm_status turbowasm_validate_element_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context);

#endif /* TURBOWASM_VALIDATE_ELEMENT_H */
