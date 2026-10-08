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

  (import "wasi:sockets/udp@0.2.8" (instance $udp-types
    (export "udp-socket" (type $udp-socket (sub resource)))
  ))
  (alias export $udp-types "udp-socket" (type $udp-socket))

  (import "wasi:sockets/udp-create-socket@0.2.8"
    (instance $udp-create
      (export "create-udp-socket"
        (func
          (param "address-family" $family)
          (result
            (result
              (own $udp-socket)
              (error $error-code)
            )
          )
        )
      )
    )
  )

  ;; result<own<udp-socket>, error-code> flattens to two values, so the
  ;; canonical ABI returns it indirectly through caller-provided memory.
  (core module $Memory
    (memory (export "mem") 1)
  )
  (core instance $memory (instantiate $Memory))
  (alias core export $memory "mem" (core memory $mem))

  (core func $create-udp-socket
    (canon lower
      (func $udp-create "create-udp-socket")
      (memory $mem)
    )
  )
  (core func $drop-udp-socket
    (canon resource.drop $udp-socket)
  )

  (core module $M
    (import "m" "mem" (memory 1))
    (import "p" "create-udp-socket"
      (func $create-udp-socket
        (param i32 i32)
      )
    )
    (import "p" "drop-udp-socket"
      (func $drop-udp-socket (param i32))
    )

    (func (export "run") (result i32)
      (local $tag i32)
      (local $payload i32)

      ;; address-family=ipv4, indirect result area at offset 0.
      (call $create-udp-socket (i32.const 0) (i32.const 0))
      (local.set $tag (i32.load8_u (i32.const 0)))
      (local.set $payload (i32.load (i32.const 4)))

      (if (result i32)
        (i32.eqz (local.get $tag))
        (then
          (call $drop-udp-socket (local.get $payload))
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
      (with "m" (instance $memory))
      (with "p" (instance
        (export "create-udp-socket" (func $create-udp-socket))
        (export "drop-udp-socket" (func $drop-udp-socket))
      ))
    )
  )

  (func (export "run")
    (result u32)
    (canon lift (core func $m "run"))
  )
)
