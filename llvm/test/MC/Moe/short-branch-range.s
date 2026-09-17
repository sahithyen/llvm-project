; RUN: not llvm-mc -triple=moe -filetype=obj %s -o /dev/null 2>&1 | FileCheck %s

; The self-test for MoeAsmBackend's range check on fixup_moe_pcrel12, which is
; the net under MoeShortBranches: that pass predicts a layout in order to
; decide which branches can be four bytes, and this is where the displacement
; is finally known. A check that has quietly stopped checking looks exactly
; like a model that is never wrong, so it gets a test that breaks it on
; purpose - the same rule ERC's and the schematic checker's self-tests follow.

; CHECK: does not fit in the 12 signed bits
jump.al.pc.s far
.space 4096
far:
  jump.al.pc.s near
near:
