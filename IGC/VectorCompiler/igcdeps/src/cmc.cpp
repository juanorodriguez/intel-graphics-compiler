/*========================== begin_copyright_notice ============================

Copyright (c) 2000-2021 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

============================= end_copyright_notice ===========================*/

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include "vc/igcdeps/cmc.h"
#include "RT_Jitter_Interface.h"
#include "inc/common/igfxfmid.h"
#include "AdaptorOCL/OCL/sp/spp_g8.h"
#include "common/secure_mem.h"
#include "common/secure_string.h"

#include <llvm/Support/raw_ostream.h>

#include <iterator>
#include <numeric>
#include <string>

#include "Probe/Assertion.h"

using namespace vc;
using namespace llvm;

CMKernel::CMKernel(const PLATFORM& platform)
    : m_platform(platform)
    , m_btiLayout(new USC::SShaderStageBTLayout)
{
    std::memset(m_btiLayout.getModifiableLayout(), 0, sizeof(USC::SShaderStageBTLayout));
}

CMKernel::~CMKernel()
{
    delete m_btiLayout.getModifiableLayout();
}

static zebin::PreDefinedAttrGetter::ArgType getZEArgType(iOpenCL::DATA_PARAMETER_TOKEN type) {
    switch(type) {
    case iOpenCL::DATA_PARAMETER_ENQUEUED_LOCAL_WORK_SIZE:
        return zebin::PreDefinedAttrGetter::ArgType::local_size;
    case iOpenCL::DATA_PARAMETER_GLOBAL_WORK_OFFSET:
        return zebin::PreDefinedAttrGetter::ArgType::global_id_offset;
    case iOpenCL::DATA_PARAMETER_NUM_WORK_GROUPS:
        // copied from OCL behavior
        return zebin::PreDefinedAttrGetter::ArgType::group_count;
    default:
        IGC_ASSERT_MESSAGE(0, "unsupported argument type");
        return zebin::PreDefinedAttrGetter::ArgType::arg_byvalue;
    }
}

static zebin::PreDefinedAttrGetter::ArgAccessType
getZEArgAccessType(GenXOCLRuntimeInfo::KernelArgInfo::AccessKindType accessKind)
{
    using ArgAccessKind = GenXOCLRuntimeInfo::KernelArgInfo::AccessKindType;
    switch(accessKind)
    {
    case ArgAccessKind::ReadOnly:
        return zebin::PreDefinedAttrGetter::ArgAccessType::readonly;
    case ArgAccessKind::WriteOnly:
        return zebin::PreDefinedAttrGetter::ArgAccessType::writeonly;
    case ArgAccessKind::ReadWrite:
        return zebin::PreDefinedAttrGetter::ArgAccessType::readwrite;
    case ArgAccessKind::None:
    default:
        IGC_ASSERT_MESSAGE(0, "invalid access type");
        return zebin::PreDefinedAttrGetter::ArgAccessType::readwrite;
    }
}

namespace {
class KernelArgInfoBuilder
{
    using ArgKind = GenXOCLRuntimeInfo::KernelArgInfo::KindType;
    using ArgAccessKind = GenXOCLRuntimeInfo::KernelArgInfo::AccessKindType;

    struct AccessQualifiers
    {
        static constexpr const char* None = "NONE";
        static constexpr const char* ReadOnly = "read_only";
        static constexpr const char* WriteOnly = "write_only";
        static constexpr const char* ReadWrite = "read_write";

        static const char* get(ArgAccessKind AccessKindID)
        {
            switch (AccessKindID)
            {
                case ArgAccessKind::None:
                    return None;
                case ArgAccessKind::ReadOnly:
                    return ReadOnly;
                case ArgAccessKind::WriteOnly:
                    return WriteOnly;
                case ArgAccessKind::ReadWrite:
                default:
                    return ReadWrite;
            }
        }
    };
    struct AddressQualifiers
    {
        static constexpr const char* Global = "__global";
        static constexpr const char* Local = "__local";
        static constexpr const char* Private = "__private";
        static constexpr const char* Constant = "__constant";
        static constexpr const char* NotSpecified = "not_specified";

