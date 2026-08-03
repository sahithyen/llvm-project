add_llvm_component_library(LLVMMoeDesc
  MoeAsmBackend.cpp
  MoeELFObjectWriter.cpp
  MoeInstPrinter.cpp
  MoeMCAsmInfo.cpp
  MoeMCCodeEmitter.cpp
  MoeMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  MoeInfo
  Support

  ADD_TO_COMPONENT
  Moe
  )
