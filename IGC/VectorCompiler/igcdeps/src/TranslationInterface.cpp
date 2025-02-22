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

#include "vc/igcdeps/TranslationInterface.h"

#include "vc/igcdeps/ShaderDump.h"
#include "vc/igcdeps/ShaderOverride.h"
#include "vc/igcdeps/cmc.h"

#include "vc/BiF/Wrapper.h"
#include "vc/Driver/Driver.h"
#include "vc/Support/Status.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Process.h>

#include <AdaptorOCL/OCL/BuiltinResource.h>
#include <AdaptorOCL/OCL/LoadBuffer.h>
#include <AdaptorOCL/OCL/TB/igc_tb.h>
#include <common/igc_regkeys.hpp>
#include <iStdLib/utility.h>
#include <inc/common/secure_mem.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <system_error>

#include <cstring>

namespace {
struct VcPayloadInfo {
  bool IsValid = false;
  uint64_t VcOptsOffset = 0;
  uint64_t IrSize = 0;
};

class BuildDiag {
public:
  void addWarning(const std::string &Str) {
    BuildLog << "warning: " << Str << "\n";
  }
  std::string getLog() const { return BuildLog.str(); }

private:
  // for now, we don't differentiate between warnings and errors
  // the expectation is that a marker indicating an error shall be
  // reported by other means
  std::ostringstream BuildLog;
};

} // namespace

static VcPayloadInfo tryExtractPayload(const char *Input, size_t InputSize) {
  // Payload format:
  // |-vc-codegen|c-str llvm-opts|i64(IR size)|i64(Payload size)|-vc-codegen|
  //
  // Should be in sync with:
  //  Source/IGC/AdaptorOCL/ocl_igc_interface/impl/fcl_ocl_translation_ctx_impl.cpp

  // Check for availability of "-vc-codegen" marker at the end.
  const std::string CodegenMarker = "-vc-codegen";
  // Make sure that we also have a room for 2 i64 size items.
  if (InputSize < (CodegenMarker.size() + 2 * sizeof(uint64_t)))
    return {};
  const char *const InputEnd = Input + InputSize;
  if (std::memcmp(InputEnd - CodegenMarker.size(), CodegenMarker.data(),
                  CodegenMarker.size()) != 0)
    return {};

  // Read IR and Payload sizes. We already ensured that we have the room.
  uint64_t IrSize;
  uint64_t PayloadSize;
  const char *IrSizeBuff =
      InputEnd - CodegenMarker.size() - 2 * sizeof(uint64_t);
  const char *PayloadSizeBuff =
      InputEnd - CodegenMarker.size() - 1 * sizeof(uint64_t);
  memcpy_s(&IrSize, sizeof(IrSize), IrSizeBuff, sizeof(IrSize));
  memcpy_s(&PayloadSize, sizeof(PayloadSize), PayloadSizeBuff,
           sizeof(PayloadSize));
  if (InputSize != (PayloadSize + IrSize))
    return {};

  // Search for the start of payload, it should start with "-vc-codegen" marker.
  const char *const IREnd = InputEnd - PayloadSize;
  if (std::memcmp(IREnd, CodegenMarker.data(), CodegenMarker.size()) != 0)
    return {};

  // Make sure that we have a zero-terminated c-string (vc-options are encoded
  // as such).
  auto NullPos = std::find(IREnd, InputEnd, 0);
  if (NullPos == InputEnd)
    return {};
  // Consistency check, see the Payload format.
  if ((NullPos + 1) != IrSizeBuff)
    return {};

  VcPayloadInfo Result;
  Result.VcOptsOffset = (IREnd + CodegenMarker.size()) - Input;
  Result.IrSize = IrSize;
  Result.IsValid = true;

  return Result;
}

static std::unique_ptr<llvm::MemoryBuffer>
getGenericModuleBuffer(int ResourceID) {
  std::ostringstream SS;
  SS << '#' << ResourceID;
  return std::unique_ptr<llvm::MemoryBuffer>{
      llvm::LoadBufferFromResource(SS.str().c_str(), "BC")};
}

template <enum vc::bif::RawKind Kind>
std::unique_ptr<llvm::MemoryBuffer> getVCModuleBuffer() {
  return llvm::MemoryBuffer::getMemBuffer(vc::bif::getRawData<Kind>(), "",
                                          false /* RequiresNullTerminator */);
}

