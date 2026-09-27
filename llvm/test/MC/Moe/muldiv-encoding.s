; RUN: llvm-mc -triple=moe -show-encoding %s | FileCheck %s
; RUN: not llvm-mc -triple=moe %s --defsym=GONE=1 2>&1 | FileCheck %s --check-prefix=GONE

; MULDIV (candidate L8): opcode 0b1011, bit 11 reserved, Function in bits 9-10,
; B in 6-8, A in 3-5, O in 0-2 - see 'MULDIV' in the Instruction set chapter.
; 0xB000 | 5 << 6 | 6 << 3 | 7 = 0xB177, then 0x200 per Function.

; CHECK: mul gp6, gp5 -> gp7    ; encoding: [0x77,0xb1]
; CHECK: mulhu gp6, gp5 -> gp7  ; encoding: [0x77,0xb3]
; CHECK: divu gp6, gp5 -> gp7   ; encoding: [0x77,0xb5]
; CHECK: remu gp6, gp5 -> gp7   ; encoding: [0x77,0xb7]
mul gp6, gp5 -> gp7
mulhu gp6, gp5 -> gp7
divu gp6, gp5 -> gp7
remu gp6, gp5 -> gp7

; INCREMENT gave its opcode up; the mnemonic must not assemble to anything.
; GONE: error: {{.*}}
.ifdef GONE
increment gp0 += 1
.endif
