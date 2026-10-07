#include <turbowasm/turbowasm.h>

#include <cstddef>
#include <type_traits>

constexpr std::size_t v128_storage_bytes = 16u;
constexpr std::size_t v128_value_bytes = 2u * v128_storage_bytes;
static_assert(sizeof(cmeta_v128) == v128_storage_bytes,
              "Salts SIMD carrier must be 128 bits");
static_assert(alignof(cmeta_v128) == v128_storage_bytes,
              "Salts SIMD carrier alignment is part of the public ABI");
static_assert(offsetof(turbowasm_v128, bits) == 0u &&
                  offsetof(turbowasm_v128, shape) == v128_storage_bytes &&
                  sizeof(turbowasm_v128) == v128_value_bytes,
              "Salts API migration must preserve the TurboWasm value layout");
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
static_assert(std::is_standard_layout<turbowasm_store>::value &&
              std::is_standard_layout<turbowasm_root>::value &&
              std::is_standard_layout<turbowasm_gcref>::value,
              "Managed-reference handles must remain C-compatible");

auto *tw_artifact_measure = &turbowasm_module_artifact_measure;
auto *tw_artifact_write = &turbowasm_module_artifact_write;
auto *tw_artifact_restore = &turbowasm_module_load_borrowed_from_artifact;
auto *tw_artifact_restore_config =
    &turbowasm_module_load_borrowed_from_artifact_with_config;

int main() {
    turbowasm_store store{};
    turbowasm_root root{};
    turbowasm_value value{};
    value.kind = TURBOWASM_VALUE_GCREF;
    if (turbowasm_store_create(&store, nullptr) != TURBOWASM_OK ||
        turbowasm_root_retain(&store, &value, &root) != TURBOWASM_OK ||
        turbowasm_root_release(&root) != TURBOWASM_OK ||
        turbowasm_store_destroy(&store) != TURBOWASM_OK)
        return 1;
    turbowasm_module module{};
    turbowasm_module_destroy(&module);
    return tw_artifact_measure == nullptr ||
           tw_artifact_write == nullptr ||
           tw_artifact_restore == nullptr ||
           tw_artifact_restore_config == nullptr;
}
