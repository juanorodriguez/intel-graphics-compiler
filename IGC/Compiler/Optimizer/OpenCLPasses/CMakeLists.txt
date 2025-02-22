#=========================== begin_copyright_notice ============================
#
# Copyright (c) 2010-2021 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom
# the Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
#
#============================ end_copyright_notice =============================

include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(AddressSpaceAliasAnalysis)
add_subdirectory(AggregateArguments)
add_subdirectory(AlignmentAnalysis)
add_subdirectory(Atomics)
add_subdirectory(BIFTransforms)
add_subdirectory(BreakConstantExpr)
add_subdirectory(CorrectlyRoundedDivSqrt)
add_subdirectory(DebuggerSupport)
add_subdirectory(DeviceEnqueueFuncs)
add_subdirectory(DisableLoopUnrollOnRetry)
add_subdirectory(ExtenstionFuncs)
add_subdirectory(GenericAddressResolution)
add_subdirectory(ImageFuncs)
add_subdirectory(LocalBuffers)
add_subdirectory(OpenCLPrintf)
add_subdirectory(PrivateMemory)
add_subdirectory(ProgramScopeConstants)
add_subdirectory(ReplaceUnsupportedIntrinsics)
add_subdirectory(ResourceAllocator)
add_subdirectory(SetFastMathFlags)
add_subdirectory(SubGroupFuncs)
add_subdirectory(StatelessToStatefull)
add_subdirectory(UndefinedReferences)
add_subdirectory(VolatileWorkaround)
add_subdirectory(WGFuncs)
add_subdirectory(WIFuncs)
add_subdirectory(ClampLoopUnroll)
add_subdirectory(Image3dToImage2darray)
add_subdirectory(RewriteLocalSize)
add_subdirectory(UnreachableHandling)



#
#

set(IGC_BUILD__SRC__OpenCLPasses
    "${CMAKE_CURRENT_SOURCE_DIR}/KernelArgs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/BreakdownIntrinsic.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/TransformUnmaskedFunctionsPass.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/KernelFunctionCloning.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ErrorCheckPass.cpp"
  )
set(IGC_BUILD__SRC__Optimizer_OpenCLPasses_All
    ${IGC_BUILD__SRC__OpenCLPasses}
    ${IGC_BUILD__SRC__OpenCLPasses_AddressSpaceAliasAnalysis}
    ${IGC_BUILD__SRC__OpenCLPasses_AggregateArguments}
    ${IGC_BUILD__SRC__OpenCLPasses_AlignmentAnalysis}
    ${IGC_BUILD__SRC__OpenCLPasses_Atomics}
    ${IGC_BUILD__SRC__OpenCLPasses_BIFTransforms}
    ${IGC_BUILD__SRC__OpenCLPasses_BreakConstantExpr}
    ${IGC_BUILD__SRC__OpenCLPasses_CorrectlyRoundedDivSqrt}
    ${IGC_BUILD__SRC__OpenCLPasses_DebuggerSupport}
    ${IGC_BUILD__SRC__OpenCLPasses_DeviceEnqueueFuncs}
    ${IGC_BUILD__SRC__OpenCLPasses_DisableLoopUnrollOnRetry}
    ${IGC_BUILD__SRC__OpenCLPasses_ExtenstionFuncs}
    ${IGC_BUILD__SRC__OpenCLPasses_GenericAddressResolution}
    ${IGC_BUILD__SRC__OpenCLPasses_ImageFuncs}
    ${IGC_BUILD__SRC__OpenCLPasses_LocalBuffers}
    ${IGC_BUILD__SRC__OpenCLPasses_LowerLocalMemPool}
    ${IGC_BUILD__SRC__OpenCLPasses_OpenCLPrintf}
    ${IGC_BUILD__SRC__OpenCLPasses_PrivateMemory}
    ${IGC_BUILD__SRC__OpenCLPasses_ProgramScopeConstants}
    ${IGC_BUILD__SRC__OpenCLPasses_ReplaceUnsupportedIntrinsics}
    ${IGC_BUILD__SRC__OpenCLPasses_ResourceAllocator}
    ${IGC_BUILD__SRC__OpenCLPasses_SetFastMathFlags}
    ${IGC_BUILD__SRC__OpenCLPasses_SubGroupFuncs}
    ${IGC_BUILD__SRC__OpenCLPasses_StatelessToStatefull}
    ${IGC_BUILD__SRC__OpenCLPasses_UndefinedReferences}
    ${IGC_BUILD__SRC__OpenCLPasses_VolatileWorkaround}
    ${IGC_BUILD__SRC__OpenCLPasses_WGFuncs}
    ${IGC_BUILD__SRC__OpenCLPasses_WIFuncs}
    ${IGC_BUILD__SRC__OpenCLPasses_ClampLoopUnroll}
    ${IGC_BUILD__SRC__OpenCLPasses_Image3dToImage2darray}
    ${IGC_BUILD__SRC__OpenCLPasses_RewriteLocalSize}
    ${IGC_BUILD__SRC__OpenCLPasses_UnreachableHandling}
  )


set(IGC_BUILD__SRC__Optimizer_OpenCLPasses
    ${IGC_BUILD__SRC__Optimizer_OpenCLPasses_All}
    PARENT_SCOPE
  )

set(IGC_BUILD__HDR__OpenCLPasses
    "${CMAKE_CURRENT_SOURCE_DIR}/KernelArgs.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/BreakdownIntrinsic.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/TransformUnmaskedFunctionsPass.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/KernelFunctionCloning.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ErrorCheckPass.h"
  )
