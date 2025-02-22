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

#include <sstream>
#include "common/LLVMWarningsPush.hpp"
#include <llvm/Support/ScaledNumber.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfo.h>
#include "common/LLVMWarningsPop.hpp"
#include "Compiler/CISACodeGen/ComputeShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/CodeGenPublic.h"
#include "Probe/Assertion.h"

namespace IGC
{

    typedef struct RetryState {
        bool allowLICM;
        bool allowCodeSinking;
        bool allowSimd32Slicing;
        bool allowPromotePrivateMemory;
        bool allowPreRAScheduler;
        bool allowLargeURBWrite;
        unsigned nextState;
    } RetryState;

    static const RetryState RetryTable[] = {
        { true, true, false, true, true, true, 1 },
        { false, true, true, false, false, false, 500 }
    };

    RetryManager::RetryManager() : enabled(false)
    {
        memset(m_simdEntries, 0, sizeof(m_simdEntries));
        firstStateId = IGC_GET_FLAG_VALUE(RetryManagerFirstStateId);
        stateId = firstStateId;
        IGC_ASSERT(stateId < getStateCnt());
    }

    bool RetryManager::AdvanceState() {
        if (!enabled || IGC_IS_FLAG_ENABLED(DisableRecompilation))
        {
            return false;
        }
        IGC_ASSERT(stateId < getStateCnt());
        stateId = RetryTable[stateId].nextState;
        return (stateId < getStateCnt());
    }
    bool RetryManager::AllowLICM() {
        IGC_ASSERT(stateId < getStateCnt());
        return RetryTable[stateId].allowLICM;
    }
    bool RetryManager::AllowPromotePrivateMemory() {
        IGC_ASSERT(stateId < getStateCnt());
        return RetryTable[stateId].allowPromotePrivateMemory;
    }
    bool RetryManager::AllowPreRAScheduler() {
        IGC_ASSERT(stateId < getStateCnt());
        return RetryTable[stateId].allowPreRAScheduler;
    }
    bool RetryManager::AllowCodeSinking() {
        IGC_ASSERT(stateId < getStateCnt());
        return RetryTable[stateId].allowCodeSinking;
    }
    bool RetryManager::AllowSimd32Slicing() {
        IGC_ASSERT(stateId < getStateCnt());
        return RetryTable[stateId].allowSimd32Slicing;
    }
    bool RetryManager::AllowLargeURBWrite() {
        IGC_ASSERT(stateId < getStateCnt());
        return RetryTable[stateId].allowLargeURBWrite;
    }
    void RetryManager::SetFirstStateId(int id) {
        firstStateId = id;
    }
    bool RetryManager::IsFirstTry() {
        return (stateId == firstStateId);
    }
    bool RetryManager::IsLastTry() {
        return (!enabled ||
            IGC_IS_FLAG_ENABLED(DisableRecompilation) ||
            lastSpillSize < IGC_GET_FLAG_VALUE(AllowedSpillRegCount) ||
            (stateId < getStateCnt() && RetryTable[stateId].nextState >= getStateCnt()));
    }
    unsigned RetryManager::GetRetryId() const { return stateId; }

    void RetryManager::Enable() { enabled = true; }
    void RetryManager::Disable() { enabled = false; }

    void RetryManager::SetSpillSize(unsigned int spillSize) { lastSpillSize = spillSize; }
    unsigned int RetryManager::GetLastSpillSize() { return lastSpillSize; }

    void RetryManager::ClearSpillParams() {
        lastSpillSize = 0;
        numInstructions = 0;
    }

    // save entry for given SIMD mode, to avoid recompile for next retry.
    void RetryManager::SaveSIMDEntry(SIMDMode simdMode, CShader* shader)
    {
        switch (simdMode)
        {
        case SIMDMode::SIMD8:   m_simdEntries[0] = shader;  break;
        case SIMDMode::SIMD16:  m_simdEntries[1] = shader;  break;
        case SIMDMode::SIMD32:  m_simdEntries[2] = shader;  break;
        default:
            IGC_ASSERT(0);
            break;
        }
    }

