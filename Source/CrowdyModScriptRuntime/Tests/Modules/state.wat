(module
  (import "ck" "state_get" (func $state_get (param i32 i32) (result i32)))
  (import "ck" "state_set" (func $state_set (param i32 i32) (result i32)))
  (memory (export "memory") 1)
  (global $fuel (export "ck_fuel") (mut i64) (i64.const 0))
  (data (i32.const 16) "abcd")

  (func (export "ck_alloc") (param $size i32) (result i32)
    (i32.const 1024))

  (func (export "ck_free") (param i32 i32))

  (func (export "init"))

  (func $expect (param $actual i32) (param $expected i32)
    (if (i32.ne (local.get $actual) (local.get $expected))
      (then (unreachable))))

  ;; Traps on the first limit that does not hold, so Ok means every check passed.
  (func (export "tick") (param $dt i32)
    ;; A 4-byte blob is kept.
    (call $expect (call $state_set (i32.const 16) (i32.const 4)) (i32.const 0))
    ;; 1 MiB + 1 is refused with 1 and the old blob stays.
    (call $expect (call $state_set (i32.const 0) (i32.const 1048577)) (i32.const 1))
    ;; A zero capacity only sizes the blob.
    (call $expect (call $state_get (i32.const 0) (i32.const 0)) (i32.const 4))
    ;; A short capacity copies a prefix and still answers the full length.
    (call $expect (call $state_get (i32.const 64) (i32.const 2)) (i32.const 4))
    (call $expect (i32.load8_u (i32.const 64)) (i32.const 97))
    (call $expect (i32.load8_u (i32.const 65)) (i32.const 98))
    (call $expect (i32.load8_u (i32.const 66)) (i32.const 0)))

  (func (export "handle_invoke") (param i32 i32) (result i64)
    (i64.const 0)))
