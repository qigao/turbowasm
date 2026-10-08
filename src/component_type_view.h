#ifndef TURBOWASM_COMPONENT_TYPE_VIEW_H
#define TURBOWASM_COMPONENT_TYPE_VIEW_H
#include "component_binary.h"

typedef struct turbowasm_component_resource_identity {
    uint64_t declaration;
    const void *runtime;
} turbowasm_component_resource_identity;

/* Private instance view. Only type-node arrays, nested instance headers and
 * identity tokens are owned; all other metadata borrows source. Tokens remain
 * stable until destroy, after every borrower of binary/type_graph has retired. */
typedef struct turbowasm_component_type_view {
    turbowasm_component_binary binary;
    turbowasm_component_resource_identity *identities;
    uint32_t identity_count;
} turbowasm_component_type_view;

/* Returns a NULL view for resource-free input. Failure preserves *out == NULL
 * and the decoded source. Inputs are validated immutable decoded metadata. */
turbowasm_status turbowasm_component_type_view_create(
    const turbowasm_component_binary *source, turbowasm_component_type_view **out);
void turbowasm_component_type_view_destroy(turbowasm_component_type_view *view);
#endif