    CShader* RetryManager::GetSIMDEntry(SIMDMode simdMode)
    {
        switch (simdMode)
        {
        case SIMDMode::SIMD8:   return m_simdEntries[0];
        case SIMDMode::SIMD16:  return m_simdEntries[1];
        case SIMDMode::SIMD32:  return m_simdEntries[2];
        default:
            IGC_ASSERT(0);
            return nullptr;
        }
    }

    RetryManager::~RetryManager()
    {
        for (unsigned i = 0; i < 3; i++)
        {
            if (m_simdEntries[i])
            {
                delete m_simdEntries[i];
            }
        }
    }

    bool RetryManager::AnyKernelSpills()
    {
        for (unsigned i = 0; i < 3; i++)
        {
            if (m_simdEntries[i] && m_simdEntries[i]->m_spillCost > 0.0)
            {
                return true;
            }
        }
        return false;
    }

    bool RetryManager::PickupKernels(CodeGenContext* cgCtx)
    {
        if (cgCtx->type == ShaderType::COMPUTE_SHADER)
        {
            return PickupCS(static_cast<ComputeShaderContext*>(cgCtx));
        }
        else
        {
            IGC_ASSERT_MESSAGE(0, "TODO for other shader types");
            return true;
        }
    }

    unsigned RetryManager::getStateCnt()
    {
        return sizeof(RetryTable) / sizeof(RetryState);
    };

    CShader* RetryManager::PickCSEntryForcedFromDriver(SIMDMode& simdMode, unsigned char forcedSIMDModeFromDriver)
    {
        if (forcedSIMDModeFromDriver == 8)
        {
            if ((m_simdEntries[0] && m_simdEntries[0]->m_spillSize == 0) || IsLastTry())
            {
                simdMode = SIMDMode::SIMD8;
                return m_simdEntries[0];
            }
        }
        else if (forcedSIMDModeFromDriver == 16)
        {
            if ((m_simdEntries[1] && m_simdEntries[1]->m_spillSize == 0) || IsLastTry())
            {
                simdMode = SIMDMode::SIMD16;
                return m_simdEntries[1];
            }
        }
        else if (forcedSIMDModeFromDriver == 32)
        {
            if ((m_simdEntries[2] && m_simdEntries[2]->m_spillSize == 0) || IsLastTry())
            {
                simdMode = SIMDMode::SIMD32;
                return m_simdEntries[2];
            }
        }
        return nullptr;
    }

    CShader* RetryManager::PickCSEntryByRegKey(SIMDMode& simdMode)
    {
        if (IGC_IS_FLAG_ENABLED(ForceCSSIMD32))
        {
            simdMode = SIMDMode::SIMD32;
            return m_simdEntries[2];
        }
        else
            if (IGC_IS_FLAG_ENABLED(ForceCSSIMD16) && m_simdEntries[1])
            {
                simdMode = SIMDMode::SIMD16;
                return m_simdEntries[1];
            }
            else
                if (IGC_IS_FLAG_ENABLED(ForceCSLeastSIMD))
                {
                    if (m_simdEntries[0])
                    {
                        simdMode = SIMDMode::SIMD8;
                        return m_simdEntries[0];
                    }
                    else
                        if (m_simdEntries[1])
                        {
                            simdMode = SIMDMode::SIMD16;
                            return m_simdEntries[1];
                        }
                        else
                        {
                            simdMode = SIMDMode::SIMD32;
                            return m_simdEntries[2];
                        }
                }

        return nullptr;
    }

