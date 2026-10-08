(component
  (type $host (instance
    (type $echo (func (param "s" string) (result string)))
    (export "echo" (func (type $echo)))))
  (import "host" (instance $host (type $host)))
  (alias export $host "echo" (func $echo))
  (core module $storage
    (memory (export "memory") 1)
    (global $heap (mut i32) (i32.const 1024))
    (func (export "realloc") (param $old i32) (param $old-size i32)
      (param $align i32) (param $size i32) (result i32) (local $p i32)
      global.get $heap local.get $align i32.const 1 i32.sub i32.add
      i32.const 0 local.get $align i32.sub i32.and local.tee $p
      local.get $size i32.add global.set $heap
      local.get $p local.get $old
      local.get $old-size local.get $size local.get $old-size local.get $size i32.lt_u select
      memory.copy local.get $p))
  (core instance $storage (instantiate $storage))
  (alias core export $storage "memory" (core memory $mem))
  (alias core export $storage "realloc" (core func $realloc))
  (core func $compact (canon lower (func $echo)
    (memory $mem) (realloc $realloc) string-encoding=latin1+utf16))
  (core func $utf16 (canon lower (func $echo)
    (memory $mem) (realloc $realloc) string-encoding=utf16))
  (core module $relay
    (import "h" "compact" (func $compact (param i32 i32 i32)))
    (import "h" "utf16" (func $utf16 (param i32 i32 i32)))
    (func (export "compact") (param i32 i32) (result i32)
      local.get 0 local.get 1 i32.const 32 call $compact i32.const 32)
    (func (export "utf16") (param i32 i32) (result i32)
      local.get 0 local.get 1 i32.const 32 call $utf16 i32.const 32))
  (core instance $imports (export "compact" (func $compact)) (export "utf16" (func $utf16)))
  (core instance $r (instantiate $relay (with "h" (instance $imports))))
  (alias core export $r "compact" (core func $compact-relay))
  (alias core export $r "utf16" (core func $utf16-relay))
  (func (export "compact") (param "s" string) (result string)
    (canon lift (core func $compact-relay) (memory $mem) (realloc $realloc) string-encoding=latin1+utf16))
  (func (export "utf16") (param "s" string) (result string)
    (canon lift (core func $utf16-relay) (memory $mem) (realloc $realloc) string-encoding=utf16))
)