static void adjustPlatform(const IGC::CPlatform &IGCPlatform,
                           vc::CompileOptions &Opts) {
  auto &PlatformInfo = IGCPlatform.getPlatformInfo();
  unsigned RevId = PlatformInfo.usRevId;
  Opts.CPUStr = cmc::getPlatformStr(PlatformInfo, /* inout */ RevId);
  Opts.RevId = RevId;
  Opts.WATable = &IGCPlatform.getWATable();
}

static void adjustFileType(TC::TB_DATA_FORMAT DataFormat,
                           vc::CompileOptions &Opts) {
  switch (DataFormat) {
  case TC::TB_DATA_FORMAT::TB_DATA_FORMAT_LLVM_TEXT:
    Opts.FType = vc::FileType::LLVM_TEXT;
    return;
  case TC::TB_DATA_FORMAT::TB_DATA_FORMAT_LLVM_BINARY:
      Opts.FType = vc::FileType::LLVM_BINARY;
      return;
  case TC::TB_DATA_FORMAT::TB_DATA_FORMAT_SPIR_V:
    Opts.FType = vc::FileType::SPIRV;
    return;
  default:
    llvm_unreachable("Data format is not supported yet");
  }
}

static void adjustOptLevel(vc::CompileOptions &Opts) {
  if (IGC_IS_FLAG_ENABLED(VCOptimizeNone))
    Opts.OptLevel = vc::OptimizerLevel::None;
}

static void adjustStackCalls(vc::CompileOptions &Opts, BuildDiag &Diag) {
  int FCtrlFlag = IGC_GET_FLAG_VALUE(FunctionControl);
  switch (FCtrlFlag) {
  default:
    Opts.FCtrl = FunctionControl::Default;
    break;
  case FLAG_FCALL_FORCE_INLINE:
    Diag.addWarning("VC does not support always inline");
    break;
  case FLAG_FCALL_FORCE_SUBROUTINE:
    Diag.addWarning("VC does not support always subroutine");
    break;
  case FLAG_FCALL_FORCE_STACKCALL:
    Opts.FCtrl = FunctionControl::StackCall;
    break;
  case FLAG_FCALL_FORCE_INDIRECTCALL:
    Diag.addWarning("VC does not support always indirect calls");
    break;
  }
}

// Overwrite binary format option for backward compatibility with
// environment variable approach.
static void adjustBinaryFormat(vc::BinaryKind &Binary) {
  if (Binary == vc::BinaryKind::OpenCL && IGC_IS_FLAG_ENABLED(EnableZEBinary))
    Binary = vc::BinaryKind::ZE;
}

static void adjustTransformationsAndOptimizations(vc::CompileOptions &Opts) {
  if (IGC_IS_FLAG_ENABLED(VCLocalizeAccUsage))
    Opts.ForceLiveRangesLocalizationForAccUsage = true;
  if (IGC_IS_FLAG_ENABLED(VCDisableNonOverlappingRegionOpt))
    Opts.ForceDisableNonOverlappingRegionOpt = true;
}

static void adjustDumpOptions(vc::CompileOptions &Opts) {
  if (IGC_IS_FLAG_ENABLED(ShaderDumpEnable)) {
    Opts.DumpIR = true;
    Opts.DumpIsa = true;
    Opts.DumpAsm = true;
    Opts.DumpDebugInfo = true;
  }
}

static void adjustOptions(const IGC::CPlatform &IGCPlatform,
                          TC::TB_DATA_FORMAT DataFormat,
                          vc::CompileOptions &Opts, BuildDiag &Diag) {
  adjustPlatform(IGCPlatform, Opts);
  adjustFileType(DataFormat, Opts);
  adjustOptLevel(Opts);
  adjustBinaryFormat(Opts.Binary);
  adjustDumpOptions(Opts);
  adjustStackCalls(Opts, Diag);

  // ZE Binary does not support debug info
  if (Opts.Binary == vc::BinaryKind::ZE && Opts.EmitDebuggableKernels) {
    Opts.EmitDebuggableKernels = false;
    Diag.addWarning("ZEBinary does not support debuggable kernels! "
                    "Emission of debuggable kernels disabled");
  }
  adjustTransformationsAndOptimizations(Opts);
}

static void setErrorMessage(const std::string &ErrorMessage,
                            TC::STB_TranslateOutputArgs &pOutputArgs) {
  pOutputArgs.pErrorString = new char[ErrorMessage.size() + 1];
  memcpy_s(pOutputArgs.pErrorString, ErrorMessage.size() + 1,
           ErrorMessage.c_str(), ErrorMessage.size() + 1);
  pOutputArgs.ErrorStringSize = ErrorMessage.size() + 1;
}

