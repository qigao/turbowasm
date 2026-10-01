(component
  (import "wasi:random/random@0.2.8" (instance $random
    (export "get-random-u64" (func (result u64)))
  ))
  (core func $random-u64 (canon lower (func $random "get-random-u64")))
  (core module $M
    (import "" "random-u64" (func $random-u64 (result i64)))
    (func (export "run") (result i64)
      (call $random-u64)
    )
  )
  (core instance $m (instantiate $M (with "" (instance
    (export "random-u64" (func $random-u64))
  ))))
  (func (export "run") (result u64)
    (canon lift (core func $m "run"))
  )
)
