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

  (core func $subscribe
    (canon lower (func $streams "[method]input-stream.subscribe"))
  )
  (core func $block
    (canon lower (func $poll "[method]pollable.block"))
  )
  (canon resource.drop $pollable (core func $drop-pollable))

  (core module $M
    (import "" "subscribe" (func $subscribe (param i32) (result i32)))
    (import "" "block" (func $block (param i32)))
    (import "" "drop" (func $drop (param i32)))
    (func (export "run") (param $stream i32) (result i32)
      (local $pollable i32)
      (local.set $pollable (call $subscribe (local.get $stream)))
      (call $block (local.get $pollable))
      (call $drop (local.get $pollable))
      (i32.const 1)
    )
  )
  (core instance $m (instantiate $M (with "" (instance
    (export "subscribe" (func $subscribe))
    (export "block" (func $block))
    (export "drop" (func $drop-pollable))
  ))))
  (func (export "run")
    (param "stream" (borrow $input-stream))
    (result u32)
    (canon lift (core func $m "run"))
  )
)
