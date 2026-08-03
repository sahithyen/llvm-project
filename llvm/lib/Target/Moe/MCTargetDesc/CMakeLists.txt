add_llvm_component_library(LLVMMoeDesc
  MoeInstPrinter.cpp
  MoeMCAsmInfo.cpp
  MoeMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  MoeInfo
  Support

  ADD_TO_COMPONENT
  Moe
  )
