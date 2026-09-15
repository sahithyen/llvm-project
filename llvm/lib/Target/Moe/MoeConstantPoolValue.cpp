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

MoeConstantPoolValue::MoeConstantPoolValue(Type *Ty, MCSymbol *Sym, int CPIRef)
    : MachineConstantPoolValue(Ty), Sym(Sym), CPIRef(CPIRef) {}

MoeConstantPoolValue *MoeConstantPoolValue::Create(Type *Ty, MCSymbol *Sym) {
  return new MoeConstantPoolValue(Ty, Sym, -1);
}

MoeConstantPoolValue *MoeConstantPoolValue::CreateCPIRef(Type *Ty,
                                                          unsigned CPI) {
  return new MoeConstantPoolValue(Ty, nullptr, (int)CPI);
}

int MoeConstantPoolValue::getExistingMachineCPValue(MachineConstantPool *CP,
                                                     Align Alignment) {
  const std::vector<MachineConstantPoolEntry> &Constants = CP->getConstants();
  for (unsigned i = 0, e = Constants.size(); i != e; ++i) {
    if (Constants[i].isMachineConstantPoolEntry() &&
        Constants[i].getAlign() >= Alignment) {
      auto *CPV =
          static_cast<MoeConstantPoolValue *>(Constants[i].Val.MachineCPVal);
      if (CPV->Sym == Sym && CPV->CPIRef == CPIRef)
        return i;
    }
  }
  return -1;
}

void MoeConstantPoolValue::addSelectionDAGCSEId(FoldingSetNodeID &ID) {
  ID.AddPointer(Sym);
  ID.AddInteger(CPIRef);
}

void MoeConstantPoolValue::print(raw_ostream &O) const {
  if (isCPIRef())
    O << "&constantpool[" << CPIRef << "]";
  else
    O << Sym->getName();
}
