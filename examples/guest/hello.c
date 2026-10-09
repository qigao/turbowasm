#include <stdio.h>
#include <stdlib.h>

static void goodbye(void) { puts("guest cleanup complete"); }
int main(int argc, char **argv) {
    if (atexit(goodbye)) return EXIT_FAILURE;
    printf("Hello from C11: %s (%d arguments)\n", argv[0], argc);
    return 0;
}
