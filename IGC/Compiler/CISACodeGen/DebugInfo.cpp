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

#include "DebugInfo.hpp"
#include "GenCodeGenModule.h"
#include "common/Types.hpp"
#include "Probe/Assertion.h"
#include "CLElfLib/ElfReader.h"

#include "DebugInfo/ScalarVISAModule.h"

using namespace llvm;
using namespace IGC;
using namespace IGC::IGCMD;
using namespace std;
// ElfReader related typedefs
using namespace CLElfLib;

void gatherDISubprogramNodes(llvm::Module& M, std::unordered_map<llvm::Function*, std::vector<llvm::DISubprogram*>>& DISubprogramNodes);

char DebugInfoPass::ID = 0;
char CatchAllLineNumber::ID = 0;

DebugInfoPass::DebugInfoPass(CShaderProgram::KernelShaderMap& k) :
    ModulePass(ID),
    kernels(k)
{
    initializeMetaDataUtilsWrapperPass(*PassRegistry::getPassRegistry());
}

DebugInfoPass::~DebugInfoPass()
{
}

bool DebugInfoPass::doInitialization(llvm::Module& M)
{
    return true;
}

bool DebugInfoPass::doFinalization(llvm::Module& M)
{
    return true;
}

bool DebugInfoPass::runOnModule(llvm::Module& M)
{
    std::vector<CShader*> units;
    auto moduleMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
    bool isOneStepElf = false;

    auto isCandidate = [](CShaderProgram* shaderProgram, SIMDMode m, ShaderDispatchMode mode = ShaderDispatchMode::NOT_APPLICABLE)
    {
        auto currShader = shaderProgram->GetShader(m, mode);
        if (!currShader || !currShader->GetDebugInfoData())
            return (CShader*)nullptr;

        if (currShader->ProgramOutput()->m_programSize == 0)
            return (CShader*)nullptr;

        return currShader;
    };

    for (auto& k : kernels)
    {
        auto shaderProgram = k.second;
        auto simd8 = isCandidate(shaderProgram, SIMDMode::SIMD8);
        auto simd16 = isCandidate(shaderProgram, SIMDMode::SIMD16);
        auto simd32 = isCandidate(shaderProgram, SIMDMode::SIMD32);

        if (simd8) units.push_back(simd8);
        if (simd16) units.push_back(simd16);
        if (simd32) units.push_back(simd32);
    }

    std::unordered_map<llvm::Function*, std::vector<llvm::DISubprogram*>> DISubprogramNodes;
    gatherDISubprogramNodes(M, DISubprogramNodes);

    auto getSPDiesCollection = [&DISubprogramNodes](std::vector<llvm::Function*>& functions)
    {
        // Function argument is list of all functions for elf.
        // Each function may require emission of one or more DISubprogram nodes.
        // Return value should be a stable vector of collection of all DISubprogram nodes
        // but without duplicates.
        std::vector<llvm::DISubprogram*> retUniqueFuncVec;
        std::unordered_set<llvm::DISubprogram*> uniqueDISP;
        for (auto& f : functions)
        {
            // iterate over all DISubprogram nodes references by llvm::Function f
            auto& DISPNodesForF = DISubprogramNodes[f];
            for (auto& SP : DISPNodesForF)
            {
                if (uniqueDISP.find(SP) == uniqueDISP.end())
                {
                    retUniqueFuncVec.push_back(SP);
                    uniqueDISP.insert(SP);
                }
            }
        }
        // This vector contains DISubprogram node pointers for which DIEs will be emitted elf
        // for current kernel.
        //
        // Input to IGC may have 100s of kernels. When emitting to dwarf, we can emit subprogram
        // DIEs defined in current kernel (+ it's recursive callees) as well as declarations of
        // other kernels and functions in input. These declarations quickly add up and cause
        // bloat of elf size without adding much benefit. This function is responsible to filter
        // and return only those DISubprogram nodes for which we want DIE emitted to elf. This
        // only includes DIEs for subprograms ever referenced in this kernel (+ it's recursive
        // callees). We skip emitting declaration DIEs for which no code is emitted in current
        // kernel.
        return retUniqueFuncVec;
    };

    for (auto& currShader : units)
    {
        // Look for the right CShaderProgram instance
        m_currShader = currShader;

        MetaDataUtils* pMdUtils = m_currShader->GetMetaDataUtils();
        if (!isEntryFunc(pMdUtils, m_currShader->entry))
            continue;

        bool isCloned = false;
        if (DebugInfoData::hasDebugInfo(m_currShader))
        {
            auto fIT = moduleMD->FuncMD.find(m_currShader->entry);
            if (fIT != moduleMD->FuncMD.end() &&
                (*fIT).second.isCloned)
            {
                isCloned = true;
            }
        }

        bool finalize = false;
        unsigned int size = m_currShader->GetDebugInfoData()->m_VISAModules.size();
        m_pDebugEmitter = m_currShader->GetDebugInfoData()->m_pDebugEmitter;
        std::vector<std::pair<unsigned int, std::pair<llvm::Function*, IGC::VISAModule*>>> sortedVISAModules;

        // Sort modules in order of their placement in binary
        DbgDecoder decodedDbg(m_currShader->ProgramOutput()->m_debugDataGenISA);
        auto getGenOff = [&decodedDbg](std::vector<std::pair<unsigned int, unsigned int>>& data, unsigned int VISAIndex)
        {
            unsigned retval = 0;
            for (auto& item : data)
            {
                if (item.first == VISAIndex)
                {
                    retval = item.second;
                }
            }
            return retval;
        };

        auto getLastGenOff = [this, &decodedDbg, &getGenOff](IGC::VISAModule* v)
        {
            unsigned int genOff = 0;
            // Detect last instructions of kernel. This information is absent in
            // dbg info. So detect is as first instruction of first subroutine - 1.
            // reloc_index, first sub inst's VISA id
            std::unordered_map<uint32_t, unsigned int> firstSubVISAIndex;

            for (auto& item : decodedDbg.compiledObjs)
            {
                firstSubVISAIndex[item.relocOffset] = item.CISAIndexMap.back().first;
                for (auto& sub : item.subs)
                {
                    auto subStartVISAIndex = sub.startVISAIndex;
                    if (firstSubVISAIndex[item.relocOffset] > subStartVISAIndex)
                        firstSubVISAIndex[item.relocOffset] = subStartVISAIndex - 1;
                }
            }

            for (auto& item : decodedDbg.compiledObjs)
            {
                auto& name = item.kernelName;
                auto firstInst = (v->GetInstInfoMap()->begin())->first;
                auto funcName = firstInst->getParent()->getParent()->getName();
                if (item.subs.size() == 0 && funcName.compare(name) == 0)
                {
                    genOff = item.CISAIndexMap.back().second;
                }
                else
                {
                    if (funcName.compare(name) == 0)
                    {
                        genOff = getGenOff(item.CISAIndexMap, firstSubVISAIndex[item.relocOffset]);
                        break;
                    }
                    for (auto& sub : item.subs)
                    {
                        auto& subName = sub.name;
                        if (funcName.compare(subName) == 0)
                        {
                            genOff = getGenOff(item.CISAIndexMap, sub.endVISAIndex);
                            break;
                        }
                    }
                }

                if (genOff)
                    break;
            }

            return genOff;
        };

        auto setType = [&decodedDbg](VISAModule* v)
        {
            auto firstInst = (v->GetInstInfoMap()->begin())->first;
            auto funcName = firstInst->getParent()->getParent()->getName();

            for (auto& item : decodedDbg.compiledObjs)
            {
                auto& name = item.kernelName;
                if (funcName.compare(name) == 0)
                {
                    if (item.relocOffset == 0)
                        v->SetType(VISAModule::ObjectType::KERNEL);
                    else
                        v->SetType(VISAModule::ObjectType::STACKCALL_FUNC);
                    return;
                }
                for (auto& sub : item.subs)
                {
                    auto& subName = sub.name;
                    if (funcName.compare(subName) == 0)
                    {
                        v->SetType(VISAModule::ObjectType::SUBROUTINE);
                        return;
                    }
                }
            }
        };

        for (auto& m : m_currShader->GetDebugInfoData()->m_VISAModules)
        {
            setType(m.second);
            auto lastVISAId = getLastGenOff(m.second);
            sortedVISAModules.push_back(std::make_pair(lastVISAId, std::make_pair(m.first, m.second)));
        }

        std::sort(sortedVISAModules.begin(), sortedVISAModules.end(),
            [](std::pair<unsigned int, std::pair<llvm::Function*, IGC::VISAModule*>>& p1,
                std::pair<unsigned int, std::pair<llvm::Function*, IGC::VISAModule*>>& p2)
        {
            return p1.first < p2.first;
        });

        for (auto& m : sortedVISAModules)
        {
            m_pDebugEmitter->registerVISA(m.second.second);
        }

        std::vector<llvm::Function*> functions;
        std::for_each(sortedVISAModules.begin(), sortedVISAModules.end(),
            [&functions](auto& item) { functions.push_back(item.second.first); });
        auto SPDiesToBuild = getSPDiesCollection(functions);
        for (auto& m : sortedVISAModules)
        {
            isOneStepElf |= m.second.second->isDirectElfInput;
            m_pDebugEmitter->setCurrentVISA(m.second.second);

            if (--size == 0)
                finalize = true;

            EmitDebugInfo(finalize, &decodedDbg, SPDiesToBuild);
        }

        // set VISA dbg info to nullptr to indicate 1-step debug is enabled
        if (isOneStepElf)
        {
            currShader->ProgramOutput()->m_debugDataGenISASize = 0;
            currShader->ProgramOutput()->m_debugDataGenISA = nullptr;
        }

        if (finalize)
        {
            IDebugEmitter::Release(m_pDebugEmitter);

            // destroy VISA builder
            auto encoder = &(m_currShader->GetEncoder());
            encoder->DestroyVISABuilder();
        }
    }

    return false;
}

