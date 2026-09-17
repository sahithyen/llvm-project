; RUN: llc -mtriple=moe -relocation-model=static < %s | FileCheck %s --check-prefix=STATIC
; RUN: llc -mtriple=moe -relocation-model=pic < %s | FileCheck %s --check-prefix=PIC

; Every way compiled code names an address, in both relocation models. The two
; differ only in the addressing mode of the trailing operand word - same
; instructions, same count, same order - which is what makes -fPIC free on this
; target. See MoeFixupKinds.h.

@g = global i32 5

declare i32 @callee(i32)

; A global's address comes out of a constant-pool word either way; only how that
; word is reached changes.
define i32 @load_global() {
; STATIC-LABEL: load_global:
; STATIC:         load.w .LCPI0_0 -> gp0
; PIC-LABEL:    load_global:
; PIC:            load.w.pc .LCPI0_0 -> gp0
  %v = load i32, ptr @g
  ret i32 %v
}

define i32 @call_and_branch(i32 %n) {
; The pool word below holds a call's return address - a code label, and so one
; of the few things in a PIE the loader really does have to patch. It therefore
; belongs in writable, relocated-then-read-only memory rather than .rodata.
; PIC:          .section .data.rel.ro
; STATIC-LABEL: call_and_branch:
; PIC-LABEL:    call_and_branch:
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %yes, label %no

; A conditional branch to one of this function's own blocks, which is the one
; place the two models now produce the *same* instruction: MoeShortBranches
; gives any branch that reaches +-2 KiB the short PC-relative form regardless of
; relocation model, because it is smaller and faster and needs no relocation
; either way. See MoeShortBranches.cpp - the ISA evaluation's N1.
; STATIC:         jump.LT.pc.s .LBB1_
; PIC:            jump.LT.pc.s .LBB1_

yes:
; The return address a call pushes, then the call's outbound jump.
; STATIC:         load.w .LCPI1_
; STATIC:         jump.al callee
; PIC:            load.w.pc .LCPI1_
; PIC:            jump.al.pc callee
  %r = call i32 @callee(i32 %n)
  ret i32 %r

no:
  ret i32 0
}
