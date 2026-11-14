(component
  (type $pair (tuple u32 u64))
  (type $args (tuple u8 u64 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32))
  (type $host (instance
    (type $pair (tuple u32 u64))
    (export "wide" (func (param "small" u8) (param "wide" u64) (param "p0" u32) (param "p1" u32) (param "p2" u32) (param "p3" u32) (param "p4" u32) (param "p5" u32) (param "p6" u32) (param "p7" u32) (param "p8" u32) (param "p9" u32) (param "p10" u32) (param "p11" u32) (param "p12" u32) (param "p13" u32) (param "p14" u32) (result $pair)))
  ))
  (import "host" (instance $host (type $host)))
  (alias export $host "wide" (func $host-wide))
  (core module $storage
    (memory (export "memory") i64 1)
    (global $calls (mut i32) (i32.const 0))
    (func (export "realloc") (param i64 i64 i64 i64) (result i64) i64.const 1024)
    (func (export "wide") (param $p i64) (result i64)
      global.get $calls i32.const 1 i32.add global.set $calls
      i64.const 768 local.get $p i32.load8_u
      local.get $p i32.load offset=16 i32.add
      local.get $p i32.load offset=20 i32.add
      local.get $p i32.load offset=24 i32.add
      local.get $p i32.load offset=28 i32.add
      local.get $p i32.load offset=32 i32.add
      local.get $p i32.load offset=36 i32.add
      local.get $p i32.load offset=40 i32.add
      local.get $p i32.load offset=44 i32.add
      local.get $p i32.load offset=48 i32.add
      local.get $p i32.load offset=52 i32.add
      local.get $p i32.load offset=56 i32.add
      local.get $p i32.load offset=60 i32.add
      local.get $p i32.load offset=64 i32.add
      local.get $p i32.load offset=68 i32.add
      local.get $p i32.load offset=72 i32.add
      i32.store
      i64.const 776 local.get $p i64.load offset=8 i64.store
      i64.const 768)
    (func (export "calls") (result i32) global.get $calls))
  (core instance $s (instantiate $storage))
  (alias core export $s "memory" (core memory $mem))
  (alias core export $s "realloc" (core func $realloc))
  (alias core export $s "wide" (core func $wide))
  (alias core export $s "calls" (core func $calls))
  (func $wide (param "small" u8) (param "wide" u64) (param "p0" u32) (param "p1" u32) (param "p2" u32) (param "p3" u32) (param "p4" u32) (param "p5" u32) (param "p6" u32) (param "p7" u32) (param "p8" u32) (param "p9" u32) (param "p10" u32) (param "p11" u32) (param "p12" u32) (param "p13" u32) (param "p14" u32) (result $pair) (canon lift (core func $wide) (memory $mem) (realloc $realloc)))
  (func $aggregate (param "args" $args) (result $pair)
    (canon lift (core func $wide) (memory $mem) (realloc $realloc)))
  (core func $aggregate (canon lower (func $aggregate) (memory $mem)))
  (core func $local (canon lower (func $wide) (memory $mem)))
  (core func $external (canon lower (func $host-wide) (memory $mem)))
  (core instance $imports (export "local" (func $local)) (export "external" (func $external))
    (export "aggregate" (func $aggregate))
  )
  (core module $relay
    (import "s" "memory" (memory i64 1))
    (import "f" "local" (func $local (param i64 i64)))
    (import "f" "aggregate" (func $aggregate (param i64 i64)))
    (import "f" "external" (func $external (param i64 i64)))
    (func $init
      i64.const 32 i32.const 7 i32.store8
      i64.const 40 i64.const 4294967296 i64.store
      i64.const 48 i32.const 1 i32.store
      i64.const 52 i32.const 2 i32.store
      i64.const 56 i32.const 3 i32.store
      i64.const 60 i32.const 4 i32.store
      i64.const 64 i32.const 5 i32.store
      i64.const 68 i32.const 6 i32.store
      i64.const 72 i32.const 7 i32.store
      i64.const 76 i32.const 8 i32.store
      i64.const 80 i32.const 9 i32.store
      i64.const 84 i32.const 10 i32.store
      i64.const 88 i32.const 11 i32.store
      i64.const 92 i32.const 12 i32.store
      i64.const 96 i32.const 13 i32.store
      i64.const 100 i32.const 14 i32.store
      i64.const 104 i32.const 15 i32.store)
    (func (export "local") (result i64)
      call $init i64.const 32 i64.const 256 call $local i64.const 256)
    (func (export "external") (result i64)
      call $init i64.const 32 i64.const 256 call $external i64.const 256)
    (func (export "aggregate") (result i64)
      call $init i64.const 32 i64.const 256 call $aggregate i64.const 256)
    (func (export "edge") (result i64)
      call $init i64.const 65456 i64.const 32 i64.const 80 memory.copy
      i64.const 65456 i64.const 256 call $external i64.const 256)
    (func (export "misaligned") (result i64)
      i64.const 33 i64.const 256 call $external i64.const 256)
    (func (export "bounds") (result i64)
      i64.const 65528 i64.const 256 call $local i64.const 256)
    (func (export "overflow") (result i64)
      i64.const -8 i64.const 256 call $external i64.const 256)
  )
  (core instance $r (instantiate $relay (with "s" (instance $s)) (with "f" (instance $imports))))
  (alias core export $r "local" (core func $local-relay))
  (func (export "local") (result $pair) (canon lift (core func $local-relay) (memory $mem)))
  (alias core export $r "external" (core func $external-relay))
  (func (export "external") (result $pair) (canon lift (core func $external-relay) (memory $mem)))
  (alias core export $r "misaligned" (core func $misaligned-relay))
  (func (export "misaligned") (result $pair) (canon lift (core func $misaligned-relay) (memory $mem)))
  (alias core export $r "bounds" (core func $bounds-relay))
  (func (export "bounds") (result $pair) (canon lift (core func $bounds-relay) (memory $mem)))
  (alias core export $r "overflow" (core func $overflow-relay))
  (func (export "overflow") (result $pair) (canon lift (core func $overflow-relay) (memory $mem)))
  (alias core export $r "aggregate" (core func $aggregate-relay))
  (func (export "aggregate") (result $pair) (canon lift (core func $aggregate-relay) (memory $mem)))
  (alias core export $r "edge" (core func $edge-relay))
  (func (export "edge") (result $pair) (canon lift (core func $edge-relay) (memory $mem)))
  (func (export "calls") (result u32) (canon lift (core func $calls)))
)
