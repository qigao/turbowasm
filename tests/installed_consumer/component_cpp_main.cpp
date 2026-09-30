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
auto *tw_component_value_destroy =
    &turbowasm_component_host_value_destroy;

int main() {
    return tw_component_load == nullptr ||
           tw_component_load_config == nullptr ||
           tw_component_destroy == nullptr ||
           tw_component_instance_create == nullptr ||
           tw_component_instance_destroy == nullptr ||
           tw_component_invoke == nullptr ||
           tw_component_value_destroy == nullptr;
}
