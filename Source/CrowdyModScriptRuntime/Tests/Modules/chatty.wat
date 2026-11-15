(module
  (import "ck" "log" (func $log (param i32 i32 i32)))
  (memory (export "memory") 1)
  (global $fuel (export "ck_fuel") (mut i64) (i64.const 0))
  (global $heap (mut i32) (i32.const 8192))
  (data (i32.const 16) "line")

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

  ;; Logs one line of 1,500 'a' bytes, then 29 lines of "line": 30 lines at once.
  (func (export "init")
    (local $left i32)
    (memory.fill (i32.const 4096) (i32.const 97) (i32.const 1500))
    (call $log (i32.const 1) (i32.const 4096) (i32.const 1500))
    (local.set $left (i32.const 29))
    (loop $more
      (call $log (i32.const 1) (i32.const 16) (i32.const 4))
      (local.set $left (i32.sub (local.get $left) (i32.const 1)))
      (br_if $more (i32.gt_s (local.get $left) (i32.const 0)))))

  (func (export "tick") (param $dt i32))

  (func (export "handle_invoke") (param i32 i32) (result i64)
    (i64.const 0)))
