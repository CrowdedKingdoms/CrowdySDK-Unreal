(module
  (import "ck" "log" (func $log (param i32 i32 i32)))
  (import "ck" "host_call" (func $host_call (param i32 i32) (result i64)))
  (memory (export "memory") 1)
  (global $fuel (export "ck_fuel") (mut i64) (i64.const 0))
  (global $heap (mut i32) (i32.const 1024))
  (data (i32.const 16) "hello init")
  (data $emit "{\"fn\":\"emit_event\",\"args\":{\"name\":\"ping\",\"payload\":{\"n\":1}}}")
  (data $hud "{\"fn\":\"hud_set\",\"args\":{\"payload\":{\"text\":\"echo\"}}}")
  (global $started (mut i32) (i32.const 0))

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

  (func (export "init")
    (call $log (i32.const 1) (i32.const 16) (i32.const 10)))

  (func $emit
    (memory.init $emit (i32.const 64) (i32.const 0) (i32.const 60))
    (drop (call $host_call (i32.const 64) (i32.const 60))))

  (func (export "tick") (param $dt i32)
    (if (i32.eqz (global.get $started))
      (then (global.set $started (i32.const 1)) (call $emit))))

  (func (export "on_event") (param $ptr i32) (param $len i32)
    (memory.init $hud (i32.const 512) (i32.const 0) (i32.const 51))
    (drop (call $host_call (i32.const 512) (i32.const 51)))
    (call $emit))

  (func (export "handle_invoke") (param i32 i32) (result i64)
    (i64.const 0)))
