#ifndef TURBOWASM_VALIDATE_SEGMENTS_H
#define TURBOWASM_VALIDATE_SEGMENTS_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>

#include "module_ir.h"
#include "reader.h"

turbowasm_status turbowasm_validate_global_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_module_ir *ir);

turbowasm_status turbowasm_validate_element_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    const turbowasm_module_ir *ir);

turbowasm_status turbowasm_validate_data_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    const turbowasm_module_ir *ir);

#endif /* TURBOWASM_VALIDATE_SEGMENTS_H */
