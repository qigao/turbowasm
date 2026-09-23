#include <turbowasm/turbowasm.h>

#include <string.h>

int main(void) {
    const char *status = turbowasm_status_string(TURBOWASM_OK);
    return status != NULL && strcmp(status, "ok") == 0 ? 0 : 1;
}
