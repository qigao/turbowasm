(module
  (memory 1)
  (func (param i32 i64 f32 f64) nop)
  (func (param i32 i64 f32 f64) return)
  (func (param i32 i64 f32 f64) br 0)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 return)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 br 0)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3
    local.get 0 br_if 0
    drop drop drop drop
    i32.const -1 i64.const -2 f32.const -3 f64.const -4)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    block (result i32 i64 f32 f64)
      local.get 0 local.get 1 local.get 2 local.get 3
      local.get 0 br_table 0 1 0
    end)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 if (result i32 i64 f32 f64)
      local.get 0 local.get 1 local.get 2 local.get 3
    else
      i32.const -1 i64.const -2 f32.const -3 f64.const -4
    end)
  (func $tail (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 if
      local.get 0 i32.const 1 i32.sub
      local.get 1 local.get 2 local.get 3 return_call $tail
    end
    local.get 0 local.get 1 local.get 2 local.get 3)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3
    i32.const 65536 i32.load drop)
  (func (param i32 i64 f32 f64)
    i32.const 65536 i32.load drop)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64 i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3
    local.get 0 local.get 1 local.get 2 local.get 3)
  (func (param i32 i64 f32 f64)
    local.get 0 i32.const 1 i32.add drop
    local.get 2 f32.const 2 f32.mul drop
    local.get 3 f64.const 3 f64.add drop)
  (func (param i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 call 0)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 call 3)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64 i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 call 15
    local.get 0 local.get 1 local.get 2 local.get 3 call 15)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64 i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 call 12)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 call 10)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 call 20)
  ;; Division keeps this callee interpreted while its caller must compile.
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 i64.const 1 i64.div_s local.get 2 local.get 3)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64)
    local.get 0 local.get 1 local.get 2 local.get 3 call 19)
  (func (result i32 i64 f32 f64 i32 i64 f32 f64)
    i32.const 1 i64.const 2 f32.const 3 f64.const 4
    i32.const 5 i64.const 6 f32.const 7 f64.const 8)
  (func (param i32 i64 f32 f64) (result i32 i64 f32 f64 i32 i64 f32 f64)
    call 22)
  (func nop)
  (func (param i32 i64 f32 f64) call 24)
  (func (param f32 f32) (result f32) local.get 0 local.get 1 f32.add)
  (func (param i32 i64 f32 f64) (result f32) local.get 2 local.get 2 call 26)
)
