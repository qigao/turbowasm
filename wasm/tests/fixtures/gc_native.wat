(module
  (type $s (struct (field (mut i32))))
  (type $p (struct (field (mut i8))))
  (type $a (array (mut i32)))
  (type $b (array (mut i8)))
  (type $f (array (mut funcref)))
  (type $v (struct (field i32) (field i64) (field f32) (field f64) (field v128) (field anyref)))
  (type $r (struct (field anyref)))
  (type $av (array (mut v128)))
  (import "h" "collect" (func $collect))
  (data $data "\ff\80\7f\01")
  (elem $elem func $one)
  (func $one (result i32) i32.const 1)
  (func (export "op0") (param i32) (result i32) local.get 0 struct.new $s struct.get $s 0)
  (func (export "op1") (param i32) (result i32) struct.new_default $s struct.get $s 0)
  (func (export "op2") (param i32) (result i32) local.get 0 struct.new $s struct.get $s 0)
  (func (export "op3") (param i32) (result i32) local.get 0 struct.new $p struct.get_s $p 0)
  (func (export "op4") (param i32) (result i32) local.get 0 struct.new $p struct.get_u $p 0)
  (func (export "op5") (param i32) (result i32) (local (ref $s))
    struct.new_default $s local.tee 1 local.get 0 struct.set $s 0 local.get 1 struct.get $s 0)
  (func (export "op6") (param i32) (result i32) local.get 0 i32.const 3 array.new $a array.len)
  (func (export "op7") (param i32) (result i32) i32.const 3 array.new_default $a array.len)
  (func (export "op8") (param i32) (result i32)
    i32.const 1 local.get 0 i32.const 3 array.new_fixed $a 3 i32.const 1 array.get $a)
  (func (export "op9") (param i32) (result i32)
    i32.const 0 i32.const 4 array.new_data $b $data i32.const 0 array.get_u $b)
  (func (export "op10") (param i32) (result i32)
    i32.const 0 i32.const 1 array.new_elem $f $elem i32.const 0 array.get $f ref.is_null)
  (func (export "op11") (param i32) (result i32)
    local.get 0 i32.const 3 array.new $a i32.const 1 array.get $a)
  (func (export "op12") (param i32) (result i32)
    local.get 0 i32.const 3 array.new $b i32.const 1 array.get_s $b)
  (func (export "op13") (param i32) (result i32)
    local.get 0 i32.const 3 array.new $b i32.const 1 array.get_u $b)
  (func (export "op14") (param i32) (result i32) (local (ref $a))
    i32.const 3 array.new_default $a local.tee 1 i32.const 1 local.get 0 array.set $a
    local.get 1 i32.const 1 array.get $a)
  (func (export "op15") (param i32) (result i32) i32.const 3 array.new_default $a array.len)
  (func (export "op16") (param i32) (result i32) (local (ref $a))
    i32.const 3 array.new_default $a local.tee 1 i32.const 0 local.get 0 i32.const 3 array.fill $a
    local.get 1 i32.const 2 array.get $a)
  (func (export "op17") (param i32) (result i32) (local (ref $a))
    local.get 0 i32.const 2 i32.const 3 array.new_fixed $a 3 local.set 1
    local.get 1 i32.const 1 local.get 1 i32.const 0 i32.const 2 array.copy $a $a
    local.get 1 i32.const 1 array.get $a)
  (func (export "op18") (param i32) (result i32) (local (ref $b))
    i32.const 4 array.new_default $b local.tee 1 i32.const 0 i32.const 0 i32.const 4 array.init_data $b $data
    local.get 1 i32.const 0 array.get_u $b)
  (func (export "op19") (param i32) (result i32) (local (ref $f))
    i32.const 1 array.new_default $f local.tee 1 i32.const 0 i32.const 0 i32.const 1 array.init_elem $f $elem
    local.get 1 i32.const 0 array.get $f ref.is_null)
  (func (export "op20") (param i32) (result i32) local.get 0 ref.i31 ref.test (ref i31))
  (func (export "op21") (param i32) (result i32) ref.null any ref.test (ref null $s))
  (func (export "op22") (param i32) (result i32)
    local.get 0 struct.new $s ref.cast (ref $s) struct.get $s 0)
  (func (export "op23") (param i32) (result i32) ref.null any ref.cast (ref null $s) ref.is_null)
  (func (export "op24") (param i32) (result i32)
    block (result (ref i31)) local.get 0 ref.i31 br_on_cast 0 (ref any) (ref i31) unreachable end i31.get_s)
  (func (export "op25") (param i32) (result i32)
    block (result (ref any)) local.get 0 ref.i31 br_on_cast_fail 0 (ref any) (ref $s) unreachable end
    ref.cast (ref i31) i31.get_s)
  (func (export "op26") (param i32) (result i32)
    local.get 0 ref.i31 extern.convert_any any.convert_extern ref.cast (ref i31) i31.get_s)
  (func (export "op27") (param i32) (result i32)
    local.get 0 ref.i31 extern.convert_any any.convert_extern ref.cast (ref i31) i31.get_s)
  (func (export "op28") (param i32) (result i32) local.get 0 ref.i31 i31.get_s)
  (func (export "op29") (param i32) (result i32) local.get 0 ref.i31 i31.get_s)
  (func (export "op30") (param i32) (result i32) local.get 0 ref.i31 i31.get_u)
  (func (export "mixed") (param i32) (result v128) (local (ref $v))
    local.get 0 i64.const -1 f32.const -0 f64.const nan:0x8000000001234
    v128.const i64x2 0x123456789abcdef 0x8000000000000000
    local.get 0 struct.new $s struct.new $v local.set 1 call $collect
    local.get 1 struct.get $v 5 ref.cast (ref $s) struct.get $s 0 local.get 0 i32.ne if unreachable end
    local.get 1 struct.get $v 4)
  (func (export "vector-array") (param v128) (result v128) (local (ref $av))
    local.get 0 i32.const 2 array.new $av local.tee 1 i32.const 1 local.get 0 array.set $av
    local.get 1 i32.const 0 array.get $av)
  (func (export "wide") (param i32) (result i32)
    local.get 0 local.get 0 local.get 0 local.get 0 local.get 0 local.get 0 local.get 0 local.get 0
    local.get 0 local.get 0 local.get 0 local.get 0 local.get 0 local.get 0 local.get 0 local.get 0
    array.new_fixed $a 16 i32.const 15 array.get $a)
  (func (export "dead-wide") (param i32) (result i32)
    unreachable array.new_fixed $a 4294967295 drop i32.const 0)
  (func (export "null") (param i32) (result i32) ref.null $s struct.get $s 0)
  (func (export "bounds") (param i32) (result i32)
    i32.const 2 array.new_default $a local.get 0 array.get $a)
  (func (export "cast") (param i32) (result i32) local.get 0 ref.i31 ref.cast (ref $s) struct.get $s 0)
  (func (export "allocate") (param i32) (result i32) local.get 0 array.new_default $a array.len)
  (func (export "collect-loop") (param i32) (result i32) (local (ref null $r))
    loop
      local.get 0 struct.new $s struct.new $r local.set 1
      local.get 0 i32.const 1 i32.sub local.tee 0 br_if 0
    end local.get 1 struct.get $r 0 ref.cast (ref $s) struct.get $s 0)
  (func (export "branch") (param i32) (result i32)
    block (result (ref null any))
      local.get 0 if (result anyref) i32.const 19 struct.new $s else ref.null any end
      br_on_cast 0 (ref null any) (ref $s) drop i32.const 7 return
    end ref.cast (ref $s) struct.get $s 0)
  (func (export "branch-fail") (param i32) (result i32)
    block (result anyref)
      local.get 0 if (result anyref) i32.const 19 struct.new $s else ref.null any end
      br_on_cast_fail 0 (ref null any) (ref $s) struct.get $s 0 return
    end ref.is_null)
  (func (export "loop") (param i32) (result i32) (local anyref)
    local.get 0 struct.new $s local.tee 1
    loop (param anyref) (result anyref)
      drop local.get 1 ref.null any local.set 1 br_on_cast 0 (ref null any) (ref $s)
    end ref.is_null)
  (func (export "convert") (param externref) (result externref) local.get 0 any.convert_extern call $collect extern.convert_any)
  (func (export "drop-data") (param i32) (result i32)
    data.drop $data i32.const 0 local.get 0 array.new_data $b $data array.len)
  (func (export "drop-elem") (param i32) (result i32)
    elem.drop $elem i32.const 0 local.get 0 array.new_elem $f $elem array.len)
)
