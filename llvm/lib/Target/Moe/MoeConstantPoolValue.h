//===-- MoeConstantPoolValue.h - Moe constantpool value --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A constant-pool entry whose CONTENTS are an address that no LLVM Constant
// can express: either a raw MCSymbol, or another constant-pool entry.
//
// Both exist for the same reason. LOADabs always dereferences its trailing
// address operand (see the spec's LOAD section), so getting any address into a
// register means reading it from somewhere that already holds it as data.
//
//  - A symbol: the CALL sequence's return-address label. A plain .addSym on
//    LOADabs would treat the label's address as something to read *from*
//    rather than as the value to load.
//  - Another pool entry: the address of a pooled aggregate, which is what
//    every reference to a table hoisted into the constant pool needs. There
//    is no Constant for "the address of constant-pool entry 3", and the label
//    it will be given does not exist until the AsmPrinter invents it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOECONSTANTPOOLVALUE_H
#define LLVM_LIB_TARGET_MOE_MOECONSTANTPOOLVALUE_H

#include "llvm/CodeGen/MachineConstantPool.h"

namespace llvm {

class MCSymbol;

class MoeConstantPoolValue : public MachineConstantPoolValue {
  MCSymbol *Sym;
  /// The index of the constant-pool entry whose address this entry holds, or
  /// -1 when this entry holds `Sym` instead.
  int CPIRef;

  MoeConstantPoolValue(Type *Ty, MCSymbol *Sym, int CPIRef);

public:
  static MoeConstantPoolValue *Create(Type *Ty, MCSymbol *Sym);
  static MoeConstantPoolValue *CreateCPIRef(Type *Ty, unsigned CPI);

  MCSymbol *getSymbol() const { return Sym; }
  bool isCPIRef() const { return CPIRef >= 0; }
  unsigned getCPIRef() const { return (unsigned)CPIRef; }

  int getExistingMachineCPValue(MachineConstantPool *CP,
                                Align Alignment) override;

  void addSelectionDAGCSEId(FoldingSetNodeID &ID) override;

  void print(raw_ostream &O) const override;
};

} // end namespace llvm

#endif
