#include "FILE.h"

size_t __stdio_read(FILE[restrict static 1], void* restrict, size_t);
size_t __stdio_write(FILE[restrict static 1], const void* restrict, size_t);
off_t __stdio_seek(FILE[static 1], off_t, int);
int __stdio_close(FILE[static 1]);

#ifdef __METALLIC_THREADS__
#define METALLIC_FILE_THREAD_FIELDS .descriptor_stream = 1,
#else
#define METALLIC_FILE_THREAD_FIELDS
#endif

#define FILE_(_fd, ...) ((FILE){ \
    .read = __stdio_read,        \
    .write = __stdio_write,      \
    .seek = __stdio_seek,        \
    .close = __stdio_close,      \
    .fd = (_fd),                 \
    METALLIC_FILE_THREAD_FIELDS \
    __VA_ARGS__                  \
})