void DebugInfoPass::EmitDebugInfo(bool finalize, DbgDecoder* decodedDbg,
                                  const std::vector<llvm::DISubprogram*>& DISubprogramNodes)
{
    std::vector<char> buffer;

    IF_DEBUG_INFO_IF(m_pDebugEmitter, buffer = m_pDebugEmitter->Finalize(finalize, decodedDbg, DISubprogramNodes);)

    if (!buffer.empty())
    {
        if (IGC_IS_FLAG_ENABLED(ShaderDumpEnable) || IGC_IS_FLAG_ENABLED(ElfDumpEnable))
        {
            std::string debugFileNameStr = IGC::Debug::GetDumpName(m_currShader, "elf");

            // Try to create the directory for the file (it might not already exist).
            if (iSTD::ParentDirectoryCreate(debugFileNameStr.c_str()) == 0)
            {
                void* dbgFile = iSTD::FileOpen(debugFileNameStr.c_str(), "wb+");
                if (dbgFile != nullptr)
                {
                    iSTD::FileWrite(buffer.data(), buffer.size(), 1, dbgFile);
                    iSTD::FileClose(dbgFile);
                }
            }
        }
    }

    void* dbgInfo = IGC::aligned_malloc(buffer.size(), sizeof(void*));
    if (dbgInfo)
        memcpy_s(dbgInfo, buffer.size(), buffer.data(), buffer.size());

    SProgramOutput* pOutput = m_currShader->ProgramOutput();
    pOutput->m_debugDataVISA = dbgInfo;
    pOutput->m_debugDataVISASize = dbgInfo ? buffer.size() : 0;
}