        static const char* get(ArgKind ArgKindID)
        {
            switch (ArgKindID)
            {
                case ArgKind::General:
                    return Private;
                case ArgKind::Buffer:
                case ArgKind::SVM:
                case ArgKind::Image1D:
                case ArgKind::Image2D:
                case ArgKind::Image3D:
                    return Global;
                case ArgKind::Sampler:
                    return Constant;
                default:
                    IGC_ASSERT_EXIT_MESSAGE(0, "implicit args cannot appear in kernel arg info");
            }
            return NotSpecified;
        }
    };
    struct TypeQualifiers
    {
        static constexpr const char* None = "NONE";
        static constexpr const char* Const = "const";
        static constexpr const char* Volatile = "volatile";
        static constexpr const char* Restrict = "restrict";
        static constexpr const char* Pipe = "pipe";
    };
    using KernArgAnnotation = iOpenCL::KernelArgumentInfoAnnotation;
    using ArgInfoSeq = std::vector<std::unique_ptr<KernArgAnnotation>>;
    ArgInfoSeq ArgInfos;

public:
    void insert(int Index, ArgKind ArgKindID, ArgAccessKind AccessKindID)
    {
        resizeStorageIfRequired(Index + 1);
        ArgInfos[Index] = get(ArgKindID, AccessKindID);
    }

    // It is users responsibility to delete the annotation.
    static std::unique_ptr<KernArgAnnotation> get(ArgKind ArgKindID,
            ArgAccessKind AccessKind = ArgAccessKind::None)
    {
        auto Annotation = std::make_unique<KernArgAnnotation>();
        Annotation->AddressQualifier = AddressQualifiers::get(ArgKindID);
        Annotation->AccessQualifier = AccessQualifiers::get(AccessKind);
        Annotation->ArgumentName = "";
        Annotation->TypeName = "";
        Annotation->TypeQualifier = TypeQualifiers::None;
        return Annotation;
    }

    ArgInfoSeq emit() &&
    {
        IGC_ASSERT_MESSAGE(checkArgInfosCorrectness(),
                           "arg info token is incorrect");
        return std::move(ArgInfos);
    }

private:
    void resizeStorageIfRequired(int RequiredSize)
    {
        IGC_ASSERT_MESSAGE(RequiredSize > 0, "invalid required size");
        if (RequiredSize <= static_cast<int>(ArgInfos.size()))
            return;

        ArgInfos.reserve(RequiredSize);
        for (int i = ArgInfos.size(); i < RequiredSize; ++i) {
          ArgInfos.emplace_back(std::unique_ptr<KernArgAnnotation>());
        }
    }

    // Returns whether arg infos are correct.
    bool checkArgInfosCorrectness() const
    {
        return std::none_of(ArgInfos.begin(), ArgInfos.end(),
            [](const auto &ArgInfo){ return ArgInfo.get() == nullptr; });
    }
};
} // anonymous namespace

void CMKernel::createConstArgumentAnnotation(unsigned argNo,
                                             unsigned sizeInBytes,
                                             unsigned payloadPosition,
                                             unsigned offsetInArg) {
    auto constInput = std::make_unique<iOpenCL::ConstantArgumentAnnotation>();

    constInput->Offset = offsetInArg;
    constInput->PayloadPosition = payloadPosition;
    constInput->PayloadSizeInBytes = sizeInBytes;
    constInput->ArgumentNumber = argNo;
    constInput->LocationIndex = 0;
    constInput->LocationCount = 0;
    constInput->IsEmulationArgument = false;

    m_kernelInfo.m_constantArgumentAnnotation.push_back(std::move(constInput));

    // EnableZEBinary: ZEBinary related code
    zebin::ZEInfoBuilder::addPayloadArgumentByValue(m_kernelInfo.m_zePayloadArgs,
        payloadPosition, sizeInBytes, argNo);
}

// TODO: this is incomplete.
void CMKernel::createSamplerAnnotation(unsigned argNo)
{
    iOpenCL::SAMPLER_OBJECT_TYPE samplerType;
    samplerType = iOpenCL::SAMPLER_OBJECT_TEXTURE;

    auto samplerArg = std::make_unique<iOpenCL::SamplerArgumentAnnotation>();
    samplerArg->SamplerType = samplerType;
    samplerArg->ArgumentNumber = argNo;
    samplerArg->SamplerTableIndex = 0;
    samplerArg->LocationIndex = 0;
    samplerArg->LocationCount = 0;
    samplerArg->IsBindlessAccess = false;
    samplerArg->IsEmulationArgument = false;
    samplerArg->PayloadPosition = 0;

    m_kernelInfo.m_samplerArgument.push_back(std::move(samplerArg));

    if (IGC_IS_FLAG_ENABLED(EnableZEBinary))
        IGC_ASSERT_MESSAGE(0, "not yet supported for L0 binary");
}

