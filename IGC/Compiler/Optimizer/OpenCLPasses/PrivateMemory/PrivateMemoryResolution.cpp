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

#include "AdaptorCommon/ImplicitArgs.hpp"
#include "Compiler/Optimizer/OpenCLPasses/PrivateMemory/PrivateMemoryResolution.hpp"
#include "Compiler/Optimizer/OpenCLPasses/KernelArgs.hpp"
#include "Compiler/MetaDataUtilsWrapper.h"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/CISACodeGen/GenCodeGenModule.h"
#include "Compiler/CISACodeGen/LowerGEPForPrivMem.hpp"
#include "llvmWrapper/IR/DerivedTypes.h"
#include "common/LLVMWarningsPush.hpp"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "common/LLVMWarningsPop.hpp"
#include "Probe/Assertion.h"

using namespace llvm;
using namespace IGC;

namespace IGC {

    class ModuleAllocaInfo;
    /// @brief  PrivateMemoryResolution pass used for resolving private memory alloca instructions.
    ///         This is done by resolving the alloca instructions.
    ///         This pass depends on the PrivateMemoryUsageAnalysis and
    ///         AddImplicitArgs passes running before it.

    class PrivateMemoryResolution : public llvm::ModulePass
    {
    public:
        // Pass identification, replacement for typeid
        static char ID;

        /// @brief  Constructor
        PrivateMemoryResolution();

        /// @brief  Destructor
        ~PrivateMemoryResolution() {}

        /// @brief  Provides name of pass
        virtual llvm::StringRef getPassName() const override
        {
            return "PrivateMemoryResolution";
        }

        /// @brief  Adds the analysis required by this pass
        virtual void getAnalysisUsage(llvm::AnalysisUsage& AU) const override;

        /// @brief  Finds all alloca instructions, replaces them with by an llvm sequences.
        ///         and creates for each function a metadata that represents the total
        ///         amount of private memory needed by each work item.
        /// @param  M The Module to process.
        bool runOnModule(llvm::Module& M) override;

        /// @brief  Resolve collected alloca instructions.
        /// @param privateOnStack: whether the private variables are allocated on the stack
        /// @return true if there were resolved alloca, false otherwise.
        bool resolveAllocaInstructions(bool privateOnStack);

        /// Initialize setup like UseScratchSpacePrivateMemory.
        bool safeToUseScratchSpace(llvm::Module& M) const;

    private:
        struct arrayIndex
        {
            llvm::GetElementPtrInst* gep;
            unsigned int operandIndex;
        };

        static bool testTransposedMemory(const Type* pTmpType, const Type* const pTypeOfAccessedObject, uint64_t tmpAllocaSize, const uint64_t bufferSizeLimit);

        /// @brief  The module level alloca information
        ModuleAllocaInfo* m_ModAllocaInfo;

        /// @brief - Metadata API
        IGCMD::MetaDataUtils* m_pMdUtils;

        /// @brief - Current processed function
        llvm::Function* m_currFunction;
    };

    ModulePass* CreatePrivateMemoryResolution()
    {
        return new PrivateMemoryResolution();
    }
} // namespace IGC

void ModuleAllocaInfo::analyze() {
    if (FGA && FGA->getModule()) {
        IGC_ASSERT(FGA->getModule() == M);
        for (auto FG : *FGA)
            analyze(FG);
    }
    else {
        for (auto& F : M->getFunctionList()) {
            if (F.empty())
                continue;

            unsigned Offset = 0;
            unsigned Alignment = 0;
            analyze(&F, Offset, Alignment);
            if (Alignment > 0)
                Offset = iSTD::Align(Offset, Alignment);
            getOrCreateFuncAllocaInfo(&F)->TotalSize = Offset;
        }
    }
}

void ModuleAllocaInfo::analyze(FunctionGroup* FG)
{
    // Calculate the size of private-memory we need to allocate to
    // every function-sub-group. Eache sub-group is led by a kernel or
    // a stack-call function.
    // Note that the function order does affect the final total amount of
    // private memory due to possible alignment constraints.
    //
    for (auto SubG : FG->Functions) {
        unsigned Offset = 0;
        unsigned Alignment = 0;
        for (Function* F : *SubG) {
            if (F->empty())
                continue;
            analyze(F, Offset, Alignment);
        }

        // Use the final offset as the total size.
        if (Alignment > 0)
            Offset = iSTD::Align(Offset, Alignment);

        // All functions in this group will get the same final size.
        for (Function* F : *SubG) {
            if (F->empty())
                continue;
            getOrCreateFuncAllocaInfo(F)->TotalSize = Offset;
        }
    }
}

void ModuleAllocaInfo::analyze(Function* F, unsigned& Offset,
    unsigned& MaxAlignment) {
    // Create alloca info even when there is no alloca, so that each function gets
    // an info entry.
    FunctionAllocaInfo* AllocaInfo = getOrCreateFuncAllocaInfo(F);

    // Collect allocas.
    SmallVector<AllocaInst*, 8> Allocas;
    for (auto& BB : F->getBasicBlockList()) {
        for (auto& Inst : BB.getInstList()) {
            if (AllocaInst * AI = dyn_cast<AllocaInst>(&Inst)) {
                Allocas.push_back(AI);
            }
        }
    }

    if (Allocas.empty())
        return;

    // Group by alignment and smallest first.
    auto getAlignment = [=](AllocaInst* AI) -> unsigned {
        unsigned Alignment = AI->getAlignment();
        if (Alignment == 0)
            Alignment = DL->getABITypeAlignment(AI->getAllocatedType());
        return Alignment;
    };

    std::sort(Allocas.begin(), Allocas.end(),
        [=](AllocaInst* AI1, AllocaInst* AI2) {
        return getAlignment(AI1) < getAlignment(AI2);
    });

    for (auto AI : Allocas) {
        // Align alloca offset.
        unsigned Alignment = getAlignment(AI);
        Offset = iSTD::Align(Offset, Alignment);

        // Keep track of the maximal alignment seen so far.
        if (Alignment > MaxAlignment)
            MaxAlignment = Alignment;

        // Compute alloca size. We don't know the variable length
        // alloca size so skip it.
        if (!isa<ConstantInt>(AI->getArraySize())) {
            continue;
        }
        ConstantInt* const SizeVal = cast<ConstantInt>(AI->getArraySize());
        IGC_ASSERT(nullptr != SizeVal);
        unsigned CurSize = (unsigned)(SizeVal->getZExtValue() *
            DL->getTypeAllocSize(AI->getAllocatedType()));
        AllocaInfo->setAllocaDesc(AI, Offset, CurSize);

        // Increment the current offset for the next alloca.
        Offset += CurSize;
    }

    // Update collected allocas into the function alloca info object.
    AllocaInfo->Allocas.swap(Allocas);
}

