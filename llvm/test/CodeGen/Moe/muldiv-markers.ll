; Candidate L8's pricing markers (MoeMulDivMarks.h) on L8's own branch: the
; 32-bit multiplies and divides are MULDIV instructions with no loop to mark, so
; only whole 64-bit helper functions are bracketed, at their prologue and
; epilogue. Emitted only under MOE_MULDIV_MARKERS.
; RUN: llc -mtriple=moe -O2 < %s | FileCheck %s --check-prefix=OFF
; RUN: env MOE_MULDIV_MARKERS=1 llc -mtriple=moe -O2 -verify-machineinstrs < %s \
; RUN:   | FileCheck %s

; OFF-NOT: muldivmark

; CHECK-LABEL: mul_var:
; CHECK-NOT: muldivmark
; CHECK: mul
define i32 @mul_var(i32 %a, i32 %b) {
  %r = mul i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: __udivdi3:
; CHECK-NEXT: %bb.0:
; CHECK-NEXT: muldivmark 9, gp0
; CHECK: muldivmark 10, gp0
; CHECK-NEXT: pop gp4
define i64 @__udivdi3(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}
