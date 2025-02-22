/*========================== begin_copyright_notice ============================

Copyright (c) 2010-2021 Intel Corporation

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

#include "Compiler/CodeGenPublic.h"
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/IGCPassSupport.h"
#include "common/IGCIRBuilder.h"
#include "LLVM3DBuilder/BuiltinsFrontend.hpp"
#include "UniformAssumptions.hpp"
#include "Probe/Assertion.h"

using namespace llvm;

namespace IGC {

    char UniformAssumptions::ID = 0;
#define PASS_FLAG "UniformAssumptions"
#define PASS_DESCRIPTION "Detect non-uniform usage of textures and samplers and check if they may be assumed uniform."
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
    IGC_INITIALIZE_PASS_BEGIN(UniformAssumptions, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
        IGC_INITIALIZE_PASS_DEPENDENCY(WIAnalysis)
        IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
        IGC_INITIALIZE_PASS_END(UniformAssumptions, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

        UniformAssumptions::UniformAssumptions() : llvm::FunctionPass(ID)
    {
        initializeUniformAssumptionsPass(*PassRegistry::getPassRegistry());
    }

    bool UniformAssumptions::runOnFunction(Function& F)
    {
        m_pCodeGenContext = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
        m_WIAnalysis = &getAnalysis<WIAnalysis>();
        if (m_WIAnalysis == nullptr || m_pCodeGenContext == nullptr)
        {
            IGC_ASSERT(0);
            return false;
        }

        // Propagate assumptions backwards (if safe) to increase the coverage.
        HoistAssumptions(F);
        // Try optimizing non-uniform resource accesses to uniform (to prevent from adding ResourceLoops)
        OptimizeResourceAccesses(F);

        return m_changed;
    }

    void UniformAssumptions::visitCallInst(CallInst& CI)
    {
        if (m_collectingAssumptions)
        {
            if (llvm::GenIntrinsicInst * pIntr = llvm::dyn_cast<llvm::GenIntrinsicInst>(&CI))
            {
                if (pIntr->getIntrinsicID() == GenISAIntrinsic::GenISA_is_uniform)
                {
                    m_assumptions.push_back(pIntr);
                }
            }
            return;
        }

        if (llvm::GenIntrinsicInst * pIntr = llvm::dyn_cast<llvm::GenIntrinsicInst>(&CI))
        {
            // Figure out the intrinsic operands for texture & sampler
            llvm::Value* pTextureValue = nullptr;
            llvm::Value*pSamplerValue = nullptr;
            IGC::getTextureAndSamplerOperands(
                pIntr,
                pTextureValue,
                pSamplerValue);

            if (pTextureValue && pTextureValue->getType()->isPointerTy() &&
                !m_WIAnalysis->isUniform(pTextureValue))
            {
                // Check assumptions for texture:
                if (IsAssumedUniform(pTextureValue))
                {
                    MakeUniformResourceOperand(pTextureValue, &CI);
                }
            }

            if (pSamplerValue && pSamplerValue->getType()->isPointerTy() &&
                !m_WIAnalysis->isUniform(pSamplerValue))
            {
                // Check assumptions for sampler:
                if (IsAssumedUniform(pSamplerValue))
                {
                    MakeUniformResourceOperand(pSamplerValue, &CI);
                }
            }
        }
    }

    void UniformAssumptions::OptimizeResourceAccesses(llvm::Function& F)
    {
        IGC_ASSERT(m_collectingAssumptions == false);
        visit(F);
    }

    bool UniformAssumptions::IsAssumedUniform(llvm::Value* V, int recursionLevel) const
    {
        if (recursionLevel >= s_cMaxRecursion)
        {
            // Limit level of recursion.
            return false;
        }

        // Check if marked as uniform by WIAnalysis:
        if (m_WIAnalysis->isUniform(V))
        {
            return true;
        }

        // Check if value can be assumed uniform:
        for (auto UI = V->use_begin(), UE = V->use_end(); UI != UE; ++UI)
        {
            if (llvm::GenIntrinsicInst * pIntr = llvm::dyn_cast<llvm::GenIntrinsicInst>(UI->getUser()))
            {
                if (pIntr->getIntrinsicID() == GenISAIntrinsic::GenISA_is_uniform)
                {
                    return true;
                }
            }
        }

        // This is very conservative simplification of rules implemented in Uniform Analysis (WIANalysis pass).
        // Uniform Analysis additionally tries to prove some instructions to be uniform even if they do not
        // have all uniform operands. Here, we simply assume that:
        // - CallInst, AllocaInst, VAArgInst and PHINode that were marked by UA as non-uniform remain non-uniform.
        // - Other instructions marked by UA as non-uniform can only be assumed uniform if all of its operands can be assumed uniform.
        // In the future, it would be better to reuse Uniform Analysis logic here - it would be more reliable
        // and would also optimize more cases.
        if (llvm::isa<llvm::CallInst>(V) ||
            llvm::isa<llvm::AllocaInst>(V) ||
            llvm::isa<llvm::VAArgInst>(V) ||
            llvm::isa<llvm::PHINode>(V))
        {
            return false;
        }

        // Check if all operands are uniform:
        if (auto inst = dyn_cast<Instruction>(V))
        {
            for (auto& op : inst->operands())
            {
                if (!IsAssumedUniform(op, recursionLevel + 1))
                {
                    return false;
                }
            }
            return true;
        }

        return false;
    }

    void UniformAssumptions::MakeUniformResourceOperand(llvm::Value* V, llvm::CallInst* CI)
    {
        // Add readFirstLane to make value uniform.
        LLVM3DBuilder<> builder(*m_pCodeGenContext->getLLVMContext(), m_pCodeGenContext->platform.getPlatformInfo());
        builder.SetInsertPoint(CI);
        Value* uniformTexture = builder.readFirstLane(V);
        for (unsigned i = 0; i < CI->getNumOperands(); i++)
        {
            if (CI->getOperand(i) == V)
            {
                CI->setOperand(i, uniformTexture);
            }
        }

        m_changed = true;
    }

    void UniformAssumptions::CollectAssumptions(llvm::Function& F)
    {
        m_assumptions.clear();
        m_collectingAssumptions = true;
        visit(F);
        m_collectingAssumptions = false;
    }

    void UniformAssumptions::HoistAssumptions(llvm::Function& F)
    {
        CollectAssumptions(F);

        // Try to propagate assumptions up.
        for (auto A : m_assumptions)
        {
            auto src = A->getOperand(0);
            bool check_further = true;
            while (check_further)
            {
                check_further = false;
                if (auto castInst = dyn_cast<CastInst>(src))
                {
                    // if ext(A) is uniform then A is uniform too.
                    if (castInst->getOpcode() == Instruction::ZExt ||
                        castInst->getOpcode() == Instruction::SExt)
                    {
                        Instruction* newAssumption = A->clone();
                        src = castInst->getOperand(0);
                        newAssumption->setOperand(0, src);
                        if (auto srcInst = dyn_cast<Instruction>(src))
                        {
                            if (isa<PHINode>(srcInst))
                            {
                                newAssumption->insertBefore(
                                    srcInst->getParent()->getFirstNonPHI());
                            }
                            else
                            {
                                newAssumption->insertAfter(srcInst);
                            }
                        }
                        else
                        {
                            newAssumption->insertBefore(
                                F.getEntryBlock().getFirstNonPHI());
                        }
                        check_further = true;
                    }
                }
            }
        }
    }

} // namespace IGC
