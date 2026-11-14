(module
  (import "host" "tuple" (func $host (param i32 i64 f32 f64) (result i32 i64 f32 f64)))
  ;; Alternating arities exercise retained dispatcher storage across native returns.
  (func $left (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0
    if (result i32 i64 f32 f64)
      local.get 0 i32.const 1 i32.sub
      local.get 1 local.get 2 local.get 3
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      i32.const 0
      return_call $right
    else
      local.get 0 local.get 1 local.get 2 local.get 3
    end)
  (func $right (param i32 i64 f32 f64 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 return_call $left)
  (func $entry (param i32) (result i32 i64 f32 f64)
    local.get 0 i64.const -123 f32.const -0.0 f64.const 7.5 return_call $left)
  (func $void-entry
    i32.const 1 i64.const 2 f32.const 3 f64.const 4 return_call $void-target)
  (func $void-target (param i32 i64 f32 f64))
  (func $interpreted-entry (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 return_call $interpreted-target)
  (func $interpreted-target (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3)
  (func $host-entry (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 return_call $host)
  (func $trap-entry (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 return_call $trap-target)
  (func $trap-target (param i32 i64 f32 f64) (result i32 i64 f32 f64) unreachable)
  (func $zero-args-entry (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    return_call $zero-args-target)
  (func $zero-args-target (result i32 i64 f32 f64)
    i32.const 5 i64.const 6 f32.const 7 f64.const 8)
)