    CShader* RetryManager::PickCSEntryEarly(SIMDMode& simdMode,
        ComputeShaderContext* cgCtx)
    {
        float spillThreshold = cgCtx->GetSpillThreshold();
        float occu8 = cgCtx->GetThreadOccupancy(SIMDMode::SIMD8);
        float occu16 = cgCtx->GetThreadOccupancy(SIMDMode::SIMD16);
        float occu32 = cgCtx->GetThreadOccupancy(SIMDMode::SIMD32);

        bool simd32NoSpill = m_simdEntries[2] && m_simdEntries[2]->m_spillCost <= spillThreshold;
        bool simd16NoSpill = m_simdEntries[1] && m_simdEntries[1]->m_spillCost <= spillThreshold;
        bool simd8NoSpill = m_simdEntries[0] && m_simdEntries[0]->m_spillCost <= spillThreshold;

        // If SIMD32/16/8 are all allowed, then choose one which has highest thread occupancy

        if (IGC_IS_FLAG_ENABLED(EnableHighestSIMDForNoSpill))
        {
            if (simd32NoSpill)
            {
                simdMode = SIMDMode::SIMD32;
                return m_simdEntries[2];
            }

            if (simd16NoSpill)
            {
                simdMode = SIMDMode::SIMD16;
                return m_simdEntries[1];
            }
        }
        else
        {
            if (simd32NoSpill)
            {
                if (occu32 >= occu16 && occu32 >= occu8)
                {
                    simdMode = SIMDMode::SIMD32;
                    return m_simdEntries[2];
                }
                // If SIMD32 doesn't spill, SIMD16 and SIMD8 shouldn't, if they exist
                IGC_ASSERT((m_simdEntries[0] == NULL) || simd8NoSpill == true);
                IGC_ASSERT((m_simdEntries[1] == NULL) || simd16NoSpill == true);
            }

            if (simd16NoSpill)
            {
                if (occu16 >= occu8 && occu16 >= occu32)
                {
                    simdMode = SIMDMode::SIMD16;
                    return m_simdEntries[1];
                }
                IGC_ASSERT_MESSAGE((m_simdEntries[0] == NULL) || simd8NoSpill == true, "If SIMD16 doesn't spill, SIMD8 shouldn't, if it exists");
            }
        }

        bool needToRetry = false;
        if (cgCtx->m_slmSize)
        {
            if (occu16 > occu8 || occu32 > occu16)
            {
                needToRetry = true;
            }
        }

        SIMDMode maxSimdMode = cgCtx->GetMaxSIMDMode();
        if (maxSimdMode == SIMDMode::SIMD8 || !needToRetry)
        {
            if (m_simdEntries[0] && m_simdEntries[0]->m_spillSize == 0)
            {
                simdMode = SIMDMode::SIMD8;
                return m_simdEntries[0];
            }
        }
        return nullptr;
    }

    CShader* RetryManager::PickCSEntryFinally(SIMDMode& simdMode)
    {
        if (m_simdEntries[0])
        {
            simdMode = SIMDMode::SIMD8;
            return m_simdEntries[0];
        }
        else
            if (m_simdEntries[1])
            {
                simdMode = SIMDMode::SIMD16;
                return m_simdEntries[1];
            }
            else
            {
                simdMode = SIMDMode::SIMD32;
                return m_simdEntries[2];
            }
    }

    void RetryManager::FreeAllocatedMemForNotPickedCS(SIMDMode simdMode)
    {
        if (simdMode != SIMDMode::SIMD8 && m_simdEntries[0] != nullptr)
        {
            if (m_simdEntries[0]->ProgramOutput()->m_programBin != nullptr)
                aligned_free(m_simdEntries[0]->ProgramOutput()->m_programBin);
        }
        if (simdMode != SIMDMode::SIMD16 && m_simdEntries[1] != nullptr)
        {
            if (m_simdEntries[1]->ProgramOutput()->m_programBin != nullptr)
                aligned_free(m_simdEntries[1]->ProgramOutput()->m_programBin);
        }
        if (simdMode != SIMDMode::SIMD32 && m_simdEntries[2] != nullptr)
        {
            if (m_simdEntries[2]->ProgramOutput()->m_programBin != nullptr)
                aligned_free(m_simdEntries[2]->ProgramOutput()->m_programBin);
        }
    }