// Mark privateBase aka ImplicitArg::PRIVATE_BASE as Output for debugging
void DebugInfoData::markOutputPrivateBase(CShader* pShader, IDebugEmitter* pDebugEmitter)
{
    IGC_ASSERT_MESSAGE(IGC_IS_FLAG_ENABLED(UseOffsetInLocation), "UseOffsetInLocation not enabled");

    if (pShader->GetContext()->getModuleMetaData()->compOpt.OptDisable)
    {
        CVariable* pVar = pShader->GetPrivateBase();
        if (pVar)
        {
            // cache privateBase as it may be destroyed if subroutine
            // is emitted.
            pDebugEmitter->getCurrentVISA()->setPrivateBase(pVar);
            pShader->GetEncoder().GetVISAKernel()->AddAttributeToVar(pVar->visaGenVariable[0], "Output", 0, nullptr);
            if (pShader->m_dispatchSize == SIMDMode::SIMD32 && pVar->visaGenVariable[1])
            {
                IGC_ASSERT_MESSAGE(false, "Private base expected to be a scalar!");  // Should never happen
                pShader->GetEncoder().GetVISAKernel()->AddAttributeToVar(pVar->visaGenVariable[1], "Output", 0, nullptr);
            }
        }
    }
}

void DebugInfoData::markOutputVar(CShader* pShader, IDebugEmitter* pDebugEmitter, llvm::Instruction* pInst, const char* pMetaDataName)
{
    Value* pValue = dyn_cast<Value>(pInst);

    IGC_ASSERT_MESSAGE(IGC_IS_FLAG_ENABLED(UseOffsetInLocation), "UseOffsetInLocation not enabled");
    IGC_ASSERT_MESSAGE(pInst, "Missing instruction");

    CVariable* pVar = pShader->GetSymbol(pValue);
    if (pVar->GetVarType() == EVARTYPE_GENERAL)
    {
        // If UseOffsetInLocation is enabled, we want to attach "Output" attribute to:
        // 1. Per thread offset only, and/or
        // 2. Compute thread and global identification variables.
        // So that finalizer can extend their liveness to end of the program.
        // This will help debugger examine their values anywhere in the code till they
        // are in scope. However, emit "Output" attribute when -g and -cl-opt-disable
        // are both passed -g by itself shouldnt alter generated code.
        if (pShader->GetContext()->getModuleMetaData()->compOpt.OptDisable)
        {
            // If "Output" attribute is emitted for perThreadOffset variable(s)
            // then debug info emission is preserved for this:
            // privateBaseMem + perThreadOffset + (simdSize*offImm + simd_lane*sizeof(elem))
            if (Instruction* pPTOorImplicitGIDInst = dyn_cast<Instruction>(pValue))
            {
                MDNode* pPTOorImplicitGIDInstMD = pPTOorImplicitGIDInst->getMetadata(pMetaDataName); // "perThreadOffset" or "implicitGlobalID"
                if (pPTOorImplicitGIDInstMD)
                {
                    pShader->GetEncoder().GetVISAKernel()->AddAttributeToVar(pVar->visaGenVariable[0], "Output", 0, nullptr);
                    if (pShader->m_dispatchSize == SIMDMode::SIMD32 && pVar->visaGenVariable[1])
                    {
                        pShader->GetEncoder().GetVISAKernel()->AddAttributeToVar(pVar->visaGenVariable[1], "Output", 0, nullptr);
                    }
                }
            }
        }
    }
    else
    {
        // Unexpected return empty location!
        IGC_ASSERT_MESSAGE(false, "No debug info value!");
    }
}

