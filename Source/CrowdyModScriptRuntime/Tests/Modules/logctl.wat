(module
  (import "ck" "log" (func $log (param i32 i32 i32)))
  (memory (export "memory") 1)
  (global $fuel (export "ck_fuel") (mut i64) (i64.const 0))
  (data (i32.const 16) "one\ntwo\rthree\1bfour")

  (func (export "ck_alloc") (param $size i32) (result i32)
    (i32.const 1024))

  (func (export "ck_free") (param i32 i32))

  ;; One log call whose text holds a newline, a carriage return and an escape.
  (func (export "init")
    (call $log (i32.const 1) (i32.const 16) (i32.const 18)))

  (func (export "tick") (param $dt i32))

  (func (export "handle_invoke") (param i32 i32) (result i64)
    (i64.const 0)))
