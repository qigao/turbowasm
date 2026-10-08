(module
  (type $throw-type (func (param i32)))
  (type $node (struct (field i32)))
  (import "h" "collect" (func $collect))
  (tag $first (export "tag") (param i32))
  (tag $second (param i32))
  (tag $mixed (param i32 i64 f32 f64 v128 anyref))
  (table 1 funcref)
  (elem (i32.const 0) func $throw)
  (func $throw (export "throw") (param i32) local.get 0 throw $first)
  (func $tail (export "tail") (param i32) local.get 0 return_call $throw)
  (func (export "inline") (param i32) (result i32)
    block (result i32) try_table (catch $first 0) local.get 0 throw $first end unreachable end)
  (func (export "direct") (param i32) (result i32)
    block (result i32) try_table (catch $first 0) local.get 0 call $throw end unreachable end)
  (func (export "indirect") (param i32) (result i32)
    block (result i32) try_table (catch $first 0) local.get 0 i32.const 0 call_indirect (type $throw-type) end unreachable end)
  (func (export "reference") (param i32) (result i32)
    block (result i32) try_table (catch $first 0) local.get 0 ref.func $throw call_ref $throw-type end unreachable end)
  (func (export "tail-caller") (param i32) (result i32)
    block (result i32) try_table (catch $first 0) local.get 0 call $tail end unreachable end)
  (func (export "tail-escape") (param i32)
    block (result i32)
      try_table (catch $first 0) local.get 0 return_call $throw end unreachable
    end drop)
  (func (export "ordered") (param i32) (result i32)
    block (result i32)
      block (result i32)
        try_table (catch $second 0) (catch $first 1) (catch $first 0)
          local.get 0 throw $first
        end unreachable
      end drop i32.const -1 return
    end)
  (func (export "nested") (param i32) (result i32)
    block (result i32)
      try_table (catch $first 0)
        block (result i32)
          try_table (catch $second 0) local.get 0 throw $first end unreachable
        end drop
      end unreachable
    end)
  (func (export "all") (param i32) (result i32)
    block try_table (catch_all 0) local.get 0 throw $first end unreachable end i32.const 7)
  (func (export "rethrow") (param i32) (result i32)
    block (result i32)
      try_table (catch $first 0)
        block (result exnref)
          try_table (catch_all_ref 0) local.get 0 throw $first end unreachable
        end throw_ref
      end unreachable
    end)
  (func (export "catch-ref") (param i32) (result i32 exnref)
    block (result i32 exnref)
      try_table (catch_ref $first 0) local.get 0 throw $first end unreachable
    end)
  (func (export "throw-ref") (param exnref) local.get 0 throw_ref)
  (func (export "null") (param i32) (result i32)
    block try_table (catch_all 0) ref.null exn throw_ref end end i32.const -1)
  (func (export "trap") (param i32) (result i32)
    block try_table (catch_all 0) unreachable end end i32.const -1)
  (func (export "normal") (param i32) (result i32)
    block (result i32)
      try_table (result i32) (catch $first 0) local.get 0 end
    end)
  (func (export "loop") (param i32) (result i32)
    local.get 0 loop (param i32) (result i32)
      local.tee 0 if (result i32)
        try_table (result i32) (catch $first 1)
          local.get 0 i32.const 1 i32.sub throw $first
        end
      else i32.const 19 end
    end)
  (func (export "make") (param i32) (result anyref) local.get 0 struct.new $node)
  (func (export "read") (param anyref) (result i32) local.get 0 ref.cast (ref $node) struct.get $node 0)
  (func (export "mixed") (param v128 anyref) (result i32 i64 f32 f64 v128 anyref)
    block (result i32 i64 f32 f64 v128 anyref exnref)
      try_table (catch_ref $mixed 0)
        i32.const -19 i64.const -123 f32.const -0 f64.const nan:0x8000000001234
        local.get 0 local.get 1 throw $mixed
      end unreachable
    end drop call $collect)
)
