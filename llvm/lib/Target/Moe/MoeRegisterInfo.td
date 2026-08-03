//===-- MoeRegisterInfo.td - Moe Register defs ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Moe's 16 architectural registers, all 32-bit, selected by the 4-bit
// "Register selection" encoding (see 'Register selection' in the Encoding
// chapter of specification/). Unlike many targets, Moe has no sub-register
// aliasing at all: Size/Bytes selection is purely how much of a register's
// bytes a given instruction touches, not a distinct architectural register
// (see CLAUDE.md's "Conventions to follow when editing the emulator").
//
//===----------------------------------------------------------------------===//

class MoeReg<bits<4> num, string n> : Register<n> {
  field bits<4> Num = num;
  let Namespace = "Moe";
  let HWEncoding{3-0} = num;
  let DwarfNumbers = [num];
}

//===----------------------------------------------------------------------===//
//  Registers
//===----------------------------------------------------------------------===//

def GP0 : MoeReg<0, "gp0">;
def GP1 : MoeReg<1, "gp1">;
def GP2 : MoeReg<2, "gp2">;
def GP3 : MoeReg<3, "gp3">;
def GP4 : MoeReg<4, "gp4">;
def GP5 : MoeReg<5, "gp5">;
def GP6 : MoeReg<6, "gp6">;
def GP7 : MoeReg<7, "gp7">;
def IA  : MoeReg<8, "ia">;
def SP  : MoeReg<9, "sp">;
def TT  : MoeReg<10, "tt">;
def F   : MoeReg<11, "f">;
def IM  : MoeReg<12, "im">;
def IFS : MoeReg<13, "ifs">;
def RA  : MoeReg<14, "ra">;
def RF  : MoeReg<15, "rf">;

//===----------------------------------------------------------------------===//
//  Register classes
//===----------------------------------------------------------------------===//

// GP0-3 are argument/caller-saved (GP0 also the return value); GP4-5 are
// caller-saved scratch reserved for literal-pool/call-sequence temporaries;
// GP6-7 are callee-saved. See the "ABI" section of the Milestone 1 plan.
// Allocation order lists caller-saved-for-arguments first, scratch last
// (scratch registers are reserved below and never handed to the allocator
// anyway, but keeping them out of the preferred order documents intent).
def GPR : RegisterClass<"Moe", [i32], 32,
  (add GP0, GP1, GP2, GP3, GP6, GP7, GP4, GP5)>;

// MOVE (and only MOVE) can target any of the 16 registers per 'Register
// selection' in the Encoding chapter - e.g. the return sequence's `MOVE
// GP4 -> IA` and MoeFrameLowering's `MOVE SP -> GP4`/`MOVE GP4 -> SP`. This
// class is never used for ordinary SSA-value allocation (MOVE has no
// SelectionDAG Pat - see MoeInstrInfo.td), only as the operand type for
// hand-built MOVE MachineInstrs with fixed physical registers.
def AllReg : RegisterClass<"Moe", [i32], 32,
  (add GPR, IA, SP, TT, F, IM, IFS, RA, RF)>;
