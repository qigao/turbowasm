(component
  (type $host (instance (export "wait" (func))))
  (import "host" (instance $host (type $host)))
  (alias export $host "wait" (func $wait))
  (core func $lower (canon lower (func $wait)))
  (core instance $imports (export "wait" (func $lower)))
  (core module $state
    (import "host" "wait" (func $wait))
    (global $entered (mut i32) (i32.const 0))
    (global $finished (mut i32) (i32.const 0))
    (tag $failure)
    (func $recurse (param $depth i32)
      local.get $depth
      if local.get $depth i32.const 1 i32.sub call $recurse end)
    (func (export "drop") (param $mode i32) (local $n i32)
      global.get $entered i32.const 1 i32.add global.set $entered
      local.get $mode i32.const 1 i32.eq if unreachable end
      local.get $mode i32.const 2 i32.eq if throw $failure end
      local.get $mode i32.const 3 i32.eq if i32.const 64 call $recurse end
      local.get $mode i32.const 4 i32.eq if call $wait end
      i32.const 8 local.set $n
      loop $again local.get $n i32.const 1 i32.sub local.tee $n br_if $again end
      global.get $finished i32.const 1 i32.add global.set $finished)
    (func (export "entered") (result i32) global.get $entered)
    (func (export "finished") (result i32) global.get $finished))
  (core instance $s (instantiate $state (with "host" (instance $imports))))
  (alias core export $s "drop" (core func $dtor))
  (alias core export $s "entered" (core func $entered))
  (alias core export $s "finished" (core func $finished))
  (type $r (resource (rep i32) (dtor (core func $dtor))))
  (core func $new (canon resource.new $r))
  (core func $drop (canon resource.drop $r))
  (core instance $builtins (export "new" (func $new)) (export "drop" (func $drop)))
  (core module $main
    (import "r" "new" (func $new (param i32) (result i32)))
    (import "r" "drop" (func $drop (param i32)))
    (global $last (mut i32) (i32.const 0))
    (func (export "make") (param i32)
      local.get 0 call $new global.set $last)
    (func (export "cleanup") global.get $last call $drop)
    (func $recurse (param $depth i32)
      local.get $depth
      if local.get $depth i32.const 1 i32.sub call $recurse
      else global.get $last call $drop end)
    (func (export "deep-cleanup") i32.const 224 call $recurse))
  (core instance $i (instantiate $main (with "r" (instance $builtins))))
  (alias core export $i "make" (core func $make))
  (alias core export $i "cleanup" (core func $cleanup))
  (alias core export $i "deep-cleanup" (core func $deep))
  (func (export "make") (param "mode" u32) (canon lift (core func $make)))
  (func (export "cleanup") (canon lift (core func $cleanup)))
  (func (export "deep-cleanup") (canon lift (core func $deep)))
  (func (export "entered") (result u32) (canon lift (core func $entered)))
  (func (export "finished") (result u32) (canon lift (core func $finished)))
)
