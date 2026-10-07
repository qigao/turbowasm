#include <turbowasm/turbowasm.h>

/* (module (type (struct (field i32)))
 *   (func (result anyref) (struct.new_default 0))) */
static const uint8_t module_bytes[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x09, 0x02, 0x5f, 0x01, 0x7f, 0x00, 0x60,
    0x00, 0x01, 0x6e, 0x03, 0x02, 0x01, 0x01, 0x0a, 0x07, 0x01, 0x05, 0x00, 0xfb, 0x01, 0x00, 0x0b};

int main(void) {
    turbowasm_store store = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_root root = {0};
    turbowasm_value value = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t count = 0u;
    int result = 1;

    if (turbowasm_store_create(&store, NULL) != TURBOWASM_OK ||
        turbowasm_module_load_borrowed(&module, module_bytes, sizeof(module_bytes)) !=
            TURBOWASM_OK ||
        turbowasm_instance_create_in_store(&instance, &module, NULL, &store) != TURBOWASM_OK ||
        turbowasm_instance_invoke(&instance, 0u, NULL, 0u, &value, 1u, &count, &trap) !=
            TURBOWASM_OK ||
        count != 1u || turbowasm_root_retain(&store, &value, &root) != TURBOWASM_OK)
        goto cleanup;

    /* The root owns reachability; the struct's type belongs to the store. */
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    if (turbowasm_store_collect(&store) != TURBOWASM_OK ||
        turbowasm_root_get(&root, &value) != TURBOWASM_OK || value.kind != TURBOWASM_VALUE_GCREF)
        goto cleanup;
    result = 0;

cleanup:
    if (root.impl != NULL && turbowasm_root_release(&root) != TURBOWASM_OK)
        result = 1;
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    if (store.impl != NULL && turbowasm_store_destroy(&store) != TURBOWASM_OK)
        result = 1;
    return result;
}
