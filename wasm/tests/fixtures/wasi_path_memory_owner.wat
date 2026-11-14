;; A second importer can mutate/grow the same storage during a host callback.
(module
  (memory (export "memory") 33 64 shared)
  (func (param i32) (result i32) local.get 0 memory.grow)
  (func (param i32 i32 i32) local.get 0 local.get 1 local.get 2 memory.fill)
  (func (param i32) (result i32) local.get 0 i32.load8_u)
)
