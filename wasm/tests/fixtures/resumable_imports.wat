(module
  (import "p" "values" (func $values (param i32 v128) (result anyref i32 v128)))
  (func (export "direct") (param i32 v128) (result anyref i32 v128)
    local.get 0 local.get 1 call $values)
  (func (export "tail") (param i32 v128) (result anyref i32 v128)
    local.get 0 local.get 1 return_call $values)
)
