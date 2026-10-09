#ifndef METALLIC_FILE_H
#define METALLIC_FILE_H

#include <stddef.h>
#include <limits.h>
#ifdef __METALLIC_THREADS__
#include <stdatomic.h>
#endif

typedef long long off_t;
typedef struct __FILE FILE;

struct __FILE
{
    unsigned state;
    unsigned avail;
    unsigned char cache[4 * MB_LEN_MAX];

    unsigned char* base;
    unsigned char* ptr;
    unsigned char* end;

    size_t (*read)(FILE*, void*, size_t);
    size_t (*write)(FILE*, const void*, size_t);
    off_t (*seek)(FILE*, off_t, int);
    int (*close)(FILE*);

    int fd;
    signed char orient;  /* -1 byte, 0 unset, +1 wide */
    unsigned pushback;   /* one-slot ungetwc buffer (live iff wpushbit_ in state) */

    /* Descriptor streams only. Formatting pseudo-streams use base/ptr/end. */
    unsigned char* buffer;
    size_t capacity;
    size_t read_pos, read_end, write_end;
    unsigned buffer_owned, io_started;
    int buffer_mode;
    FILE* flush_next;
#ifdef __METALLIC_THREADS__
    atomic_uint lock_owner;
    unsigned lock_depth;
    atomic_uint flush_refs;
    unsigned retired;
    unsigned descriptor_stream; /* Immutable after construction, including reopen. */
#endif
};

typedef struct { FILE *stream; unsigned active; } metallic_stdio_guard;
#ifdef __METALLIC_THREADS__
metallic_stdio_guard __stdio_operation_enter(FILE *, int input);
void __stdio_operation_leave(metallic_stdio_guard *);
void __stdio_wait_flush_refs(FILE *);
void __stdio_reset(FILE *, const FILE *);
#else
static inline metallic_stdio_guard __stdio_operation_enter(FILE *stream, int input) {
    (void)input;
    return (metallic_stdio_guard){stream, 0};
}
static inline void __stdio_operation_leave(metallic_stdio_guard *guard) { (void)guard; }
static inline void __stdio_wait_flush_refs(FILE *stream) { (void)stream; }
static inline void __stdio_reset(FILE *stream, const FILE *fresh) { *stream = *fresh; }
#endif
/* The guest toolchain is LLVM. A cleanup guard covers every ordinary return,
 * including formatter/scanner error exits, without a libc -> CMeta dependency.
 * fclose/freopen release it explicitly before freeing their FILE. */
#define METALLIC_STDIO_GUARD(stream_, input_) \
    metallic_stdio_guard stdio_guard __attribute__((cleanup(__stdio_operation_leave))) = \
        __stdio_operation_enter((stream_), (input_))

static inline void __stdio_retire(FILE *stream) {
#ifdef __METALLIC_THREADS__
    stream->retired = 1;
#else
    (void)stream;
#endif
}

int __stdio_flush(FILE*);
int __stdio_flush_all(int line_only);
void __stdio_buffer_release(FILE*);
void __stdio_buffer_register(FILE*);
int __stdio_position(FILE*, off_t, int);
size_t __stdio_write_raw(FILE*, const void*, size_t);

enum
{
    eofbit_ = 0x10,
    errbit_ = 0x20,
    appbit_ = 0x80,
    wpushbit_ = 0x100,  /* ungetwc has a pushback character ready */
};

#endif
