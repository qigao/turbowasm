(module
  (type $node (struct (field i32)))
  (type $unary (func (param i32) (result i32)))
  (import "h" "wait" (func $wait (result i32)))
  (global $effects (mut i32) (i32.const 0))
  (tag $error (param i32))
  (table 1 funcref)
  (elem (i32.const 0) func $leaf)
  (func $leaf (export "leaf") (param i32) (result i32)
    global.get $effects i32.const 1 i32.add global.set $effects
    local.get 0 call $wait i32.add)
  (func $direct (export "direct") (param i32) (result i32)
    local.get 0 call $leaf)
  (func (export "indirect") (param i32) (result i32)
    local.get 0 i32.const 0 call_indirect (type $unary))
  (func (export "reference") (param i32) (result i32)
    local.get 0 ref.func $leaf call_ref $unary)
  (func $tail (export "tail") (param i32) (result i32)
    local.get 0 i32.eqz if
      i32.const 0 return_call $leaf
    end
    local.get 0 i32.const 1 i32.sub return_call $tail)
  (func (export "values") (param i32 v128) (result anyref i32 v128)
    local.get 0 struct.new $node
    local.get 0 call $direct
    local.get 1)
  (func (export "read") (param anyref) (result i32)
    local.get 0 ref.cast (ref $node) struct.get $node 0)
  (func (export "effects") (result i32) global.get $effects)
  (func (export "eh") (param i32) (result i32)
    block (result i32)
      try_table (catch $error 0)
        local.get 0 call $direct throw $error
      end unreachable
    end)
  (func (export "trap") call $wait drop unreachable)
  (func (export "throw") call $wait throw $error)
  (func (export "allocate-after-wait") (result anyref)
    call $wait struct.new $node)
  (func (export "loop") (param i32) (result i32)
    block
      loop
        local.get 0 i32.eqz br_if 1
        global.get $effects i32.const 1 i32.add global.set $effects
        local.get 0 i32.const 1 i32.sub local.set 0 br 0
      end
    end
    global.get $effects)
)
