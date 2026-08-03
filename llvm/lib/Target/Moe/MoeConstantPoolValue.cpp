//===-- MoeConstantPoolValue.cpp - Moe constantpool value ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MoeConstantPoolValue.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

MoeConstantPoolValue::MoeConstantPoolValue(Type *Ty, MCSymbol *Sym)
    : MachineConstantPoolValue(Ty), Sym(Sym) {}

MoeConstantPoolValue *MoeConstantPoolValue::Create(Type *Ty, MCSymbol *Sym) {
  return new MoeConstantPoolValue(Ty, Sym);
}

int MoeConstantPoolValue::getExistingMachineCPValue(MachineConstantPool *CP,
                                                     Align Alignment) {
  const std::vector<MachineConstantPoolEntry> &Constants = CP->getConstants();
  for (unsigned i = 0, e = Constants.size(); i != e; ++i) {
    if (Constants[i].isMachineConstantPoolEntry() &&
        Constants[i].getAlign() >= Alignment) {
      auto *CPV =
          static_cast<MoeConstantPoolValue *>(Constants[i].Val.MachineCPVal);
      if (CPV->Sym == Sym)
        return i;
    }
  }
  return -1;
}

void MoeConstantPoolValue::addSelectionDAGCSEId(FoldingSetNodeID &ID) {
  ID.AddPointer(Sym);
}

void MoeConstantPoolValue::print(raw_ostream &O) const { O << Sym->getName(); }
