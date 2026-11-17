(module
  (import "ck" "log" (func $log (param i32 i32 i32)))
  (memory (export "memory") 1)
  (global $fuel (export "ck_fuel") (mut i64) (i64.const 0))
  (global $heap (mut i32) (i32.const 1024))
  (data (i32.const 16) "boot")

  (func (export "ck_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local $end i32)
    (local.set $ptr (global.get $heap))
    (local.set $end (i32.and (i32.add (i32.add (local.get $ptr) (local.get $size)) (i32.const 7)) (i32.const -8)))
    (block $fits
      (br_if $fits (i32.le_u (local.get $end) (i32.shl (memory.size) (i32.const 16))))
      (br_if $fits (i32.ne (memory.grow (i32.add (i32.shr_u (i32.sub (local.get $end) (i32.shl (memory.size) (i32.const 16))) (i32.const 16)) (i32.const 1))) (i32.const -1)))
      (unreachable))
    (global.set $heap (local.get $end))
    (local.get $ptr))

  (func (export "ck_free") (param i32 i32))

  ;; Each init logs "boot" at level 2, so a log shows how many instances ran init.
  (func (export "init")
    (call $log (i32.const 2) (i32.const 16) (i32.const 4)))

  ;; Never touches ck_fuel, so every tick ends on the watchdog and the run starts a fresh instance.
  (func (export "tick") (param $dt i32)
    (loop $forever
      (br $forever)))

  (func (export "handle_invoke") (param i32 i32) (result i64)
    (i64.const 0)))