// Register pass to igc-opt
#define PASS_FLAG "igc-private-mem-resolution"
#define PASS_DESCRIPTION "Resolves private memory allocation"
#define PASS_CFG_ONLY true
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(PrivateMemoryResolution, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_END(PrivateMemoryResolution, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char PrivateMemoryResolution::ID = 0;

PrivateMemoryResolution::PrivateMemoryResolution() : ModulePass(ID)
{
    initializePrivateMemoryResolutionPass(*PassRegistry::getPassRegistry());
}

void PrivateMemoryResolution::getAnalysisUsage(llvm::AnalysisUsage& AU) const
{
    AU.setPreservesCFG();
    AU.addRequired<MetaDataUtilsWrapper>();
    AU.addRequired<CodeGenContextWrapper>();
}

bool PrivateMemoryResolution::safeToUseScratchSpace(llvm::Module& M) const
{
    ModuleMetaData& modMD = *getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
    CodeGenContext& Ctx = *getAnalysis<CodeGenContextWrapper>().getCodeGenContext();

    //
    // Update UseScratchSpacePrivateMemory based on WA and be consistent with
    // the implementation of CEncoder::ByteScatter().
    //
    if (Ctx.m_DriverInfo.NeedWAToTransformA32MessagesToA64()
        && Ctx.platform.getWATable().WaNoA32ByteScatteredStatelessMessages)
    {
        return false;
    }

    //
    // For now, all APIs that use scratch space for private memory, must use scratch
    // memory except OpenCL, which can also use non-scratch space. For debugging
    // purpose, a registry key is used for OCL to turn ocl-use-scratch on/off.
    //
    bool supportsScratchSpacePrivateMemory = Ctx.m_DriverInfo.supportsScratchSpacePrivateMemory();
    bool supportsStatelessSpacePrivateMemory = Ctx.m_DriverInfo.supportsStatelessSpacePrivateMemory();
    bool bOCLLegacyStatelessCheck = true;

    if (Ctx.m_instrTypes.hasDynamicGenericLoadStore && Ctx.platform.canForcePrivateToGlobal())
    {
        return false;
    }

    if ((modMD.compOpt.OptDisable && bOCLLegacyStatelessCheck) || !supportsScratchSpacePrivateMemory
        || Ctx.forceGlobalMemoryAllocation()) {
        return false;
    }

    //
    // Do not use scratch space if module has any stack call.
    //
    if (bOCLLegacyStatelessCheck) {
        if (auto * FGA = getAnalysisIfAvailable<GenXFunctionGroupAnalysis>()) {
            if (FGA->getModule() == &M) {
                for (auto& I : *FGA) {
                    if (I->hasStackCall())
                        return false;
                }
            }
        }
    }

    const llvm::DataLayout* DL = &M.getDataLayout();

    for (auto& F : M) {
        if (F.isDeclaration())
            continue;

        // Check each instr of this function.
        for (auto& BB : F) {
            for (auto& I : BB) {
                if (AddrSpaceCastInst * CI = dyn_cast<AddrSpaceCastInst>(&I)) {
                    // It is not safe to use scratch space as private memory if kernel does
                    // AS casting to ADDRESS_SPACE_GLOBAL_OR_PRIVATE or ADDRESS_SPACE_PRIVATE.
                    // See speical hack CI code generated at ProgramScopeConstantResolution
                    const ADDRESS_SPACE targetAS = (ADDRESS_SPACE)(cast<PointerType>(CI->getType()))->getAddressSpace();
                    if (targetAS == ADDRESS_SPACE_GLOBAL_OR_PRIVATE || targetAS == ADDRESS_SPACE_PRIVATE) {
                        return false;
                    }
                }
            }
        }

        if (!isEntryFunc(m_pMdUtils, &F))
            continue;

        //
        // OCL kernel arguments with type like queue_t and struct are expressed as
        // pointer type. Since there is no explicit AS associated with those pointers,
        // e.g., %opencl.queue_t*, to have both host and device use the same pointer
        // size for those arguments, it is better to disable the use of scratch memory.
        //
        // TODO: fixed those types (they should be in global address space)
        if (Ctx.type == ShaderType::OPENCL_SHADER && IGC_IS_FLAG_ENABLED(ForceStatelessForQueueT)) {
            if (!F.arg_empty()) {
                KernelArgs kernelArgs(F, DL, m_pMdUtils, &modMD, Ctx.platform.getGRFSize());
                for (auto arg : kernelArgs) {
                    const KernelArg::ArgType argTy = arg.getArgType();
                    if (argTy == KernelArg::ArgType::PTR_DEVICE_QUEUE)
                    {
                        return false;
                    }
                }
            }
        }

        //
        // Each thread has up to 2 MB scratch space to use. That is, each WI
        // has up to (2*1024*1024 / 8) bytes of scratch space in SIMD8 mode.
        //
        auto funcInfoMD = m_pMdUtils->getFunctionsInfoItem(&F);
        bool isGeometryStageShader = Ctx.type == ShaderType::VERTEX_SHADER ||
            Ctx.type == ShaderType::HULL_SHADER ||
            Ctx.type == ShaderType::DOMAIN_SHADER ||
            Ctx.type == ShaderType::GEOMETRY_SHADER;

        // Start with simd16, which allows the medium size of space per WI
        // (simd8: largest, simd32, smallest). In doing so, there will be
        // some space left for spilling in simd8 if spilling happens.
        int32_t simd_size = isGeometryStageShader ? numLanes(Ctx.platform.getMinDispatchMode()) :
            numLanes(SIMDMode::SIMD16);
        const int32_t subGrpSize = funcInfoMD->getSubGroupSize()->getSIMD_size();
        if (subGrpSize > simd_size)
            simd_size = std::min(subGrpSize, static_cast<int32_t>(numLanes(SIMDMode::SIMD32)));
        int32_t groupSize = IGCMD::IGCMetaDataHelper::getThreadGroupSize(*m_pMdUtils, &F);
        if (groupSize == 0)
            groupSize = IGCMD::IGCMetaDataHelper::getThreadGroupSizeHint(*m_pMdUtils, &F);
        if (groupSize > simd_size)
            simd_size = std::min(groupSize, static_cast<int32_t>(numLanes(SIMDMode::SIMD32)));

        unsigned maxScratchSpaceBytes = Ctx.platform.maxPerThreadScratchSpace();
        unsigned scratchSpaceLimitPerWI = maxScratchSpaceBytes / simd_size;
        //
        // If spill happens, since the offset of scratch block rw send message
        // has only 12b, an assertion will be triggered if used scratch space
        // size >= 128 KB, here 128 KB = 2^12 * 256b.
        //
        const unsigned int totalPrivateMemPerWI = m_ModAllocaInfo->getTotalPrivateMemPerWI(&F);

        if (totalPrivateMemPerWI > scratchSpaceLimitPerWI) {
            return false;
        }
    }

    // It is safe to use scratch space for private memory.
    return true;
}

bool PrivateMemoryResolution::runOnModule(llvm::Module& M)
{
    // Get the analysis
    m_pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    const llvm::DataLayout* DL = &M.getDataLayout();
    auto* FGA = getAnalysisIfAvailable<GenXFunctionGroupAnalysis>();
    bool changed = false;

    ModuleAllocaInfo MemInfo(&M, DL, FGA);
    m_ModAllocaInfo = &MemInfo;

    ModuleMetaData& modMD = *getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();

    // This is the only place to initialize and define UseScratchSpacePrivateMemory.
    // we do not use scratch-space if any kernel uses stack-call because,
    // in order to use scratch-space, we change data-layout for the module,
    // change pointer-size of AS-private to 32-bit.
    bool bRet = safeToUseScratchSpace(M);
    modMD.compOpt.UseScratchSpacePrivateMemory = bRet;

    for (Function& F : M)
    {
        m_currFunction = &F;
        if (m_currFunction->isDeclaration())
        {
            continue;
        }
        if (m_pMdUtils->findFunctionsInfoItem(m_currFunction) ==
            m_pMdUtils->end_FunctionsInfo())
        {
            continue;
        }
        bool hasStackCall = (FGA && FGA->getGroup(m_currFunction) && FGA->getGroup(m_currFunction)->hasStackCall()) || m_currFunction->hasFnAttribute("visaStackCall");
        bool hasVLA = (FGA && FGA->getGroup(m_currFunction) && FGA->getGroup(m_currFunction)->hasVariableLengthAlloca()) || m_currFunction->hasFnAttribute("hasVLA");
        // Resolve collected alloca instructions for current function
        changed |= resolveAllocaInstructions(hasStackCall || hasVLA);
    }

    if (changed)
    {
        m_pMdUtils->save(M.getContext());
    }

    return changed;
}

// Sink allocas into its first dominating use if possible. Alloca instructions
// are placed in the first basic block which dominates all other blocks. During
// alloca resolution, all address computations are done in the first block. And
// the address objects are live from the starting point. E.g.
//
//  int i = x;
//  foo(i);
//  int j = y;
//  bar(j);
//
// Variables i, j do not overlap in the source. When i and j are both in
// memory (optimizations disabled), llvm IR looks like
//
// [0] alloca i
// [1] alloca j
// [2] store x into &i
// [3] load i
// [4] foo(i)
// [5] store y into &j
// [6] load j
// [7] bar(j)
// Notice that address &i and &j overlap, [0-4) and [1-7) resp. Sinking allocas
// i and j to their lifetime start alleviates this issue.
//
// [0] alloca i
// [1] store x into &i
// [2] load i
// [3] foo(i)
// [4] alloca j
// [5] store y into &j
// [6] load j
// [7] bar(j)
//
static void sinkAllocas(SmallVectorImpl<AllocaInst*>& Allocas) {
    IGC_ASSERT(false == Allocas.empty());
    DominatorTree DT;
    bool Calcuated = false;

    // For each alloca, sink it if it has a use that dominates all other uses.
    // This use is called the dominating use.
    for (auto AI : Allocas) {
        if (AI->user_empty())
            continue;

        // If an alloca is used other than in an instruction, skip it.
        bool Skip = false;
        SmallVector<Instruction*, 8> UInsts;
        for (auto U : AI->users()) {
            auto UI = dyn_cast<Instruction>(U);
            //can't sink the alloca in the same BB where a PHI node exists
            //As it will violate the basic block structure, since phi nodes
            //will always be at the beginging of a BB
            if (!UI || isa<PHINode>(UI)) {
                Skip = true;
                break;
            }
            UInsts.push_back(UI);
        }

        if (Skip)
            continue;

        // Compute dominator tree lazily.
        if (!Calcuated) {
            Function* F = AI->getParent()->getParent();
            DT.recalculate(*F);
            Calcuated = true;
        }

        // Find the Nearest Common Denominator for all the uses
        Instruction* DomUse = UInsts[0];
        BasicBlock* DomBB = DomUse->getParent();
        for (unsigned i = 1; i < UInsts.size(); ++i) {
            Instruction* Use = UInsts[i];
            BasicBlock* UseBB = Use->getParent();
            DomBB = DT.findNearestCommonDominator(DomBB, UseBB);
            if (!DomBB) {
                break;
            }
        }

        if (DomBB) {
            // If DomBB has a use in it, insert it just before the first use.
            // Otherwise, append it to the end of the block, to reduce register pressure.
            Instruction* InsertPt = DomBB->getTerminator();
            for (Instruction* Use : UInsts) {
                if (DomBB == Use->getParent() && DT.dominates(Use, InsertPt)) {
                    InsertPt = Use;
                }
            }
            AI->moveBefore(InsertPt);
        }
    }
}

static void sinkAllocaSingleUse(SmallVectorImpl<AllocaInst*>& Allocas) {
    IGC_ASSERT(false == Allocas.empty());
    DominatorTree DT;
    bool Calcuated = false;

    // For each alloca's use, sink it if it has a use that dominates all other uses.
    // This use is called the dominating use.
    for (auto AI : Allocas) {
        if (AI->user_empty())
            continue;

        for (auto A : AI->users())
        {
            bool Skip = false;
            SmallVector<Instruction*, 8> UInsts;
            auto UI = dyn_cast<Instruction>(A);
            for (auto U : UI->users()) {
                auto UUI = dyn_cast<Instruction>(U);
                //can't sink the use in the same BB where a PHI node exists
                //As it will violate the basic block structure, since phi nodes
                //will always be at the beginging of a BB
                if (!UUI || isa<PHINode>(UUI)) {
                    Skip = true;
                    break;
                }
                UInsts.push_back(UUI);
            }
            if (Skip || UInsts.size() == 0)
                continue;
            // Compute dominator tree lazily.
            if (!Calcuated) {
                Function* F = AI->getParent()->getParent();
                DT.recalculate(*F);
                Calcuated = true;
            }

            // Find the Nearest Common Denominator for all the uses
            Instruction* DomUse = UInsts[0];
            BasicBlock* DomBB = DomUse->getParent();
            for (unsigned i = 1; i < UInsts.size(); ++i) {
                Instruction* Use = UInsts[i];
                BasicBlock* UseBB = Use->getParent();
                DomBB = DT.findNearestCommonDominator(DomBB, UseBB);
                if (!DomBB) {
                    break;
                }
            }

            if (DomBB) {
                // If DomBB has a use in it, insert it just before the first use.
                // Otherwise, append it to the end of the block, to reduce register pressure.
                Instruction* InsertPt = DomBB->getTerminator();
                for (Instruction* Use : UInsts) {
                    if (DomBB == Use->getParent() && DT.dominates(Use, InsertPt)) {
                        InsertPt = Use;
                    }
                }
                UI->moveBefore(InsertPt);
            }
        }
    }
}

class TransposeHelperPrivateMem : public TransposeHelper
{
public:
    Value* simdSize;
    Value* base;
    unsigned int elementSize;
    bool vectorIO;
    TransposeHelperPrivateMem(Value* b, Value* size, unsigned int eltSize, bool vectorType) : TransposeHelper(vectorType) {
        simdSize = size;
        base = b;
        elementSize = eltSize;
        vectorIO = vectorType;
    }
    void handleLoadInst(LoadInst* pLoad, Value* pScalarizedIdx)
    {
        IGC_ASSERT(nullptr != pLoad);
        IGC_ASSERT(pLoad->isSimple());
        IRBuilder<> IRB(pLoad);
        if (isa<Instruction>(pLoad->getPointerOperand()))
        {
            IRB.SetInsertPoint(cast<Instruction>(pLoad->getPointerOperand()));
        }
        Value* eltSize = IRB.getInt32(elementSize);
        Value* stride = IRB.CreateMul(simdSize, eltSize);
        Value* address = IRB.CreateMul(pScalarizedIdx, stride);
        address = IRB.CreateAdd(base, address);
        IRB.SetInsertPoint(pLoad);
        if (!vectorIO && pLoad->getType()->isVectorTy())
        {
            Type* scalarType = pLoad->getPointerOperand()->getType()->getPointerElementType()->getScalarType();
            IGC_ASSERT(nullptr != scalarType);
            Type* scalarptrTy = PointerType::get(scalarType, pLoad->getPointerAddressSpace());
            IGC_ASSERT(scalarType->getPrimitiveSizeInBits() / 8 == elementSize);
            Value* vec = UndefValue::get(pLoad->getType());
            auto pLoadVT = cast<IGCLLVM::FixedVectorType>(pLoad->getType());
            for (unsigned i = 0, e = (unsigned)pLoadVT->getNumElements(); i < e; ++i)
            {
                Value* ptr = IRB.CreateIntToPtr(address, scalarptrTy);
                Value* v = IRB.CreateLoad(ptr);
                vec = IRB.CreateInsertElement(vec, v, IRB.getInt32(i));
                address = IRB.CreateAdd(address, stride);
            }
            pLoad->replaceAllUsesWith(vec);
            pLoad->eraseFromParent();
        }
        else
        {
            Value* ptr = IRB.CreateIntToPtr(address, pLoad->getPointerOperand()->getType());
            pLoad->setOperand(0, ptr);
        }
    }
    void handleStoreInst(StoreInst* pStore, Value* pScalarizedIdx)
    {
        IGC_ASSERT(nullptr != pStore);
        IGC_ASSERT(pStore->isSimple());
        IRBuilder<> IRB(pStore);
        if (isa<Instruction>(pStore->getPointerOperand()))
        {
            IRB.SetInsertPoint(cast<Instruction>(pStore->getPointerOperand()));
        }
        Value* eltSize = IRB.getInt32(elementSize);
        Value* stride = IRB.CreateMul(simdSize, eltSize);
        Value* address = IRB.CreateMul(pScalarizedIdx, stride);
        address = IRB.CreateAdd(base, address);
        IRB.SetInsertPoint(pStore);
        if (!vectorIO && pStore->getValueOperand()->getType()->isVectorTy())
        {
            Type* scalarType = pStore->getPointerOperand()->getType()->getPointerElementType()->getScalarType();
            IGC_ASSERT(nullptr != scalarType);
            Type* scalarptrTy = PointerType::get(scalarType, pStore->getPointerAddressSpace());
            IGC_ASSERT(scalarType->getPrimitiveSizeInBits() / 8 == elementSize);
            Value* vec = pStore->getValueOperand();

            unsigned vecNumElts = (unsigned)cast<IGCLLVM::FixedVectorType>(vec->getType())->getNumElements();
            for (unsigned i = 0; i < vecNumElts; ++i)
            {
                Value* ptr = IRB.CreateIntToPtr(address, scalarptrTy);
                IRB.CreateStore(IRB.CreateExtractElement(vec, IRB.getInt32(i)), ptr);
                address = IRB.CreateAdd(address, stride);
            }
            pStore->eraseFromParent();
        }
        else
        {
            Value* ptr = IRB.CreateIntToPtr(address, pStore->getPointerOperand()->getType());
            pStore->setOperand(1, ptr);
        }
    }
};

bool PrivateMemoryResolution::testTransposedMemory(const Type* pTmpType, const Type* const pTypeOfAccessedObject, uint64_t tmpAllocaSize, const uint64_t bufferSizeLimit)
{
    // verify that the size of transposed memory fits into the allocated scratch region

    bool ok = true;

    if(ok)
    {
        ok = (nullptr != pTmpType);
        IGC_ASSERT(ok);
    }

    if(ok)
    {
        ok = (nullptr != pTypeOfAccessedObject);
        IGC_ASSERT(ok);
    }

    if(ok)
    {
        ok = (0 < tmpAllocaSize);
        IGC_ASSERT(ok);
    }

    if(ok)
    {
        ok = (tmpAllocaSize <= bufferSizeLimit);
        IGC_ASSERT(ok);
    }

    while(ok && (pTypeOfAccessedObject != pTmpType))
    {
        if(pTmpType->isStructTy() && (pTmpType->getStructNumElements() == 1))
        {
            pTmpType = pTmpType->getStructElementType(0);
            ok = (nullptr != pTmpType);
            IGC_ASSERT(ok);
        }
        else if(pTmpType->isArrayTy())
        {
            tmpAllocaSize *= pTmpType->getArrayNumElements();
            pTmpType = pTmpType->getContainedType(0);
            ok = (nullptr != pTmpType);
            IGC_ASSERT(ok);
        }
        else if(pTmpType->isVectorTy())
        {
            auto pTmpVType = cast<IGCLLVM::FixedVectorType>(pTmpType);
            tmpAllocaSize *= pTmpVType->getNumElements();
            pTmpType = pTmpType->getContainedType(0);
            ok = (nullptr != pTmpType);
            IGC_ASSERT(ok);
        }
        else
        {
            // unsupported type for memory transposition
            ok = false;
            IGC_ASSERT(ok);
        }
    }

    if(ok)
    {
        ok = (0 < tmpAllocaSize);
        IGC_ASSERT(ok);
    }

    if(ok)
    {
        ok = (tmpAllocaSize <= bufferSizeLimit);
        IGC_ASSERT(ok);
    }

    return ok;
}

bool PrivateMemoryResolution::resolveAllocaInstructions(bool privateOnStack)
{
    CodeGenContext& Ctx = *getAnalysis<CodeGenContextWrapper>().getCodeGenContext();

    // It is possible that there is no alloca instruction in the caller but there
    // is alloca in the callee. Save the total private memory to the metadata.
    unsigned int totalPrivateMemPerWI = m_ModAllocaInfo->getTotalPrivateMemPerWI(m_currFunction);

    // This change is only till the FuncMD is ported to new MD framework
    ModuleMetaData* const modMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
    IGC_ASSERT(nullptr != modMD);
    modMD->FuncMD[m_currFunction].privateMemoryPerWI = totalPrivateMemPerWI;
    modMD->privateMemoryPerWI = totalPrivateMemPerWI;//redundant ?

    SmallVector<AllocaInst*, 8> & allocaInsts = m_ModAllocaInfo->getAllocaInsts(m_currFunction);
    if (allocaInsts.empty())
    {
        // No alloca instructions to process.
        return false;
    }

    // Disable sinking of alloca when optimizations are disabled. allocas (and associated bitcasts) dont
    // contain any DebugLoc. Emitting low/high_pc for inlined subprograms becomes messy as allocas appear
    // in subprogram prolog without any debug info. This makes debug info assume alloca code belongs to
    // main program causing a break in low/high_pc of such subprograms. Not sinking allocas makes them
    // appear in entry block of entry function. All such allocas get treated as being in prolog of
    // entry function. This results in live-range extension, but with -O0 this is not a problem as debugging
    // experience is more important than extra spills.
    if (!modMD->compOpt.OptDisable)
    {
        if (Ctx.m_instrTypes.numAllocaInsts > IGC_GET_FLAG_VALUE(AllocaRAPressureThreshold))
        {
            sinkAllocaSingleUse(allocaInsts);
        }
        sinkAllocas(allocaInsts);
    }
    // If there are N+1 private buffers, and M+1 threads,
    // the layout representing the private memory will look like this:

    // [buffer0 thread0][buffer1 thread0]...[bufferN thread0]
    // [buffer0 thread1][buffer1 thread1]...[bufferN thread1]
    // ...
    // [buffer0 threadM][buffer1 threadM]...[bufferN threadM]
    // Note that for each thread, all SIMD lanes of the same buffers are
    // laid out sequentially to preserve locality.
    // So, in fact, [buffer0 thread0] represents
    // [buffer0 lane0][buffer0 lane1]...[buffer0 laneK]
    // where the SIMD width is K-1.

    // Each row represent total private memory per thread

    // To get buffer i of thread j we need to calculate:
    // {buffer i ptr} = privateBase +
    //                  threadId * {total private mem per thread} +
    //                  {buffer offset} +
    //                  {per lane offset}

    // Where:
    // privateBase                      = implicit argument, points to [buffer0 thread0]
    // {total private mem per thread}   = simdSize * {total private mem per WI}
    // {buffer offset}                  = simdSize * {buffer i offset per WI}
    // {per lane offset}                = simdLaneId * sizeof(buffer i)

    // simdSize and simdOffsetBase are calculated using intrinsics that are planted in this pass
    // and resolved in the code gen

    LLVMContext& C = m_currFunction->getContext();

    IntegerType* typeInt32 = Type::getInt32Ty(C);
    // Creates intrinsics that will be lowered in the CodeGen and will handle the simd lane id
    Function* simdLaneIdFunc = GenISAIntrinsic::getDeclaration(m_currFunction->getParent(), GenISAIntrinsic::GenISA_simdLaneId);
    // Creates intrinsics that will be lowered in the CodeGen and will handle the simd size
    Function* simdSizeFunc = GenISAIntrinsic::getDeclaration(m_currFunction->getParent(), GenISAIntrinsic::GenISA_simdSize);

    llvm::IRBuilder<> entryBuilder(&*m_currFunction->getEntryBlock().getFirstInsertionPt());
    ImplicitArgs implicitArgs(*m_currFunction, m_pMdUtils);

    // Construct an empty DebugLoc.
    IF_DEBUG_INFO(DebugLoc entryDebugLoc);
    // Assign with the function location if available.
    IF_DEBUG_INFO_IF(DISubprogram *subprogram = m_currFunction->getSubprogram(), entryDebugLoc = DILocation::get(subprogram->getContext(), subprogram->getLine(), 0, subprogram););
    IF_DEBUG_INFO(entryBuilder.SetCurrentDebugLocation(entryDebugLoc));

    if (privateOnStack)
    {
        // Creates intrinsics that will be lowered in the CodeGen and will handle the stack-pointer
        Instruction* simdLaneId16 = entryBuilder.CreateCall(simdLaneIdFunc, llvm::None, VALUE_NAME("simdLaneId16"));
        Value* simdLaneId = entryBuilder.CreateIntCast(simdLaneId16, typeInt32, false, VALUE_NAME("simdLaneId"));
        Instruction* simdSize = entryBuilder.CreateCall(simdSizeFunc, llvm::None, VALUE_NAME("simdSize"));
        for (auto pAI : allocaInsts)
        {
            bool isUniform = pAI->getMetadata("uniform") != nullptr;
            llvm::IRBuilder<> builder(pAI);
            IF_DEBUG_INFO(builder.SetCurrentDebugLocation(entryDebugLoc));

            // buffer of this private var
            Value* privateBuffer = nullptr;
            if (!isa<ConstantInt>(pAI->getArraySize()))
            {
                // vla array must be AOS layout on stack
                Value* increment = isUniform ? builder.getInt32(0) : simdLaneId;
                // truncate alloca size to i32
                Value* arraySize = builder.CreateTrunc(pAI->getArraySize(), increment->getType(), VALUE_NAME("TruncVLASize"));
                Value* sizeWithType = builder.CreateMul(arraySize,
                    builder.getInt32(static_cast<uint32_t>(m_currFunction->getParent()->getDataLayout().getTypeAllocSize(pAI->getAllocatedType()))),
                    VALUE_NAME("VLASizeWithType"));
                Value* perLaneOffset = builder.CreateMul(increment, sizeWithType, VALUE_NAME("VLAPerLaneOffset"));
                // Create VLAStackAlloca intrinsic which will set private buffer offset to "SP + laneOffset",
                // and set SP to "SP + buffer_size" in visa emitPass
                Value* intrinArgs[] = { perLaneOffset, sizeWithType };
                Function* stackAllocaFunc = GenISAIntrinsic::getDeclaration(m_currFunction->getParent(), GenISAIntrinsic::GenISA_VLAStackAlloca);
                Value* stackAlloca = builder.CreateCall(stackAllocaFunc, intrinArgs , VALUE_NAME("VLAStackAlloca"));
                privateBuffer = builder.CreatePointerCast(stackAlloca, pAI->getType(), VALUE_NAME(pAI->getName() + ".privateBuffer"));
            }
            else
            {
                int scalarBufferOffset = m_ModAllocaInfo->getConstBufferOffset(pAI);
                unsigned int bufferSize = m_ModAllocaInfo->getConstBufferSize(pAI);

                Value* bufferOffset = builder.CreateMul(simdSize, ConstantInt::get(typeInt32, scalarBufferOffset), VALUE_NAME(pAI->getName() + ".SIMDBufferOffset"));
                Value* increment = isUniform ? builder.getInt32(0) : simdLaneId;
                Value* perLaneOffset = builder.CreateMul(increment, ConstantInt::get(typeInt32, bufferSize), VALUE_NAME("perLaneOffset"));
                Value* totalOffset = builder.CreateAdd(bufferOffset, perLaneOffset, VALUE_NAME(pAI->getName() + ".totalOffset"));
                Function* stackAllocaFunc = GenISAIntrinsic::getDeclaration(m_currFunction->getParent(), GenISAIntrinsic::GenISA_StackAlloca);
                Value* stackAlloca = builder.CreateCall(stackAllocaFunc, totalOffset, VALUE_NAME("stackAlloca"));
                privateBuffer = builder.CreatePointerCast(stackAlloca, pAI->getType(), VALUE_NAME(pAI->getName() + ".privateBuffer"));
                auto DbgUses = llvm::FindDbgAddrUses(pAI);
                for (auto Use : DbgUses)
                {
                    if (auto DbgDcl = dyn_cast_or_null<DbgDeclareInst>(Use))
                    {
                        // Attach metadata to instruction containing offset of storage
                        auto OffsetMD = MDNode::get(builder.getContext(), ConstantAsMetadata::get(builder.getInt32(scalarBufferOffset)));
                        DbgDcl->setMetadata("StorageOffset", OffsetMD);
                        if (IGC_IS_FLAG_ENABLED(UseOffsetInLocation))
                        {
                            auto SizeMD = MDNode::get(builder.getContext(), ConstantAsMetadata::get(builder.getInt32(bufferSize)));
                            DbgDcl->setMetadata("StorageSize", SizeMD);
                        }
                    }
                }
            }

            // Replace all uses of original alloca with the bitcast
            pAI->replaceAllUsesWith(privateBuffer);
            pAI->eraseFromParent();
        }
        return true;
    }

    // What is the size limit of this scratch memory? If we use >= 128 KB for private data, then we have
    // no space left for later spilling.
    bool useStateless = false;
    if (modMD && (modMD->compOpt.UseScratchSpacePrivateMemory || useStateless)) {
        // We want to use this pass to lower alloca instruction
        // to remove some redundant instruction caused by alloca. For original approach,
        // different threads use the same private base. While for this approach, each
        // thread has its own private base, so we don't have to calculate the
        // private base from threadid as we did previously.  In this case, we only need
        // PrivateMemoryUsageAnalysis pass, no need to run AddImplicitArgs pass.

        Instruction* simdLaneId16 = entryBuilder.CreateCall(simdLaneIdFunc, llvm::None, VALUE_NAME("simdLaneId16"));
        Value* simdLaneId = entryBuilder.CreateIntCast(simdLaneId16, typeInt32, false, VALUE_NAME("simdLaneId"));
        Instruction* simdSize = entryBuilder.CreateCall(simdSizeFunc, llvm::None, VALUE_NAME("simdSize"));

        Value* privateBase = nullptr;
        if (modMD->compOpt.UseScratchSpacePrivateMemory)
        {
            Argument* r0Arg = implicitArgs.getArgInFunc(*m_currFunction, ImplicitArg::R0);
            Value* r0_5 = entryBuilder.CreateExtractElement(r0Arg, ConstantInt::get(typeInt32, 5), VALUE_NAME("r0.5"));
            privateBase = entryBuilder.CreateAnd(r0_5, ConstantInt::get(typeInt32, 0xFFFFFC00), VALUE_NAME("privateBase"));
        }

        ADDRESS_SPACE scratchMemoryAddressSpace = ADDRESS_SPACE_PRIVATE;

        for (auto pAI : allocaInsts)
        {
            bool isUniform = pAI->getMetadata("uniform") != nullptr;
            llvm::IRBuilder<> builder(pAI);
            // Post upgrade to LLVM 3.5.1, it was found that inliner propagates debug info of callee
            // in to the alloca. Further, those allocas are somehow hoisted to the top of program.
            // When those allocas are lowered to below sequence, they result in prologue instructions
            // pointing to a much later line of code. This causes a single src line to now have
            // multiple VISA offset mappings and prevents debugger from setting breakpoints
            // correctly. So instead, we set DebugLoc for the instructions generated by lowering
            // alloca to mark that they are part of the prologue.
            // Note: As per Amjad, later LLVM version has a fix for this in llvm/lib/Transforms/Utils/InlineFunction.cpp.
            IF_DEBUG_INFO(builder.SetCurrentDebugLocation(pAI->getDebugLoc()));

            // Get buffer information from the analysis
            unsigned int scalarBufferOffset = m_ModAllocaInfo->getConstBufferOffset(pAI);
            // If we can use SOA layout transpose the memory
            Type* pTypeOfAccessedObject = nullptr;

            bool TransposeMemLayout =
                CanUseSOALayout(pAI, pTypeOfAccessedObject);

            unsigned int bufferSize = 0;
            if (TransposeMemLayout)
            {
                auto DL = &m_currFunction->getParent()->getDataLayout();
                bufferSize = (unsigned)DL->getTypeAllocSize(pTypeOfAccessedObject);
                IGC_ASSERT(testTransposedMemory((pAI->getType()->getPointerElementType()), pTypeOfAccessedObject, bufferSize, (m_ModAllocaInfo->getConstBufferSize(pAI))));
            }
            else
            {
                bufferSize = m_ModAllocaInfo->getConstBufferSize(pAI);
            }

            Value* bufferOffset = builder.CreateMul(simdSize, ConstantInt::get(typeInt32, scalarBufferOffset), VALUE_NAME(pAI->getName() + ".SIMDBufferOffset"));
            Value* perLaneOffset = isUniform ? builder.getInt32(0) : simdLaneId;
            perLaneOffset = builder.CreateMul(perLaneOffset, ConstantInt::get(typeInt32, bufferSize), VALUE_NAME("perLaneOffset"));
            Value* totalOffset = builder.CreateAdd(bufferOffset, perLaneOffset, VALUE_NAME(pAI->getName() + ".totalOffset"));
            totalOffset = builder.CreateZExt(totalOffset, privateBase->getType());
            Value* threadOffset = builder.CreateAdd(privateBase, totalOffset, VALUE_NAME(pAI->getName() + ".threadOffset"));
            Value* privateBufferPTR = builder.CreateIntToPtr(threadOffset, Type::getInt8Ty(C)->getPointerTo(scratchMemoryAddressSpace), VALUE_NAME(pAI->getName() + ".privateBufferPTR"));
            Value* privateBuffer = builder.CreatePointerCast(privateBufferPTR, pAI->getType(), VALUE_NAME(pAI->getName() + ".privateBuffer"));

            if (TransposeMemLayout)
            {
                TransposeHelperPrivateMem helper(threadOffset, simdSize, bufferSize, pTypeOfAccessedObject->isVectorTy());
                Value* Idx = builder.getInt32(0);
                helper.HandleAllocaSources(pAI, Idx);
                helper.EraseDeadCode();
            }

            // Replace all uses of original alloca with the bitcast
            pAI->replaceAllUsesWith(privateBuffer);
            pAI->eraseFromParent();
        }

        return true;
    }

    // Find the implicit argument representing r0 and the private memory base.
    Argument* r0Arg = implicitArgs.getArgInFunc(*m_currFunction, ImplicitArg::R0);
    Argument* privateMemArg = implicitArgs.getArgInFunc(*m_currFunction, ImplicitArg::PRIVATE_BASE);
    // Note: for debugging purposes privateMemArg will be marked as Output to keep its liveness all time

    // Resolve the call

    // Receives:
    // %privateMem = alloca ...

    // Create a GEP to get to the right offset from the private memory base implicit arg:

    // %simdLaneId16                = call i16 @llvm.gen.simdLaneId()
    // %simdLaneId                  = zext i16 simdLaneId16 to i32
    // %simdSize                    = call i32 @llvm.gen.simdSize()
    // %totalPrivateMemPerThread    = mul i32 %simdSize, <totalPrivateMemPerWI>

    // %r0.5                        = extractelement <8 x i32> %r0, i32 5
    // %threadId                    = and i32 %r0.5, 0x1FF|0x3FF   (Thread ID is in the lower 9 bits or 10 bit(KBL & CNL+) of r0.5)
    // %perThreadOffset             = mul i32 %threadId, %totalPrivateMemPerThread

    ConstantInt* totalPrivateMemPerWIValue = ConstantInt::get(typeInt32, totalPrivateMemPerWI);

    Instruction* simdLaneId16 = entryBuilder.CreateCall(simdLaneIdFunc, llvm::None, VALUE_NAME("simdLaneId16"));
    Value* simdLaneId = entryBuilder.CreateIntCast(simdLaneId16, typeInt32, false, VALUE_NAME("simdLaneId"));
    Instruction* simdSize = entryBuilder.CreateCall(simdSizeFunc, llvm::None, VALUE_NAME("simdSize"));
    Value* totalPrivateMemPerThread = entryBuilder.CreateMul(simdSize, totalPrivateMemPerWIValue, VALUE_NAME("totalPrivateMemPerThread"));

    Function* pHWTIDFunc = GenISAIntrinsic::getDeclaration(m_currFunction->getParent(), GenISAIntrinsic::GenISA_hw_thread_id_alloca, Type::getInt32Ty(C));
    Value* threadId = entryBuilder.CreateCall(pHWTIDFunc);

    if (Ctx.platform.supportTwoStackTSG() && IGC_IS_FLAG_ENABLED(EnableGen11TwoStackTSG))
    {
        // Gen11 , 2 - stack configuration : (FFTID[9:0] << 1) | FFSID[0]) * scratch_size
        Value* shlThreadID = entryBuilder.CreateShl(threadId, ConstantInt::get(typeInt32, 1), VALUE_NAME("shlThreadID"));

        // FFSID - r0.0 bit 16
        Value* r0_0 = entryBuilder.CreateExtractElement(r0Arg, ConstantInt::get(typeInt32, 0), VALUE_NAME("r0.0"));
        Value* FFSIDbit = entryBuilder.CreateLShr(r0_0, ConstantInt::get(typeInt32, 16), VALUE_NAME("FFSIDbit"));
        Value* FFSID = entryBuilder.CreateAnd(FFSIDbit, ConstantInt::get(typeInt32, 1), VALUE_NAME("FFSID"));

        threadId = entryBuilder.CreateOr(FFSID, shlThreadID, VALUE_NAME("threadId"));
    }

    Value* perThreadOffset = entryBuilder.CreateMul(threadId, totalPrivateMemPerThread, VALUE_NAME("perThreadOffset"));
    auto perThreadOffsetInst = dyn_cast_or_null<Instruction>(perThreadOffset);

    if (IGC_IS_FLAG_ENABLED(UseOffsetInLocation) &&
        (privateOnStack == false) &&
        (IGC_GET_FLAG_VALUE(FunctionControl) < FLAG_FCALL_FORCE_STACKCALL))
    {
        IGC_ASSERT_MESSAGE(perThreadOffsetInst, "perThreadOffset will not be marked as Output");
        if (perThreadOffsetInst)
        {
            // Note: for debugging purposes privateMemArg, as well as privateMemArg (aka ImplicitArg::PRIVATE_BASE)
            // will be marked as Output to keep its liveness all time
            auto perThreadOffsetMD = MDNode::get(entryBuilder.getContext(), nullptr); // ConstantAsMetadata::get(entryBuilder.getInt32(1)));
            perThreadOffsetInst->setMetadata("perThreadOffset", perThreadOffsetMD);
        }
    }

    for (auto pAI : allocaInsts)
    {
        // %bufferOffset                = mul i32 %simdSize, <scalarBufferOffset>
        // %bufferOffsetForThread       = add i32 %perThreadOffset, %bufferOffset
        // %perLaneOffset               = mul i32 %simdLaneId, <bufferSize>
        // %totalOffset                 = add i32 %bufferOffsetForThread, %perLaneOffset
        // %privateBufferGEP            = getelementptr i8* %privateBase, i32 %totalOffset
        // %privateBuffer               = bitcast i8* %offsettmp1 to <buffer type>

        llvm::IRBuilder<> builder(pAI);
        IF_DEBUG_INFO(builder.SetCurrentDebugLocation(entryDebugLoc));
        bool isUniform = pAI->getMetadata("uniform") != nullptr;
        // Get buffer information from the analysis
        unsigned int scalarBufferOffset = m_ModAllocaInfo->getConstBufferOffset(pAI);
        unsigned int bufferSize = m_ModAllocaInfo->getConstBufferSize(pAI);

        Value* bufferOffset = builder.CreateMul(simdSize, ConstantInt::get(typeInt32, scalarBufferOffset), VALUE_NAME(pAI->getName() + ".SIMDBufferOffset"));
        Value* bufferOffsetForThread = builder.CreateAdd(perThreadOffset, bufferOffset, VALUE_NAME(pAI->getName() + ".bufferOffsetForThread"));
        Value* perLaneOffset = isUniform ? builder.getInt32(0) : simdLaneId;
        perLaneOffset = builder.CreateMul(perLaneOffset, ConstantInt::get(typeInt32, bufferSize), VALUE_NAME("perLaneOffset"));
        Value* totalOffset = builder.CreateAdd(bufferOffsetForThread, perLaneOffset, VALUE_NAME(pAI->getName() + ".totalOffset"));
        Value* privateBufferGEP = builder.CreateGEP(privateMemArg, totalOffset, VALUE_NAME(pAI->getName() + ".privateBufferGEP"));
        Value* privateBuffer = builder.CreatePointerCast(privateBufferGEP, pAI->getType(), VALUE_NAME(pAI->getName() + ".privateBuffer"));

        auto DbgUses = llvm::FindDbgAddrUses(pAI);
        for (auto Use : DbgUses)
        {
            if (auto DbgDcl = dyn_cast_or_null<DbgDeclareInst>(Use))
            {
                // Attach metadata to instruction containing offset of storage
                auto OffsetMD = MDNode::get(builder.getContext(), ConstantAsMetadata::get(builder.getInt32(scalarBufferOffset)));
                DbgDcl->setMetadata("StorageOffset", OffsetMD);
            }
        }

        // Replace all uses of original alloca with the bitcast
        pAI->replaceAllUsesWith(privateBuffer);
        pAI->eraseFromParent();
    }

    return true;
}
