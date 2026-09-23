#include <turbowasm/turbowasm.h>

#include <stdio.h>
#include <stdlib.h>

static unsigned char *read_file(const char *path, size_t *size_out) {
    FILE *file;
    long length;
    unsigned char *bytes;

    if (path == NULL || size_out == NULL) return NULL;
    file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    bytes = (unsigned char *)malloc((size_t)length);
    if (bytes == NULL && length != 0) {
        fclose(file);
        return NULL;
    }
    if (length != 0 &&
        fread(bytes, 1u, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (size_t)length;
    return bytes;
}

int main(int argc, char **argv) {
    unsigned char *bytes;
    size_t size = 0u;
    turbowasm_module module = {0};
    turbowasm_status status;

    if (argc != 2) {
        fprintf(stderr, "usage: turbowasm <module.wasm>\n");
        return 2;
    }

    bytes = read_file(argv[1], &size);
    if (bytes == NULL) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return 2;
    }

    status = turbowasm_module_load_borrowed(&module, bytes, size);
    if (status != TURBOWASM_OK) {
        fprintf(stderr, "%s: %s\n", argv[1], turbowasm_status_string(status));
        free(bytes);
        return 1;
    }

    printf("%s: wasm header ok (%zu bytes)\n", argv[1], size);
    turbowasm_module_destroy(&module);
    free(bytes);
    return 0;
}
