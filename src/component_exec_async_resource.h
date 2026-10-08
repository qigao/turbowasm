#ifndef TURBOWASM_COMPONENT_EXEC_ASYNC_RESOURCE_H
#define TURBOWASM_COMPONENT_EXEC_ASYNC_RESOURCE_H
#include "component_canonical.h"

struct turbowasm_component_exec;
struct turbowasm_component_async_resource_owner;
struct turbowasm_component_resource_identity;
struct turbowasm_component_task;
typedef struct turbowasm_component_exec_resource_codec {
    struct turbowasm_component_exec *exec;
    struct turbowasm_component_task *borrow_scope;
    struct turbowasm_component_async_resource_owner *lower_head;
} turbowasm_component_exec_resource_codec;

turbowasm_status turbowasm_component_exec_resource_imports_init(struct turbowasm_component_exec *exec);
void turbowasm_component_exec_resource_imports_destroy(struct turbowasm_component_exec *exec);
const struct turbowasm_component_resource_identity *turbowasm_component_exec_resource_identity(
    const struct turbowasm_component_exec *exec, uint64_t declaration);

/* Owner-thread-only instantiated-resource conversion. Lifted values retain the exec;
 * borrowed source handles stay lent until their values are destroyed at terminal
 * delivery. Each lower scope belongs to one retained call direction. */
void turbowasm_component_exec_resource_codec_bind(turbowasm_component_exec_resource_codec *codec,
    struct turbowasm_component_exec *exec, turbowasm_component_canonical_memory *memory);
turbowasm_status turbowasm_component_exec_resource_codec_preflight(const turbowasm_component_exec_resource_codec *codec);
turbowasm_status turbowasm_component_exec_resource_codec_commit(turbowasm_component_exec_resource_codec *codec);
turbowasm_status turbowasm_component_exec_resource_codec_rollback(turbowasm_component_exec_resource_codec *codec);
#endif
