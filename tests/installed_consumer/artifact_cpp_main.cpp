#include <turbowasm/turbowasm.h>

#include <array>
#include <cstddef>
#include <cstdint>

int main() {
    static const std::uint8_t module_bytes[] = {
        0x00, 0x61, 0x73, 0x6d,
        0x01, 0x00, 0x00, 0x00
    };

    auto *measure = &turbowasm_module_artifact_measure;
    auto *write = &turbowasm_module_artifact_write;
    auto *restore = &turbowasm_module_load_borrowed_from_artifact;
    if (measure == nullptr || write == nullptr || restore == nullptr)
        return 1;

    turbowasm_module fresh{};
    turbowasm_module restored{};
    std::array<std::uint8_t, 4096> artifact{};
    std::size_t required = 0u;
    std::size_t written = 0u;

    if (turbowasm_module_load_borrowed(
            &fresh, module_bytes, sizeof(module_bytes)) != TURBOWASM_OK)
        return 2;
    if (turbowasm_module_artifact_measure(
            &fresh, &required) != TURBOWASM_OK ||
        required == 0u || required > artifact.size()) {
        turbowasm_module_destroy(&fresh);
        return 3;
    }
    if (turbowasm_module_artifact_write(
            &fresh, artifact.data(), artifact.size(), &written) !=
            TURBOWASM_OK ||
        written != required) {
        turbowasm_module_destroy(&fresh);
        return 4;
    }
    if (turbowasm_module_load_borrowed_from_artifact(
            &restored,
            module_bytes,
            sizeof(module_bytes),
            artifact.data(),
            written) != TURBOWASM_OK) {
        turbowasm_module_destroy(&fresh);
        return 5;
    }

    turbowasm_module_destroy(&restored);
    turbowasm_module_destroy(&fresh);
    return 0;
}
