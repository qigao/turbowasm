(module
  (type $v (func (param v128) (result v128)))
  (import "p" "identity" (func $identity (type $v)))
  (import "p" "table" (table 4 funcref))
  (func (export "direct") (type $v) local.get 0 call $identity)
  (func (export "tail") (type $v) local.get 0 return_call $identity)
  (func (export "table-call") (type $v) local.get 0 i32.const 0 call_indirect (type $v))
  (func (export "table-tail") (type $v) local.get 0 i32.const 0 return_call_indirect (type $v))
)
