(component
  (import "wasi:cli/exit@0.2.8" (instance $exit
    (export "exit" (func (param "status" (result))))
  ))
  (core func $exit-fn (canon lower (func $exit "exit")))
  (core module $M
    (import "" "exit" (func $exit (param i32)))
    (func (export "run")
      (call $exit (i32.const 0))
    )
  )
  (core instance $m (instantiate $M (with "" (instance
    (export "exit" (func $exit-fn))
  ))))
  (func (export "run")
    (canon lift (core func $m "run"))
  )
)