void CMKernel::createImageAnnotation(unsigned argNo, unsigned BTI,
                                     unsigned dim, ArgAccessKind Access)
{
    auto imageInput = std::make_unique<iOpenCL::ImageArgumentAnnotation>();
    // As VC uses only statefull addrmode.
    constexpr int PayloadPosition = 0;
    constexpr int ArgSize = 0;

    imageInput->ArgumentNumber = argNo;
    imageInput->IsFixedBindingTableIndex = true;
    imageInput->BindingTableIndex = BTI;
    if (dim == 1)
        imageInput->ImageType = iOpenCL::IMAGE_MEMORY_OBJECT_1D;
    else if (dim == 2)
        imageInput->ImageType = iOpenCL::IMAGE_MEMORY_OBJECT_2D_MEDIA_BLOCK;
    else if (dim == 3)
        imageInput->ImageType = iOpenCL::IMAGE_MEMORY_OBJECT_3D;
    else
        IGC_ASSERT_MESSAGE(0, "unsupported image dimension");
    imageInput->LocationIndex = 0;
    imageInput->LocationCount = 0;
    imageInput->IsEmulationArgument = false;
    imageInput->AccessedByFloatCoords = false;
    imageInput->AccessedByIntCoords = false;
    imageInput->IsBindlessAccess = false;
    imageInput->PayloadPosition = PayloadPosition;
    imageInput->Writeable = Access != ArgAccessKind::ReadOnly;
    m_kernelInfo.m_imageInputAnnotations.push_back(std::move(imageInput));

    zebin::ZEInfoBuilder::addPayloadArgumentByPointer(m_kernelInfo.m_zePayloadArgs,
        PayloadPosition, ArgSize, argNo, zebin::PreDefinedAttrGetter::ArgAddrMode::stateful,
        zebin::PreDefinedAttrGetter::ArgAddrSpace::image,
        getZEArgAccessType(Access));
    zebin::ZEInfoBuilder::addBindingTableIndex(m_kernelInfo.m_zeBTIArgs, BTI,
                                               argNo);
}

void CMKernel::createImplicitArgumentsAnnotation(unsigned payloadPosition)
{
    createSizeAnnotation(payloadPosition, iOpenCL::DATA_PARAMETER_GLOBAL_WORK_OFFSET);
    payloadPosition += 3 * iOpenCL::DATA_PARAMETER_DATA_SIZE;
    createSizeAnnotation(payloadPosition, iOpenCL::DATA_PARAMETER_LOCAL_WORK_SIZE);
}

void CMKernel::createPointerGlobalAnnotation(unsigned index, unsigned offset,
                                             unsigned sizeInBytes, unsigned BTI,
                                             ArgAccessKind access)
{
    auto ptrAnnotation = std::make_unique<iOpenCL::PointerArgumentAnnotation>();
    ptrAnnotation->IsStateless = true;
    ptrAnnotation->IsBindlessAccess = false;
    ptrAnnotation->AddressSpace = iOpenCL::KERNEL_ARGUMENT_ADDRESS_SPACE_GLOBAL;
    ptrAnnotation->ArgumentNumber = index;
    ptrAnnotation->BindingTableIndex = BTI;
    ptrAnnotation->PayloadPosition = offset;
    ptrAnnotation->PayloadSizeInBytes = sizeInBytes;
    ptrAnnotation->LocationIndex = 0;
    ptrAnnotation->LocationCount = 0;
    ptrAnnotation->IsEmulationArgument = false;
    m_kernelInfo.m_pointerArgument.push_back(std::move(ptrAnnotation));

    // EnableZEBinary: ZEBinary related code
    zebin::ZEInfoBuilder::addPayloadArgumentByPointer(
        m_kernelInfo.m_zePayloadArgs, offset, sizeInBytes, index,
        zebin::PreDefinedAttrGetter::ArgAddrMode::stateless,
        zebin::PreDefinedAttrGetter::ArgAddrSpace::global,
        getZEArgAccessType(access));
    zebin::ZEInfoBuilder::addBindingTableIndex(m_kernelInfo.m_zeBTIArgs, BTI,
                                               index);
}

void CMKernel::createPrivateBaseAnnotation(
    unsigned argNo, unsigned byteSize, unsigned payloadPosition, int BTI,
    unsigned statelessPrivateMemSize) {
  auto ptrAnnotation = std::make_unique<iOpenCL::PrivateInputAnnotation>();

  ptrAnnotation->AddressSpace = iOpenCL::KERNEL_ARGUMENT_ADDRESS_SPACE_PRIVATE;
  ptrAnnotation->ArgumentNumber = argNo;
  // PerThreadPrivateMemorySize is defined as "Total private memory requirements for each OpenCL work-item."
  ptrAnnotation->PerThreadPrivateMemorySize = statelessPrivateMemSize;
  ptrAnnotation->BindingTableIndex = BTI;
  ptrAnnotation->IsStateless = true;
  ptrAnnotation->PayloadPosition = payloadPosition;
  ptrAnnotation->PayloadSizeInBytes = byteSize;
  m_kernelInfo.m_pointerInput.push_back(std::move(ptrAnnotation));

    // EnableZEBinary: ZEBinary related code
    zebin::ZEInfoBuilder::addPayloadArgumentImplicit(m_kernelInfo.m_zePayloadArgs,
        zebin::PreDefinedAttrGetter::ArgType::private_base_stateless,
        payloadPosition, byteSize);
}

