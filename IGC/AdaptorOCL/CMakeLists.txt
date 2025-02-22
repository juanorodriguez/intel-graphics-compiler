#=========================== begin_copyright_notice ============================
#
# Copyright (c) 2015-2021 Intel Corporation
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


set(IGC_BUILD__PROJ__AdaptorOCL       "${IGC_BUILD__PROJ_NAME_PREFIX}AdaptorOCL")
set(IGC_BUILD__PROJ__AdaptorOCL       "${IGC_BUILD__PROJ__AdaptorOCL}" PARENT_SCOPE) # set directly
set(IGC_BUILD__PROJ_LABEL__AdaptorOCL "AdaptorOCL")

include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

#include_directories("${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_shared/executable_format")

set(IGC_BUILD__SRC__AdaptorOCL
    "${CMAKE_CURRENT_SOURCE_DIR}/UnifyIROCL.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MoveStaticAllocas.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/CleanupInputIR.cpp"
  )

if(IGC_BUILD__SPIRV_ENABLED)
  list(APPEND IGC_BUILD__SRC__AdaptorOCL
      SPIRV/libSPIRV/OpenCL.std.h
      SPIRV/libSPIRV/OpenCL.stdfuncs.h
      SPIRV/libSPIRV/SPIRVBasicBlock.h
      SPIRV/libSPIRV/SPIRVDebug.h
      SPIRV/libSPIRV/SPIRVDecorate.h
      SPIRV/libSPIRV/SPIRVEntry.h
      SPIRV/libSPIRV/SPIRVEnum.h
      SPIRV/libSPIRV/SPIRVError.h
      SPIRV/libSPIRV/SPIRVErrorEnum.h
      SPIRV/libSPIRV/SPIRVExtInst.h
      SPIRV/libSPIRV/SPIRVFunction.h
      SPIRV/libSPIRV/SPIRVInstruction.h
      SPIRV/libSPIRV/SPIRVModule.h
      SPIRV/libSPIRV/SPIRVOpCode.h
      SPIRV/libSPIRV/SPIRVOpCodeEnum.h
      SPIRV/libSPIRV/SPIRVBuiltinEnum.h
      SPIRV/libSPIRV/SPIRVStream.h
      SPIRV/libSPIRV/SPIRVType.h
      SPIRV/libSPIRV/SPIRVUtil.h
      SPIRV/libSPIRV/SPIRVValue.h
      SPIRV/libSPIRV/spirv.hpp
      SPIRV/SPIRVconsum.h
      SPIRV/SPIRVInternal.h
      SPIRV/libSPIRV/SPIRVBasicBlock.cpp
      SPIRV/libSPIRV/SPIRVDebug.cpp
      SPIRV/libSPIRV/SPIRVDecorate.cpp
      SPIRV/libSPIRV/SPIRVEntry.cpp
      SPIRV/libSPIRV/SPIRVFunction.cpp
      SPIRV/libSPIRV/SPIRVInstruction.cpp
      SPIRV/libSPIRV/SPIRVModule.cpp
      SPIRV/libSPIRV/SPIRVStream.cpp
      SPIRV/libSPIRV/SPIRVType.cpp
      SPIRV/libSPIRV/SPIRVValue.cpp
      SPIRV/SPIRVReader.cpp
      SPIRV/SPIRVUtil.cpp
      SPIRV/libSPIRV/SPIRV.DebugInfo.h
      SPIRV/libSPIRV/SPIRV.DebugInfofuncs.h
      SPIRV/libSPIRV/SPIRVDebugInfoExt.h
    )
