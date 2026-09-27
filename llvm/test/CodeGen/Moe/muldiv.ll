; RUN: llc < %s -mtriple=moe -O0 -verify-machineinstrs | FileCheck %s
; RUN: llc < %s -mtriple=moe -O2 -verify-machineinstrs | FileCheck %s --check-prefixes=CHECK,OPT
;
; Candidate L8: MUL, MULHU, DIVU and REMU are MULDIV's four functions, so a
; variable multiply or unsigned divide is one instruction and no loop. A signed
; divide is a sign fix-up around DIVU/REMU, since there are no signed forms. A
; 64-bit multiply is MUL and MULHU on the halves, not a call to __muldi3. The
; numbers are checked by llvm-tests/run-mul-test.sh, run-muldiv-test.sh,
; run-sdiv-test.sh and run-arith-loops-test.sh, which execute them.

; CHECK-LABEL: mulf:
; CHECK-NOT: jump
; CHECK: mul gp{{[0-7]}}, gp{{[0-7]}} -> gp{{[0-7]}}
; CHECK-NOT: jump
; CHECK: move gp4 -> ia
define i32 @mulf(i32 %a, i32 %b) {
  %r = mul i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: udivremf:
; CHECK-NOT: jump
; CHECK-DAG: divu gp{{[0-7]}}, gp{{[0-7]}} -> gp{{[0-7]}}
; CHECK-DAG: remu gp{{[0-7]}}, gp{{[0-7]}} -> gp{{[0-7]}}
; CHECK: move gp4 -> ia
define i32 @udivremf(i32 %a, i32 %b) {
  %q = udiv i32 %a, %b
  %r = urem i32 %a, %b
  %shifted = shl i32 %q, 8
  %packed = or i32 %shifted, %r
  ret i32 %packed
}

; A divide by a constant stays a divide: MULDIV's DIVU costs what the MULHU of
; a magic-number division would.
; CHECK-LABEL: udiv10:
; CHECK-NOT: mulhu
; CHECK: divu
define i32 @udiv10(i32 %a) {
  %q = udiv i32 %a, 10
  ret i32 %q
}

; CHECK-LABEL: sdivremf:
; CHECK: shift.right.arith
; CHECK-DAG: divu
; CHECK-DAG: remu
; CHECK-NOT: jump
; CHECK: move gp4 -> ia
define i32 @sdivremf(i32 %a, i32 %b) {
  %q = sdiv i32 %a, %b
  %r = srem i32 %a, %b
  %shifted = shl i32 %q, 8
  %packed = or i32 %shifted, %r
  ret i32 %packed
}

; CHECK-LABEL: mul64:
; CHECK-NOT: __muldi3
; CHECK: mulhu
; CHECK: move gp4 -> ia
define i64 @mul64(i64 %a, i64 %b) {
  %r = mul i64 %a, %b
  ret i64 %r
}

; A signed divide or remainder by a power of two stays shifts, at both widths,
; once optimising (at -O0 the combiner that folds it does not run):
; the kernel's 64-bit ones would otherwise be __divdi3/__moddi3 calls, which it
; does not link against.
; OPT-LABEL: sdiv4:
; OPT-NOT: divu
; OPT: shift.right.arith
; OPT: move gp4 -> ia
define i32 @sdiv4(i32 %a) {
  %q = sdiv i32 %a, 4
  ret i32 %q
}

; OPT-LABEL: srem8:
; OPT-NOT: remu
; OPT: move gp4 -> ia
define i32 @srem8(i32 %a) {
  %r = srem i32 %a, 8
  ret i32 %r
}

; OPT-LABEL: sdiv64pow2:
; OPT-NOT: __divdi3
; OPT: move gp4 -> ia
define i64 @sdiv64pow2(i64 %a) {
  %q = sdiv i64 %a, 4096
  ret i64 %q
}
