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

    /* Appended full Preview1 filestat fields; zero is allowed when unknown. */
    uint64_t device;
    uint64_t inode;
    uint64_t link_count;
    uint64_t accessed_ns;
    uint64_t changed_ns;
} turbowasm_wasi_fs_stat;

enum {
    TURBOWASM_WASI_FS_DIRENT_NAME_MAX = 255
};

typedef struct turbowasm_wasi_fs_dirent {
    uint64_t next_cookie;
    uint64_t inode;
    uint32_t name_length;
    uint8_t file_type;
    uint8_t name[TURBOWASM_WASI_FS_DIRENT_NAME_MAX];
} turbowasm_wasi_fs_dirent;

/*
 * Close ownership contract:
 *
 * - returning an errno means the provider identity is still valid and the
 *   caller may retry close;
 * - a backend whose native close consumes its identity even when the native
 *   close reports an error must translate that ownership-consuming close to
 *   TURBOWASM_WASI_ERRNO_SUCCESS.
 *
 * This prevents the descriptor table from retaining a provider identity that
 * no longer exists.
 */
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

typedef uint32_t (*turbowasm_wasi_fs_path_stat_fn)(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t lookup_flags,
    const uint8_t *path,
    size_t path_length,
    turbowasm_wasi_fs_stat *out_stat);

typedef uint32_t (*turbowasm_wasi_fs_path_mutation_fn)(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length);

typedef uint32_t (*turbowasm_wasi_fs_readdir_fn)(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint64_t cookie,
    turbowasm_wasi_fs_dirent *out_entry,
    bool *out_has_entry);

typedef struct turbowasm_wasi_fs_provider {
    void *context;
    turbowasm_wasi_fs_close_fn close;
    turbowasm_wasi_fs_read_fn read;
    turbowasm_wasi_fs_write_fn write;
    turbowasm_wasi_fs_seek_fn seek;
    turbowasm_wasi_fs_tell_fn tell;
    turbowasm_wasi_fs_stat_fn stat;
    turbowasm_wasi_fs_path_open_fn path_open;
    turbowasm_wasi_fs_path_stat_fn path_stat;
    turbowasm_wasi_fs_path_mutation_fn path_create_directory;
    turbowasm_wasi_fs_path_mutation_fn path_remove_directory;
    turbowasm_wasi_fs_path_mutation_fn path_unlink_file;
    turbowasm_wasi_fs_readdir_fn readdir;
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
 * Retriable provider close errors remain observable and exactly-once; an
 * ownership-consuming provider reports success once its identity is consumed.
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

bool turbowasm_wasi_fs_retained_descriptor_info_get(
    const turbowasm_wasi_fs *filesystem,
    turbowasm_wasi_fs_descriptor descriptor,
    turbowasm_wasi_fs_descriptor_info *out_info);

/*
 * Enumerate current preopen capabilities without assuming guest-fd numbering.
 *
 * preopen_at uses dense preopen ordinals [0, count). The returned guest_path
 * pointer is borrowed from the filesystem table and remains valid until that
 * descriptor is closed or the filesystem is destroyed.
 */
size_t turbowasm_wasi_fs_preopen_count(
    const turbowasm_wasi_fs *filesystem);

bool turbowasm_wasi_fs_preopen_at(
    const turbowasm_wasi_fs *filesystem,
    size_t preopen_index,
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

uint32_t turbowasm_wasi_fs_fd_stat(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    turbowasm_wasi_fs_stat *out_stat);

uint32_t turbowasm_wasi_fs_path_stat(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    uint32_t lookup_flags,
    const uint8_t *path,
    size_t path_length,
    turbowasm_wasi_fs_stat *out_stat);

uint32_t turbowasm_wasi_fs_path_create_directory(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    const uint8_t *path,
    size_t path_length);

uint32_t turbowasm_wasi_fs_path_remove_directory(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    const uint8_t *path,
    size_t path_length);

uint32_t turbowasm_wasi_fs_path_unlink_file(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    const uint8_t *path,
    size_t path_length);

uint32_t turbowasm_wasi_fs_fd_readdir(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    uint64_t cookie,
    turbowasm_wasi_fs_dirent *out_entry,
    bool *out_has_entry);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI_FS_H */
