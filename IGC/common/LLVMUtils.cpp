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

#include "Compiler/CodeGenPublic.h"
#include "Compiler/CISACodeGen/PassTimer.hpp"
#include "Compiler/CISACodeGen/TimeStatsCounter.h"
#include "common/Stats.hpp"
#include "common/debug/Dump.hpp"
#include "common/shaderOverride.hpp"
#include "common/IntrinsicAnnotator.hpp"
#include "common/LLVMUtils.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include "common/LLVMWarningsPop.hpp"

using namespace IGC;
using namespace IGC::Debug;
using namespace llvm;

bool getPassToggles(std::bitset<1024>& toggles)
{
    const char* passToggles = IGC_GET_REGKEYSTRING(DisablePassToggles);
    if (passToggles != nullptr && strlen(passToggles) > 0)
    {
        std::string szBin;
        std::string szHexLL;
        unsigned int len = 0;
        unsigned long long x = 0;
        std::string szHex = passToggles;
        for (size_t i = 0; i < szHex.size(); i += 16)
        {
            szHexLL = szHex.substr(i, 16);
            len = szHexLL.size() * 4;
            x = std::stoull(szHexLL, nullptr, 16);
            szBin += std::bitset<64>(x).to_string().substr(64 - len, len);
        }

        toggles = std::bitset<1024>(szBin);
        return true;
    }

    return false;
}

void IGCPassManager::add(Pass *P)
{
    //check only once
    static bool checkedToggles = false;
    static bool hasToggles = false;
    static std::bitset<1024> toggles;
    if (!checkedToggles)
    {
        checkedToggles = true;
        hasToggles = getPassToggles(toggles);
    }
    if (hasToggles && m_pContext->m_numPasses < 1024 && toggles[m_pContext->m_numPasses])
    {
        errs() << "Skipping pass: '" << P->getPassName() << "\n";
        m_pContext->m_numPasses++;
        return;
    }

    if (IGC_IS_FLAG_ENABLED(ShaderDisableOptPassesAfter)
            && m_pContext->m_numPasses > IGC_GET_FLAG_VALUE(ShaderDisableOptPassesAfter)
            && m_name == "OPT") {
        errs() << "Skipping optimization pass: '" << P->getPassName()
               << "' (threshold: " << IGC_GET_FLAG_VALUE(ShaderDisableOptPassesAfter) << ").\n";
        return;
    }

    if (isPrintBefore(P))
    {
        addPrintPass(P, true);
    }

    if (IGC_REGKEY_OR_FLAG_ENABLED(DumpTimeStatsPerPass, TIME_STATS_PER_PASS))
    {
        PassManager::add(createTimeStatsIGCPass(m_pContext, m_name + '_' + std::string(P->getPassName()), STATS_COUNTER_START));
    }

    PassManager::add(P);

    if (IGC_REGKEY_OR_FLAG_ENABLED(DumpTimeStatsPerPass, TIME_STATS_PER_PASS))
    {
        PassManager::add(createTimeStatsIGCPass(m_pContext, m_name + '_' + std::string(P->getPassName()), STATS_COUNTER_END));
    }

    if (isPrintAfter(P))
    {
        addPrintPass(P, false);
    }
}

// List: a comma/semicolon-separated list of pass names.
//    N: a pass name
// return true if N is in List.
bool IGCPassManager::isInList(const StringRef& N, const StringRef& List) const
{
    StringRef Separators(",;");
    size_t startPos = 0;
    while (startPos != StringRef::npos)
    {
        size_t endPos = List.find_first_of(Separators, startPos);
        size_t len = (endPos != StringRef::npos ? endPos - startPos : endPos);
        StringRef Name = List.substr(startPos, len);
        if (Name.equals_lower(N))
        {
            return true;
        }
        startPos = (endPos != StringRef::npos ? endPos + 1 : StringRef::npos);
    }
    return false;
}

bool IGCPassManager::isPrintBefore(Pass* P)
{
    if (IGC_IS_FLAG_ENABLED(PrintBefore))
    {
        // PrintBefore=N0,N1,N2  : comma-separate list of pass names
        //                         or pass command args registered in passInfo.
        StringRef  passNameList(IGC_GET_REGKEYSTRING(PrintBefore));
        StringRef PN = P->getPassName();
        if (passNameList.equals_lower("all") || isInList(PN, passNameList))
            return true;

        // further check passInfo
        if (const PassInfo* PI = Pass::lookupPassInfo(P->getPassID()))
        {
            return isInList(PI->getPassArgument(), passNameList);
        }
    }
    return false;
}

