(module
  (import "t" "get" (func $get (param i32) (result i64)))
  (import "t" "return" (func $return (param i64)))
  (import "e" "snew" (func $snew (result i64)))
  (import "e" "fnew" (func $fnew (result i64)))
  (import "e" "sread" (func $sread (param i32 i32 i32) (result i32)))
  (import "e" "swrite" (func $swrite (param i32 i32 i32) (result i32)))
  (import "e" "sreadsync" (func $sreadsync (param i32 i32 i32) (result i32)))
  (import "e" "swritesync" (func $swritesync (param i32 i32 i32) (result i32)))
  (import "e" "sread64" (func $sread64 (param i32 i64 i64) (result i64)))
  (import "e" "swrite64" (func $swrite64 (param i32 i64 i64) (result i64)))
  (import "e" "fread" (func $fread (param i32 i32) (result i32)))
  (import "e" "fwrite" (func $fwrite (param i32 i32) (result i32)))
  (import "e" "freadsync" (func $freadsync (param i32 i32) (result i32)))
  (import "e" "fwritesync" (func $fwritesync (param i32 i32) (result i32)))
  (import "e" "scancelr" (func $scancelr (param i32) (result i32)))
  (import "e" "scancelw" (func $scancelw (param i32) (result i32)))
  (import "e" "fcancelr" (func $fcancelr (param i32) (result i32)))
  (import "e" "fcancelw" (func $fcancelw (param i32) (result i32)))
  (import "e" "sdropr" (func $sdropr (param i32)))
  (import "e" "sdropw" (func $sdropw (param i32)))
  (import "e" "fdropr" (func $fdropr (param i32)))
  (import "e" "fdropw" (func $fdropw (param i32)))
  (import "e" "sforward" (func $sforward (param i32 i32)))
  (import "e" "fforward" (func $fforward (param i32 i32)))
  (import "e" "fread64" (func $fread64 (param i32 i64) (result i32)))
  (import "e" "fwrite64" (func $fwrite64 (param i32 i64) (result i32)))
  (import "e" "textread" (func $textread (param i32 i32 i32) (result i32)))
  (import "e" "textwrite" (func $textwrite (param i32 i32 i32) (result i32)))
  (import "e" "unitread" (func $unitread (param i32 i32 i32) (result i32)))
  (import "e" "unitwrite" (func $unitwrite (param i32 i32 i32) (result i32)))
  (memory (export "m32") 1)
  (memory (export "m64") i64 1)
  (func (export "snew")
    call $snew call $return)
  (func (export "fnew")
    call $fnew call $return)
  (func (export "sread")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    i32.const 2 call $get i32.wrap_i64
    call $sread i64.extend_i32_u call $return)
  (func (export "swrite")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    i32.const 2 call $get i32.wrap_i64
    call $swrite i64.extend_i32_u call $return)
  (func (export "sreadsync")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    i32.const 2 call $get i32.wrap_i64
    call $sreadsync i64.extend_i32_u call $return)
  (func (export "swritesync")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    i32.const 2 call $get i32.wrap_i64
    call $swritesync i64.extend_i32_u call $return)
  (func (export "sread64")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get
    i32.const 2 call $get
    call $sread64 call $return)
  (func (export "swrite64")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get
    i32.const 2 call $get
    call $swrite64 call $return)
  (func (export "fread")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    call $fread i64.extend_i32_u call $return)
  (func (export "fwrite")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    call $fwrite i64.extend_i32_u call $return)
  (func (export "freadsync")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    call $freadsync i64.extend_i32_u call $return)
  (func (export "fwritesync")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    call $fwritesync i64.extend_i32_u call $return)
  (func (export "scancelr")
    i32.const 0 call $get i32.wrap_i64
    call $scancelr i64.extend_i32_u call $return)
  (func (export "scancelw")
    i32.const 0 call $get i32.wrap_i64
    call $scancelw i64.extend_i32_u call $return)
  (func (export "fcancelr")
    i32.const 0 call $get i32.wrap_i64
    call $fcancelr i64.extend_i32_u call $return)
  (func (export "fcancelw")
    i32.const 0 call $get i32.wrap_i64
    call $fcancelw i64.extend_i32_u call $return)
  (func (export "sdropr")
    i32.const 0 call $get i32.wrap_i64
    call $sdropr i64.const 0 call $return)
  (func (export "sdropw")
    i32.const 0 call $get i32.wrap_i64
    call $sdropw i64.const 0 call $return)
  (func (export "fdropr")
    i32.const 0 call $get i32.wrap_i64
    call $fdropr i64.const 0 call $return)
  (func (export "fdropw")
    i32.const 0 call $get i32.wrap_i64
    call $fdropw i64.const 0 call $return)
  (func (export "sforward")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    call $sforward i64.const 0 call $return)
  (func (export "fforward")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    call $fforward i64.const 0 call $return)
  (func (export "fread64")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get
    call $fread64 i64.extend_i32_u call $return)
  (func (export "fwrite64")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get
    call $fwrite64 i64.extend_i32_u call $return)
  (func (export "textread")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    i32.const 2 call $get i32.wrap_i64
    call $textread i64.extend_i32_u call $return)
  (func (export "textwrite")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    i32.const 2 call $get i32.wrap_i64
    call $textwrite i64.extend_i32_u call $return)
  (func (export "unitread")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    i32.const 2 call $get i32.wrap_i64
    call $unitread i64.extend_i32_u call $return)
  (func (export "unitwrite")
    i32.const 0 call $get i32.wrap_i64
    i32.const 1 call $get i32.wrap_i64
    i32.const 2 call $get i32.wrap_i64
    call $unitwrite i64.extend_i32_u call $return)
)