void CMKernel::createBufferStatefulAnnotation(unsigned argNo,
                                              ArgAccessKind accessKind)
{
    auto constInput = std::make_unique<iOpenCL::ConstantInputAnnotation>();

    constInput->ConstantType = iOpenCL::DATA_PARAMETER_BUFFER_STATEFUL;
    constInput->Offset = 0;
    constInput->PayloadPosition = 0;
    constInput->PayloadSizeInBytes = 0;
    constInput->ArgumentNumber = argNo;
    constInput->LocationIndex = 0;
    constInput->LocationCount = 0;
    m_kernelInfo.m_constantInputAnnotation.push_back(std::move(constInput));

    // EnableZEBinary: ZEBinary related code
    zebin::ZEInfoBuilder::addPayloadArgumentByPointer(m_kernelInfo.m_zePayloadArgs,
        0, 0, argNo,
        zebin::PreDefinedAttrGetter::ArgAddrMode::stateful,
        zebin::PreDefinedAttrGetter::ArgAddrSpace::global,
        getZEArgAccessType(accessKind));
}

void CMKernel::createSizeAnnotation(unsigned initPayloadPosition,
                                    iOpenCL::DATA_PARAMETER_TOKEN Type)
{
    for (int i = 0, payloadPosition = initPayloadPosition;
         i < 3;
         ++i, payloadPosition += iOpenCL::DATA_PARAMETER_DATA_SIZE)
    {
        auto constInput = std::make_unique<iOpenCL::ConstantInputAnnotation>();
        DWORD sizeInBytes = iOpenCL::DATA_PARAMETER_DATA_SIZE;

        constInput->ConstantType = Type;
        constInput->Offset = i * sizeInBytes;
        constInput->PayloadPosition = payloadPosition;
        constInput->PayloadSizeInBytes = sizeInBytes;
        constInput->ArgumentNumber = 0;
        constInput->LocationIndex = 0;
        constInput->LocationCount = 0;
        m_kernelInfo.m_constantInputAnnotation.push_back(std::move(constInput));
    }
    // EnableZEBinary: ZEBinary related code
    zebin::ZEInfoBuilder::addPayloadArgumentImplicit(m_kernelInfo.m_zePayloadArgs,
        getZEArgType(Type), initPayloadPosition,
        iOpenCL::DATA_PARAMETER_DATA_SIZE * 3);
}

// TODO: refactor this function with the OCL part.
void CMKernel::RecomputeBTLayout(int numUAVs, int numResources)
{
    USC::SShaderStageBTLayout* layout = m_btiLayout.getModifiableLayout();

    // The BT layout contains the minimum and the maximum number BTI for each kind
    // of resource. E.g. UAVs may be mapped to BTIs 0..3, SRVs to 4..5, and the scratch
    // surface to 6.
    // Note that the names are somewhat misleading. They are used for the sake of consistency
    // with the ICBE sources.

    // Some fields are always 0 for OCL.
    layout->resourceNullBoundOffset = 0;
    layout->immediateConstantBufferOffset = 0;
    layout->interfaceConstantBufferOffset = 0;
    layout->constantBufferNullBoundOffset = 0;
    layout->JournalIdx = 0;
    layout->JournalCounterIdx = 0;

    // And TGSM (aka SLM) is always 254.
    layout->TGSMIdx = 254;

    int index = 0;

    // Now, allocate BTIs for all the SRVs.
    layout->minResourceIdx = index;
    if (numResources) {
        index += numResources - 1;
        layout->maxResourceIdx = index++;
    } else {
        layout->maxResourceIdx = index;
    }

    // Now, ConstantBuffers - used as a placeholder for the inline constants, if present.
    layout->minConstantBufferIdx = index;
    layout->maxConstantBufferIdx = index;

    // Now, the UAVs
    layout->minUAVIdx = index;
    if (numUAVs) {
        index += numUAVs - 1;
        layout->maxUAVIdx = index++;
    } else {
        layout->maxUAVIdx = index;
    }

    // And finally, the scratch surface
    layout->surfaceScratchIdx = index++;

    // Overall number of used BT entries, not including TGSM.
    layout->maxBTsize = index;
}