    bool RetryManager::PickupCS(ComputeShaderContext* cgCtx)
    {
        SIMDMode simdMode = SIMDMode::UNKNOWN;
        CComputeShader* shader = nullptr;
        SComputeShaderKernelProgram* pKernelProgram = &cgCtx->programOutput;

        if (cgCtx->getModuleMetaData()->csInfo.forcedSIMDSize != 0)
        {
            shader = static_cast<CComputeShader*>(
                PickCSEntryForcedFromDriver(simdMode, cgCtx->getModuleMetaData()->csInfo.forcedSIMDSize));
        }
        if (!shader)
        {
            shader = static_cast<CComputeShader*>(
                PickCSEntryByRegKey(simdMode));
        }
        if (!shader)
        {
            shader = static_cast<CComputeShader*>(
                PickCSEntryEarly(simdMode, cgCtx));
        }
        if (!shader && IsLastTry())
        {
            shader = static_cast<CComputeShader*>(
                PickCSEntryFinally(simdMode));
            IGC_ASSERT(shader != nullptr);
        }

        if (shader)
        {
            switch (simdMode)
            {
            case SIMDMode::SIMD8:
                pKernelProgram->simd8 = *shader->ProgramOutput();
                pKernelProgram->SimdWidth = USC::GFXMEDIA_GPUWALKER_SIMD8;
                cgCtx->SetSIMDInfo(SIMD_SELECTED, simdMode,
                    ShaderDispatchMode::NOT_APPLICABLE);
                break;

            case SIMDMode::SIMD16:
                pKernelProgram->simd16 = *shader->ProgramOutput();
                pKernelProgram->SimdWidth = USC::GFXMEDIA_GPUWALKER_SIMD16;
                cgCtx->SetSIMDInfo(SIMD_SELECTED, simdMode,
                    ShaderDispatchMode::NOT_APPLICABLE);
                break;

            case SIMDMode::SIMD32:
                pKernelProgram->simd32 = *shader->ProgramOutput();
                pKernelProgram->SimdWidth = USC::GFXMEDIA_GPUWALKER_SIMD32;
                cgCtx->SetSIMDInfo(SIMD_SELECTED, simdMode,
                    ShaderDispatchMode::NOT_APPLICABLE);
                break;

            default:
                IGC_ASSERT_MESSAGE(0, "Invalie SIMDMode");
                break;
            }
            shader->FillProgram(pKernelProgram);
            pKernelProgram->SIMDInfo = cgCtx->GetSIMDInfo();


            // free allocated memory for the remaining kernels
            FreeAllocatedMemForNotPickedCS(simdMode);

            return true;
        }
        return false;
    }

    LLVMContextWrapper::LLVMContextWrapper(bool createResourceDimTypes)
    {
        if (createResourceDimTypes)
        {
            CreateResourceDimensionTypes(*this);
        }
    }

    void LLVMContextWrapper::AddRef()
    {
        refCount++;
    }

    void LLVMContextWrapper::Release()
    {
        refCount--;
        if (refCount == 0)
        {
            delete this;
        }
    }

    /** get shader's thread group size */
    unsigned ComputeShaderContext::GetThreadGroupSize()
    {
        llvm::GlobalVariable* pGlobal = getModule()->getGlobalVariable("ThreadGroupSize_X");
        unsigned threadGroupSize_X = int_cast<unsigned>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

        pGlobal = getModule()->getGlobalVariable("ThreadGroupSize_Y");
        unsigned threadGroupSize_Y = int_cast<unsigned>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

        pGlobal = getModule()->getGlobalVariable("ThreadGroupSize_Z");
        unsigned threadGroupSize_Z = int_cast<unsigned>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

        return threadGroupSize_X * threadGroupSize_Y * threadGroupSize_Z;
    }

    unsigned ComputeShaderContext::GetSlmSizePerSubslice()
    {
        return platform.getSlmSizePerSsOrDss();
    }

    unsigned ComputeShaderContext::GetSlmSize() const
    {
        return m_slmSize;
    }

    float ComputeShaderContext::GetThreadOccupancy(SIMDMode simdMode)
    {
        return GetThreadOccupancyPerSubslice(simdMode, GetThreadGroupSize(), GetHwThreadsPerWG(platform), m_slmSize, GetSlmSizePerSubslice());
    }

    /** get smallest SIMD mode allowed based on thread group size */
    SIMDMode ComputeShaderContext::GetLeastSIMDModeAllowed()
    {
        SIMDMode mode = getLeastSIMDAllowed(
            GetThreadGroupSize(),
            GetHwThreadsPerWG(platform));
        return mode;
    }

    /** get largest SIMD mode for performance based on thread group size */
    SIMDMode ComputeShaderContext::GetMaxSIMDMode()
    {
        unsigned threadGroupSize = GetThreadGroupSize();
        SIMDMode mode;
        if (threadGroupSize <= 8)
        {
            mode = SIMDMode::SIMD8;
        }
        else if (threadGroupSize <= 16)
        {
            mode = SIMDMode::SIMD16;
        }
        else
        {
            mode = SIMDMode::SIMD32;
        }
        return mode;
    }

