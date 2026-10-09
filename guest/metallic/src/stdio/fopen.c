#include "modeflags.h"
#include "FILE_.h"
#include <stdlib.h>
#include <errno.h>
#include "../wasi/wasi.h"

int open_path_(const char *path, int flags);

FILE* fopen(const char path[restrict static 1], const char mode[restrict static 1])
{
    int flags = modeflags_(mode);
    int fd = open_path_(path, flags);

    if (fd < 0)
        return (void*)0;

    FILE* file = malloc(sizeof(FILE));
    if (!file) {
        __wasi_fd_close((__wasi_fd_t)fd);
        errno = ENOMEM;
        return NULL;
    }
    *file = FILE_(fd, .state = flags & O_APPEND ? appbit_ : 0);
    return file;
}