static void setSymbolsInfo(const GenXOCLRuntimeInfo::KernelInfo &Info,
                           IGC::SProgramOutput &KernelProgram) {
  if (Info.getRelocationTable().Size > 0) {
    KernelProgram.m_funcRelocationTable = Info.getRelocationTable().Buffer;
    KernelProgram.m_funcRelocationTableSize = Info.getRelocationTable().Size;
    KernelProgram.m_funcRelocationTableEntries =
        Info.getRelocationTable().Entries;
    // EnableZEBinary: ZEBinary related code
    KernelProgram.m_relocs = Info.ZEBinInfo.Relocations;
  }
  if (Info.getSymbolTable().Size > 0) {
    KernelProgram.m_funcSymbolTable = Info.getSymbolTable().Buffer;
    KernelProgram.m_funcSymbolTableSize = Info.getSymbolTable().Size;
    KernelProgram.m_funcSymbolTableEntries = Info.getSymbolTable().Entries;
    // EnableZEBinary: ZEBinary related code
    KernelProgram.m_symbols.function = Info.ZEBinInfo.Symbols.Functions;
    KernelProgram.m_symbols.global = Info.ZEBinInfo.Symbols.Globals;
    KernelProgram.m_symbols.globalConst = Info.ZEBinInfo.Symbols.Constants;
  }
  // EnableZEBinary: ZEBinary related code
  KernelProgram.m_symbols.local = Info.ZEBinInfo.Symbols.Local;
}

static void generateKernelArgInfo(
    const GenXOCLRuntimeInfo::KernelInfo &KInfo,
    std::vector<std::unique_ptr<iOpenCL::KernelArgumentInfoAnnotation>> &ArgsAnnotation) {
  KernelArgInfoBuilder ArgsAnnotationBuilder;
  for (auto &Arg : KInfo.args()) {
    using KindType = GenXOCLRuntimeInfo::KernelArgInfo::KindType;
    switch (Arg.getKind()) {
    case KindType::General:
    case KindType::Buffer:
    case KindType::SVM:
    case KindType::Sampler:
    case KindType::Image1D:
    case KindType::Image2D:
    case KindType::Image3D:
      ArgsAnnotationBuilder.insert(Arg.getIndex(), Arg.getKind(),
                                   Arg.getAccessKind());
      break;
    default:
      continue;
    }
  }
  ArgsAnnotation = std::move(ArgsAnnotationBuilder).emit();
}