static std::error_code getError(llvm::Error Err,
                                TC::STB_TranslateOutputArgs *OutputArgs) {
  std::error_code Status;
  llvm::handleAllErrors(
      std::move(Err), [&Status, OutputArgs](const llvm::ErrorInfoBase &EI) {
        Status = EI.convertToErrorCode();
        setErrorMessage(EI.message(), *OutputArgs);
      });
  return Status;
}

static std::error_code getError(std::error_code Err,
                                TC::STB_TranslateOutputArgs *OutputArgs) {
  setErrorMessage(Err.message(), *OutputArgs);
  return Err;
}

static void outputBinary(llvm::StringRef Binary, llvm::StringRef DebugInfo,
                         const BuildDiag &Diag,
                         TC::STB_TranslateOutputArgs *OutputArgs) {
  size_t BinarySize = Binary.size();
  char *BinaryOutput = new char[BinarySize];
  memcpy_s(BinaryOutput, BinarySize, Binary.data(), BinarySize);
  OutputArgs->OutputSize = static_cast<uint32_t>(BinarySize);
  OutputArgs->pOutput = BinaryOutput;
  if (DebugInfo.size()) {
    char *DebugInfoOutput = new char[DebugInfo.size()];
    memcpy_s(DebugInfoOutput, DebugInfo.size(), DebugInfo.data(),
             DebugInfo.size());
    OutputArgs->pDebugData = DebugInfoOutput;
    OutputArgs->DebugDataSize = DebugInfo.size();
  }
  const std::string &BuildLog = Diag.getLog();
  if (!BuildLog.empty()) {
    // Currently, if warnings are reported, we expected that there was no
    // error string set.
    IGC_ASSERT(OutputArgs->pErrorString == nullptr);
#ifndef NDEBUG
    llvm::errs() << BuildLog;
#endif
    setErrorMessage(BuildLog, *OutputArgs);
  }
}

// Similar to ShaderHashOCL though reinterpretation is hidden inside
// iStdLib so probably it will be safer (to use more specialized things).
static ShaderHash getShaderHash(llvm::ArrayRef<char> Input) {
  ShaderHash Hash;
  Hash.asmHash = iSTD::HashFromBuffer(Input.data(), Input.size());
  return Hash;
}

static void dumpInputData(vc::ShaderDumper &Dumper, llvm::StringRef ApiOptions,
                          llvm::StringRef InternalOptions,
                          llvm::ArrayRef<char> Input, bool IsRaw) {
  if (!IGC_IS_FLAG_ENABLED(ShaderDumpEnable))
    return;

  Dumper.dumpText(ApiOptions, IsRaw ? "options_raw.txt" : "options.txt");
  Dumper.dumpText(InternalOptions,
                  IsRaw ? "internal_options_raw.txt" : "internal_options.txt");
  Dumper.dumpBinary(Input, IsRaw ? "igc_input_raw.spv" : "igc_input.spv");
}

static bool tryAddAuxiliaryOptions(llvm::StringRef AuxOpt,
                                   llvm::StringRef InOpt,
                                   std::string &Storage) {
  if (AuxOpt.empty())
    return false;

  Storage.clear();
  Storage.append(InOpt.data(), InOpt.size())
      .append(" ")
      .append(AuxOpt.data(), AuxOpt.size());
  return true;
}

