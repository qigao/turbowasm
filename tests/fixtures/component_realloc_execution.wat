(component
  (type $host (instance (export "text" (func (result string)))))
  (import "host" (instance $host (type $host)))
  (alias export $host "text" (func $text))
  (core module $storage
    (memory (export "memory") 1)
    (global $mode (mut i32) (i32.const 0))
    (global $entered (mut i32) (i32.const 0))
    (global $finished (mut i32) (i32.const 0))
    (tag $failure)
    (func $recurse (param $depth i32)
      local.get $depth i32.eqz
      if else local.get $depth i32.const 1 i32.sub call $recurse end)
    (func (export "realloc") (param i32 i32 i32 i32) (result i32) (local $n i32)
      global.get $entered i32.const 1 i32.add global.set $entered
      global.get $mode i32.const 1 i32.eq if unreachable end
      global.get $mode i32.const 2 i32.eq if throw $failure end
      global.get $mode i32.const 3 i32.eq if i32.const 64 call $recurse end
      i32.const 8 local.set $n
      block $done loop $again
        local.get $n i32.eqz br_if $done
        local.get $n i32.const 1 i32.sub local.set $n br $again
      end end
      global.get $finished i32.const 1 i32.add global.set $finished
      i32.const 1024)
    (func (export "mode") (param i32) local.get 0 global.set $mode)
    (func (export "entered") (result i32) global.get $entered)
    (func (export "finished") (result i32) global.get $finished))
  (core instance $s (instantiate $storage))
  (alias core export $s "memory" (core memory $mem))
  (alias core export $s "realloc" (core func $realloc))
  (alias core export $s "mode" (core func $mode))
  (alias core export $s "entered" (core func $entered))
  (alias core export $s "finished" (core func $finished))
  (core func $lower (canon lower (func $text) (memory $mem) (realloc $realloc)))
  (core instance $imports (export "text" (func $lower)))
  (core module $relay
    (import "host" "text" (func $text (param i32)))
    (func $run (export "run") (result i32)
      i32.const 0 call $text i32.const 0)
    (func $recurse (param $depth i32) (result i32)
      local.get $depth i32.eqz
      if (result i32) call $run
      else local.get $depth i32.const 1 i32.sub call $recurse end)
    (func (export "deep") (result i32) i32.const 224 call $recurse))
  (core instance $r (instantiate $relay (with "host" (instance $imports))))
  (alias core export $r "run" (core func $run))
  (alias core export $r "deep" (core func $deep))
  (func (export "run") (result string) (canon lift (core func $run) (memory $mem)))
  (func (export "deep") (result string) (canon lift (core func $deep) (memory $mem)))
  (func (export "mode") (param "mode" u32) (canon lift (core func $mode)))
  (func (export "entered") (result u32) (canon lift (core func $entered)))
  (func (export "finished") (result u32) (canon lift (core func $finished)))
)
