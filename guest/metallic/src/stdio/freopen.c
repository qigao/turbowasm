#include "flush.h"
#include "modeflags.h"
#include "FILE_.h"
#include "../wasi/wasi.h"
#include "../wasi/errno.h"
#include <stdlib.h>

int open_path_(const char *path, int flags);

FILE* freopen(const char *restrict path, const char mode[restrict static 1], FILE stream[restrict static 1])
{
    int flags = modeflags_(mode);
    flush_(stream);

    if (path) {
        stream->close(stream);
        int fd = open_path_(path, flags);

        if (fd >= 0) {
            *stream = FILE_(fd, .state = flags & O_APPEND ? appbit_ : 0);
            return stream;
        }
    }
    else {
        /* This profile permits append changes, not access changes or truncate
         * through a null pathname. C11 leaves this admission policy defined by
         * the implementation; never report a mode change that did not happen. */
        __wasi_fdstat_t current;
        __wasi_errno_t e = __wasi_fd_fdstat_get((__wasi_fd_t)stream->fd, &current);
        if (e) { wasi_seterrno(e); goto failed; }
        int access = flags & O_ACCMODE;
        int read = access == O_RDONLY || access == O_RDWR;
        int write = access == O_WRONLY || access == O_RDWR;
        if ((flags & O_TRUNC) ||
            read != !!(current.fs_rights_base & __WASI_RIGHTS_FD_READ) ||
            write != !!(current.fs_rights_base & __WASI_RIGHTS_FD_WRITE)) {
            errno = ENOTSUP;
            goto failed;
        }
        /* WASI has no FD_CLOEXEC concept; O_CLOEXEC has no effect here. */
        __wasi_fdflags_t f = 0;
        if (flags & O_APPEND)   f |= __WASI_FDFLAGS_APPEND;
        if (flags & O_NONBLOCK) f |= __WASI_FDFLAGS_NONBLOCK;
        if (flags & O_DSYNC)    f |= __WASI_FDFLAGS_DSYNC;
        if (flags & O_SYNC)     f |= __WASI_FDFLAGS_SYNC;

        e = __wasi_fd_fdstat_set_flags((__wasi_fd_t)stream->fd, f);
        if (!e) {
            *stream = FILE_(stream->fd, .state = flags & O_APPEND ? appbit_ : 0);
            return stream;
        }
        errno = wasi_to_posix[e];
failed:
        stream->close(stream);
    }

    if (stream != stdin && stream != stdout && stream != stderr) free(stream);
    return (void*)0;
}
