;; Conformance-harness smoke for v128 values and permitted result sets.
(module
  (func (export "identity") (param v128) (result v128)
    (local.get 0))
  (func (export "nan-lanes") (result v128)
    (v128.const f32x4 nan:canonical 1.0 -0.0 2.0))
)

(assert_return
  (invoke "identity" (v128.const i32x4 1 2 3 4))
  (v128.const i32x4 1 2 3 4))

(assert_return
  (invoke "identity" (v128.const i32x4 1 2 3 4))
  (either
    (v128.const i32x4 9 9 9 9)
    (v128.const i32x4 1 2 3 4)))

(assert_return
  (invoke "nan-lanes")
  (v128.const f32x4 nan:canonical 1.0 -0.0 2.0))
