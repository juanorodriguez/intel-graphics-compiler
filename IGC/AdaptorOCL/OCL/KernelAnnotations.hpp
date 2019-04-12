/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#ifndef _KERNEL_ANNOTATIONS_H_
#define _KERNEL_ANNOTATIONS_H_

#include <string>
#include "patch_list.h"

namespace iOpenCL
{

enum POINTER_ADDRESS_SPACE
{
    KERNEL_ARGUMENT_ADDRESS_SPACE_INVALID,

    KERNEL_ARGUMENT_ADDRESS_SPACE_GLOBAL,
    KERNEL_ARGUMENT_ADDRESS_SPACE_CONSTANT,
    KERNEL_ARGUMENT_ADDRESS_SPACE_PRIVATE,
    KERNEL_ARGUMENT_ADDRESS_SPACE_LOCAL,
    KERNEL_ARGUMENT_ADDRESS_SPACE_DEVICE_QUEUE,

    ADDRESS_SPACE_INTERNAL_DEFAULT_DEVICE_QUEUE,
    ADDRESS_SPACE_INTERNAL_EVENT_POOL,
    ADDRESS_SPACE_INTERNAL_PRINTF,

    NUM_KERNEL_ARGUMENT_ADDRESS_SPACE
};

enum TYPE_FORMAT
{
    TYPE_FORMAT_INVALID,

    TYPE_FORMAT_FLOAT,
    TYPE_FORMAT_SINT,
    TYPE_FORMAT_UINT,

    NUM_TYPE_FORMATS
};


struct KernelAnnotation
{
    DWORD AnnotationSize;
};


// Generated by the frontend
struct KernelArgumentAnnotation : KernelAnnotation
{
    DWORD ArgumentNumber;
};


// Generated by frontend - completed by backend
struct PointerArgumentAnnotation : KernelArgumentAnnotation
{
    POINTER_ADDRESS_SPACE  AddressSpace;

    bool   IsStateless;
    DWORD  BindingTableIndex;
    DWORD  PayloadPosition;
    DWORD  PayloadSizeInBytes;
    DWORD  LocationIndex;
    DWORD  LocationCount;
    bool   IsEmulationArgument;
    bool   IsBindlessAccess;

    static bool compare( const PointerArgumentAnnotation* a, const PointerArgumentAnnotation* b )
    {
        return ( a->PayloadPosition < b->PayloadPosition );
    }
};

// Should be used for __local address space pointer arguments, as it
// contains rather different fields from PointerArgumentAnnotation
struct LocalArgumentAnnotation : KernelArgumentAnnotation
{
    DWORD  Alignment;
    DWORD  PayloadPosition;
    DWORD  PayloadSizeInBytes;
};


// Generated by frontend - completed by backend
struct PointerInputAnnotation : KernelAnnotation
{
    POINTER_ADDRESS_SPACE  AddressSpace;

    bool   IsStateless;
    DWORD  BindingTableIndex;
    DWORD  PayloadPosition;
    DWORD  PayloadSizeInBytes;
    DWORD  ArgumentNumber;
};


struct PrivateInputAnnotation : PointerInputAnnotation {
    DWORD PerThreadPrivateMemorySize;
};

// Generated by frontend - completed by backend
struct ConstantArgumentAnnotation : KernelArgumentAnnotation
{
    DWORD  TypeSize;
    DWORD  Offset;

    DWORD  PayloadPosition;
    DWORD  PayloadSizeInBytes;
    DWORD  LocationIndex;
    DWORD  LocationCount;
    bool   IsEmulationArgument;
};


// Generated by frontend - completed by backend
struct ConstantInputAnnotation : KernelAnnotation
{
    // ConstantInputAnnotations, while not being argument annotations,
    // may refer to arguments.
    DWORD ArgumentNumber;

    DWORD TypeSize;
    DWORD ConstantType;
    DWORD Offset;

    DWORD PayloadPosition;
    DWORD PayloadSizeInBytes;
    DWORD LocationIndex;
    DWORD LocationCount;
};


// Generated by frontend - completed by backend
struct ImageArgumentAnnotation : KernelArgumentAnnotation
{
    IMAGE_MEMORY_OBJECT_TYPE  ImageType;
    bool   Writeable;
    bool   IsFixedBindingTableIndex;
    DWORD  BindingTableIndex;
    DWORD  LocationIndex;
    DWORD  LocationCount;
    DWORD  PayloadPosition;
    bool   AccessedByIntCoords;
    bool   AccessedByFloatCoords;
    bool   IsBindlessAccess;
    bool   IsEmulationArgument;
};

struct SamplerInputAnnotation : KernelAnnotation
{
    SAMPLER_OBJECT_TYPE  SamplerType;

    DWORD  SamplerTableIndex;

    bool                            NormalizedCoords;
    SAMPLER_MAPFILTER_TYPE          MagFilterType;
    SAMPLER_MAPFILTER_TYPE          MinFilterType;
    SAMPLER_MIPFILTER_TYPE          MipFilterType;
    SAMPLER_TEXTURE_ADDRESS_MODE    TCXAddressMode;
    SAMPLER_TEXTURE_ADDRESS_MODE    TCYAddressMode;
    SAMPLER_TEXTURE_ADDRESS_MODE    TCZAddressMode;
    SAMPLER_COMPARE_FUNC_TYPE       CompareFunc;

