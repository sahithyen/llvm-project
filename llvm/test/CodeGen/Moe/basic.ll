; RUN: llc < %s -mtriple=moe -O0 | FileCheck %s
;
; Milestone 1's "done" bar (see the Milestone 1 plan): a stack-allocated
; local with a store/load round trip, and enough simultaneously-live values
; to force a register-allocator spill, proving both Register-indirect
; addressing paths - MoeISelDAGToDAG::SelectAddr/eliminateFrameIndex (for
; the alloca) and MoeInstrInfo::storeRegToStackSlot/loadRegFromStackSlot
; (for the spill) - actually work, not just avoid being exercised.

; CHECK-LABEL: alloca_roundtrip:
; CHECK: move sp -> gp4
; CHECK: sub gp4, gp5 -> gp4
; CHECK: move gp4 -> sp
; CHECK: store.w gp0 -> [sp+0]
; CHECK: load.w [sp+0] -> gp1
; CHECK: add gp0, gp1 -> gp0
; CHECK: add gp4, gp5 -> gp4
; CHECK: move gp4 -> sp
; CHECK: pop gp4
; CHECK: move gp4 -> ia
define i32 @alloca_roundtrip(i32 %x) {
  %ptr = alloca i32
  store i32 %x, ptr %ptr
  %other = add i32 %x, 1
  %v = load i32, ptr %ptr
  %r = add i32 %v, %other
  ret i32 %r
}

; CHECK-LABEL: forced_spill:
; CHECK: push gp6
; CHECK: push gp7
; CHECK: store.w {{.*}} -> [sp+{{[0-9]+}}] {{.*}}Folded Spill
; CHECK: load.w [sp+{{[0-9]+}}] -> {{.*}}Folded Reload
; CHECK: pop gp7
; CHECK: pop gp6
; CHECK: pop gp4
; CHECK: move gp4 -> ia
define i32 @forced_spill(i32 %a, i32 %b, i32 %c, i32 %d) {
  %s1 = add i32 %a, 11
  %s2 = add i32 %b, 22
  %s3 = add i32 %c, 33
  %s4 = add i32 %d, 44
  %s5 = xor i32 %a, %b
  %s6 = xor i32 %c, %d
  %s7 = and i32 %a, %c
  %s8 = and i32 %b, %d
  %t1 = add i32 %s1, %s5
  %t2 = add i32 %s2, %s6
  %t3 = add i32 %s3, %s7
  %t4 = add i32 %s4, %s8
  %u1 = add i32 %t1, %t2
  %u2 = add i32 %t3, %t4
  %r = add i32 %u1, %u2
  ret i32 %r
}
