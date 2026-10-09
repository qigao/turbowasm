#include "cmeta_fixture.h"
#include <cmeta/invoke_decl.h>
#include <cmeta/object.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int invoked, destroyed, restored;
FunctionInvokeDecl(value, int, guest_increment, (int, input, CMETA_PARAM_IN));
int guest_increment(int input) { ++invoked; return input + 1; }

int cmeta_metadata(void) {
    const cmeta_struct_desc *a = StructMeta(guest_record), *b = guest_peer_metadata();
    if (!a || !b || strcmp(a->name, b->name) || a->size != b->size ||
        a->align != b->align || a->field_count != b->field_count) return -1;
    for (size_t i = 0; i < a->field_count; ++i) {
        const cmeta_field_desc *x = &a->fields[i], *y = &b->fields[i];
        if (strcmp(x->name, y->name) || x->offset != y->offset || x->size != y->size ||
            !cmeta_type_equal(x->type, y->type)) return -2;
    }
    return cmeta_struct_find_field(a, "value") && !cmeta_struct_find_field(b, "missing") ? 0 : -3;
}

int cmeta_calls(void) {
    int input = 41, output = 0;
    void *args[] = {&input};
    invoked = 0;
    if (FunctionInvoke(guest_increment)(&output, args, 0) ||
        FunctionInvoke(guest_increment)(NULL, args, 1) ||
        FunctionInvoke(guest_increment)(&output, NULL, 1) || invoked) return -1;
    if (!FunctionInvoke(guest_increment)(&output, args, 1) || output != 42 || invoked != 1) return -2;
    return cmeta_function_abi_desc_valid(FunctionAbi(guest_increment)) ? 0 : -3;
}

static void destroy_int(void *context, void *object) {
    (void)context; ++destroyed; free(object);
}
static const cmeta_object_lifecycle lifetime = {
    .size = sizeof(cmeta_object_lifecycle), .destroy = destroy_int
};
static cmeta_object_ref persistent = CMETA_OBJECT_REF_INIT;

int cmeta_object_open(void) {
    if (persistent.object) return -1;
    int *value = malloc(sizeof(*value));
    if (!value) return -2;
    *value = 42;
    if (cmeta_object_borrow(&persistent, value, &cmeta_data_int, NULL) != CMETA_OK ||
        cmeta_object_take(&persistent, &lifetime) != CMETA_OK) {
        cmeta_object_release(&persistent); free(value); return -3;
    }
    return *value;
}
int cmeta_object_read(void) { return persistent.object ? *(int *)persistent.object : -1; }
int cmeta_object_close(void) {
    cmeta_object_release(&persistent);
    cmeta_object_release(&persistent);
    return destroyed;
}

typedef struct { _Alignas(64) unsigned char bytes[4096]; } aligned_record;
static const cmeta_type_desc aligned_type = {
    .name = "guest.aligned_record", .size = sizeof(aligned_record),
    .align = _Alignof(aligned_record), .kind = CMETA_T_OBJECT
};
static cmeta_status aligned_init(void *value) { memset(value, 0, sizeof(aligned_record)); return CMETA_OK; }
static void aligned_restore(void *value) { ++restored; memset(value, 0, sizeof(aligned_record)); }
static void aligned_move(void *destination, void *source) {
    memcpy(destination, source, sizeof(aligned_record));
    memset(source, 0, sizeof(aligned_record));
}
static const cmeta_data_construct_ops aligned_ops = {
    .struct_size = sizeof(cmeta_data_construct_ops), .abi_version = CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION,
    .storage_type = &aligned_type, .init_zero = aligned_init, .restore_zero = aligned_restore, .move = aligned_move
};
static const cmeta_data_desc aligned_data = {
    .struct_size = sizeof(cmeta_data_desc), .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "guest.aligned_record", .display_name = "aligned record", .kind = CMETA_DATA_CUSTOM,
    .storage_type = &aligned_type, .shape = &aligned_type, .construct_ops = &aligned_ops
};
int cmeta_alignment(void) {
    cmeta_data_temp temp = {0};
    restored = 0;
    if (cmeta_data_temp_open(&aligned_data, sizeof(aligned_record) - 1, &temp) != CMETA_CAPACITY_EXCEEDED || temp.storage) return -1;
    for (int i = 0; i < 512; ++i) {
        if (cmeta_data_temp_open(&aligned_data, sizeof(aligned_record), &temp) != CMETA_OK) return -2;
        if ((uintptr_t)temp.storage % 64 || temp.extent != sizeof(aligned_record)) {
            cmeta_data_temp_close(&temp); return -3;
        }
        unsigned char *bytes = temp.storage;
        if (bytes[0] || bytes[4095]) { cmeta_data_temp_close(&temp); return -4; }
        bytes[0] = 42;
        cmeta_data_temp_close(&temp);
        cmeta_data_temp_close(&temp);
    }
    return restored == 512 ? 0 : -5;
}
