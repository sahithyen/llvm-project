add_llvm_component_library(LLVMMoeAsmParser
  MoeAsmParser.cpp

  LINK_COMPONENTS
  CodeGenTypes
  MC
  MCParser
  MoeDesc
  MoeInfo
  Support

  ADD_TO_COMPONENT
  Moe
  )