static void setArgumentsInfo(const GenXOCLRuntimeInfo::KernelInfo &Info,
                             CMKernel &Kernel) {
  llvm::transform(
      llvm::enumerate(Info.getPrintStrings()),
      std::back_inserter(Kernel.m_kernelInfo.m_printfStringAnnotations),
      [](const auto EnumStr) {
        auto StringAnnotation = std::make_unique<iOpenCL::PrintfStringAnnotation>();
        StringAnnotation->Index = EnumStr.index();
        const std::string &PrintString = EnumStr.value();
        const unsigned StringSize = PrintString.size();
        StringAnnotation->StringSize = StringSize;
        // Though string size is present in annotation, string
        // should be null terminated: patchtokens processor
        // ignores size field for some reason.
        StringAnnotation->StringData = new char[StringSize + 1];
        std::copy_n(PrintString.c_str(), StringSize + 1,
                    StringAnnotation->StringData);
        return StringAnnotation;
      });

  // This is the starting constant thread payload
  // r0-r1 are reserved for SIMD1 dispatch
  const unsigned ConstantPayloadStart = Info.getGRFSizeInBytes() * 2;

  // Setup argument to BTI mapping.
  Kernel.m_kernelInfo.m_argIndexMap.clear();

  for (const GenXOCLRuntimeInfo::KernelArgInfo &Arg : Info.args()) {
    IGC_ASSERT_MESSAGE(Arg.getOffset() >= ConstantPayloadStart,
                       "Argument overlaps with thread payload");
    const unsigned ArgOffset = Arg.getOffset() - ConstantPayloadStart;

    using ArgKind = GenXOCLRuntimeInfo::KernelArgInfo::KindType;
    switch (Arg.getKind()) {
    default:
      break;
    case ArgKind::General:
      Kernel.createConstArgumentAnnotation(Arg.getIndex(), Arg.getSizeInBytes(),
                                           ArgOffset, Arg.getOffsetInArg());
      break;
    case ArgKind::LocalSize:
      Kernel.createSizeAnnotation(
          ArgOffset, iOpenCL::DATA_PARAMETER_ENQUEUED_LOCAL_WORK_SIZE);
      break;
    case ArgKind::GroupCount:
      Kernel.createSizeAnnotation(ArgOffset,
                                  iOpenCL::DATA_PARAMETER_NUM_WORK_GROUPS);
      break;
    case ArgKind::Buffer:
      Kernel.createPointerGlobalAnnotation(Arg.getIndex(), ArgOffset,
                                           Arg.getSizeInBytes(), Arg.getBTI(),
                                           Arg.getAccessKind());
      Kernel.createBufferStatefulAnnotation(Arg.getIndex(),
                                            Arg.getAccessKind());
      Kernel.m_kernelInfo.m_argIndexMap[Arg.getIndex()] = Arg.getBTI();
      break;
    case ArgKind::SVM:
      Kernel.createPointerGlobalAnnotation(Arg.getIndex(), ArgOffset,
                                           Arg.getSizeInBytes(), Arg.getBTI(),
                                           Arg.getAccessKind());
      Kernel.m_kernelInfo.m_argIndexMap[Arg.getIndex()] = Arg.getBTI();
      break;
    case ArgKind::Sampler:
      Kernel.createSamplerAnnotation(Arg.getIndex());
      Kernel.m_kernelInfo.m_argIndexMap[Arg.getIndex()] = Arg.getBTI();
      break;
    case ArgKind::Image1D:
      Kernel.createImageAnnotation(Arg.getIndex(), Arg.getBTI(), /*dim=*/1,
                                   Arg.getAccessKind());
      Kernel.m_kernelInfo.m_argIndexMap[Arg.getIndex()] = Arg.getBTI();
      break;
    case ArgKind::Image2D:
      Kernel.createImageAnnotation(Arg.getIndex(), Arg.getBTI(), /*dim=*/2,
                                   Arg.getAccessKind());
      Kernel.m_kernelInfo.m_argIndexMap[Arg.getIndex()] = Arg.getBTI();
      break;
    case ArgKind::Image3D:
      Kernel.createImageAnnotation(Arg.getIndex(), Arg.getBTI(), /*dim=*/3,
                                   Arg.getAccessKind());
      Kernel.m_kernelInfo.m_argIndexMap[Arg.getIndex()] = Arg.getBTI();
      break;
    case ArgKind::PrintBuffer:
      Kernel.m_kernelInfo.m_printfBufferAnnotation =
          std::make_unique<iOpenCL::PrintfBufferAnnotation>();
      Kernel.m_kernelInfo.m_argIndexMap[Arg.getIndex()] = Arg.getBTI();
      Kernel.m_kernelInfo.m_printfBufferAnnotation->ArgumentNumber =
          Arg.getIndex();
      Kernel.m_kernelInfo.m_printfBufferAnnotation->PayloadPosition = ArgOffset;
      Kernel.m_kernelInfo.m_printfBufferAnnotation->Index = 0;
      Kernel.m_kernelInfo.m_printfBufferAnnotation->DataSize = 8;
      break;
    case ArgKind::PrivateBase:
      if (Info.getStatelessPrivMemSize() != 0) {
        Kernel.createPrivateBaseAnnotation(Arg.getIndex(), Arg.getSizeInBytes(),
                                           ArgOffset, Arg.getBTI(),
                                           Info.getStatelessPrivMemSize());
        Kernel.m_kernelInfo.m_executionEnivronment
            .PerThreadPrivateOnStatelessSize = Info.getStatelessPrivMemSize();
        Kernel.m_kernelInfo.m_argIndexMap[Arg.getIndex()] = Arg.getBTI();
      }
      break;
    case ArgKind::ByValSVM:
      // Do nothing because it has already been linearized and implicit args
      // will be set instead of it.
      break;
    }
  }
  generateKernelArgInfo(Info, Kernel.m_kernelInfo.m_kernelArgInfo);

  const unsigned MaxArgEnd = std::accumulate(
      Info.arg_begin(), Info.arg_end(), ConstantPayloadStart,
      [](unsigned MaxArgEnd, const GenXOCLRuntimeInfo::KernelArgInfo &Arg) {
        return std::max(MaxArgEnd, Arg.getOffset() + Arg.getSizeInBytes());
      });
  const unsigned ConstantBufferLengthInGRF =
      iSTD::Align(MaxArgEnd - ConstantPayloadStart, Info.getGRFSizeInBytes()) /
      Info.getGRFSizeInBytes();
  Kernel.m_kernelInfo.m_kernelProgram.ConstantBufferLength =
      ConstantBufferLengthInGRF;
}