    float ComputeShaderContext::GetSpillThreshold() const
    {
        float spillThresholdSLM =
            float(IGC_GET_FLAG_VALUE(CSSpillThresholdSLM)) / 100.0f;
        float spillThresholdNoSLM =
            float(IGC_GET_FLAG_VALUE(CSSpillThresholdNoSLM)) / 100.0f;
        return m_slmSize ? spillThresholdSLM : spillThresholdNoSLM;
    }

    bool OpenCLProgramContext::isSPIRV() const
    {
        return isSpirV;
    }

    void OpenCLProgramContext::setAsSPIRV()
    {
        isSpirV = true;
    }
    float OpenCLProgramContext::getProfilingTimerResolution()
    {
        return m_ProfilingTimerResolution;
    }

    uint32_t OpenCLProgramContext::getNumThreadsPerEU() const
    {
        return 0;
    }

    uint32_t OpenCLProgramContext::getNumGRFPerThread() const
    {
        return CodeGenContext::getNumGRFPerThread();
    }

    bool OpenCLProgramContext::forceGlobalMemoryAllocation() const
    {
        return m_InternalOptions.IntelForceGlobalMemoryAllocation;
    }

    bool OpenCLProgramContext::hasNoLocalToGenericCast() const
    {
        return m_InternalOptions.hasNoLocalToGeneric;
    }

    int16_t OpenCLProgramContext::getVectorCoalescingControl() const
    {
        // cmdline option > registry key
        int val = m_InternalOptions.VectorCoalescingControl;
        if (val < 0)
        {
            // no cmdline option
            val = IGC_GET_FLAG_VALUE(VATemp);
        }
        return val;
    }

    void CodeGenContext::initLLVMContextWrapper(bool createResourceDimTypes)
    {
        llvmCtxWrapper = new LLVMContextWrapper(createResourceDimTypes);
        llvmCtxWrapper->AddRef();
    }

    llvm::LLVMContext* CodeGenContext::getLLVMContext() const {
        return llvmCtxWrapper;
    }

    IGC::IGCMD::MetaDataUtils* CodeGenContext::getMetaDataUtils()
    {
        IGC_ASSERT_MESSAGE(nullptr != m_pMdUtils, "Metadata Utils is not initialized");
        return m_pMdUtils;
    }

    IGCLLVM::Module* CodeGenContext::getModule() const { return module; }

    static void initCompOptionFromRegkey(CodeGenContext* ctx)
    {
        CompOptions& opt = ctx->getModuleMetaData()->compOpt;

        opt.pixelShaderDoNotAbortOnSpill =
            IGC_IS_FLAG_ENABLED(PixelShaderDoNotAbortOnSpill);
        opt.forcePixelShaderSIMDMode =
            IGC_GET_FLAG_VALUE(ForcePixelShaderSIMDMode);
    }

    void CodeGenContext::setModule(llvm::Module* m)
    {
        module = (IGCLLVM::Module*)m;
        m_pMdUtils = new IGC::IGCMD::MetaDataUtils(m);
        modMD = new IGC::ModuleMetaData();
        initCompOptionFromRegkey(this);
    }

    // Several clients explicitly delete module without resetting module to null.
    // This causes the issue later when the dtor is invoked (trying to delete a
    // dangling pointer again). This function is used to replace any explicit
    // delete in order to prevent deleting dangling pointers happening.
    void CodeGenContext::deleteModule()
    {
        delete m_pMdUtils;
        delete modMD;
        delete module;
        m_pMdUtils = nullptr;
        modMD = nullptr;
        module = nullptr;
        delete annotater;
        annotater = nullptr;
    }

    IGC::ModuleMetaData* CodeGenContext::getModuleMetaData() const
    {
        IGC_ASSERT_MESSAGE(nullptr != modMD, "Module Metadata is not initialized");
        return modMD;
    }

