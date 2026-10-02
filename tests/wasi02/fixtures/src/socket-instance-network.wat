(component
  (import "wasi:sockets/network@0.2.8" (instance $network-types
    (export "network" (type $network (sub resource)))
  ))
  (alias export $network-types "network" (type $network))

  (import "wasi:sockets/instance-network@0.2.8" (instance $instance-network
    (export "instance-network"
      (func (result (own $network)))
    )
  ))

  (core func $instance-network
    (canon lower (func $instance-network "instance-network"))
  )
  (core func $drop-network
    (canon resource.drop $network)
  )

  (core module $M
    (import "" "instance-network"
      (func $instance-network (result i32)))
    (import "" "drop-network"
      (func $drop-network (param i32)))
    (func (export "run") (result i32)
      (local $network i32)
      (local.set $network (call $instance-network))
      (call $drop-network (local.get $network))
      (i32.const 1)
    )
  )

  (core instance $m
    (instantiate $M
      (with "" (instance
        (export "instance-network" (func $instance-network))
        (export "drop-network" (func $drop-network))
      ))
    )
  )

  (func (export "run")
    (result u32)
    (canon lift (core func $m "run"))
  )
)