// Parse initial options dumping all needed data.
// Return structure that describes compilation setup (CompileOptions).
// FIXME: payload decoding requires modification of input data so make
// it reference until the problem with option passing from FE to BE is
// solved in more elegant way.
static llvm::Expected<vc::CompileOptions>
parseOptions(vc::ShaderDumper &Dumper, llvm::StringRef ApiOptions,
             llvm::StringRef InternalOptions, llvm::ArrayRef<char> &Input) {
  auto RawInputDumper = llvm::make_scope_exit([=, &Dumper]() {
    dumpInputData(Dumper, ApiOptions, InternalOptions, Input, /*IsRaw=*/true);
  });

  auto NewPathPayload = tryExtractPayload(Input.data(), Input.size());
  if (NewPathPayload.IsValid) {
    ApiOptions = "-vc-codegen";
    InternalOptions = Input.data() + NewPathPayload.VcOptsOffset;
    Input = Input.take_front(static_cast<size_t>(NewPathPayload.IrSize));
  }

  std::string AuxApiOptions;
  std::string AuxInternalOptions;
  if (tryAddAuxiliaryOptions(IGC_GET_REGKEYSTRING(VCApiOptions), ApiOptions,
                             AuxApiOptions))
    ApiOptions = {AuxApiOptions.data(), AuxApiOptions.size()};
  if (tryAddAuxiliaryOptions(IGC_GET_REGKEYSTRING(VCInternalOptions),
                             InternalOptions, AuxInternalOptions))
    InternalOptions = {AuxInternalOptions.data(), AuxInternalOptions.size()};

  auto InputDumper = llvm::make_scope_exit([=, &Dumper]() {
    dumpInputData(Dumper, ApiOptions, InternalOptions, Input, /*IsRaw=*/false);
  });

  const bool IsStrictParser = IGC_GET_FLAG_VALUE(VCStrictOptionParser);
  auto ExpOptions =
      vc::ParseOptions(ApiOptions, InternalOptions, IsStrictParser);
  if (ExpOptions.errorIsA<vc::NotVCError>()) {
    RawInputDumper.release();
    InputDumper.release();
  }
  return std::move(ExpOptions);
}

static llvm::Optional<vc::ExternalData> fillExternalData() {
  vc::ExternalData ExtData;
  ExtData.OCLGenericBIFModule =
      getGenericModuleBuffer(OCL_BC);
  if (!ExtData.OCLGenericBIFModule)
    return {};
  ExtData.OCLFP64BIFModule =
      getGenericModuleBuffer(OCL_BC_FP64);
  if (!ExtData.OCLFP64BIFModule)
    return {};
  // FIXME: consider other binary kinds besides ocl.
  ExtData.VCPrintf32BIFModule =
      getVCModuleBuffer<vc::bif::RawKind::PrintfOCL32>();
  if (!ExtData.VCPrintf32BIFModule)
    return {};
  ExtData.VCPrintf64BIFModule =
      getVCModuleBuffer<vc::bif::RawKind::PrintfOCL64>();
  if (!ExtData.VCPrintf64BIFModule)
    return {};
  ExtData.VCEmulationBIFModule =
      getVCModuleBuffer<vc::bif::RawKind::Emulation>();
  if (!ExtData.VCEmulationBIFModule)
    return {};
  return std::move(ExtData);
}

static void dumpPlatform(const vc::CompileOptions &Opts, PLATFORM Platform,
                         vc::ShaderDumper &Dumper) {
#if defined(_DEBUG) || defined(_INTERNAL)
  if (!IGC_IS_FLAG_ENABLED(ShaderDumpEnable))
    return;

  std::ostringstream Os;
  auto Core = Platform.eDisplayCoreFamily;
  auto RenderCore = Platform.eRenderCoreFamily;
  auto Product = Platform.eProductFamily;
  auto RevId = Platform.usRevId;

  Os << "NEO passed: DisplayCore = " << Core << ", RenderCore = " << RenderCore
     << ", Product = " << Product << ", Revision = " << RevId << "\n";
  Os << "IGC translated into: " << Opts.CPUStr << ", " << Opts.RevId << "\n";

  Dumper.dumpText(Os.str(), "platform.be.txt");
#endif
}