    float                           BorderColorR;
    float                           BorderColorG;
    float                           BorderColorB;
    float                           BorderColorA;
};

// Generated by frontend - completed by backend
struct SamplerArgumentAnnotation : KernelArgumentAnnotation
{
    // Generated by the front end
    SAMPLER_OBJECT_TYPE  SamplerType;

    // Generated by the backend
    DWORD  SamplerTableIndex;
    DWORD  LocationIndex;
    DWORD  LocationCount;
    DWORD  PayloadPosition;
    bool   IsBindlessAccess;
    bool   IsEmulationArgument;
};

// Annotation for format string of printf
struct PrintfStringAnnotation : KernelAnnotation
{
    DWORD  Index;
    DWORD  StringSize;
    char  *StringData;
};

// Annotation for printf output buffer.
struct PrintfBufferAnnotation : KernelArgumentAnnotation
{
    DWORD  Index;
    DWORD  DataSize;
    DWORD  PayloadPosition;
};

// Generated by front end
struct KernelConstantRegisterAnnotation
{
    DWORD Index;
    DWORD Channel;
};

struct InitConstantAnnotation
{
    std::vector<unsigned char> InlineData;
    int Alignment;
};

struct InitGlobalAnnotation
{
    std::vector<unsigned char> InlineData;
    int Alignment;
};

struct ConstantPointerAnnotation
{
    unsigned PointerBufferIndex;
    unsigned PointerOffset;
    unsigned PointeeAddressSpace;
    unsigned PointeeBufferIndex;
};

struct GlobalPointerAnnotation
{
    unsigned PointerBufferIndex;
    unsigned PointerOffset;
    unsigned PointeeAddressSpace;
    unsigned PointeeBufferIndex;
};

struct ThreadPayload
{
    bool  HasLocalIDx;
    bool  HasLocalIDy;
    bool  HasLocalIDz;
    bool  HasGlobalIDOffset;
    bool  HasGroupID;
    bool  HasLocalID;
    bool  HasFlattenedLocalID;
    bool  CompiledForIndirectPayloadStorage;
    bool  UnusedPerThreadConstantPresent;
    bool  HasStageInGridOrigin;
    bool  HasStageInGridSize;
    uint32_t OffsetToSkipPerThreadDataLoad;
    bool  PassInlineData;
};

struct ExecutionEnivronment
{
    DWORD  CompiledSIMDSize                           = 0;
    DWORD  CompiledSubGroupsNumber                    = 0;
    DWORD  PerThreadSpillFillSize                     = 0;
    DWORD  PerThreadScratchSpace                      = 0;
    DWORD  PerThreadScratchUseGtpin                   = 0;
    DWORD  SumFixedTGSMSizes                          = 0;
    bool   HasDeviceEnqueue                           = false;
    bool   HasBarriers                                = false;
    bool   IsSingleProgramFlow                        = false;
    DWORD  PerSIMDLanePrivateMemorySize               = 0;
    bool   HasFixedWorkGroupSize                      = false;
    bool   HasReadWriteImages                         = false;
    bool   DisableMidThreadPreemption                 = false;
    bool   IsInitializer                              = false;
    bool   IsFinalizer                                = false;
    bool   SubgroupIndependentForwardProgressRequired = false;
    bool   CompiledForGreaterThan4GBBuffers           = false;
    DWORD  FixedWorkgroupSize[3];
    DWORD  NumGRFRequired;
    DWORD  WorkgroupWalkOrder[3] = { 3, 3, 3 };
    bool   HasGlobalAtomics                           = false;
};

struct KernelTypeProgramBinaryInfo
{
    DWORD Type;
    std::string KernelName;
};

struct KernelArgumentInfoAnnotation
{
    std::string AddressQualifier;
    std::string AccessQualifier;
    std::string ArgumentName;
    std::string TypeName;
    std::string TypeQualifier;
};

struct StartGASAnnotation
{
    DWORD  Offset;
    DWORD  gpuPointerSizeInBytes;
};

struct WindowSizeGASAnnotation
{
    DWORD  Offset;
};

struct PrivateMemSizeAnnotation
{
    DWORD  Offset;
};

typedef std::vector<PointerInputAnnotation*>::const_iterator PointerInputIterator;
typedef std::vector<PointerArgumentAnnotation*>::const_iterator PointerArgumentIterator;
typedef std::vector<LocalArgumentAnnotation*>::const_iterator LocalArgumentIterator;
typedef std::vector<ConstantInputAnnotation*>::const_iterator ConstantInputIterator;
typedef std::vector<ConstantArgumentAnnotation*>::const_iterator ConstantArgumentIterator;
typedef std::vector<SamplerInputAnnotation*>::const_iterator SamplerInputIterator;
typedef std::vector<SamplerArgumentAnnotation*>::const_iterator SamplerArgumentIterator;
typedef std::vector<ImageArgumentAnnotation*>::const_iterator ImageArgumentIterator;
typedef std::vector<InitConstantAnnotation*>::const_iterator InitConstantIterator;
typedef std::vector<KernelArgumentInfoAnnotation*>::const_iterator KernelArgumentInfoIterator;
typedef std::vector<PrintfStringAnnotation*>::const_iterator PrintfStringIterator;

}

#endif
