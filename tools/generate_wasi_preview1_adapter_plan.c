#include <data_bind_cmeta_adapter_plan.h>
#include <turbowasm/wasi.h>

#include <stdio.h>

static const cmeta_function_desc *preview1_at(
    void *context, size_t index) {
    (void)context;
    return turbowasm_wasi_preview1_function_at(index);
}

static bool file_write(void *context, const char *data, size_t size) {
    return fwrite(data, 1u, size, (FILE *)context) == size;
}

int main(int argc, char **argv) {
    DataBindCMetaFunctionManifest manifest = {
        turbowasm_wasi_preview1_function_count(),
        preview1_at,
        NULL
    };
    DataBindCMetaAdapterPlanConfig config = {
        "turbowasm_wasi_preview1"
    };
    DataBindCMetaAdapterPlanStatus status;
    size_t error_index = SIZE_MAX;
    FILE *file;

    if (argc != 2)
        return 2;
    file = fopen(argv[1], "wb");
    if (file == NULL)
        return 3;

    status = data_bind_cmeta_adapter_plan_emit(
        &manifest, &config, file_write, file, &error_index);
    if (fclose(file) != 0)
        return 4;

    if (status != DATA_BIND_CMETA_ADAPTER_PLAN_OK) {
        fprintf(stderr, "adapter-plan generation failed: %s at index %zu\n",
                data_bind_cmeta_adapter_plan_status_name(status),
                error_index);
        return 5;
    }
    return error_index == SIZE_MAX ? 0 : 6;
}
