//===-- Moe.td - Describe the Moe Target Machine -----------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This is the top level entry point for the Moe target.
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// Moe supported processors. No subtarget feature variants for Milestone 1 -
// the ISA has no optional extensions, just the one fixed 16-opcode set.
//===----------------------------------------------------------------------===//

def : Processor<"generic", NoItineraries, []>;

//===----------------------------------------------------------------------===//
// Register File Description
//===----------------------------------------------------------------------===//

include "MoeRegisterInfo.td"

//===----------------------------------------------------------------------===//
// Calling Convention Description
//===----------------------------------------------------------------------===//

include "MoeCallingConv.td"

//===----------------------------------------------------------------------===//
// Instruction Descriptions
//===----------------------------------------------------------------------===//

include "MoeInstrInfo.td"

defm : RemapAllTargetPseudoPointerOperands<GPR>;

def MoeInstrInfo : InstrInfo;

//===----------------------------------------------------------------------===//
// Assembly Printer
//===----------------------------------------------------------------------===//

def MoeAsmWriter : AsmWriter {
  string AsmWriterClassName = "InstPrinter";
}

//===----------------------------------------------------------------------===//
// Target Declaration
//===----------------------------------------------------------------------===//

def Moe : Target {
  let InstructionSet = MoeInstrInfo;
}
