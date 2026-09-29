#include <turbowasm/turbowasm.h>

#include <type_traits>

static_assert(sizeof(salts_v128) == 16u, "Salts SIMD carrier must be 128 bits");
static_assert(std::is_standard_layout<turbowasm_v128>::value,
              "TurboWasm v128 must remain standard-layout");
static_assert(std::is_standard_layout<turbowasm_funcref>::value,
              "TurboWasm funcref must remain standard-layout");
static_assert(std::is_standard_layout<turbowasm_name>::value,
              "TurboWasm name spans must remain standard-layout");
static_assert(std::is_standard_layout<turbowasm_import_desc>::value,
              "TurboWasm import descriptors must remain standard-layout");
static_assert(std::is_standard_layout<turbowasm_export_desc>::value,
              "TurboWasm export descriptors must remain standard-layout");
static_assert(std::is_standard_layout<turbowasm_linker>::value,
              "TurboWasm linker handle must remain standard-layout");

auto *tw_artifact_measure = &turbowasm_module_artifact_measure;
auto *tw_artifact_write = &turbowasm_module_artifact_write;
auto *tw_artifact_restore = &turbowasm_module_load_borrowed_from_artifact;
auto *tw_artifact_restore_config =
    &turbowasm_module_load_borrowed_from_artifact_with_config;

int main() {
    turbowasm_module module{};
    turbowasm_module_destroy(&module);
    return tw_artifact_measure == nullptr ||
           tw_artifact_write == nullptr ||
           tw_artifact_restore == nullptr ||
           tw_artifact_restore_config == nullptr;
}
