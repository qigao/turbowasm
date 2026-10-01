(component
  (import "wasi:filesystem/types@0.2.8" (instance $types
    (export "descriptor" (type $descriptor (sub resource)))
  ))
  (alias export $types "descriptor" (type $descriptor))
  (import "wasi:filesystem/preopens@0.2.8" (instance $preopens
    (export "get-directories"
      (func (result (list (tuple (own $descriptor) string))))
    )
  ))

  (core module $Memory
    (memory (export "mem") 1)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
      (param $old i32) (param $old-size i32)
      (param $align i32) (param $new-size i32)
      (result i32)
      (local $p i32)
      (local.set $p (global.get $next))
      (global.set $next
        (i32.add (local.get $p) (local.get $new-size)))
      (local.get $p)
    )
  )
  (core instance $memory (instantiate $Memory))
  (alias core export $memory "mem" (core memory $mem))
  (alias core export $memory "realloc" (core func $realloc))
  (core func $get-directories
    (canon lower
      (func $preopens "get-directories")
      (memory $mem)
      (realloc $realloc)
    )
  )
  (core func $drop-descriptor
    (canon resource.drop $descriptor)
  )

  (core module $M
    (import "m" "mem" (memory 1))
    (import "p" "get-directories" (func $get-directories (param i32)))
    (import "p" "drop-descriptor" (func $drop-descriptor (param i32)))
    (func (export "run") (result i32)
      (local $ptr i32)
      (local $len i32)
      (local $handle i32)
      (call $get-directories (i32.const 0))
      (local.set $ptr (i32.load (i32.const 0)))
      (local.set $len (i32.load (i32.const 4)))
      (if (i32.gt_u (local.get $len) (i32.const 0))
        (then
          (local.set $handle (i32.load (local.get $ptr)))
          (call $drop-descriptor (local.get $handle))
        )
      )
      (local.get $len)
    )
  )
  (core instance $m (instantiate $M
    (with "m" (instance
      (export "mem" (memory $mem))
    ))
    (with "p" (instance
      (export "get-directories" (func $get-directories))
      (export "drop-descriptor" (func $drop-descriptor))
    ))
  ))
  (func (export "run") (result u32)
    (canon lift (core func $m "run"))
  )
)