static void setExecutionInfo(const GenXOCLRuntimeInfo::KernelInfo &BackendInfo,
                             const FINALIZER_INFO &JitterInfo,
                             CMKernel &Kernel) {
  Kernel.m_GRFSizeInBytes = BackendInfo.getGRFSizeInBytes();
  Kernel.m_kernelInfo.m_kernelName = BackendInfo.getName();

  iOpenCL::ExecutionEnivronment &ExecEnv =
      Kernel.m_kernelInfo.m_executionEnivronment;
  ExecEnv.CompiledSIMDSize = BackendInfo.getSIMDSize();
  // SLM size in bytes, align to 1KB.
  ExecEnv.SumFixedTGSMSizes = iSTD::Align(BackendInfo.getSLMSize(), 1024);
  ExecEnv.HasBarriers = BackendInfo.usesBarriers();
  ExecEnv.HasReadWriteImages = BackendInfo.usesReadWriteImages();
  ExecEnv.SubgroupIndependentForwardProgressRequired = true;
  ExecEnv.NumGRFRequired = JitterInfo.numGRFTotal;

  // Allocate spill-fill buffer
  if (JitterInfo.isSpill || JitterInfo.hasStackcalls)
    ExecEnv.PerThreadScratchSpace += JitterInfo.spillMemUsed;
  if (!JitterInfo.hasStackcalls && BackendInfo.getTPMSize() != 0)
    // CM stack calls and thread-private memory use the same value to control
    // scratch space. Consequently, if we have stack calls, there is no need
    // to add this value for thread-private memory. It should be fixed if
    // these features begin to calculate the required space separately.
    ExecEnv.PerThreadScratchSpace += BackendInfo.getTPMSize();

  // ThreadPayload.
  {
    iOpenCL::ThreadPayload &Payload = Kernel.m_kernelInfo.m_threadPayload;
    // Local IDs are always present now.
    Payload.HasLocalIDx = BackendInfo.usesLocalIdX();
    Payload.HasLocalIDy = BackendInfo.usesLocalIdY();
    Payload.HasLocalIDz = BackendInfo.usesLocalIdZ();
    Payload.HasGroupID = BackendInfo.usesGroupId();
    Payload.HasLocalID =
        Payload.HasLocalIDx || Payload.HasLocalIDy || Payload.HasLocalIDz;
    Payload.CompiledForIndirectPayloadStorage = true;
    Payload.OffsetToSkipPerThreadDataLoad =
        JitterInfo.offsetToSkipPerThreadDataLoad;
  }

  int NumUAVs = 0;
  int NumResources = 0;
  // cmc does not do stateless-to-stateful optimization, therefore
  // set >4GB to true by default, to false if we see any resource-type.
  ExecEnv.CompiledForGreaterThan4GBBuffers = true;
  for (const GenXOCLRuntimeInfo::KernelArgInfo &Arg : BackendInfo.args()) {
    if (Arg.isResource()) {
      if (!Arg.isImage() || Arg.isWritable())
        NumUAVs++;
      else
        NumResources++;
      ExecEnv.CompiledForGreaterThan4GBBuffers = false;
    }
  }
  // Update BTI layout.
  Kernel.RecomputeBTLayout(NumUAVs, NumResources);
}

static void setGenBinary(const FINALIZER_INFO &JitterInfo,
                         const std::vector<char> &GenBinary, CMKernel &Kernel) {
  // Kernel binary, padding is hard-coded.
  const size_t Size = GenBinary.size();
  const size_t Padding = iSTD::GetAlignmentOffset(Size, 64);
  void *KernelBin = IGC::aligned_malloc(Size + Padding, 16);
  memcpy_s(KernelBin, Size + Padding, GenBinary.data(), Size);
  // Pad out the rest with 0s.
  std::memset(static_cast<char *>(KernelBin) + Size, 0, Padding);

  // Update program info.
  IGC::SProgramOutput &PO = Kernel.getProgramOutput();
  PO.m_programBin = KernelBin;
  PO.m_programSize = Size + Padding;
  PO.m_unpaddedProgramSize = Size;
  PO.m_InstructionCount = JitterInfo.numAsmCount;
}

static void setDebugInfo(const std::vector<char> &DebugInfo, CMKernel &Kernel) {
  if (DebugInfo.empty())
    return;

  const size_t DebugInfoSize = DebugInfo.size();
  void *DebugInfoBuf = IGC::aligned_malloc(DebugInfoSize, sizeof(void *));
  memcpy_s(DebugInfoBuf, DebugInfoSize, DebugInfo.data(), DebugInfoSize);
  Kernel.getProgramOutput().m_debugDataVISA = DebugInfoBuf;
  Kernel.getProgramOutput().m_debugDataVISASize = DebugInfoSize;
}

