#ifndef TURBOWASM_VALIDATE_DATA_H
#define TURBOWASM_VALIDATE_DATA_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>

#include "reader.h"

turbowasm_status turbowasm_validate_global_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary);
turbowasm_status turbowasm_validate_data_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary);

#endif /* TURBOWASM_VALIDATE_DATA_H */
