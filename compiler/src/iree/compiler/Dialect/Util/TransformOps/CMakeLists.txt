################################################################################
# Autogenerated by build_tools/bazel_to_cmake/bazel_to_cmake.py from           #
# compiler/src/iree/compiler/Dialect/Util/TransformOps/BUILD.bazel             #
#                                                                              #
# Use iree_cmake_extra_content from iree/build_defs.oss.bzl to add arbitrary   #
# CMake-only content.                                                          #
#                                                                              #
# To disable autogeneration for this file entirely, delete this header.        #
################################################################################

iree_add_all_subdirs()

iree_tablegen_library(
  NAME
    UtilTransformOpsGen
  TD_FILE
    "UtilTransformOps.td"
  OUTS
    --gen-op-decls UtilTransformOps.h.inc
    --gen-op-defs UtilTransformOps.cpp.inc
)

iree_cc_library(
  NAME
    TransformOps
  HDRS
    "UtilTransformOps.h"
    "UtilTransformOps.h.inc"
  SRCS
    "UtilTransformOps.cpp"
    "UtilTransformOps.cpp.inc"
  DEPS
    ::UtilTransformOpsGen
    LLVMSupport
    MLIRBytecodeWriter
    MLIRIR
    MLIRPDLDialect
    MLIRParser
    MLIRSupport
    MLIRTransformDialect
    MLIRTransformDialectInterfaces
    MLIRTransformUtils
    iree::compiler::Dialect::Util::IR
    iree::compiler::Utils
  PUBLIC
)

### BAZEL_TO_CMAKE_PRESERVES_ALL_CONTENT_BELOW_THIS_LINE ###