endif(IGC_BUILD__SPIRV_ENABLED)

  # NOTE: Exported functions must be added to final libraries/executables directly.

  set(IGC_BUILD__SRC__IGC_AdaptorOCL
      "${CMAKE_CURRENT_SOURCE_DIR}/dllInterfaceCompute.cpp"
    )

  set(IGC_BUILD__HDR__IGC_AdaptorOCL "")

  list(APPEND IGC_BUILD__SRC__IGC_AdaptorOCL
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/igc_features_and_workarounds_impl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/igc_ocl_device_ctx_impl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/igc_ocl_translation_ctx_impl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/ocl_gen_binary_impl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/ocl_translation_output_impl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/gt_system_info_impl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/platform_impl.cpp"
    ${CIF_SOURCES_EXPORT_ABSOLUTE_PATH})
  list(APPEND IGC_BUILD__HDR__IGC_AdaptorOCL
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/igc_features_and_workarounds_impl.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/igc_ocl_device_ctx_impl.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/igc_ocl_translation_ctx_impl.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/ocl_gen_binary_impl.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/ocl_translation_output_impl.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/gt_system_info_impl.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/impl/platform_impl.h"

    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/igc_features_and_workarounds.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/igc_ocl_device_ctx.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/igc_ocl_translation_ctx.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/ocl_gen_binary.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/ocl_translation_output.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/gt_system_info.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ocl_igc_interface/platform.h"
    ${CIF_HEADERS_ABSOLUTE_PATH})

  set(IGC_BUILD__SRC__IGC_AdaptorOCL ${IGC_BUILD__SRC__IGC_AdaptorOCL} PARENT_SCOPE) # set directly
  set(IGC_BUILD__HDR__IGC_AdaptorOCL ${IGC_BUILD__HDR__IGC_AdaptorOCL} PARENT_SCOPE)

# Will be set in IGC's level cmake


set(IGC_BUILD__HDR__AdaptorOCL
    "${CMAKE_CURRENT_SOURCE_DIR}/../AdaptorCommon/customApi.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../AdaptorCommon/ProcessFuncAttributes.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../AdaptorCommon/LegalizeFunctionSignatures.h"

    "${CMAKE_CURRENT_SOURCE_DIR}/OCL/KernelAnnotations.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/OCL/CommandStream/SamplerTypes.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/OCL/CommandStream/SurfaceTypes.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/DriverInfoOCL.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/UnifyIROCL.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MoveStaticAllocas.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/CleanupInputIR.hpp"

    #"${IGC_BUILD__COMMON_COMPILER_DIR}/adapters/d3d10/API/USC_d3d10.h"
    #"${IGC_BUILD__COMMON_COMPILER_DIR}/adapters/d3d10/usc_d3d10_umd.h"
    #"${IGC_BUILD__COMMON_COMPILER_DIR}/adapters/d3d9/api/usc_d3d9.h"
    "${IGC_BUILD__COMMON_COMPILER_DIR}/API/ErrorCode.h"
    "${IGC_BUILD__COMMON_COMPILER_DIR}/API/SurfaceFormats.h"
    "${IGC_BUILD__COMMON_COMPILER_DIR}/API/usc.h"
    "${IGC_BUILD__COMMON_COMPILER_DIR}/API/usc_config.h"
    "${IGC_BUILD__COMMON_COMPILER_DIR}/API/usc_debugControl.h"
    "${IGC_BUILD__COMMON_COMPILER_DIR}/API/usc_gen7.h"
    "${IGC_BUILD__COMMON_COMPILER_DIR}/API/usc_gen8.h"
    "${IGC_BUILD__COMMON_COMPILER_DIR}/API/usc_gen9.h"
  )
if(IGC_BUILD__SPIRV/ENABLED)
  list(APPEND IGC_BUILD__HDR__AdaptorOCL
        "SPIRV/SPIRVconsum.h"
      )
endif()

igc_regex_escape(_reSrcDir "${CMAKE_CURRENT_SOURCE_DIR}")

add_library("${IGC_BUILD__PROJ__AdaptorOCL}" STATIC
    ${IGC_BUILD__SRC__AdaptorOCL}
    ${IGC_BUILD__HDR__AdaptorOCL}
  )

add_dependencies("${IGC_BUILD__PROJ__AdaptorOCL}" "${IGC_BUILD__PROJ__GenISAIntrinsics}")
set_property(TARGET "${IGC_BUILD__PROJ__AdaptorOCL}" PROPERTY PROJECT_LABEL "${IGC_BUILD__PROJ_LABEL__AdaptorOCL}")

if(IGC_BUILD__VC_ENABLED)
  target_link_libraries("${IGC_BUILD__PROJ__AdaptorOCL}"
      PUBLIC
        VCCodeGen
        VCIGCDeps
    )
endif()


igc_sg_register(
    IGC__AdaptorOCL
    "DriverInterface\\OCL"
    FILES
      ${IGC_BUILD__SRC__IGC_AdaptorOCL}
      ${IGC_BUILD__HDR__IGC_AdaptorOCL}
  )
