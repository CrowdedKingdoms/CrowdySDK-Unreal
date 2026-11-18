(module
  (memory (export "memory") 1)
  (global $fuel (export "ck_fuel") (mut i64) (i64.const 0))

  (func (export "ck_alloc") (param $size i32) (result i32)
    (i32.const 1024))

  (func (export "ck_free") (param i32 i32))

  ;; Exports no tick.
  (func (export "init"))

  (func (export "handle_invoke") (param i32 i32) (result i64)
    (i64.const 0)))