static void setGtpinInfo(const FINALIZER_INFO &JitterInfo,
                         const GenXOCLRuntimeInfo::GTPinInfo &GtpinInfo,
                         CMKernel &Kernel) {
  Kernel.getProgramOutput().m_scratchSpaceUsedByGtpin =
      JitterInfo.numBytesScratchGtpin;
  Kernel.m_kernelInfo.m_executionEnivronment.PerThreadScratchSpace +=
      JitterInfo.numBytesScratchGtpin;

  if (!GtpinInfo.getGTPinBuffer().empty()) {
    const size_t BufSize = GtpinInfo.getGTPinBuffer().size();
    void *GtpinBuffer = IGC::aligned_malloc(BufSize, 16);
    memcpy_s(GtpinBuffer, BufSize, GtpinInfo.getGTPinBuffer().data(), BufSize);
    Kernel.getProgramOutput().m_gtpinBufferSize = BufSize;
    Kernel.getProgramOutput().m_gtpinBuffer = GtpinBuffer;
  }
}

// Transform backend collected into encoder format (OCL patchtokens or L0
// structures).
static void fillKernelInfo(const GenXOCLRuntimeInfo::CompiledKernel &CompKernel,
                           CMKernel &ResKernel) {
  setExecutionInfo(CompKernel.getKernelInfo(), CompKernel.getJitterInfo(),
                   ResKernel);
  setArgumentsInfo(CompKernel.getKernelInfo(), ResKernel);
  setSymbolsInfo(CompKernel.getKernelInfo(), ResKernel.getProgramOutput());

  setGenBinary(CompKernel.getJitterInfo(), CompKernel.getGenBinary(),
               ResKernel);
  setDebugInfo(CompKernel.getDebugInfo(), ResKernel);
  setGtpinInfo(CompKernel.getJitterInfo(), CompKernel.getGTPinInfo(),
               ResKernel);
}

template <typename AnnotationT>
std::unique_ptr<AnnotationT>
getDataAnnotation(const GenXOCLRuntimeInfo::DataInfoT &DataInfo) {
  auto AllocSize = DataInfo.Buffer.size() + DataInfo.AdditionalZeroedSpace;
  IGC_ASSERT_MESSAGE(AllocSize >= 0, "illegal allocation size");
  if (AllocSize == 0)
    return nullptr;
  auto InitConstant = std::make_unique<AnnotationT>();
  InitConstant->Alignment = DataInfo.Alignment;
  InitConstant->AllocSize = AllocSize;

  auto BufferSize = DataInfo.Buffer.size();
  InitConstant->InlineData.resize(BufferSize);
  memcpy_s(InitConstant->InlineData.data(), BufferSize, DataInfo.Buffer.data(),
           BufferSize);

  return std::move(InitConstant);
}

static void
fillOCLProgramInfo(IGC::SOpenCLProgramInfo &ProgramInfo,
                   const GenXOCLRuntimeInfo::ModuleInfoT &ModuleInfo) {
  auto ConstantAnnotation = getDataAnnotation<iOpenCL::InitConstantAnnotation>(
      ModuleInfo.ConstantData);
  if (ConstantAnnotation)
    ProgramInfo.m_initConstantAnnotation.push_back(
        std::move(ConstantAnnotation));
  auto GlobalAnnotation =
      getDataAnnotation<iOpenCL::InitGlobalAnnotation>(ModuleInfo.GlobalData);
  if (GlobalAnnotation)
    ProgramInfo.m_initGlobalAnnotation.push_back(std::move(GlobalAnnotation));
};

void vc::createBinary(
    vc::CGen8CMProgram &CMProgram,
    const GenXOCLRuntimeInfo::CompiledModuleT &CompiledModule) {
  bool ProgramIsDebuggable = false;
  fillOCLProgramInfo(*CMProgram.m_programInfo, CompiledModule.ModuleInfo);
  for (const GenXOCLRuntimeInfo::CompiledKernel &CompKernel :
       CompiledModule.Kernels) {
    CMKernel *K = new CMKernel(CMProgram.getPlatform());
    CMProgram.m_kernels.push_back(K);
    fillKernelInfo(CompKernel, *K);
    ProgramIsDebuggable |= !CompKernel.getDebugInfo().empty();
  }
  CMProgram.m_ContextProvider.updateDebuggableStatus(ProgramIsDebuggable);
}