set(IGC_BUILD__HDR__Optimizer_OpenCLPasses_All
    ${IGC_BUILD__HDR__OpenCLPasses}
    ${IGC_BUILD__HDR__OpenCLPasses_AddressSpaceAliasAnalysis}
    ${IGC_BUILD__HDR__OpenCLPasses_AggregateArguments}
    ${IGC_BUILD__HDR__OpenCLPasses_AlignmentAnalysis}
    ${IGC_BUILD__HDR__OpenCLPasses_Atomics}
    ${IGC_BUILD__HDR__OpenCLPasses_BIFTransforms}
    ${IGC_BUILD__HDR__OpenCLPasses_BreakConstantExpr}
    ${IGC_BUILD__HDR__OpenCLPasses_CorrectlyRoundedDivSqrt}
    ${IGC_BUILD__HDR__OpenCLPasses_DebuggerSupport}
    ${IGC_BUILD__HDR__OpenCLPasses_DeviceEnqueueFuncs}
    ${IGC_BUILD__HDR__OpenCLPasses_DisableLoopUnrollOnRetry}
    ${IGC_BUILD__HDR__OpenCLPasses_ExtenstionFuncs}
    ${IGC_BUILD__HDR__OpenCLPasses_GenericAddressResolution}
    ${IGC_BUILD__HDR__OpenCLPasses_ImageFuncs}
    ${IGC_BUILD__HDR__OpenCLPasses_LocalBuffers}
    ${IGC_BUILD__HDR__OpenCLPasses_LowerLocalMemPool}
    ${IGC_BUILD__HDR__OpenCLPasses_OpenCLPrintf}
    ${IGC_BUILD__HDR__OpenCLPasses_PrivateMemory}
    ${IGC_BUILD__HDR__OpenCLPasses_ProgramScopeConstants}
    ${IGC_BUILD__HDR__OpenCLPasses_ReplaceUnsupportedIntrinsics}
    ${IGC_BUILD__HDR__OpenCLPasses_ResourceAllocator}
    ${IGC_BUILD__HDR__OpenCLPasses_SetFastMathFlags}
    ${IGC_BUILD__HDR__OpenCLPasses_SubGroupFuncs}
    ${IGC_BUILD__HDR__OpenCLPasses_StatelessToStatefull}
    ${IGC_BUILD__HDR__OpenCLPasses_UndefinedReferences}
    ${IGC_BUILD__HDR__OpenCLPasses_VolatileWorkaround}
    ${IGC_BUILD__HDR__OpenCLPasses_WGFuncs}
    ${IGC_BUILD__HDR__OpenCLPasses_WIFuncs}
    ${IGC_BUILD__HDR__OpenCLPasses_ClampLoopUnroll}
    ${IGC_BUILD__HDR__OpenCLPasses_Image3dToImage2darray}
    ${IGC_BUILD__HDR__OpenCLPasses_RewriteLocalSize}
    ${IGC_BUILD__HDR__OpenCLPasses_UnreachableHandling}
  )


set(IGC_BUILD__HDR__Optimizer_OpenCLPasses
    ${IGC_BUILD__HDR__Optimizer_OpenCLPasses_All}
    PARENT_SCOPE
  )


set(IGC_BUILD_Compiler_OpenCLPasses_Groups
    Compiler__OpenCLPasses_AddressSpaceAliasAnalysis
    Compiler__OpenCLPasses_AggregateArguments
    Compiler__OpenCLPasses_AlignmentAnalysis
    Compiler__OpenCLPasses_Atomics
    Compiler__OpenCLPasses_BIFTransforms
    Compiler__OpenCLPasses_BreakConstantExpr
    Compiler__OpenCLPasses_CorrectlyRoundedDivSqrt
    Compiler__OpenCLPasses_DebuggerSupport
    Compiler__OpenCLPasses_DeviceEnqueueFuncs
    Compiler__OpenCLPasses_DisableLoopUnrollOnRetry
    Compiler__OpenCLPasses_ExtenstionFuncs
    Compiler__OpenCLPasses_GenericAddressResolution
    Compiler__OpenCLPasses_GenericAddressSpaceStaticResolution
    Compiler__OpenCLPasses_ImageFuncs
    Compiler__OpenCLPasses_LocalBuffers
    Compiler__OpenCLPasses_LowerLocalMemPool
    Compiler__OpenCLPasses_OpenCLPrintf
    Compiler__OpenCLPasses_PrivateMemory
    Compiler__OpenCLPasses_ProgramScopeConstants
    Compiler__OpenCLPasses_ReplaceUnsupportedIntrinsics
    Compiler__OpenCLPasses_SetFastMathFlags
    Compiler__OpenCLPasses_SubGroupFuncs
    Compiler__OpenCLPasses_StatelessToStatefull
    Compiler__OpenCLPasses_UndefinedReferences
    Compiler__OpenCLPasses_VolatileWorkaround
    Compiler__OpenCLPasses_WGFuncs
    Compiler__OpenCLPasses_WIFuncs
    Compiler__OpenCLPasses_ClampLoopUnroll
    Compiler__OpenCLPasses_Image3dToImage2darray
    Compiler__OpenCLPasses_RewriteLocalSize
    Compiler__OpenCLPasses_UnreachableHandling
  )


igc_sg_register(
    Compiler__Optimizer_OpenCLPasses
    "OpenCLPasses"
    GROUPS
      ${IGC_BUILD_Compiler_OpenCLPasses_Groups}
    FILES
      ${IGC_BUILD__SRC__OpenCLPasses}
      ${IGC_BUILD__HDR__OpenCLPasses}
      ${IGC_BUILD__SRC__OpenCLPasses_ResourceAllocator}
      ${IGC_BUILD__HDR__OpenCLPasses_ResourceAllocator}
  )
