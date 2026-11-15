(module
  (memory (export "memory") 1)
  ;; ck_fuel must be a mutable i64.
  (global $fuel (export "ck_fuel") (mut i32) (i32.const 0))

  (func (export "ck_alloc") (param $size i32) (result i32)
    (i32.const 1024))

  (func (export "ck_free") (param i32 i32))

  (func (export "init"))

  (func (export "tick") (param $dt i32))

  (func (export "handle_invoke") (param i32 i32) (result i64)
    (i64.const 0)))
