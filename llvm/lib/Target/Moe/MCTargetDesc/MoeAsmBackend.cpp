//===-- MoeAsmBackend.cpp - Moe Assembler Backend ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/MoeFixupKinds.h"
#include "MCTargetDesc/MoeMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
class MoeAsmBackend : public MCAsmBackend {
public:
  MoeAsmBackend() : MCAsmBackend(llvm::endianness::little) {}

  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createMoeELFObjectWriter();
  }

  // Two fixup kinds, and they are shaped quite differently. An absolute
  // reference is the generic FK_Data_4 over a whole word, which the base class
  // already handles correctly. A PC-relative one writes a 28-bit field with
  // four bits of base-register select underneath it, which nothing generic
  // knows how to do - hence both overrides below.
  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override {
    if (Kind == Moe::fixup_moe_pcrel28)
      return {"fixup_moe_pcrel28", /*TargetOffset=*/4, /*TargetSize=*/28,
              /*Flags=*/0};
    return MCAsmBackend::getFixupKindInfo(Kind);
  }

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;
};

void MoeAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                const MCValue &Target, uint8_t *Data,
                                uint64_t Value, bool IsResolved) {
  maybeAddReloc(F, Fixup, Target, Value, IsResolved);
  if (!Value)
    return;

  if (Fixup.getKind() == Moe::fixup_moe_pcrel28) {
    // The displacement occupies bits 31-4, so it is shifted into place rather
    // than written where it lies - and the four bits underneath it are the base
    // register select the streamer has already put there, which is why the
    // OR-in below must not disturb them.
    //
    // Range is checked rather than truncated: 28 signed bits reach +-128 MiB,
    // which nothing here comes close to, and a silent wrap would produce a
    // program that jumps somewhere plausible instead of one that fails to link.
    int64_t Displacement = static_cast<int64_t>(Value);
    if (!isInt<28>(Displacement)) {
      Asm->getContext().reportError(
          Fixup.getLoc(),
          "PC-relative displacement of " + Twine(Displacement) +
              " does not fit in the 28 signed bits of a trailing operand word");
      return;
    }
    uint32_t Field = static_cast<uint32_t>(Displacement) << 4;
    for (unsigned i = 0; i != 4; ++i)
      Data[i] |= uint8_t((Field >> (i * 8)) & 0xff);
    return;
  }

  MCFixupKindInfo Info = getFixupKindInfo(Fixup.getKind());
  unsigned NumBytes = alignTo(Info.TargetSize + Info.TargetOffset, 8) / 8;
  assert(Fixup.getOffset() + NumBytes <= F.getSize() && "Invalid fixup offset!");
  for (unsigned i = 0; i != NumBytes; ++i)
    Data[i] |= uint8_t((Value >> (i * 8)) & 0xff);
}

bool MoeAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                  const MCSubtargetInfo *STI) const {
  // Moe has no NOP opcode - all 16 opcodes are real instructions (see
  // CLAUDE.md's "no opcode sharing" note) - so the no-op has to be some real
  // instruction with no effect. `move gp0 -> gp0` with a full 0b1111 byte mask
  // is the obvious one: MOVE is the only register-to-register copy, it does
  // not touch the flags (see 'Flags' in the Encoding chapter, which lists the
  // ALU instructions that do, and MOVE is not among them), and copying a
  // register onto itself changes nothing at all.
  //
  // This used to be a zero-fill, on the reasoning that padding is never
  // reached by control flow. That reasoning holds for the padding between an
  // instruction and its own trailing operand word, which hardware skips by
  // construction - and it is still a zero-fill, emitted by MoeELFStreamer with
  // an explicit fill byte rather than through here. It does NOT hold for
  // `.p2align` in the middle of a run of instructions, which hand-written
  // assembly does routinely and compiled output never did, and which lands
  // padding directly in the instruction stream.
  //
  // On Moe that is not a harmless mistake: a zero halfword is opcode 0b0000
  // with Addressing mode 0 and Condition 0, an unconditional absolute JUMP,
  // which then reads whatever follows as its target address. Found by running
  // one - a `.p2align 2` mid-function in llvm-tests/paging_test.s turned into
  // `JUMP.AL` to a garbage address and took the program off into unmapped
  // memory. Zero is the single worst byte value this could have been.
  //
  // An odd Count can only happen when the region starts on an odd address,
  // since it always ends on an aligned one; instructions are halfword-aligned
  // (see 'Memory alignment'), so that leading byte cannot be part of one and
  // is left as zero.
  if (Count % 2) {
    OS.write('\0');
    --Count;
  }
  for (uint64_t i = 0; i != Count; i += 2) {
    // 0x4F00 little-endian: opcode 0b0100, Mask 0b1111, From 0, To 0.
    OS.write('\x00');
    OS.write('\x4f');
  }
  return true;
}

} // end anonymous namespace

MCAsmBackend *llvm::createMoeMCAsmBackend(const Target &T,
                                           const MCSubtargetInfo &STI,
                                           const MCRegisterInfo &MRI,
                                           const MCTargetOptions &Options) {
  return new MoeAsmBackend();
}
