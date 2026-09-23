#ifndef TURBOWASM_VALIDATE_LINKAGE_H
#define TURBOWASM_VALIDATE_LINKAGE_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>

#include "reader.h"
#include "validation_context.h"

turbowasm_status turbowasm_validate_import_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context);
turbowasm_status turbowasm_validate_table_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context);
turbowasm_status turbowasm_validate_memory_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context);
turbowasm_status turbowasm_validate_export_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context);
turbowasm_status turbowasm_validate_start_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    const turbowasm_validation_context *context);
turbowasm_status turbowasm_validate_data_count_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context);

#endif /* TURBOWASM_VALIDATE_LINKAGE_H */
