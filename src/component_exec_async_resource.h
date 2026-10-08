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

/* Private host admission adapter. Does not consume the original owner on
 * allocation failure. A successful value borrows admitted/context until release;
 * lowering requires *admitted. Published runs after ownership publication and
 * must not fail, suspend or run guest code. Finish runs once, after all codec reservations
 * have ended, and distinguishes guest publication from cancellation. It must
 * preserve the original owner when admission itself has not committed. */
typedef turbowasm_status (*turbowasm_component_resource_host_finish_fn)(
    void *context, bool published);
typedef void (*turbowasm_component_resource_host_published_fn)(void *context);
size_t turbowasm_component_exec_resource_adopt_size(void);
turbowasm_status turbowasm_component_exec_resource_adopt(
    struct turbowasm_component_exec *exec, turbowasm_component_value *value,
    const bool *admitted, turbowasm_component_resource_host_published_fn published,
    turbowasm_component_resource_host_finish_fn finish,
    void *context);
bool turbowasm_component_exec_resource_value_idle(const turbowasm_component_value *value);
/* Independent own result: fresh, idle and owned by this exec; admission proxies
 * borrow their original owner and cannot escape as independent results. */
bool turbowasm_component_exec_resource_value_owned(
    const struct turbowasm_component_exec *exec, const turbowasm_component_value *value);
/* Fresh owner with an independent public-domain instance keepalive. */
bool turbowasm_component_exec_resource_value_retained(const turbowasm_component_value *value);
/* Infallible after value_owned admission, when exclusive ownership is published.
 * Drops canonical storage/quota without destroying the now-transferred rep. */
void turbowasm_component_exec_resource_value_disown(turbowasm_component_value *value);

/* Owner-thread-only instantiated-resource conversion. Lifted values retain the exec;
 * borrowed source handles stay lent until their values are destroyed at terminal
 * delivery. Each lower scope belongs to one retained call direction. */
void turbowasm_component_exec_resource_codec_bind(turbowasm_component_exec_resource_codec *codec,
    struct turbowasm_component_exec *exec, turbowasm_component_canonical_memory *memory);
turbowasm_status turbowasm_component_exec_resource_codec_preflight(const turbowasm_component_exec_resource_codec *codec);
turbowasm_status turbowasm_component_exec_resource_codec_commit(turbowasm_component_exec_resource_codec *codec);
turbowasm_status turbowasm_component_exec_resource_codec_rollback(turbowasm_component_exec_resource_codec *codec);
#endif
