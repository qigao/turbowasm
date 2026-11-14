(component
  (core module $m
    (memory (export "memory") 1)
    (global $heap (mut i32) (i32.const 1024))
    (func (export "realloc") (param $old i32) (param $old-size i32)
      (param $align i32) (param $size i32) (result i32) (local $p i32)
      global.get $heap local.get $align i32.const 1 i32.sub i32.add
      i32.const 0 local.get $align i32.sub i32.and local.tee $p
      local.get $size i32.add global.set $heap
      local.get $p local.get $old
      local.get $old-size local.get $size local.get $old-size local.get $size i32.lt_u select
      memory.copy local.get $p)
    (func (export "echo") (param i32 i32) (result i32)
      i32.const 0 local.get 0 i32.store
      i32.const 4 local.get 1 i32.store i32.const 0)
    (func (export "length") (param i32 i32) (result i32) local.get 1))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "realloc" (core func $realloc))
  (alias core export $i "echo" (core func $echo))
  (alias core export $i "length" (core func $length))
  (func $utf16 (export "utf16") (param "s" string) (result string)
    (canon lift (core func $echo) (memory $mem) (realloc $realloc) string-encoding=utf16))
  (func (export "compact") (param "s" string) (result string)
    (canon lift (core func $echo) (memory $mem) (realloc $realloc) string-encoding=latin1+utf16))
  (func (export "utf16-length") (param "s" string) (result u32)
    (canon lift (core func $length) (memory $mem) (realloc $realloc) string-encoding=utf16))
  (func (export "compact-length") (param "s" string) (result u32)
    (canon lift (core func $length) (memory $mem) (realloc $realloc) string-encoding=latin1+utf16))
  (type $strings (list string))
  (export $list "strings" (type $strings))
  (func (export "utf16-list") (param "s" $list) (result $list)
    (canon lift (core func $echo) (memory $mem) (realloc $realloc) string-encoding=utf16))
)
