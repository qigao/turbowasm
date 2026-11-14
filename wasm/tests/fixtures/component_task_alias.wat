(module
  (import "p" "m64" (memory i64 1))
  (import "p" "m32" (memory 1))
  (export "m32" (memory 1))
  (export "m64" (memory 0))
)
