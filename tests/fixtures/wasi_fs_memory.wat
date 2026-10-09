;; wasi_fs_memory.h embeds this fixture and its unshared-memory variant.
(module
  (import "wasi_snapshot_preview1" "fd_seek" (func (param i32 i64 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_tell" (func (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_filestat_get" (func (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_prestat_get" (func (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_prestat_dir_name" (func (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_readdir" (func (param i32 i32 i32 i64 i32) (result i32)))
  (import "p" "memory" (memory 1 4 shared))
  (func (param i32) (result i32) local.get 0 i32.load8_u)
  (func (param i32 i32) local.get 0 local.get 1 i32.store8)
)