void DebugInfoData::markOutput(llvm::Function& F, CShader* pShader, IDebugEmitter* pDebugEmitter)
{
    IGC_ASSERT_MESSAGE(pDebugEmitter, "Missing debug emitter");
    VISAModule* visaModule = pDebugEmitter->getCurrentVISA();
    IGC_ASSERT_MESSAGE(visaModule, "Missing visa module");

    for (auto& bb : F)
    {
        for (auto& pInst : bb)
        {
            if (MDNode* perThreadOffsetMD = pInst.getMetadata("perThreadOffset"))
            {
                // Per Thread Offset non-debug instruction must have 'Output' attribute
                // added in the function to be called.
                markOutputVar(pShader, pDebugEmitter, &pInst, "perThreadOffset");
                if (F.getCallingConv() == CallingConv::SPIR_KERNEL)
                {
                    markOutputPrivateBase(pShader, pDebugEmitter); // Mark privateBase aka ImplicitArg::PRIVATE_BASE as Output for debugging
                }
                else
                {
                    // TODO: Apply privateBase of kernel to SPIR_FUNC if its a subroutine
                }
                ScalarVisaModule* scVISAModule = (ScalarVisaModule*)visaModule;
                IGC_ASSERT_MESSAGE(scVISAModule->getPerThreadOffset()==nullptr, "setPerThreadOffset was set earlier");
                scVISAModule->setPerThreadOffset(&pInst);
                if (((OpenCLProgramContext*)(pShader->GetContext()))->m_InternalOptions.KernelDebugEnable == false)
                {
                    return;
                }
            }
        }
    }

    if (((OpenCLProgramContext*)(pShader->GetContext()))->m_InternalOptions.KernelDebugEnable)
    {
        // Compute thread and group identification instructions will be marked here
        // regardless of stack calls detection in this shader, so not only when per thread offset
        // as well as a private base have been marked as Output earlier in this function.
        // When stack calls are in use then only these group ID instructions are marked as Output.
        for (auto& bb : F)
        {
            for (auto& pInst : bb)
            {
                if (MDNode* implicitGlobalIDMD = pInst.getMetadata("implicitGlobalID"))
                {
                    // Compute thread and group identification instructions must have
                    // 'Output' attribute added in the function to be called.
                    markOutputVar(pShader, pDebugEmitter, &pInst, "implicitGlobalID");
                }
            }
        }
    }
}

void DebugInfoData::markOutput(llvm::Function& F, CShader* m_currShader)
{
    for (auto& bb : F)
    {
        for (auto& pInst : bb)
        {
            markOutputVars(&pInst);
        }
    }
}

