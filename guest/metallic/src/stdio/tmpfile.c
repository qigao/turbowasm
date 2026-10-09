#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include "FILE_.h"
#include "../wasi/wasi.h"
#include "../wasi/errno.h"
#include "../wasi/preopen.h"

/* Names are not secrets. Exclusive creation prevents collisions and the
 * preopened directory bounds authority. Unlink before exposing the stream. */
FILE* tmpfile(void)
{
    static uint32_t serial;
    static const char hex[] = "0123456789abcdef";
    char path[] = ".metallic-00000000.tmp";
    FILE *file = malloc(sizeof(*file));
    if (!file) { errno = ENOMEM; return NULL; }
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        uint32_t id = ++serial;
        for (unsigned i = 0; i < 8; ++i)
            path[10 + i] = hex[(id >> (28 - 4 * i)) & 15];
        int base;
        const char *relative;
        size_t length;
        if (preopen_lookup(path, &base, &relative, &length) < 0) break;
        /* Admit deletion before creating anything: a restricted preopen must
         * not leave behind a named file when tmpfile cannot unlink it. */
        __wasi_fdstat_t directory;
        __wasi_errno_t e = __wasi_fd_fdstat_get(base, &directory);
        if (e) { wasi_seterrno(e); break; }
        if (!(directory.fs_rights_base & __WASI_RIGHTS_PATH_UNLINK_FILE)) {
            errno = EACCES;
            break;
        }
        __wasi_fd_t fd;
        __wasi_rights_t rights = __WASI_RIGHTS_FD_READ | __WASI_RIGHTS_FD_WRITE |
            __WASI_RIGHTS_FD_SEEK | __WASI_RIGHTS_FD_TELL | __WASI_RIGHTS_FD_FILESTAT_GET;
        e = __wasi_path_open(base, 0, relative, length,
            __WASI_OFLAGS_CREAT | __WASI_OFLAGS_EXCL, rights, 0, 0, &fd);
        if (e == __WASI_ERRNO_EXIST) { errno = EEXIST; continue; }
        if (e) { wasi_seterrno(e); break; }
        e = __wasi_path_unlink_file(base, relative, length);
        if (e) {
            __wasi_fd_close(fd);
            wasi_seterrno(e);
            break;
        }
        *file = FILE_(fd);
        return file;
    }
    free(file);
    return NULL;
}
