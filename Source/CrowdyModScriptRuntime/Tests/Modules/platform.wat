;; Sets the player state, reads it back, and only when the read answered a state row sends one spatial client event and two
;; channel messages: to channel 13, then 12.
(module
  (import "ck" "host_call" (func $host_call (param i32 i32) (result i64)))
  (memory (export "memory") 1)
  (global $fuel (export "ck_fuel") (mut i64) (i64.const 0))
  (global $heap (mut i32) (i32.const 1024))
  (data (i32.const 64) "{\"fn\":\"user_state_set\",\"args\":{\"stateBase64\":\"AAE=\"}}")
  (data (i32.const 160) "{\"fn\":\"user_state_get\",\"args\":{}}")
  (data (i32.const 224) "{\"fn\":\"emit_spatial\",\"args\":{\"kind\":\"client_event\",\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"payloadBase64\":\"BwA=\",\"distance\":2}}")
  (data (i32.const 384) "{\"fn\":\"emit_channel\",\"args\":{\"channelId\":\"13\",\"payloadBase64\":\"aGk=\"}}")
  (data (i32.const 480) "{\"fn\":\"emit_channel\",\"args\":{\"channelId\":\"12\",\"payloadBase64\":\"aGk=\"}}")

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

  (func (export "init"))

  (func (export "tick") (param $dt i32)
    (local $answer i64)
    (local $at i32)
    (drop (call $host_call (i32.const 64) (i32.const 53)))
    (local.set $answer (call $host_call (i32.const 160) (i32.const 33)))
    (local.set $at (i32.wrap_i64 (i64.shr_u (local.get $answer) (i64.const 32))))
    ;; An answer of a row has t at 6 and { at 18: {"ok":true,"data":{
    (if (i32.and (i32.eq (i32.load8_u offset=6 (local.get $at)) (i32.const 116)) (i32.eq (i32.load8_u offset=18 (local.get $at)) (i32.const 123)))
      (then
        (drop (call $host_call (i32.const 224) (i32.const 121)))
        (drop (call $host_call (i32.const 384) (i32.const 70)))
        (drop (call $host_call (i32.const 480) (i32.const 70))))))

  (func (export "handle_invoke") (param i32 i32) (result i64)
    (i64.const 0)))
