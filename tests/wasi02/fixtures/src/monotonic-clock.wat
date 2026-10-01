(component
  (import "wasi:clocks/monotonic-clock@0.2.8" (instance $clock
    (export "now" (func (result u64)))
  ))
  (core func $now (canon lower (func $clock "now")))
  (core module $M
    (import "" "now" (func $now (result i64)))
    (func (export "run") (result i64)
      (call $now)
    )
  )
  (core instance $m (instantiate $M (with "" (instance
    (export "now" (func $now))
  ))))
  (func (export "run") (result u64)
    (canon lift (core func $m "run"))
  )
)
