/* Freestanding test guest. No Metallic or WASI dependency. */
#include <stdint.h>
__attribute__((import_module("fixture:db"), import_name("admit")))
extern int32_t admit(uint32_t opcode, uint64_t owner, uint64_t address, uint64_t length);
__attribute__((export_name("check")))
int32_t check(uint32_t opcode, uint64_t owner, uint64_t address, uint64_t length) {
    return admit(opcode, owner, address, length);
}
__attribute__((export_name("put")))
void put(uint32_t address, uint32_t value) {
    *(volatile uint8_t *)(uintptr_t)address = (uint8_t)value;
}