    unsigned int CodeGenContext::getRegisterPointerSizeInBits(unsigned int AS) const
    {
        unsigned int pointerSizeInRegister = 32;
        switch (AS)
        {
        case ADDRESS_SPACE_GLOBAL:
        case ADDRESS_SPACE_CONSTANT:
        case ADDRESS_SPACE_GENERIC:
        case ADDRESS_SPACE_GLOBAL_OR_PRIVATE:
            pointerSizeInRegister =
                getModule()->getDataLayout().getPointerSizeInBits(AS);
            break;
        case ADDRESS_SPACE_LOCAL:
        case ADDRESS_SPACE_A32:
            pointerSizeInRegister = 32;
            break;
        case ADDRESS_SPACE_PRIVATE:
            if (getModuleMetaData()->compOpt.UseScratchSpacePrivateMemory)
            {
                pointerSizeInRegister = 32;
            }
            else
            {
                pointerSizeInRegister = ((type == ShaderType::OPENCL_SHADER) ?
                    getModule()->getDataLayout().getPointerSizeInBits(AS) : 64);
            }
            break;
        default:
            pointerSizeInRegister = 32;
            break;
        }
        return pointerSizeInRegister;
    }

    bool CodeGenContext::enableFunctionCall() const
    {
        return (m_enableSubroutine || m_enableFunctionPointer);
    }

    /// Check for user functions in the module and enable the m_enableSubroutine flag if exists
    void CodeGenContext::CheckEnableSubroutine(llvm::Module& M)
    {
        bool EnableSubroutine = false;
        for (auto& F : M)
        {
            if (F.isDeclaration() ||
                F.use_empty() ||
                isEntryFunc(getMetaDataUtils(), &F))
            {
                continue;
            }

            if (F.hasFnAttribute("KMPLOCK") ||
                F.hasFnAttribute(llvm::Attribute::NoInline) ||
                !F.hasFnAttribute(llvm::Attribute::AlwaysInline))
            {
                EnableSubroutine = true;
                break;
            }
        }
        m_enableSubroutine = EnableSubroutine;
    }

    void CodeGenContext::InitVarMetaData() {}

    CodeGenContext::~CodeGenContext()
    {
        clear();
    }


    void CodeGenContext::clear()
    {
        m_enableSubroutine = false;
        m_enableFunctionPointer = false;

        delete modMD;
        delete m_pMdUtils;
        modMD = nullptr;
        m_pMdUtils = nullptr;

        delete module;
        llvmCtxWrapper->Release();
        module = nullptr;
        llvmCtxWrapper = nullptr;
    }

    static const llvm::Function *getRelatedFunction(const llvm::Value *value)
    {
        if (value == nullptr)
            return nullptr;

        if (const llvm::Function *F = llvm::dyn_cast<llvm::Function>(value)) {
            return F;
        }
        if (const llvm::Argument *A = llvm::dyn_cast<llvm::Argument>(value)) {
            return A->getParent();
        }
        if (const llvm::BasicBlock *BB = llvm::dyn_cast<llvm::BasicBlock>(value)) {
            return BB->getParent();
        }
        if (const llvm::Instruction *I = llvm::dyn_cast<llvm::Instruction>(value)) {
            return I->getParent()->getParent();
        }

        return nullptr;
    }

    static bool isEntryPoint(CodeGenContext *ctx, const llvm::Function *F)
    {
        if (F == nullptr) {
            return false;
        }

        auto& FuncMD = ctx->getModuleMetaData()->FuncMD;
        auto FuncInfo = FuncMD.find(const_cast<llvm::Function *>(F));
        if (FuncInfo == FuncMD.end()) {
            return false;
        }

        const FunctionMetaData* MD = &FuncInfo->second;
        return MD->functionType == KernelFunction;
    }

    static void findCallingKernles
        (CodeGenContext *ctx, const llvm::Function *F, llvm::SmallPtrSetImpl<const llvm::Function *> *kernels)
    {
        if (F == nullptr || kernels->count(F))
            return;

        for (const llvm::User *U : F->users()) {
            auto *CI = llvm::dyn_cast<llvm::CallInst>(U);
            if (CI == nullptr)
                continue;

            if (CI->getCalledFunction() != F)
                continue;

            const llvm::Function *caller = getRelatedFunction(CI);
            if (isEntryPoint(ctx, caller)) {
                kernels->insert(caller);
                continue;
            }
            // Caller is not a kernel, try to check which kerneles might
            // be calling it:
            findCallingKernles(ctx, caller, kernels);
        }
    }

