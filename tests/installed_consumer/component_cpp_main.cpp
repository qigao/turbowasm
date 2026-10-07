#include <turbowasm/component.h>

#include <type_traits>

static_assert(
    std::is_standard_layout<turbowasm_component>::value,
    "Component handle must remain standard-layout");
static_assert(
    std::is_standard_layout<turbowasm_component_instance>::value,
    "Component instance handle must remain standard-layout");
static_assert(
    std::is_standard_layout<turbowasm_component_host_value>::value,
    "Component host value must remain standard-layout");
static_assert(
    std::is_standard_layout<turbowasm_component_call>::value,
    "Component call handle must remain standard-layout");

static_assert(std::is_same<decltype(&turbowasm_component_host_value_destroy),
    turbowasm_status (*)(turbowasm_component_host_value *)>::value,
    "Component value destruction reports status");
static_assert(std::is_standard_layout<turbowasm_component_host_variant>::value &&
    std::is_standard_layout<turbowasm_component_host_sequence>::value &&
    std::is_standard_layout<turbowasm_component_host_flags>::value,
    "Composite carriers must remain C-compatible");

auto *tw_component_load = &turbowasm_component_load_borrowed;
auto *tw_component_load_config =
    &turbowasm_component_load_borrowed_with_config;
auto *tw_component_destroy = &turbowasm_component_destroy;
auto *tw_component_instance_create =
    &turbowasm_component_instance_create;
auto *tw_component_instance_destroy =
    &turbowasm_component_instance_destroy;
auto *tw_component_invoke =
    &turbowasm_component_instance_invoke;
auto *tw_component_call_create =
    &turbowasm_component_call_create;
auto *tw_component_call_destroy =
    &turbowasm_component_call_destroy;
auto *tw_component_call_resume =
    &turbowasm_component_call_resume;
auto *tw_component_call_state_get =
    &turbowasm_component_call_state_get;
auto *tw_component_call_yield_reason_get =
    &turbowasm_component_call_yield_reason_get;
auto *tw_component_call_pending_host_wait =
    &turbowasm_component_call_pending_host_wait;
auto *tw_component_call_complete_host_wait =
    &turbowasm_component_call_complete_host_wait;
auto *tw_component_call_terminal_status =
    &turbowasm_component_call_terminal_status;
auto *tw_component_call_trap =
    &turbowasm_component_call_trap;
auto *tw_component_call_result_count =
    &turbowasm_component_call_result_count;
auto *tw_component_call_take_result =
    &turbowasm_component_call_take_result;
auto *tw_component_value_destroy =
    &turbowasm_component_host_value_destroy;

int main() {
    return tw_component_load == nullptr ||
           tw_component_load_config == nullptr ||
           tw_component_destroy == nullptr ||
           tw_component_instance_create == nullptr ||
           tw_component_instance_destroy == nullptr ||
           tw_component_invoke == nullptr ||
           tw_component_call_create == nullptr ||
           tw_component_call_destroy == nullptr ||
           tw_component_call_resume == nullptr ||
           tw_component_call_state_get == nullptr ||
           tw_component_call_yield_reason_get == nullptr ||
           tw_component_call_pending_host_wait == nullptr ||
           tw_component_call_complete_host_wait == nullptr ||
           tw_component_call_terminal_status == nullptr ||
           tw_component_call_trap == nullptr ||
           tw_component_call_result_count == nullptr ||
           tw_component_call_take_result == nullptr ||
           tw_component_value_destroy == nullptr;
}
