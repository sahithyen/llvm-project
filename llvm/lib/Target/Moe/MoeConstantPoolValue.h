//===-- MoeConstantPoolValue.h - Moe constantpool value --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Wraps a raw MCSymbol (not an LLVM Constant/GlobalValue) so it can sit in a
// MachineConstantPool entry. Used for the CALL sequence's return-address
// label: LOADabs always dereferences its trailing address operand (see the
// spec's LOAD section), so getting a label's address as a *value* into a
// register requires the same pool-indirection LowerGlobalAddress/
// LowerConstantPool use for every other constant - a plain .addSym(RetSym)
// on LOADabs is wrong, since that treats RetSym's address as something to
// read *from* rather than the value to load.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOECONSTANTPOOLVALUE_H
#define LLVM_LIB_TARGET_MOE_MOECONSTANTPOOLVALUE_H

#include "llvm/CodeGen/MachineConstantPool.h"

namespace llvm {

class MCSymbol;

class MoeConstantPoolValue : public MachineConstantPoolValue {
  MCSymbol *Sym;

  MoeConstantPoolValue(Type *Ty, MCSymbol *Sym);

public:
  static MoeConstantPoolValue *Create(Type *Ty, MCSymbol *Sym);

  MCSymbol *getSymbol() const { return Sym; }

  int getExistingMachineCPValue(MachineConstantPool *CP,
                                Align Alignment) override;

  void addSelectionDAGCSEId(FoldingSetNodeID &ID) override;

  void print(raw_ostream &O) const override;
};

} // end namespace llvm

#endif