    // TODO: remove this wrapper once we move to LLVM 11
    static std::string demangle_wrapper(const std::string &name) {
#if LLVM_VERSION_MAJOR >= 11
        return llvm::demangle(name);
#else
        char *demangled = nullptr;

        demangled = llvm::itaniumDemangle(name.c_str(), nullptr, nullptr, nullptr);
        if (demangled == nullptr) {
            demangled = llvm::microsoftDemangle(name.c_str(), nullptr, nullptr, nullptr);
        }

        if (demangled == nullptr) {
            return name;
        }

        std::string result = demangled;
        std::free(demangled);
        return result;
#endif
    }

    void CodeGenContext::EmitError(const char* errorstr, const llvm::Value *context)
    {
        std::stringstream &ss = this->oclErrorMessage;

        ss << "\nerror: ";
        ss << errorstr;
        // Try to get debug location to print out the relevant info.
        if (const llvm::Instruction *I = llvm::dyn_cast_or_null<llvm::Instruction>(context)) {
            if (const llvm::DILocation *DL = I->getDebugLoc()) {
                ss << "\nin file: " << DL->getFilename().str() << ":" << DL->getLine() << "\n";
            }
        }
        // Try to find function related to given context
        // to print more informative error message.
        if (const llvm::Function *F = getRelatedFunction(context)) {
            // If the function is a kernel just print the kernel name.
            if (isEntryPoint(this, F)) {
                ss << "\nin kernel: '" << demangle_wrapper(std::string(F->getName())) << "'";
            // If the function is not a kernel try to print all kernels that
            // might be using this function.
            } else {
                llvm::SmallPtrSet<const llvm::Function *, 16> kernels;
                findCallingKernles(this, F, &kernels);

                const size_t kernelsCount = kernels.size();
                ss << "\nin function: '" << demangle_wrapper(std::string(F->getName())) << "' ";
                if (kernelsCount == 0) {
                    ss << "called indirectly by at least one of the kernels.\n";
                } else if (kernelsCount == 1) {
                    const llvm::Function *kernel = *kernels.begin();
                    ss << "called by kernel: '" << demangle_wrapper(std::string(kernel->getName())) << "'\n";
                } else {
                    ss << "called by kernels:\n";
                    for (const llvm::Function *kernel : kernels) {
                        ss << "  - '" << demangle_wrapper(std::string(kernel->getName())) << "'\n";
                    }
                }
            }
        }
        ss << "\nerror: backend compiler failed build.\n";
    }

    void CodeGenContext::EmitWarning(const char* warningstr)
    {
        this->oclWarningMessage << "\nwarning: ";
        this->oclWarningMessage << warningstr;
        this->oclWarningMessage << "\n";
    }

    CompOptions& CodeGenContext::getCompilerOption()
    {
        return getModuleMetaData()->compOpt;
    }

    void CodeGenContext::resetOnRetry()
    {
        m_tempCount = 0;
    }

    uint32_t CodeGenContext::getNumThreadsPerEU() const
    {
        return 0;
    }

    uint32_t CodeGenContext::getNumGRFPerThread() const
    {
        if (IGC_GET_FLAG_VALUE(TotalGRFNum) != 0)
        {
            return IGC_GET_FLAG_VALUE(TotalGRFNum);
        }
        if (getModuleMetaData()->csInfo.forceTotalGRFNum != 0)
        {
            return getModuleMetaData()->csInfo.forceTotalGRFNum;
        }
        return 128;
    }

    bool CodeGenContext::forceGlobalMemoryAllocation() const
    {
        return false;
    }

    bool CodeGenContext::hasNoLocalToGenericCast() const
    {
        return false;
    }

    int16_t CodeGenContext::getVectorCoalescingControl() const
    {
        return 0;
    }

    bool CodeGenContext::isPOSH() const
    {
        return this->getModule()->getModuleFlag(
            "IGC::PositionOnlyVertexShader") != nullptr;
    }

    void CodeGenContext::setFlagsPerCtx()
    {
        if (m_DriverInfo.DessaAliasLevel() != -1) {
            if ((int)IGC_GET_FLAG_VALUE(EnableDeSSAAlias) > m_DriverInfo.DessaAliasLevel())
            {
                IGC_SET_FLAG_VALUE(EnableDeSSAAlias, m_DriverInfo.DessaAliasLevel());
            }
        }
    }


}
