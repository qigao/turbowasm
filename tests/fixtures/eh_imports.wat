(module
  (import "p" "tag" (tag $tag (param i32)))
  (import "p" "throw" (func $throw (param i32)))
  (tag $different (param i32))
  (func (export "catch") (param i32) (result i32)
    block (result i32)
      block (result i32)
        try_table (catch $different 0) (catch $tag 1) local.get 0 call $throw end unreachable
      end drop i32.const -1 return
    end)
  (func (export "tail") (param i32) local.get 0 return_call $throw)
)
