; RUN: llc < %s -mtriple=moe -O0 | FileCheck %s
;
; Milestone 3's software mul/div checkpoint (see the Milestone 3 plan's
; "Software multiply"/"Software divide/remainder" sections): MUL/UDIV/UREM/
; SDIV/SREM all expand to custom-inserter loops built from SHIFT/ADD/SUB/
; INCREMENT/JCC, since there's no hardware multiply/divide and no runtime
; library to LibCall out to. CHECK (not CHECK-NEXT) is used throughout,
; since exact spill-slot offsets are a register-allocator implementation
; detail, not part of what this test is verifying - the real numeric
; correctness proof is llvm-tests/run-mul-test.sh, run-muldiv-test.sh and
; run-sdiv-test.sh, which actually execute the compiled output.

; CHECK-LABEL: mulf:
; CHECK: xor {{.*}} -> [[RESULT:gp[0-9]+]]
; CHECK: increment {{.*}} += 32
; CHECK: shift.right {{.*}} -> {{.*}}
; CHECK: jump.CC
; CHECK: add {{.*}} -> {{.*}}
; CHECK: shift.left {{.*}} -> {{.*}}
; CHECK: increment {{.*}} += 1
; CHECK: sub {{.*}} -> (dead)
; CHECK: jump.NE
define i32 @mulf(i32 %a, i32 %b) {
  %r = mul i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: udivremf:
; CHECK: increment {{.*}} += 32
; CHECK: shift.left {{.*}} -> {{.*}}
; CHECK: shift.left.c {{.*}} -> {{.*}}
; CHECK: sub {{.*}} -> {{.*}}
; CHECK: jump.CS
; CHECK: sub {{.*}} -> {{.*}}
; CHECK: increment {{.*}} += 1
define i32 @udivremf(i32 %a, i32 %b) {
  %q = udiv i32 %a, %b
  %r = urem i32 %a, %b
  %shifted = shl i32 %q, 8
  %packed = or i32 %shifted, %r
  ret i32 %packed
}

; CHECK-LABEL: sdivremf:
; CHECK: xor {{.*}} -> {{.*}}
; CHECK: sub {{.*}} -> (dead)
; CHECK: jump.PL
; CHECK: shift.left.c {{.*}} -> {{.*}}
; CHECK: jump.CS
define i32 @sdivremf(i32 %a, i32 %b) {
  %q = sdiv i32 %a, %b
  %r = srem i32 %a, %b
  %shifted = shl i32 %q, 8
  %packed = or i32 %shifted, %r
  ret i32 %packed
}
