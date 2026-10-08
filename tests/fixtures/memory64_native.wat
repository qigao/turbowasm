(module
  (memory $wide i64 1 2)
  (memory $narrow 1 1)
  (data $payload "abc")
  (func (export "size") (result i64) memory.size $wide)
  (func (export "grow") (param i64) (result i64)
    local.get 0 memory.grow $wide)
  (func (export "load") (param i64) (result i64)
    local.get 0 i64.load $wide)
  (func (export "store") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.store $wide
    local.get 0 i64.load $wide)
  (func (export "high_offset") (result i64)
    i64.const 0 i64.load $wide offset=4294967296)
  (func (export "overflow") (result i64)
    i64.const 1 i64.load $wide offset=18446744073709551615)
  (func (export "fill") (result i32)
    i64.const 8 i32.const 42 i64.const 3 memory.fill $wide
    i64.const 10 i32.load8_u $wide)
  (func (export "copy") (result i32)
    i64.const 12 i64.const 8 i64.const 3 memory.copy $wide $wide
    i64.const 14 i32.load8_u $wide)
  (func (export "init") (result i32)
    i64.const 16 i32.const 0 i32.const 3 memory.init $wide $payload
    i64.const 18 i32.load8_u $wide)
  (func (export "drop") (result i32)
    data.drop $payload i32.const 7)
  (func (export "mixed_copy") (result i32)
    i32.const 20 i64.const 8 i32.const 3 memory.copy $narrow $wide
    i64.const 24 i32.const 20 i32.const 3 memory.copy $wide $narrow
    i64.const 26 i32.load8_u $wide)
  (func (export "f32") (result f32)
    i64.const 32 f32.const -0 f32.store $wide
    i64.const 32 f32.load $wide)
  (func (export "f64") (result f64)
    i64.const 40 f64.const -0 f64.store $wide
    i64.const 40 f64.load $wide)
  (func (export "simd") (result i32)
    i64.const 48 v128.const i32x4 -1 0 -1 0 v128.store $wide
    i64.const 48 v128.load $wide i32x4.bitmask)
  (func (export "simd_high") (result i32)
    i64.const 0 v128.load $wide offset=4294967296 i32x4.bitmask)
  (func (export "block") (param i64) (result i64)
    block (result i64) local.get 0 i64.load $wide end)
  (func (export "narrow_signed") (result i64)
    i64.const 64 i64.const -1 i64.store8 $wide
    i64.const 64 i64.load8_s $wide)
  (func (export "fill_high") (result i32)
    i64.const 4294967296 i32.const 42 i64.const 1 memory.fill $wide
    i32.const 0)
  (func (export "load_f32_arg") (param i64) (result f32)
    local.get 0 f32.load $wide)
  (func (export "store_f32_arg") (param i64 f32) (result f32)
    local.get 0 local.get 1 f32.store $wide
    local.get 0 f32.load $wide)
  (func (export "store_f64_mixed") (param i64 f64 i32 i64) (result f64)
    (local f64 i64)
    local.get 1 local.set 4
    local.get 0 local.set 5
    block (result f64)
      local.get 5 local.get 4 f64.store $wide
      local.get 5 f64.load $wide
    end)
  (func (export "sum_four") (param i64 i64 i64 i64) (result i64)
    local.get 0 local.get 1 i64.add
    local.get 2 i64.add local.get 3 i64.add)
  (func (export "mixed_i32") (param f64 i64 f32 i32) (result i32)
    local.get 3)
)
