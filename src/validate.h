#ifndef TURBOWASM_VALIDATE_H
#define TURBOWASM_VALIDATE_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>

#include "module_ir.h"
#include "reader.h"

turbowasm_status turbowasm_validate_sections(
    turbowasm_reader *reader,
    turbowasm_module_summary *summary,
    turbowasm_module_ir *ir);

#endif /* TURBOWASM_VALIDATE_H */
