#include <turbowasm/turbowasm.h>

#include <type_traits>

static_assert(sizeof(salts_v128) == 16u, "Salts SIMD carrier must be 128 bits");
static_assert(std::is_standard_layout<turbowasm_v128>::value,
              "TurboWasm v128 must remain standard-layout");
static_assert(std::is_standard_layout<turbowasm_name>::value,
              "TurboWasm name spans must remain standard-layout");
static_assert(std::is_standard_layout<turbowasm_import_desc>::value,
              "TurboWasm import descriptors must remain standard-layout");
static_assert(std::is_standard_layout<turbowasm_export_desc>::value,
              "TurboWasm export descriptors must remain standard-layout");

int main() {
    turbowasm_module module{};
    turbowasm_module_destroy(&module);
    return 0;
}
