(component
  (type $host (instance (export "text" (func (result string)))))
  (import "host" (instance $host (type $host)))
  (alias export $host "text" (func $text))
  (core module $storage
    (memory (export "memory") i64 1)
    (global $mode (mut i64) (i64.const 0))
    (global $entered (mut i64) (i64.const 0))
    (global $finished (mut i64) (i64.const 0))
    (tag $failure)
    (func $recurse (param $depth i64)
      local.get $depth i64.eqz
      if else local.get $depth i64.const 1 i64.sub call $recurse end)
    (func (export "realloc") (param i64 i64 i64 i64) (result i64) (local $n i64)
      global.get $entered i64.const 1 i64.add global.set $entered
      global.get $mode i64.const 1 i64.eq if unreachable end
      global.get $mode i64.const 2 i64.eq if throw $failure end
      global.get $mode i64.const 3 i64.eq if i64.const 64 call $recurse end
      i64.const 8 local.set $n
      block $done loop $again
        local.get $n i64.eqz br_if $done
        local.get $n i64.const 1 i64.sub local.set $n br $again
      end end
      global.get $finished i64.const 1 i64.add global.set $finished
      i64.const 1024)
    (func (export "mode") (param i64) local.get 0 global.set $mode)
    (func (export "entered") (result i64) global.get $entered)
    (func (export "finished") (result i64) global.get $finished))
  (core instance $s (instantiate $storage))
  (alias core export $s "memory" (core memory $mem))
  (alias core export $s "realloc" (core func $realloc))
  (alias core export $s "mode" (core func $mode))
  (alias core export $s "entered" (core func $entered))
  (alias core export $s "finished" (core func $finished))
  (core func $lower (canon lower (func $text) (memory $mem) (realloc $realloc)))
  (core instance $imports (export "text" (func $lower)))
  (core module $relay
    (import "host" "text" (func $text (param i64)))
    (func $run (export "run") (result i64)
      i64.const 0 call $text i64.const 0)
    (func $recurse (param $depth i64) (result i64)
      local.get $depth i64.eqz
      if (result i64) call $run
      else local.get $depth i64.const 1 i64.sub call $recurse end)
    (func (export "deep") (result i64) i64.const 224 call $recurse))
  (core instance $r (instantiate $relay (with "host" (instance $imports))))
  (alias core export $r "run" (core func $run))
  (alias core export $r "deep" (core func $deep))
  (func (export "run") (result string) (canon lift (core func $run) (memory $mem)))
  (func (export "deep") (result string) (canon lift (core func $deep) (memory $mem)))
  (func (export "mode") (param "mode" u64) (canon lift (core func $mode)))
  (func (export "entered") (result u64) (canon lift (core func $entered)))
  (func (export "finished") (result u64) (canon lift (core func $finished)))
)
