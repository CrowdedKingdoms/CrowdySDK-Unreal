(module
  (import "ck" "host_call" (func $host_call (param i32 i32) (result i64)))
  (memory (export "memory") 1)
  (global $fuel (export "ck_fuel") (mut i64) (i64.const 0))

  ;; Makes a host call from inside ck_alloc. An over-limit request is answered at once, so unchecked this recurses forever.
  (func (export "ck_alloc") (param $size i32) (result i32)
    (drop (call $host_call (i32.const 0) (i32.const 300000)))
    (i32.const 1024))

  (func (export "ck_free") (param i32 i32))

  (func (export "init"))

  (func (export "tick") (param $dt i32)
    (drop (call $host_call (i32.const 0) (i32.const 300000))))

  (func (export "handle_invoke") (param i32 i32) (result i64)
    (i64.const 0)))
