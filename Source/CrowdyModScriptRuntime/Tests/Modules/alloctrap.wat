(module
  (import "ck" "host_call" (func $host_call (param i32 i32) (result i64)))
  (memory (export "memory") 1)
  (global $fuel (export "ck_fuel") (mut i64) (i64.const 0))

  (func (export "ck_alloc") (param $size i32) (result i32)
    (unreachable))

  (func (export "ck_free") (param i32 i32))

  (func (export "init"))

  ;; An over-limit request is answered by the runtime itself, which then asks ck_alloc for room.
  (func (export "tick") (param $dt i32)
    (drop (call $host_call (i32.const 0) (i32.const 300000))))

  (func (export "handle_invoke") (param i32 i32) (result i64)
    (i64.const 0)))
