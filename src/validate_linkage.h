#ifndef TURBOWASM_VALIDATE_LINKAGE_H
#define TURBOWASM_VALIDATE_LINKAGE_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>

#include "module_ir.h"
#include "reader.h"

turbowasm_status turbowasm_validate_import_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_module_ir *ir);
turbowasm_status turbowasm_validate_table_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_module_ir *ir);
turbowasm_status turbowasm_validate_memory_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_module_ir *ir);
turbowasm_status turbowasm_validate_export_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary);
turbowasm_status turbowasm_validate_start_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    const turbowasm_module_ir *ir);
turbowasm_status turbowasm_validate_data_count_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary);

#endif /* TURBOWASM_VALIDATE_LINKAGE_H */
