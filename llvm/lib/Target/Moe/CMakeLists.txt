add_llvm_component_group(Moe)

set(LLVM_TARGET_DEFINITIONS Moe.td)

tablegen(LLVM MoeGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM MoeGenCallingConv.inc -gen-callingconv)
tablegen(LLVM MoeGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM MoeGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM MoeGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM MoeGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM MoeGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(MoeCommonTableGen)

add_llvm_target(MoeCodeGen
  MoeConstantPoolValue.cpp
  MoeISelDAGToDAG.cpp
  MoeISelLowering.cpp
  MoeInstrInfo.cpp
  MoeFrameLowering.cpp
  MoeMachineFunctionInfo.cpp
  MoeRegisterInfo.cpp
  MoeSubtarget.cpp
  MoeTargetMachine.cpp
  MoeAsmPrinter.cpp
  MoeMCInstLower.cpp

  LINK_COMPONENTS
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  MC
  MoeDesc
  MoeInfo
  SelectionDAG
  Support
  Target
  TargetParser

  ADD_TO_COMPONENT
  Moe
  )

add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
