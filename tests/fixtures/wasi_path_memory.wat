;; wasi_path_memory.h embeds this fixture and its unshared variant.
(module
  (import "wasi_snapshot_preview1" "path_open" (func (param i32 i32 i32 i32 i32 i64 i64 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_filestat_get" (func (param i32 i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_create_directory" (func (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_remove_directory" (func (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_unlink_file" (func (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_rename" (func (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "random_get" (func (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_fdstat_get" (func (param i32 i32) (result i32)))
  (import "p" "memory" (memory 33 64 shared))
)
