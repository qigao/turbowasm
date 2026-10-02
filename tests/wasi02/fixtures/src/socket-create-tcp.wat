(component
  (import "wasi:sockets/network@0.2.8" (instance $network-types
    (type $error-code (enum
      "unknown"
      "access-denied"
      "not-supported"
      "invalid-argument"
      "out-of-memory"
      "timeout"
      "concurrency-conflict"
      "not-in-progress"
      "would-block"
      "invalid-state"
      "new-socket-limit"
      "address-not-bindable"
      "address-in-use"
      "remote-unreachable"
      "connection-refused"
      "connection-reset"
      "connection-aborted"
      "datagram-too-large"
      "name-unresolvable"
      "temporary-resolver-failure"
      "permanent-resolver-failure"
    ))
    (export "error-code" (type (eq $error-code)))
    (type $family (enum "ipv4" "ipv6"))
    (export "ip-address-family" (type (eq $family)))
  ))
  (alias export $network-types "error-code" (type $error-code))
  (alias export $network-types "ip-address-family" (type $family))

  (import "wasi:sockets/tcp@0.2.8" (instance $tcp-types
    (type $tcp-socket (sub resource))
    (export "tcp-socket" (type (eq $tcp-socket)))
  ))
  (alias export $tcp-types "tcp-socket" (type $tcp-socket))

  (import "wasi:sockets/tcp-create-socket@0.2.8"
    (instance $tcp-create
      (export "create-tcp-socket"
        (func
          (param "address-family" $family)
          (result
            (result
              (own $tcp-socket)
              (error $error-code)
            )
          )
        )
      )
    )
  )

  (core func $create-tcp-socket
    (canon lower (func $tcp-create "create-tcp-socket"))
  )
  (core func $drop-tcp-socket
    (canon resource.drop $tcp-socket)
  )

  (core module $M
    (import "" "create-tcp-socket"
      (func $create-tcp-socket
        (param i32)
        (result i32 i32)
      )
    )
    (import "" "drop-tcp-socket"
      (func $drop-tcp-socket (param i32))
    )

    (func (export "run") (result i32)
      (local $tag i32)
      (local $payload i32)

      (call $create-tcp-socket (i32.const 0))
      (local.set $payload)
      (local.set $tag)

      (if (result i32)
        (i32.eqz (local.get $tag))
        (then
          (call $drop-tcp-socket (local.get $payload))
          (i32.const 1)
        )
        (else
          (i32.const 0)
        )
      )
    )
  )

  (core instance $m
    (instantiate $M
      (with "" (instance
        (export "create-tcp-socket" (func $create-tcp-socket))
        (export "drop-tcp-socket" (func $drop-tcp-socket))
      ))
    )
  )

  (func (export "run")
    (result u32)
    (canon lift (core func $m "run"))
  )
)
