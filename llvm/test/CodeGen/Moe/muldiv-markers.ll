; Candidate L8's pricing markers (MoeMulDivMarks.h): emitted only under
; MOE_MULDIV_MARKERS, naming the multiplier or divisor at a begin marker, with
; a constant second operand given its own code, and whole 64-bit helpers
; bracketed at their prologue and epilogue. MOE_NO_SHIFT_ADD_MUL, because a
; multiply by a constant is otherwise no loop at all (mul-const.ll) - and the
; constant codes still have to work for the arm that measures that.
; RUN: llc -mtriple=moe -O2 < %s | FileCheck %s --check-prefix=OFF
; RUN: env MOE_MULDIV_MARKERS=1 MOE_NO_SHIFT_ADD_MUL=1 \
; RUN:   llc -mtriple=moe -O2 -verify-machineinstrs < %s | FileCheck %s

; OFF-NOT: muldivmark

; CHECK-LABEL: mul_var:
; CHECK: muldivmark 1, [[B:gp[0-9]]]
; CHECK: muldivmark 2,
define i32 @mul_var(i32 %a, i32 %b) {
  %r = mul i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: mul_const:
; CHECK: muldivmark 11,
; CHECK: muldivmark 12,
define i32 @mul_const(i32 %a) {
  %r = mul i32 %a, 11
  ret i32 %r
}

; CHECK-LABEL: div_var:
; CHECK: muldivmark 3,
; CHECK: muldivmark 4,
define i32 @div_var(i32 %a, i32 %b) {
  %r = udiv i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: div_const:
; CHECK: muldivmark 13,
; CHECK: muldivmark 14,
define i32 @div_const(i32 %a) {
  %r = urem i32 %a, 10
  ret i32 %r
}

; CHECK-LABEL: sdiv_var:
; CHECK: muldivmark 5,
; CHECK: muldivmark 6,
define i32 @sdiv_var(i32 %a, i32 %b) {
  %r = sdiv i32 %a, %b
  ret i32 %r
}

; A helper is bracketed as a whole, begin before anything else it does and end
; after its callee-saved registers are restored.
; CHECK-LABEL: __muldi3:
; CHECK-NEXT: %bb.0:
; CHECK-NEXT: muldivmark 7, gp0
; CHECK: muldivmark 8, gp0
; CHECK-NEXT: pop gp4
define i64 @__muldi3(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}