bool IGCPassManager::isPrintAfter(Pass* P)
{
    if (IGC_IS_FLAG_ENABLED(ShaderDumpEnableAll))
    {
        return true;
    }
    if (IGC_IS_FLAG_ENABLED(PrintAfter))
    {
        // PrintAfter=N0,N1,N2  : comma-separate list of pass names or
        //                         or pass command args registered in passInfo.
        StringRef  passNameList(IGC_GET_REGKEYSTRING(PrintAfter));
        StringRef PN = P->getPassName();
        if (passNameList.equals_lower("all") || isInList(PN, passNameList))
            return true;

        // further check passInfo
        if (const PassInfo* PI = Pass::lookupPassInfo(P->getPassID()))
        {
            return isInList(PI->getPassArgument(), passNameList);
        }
    }
    return false;
}

void IGCPassManager::addPrintPass(Pass* P, bool isBefore)
{
    std::string passName =
        m_name + (isBefore ? "_before_" : "_after_") + std::string(P->getPassName());
    auto name =
        IGC::Debug::DumpName(IGC::Debug::GetShaderOutputName())
        .Type(m_pContext->type)
        .Hash(m_pContext->hash)
        .Pass(passName, m_pContext->m_numPasses++)
        .StagedInfo(m_pContext)
        .Extension("ll");
    // The dump object needs to be on the Heap because it owns the stream, and the stream
    // is taken by reference into the printer pass. If the Dump object had been on the
    // stack, then that reference would go bad as soon as we exit this scope, and then
    // the printer pass would access an invalid pointer later on when we call PassManager::run()
    m_irDumps.emplace_front(name, IGC::Debug::DumpType::PASS_IR_TEXT);
    PassManager::add(P->createPrinterPass(m_irDumps.front().stream(), ""));
    PassManager::add(new FlushDumpPass(m_irDumps.front()));
}

void DumpLLVMIR(IGC::CodeGenContext* pContext, const char* dumpName)
{
    SetCurrentDebugHash(pContext->hash.asmHash);

    if (IGC_IS_FLAG_ENABLED(DumpLLVMIR))
    {
        pContext->getMetaDataUtils()->save(*pContext->getLLVMContext());
        serialize(*(pContext->getModuleMetaData()), pContext->getModule());
        using namespace IGC::Debug;
        auto name =
            DumpName(IGC::Debug::GetShaderOutputName())
            .Hash(pContext->hash)
            .Type(pContext->type)
            .Pass(dumpName)
            .Retry(pContext->m_retryManager.GetRetryId())
            .Extension("ll");
        auto new_annotator = IntrinsicAnnotator();
        auto annotator = (pContext->annotater != nullptr) ? pContext->annotater : &new_annotator;
        DumpLLVMIRText(
            pContext->getModule(),
            Dump(name, DumpType::PASS_IR_TEXT),
            annotator);
    }
    if (IGC_IS_FLAG_ENABLED(ShaderOverride))
    {
        auto name =
            DumpName(IGC::Debug::GetShaderOutputName())
            .Hash(pContext->hash)
            .Type(pContext->type)
            .Pass(dumpName)
            .Extension("ll");
        SMDiagnostic Err;
        std::string fileName = name.overridePath();
        FILE* fp = fopen(fileName.c_str(), "r");
        if (fp != nullptr)
        {
            fclose(fp);
            errs() << "Override shader: " << fileName << "\n";
            Module* mod = parseIRFile(fileName, Err, *pContext->getLLVMContext()).release();
            if (mod)
            {
                pContext->deleteModule();
                pContext->setModule(mod);
                deserialize(*(pContext->getModuleMetaData()), mod);
                appendToShaderOverrideLogFile(fileName, "OVERRIDEN: ");
            }
            else
            {
                std::stringstream ss;
                ss << "Parse IR failed.\n";
                ss << Err.getLineNo() << ": "
                    << Err.getLineContents().str() << "\n"
                    << Err.getMessage().str() << "\n";

                std::string str = ss.str();
                errs() << str;
                appendToShaderOverrideLogFile(fileName, str.c_str());
            }
        }
    }
}
