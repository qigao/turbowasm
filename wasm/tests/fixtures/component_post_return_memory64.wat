(component
  (core module $m
    (memory (export "memory") i64 1)
    (global $post (mut i64) (i64.const 0))
    (data (i64.const 32) "hello")
    (func (export "text") (result i64)
      i64.const 0 i64.const 32 i64.store
      i64.const 8 i64.const 5 i64.store i64.const 0)
    (func (export "post") (param i64)
      local.get 0 i64.eqz if else unreachable end
      i64.const 32 i32.const 0 i64.const 5 memory.fill
      i64.const 1 global.set $post)
    (func (export "posts") (result i64) global.get $post))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "text" (core func $text))
  (alias core export $i "post" (core func $post))
  (alias core export $i "posts" (core func $posts))
  (func (export "text") (result string)
    (canon lift (core func $text) (memory $mem) (post-return $post)))
  (func (export "posts") (result u64) (canon lift (core func $posts)))
)