void DebugInfoData::markOutputVars(const llvm::Instruction* pInst)
{
    const Value* pVal = nullptr;
    if (const DbgDeclareInst * pDbgAddrInst = dyn_cast<DbgDeclareInst>(pInst))
    {
        pVal = pDbgAddrInst->getAddress();
    }
    else if (const DbgValueInst * pDbgValInst = dyn_cast<DbgValueInst>(pInst))
    {
        pVal = pDbgValInst->getValue();
    }
    else
    {
        return;
    }

    if (!pVal || isa<UndefValue>(pVal))
    {
        // No debug info value, return empty location!
        return;
    }

    if (dyn_cast<Constant>(pVal))
    {
        if (!isa<GlobalVariable>(pVal) && !isa<ConstantExpr>(pVal))
        {
            return;
        }
    }

    Value* pValue = const_cast<Value*>(pVal);
    if (isa<GlobalVariable>(pValue))
    {
        return;
    }

    if (!m_pShader->IsValueUsed(pValue)) {
        return;
    }

    CVariable* pVar = m_pShader->GetSymbol(pValue);
    if (pVar->GetVarType() == EVARTYPE_GENERAL)
    {
        // We want to attach "Output" attribute to all variables:
        // - if UseOffsetInLocation is disabled, or
        // - if UseOffsetInLocation is enabled but there is a stack call in use,
        // so that finalizer can extend their liveness to end of
        // the program. This will help debugger examine their
        // values anywhere in the code till they are in scope.
        if (m_outputVals.find(pVar) == m_outputVals.end())
        {
            if (m_pShader->GetContext()->getModuleMetaData()->compOpt.OptDisable)
            {
                // Emit "Output" attribute only when -g and -cl-opt-disable are both passed
                // -g by itself shouldnt alter generated code
                m_pShader->GetEncoder().GetVISAKernel()->AddAttributeToVar(pVar->visaGenVariable[0], "Output", 0, nullptr);
                if (m_pShader->m_dispatchSize == SIMDMode::SIMD32 && pVar->visaGenVariable[1])
                {
                    m_pShader->GetEncoder().GetVISAKernel()->AddAttributeToVar(pVar->visaGenVariable[1], "Output", 0, nullptr);
                }
                (void)m_outputVals.insert(pVar);
            }
        }
    }
}

void DebugInfoData::transferMappings(const llvm::Function& F)
{
    // Store llvm::Value->CVariable mappings from CShader.
    // CShader clears these mappings before compiling a new function.
    // Debug info is computed after all functions are compiled.
    // This instance stores mappings per llvm::Function so debug
    // info generation can emit variable locations correctly.
    auto& SymbolMapping = m_pShader->GetSymbolMapping();
    m_FunctionSymbols[&F] = SymbolMapping;
}

CVariable* DebugInfoData::getMapping(const llvm::Function& F, const llvm::Value* V)
{
    auto& Data = m_FunctionSymbols[&F];
    auto Iter = Data.find(V);
    if (Iter != Data.end())
        return (*Iter).second;
    return nullptr;
}

// Register pass to igc-opt
#define PASS_FLAG "igc-catch-all-linenum"
#define PASS_DESCRIPTION "CatchAllLineNumber pass"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(CatchAllLineNumber, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(CatchAllLineNumber, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

CatchAllLineNumber::CatchAllLineNumber() :
    FunctionPass(ID)
{
    initializeMetaDataUtilsWrapperPass(*PassRegistry::getPassRegistry());
}

CatchAllLineNumber::~CatchAllLineNumber()
{
}

bool CatchAllLineNumber::runOnFunction(llvm::Function& F)
{
    // Insert placeholder intrinsic instruction in each kernel.
    if (!F.getSubprogram() || F.isDeclaration())
        return false;

    if (F.getCallingConv() != llvm::CallingConv::SPIR_KERNEL)
        return false;

    llvm::IRBuilder<> Builder(F.getParent()->getContext());
    DIBuilder di(*F.getParent());
    Function* lineNumPlaceholder = GenISAIntrinsic::getDeclaration(F.getParent(), GenISAIntrinsic::ID::GenISA_CatchAllDebugLine);
    auto intCall = Builder.CreateCall(lineNumPlaceholder);

    auto line = F.getSubprogram()->getLine();
    auto scope = F.getSubprogram();

    auto dbg = DILocation::get(F.getParent()->getContext(), line, 0, scope);

    intCall->setDebugLoc(dbg);

    intCall->insertBefore(&*F.getEntryBlock().getFirstInsertionPt());

    return true;
}
