#ifndef TURBOWASM_WASI_FS_H
#define TURBOWASM_WASI_FS_H

#include <turbowasm/wasi.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbowasm_wasi_fs {
    void *impl;
} turbowasm_wasi_fs;

/*
 * Provider-owned opaque file identity. It is intentionally not a native OS
 * handle. Providers choose object/generation semantics and keep the underlying
 * resource lifecycle private.
 */
typedef struct turbowasm_wasi_fs_file {
    uint64_t object;
    uint32_t generation;
} turbowasm_wasi_fs_file;

/*
 * Generation-checked descriptor identity used by the WASI layer internally.
 * guest_fd is not generation-safe by itself; async work should retain this
 * handle and validate it before acting on a reused table slot.
 */
typedef struct turbowasm_wasi_fs_descriptor {
    uint32_t slot;
    uint32_t generation;
} turbowasm_wasi_fs_descriptor;

typedef struct turbowasm_wasi_fs_stat {
    uint64_t size;
    uint64_t modified_ns;
    uint8_t file_type;
} turbowasm_wasi_fs_stat;

typedef uint32_t (*turbowasm_wasi_fs_close_fn)(
    void *context,
    turbowasm_wasi_fs_file file);

typedef uint32_t (*turbowasm_wasi_fs_read_fn)(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read);

typedef uint32_t (*turbowasm_wasi_fs_write_fn)(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written);

typedef uint32_t (*turbowasm_wasi_fs_seek_fn)(
    void *context,
    turbowasm_wasi_fs_file file,
    int64_t offset,
    uint8_t whence,
    uint64_t *out_offset);

typedef uint32_t (*turbowasm_wasi_fs_tell_fn)(
    void *context,
    turbowasm_wasi_fs_file file,
    uint64_t *out_offset);

typedef uint32_t (*turbowasm_wasi_fs_stat_fn)(
    void *context,
    turbowasm_wasi_fs_file file,
    turbowasm_wasi_fs_stat *out_stat);

typedef uint32_t (*turbowasm_wasi_fs_path_open_fn)(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t dirflags,
    const uint8_t *path,
    size_t path_length,
    uint32_t oflags,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    uint32_t fdflags,
    turbowasm_wasi_fs_file *out_file);

typedef struct turbowasm_wasi_fs_provider {
    void *context;
    turbowasm_wasi_fs_close_fn close;
    turbowasm_wasi_fs_read_fn read;
    turbowasm_wasi_fs_write_fn write;
    turbowasm_wasi_fs_seek_fn seek;
    turbowasm_wasi_fs_tell_fn tell;
    turbowasm_wasi_fs_stat_fn stat;
    turbowasm_wasi_fs_path_open_fn path_open;
} turbowasm_wasi_fs_provider;

typedef struct turbowasm_wasi_fs_config {
    size_t descriptor_capacity;
    turbowasm_wasi_fs_provider provider;
} turbowasm_wasi_fs_config;

typedef struct turbowasm_wasi_fs_descriptor_info {
    turbowasm_wasi_fs_descriptor descriptor;
    uint32_t guest_fd;
    turbowasm_wasi_fs_file file;
    bool preopen;
    const char *guest_path;
    uint64_t rights_base;
    uint64_t rights_inheriting;
} turbowasm_wasi_fs_descriptor_info;

turbowasm_status turbowasm_wasi_fs_init(
    turbowasm_wasi_fs *filesystem,
    const turbowasm_wasi_fs_config *config);

/*
 * Destroy requires all descriptors to have been closed explicitly.
 * This keeps provider close errors observable and exactly-once.
 */
turbowasm_status turbowasm_wasi_fs_destroy(
    turbowasm_wasi_fs *filesystem);

/*
 * Bind one provider identity to an explicit guest fd. guest_path is copied
 * when preopen is true and must be non-empty. Table capacity never grows.
 */
turbowasm_status turbowasm_wasi_fs_bind_descriptor(
    turbowasm_wasi_fs *filesystem,
    uint32_t guest_fd,
    turbowasm_wasi_fs_file file,
    bool preopen,
    const char *guest_path,
    turbowasm_wasi_fs_descriptor *out_descriptor);

turbowasm_status turbowasm_wasi_fs_bind_descriptor_with_rights(
    turbowasm_wasi_fs *filesystem,
    uint32_t guest_fd,
    turbowasm_wasi_fs_file file,
    bool preopen,
    const char *guest_path,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    turbowasm_wasi_fs_descriptor *out_descriptor);

turbowasm_status turbowasm_wasi_fs_bind_next_descriptor(
    turbowasm_wasi_fs *filesystem,
    turbowasm_wasi_fs_file file,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    turbowasm_wasi_fs_descriptor *out_descriptor,
    uint32_t *out_guest_fd);

uint32_t turbowasm_wasi_fs_path_open(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    uint32_t dirflags,
    const uint8_t *path,
    size_t path_length,
    uint32_t oflags,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    uint32_t fdflags,
    uint32_t *out_guest_fd);

bool turbowasm_wasi_fs_descriptor_info_get(
    const turbowasm_wasi_fs *filesystem,
    uint32_t guest_fd,
    turbowasm_wasi_fs_descriptor_info *out_info);

/* Generation-safe close for retained async/internal descriptor identities. */
uint32_t turbowasm_wasi_fs_close_descriptor(
    turbowasm_wasi_fs *filesystem,
    turbowasm_wasi_fs_descriptor descriptor);

/* Guest-facing close by current fd identity. */
uint32_t turbowasm_wasi_fs_close_fd(
    turbowasm_wasi_fs *filesystem,
    uint32_t guest_fd);

/*
 * These match turbowasm_wasi_fd_read_fn / turbowasm_wasi_fd_write_fn and can
 * be installed directly into turbowasm_wasi_preview1_config.
 */
uint32_t turbowasm_wasi_fs_fd_read(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read);

uint32_t turbowasm_wasi_fs_fd_write(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written);

uint32_t turbowasm_wasi_fs_fd_seek(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    int64_t offset,
    uint8_t whence,
    uint64_t *out_offset);

uint32_t turbowasm_wasi_fs_fd_tell(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    uint64_t *out_offset);

uint32_t turbowasm_wasi_fs_fd_filestat_get(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    turbowasm_wasi_fs_stat *out_stat);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI_FS_H */
