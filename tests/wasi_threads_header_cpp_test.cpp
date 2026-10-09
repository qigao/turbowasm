#include <turbowasm/wasi_threads.h>

#include <type_traits>

static_assert(std::is_standard_layout<turbowasm_wasi_threads>::value,
              "WASIThreads handle must remain standard-layout");
static_assert(
    std::is_standard_layout<turbowasm_wasi_threads_execution_policy>::value,
    "WASIThreads execution policy must remain standard-layout");
static_assert(std::is_standard_layout<turbowasm_wasi_threads_config>::value,
              "WASIThreads config must remain standard-layout");

auto *tw_threads_policy_init =
    &turbowasm_wasi_threads_execution_policy_init;
auto *tw_threads_policy_apply =
    &turbowasm_wasi_threads_execution_policy_apply;
auto *tw_threads_init = &turbowasm_wasi_threads_init;
auto *tw_threads_init_pool = &turbowasm_wasi_threads_init_pool;
auto *tw_threads_destroy = &turbowasm_wasi_threads_destroy;
auto *tw_threads_define = &turbowasm_wasi_threads_define;
auto *tw_threads_active = &turbowasm_wasi_threads_active;
auto *tw_threads_group_fatal = &turbowasm_wasi_threads_group_fatal;
auto *tw_threads_proc_exit = &turbowasm_wasi_threads_proc_exit;
auto *tw_threads_group_exit_code =
    &turbowasm_wasi_threads_group_exit_code;
auto *tw_threads_group_exit = &turbowasm_wasi_threads_group_exit;

int main() {
    return tw_threads_policy_init == nullptr ||
           tw_threads_policy_apply == nullptr ||
           tw_threads_init == nullptr ||
           tw_threads_init_pool == nullptr ||
           tw_threads_destroy == nullptr ||
           tw_threads_define == nullptr ||
           tw_threads_active == nullptr ||
           tw_threads_group_fatal == nullptr ||
           tw_threads_proc_exit == nullptr ||
           tw_threads_group_exit_code == nullptr ||
           tw_threads_group_exit == nullptr;
}
