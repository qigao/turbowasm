(component
  (import "wasi:io/poll@0.2.8" (instance $poll
    (export "pollable" (type $pollable (sub resource)))
    (export "[method]pollable.block"
      (func (param "self" (borrow $pollable)))
    )
  ))
  (alias export $poll "pollable" (type $pollable))

  (import "wasi:io/streams@0.2.8" (instance $streams
    (export "input-stream" (type $input-stream (sub resource)))
    (export "[method]input-stream.subscribe"
      (func
        (param "self" (borrow $input-stream))
        (result (own $pollable))
      )
    )
  ))
  (alias export $streams "input-stream" (type $input-stream))

  (import "wasi:cli/stdin@0.2.8" (instance $stdin
    (export "get-stdin"
      (func (result (own $input-stream)))
    )
  ))

  (core func $get-stdin
    (canon lower (func $stdin "get-stdin"))
  )
  (core func $subscribe
    (canon lower (func $streams "[method]input-stream.subscribe"))
  )
  (core func $block
    (canon lower (func $poll "[method]pollable.block"))
  )
  (core func $drop-pollable
    (canon resource.drop $pollable)
  )
  (core func $drop-input
    (canon resource.drop $input-stream)
  )

  (core module $M
    (import "" "get-stdin" (func $get-stdin (result i32)))
    (import "" "subscribe" (func $subscribe (param i32) (result i32)))
    (import "" "block" (func $block (param i32)))
    (import "" "drop-pollable" (func $drop-pollable (param i32)))
    (import "" "drop-input" (func $drop-input (param i32)))
    (func (export "run") (result i32)
      (local $stream i32)
      (local $pollable i32)
      (local.set $stream (call $get-stdin))
      (local.set $pollable (call $subscribe (local.get $stream)))
      (call $block (local.get $pollable))
      (call $drop-pollable (local.get $pollable))
      (call $drop-input (local.get $stream))
      (i32.const 1)
    )
  )
  (core instance $m (instantiate $M (with "" (instance
    (export "get-stdin" (func $get-stdin))
    (export "subscribe" (func $subscribe))
    (export "block" (func $block))
    (export "drop-pollable" (func $drop-pollable))
    (export "drop-input" (func $drop-input))
  ))))
  (func (export "run")
    (result u32)
    (canon lift (core func $m "run"))
  )
)