std::error_code vc::translateBuild(const TC::STB_TranslateInputArgs *InputArgs,
                                   TC::STB_TranslateOutputArgs *OutputArgs,
                                   TC::TB_DATA_FORMAT InputDataFormatTemp,
                                   const IGC::CPlatform &IGCPlatform,
                                   float ProfilingTimerResolution) {
  llvm::StringRef ApiOptions{InputArgs->pOptions, InputArgs->OptionsSize};
  llvm::StringRef InternalOptions{InputArgs->pInternalOptions,
                                  InputArgs->InternalOptionsSize};

  llvm::ArrayRef<char> Input{InputArgs->pInput, InputArgs->InputSize};

  const ShaderHash Hash = getShaderHash(Input);
  std::unique_ptr<vc::ShaderDumper> Dumper;
  if (IGC_IS_FLAG_ENABLED(ShaderDumpEnable)) {
    Dumper = vc::createVC_IGCFileDumper(Hash);
  } else {
    Dumper = vc::createDefaultShaderDumper();
  }

  auto ExpOptions = parseOptions(*Dumper, ApiOptions, InternalOptions, Input);
  // If vc was not called, then observable state should not be changed.
  if (ExpOptions.errorIsA<vc::NotVCError>()) {
    llvm::consumeError(ExpOptions.takeError());
    return vc::errc::not_vc_codegen;
  }
  // Other errors are VC related and should be reported.
  if (!ExpOptions)
    return getError(ExpOptions.takeError(), OutputArgs);

  // Reset options when everything is done here.
  // This is needed to not interfere with subsequent translations.
  const auto ClOptGuard =
      llvm::make_scope_exit([]() { llvm::cl::ResetAllOptionOccurrences(); });

  BuildDiag Diag;
  vc::CompileOptions &Opts = ExpOptions.get();
  adjustOptions(IGCPlatform, InputDataFormatTemp, Opts, Diag);

  // here we have Opts set and can dump what we got from runtime and how
  // we understood it
  dumpPlatform(Opts, IGCPlatform.getPlatformInfo(), *Dumper);

  if (IGC_IS_FLAG_ENABLED(ShaderOverride))
    Opts.ShaderOverrider =
        vc::createVC_IGCShaderOverrider(Hash, IGCPlatform.getPlatformInfo());

  Opts.Dumper = std::move(Dumper);

  auto ExtData = fillExternalData();
  if (!ExtData)
    return getError(vc::make_error_code(vc::errc::bif_load_fail),
                    OutputArgs);

  llvm::ArrayRef<uint32_t> SpecConstIds{InputArgs->pSpecConstantsIds,
                                        InputArgs->SpecConstantsSize};
  llvm::ArrayRef<uint64_t> SpecConstValues{InputArgs->pSpecConstantsValues,
                                           InputArgs->SpecConstantsSize};
  auto ExpOutput = vc::Compile(Input, Opts, ExtData.getValue(), SpecConstIds,
                               SpecConstValues);
  if (!ExpOutput)
    return getError(ExpOutput.takeError(), OutputArgs);
  vc::CompileOutput &Res = ExpOutput.get();

  switch (Opts.Binary) {
  case vc::BinaryKind::CM: {
    auto &CompileResult = std::get<vc::cm::CompileOutput>(Res);
    outputBinary(CompileResult.IsaBinary, llvm::StringRef(), Diag, OutputArgs);
    break;
  }
  case vc::BinaryKind::OpenCL: {
    auto &CompileResult = std::get<vc::ocl::CompileOutput>(Res);
    vc::CGen8CMProgram CMProgram{IGCPlatform.getPlatformInfo(), IGCPlatform.getWATable()};
    vc::createBinary(CMProgram, CompileResult);
    CMProgram.CreateKernelBinaries();
    Util::BinaryStream ProgramBinary;
    CMProgram.GetProgramBinary(ProgramBinary, CompileResult.PointerSizeInBytes);
    llvm::StringRef BinaryRef{ProgramBinary.GetLinearPointer(),
                              static_cast<std::size_t>(ProgramBinary.Size())};

    Util::BinaryStream ProgramDebugData;
    CMProgram.GetProgramDebugData(ProgramDebugData);
    llvm::StringRef DebugInfoRef{
        ProgramDebugData.GetLinearPointer(),
        static_cast<std::size_t>(ProgramDebugData.Size())};

    outputBinary(BinaryRef, DebugInfoRef, Diag, OutputArgs);
    break;
  }
  case vc::BinaryKind::ZE: {
    auto &CompileResult = std::get<vc::ocl::CompileOutput>(Res);
    vc::CGen8CMProgram CMProgram{IGCPlatform.getPlatformInfo(), IGCPlatform.getWATable()};
    vc::createBinary(CMProgram, CompileResult);
    llvm::SmallVector<char, 0> ProgramBinary;
    llvm::raw_svector_ostream ProgramBinaryOS{ProgramBinary};
    CMProgram.GetZEBinary(ProgramBinaryOS, CompileResult.PointerSizeInBytes);
    llvm::StringRef BinaryRef{ProgramBinary.data(), ProgramBinary.size()};

    Util::BinaryStream ProgramDebugData;
    CMProgram.GetProgramDebugData(ProgramDebugData);
    llvm::StringRef DebugInfoRef{
        ProgramDebugData.GetLinearPointer(),
        static_cast<std::size_t>(ProgramDebugData.Size())};

    outputBinary(BinaryRef, DebugInfoRef, Diag, OutputArgs);
    break;
  }
  }

  return {};
}
