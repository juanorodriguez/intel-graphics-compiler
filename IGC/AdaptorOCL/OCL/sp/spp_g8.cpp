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

#include "llvm/Config/llvm-config.h"
#include "spp_g8.h"
#include "../../../Compiler/CodeGenPublic.h"
#include "program_debug_data.h"
#include "../../../common/SystemThread.h"
#include "../../../common/Types.hpp"
#include "../../../common/shaderOverride.hpp"
#include "../../../Compiler/CISACodeGen/OpenCLKernelCodeGen.hpp"

#include <iomanip>
#include <fstream>
#include "Probe/Assertion.h"

namespace iOpenCL
{

extern RETVAL g_cInitRetValue;

ShaderHash CGen8OpenCLProgram::CLProgramCtxProvider::getProgramHash() const {
    return m_Context.hash;
}
bool CGen8OpenCLProgram::CLProgramCtxProvider::needsSystemKernel() const {
    const auto& options = m_Context.m_InternalOptions;
    return options.IncludeSIPCSR ||
        options.IncludeSIPKernelDebug ||
        options.IncludeSIPKernelDebugWithLocalMemory;
}
bool CGen8OpenCLProgram::CLProgramCtxProvider::isProgramDebuggable() const {
    return m_Context.m_InternalOptions.KernelDebugEnable;
}
bool CGen8OpenCLProgram::CLProgramCtxProvider::hasProgrammableBorderColor() const {
    return m_Context.m_DriverInfo.ProgrammableBorderColorInCompute();
}

CGen8OpenCLProgramBase::CGen8OpenCLProgramBase(PLATFORM platform,
                                               const CGen8OpenCLStateProcessor::IProgramContext& Ctx, const WA_TABLE& WATable)
    : m_Platform(platform),
      m_StateProcessor(platform, Ctx, WATable)
{
    m_ProgramScopePatchStream = new Util::BinaryStream;
}

CGen8OpenCLProgramBase::~CGen8OpenCLProgramBase()
{
    delete m_ProgramScopePatchStream;
    for (auto& data : m_KernelBinaries)
    {
        delete data.kernelBinary;
        delete data.kernelDebugData;
        delete data.dbgInfo.header;
    }

    if (m_pSystemThreadKernelOutput)
    {
        SIP::CSystemThread::DeleteSystemThreadKernel(m_pSystemThreadKernelOutput);
    }
}

RETVAL CGen8OpenCLProgramBase::GetProgramBinary(
    Util::BinaryStream& programBinary,
    unsigned pointerSizeInBytes )
{
    RETVAL retValue = g_cInitRetValue;

    iOpenCL::SProgramBinaryHeader   header;

    memset( &header, 0, sizeof( header ) );

    header.Magic = iOpenCL::MAGIC_CL;
    header.Version = iOpenCL::CURRENT_ICBE_VERSION;
    header.Device = m_Platform.eRenderCoreFamily;
    header.GPUPointerSizeInBytes = pointerSizeInBytes;
    header.NumberOfKernels = m_KernelBinaries.size();
    header.SteppingId = m_Platform.usRevId;
    header.PatchListSize = int_cast<DWORD>(m_ProgramScopePatchStream->Size());

    if (IGC_IS_FLAG_ENABLED(DumpOCLProgramInfo))
    {
        DebugProgramBinaryHeader(&header, m_StateProcessor.m_oclStateDebugMessagePrintOut);
    }

    programBinary.Write( header );

    programBinary.Write( *m_ProgramScopePatchStream );

    for( auto& data : m_KernelBinaries )
    {
        programBinary.Write( *(data.kernelBinary) );
    }

    return retValue;
}

void CGen8OpenCLProgramBase::CreateProgramScopePatchStream(const IGC::SOpenCLProgramInfo& annotations)
{
    m_StateProcessor.CreateProgramScopePatchStream(annotations, *m_ProgramScopePatchStream);
}

CGen8OpenCLProgram::CGen8OpenCLProgram(PLATFORM platform, const IGC::OpenCLProgramContext& context)
    : m_Context(context),
      m_ContextProvider(context),
      CGen8OpenCLProgramBase(platform, m_ContextProvider, context.platform.getWATable())
{
}

CGen8OpenCLProgram::~CGen8OpenCLProgram()
{
    for (auto p : m_ShaderProgramList)
    {
        delete p;
    }
    m_ShaderProgramList.clear();
}

RETVAL CGen8OpenCLProgramBase::GetProgramDebugData(Util::BinaryStream& programDebugData)
{
    // Used by VC only
    RETVAL retValue = g_cInitRetValue;

    unsigned numDebugBinaries = 0;
    for (auto data : m_KernelBinaries)
    {
        if (data.kernelDebugData && data.kernelDebugData->Size() > 0)
        {
            numDebugBinaries++;
        }
    }

    if (numDebugBinaries)
    {
        iOpenCL::SProgramDebugDataHeaderIGC header;

        memset(&header, 0, sizeof(header));

        header.Magic = iOpenCL::MAGIC_CL;
        header.Version = iOpenCL::CURRENT_ICBE_VERSION;
        header.Device = m_Platform.eRenderCoreFamily;
        header.NumberOfKernels = numDebugBinaries;
        header.SteppingId = m_Platform.usRevId;

        programDebugData.Write(header);

        for (auto data : m_KernelBinaries)
        {
            if (data.kernelDebugData && data.kernelDebugData->Size() > 0)
            {
                programDebugData.Write(*(data.kernelDebugData));
            }
        }
    }

    return retValue;
}

RETVAL CGen8OpenCLProgramBase::GetProgramDebugDataSize(size_t& totalDbgInfoBufferSize)
{
    RETVAL retValue = g_cInitRetValue;

    unsigned numDebugBinaries = 0;
    for (auto& data : m_KernelBinaries)
    {
        if (data.dbgInfo.header &&
            data.dbgInfo.header->Size() > 0 &&
            data.dbgInfo.dbgInfoBufferSize > 0)
        {
            numDebugBinaries++;
        }
    }

    totalDbgInfoBufferSize = 0;
    if (numDebugBinaries)
    {
        totalDbgInfoBufferSize += sizeof(iOpenCL::SProgramDebugDataHeaderIGC);
        for (auto& data : m_KernelBinaries)
        {
            totalDbgInfoBufferSize += (size_t)data.dbgInfo.header->Size() +
                (size_t)(data.dbgInfo.dbgInfoBufferSize +
                data.dbgInfo.extraAlignBytes);
        }
    }

    return retValue;
}

RETVAL CGen8OpenCLProgramBase::GetProgramDebugData(char* dstBuffer, size_t dstBufferSize)
{
    RETVAL retValue = g_cInitRetValue;
    size_t offset = 0;

    auto Append = [&offset, dstBuffer, dstBufferSize](void* src, size_t srcSize)
    {
        memcpy_s(dstBuffer + offset, dstBufferSize - offset, src, srcSize);
        offset += srcSize;
    };

    unsigned numDebugBinaries = 0;
    for (auto& data : m_KernelBinaries)
    {
        if (data.dbgInfo.header &&
            data.dbgInfo.header->Size() > 0 &&
            data.dbgInfo.dbgInfoBufferSize > 0)
        {
            numDebugBinaries++;
        }
    }

    if( numDebugBinaries )
    {
        iOpenCL::SProgramDebugDataHeaderIGC header;

        memset( &header, 0, sizeof( header ) );

        header.Magic = iOpenCL::MAGIC_CL;
        header.Version = iOpenCL::CURRENT_ICBE_VERSION;
        header.Device = m_Platform.eRenderCoreFamily;
        header.NumberOfKernels = numDebugBinaries;
        header.SteppingId = m_Platform.usRevId;

        Append(&header, sizeof(header));

        const uint64_t zero = 0;
        for (auto& data : m_KernelBinaries)
        {
            if (data.dbgInfo.header &&
                data.dbgInfo.header->Size() > 0 &&
                data.dbgInfo.dbgInfoBufferSize > 0)
            {
                Append((void*)data.dbgInfo.header->GetLinearPointer(), (size_t)data.dbgInfo.header->Size());
                Append(data.dbgInfo.dbgInfoBuffer, data.dbgInfo.dbgInfoBufferSize);
                IGC_ASSERT(data.dbgInfo.extraAlignBytes <= sizeof(zero));
                Append((void*)&zero, data.dbgInfo.extraAlignBytes);
            }
        }
    }

    return retValue;
}

void dumpOCLKernelBinary(
    const IGC::COpenCLKernel *Kernel,
    const KernelData &data)
{
#if LLVM_VERSION_MAJOR >= 7
    using namespace IGC;
    using namespace IGC::Debug;

    auto *Ctx = Kernel->GetContext();

    auto &kernelName = Kernel->m_kernelInfo.m_kernelName;

    auto name = DumpName(IGC::Debug::GetShaderOutputName())
        .Hash(Ctx->hash)
        .Type(ShaderType::OPENCL_SHADER)
        .PostFix(kernelName)
        .Extension("kernbin");

    auto *KernBin = data.kernelBinary;

    std::error_code EC;
    llvm::raw_fd_ostream f(name.str(), EC);
    if (!EC)
        f.write(KernBin->GetLinearPointer(), (size_t)KernBin->Size());
#endif
}

void overrideOCLKernelBinary(
    const IGC::COpenCLKernel *Kernel,
    KernelData &data)
{
    using namespace IGC;
    using namespace IGC::Debug;

    auto *Ctx = Kernel->GetContext();

    auto &kernelName = Kernel->m_kernelInfo.m_kernelName;

    auto name = DumpName(IGC::Debug::GetShaderOutputName())
        .Hash(Ctx->hash)
        .Type(ShaderType::OPENCL_SHADER)
        .PostFix(kernelName)
        .Extension("kernbin");

    std::string Path = name.overridePath();

    std::ifstream f(Path, std::ios::binary);
    if (!f.is_open())
        return;

    appendToShaderOverrideLogFile(Path, "OVERRIDDEN: ");

    f.seekg(0, f.end);
    int newBinarySize = (int)f.tellg();
    f.seekg(0, f.beg);

    auto *&KernBin = data.kernelBinary;

    delete KernBin;
    KernBin = new Util::BinaryStream();

    std::unique_ptr<char[]> Buf(new char[newBinarySize]);
    f.read(Buf.get(), newBinarySize);

    IGC_ASSERT_MESSAGE(f.good(), "Not fully read!");

    KernBin->Write(Buf.get(), newBinarySize);
}

void dumpOCLCos(const IGC::CShader *Kernel, const std::string &stateDebugMsg) {
      auto name = IGC::Debug::GetDumpNameObj(Kernel, "cos");
      auto dump = IGC::Debug::Dump(name, IGC::Debug::DumpType::COS_TEXT);

      IGC::Debug::DumpLock();
      dump.stream() << stateDebugMsg;
      IGC::Debug::DumpUnlock();
}

void CGen8OpenCLProgram::GetZEBinary(
    llvm::raw_pwrite_stream& programBinary, unsigned pointerSizeInBytes,
    const char* spv, uint32_t spvSize)
{
    auto isValidShader = [&](IGC::COpenCLKernel* shader)->bool
    {
        return (shader && shader->ProgramOutput()->m_programSize > 0);
    };

    ZEBinaryBuilder zebuilder(m_Platform, pointerSizeInBytes == 8,
        m_Context.m_programInfo, (const uint8_t*)spv, spvSize);

    for (auto pKernel : m_ShaderProgramList)
    {
        IGC::COpenCLKernel* simd8Shader = static_cast<IGC::COpenCLKernel*>(pKernel->GetShader(SIMDMode::SIMD8));
        IGC::COpenCLKernel* simd16Shader = static_cast<IGC::COpenCLKernel*>(pKernel->GetShader(SIMDMode::SIMD16));
        IGC::COpenCLKernel* simd32Shader = static_cast<IGC::COpenCLKernel*>(pKernel->GetShader(SIMDMode::SIMD32));

        // Determine how many simd modes we have per kernel
        // FIXME: We actually expect only one simd mode per kernel. There should not be multiple SIMD mode available
        // for one kernel (runtime cannot support that). So these check can be simplified
        std::vector<IGC::COpenCLKernel*> kernelVec;
        if ((m_Context.m_DriverInfo.sendMultipleSIMDModes() || m_Context.m_enableSimdVariantCompilation)
            && (m_Context.getModuleMetaData()->csInfo.forcedSIMDSize == 0))
        {
            // For multiple SIMD modes, send SIMD modes in descending order
            if (isValidShader(simd32Shader))
                kernelVec.push_back(simd32Shader);
            if (isValidShader(simd16Shader))
                kernelVec.push_back(simd16Shader);
            if (isValidShader(simd8Shader))
                kernelVec.push_back(simd8Shader);
        }
        else
        {
            if (isValidShader(simd32Shader))
                kernelVec.push_back(simd32Shader);
            else if (isValidShader(simd16Shader))
                kernelVec.push_back(simd16Shader);
            else if (isValidShader(simd8Shader))
                kernelVec.push_back(simd8Shader);
        }

        for (auto kernel : kernelVec)
        {
            IGC::SProgramOutput* pOutput = kernel->ProgramOutput();

            zebuilder.createKernel(
                (const char*)pOutput->m_programBin,
                pOutput->m_programSize,
                kernel->m_kernelInfo,
                kernel->getGRFSize());

            // FIXME: Handle IGC_IS_FLAG_ENABLED(ShaderDumpEnable) and
            // IGC_IS_FLAG_ENABLED(ShaderOverride)

            // ... Create the debug data binary streams

            if (IGC_IS_FLAG_ENABLED(EnableElf2ZEBinary))
            {
                // Copy sections one by one from ELF file to zeBinary with relocations adjusted.
                zebuilder.addElfSections(pOutput->m_debugDataVISA, pOutput->m_debugDataVISASize);
            }
        }
    }

    zebuilder.getBinaryObject(programBinary);
}

void CGen8OpenCLProgram::CreateKernelBinaries()
{
    auto isValidShader = [&](IGC::COpenCLKernel* shader)->bool
    {
        return (shader && shader->ProgramOutput()->m_programSize > 0);
    };

    for (auto pKernel : m_ShaderProgramList)
    {
        IGC::COpenCLKernel* simd8Shader = static_cast<IGC::COpenCLKernel*>(pKernel->GetShader(SIMDMode::SIMD8));
        IGC::COpenCLKernel* simd16Shader = static_cast<IGC::COpenCLKernel*>(pKernel->GetShader(SIMDMode::SIMD16));
        IGC::COpenCLKernel* simd32Shader = static_cast<IGC::COpenCLKernel*>(pKernel->GetShader(SIMDMode::SIMD32));

        // Determine how many simd modes we have per kernel
        std::vector<IGC::COpenCLKernel*> kernelVec;
        if ((m_Context.m_DriverInfo.sendMultipleSIMDModes() || m_Context.m_enableSimdVariantCompilation)
            && (m_Context.getModuleMetaData()->csInfo.forcedSIMDSize == 0))
        {
            // For multiple SIMD modes, send SIMD modes in descending order
            if (isValidShader(simd32Shader))
                kernelVec.push_back(simd32Shader);
            if (isValidShader(simd16Shader))
                kernelVec.push_back(simd16Shader);
            if (isValidShader(simd8Shader))
                kernelVec.push_back(simd8Shader);
        }
        else
        {
            if (isValidShader(simd32Shader))
                kernelVec.push_back(simd32Shader);
            else if (isValidShader(simd16Shader))
                kernelVec.push_back(simd16Shader);
            else if (isValidShader(simd8Shader))
                kernelVec.push_back(simd8Shader);
        }

        for (auto kernel : kernelVec)
        {
            IGC::SProgramOutput* pOutput = kernel->ProgramOutput();

            // Create the kernel binary streams
            KernelData data;
            data.kernelBinary = new Util::BinaryStream();

            m_StateProcessor.CreateKernelBinary(
                (const char*)pOutput->m_programBin,
                pOutput->m_programSize,
                kernel->m_kernelInfo,
                m_Context.m_programInfo,
                m_Context.btiLayout,
                *(data.kernelBinary),
                m_pSystemThreadKernelOutput,
                pOutput->m_unpaddedProgramSize);

            if (IGC_IS_FLAG_ENABLED(EnableCosDump))
                  dumpOCLCos(kernel,
                             m_StateProcessor.m_oclStateDebugMessagePrintOut);

            if (IGC_IS_FLAG_ENABLED(ShaderDumpEnable))
                dumpOCLKernelBinary(kernel, data);

            if (IGC_IS_FLAG_ENABLED(ShaderOverride))
                overrideOCLKernelBinary(kernel, data);

            IGC_ASSERT(data.kernelBinary && data.kernelBinary->Size() > 0);

            // Create the debug data binary streams
            if (pOutput->m_debugDataVISASize > 0 || pOutput->m_debugDataGenISASize > 0)
            {
                data.dbgInfo.header = new Util::BinaryStream();

                m_StateProcessor.CreateKernelDebugData(
                    (const char*)pOutput->m_debugDataVISA,
                    pOutput->m_debugDataVISASize,
                    (const char*)pOutput->m_debugDataGenISA,
                    pOutput->m_debugDataGenISASize,
                    kernel->m_kernelInfo.m_kernelName,
                    data.dbgInfo);
            }

            m_KernelBinaries.push_back(data);
            m_StateProcessor.m_oclStateDebugMessagePrintOut.clear();
        }
    }
}

} // namespace iOpenCL
