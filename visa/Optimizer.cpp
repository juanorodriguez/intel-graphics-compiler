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

#include "Optimizer.h"
#include "G4_Opcode.h"
#include "Timer.h"
#include "G4_Verifier.hpp"
#include "LVN.h"
#include "ifcvt.h"
#include "FlowGraph.h"
#include "SendFusion.h"
#include "Common_BinaryEncoding.h"
#include "DebugInfo.h"
#include "AccSubstitution.h"
#include "MergeScalar.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <tuple>
#include <vector>

using namespace vISA;

void Optimizer::LVN()
{
    // Run a simple LVN pass that replaces redundant
    // immediate loads in current BB. Also this pass
    // does not optimize operations like a
    // conventional VN pass because those require
    // more compile time, and are presumably already
    // done by FE generating VISA. This pass catches
    // redundancies that got introduced mainly by HW
    // conformity or due to VISA lowering.
    int numInstsRemoved = 0;
    Mem_Manager mem(1024);
    PointsToAnalysis p(kernel.Declares, kernel.fg.getNumBB());
    p.doPointsToAnalysis(kernel.fg);
    for (auto bb : kernel.fg)
    {
        ::LVN lvn(fg, bb, mem, *fg.builder, p);
        lvn.doLVN();

        numInstsRemoved += lvn.getNumInstsRemoved();

        numInstsRemoved += ::LVN::removeRedundantSamplerMovs(kernel, bb);
    }

    if (kernel.getOption(vISA_OptReport))
    {
        std::ofstream optreport;
        getOptReportStream(optreport, kernel.getOptions());
        optreport << "===== LVN =====" << std::endl;
        optreport << "Number of instructions removed: " << numInstsRemoved << std::endl << std::endl;
        closeOptReportStream(optreport);
    }
}

// helper functions

static int getDstSubReg(G4_DstRegRegion *dst)
{
    int dstSubReg;
    if (dst->getBase()->isPhyReg())
    {
        dstSubReg = dst->getSubRegOff();
    }
    else
    {
        dstSubReg = dst->getSubRegOff() +
            static_cast<G4_RegVar*>(dst->getBase())->getPhyRegOff();
    }

    return dstSubReg;
}

static int getSrcSubReg(G4_Operand *src)
{
    MUST_BE_TRUE(src->isSrcRegRegion(), "expect Src Reg Region");
    int srcSubReg;
    if (src->asSrcRegRegion()->getBase()->isPhyReg())
    {
        srcSubReg = src->asSrcRegRegion()->getSubRegOff();
    }
    else
    {
        srcSubReg = src->asSrcRegRegion()->getSubRegOff() +
            static_cast<G4_RegVar*>(src->asSrcRegRegion()->getBase())->getPhyRegOff();
    }
    return srcSubReg;
}

//
// determine if fall-through jump is needed
//
// also remove redundant jumps
//   if there is no predicate applied to a jump and its target is its fall though BB,
//   remove the jump instruction.

void Optimizer::insertFallThroughJump()
{

    fg.setPhysicalPredSucc();
    for (BB_LIST_ITER it = fg.begin(); it != fg.end();)
    {
        G4_BB* bb = *it;
        BB_LIST_ITER next = ++it;
        //
        // determine if the current bb needs a fall through jump
        // check if the fall-through bb follows the current bb
        //
        G4_BB* fb = bb->fallThroughBB();
        if (fb && (next == fg.end() || // bb is the last bb
            fb != (*next)))
        {
            // This is bogus in SIMD CF, as bad things happen when you randomly insert jumps
            // in the middle of SIMD CF
        }
        else if (next != fg.end())
        {
            // do not remove a jmpi if it's the target of an indirect jmp
            // this makes the code more readable
            if (!(*next)->empty() && (*next)->front()->isLabel() &&
                !bb->empty() && bb->back()->opcode() == G4_jmpi &&
                bb->back()->getPredicate() == NULL &&
                !fg.isIndirectJmpTarget(bb->back()))
                {
                    if ((*next)->front()->getSrc(0) == bb->back()->getSrc(0))
                    {
                        std::list<G4_INST*>::iterator it = bb->end();
                        it--;
                        bb->erase(it);
                    }
            }
        }
        it = next;
    }
}

void Optimizer::regAlloc()
{

    fg.prepareTraversal();

    //
    // assign registers
    //
    int status = ::regAlloc(builder, builder.phyregpool, kernel);
    if (status != VISA_SUCCESS)
    {
        RAFail = true;
    }
}

void Optimizer::adjustIndirectCallOffset()
{
    // the call code sequence done at Optimizer::expandIndirectCallWithRegTarget
    // is:
    //       add  r2.0  -IP   call_target
    //       add  r2.0  r2.0  -32
    //       call r1.0  r2.0
    // -32 is hardcoded. But SWSB could've inserted sync instructions between
    // call and add. So we need to re-adjust the offset

    for (auto bb : kernel.fg)
    {
        if (bb->empty())
            continue;

        // At this point G4_pseudo_fcall may be converted to G4_call
        if (bb->back()->isCall() || bb->back()->isFCall())
        {
            G4_INST* fcall = bb->back();
            if (fcall->getSrc(0)->isGreg() || fcall->getSrc(0)->isA0())
            {
                // for every indirect call, count # of instructions inserted
                // between call and the first add
                uint64_t sync_offset = 0;
                G4_INST* first_add = nullptr;
                INST_LIST::reverse_iterator it = bb->rbegin();
                // skip call itself
                ++it;
                for (; it != bb->rend(); ++it)
                {
                    G4_INST* inst = *it;
                    G4_opcode op = inst->opcode();
                    if (op == G4_sync_allrd || op == G4_sync_allwr)
                    {
                        inst->setNoCompacted();
                        sync_offset += 16;
                        continue;
                    }
                    else if (op == G4_sync_nop) {
                        inst->setCompacted();
                        sync_offset += 8;
                        continue;
                    }
                    else if (op == G4_add)
                    {
                        if (first_add == nullptr)
                        {
                            first_add = inst;
                            continue;
                        }
                        else
                        {
                            break;
                        }
                    }
                    // instructions between call and add could only be
                    // sync.nop, sync.allrd or sync.allwr
                    assert(0);
                }
                assert(first_add->getSrc(1)->isImm());
                int64_t adjust_off = first_add->getSrc(1)->asImm()->getInt() - sync_offset;
                first_add->setSrc(builder.createImm(adjust_off, Type_D), 1);
            }
        }
    }
}

void Optimizer::addSWSBInfo()
{
    if (!builder.hasSWSB())
    {
        return;
    }

    if (!builder.getOption(vISA_forceDebugSWSB))
    {
        SWSB swsb(kernel, mem);
        swsb.SWSBGenerator();
    }
    else
    {
        forceDebugSWSB(&kernel);
    }
    kernel.dumpDotFile("after.SWSB");

    if (builder.getOptions()->getuInt32Option(vISA_SWSBTokenBarrier) != 0)
    {
        singleInstStallSWSB(&kernel,
            builder.getOptions()->getuInt32Option(vISA_SWSBTokenBarrier), 0, true);
    }

    if (builder.getOptions()->getuInt32Option(vISA_SWSBInstStall) != 0)
    {
        singleInstStallSWSB(&kernel,
            builder.getOptions()->getuInt32Option(vISA_SWSBInstStall),
            builder.getOptions()->getuInt32Option(vISA_SWSBInstStallEnd), false);
    }

    if (kernel.hasIndirectCall() && !builder.supportCallaRegSrc())
    {
        adjustIndirectCallOffset();
    }
    return;
}

void Optimizer::countBankConflicts()
{
    std::list<G4_INST*> conflicts;
    unsigned int numLocals = 0, numGlobals = 0;

    for (auto curBB : kernel.fg)
    {
        for (INST_LIST_ITER inst_it = curBB->begin();
            inst_it != curBB->end();
            inst_it++)
        {
            G4_INST* curInst = (*inst_it);
            G4_Operand* src0 = curInst->getSrc(0);
            G4_Operand* src1 = curInst->getSrc(1);
            G4_Operand* src2 = curInst->getSrc(2);

            if (src0 == NULL || src1 == NULL || src2 == NULL)
                continue;

            if (!src0->isSrcRegRegion() || !src1->isSrcRegRegion() || !src2->isSrcRegRegion())
                continue;

            if (!src0->asSrcRegRegion()->getBase()->asRegVar()->getPhyReg()->isGreg() ||
                !src1->asSrcRegRegion()->getBase()->asRegVar()->getPhyReg()->isGreg() ||
                !src1->asSrcRegRegion()->getBase()->asRegVar()->getPhyReg()->isGreg())
                continue;

            // We have a 3 src instruction with each src operand a GRF register region
            unsigned int src0grf = 0, src1grf = 0, src2grf = 0;

            src0grf = src0->getBase()->asRegVar()->getPhyReg()->asGreg()->getRegNum() +
                src0->asSrcRegRegion()->getRegOff();
            src1grf = src1->getBase()->asRegVar()->getPhyReg()->asGreg()->getRegNum() +
                src1->asSrcRegRegion()->getRegOff();
            src2grf = src2->getBase()->asRegVar()->getPhyReg()->asGreg()->getRegNum() +
                src2->asSrcRegRegion()->getRegOff();

            bool isConflict = false;

            unsigned int src0partition = 0, src1partition = 0, src2partition = 0;

            if (src0grf < 64)
            {
                src0partition = 1;
                if (src0grf % 2 == 0)
                    src0partition = 0;
            }
            else
            {
                src0partition = 3;
                if (src0grf % 2 == 0)
                    src0partition = 2;
            }

            if (src1grf < 64)
            {
                src1partition = 1;
                if (src1grf % 2 == 0)
                    src1partition = 0;
            }
            else
            {
                src1partition = 3;
                if (src1grf % 2 == 0)
                    src1partition = 2;
            }

            if (src2grf < 64)
            {
                src2partition = 1;
                if (src2grf % 2 == 0)
                    src2partition = 0;
            }
            else
            {
                src2partition = 3;
                if (src2grf % 2 == 0)
                    src2partition = 2;
            }

            if (src0partition == src1partition && src1partition == src2partition)
                isConflict = true;

            if (curInst->getExecSize() == g4::SIMD16)
                isConflict = true;

            if (isConflict == true)
            {
                conflicts.push_back(curInst);
                numBankConflicts++;

                auto isGlobal = [](G4_Operand* opnd, FlowGraph& fg)
                    -> bool {
                    if (fg.globalOpndHT.isOpndGlobal(opnd))
                        return true;
                    return false;
                };

                if (!isGlobal(curInst->getSrc(0), kernel.fg) &&
                    curInst->getSrc(0)->getTopDcl()->getRegVar()->getPhyReg())
                    numLocals++;
                else
                    numGlobals++;

                if (!isGlobal(curInst->getSrc(1), kernel.fg) &&
                    curInst->getSrc(1)->getTopDcl()->getRegVar()->getPhyReg())
                    numLocals++;
                else
                    numGlobals++;

                if (!isGlobal(curInst->getSrc(2), kernel.fg) &&
                    curInst->getSrc(2)->getTopDcl()->getRegVar()->getPhyReg())
                    numLocals++;
                else
                    numGlobals++;
            }
        }
    }

    if (numBankConflicts > 0)
    {
        std::ofstream optreport;
        getOptReportStream(optreport, builder.getOptions());

        optreport << std::endl << std::endl;

        optreport << "===== Bank conflicts =====" << std::endl;
        optreport << "Found " << numBankConflicts << " conflicts (" << numLocals << " locals, " << numGlobals << " globals) in kernel: " << kernel.getName() << std::endl;
        for (std::list<G4_INST*>::iterator it = conflicts.begin();
            it != conflicts.end();
            it++)
        {
            G4_INST* i = (*it);
            i->emit(optreport);
            optreport << " // $" << i->getCISAOff() << ":#" << i->getLineNo() << std::endl;
        }

        optreport << std::endl << std::endl;

        closeOptReportStream(optreport);
    }
}

void Optimizer::insertDummyCompactInst()
{
    // Only for SKL+ and compaction is enabled.
    if (builder.getPlatform() < GENX_SKL || !builder.getOption(vISA_Compaction))
        return;

    // Insert mov (1) r0 r0 at the beginning of this kernel.
    G4_Declare *dcl = builder.getBuiltinR0();
    auto src = builder.createSrc(
        dcl->getRegVar(), 0, 0, builder.getRegionScalar(), Type_F);
    auto dst = builder.createDst(dcl->getRegVar(), 0, 0, 1, Type_F);
    G4_INST *movInst = builder.createMov(g4::SIMD1, dst, src, InstOpt_WriteEnable, false);

    auto bb = fg.getEntryBB();
    for (auto it = bb->begin(), ie = bb->end(); it != ie; ++it)
    {
        if ((*it)->opcode() != G4_label)
        {
            bb->insertBefore(it, movInst);
            return;
        }
    }

    // The entry block is empty or only contains a label.
    bb->push_back(movInst);
}

// Float and DP share same GRF cache.
// Integer and Math shader same GRF cache.
void Optimizer::insertDummyMad(G4_BB* bb, INST_LIST_ITER inst_it)
{
    //Dst
    auto nullDst1 = builder.createNullDst(Type_W);
    auto nullDst2 = builder.createNullDst(Type_F);

    const RegionDesc* region = builder.createRegionDesc(8, 8, 1);

    unsigned rsReg = builder.getOptions()->getuInt32Option(vISA_registerHWRSWA);
    //Src0
    auto src0Dcl_0 = builder.createHardwiredDeclare(1, Type_W, rsReg, 0);
    auto src0Dcl_1 = builder.createHardwiredDeclare(1, Type_F, rsReg, 0);
    G4_SrcRegRegion* src0Opnd_0 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_0, region);
    G4_SrcRegRegion* src0Opnd_1 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_1, region);

    G4_SrcRegRegion* src1Opnd_0 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_0, region);
    G4_SrcRegRegion* src1Opnd_1 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_1, region);

    G4_SrcRegRegion* src2Opnd_0 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_0, region);
    G4_SrcRegRegion* src2Opnd_1 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_1, region);

    auto madInst1 = builder.createInternalInst(
        nullptr, G4_mad, nullptr, g4::NOSAT, g4::SIMD8,
        nullDst1, src0Opnd_0, src1Opnd_0, src2Opnd_0,
        InstOpt_NoOpt);

    auto madInst2 = builder.createInternalInst(
        nullptr, G4_mad, nullptr, g4::NOSAT, g4::SIMD8,
        nullDst2, src0Opnd_1, src1Opnd_1, src2Opnd_1,
        InstOpt_NoOpt);

    bb->insertBefore(inst_it, madInst1);
    bb->insertBefore(inst_it, madInst2);

    G4_SrcRegRegion* src = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_1, region);
    G4_DstRegRegion* dst = kernel.fg.builder->Create_Dst_Opnd_From_Dcl(src0Dcl_1, 1);
    G4_INST* movInst = builder.createMov(g4::SIMD8, dst, src, InstOpt_NoOpt, false);

    bb->insertBefore(inst_it, movInst);
}

void Optimizer::insertDummyCsel(G4_BB* bb, INST_LIST_ITER inst_it, bool newBB)
{
    const RegionDesc* region = builder.createRegionDesc(4, 4, 1);
    unsigned rsReg = builder.getOptions()->getuInt32Option(vISA_registerHWRSWA);

    G4_Declare* dummyFlagDcl = builder.createTempFlag(1, "dmflag");
    dummyFlagDcl->getRegVar()->setPhyReg(builder.phyregpool.getFlagAreg(0), 0);
    auto dummyCondMod0 = builder.createCondMod(Mod_e, dummyFlagDcl->getRegVar(), 0);
    auto src0Dcl_0 = builder.createHardwiredDeclare(4, Type_W, rsReg, 0);
    G4_SrcRegRegion* src0Opnd_0 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_0, region);
    G4_SrcRegRegion* src1Opnd_0 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_0, region);
    G4_SrcRegRegion* src2Opnd_0 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_0, region);
    G4_DstRegRegion* dst0 = kernel.fg.builder->Create_Dst_Opnd_From_Dcl(src0Dcl_0, 1);
    auto cselInst0 = builder.createInternalInst(
        nullptr, G4_csel, dummyCondMod0, g4::NOSAT, g4::SIMD4,
        dst0, src0Opnd_0, src1Opnd_0, src2Opnd_0,
        InstOpt_WriteEnable);

    if (newBB)
    {
        bb->push_back(cselInst0);
    }
    else
    {
        bb->insertBefore(inst_it, cselInst0);
    }

    if (!builder.hasSingleALUPipe())
    {
        auto src0Dcl_1 = builder.createHardwiredDeclare(4, Type_F, rsReg, 4);
        G4_SrcRegRegion* src0Opnd_1 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_1, region);
        G4_SrcRegRegion* src1Opnd_1 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_1, region);
        G4_SrcRegRegion* src2Opnd_1 = kernel.fg.builder->Create_Src_Opnd_From_Dcl(src0Dcl_1, region);
        G4_DstRegRegion* dst1 = kernel.fg.builder->Create_Dst_Opnd_From_Dcl(src0Dcl_1, 1);
        auto dummyCondMod1 = builder.createCondMod(Mod_e, dummyFlagDcl->getRegVar(), 0);
        auto cselInst1 = builder.createInternalInst(
            nullptr, G4_csel, dummyCondMod1, g4::NOSAT, g4::SIMD4,
            dst1, src0Opnd_1, src1Opnd_1, src2Opnd_1,
            InstOpt_WriteEnable);
        if (newBB)
        {
            bb->push_back(cselInst1);
        }
        else
        {
            bb->insertBefore(inst_it, cselInst1);
        }
    }
}

void Optimizer::insertDummyMov(G4_BB *bb, INST_LIST_ITER inst_it, G4_Operand *opnd)
{
    G4_SrcRegRegion* src = builder.createSrc(
        opnd->getBase(),
        opnd->asSrcRegRegion()->getRegOff(),
        0,
        builder.createRegionDesc(8, 8, 1),
        Type_UD);
    G4_DstRegRegion* dst = builder.createDst(
        opnd->getBase(),
        opnd->asSrcRegRegion()->getRegOff(),
        0,
        1,
        Type_UD);
    G4_INST* movInst = builder.createMov(g4::SIMD8, dst, src, InstOpt_NoOpt, false);
    bb->insertBefore(inst_it, movInst);

    return;
}


void Optimizer::insertDummyMovForHWRSWA()
{
    if (!VISA_WA_CHECK(builder.getPWaTable(), Wa_16012061344) &&
        !VISA_WA_CHECK(builder.getPWaTable(), Wa_16012292205))
    {
        return;
    }
    bool hasNonUniformBranch = false;
    bool hasPredicatedSendOrIndirect = false;

    for (BB_LIST_ITER bb_it = kernel.fg.begin();
        bb_it != kernel.fg.end();
        bb_it++)
    {
        G4_BB* bb = (*bb_it);

        if (bb->empty())
        {
            continue;
        }

        INST_LIST_ITER curr_iter = bb->begin();
        while (curr_iter != bb->end())
        {
            G4_INST* inst = (*curr_iter);


            if (inst->getPredicate() &&
                inst->getDst() &&
                !inst->getDst()->isNullReg())
            {
                if (inst->isSend())
                {
                    insertDummyCsel(bb, curr_iter, false);
                    hasPredicatedSendOrIndirect = true;
                }
            }

            ++curr_iter;
        }

        bool newBB = false;
        G4_INST* inst = (bb->getInstList().back());
        if (inst->isRSWADivergentInst() && !inst->asCFInst()->isUniform())
        {
            bool previousElse = false;

            G4_BB* preBB = bb->getPhysicalPred();
            if (preBB && preBB->getInstList().size())
            {
                G4_INST* preBBLastInst = (preBB->getInstList().back());
                previousElse = (preBBLastInst->opcode() == G4_else);
            }

            INST_LIST_ITER iter = bb->end();
            iter--;
            if (iter != bb->begin() && !previousElse)
            {
                INST_LIST_ITER preIter = iter;
                preIter--;
                G4_INST* preInst = (*preIter);
                if (preInst->isLabel())
                {
                    bool hasJmpIPred = false;

                    for (BB_LIST_ITER biter = bb->Preds.begin(), E1 = bb->Preds.end(); biter != E1; ++biter)
                    {
                        G4_BB* predBB = (*biter);
                        G4_INST* predBBLastInst = NULL;
                        if (!predBB->empty())
                        {
                            predBBLastInst = predBB->getInstList().back();
                        }
                        if (predBBLastInst && predBBLastInst->opcode() == G4_jmpi)
                        {
                            hasJmpIPred = true;
                        }
                    }
                    G4_BB* wa_bb = hasJmpIPred ?
                        kernel.fg.createNewBBWithLabel("WA_", preInst->getLineNo(), preInst->getCISAOff()) :
                        kernel.fg.createNewBB();
                    kernel.fg.insert(bb_it, wa_bb);
                    G4_Label* newLabel = hasJmpIPred ? wa_bb->getLabel() : NULL;

                    //replace bb with wa_bb in the pred BB of bb.
                    for (BB_LIST_ITER biter = bb->Preds.begin(), E1 = bb->Preds.end(); biter != E1; ++biter)
                    {
                        G4_BB* predBB = (*biter);
                        G4_INST* predBBLastInst = NULL;
                        if (!predBB->empty())
                        {
                            predBBLastInst = predBB->getInstList().back();
                        }
                        if (predBBLastInst && predBBLastInst->opcode() == G4_jmpi)
                        {
                            assert(newLabel);
                            predBBLastInst->setSrc(newLabel, 0);
                        }

                        for (BB_LIST_ITER succiter = predBB->Succs.begin(), E2 = predBB->Succs.end(); succiter != E2; ++succiter)
                        {
                            if (*succiter == bb)
                            {
                                *succiter = wa_bb;
                            }
                        }
                        wa_bb->Preds.push_back(predBB);
                    }
                    wa_bb->Succs.push_back(bb);
                    bb->Preds.clear();
                    bb->Preds.push_back(wa_bb);
                    newBB = true;
                    bb = wa_bb;
                }
            }

            insertDummyCsel(bb, iter, newBB);
            hasNonUniformBranch = true;
        }
    }

}

void Optimizer::insertHashMovs()
{
    // As per request from IGC team, we want to conditionally insert
    // two mov instructions like following:
    //
    // send ... {EOT}
    // mov (16) null<1>:d        lo32 {NoMask}
    // mov (16) null<1>:d        hi32 {NoMask}
    //
    for (BB_LIST_ITER bb_it = kernel.fg.begin();
        bb_it != kernel.fg.end();
        bb_it++)
    {
        G4_BB* bb = (*bb_it);

        for (INST_LIST_ITER inst_it = bb->begin();
            inst_it != bb->end();
            inst_it++)
        {
            G4_INST* inst = (*inst_it);

            if (inst->isEOT())
            {
                // We have to insert new instructions after EOT.
                // Lexically, EOT could even be in the middle
                // of the program.
                auto insertHashMovInsts = [&](uint64_t hashVal)
                {
                    if (hashVal == 0)
                        return;

                    G4_INST* lo;
                    G4_INST* hi;
                    lo = kernel.fg.builder->createMov(
                        g4::SIMD16,
                        kernel.fg.builder->createNullDst(Type_UD),
                        kernel.fg.builder->createImm((unsigned int)(hashVal & 0xffffffff), Type_UD),
                        InstOpt_WriteEnable, false);

                    hi = kernel.fg.builder->createMov(
                        g4::SIMD16,
                        kernel.fg.builder->createNullDst(Type_UD),
                        kernel.fg.builder->createImm((unsigned int)((hashVal >> 32) & 0xffffffff), Type_UD),
                        InstOpt_WriteEnable, false);

                    bb->push_back(lo);
                    bb->push_back(hi);
                };
                uint64_t hashVal1 = builder.getOptions()->getuInt64Option(vISA_HashVal);
                uint64_t hashVal2 = builder.getOptions()->getuInt64Option(vISA_HashVal1);
                insertHashMovInsts(hashVal1);
                insertHashMovInsts(hashVal2);
                break;
            }
        }
    }
}

//
// Break a sample instruction
// send.smpl (16) dst src0 src1
// into
// (P1) send.smpl (16) dst src0 src1
// (~P1) send.smpl (16) dst src0 src1
// where P1 is 0x0F0F (i.e., the even sub-spans)
//
// P1 is initialized per BB before the first sample inst; we could make it per shader but I'm worried about flag spill
// this works for SIMD8 and SIMD32 shaders as well.
//
void Optimizer::cloneSampleInst()
{
    bool cloneSample = builder.getOption(vISA_cloneSampleInst);
    bool cloneEvaluateSample = builder.getOption(vISA_cloneEvaluateSampleInst);
    if (!cloneSample && !cloneEvaluateSample)
    {
        return;
    }

    bool isSIMD32 = kernel.getSimdSize() == 32;
    for (auto&& bb : kernel.fg)
    {
        auto tmpFlag = builder.createTempFlag(isSIMD32 ? 2 : 1);
        auto hasSample = false;
        for (auto I = bb->begin(), E = bb->end(); I != E;)
        {
            auto Next = std::next(I);
            auto inst = *I;
            if (inst->isSend() && inst->asSendInst()->getMsgDesc()->isSampler() && inst->getExecSize() >= builder.getNativeExecSize())
            {
                G4_InstSend* sendInst = inst->asSendInst();
                bool isEval = sendInst->getMsgDesc()->ResponseLength() == 0;
                uint32_t messageType = sendInst->getMsgDesc()->getSamplerMessageType();
                assert(!inst->getPredicate() && "do not handle predicated sampler inst for now");
                if (!isEval && cloneSample)
                {
                    if (!hasSample)
                    {
                        hasSample = true;
                        auto flagInit = builder.createMov(g4::SIMD1, builder.createDst(tmpFlag->getRegVar(), isSIMD32 ? Type_UD : Type_UW),
                            builder.createImm(isSIMD32 ? 0x0F0F0F0F : 0x0F0F, isSIMD32 ? Type_UD : Type_UW), InstOpt_WriteEnable, false);
                        bb->insertBefore(I, flagInit);
                    }
                    auto newInst = inst->cloneInst();
                    inst->setPredicate(builder.createPredicate(PredState_Plus, tmpFlag->getRegVar(), 0));
                    newInst->setPredicate(builder.createPredicate(PredState_Minus, tmpFlag->getRegVar(), 0));
                    auto newInstIt = bb->insertAfter(I, newInst);

                    uint16_t rspLen = inst->asSendInst()->getMsgDesc()->ResponseLength();
                    // If Pixel Null Mask feedback is requested sampler message
                    // has header, all data channels enabled and an additional
                    // GRF of writeback payload with Pixel Null Mask.
                    // Possible message response lengths are:
                    // - 5 GRFs for all simd8 messages and for simd16 messages
                    //   with 16-bit return format
                    // - 9 GRFs for simd16 message with 32-bit return format
                    // It is enough to check send's response length to determine
                    // if Pixel Null Mask feedback is enabled.
                    assert(inst->getExecSize() == g4::SIMD8 || inst->getExecSize() == g4::SIMD16);
                    uint16_t pixelNullMaskRspLen =
                        (inst->getExecSize() == g4::SIMD16 && !sendInst->getMsgDesc()->is16BitReturn()) ? 9 : 5;

                    if (sendInst->getMsgDesc()->isHeaderPresent() &&
                        rspLen == pixelNullMaskRspLen)
                    {
                        // Pixel Null Mask is in the first word of the last GRF
                        // of send's writeback message. This mask has bits set
                        // to 0 for pixels in which a null page was source for
                        // at least one texel. Otherwise bits are set to 1.

                        // Create a copy of Pixel Null Mask from the first send
                        // writeback message and AND it with the mask from the
                        // second send.
                        G4_Declare* maskCopy = builder.createTempVar(1, Type_UW, Any);
                        G4_Declare* maskAlias = builder.createTempVar(1, Type_UW, Any);
                        maskAlias->setAliasDeclare(
                            inst->getDst()->getBase()->asRegVar()->getDeclare(),
                            (inst->getDst()->getRegOff() + rspLen - 1) * numEltPerGRF<Type_UB>());
                        G4_SrcRegRegion* src = builder.Create_Src_Opnd_From_Dcl(
                            maskAlias, builder.getRegionScalar());
                        G4_DstRegRegion* dst = builder.createDst(maskCopy->getRegVar(), Type_UW);
                        G4_INST* movInst = builder.createMov(
                            g4::SIMD1, dst, src, InstOpt_WriteEnable, false);
                        bb->insertAfter(I, movInst);
                        G4_SrcRegRegion* src0 = builder.createSrcRegRegion(*src);
                        G4_SrcRegRegion* src1 = builder.Create_Src_Opnd_From_Dcl(
                            maskCopy, builder.getRegionScalar());
                        dst = builder.createDst(maskAlias->getRegVar(), Type_UW);
                        G4_INST* andInst = builder.createBinOp(G4_and, g4::SIMD1,
                            dst, src0, src1, InstOpt_WriteEnable, false);
                        bb->insertAfter(newInstIt, andInst);
                    }
                }
                else if(isEval && cloneEvaluateSample && messageType != 0x1F)
                {
                    // 0x1F is the opcode for sampler cache flush
                    uint32_t newExecSize = (messageType == VISA_3D_SAMPLE_L || messageType == VISA_3D_LD) ? 8 : 1;
                    uint32_t mask = (1 << newExecSize) - 1;
                    auto evalTmpFlag = builder.createTempFlag(isSIMD32 ? 2 : 1);
                    auto flagInit = builder.createMov(g4::SIMD1, builder.createDst(evalTmpFlag->getRegVar(), isSIMD32 ? Type_UD : Type_UW),
                        builder.createImm(mask, isSIMD32 ? Type_UD : Type_UW), InstOpt_WriteEnable, false);
                    bb->insertBefore(I, flagInit);
                    inst->setPredicate(builder.createPredicate(PredState_Plus, evalTmpFlag->getRegVar(), 0));
                    unsigned numInsts = kernel.getSimdSize() / newExecSize;
                    for (unsigned int i = 1; i < numInsts; i++)
                    {
                        auto newInst = inst->cloneInst();
                        bb->insertAfter(I, newInst);
                        evalTmpFlag = builder.createTempFlag(isSIMD32 ? 2 : 1);
                        flagInit = builder.createMov(g4::SIMD1, builder.createDst(evalTmpFlag->getRegVar(), isSIMD32 ? Type_UD : Type_UW),
                            builder.createImm(mask << (i * newExecSize), isSIMD32 ? Type_UD : Type_UW), InstOpt_WriteEnable, false);
                        newInst->setPredicate(builder.createPredicate(PredState_Plus, evalTmpFlag->getRegVar(), 0));
                        bb->insertAfter(I, flagInit);
                    }
                }
            }
            I = Next;
        }
    }
}

bool isLifetimeOpRemovalCandidate(G4_INST* inst)
{
    if (inst->isPseudoKill() || inst->isLifeTimeEnd())
    {
        return true;
    }

    return false;

}

bool isPseudoUse(G4_INST* inst)
{
    if (inst->isPseudoUse())
    {
        return true;
    }
    return false;
}

void Optimizer::removeLifetimeOps()
{
    // Call this function after RA only.

    // Remove all pseudo_kill and lifetime.end
    // instructions.
    // Also remove pseudo_use instructions.
    for (BB_LIST_ITER bbs = kernel.fg.begin();
        bbs != fg.end();
        bbs++)
    {
        G4_BB* bb = *bbs;
        bb->erase(
            std::remove_if (bb->begin(), bb->end(),
            [](G4_INST* inst) { return inst->isPseudoKill() || inst->isLifeTimeEnd() || inst->isPseudoUse(); }),
            bb->end());
    }
}

void Optimizer::runPass(PassIndex Index)
{
    const PassInfo &PI = Passes[Index];

    // Do not execute.
    if (PI.Option != vISA_EnableAlways && !builder.getOption(PI.Option))
        return;

    bool isImportantPass =
        Index == PI_dce ||
        Index == PI_regAlloc ||
        Index == PI_preRA_Schedule ||
        Index == PI_LVN ||
        Index == PI_HWConformityChk ||
        Index == PI_HWWorkaround;
    bool shouldDumpG4 =
        builder.getOption(vISA_DumpDotAll) ||
        builder.getuint32Option(vISA_DumpPassesSubset) >= 2 ||
        (builder.getuint32Option(vISA_DumpPassesSubset) == 1 && isImportantPass);

    std::string Name = PI.Name;

    if (PI.Timer != TimerID::NUM_TIMERS)
        startTimer(PI.Timer);

    if (shouldDumpG4)
        kernel.dumpDotFile(("before." + Name).c_str());

    // Execute pass.
    (this->*(PI.Pass))();

    if (PI.Timer != TimerID::NUM_TIMERS)
        stopTimer(PI.Timer);

    if (shouldDumpG4)
        kernel.dumpDotFile(("after." + Name).c_str());

#ifdef _DEBUG
    bool skipVerify = Index == PI_regAlloc && RAFail == true;
    if (!skipVerify)
    {
        verifyG4Kernel(kernel, Index, true, G4Verifier::VC_ASSERT);
    }
#endif
}

void Optimizer::initOptimizations()
{
#define INITIALIZE_PASS(Name, Option, Timer) \
    Passes[PI_##Name] = PassInfo(&Optimizer::Name, ""#Name, Option, Timer)

    // To initialize a pass, the member function name is the first argument.
    // This member function must return void and take no argument.
    //
    // The second argument is the corresponding option to enable this pass.
    // If it always runs then use vISA_EnableAlways.
    //
    // The third argument is the intended timer for this pass. If no timing
    // is necessary, then TIMER_NUM_TIMERS can be used.
    //
    INITIALIZE_PASS(cleanMessageHeader,      vISA_LocalCleanMessageHeader, TimerID::OPTIMIZER);
    INITIALIZE_PASS(sendFusion,              vISA_EnableSendFusion,        TimerID::OPTIMIZER);
    INITIALIZE_PASS(renameRegister,          vISA_LocalRenameRegister,     TimerID::OPTIMIZER);
    INITIALIZE_PASS(newLocalDefHoisting,     vISA_LocalDefHoist,           TimerID::OPTIMIZER);
    INITIALIZE_PASS(newLocalCopyPropagation, vISA_LocalCopyProp,           TimerID::OPTIMIZER);
    INITIALIZE_PASS(cselPeepHoleOpt,         vISA_enableCSEL,              TimerID::OPTIMIZER);
    INITIALIZE_PASS(optimizeLogicOperation,  vISA_EnableAlways,            TimerID::OPTIMIZER);
    INITIALIZE_PASS(HWConformityChk,         vISA_EnableAlways,            TimerID::HW_CONFORMITY);
    INITIALIZE_PASS(preRA_Schedule,          vISA_preRA_Schedule,          TimerID::PRERA_SCHEDULING);
    INITIALIZE_PASS(preRA_HWWorkaround,      vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(regAlloc,                vISA_EnableAlways,            TimerID::TOTAL_RA);
    INITIALIZE_PASS(removeLifetimeOps,       vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(countBankConflicts,      vISA_OptReport,               TimerID::MISC_OPTS);
    INITIALIZE_PASS(removeRedundMov,         vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(removeEmptyBlocks,       vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(insertFallThroughJump,   vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(reassignBlockIDs,        vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(evalAddrExp,             vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(FoldAddrImmediate,       vISA_FoldAddrImmed,           TimerID::MISC_OPTS);
    INITIALIZE_PASS(localSchedule,           vISA_LocalScheduling,         TimerID::SCHEDULING);
    INITIALIZE_PASS(HWWorkaround,            vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(insertInstLabels,        vISA_EnableAlways,            TimerID::NUM_TIMERS);
    INITIALIZE_PASS(insertHashMovs,          vISA_InsertHashMovs,          TimerID::NUM_TIMERS);
    INITIALIZE_PASS(insertDummyMovForHWRSWA, vISA_InsertDummyMovForHWRSWA, TimerID::NUM_TIMERS);
    INITIALIZE_PASS(insertDummyCompactInst,  vISA_InsertDummyCompactInst,  TimerID::NUM_TIMERS);
    INITIALIZE_PASS(mergeScalarInst,         vISA_MergeScalar,             TimerID::OPTIMIZER);
    INITIALIZE_PASS(lowerMadSequence,        vISA_EnableMACOpt,            TimerID::OPTIMIZER);
    INITIALIZE_PASS(LVN,                     vISA_LVN,                     TimerID::OPTIMIZER);
    INITIALIZE_PASS(ifCvt,                   vISA_ifCvt,                   TimerID::OPTIMIZER);
    INITIALIZE_PASS(dumpPayload,             vISA_dumpPayload,             TimerID::MISC_OPTS);
    INITIALIZE_PASS(normalizeRegion,         vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(collectStats,            vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(createR0Copy,            vISA_enablePreemption,        TimerID::MISC_OPTS);
    INITIALIZE_PASS(initializePayload,       vISA_InitPayload,             TimerID::NUM_TIMERS);
    INITIALIZE_PASS(cleanupBindless,         vISA_enableCleanupBindless,   TimerID::OPTIMIZER);
    INITIALIZE_PASS(countGRFUsage,           vISA_PrintRegUsage,           TimerID::MISC_OPTS);
    INITIALIZE_PASS(changeMoveType,          vISA_ChangeMoveType,          TimerID::MISC_OPTS);
    INITIALIZE_PASS(reRAPostSchedule,        vISA_ReRAPostSchedule,        TimerID::OPTIMIZER);
    INITIALIZE_PASS(accSubPostSchedule,      vISA_accSubstitution,         TimerID::OPTIMIZER);
    INITIALIZE_PASS(dce,                     vISA_EnableDCE,               TimerID::OPTIMIZER);
    INITIALIZE_PASS(reassociateConst,        vISA_reassociate,             TimerID::OPTIMIZER);
    INITIALIZE_PASS(split4GRFVars,           vISA_split4GRFVar,            TimerID::OPTIMIZER);
    INITIALIZE_PASS(addFFIDProlog,           vISA_addFFIDProlog,           TimerID::MISC_OPTS);
    INITIALIZE_PASS(loadThreadPayload,       vISA_loadThreadPayload,       TimerID::MISC_OPTS);
    INITIALIZE_PASS(insertFenceBeforeEOT,    vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(insertScratchReadBeforeEOT, vISA_clearScratchWritesBeforeEOT, TimerID::MISC_OPTS);
    INITIALIZE_PASS(mapOrphans,              vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(varSplit,                vISA_EnableAlways,            TimerID::OPTIMIZER);
    INITIALIZE_PASS(legalizeType,            vISA_EnableAlways,            TimerID::MISC_OPTS);
    INITIALIZE_PASS(analyzeMove,             vISA_analyzeMove,             TimerID::MISC_OPTS);
    INITIALIZE_PASS(removeInstrinsics,       vISA_removeInstrinsics,       TimerID::MISC_OPTS);
    INITIALIZE_PASS(expandMulPostSchedule,   vISA_expandMulPostSchedule,   TimerID::MISC_OPTS);
    INITIALIZE_PASS(addSWSBInfo,             vISA_addSWSBInfo,             TimerID::MISC_OPTS);

    // Verify all passes are initialized.
#ifdef _DEBUG
    for (unsigned i = 0; i < PI_NUM_PASSES; ++i)
    {
        ASSERT_USER(Passes[i].Pass, "uninitialized pass");
    }
#endif
}

void replaceAllSpilledRegions(G4_Kernel& kernel, G4_Declare* oldDcl, G4_Declare* newDcl)
{
    // Iterate fg and replace all references to oldDcl with newDcl.
    // This requires creating new operands since changing dcl in
    // existing operands is disallowed.
    for (auto bb : kernel.fg)
    {
        for (auto inst : *bb)
        {
            auto dst = inst->getDst();
            if (dst && !dst->isNullReg())
            {
                if (dst->getTopDcl() == oldDcl)
                {
                    // code does not appear to handle indirect
                    assert(!dst->isIndirect());
                    G4_DstRegRegion* newDstRgn = kernel.fg.builder->createDst(
                        newDcl->getRegVar(), dst->getRegOff(),
                        dst->getSubRegOff(), dst->getHorzStride(), dst->getType());
                    inst->setDest(newDstRgn);
                }
            }

            for (unsigned int i = 0; i < G4_MAX_SRCS; i++)
            {
                auto src = inst->getSrc(i);

                if (src &&
                    src->isSrcRegRegion())
                {
                    auto srcRgn = inst->getSrc(i)->asSrcRegRegion();

                    if (srcRgn->getTopDcl() == oldDcl)
                    {
                        G4_SrcRegRegion* newSrcRgn = kernel.fg.builder->createSrcRegRegion(
                            srcRgn->getModifier(), srcRgn->getRegAccess(), newDcl->getRegVar(),
                            srcRgn->getRegOff(), srcRgn->getSubRegOff(), srcRgn->getRegion(),
                            srcRgn->getType());
                        inst->setSrc(newSrcRgn, i);
                    }
                }
            }
        }
    }
}

void getPhyRegs(G4_Operand* opnd, unsigned int& start, unsigned int& end)
{
    // start/end are inclusive, ie both offsets are referenced by the variable
    start = 0;
    end = 0;

    auto b = opnd->getBase();
    if (opnd->isAddrExp())
    {
        b = opnd->asAddrExp()->getRegVar();
    }

    if (b && b->isRegVar())
    {
        auto r = b->asRegVar();
        if (r->getPhyReg() &&
            r->getPhyReg()->isGreg())
        {
            if (!opnd->isAddrExp())
            {
                start = opnd->getLinearizedStart();
                end = opnd->getLinearizedEnd();
            }
            else
            {
                r = r->getDeclare()->getRootDeclare()->getRegVar();
                auto phyReg = r->getPhyReg()->asGreg();
                auto subRegOff = r->getPhyRegOff();
                start = phyReg->getRegNum() * numEltPerGRF<Type_UB>();
                start += subRegOff * r->getDeclare()->getElemSize();
                end = start + r->getDeclare()->getRootDeclare()->getByteSize() - 1;
            }
        }
    }
}

void computeGlobalFreeGRFs(G4_Kernel& kernel)
{
    auto gtpin = kernel.getGTPinData();
    gtpin->clearFreeGlobalRegs();
    std::vector<bool> freeGRFs(kernel.getNumRegTotal() * numEltPerGRF<Type_UB>(), true);
    unsigned int start = 0, end = 0;

    // Mark r0 as busy. Done explicitly because move from r0 is inserted
    // after reRA pass.
    for (unsigned int i = 0; i < numEltPerGRF<Type_UB>(); i++)
    {
        freeGRFs[i] = false;
    }

    for (auto bb : kernel.fg)
    {
        for (auto inst : *bb)
        {
            auto dst = inst->getDst();
            if (dst &&
                dst->getBase() &&
                dst->getBase()->isRegVar())
            {
                getPhyRegs(dst, start, end);
                for (unsigned int i = start; i <= end; i++)
                {
                    freeGRFs[i] = false;
                }
            }

            for (unsigned int i = 0; i < G4_MAX_SRCS; i++)
            {
                auto src = inst->getSrc(i);

                if (!src)
                    continue;

                if (src->isSrcRegRegion() &&
                    src->asSrcRegRegion()->getBase()->isRegVar())
                {
                    getPhyRegs(src, start, end);
                    for (unsigned int i = start; i <= end; i++)
                    {
                        freeGRFs[i] = false;
                    }
                }
                else if (src->isAddrExp())
                {
                    getPhyRegs(src, start, end);
                    for (unsigned int i = start; i <= end; i++)
                    {
                        freeGRFs[i] = false;
                    }
                }
            }
        }
    }

    for (unsigned int i = 0; i < freeGRFs.size(); i++)
    {
        if (freeGRFs[i])
        {
            gtpin->addFreeGlobalReg(i);
        }
    }
}

typedef struct Assignment
{
    G4_Declare* dcl = nullptr;
    AssignedReg reg;
    unsigned int GRFBaseOffset = 0;
} Assignment;

void storeGRFAssignments(DECLARE_LIST& declares, std::vector<Assignment>& assignments)
{
    for (auto dcl : declares)
    {
        auto regVar = dcl->getRegVar();
        if (regVar &&
            regVar->isPhyRegAssigned() &&
            regVar->getPhyReg()->isGreg())
        {
            Assignment a;
            AssignedReg assignedPhyReg;
            assignedPhyReg.phyReg = regVar->getPhyReg();
            assignedPhyReg.subRegOff = regVar->getPhyRegOff();
            a.dcl = dcl;
            a.reg = assignedPhyReg;
            a.GRFBaseOffset = dcl->getGRFBaseOffset();
            assignments.push_back(a);

        }
    }
}

void restoreGRFAssignments(std::vector<Assignment>& assignments)
{
    for (auto& a : assignments)
    {
        auto regVar = a.dcl->getRegVar();
        regVar->setPhyReg(a.reg.phyReg, a.reg.subRegOff);
        a.dcl->setGRFBaseOffset(a.GRFBaseOffset);
    }
}

// simple heuristics to decide if it's profitable to do copy propagation for the move
// add more as necessary
bool Optimizer::isCopyPropProfitable(G4_INST* movInst) const
{
    assert(movInst->opcode() == G4_mov && "execpt move instruction");

    // if inst is a simd16 W/HF packing, we don't want to optimize it if
    // there are >=2 simd16 mad uses, since it will slow down the mad.
    // for gen9 additionally check for simd8 mad as it doesn't support strided regions
    auto dst = movInst->getDst();
    auto src0 = movInst->getSrc(0);
    auto hasStrideSource = dst->getHorzStride() == 1 &&
        src0->isSrcRegRegion() &&
        !(src0->asSrcRegRegion()->getRegion()->isContiguous(movInst->getExecSize()) ||
            src0->asSrcRegRegion()->getRegion()->isScalar());

    hasStrideSource &= movInst->getExecSize() == g4::SIMD16 ||
        (!builder.hasAlign1Ternary() && movInst->getExecSize() == g4::SIMD8);

    auto hasNSIMD16or8MadUse = [](G4_INST* movInst, int N, bool checkSIMD8)
    {
        int numMadUses = 0;
        for (auto iter = movInst->use_begin(), iterEnd = movInst->use_end(); iter != iterEnd; ++iter)
        {
            auto use = *iter;
            auto inst = use.first;
            if (inst->opcode() == G4_pseudo_mad &&
                (inst->getExecSize() == g4::SIMD16 || (checkSIMD8 && inst->getExecSize() == g4::SIMD8)))
            {
                ++numMadUses;
                if (numMadUses == N)
                {
                    return true;
                }
            }
        }
        return false;
    };

    if (hasStrideSource)
    {
        if (hasNSIMD16or8MadUse(movInst, 2, true))
        {
            return false;
        }
    }

    // another condition where copy prop may be not profitable:
    // mov is from HF to F and dst is used in simd16 mad.
    // copy propagating away the move results in mix mode mad which is bad for bank conflicts
    if (dst->getType() == Type_F && src0->getType() == Type_HF)
    {
        if (hasNSIMD16or8MadUse(movInst, 4, false))
        {
            return false;
        }
    }
    return true;
}

void Optimizer::reRAPostSchedule()
{
    // No rera for stackcall functions
    if (kernel.fg.getHasStackCalls() || kernel.fg.getIsStackCallFunc())
        return;

    std::vector<Assignment> assignments;
    auto finalizerInfo = *builder.getJitInfo();
    auto oldRAType = kernel.getRAType();
    auto gtpin = kernel.getGTPinData();
    computeGlobalFreeGRFs(kernel);
    auto freeGRFsBeforeReRA = gtpin->getNumFreeGlobalRegs();

    storeGRFAssignments(kernel.Declares, assignments);
    // This pass is run for gtpin since they need re-allocation after
    // scheduling pass to get as many free registers for instrumentation.

    // Store dcls referenced by srcs of 3-src instructions. These will
    // not be participate in re-allocation because that could potentially
    // alter bank conflict profile which might affect kernel performance.
    std::set<G4_Declare*> threeSrcDcl;
    for (auto bb : kernel.fg)
    {
        for (auto inst : *bb)
        {
            if (inst->isMovAddr() &&
                inst->getDst()->getTopDcl()->getRegFile() == G4_RegFileKind::G4_GRF)
            {
                // inst's dst is a spilled address register from previous RA pass.
                // Addr/flags are spilled to GRF. When address regs are spilled to GRF,
                // points2 analysis will miss those entries since it expects only addr
                // reg when src is G4_AddrExp type. So we replace such spilled addr
                // operands with new address operands.
                G4_Declare* dstDcl = inst->getDst()->getTopDcl();
                auto name = builder.getNameString(fg.mem, 16, "ADDR%d", kernel.Declares.size());
                G4_Declare* newDstAddrDcl = kernel.fg.builder->createDeclareNoLookup(name,
                    G4_RegFileKind::G4_ADDRESS, dstDcl->getNumElems(),
                    dstDcl->getNumRows(), dstDcl->getElemType());
                replaceAllSpilledRegions(kernel, dstDcl, newDstAddrDcl);
            }

            if (inst->getNumSrc() == 3 && !inst->isSend())
            {
                for (auto i = 0; i < inst->getNumSrc(); i++)
                {
                    auto srcOpnd = inst->getSrc(i);
                    if (srcOpnd->isSrcRegRegion())
                    {
                        auto dcl = srcOpnd->getTopDcl();
                        threeSrcDcl.insert(dcl);
                    }
                }
            }
        }
    }

    for (auto dcl : kernel.Declares)
    {
        if (threeSrcDcl.find(dcl->getRootDeclare()) == threeSrcDcl.end())
        {
            dcl->prepareForRealloc(&kernel);
        }
    }

    kernel.fg.clearBBLRASummaries();

    gtpin->setRAPass(gtPinData::RAPass::ReRAPass);

    gtpin->removeUnmarkedInsts();

    // This time we want to try only first-fit and no localra
    regAlloc();

    removeLifetimeOps();

    computeGlobalFreeGRFs(kernel);

    if (RAFail ||
        gtpin->getNumFreeGlobalRegs() < freeGRFsBeforeReRA)
    {
        restoreGRFAssignments(assignments);
        kernel.setRAType(oldRAType);

        computeGlobalFreeGRFs(kernel);
    }

    *builder.getJitInfo() = finalizerInfo;
}

void Optimizer::accSubPostSchedule()
{
    if (!builder.doAccSub() || !builder.getOption(vISA_doAccSubAfterSchedule))
    {
        return;
    }

    kernel.fg.resetLocalDataFlowData();
    kernel.fg.localDataFlowAnalysis();

    if (builder.getOption(vISA_localizationForAccSub))
    {
        HWConformity hwConf(builder, kernel, mem);
        for (auto bb : kernel.fg)
        {
            hwConf.localizeForAcc(bb);
        }

        kernel.fg.resetLocalDataFlowData();
        kernel.fg.localDataFlowAnalysis();
    }

    AccSubPass accSub(builder, kernel);
    accSub.run();
}

int Optimizer::optimization()
{
    // remove redundant message headers.
    runPass(PI_cleanMessageHeader);


    runPass(PI_sendFusion);

    // rename registers.
    runPass(PI_renameRegister);

    runPass(PI_newLocalDefHoisting);

    // remove redundant movs
    runPass(PI_newLocalCopyPropagation);

    runPass(PI_mergeScalarInst);

    runPass(PI_cselPeepHoleOpt);

    runPass(PI_reassociateConst);

    runPass(PI_lowerMadSequence);

    // optimize logic operantions
    runPass(PI_optimizeLogicOperation);

    // Dead code elimination
    runPass(PI_dce);

    // HW conformity check
    runPass(PI_HWConformityChk);

    // Local Value Numbering
    runPass(PI_LVN);

    // this must be run after copy prop cleans up the moves
    runPass(PI_cleanupBindless);

    runPass(PI_split4GRFVars);

    runPass(PI_insertFenceBeforeEOT);

    runPass(PI_varSplit);

    // PreRA scheduling
    runPass(PI_preRA_Schedule);

    // HW workaround before RA (assume no pseudo inst)
    runPass(PI_preRA_HWWorkaround);

    // perform register allocation
    runPass(PI_regAlloc);
    if (RAFail)
    {
        return VISA_SPILL;
    }

    runPass(PI_removeLifetimeOps);

    runPass(PI_countBankConflicts);

    //
    // if a fall-through BB does not immediately follow its predecessor
    // in the code layout, then insert a jump-to-fall-through in the predecessor
    //
    runPass(PI_insertFallThroughJump);

    // Run if-conversion to convert short if-blocks.
    runPass(PI_ifCvt);

    //
    // re-assign block ID so that we can use id to determine the ordering of
    // two blocks in the code layout
    //
    runPass(PI_reassignBlockIDs);

    runPass(PI_FoldAddrImmediate);

    // FIXME houjenko: Disable local scheduling due to issues when
    // using extra regiser that may corrupt unknown liveout
    if (!builder.getIsPayload())
    {
        runPass(PI_localSchedule);
    }

    runPass(PI_expandMulPostSchedule);

    runPass(PI_accSubPostSchedule);

    runPass(PI_legalizeType);

    runPass(PI_changeMoveType);

    runPass(PI_reRAPostSchedule);

    // No pass after this should expect def-use to be preserved as this pass
    // removes raw movs with identical src/dst physical GRFs.
    runPass(PI_removeRedundMov);

    // remove any placeholders blocks inserted to aid regalloc
    // run this pass after reRA pass otherwise CFG can become
    // invalid (funcInfo, calleeInfo may point to bad initBB).
    runPass(PI_removeEmptyBlocks);

    runPass(PI_insertScratchReadBeforeEOT);

    // HW workaround
    runPass(PI_HWWorkaround);

    runPass(PI_normalizeRegion);

    runPass(PI_countGRFUsage);

    runPass(PI_dumpPayload);

    // this must be the last step of the optimization so as to not violate
    // the CFG assumption
    runPass(PI_insertInstLabels);

    runPass(PI_insertHashMovs);

    runPass(PI_insertDummyMovForHWRSWA);

    runPass(PI_collectStats);

    // Create a copy of R0 at the top of kernel.
    // This must be done after all other optimizer
    // passes except for loadThreadPlayoad
    runPass(PI_createR0Copy);

    runPass(PI_initializePayload);

    runPass(PI_loadThreadPayload);

    runPass(PI_addFFIDProlog);

    // Insert a dummy compact instruction if requested for SKL+
    runPass(PI_insertDummyCompactInst);

    runPass(PI_mapOrphans);

    runPass(PI_analyzeMove);

    runPass(PI_removeInstrinsics);

    //-----------------------------------------------------------------------------------------------------------------
    //------NOTE!!!! No instruction change(add/remove, or operand associated change) is allowed after SWSB-------------
    //-----------------------------------------------------------------------------------------------------------------
    runPass(PI_addSWSBInfo);

    return VISA_SUCCESS;
}

//  When constructing CFG we have the assumption that a label must be the first
//  instruction in a bb.  During structure analysis, however, we may end up with a bb that
//  starts with multiple endifs if the bb is the target of multiple gotos that have been
//  converted to an if. Instead of creating a BB for each of the endif, we associate each endif with a label
//  and emit them only at the very end.
//
//  For break and continue, UIP must be the lable directly attached to the while
//  op. If not, create such a label
//
//  DO
//    IF
//      P =
//      CONT L1
//    ENDIF L1
//    IF
//      BREAK L2
//    ENDIF L1
//  L1
//  (P) WHILE
//  L2
//
//  will be transfered into
//
//  DO
//    IF
//      P =
//      Spill <- P
//      CONT L3         // UIP becomes L3
//    ENDIF L1
//    IF
//      BREAK L3        // UIP becomes L3
//    ENDIF L1
//  L1                  // existing label
//  P <- fill
//  L3                  // new label
//  (P) WHILE
//  L2
//
void Optimizer::insertInstLabels()
{
    for (BB_LIST_CITER iter = fg.cbegin(), bend = fg.cend(); iter != bend; ++iter)
    {
        G4_BB* bb = *iter;
        INST_LIST_ITER iter2 = bb->begin();
        INST_LIST_ITER iend  = bb->end();
        while (iter2 != iend)
        {
            INST_LIST_ITER currIter = iter2;
            ++iter2;

            G4_INST* inst = *currIter;
            G4_Label* endifLabel = fg.getLabelForEndif (inst);
            if (endifLabel)
            {
                G4_INST* labelInst = fg.createNewLabelInst(endifLabel, inst->getLineNo(), inst->getCISAOff());
                bb->insertBefore(currIter, labelInst);
            }
        }
    }

    // Patch labels if necessary.
    for (auto iter = fg.begin(), iend = fg.end(); iter != iend; ++iter)
    {
        G4_BB *bb = *iter;
        if (bb->empty())
            continue;

        G4_INST *inst = bb->back();
        G4_opcode opc = inst->opcode();
        if (opc != G4_cont && opc != G4_break)
            continue;

        // The matching while BB.
         G4_BB *whileBB = nullptr;
         if (opc == G4_cont)
         {
             // The whileBB is the first successor bb, if this is continue.
             whileBB = bb->Succs.front();
         }
         else
         {
             // For break, the whileBB should be the physical predecessor of
             // break's first successor bb.
             BB_LIST_ITER iter = bb->Succs.begin();
             while (iter != bb->Succs.end())
             {
                 G4_BB * succBB = (*iter);
                 if (succBB->getPhysicalPred() &&
                     (!succBB->getPhysicalPred()->empty()) &&
                     (succBB->getPhysicalPred()->back()->opcode() == G4_while))
                 {
                     whileBB = succBB->getPhysicalPred();
                     break;
                 }
                 iter++;
             }
         }

         if (whileBB == nullptr ||
             whileBB->empty() ||
             whileBB->back()->opcode() != G4_while)
         {
             ASSERT_USER(false, "can not find while BB");
             continue;
         }

         // If while instruction is following the label, then no need
         // to insert a new uip label, just use the existing one.
         G4_InstCF *instCF = inst->asCFInst();
         auto whileIter = std::prev(whileBB->end());
         G4_INST *prevInst = *std::prev(whileIter);
         if (prevInst->isLabel())
         {
             instCF->setUip(prevInst->getLabel());
         }
         else
         {
             std::string NewUipName = instCF->getUipLabelStr();
             NewUipName += "_UIP";
             G4_Label *label = builder.createLabel(NewUipName, LABEL_BLOCK);
             instCF->setUip(label);

             G4_INST *newInst = fg.createNewLabelInst(label, inst->getLineNo(),
                                                      inst->getCISAOff());

             whileBB->insertBefore(whileIter, newInst);
         }
    }
}

//Fold address register into address register offset, such that we can same one instruction
//that computes:
//add a0.0 a0.0 immed;
//mul dst r[a0.0, 0] src2
//-->
//mul dst r[a0.0, immed] src2

//The condition is that immed is in range [-512..511] and it is dividable by 32.
//This is a local OPT.
//For simplicity, only execsize 1 is considered.
//Since physical registers are alreasy assigned, we use the info directly here without check.

void Optimizer::reverseOffsetProp(
    AddrSubReg_Node addrRegInfo[8],
    int subReg,
    unsigned int srcNum,
    INST_LIST_ITER lastIter,
    INST_LIST_ITER iend
   )
{
    if (addrRegInfo[subReg].usedImmed && addrRegInfo[subReg].canUseImmed)
    {
        INST_LIST_ITER iter;
        G4_INST *inst;
        G4_Operand *inst_src;
        G4_DstRegRegion *inst_dst;
        for (iter = addrRegInfo[subReg].iter; iter != lastIter; ++iter)
        {
            if (iter == lastIter)
                break;
            inst = *iter;
            if (inst->isDead())
                continue;
            inst_dst = inst->getDst();
            if (inst_dst &&
                inst_dst->getRegAccess() != Direct)
            {
                int subReg1 = getDstSubReg(inst_dst);

                short currOff = inst_dst->getAddrImm();
                if (subReg1 == subReg)
                {
                    // create a new dst
                    G4_DstRegRegion tmpRgn(*inst_dst);
                    G4_DstRegRegion *newDst = &tmpRgn;
                    newDst->setImmAddrOff(short(currOff - addrRegInfo[subReg].immAddrOff));
                    inst->setDest(builder.createDstRegRegion(*newDst));
                }
            }
            for (int i = 0; i < inst->getNumSrc(); i++)
            {
                inst_src = inst->getSrc(i);
                if (inst_src && inst_src->isSrcRegRegion() &&
                    inst_src->asSrcRegRegion()->getRegAccess() != Direct)
                {
                    int subReg1 = getSrcSubReg(inst_src);

                    short currOff = inst_src->asSrcRegRegion()->getAddrImm();
                    if (subReg1 == subReg)
                    {
                        G4_SrcRegRegion tmpRgn(*inst_src->asSrcRegRegion());
                        G4_SrcRegRegion *newSrc = &tmpRgn;
                        newSrc->setImmAddrOff(short(currOff - addrRegInfo[subReg].immAddrOff));
                        inst->setSrc(builder.createSrcRegRegion(*newSrc) , i);
                    }
                }
            }
        }
        // Immed has been propagated to srcs before this src in *ii, also reverse this
        if (srcNum > 0)
        {
            inst = *lastIter;
            for (unsigned i = 0; i < srcNum; i++)
            {
                inst_src = inst->getSrc(i);
                if (inst_src && inst_src->isSrcRegRegion() &&
                    inst_src->asSrcRegRegion()->getRegAccess() != Direct)
                {
                    int subReg1 = getSrcSubReg(inst_src);

                    short currOff = inst_src->asSrcRegRegion()->getAddrImm();
                    if (subReg1 == subReg)
                    {
                        G4_SrcRegRegion tmpRgn(*inst_src->asSrcRegRegion());
                        G4_SrcRegRegion *newSrc = &tmpRgn;
                        newSrc->setImmAddrOff(short(currOff - addrRegInfo[subReg].immAddrOff));
                        inst->setSrc(builder.createSrcRegRegion(*newSrc) , i);
                    }
                }
            }
        }
    }

    addrRegInfo[subReg].immAddrOff = 0;
    addrRegInfo[subReg].iter = iend;
    addrRegInfo[subReg].canRemoveInst = false;
    addrRegInfo[subReg].canUseImmed = false;
    addrRegInfo[subReg].usedImmed = false;
}

void Optimizer::FoldAddrImmediate()
{
    AddrSubReg_Node* addrRegInfo = new AddrSubReg_Node[getNumAddrRegisters()];
    BB_LIST_ITER ib, bend(fg.end());
    int dst_subReg = 0, src0_subReg = 0;
    G4_DstRegRegion *dst;
    G4_Operand *src0, *src1;
    unsigned num_srcs;

    for (ib = fg.begin(); ib != bend; ++ib)
    {
        G4_BB* bb = (*ib);
        INST_LIST_ITER ii, iend(bb->end());
        // reset address offset info
        for (unsigned i = 0; i < getNumAddrRegisters(); i++)
        {
            addrRegInfo[i].subReg = 0;
            addrRegInfo[i].immAddrOff = 0;
            addrRegInfo[i].iter = iend;
            addrRegInfo[i].canRemoveInst = false;
            addrRegInfo[i].canUseImmed = false;
            addrRegInfo[i].usedImmed = false;
        }
        for (ii = bb->begin(); ii != iend; ii++)
        {
            G4_INST *inst = *ii;
            if (inst->isDead())
            {
                continue;
            }
            num_srcs = inst->getNumSrc();
            dst = inst->getDst();
            if (dst)
            {
                dst_subReg = getDstSubReg(dst);
            }
            src0 = inst->getSrc(0);
            if (src0 && src0->isSrcRegRegion())
            {
                src0_subReg = getSrcSubReg(src0);
            }
            src1 = inst->getSrc(1);

            if (dst != NULL && dst->getRegAccess() == Direct &&
                src0 != NULL && src0->isSrcRegRegion() && src0->asSrcRegRegion()->getRegAccess() == Direct &&
                dst->isAreg() && dst->isA0() &&
                src0->isAreg() && src0->asSrcRegRegion()->isA0() &&
                src1==NULL)
            {
                continue;
            }

            if (inst->opcode() == G4_add && inst->getExecSize() == g4::SIMD1 &&
                inst->getPredicate() == NULL &&
                (src1->isImm() && !src1->isRelocImm()) &&
                dst && dst->getRegAccess() == Direct &&
                src0 && src0->isSrcRegRegion() && src0->asSrcRegRegion()->getRegAccess() == Direct &&
                dst->isAreg() && dst->isA0() &&
                src0->isAreg() && src0->asSrcRegRegion()->isA0() &&
                dst_subReg == src0_subReg)
            {
                // since there is use of a0.x here, we can not remove the former def of a0.x
                // reverse immed offset propagation
                reverseOffsetProp(addrRegInfo, dst_subReg, 0, ii, iend);

                int64_t offset = src1->asImm()->getImm();
                if (offset >= -512 && offset <= 511 && offset%0x20 == 0)
                {
                    // this kills the previous def on a0.x
                    if (addrRegInfo[dst_subReg].canRemoveInst && addrRegInfo[dst_subReg].iter != iend)
                    {
                        // mark dead
                        (*(addrRegInfo[dst_subReg].iter))->markDead();
                    }
                    addrRegInfo[dst_subReg].subReg = dst_subReg;
                    addrRegInfo[dst_subReg].immAddrOff = (short)offset;
                    addrRegInfo[dst_subReg].iter = ii;
                    addrRegInfo[dst_subReg].canRemoveInst = true;
                    addrRegInfo[dst_subReg].canUseImmed = true;
                    addrRegInfo[dst_subReg].usedImmed = false;
                }
            }
            else
            {
                G4_Operand *src;
                // if there is any direct use of addr reg, the ADD inst can not be removed
                for (unsigned i = 0; i < num_srcs; i++)
                {
                    src = inst->getSrc(i);
                    if (src && src->isSrcRegRegion() && src->asSrcRegRegion()->getRegAccess() == Direct &&
                        src->asSrcRegRegion()->isAreg() && src->asSrcRegRegion()->isA0())
                    {
                        // TODO: show if an inst is generated for spill code
                        // if there is no regVar for this srcRegion, the physical register is hard-wired in input or generated by spillCode.
                        // in this case, the subregister info is in the subRegOff of G4_SrcRegRegion
                        // this also applies to dst register
                        int subReg = getSrcSubReg(src);

                        // it is possible that several elements are used
                        int width, hstride, vstride, outerloop = 1;
                        width = src->asSrcRegRegion()->getRegion()->width;
                        hstride = src->asSrcRegRegion()->getRegion()->horzStride;
                        vstride = src->asSrcRegRegion()->getRegion()->vertStride;
                        if (vstride != 0)
                        {
                            outerloop = inst->getExecSize() / vstride;
                        }

                        for (int k = 0; k < outerloop; k++)
                        {
                            for (int j = 0; j < width; j++)
                            {
                                int currSubreg = subReg + k * vstride + j * hstride;
                                // there may be inst whose src or dst addr immediate offset are already changed
                                // reverse the change
                                reverseOffsetProp(addrRegInfo, currSubreg, i, ii, iend);
                            }
                        }
                    }
                }
                // use of address register in index region
                for (unsigned i = 0; i < num_srcs; i++)
                {
                    src = inst->getSrc(i);
                    if (src && src->isSrcRegRegion() && src->asSrcRegRegion()->getRegAccess() != Direct)
                    {
                        int subReg = getSrcSubReg(src);

                        // if VxH is used and more than one sub registers are used in addressing
                        // do not fold the immediate even though they have the same immediate value
                        unsigned short vertStride = src->asSrcRegRegion()->getRegion()->vertStride;
                        if (vertStride == UNDEFINED_SHORT ||
                            (vertStride > 0 && (unsigned short)inst->getExecSize() / vertStride > 1))
                        {
                            int numSubReg = 0;
                            if (vertStride == UNDEFINED_SHORT)
                            {
                                numSubReg = inst->getExecSize()/src->asSrcRegRegion()->getRegion()->width;
                            }
                            else
                            {
                                numSubReg = 1; //inst->getExecSize()/vertStride;
                            }
                            for (int j = subReg; j < subReg + numSubReg; j++)
                            {
                                reverseOffsetProp(addrRegInfo, j, i, ii, iend);
                            }
                        }
                        else
                        {
                            // we check the existing address reg imm offset.
                            short currOff = src->asSrcRegRegion()->getAddrImm();
                            if (addrRegInfo[subReg].canUseImmed)
                            {
                                if (currOff % 0x20 == 0 &&
                                    (currOff + addrRegInfo[subReg].immAddrOff) <= 511 &&
                                    (currOff + addrRegInfo[subReg].immAddrOff) >= -512)
                                {
                                    G4_SrcRegRegion tmpRgn(*src->asSrcRegRegion());
                                    G4_SrcRegRegion* newSrc = &tmpRgn;
                                    newSrc->setImmAddrOff(short(currOff + addrRegInfo[subReg].immAddrOff));
                                    inst->setSrc(builder.createSrcRegRegion(*newSrc), i);

                                    addrRegInfo[subReg].usedImmed = true;
                                }
                                else
                                {
                                    // if the offset can not be folded into all uses of a0.0, reverse the former folding
                                    reverseOffsetProp(addrRegInfo, subReg, i, ii, iend);
                                }
                            }
                        }
                    }
                }
                if (dst)
                {
                    // make sure the addr reg is not redefined
                    // direct access to a0.x
                    if (dst->isAreg() && dst->getRegAccess() == Direct && dst->isA0())
                    {
                        int width, hstride;
                        width = inst->getExecSize();
                        hstride = dst->getHorzStride();

                        for (int j = 0; j < width; j++)
                        {
                            int currSubreg = dst_subReg + j * hstride;
                            // this kills the previous def on a0.x
                            if (addrRegInfo[currSubreg].iter != iend && addrRegInfo[currSubreg].canRemoveInst)
                            {
                                // mark dead
                                (*(addrRegInfo[currSubreg].iter))->markDead();
                            }
                            addrRegInfo[currSubreg].immAddrOff = 0;
                            addrRegInfo[currSubreg].iter = iend;
                            addrRegInfo[currSubreg].canRemoveInst = false;
                            addrRegInfo[currSubreg].canUseImmed = false;
                            addrRegInfo[currSubreg].usedImmed = false;
                        }
                    }
                    // check if dst is indirectly addressed
                    else if (dst->getRegAccess() != Direct)
                    {
                        short currOff = dst->getAddrImm();
                        if (addrRegInfo[dst_subReg].canUseImmed)
                        {
                            if (currOff % 0x20 == 0 &&
                                (currOff + addrRegInfo[dst_subReg].immAddrOff) <= 511 &&
                                (currOff + addrRegInfo[dst_subReg].immAddrOff) >= -512)
                            {
                                // create a new dst
                                G4_DstRegRegion tmpRgn(*dst);
                                G4_DstRegRegion *newDst = &tmpRgn;
                                newDst->setImmAddrOff(short(currOff + addrRegInfo[dst_subReg].immAddrOff));
                                inst->setDest(builder.createDstRegRegion(*newDst));
                                addrRegInfo[dst_subReg].usedImmed = true;
                            }
                            else
                            {
                                // if the offset can not be folded into all uses of a0.0, reverse the former folding
                                reverseOffsetProp(addrRegInfo, dst_subReg, 0, ii, iend);
                            }
                        }
                    }
                }
            }
        }

        // if a def lives out of this BB, we can not delete the defining inst
        for (unsigned i = 0; i < getNumAddrRegisters(); i++)
        {
            // reverse immed offset propagation
            reverseOffsetProp(addrRegInfo, i, 0, iend, iend);
        }
        // remove the ADD instructions that marked as dead
        for (ii = bb->begin(); ii != bb->end();)
        {
            G4_INST *inst = *ii;
            INST_LIST_ITER curr = ii++;
            if (inst->isDead())
            {
                bb->erase(curr);
            }
        }
    }

    delete [] addrRegInfo;
}
G4_SrcModifier Optimizer::mergeModifier(G4_Operand *src, G4_Operand *use)
{
    if ((src == NULL || !src->isSrcRegRegion())  && use && use->isSrcRegRegion()) {
        return use->asSrcRegRegion()->getModifier();
    }else if ((use == NULL || !use->isSrcRegRegion()) && src && src->isSrcRegRegion()) {
        return src->asSrcRegRegion()->getModifier();
    }else if (src && src->isSrcRegRegion() && use && use->isSrcRegRegion()) {
        G4_SrcModifier mod1 = src->asSrcRegRegion()->getModifier(), mod2 = use->asSrcRegRegion()->getModifier();
        if (mod2 == Mod_Abs || mod2 == Mod_Minus_Abs) {
            return mod2;
        }else if (mod2 == Mod_src_undef) {
            return mod1;
        }else{
            // mod2 == Minus
            if (mod1 == Mod_Minus) {
                return Mod_src_undef;
            }else if (mod1 == Mod_Abs) {
                return Mod_Minus_Abs;
            }else if (mod1 == Mod_Minus_Abs) {
                return Mod_Abs;
            }else{
                return mod2;
            }
        }
    }else{
        return Mod_src_undef;
    }
}

// Prevent sinking in presence of lifetime.end for any src op.
// For eg,
// add V33, V32, 0x1
// ...
// lifetime.end V32 <-- This prevents sinking of add to mov
// ...
// pseudo_kill V34 <-- This prevents hoisting of V34 to add dst
// mov V34, V33
//
static bool checkLifetime(G4_INST *defInst, G4_INST *inst)
{
    // Check whether current instruction ends any src opnd of op
    if (!inst->isLifeTimeEnd())
        return true;

    G4_RegVar *Var = GetTopDclFromRegRegion(inst->getSrc(0))->getRegVar();
    // Check whether lifetime op corresponds to any operand of current inst.
    if (defInst->getPredicate())
    {
        G4_RegVar *opndVar = defInst->getPredicate()->asPredicate()->getBase()->asRegVar();
        if (opndVar == Var)
            return false;
    }
    if (defInst->getCondMod())
    {
        G4_RegVar *opndVar = defInst->getCondMod()->asCondMod()->getBase()->asRegVar();
        if (opndVar == Var)
            return false;
    }
    if (defInst->getDst() &&
        !defInst->getDst()->isNullReg())
    {
        G4_RegVar *opndVar = GetTopDclFromRegRegion(defInst->getDst())->getRegVar();
        if (opndVar == Var)
            return false;
    }
    for (unsigned int srcOpnd = 0; srcOpnd < G4_MAX_SRCS; srcOpnd++)
    {
        G4_Operand *src = defInst->getSrc(srcOpnd);
        if (src && src->isSrcRegRegion())
        {
            G4_RegVar *opndVar = GetTopDclFromRegRegion(src)->getRegVar();
            if (opndVar == Var)
                return false;
        }
    }

    return true;
}

//
// Sink definition towards its use.
//
// For example, without sinking the def instruction once, use cannot
// be hoisted, since there is a data dependency between the middle
// instruction and the last move.
//
// def: shr (1) V39(0,0)<1>:ud V38(0,0)<0;1,0>:d 0x4:w {Align1, Q1}
//      mov (8) V68(0,0)<1>:ud r0.0<8;8,1>:ud {Align1, NoMask}
// use: mov (1) V68(0,2)<1>:ud V39(0,0)<0;1,0>:ud {Align1, NoMask}
//
// after sinking, it becomes
//
//      mov (8) V68(0,0)<1>:ud r0.0<8;8,1>:ud {Align1, NoMask}
// def: shr (1) V39(0,0)<1>:ud V38(0,0)<0;1,0>:d 0x4:w {Align1, Q1}
// use: mov (1) V68(0,2)<1>:ud V39(0,0)<0;1,0>:ud {Align1, NoMask}
//
// which makes local def hoisting possible.
//
// The third argument 'other' points to the first instruction (upwards) that
// has data-dependency with the use instruction.
//
static bool canSink(G4_BB *bb, INST_LIST_RITER revIter, INST_LIST_RITER other)
{
    // The use instruction.
    G4_INST *inst = *revIter;

    // Currently we do not handle multiple definition for this optimization.
    if (inst->def_size() != 1)
        return false;

    // Find its def instruction.
    G4_INST *defInst = inst->def_back().first;

    ASSERT_USER(*other != defInst, "iterator points to def already");

    // Walk up to check if sinking is safe.
    INST_LIST_RITER it = other;

    while (*it != defInst)
    {
       if ((*it)->isWAWdep(defInst) ||
           (*it)->isRAWdep(defInst) ||
           (*it)->isWARdep(defInst))
           return false;

       if (!checkLifetime(defInst, *it))
           return false;

       // move towards to defInst.
       ++it;
    }

    // At this point, there is no data dependency and sinking is safe.
    //
    // We do sinking right here.
    //
    ASSERT_USER(*it == defInst, "iterator out of sync");

    // Both 'other' and 'it' are reverse iterators, and sinking is through
    // forward iterators. The fisrt base should not be decremented by 1,
    // otherwise, the instruction will be inserted before not after.
    bb->insertBefore(other.base(), defInst);
    bb->erase(--it.base());

    return true;
}

static bool canHoist(FlowGraph &fg, G4_BB *bb, INST_LIST_RITER revIter)
{
    G4_INST *inst = *revIter;

    if (inst->isMixedMode() && fg.builder->getOption(vISA_DisableleHFOpt))
        return false;
    // Cannot hoist if this is not a move, or it is a global operand.
    if  (inst->opcode() != G4_mov ||
         fg.globalOpndHT.isOpndGlobal(inst->getSrc(0)) ||
         !inst->canHoist(!bb->isAllLaneActive(), fg.builder->getOptions()))
    {
         return false;
    }

    // Do not do def-hoisting for setting flags which is likely to increase flag
    // register pressure.
    if (auto Dst = inst->getDst())
    {
        G4_Declare *Dcl = Dst->getTopDcl();
        if (Dcl && Dcl->getRegFile() == G4_RegFileKind::G4_FLAG)
        {
            return false;
        }

        if (!fg.builder->hasByteALU() && Dst->getTypeSize() == 1)
        {
            return false;
        }
    }

    // Now check each definition of src(0)
    for (auto I = inst->def_begin(), E = inst->def_end(); I != E; ++I)
    {
        ASSERT_USER(I->second == Opnd_src0, "invalid use-def chain");
        if (!inst->canHoistTo(I->first, !bb->isAllLaneActive()))
            return false;

        auto defInst = I->first;
        if (fg.globalOpndHT.isOpndGlobal(defInst->getDst()))
        {
            return false;
        }

        // Further check data-dependency, that is, no other instruction
        // should have WAR or WAW dependency with this inst.
        //
        //   defInst
        //
        //   other inst
        //
        //   inst <-- revIter
        //
        INST_LIST_RITER other = revIter;
        ++other;

        // Measure the distance in between
        unsigned distance = 0;

        // Walkup until hits its defining instruction.
        while (*other != I->first)
        {
            // FIXME: remove duplicate computations for multiple definitions.
            if (inst->isWAWdep(*other) || inst->isWARdep(*other))
            {
                break;
            }
            ++other;
            ++distance;
        }

        // Check the distance first, if this is too far then the following
        // sinking optimization is very expensive.
        #define MAX_DEF_HOIST_DIST 160
        if (distance > MAX_DEF_HOIST_DIST)
            return false;

        // There is a data dependency.
        if (*other != I->first)
        {
            // check if sinking is possible.
            if (!canSink(bb, revIter, other))
                return false;
        }
    }
    return true;
}

static G4_DstRegRegion *buildNewDstOperand(FlowGraph &fg, G4_INST *inst, G4_INST *defInst)
{
    G4_Operand *src = inst->getSrc(0);
    G4_DstRegRegion *dst = inst->getDst();

    G4_Type srcType = src->getType();
    G4_Type dstType = dst->getType();
    G4_DstRegRegion *dstRegion = dst;
    bool indirectDst = (dstRegion->getRegAccess() != Direct);
    unsigned char srcElSize = (unsigned char)TypeSize(srcType);

    G4_DstRegRegion *defDstRegion = defInst->getDst();
    G4_DstRegRegion *newDstOpnd = dst;

    unsigned char defDstElSize = (unsigned char)defDstRegion->getTypeSize();
    G4_CmpRelation rel = src->compareOperand(defDstRegion);
    G4_Type defDstType = defDstRegion->getType();

    unsigned char dstElSize = (unsigned char)TypeSize(dstType);
    unsigned short dstHS = dst->getHorzStride();

    if (rel == Rel_gt || srcElSize != defDstElSize ||
        (defInst->getSaturate() && srcType != defDstType) || inst->isRawMov() ||
        (dstType != defDstType &&
         (IS_FTYPE(defDstType) || (IS_FTYPE(dstType) && defDstType != srcType))))
    {
        unsigned short regOff = 0, subRegOff = 0;
        if (rel == Rel_gt)
        {
            // compute new dst for defInst
            // get dst portion based on src region
            unsigned defDstLB = defDstRegion->getLeftBound();

            unsigned srcLB = src->getLeftBound();
            const RegionDesc *srcRegionDesc = src->asSrcRegRegion()->getRegion();
            bool contRegion = srcRegionDesc->isContiguous(inst->getExecSize());

            uint32_t dist = defDstLB - srcLB, dstDist = 0, tempLen = 0;
            if (src->asSrcRegRegion()->isScalar() || contRegion)
            {
                // mov (1) V18(0,0)[1]:b 0x73:w [Align1]
                // mov (1) V18(0,1)[1]:b 0x61:w [Align1]
                // mov (1) V18(0,2)[1]:b 0x70:w [Align1]
                // mov (1) V18(0,3)[1]:b 0:w [Align1]
                // mov (1) V20(1,0)[1]:ud V18(0,0)[0;1,0]:ud [Align1]
                // length of subregoff part
                tempLen = dstRegion->getSubRegOff() * dstElSize + dist * dstHS;

                if (tempLen >= numEltPerGRF<Type_UB>())
                {
                    regOff = dst->getRegOff() + 1;
                    subRegOff = (unsigned short)((tempLen - numEltPerGRF<Type_UB>()) / defDstElSize);
                }
                else
                {
                    regOff = dst->getRegOff();
                    subRegOff = (unsigned short)tempLen / defDstElSize;
                }
            }
            else
            {
                // mov (16) V18(0,0)[1]:b 0x73:w [Align1]
                // mov (16) V18(0,16)[1]:b 0x61:w [Align1]
                // mov (16) V18(1,0)[1]:b 0x70:w [Align1]
                // mov (16) V18(1,16)[1]:b 0:w [Align1]
                // mov (32) V20(1,0)[1]:b V18(0,0)[32;16,1]:b [Align1]
                // mov (32) V20(2,0)[1]:b V18(0,16)[32;16,1]:b [Align1]

                // Compute the linear index of the first element from defInst's dst
                // in useInst's src.
                //
                // mov <2> V50(0, 14)<1>:b 0xa:w                  <- defInst
                // mov <16> V51(0, 9)<2>:b V50(0, 0)<0; 16, 2>:b  <- useInst
                //
                // Starting from left bound difference, dist = 14.
                //
                // FirstEltIndex is 7 = 14 / 2. With this index, we can compute
                // the register offset and sub-register offset in useInst's dst.
                //
                // In the above example, there is only a single row. In general
                // there may have multiple rows in useInst's src region.
                //
                // (1) convert difference in number of elements.
                MUST_BE_TRUE(dist % srcElSize == 0, "unexpected difference");
                dist = dist / srcElSize;

                // (2) compute row and column index, by default a single row.
                unsigned rowIndex = 0, colIndex = dist;
                if (srcRegionDesc->vertStride > 0)
                {
                    rowIndex = dist / srcRegionDesc->vertStride;
                    colIndex = dist % srcRegionDesc->vertStride;
                }

                // (3) compute the final linear index.
                MUST_BE_TRUE(srcRegionDesc->horzStride == 0 || colIndex % srcRegionDesc->horzStride == 0, "invalid region");
                unsigned FirstEltIndex = rowIndex * srcRegionDesc->width +
                                         (srcRegionDesc->horzStride == 0 ? colIndex : (colIndex / srcRegionDesc->horzStride));

                // (4) compute the register and subregister offet in useInst's dst.
                dstDist = FirstEltIndex * dstElSize * dstHS;
                tempLen = dstDist + dst->getSubRegOff() * dstElSize;
                regOff = (unsigned short)(dst->getRegOff() +
                    tempLen / numEltPerGRF<Type_UB>());

                subRegOff = (unsigned short)(tempLen % numEltPerGRF<Type_UB>()) / defDstElSize;
            }

            unsigned short defDstHS = defDstRegion->getHorzStride();
            if (!indirectDst)
            {
                newDstOpnd = fg.builder->createDst(
                    dst->getBase(),
                    regOff,
                    subRegOff,
                    dstHS * defDstHS, defDstRegion->getType());
            }
            else
            {
                newDstOpnd = fg.builder->createIndirectDst(
                    dst->getBase(),
                    dst->getSubRegOff(),
                    dstHS * defDstHS, defDstRegion->getType(),
                    (int16_t)dst->getAddrImm() + tempLen);
            }
        }
        else
        {
            unsigned char scale = dstElSize / defDstElSize;

            // If instruction that gets def hoisted is just re-interpretation of bits,
            // doesn't do conversion, then I think original type of the defInst
            // should be used. This preserves the original behavior.
            //
            // mov (8) V57(0,0)[1]:hf V52(0,0)[8;8,1]:f [Align1, Q1] %21
            // mov (8) V58(0,0)[1]:w  V59(0,0)[8;8,1]:w [Align1, Q1] %22
            if (dst->getType() == src->getType())
            {
                if (!indirectDst)
                {
                    newDstOpnd = fg.builder->createDst(
                        dst->getBase(),
                        dst->getRegOff(),
                        (scale == 0 ?
                        dst->getSubRegOff() / (defDstElSize / dstElSize) :
                        dst->getSubRegOff() * scale),
                        dstHS, defDstRegion->getType());
                }
                else
                {
                    newDstOpnd = fg.builder->createIndirectDst(
                        dst->getBase(),
                        dst->getSubRegOff(),
                        dstHS, defDstRegion->getType(), dst->getAddrImm());
                }
            }
            else
            {
                if (!indirectDst)
                {
                    newDstOpnd = fg.builder->createDst(
                        dst->getBase(),
                        dst->getRegOff(),
                        dst->getSubRegOff(), dstHS,
                        dst->getType());
                }
                else
                {
                    newDstOpnd = fg.builder->createIndirectDst(
                    dst->getBase(),
                    dst->getSubRegOff(), dstHS,
                    dst->getType(), dst->getAddrImm());
                }
            }
        }
    }
    return newDstOpnd;
}

//
//  inst0:   op0  rx<2>(0), ry0, ry1
//  inst1:   op1  rx<2>(1), ry2, ry3
//  inst2:   mov  rz<1>(0), rx<0, 0>(8; 8, 1)
//
// ==>
//
//  inst0:   op0, rz<2>(0), ry0, ry1
//  inst1:   op1, rz<2>(1), ry2, ry3
//  inst2:   mov  rz<1>(0), rx<0, 0>(8; 8, 1)  (to be deleted)
//
// Def-use/use-def chains will be updated as follows:
//
// (0) all use-defs remain the same for inst0 and inst1.
//
// (1) remove all use-defs of inst2. (They must be from inst0 and inst1,
//     which is the pre-condition of doHoisting.)
//
// (2) remove all def-uses of inst0 and inst1 from dst.
//
// (3) remove all def-uses of inst2.
//
// (4) add new def-uses to inst0 and inst1.
//
static void doHoisting(FlowGraph &fg, G4_BB *bb, INST_LIST_RITER revIter)
{
    G4_INST *inst = *revIter;

    for (auto I = inst->def_begin(), E = inst->def_end(); I != E; ++I)
    {
        G4_INST *defInst = I->first;

        // Build a new dst operand for each def instruction.
        G4_DstRegRegion *newDst = buildNewDstOperand(fg, inst, defInst);

        // Update the defInst with a new operand and set attributes properly.
        if (inst->def_size() == 1)
        {
            defInst->setDest(newDst);
        }
        else
        {
            defInst->setDest(fg.builder->duplicateOperand(newDst)->asDstRegRegion());
        }

        // (4) for each def-use of inst, add it to defInst, if it is
        // an effective use.
        for (auto UI = inst->use_begin(), UE = inst->use_end(); UI != UE; ++UI)
        {
            G4_Operand *UseOpnd = UI->first->getOperand(UI->second);
            // this comparison is necessary, since uses of inst's dst may be
            // different from those from defInst's dst.
            G4_CmpRelation rel = defInst->getDst()->compareOperand(UseOpnd);
            if (rel != Rel_disjoint)
            {
                defInst->addDefUse(UI->first, UI->second);
            }
        }

        if (inst->getPredicate())
        {
            ASSERT_USER(inst->def_size() == 1, "multiple defs not implemented");
            // Remove existing definitions on defInst[opnd_pred].
            defInst->removeDefUse(Opnd_pred);

            defInst->setPredicate(inst->getPredicate());

            // (4) Transfer definitions of inst[opnd_pred] to definitions of
            // defInst[opnd_pred].
            inst->transferDef(defInst, Opnd_pred, Opnd_pred);
        }
        if (inst->getSrc(0)->asSrcRegRegion()->isScalar() &&
            inst->getExecSize() > g4::SIMD1)
        {
            defInst->setExecSize(G4_ExecSize(defInst->getExecSize() * inst->getExecSize()));
        }
        defInst->setSaturate(inst->getSaturate() || defInst->getSaturate());
        if (!bb->isAllLaneActive())
        {
            // set writeEnable of dstInst to be off
            defInst->setOptions((defInst->getOption() & ~0xFFF000C) |
                                (inst->getMaskOption()));
        }
    }

    // (1), (2), (3) Remove all defs/uses and it is ready to be deleted.
    inst->removeAllDefs();
    inst->removeAllUses();
}

void Optimizer::newLocalDefHoisting()
{
    unsigned numDefHoisted = 0;
    for (auto bb : fg)
    {
        for (auto I = bb->rbegin(); I != bb->rend(); /* empty */)
        {
            if (canHoist(fg, bb, I))
            {
                doHoisting(fg, bb, I);
                ++numDefHoisted;

                // list::erase does not take a reverse_iterator.
                //
                // The base iterator is an iterator of the same type as the one
                // used to construct the reverse_iterator, but pointing to the
                // element next to the one that the reverse_iterator is currently
                // pointing to (a reverse_iterator has always an offset of -1
                // with respect to its base iterator).
                I = INST_LIST::reverse_iterator(bb->erase(--I.base()));
            }
            else
            {
                ++I;
            }
        }
    }

    if (builder.getOption(vISA_OptReport))
    {
        std::ofstream optReport;
        getOptReportStream(optReport, builder.getOptions());
        optReport << "             === Local Definition Hoisting Optimization ===\n";
        optReport << "Number of defs hoisted: " << numDefHoisted << "\n";
        closeOptReportStream(optReport);
    }
}

//  find a common (integer) type for constant folding.  The rules are:
//  -- both types must be int
//  -- Q and UQ are not folded
//  -- UD if one of the type is UD
//  -- D otherwise
//
//  returns Type_UNDEF if no appropriate type can be found
//
static G4_Type findConstFoldCommonType(G4_Type type1, G4_Type type2)
{
    if (IS_TYPE_INT(type1) && IS_TYPE_INT(type2))
    {
        if (TypeSize(type1) == 8 || TypeSize(type2) == 8)
        {
            return Type_UNDEF;
        }
        if (type1 == Type_UD || type2 == Type_UD)
        {
            return Type_UD;
        }
        else
        {
            return Type_D;
        }
    }
    return Type_UNDEF;
}

// Do the following algebraic simplification:
// - mul v, src0, 0 ==> 0, commutative
// - and v, src0, 0 ==> 0, commutative
// - mul v, src0, 1 ==> src0, commutative
// - shl v, src0, 0 ==> src0
// - shr v, src0, 0 ==> src0
// - asr v, src0, 0 ==> src0
// - add v, src0, 0 ==> src0, commutative
void Optimizer::doSimplification(G4_INST *inst)
{
    // Just handle following commonly used ops for now.
    if (inst->opcode() != G4_mul && inst->opcode() != G4_and &&
        inst->opcode() != G4_add && inst->opcode() != G4_shl &&
        inst->opcode() != G4_shr && inst->opcode() != G4_asr &&
        inst->opcode() != G4_mov)
    {
        return;
    }


    // Perform 'mov' to 'movi' transform when it's a 'mov' of
    // - simd8
    // - it's a raw mov
    // - dst is within a single GRF.
    // - src uses VxH indirect access.
    // - src is within one GRF.
    // - indices to src are all within src.
    if (inst->opcode() == G4_mov && inst->getExecSize() == g4::SIMD8 &&
        inst->isRawMov() && inst->getDst() &&
        !inst->getDst()->asDstRegRegion()->isCrossGRFDst() &&
        inst->getSrc(0) && inst->getSrc(0)->isSrcRegRegion() &&
        inst->getSrc(0)->asSrcRegRegion()->isIndirect() &&
        inst->getSrc(0)->asSrcRegRegion()->getRegion()->isRegionWH() &&
        inst->getSrc(0)->asSrcRegRegion()->getRegion()->width == 1 &&
        // destination stride in bytes must be equal to the source element size
        // in bytes.
        inst->getSrc(0)->getTypeSize() ==
            inst->getDst()->getTypeSize() *
                inst->getDst()->asDstRegRegion()->getHorzStride())
    {
        // Convert 'mov' to 'movi' if the following conditions are met.

        auto getSingleDefInst = [](G4_INST *UI,
                                   Gen4_Operand_Number OpndNum)
            -> G4_INST * {
            G4_INST *Def = nullptr;
            for (auto I = UI->def_begin(), E = UI->def_end(); I != E; ++I) {
                if (I->second != OpndNum)
                    continue;
                if (Def) {
                    // Not single defined, bail out
                    Def = nullptr;
                    break;
                }
                Def = I->first;
            }
            return Def;
        };

        unsigned SrcSizeInBytes =
            inst->getExecSize() * inst->getSrc(0)->getTypeSize();
        if (SrcSizeInBytes == numEltPerGRF<Type_UB>()/2 ||
            SrcSizeInBytes == numEltPerGRF<Type_UB>())
        {
            G4_INST *LEA = getSingleDefInst(inst, Opnd_src0);
            if (LEA && LEA->opcode() == G4_add &&
                LEA->getExecSize() == inst->getExecSize()) {
                G4_Operand *Op0 = LEA->getSrc(0);
                G4_Operand *Op1 = LEA->getSrc(1);
                G4_Declare *Dcl = nullptr;
                int Offset = 0;
                if (Op0->isAddrExp()) {
                    G4_AddrExp *AE = Op0->asAddrExp();
                    Dcl = AE->getRegVar()->getDeclare();
                    Offset = AE->getOffset();
                }
                if (Dcl && (Offset % SrcSizeInBytes) == 0 &&
                    Op1->isImm() && Op1->getType() == Type_UV) {
                    // Immeidates in 'uv' ensures each element is a
                    // byte-offset within half-GRF.
                    G4_SubReg_Align SubAlign = GRFALIGN;
                    if (SrcSizeInBytes <= numEltPerGRF<Type_UB>()/2u)
                        SubAlign = (G4_SubReg_Align)(numEltPerGRF<Type_UW>()/2);
                    inst->setOpcode(G4_movi);
                    if (!Dcl->isEvenAlign() && Dcl->getSubRegAlign() != GRFALIGN)
                    {
                        Dcl->setSubRegAlign(SubAlign);
                    }
                    const RegionDesc *rd = builder.getRegionStride1();
                    inst->getSrc(0)->asSrcRegRegion()->setRegion(rd);
                    // Set subreg alignment for the address variable.
                    Dcl =
                        LEA->getDst()->getBase()->asRegVar()->getDeclare();
                    assert(Dcl->getRegFile() == G4_ADDRESS &&
                           "Address variable is required.");
                    Dcl->setSubRegAlign(Eight_Word);
                }
            }
        }
    }

    auto isInteger = [](G4_Operand *opnd, int64_t val)
    {
        if (opnd && IS_TYPE_INT(opnd->getType()) && !opnd->isRelocImm())
        {
            return opnd->isImm() && opnd->asImm()->getInt() == val;
        }
        return false;
    };

    G4_Operand *src0 = inst->getSrc(0);
    G4_Operand *src1 = inst->getSrc(1);
    G4_Operand *newSrc = nullptr;
    if (inst->opcode() == G4_mul || inst->opcode() == G4_and)
    {
        if (isInteger(src1, 0))
        {
            inst->removeDefUse(Opnd_src0);
            newSrc = builder.createImm(0, Type_W);
        }
        else if (isInteger(src0, 0))
        {
            inst->removeDefUse(Opnd_src1);
            newSrc = builder.createImm(0, Type_W);
        }
        else if (inst->opcode() == G4_mul)
        {
            if (isInteger(src1, 1))
            {
                newSrc = src0;
            }
            else if (isInteger(src0, 1))
            {
                inst->swapDefUse();
                newSrc = src1;
            }
        }
    }
    else if (inst->opcode() == G4_shl || inst->opcode() == G4_shr ||
             inst->opcode() == G4_asr || inst->opcode() == G4_add)
    {
        if (isInteger(src1, 0))
        {
            newSrc = src0;
        }
        else if (inst->opcode() == G4_add && isInteger(src0, 0))
        {
            inst->swapDefUse();
            newSrc = src1;
        }
    }

    if (newSrc != nullptr)
    {
        inst->setOpcode(G4_mov);
        if (newSrc != src0)
        {
            inst->setSrc(newSrc, 0);
        }
        inst->setSrc(nullptr, 1);
    }
}

//
// Do very simple const only reassociation to fold const values
// e.g.,
// V2 = V1 + K1
// V3 = V2 + K2
// -->
// V3 = V1 + (K1 + K2)
// we only search one level (+ and +, * and *) for now as more complex reassociation should be taken
// care of by IGC earlier.
// also only do it for integer type for now
//
void Optimizer::reassociateConst()
{
    for (auto BB : fg)
    {
        for (auto iter = BB->begin(), iterEnd = BB->end(); iter != iterEnd; ++iter)
        {
            G4_INST* inst = *iter;
            if (inst->opcode() != G4_add && inst->opcode() != G4_mul)
            {
                continue;
            }
            auto isSrc1Const = [](G4_INST* inst)
            {
                if (!IS_INT(inst->getDst()->getType()))
                {
                    return false;
                }
                if (!inst->getSrc(0)->isImm() && inst->getSrc(1)->isImm())
                {
                    return true;
                }
                else if (inst->getSrc(0)->isImm() && !inst->getSrc(1)->isImm())
                {
                    inst->swapSrc(0, 1);
                    inst->swapDefUse(); // swap def/use for src0 and src1
                    return true;
                }
                return false;
            };
            if (!isSrc1Const(inst))
            {
                continue;
            }
            auto src0Def = inst->getSingleDef(Opnd_src0);
            if (!src0Def)
            {
                continue;
            }

            auto isGoodSrc0Def = [isSrc1Const](G4_INST* def, G4_INST* use)
            {
                assert(use->getSrc(0)->isSrcRegRegion() && "expect src0 to be src region");
                if (def->opcode() != use->opcode())
                {
                    return false;
                }
                if (def->getSaturate() || def->getPredicate() || def->getCondMod() ||
                    def->getMaskOffset() != use->getMaskOffset())
                {
                    return false;
                }
                if (!isSrc1Const(def))
                {
                    return false;
                }
                auto useSrc = use->getSrc(0)->asSrcRegRegion();
                if (useSrc->hasModifier() || def->getDst()->getTypeSize() != useSrc->getTypeSize() ||
                    def->getDst()->compareOperand(useSrc) != Rel_eq)
                {
                    // make sure def fully defines use and have the same integer type size (signed-ness should not matter)
                    return false;
                }
                if (def->getDst()->compareOperand(def->getSrc(0)) != Rel_disjoint)
                {
                    // can't sink source if def overwrites it
                    return false;
                }
                // additionally check for the use inst that dst type size is >= src type size
                // otherwise the first add may truncate upper bits due to overflow,
                // which makes reassociation unsafe
                if (useSrc->getTypeSize() < use->getDst()->getTypeSize())
                {
                    return false;
                }

                return true;
            };

            if (isGoodSrc0Def(src0Def, inst) && !chkFwdOutputHazard(src0Def, iter))
            {
                //std::cout << "reassociate: \n";
                //src0Def->dump();
                //inst->dump();
                G4_Imm* constOne = src0Def->getSrc(1)->asImm();
                G4_Imm* constTwo = inst->getSrc(1)->asImm();
                G4_Imm* resultImm = foldConstVal(constOne, constTwo, inst->opcode());

                if (resultImm)
                {
                    inst->setSrc(builder.duplicateOperand(src0Def->getSrc(0)), 0);
                    inst->setSrc(resultImm, 1);
                    inst->removeDefUse(Opnd_src0);
                    src0Def->copyDef(inst, Opnd_src0, Opnd_src0);
                    //ToDo: remove this when DCE pass is enabled
                    if (src0Def->use_size() == 0 && !fg.globalOpndHT.isOpndGlobal(src0Def->getDst()) &&
                        !src0Def->getDst()->isIndirect())
                    {
                        src0Def->markDead();
                        src0Def->removeAllDefs();
                    }
                    //std::cout << "--> new inst:\t";
                    //inst->dump();
                }
            }
        }
        BB->erase(std::remove_if (BB->begin(), BB->end(), [](G4_INST* inst) { return inst->isDead(); }),
            BB->end());
    }

}

// helper function to fold BinOp with two immediate operands
// supported opcodes are given below in doConsFolding
// returns nullptr if the two constants may not be folded
G4_Imm* Optimizer::foldConstVal(G4_Imm* const1, G4_Imm* const2, G4_opcode op)
{
    bool isNonQInt = IS_TYPE_INT(const1->getType()) && IS_TYPE_INT(const2->getType()) &&
        !IS_QTYPE(const1->getType()) && !IS_QTYPE(const2->getType());

    if (!isNonQInt)
    {
        return nullptr;
    }

    G4_Type src0T = const1->getType(), src1T = const2->getType(), resultType = src0T;

    if (op == G4_mul || op == G4_add || op == G4_and || op == G4_xor || op == G4_or)
    {
        resultType = findConstFoldCommonType(src0T, src1T);
        if (resultType == Type_UNDEF)
        {
            return nullptr;
        }

        int64_t res;
        switch (op)
        {
        case G4_and:
            res = (int64_t)(const1->getInt()) & (int64_t)(const2->getInt());
            break;

        case G4_xor:
            res = (int64_t)(const1->getInt()) ^ (int64_t)(const2->getInt());
            break;

        case G4_or:
            res = (int64_t)(const1->getInt()) | (int64_t)(const2->getInt());
            break;

        case G4_add:
            res = (int64_t)(const1->getInt()) + (int64_t)(const2->getInt());
            break;

        case G4_mul:
            res = (int64_t)(const1->getInt()) * (int64_t)(const2->getInt());
            break;

        default:
            return nullptr;
        }

        // result type is either D or UD
        // don't fold if the value overflows D/UD
        if (!G4_Imm::isInTypeRange(res, resultType))
        {
            return nullptr;
        }
        return builder.createImmWithLowerType(res, resultType);
    }
    else
    {
        uint32_t shift = const2->getInt() % 32;

        if (op == G4_shl || op == G4_shr)
        {
            uint32_t value = (uint32_t)const1->getInt();
            // set result type to D/UD as it may overflow W. If the value fits the type will be lowered later
            // source type matters here since it affects sign extension
            resultType = IS_SIGNED_INT(resultType) ? Type_D : Type_UD;
            int64_t res = op == G4_shl ?
                ((int64_t)value) << shift :
                value >> shift;
            if (!G4_Imm::isInTypeRange(res, resultType))
            {
                return nullptr;
            }

            return builder.createImmWithLowerType(res, resultType);
        }
        else if (op == G4_asr)
        {
            if (IS_SIGNED_INT(resultType))
            {
                int64_t value = const1->getInt();
                int64_t res = value >> shift;
                return builder.createImmWithLowerType(res, resultType);
            }
            else
            {
                uint64_t value = const1->getInt();
                uint64_t res = value >> shift;
                return builder.createImmWithLowerType(res, resultType);
            }
        }
    }
    return nullptr;
}



// Currently constant folding is done for the following code patterns:
//
// - op v, imm, imm
//    where op is shl, shr, asr, or, xor, and, add, mul
// Restrictions:
// - operand type cannot be float or Q/UQ
// - saturation is not allowed
void Optimizer::doConsFolding(G4_INST *inst)
{
    G4_Operand *src0, *src1;

    src0 = inst->getSrc(0);
    src1 = inst->getSrc(1);

    G4_Imm *newSrc = nullptr;

    if (src0 && src0->isImm() && !src0->isRelocImm() &&
        src1 && src1->isImm() && !src1->isRelocImm() &&
        inst->getSaturate() == false)
    {
        newSrc = foldConstVal(src0->asImm(), src1->asImm(), inst->opcode());
    }

    if (newSrc != NULL)
    {
        // change instruction into a MOV
        inst->setOpcode(G4_mov);
        inst->setSrc(newSrc, 0);
        inst->setSrc(NULL, 1);
    }
}

static void hoistUseInst(G4_BB *bb, G4_INST *inst , INST_LIST_ITER forwardIter, bool &canRemove)
{
    // check if we can move the use inst up.
    // currently we do not handle multiple use for this optimization
    G4_INST* useInst = inst->use_front().first;
    if (inst->hasOneUse())
    {
        forwardIter--;
        INST_LIST_ITER backwardIter = forwardIter;
        INST_LIST_ITER instListEnd = bb->end();
        while (backwardIter != instListEnd &&
               *backwardIter != useInst)
        {
            backwardIter++;
        }

        INST_LIST_ITER useInstIter = backwardIter;
        backwardIter--;
        while (backwardIter != forwardIter)
        {
            if (useInst->isWAWdep(*backwardIter) ||
                useInst->isRAWdep(*backwardIter) ||
                useInst->isWARdep(*backwardIter))
            {
                break;
            }
            backwardIter--;
        }
        if (backwardIter != forwardIter)
        {
            canRemove = false;
        }
        else
        {
            // hoisting
            backwardIter++;
            bb->insertBefore(backwardIter, useInst);
            bb->erase(useInstIter);
        }
    }
    else
    {
        canRemove = false;
    }
}

// conver modifier(imm)
template<class T>
static
typename std::enable_if<std::is_floating_point<T>::value, T>::type
getImmValue(T imm, G4_SrcModifier modifier)
{
    switch (modifier)
    {
    case Mod_Minus:
        return -imm;
    case Mod_Abs:
        return std::abs(imm);
    case Mod_Minus_Abs:
        return -(std::abs(imm));
    case Mod_Not:
        MUST_BE_TRUE(false, "unexpected not modifier for floating types");
        return imm;
    default:
        return imm;
    }
}

template<class T>
static
typename std::enable_if<std::is_integral<T>::value, T>::type
getImmValue(T imm, G4_SrcModifier modifier)
{
    switch (modifier)
    {
    case Mod_Minus:
        return -imm;
    case Mod_Abs:
        return std::llabs(imm);
    case Mod_Minus_Abs:
        return -(std::llabs(imm));
    case Mod_Not:
        return ~imm;
    default:
        return imm;
    }
}

// Source operand of the MOV instruction is already known being able to be
// propagated into all its uses. But, due to the dependency issue, it cannot be
// propagated. Try to propagate the type if a narrower type could be used.
static bool propagateType(IR_Builder &Builder, G4_BB *BB, G4_INST *Mov, G4_INST::MovType MT) {
    // Only propagate type if a narrower type could be used.
    if (MT != G4_INST::ZExt && MT != G4_INST::SExt)
        return false;

    G4_DstRegRegion *Dst = Mov->getDst();
    if (Dst->isIndirect())
        return false;

    // Check all propagation types are the same.
    G4_Type PT = Type_UNDEF;
    for (auto UI = Mov->use_begin(), UE = Mov->use_end(); UI != UE; ++UI) {
        auto Use = UI->first;
        auto OpndNum = UI->second;
        auto Opnd = Use->getOperand(OpndNum);
        if (!Opnd->isSrcRegRegion())
          return false;
        if (Opnd->asSrcRegRegion()->isIndirect())
          return false;
        G4_Type PropType = Use->getPropType(OpndNum, MT, Mov);
        if (PropType == Type_UNDEF)
          return false;
        if (PT != Type_UNDEF && PT != PropType)
          return false;
        PT = PropType;
    }
    if (PT == Type_UNDEF)
        return false;
    // Create a new destination of MOV of the propagation type.
    unsigned NumElt = Mov->getExecSize();
    auto NewDcl = Builder.createTempVar(NumElt, PT, Any);
    auto NewDst = Builder.Create_Dst_Opnd_From_Dcl(NewDcl, Dst->getHorzStride());
    Mov->setDest(NewDst);
    // Propagate type
    for (auto UI = Mov->use_begin(), UE = Mov->use_end(); UI != UE; ++UI) {
        auto Use = UI->first;
        auto OpndNum = UI->second;
        auto Opnd = Use->getOperand(OpndNum)->asSrcRegRegion();
        auto NewOpnd = Builder.Create_Src_Opnd_From_Dcl(NewDcl, Opnd->getRegion());
        Use->setSrc(NewOpnd, OpndNum - 1);
    }
    return true;
}

void Optimizer::newLocalCopyPropagation()
{
    BB_LIST_ITER ib, bend(fg.end());

    for (ib = fg.begin(); ib != bend; ++ib)
    {
        G4_BB* bb = *ib;

        bb->resetLocalId();

        INST_LIST_ITER ii = bb->begin(), iend(bb->end());
        while (ii != iend)
        {
            G4_INST *inst = *ii;
            G4_Operand *dst = inst->getDst();
            if (!dst)
            {
                ii++;
                continue;
            }

            doConsFolding(inst);
            doSimplification(inst);

            G4_INST::MovType MT = inst->canPropagate();
            // Skip super mov.
            if (MT == G4_INST::SuperMov) {
                ii++;
                continue;
            }

            if (!isCopyPropProfitable(inst))
            {
                ++ii;
                continue;
            }
            bool canRemove = true;

            // check if each use may be copy propagated.
            USE_EDGE_LIST_ITER iter, iend1(inst->use_end());
            for (iter = inst->use_begin(); iter != iend1; iter++)
            {
                G4_INST *useInst = iter->first;
                Gen4_Operand_Number opndNum = iter->second;

                if (inst->getDst()->isA0() && useInst->isSplitSend() && VISA_WA_CHECK(builder.getPWaTable(), WaSendSEnableIndirectMsgDesc))
                {
                    canRemove = false;
                    break;
                }

                if (!inst->canPropagateTo(useInst, opndNum, MT, !bb->isAllLaneActive()))
                {
                    canRemove = false;
                    break;
                }

                // Make sure there is no lifetime.end for src0 of the move inst
                INST_LIST_ITER cpIter = ii;
                cpIter++;
                while (*cpIter != useInst)
                {
                    // Detect patterns like:
                    //
                    // mov A, B
                    // ...
                    // lifetime.end B
                    // op C, A, D
                    //
                    // Because of presence of lifetime.end B, copy propagation for inst
                    // mov A, B
                    // cannot be done

                    if ((*cpIter)->isLifeTimeEnd())
                    {
                        // Check whether lifetime end is for same opnd
                        G4_Declare* lifetimeEndTopDcl = GetTopDclFromRegRegion((*cpIter)->getSrc(0));
                        G4_Declare* curInstDstTopDcl = GetTopDclFromRegRegion((*ii)->getDst());

                        if (lifetimeEndTopDcl == curInstDstTopDcl)
                        {
                            canRemove = false;
                            break;
                        }
                    }

                    cpIter++;
                }
            }

            if (canRemove && inst->getSrc(0)->isSrcRegRegion())
            {
                // check for anti-dependencies for src0 of the move instruction
                bool def_use_in_between = false;

                G4_INST* lastUse = inst->use_front().first;
                for (USE_EDGE_LIST_ITER
                        iter = inst->use_begin(),
                        uend = inst->use_end(); iter != uend; ++iter)
                {
                    G4_INST* useInst = iter->first;
                    if (useInst->getLocalId() > lastUse->getLocalId())
                    {
                        lastUse = useInst;
                    }
                }

                INST_LIST_ITER forwardIter = ii;
                forwardIter++;
                INST_LIST_ITER instListEnd = bb->end();

                while (!def_use_in_between &&
                       forwardIter != instListEnd &&
                       *forwardIter != lastUse)
                {
                    if ((*forwardIter)->isWARdep(inst))
                    {
                        def_use_in_between = true;
                        break;
                    }
                    forwardIter++;
                }

                // check if hoisting is possible
                if (def_use_in_between)
                {
                    hoistUseInst(bb, inst, forwardIter, canRemove);
                }

                if (!canRemove) {
                    // Check whether the type could be propagated instead to demote
                    // type if possible.
                    propagateType(builder, bb, inst, MT);
                }
            }

            if (!canRemove)
            {
                ii++;
                continue;
            }

            G4_Operand *src = inst->getSrc(0);
            // do propagation
            for (iter = inst->use_begin(); iter != iend1; /* empty */)
            {
                G4_INST *useInst = (*iter).first;
                Gen4_Operand_Number opndNum = (*iter).second;
                G4_Operand *use = useInst->getOperand(opndNum);
                G4_Type propType = useInst->getPropType(opndNum, MT, inst);

                // replace use with def
                if (src->isImm())
                {
                    auto newImmVal = G4_Imm::typecastVals(src->asImm()->getImm(),
                        propType);
                    G4_Imm* newImm = builder.createImm(newImmVal,
                        propType);
                    G4_SrcModifier modifier = use->asSrcRegRegion()->getModifier();
                    if (modifier != Mod_src_undef)
                    {
                        if (IS_TYPE_FLOAT_ALL(propType))
                        {
                            if (propType == Type_DF)
                            {
                                double imm = getImmValue(newImm->getDouble(), modifier);
                                newImm = builder.createDFImm(imm);
                            }
                            else
                            {
                                float imm = getImmValue(newImm->getFloat(), modifier);
                                newImm = builder.createImm(imm);
                            }
                        }
                        else
                        {
                            int64_t imm = getImmValue(newImm->getImm(), modifier);
                            newImm = builder.createImm(imm, propType);
                        }
                    }
                    useInst->setSrc(newImm,
                                    opndNum - 1);
                }
                else
                {
                    if (use == NULL)
                    {
                        break;
                    }
                    G4_SrcModifier new_mod = mergeModifier(src, use);

                    unsigned use_elsize = use->getTypeSize();
                    unsigned dstElSize = inst->getDst()->getTypeSize();
                    const RegionDesc *rd = src->asSrcRegRegion()->getRegion();
                    G4_Operand *new_src_opnd = NULL;
                    bool new_src = false;
                    unsigned char scale = 1, newExecSize = useInst->getExecSize();

                    // Compute the composed region if exists.
                    auto getComposedRegion = [=](
                        unsigned dStride, unsigned ex1,
                        const RegionDesc *rd1, unsigned ex2,
                        const RegionDesc *rd2) -> const RegionDesc *
                    {
                        // Easy cases.
                        if (rd1->isScalar())
                            return rd1;
                        else if (rd2->isScalar())
                            return rd2;
                        else if (dStride == 1 && rd1->isContiguous(ex1))
                            return rd2;
                        else if (dStride == 1 && rd2->isContiguous(ex2))
                            return rd1;

                        // rd1 and rd2 must be single strided. Use a non-zero
                        // invalid stride value as the initial value, which
                        // simplifies and unifies the checking.
                        uint16_t stride1 = 64;
                        if (rd1->isContiguous(ex1))
                            stride1 = 1;
                        else
                            rd1->isSingleNonUnitStride(ex1, stride1);

                        uint16_t stride2 = 64;
                        if (rd2->isContiguous(ex2))
                            stride2 = 1;
                        else
                            rd2->isSingleNonUnitStride(ex2, stride2);

                        // All are single strided; the composition is the product of strides.
                        if (stride1 * stride2 * dStride <= 32)
                            return builder.createRegionDesc((uint16_t)ex2, stride1 * stride2 * dStride, 1, 0);

                        // Should be unreachable, since the legality check
                        // before should reject cases that are difficult to do
                        // composition. Assert?
                        return nullptr;
                    };

                    if (MT == G4_INST::Trunc)
                    {
                        G4_DstRegRegion *dst = inst->getDst();
                        G4_SrcRegRegion *src0 = src->asSrcRegRegion();
                        unsigned typeSizeRatio = src0->getTypeSize() / dst->getTypeSize();
                        unsigned numElt = src0->isScalar() ? 1 : inst->getExecSize() * typeSizeRatio;
                        // src0 region is guaranteed to be scalar/contiguous due to canPropagate() check earlier
                        const RegionDesc* region = src0->isScalar() ?
                            builder.getRegionScalar() :
                            builder.createRegionDesc(useInst->getExecSize(), (uint16_t)inst->getExecSize()
                            * typeSizeRatio, inst->getExecSize(),
                            (uint16_t)typeSizeRatio);
                        if (src0->isIndirect())
                        {
                            new_src_opnd = builder.createIndirectSrc(new_mod,
                                src0->getBase(), src0->getRegOff(), src0->getSubRegOff() * typeSizeRatio,
                                region, propType, src0->getAddrImm());
                        }
                        else
                        {
                            G4_Declare* newDcl = builder.createTempVar(numElt, inst->getDst()->getType(), Any);
                            newDcl->setAliasDeclare(src0->getBase()->asRegVar()->getDeclare(), 0);

                            new_src_opnd =
                                builder.createSrcRegRegion(new_mod, Direct,
                                newDcl->getRegVar(), src0->getRegOff(),
                                src0->getSubRegOff() * typeSizeRatio,
                                region, propType);
                        }
                        new_src = true;
                    }
                    else if (dstElSize < use_elsize)
                    {
                        // FIXME: How could this happen? Revisit later if
                        // NoMask is guaranteed.
                        // TODO!!! src is aligned to use type. -- should check this.
                        new_src = true;
                        scale = use_elsize/dstElSize;
                        unsigned short vs = rd->vertStride, wd = rd->width;
                        // packed word/byte
                        if (use->asSrcRegRegion()->isScalar())
                        {
                            rd = builder.getRegionScalar();
                        }
                        else if (inst->isComprInst() && vs == wd)
                        {
                            rd = builder.getRegionStride1();
                        }
                        else
                        {
                            rd = builder.createRegionDesc(vs/scale, wd/scale, 1);
                        }
                    }
                    else if (inst->getExecSize() < useInst->getExecSize() &&
                               rd && use->isSrcRegRegion())
                    {
                        unsigned dStride = inst->getDst()->getHorzStride();
                        const RegionDesc *rd2 = use->asSrcRegRegion()->getRegion();
                        if (auto compRd = getComposedRegion(dStride, inst->getExecSize(),
                                                            rd, newExecSize, rd2))
                        {
                            new_src = true;
                            rd = compRd;
                        }
                    }

                    if (new_mod != Mod_src_undef || new_src)
                    {
                        // For truncation case, new src operand is already built.
                        if (MT != G4_INST::Trunc) {
                            new_src_opnd = builder.createSrcRegRegion(
                                new_mod,
                                src->asSrcRegRegion()->getRegAccess(),
                                src->asSrcRegRegion()->getBase(),
                                src->asSrcRegRegion()->getRegOff(),
                                src->asSrcRegRegion()->getSubRegOff()/scale,
                                rd,
                                propType);
                            if (src->asSrcRegRegion()->getRegAccess() != Direct)
                            {
                                new_src_opnd->asSrcRegRegion()->setImmAddrOff(src->asSrcRegRegion()->getAddrImm());
                            }
                        }
                    }
                    else if (inst->hasOneUse())
                    {
                        new_src_opnd = src;
                        new_src_opnd->asSrcRegRegion()->setModifier(new_mod);
                        new_src_opnd->asSrcRegRegion()->setType(propType);
                    }
                    else
                    {
                        new_src_opnd = builder.duplicateOperand(src);
                        new_src_opnd->asSrcRegRegion()->setModifier(new_mod);
                        new_src_opnd->asSrcRegRegion()->setType(propType);
                    }
                    useInst->setSrc(new_src_opnd, opndNum - 1);
                }

                iter = inst->eraseUse(iter);
                // due to truncation a (partial) def of the move may no longer be a def of the use
                inst->copyDef(useInst, Opnd_src0, opndNum, true);

                doConsFolding(useInst);
            }
            // remove decl corresponding to this def
            // TODO!!! what if there is some alias to this decl?
            // remove MOV inst

            // remove it from the use list of its deflists
            inst->removeDefUse(Opnd_src0);

            INST_LIST_ITER tmp = ii;
            ii++;
            bb->erase(tmp);
        }
    }
}

void Optimizer::cselPeepHoleOpt()
{
    if (!builder.hasCondModForTernary())
    {
        return;
    }
    BB_LIST_ITER ib, bend(fg.end());
    G4_SrcRegRegion *cmpSrc0 = NULL;
    G4_Operand *cmpSrc1 = NULL;
    for (ib = fg.begin(); ib != bend; ++ib)
    {
        G4_BB* bb = (*ib);
        INST_LIST_ITER ii;
        INST_LIST_ITER nextIter;
        INST_LIST_ITER iiEnd;
        if (bb->empty())
        {
            continue;
        }

        bb->resetLocalId();
        ii = bb->begin();
        iiEnd = bb->end();

        nextIter = ii;

        do
        {
            ii = nextIter;
            ++nextIter;
            G4_INST *inst = *ii;
            G4_opcode op = inst->opcode();
            bool hasGRFDst = inst->getDst() && !inst->hasNULLDst();
            /*
            csel doesn't have the same symantics for destination
            as cmp instruction
            */
            if (op != G4_cmp || hasGRFDst || inst->getPredicate() ||
                inst->isDead() || !inst->getSrc(0)->isSrcRegRegion())
            {
                continue;
            }

            cmpSrc0 = inst->getSrc(0)->asSrcRegRegion();
            cmpSrc1 = inst->getSrc(1);

            G4_CondMod *cModifier = inst->getCondMod();

                // check if dst is global
            if (fg.globalOpndHT.isOpndGlobal(cModifier))
            {
                continue;
            }

            /*
            csel instruction implicitly compares src2 to 0
            only supports floats
            no predication
            */

            if (!cmpSrc1->isImm()                                                                            ||
                (cmpSrc1->asImm()->getImm() != 0 &&
                    (cmpSrc1->asImm()->getType() != Type_F || cmpSrc1->asImm()->getFloat()!= -0.0f))        ||
                cmpSrc0->getType() != Type_F                                                                ||
                cmpSrc0->isImm())
                continue;

            if (inst->getSrc(0)->isRelocImm() ||
                inst->getSrc(1)->isRelocImm())
            {
                continue;
            }

            // Only allow single strided regions.
            uint16_t src0Stride = 0;
            if (!cmpSrc0->getRegion()->isSingleStride(inst->getExecSize(), src0Stride))
                continue;

            /*
                Can do scan until use instruction to see if src0 is modified
                but I think for general case this will suffice. If we are
                not capturing opportunities can revisit.
            */

            if (inst->useEmpty())
                continue;

            int execSize = inst->getExecSize();
            if (execSize == 2)
                continue;

            USE_EDGE_LIST_ITER iter = inst->use_begin();
            USE_EDGE_LIST_ITER endUseList = inst->use_end();

            bool canOpt = true;
            int maxInstID = 0;

            for (; iter != endUseList; ++iter)
            {
                G4_INST* useInst = (*iter).first;

                if (useInst->getNumSrc() != 2)
                {
                    canOpt = false;
                    break;
                }

                maxInstID = std::max(useInst->getLocalId(), maxInstID);
                G4_Operand *dstUse = useInst->getDst();
                G4_Operand *selSrc0 = useInst->getSrc(0);
                G4_Operand *selSrc1 = useInst->getSrc(1);

                if (useInst->opcode() != G4_sel     ||
                    selSrc0->isImm()                ||
                    selSrc1->isImm()                ||
                    selSrc0->getType() != Type_F    ||
                    selSrc1->getType() != Type_F    ||
                    dstUse->getType()   != Type_F   ||
                    //3-src restriction
                    !builder.isOpndAligned(dstUse, 16)  ||
                    !builder.isOpndAligned(selSrc0, 16) ||
                    !builder.isOpndAligned(selSrc1, 16))
                {
                    canOpt = false;
                    break;
                }

                //   if inst is NoMask use inst can be anything.
                //   if inst is not NoMask then useInst needs to be subset of inst.
                if (!(inst->getMaskOption() & InstOpt_WriteEnable))
                {
                    auto isInclusive = [](int lb1, int rb1, int lb2, int rb2)
                    {
                        return lb1 <= lb2 && rb1 >= rb2;
                    };
                    if (!isInclusive(inst->getMaskOffset(), inst->getMaskOffset() + inst->getExecSize(),
                        useInst->getMaskOffset(), useInst->getMaskOffset() + useInst->getExecSize()))
                    {
                        canOpt = false;
                        break;
                    }
                }

                uint8_t numPredDefs = 0;
                DEF_EDGE_LIST_ITER useIter = useInst->def_begin();
                DEF_EDGE_LIST_ITER iterEnd = useInst->def_end();

                //    Just in case some weird code is generated with partial writes to predicate
                for (; useIter != iterEnd; ++useIter)
                {
                    if ((*useIter).second == Opnd_pred)
                        ++numPredDefs;

                    // Check whether pseudo_kill for dst exists between cmp and sel
                    // cmp.xx.fx.0 (8) ... src0 $0
                    // ...
                    // pseudo_kill dst
                    // (f0) sel (8) dst src1 src2
                    //
                    // These two cannot be merged because pseudo_kill is in between them

                    INST_LIST_ITER cselOptIt = ii;
                    cselOptIt++;
                    while ((*cselOptIt) != useInst)
                    {
                        if ((*cselOptIt)->isLifeTimeEnd())
                        {
                            if (GetTopDclFromRegRegion((*cselOptIt)->getDst()) ==
                                GetTopDclFromRegRegion(useInst->getDst()))
                            {
                                canOpt = false;
                                break;
                            }
                        }

                        cselOptIt++;
                    }
                }

                if (numPredDefs > 1)
                {
                    canOpt = false;
                    break;
                }
            }

            INST_LIST_ITER tempInstIter = nextIter;
            //explicit check that cmp sr0 is not over written or partially writen to
            //between cmp and sel.
            for (;tempInstIter != iiEnd; ++tempInstIter)
            {
                G4_INST *tempInst = *tempInstIter;

                if (tempInst->getLocalId() == maxInstID)
                {
                    break;
                }

                if (!tempInst->getDst())
                    continue;

                //also checks for indirect, will return inerference.
                G4_CmpRelation rel = tempInst->getDst()->compareOperand(cmpSrc0);
                if (rel != Rel_disjoint)
                {
                    canOpt = false;
                    break;
                }
            }

            if (canOpt)
            {
                for (auto iter = inst->use_begin(); iter != inst->use_end(); /*empty*/)
                {
                    G4_INST* useInst = (*iter).first;
                    G4_CondMod *mod = inst->getCondMod();
                    useInst->setOpcode(G4_csel);
                    useInst->setSrc(builder.duplicateOperand(inst->getSrc(0)), 2);
                    useInst->setCondMod(builder.duplicateOperand(mod));
                    useInst->setPredicate(NULL);

                    G4_SrcRegRegion* opnd2 = useInst->getSrc(2)->asSrcRegRegion();

                    if (!opnd2->isScalar() && inst->getExecSize() > useInst->getExecSize())
                    {
                        //earlier check establishes that useInst mask is equivalent or subset
                        //sel instruction
                        /*
                            case which considering:
                            cmp (16)
                            sel (8)
                        */
                        if (useInst->getMaskOffset() != inst->getMaskOffset())
                        {
                            //check elsewhere guarantees this is float.
                            G4_Type type = opnd2->getType();
                            unsigned short typeSize = TypeSize(type);
                            unsigned offset = opnd2->getRegOff() * numEltPerGRF<Type_UB>() + opnd2->getSubRegOff() * typeSize;
                            offset += useInst->getExecSize() * src0Stride * typeSize;

                            auto newSrc2 = builder.createSrcRegRegion(opnd2->getModifier(), Direct, opnd2->getBase(),
                                offset / numEltPerGRF<Type_UB>(), (offset % numEltPerGRF<Type_UB>()) / typeSize, opnd2->getRegion(),
                                opnd2->getType());
                            useInst->setSrc(newSrc2, 2);
                        }
                    }
                    //
                    // Modifying useDef links
                    //
                    // cmp.xx.f0.0 (8) ... src2 $0  <- inst (to be deleted)
                    // (f0) sel (8) dst src0 src1   <- useInst
                    // =>
                    // csel.xx.f0.0 (8) dst src0 src1 src2

                    // useInst's predicate becomes NULL.
                    iter = inst->eraseUse(iter);

                    // inst's src0 becomes useInst's src2.
                    inst->copyDef(useInst, Opnd_src0, Opnd_src2);
                }
                ASSERT_USER(inst->useEmpty(), "all predicate uses are removed.");
                inst->removeAllDefs();
                bb->erase(ii);
            }
        } while (nextIter != iiEnd);
    }
}

// helper function to convert
// and/or p3 p1 p2
// ==>
// (p1) sel t1 1 0
// (p2) sel t2 1 0
// and/or.nz.p3 t1 t2
// if the original inst is NoMask and Q1/H1, we do
// and/or p3 p1 p2
// ==>
// and/or (1) p3 p1 p2
// p3's type is uw for simd8/16 and ud for simd32
static void expandPseudoLogic(IR_Builder& builder,
                              G4_BB* bb,
                              INST_LIST_ITER& iter)

{
    G4_INST* inst = *iter;
    MUST_BE_TRUE(inst->isPseudoLogic(),
        "inst must be either pseudo_and/or/xor/not");
    INST_LIST_ITER newIter = iter;

    bool isFirstInst = iter == bb->begin();
    if (!isFirstInst)
    {
        --iter;
    }

    auto canFoldOnSIMD1 = [=, &builder]()
    {
        if (inst->isWriteEnableInst() &&
            (inst->getMaskOffset() == 0 || inst->getMaskOffset() == 16) &&
            // we can't do this for simd8 inst in simd16 kernels as it will overwrite upper flag bits
            (inst->getExecSize() > g4::SIMD8 || inst->getExecSize() == builder.kernel.getSimdSize()))
        {
            return true;
        }

        // inst writes the whole flag.
        if (inst->isWriteEnableInst() && inst->getMaskOffset() == 0)
        {
            auto Dcl = inst->getDst()->getTopDcl();
            if (Dcl && Dcl->getNumberFlagElements() <= inst->getExecSize())
            {
                return true;
            }
        }

        return false;
    };

    if (canFoldOnSIMD1())
    {
        G4_opcode newOpcode = G4_illegal;
        if (inst->getMaskOffset() == 16)
        {
            MUST_BE_TRUE(inst->getExecSize() == g4::SIMD16, "Only support simd16 pseudo-logic instructions");
            // we have to use the upper flag bits (.1) instead
            MUST_BE_TRUE(inst->getSrc(0)->isSrcRegRegion() && inst->getSrc(0)->isFlag(),
                "expect src0 to be flag");
            auto newSrc0 = builder.createSrcWithNewSubRegOff(inst->getSrc(0)->asSrcRegRegion(), 1);
            inst->setSrc(newSrc0, 0);
            if (inst->getSrc(1) != nullptr)
            {
                MUST_BE_TRUE(inst->getSrc(1)->isSrcRegRegion() && inst->getSrc(1)->isFlag(),
                    "expect src1 to be flag");
                auto newSrc1 = builder.createSrcWithNewSubRegOff(inst->getSrc(1)->asSrcRegRegion(), 1);
                inst->setSrc(newSrc1, 1);
            }
            auto newDst = builder.createDstWithNewSubRegOff(inst->getDst(), 1);
            inst->setDest(newDst);
        }

        switch (inst->opcode())
        {
            case G4_pseudo_and:
                newOpcode = G4_and;
                break;
            case G4_pseudo_or:
                newOpcode = G4_or;
                break;
            case G4_pseudo_xor:
                newOpcode = G4_xor;
                break;
            case G4_pseudo_not:
                newOpcode = G4_not;
                break;
            default:
                MUST_BE_TRUE(false, "unexpected opcode for pseudo-logic instructions");
        }

        inst->setOpcode(newOpcode);
        inst->setExecSize(g4::SIMD1);
    }
    else
    {
        G4_ExecSize tmpSize = inst->getExecSize();
        auto LowerOpnd = [=, &builder](Gen4_Operand_Number opNum, G4_INST* &SI) -> G4_Operand *
        {
            G4_Operand *Opnd = inst->getOperand(opNum);
            if (Opnd)
            {
                auto src = Opnd->asSrcRegRegion();
                auto newDcl = builder.createTempVar(tmpSize, Type_UW, Any);
                auto newDst = builder.createDst(newDcl->getRegVar(), 0, 0, 1, Type_UW);
                auto newPred = builder.createPredicate(PredState_Plus, src->getBase(), src->getSubRegOff());
                auto newSel = builder.createInternalInst(
                    newPred, G4_sel, nullptr, g4::NOSAT, tmpSize,
                    newDst,
                    builder.createImm(1, Type_UW),
                    builder.createImm(0, Type_UW),
                    inst->getOption());
                inst->transferDef(newSel, opNum, Gen4_Operand_Number::Opnd_pred);
                bb->insertBefore(newIter, newSel);
                SI = newSel;
                const RegionDesc *rd = tmpSize == g4::SIMD1 ?
                    builder.getRegionScalar() : builder.getRegionStride1();
                return builder.Create_Src_Opnd_From_Dcl(newDcl, rd);
            }
            return Opnd;
        };

        G4_INST *Sel0 = nullptr;
        G4_Operand *logicSrc0 = LowerOpnd(Gen4_Operand_Number::Opnd_src0, Sel0);

        G4_INST *Sel1 = nullptr;
        G4_Operand *logicSrc1 = LowerOpnd(Gen4_Operand_Number::Opnd_src1, Sel1);

        if (logicSrc1  == nullptr)
        {
            MUST_BE_TRUE(inst->opcode() == G4_pseudo_not, "Must be a pseudo-not instruction");
            // for not P1 P0
            // we generate
            // (P0) sel V1 1 0
            // xor.P1 null V1 1
            // so that the upper bits would stay zero
            logicSrc1 = builder.createImm(1, Type_UW);
        }

        auto nullDst = builder.createNullDst(Type_UW);
        auto newCondMod = builder.createCondMod(Mod_nz, inst->getDst()->getBase()->asRegVar(), 0);
        G4_opcode newOpcode = G4_illegal;
        switch (inst->opcode())
        {
            case G4_pseudo_and:
                newOpcode = G4_and;
                break;
            case G4_pseudo_or:
                newOpcode = G4_or;
                break;
            case G4_pseudo_xor:
                newOpcode = G4_xor;
                break;
            case G4_pseudo_not:
                // see comment above
                newOpcode = G4_xor;
                break;
            default:
                MUST_BE_TRUE(false, "unexpected opcode for pseudo-logic instructions");
        }

        G4_INST* newLogicOp = builder.createInternalInst(
            NULL,
            newOpcode,
            newCondMod,
            g4::NOSAT,
            tmpSize,
            nullDst,
            logicSrc0,
            logicSrc1,
            inst->getOption()  // keep the original instruction emask
            );

        // Fix def-use
        if (Sel0 != nullptr)
        {
            Sel0->addDefUse(newLogicOp, Gen4_Operand_Number::Opnd_src0);
        }
        if (Sel1 != nullptr)
        {
            Sel1->addDefUse(newLogicOp, Gen4_Operand_Number::Opnd_src1);
        }
        inst->transferUse(newLogicOp);
        bb->insertBefore(newIter, newLogicOp);
        bb->erase(newIter);
    }

    // iter either points to the start or the first expanded instruction.  Caller will advance it to the previous instruction
    if (isFirstInst)
    {
        iter = bb->begin();
    }
    else
    {
        ++iter;
    }
}

// mov(1) P0 Imm(NoMask)
// (P0) mov(esize) r[A0, 0] src0 src1
// == >
// smov(esize) r[A0, 0] src0 src1 Imm
//
// esize is either 8 or 16
bool Optimizer::createSmov(G4_BB *bb, G4_INST* flagMove, G4_INST* next_inst)
{
    if ((next_inst->getExecSize() != g4::SIMD8 && next_inst->getExecSize() != g4::SIMD16) ||
        next_inst->getPredicate() == NULL ||
        next_inst->getCondMod() != NULL ||
        next_inst->getSaturate() == true ||
        next_inst->getDst()->getRegAccess() == Direct ||
        next_inst->getDst()->getTypeSize() == 1 ||
        next_inst->getSrc(0)->getTypeSize() == 1 ||
        (builder.getPlatform() < GENX_SKL && builder.getPlatform() != GENX_BDW) ||
        next_inst->getDst()->getTypeSize() < next_inst->getSrc(0)->getTypeSize())
    {
        return false;
    }

    if (next_inst->getSrc(0)->isSrcRegRegion() &&
        next_inst->getSrc(0)->asSrcRegRegion()->getModifier() != Mod_src_undef)
    {
        return false;
    }

    if (flagMove->use_size() != 1 || flagMove->use_front().first != next_inst)
    {
        return false;
    }

    G4_CmpRelation rel = flagMove->getDst()->compareOperand(next_inst->getPredicate());
    if (rel != Rel_eq && !(rel == Rel_gt && next_inst->getMaskOffset() == 0))
    {
        return false;
    }

    if (kernel.getKernelType() == VISA_3D || !bb->isAllLaneActive())
    {
        if (!flagMove->isWriteEnableInst())
        {
            return false;
        }
    }

    next_inst->setOpcode(G4_smov);
    next_inst->setSrc(flagMove->getSrc(0), 1);
    next_inst->setPredicate(nullptr);

    flagMove->removeUseOfInst();
    return true;
}

// Returns true if *iter has an use that is a cmp and we can fold that cmp
// into *iter as a conditional modifier. The cmp instruction is deleted as part of folding.
// Note that iter may be modified to point to the next inst if we decide to sync *iter to
// where the cmp was to work around dependencies
bool Optimizer::foldCmpToCondMod(G4_BB* bb, INST_LIST_ITER& iter)
{
    // find a cmp that uses inst dst
    G4_INST* inst = *iter;
    G4_INST* cmpInst = nullptr;
    bool canFold = false;

    if (inst->getCondMod())
    {
        return false;
    }

    for (auto UI = inst->use_begin(), UE = inst->use_end(); UI != UE; ++UI)
    {
        cmpInst = (*UI).first;

        // cmp instruction must be of the form
        // cmp [<op> P0] null src 0
        // where src is singly defined by inst
        if (cmpInst->opcode() == G4_cmp && cmpInst->getExecSize() == inst->getExecSize() &&
            cmpInst->hasNULLDst() &&
            cmpInst->getSrc(0)->isSrcRegRegion() &&
            cmpInst->getSrc(0)->asSrcRegRegion()->getModifier() == Mod_src_undef &&
            cmpInst->def_size() == 1 && !cmpInst->getPredicate() &&
            cmpInst->getSrc(1)->isImm() && cmpInst->getSrc(1)->asImm()->isZero())
        {
            canFold = true;
            break;
        }
    }

    if (!canFold)
    {
       return false;
    }

    // floating point cmp may flush denorms to zero, but mov may not.
    //
    // mov(1|M0) (lt)f0.0 r6.2<1>:f r123.2<0;1,0>:f
    // may not be the same as
    // mov(1|M0)          r6.2<1>:f r123.2<0;1,0>:f
    // cmp(1|M0) (lt)f0.0 null<1>:f 6.2<0;1,0>:f 0x0:f
    // for denorm inputs.
    if (inst->opcode() == G4_mov && IS_TYPE_FLOAT_ALL(cmpInst->getSrc(0)->getType()))
    {
        return false;
    }

    // If dst is B and exectype is Q, dst is misaligned. This misaligned inst
    // will be split in HWComformity. If a condMod is present, it would be
    // harder to split and may result in wrong code. Here, simply disable folding.
    //
    // For example,
    //    (W) mov (1)     conv_i(0,0)<1>:b   V0040(0,0)<0;1,0>:q
    //    (W) cmp(1)  (eq)P01.0   null<1>:w  conv_i(0, 0)<0;1,0>:b  0:w
    //
    int extypesize = 0;
    (void) inst->getOpExecType(extypesize);
    if (inst->getDst()->getTypeSize() == 1 && extypesize == 8)
    {
        return false;
    }

    auto cmpIter = std::find(iter, bb->end(), cmpInst);

    // check if cmp instruction is close enough
    constexpr int DISTANCE = 60;
    if (std::distance(iter, cmpIter) > DISTANCE)
    {
        return false;
    }

    auto isSupportedCondMod = [](G4_CondModifier mod)
    {
        return mod == Mod_g || mod == Mod_ge || mod == Mod_l || mod == Mod_le ||
            mod == Mod_e || mod == Mod_ne;
    };
    G4_CondModifier mod = cmpInst->getCondMod()->getMod();
    if (!isSupportedCondMod(mod))
    {
        return false;
    }

    if (kernel.getKernelType() == VISA_3D || !bb->isAllLaneActive())
    {
        // Make sure masks of both instructions are same
        if (inst->getMaskOption() != cmpInst->getMaskOption())
        {
            return false;
        }
    }

    auto getUnsignedTy = [](G4_Type Ty)
    {
        switch (Ty)
        {
        case Type_D:
            return Type_UD;
        case Type_W:
            return Type_UW;
        case Type_B:
            return Type_UB;
        case Type_Q:
            return Type_UQ;
        case Type_V:
            return Type_UV;
        default:
            break;
        }
        return Ty;
    };

    G4_Type T1 = inst->getDst()->getType();
    G4_Type T2 = cmpInst->getSrc(0)->getType();
    if (getUnsignedTy(T1) != getUnsignedTy(T2))
    {
        return false;
    }
    // Skip if the dst needs saturating but it's used as different sign.
    if (inst->getSaturate() && T1 != T2)
    {
        return false;
    }

    if (chkBwdOutputHazard(iter, cmpIter))
    {
        return false;
    }

    auto isSafeToSink = [](INST_LIST_ITER defIter,
        INST_LIST_ITER beforeIter, int maxDist)
    {
        G4_INST *inst = *defIter;
        int dist = 0;
        for (auto it = std::next(defIter); it != beforeIter; ++it)
        {
            if (dist++ >= maxDist)
                return false;
            if (inst->isWAWdep(*it) || inst->isRAWdep(*it) ||
                inst->isWARdep(*it))
                return false;
            if (!checkLifetime(inst, *it))
                return false;
        }
        return true;
    };

    // Merge two instructions
    // If legal, use the cmp location as new insert position.
    bool sinkInst = false;

    if (inst->getDst()->compareOperand(cmpInst->getSrc(0)) == Rel_eq)
    {
        if (inst->use_size() == 1)
        {
            // see if we can replace dst with null
            if (inst->supportsNullDst() && !fg.globalOpndHT.isOpndGlobal(inst->getDst()))
            {
                inst->setDest(builder.createDst(
                    builder.phyregpool.getNullReg(),
                    0,
                    0,
                    inst->getDst()->getHorzStride(),
                    inst->getDst()->getType()));
            }
            // Check if it is safe to sink inst right before cmp inst, which lowers flag pressure in general.
            const int MAX_DISTANCE = 20;
            sinkInst = isSafeToSink(iter, cmpIter, MAX_DISTANCE);
        }
        inst->setCondMod(cmpInst->getCondMod());
        inst->setOptions((inst->getOption() & ~InstOpt_Masks) | (cmpInst->getMaskOption()));
        // The sign of dst should follow its use instead of its
        // def. The later is meaningless from how hardware works.
        auto honorSignedness = [](G4_CondModifier mod) {
            switch (mod) {
            case Mod_g:
            case Mod_ge:
            case Mod_l:
            case Mod_le:
                return true;
            default:
                break;
            }
            return false;
        };
        if (honorSignedness(inst->getCondMod()->getMod()))
            inst->getDst()->setType(T2);

        // update def-use
        // since cmp is deleted, we have to
        // -- transfer cmp's use to inst
        // -- remove cmp from its definition's use list
        cmpInst->transferUse(inst, true);
        cmpInst->removeUseOfInst();
        if (!sinkInst)
        {
            bb->erase(cmpIter);
        }
        else
        {
            // Before and <- ii
            //        cmp <- next_iter
            // After  cmp <- ii
            //        and <- next
            std::iter_swap(iter, cmpIter);
            auto nextii = std::next(iter);
            bb->erase(iter);
            iter = nextii;
        }
        return true;
    }
    return false;
}

// Return true  : folding cmp and sel is performed
//        false : no folding is performed.
bool Optimizer::foldCmpSel(G4_BB *BB, G4_INST *selInst, INST_LIST_ITER &selInst_II)
{
    MUST_BE_TRUE((selInst->opcode() == G4_sel &&
                   selInst->getPredicate() &&
                   selInst->getCondMod() == NULL),
                  "foldCmpSel: Inst should be a sel with predicate and without cmod.");
    G4_Predicate *pred = selInst->getPredicate();

    // global predicates are not eligible since folding the cmp removes the def
    if (fg.globalOpndHT.isOpndGlobal(pred))
    {
        return false;
    }

    // Needs to find cmp instruction that defines predicate of sel. The cmp is
    // not necessarily right before the sel. To be able to fold the cmp to the
    // sel, we have to check if the cmp can be moved right before the sel. It not,
    // no folding is performed.
    G4_INST *cmpInst = nullptr;
    for (auto DI = selInst->def_begin(), DE = selInst->def_end(); DI != DE; ++DI)
    {
        if ((*DI).second == Opnd_pred)
        {
            if (cmpInst)
            {   // only handle single def.
                cmpInst = nullptr;
                break;
            }
            cmpInst = (*DI).first;
            if (cmpInst && cmpInst->opcode() != G4_cmp)
            {
                cmpInst = nullptr;
                break;
            }
        }
    }
    if (!cmpInst)
    {
        // G4_cmp is not found, skip optimization.
        return false;
    }

    // Do a fast check first. Note that sel w/ cmod
    // does not allow predication. So, we will give up if cmp has predicate.
    bool isSubExecSize = (selInst->getExecSize() < cmpInst->getExecSize());
    bool isSameExecSize = (selInst->getExecSize() == cmpInst->getExecSize());
    if ((!isSameExecSize && !isSubExecSize) ||
        (cmpInst->getDst() && !cmpInst->hasNULLDst()) ||
        cmpInst->use_size() != 1 ||
        cmpInst->getPredicate() != nullptr)
    {
        return false;
    }

    // Check if two source operands have the same type and value.
    auto IsEqual = [](G4_Operand *opnd1, G4_Operand *opnd2)
    {
        if (opnd1->isImm() && opnd2->isImm())
            return opnd1->asImm()->isEqualTo(opnd2->asImm());
        if (opnd1->compareOperand(opnd2) != Rel_eq)
            return false;
        // footprint does not imply equality.
        // (1) region difference:  r10.0<1;4,2>:f vs r10.0<8;8,1>
        // (2) source modifier: r10.0<0;1,0>:f vs -r10.0<0;1,0>
        //
        if (opnd1->isSrcRegRegion() && opnd2->isSrcRegRegion())
            return opnd1->asSrcRegRegion()->sameSrcRegRegion(*opnd2->asSrcRegRegion());

        // Common cases should be covered.
        return false;
    };

    G4_Operand *sel_src0 = selInst->getSrc(0);
    G4_Operand *sel_src1 = selInst->getSrc(1);
    G4_Operand *cmp_src0 = cmpInst->getSrc(0);
    G4_Operand *cmp_src1 = cmpInst->getSrc(1);

    // Normalize SEL's predicate state to Plus.
    if (G4_PredState::PredState_Minus == pred->getState())
    {
        selInst->swapSrc(0, 1);
        selInst->swapDefUse();
        pred->setState(G4_PredState::PredState_Plus);
        std::swap(sel_src0, sel_src1);
    }

    // source operands of SEL and CMP are reversed or not.
    bool reversed = false;
    G4_CondMod* condMod = cmpInst->getCondMod();

    auto canFold = [=, &reversed]() {
        G4_CmpRelation condRel = pred->asPredicate()->compareOperand(condMod);
        if (!(condRel == Rel_eq && isSameExecSize) &&
            !(condRel == Rel_lt && isSubExecSize))
            return false;

        if (!cmpInst->isWriteEnableInst() &&
            cmpInst->getMaskOption() != selInst->getMaskOption())
            return false;

        // P = cmp.gt A, B
        // C = (+P) sel A, B  => C = sel.gt A, B
        //
        // P = cmp.ne A, B
        // C = (+P) sel A, B  => C = sel.ne A, B
        //
        if (IsEqual(sel_src0, cmp_src0) && IsEqual(sel_src1, cmp_src1))
            return true;

        // Sel operands are reversed.
        // P = cmp.gt A, B
        // C = (+P) sel B, A  => C = sel.le B, A
        //
        // P = cmp.ne A, B
        // C = (+P) sel B, A  => C = sel.ne B, A
        //
        if (IsEqual(sel_src0, cmp_src1) && IsEqual(sel_src1, cmp_src0))
        {
            reversed = true;
            return true;
        }
        return false;
    };

    if (!canFold())
    {
        return false;
    }

    // check if cmpInst can be legally moved right before selInst;
    // if it cannot, we cannot fold cmp to sel!.
    INST_LIST_ITER cmpInst_II = selInst_II;
    while (cmpInst_II != BB->begin())
    {
        cmpInst_II--;
        if (*cmpInst_II == cmpInst)
        {
            break;
        }
    }

    // No local def (possible?)
    if (cmpInst_II == BB->begin())
    {
        return false;
    }

    // If cmpInst has no WAR harzard b/w cmpInst and selInst, cmpInst
    // can be moved right before selInst.
    if (chkFwdOutputHazard(cmpInst_II, selInst_II))
    {
        return false;
    }

    G4_CondModifier mod = condMod->getMod();
    if (reversed)
        mod = G4_CondMod::getReverseCondMod(mod);
    G4_CondMod* cmod = builder.createCondMod(mod, condMod->getBase(),
                                             condMod->getSubRegOff());
    selInst->setCondMod(cmod);
    selInst->setPredicate(nullptr);

    // update def-use
    // since cmp is deleted, we have to
    // -- remove def-use between cmp and sel
    // -- transfer cmp's remaining use to sel
    // -- remove cmp and its definitions' use
    selInst->removeDefUse(Opnd_pred);
    cmpInst->transferUse(selInst, true);
    cmpInst->removeUseOfInst();
    BB->erase(cmpInst_II);
    return true;
}

// try to fold a pseudo not instruction into its use(s)
// return true if successful
bool Optimizer::foldPseudoNot(G4_BB* bb, INST_LIST_ITER& iter)
{
    G4_INST* notInst = *iter;
    assert(notInst->opcode() == G4_pseudo_not && "expect not instruction");
    if (notInst->getPredicate() || notInst->getCondMod())
    {
        return false;
    }

    G4_DstRegRegion* dst = notInst->getDst();
    if (fg.globalOpndHT.isOpndGlobal(dst))
    {
        return false;
    }
    if (!notInst->getSrc(0)->isSrcRegRegion())
    {
        return false;
    }

    // unfortunately, def-use chain is not always properly maintained, so we have to skip opt
    // even if we can't find a use
    bool canFold = notInst->use_size() > 0;
    for (auto uses = notInst->use_begin(), end = notInst->use_end(); uses != end; ++uses)
    {
        auto&& use = *uses;
        G4_INST* useInst = use.first;
        Gen4_Operand_Number opndPos = use.second;
        if (!useInst->isLogic() || !G4_INST::isSrcNum(opndPos))
        {
            canFold = false;
            break;
        }

        // check the case where flag is partially used
        G4_SrcRegRegion* opnd = useInst->getSrc(G4_INST::getSrcNum(opndPos))->asSrcRegRegion();
        if (dst->compareOperand(opnd) != Rel_eq)
        {
            canFold = false;
            break;
        }
    }

    if (canFold)
    {
        G4_SrcRegRegion* origUse = notInst->getSrc(0)->asSrcRegRegion();
        if (notInst->getMaskOffset() == 16)
        {
            // Fold upper bits
            assert(notInst->getExecSize() == g4::SIMD16);
            origUse = builder.createSrcWithNewSubRegOff(origUse, 1);
            notInst->setSrc(origUse, 0);
        }
        for (auto uses = notInst->use_begin(), uend = notInst->use_end(); uses != uend; ++uses)
        {
            auto use = *uses;
            G4_INST* useInst = use.first;
            Gen4_Operand_Number opndPos = use.second;
            G4_SrcRegRegion* opnd = useInst->getSrc(G4_INST::getSrcNum(opndPos))->asSrcRegRegion();
            int numNot = 1 + (origUse->getModifier() == Mod_Not ? 1 : 0) +
                (opnd->getModifier() == Mod_Not ? 1 : 0);
            G4_SrcModifier newModifier = numNot & 0x1 ? Mod_Not : Mod_src_undef;
            G4_SrcRegRegion* newSrc = builder.createSrcRegRegion(*origUse);
            newSrc->setModifier(newModifier);
            useInst->setSrc(newSrc, G4_INST::getSrcNum(opndPos));

        }

        for (auto defs = notInst->def_begin(); defs != notInst->def_end(); ++defs)
        {
            auto def = *defs;
            G4_INST* defInst = def.first;
            notInst->copyUsesTo(defInst, false);
        }
        notInst->removeAllDefs();
        notInst->removeAllUses();
        bb->erase(iter);
        return true;
    }
    return false;
}

/***
this function optmize the following cases:

case 1:
cmp.gt.P0 s0 s1
(P0) sel d s0 s1
==>
sel.gt.P0 d s0 s1

case 2:
and dst src0 src1   -- other OPs are also optimized: or, xor...
cmp.nz.P0 NULL dst 0
(P0) ...
==>
and.nz.P0 dst src0 src1
(P0) ...
add/addc instructions also handled in case 2. Few more cond
modifiers supported to such arithmetic instructions.

case 3:
cmp.l.P0 NULL src0 src1
cmp.l.P1 NULL src2 src3
and P2 P0 P1   -- other OPs are also optimized: or, xor...
==>
cmp.l.P2 NULL src0 src1
(P2)cmp.l.P2 NULL src2 src3

case 4:
mov (1) P0 Imm  (NoMask)
(P0) mov (8) r[A0, 0] src0 src1
==>
smov (8) r[A0, 0] src0 src1 Imm

case 5:
psuedo_not (1) P2 P1
and (1) P4 P3 P2
==>
and (1) P4 P3 ~P1
*/

void Optimizer::optimizeLogicOperation()
{
    BB_LIST_ITER ib, bend(fg.end());
    G4_Operand *dst = NULL;
    bool resetLocalIds =  false;
    bool doLogicOpt = builder.getOption(vISA_LocalFlagOpt);

    if (!doLogicOpt)
    {
        // we still need to expand the pseudo logic ops
        for (auto bb : fg)
        {
            for (auto I = bb->begin(), E = bb->end(); I != E; ++I)
            {
                auto inst = *I;
                if (inst->isPseudoLogic())
                {
                    expandPseudoLogic(builder, bb, I);
                }
            }
        }
        return;
    }

    for (ib = fg.begin(); ib != bend; ++ib)
    {
        G4_BB* bb = *ib;
        INST_LIST_ITER ii;
        if ((bb->begin() == bb->end())) {
            continue;
        }
        resetLocalIds =  false;
        ii = bb->end();
        do
        {
            ii--;
            G4_INST *inst = *ii;
            G4_opcode op = inst->opcode();
            dst = inst->getDst();
            bool nullDst = inst->hasNULLDst();
            G4_Declare *dcl = nullptr;
            if (dst)
            {
                dcl = dst->getTopDcl();
            }

            if ((op != G4_sel && op != G4_cmp && !inst->canSupportCondMod() && !inst->isPseudoLogic()) ||
                !dst || nullDst || (dcl && dcl->isOutput()))
            {
                continue;
            }

            INST_LIST_ITER next_iter = std::next(ii);

            if (!resetLocalIds)
            {
                bb->resetLocalId();
                resetLocalIds =  true;
            }

            // case 5
            if (inst->opcode() == G4_pseudo_not)
            {
                bool folded = foldPseudoNot(bb, ii);
                if (folded)
                {
                    ii = next_iter;
                    continue;
                }
            }

            // case 1
            if (ii != bb->begin() && op == G4_sel && inst->getPredicate() && !inst->getCondMod())
            {
                if (!foldCmpSel(bb, inst, ii)) {
                    // Compare two operands with special simple checking on
                    // indirect access. That checking could be simplified as
                    // only dst/src of the same instruction are checked.
                    auto compareOperand =
                        [](G4_DstRegRegion *A, G4_Operand *B, unsigned ExecSize)
                        -> G4_CmpRelation {
                        G4_CmpRelation Res = A->compareOperand(B);
                        if (Res != Rel_interfere)
                            return Res;
                        if (A->getRegAccess() != IndirGRF ||
                            B->getRegAccess() != IndirGRF)
                            return Res;
                        if (A->getHorzStride() != 1)
                            return Res;
                        // Extra check if both are indirect register accesses.
                        G4_VarBase *BaseA = A->getBase();
                        G4_VarBase *BaseB = B->getBase();
                        if (!BaseA || !BaseB || BaseA != BaseB || !BaseA->isRegVar())
                            return Res;
                        if (!B->isSrcRegRegion())
                            return Res;
                        G4_SrcRegRegion *S = B->asSrcRegRegion();
                        if (!S->getRegion()->isContiguous(ExecSize))
                            return Res;
                        if (A->getRegOff() != S->getRegOff() ||
                            A->getSubRegOff() != S->getSubRegOff())
                            return Res;
                        if (A->getAddrImm() != S->getAddrImm())
                            return Res;
                        return Rel_eq;
                    };

                    unsigned ExSz = inst->getExecSize();
                    G4_DstRegRegion *Dst = inst->getDst();
                    G4_Operand *Src0 = inst->getSrc(0);
                    G4_Operand *Src1 = inst->getSrc(1);
                    int OpndIdx = -1;
                    if (compareOperand(Dst, Src0, ExSz) == Rel_eq &&
                        Src0->isSrcRegRegion() &&
                        Src0->asSrcRegRegion()->getModifier() == Mod_src_undef)
                        OpndIdx = 0;
                    else if (compareOperand(Dst, Src1, ExSz) == Rel_eq &&
                             Src1->isSrcRegRegion() &&
                             Src1->asSrcRegRegion()->getModifier() == Mod_src_undef)
                        OpndIdx = 1;
                    if (OpndIdx >= 2) {
                        // If dst is equal to one of operands of 'sel', that
                        // 'sel' could be transformed into a predicated 'mov',
                        // i.e.,
                        //
                        // transforms
                        //
                        //  (+p) sel dst, src0, src1
                        //
                        // into
                        //
                        //  (+p) mov dst, src0   if dst == src1
                        //
                        // or
                        //
                        //  (-p) mov dst, src1   if dst == src0
                        //
                        if (OpndIdx == 0) {
                            // Inverse predicate.
                            G4_Predicate *Pred = inst->getPredicate();
                            G4_PredState State = Pred->getState();
                            State = (State == PredState_Plus) ? PredState_Minus
                                                              : PredState_Plus;
                            Pred->setState(State);
                            // Swap source operands.
                            inst->swapSrc(0, 1);
                            inst->swapDefUse();
                        }
                        inst->setOpcode(G4_mov);
                        inst->setSrc(nullptr, 1);
                        inst->removeDefUse(Opnd_src1);
                    }
                }
            }
            else if (builder.hasSmov() && inst->opcode() == G4_mov &&
                inst->getPredicate() == NULL &&
                inst->getCondMod()  == NULL &&
                inst->getExecSize() == g4::SIMD1 &&
                inst->getSrc(0)->isImm() &&
                inst->getDst()->isFlag() &&
                next_iter != bb->end() &&
                (*next_iter)->opcode() == G4_mov)
            {
                // case 4
                G4_INST *next_inst = *next_iter;
                if (createSmov(bb, inst, next_inst))
                {
                    bb->erase(ii);
                    ii = next_iter;
                }
                continue;
            }
            else if (!inst->getPredicate() && inst->canSupportCondMod())
            {
                // FIXME: why this condition?
                if (op == G4_pseudo_mad && inst->getExecSize() == g4::SIMD1)
                {
                    continue;
                }

                // case 2
                foldCmpToCondMod(bb, ii);
            }
            else if (inst->getPredicate() == NULL && inst->isPseudoLogic())
            {
                bool merged = false;

                if (op == G4_pseudo_and || op == G4_pseudo_or)
                {
                    merged = foldPseudoAndOr(bb, ii);
                }

                // translate the psuedo op
                if (!merged)
                {
                    expandPseudoLogic(builder, bb, ii);
                }
            }
        }
        while (ii != bb->begin());
    }
}

// see if we can fold a pseudo and/or instruction with previous cmp
// returns true if successful, and ii (inst-list-iter) is also updated
// to the next inst
bool Optimizer::foldPseudoAndOr(G4_BB* bb, INST_LIST_ITER& ii)
{
    // case 3

    // optimization should apply even when the dst of the pseudo-and/pseudo-or is global,
    // since we are just hoisting it up, and WAR/WAW checks should be performed as we search
    // for the src0 and src1 inst.

    G4_INST* inst = *ii;
    // look for def of srcs
    G4_Operand* src0 = inst->getSrc(0);
    G4_Operand* src1 = inst->getSrc(1);

    /*
    Old logic would scan from inst (and/or) up until it encountered
    def instructions that did full write.
    It would scan from def instruction down to inst looking for WAW, WAR
    conflicts between def instruction and intermediate instructions.
    Basically insuring that there were no partial writes in to flag registers
    use by the inst.

    The new code uses defInstList directly, and aborts if there are more then are
    two definitions. Which means there is more then one instruction writing to source.
    Disadvantage of that is that it is less precisise. For example if we are folding in to
    closest definition then before it was OK, but now will be disallowed.
    */
    int32_t maxSrc1 = 0;
    int32_t maxSrc2 = 0;
    G4_INST *defInstructions[2] = { nullptr, nullptr };
    G4_INST* src0DefInst = nullptr;
    G4_INST* src1DefInst = nullptr;

    //Picks the latest two instructions to compare.
    //local IDs are reset at the beginning of this function.
    if (inst->def_size() < 2)
    {
        return false;
    }
    //trying to find latest instructions that define src0 and src1
    for (auto I = inst->def_begin(), E= inst->def_end(); I != E; ++I)
    {
        G4_INST* srcInst = I->first;
        if ((I->second == Opnd_src0) && (srcInst->getLocalId() >= maxSrc1))
        {
            maxSrc1 = srcInst->getLocalId();
            defInstructions[0] = srcInst;
            src0DefInst = srcInst;
        }
        else if ((I->second == Opnd_src1) && (srcInst->getLocalId() >= maxSrc2))
        {
            maxSrc2 = srcInst->getLocalId();
            defInstructions[1] = srcInst;
            src1DefInst = srcInst;
        }
    }

    // Making sure that dst of pseudo instruction is not used or defined between pseudo instruction
    // and the first definition of the source
    if (defInstructions[0] && defInstructions[1])
    {
        // make defInst[0] the closer def to the pseudo-and/or
        if (maxSrc2 > maxSrc1)
        {
            std::swap(defInstructions[0], defInstructions[1]);
            std::swap(maxSrc1, maxSrc2);
        }
        //Doing backward scan until earlist src to make sure dst of and/or is not being written to
        //or being read
        /*
        handling case like in spmv_csr
        cmp.lt (M1, 1) P15 V40(0,0)<0;1,0> 0x10:w                                    /// $191
        cmp.lt (M1, 1) P16 V110(0,0)<0;1,0> V34(0,0)<0;1,0>                          /// $192
        and (M1, 1) P16 P16 P15                                                      /// $193
        */
        if (chkBwdOutputHazard(defInstructions[1], ii, defInstructions[0]))
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    // check if the defInst can be folded into the pseudo and/or for a given source
    // folding is legal if
    // -- src is the only use of defInst
    // -- def completely defines the src
    // -- def inst does not have predicate
    // -- the defInst closer to the pseudo inst is a cmp without pred
    // -- def inst is not global operand
    // the last condition can be relaxed if defInst is the same as the inst's dst,
    // as they will be the same flag
    if (!(defInstructions[0]->opcode() == G4_cmp && defInstructions[0]->getPredicate() == nullptr))
    {
        return false;
    }

    auto checkSource = [this, inst](G4_INST* defInst, G4_Operand* src)
    {
        if (defInst == nullptr)
        {
            return false;
        }

        if (defInst->use_size() > 1)
        {
            return false;
        }
        G4_Operand* curr_dst = defInst->getCondMod() ?
            defInst->getCondMod() : (G4_Operand*) defInst->getDst();

        G4_CmpRelation rel = curr_dst->compareOperand(src);
        if (rel != Rel_eq ||
            (defInst->getPredicate() && defInst->opcode() != G4_sel))
        {
            return false;
        }

        if (fg.globalOpndHT.isOpndGlobal(curr_dst))
        {
            G4_DstRegRegion* dst = inst->getDst();
            if (dst->compareOperand(curr_dst) != Rel_eq)
            {
                return false;
            }
        }

        return true;
    };

    if (!checkSource(src0DefInst, src0) || !checkSource(src1DefInst, src1))
    {
        return false;
    }

    // do the case 3 optimization

    G4_PredState ps = (inst->opcode() == G4_pseudo_or) ? PredState_Minus : PredState_Plus;
    G4_INST *first_inst = defInstructions[1];
    G4_INST *second_inst = defInstructions[0];

    // unify subregister according to logic op dst
    G4_VarBase *curr_base = inst->getDst()->getBase()->asRegVar();
    unsigned short curr_subreg = 0;

    G4_Operand *first_def, *second_def;
    G4_VarBase *first_def_base, *second_def_base;
    int first_def_subreg, second_def_subreg;

    // change condmod and predicate of second inst

    if (first_inst->getCondMod())
    {
        first_def = first_inst->getCondMod();
        first_def_base = first_def->asCondMod()->getBase();
        first_def_subreg = first_def->asCondMod()->getSubRegOff();
    }
    else
    {
        first_def = first_inst->getDst();
        first_def_base = first_def->asDstRegRegion()->getBase()->asRegVar();
        first_def_subreg = 0;
    }

    if (second_inst->getCondMod())
    {
        second_def = second_inst->getCondMod();
        second_def_base = second_def->asCondMod()->getBase();
        second_def_subreg = second_def->asCondMod()->getSubRegOff();
    }
    else
    {
        second_def = second_inst->getDst();
        second_def_base = second_def->asDstRegRegion()->getBase()->asRegVar();
        second_def_subreg = 0;
    }

    bool change_flag = false;
    if (second_inst->getCondMod() &&
        (second_def_base != curr_base || second_def_subreg != curr_subreg))
    {
        change_flag = true;
        G4_CondMod *new_condMod = builder.createCondMod(
            second_inst->getCondMod()->getMod(),
            curr_base,
            curr_subreg);

        second_inst->setCondMod(new_condMod);
    }
    // create a predicate

    G4_Predicate *new_pred = builder.createPredicate(
        ps,
        curr_base,
        curr_subreg);

    second_inst->setPredicate(new_pred);

    if (change_flag)
    {
        for (USE_EDGE_LIST_ITER iter = second_inst->use_begin();
            iter != second_inst->use_end();
            ++iter)
        {
            G4_INST *curr_inst = (*iter).first;
            if (curr_inst == inst)
            {
                continue;
            }
            if (curr_inst->getPredicate() &&
                (curr_inst->getPredicate()->getBase() != curr_base ||
                    curr_inst->getPredicate()->getSubRegOff() != curr_subreg) &&
                curr_inst->getPredicate()->compareOperand(second_def) == Rel_eq)
            {
                curr_inst->setPredicate(builder.duplicateOperand(new_pred));
            }

            for (int k = 0; k < curr_inst->getNumSrc(); k++)
            {
                G4_Operand *curr_src = curr_inst->getSrc(k);
                if (curr_src->isSrcRegRegion() && !(curr_inst->isMath() && k == 1 && curr_src->isNullReg()))
                {
                    if (curr_src->asSrcRegRegion()->compareOperand(second_def) == Rel_eq)
                    {
                        G4_SrcRegRegion *new_src_opnd = builder.createSrcRegRegion(
                            curr_src->asSrcRegRegion()->getModifier(),
                            curr_src->asSrcRegRegion()->getRegAccess(),
                            inst->getDst()->getBase(),
                            0,
                            0,
                            curr_src->asSrcRegRegion()->getRegion(),
                            curr_src->asSrcRegRegion()->getType());

                        curr_inst->setSrc(new_src_opnd, k);
                    }
                }
            }
        }
    }

    if (first_def_base != curr_base || first_def_subreg != curr_subreg)
    {
        if (first_inst->getCondMod() && first_def->isCondMod())
        {
            G4_CondMod *new_condMod = builder.createCondMod(
                first_inst->getCondMod()->getMod(),
                curr_base,
                curr_subreg);
            first_inst->setCondMod(new_condMod);
        }
        else
        {
            first_inst->setDest(builder.duplicateOperand(inst->getDst()));
        }
        for (USE_EDGE_LIST_ITER iter = first_inst->use_begin();
            iter != first_inst->use_end();
            ++iter)
        {
            G4_INST *curr_inst = (*iter).first;
            if (curr_inst == inst)
            {
                continue;
            }
            if (curr_inst->getPredicate() &&
                (curr_inst->getPredicate()->getBase() != curr_base ||
                    curr_inst->getPredicate()->getSubRegOff() != curr_subreg) &&
                curr_inst->getPredicate()->compareOperand(first_def) == Rel_eq)
            {
                curr_inst->setPredicate(builder.duplicateOperand(new_pred));
            }

            for (int k = 0; k < curr_inst->getNumSrc(); k++)
            {
                G4_Operand *curr_src = curr_inst->getSrc(k);
                if (curr_src->isSrcRegRegion() && !(curr_inst->isMath() && k == 1 && curr_src->isNullReg()))
                {
                    if (curr_src->asSrcRegRegion()->compareOperand(first_def) == Rel_eq)
                    {
                        G4_SrcRegRegion *new_src_opnd = builder.createSrcRegRegion(
                            curr_src->asSrcRegRegion()->getModifier(),
                            curr_src->asSrcRegRegion()->getRegAccess(),
                            curr_base,
                            0,
                            curr_subreg,
                            curr_src->asSrcRegRegion()->getRegion(),
                            curr_src->asSrcRegRegion()->getType());
                        curr_inst->setSrc(new_src_opnd, k);
                    }
                }
            }
        }
    }

    // update def-use
    // since inst (the pseudo-and/or) is deleted and the same flag is used for first
    // and second inst, we have to
    // -- transfer inst's use to the second_inst
    // -- add def-use between first_inst and second_inst
    // -- remove inst from first_inst and second_inst's use
    inst->transferUse(second_inst, true);
    first_inst->addDefUse(second_inst, Opnd_pred);
    inst->removeUseOfInst();

    INST_LIST_ITER new_iter = ii;
    ++ii;
    bb->erase(new_iter);
    return true;
}


    /***  The beginning of message header optimization  ***/

    /*
    * reuse the previous header which can save the redundant definitions.
    */
    void MSGTable::reusePreviousHeader(G4_INST *dest,
        G4_INST *source,
        G4_INST *mDot2,
        IR_Builder &builder)
    {
        if (dest==NULL) return;
        if (source != NULL)
        {
            dest->setDest(builder.duplicateOperand(source->getDst()));
        }
        else
        {
            short subRegOff = dest->getDst()->getSubRegOff();
            auto newDst = builder.createDstWithNewSubRegOff(mDot2->getDst(), subRegOff);
            dest->setDest(newDst);
        }
    }

    /*
    * insert a mov, from the previous header to current header
    * this is only required for the instruction,
    * whose payload size >1 so that we can't directly reuse
    * the previous header. keep a copy from the previous header,
    * so that we only need to update the fields that need to be changed.
    */
    void MSGTable::insertHeaderMovInst(G4_INST *source_send,
        IR_Builder& builder,
        G4_BB *bb)
    {
        G4_INST *inst = NULL;
        INST_LIST_ITER pos;

        switch (first)
        {
        case HEADER_FULL_REGISTER:
            inst = m;
            pos  = m_it;
            break;
        case HEADER_X:
            inst = mDot0;
            pos  = mDot0_it;
            break;
        case HEADER_Y:
            inst = mDot1;
            pos  = mDot1_it;
            break;
        case HEADER_SIZE:
            inst = mDot2;
            pos  = mDot2_it;
            break;
        default:
            MUST_BE_TRUE(false, "did not catch the first def instruction correctly");
            return;
        }

        // mov(8) m_new<1>, m_old<8;8:1>  {Align1}
        G4_Declare *srcDcl = source_send->getSrc(0)->getBase()->asRegVar()->getDeclare();
        G4_SrcRegRegion* newSrcOpnd = builder.Create_Src_Opnd_From_Dcl(srcDcl, builder.getRegionStride1());

        G4_INST* mov = builder.createMov(
            m->getExecSize(),
            builder.duplicateOperand(m->getDst()),
            newSrcOpnd,
            m->getOption(),
            false);
        bb->insertBefore(pos, mov);

        // maintain def-use.
        //
        // (1) Uses. m is ready to be deleted.
        m->transferUse(mov);

        // (2) Defs
        // The defs should be from definitions for source->send's src(0).
        //
        // mov (8) V244(1,0)<1>:ud V88(0,0)<8;8,1>:ud {Align1, NoMask}
        // mov (8) V244(0,0)<1>:ud r0.0<8;8,1>:ud {Align1, NoMask}
        // mov (1) V244(0,2)<1>:ud 0x1f:ud {Align1, NoMask}
        // mov (1) V244(0,0)<1>:ud 0:uw {Align1, NoMask}
        // mov (1) V244(0,1)<1>:ud 0:uw {Align1, NoMask}
        // add (1) a0.0<1>:ud r1.1<0;1,0>:ud 0x40a8000:ud {Align1, NoMask}
        // send (8) null<1>:ud V244(0,0)<8;8,1>:ud a0.0<0;1,0>:ud {Align1, NoMask}  <- source_send
        // mov (8) V89(0,0)<1>:d V34_histogram(1,0)<8;8,1>:d {Align1, Q1}
        // mov (8) V246(1,0)<1>:ud V89(0,0)<8;8,1>:ud {Align1, NoMask}
        // mov (8) V246(0,0)<1>:ud V244(0,0)<8;8,1>:ud {Align1, NoMask} <-- mov
        // mov (8) V246(0,0)<1>:ud r0.0<8;8,1>:ud {Align1, NoMask}      <-- m
        //
        // There are more than one defs here.
        //
        // Note that
        //
        // mov (8) V244(1,0)<1>:ud V88(0,0)<8;8,1>:ud {Align1, NoMask}
        //
        // is a definition of send but not for mov. We enable checked
        // while copying defs.
        source_send->copyDef(mov, Opnd_src0, Opnd_src0, /*checked*/true);
    }

    /*
    * compare the two instructions
    * they must have:
    * the same instruction opcode, predicate, condmodifier, optionString
    * the same dst, and number of src args
    */
    bool Optimizer::isHeaderOptCandidate(G4_INST *dst, G4_INST *src)
    {
        if (!dst   ||
            !src)
        {
            return true;
        }

        // Compare instructions
        if (dst->opcode() != src->opcode()             ||
            dst->getOption() != src->getOption()       ||
            dst->getExecSize() != src->getExecSize()   ||
            dst->getPredicate() != src->getPredicate() ||
            dst->getCondMod() != src->getCondMod())
        {
            return false;
        }

        if (dst->getNumSrc() != src->getNumSrc())
        {
            return false;
        }

        // Compare destination args
        G4_Operand *dst_dest = dst->getDst();
        G4_Operand *src_dest = src->getDst();
        if (!dst_dest   ||
            !src_dest  )
        {
            return false;
        }

        return true;
    }

    /*
    * compare the two instructions to see if they are redundent
    * they must have:
    * the same src args
    * the same def
    */
    bool Optimizer::isHeaderOptReuse(G4_INST *dst, G4_INST *src)
    {
        if (!dst   &&
            !src)
        {
            return true;
        }
        if (!dst   ||
            !src  )
        {
            return false;
        }

        for (unsigned int i = 0; i < G4_MAX_SRCS; i++)
        {
            G4_Operand* opnd = dst->getSrc(i);

            if (opnd != NULL && opnd->compareOperand(src->getSrc(i)) != Rel_eq)
            {
                return false;
            }
        }

        // The same number of def instructions
        if (dst->def_size() != src->def_size())
        {
            return false;
        }

        if (dst->def_size() == 0)
        {
            return true; // both have no def at all
        }

        for (auto ii = dst->def_begin(); ii != dst->def_end(); ii++)
        {
            bool sameDef = false;
            for (auto jj = src->def_begin(); jj != src->def_end(); jj++)
            {
                if ((*ii).first == (*jj).first &&
                    (*ii).second == (*jj).second)
                {
                    sameDef = true;
                    break; // break the inner jj loop
                }
            }
            if (sameDef == false)
            {
                return false;
            }
        }
        return true;
    }

    bool Optimizer::headerOptValidityCheck(MSGTable *dest, MSGTable *source)
    {
        if (!isHeaderOptCandidate(dest->a0Dot0, source->a0Dot0)   ||
            !isHeaderOptCandidate(dest->mDot0,  source->mDot0)    ||
            !isHeaderOptCandidate(dest->m,      source->m)        ||
            !isHeaderOptCandidate(dest->mDot1,  source->mDot1)    ||
            !isHeaderOptCandidate(dest->mDot2, source->mDot2))
        {
            return false;
        }

        if (dest->m)
        {
            if (!(dest->m->hasOneUse() && dest->m->use_front().first == dest->send))
            {
                return false;
            }
        }
        if (dest->mDot0)
        {
            if (!(dest->mDot0->hasOneUse() && dest->mDot0->use_front().first == dest->send))
            {
                return false;
            }
        }
        if (dest->mDot1)
        {
            if (!(dest->mDot1->hasOneUse() && dest->mDot1->use_front().first == dest->send))
            {
                return false;
            }
        }
        if (dest->mDot2)
        {
            if (!(dest->mDot2->hasOneUse() && dest->mDot2->use_front().first == dest->send))
            {
                return false;
            }
        }

        if (dest->send                         &&
            dest->send->getSrc(0)              &&
            dest->send->getSrc(0)->getTopDcl() &&
            source->send                       &&
            source->send->getSrc(0)            &&
            source->send->getSrc(0)->getTopDcl())
        {
            unsigned short dstSize, sourceSize;
            dstSize    = dest->send->getSrc(0)->getTopDcl()->getTotalElems() *
                dest->send->getSrc(0)->getTopDcl()->getElemSize();
            sourceSize = source->send->getSrc(0)->getTopDcl()->getTotalElems() *
                source->send->getSrc(0)->getTopDcl()->getElemSize();
            if (dstSize != sourceSize)
            {
                return false;
            }
        }

        return true;
    }

    // a class to store all presently valid values, each value is an instruction
    // if the number of values exceeds the max allowed, the oldest is removed
    class InstValues
    {
        const int maxNumVal;
        std::list<G4_INST*> values;

    public:
        InstValues(int maxCount) : maxNumVal(maxCount) {}

        void addValue(G4_INST* inst)
        {
            if (values.size() == maxNumVal)
            {
                values.pop_front();
            }
            values.push_back(inst);
        }

        // delete all values that may be invalid after inst
        void deleteValue(G4_INST* inst)
        {
            if (inst->isOptBarrier())
            {
                values.clear();
                return;
            }

            auto hasIndirectGather = [](G4_INST *inst)
            {
                for (int i = 0, numSrc = inst->getNumSrc(); i < numSrc; ++i)
                {
                    auto src = inst->getSrc(i);
                    if (src && src->isSrcRegRegion() && src->asSrcRegRegion()->isIndirect()
                        && src->asSrcRegRegion()->getRegion()->isRegionWH())
                    {
                        return true;
                    }
                }
                return false;
            };

            if (hasIndirectGather(inst))
            {
                // optimization is likely unprofitable due to high address register pressure in this case.
                // more importantly, it may actually cause RA to fail since we don't spill physical a0.0
                values.clear();
                return;
            }

            G4_DstRegRegion* dst = inst->getDst();
            auto interferes = [dst](G4_INST* valInst)
            {
                G4_DstRegRegion* valDst = valInst->getDst();
                if (dst->compareOperand(valDst) != Rel_disjoint)
                {
                    return true;
                }
                for (int i = 0, numSrc = valInst->getNumSrc(); i < numSrc; ++i)
                {
                    G4_Operand* src = valInst->getSrc(i);
                    if (src != nullptr && dst->compareOperand(src) != Rel_disjoint)
                    {
                        return true;
                    }
                }
                return false;
            };
            if (dst != nullptr)
            {
                values.remove_if (interferes);
            }
        }

        G4_INST* findValue(G4_INST* inst)
        {
            for (auto valInst : values)
            {
                if (inst->opcode() != valInst->opcode() ||
                    inst->getExecSize() != valInst->getExecSize())
                {
                    continue;
                }
                // skip flags for now
                if ((inst->getPredicate() || valInst->getPredicate()) ||
                    (inst->getCondMod() || valInst->getCondMod()))
                {
                    continue;
                }
                // emask checks
                if (inst->getMaskOffset() != valInst->getMaskOffset() ||
                    inst->isWriteEnableInst() ^ valInst->isWriteEnableInst())
                {
                    continue;
                }
                // all source should be isomorphic (same type/shape)
                bool srcMatch = true;
                for (int i = 0, numSrc = inst->getNumSrc(); i < numSrc; ++i)
                {
                    G4_Operand* src = inst->getSrc(i);
                    G4_Operand* valSrc = valInst->getSrc(i);
                    if (src == nullptr || valSrc == nullptr || src->compareOperand(valSrc) != Rel_eq)
                    {
                        srcMatch = false;
                        break;
                    }
                }
                if (srcMatch)
                {
                    return valInst;
                }
            }
            return nullptr;
        }

        void clear()
        {
            values.clear();
        }
    };

    G4_Operand* Optimizer::updateSendsHeaderReuse(
        std::vector<std::vector<G4_INST*>> &instLookUpTable,
        std::vector<G4_INST*> &iVector, INST_LIST_ITER endIter)
    {
        int bSize = (int)iVector.size();
        for (auto & Cache : instLookUpTable)
        {
            if (Cache.size() == bSize)
            {
                bool match[8] = { false };
                bool anyMatch = false;
                for (int index = 0; index < bSize; ++index)
                {
                    G4_INST* cInst = Cache[index];
                    G4_INST* iInst = iVector[index];

                    // opcode check
                    if (cInst->opcode() != iInst->opcode() ||
                        cInst->getExecSize() != iInst->getExecSize())
                    {
                        continue;
                    }
                    // flag check
                    if (cInst->getPredicate() != iInst->getPredicate() ||
                        cInst->getCondMod() != iInst->getCondMod())
                    {
                        continue;
                    }
                    // emask check
                    if (cInst->getMaskOffset() != iInst->getMaskOffset() ||
                        cInst->isWriteEnableInst() ^ iInst->isWriteEnableInst())
                    {
                        continue;
                    }
                    // dst check
                    G4_DstRegRegion *cDstRgn = cInst->getDst();
                    G4_DstRegRegion *iDstRgn = iInst->getDst();
                    if (cDstRgn->getRegOff() != iDstRgn->getRegOff() ||
                        cDstRgn->getSubRegOff() != iDstRgn->getSubRegOff() ||
                        cDstRgn->getHorzStride() != iDstRgn->getHorzStride() ||
                        cDstRgn->getRegAccess() != iDstRgn->getRegAccess() ||
                        cDstRgn->getType() != iDstRgn->getType())
                    {
                        continue;
                    }

                    // all source should be isomorphic (same type/shape) and unaltered between declaration and reuse
                    bool srcMatch = true;

                    for (int iSrc = 0, numSrc = cInst->getNumSrc(); iSrc < numSrc; ++iSrc)
                    {
                        G4_Operand* cOpnd = cInst->getSrc(iSrc);
                        G4_Operand* iOpnd = iInst->getSrc(iSrc);
                        if (cOpnd == nullptr || iOpnd == nullptr || cOpnd->compareOperand(iOpnd) != Rel_eq)
                        {
                            srcMatch = false;
                            break;
                        }
                    }

                    if (chkBwdWARdep(cInst, endIter))
                        srcMatch = false;

                    match[index] = srcMatch;
                    anyMatch |= srcMatch;
                }

                if (anyMatch)
                {
                    // at least partial match

                    for (int index = 0; index < bSize; ++index)
                    {
                        G4_INST* cInst = Cache[index];
                        G4_INST* iInst = iVector[index];

                        // mark it if there is match
                        if (match[index])
                        {
                            iInst->markDead();
                            continue;
                        }

                        // create new dst region to replace one in iVector[i]
                        G4_DstRegRegion *cDst = cInst->getDst();
                        G4_DstRegRegion *iDst = iInst->getDst();
                        G4_DstRegRegion *newDstRegion = builder.createDst(cDst->getTopDcl()->getRegVar(),
                                iDst->getRegOff(), iDst->getSubRegOff(), iDst->getHorzStride(), iDst->getType());
                        iInst->setDest(newDstRegion);

                        // update the look-up table list
                        Cache[index] = iInst;
                    }
                    return Cache[0]->getDst();
                }
            }
        }
        return nullptr;
    }

    //
    // Perform value numbering on writes to the extended msg descriptor for bindless access of the form
    // op (1) a0.2<1>:ud src0 src1 src2 {NoMask}
    // and remove redundant instructions.  This is limited to within BB
    //
    void Optimizer::cleanupBindless()
    {
        kernel.fg.resetLocalDataFlowData();
        kernel.fg.localDataFlowAnalysis();

        // Perform send header cleanup for bindless sampler/surface
        for (auto bb : fg)
        {
            std::vector<std::vector<G4_INST*>> instLookUpTable;
            std::vector<G4_INST*> instVector;
            for (auto iter = bb->begin(), iterEnd = bb->end();
                iter != iterEnd; ++iter)
            {
                G4_INST *inst = *iter;
                G4_DstRegRegion *dst = inst->getDst();

                if (dst != nullptr && dst->getTopDcl() != nullptr &&
                    dst->getTopDcl()->getCapableOfReuse())
                {
                    // it is header definition instruction
                    instVector.push_back(inst);
                }

                if (inst->isSplitSend())
                {
                    G4_Operand *header = inst->getSrc(0);
                    G4_Operand *exDesc = inst->getSrc(3);

                    if (header->getTopDcl() && header->getTopDcl()->getCapableOfReuse() &&
                        exDesc->isSrcRegRegion())
                    {

                        if (instVector.size() != 0)
                        {
                            // check if we can reuse cached values
                            G4_Operand *value = updateSendsHeaderReuse(instLookUpTable, instVector, iter);
                            if (value == nullptr)
                            {
                                // no found, cache the header
                                instLookUpTable.push_back(instVector);
                            }
                            else
                            {
                                // update sends header src
                                G4_SrcRegRegion* newHeaderRgn = builder.createSrc(value->getBase(), 0, 0, builder.getRegionStride1(), Type_UD);
                                inst->setSrc(newHeaderRgn, 0);
                            }
                        }
                    }
                    // clear header def
                    instVector.clear();
                }
                else if (inst->isSend())
                {
                    instVector.clear();
                }
            }
            bb->erase(
                std::remove_if (bb->begin(), bb->end(), [](G4_INST* inst) { return inst->isDead(); }),
                bb->end());
        }

        for (auto bb : fg)
        {
            InstValues values(4);
            for (auto iter = bb->begin(), iterEnd = bb->end(); iter != iterEnd;)
            {
                G4_INST* inst = *iter;

                auto isDstExtDesc = [](G4_INST* inst)
                {
                    G4_DstRegRegion* dst = inst->getDst();
                    if (dst && dst->getTopDcl() && dst->getTopDcl()->isMsgDesc())
                    {
                        // check that its single use is at src3 of split send
                        if (inst->use_size() != 1)
                        {
                            return false;
                        }
                        auto use = inst->use_front();
                        G4_INST* useInst = use.first;
                        if (useInst->isSend())
                        {
                            return true;
                        }
                    }
                    return false;
                };

                if (isDstExtDesc(inst))
                {
                    G4_INST* valInst = values.findValue(inst);
                    if (valInst != nullptr)
                    {
#if 0
                        std::cout << "can replace \n";
                        inst->emit(std::cout);
                        std::cout << "\n with \n";
                        valInst->emit(std::cout);
                        std::cout << "\n";
#endif
                        for (auto I = inst->use_begin(), E = inst->use_end(); I != E; ++I)
                        {
                            // each use is in the form of A0(0,0)<0;1,0>:ud in a send
                            G4_INST* useInst = I->first;
                            Gen4_Operand_Number num = I->second;
                            assert(useInst->isSend() && "use inst must be a send");
                            G4_SrcRegRegion* newExDesc = builder.createSrc(valInst->getDst()->getBase(), 0, 0,
                                builder.getRegionScalar(), Type_UD);
                            useInst->setSrc(newExDesc, useInst->getSrcNum(num));
                        }
                        iter = bb->erase(iter);
                        continue;
                    }
                    else
                    {
#ifdef DEBUG_VERBOSE_ON
                        std::cout << "add new value:\n";
                        inst->emit(std::cout);
                        std::cout << "\n";
#endif
                        // this is necessary since for msg desc we always the physical a0.0,
                        // so a new inst will invalidate the previous one
                        values.deleteValue(inst);
                        values.addValue(inst);
                    }
                }
                else
                {
                    values.deleteValue(inst);
                }
                ++iter;
            }
        }
    }

    /*
    * compare the two send and their defs
    * determine whether to remove the redundant mov inst
    * or reuse the previous header
    *
    * 1    mov (8) V152(0,0)<1>:ud r0.0<8;8,1>:ud {Align1, NoMask}
    * 2    mov (1) V152(0,2)<1>:ud 0x7000f:ud {Align1, NoMask}
    * 3    mov (1) V152(0,0)<1>:ud 0:uw {Align1, NoMask}
    * 4    mov (1) V152(0,1)<1>:ud 0:uw {Align1, NoMask}
    * 5    add (1) a0.0<1>:ud r1.0<0;1,0>:ud 0x2490000:ud {Align1, NoMask}
    * 6    send (8) V32_in(0,0)<1>:ud V152(0,0)<8;8,1>:ud a0.0<0;1,0>:ud {Align1, NoMask}
    *
    * 7    mov (8) V154(0,0)<1>:ud r0.0<8;8,1>:ud {Align1, NoMask}
    * 8    mov (1) V152(0,2)<1>:ud 0x1f:ud {Align1, NoMask}
    * 9    mov (1) V154(0,0)<1>:ud 0:uw {Align1, NoMask}
    * 10   mov (1) V154(0,1)<1>:ud 0:uw {Align1, NoMask}
    * 11   add (1) a0.0<1>:ud r1.1<0;1,0>:ud 0x2190000:ud {Align1, NoMask}
    * 12   send (8) V33(0,0)<1>:d V152(0,0)<8;8,1>:ud a0.0<0;1,0>:ud {Align1, NoMask}
    *
    * It is rather tricky to maintain def-use chains for this optimization.
    * The send instruction (Line 12) can reuse Inst[1, 3, 4] and need to
    * keep Inst[8, 11]. The defs for send at line 12 is Inst[1, 3, 4, 8, 11]
    * and the defs for send at line 6 is Inst[1, 2, 3, 4, 5].
    *
    * We take the following approach to maintain def-use.
    *
    * - Starting with initial valid defs, [7, 8, 9, 10, 11].
    *
    * - Each to be removed instruction transfers its use to a proper
    *   previous definition.
    *
    * - Each to be kept instruction remains, even there may have changes
    *   in its definition. For example, dst of move instruction at Line 8
    *   is changed from V154 to V152, but no def-use modification should
    *   be made for this instruction.
    *
    * - No def-use modification should be made to the final send, since
    *   all have been properly set.
    */
    void Optimizer::optMessageHeaders(MSGTableList& msgList,
        G4_BB* bb,
        DEFA0& myA0)
    {
        unsigned char redundancyCount = 0;
        bool isSameX, isSameY, isSameSize;
        bool replaceOldHeader = false;

        uint16_t payLoadSize;

        MSGTable *dest, *source;
        MSGTable_ITER iter=msgList.begin();

        if (iter == msgList.end())
        {
            return;
        }
        dest = *iter;   // dest is the front
        iter++;
        if (iter == msgList.end())
        {
            return;
        }
        source = *iter; // source is the cached one

        if (!headerOptValidityCheck(dest, source))
        {
            return;
        }

        if (isHeaderOptReuse(dest->a0Dot0, myA0.pred)  &&
            !myA0.isA0Redef)
        {
            // Transfer uses of dstDot0 to myA0.pred. This removes uses from
            // dest->a0Dot0 and add to myA0.pred. dest->a0Dot0 to be deleted.
            dest->a0Dot0->transferUse(myA0.pred, /*keepExisting*/true);
            dest->a0Dot0->markDead();
        }

        payLoadSize = dest->send->getMsgDesc()->MessageLength();

        isSameX = isHeaderOptReuse(dest->mDot0, source->mDot0)      &&
            !source->isXRedef ;

        isSameY = isHeaderOptReuse(dest->mDot1, source->mDot1)      &&
            !source->isYRedef;

        isSameSize = isHeaderOptReuse(dest->mDot2, source->mDot2)   &&
            !source->isSizeRedef;

        if (isSameX && dest->mDot0)
        {
            redundancyCount++;
        }
        if (isSameY && dest->mDot1)
        {
            redundancyCount++;
        }
        if (isSameSize && dest->mDot2)
        {
            redundancyCount++;
        }

        if (payLoadSize > 1                    &&
            redundancyCount < MESSAGE_HEADER_THRESHOLD)
        {
            return; // don't delete if redunant insts >=THRESHold
        };

        if (payLoadSize > 1 &&
            !(redundancyCount == 3 &&
                dest->send->getSrc(0)->compareOperand(source->send->getSrc(0))
                == Rel_eq))
        {
            dest->insertHeaderMovInst(source->send, builder, bb);
            replaceOldHeader = true;
        }

        {   // always remove "mov(8) Mx<1>, r0.0<8;8,1>:ud{Align1}"
            dest->m->markDead();
            if (!replaceOldHeader)
            {
                dest->m->transferUse(source->m, /*keepExisting*/true);
                dest->m=source->m;
            }
        }

        if (isSameX && dest->mDot0)
        {
            dest->mDot0->markDead();
            if (!replaceOldHeader)
            {
                dest->mDot0->transferUse(source->mDot0, /*keepExisting*/true);
                dest->mDot0 = source->mDot0;
            }
        }
        else if (payLoadSize==1 && dest->mDot0)
        {
            dest->reusePreviousHeader(dest->mDot0, source->mDot0, source->mDot2, builder);
            if (!replaceOldHeader)
            {
                source->mDot0 = dest->mDot0;
            }
        }

        if (isSameY && dest->mDot1)
        {
            dest->mDot1->markDead();
            if (!replaceOldHeader)
            {
                dest->mDot1->transferUse(source->mDot1, /*keepExisting*/true);
                dest->mDot1 = source->mDot1;
            }
        }
        else if (payLoadSize==1 && dest->mDot1)
        {
            dest->reusePreviousHeader(dest->mDot1, source->mDot1, source->mDot2, builder);
            if (!replaceOldHeader)
            {
                source->mDot1 = dest->mDot1;
            }
        }

        if (isSameSize && dest->mDot2)
        {
            dest->mDot2->markDead();
            if (!replaceOldHeader)
            {
                dest->mDot2->transferUse(source->mDot2, /*keepExisting*/true);
                dest->mDot2 = source->mDot2;
            }
        }
        else if (payLoadSize==1 && dest->mDot2)
        {
            dest->reusePreviousHeader(dest->mDot2, source->mDot2, source->mDot2, builder);
            if (!replaceOldHeader)
            {
                source->mDot2 = dest->mDot2;
            }
        }

        if (payLoadSize==1)
        {
            // Check this function's comments for why no def-use changes
            // should be made on resetting src(0).
            G4_Operand *src0 = source->send->getSrc(0);
            dest->send->setSrc(builder.duplicateOperand(src0), 0);
        }

        dest->opt = true;

        return;
    }

    bool Optimizer::isHeaderCachingCandidate(G4_INST *inst)
    {
        if (inst->isSend())
        {
            return true;
        }

        if (inst->useEmpty())
        {
            return false;
        }

        for (USE_EDGE_LIST_ITER iter = inst->use_begin(), iend = inst->use_end(); iter != iend; ++iter)
        {
            if ((*iter).first->isSend())
            {
                G4_INST *send = (*iter).first;
                G4_Operand *header = send->getSrc(0);
                G4_DstRegRegion *dst = inst->getDst();

                //def to BuiltInA0 is part of header opt
                if (inst->getDst()                                            &&
                    inst->getDst()->getBase()                   &&
                    inst->getDst()->getBase()->isRegVar()       &&
                    inst->getDst()->getBase()->asRegVar() ==
                    builder.getBuiltinA0()->getRegVar()                         &&
                    inst->getDst()->getRegOff() == 0            &&
                    inst->getDst()->getSubRegOff() == 0        )
                {
                    return true;
                }

                // make sure that dst of the current inst is header, not payload
                // header is hard-coded to be 32 bytes
                if (header->getTopDcl()    == dst->getTopDcl()         &&
                    dst->getLeftBound() >= header->getLeftBound()      &&
                    dst->getRightBound() <= header->getLeftBound() + numEltPerGRF<Type_UB>() -1)
                {
                    return true;
                }
                return false;
            }
        }

        return false;
    }

   /*
    * mark the below "and" instruction as redundant;
    * the "and" instruction is the header for a barrier:
    *   and (8) r1.0<1>:ud r0.2<0;1,0>:ud 0xf000000:ud {Align1, NoMask}
    *   send (1) null<1>:ud r1 0x3 0x2000004:ud{Align1}
    *   wait n0:ud {Align1}
    */
    void Optimizer::removeRedundantBarrierHeaders(G4_INST *sendInst,
                                          G4_SrcRegRegion* barrierSrc0,
                                          bool first)
    {
        bool barrier = false;
        G4_SrcRegRegion *src0 = NULL;
        if (!first) // skip the check as already done so for first barrier
        {
            barrier = isBarrierPattern(sendInst, src0);
        }
        if (barrier || first)
        {
            auto item = sendInst->def_begin();
            G4_INST *andInst = item->first;
            // delete all the uses of andInst
            // addInst and sendInst will have no def and no use
            andInst->removeAllUses();
            // sendInst.src0 (addInst.dst) will be replaced by barrierSend.src0
            // create a new G4_SrcRegRegion, which is a copy of barrierSend.src0
            G4_SrcRegRegion *src = builder.createSrc(
                    barrierSrc0->getTopDcl()->getRegVar(),
                    0,
                    0,
                    barrierSrc0->getRegion(),
                    barrierSrc0->getTopDcl()->getElemType());
            sendInst->setSrc(src, 0);
            andInst->markDead();
        }
    }

    /*
    * pattern match of a code sequence for ISA_BARRIER:
    *   and (8) r1.0<1>:ud r0.2<0;1,0>:ud 0xf000000:ud {Align1, NoMask}
    *   send (1) null<1>:ud r1 0x3 0x2000004:ud{Align1}
    *   wait n0:ud {Align1}
    */
    bool Optimizer::isBarrierPattern(G4_INST *sendInst,
                                     G4_SrcRegRegion *& barrierSendSrc0)
    {
        /*
         * check G4_send
         */
        G4_SendMsgDescriptor *desc = sendInst->getMsgDesc();
        uint32_t descVal = desc->getDesc();
        if ((desc->getFuncId() == SFID::GATEWAY) &&
            (descVal == (0x1 << 25) + 0x4) && // 0x2000004
            (sendInst->def_size() == 1))
        {
            auto item = sendInst->def_begin();
            G4_INST *andInst = item->first; // getting "and" from send's def

           /*
            * check G4_and
            */
            if ((andInst) && (andInst->opcode() == G4_and) &&
                (item->second == Opnd_src0))
            {
                G4_Operand *src0 = andInst->getSrc(0);
                G4_Operand *src1 = andInst->getSrc(1);

                bool isSrc0 =
                    ((src0->isSrcRegRegion()) &&
                     (src0->asSrcRegRegion()->getBase()) &&
                     (src0->asSrcRegRegion()->getBase()->isRegVar()) &&
                     (src0->asSrcRegRegion()->getBase()->asRegVar() ==
                      builder.getBuiltinR0()->getRegVar()) &&
                     (src0->asSrcRegRegion()->getRegOff() == 0) &&
                     (src0->asSrcRegRegion()->getSubRegOff() == 2)); // r0.2

                bool isSrc1 = src1->isImm() && !src1->isRelocImm() &&
                    src1->asImm()->getInt() == (builder.getPlatform() >= GENX_SKL ? 0x8F000000 : 0x0F000000);

                if (isSrc0 && isSrc1 && sendInst->getSrc(0) &&
                    sendInst->getSrc(0)->isSrcRegRegion())
                {
                    barrierSendSrc0 = sendInst->getSrc(0)->asSrcRegRegion();
                    return true;
                }
            }
        }

        return false;
    }

    /*
     * add the the barrier header as the top instruction
     */
    void Optimizer::hoistBarrierHeaderToTop(G4_SrcRegRegion *barrierSendSrc0)
    {
        G4_Declare *dcl = barrierSendSrc0->getTopDcl();
        IR_Builder *mybuilder = &builder;

        // below code is copied from translateVISASyncInst() for ISA_BARRIER
        // all other dwords are ignored
        // and (8) r32.0:ud r0.2:ud 0x0F000000

        G4_SrcRegRegion* r0_src_opnd = builder.createSrc(
            mybuilder->getBuiltinR0()->getRegVar(),
            0,
            2,
            builder.getRegionScalar(),
            Type_UD);
        G4_DstRegRegion *dst1_opnd = builder.createDst(
            dcl->getRegVar(),
            0,
            0,
            1,
            Type_UD);

        G4_Imm * g4Imm = NULL;

        //for SKL+ there are 5 bits for barrierID
        //5th bit is stored in bit 31 of second dword
        if (builder.getPlatform() < GENX_SKL)
        {
            g4Imm = builder.createImm(0x0F000000, Type_UD);
        }
        else
        {
            g4Imm = builder.createImm(0x8F000000, Type_UD);
        }

        G4_INST *andInst = builder.createBinOp(G4_and, g4::SIMD8, dst1_opnd, r0_src_opnd, g4Imm, InstOpt_WriteEnable, false);
        for (auto bb : fg)
        {
            auto iter = std::find_if(bb->begin(), bb->end(), [](G4_INST* inst) { return !inst->isLabel();});
            if (iter != bb->end())
            {
                bb->insertBefore(iter, andInst);
                return;
            }
        }
    }

    /*
     * check whether there are new definitions in order to determine redundancy
     */
    bool Optimizer::chkNewDefBetweenSends(G4_INST *inst, MSGTableList& msgList, DEFA0& myA0)
    {
        bool isDef = false;
        msgList.unique();

        // check SIMD8 VxH region everytime
        if (inst->getDst()                         &&
            inst->getDst()->isAddress())
        {
            isDef = myA0.isA0Redef = true;
        }
        else
        {
            for (int i = 0; i < inst->getNumSrc(); i++)
            {
                if (inst->getSrc(i)                     &&
                    inst->getSrc(i)->isAddress())
                {
                    isDef = myA0.isA0Redef = true;
                    break;
                }
            }
        }

        if (msgList.size() < 2)
        {
            return false;
        }
        MSGTable_ITER ii = msgList.begin();
        if (ii == msgList.end())
        {
            return false;
        }

        ii++;

        if (ii == msgList.end())
        {
            return false;
        }

        MSGTable* last = *(ii);
        if (last == NULL       ||
            last->send == NULL)
        {
            return false;
        }
        G4_Operand *def = inst->getDst();
        if (def == NULL)
        {
            return false;
        }
        if (last->mDot0                                        &&
             (def->compareOperand(last->mDot0->getSrc(0)) == Rel_eq ||
               (last->mDot0->getSrc(1) && def->compareOperand(last->mDot0->getSrc(1)) == Rel_eq) ||
                (last->mDot0->getSrc(2) && def->compareOperand(last->mDot0->getSrc(2)) == Rel_eq)))
        {
            isDef = last->isXRedef = true;
        }
        else if (last->mDot1                                   &&
             (def->compareOperand(last->mDot1->getSrc(0)) == Rel_eq ||
               (last->mDot1->getSrc(1) && def->compareOperand(last->mDot1->getSrc(1)) == Rel_eq) ||
               (last->mDot1->getSrc(2) && def->compareOperand(last->mDot1->getSrc(2)) == Rel_eq)))
        {
            isDef = last->isYRedef = true;
        }
        else if (last->mDot2                                   &&
             (def->compareOperand(last->mDot2->getSrc(0)) == Rel_eq  ||
               (last->mDot2->getSrc(1) && def->compareOperand(last->mDot2->getSrc(1)) == Rel_eq) ||
               (last->mDot2->getSrc(2) && def->compareOperand(last->mDot2->getSrc(2)) == Rel_eq)))
        {
             isDef = last->isSizeRedef = true;
        }
        else if (last->m                                       &&
            (def->compareOperand(last->m->getSrc(0)) == Rel_eq ||
              (last->m->getSrc(1) && def->compareOperand(last->m->getSrc(1)) == Rel_eq) ||
              (last->m->getSrc(2) && def->compareOperand(last->m->getSrc(2)) == Rel_eq)))
        {
            isDef = last->isR0Dot0Redef = true;
        }
        return isDef;

    }

    /*
    * Cache the send and its def into a table for optimization
    */
    void Optimizer::addEntryToMessageTable(G4_INST *inst,
        MSGTableList& msgList,
        G4_BB* bb,
        INST_LIST_ITER ii,
        DEFA0 &myA0)
    {
        MSGTable *item = msgList.front();
        if (inst->isSend())
        {
            item->send = inst;
            item->opt           = false;
            item->isR0Dot0Redef = false;
            item->isXRedef      = false;
            item->isYRedef      = false;
            item->isSizeRedef   = false;

            if (item->invalid)
            {
                msgList.pop_front();
            }
            else if (item->a0Dot0 != NULL &&   // only def a0.0
                 (item->m == NULL  || item->mDot2 == NULL))
            {
                if (isHeaderOptCandidate(item->a0Dot0, myA0.pred))
                {
                    if (isHeaderOptReuse(item->a0Dot0, myA0.pred)  &&
                        !myA0.isA0Redef)
                    {
                        // Transfer uses of a0Dot0 to myA0.pred. This removes uses from
                        // a0Dot0 and add to myA0.pred. item->a0Dot0 to be deleted.
                        item->a0Dot0->transferUse(myA0.pred, /*keepExisting*/true);
                        item->a0Dot0->markDead();
                    }
                }
                msgList.pop_front();
            }
            else if (item->a0Dot0 &&  item->m && item->mDot2) // complete header def
            {

                msgList.unique();
                if (msgList.size() >= 2)
                {
                    optMessageHeaders(msgList, bb, myA0);
                    if (msgList.front()->opt &&
                        msgList.front()->send->getMsgDesc()->MessageLength() == 1)
                    {
                        // keep the oldest send for subsequent read operations
                        // but the instruction to define a0.0 needs to be latest
                        //msgList.back()->a0Dot0 = msgList.front()->a0Dot0;
                        msgList.pop_front(); // delete first element
                    }
                    else if (msgList.front()->opt &&
                        msgList.front()->send->getMsgDesc()->MessageLength() >= 1)
                    {
                        // keep the latest send for subsequent write operations
                        msgList.pop_back();
                    }
                    else
                    {
                        msgList.pop_back();
                    }
                    myA0.isA0Redef      = false;
                }
            }
            else
            {
                // not an optimization candidate
                msgList.pop_front();
            }
        }
        else if (inst->getDst() &&
            inst->getDst()->getBase() &&
            inst->getDst()->getBase()->isRegVar() &&
            inst->getDst()->getBase()->asRegVar() == builder.getBuiltinA0()->getRegVar() &&
            inst->getDst()->getRegOff() == 0 &&
            inst->getDst()->getSubRegOff() == 0)
        {
            // is builtInA0.0
            item->a0Dot0 = inst;
            item->a0Dot0_it = ii;

            if (myA0.curr == NULL)
            {
                myA0.pred = NULL;
                myA0.isA0Redef      = false;
            }
            else if (!myA0.curr->isDead())
            {
                // only update the a0 def when we didn't remove it
                myA0.pred = myA0.curr;
                myA0.predIt = myA0.currIt;
            }
            myA0.currIt = ii;
            myA0.curr = inst;
        }
       else if (inst->getSrc(0))
       {
            G4_DstRegRegion *dst = inst->getDst();
            if (dst)
            {
                if (dst->getRegOff() == 0)
                {
                    // mov(8) m.0, builtInR0.0
                    G4_Operand *src = inst->getSrc(0);
                    if (dst->getSubRegOff() == 0 &&
                        inst->getExecSize() == g4::SIMD8 &&
                        src &&
                        src->isSrcRegRegion() &&
                        src->asSrcRegRegion()->getBase() &&
                        src->asSrcRegRegion()->getBase()->isRegVar() &&
                        src->asSrcRegRegion()->getBase()->asRegVar() == builder.getBuiltinR0()->getRegVar() &&
                        src->asSrcRegRegion()->getRegOff() == 0 &&
                        src->asSrcRegRegion()->getSubRegOff() == 0)
                    {
                        if (item->first == HEADER_UNDEF)
                            item->first = HEADER_FULL_REGISTER;
                        item->m = inst;
                        item->m_it = ii;
                    }
                    // mov(1) m.0
                    else if (dst->getSubRegOff() == 0 &&
                        inst->getExecSize() == g4::SIMD1)
                    {
                        if (item->first == HEADER_UNDEF)
                            item->first = HEADER_X;
                        item->mDot0 = inst;
                        item->mDot0_it = ii;
                    }
                    // mov(1) m.1
                    else if (dst->getSubRegOff() == 1 &&
                        inst->getExecSize() == g4::SIMD1)
                    {
                        if (item->first == HEADER_UNDEF)
                            item->first = HEADER_Y;
                        item->mDot1 = inst;
                        item->mDot1_it = ii;
                    }
                    // mov(1) m0.2
                    else if (dst->getSubRegOff() == 2  &&
                        inst->getExecSize() == g4::SIMD1)
                    {
                        if (item->first == HEADER_UNDEF)
                            item->first = HEADER_SIZE;
                        item->mDot2 = inst;
                        item->mDot2_it = ii;
                    }
                    else
                    {
                        // unrecognized update to header
                        item->invalid = true;
                    }
                }
            }
        }

    }

    void Optimizer::messageHeaderReport(size_t ic_before,
        size_t ic_after,
        G4_Kernel& kernel)
    {
        if (builder.getOption(vISA_OptReport))
        {
            std::ofstream optReport;
            getOptReportStream(optReport, builder.getOptions());
            optReport << "             === Message Header Optimization ===\n";
            optReport<< std::fixed << "\n";
            optReport<< kernel.getName() <<" is reduced from "
                <<ic_before <<" to " <<ic_after <<" instructions.\n";
            if (((float)(ic_before)) != 0.0)
            {
            optReport << std::setprecision(0)
                << (float)((ic_before-ic_after)*100)/(float)(ic_before)
                << "% instructions of this kernel are removed.\n";
            }
            optReport << "\n";
            closeOptReportStream(optReport);
        }
    }

    //
    // optimizer for removal of redundant message header instructions
    //
    void Optimizer::cleanMessageHeader()
    {
        MSGTableList msgList;
        BB_LIST_ITER ib, bend(fg.end());
        size_t ic_before = 0;
        size_t ic_after = 0;

        std::stack<MSGTable*> toDelete;

        bool isRedundantBarrier = false;
        G4_SrcRegRegion *barrierSendSrc0 = NULL;

        for (ib = fg.begin(); ib != bend; ++ib)
        {

            msgList.clear();
            MSGTable *newItem   = (MSGTable *)mem.alloc(sizeof(MSGTable));
            toDelete.push(newItem);
            memset(newItem, 0, sizeof(MSGTable));
            newItem->first      = HEADER_UNDEF;

            msgList.push_front(newItem);
            G4_BB* bb = (*ib);
            INST_LIST_ITER ii = bb->begin();
            INST_LIST_ITER iend = bb->end();
            ic_before += bb->size();

            DEFA0 myA0;
            myA0.curr      = NULL;
            myA0.pred      = NULL;
            myA0.isA0Redef = false;

            for (; ii != iend; ii++)
            {
                G4_INST *inst = *ii;
                if (isHeaderCachingCandidate(inst))
                {
                    if (inst->opcode() == G4_send && isRedundantBarrier)
                    {
                        removeRedundantBarrierHeaders(inst, barrierSendSrc0, false);
                    }
                    else if (inst->opcode() == G4_send && !isRedundantBarrier)
                    {
                        isRedundantBarrier = isBarrierPattern(inst, barrierSendSrc0);
                        if (isRedundantBarrier)
                        {
                            removeRedundantBarrierHeaders(inst, barrierSendSrc0, true);
                        }
                    }

                    addEntryToMessageTable(inst, msgList, bb, ii, myA0);
                    if (inst->isSend())
                    {
                        MSGTable * item = (MSGTable *)mem.alloc(sizeof(MSGTable));
                        toDelete.push(item);
                        memset(item, 0, sizeof(MSGTable));
                        item->first  = HEADER_UNDEF;
                        msgList.push_front(item);
                    }
                }
                else
                {
                    chkNewDefBetweenSends(inst, msgList, myA0);
                }
            }

            // Dead code elimination
            for (ii = bb->begin(); ii != bb->end();)
            {
                G4_INST *inst = *ii;
                INST_LIST_ITER curr = ii++;
                if (inst->isDead())
                {
                    inst->removeUseOfInst();
                    bb->erase(curr);
                }
            }

            ic_after += bb->size();
        }

        messageHeaderReport(ic_before, ic_after, kernel);

        if (isRedundantBarrier)
        {
            hoistBarrierHeaderToTop(barrierSendSrc0);
        }

        //
        // Destroy STL iterators allocated using mem manager
        //
        while (toDelete.size() > 0)
        {
            toDelete.top()->~MSGTable();
            toDelete.pop();
        }
        msgList.clear();
    }
    //  The end of message header optimization

    void getOptReportStream(std::ofstream& reportStream, const Options *opt)
    {
        const char *asmFileName;
        opt->getOption(VISA_AsmFileName, asmFileName);
        std::string optReportFileName = std::string(asmFileName) + "_optreport.txt";
        reportStream.open(optReportFileName, std::ios::out | std::ios::app );
        MUST_BE_TRUE(reportStream, "Fail to open " << optReportFileName);
    }

    void closeOptReportStream(std::ofstream& reportStream)
    {
        reportStream.close();
    }

    void Optimizer::sendFusion()
    {
        (void) doSendFusion(&fg, &mem);
    }

    // For a subroutine, insert a dummy move with {Switch} option immediately
    // before the first non-label instruction in BB. Otherwie, for a following
    // basic block, insert a dummy move before *any* instruction to ensure that
    // no instruction should be placed between the targe jip/uip label and its
    // associated instruction.
    void Optimizer::addSwitchOptionToBB(G4_BB* bb, bool isSubroutine)
    {
        auto instIter = bb->begin();
        if (isSubroutine)
        {
            for (auto instEnd = bb->end(); instIter != instEnd; ++instIter)
            {
                G4_INST* bbInst = *instIter;
                if (bbInst->isLabel())
                {
                    continue;
                }
                else
                {
                    break;
                }
            }
        }

        if (instIter != bb->end() && ((*instIter)->getOption() & InstOpt_Switch))
        {
            // this BB is already processed, skip
            return;
        }

        // mov (1) null<1>:ud r0.0<0;1,0>:ud {Switch}
        G4_DstRegRegion* movDst = builder.createNullDst(Type_UD);
        G4_SrcRegRegion* movSrc = builder.Create_Src_Opnd_From_Dcl(builder.getBuiltinR0(), builder.getRegionScalar());
        G4_INST* movInst = builder.createMov(g4::SIMD1, movDst, movSrc, InstOpt_WriteEnable, false);
        movInst->setOptionOn(InstOpt_Switch);
        bb->insertBefore(instIter, movInst);
    }

    void Optimizer::linePlaneWA(G4_INST* inst)
    {
        // Putting it here instead of in HW confomrity because we need original src0 region
        // in scheduler to calculate RB correctly. Otherwise setup moves for src0 get scheduled after instruction
        //
        // HW check #12: Check and correct the first operand for line instruction
        // Actually it must be a replicated stream of 4 contiguous elements.
        // That means <0;4,1> region. But in asm code it must be presented as
        // replicated scalar - <0;1,0>.
        if (inst->opcode() == G4_line || inst->opcode() == G4_pln)
        {
            G4_Operand *src = inst->getSrc(0);
            const RegionDesc *rd = src->isSrcRegRegion() ? src->asSrcRegRegion()->getRegion() : NULL;
            MUST_BE_TRUE(rd != NULL, " Src0 of line inst is not regregion. ");
            if (rd->isScalar())
            {
                return;
            }
            MUST_BE_TRUE((rd->vertStride == 0 || rd->vertStride == 4) && rd->width == 4,
                "Unexpected region for the first line operand.");

            // create a new rd for src0
            const RegionDesc *new_rd = builder.getRegionScalar();
            src->asSrcRegRegion()->setRegion(new_rd);
        }
    }

    //
    // This inserts two dummy moves to clear flag dependencies before EOT:
    // mov(1) null:ud f0.0<0;1,0>:ud{ Align1, Q1, NoMask }
    // mov(1) null:ud f1.0<0;1,0>:ud{ Align1, Q1, NoMask }
    // This is done if f0/f1 is ever defined in a BB but not used in it, as we conservatively assume
    // that the flag may be undefined when the EOT is reached.
    // Note that USC only does this if EOT is inside control flow, i.e., EOT is an early exit
    //
    void Optimizer::clearARFDependencies()
    {
        auto flagToInt = [](G4_Areg* areg)
        {
            MUST_BE_TRUE(areg->isFlag(), "expect F0 or F1");
            return areg->getArchRegType() == AREG_F0 ? 0 : 1;
        };
        // see if F0 and F1 are ever defined but not used in the same BB
        bool unusedFlag[2]; // f0 and f1
        unusedFlag[0] = unusedFlag[1] = false;
        for (auto bb : fg)
        {
            bool unusedFlagLocal[2]; // f0 and f1
            unusedFlagLocal[0] = unusedFlagLocal[1] = false;

            for (auto inst : *bb)
            {
                // check predicate source
                if (inst->getPredicate())
                {
                    G4_VarBase* flag = inst->getPredicate()->getBase();
                    if (flag->isRegVar())
                    {
                        G4_Areg* areg = flag->asRegVar()->getPhyReg()->asAreg();
                        unusedFlagLocal[flagToInt(areg)] = false;
                    }
                }
                else
                {
                    // check explicit source
                    for (int i = 0; i < inst->getNumSrc(); ++i)
                    {
                        if (inst->getSrc(i) && inst->getSrc(i)->isSrcRegRegion() && inst->getSrc(i)->isFlag())
                        {
                            G4_SrcRegRegion* src = inst->getSrc(i)->asSrcRegRegion();
                            if (src->getBase()->isRegVar())
                            {
                                G4_Areg* flag = src->getBase()->asRegVar()->getPhyReg()->asAreg();
                                unusedFlagLocal[flagToInt(flag)] = false;
                            }
                        }
                    }
                }

                // check explicit dst
                if (inst->getDst() && inst->getDst()->isFlag())
                {
                    // flag is an explicit dst
                    G4_DstRegRegion* dst = inst->getDst();
                    if (dst->getBase()->isRegVar())
                    {
                        G4_Areg* flag = dst->getBase()->asRegVar()->getPhyReg()->asAreg();
                        unusedFlagLocal[flagToInt(flag)] = true;
                    }
                }
                // check cond mod
                else if (G4_VarBase* flag = inst->getCondModBase())
                {
                    if (flag->isRegVar())
                    {
                        G4_Areg* areg = flag->asRegVar()->getPhyReg()->asAreg();
                        unusedFlagLocal[flagToInt(areg)] = true;
                    }
                }
            }

            if (unusedFlagLocal[0] && unusedFlag[0] == false)
            {
                unusedFlag[0] = true;
            }

            if (unusedFlagLocal[1] && unusedFlag[1] == false)
            {
                unusedFlag[1] = true;
            }

            if (unusedFlag[0] && unusedFlag[1])
            {
                break;
            }
        }

        if (unusedFlag[0] || unusedFlag[1])
        {
            for (auto bb : fg)
            {
                if (bb->size() == 0)
                {
                    return;
                }
                G4_INST* inst = bb->back();
                if (inst->isEOT())
                {
                    auto instIter = bb->end();
                    --instIter;
                    if (unusedFlag[0])
                    {
                        G4_SrcRegRegion* flagSrc = builder.createSrc(builder.phyregpool.getF0Reg(), 0, 0,
                            builder.getRegionScalar(), Type_UD);
                        G4_DstRegRegion* nullDst = builder.createNullDst(Type_UD);
                        G4_INST* inst = builder.createMov(g4::SIMD1, nullDst, flagSrc, InstOpt_WriteEnable, false);
                        bb->insertBefore(instIter, inst);
                    }
                    if (unusedFlag[1])
                    {
                        G4_SrcRegRegion* flagSrc = builder.createSrc(builder.phyregpool.getF1Reg(), 0, 0,
                            builder.getRegionScalar(), Type_UD);
                        G4_DstRegRegion* nullDst = builder.createNullDst(Type_UD);
                        G4_INST* inst = builder.createMov(g4::SIMD1, nullDst, flagSrc, InstOpt_WriteEnable, false);
                        bb->insertBefore(instIter, inst);
                    }
                }
            }
        }
    }

    // change the send src0 region to be consistent with assembler expectation
    // We do it here instead of HW conformity since they only affect binary encoding
    // ToDo: this should not be necessary anymore, should see if we can remove
    void Optimizer::fixSendSrcRegion(G4_INST* inst)
    {
        if (inst->isSend() && inst->getSrc(0) != NULL)
        {
            const RegionDesc* newDesc = NULL;
            uint8_t execSize = inst->getExecSize();
            if (execSize == 1)
            {
                newDesc = builder.getRegionScalar();
            }
            else if (execSize > 8)
            {
                newDesc = builder.getRegionStride1();
            }
            else
            {
                newDesc = builder.createRegionDesc(execSize, execSize, 1);
            }
            inst->getSrc(0)->asSrcRegRegion()->setRegion(newDesc);
        }
    }

    G4_SrcRegRegion* IR_Builder::createSubSrcOperand(
        G4_SrcRegRegion* src, uint16_t start, uint8_t size, uint16_t newVs, uint16_t newWd)
    {
        const RegionDesc* rd = NULL;
        uint16_t vs = src->getRegion()->vertStride, hs = src->getRegion()->horzStride, wd = src->getRegion()->width;
        G4_Type srcType = src->getType();
        // even if src has VxH region, it could have a width that is equal to the new exec_size,
        // meaning that it's really just a 1x1 region.
        auto isVxHRegion = src->getRegion()->isRegionWH() && wd < size;
        if (!isVxHRegion)
        {
            // r[a0.0,0]<4;2,1> and size is 4 or 1
            if (size < newWd)
            {
                newWd = size;
            }
            rd = size == 1 ? getRegionScalar() :
                createRegionDesc(size == newWd ? newWd * hs : newVs, newWd, hs);
            rd = getNormalizedRegion(size, rd);
        }

        if (src->getRegAccess() != Direct)
        {
            if (isVxHRegion)
            {
                // just handle <1,0>
                if (start > 0)
                {
                    // Change a0.N to a0.(N+start)
                    assert((start % wd == 0) && "illegal starting offset and width combination");
                    uint16_t subRegOff = src->getSubRegOff() + start / wd;
                    return createIndirectSrc(src->getModifier(), src->getBase(), src->getRegOff(), subRegOff,
                        src->getRegion(), src->getType(), src->getAddrImm());
                }
                else
                {
                    return duplicateOperand(src);
                }
            }

            if (start > 0)
            {
                short numRows = start / wd;
                short numCols = start % wd;
                short newOff = (numRows * vs + numCols * hs) * TypeSize(srcType);
                auto newSrc = createIndirectSrc(src->getModifier(), src->getBase(), src->getRegOff(), src->getSubRegOff(), rd,
                    src->getType(), src->getAddrImm() + newOff);
                return newSrc;
            }
            else
            {
                G4_SrcRegRegion* newSrc = duplicateOperand(src);
                newSrc->setRegion(rd);
                return newSrc;
            }
        }

        // direct access oprand
        if (src->isAccReg())
        {
            switch (srcType)
            {
            case Type_F:
                // must be acc1.0 as result of simd16 -> 8 split
                assert(size == 8 && "only support simd16->simd8 for now");
                return createSrcRegRegion(src->getModifier(), Direct, phyregpool.getAcc1Reg(), 0, 0, src->getRegion(), srcType);
            case Type_HF:
            {
                // can be one of acc0.8, acc1.0, acc1.8
                if (src->getBase()->asAreg()->getArchRegType() == AREG_ACC1)
                {
                    start += 16;
                }
                G4_Areg* accReg = start >= 16 ? phyregpool.getAcc1Reg() : phyregpool.getAcc0Reg();
                return createSrcRegRegion(src->getModifier(), Direct, accReg, 0, start % 16, src->getRegion(), srcType);

            }
            default:
                // Keep using acc0 for other types.
                return duplicateOperand(src);
            }
        }

        // Since this function creates a new sub src operand based on a start offset,
        // the reg and subreg offsets need to be re-computed.
        uint16_t regOff, subRegOff, subRegOffByte, newSubRegOffByte, newEleOff, newEleOffByte, crossGRF;

        newEleOff = start * hs +
            (start >= wd && vs != wd * hs ? (start / wd * (vs - wd * hs)) : 0);

        // Linearize offsets into bytes to verify potential GRF crossing
        newEleOffByte = newEleOff * src->getTypeSize();
        subRegOffByte = src->getSubRegOff() * src->getTypeSize();

        // If subreg crosses GRF size, update reg and subreg offset accordingly
        newSubRegOffByte = subRegOffByte + newEleOffByte;
        crossGRF = newSubRegOffByte / numEltPerGRF<Type_UB>();

        newSubRegOffByte = newSubRegOffByte - crossGRF * numEltPerGRF<Type_UB>();

        // Compute final reg and subreg offsets
        regOff = src->getRegOff() + crossGRF;
        subRegOff = newSubRegOffByte / src->getTypeSize();

        return createSrcRegRegion(src->getModifier(), Direct, src->getBase(), regOff, subRegOff, rd,
            srcType, src->getAccRegSel());

    }

    G4_DstRegRegion* IR_Builder::createSubDstOperand(G4_DstRegRegion* dst, uint16_t start, uint8_t size)
    {
        if (dst->getRegAccess() != Direct)
        {
            if (start > 0)
            {
                // just change immediate offset
                uint16_t newOff = start * dst->getTypeSize() * dst->getHorzStride();
                G4_DstRegRegion* newDst = duplicateOperand(dst);
                newDst->setImmAddrOff(dst->getAddrImm() + newOff);
                return newDst;
            }
            else
            {
                return duplicateOperand(dst);
            }
        }

        uint16_t regOff, subRegOff;
        if (start > 0)
        {
            G4_Type dstType = dst->getType();
            uint16_t hs = dst->getHorzStride();
            if (dst->isAccReg())
            {
                switch (dstType)
                {
                case Type_F:
                    // must be acc1.0 as result of simd16 -> 8 split
                    assert(size == 8 && "only support simd16->simd8 for now");
                    return createDst(
                        phyregpool.getAcc1Reg(),
                        0,
                        0,
                        hs,
                        dstType);
                case Type_HF:
                {
                    // can be one of acc0.8, acc1.0, acc1.8
                    if (dst->getBase()->asAreg()->getArchRegType() == AREG_ACC1)
                    {
                        start += 16;
                    }
                    G4_Areg* accReg = start >= 16 ? phyregpool.getAcc1Reg() : phyregpool.getAcc0Reg();
                    return createDst(accReg, 0, start % 16, hs, dstType);
                }
                default:

                    // other types do not support acc1, we have to continue to use acc0
                    // whoever doing the split must fix the dependencies later by shuffling instructions
                    // so that acc0 does not get overwritten
                    return createDstRegRegion(*dst);
                }
            }

            // Linearize offsets into bytes to verify potential GRF crossing
            uint16_t newSubRegOff, newSubRegOffByte, crossGRF;

            newSubRegOff = dst->getSubRegOff() + start * hs;
            newSubRegOffByte = newSubRegOff * TypeSize(dstType);

            crossGRF = newSubRegOffByte / numEltPerGRF<Type_UB>();
            newSubRegOffByte = newSubRegOffByte - crossGRF * numEltPerGRF<Type_UB>();

            // Compute final reg and subreg offsets
            regOff = dst->getRegOff() + crossGRF;
            subRegOff = newSubRegOffByte / TypeSize(dstType);

            // create a new one
            return createDst(dst->getBase(), regOff, subRegOff, hs, dst->getType(),
                dst->getAccRegSel());
        }
        else
        {
            G4_DstRegRegion* newDst = duplicateOperand(dst);
            return newDst;
        }
    }

    G4_INST* IR_Builder::makeSplittingInst(G4_INST* inst, G4_ExecSize ExSize)
    {
        // Instruction's option is reused. Call sites should reset this field
        // properly. FIXME: fix all call sites.
        G4_INST* newInst = NULL;
        G4_opcode op = inst->opcode();
        if (inst->isMath())
        {
            newInst = createMathInst(NULL, inst->getSaturate(), ExSize,
                NULL, NULL, NULL, inst->asMathInst()->getMathCtrl(),
                inst->getOption(), true);
        }
        else if (inst->getNumSrc() < 3)
        {
            newInst = createInternalInst(
                NULL, op, NULL, inst->getSaturate(), ExSize, NULL, NULL, NULL,
                inst->getOption());
        }
        else
        {
            newInst = createInternalInst(
                NULL, op, NULL, inst->getSaturate(), ExSize, NULL, NULL, NULL,
                NULL, inst->getOption());
        }

        newInst->inheritDIFrom(inst);

        return newInst;
    }

    // HW WAs that are done before RA.
    void Optimizer::preRA_HWWorkaround()
    {
        // -forceNoMaskWA : to force running this WA pass on platform other than TGLLP.
        // noMaskWA:  only apply on TGLLP
        //   bit[1:0]:  0 - off
        //              1 - on, replacing nomask in any divergent BB (conservative)
        //              2 - on, replacing nomask in nested divergent BB (aggressive)
        //              3 - not used, will behave the same as 2
        //     bit[2]:  0 - optimized. "emask flag" is created once per each BB
        //              1 - simple insertion of "emask flag". A new flag is created
        //                  each time it is needed, that is, created per each inst.
        //  (See comments for more details at doNoMaskWA().
        if (kernel.getInt32KernelAttr(Attributes::ATTR_Target) != VISA_CM &&
            ((builder.getuint32Option(vISA_noMaskWA) & 0x3) > 0 ||
             builder.getOption(vISA_forceNoMaskWA)))
        {
            doNoMaskWA();
        }

        insertFenceAtEntry();

        cloneSampleInst();
    }


    // returns for this fence instruction the iterator position where the commit move should be inserted.
    // We conservatively assume a commit is needed before
    // -- another send
    // -- any optimization barrier
    // -- any instruction that writes to fence's dst GRF
    // If another instruction happens to read dst GRF, then it serves as the commit and we don't need the dummy move
    std::optional<INST_LIST_ITER> Optimizer::findFenceCommitPos(INST_LIST_ITER fence, G4_BB* bb) const
    {
        auto fenceInst = *fence;
        assert(fenceInst->isSend() && fenceInst->asSendInst()->isFence());
        auto dst = fenceInst->getDst();
        auto I = std::next(fence);
        for (auto E = bb->end(); I != E; ++I)
        {
            G4_INST* inst = *I;
            if (inst->isSend() || inst->isCFInst() || inst->isLabel() || inst->isOptBarrier())
            {
                break;
            }
            if (dst->hasOverlappingGRF(inst->getDst()))
            {
                break;
            }
            for (auto SI = inst->src_begin(), SE = inst->src_end(); SI != SE; ++SI)
            {
                auto src = *SI;
                if (dst->hasOverlappingGRF(src))
                {
                    return std::nullopt;
                }
            }
        }
        return I;
    }

    bool Optimizer::addFenceCommit(INST_LIST_ITER ii, G4_BB* bb, bool scheduleFenceCommit)
    {
        G4_INST* inst = *ii;
        G4_InstSend* sendInst = inst->asSendInst();
        assert(sendInst);
        if (sendInst && sendInst->getMsgDesc()->ResponseLength() > 0)
        {
            // commit is enabled for the fence, need to generate a move after to make sure the fence is complete
            // mov (8) r1.0<1>:ud r1.0<8;8,1>:ud {NoMask}
            auto nextIter = std::next(ii);
            if (scheduleFenceCommit)
            {
                auto iter = findFenceCommitPos(ii, bb);
                if (!iter)
                {
                    return false;   // skip commit for this fence
                }
                nextIter = *iter;
            }
            auto dst = inst->getDst();
            G4_Declare* fenceDcl = dst->getBase()->asRegVar()->getDeclare();
            G4_DstRegRegion* movDst = builder.createDst(
                builder.phyregpool.getNullReg(), 0, 0, 1, fenceDcl->getElemType());
            G4_SrcRegRegion* movSrc = builder.Create_Src_Opnd_From_Dcl(fenceDcl, builder.createRegionDesc(8, 8, 1));
            G4_INST* movInst = builder.createMov(g4::SIMD8, movDst, movSrc, InstOpt_WriteEnable, false);
            movInst->setComments("memory fence commit");
            bb->insertBefore(nextIter, movInst);
        }
        return true;
    }

    // some workaround for HW restrictions.  We apply them here so as not to affect optimizations, RA, and scheduling
    void Optimizer::HWWorkaround()
    {
        // Ensure the first instruction of a stack function has switch option.
        if (fg.getIsStackCallFunc() &&
            VISA_WA_CHECK(builder.getPWaTable(), WaThreadSwitchAfterCall) &&
            !builder.getOption(vISA_enablePreemption))
        {
            addSwitchOptionToBB(fg.getEntryBB(), true);
        }

        // set physical pred/succ as it's needed for the call WA
        fg.setPhysicalPredSucc();
        const bool scheduleFenceCommit = builder.getOption(vISA_scheduleFenceCommit) && builder.getPlatform() >= GENX_TGLLP;
        BB_LIST_ITER ib, bend(fg.end());
        for (ib = fg.begin(); ib != bend; ++ib)
        {
            G4_BB* bb = (*ib);
            INST_LIST_ITER ii = bb->begin();

            while (ii != bb->end())
            {
                G4_INST *inst = *ii;

                G4_InstSend* sendInst = inst->asSendInst();
                if (sendInst && sendInst->isFence() && !builder.getOption(vISA_skipFenceCommit))
                {
                    addFenceCommit(ii, bb, scheduleFenceCommit);
                }
                if (inst->isCall() || inst->isFCall())
                {
                    if (VISA_WA_CHECK(builder.getPWaTable(), WaThreadSwitchAfterCall))
                    {
                        // WA:
                        // A call instruction must be followed by an instruction that supports Switch.
                        // When call takes a jump, the first instruction must have a Switch.
                        BB_LIST_ITER nextBBIter = ib;
                        ++nextBBIter;
                        if (nextBBIter != bend)
                        {
                            addSwitchOptionToBB(*nextBBIter, false);
                        }
                        // also do this for call target
                        addSwitchOptionToBB(bb->Succs.front(), true);
                    }
                }

                // we must set {Switch} if the instruction updates ARF with no scoreboard
                {
                    G4_DstRegRegion* dst = inst->getDst();
                    if (dst != nullptr && dst->getBase()->noScoreBoard())
                    {
                        inst->setOptionOn(InstOpt_Switch);
                    }
                }

                if (inst->isSend() && !inst->isNoPreemptInst() && builder.needsNoPreemptR2ForSend())
                {
                    G4_Operand *Src0 = inst->getSrc(0);
                    if (Src0 && Src0->isGreg())
                    {
                        unsigned LB = Src0->getLinearizedStart();
                        if (LB == 2 * numEltPerGRF<Type_UB>())
                        {
                            inst->setOptionOn(InstOpt_NoPreempt);
                        }
                    }
                }

                if (builder.hasFdivPowWA() &&
                    inst->isMath() &&
                    (inst->asMathInst()->getMathCtrl() == MATH_FDIV || inst->asMathInst()->getMathCtrl() == MATH_POW))
                {
                    INST_LIST_ITER nextIter = ii;
                    nextIter++;
                    if (nextIter == bb->end())
                    {
                        break;
                    }
                    // check next inst
                    G4_INST *nextInst = *nextIter;
                    if (!nextInst->isSend() && nextInst->getDst() && !nextInst->hasNULLDst() && nextInst->getDst()->crossGRF())
                    {
                        // insert a nop
                        G4_INST *nopInst = builder.createNop(inst->getOption());
                        bb->insertBefore(nextIter, nopInst);
                    }
                }

                if (inst->isCall() || inst->isReturn())
                {
                    inst->setExecSize(kernel.getSimdSize());
                }

                // HW Workaround: for platforms without 64-bit regioning, change send src/dst type from QWord to DWord
                if (builder.no64bitRegioning() && inst->isSend())
                {
                    G4_DstRegRegion* dst = inst->getDst();
                    if (dst != nullptr && dst->getTypeSize() == 8)
                    {
                        dst->setType(Type_D);
                    }

                    G4_Operand *src0 = inst->getSrc(0);
                    if (src0 != nullptr && src0->getTypeSize() == 8)
                    {
                        src0->asSrcRegRegion()->setType(Type_D);
                    }

                    if (inst->isSplitSend())
                    {
                        G4_Operand *src1 = inst->getSrc(1);
                        if (src1 != nullptr && src1->getTypeSize() == 8)
                        {
                            src1->asSrcRegRegion()->setType(Type_D);
                        }
                    }
                }

                if (inst->isEOT() && VISA_WA_CHECK(builder.getPWaTable(), WaClearTDRRegBeforeEOTForNonPS))
                {
                    // insert
                    // mov(8) tdr0:uw 0x0:uw {NoMask}
                    G4_DstRegRegion* tdrDst = builder.createDst(builder.phyregpool.getTDRReg(),
                        0, 0, 1, Type_UW);
                    G4_Imm* src = builder.createImm(0, Type_UW);
                    G4_INST* movInst = builder.createMov(g4::SIMD8, tdrDst, src, InstOpt_WriteEnable | InstOpt_Switch, false);
                    bb->insertBefore(ii, movInst);
                }

                if (inst->isEOT() && VISA_WA_CHECK(builder.getPWaTable(), Wa_14010017096))
                {
                    // insert "(W) mov(16) acc0.0:f 0x0:f" before EOT
                    G4_INST* movInst = builder.createMov(g4::SIMD16,
                        builder.createDst(builder.phyregpool.getAcc0Reg(),0, 0, 1, Type_F),
                        builder.createImm(0, Type_F), InstOpt_WriteEnable, false);
                    // insert mov before contiguous send, in case that there are instruction combined set on continuous
                    // two send
                    INST_LIST_ITER insert_point = ii;
                    for (; insert_point != bb->begin(); --insert_point)
                        if (!(*insert_point)->isSend())
                            break;

                    if (!(*insert_point)->isEOT())
                        ++insert_point;
                    bb->insertBefore(insert_point, movInst);
                }

                if (VISA_WA_CHECK(builder.getPWaTable(), WaResetN0BeforeGatewayMessage) &&
                    inst->isSend() && inst->getMsgDesc()->isBarrierMsg())
                {
                    // mov (1) n0.0 0x0 {Switch}
                    G4_DstRegRegion* n0Dst = builder.createDst(
                        builder.phyregpool.getN0Reg(), 0, 0, 1, Type_UD);
                    auto movInst = builder.createMov(g4::SIMD1, n0Dst,
                        builder.createImm(0, Type_UD), InstOpt_WriteEnable | InstOpt_Switch, false);
                    bb->insertBefore(ii, movInst);
                }

                linePlaneWA(inst);
                fixSendSrcRegion(inst);
                ii++;
            }
        }

        if (VISA_WA_CHECK(builder.getPWaTable(), WaClearArfDependenciesBeforeEot))
        {
            clearARFDependencies();
        }
        if (VISA_WA_CHECK(builder.getPWaTable(), Wa_2201674230))
        {
            clearSendDependencies();
        }

        if (builder.needResetA0forVxHA0())
        {
            // reset a0 to 0 at the beginning of a shader.
            // The goal of this initialization is to make sure that there is no
            // garbage values in the address register for inactive simd lanes.
            // With indirect addressing HW requires that there is no
            // out-of-bounds access even on inactive simd lanes.


            // Note: this initialization doesn't cover scenarios where the
            // address register is used in a send descriptor and later used in
            // indirect addressing.
            resetA0();
        }

        if (builder.getOption(vISA_setA0toTdrForSendc))
        {
            // set A0 to tdr0 before sendc/sendsc. TGL WA
            setA0toTdrForSendc();
        }

        if (builder.needReplaceIndirectCallWithJmpi() &&
            kernel.getBoolKernelAttr(Attributes::ATTR_Extern))
        {
            // jmpi WA can't properly work on platforms with SWSB. We didn't re-caculate the
            // jump offset after swsb insertion.
            assert(!builder.hasSWSB());
            // replace ret in the external functions with jmpi. That we will
            // also replace the call with jmpi in Optimizer::expandIndirectCallWithRegTarget
            replaceRetWithJmpi();
        }

        if (!builder.supportCallaRegSrc() && kernel.hasIndirectCall())
        {
            // If the indirect call has regiser src0, the register must be a
            // ip-based address of the call target. Insert instructions before call to
            // calculate the relative offset from call to the target
            expandIndirectCallWithRegTarget();
        }
    }

    //
    // rewrite source regions to satisfy various HW requirements.  This pass will not modify the instrppuctions otherwise
    // -- rewrite <1;1,0> to <2;2,1> when possible (exec size > 1, width is not used to cross GRF)
    //    as HW doesn't allow <1;1,0> to be co-issued
    //
    void Optimizer::normalizeRegion()
    {
        for (auto bb : fg)
        {
            for (auto inst : *bb)
            {
                if (inst->isCall() ||
                    inst->isFCall() ||
                    inst->isReturn() ||
                    inst->isFReturn())
                {
                    // Do not rewrite region for call or return,
                    // as the effective execution size is 2.
                    continue;
                }

                if (inst->getExecSize() == g4::SIMD1)
                {
                    // Replace: mov (1) r64.0<4>:df  r3.0<0;1,0>:df
                    // with:    mov (1) r64.0<1>:df  r3.0<0;1,0>:df
                    // otherwise, will get incorrect results for HSW, HW mode
                    G4_Operand* dst = inst->getDst();
                    if (dst != NULL &&
                        dst->asDstRegRegion()->getHorzStride() > 1 &&
                        dst->getTypeSize() == 8)
                    {
                        dst->asDstRegRegion()->setHorzStride(1);
                    }
                }
                else
                {
                    for (int i = 0; i < inst->getNumSrc(); ++i)
                    {
                        G4_Operand* src = inst->getSrc(i);
                        // Only rewrite direct regions.
                        if (src && src->isSrcRegRegion() && src->asSrcRegRegion()->getRegAccess() == Direct)
                        {
                            G4_SrcRegRegion* srcRegion = src->asSrcRegRegion();
                            if (srcRegion->getRegion()->isContiguous(inst->getExecSize()))
                            {
                                srcRegion->rewriteContiguousRegion(builder, i);
                            }
                            else if (inst->isAlign1Ternary())
                            {
                                // special checks for 3src inst with single non-unit stride region
                                // rewrite it as <s*2;s>
                                uint16_t stride = 0;
                                if (srcRegion->getRegion()->isSingleNonUnitStride(inst->getExecSize(), stride))
                                {
                                    MUST_BE_TRUE(stride <= 4, "illegal stride for align1 ternary region");
                                    srcRegion->setRegion(kernel.fg.builder->createRegionDesc(stride * 2, 2, stride));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void Optimizer::countGRFUsage()
    {
        unsigned int maxGRFNum = kernel.getNumRegTotal();
        int count = 0;
        bool *GRFUse = (bool *) builder.mem.alloc(sizeof(bool) * maxGRFNum);
        for (unsigned int i = 0; i < maxGRFNum; ++i)
        {
            GRFUse[i] = false;
        }
        for (auto dcl : kernel.Declares)
        {
            if (dcl->getRegVar()->isGreg())
            {
                int GRFStart = dcl->getRegVar()->getPhyReg()->asGreg()->getRegNum();
                int numRows = dcl->getNumRows();
                MUST_BE_TRUE(GRFStart >= 0 && (GRFStart + numRows) <= (int)maxGRFNum,
                    "illegal GRF assignment");
                for (int i = GRFStart; i < GRFStart + numRows; ++i)
                {
                    GRFUse[i] = true;
                }
            }
        }

        for (unsigned int i = 0; i < maxGRFNum; ++i)
        {
            if (GRFUse[i])
            {
                count++;
            }
        }
        fg.builder->getJitInfo()->numGRFUsed = count;
        fg.builder->criticalMsgStream() << "\tKernel " << kernel.getName() << " : " <<
            count << " registers\n";
    }

    //
    //  Dump the input payload to start of scratch space.
    //  this is strictly for debugging and we do not care if this gets overwritten by
    //  other usage of the scratch space (private memory, spill, etc.)
    //
    void Optimizer::dumpPayload()
    {
        int inputEnd = 0;
        for (int i = 0, numInput = kernel.fg.builder->getInputCount(); i < numInput; ++i)
        {
            input_info_t* input_info = kernel.fg.builder->getInputArg(i);
            if (inputEnd < input_info->size + input_info->offset)
            {
                inputEnd = input_info->size + input_info->offset;
            }
        }

        G4_BB* bb = kernel.fg.getEntryBB();
        // iter points to the first non-label inst
        auto iter = bb->begin(), bbEnd = bb->end();
        while (iter != bbEnd)
        {
            if (!(*iter)->isLabel())
            {
                break;
            }
            ++iter;
        }

        int regOffset = (inputEnd + numEltPerGRF<Type_UB>() - 1) / numEltPerGRF<Type_UB>();

        static const unsigned SCRATCH_MSG_DESC_CATEGORY = 18;
        static const unsigned SCRATCH_MSG_DESC_OPERATION_MODE = 17;
        static const unsigned SCRATCH_MSG_DESC_CHANNEL_MODE = 16;
        static const unsigned SCRATCH_MSG_DESC_BLOCK_SIZE = 12;

        // write 8 GRFs at a time
        int msgSize = 8;
        for (int i = 1; i < regOffset; i += msgSize)
        {
            uint16_t extFuncCtrl = 0;
            // both scratch and block read use DC
            SFID funcID = SFID::DP_DC;

            uint32_t headerPresent = 0x80000;
            uint32_t msgDescImm = headerPresent;
            uint32_t msgLength = 1;
            uint32_t blocksizeEncoding = 0x3; // 8 GRF
            msgDescImm |= (msgLength << getSendMsgLengthBitOffset());
            msgDescImm |= (1 << SCRATCH_MSG_DESC_CATEGORY);
            msgDescImm |= (1 << SCRATCH_MSG_DESC_CHANNEL_MODE);
            msgDescImm |= (1 << SCRATCH_MSG_DESC_OPERATION_MODE);

            msgDescImm |= (blocksizeEncoding << SCRATCH_MSG_DESC_BLOCK_SIZE);
            msgDescImm |= i;

            G4_SendMsgDescriptor* desc = kernel.fg.builder->createSendMsgDesc(
                msgDescImm, 0, 1, funcID, msgSize, extFuncCtrl, SendAccess::WRITE_ONLY);
            const RegionDesc* region = kernel.fg.builder->getRegionStride1();
            G4_SrcRegRegion* headerOpnd = kernel.fg.builder->Create_Src_Opnd_From_Dcl(kernel.fg.builder->getBuiltinR0(), region);
            G4_Declare* tempDcl = builder.createHardwiredDeclare(msgSize * 8, Type_UD, i, 0);
            G4_SrcRegRegion* srcOpnd = kernel.fg.builder->Create_Src_Opnd_From_Dcl(tempDcl, region);
            G4_DstRegRegion* dstOpnd = kernel.fg.builder->createNullDst(Type_UD);

            G4_INST* sendInst = kernel.fg.builder->createSplitSendInst(
                nullptr, G4_sends, g4::SIMD16, dstOpnd, headerOpnd, srcOpnd,
                kernel.fg.builder->createImm(msgDescImm, Type_UD), InstOpt_WriteEnable, desc, nullptr, true);
            bb->insertBefore(iter, sendInst);
        }
    }

    // perform simple stat collection (e.g., numBarriers, numSends)
    // IR is not modified
    void Optimizer::collectStats()
    {
        builder.getJitInfo()->usesBarrier = false;
        uint32_t numSends = 0;
        for (auto bb : fg)
        {
            for (auto inst : *bb)
            {
                if (inst->isSend())
                {
                    numSends++;
                    if (inst->asSendInst()->getMsgDesc()->isBarrierMsg())
                    {
                        // Propagate information about barriers presence back to IGC. It's safer to
                        // depend on vISA statistics as IGC is not able to detect barriers if they are
                        // used as a part of Inline vISA code.
                        // This information is used by legacy CMRT as well as OpenCL/L0 runtime.
                        builder.getJitInfo()->usesBarrier = true;
                    }
                }
            }
        }
        builder.getcompilerStats().SetI64(CompilerStats::numSendStr(), numSends, builder.kernel.getSimdSize());
    }

    // Create a copy of R0 at top of kernel,
    // to support midthread preemption.
    void Optimizer::createR0Copy()
    {
        if (!builder.getIsKernel())
        {
            return;
        }

        // Skip copying of ``copy of R0'' if it's never assigned, a case where
        // ``copy of R0'' is never used. As EOT always use ``copy of R0'', that
        // case only happens for synthetic tests where no practical code is
        // generated.
        if (!builder.getBuiltinR0()->getRegVar()->isPhyRegAssigned() )
            return;

        G4_Declare *R0Dcl = builder.getRealR0();
        G4_SrcRegRegion* R0Opnd = builder.Create_Src_Opnd_From_Dcl(R0Dcl, builder.getRegionStride1());

        G4_DstRegRegion* R0CopyOpnd = builder.createDst(
            builder.getBuiltinR0()->getRegVar(),
            0,
            0,
            1,
            Type_UD);
        R0CopyOpnd->computePReg();

        unsigned int options = InstOpt_WriteEnable;
        G4_INST *movInst =
            builder.createMov(g4::SIMD8, R0CopyOpnd, R0Opnd, options, false);

        BB_LIST_ITER ib = kernel.fg.begin();
        G4_INST *inst = NULL;
        BB_LIST_ITER bend(kernel.fg.end());
        INST_LIST_ITER ii;

        for (ib = kernel.fg.begin(); ib != bend; ++ib)
        {
            G4_BB* bb = (*ib);
            ii = bb->begin();
            INST_LIST_ITER iend = bb->end();
            for (; ii != iend; ii++)
            {
                inst = *ii;
                if (inst->opcode() != G4_label)
                {
                    bb->insertBefore(ii, movInst);
                    return;
                }
            }
        }
    }

    void Optimizer::initializePayload()
    {
        if (!kernel.fg.builder->getIsKernel())
        {
            return;
        }

        unsigned int inputEnd = 32;
        unsigned int inputCount = kernel.fg.builder->getInputCount();
        for (unsigned int id = 0; id < inputCount; id++)
        {
            input_info_t* input_info = kernel.fg.builder->getInputArg(id);
            if (inputEnd < (unsigned)(input_info->size + input_info->offset))
            {
                inputEnd = input_info->size + input_info->offset;
            }
        }

        int reg_offset = (inputEnd + 31) / 32;
        //bytes whithin grf that need to be initialized
        int remainder_bytes = 32 - (inputEnd % 32);
        //number of full grfs that need to be initialized
        int num_reg_init = 128 - reg_offset;
        //beginning execution size for byte remainder initialization
        uint16_t exec_size = 16;
        //offset within GRF from which to start to initialize
        int sub_offset = (inputEnd % 32);
        G4_BB* bb = kernel.fg.getEntryBB();

        // iter points to the first non-label inst
        auto iter = bb->begin(), bbEnd = bb->end();
        while (iter != bbEnd)
        {
            if (!(*iter)->isLabel())
            {
                break;
            }
            ++iter;
        }

        //inits builk of GRFS, two at a time
        for (int i = 0; i < num_reg_init - (num_reg_init % 2); i += 2)
        {
            int reg_init = reg_offset + i;
            G4_Declare* tempDcl = builder.createHardwiredDeclare(16, Type_UD, reg_init, 0);
            G4_DstRegRegion* dst = builder.createDst(tempDcl->getRegVar(), 0, 0, 1, Type_UD);
            G4_Imm * src0 = builder.createImm(0, Type_UD);
            G4_INST* initInst = builder.createMov(g4::SIMD16, dst, src0, InstOpt_WriteEnable, false);
            bb->insertBefore(iter, initInst);
        }

        //init last register if bulk of GRFs was odd
        if ((num_reg_init % 2))
        {
            G4_Declare* tempDcl = builder.createHardwiredDeclare(8, Type_UD, 128 - (num_reg_init % 2), 0);
            G4_DstRegRegion* dst = builder.createDst(tempDcl->getRegVar(), 0, 0, 1, Type_UD);
            G4_Imm * src0 = builder.createImm(0, Type_UD);
            G4_INST* spIncInst = builder.createMov(g4::SIMD8, dst, src0, InstOpt_WriteEnable, false);
            G4_BB* bb = kernel.fg.getEntryBB();
            bb->insertBefore(iter, spIncInst);
        }

        //inits remainder GRF
        //loops untill all bytes within GRF are initialized
        //on each iteration goes down by execution size
        //There was a small bug if inputEnd offset is GRF aligned it would think all of
        //last payload register is the "remainder" and will initialize it.
        while (remainder_bytes && (inputEnd % 32))
        {
            for (int i = 0; i < remainder_bytes - (remainder_bytes % exec_size); i += exec_size, sub_offset += exec_size)
            {
                G4_Declare* tempDcl = builder.createHardwiredDeclare(exec_size, Type_UB, reg_offset - 1, sub_offset);
                G4_DstRegRegion* dst = builder.createDst(tempDcl->getRegVar(), 0, 0, 1, Type_UB);

                G4_Declare* tempDclSrc = builder.createHardwiredDeclare(1, Type_UD, 126, 0);
                G4_SrcRegRegion *src0 = builder.createSrc(tempDclSrc->getRegVar(), 0, 0, builder.getRegionScalar(), Type_UB);

                G4_INST* initInst = builder.createMov((G4_ExecSize)exec_size, dst, src0, InstOpt_WriteEnable, false);
                bb->insertBefore(iter, initInst);
            }
            //caluclates bytes that remain to be initialized
            remainder_bytes = remainder_bytes % exec_size;
            //next lowest execution size
            exec_size = (uint16_t)std::max(1, exec_size / 2);
        }

        //Initializaing Flag register
        int num32BitFlags = builder.getNumFlagRegisters() / 2;
        for (int i = 0; i < num32BitFlags; ++i)
        {
            G4_Declare* tmpFlagDcl = builder.createTempFlag(2);
            tmpFlagDcl->getRegVar()->setPhyReg(builder.phyregpool.getFlagAreg(i), 0);
            G4_DstRegRegion *tempPredVar = builder.createDst(tmpFlagDcl->getRegVar(), 0, 0, 1, Type_UD);
            G4_INST *predInst = builder.createMov(g4::SIMD1, tempPredVar, builder.createImm(0, Type_UW), InstOpt_WriteEnable, false);
            bb = kernel.fg.getEntryBB();
            bb->insertBefore(iter, predInst);
        }
    }

    // create prolog to set sr0 to FFID. TGL WA.
    // Do only when there is cr0 write inside the kernel
    void Optimizer::addFFIDProlog()
    {
        if (!builder.getIsKernel())
            return;

        FFID ffid = static_cast<FFID>(builder.getOptions()->getuInt32Option(vISA_setFFID));
        // return if FFID is not given
        if (ffid == FFID_INVALID)
            return;

        // get r127.0 decl
        G4_Declare* rtail =
            builder.createHardwiredDeclare(8, Type_UD, kernel.getNumRegTotal() - 1, 0);

        // (W) and (1|M0)  r127.0 <1>:ud   sr0.0 <0;1,0>:ud  0xF0FFFFFF:ud
        auto createAnd = [this, &rtail]()
        {
            auto src0 = builder.createSrc(builder.phyregpool.getSr0Reg(), 0, 0,
                builder.getRegionScalar(), Type_UD);
            auto src1 = builder.createImm(0xF0FFFFFF, Type_UD);
            auto dst = builder.createDst(rtail->getRegVar(), 0, 0, 1, Type_UD);

            return builder.createBinOp(G4_and, g4::SIMD1,
                dst, src0, src1, InstOpt_WriteEnable, false);
        };

        // (W) or  (1|M0)  sr0.0<1>:ud   127.0<0;1,0>:ud    imm:ud
        auto createOr = [this, &rtail](uint32_t imm)
        {
            auto src0 = builder.createSrc(rtail->getRegVar(), 0, 0,
                builder.getRegionScalar(), Type_UD);
            auto src1 = builder.createImm(imm, Type_UD);
            auto dst = builder.createDst(
                builder.phyregpool.getSr0Reg(), 0, 0, 1, Type_UD);

            return builder.createBinOp(G4_or, g4::SIMD1,
                dst, src0, src1, InstOpt_WriteEnable, false);
        };

        // (W) jmpi (1|M0) label
        auto createJmpi = [this](G4_Label* label)
        {
            return builder.createInternalInst(
                nullptr, G4_jmpi, nullptr, g4::NOSAT, g4::SIMD1,
                nullptr, label, nullptr, InstOpt_WriteEnable);
        };

        auto createLabelInst = [this](G4_Label* label)
        {
            return kernel.fg.createNewLabelInst(label);
        };

        // for compute shader, create two entris
        if (ffid == FFID_GP || ffid == FFID_GP1)
        {
            // Entry0: Set sr0 to FFID_GP (0x7)
            //     (W) and (1|M0)  r127.0 <1>:ud   sr0.0 <0;1,0>:ud   0xF0FFFFFF:ud
            //     (W) or  (1|M0)  sr0.0<1>:ud     127.0<0;1,0>:ud    0x07000000:ud
            //     jmpi ffid_prolog_end
            // Entry1: Set sr0 to FFID_GP1 (0x8)
            //     (W) and (1|M0)  r127.0 <1>:ud   sr0.0 <0;1,0>:ud   0xF0FFFFFF:ud
            //     (W) or  (1|M0)  sr0.0<1>:ud     127.0<0;1,0>:ud    0x08000000:ud
            //     ffid_prolog_end:

            // Put the entry0 block into a new BB, so that we can make it 64-bit
            // aligned in BinaryEncodingIGA
            G4_BB* entry_0_bb = kernel.fg.createNewBB();
            entry_0_bb->push_back(createAnd());
            entry_0_bb->push_back(createOr(0x07000000));

            // get jmp target label. If the next bb has no label, create one and insert it
            // at the beginning
            G4_Label* jmp_label = nullptr;
            assert(kernel.fg.begin() != kernel.fg.end());
            G4_BB* next_bb = *kernel.fg.begin();
            if (next_bb->front()->isLabel())
            {
                jmp_label = next_bb->front()->getSrc(0)->asLabel();
            }
            else
            {
                std::string label_name("ffid_prolog_end");
                jmp_label = builder.createLabel(label_name, LABEL_BLOCK);
                next_bb->insertBefore(next_bb->begin(), createLabelInst(jmp_label));
            }
            entry_0_bb->push_back(createJmpi(jmp_label));

            // Put the rest in another BB
            G4_BB* entry_1_bb = kernel.fg.createNewBB();
            entry_1_bb->push_back(createAnd());
            entry_1_bb->push_back(createOr(0x08000000));

            // add these two BB to be the first two in the shader
            kernel.fg.addPrologBB(entry_1_bb);
            kernel.fg.addPrologBB(entry_0_bb);
            builder.setHasComputeFFIDProlog();
        }
        else
        {
            // for other shaders, set the FFID
            //     (W) and (1|M0)  r127.0 <1>:ud   sr0.0 <0;1,0>:ud   0xF0FFFFFF:ud
            //     (W) or  (1|M0)  sr0.0<1>:ud     127.0<0;1,0>:ud    (FFID << 24):ud
            G4_BB* bb = kernel.fg.createNewBB();
            bb->push_back(createAnd());
            bb->push_back(createOr(ffid << 24));
            kernel.fg.addPrologBB(bb);
        }
    }

    void Optimizer::loadThreadPayload()
    {
    }

    // some platform/shaders require a memory fence before the end of thread
    // ToDo: add fence only when the writes can reach EOT without a fence in between
    void Optimizer::insertFenceBeforeEOT()
    {
        if (!builder.getOption(vISA_clearHDCWritesBeforeEOT))
        {
            return;
        }

        if (!kernel.fg.builder->getIsKernel())
        {
            // we dont allow a function to exit
            return;
        }

        bool hasUAVWrites = false;
        bool hasSLMWrites = false;
        bool hasTypedWrites = false;

        for (auto bb : kernel.fg)
        {
            if (bb->isEndWithFCall())
            {
                // conservatively assume we need a fence
                // ToDo: we don't need a SLM fence if kernel doesnt use SLM, since function can't allocate SLM on its own
                // We can move this W/A to IGC for more precise analysis
                hasUAVWrites = true;
                hasSLMWrites = true;
                hasTypedWrites = true;
                break;
            }

            for (auto inst : *bb)
            {
                if (inst->isSend())
                {
                    auto msgDesc = inst->asSendInst()->getMsgDesc();
                    if (msgDesc->isDataPortWrite())
                    {
                        if (msgDesc->isHDC())
                        {
                            if (msgDesc->isSLMMessage())
                            {
                                hasSLMWrites = true;
                            }
                            else if (msgDesc->isHdcTypedSurfaceWrite())
                            {
                                hasTypedWrites = true;
                            }
                            else
                            {
                                hasUAVWrites = true;
                            }
                        }

                    }
                }
            }
        }

        if (!hasUAVWrites && !hasSLMWrites && !hasTypedWrites)
        {
            return;
        }

        for (auto bb : kernel.fg)
        {
            if (bb->isLastInstEOT())
            {
                auto iter = std::prev(bb->end());

                {
                    if (builder.getPlatform() == GENX_ICLLP)
                    {
                        hasTypedWrites = false; // Workaround Under debug and being clarified
                        hasSLMWrites = false;   // Workaround not needed for ICL SLM Writes
                    }
                    if (hasUAVWrites || hasTypedWrites)
                    {
                        auto fenceInst = builder.createFenceInstruction(0, true, true, false);
                        bb->insertBefore(iter, fenceInst);
                    }
                    if (hasSLMWrites)
                    {
                        auto fenceInst = builder.createFenceInstruction(0, true, false, false);
                        bb->insertBefore(iter, fenceInst);
                    }
                }
                builder.instList.clear();
            }
        }
    }

    // some platform/shaders require a memory fence at kernel entry
    // this needs to be called before RA since fence may have a (dummy) destination.
    void Optimizer::insertFenceAtEntry()
    {
    }


    void Optimizer::mapOrphans()
    {
        auto catchAllCISAOff = builder.debugInfoPlaceholder;
        if (catchAllCISAOff == UNMAPPABLE_VISA_INDEX)
            return;

        for (auto bb : kernel.fg)
        {
            for (auto inst : *bb)
            {
                if (inst->getCISAOff() == UNMAPPABLE_VISA_INDEX)
                {
                    inst->setCISAOff(catchAllCISAOff);
                }
            }
        }
    }

    void Optimizer::varSplit()
    {
        VarSplitPass* splitPass = kernel.getVarSplitPass();
        // Run explicit variable split pass
        if (kernel.getOption(vISA_IntrinsicSplit))
        {
            splitPass->run();
        }
    }

    // some platforms require extra instruction before an EOT to
    // ensure that all outstanding scratch writes are globally observed
    void Optimizer::insertScratchReadBeforeEOT()
    {
        int globalScratchOffset = kernel.getInt32KernelAttr(Attributes::ATTR_SpillMemOffset);
        if (builder.needFenceBeforeEOT() ||
            (globalScratchOffset == 0 && builder.getJitInfo()->spillMemUsed == 0))
        {
            return;
        }

        struct ScratchReadDesc
        {
            uint32_t addrOffset : 12;
            uint32_t dataElements : 2;
            uint32_t reserved : 3;
            uint32_t opType : 2;
            uint32_t header : 1;
            uint32_t resLen : 5;
            uint32_t msgLen : 4;
            uint32_t reserved2: 3;
        };

        union {
            uint32_t value;
            ScratchReadDesc layout;
        } desc;

        // msg desc for 1GRF scratch block read
        desc.value = 0;
        desc.layout.opType = 2;
        desc.layout.header = 1;
        desc.layout.resLen = 1;
        desc.layout.msgLen = 1;

        for (auto bb : kernel.fg)
        {
            if (bb->isLastInstEOT())
            {
                auto iter = std::prev(bb->end());
                if (getPlatformGeneration(builder.getPlatform()) >= PlatformGen::GEN10)
                {
                    // an HDC fence is more efficient in this case
                    // fence with commit enable
                    int fenceDesc = G4_SendMsgDescriptor::createDesc((0x7 << 14) | (1 << 13), true, 1, 1);
                    auto msgDesc = builder.createSyncMsgDesc(SFID::DP_DC, fenceDesc);
                    auto src = builder.Create_Src_Opnd_From_Dcl(builder.getBuiltinR0(), builder.getRegionStride1());
                    auto dst = builder.Create_Dst_Opnd_From_Dcl(builder.getBuiltinR0(), 1);
                    G4_INST* inst = builder.createSendInst(
                        nullptr, G4_send, g4::SIMD8, dst, src,
                        builder.createImm(fenceDesc, Type_UD), InstOpt_WriteEnable, msgDesc, true);
                    bb->insertBefore(iter, inst);
                }
                else
                {
                    // insert a dumy scratch read
                    auto msgDesc = builder.createReadMsgDesc(SFID::DP_DC, desc.value);
                    auto src = builder.Create_Src_Opnd_From_Dcl(builder.getBuiltinR0(), builder.getRegionStride1());
                    // We can use any dst that does not conflcit with EOT src, which must be between r112-r127
                    auto dstDcl = builder.createHardwiredDeclare(8, Type_UD, 1, 0);
                    auto dst = builder.Create_Dst_Opnd_From_Dcl(dstDcl, 1);
                    G4_INST* sendInst = builder.createSendInst(
                        nullptr, G4_send, g4::SIMD8, dst, src,
                        builder.createImm(desc.value, Type_UD), InstOpt_WriteEnable, msgDesc, true);
                    bb->insertBefore(iter, sendInst);
                }

                builder.instList.clear();
            }
        }
    }

    // Reset A0 to 0 at the beginning of the shader if the shader use VxH a0
    void Optimizer::resetA0()
    {
        // check all instructions to see if VxH a0 src is used
        // only reset A0 when it's used
        bool hasA0 = false;
        for (auto bb : kernel.fg)
        {
            for (auto inst : *bb)
            {
                // VxH must be in src0
                if (inst->getSrc(0) != nullptr &&
                    inst->getSrc(0)->isSrcRegRegion() &&
                    inst->getSrc(0)->asSrcRegRegion()->getBase()->isA0() &&
                    inst->getSrc(0)->asSrcRegRegion()->getRegion()->isRegionWH())
                {
                    hasA0 = true;
                    break;
                }
            }
            if (hasA0)
                break;
        }

        if (!hasA0)
            return;

        // insert "mov (16) a0.0:uw 0x0:uw" at the beginning of the shader
        if (kernel.fg.begin() != kernel.fg.end()) {
            G4_BB* bb = *kernel.fg.begin();
            auto insertIt = std::find_if(bb->begin(), bb->end(),
                [](G4_INST* inst) { return !inst->isLabel(); });
            bb->insertBefore(insertIt,
                builder.createMov(g4::SIMD16,
                    builder.createDst(builder.phyregpool.getAddrReg(), 0, 0, 1, Type_UW),
                    builder.createImm(0, Type_UW), InstOpt_WriteEnable, false));
        }
    }

    G4_Declare* Optimizer::createInstsForCallTargetOffset(
        InstListType& insts, G4_INST* fcall, int64_t adjust_off)
    {
        // create instruction sequence:
        //       add  r2.0  -IP   call_target
        //       add  r2.0  r2.0  adjust_off

        // call's dst must be r125.0, which is reserved at
        // GlobalRA::setABIForStackCallFunctionCalls.
        assert(fcall->getDst()->isGreg());
        // call dst must not be overlapped with r2 which is hardcoded as the new jump target
        assert((fcall->getDst()->getLinearizedStart() / numEltPerGRF<Type_UB>()) != 2);


        // hardcoded add's dst to r2
        // the reg offset must be the same as call's dst reg, and must be 0 (HW restriction)
        uint32_t reg_off = fcall->getDst()->getLinearizedStart() % numEltPerGRF<Type_UB>()
            / fcall->getDst()->getTypeSize();

        G4_Declare* add_dst_decl =
            builder.createHardwiredDeclare(1, fcall->getDst()->getType(), 2, reg_off);

        // create the first add instruction
        // add  r2.0  -IP   call_target
        G4_INST* add_inst = builder.createBinOp(
            G4_add, g4::SIMD1,
            builder.Create_Dst_Opnd_From_Dcl(add_dst_decl, 1),
            builder.createSrcRegRegion(
                Mod_Minus, Direct, builder.phyregpool.getIpReg(), 0, 0,
                builder.getRegionScalar(), Type_UD),
            fcall->getSrc(0), InstOpt_WriteEnable | InstOpt_NoCompact, false);

        // create the second add to add the -ip to adjust_off, adjust_off dependes
        // on how many instructions from the fist add to the jmp instruction, and
        // if it's post-increment (jmpi) or pre-increment (call)
        // add  r2.0  r2.0  adjust_off
        G4_INST* add_inst2 = builder.createBinOp(
            G4_add, g4::SIMD1,
            builder.Create_Dst_Opnd_From_Dcl(add_dst_decl, 1),
            builder.Create_Src_Opnd_From_Dcl(
                add_dst_decl, builder.getRegionScalar()),
            builder.createImm(adjust_off, Type_D),
            InstOpt_WriteEnable | InstOpt_NoCompact, false);

        insts.push_back(add_inst);
        insts.push_back(add_inst2);

        return add_dst_decl;
    }

    void Optimizer::createInstForJmpiSequence(InstListType& insts, G4_INST* fcall)
    {
        // SKL workaround for indirect call
        // r125.0 is the return IP (the instruction right after jmpi)
        // r125.1 is the return mask. While we'll replace the ret in calee to jmpi as well,
        // we do not need to consider the return mask here.

        // Do not allow predicate call on jmpi WA
        assert(fcall->getPredicate() == nullptr);

        // calculate the reserved register's num and offset from fcall's dst register (shoud be r125.0)
        assert(fcall->getDst()->isGreg());
        uint32_t reg_num = fcall->getDst()->getLinearizedStart() / numEltPerGRF<Type_UB>();
        uint32_t reg_off = fcall->getDst()->getLinearizedStart() % numEltPerGRF<Type_UB>()
            / fcall->getDst()->getTypeSize();

        G4_Declare* new_target_decl = createInstsForCallTargetOffset(insts, fcall, -64);

        // add  r125.0   IP   32
        G4_Declare* ret_decl =
            builder.createHardwiredDeclare(1, fcall->getDst()->getType(), reg_num, reg_off);
        insts.push_back(builder.createBinOp(
            G4_add, g4::SIMD1,
            builder.Create_Dst_Opnd_From_Dcl(ret_decl, 1),
            builder.createSrc(builder.phyregpool.getIpReg(), 0, 0,
                builder.getRegionScalar(), Type_UD),
            builder.createImm(32, Type_UD),
            InstOpt_WriteEnable | InstOpt_NoCompact, false));

        // jmpi r2.0
        // update jump target (src0) to add's dst
        G4_SrcRegRegion* jump_target = builder.Create_Src_Opnd_From_Dcl(
            new_target_decl, builder.getRegionScalar());
        jump_target->setType(Type_D);
        insts.push_back(builder.createJmp(nullptr, jump_target, InstOpt_NoCompact, false));
    }

    void Optimizer::expandIndirectCallWithRegTarget()
    {
        // check every fcall
        for (auto bb : kernel.fg)
        {
            if (bb->back()->isFCall())
            {
                G4_INST* fcall = bb->back();
                if (fcall->getSrc(0)->isGreg() || fcall->getSrc(0)->isA0()) {
                    // at this point the call instruction's src0 has the target_address
                    // and the call dst is the reserved register (r125.0) for ret
                    // All the caller save register should be saved. We usd r2 directly
                    // here to calculate the new call's target.
                    //
                    // expand call
                    // From:
                    //       call r125.0 call_target
                    // To:
                    //       add  r2.0  -IP   call_target
                    //       add  r2.0  r2.0  -32
                    //       call r125.0  r2.0

                    // For SKL workaround, expand call
                    // From:
                    //       call r125.0 call_target
                    // To:
                    //       add  r2.0     -IP    call_target
                    //       add  r2.0     r2.0   -64
                    //       add  r125.0   IP     32          // set the return IP
                    //       jmpi r2.0

                    InstListType expanded_insts;
                    if (builder.needReplaceIndirectCallWithJmpi()) {
                        createInstForJmpiSequence(expanded_insts, fcall);
                    }
                    else {
                        G4_Declare* jmp_target_decl =
                            createInstsForCallTargetOffset(expanded_insts, fcall, -32);
                        // Updated call's target to the new target
                        G4_SrcRegRegion* jump_target = builder.Create_Src_Opnd_From_Dcl(
                            jmp_target_decl, builder.getRegionScalar());
                        fcall->setSrc(jump_target, 0);
                        fcall->setNoCompacted();
                    }
                    // then insert the expaneded instructions right before the call
                    INST_LIST_ITER insert_point = bb->end();
                    --insert_point;
                    for (auto inst_to_add : expanded_insts) {
                        bb->insertBefore(insert_point, inst_to_add);
                    }

                    // remove call from the instlist for Jmpi WA
                    if (builder.needReplaceIndirectCallWithJmpi())
                        bb->erase(--bb->end());
                }
            }
        }

    }

    // Replace ret with jmpi, must be single return
    void Optimizer::replaceRetWithJmpi()
    {
        size_t num_ret = 0;

        for (G4_BB* bb : kernel.fg)
        {
            if (bb->empty())
                continue;
            if (bb->isEndWithFRet())
            {
                ++num_ret;
                G4_INST* ret_inst = bb->back();

                // ret dst's decl
                G4_Declare* ret_reg = ret_inst->getSrc(0)->getTopDcl();

                // calculate the jmpi target offset
                // expand the original ret from:
                //     ret r125.0
                // To:
                //     add   r125.0  -ip   r125.0
                //     add   r125.0  r125.0  -48
                //     jmpi  r125.0

                // add   r125.0  -ip   r125.0
                G4_INST* add0 = builder.createBinOp(
                    G4_add, g4::SIMD1,
                    builder.Create_Dst_Opnd_From_Dcl(ret_reg, 1),
                    builder.createSrcRegRegion(
                        Mod_Minus, Direct, builder.phyregpool.getIpReg(), 0, 0,
                        builder.getRegionScalar(), Type_UD),
                    builder.Create_Src_Opnd_From_Dcl(ret_reg, builder.getRegionScalar()),
                    InstOpt_WriteEnable | InstOpt_NoCompact, false);

                // add   r125.0  r125.0  -48
                G4_INST* add1 = builder.createBinOp(
                    G4_add, g4::SIMD1,
                    builder.Create_Dst_Opnd_From_Dcl(ret_reg, 1),
                    builder.Create_Src_Opnd_From_Dcl(ret_reg, builder.getRegionScalar()),
                    builder.createImm(-48, Type_D), InstOpt_WriteEnable | InstOpt_NoCompact, false);

                // jmpi r125.0
                G4_SrcRegRegion* jmpi_target = builder.Create_Src_Opnd_From_Dcl(
                    ret_reg, builder.getRegionScalar());
                jmpi_target->setType(Type_D);
                G4_INST* jmpi = builder.createJmp(nullptr, jmpi_target, InstOpt_NoCompact, false);

                // remove the ret
                bb->pop_back();
                // add the jmpi
                bb->push_back(add0);
                bb->push_back(add1);
                bb->push_back(jmpi);
            }
        }

        // there should be exactly one ret in a external function. We did not try
        // to restore the CallMask. We rely on single return of a function to make
        // sure the CallMask before and after calling this function is the same.
        assert(num_ret == 1);
    }

    // Set a0 to tdr0 before snedc/sendsc
    void Optimizer::setA0toTdrForSendc()
    {
        // check for the last inst of each BB, if it's sendc/sendsc, insert
        // "(W) mov(8) a0.0:uw tdr0.0:uw" right before it
        for (G4_BB* bb : kernel.fg)
        {
            if (bb->empty())
                continue;
            if (bb->back()->opcode() == G4_opcode::G4_sendc ||
                bb->back()->opcode() == G4_opcode::G4_sendsc)
            {
                // "(W) mov(8) a0.0:uw tdr0.0:uw"
                bb->insertBefore(--bb->end(), builder.createMov(
                    g4::SIMD8,
                    builder.createDst(builder.phyregpool.getAddrReg(), 0, 0, 1, Type_UW),
                    builder.createSrc(builder.phyregpool.getTDRReg(), 0,
                                               0, builder.getRegionScalar(), Type_UW),
                    InstOpt_WriteEnable, false));
            }
        }
    }

    // Check if there is WAR/WAW dependency between end inst and the preceding instruction
    bool Optimizer::chkFwdOutputHazard(INST_LIST_ITER& startIter, INST_LIST_ITER& endIter)
    {
        G4_INST *startInst = *startIter;

        INST_LIST_ITER forwardIter = startIter;
        forwardIter++;
        while (forwardIter != endIter)
        {
            if ((*forwardIter)->isWAWdep(startInst) ||
                (*forwardIter)->isWARdep(startInst))
            {
                break;
            }
            forwardIter++;
        }

        if (forwardIter != endIter)
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    // check if startInst has any WAR/WAW conflicts with subsequent insts up till endIter (excluding endIter)
    // precondition: startInst must be before endIter in the same BB
    // this is used to sink an inst (or its sources) to the endIter location
    bool Optimizer::chkFwdOutputHazard(G4_INST* startInst, INST_LIST_ITER endIter)
    {
        INST_LIST_ITER backIter = std::prev(endIter, 1);
        while (*backIter != startInst)
        {
            G4_INST* inst = *backIter;
            if (inst->isWARdep(startInst) || inst->isWAWdep(startInst))
            {
                return true;
            }
            --backIter;
        }
        return false;
    }

    bool Optimizer::chkBwdOutputHazard(INST_LIST_ITER& startIter, INST_LIST_ITER& endIter)
    {
        G4_INST *endInst = *endIter;

        INST_LIST_ITER backwardIter = endIter;
        backwardIter--;
        while (backwardIter != startIter)
        {
            if (endInst->isWAWdep(*backwardIter) ||
                endInst->isWARdep(*backwardIter))
            {
                break;
            }
            backwardIter--;
        }

        if (backwardIter != startIter)
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    bool Optimizer::chkBwdOutputHazard(G4_INST* startInst, INST_LIST_ITER& endIter)
    {
        G4_INST *endInst = *endIter;

        INST_LIST_ITER backwardIter = endIter;
        backwardIter--;
        while (*backwardIter != startInst)
        {
            if (endInst->isWAWdep(*backwardIter) ||
                //    Makes sure there is not WAR conflict between this instruction and instruction preceding it:
                //    ... grf1(use preceding inst)
                //    grf1 <---- def this inst
                endInst->isWARdep(*backwardIter))
            {
                break;
            }
            backwardIter--;
        }

        if (*backwardIter != startInst)
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    /*
    Skips WAW check for the skipInst
    */
    bool Optimizer::chkBwdOutputHazard(G4_INST* startInst, INST_LIST_ITER& endIter, G4_INST *skipInst)
    {
        G4_INST *endInst = *endIter;

        INST_LIST_ITER backwardIter = endIter;
        backwardIter--;
        while (*backwardIter != startInst)
        {
            if (skipInst == *backwardIter)
            {
                --backwardIter;
                continue;
            }

            // Makes sure there is not WAR conflict between this instruction and instruction preceding it:
            // ... grf1(use preceding inst)
            // grf1 <---- def this inst
            if (endInst->isWARdep(*backwardIter) ||
                endInst->isWAWdep(*backwardIter))
            {
                break;
            }
            --backwardIter;
        }

        if (*backwardIter != startInst)
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    bool Optimizer::chkBwdWARdep(G4_INST* startInst, INST_LIST_ITER endIter)
    {
        while (*endIter != startInst)
        {
            G4_INST* inst = *endIter;
            if (inst->isWARdep(startInst))
            {
                return true;
            }
            --endIter;
        }
        return false;
    }

    // This function performs the following renaming to enable further optimization opportunities:
    //
    // op   v3, v1, v2
    // mov  v4, v3
    // mov  v5, v3
    // mov  v6, v3
    // ======>
    // op   v3, v1, v2
    // mov  v4, v3
    // mov  v5, v4
    // mov  v6, v4
    void Optimizer::renameRegister()
    {
        const int MAX_REG_RENAME_DIST = 250;
        const int MAX_REG_RENAME_SIZE = 2;

        BB_LIST_ITER ib, bend(fg.end());

        for (ib = fg.begin(); ib != bend; ++ib)
        {
            G4_BB* bb = (*ib);

            bb->resetLocalId();
            std::unordered_set<G4_INST *> Seen;

            INST_LIST_ITER ii = bb->begin(), iend(bb->end());
            while (ii != iend)
            {
                G4_INST *inst = *ii;

                if (!inst->isRawMov() ||
                    inst->getPredicate() ||
                    Seen.count(inst) > 0 ||
                    inst->def_size() != 1 ||
                    !inst->canHoist(!bb->isAllLaneActive(), fg.builder->getOptions()))
                {
                    ii++;
                    continue;
                }

                G4_Operand *src = inst->getSrc(0);
                G4_DstRegRegion *dst = inst->getDst();

                G4_Declare *srcDcl = src->isRegRegion() ? GetTopDclFromRegRegion(src) : nullptr;
                G4_Declare *dstDcl = GetTopDclFromRegRegion(dst);

                if (!srcDcl || !dstDcl)
                {
                    ++ii;
                    continue;
                }

                // If this move is between two different register files, then
                // do not do register renaming.
                if (srcDcl && dstDcl && srcDcl->getRegFile() != dstDcl->getRegFile())
                {
                    ++ii;
                    continue;
                }

                G4_INST *defInst = inst->def_front().first;
                G4_Declare *defDstDcl = GetTopDclFromRegRegion(defInst->getDst());
                if ((dstDcl && dstDcl->getAddressed()) ||
                    (defDstDcl && defDstDcl->getAddressed()))
                {
                    ii++;
                    continue;
                }

                G4_DstRegRegion* defDstRegion = defInst->getDst();
                if (Seen.count(defInst) > 0 ||
                    src->compareOperand(defDstRegion) != Rel_eq)
                {
                    ii++;
                    continue;
                }

                unsigned int instMaskOption = inst->getMaskOption();
                bool canRename = true;

                if (defInst->use_size() == 1)
                {
                    ii++;
                    continue;
                }

                int32_t sizeRatio = dstDcl->getByteSize() / srcDcl->getByteSize();

                G4_INST* lastUse = defInst->use_front().first;
                for (auto iter = defInst->use_begin(), E = defInst->use_end(); iter != E; ++iter)
                {
                    G4_INST* useInst = (*iter).first;

                    if (useInst == inst ||
                        ((useInst->getLocalId() - inst->getLocalId()) > MAX_REG_RENAME_DIST &&
                           sizeRatio > MAX_REG_RENAME_SIZE))
                    {
                        continue;
                    }

                    /*
                        it incorrectly renames in this case, because it doesn't consider
                        defInst mask.
                        BEFORE:
                        <TR><TD ALIGN="LEFT"><FONT color="black">0:mov (1) V44(0,0)[1]:d V46(0,0)[0;1,0]:d [Align1, NoMask] %11</FONT></TD></TR>
                        <TR><TD ALIGN="LEFT"><FONT color="black">0:mov (8) V48(0,0)[1]:d V44(0,0)[0;1,0]:d [Align1, Q1] %12</FONT></TD></TR>
                        <TR><TD ALIGN="LEFT"><FONT color="black">0:mov (8) V56(0,0)[1]:d V44(0,0)[0;1,0]:d [Align1, Q1] %22</FONT></TD></TR>

                        AFTER:
                        <TR><TD ALIGN="LEFT"><FONT color="black">0:mov (1) V44(0,0)[1]:d V46(0,0)[0;1,0]:d [Align1, NoMask] %11</FONT></TD></TR>
                        <TR><TD ALIGN="LEFT"><FONT color="black">0:mov (8) V48(0,0)[1]:d V44(0,0)[0;1,0]:d [Align1, Q1] %12</FONT></TD></TR>
                        <TR><TD ALIGN="LEFT"><FONT color="black">0:mov (8) V56(0,0)[1]:d V48(0,0)[0;1,0]:d [Align1, Q1] %22</FONT></TD></TR>

                        Fix: BB in SIMD control flow && (inst not NoMask) !(instMask == defFask == useMask)

                        Disallow replication?
                    */
                    if (useInst->getLocalId() < inst->getLocalId() ||
                        !useInst->isRawMov() ||
                        inst->getExecSize() != useInst->getExecSize() ||
                        (useInst->getSrc(0))->compareOperand(defDstRegion) != Rel_eq ||
                        useInst->def_size() > 1 ||
                        (!(inst->isWriteEnableInst()) &&
                        useInst->getMaskOption() != instMaskOption) ||
                        //fix described above
                        (!bb->isAllLaneActive() &&
                        !inst->isWriteEnableInst() &&
                        !(inst->getExecSize() == defInst->getExecSize() &&
                                inst->getExecSize() == useInst->getExecSize())
                       )
                     )
                    {
                        canRename = false;
                        break;
                    }

                    if (useInst->getLocalId() > lastUse->getLocalId())
                    {
                        lastUse = useInst;
                    }
                }

                for (auto iter = defInst->use_begin(), E = defInst->use_end(); iter != E; ++iter)
                {
                    G4_INST* useInst = (*iter).first;
                    Seen.insert(useInst);
                }

                if (!canRename)
                {
                    ii++;
                    continue;
                }

                INST_LIST_ITER forwardIter = ii;
                forwardIter++;
                while (canRename &&
                       *forwardIter != lastUse &&
                       (((*forwardIter)->getLocalId() - inst->getLocalId()) <= MAX_REG_RENAME_DIST ||
                          sizeRatio <= MAX_REG_RENAME_SIZE))
                {
                    if ((*forwardIter)->isWAWdep(inst))
                    {
                        canRename = false;
                        break;
                    }
                    forwardIter++;
                }

                if (!canRename)
                {
                    ii++;
                    continue;
                }

                for (auto useIter = defInst->use_begin(); useIter != defInst->use_end(); /*empty*/)
                {
                    G4_INST* useInst = (*useIter).first;

                    if (useInst == inst ||
                        ((useInst->getLocalId() - inst->getLocalId()) > MAX_REG_RENAME_DIST &&
                          sizeRatio > MAX_REG_RENAME_SIZE))
                    {
                        useIter++;
                        continue;
                    }

                    G4_Operand *useSrc = useInst->getSrc(0);
                    unsigned char execSize = useInst->getExecSize();
                    unsigned short dstHS = dst->getHorzStride();
                    const RegionDesc *newSrcRd;

                    if (useSrc->asSrcRegRegion()->isScalar())
                    {
                        newSrcRd = builder.getRegionScalar();
                    }
                    else
                    {
                        unsigned tExecSize = (execSize > 8) ? 8 : execSize;
                        if (RegionDesc::isLegal(tExecSize * dstHS, execSize, dstHS))
                        {
                            newSrcRd = builder.createRegionDesc((uint16_t)tExecSize*dstHS, execSize, dstHS);
                        }
                        else
                        {
                            // Skip this use. TODO: normalize this region.
                            ++useIter;
                            continue;
                        }
                    }

                    G4_SrcRegRegion *newSrcOpnd = builder.createSrcRegRegion(
                        Mod_src_undef,
                        dst->getRegAccess(),
                        dst->getBase(),
                        dst->getRegOff(),
                        dst->getSubRegOff(),
                        newSrcRd,
                        useSrc->getType());
                    if (dst->getRegAccess() != Direct)
                    {
                        newSrcOpnd->asSrcRegRegion()->setImmAddrOff(dst->getAddrImm());
                    }

                    useInst->getSrc(0)->~G4_Operand();
                    useInst->setSrc(newSrcOpnd, 0);

                    // Maintain def-use for this change:
                    // - remove this use from defInst
                    // - add a new use to inst
                    useIter = defInst->eraseUse(useIter);
                    inst->addDefUse(useInst, Opnd_src0);
                }

                ii++;
            }
        }
    }

//
// recompute bounds for declares in the given unordered_set
// this only affects DstRegRegion and SrcRegRegion
// If the operand is global, we have to update the global operand table as well
// as the bounds may have changed
//
void Optimizer::recomputeBound(std::unordered_set<G4_Declare*>& declares)
{

    for (auto bb : fg)
    {
        for (auto ii = bb->begin(), iiEnd = bb->end(); ii != iiEnd; ++ii)
        {
            G4_INST* inst = *ii;
            if (inst->getDst() != NULL)
            {
                G4_DstRegRegion* dst = inst->getDst();
                if (dst->getTopDcl() != NULL && declares.find(dst->getTopDcl()) != declares.end())
                {
                    bool isGlobal = builder.kernel.fg.globalOpndHT.isOpndGlobal(dst);
                    dst->computeLeftBound();
                    inst->computeRightBound(dst);
                    if (isGlobal)
                    {
                        builder.kernel.fg.globalOpndHT.addGlobalOpnd(dst);
                    }
                }
            }
            for (int i = 0, numSrc = inst->getNumSrc(); i < numSrc; ++i)
            {
                if (inst->getSrc(i) != NULL && inst->getSrc(i)->isSrcRegRegion())
                {
                    G4_SrcRegRegion* src = inst->getSrc(i)->asSrcRegRegion();
                    if (src->getTopDcl() != NULL && declares.find(src->getTopDcl()) != declares.end())
                    {
                        bool isGlobal = builder.kernel.fg.globalOpndHT.isOpndGlobal(src);
                        src->computeLeftBound();
                        inst->computeRightBound(src);
                        if (isGlobal)
                        {
                            builder.kernel.fg.globalOpndHT.addGlobalOpnd(src);
                        }
                    }
                }
            }
        }
    }
}

//
// Given a sequence of simd1 instructions (max 4), try to merge them into a single instruction
// e.g.,
// mul (1) r0.4<1>:f r0.0<0;1,0>:f r6.5<0;1,0>:f {NoMask}
// mul (1) r0.5<1>:f r0.1<0;1,0>:f r6.5<0;1,0>:f {NoMask}
// mul (1) r0.6<1>:f r0.2<0;1,0>:f r6.5<0;1,0>:f {NoMask}
// mul (1) r0.7<1>:f r0.3<0;1,0>:f r6.5<0;1,0>:f {NoMask}
// becomes
// mul (4) r0.4<1>:f r0.0<1;1,0>:f r6.5<0;1,0>:f {NoMask}
// A bunch of conditions have to be satisified; check BUNDLE_INFO for more details.
// This is only performed for 3D input as CM is very unlikely to benefit from this
// (put another way, if this succeeds for CM our FE is doing something wrong)
//
void Optimizer::mergeScalarInst()
{

    int bundleSizeLimit = BUNDLE_INFO::maxBundleSize;
    if (kernel.getInt32KernelAttr(Attributes::ATTR_Target) == VISA_3D)
    {
        bundleSizeLimit = 4;
    }

    Mem_Manager mergeManager(1024);
    // set of declares that have been changed to alias to another declare
    std::unordered_set<G4_Declare*> modifiedDcl;
    std::vector<G4_Declare*> newInputs;

    //stats
    int numBundles = 0;
    int numDeletedInst = 0;

    for (G4_BB* bb : fg)
    {
        std::vector<BUNDLE_INFO*> bundles;
        INST_LIST_ITER ii = bb->begin(), iiEnd = bb->end();
        while (ii != iiEnd)
        {
            G4_INST *inst = *ii;
            auto nextIter = ii;
            ++nextIter;
            if (nextIter != iiEnd && BUNDLE_INFO::isMergeCandidate(inst, builder, !bb->isAllLaneActive()))
            {
                BUNDLE_INFO* bundle = new (mergeManager) BUNDLE_INFO(bb, ii, bundleSizeLimit);
                bundle->findInstructionToMerge(nextIter, builder);
                if (bundle->size > 1)
                {
                    bundles.push_back(bundle);
                }
                else
                {
                    bundle->~BUNDLE_INFO();
                }
                ii = nextIter;
            }
            else
            {
                ++ii;
            }
        }

        for (auto bundle : bundles)
        {
            bool success = bundle->doMerge(builder, modifiedDcl, newInputs);
            if (success)
            {
                numBundles++;
                numDeletedInst += bundle->size - 1;
            }
            bundle->~BUNDLE_INFO();
        }
    }

    // we have to reset the bound for all operands whose declares have been modified
    recomputeBound(modifiedDcl);

    if (builder.getOption(vISA_OptReport))
    {
        std::ofstream optReport;
        getOptReportStream(optReport, builder.getOptions());
        optReport << "             === Merge Scalar Optimization ===\n";
        optReport << "Number of optimized bundles:\t" << numBundles << "\n";
        optReport << "Number of instructions saved:\t" << numDeletedInst << "\n";
        closeOptReportStream(optReport);
    }
}

static bool isMad(G4_INST *I)
{
    return I->opcode() == G4_pseudo_mad;
}

static inline bool isWBTypeAndNotNull(G4_Operand *opnd)
{
    // Requires opnd to be not null.
    return opnd && (IS_BTYPE(opnd->getType()) || IS_WTYPE(opnd->getType()));
}

namespace {

/// Class to describe a mad sequence that can be turned into a sequence of MAC
/// instructions.
class MadSequenceInfo {
public:
    // IR builder object.
    IR_Builder &builder;

    // The basic block being examined.
    G4_BB *bb;

    enum MadSeqKind { MK_unknown, MK_isSafe, MK_isNotSafe};

private:
    // Flag indicates if ACC transformation is safe.
    MadSeqKind kind;

    // The single definition that defines the first mad's src2. If there are
    // multiple defintions, it is nullptr.
    G4_INST *src2Def;

    // The sequence of mad instruction to be examined.
    std::vector<G4_INST *> madSequence;

    // The use chain of the last mad. This use chain ended with an instruction
    // that has *B/*W type, and the chain length is limited by a predefined
    // constant.
    std::vector<G4_INST *> lastMadUserChain;

public:
    MadSequenceInfo(IR_Builder &builder, G4_BB *bb)
        : builder(builder), bb(bb), kind(MK_unknown), src2Def(nullptr) {}

    bool isSafe() const { return kind == MK_isSafe; }
    bool isNotSafe() const { return kind == MK_isNotSafe; }
    void setNotSafe() { kind = MK_isNotSafe ; }
    void setSafe() { kind = MK_isSafe; }

    G4_INST *getSrc2Def() { return src2Def; }
    G4_INST *getFirstMad() const { return madSequence.front(); }
    G4_INST *getLastMad() const { return madSequence.back(); }
    void appendMad(INST_LIST_ITER begin, INST_LIST_ITER end)
    {
        madSequence.insert(madSequence.end(), begin, end);
    }
    typedef std::vector<G4_INST *>::iterator mad_iter;
    mad_iter mad_begin() { return madSequence.begin(); }
    mad_iter mad_end() { return madSequence.end(); }

    void appendUser(G4_INST *inst) { lastMadUserChain.push_back(inst); }
    G4_INST *getLastUser() { return lastMadUserChain.back(); }

    void reset()
    {
        kind = MK_unknown;
        src2Def = nullptr;
        madSequence.clear();
        lastMadUserChain.clear();
    }

    // Collect all candidate instructions and perform minimal checks.
    INST_LIST_ITER populateCandidates(INST_LIST_ITER iter);

    // Make changes to all the candidates collected. This is the only
    // function that makes changes to the IR.
    void processCandidates();

private:
    void populateUserChain(G4_INST *defInst, int level);
    void populateSrc2Def();

    // Check whether this mad sequence can be turned into a MAC sequence.
    bool checkMadSequence();

    // Check whether the user chain blocks this transformation or not.
    bool checkUserChain();

    // Check if other instructions between defInst and useInst are also updating
    // ACC registers, which may block this transformation.
    bool checkACCDependency(G4_INST *defInst, G4_INST *useInst);

    // The common type for accumulator operands.
    G4_Type getCommonACCType()
    {
        G4_Type T = Type_UNDEF;

        // If there is no user chain to check. Use the last mad's destionation
        // operand type as the common type. Otherwise, use the last user's
        // destionation type.
        if (lastMadUserChain.empty())
            T = getLastMad()->getDst()->getType();
        else
            T = getLastUser()->getDst()->getType();

        return IS_FTYPE(T) ? Type_F : (IS_SIGNED_INT(T) ? Type_W : Type_UW);
    }
};

class AccRestriction {
    unsigned encoding;

public:
    // Accumulator Restriction Kind:
    enum Accumulator_RK {
      ARK_NoRestriction = 0x01,              // No restrictions.
      ARK_NoAccess = 0x02,                   // No accumulator access, implicit or explicit.
      ARK_NoSourceOperand = 0x04,            // Source operands cannot be accumulators.
      ARK_NoModifier = 0x08,                 // Source modifier is not allowed if source is an accumulator.
      ARK_NoExplicitSource = 0x010,          // Accumulator is an implicit source and thus cannot be an explicit source operand.
      ARK_NoDst = 0x20,                      // Accumulator cannot be destination, implicit or explicit.
      ARK_AccWrEnRequired = 0x40,            // AccWrEn is required. The accumulator is an implicit
                                             // destination and thus cannot be an explicit destination operand.
      ARK_NoIntegerSource = 0x80,            // Integer source operands cannot be accumulators.
      ARK_NoExplicitSrcAllowAccWrEn = 0x100, // No explicit accumulator access because this is a three-source instruction.
                                             // AccWrEn is allowed for implicitly updating the accumulator.
      ARK_NoBothSrcAndDst = 0x200            // An accumulator can be a source or destination operand but not both.
    };

    AccRestriction(unsigned val) : encoding(val) {}

    bool useAccAsSrc(G4_SrcRegRegion *opnd, bool isExplicit = true,
                     bool isAlreadyDst = false) const
    {
        if (!opnd)
            return false;

        if (encoding & ARK_NoAccess)
            return false;

        if (encoding & ARK_NoRestriction)
            return true;

        if (encoding & ARK_NoSourceOperand)
            return false;

        if (encoding & ARK_NoIntegerSource)
            return !IS_TYPE_INT(opnd->getType());

        if (encoding & ARK_NoExplicitSource)
            return !isExplicit;

        if (encoding & ARK_NoExplicitSrcAllowAccWrEn)
            return false;

        if (encoding & ARK_NoBothSrcAndDst)
            return !isAlreadyDst;

        if (encoding & ARK_NoModifier)
            return opnd->getModifier() == Mod_src_undef;

        return true;
    }

    bool useAccAsDst(G4_DstRegRegion *opnd, bool isExplicit = true,
                     bool isAlreadySrc = false) const
    {
        if (!opnd)
            return false;

        if (encoding & ARK_NoAccess)
            return false;

        if (encoding & ARK_NoRestriction)
            return true;

        if (encoding & ARK_NoDst)
            return false;

        if (encoding & ARK_AccWrEnRequired)
            return !isExplicit;

        if (encoding & ARK_NoBothSrcAndDst)
            return !isAlreadySrc;

        return true;
    }

    static AccRestriction getRestrictionKind(G4_INST *inst);
};

} // namespace

AccRestriction AccRestriction::getRestrictionKind(G4_INST *inst)
{
    switch (inst->opcode()) {
    default:
        break;
    case G4_add:
    case G4_asr:
    case G4_avg:
    case G4_csel:
    case G4_frc:
    case G4_sel:
    case G4_shr:
    case G4_smov:
        return ARK_NoRestriction;
    case G4_addc:
    case G4_subb:
        return ARK_AccWrEnRequired;
    case G4_and:
    case G4_not:
    case G4_or:
    case G4_xor:
        return ARK_NoModifier;
    case G4_cmp:
    case G4_cmpn:
    case G4_lzd:
        return ARK_NoRestriction;
    case G4_dp2:
    case G4_dp3:
    case G4_dp4:
    case G4_dph:
    case G4_line:
    case G4_movi:
    case G4_pln:
    case G4_sad2:
    case G4_sada2:
        return ARK_NoSourceOperand;
    case G4_lrp:
    case G4_mac:
        return ARK_NoExplicitSource;
    case G4_madm:
        return ARK_NoExplicitSrcAllowAccWrEn;
    case G4_mach:
        return ARK_NoExplicitSource | ARK_AccWrEnRequired;
    case G4_mov:
        return ARK_NoBothSrcAndDst;
    case G4_mul:
        return ARK_NoIntegerSource;
    case G4_rndd:
    case G4_rnde:
    case G4_rndu:
    case G4_rndz:
        return ARK_NoRestriction;
    case G4_shl:
        return ARK_NoDst;
    }

    return ARK_NoAccess;
}

/// Check this pseudo-mad's dst operand. Returns false if there is anything
/// blocking acc's usage.
static bool checkMadDst(G4_INST *inst, IR_Builder &builder)
{
    G4_DstRegRegion *dst = inst->getDst();
    if (!dst || builder.kernel.fg.globalOpndHT.isOpndGlobal(dst))
        return false;

    if (dst->getRegAccess() != Direct)
        return false;

    // Only acc0 is available for w/uw destination.
    // FIXME: This acc type size is only for simd 16.
    unsigned Sz = TypeSize(Type_W);
    Sz *= dst->getHorzStride() * inst->getExecSize();
    return Sz <= numEltPerGRF<Type_UB>();
}

// Check whether this mad sequence can be turned into a MAC sequence.
bool MadSequenceInfo::checkMadSequence()
{
    unsigned int maskOffset = getFirstMad()->getMaskOffset();

    // First check each individual mad.
    for (auto inst : madSequence)
    {
        ASSERT_USER(isMad(inst), "not a mad");

        // Only for simd 16. TODO: support simd8.
        if (inst->getExecSize() != g4::SIMD16)
            return false;

        if (inst->getMaskOffset() != maskOffset)
            return false;

        // Do not handle predicate yet.
        if (inst->getPredicate() != nullptr)
            return false;

        // Do not handle cond modifier yet.
        if (inst->getCondMod() != nullptr)
            return false;

        if (!checkMadDst(inst, builder))
            return false;

        G4_Operand *src0 = inst->getSrc(0);
        G4_Operand *src1 = inst->getSrc(1);
        G4_Operand *src2 = inst->getSrc(2);

        if (!src0 || !src1 || !src2)
            return false;

        if (IS_FTYPE(src0->getType()) && IS_FTYPE(src1->getType()) &&
            IS_FTYPE(src2->getType()))
        {
            // ok
        }
        else if ((!IS_BTYPE(src0->getType()) && !IS_WTYPE(src0->getType())) ||
            (!IS_BTYPE(src1->getType()) && !IS_WTYPE(src1->getType())))
        {
            // Only when src0 and src1 are of Byte/Word types.
            return false;
        }
        else if (!IS_BTYPE(src2->getType()) && !IS_WTYPE(src2->getType()) &&
                !IS_DTYPE(src2->getType()))
        {
            // Only when src2 is of Byte/Word/DWord types.
            return false;
        }

        if (!builder.hasByteALU())
        {
            if (IS_BTYPE(src0->getType()) || IS_BTYPE(src1->getType()))
            {
                return false;
            }
        }

        if (builder.avoidAccDstWithIndirectSource())
        {
            if (src0->isSrcRegRegion() && src0->asSrcRegRegion()->isIndirect())
            {
                return false;
            }
            if (src1->isSrcRegRegion() && src1->asSrcRegRegion()->isIndirect())
            {
                return false;
            }
        }

        // If there is a modifier for src2, or src2 is accessed somewhere
        // indirectly then we will not generate a MAC.
        if (!src2->isSrcRegRegion())
            return false;

        if (src2->asSrcRegRegion()->getModifier() != Mod_src_undef ||
            src2->asSrcRegRegion()->getRegAccess() != Direct ||
            (src2->getTopDcl() && src2->getTopDcl()->getAddressed()))
            return false;
    }

    // Now check instructions in pairs.
    G4_INST *defInst = getSrc2Def();
    G4_INST *lastMad = getLastMad();
    for (auto I = mad_begin(); defInst != lastMad; ++I)
    {
        G4_INST *useInst = *I;
        G4_Operand *dst = defInst->getDst();
        G4_Operand *src2 = useInst->getSrc(2);
        ASSERT_USER(dst && dst->isDstRegRegion(), "invalid dst");
        ASSERT_USER(src2 && src2->isSrcRegRegion(), "invalid src2");
        if (dst->compareOperand(src2) != Rel_eq)
            return false;

        // Move the next pair.
        defInst = useInst;
    }

    return true;
}

bool MadSequenceInfo::checkACCDependency(G4_INST *defInst, G4_INST *useInst)
{
    auto iter = std::find(bb->begin(), bb->end(), defInst);
    ASSERT_USER(iter != bb->end(), "no instruction found?");

    for (++iter; (*iter) != useInst; ++iter) {
        if ((*iter)->defAcc() || (*iter)->useAcc() || (*iter)->mayExpandToAccMacro())
            return false;
    }
    return true;
}

// Check whether this user chain is safe to use ACC.
bool MadSequenceInfo::checkUserChain()
{
    // skip if there is no user to be analyzed.
    if (lastMadUserChain.empty())
        return true;

    G4_INST *defInst = getLastMad();
    G4_INST *useInst = defInst->use_back().first;

    while (true)
    {
        // TODO: enable simd8.
        if (useInst->getExecSize() != g4::SIMD16)
            return false;

        // Only when used as a source operand.
        Gen4_Operand_Number opndNum = defInst->use_back().second;
        if (!G4_INST::isSrcNum(opndNum))
            return false;

        G4_Operand *useOpnd = useInst->getSrc(G4_INST::getSrcNum(opndNum));
        if (!useOpnd || !useOpnd->isSrcRegRegion())
            return false;

        if (useOpnd->asSrcRegRegion()->isIndirect())
            return false;

        // check other source type.
        for (int i = 0, e = useInst->getNumSrc(); i < e; ++i)
        {
            G4_Operand *otherSrc = useInst->getSrc(i);
            if (otherSrc == useOpnd)
                continue;

            if (!isWBTypeAndNotNull(otherSrc))
                return false;
        }

        bool isLastUser = (useInst == getLastUser());

        // The last user does not use ACC as its dst.
        AccRestriction AR = AccRestriction::getRestrictionKind(useInst);
        if (!AR.useAccAsSrc(useOpnd->asSrcRegRegion(), true, !isLastUser))
            return false;

        // Now check between defInst and useInst, no ACC will be written by
        // other instructions.
        if (!checkACCDependency(defInst, useInst))
            return false;

        if (isLastUser)
            // No extra check for the last user.
            break;

        // This is not the last user. We check its dst too.
        G4_Operand *useDst = useInst->getDst();
        if (!useDst)
            return false;

        // check type, no support for *Q types yet.
        if (!IS_DTYPE(useDst->getType()))
            return false;

        // For each inner user, need to use ACC as its explicit dst.
        if (!AR.useAccAsDst(useDst->asDstRegRegion(), true, true))
            return false;

        if (defInst->getDst()->compareOperand(useOpnd) != Rel_eq)
            return false;

        // move to next pair.
        defInst = useInst;
        useInst = defInst->use_back().first;
    }

    // nothing wrong.
    return true;
}

void MadSequenceInfo::populateSrc2Def()
{
    G4_INST *firstMad = getFirstMad();
    ASSERT_USER(firstMad && isMad(firstMad), "invalid mad");

    src2Def = firstMad->getSingleDef(Opnd_src2, true);
    if (src2Def == nullptr)
        // Cannot find a single definition.
        return setNotSafe();

    // Check it right here.
    // Only support splats or simd16 initialization.
    if (src2Def->getExecSize() != g4::SIMD16 && src2Def->getExecSize() != g4::SIMD1)
    {
        return setNotSafe();
    }

    G4_Operand *Dst = src2Def->getDst();
    if (!Dst || builder.kernel.fg.globalOpndHT.isOpndGlobal(Dst))
        return setNotSafe();

    if (Dst->asDstRegRegion()->getRegAccess() != Direct)
        return setNotSafe();

    if (src2Def->getPredicate() || src2Def->getSaturate() ||
        !src2Def->hasOneUse())
        return setNotSafe();

    if (IS_DTYPE(src2Def->getExecType()))
    {
        // since we use <1>:w region for our acc temp, due to alignment requirements
        // we can't allow dword source types
        return setNotSafe();
    }

    if (!builder.hasByteALU())
    {
        // do not allow acc if src2Dest inst has byte source
        for (int i = 0; i < src2Def->getNumSrc(); ++i)
        {
            if (IS_BTYPE(src2Def->getSrc(i)->getType()))
            {
                return setNotSafe();
            }
        }
    }

    if (builder.avoidAccDstWithIndirectSource())
    {
        for (int i = 0; i < src2Def->getNumSrc(); ++i)
        {
            if (src2Def->getSrc(i)->isSrcRegRegion() && src2Def->getSrc(i)->asSrcRegRegion()->isIndirect())
            {
                return setNotSafe();
            }
        }
    }


    // Check if there is any ACC dependency.
    if (!checkACCDependency(src2Def, firstMad))
        return setNotSafe();

    // Check restrictions on compression to ensure that changing the destination
    // type will not change the source region meaning due to instruction
    // compression.
    //
    // If both instructions are compressed or both non-compressed then it is
    // safe. Otherwise, check whether source regions are compression invariants.
    if (src2Def->isComprInst() ^ firstMad->isComprInst())
    {
        auto checkCompression = [](G4_INST *inst)
        {
            for (int i = 0; i < inst->getNumSrc(); ++i)
            {
                G4_Operand *opnd = inst->getSrc(i);
                if (!opnd || !opnd->isSrcRegRegion())
                    continue;
                if (!inst->isComprInvariantSrcRegion(opnd->asSrcRegRegion(), i))
                    return false;
                if (IS_DTYPE(opnd->getType()))
                    return false;
            }
            return true;
        };

        if (src2Def->isComprInst())
        {
            if (!checkCompression(src2Def))
               return setNotSafe();
        }
        else
        {
            if (!checkCompression(firstMad))
               return setNotSafe();
        }
    }

    AccRestriction AR = AccRestriction::getRestrictionKind(src2Def);
    if (!AR.useAccAsDst(Dst->asDstRegRegion()))
        return setNotSafe();
}

void MadSequenceInfo::populateUserChain(G4_INST *defInst, int level)
{
    // Failed.
    if (level <= 0)
        return setNotSafe();

    // Only for a single use.
    if (!defInst->hasOneUse())
        return setNotSafe();

    // Only when used as a source operand.
    Gen4_Operand_Number opndNum = defInst->use_back().second;
    if (!G4_INST::isSrcNum(opndNum))
        return setNotSafe();

    G4_INST *useInst = defInst->use_back().first;
    auto useDst = useInst->getDst();

    if (useDst == nullptr)
        return setNotSafe();

    appendUser(useInst);

    // If this user has *W/*B types then candidate found and stop the search.
    if (IS_BTYPE(useDst->getType()) || IS_WTYPE(useDst->getType()))
        return;

    // Search to the next level.
    return populateUserChain(useInst, level - 1);
}

// Currently, we assume that MAD instructions are back-to-back. This avoids
// dependency checking among mad and non-mad instructions.
//
// TODO: hoist or sink instructions.
//
INST_LIST_ITER MadSequenceInfo::populateCandidates(INST_LIST_ITER iter)
{
    // Find the first pseudo-mad instruction.
    iter = std::find_if(iter, bb->end(), isMad);

    // No mad found
    if (iter == bb->end())
        return iter;

    // Find the first non-mad instruction following this sequence.
    auto end = std::find_if_not(iter, bb->end(), isMad);

    ASSERT_USER(iter != end, "out of sync");
    appendMad(iter, end);

    // Checking the first mad's src2.
    populateSrc2Def();

    // If the mad sequence has *W/*B types then it is safe to use ACC regardless
    // of the destionation operand type in its use.
    // ..
    // mac (16) acc0.0<1>:w r10.7<0;1,0>:w r62.16<16;16,1>:ub {Align1, H1}
    // mac (16) acc0.0<1>:w r11.6<0;1,0>:w r63.0<16;16,1>:ub {Align1, H1}
    // mac (16) r24.0<1>:w r11.7<0;1,0>:w r63.16<16;16,1>:ub {Align1, H1}
    // add (16) r14.0<1>:d r14.0<8;8,1>:d r24.0<8;8,1>:w {Align1, H1}
    //
    // could be generated.
    G4_Type MadDstType = getLastMad()->getDst()->getType();
    if (IS_DTYPE(MadDstType))
    {
        // Populate the user chain up to some predetermined level.
        const int level = 4;
        populateUserChain(getLastMad(), level);
    }

    // We have gathered all candidates for this optimization. Now we make
    // comprehensive checks on the mad sequence and the user chain.
    if (isNotSafe())
        return end;

    if (!checkMadSequence())
        return end;

    if (!checkUserChain())
        return end;

    // everything is OK, preceed to do transformation.
    setSafe();
    return end;
}

// Makes changes to the mad sequence and its users.
void MadSequenceInfo::processCandidates()
{
    ASSERT_USER(isSafe(), "not safe for ACC");
    ASSERT_USER(getSrc2Def(), "null src");
    ASSERT_USER(!madSequence.empty(), "no mad");

    // In this function we replace all src2 to implicit ACC registers and
    // update operands in its use chain.
    G4_Type AdjustedType = getCommonACCType();

    // Fix src2Def
    G4_INST *src2Def = getSrc2Def();
    {
        // change dst of the last MAD
        G4_DstRegRegion *accDstOpnd = builder.createDst(builder.phyregpool.getAcc0Reg(), 0, 0, 1, AdjustedType);
        src2Def->setDest(accDstOpnd);

        // Convert splat.
        if (src2Def->getExecSize() == g4::SIMD1)
        {
            src2Def->setExecSize(getFirstMad()->getExecSize());
        }
    }

    // update use-chain
    if (!lastMadUserChain.empty())
    {
        G4_INST *defInst = getLastMad();
        G4_INST *useInst = defInst->use_back().first;
        Gen4_Operand_Number opndNum = defInst->use_back().second;
        ASSERT_USER(defInst->hasOneUse(), "bad candidate");

        while (true)
        {
            const RegionDesc *rd = builder.getRegionStride1();
            auto mod = useInst->getOperand(opndNum)->asSrcRegRegion()->getModifier();
            G4_SrcRegRegion *accSrcOpnd = builder.createSrcRegRegion(
                mod, Direct, builder.phyregpool.getAcc0Reg(), 0, 0, rd, AdjustedType);
            useInst->setSrc(accSrcOpnd, G4_INST::getSrcNum(opndNum));

            // The last use, only update source, and exit.
            if (useInst == getLastUser())
                break;

            // Also update the destination.
            G4_DstRegRegion *accDstOpnd = builder.createDst(builder.phyregpool.getAcc0Reg(), 0, 0, 1, AdjustedType);
            useInst->setDest(accDstOpnd);

            // move to the next pair.
            defInst = useInst;
            useInst = defInst->use_back().first;
            opndNum = defInst->use_back().second;
            ASSERT_USER(defInst->hasOneUse(), "bad candidate");
        }
    }

    // update mad sequence
    for (auto I = mad_begin(), E = mad_end(); I != E; ++I) {
        G4_INST *inst = *I;
        ASSERT_USER(isMad(inst), "not a mad");

        const RegionDesc *rd = builder.getRegionStride1();
        G4_SrcRegRegion *accSrcOpnd = builder.createSrc(builder.phyregpool.getAcc0Reg(), 0, 0, rd,
            AdjustedType);

        inst->setImplAccSrc(accSrcOpnd);
        inst->setSrc(nullptr, 2);
        // For the last mad, if it has *B/*W type, then no user will be modified
        // and do not change its destination operand. Otherwise, use acc as the
        // destination.
        if (getLastMad() != inst || !lastMadUserChain.empty())
        {
            G4_DstRegRegion *accDstOpnd = builder.createDst(builder.phyregpool.getAcc0Reg(), 0, 0, 1, AdjustedType);
            inst->setDest(accDstOpnd);
        }
        inst->setOpcode(G4_mac);
        inst->fixMACSrc2DefUse();
    }
}

// Do any kind of proprocessing in this basic block to help MAC transformation.
// Returns false if we can easily detect this optimization is not possible.
// Otherwise, returns true.
static bool preprocessMadInBlock(IR_Builder &builder, G4_BB *bb)
{
    bool hasMad = false;
    for (auto inst : *bb)
    {
        if (isMad(inst))
        {
            hasMad = true;
            HWConformity::tryEliminateMadSrcModifier(builder, inst);
        }
    }

    // nothing to do if there is no mad.
    return hasMad;
}

//
// mul (16) V48(0,0)<1>:d r0.1<0;1,0>:w V42_in(7,2)<16;16,1>:ub {Align1, H1}
// psuedo_mad (16) V51_tempConvolve(0,0)<1>:d r1.0<0;1,0>:w V42_in(7,1)<16;16,1>:ub V48(0,0)<16;16,1>:d {Align1, H1}
// psuedo_mad (16) V51_tempConvolve(0,0)<1>:d r0.2<0;1,0>:w V42_in(7,3)<16;16,1>:ub V51_tempConvolve(0,0)<16;16,1>:d {Align1, H1}
// psuedo_mad (16) V51_tempConvolve(0,0)<1>:d r0.3<0;1,0>:w V42_in(8,1)<16;16,1>:ub V51_tempConvolve(0,0)<16;16,1>:d {Align1, H1}
// psuedo_mad (16) V51_tempConvolve(0,0)<1>:d r0.4<0;1,0>:w V42_in(8,2)<16;16,1>:ub V51_tempConvolve(0,0)<16;16,1>:d {Align1, H1}
// psuedo_mad (16) V51_tempConvolve(0,0)<1>:d r0.5<0;1,0>:w V42_in(8,3)<16;16,1>:ub V51_tempConvolve(0,0)<16;16,1>:d {Align1, H1}
// psuedo_mad (16) V51_tempConvolve(0,0)<1>:d r0.6<0;1,0>:w V42_in(9,1)<16;16,1>:ub V51_tempConvolve(0,0)<16;16,1>:d {Align1, H1}
// psuedo_mad (16) V51_tempConvolve(0,0)<1>:d r0.7<0;1,0>:w V42_in(9,2)<16;16,1>:ub V51_tempConvolve(0,0)<16;16,1>:d {Align1, H1}
// psuedo_mad (16) V51_tempConvolve(0,0)<1>:d r0.8<0;1,0>:w V42_in(9,3)<16;16,1>:ub V51_tempConvolve(0,0)<16;16,1>:d {Align1, H1}
// add (16) V51_tempConvolve(0,0)<1>:d V51_tempConvolve(0,0)<16;16,1>:d 0x4000:w {Align1, H1}
// shr.sat (16) V52(0,0)<1>:ub V51_tempConvolve(0,0)<16;16,1>:d 0xf:w {Align1, H1}
//
void Optimizer::lowerMadSequence()
{

    // Only enable CM for now.
    if (kernel.getInt32KernelAttr(Attributes::ATTR_Target) != VISA_CM)
        return;

    for (G4_BB *bb : fg)
    {
        // Preprocess this basic block. If no mad sequence found then skip to
        // the next basic block right away.
        if (!preprocessMadInBlock(builder, bb))
            continue;

        // Object to gather information for ACC optimization.
        MadSequenceInfo madInfo(builder, bb);

        auto iter = bb->begin();
        while (iter != bb->end())
        {
            // Returns an iterator to the next non-mad instruction after the mad
            // sequence. It is safe to insert/delete instructions before it.
            iter = madInfo.populateCandidates(iter);

            // Perform transformation. The resulted IR may still need to be
            // fixed by HWConformity, e.g. the dst may still have *B type.
            if (madInfo.isSafe())
                madInfo.processCandidates();

            // Cleanup immediate results, whether change has been made or not.
            madInfo.reset();
        }
    }
}

void Optimizer::ifCvt()
{
    runIfCvt(fg);
}



namespace {

enum SplitBounds : unsigned {
    LoLBound = 0,
    LoRBound = 63,
    HiLBound = 64,
    HiRBound = 127
};

static bool isCandidateDecl(G4_Declare *Dcl, const IR_Builder& builder)
{
    G4_Declare *RootDcl = Dcl->getRootDeclare();
    if (RootDcl->getRegFile() != G4_GRF)
        return false;

    // Only split 4GRF variables. We should be able to split > 4GRF variables,
    // but this should have been done in FE.
    if (RootDcl->getByteSize() != 4 * numEltPerGRF<Type_UB>())
        return false;

    if (RootDcl->getAddressed())
        return false;

    if (builder.isPreDefArg(RootDcl) || builder.isPreDefRet(RootDcl))
    {
        return false;
    }

    // ToDo: add more special declares to exclude list

    return true;
}

// Associated declarations for splitting.
struct DclMapInfo {
    // The low part of the splitted variable.
    G4_Declare *DclLow;

    // The high part of the splitted variable.
    G4_Declare *DclHigh;

    // Aliases of the low part. Created if needed for different types.
    std::vector<G4_Declare *> AliasLow;

    // Aliases of the high part. Created if needed for different types.
    std::vector<G4_Declare *> AliasHigh;

    DclMapInfo(G4_Declare *Lo, G4_Declare *Hi) : DclLow(Lo), DclHigh(Hi) {}

    // Return an appropriate declaration/alias for low or high part.
    G4_Declare *getDcl(IR_Builder &Builder, G4_Type Ty, bool IsLow)
    {
        return IsLow ? getDcl(Builder, Ty, DclLow, AliasLow)
                     : getDcl(Builder, Ty, DclHigh, AliasHigh);
    }

private:
    G4_Declare *getDcl(IR_Builder &Builder, G4_Type Ty, G4_Declare *RootDcl,
                       std::vector<G4_Declare *> &Aliases)
    {
        if (Ty == RootDcl->getElemType())
            return RootDcl;

        for (auto AL : Aliases)
        {
            if (Ty == AL->getElemType())
                return AL;
        }

        // Create such an alias if it does not exist yet.
        unsigned NElts = RootDcl->getByteSize() / TypeSize(Ty);
        auto Alias = Builder.createTempVar(NElts, Ty, Any,
            (std::string(RootDcl->getName()) + "_" + TypeSymbol(Ty)).c_str(), false);
        Alias->setAliasDeclare(RootDcl, 0);
        Aliases.push_back(Alias);
        return Alias;
    }
};

}  // namespace

//
// We split any 4GRF variables (they typically result from simd16 64-bit vars) into two half if
// -- they are not address taken or used in send
// -- none of the operands cross from the 2nd to the 3rd GRF
// This is intended to give RA more freedom as the split variables do
// not have to be allocated contiguously.
// Note that this invalidates existing def-use chains
//
void Optimizer::split4GRFVars()
{
    std::unordered_set<G4_Declare*> varToSplit;
    std::vector<G4_Declare*> varToSplitOrdering;
    // map each split candidate to their replacement split variables
    std::unordered_map<const G4_Declare *, DclMapInfo *> DclMap;

    if (kernel.getInt32KernelAttr(Attributes::ATTR_Target) != VISA_3D)
    {
        return;
    }

    if (builder.getOption(vISA_Debug))
    {
        return;
    }

    // Only for simd16 and simd32.
    if (kernel.getSimdSize() == g4::SIMD8)
    {
        return;
    }

    // first scan the decl list
    for (auto dcl : kernel.Declares)
    {
        if (dcl->getAliasDeclare() == nullptr)
        {
            if (isCandidateDecl(dcl, builder))
            {
                if (varToSplit.find(dcl) == varToSplit.end())
                {
                    varToSplitOrdering.push_back(dcl);
                }

                varToSplit.emplace(dcl);
            }
        }
        else
        {
            // strictly speaking this condition is not necesary, but having
            // no aliases that could point into a middle of the split candidate
            // makes replacing the split var much easier. By construction the root
            // must appear before its alias decls
            uint32_t offset = 0;
            G4_Declare* rootDcl = dcl->getRootDeclare(offset);
            if (offset != 0 && isCandidateDecl(rootDcl, builder))
            {
                varToSplit.erase(rootDcl);
            }
        }
    }

    if (varToSplit.empty())
    {
        // early exit if there are no split candidate
        return;
    }

    // first pass is to make sure the validity of all split candidates
    for (auto bb : kernel.fg)
    {
        for (auto inst : *bb)
        {
            auto removeCandidate = [&varToSplit](G4_Declare* dcl)
            {
                if (dcl)
                {
                    dcl = dcl->getRootDeclare();
                    varToSplit.erase(dcl);
                }
            };

            if (inst->isSend())
            {
                removeCandidate(inst->getDst()->getTopDcl());
                removeCandidate(inst->getSrc(0)->getTopDcl());
                if (inst->isSplitSend())
                {
                    removeCandidate(inst->getSrc(1)->getTopDcl());
                }
            }
            else
            {
                auto cross2GRF = [](G4_Operand* opnd)
                {
                    uint32_t lb = opnd->getLeftBound();
                    uint32_t rb = opnd->getRightBound();
                    return (lb < 2u * numEltPerGRF<Type_UB>()) && (rb >= 2u * numEltPerGRF<Type_UB>());
                };
                // check and remove decls with operands that cross 2GRF boundary
                if (inst->getDst())
                {
                    G4_Declare* dstDcl = inst->getDst()->getTopDcl();
                    if (dstDcl && cross2GRF(inst->getDst()))
                    {
                        removeCandidate(dstDcl);
                    }
                }
                for (int i = 0, numSrc = inst->getNumSrc(); i < numSrc; ++i)
                {
                    G4_Operand* src = inst->getSrc(i);
                    if (src && src->getTopDcl() && cross2GRF(src))
                    {
                        removeCandidate(src->getTopDcl());
                    }
                }
            }
        }
    }

    if (varToSplit.empty())
    {
        // early exit if there are no split candidate
        return;
    }

    // create Lo/Hi for each variable being split
    for (auto splitDcl : varToSplitOrdering)
    {
        // varToSplitOrdering may have extra elements since we never delete any inserted dcl from it.
        if (varToSplit.find(splitDcl) == varToSplit.end())
            continue;
        G4_Type Ty = splitDcl->getElemType();
        unsigned NElts = splitDcl->getTotalElems();
        std::string varName(splitDcl->getName());
        auto DclLow = builder.createTempVar(NElts / 2, Ty, GRFALIGN,
            (varName + "Lo").c_str(), false);
        auto DclHi = builder.createTempVar(NElts / 2, Ty, GRFALIGN,
            (varName + "Hi").c_str(), false);
        DclMap[splitDcl] = new DclMapInfo(DclLow, DclHi);
        //std::cerr << "split " << splitDcl->getName() << " into (" <<
        //   DclLow->getName() << ", " << DclHi->getName() << ")\n";
    }

    // second pass actually does the replacement
    for (auto bb : kernel.fg)
    {
        for (auto inst : *bb)
        {
            auto dst = inst->getDst();
            if (dst && dst->getTopDcl())
            {
                G4_Declare* dstRootDcl = dst->getTopDcl()->getRootDeclare();
                if (DclMap.count(dstRootDcl))
                {
                    bool isLow = dst->getLeftBound() < 2u * numEltPerGRF<Type_UB>();
                    auto NewDcl = DclMap[dstRootDcl]->getDcl(builder, dst->getType(), isLow);
                    auto NewDst = builder.createDst(NewDcl->getRegVar(),
                        dst->getRegOff() - (isLow ? 0 : 2), dst->getSubRegOff(),
                        dst->getHorzStride(), dst->getType(), dst->getAccRegSel());
                    inst->setDest(NewDst);
                }
            }

            for (int i = 0, numSrc = inst->getNumSrc(); i < numSrc; ++i)
            {
                G4_Operand* src = inst->getSrc(i);
                if (src && src->getTopDcl())
                {
                    G4_SrcRegRegion* srcRegion = src->asSrcRegRegion();
                    G4_Declare* srcRootDcl = src->getTopDcl()->getRootDeclare();
                    if (DclMap.count(srcRootDcl))
                    {
                        bool isLow = src->getLeftBound() < 2u * numEltPerGRF<Type_UB>();
                        auto NewSrcDcl = DclMap[srcRootDcl]->getDcl(builder, src->getType(), isLow);
                        auto NewSrc = builder.createSrcRegRegion(
                            srcRegion->getModifier(), src->getRegAccess(),
                            NewSrcDcl->getRegVar(), srcRegion->getRegOff() - (isLow ? 0 : 2),
                            srcRegion->getSubRegOff(), srcRegion->getRegion(), src->getType(), src->getAccRegSel());
                        inst->setSrc(NewSrc, i);
                    }
                }
            }
        }
    }

    for (auto DI : DclMap)
    {
        delete DI.second;
    }
}

//
// A platform may not support 64b types (FP64, INT64, or neither).
// While HW conformity should have legalized away use of such types, they may get
// re-introduced again later due to copy moves inserted by spill code generation, rematerialization etc.
// Instead of checking whether 64b type is used at each createMov(), we add a catch-all pass here.
// Since this is called post-RA the change we can make are very limited, for now just handle copy moves.
// We make this a separate pass instead of part of changeMoveType() as the latter is considered an optimization.
//
void Optimizer::legalizeType()
{
    if (builder.noFP64() || builder.noInt64())
    {
        for (auto bb : kernel.fg)
        {
            for (auto inst : *bb)
            {
                auto uses64bType = [](G4_INST* inst)
                {
                    bool useFP64 = false;
                    bool useInt64 = false;
                    {
                        auto dstTy = inst->getDst() ? inst->getDst()->getType() : Type_UNDEF;
                        if (dstTy == Type_DF)
                        {
                            useFP64 = true;
                        }
                        else if (dstTy == Type_Q || dstTy == Type_UQ)
                        {
                            useInt64 = true;
                        }
                    }
                    for (int i = 0, numSrc = inst->getNumSrc(); i < numSrc; ++i)
                    {
                        auto srcTy = inst->getSrc(i) ? inst->getSrc(i)->getType() : Type_UNDEF;
                        if (srcTy == Type_DF)
                        {
                            useFP64 = true;
                        }
                        else if (srcTy == Type_Q || srcTy == Type_UQ)
                        {
                            useInt64 = true;
                        }
                    }
                    return std::make_tuple(useFP64, useInt64);
                };
                //ToDo: handle more cases (e.g., immSrc, use UD for copy moves)
                if (inst->isRawMov() && inst->getSrc(0)->isSrcRegRegion())
                {
                    bool hasFP64 = false, hasInt64 = false;
                    std::tie(hasFP64, hasInt64) = uses64bType(inst);
                    if (hasFP64 && hasInt64)
                    {
                        assert(false && "can't handle inst with both FP64 and INT64 at this point");
                        return;
                    }
                    if (hasFP64 && builder.noFP64())
                    {
                        assert(!builder.noInt64() && "can't change DF to UQ");
                        inst->getDst()->setType(Type_UQ);
                        inst->getSrc(0)->asSrcRegRegion()->setType(Type_UQ);
                    }
                    if (hasInt64 && builder.noInt64() && !builder.noFP64())
                    {
                        inst->getDst()->setType(Type_DF);
                        inst->getSrc(0)->asSrcRegRegion()->setType(Type_DF);
                    }
                }
            }
        }
    }
}


//
// Categorize move instructions to help with performance analysis
//
void Optimizer::analyzeMove()
{

    #define MOVE_TYPE(DO) \
    DO(Total) \
    DO(SatOrMod) \
    DO(Imm32) \
    DO(Imm64) \
    DO(FPConvert) \
    DO(Trunc) \
    DO(Extend) \
    DO(Broadcast) \
    DO(UNPACK) \
    DO(PACK) \
    DO(Copy) \
    DO(Misc) \
    DO(LAST)

    enum MovTypes {
        MOVE_TYPE(MAKE_ENUM)
    };

    static const char* moveNames[] =
    {
        MOVE_TYPE(STRINGIFY)
    };

    std::array<int, MovTypes::LAST> moveCount = {0};

    for (auto bb : kernel.fg)
    {
        for (auto inst : *bb)
        {
            if (!inst->isMov())
            {
                continue;
            }
            moveCount[MovTypes::Total]++;

            if (inst->getSaturate())
            {
                moveCount[MovTypes::SatOrMod]++;
                continue;
            }
            auto dstTy = inst->getDst()->getType();
            auto srcTy = inst->getSrc(0)->getType();
            if (inst->getSrc(0)->isImm())
            {
                moveCount[TypeSize(srcTy) == 8 ? MovTypes::Imm64 : MovTypes::Imm32]++;
            }
            else if (inst->getSrc(0)->isSrcRegRegion())
            {
                auto srcRR = inst->getSrc(0)->asSrcRegRegion();
                if (srcRR->getModifier() != Mod_src_undef)
                {
                    moveCount[SatOrMod]++;
                    continue;
                }
                bool signChange = false;
                if (dstTy != srcTy)
                {
                    if (IS_FTYPE(srcTy) || IS_FTYPE(dstTy))
                    {
                        // distinguish inttofp and fpconvert?
                        moveCount[MovTypes::FPConvert]++;
                    }
                    else if (TypeSize(dstTy) > TypeSize(srcTy))
                    {
                        moveCount[MovTypes::Extend]++;
                    }
                    else if (TypeSize(srcTy) > TypeSize(dstTy))
                    {
                        moveCount[MovTypes::Trunc]++;
                    }
                    else
                    {
                        signChange = true;
                    }
                }
                if (dstTy == srcTy || signChange)
                {
                    if (srcRR->isScalar())
                    {
                        moveCount[inst->getExecSize() > g4::SIMD1 ? MovTypes::Broadcast : MovTypes::Copy]++;
                    }
                    else if (srcRR->getRegion()->isContiguous(inst->getExecSize()))
                    {
                        moveCount[inst->getDst()->getHorzStride() == 1 ? MovTypes::Copy : MovTypes::UNPACK]++;
                    }
                    else
                    {
                        bool singleStride = srcRR->getRegion()->isSingleStride(inst->getExecSize());
                        if (singleStride && inst->getDst()->getHorzStride() == 1)
                        {
                            moveCount[MovTypes::PACK]++;
                        }
                        else
                        {
                            // give up
                            moveCount[MovTypes::Misc]++;
                        }
                    }
                }
            }
            else
            {
                moveCount[MovTypes::Misc]++;
            }
        }
    }

    std::cerr << "Move classification:\n";
    for (int i = 0; i < MovTypes::LAST; ++i)
    {
        if (moveCount[i] > 0)
        {
            std::cerr << "\t" << moveNames[i] << ":\t" << moveCount[i] << "\n";
        }
    }

    #undef MOVE_TYPE
}

//
// remove Intrinsics
//
void Optimizer::removeInstrinsics()
{
    for (auto bb : kernel.fg)
    {
        bb->removeIntrinsics(Intrinsic::MemFence);
    }
}

//
// for some platforms int has half throughout compared to float,
// so for copy moves we should change their type
// from D/UD to F or W/UW to HF when possible
//
void Optimizer::changeMoveType()
{
    if (!builder.favorFloatMov() && !builder.balanceIntFloatMoves())
    {
        return;
    }

    auto changeType = [this](G4_INST* movInst, G4_Type newTy)
    {
        movInst->getDst()->setType(newTy);
        auto src0 = movInst->getSrc(0);
        if (src0->isImm())
        {
            uint32_t mask = TypeSize(newTy) == 4 ? 0xFFFFFFFF : 0xFFFF;
            movInst->setSrc(fg.builder->createImm(src0->asImm()->getImm() & mask, newTy), 0);
        }
        else
        {
            movInst->getSrc(0)->asSrcRegRegion()->setType(newTy);
        }
    };




    for (auto bb : fg)
    {
        for (auto inst : *bb)
        {
            if (inst->opcode() != G4_mov)
            {
                continue;
            }
            // copy move means dst and src has identical bits (implies same type width), and there are no
            // sat/conditional modifier as well as src modifier
            // ToDo: we should probably change isRawMov() to include mov UD D
            auto src0 = inst->getSrc(0);

            // FIXME: This is a quick WA to bypass RelocImm, so that it won't create a new src0 and
            // overwrite the original RelocImm
            // While this optimization should still be able to apply to RelocImm. Once we turn on this optimization
            // for RelocImm, we should update assert in VISAKernelImpl::GetGenRelocEntryBuffer to allow float type
            if (src0->isRelocImm())
                continue;

            G4_Type dstTy = inst->getDst()->getType();
            G4_Type src0Ty = src0->getType();
            bool hasNoModifier = !inst->getSaturate() && !inst->getCondMod() &&
               (src0->isImm() || (src0->isSrcRegRegion() && src0->asSrcRegRegion()->getModifier() == Mod_src_undef));

            // it may be unsafe to change the move type for acc as it has higher precision
            if (inst->getDst()->isGreg() && hasNoModifier)
            {
                if (src0->isGreg())
                {
                    bool isIntCopyMove = IS_TYPE_INT(dstTy) && IS_TYPE_INT(src0Ty) &&
                        TypeSize(dstTy) == TypeSize(src0Ty);
                    if (isIntCopyMove)
                    {
                        if (dstTy == Type_D || dstTy == Type_UD)
                        {
                            changeType(inst, Type_F);
                        }
                        else if (dstTy == Type_W || dstTy == Type_UW)
                        {
                            changeType(inst, Type_HF);
                        }
                    }
                }
                else if (src0->isImm())
                {
                    // allow sext and zext imm moves
                    int64_t immVal = src0->asImm()->getImm();
                    bool isIntImmMove = IS_TYPE_INT(dstTy) && IS_TYPE_INT(src0Ty) &&
                        G4_Imm::isInTypeRange(immVal, dstTy);
                    if (isIntImmMove)
                    {
                        if (dstTy == Type_D || dstTy == Type_UD)
                        {
                            changeType(inst, Type_F);
                        }
                        else if (dstTy == Type_W || dstTy == Type_UW)
                        {
                            changeType(inst, Type_HF);
                        }
                    }
                }
            }
        }
    }
}

static bool isDeadInst(FlowGraph& fg, G4_INST* Inst)
{
    if (Inst->isMov() || Inst->isLogic() || Inst->isCompare() ||
        Inst->isArithmetic() || Inst->isVector()) {

        // Check side-effects.
        // - no global
        // - no indirect
        // - not physically assigned (including ARF)
        auto checkOpnd = [&](G4_Operand *Opnd) {
            if (Opnd == nullptr || Opnd->isNullReg())
                return true;
            if (fg.globalOpndHT.isOpndGlobal(Opnd))
                return false;
            if (Opnd->isDstRegRegion() && Opnd->asDstRegRegion()->isIndirect())
                return false;
            G4_VarBase *Base = Opnd->getBase();
            if (!Base->isRegVar())
                return false;
            if (Base->asRegVar()->isPhyRegAssigned())
                return false;
            return true;
        };

        // Should have no use.
        if (Inst->use_size() > 0)
            return false;

        // Skip instructions with special attributes.
        if (Inst->isYieldInst() || Inst->isBreakPointInst())
            return false;

        // Check defs. Assuming acc operands are all locally defined
        // and def-use are correctly maintained.
        if (!checkOpnd(Inst->getDst()) ||
            !checkOpnd(Inst->getCondMod()))
            return false;

        // At this point, this instruction is dead.
        return true;
    }

    // By default it is not dead.
    return false;
}

void Optimizer::dce()
{
    for (auto bb : fg) {
        for (auto I = bb->rbegin(), E = bb->rend(); I != E; ++I) {
            G4_INST* Inst = *I;
            if (isDeadInst(fg, Inst)) {
                Inst->removeAllDefs();
                Inst->markDead();
            }
        }
        bb->erase(
            std::remove_if(bb->begin(), bb->end(), [](G4_INST* Inst) { return Inst->isDead(); }),
            bb->end());
    }
}

static bool retires(G4_Operand* Opnd, G4_INST* SI)
{
    assert(Opnd && Opnd->isGreg());
    unsigned LB = Opnd->getLinearizedStart() / numEltPerGRF<Type_UB>();
    unsigned RB = Opnd->getLinearizedEnd() / numEltPerGRF<Type_UB>();

    auto overlaps = [=](G4_Operand* A) {
        if (A == nullptr || A->isNullReg() || !A->isGreg())
            return false;
        unsigned LB1 = A->getLinearizedStart() / numEltPerGRF<Type_UB>();
        unsigned RB1 = A->getLinearizedEnd() / numEltPerGRF<Type_UB>();
        return (RB >= LB1 && RB1 >= LB);
    };

    // RAW or WAW
    if (overlaps(SI->getDst()))
        return true;

    if (Opnd->isSrcRegRegion())
        return false;

    // WAR.
    if (overlaps(SI->getSrc(0)))
        return true;
    if (SI->isSplitSend() && overlaps(SI->getSrc(1)))
        return true;

    // Do not retire this send.
    return false;
}

// Emit a self-move to retire this send.
static G4_INST* emitRetiringMov(IR_Builder& builder, G4_BB* BB, G4_INST* SI,
                                INST_LIST_ITER InsertBefore)
{
    assert(SI && SI->isSend());
    G4_Operand* Src0 = SI->getSrc(0);

    unsigned RegNum = Src0->getLinearizedStart() / numEltPerGRF<Type_UB>();
    G4_Declare* Dcl = builder.createTempVar(16, Type_F, Any);
    Dcl->setGRFBaseOffset(RegNum * numEltPerGRF<Type_UB>());
    Dcl->getRegVar()->setPhyReg(builder.phyregpool.getGreg(RegNum), 0);

    G4_DstRegRegion* MovDst = builder.createDst(Dcl->getRegVar(), 0, 0, 1, Type_F);
    G4_SrcRegRegion* MovSrc = builder.createSrc(Dcl->getRegVar(), 0, 0,
        builder.getRegionStride1(), Type_F);
    G4_INST* MovInst = builder.createMov(g4::SIMD8, MovDst, MovSrc, InstOpt_M0 | InstOpt_WriteEnable, false);
    BB->insertBefore(InsertBefore, MovInst);
    return MovInst;
}

// Use this instruction to retire live sends.
static void retireSends(std::vector<G4_INST*>& LiveSends, G4_INST* Inst)
{
    if (LiveSends.empty())
        return;

    // Predicated instructions may not retire a send.
    if (Inst->getPredicate() != nullptr && Inst->opcode() != G4_sel)
        return;

    // Collect operands for dependency checking.
    std::vector<G4_Operand*> Opnds;
    if (G4_DstRegRegion* Dst = Inst->getDst()) {
        if (!Dst->isNullReg() && !Dst->isIndirect() && Dst->isGreg())
            Opnds.push_back(Dst);
    }
    for (int i = 0; i < Inst->getNumSrc(); ++i) {
        G4_Operand* Opnd = Inst->getSrc(i);
        if (Opnd == nullptr || !Opnd->isSrcRegRegion() || Opnd->isNullReg())
            continue;
        G4_SrcRegRegion* Src = Opnd->asSrcRegRegion();
        if (!Src->isIndirect() && Src->isGreg())
            Opnds.push_back(Opnd);
    }

    // WRA, RAW or WAW dependency retires a live send.
    bool Changed = false;
    for (auto Opnd : Opnds) {
        for (auto& SI : LiveSends) {
            if (SI && retires(Opnd, SI)) {
                SI = nullptr;
                Changed = true;
            }
        }
    }
    // Remove nullptr values when there are changes.
    if (Changed) {
        auto Iter = std::remove(LiveSends.begin(), LiveSends.end(), (G4_INST*)nullptr);
        LiveSends.erase(Iter, LiveSends.end());
    }
}

// Limit the number of live sends and clear all sends at the end of a block.
void Optimizer::clearSendDependencies()
{
    for (auto BB : fg) {
        // Live send instructions. This vector will only have MAX_SENDS
        // or less instructions.
        const unsigned MAX_SENDS = 3;
        std::vector<G4_INST*> LiveSends;

        for (auto I = BB->begin(); I != BB->end(); /*empty*/) {
            auto CurI = I++;
            G4_INST* Inst = *CurI;

            // Try to retire live sends.
            retireSends(LiveSends, Inst);
            if (!Inst->isSend())
                continue;

            // This is a send.
            if (LiveSends.size() >= MAX_SENDS) {
                // OK, too many live sends. Retire the earliest live send.
                G4_INST* SI = LiveSends.front();
                G4_INST* MovInst = emitRetiringMov(builder, BB, SI, CurI);
                retireSends(LiveSends, MovInst);
                assert(LiveSends.size() < MAX_SENDS);
            }

            // If this is EOT and send queue is not full, then nothing to do.
            // Otherwise a new send becomes live.
            if (Inst->isEOT())
                LiveSends.clear();
            else
                LiveSends.push_back(Inst);
        }

        // Retire remainig live sends in this block, if any.
        for (auto SI : LiveSends) {
            assert(SI && SI->isSend());
            auto InsertBefore = BB->end();
            G4_INST* LastInst = BB->back();
            if (LastInst->isFlowControl())
                InsertBefore = std::prev(InsertBefore);
            emitRetiringMov(builder, BB, SI, InsertBefore);
        }
    }
}

// [HW WA]
// Fused Mask cannot change from 01 to 00, therefore, EU will go through
// insts that it should skip, causing incorrect result if NoMask instructions
// that has side-effect (send, or modifying globals, etc) are executed.
// A WA is to change any NoMask instruction by adding a predicate to it.
// And the predicated is equivalent to NoMask. For example, the following
// instruction
//
//    (W) add (8|M0)  r10.0<1>:d  r11.0<1;1,0>:d  r12.0<1;1,0>:d
//
//  will be changed to
//
//    (W)  mov (1|M0) f0.0<1>:w  0
//         cmp (8|M0) (eq)f0.0 r0:uw  r0:uw
//    (W&f0.0.any8h) add (8|M0)  r10.0<1>:d  r11.0<1;1,0>:d  r12.0<1;1,0>:d
//
//  Note that f0.0 is called "emask flag".
//
// Even with this HW bug, the HW still have the correct CE mask so that the
// above mov&cmp sequence still works, that is, f0.0 will be all zero if no
// active lanes and will not be zero if there is at least one active lane.
//
// Nested Divergence
//   For a fused mask to be 01,  the control-flow must be divergent
//   at that point. Furthermore, changing 01 to 00 happens only if a further
//   divergence happens within a already-divergent path. This further
//   divergence is called nested divergence here.
//
//   As changing from 01 to 00 never happens with backward goto, backward
//   goto is treated as divergent, but not nested divergent for the purpose
//   of this WA.
//
// This function first finds out which BB are in nested divergent branch and
// then add predicates to those NoMask instructions.
//
void Optimizer::doNoMaskWA()
{
    std::unordered_map<G4_BB*, int> nestedDivergentBBs;

    // Identify BBs that need WA
    fg.reassignBlockIDs();
    fg.findNestedDivergentBBs(nestedDivergentBBs);

    std::vector<INST_LIST_ITER> NoMaskCandidates;
    G4_ExecSize simdsize = fg.getKernel()->getSimdSize();

    // When using cmp to generate emask, the default is to create
    // all-one flag so it applies to all execution size and quarter
    // control combination. Doing so needs 3 instructions for each BB.
    // On the other hand, if anyh can be used, 2 insts would be needed
    // so that we save 1 insts for each BB. The condition that anyh
    // can be used is that M0 is used for all NoMask insts that needs
    // WA and all its execsize is no larger than simdsize.
    bool enableAnyh = true; // try to use anyh if possible
    bool  useAnyh = enableAnyh;  // Set for each BB/inst.

    auto getPredCtrl = [&](bool isUseAnyh) -> G4_Predicate_Control
    {
        if (isUseAnyh)
        {
            return simdsize == 8 ? PRED_ANY8H
                                 : (simdsize == 16 ? PRED_ANY16H : PRED_ANY32H);
        }
        return PRED_DEFAULT;
    };

    // Return condMod if a flag register is used. Since sel
    // does not update flag register, return null for sel.
    auto getFlagModifier = [](G4_INST* I) -> G4_CondMod* {
        if (I->opcode() == G4_sel) {
            return nullptr;
        }
        return I->getCondMod();
    };

    // Return true if a NoMask inst is either send or global
    auto isCandidateInst = [&](G4_INST* Inst, FlowGraph& cfg) -> bool {
        // pseudo should be gone at this time [skip all pseudo].
        if (!Inst->isWriteEnableInst() ||
            Inst->isCFInst() ||
            Inst->isPseudoLogic() ||
            Inst->isPseudoKill())
        {
            return false;
        }

        G4_DstRegRegion* dst = Inst->getDst();
        G4_CondMod* condmod = getFlagModifier(Inst);
        bool dstGlb = (dst && !dst->isNullReg() && cfg.globalOpndHT.isOpndGlobal(dst));
        bool condmodGlb = (condmod && cfg.globalOpndHT.isOpndGlobal(condmod));
        if (Inst->isSend() || dstGlb || condmodGlb)
        {
            if (Inst->isSend() && Inst->getPredicate() &&
                Inst->getExecSize() > simdsize)
            {
                // fused send, already correctly predicated, skip
                return false;
            }
            return true;
        }
        return false;
    };

    // Use cmp to calculate "emask flag(or flag)" (see comment at entry of this function)
    auto createFlagFromCmp = [&](G4_INST*& flagDefInst,
        uint32_t flagBits, G4_BB* BB, INST_LIST_ITER& II)->G4_RegVar*
    {
        //  Ty  (for flag):  big enough to hold flag for either anyh or all one flag.
        //                   if useAnyh
        //                      Ty = (simdsize > 16 ? UD : UW
        //                   else
        //                      Ty = max(simdsize, flagBits) > 16 ? UD : UW
        //
        //  if useAnyh
        //    I0:               (W) mov (1|M0)  flag:Ty,  0
        //    flagDefInst:          cmp (simdsize|M0) (eq)flag  r0:uw  r0:uw
        //  else
        //    I0:               (W) mov (1|M0)  flag:Ty,  0
        //    I1:                   cmp (simdsize|M0) (eq)flag  r0:uw  r0:uw
        //    flagDefInst: (W&flag.anyh) mov flag:Ty 0xFFFFFFFF:Ty
        //
        G4_Type Ty = (simdsize > 16) ? Type_UD : Type_UW;
        if (!useAnyh && flagBits == 32 && flagBits > simdsize)
        {
            Ty = Type_UD;
        }
        G4_Declare* flagDecl = builder.createTempFlag((Ty == Type_UW ? 1 : 2), "cmpFlag");
        G4_RegVar* flagVar = flagDecl->getRegVar();
        G4_DstRegRegion* flag = builder.createDst(flagVar, 0, 0, 1, Ty);
        G4_INST* I0 = builder.createMov(g4::SIMD1, flag,
            builder.createImm(0, Ty), InstOpt_WriteEnable, false);
        BB->insertBefore(II, I0);

        G4_SrcRegRegion* r0_0 = builder.createSrc(
            builder.getRealR0()->getRegVar(), 0, 0,
            builder.getRegionScalar(), Type_UW);
        G4_SrcRegRegion* r0_1 = builder.createSrc(
            builder.getRealR0()->getRegVar(), 0, 0,
            builder.getRegionScalar(), Type_UW);
        G4_CondMod* flagCM = builder.createCondMod(Mod_e, flagVar, 0);
        G4_DstRegRegion* nullDst = builder.createNullDst(Type_UW);
        G4_INST* I1 = builder.createInternalInst(
            NULL, G4_cmp, flagCM, g4::NOSAT, simdsize,
            nullDst, r0_0, r0_1, InstOpt_M0);
        BB->insertBefore(II, I1);

        if (useAnyh)
        {
            flagDefInst = I1;
            return flagVar;
        }

        G4_Imm* allone = builder.createImm(0xFFFFFFFF, Ty);
        G4_DstRegRegion* tFlag = builder.createDst(flagVar, 0, 0, 1, Ty);
        flagDefInst = builder.createMov(g4::SIMD1, tFlag, allone, InstOpt_WriteEnable, false);
        G4_Predicate* tP = builder.createPredicate(
            PredState_Plus, flagVar, 0,
            (simdsize == 8 ? PRED_ANY8H : (simdsize == 16 ? PRED_ANY16H : PRED_ANY32H)));
        flagDefInst->setPredicate(tP);
        tP->setSameAsNoMask(true);
        BB->insertBefore(II, flagDefInst);

        // update DefUse
        flagDefInst->addDefUse(I0, Opnd_pred);
        flagDefInst->addDefUse(I1, Opnd_pred);
        return flagVar;
    };

    // flagVar : emask flag for this BB:
    // currII:  iter to I
    //   positive predicate:
    //        I :  (W&P) <inst> (8|M0) ...
    //      to:
    //        I0:  (W&flagVar) sel (1|M0) tP P  0
    //        I :  (W&tP) <inst> (8|M0) ...
    //
    //   negative predicate:
    //        I :  (W&-P) <inst> (8|M0) ...
    //      to:
    //        I0:  (W&flagVar) sel  (1|M0) tP  P  0xFF
    //        I :  (W&-tP) <inst> (8|M0) ...
    //
    // where the predCtrl of tP at 'I' shall be the same as the original
    // predCtrl /(anyh, anyv, etc) of P at 'I'
    //
    auto doPredicateInstWA = [&](
        G4_INST* flagVarDefInst,  // inst that defines flagVar
        G4_RegVar* flagVar,
        G4_BB* currBB,
        INST_LIST_ITER& currII) -> void
    {
        G4_INST* I = *currII;
        G4_Predicate* P = I->getPredicate();
        assert((P && !I->getCondMod()) && "ICE: expect predicate and no flagModifier!");

        uint32_t flagBits = P->getRightBound() + 1;
        assert((16 * flagVar->getDeclare()->getRootDeclare()->getWordSize()) >= flagBits &&
            "ICE[vISA]: WA's flagVar should not be smaller!");

        G4_Type Ty = (flagBits > 16) ? Type_UD : Type_UW;
        G4_Declare* tPDecl = builder.createTempFlag(
            (Ty == Type_UD) ? 2 : 1, "tFlag");
        G4_RegVar* tPVar = tPDecl->getRegVar();
        G4_SrcRegRegion* Src0 = builder.createSrc(
            P->getTopDcl()->getRegVar(),
            0, 0, builder.getRegionScalar(), Ty);
        G4_Imm* Src1;
        if (P->getState() == PredState_Plus) {
            Src1 = builder.createImm(0, Ty);
        }
        else {
            Src1 = builder.createImm(I->getExecLaneMask(), Ty);
        }
        G4_DstRegRegion* tDst = builder.createDst(tPVar, 0, 0, 1, Ty);
        G4_Predicate* flag0 = builder.createPredicate(
            PredState_Plus, flagVar, 0, getPredCtrl(useAnyh));
        G4_INST* I0 = builder.createInternalInst(
            flag0, G4_sel, nullptr, g4::NOSAT, g4::SIMD1,
            tDst, Src0, Src1, InstOpt_WriteEnable);
        currBB->insertBefore(currII, I0);
        flag0->setSameAsNoMask(true);

        flagVarDefInst->addDefUse(I0, Opnd_pred);
        if (!fg.globalOpndHT.isOpndGlobal(P)) {
            I->transferDef(I0, Opnd_pred, Opnd_src0);
        }

        G4_Predicate* tP = builder.createPredicate(P->getState(), tPVar, 0, P->getControl());
        I->setPredicate(tP);

        // update defUse
        I0->addDefUse(I, Opnd_pred);
    };

    // flagVar : emask for this BB
    //    Note that sel does not update flag register. When condMod is
    //    used, predicate is not allowed.
    // Before:
    //     I:  (W) sel.ge.f0.0  (1|M0)   r10.0<1>:f  r20.0<0;1,0>:f  0:f
    // After
    //     I:  (W) sel.ge.f0.0  (1|M0)  t:f  r20.0<0;1,0>:f   0:f
    //     I0: (W&flagVar) mov (1|M0)  r10.0<1>:f t:f
    //
    auto doFlagModifierSelInstWA = [&](
        G4_INST* flagVarDefInst,  // inst that defines flagVar
        G4_RegVar* flagVar,
        G4_BB* currBB,
        INST_LIST_ITER& currII) -> void
    {
        G4_INST* I = *currII;
        G4_CondMod* P = I->getCondMod();
        assert((P && !I->getPredicate()) && "ICE [sel]: expect flagModifier and no predicate!");

        G4_DstRegRegion* dst = I->getDst();
        assert((dst && !dst->isNullReg()) && "ICE: expect dst to be non-null!");

        // Create a temp that's big enough to hold data and possible gap
        // b/w data due to alignment/hw restriction.
        G4_Declare* saveDecl = builder.createTempVar(
            I->getExecSize() * dst->getHorzStride(), dst->getType(), Any, "saveTmp");

        G4_DstRegRegion* tDst = builder.createDst(
            saveDecl->getRegVar(), 0, 0, dst->getHorzStride(), dst->getType());
        I->setDest(tDst);

        const RegionDesc* regionSave;
        if (I->getExecSize() == g4::SIMD1) {
            regionSave = builder.getRegionScalar();
        }
        else {
            switch (dst->getHorzStride())
            {
            case 1: regionSave = builder.getRegionStride1(); break;
            case 2: regionSave = builder.getRegionStride2(); break;
            case 4: regionSave = builder.getRegionStride4(); break;
            default:
                assert(false && "ICE: unsupported dst horz stride!");
            }
        }

        G4_SrcRegRegion* tSrc = builder.createSrc(
            saveDecl->getRegVar(), 0, 0, regionSave, dst->getType());
        G4_INST* I0 = builder.createMov(
            I->getExecSize(), dst, tSrc, InstOpt_WriteEnable, false);
        G4_Predicate* flag0 = builder.createPredicate(
            PredState_Plus, flagVar, 0, getPredCtrl(useAnyh));
        I0->setPredicate(flag0);
        flag0->setSameAsNoMask(true);

        auto nextII = currII;
        ++nextII;
        currBB->insertBefore(nextII, I0);

        flagVarDefInst->addDefUse(I0, Opnd_pred);
        // As I's dst must be global (otherwise, WA is not needed),
        // no need to do "I->transferUse(I0)".
        I->addDefUse(I0, Opnd_src0);
    };

    //  Non-predicated inst with flagModifier.
    //    flagVar : emask for this BB.
    //    Note that if 32-bit flag is used, flagVar and this instruction I's condMod
    //    take two flag registers, leaving no flag for temporary. In this case, we
    //    will do manual spill, ie,  save and restore the original flag (case 1 and 3).
    //
    //    Before:
    //       I:  (W)  cmp (16|M16) (ne)P  D ....   // 32-bit flag
    //         or
    //           (W)  cmp (16|M0)  (ne)P  D ....   // 16-bit flag
    //
    //    After:
    //      (1) D = null (common)
    //           I0: (W)           mov (1|M0) save:ud  P<0;1,0>:ud
    //           I:  (W)           cmp (16|M16) (ne)P  ....
    //           I1: (W&-flagVar)  mov (1|M0)  P   save:ud
    //      (2) 'I' uses 16-bit flag (common)
    //           I0: (W)           mov  (1)  nP<1>:uw    flagVar.0<0;1,0>:uw
    //           I:  (W&nP)        cmp (16|M0) (ne)nP  ....
    //           I1: (W&flagVar)   mov (1|M0)  P<1>:uw  nP<0;1,0>:uw
    //      (3) otherwise(less common)
    //           I0: (W)           mov (1|M0) save:ud  P<0;1,0>:ud
    //           I1: (W)           mov (16|M16) (nz)P  null flagVar
    //           I:  (W&P)         cmp (16|M16) (ne)P  D ...
    //           I2: (W&-flagVar)  mov (1|M0)  P   save:ud
    //
    auto doFlagModifierInstWA = [&](
        G4_INST* flagVarDefInst,  // inst that defines flagVar
        G4_RegVar* flagVar,
        G4_BB* currBB,
        INST_LIST_ITER& currII) -> void
    {
        G4_INST* I = *currII;
        G4_CondMod* P = I->getCondMod();
        assert((P && !I->getPredicate()) && "ICE: expect flagModifier and no predicate!");

        if (I->opcode() == G4_sel)
        {
            // Special handling of sel inst
            doFlagModifierSelInstWA(flagVarDefInst, flagVar, currBB, currII);
            return;
        }

        bool condModGlb = fg.globalOpndHT.isOpndGlobal(P);
        G4_Declare* modDcl = P->getTopDcl();
        G4_Type Ty = (modDcl->getWordSize() > 1) ? Type_UD : Type_UW;
        if (I->hasNULLDst())
        {   // case 1
            G4_Declare* saveDecl = builder.createTempVar(1, Ty, Any, "saveTmp");
            G4_RegVar* saveVar = saveDecl->getRegVar();
            G4_SrcRegRegion* I0S0 = builder.createSrc(
                modDcl->getRegVar(),
                0, 0, builder.getRegionScalar(), Ty);
            G4_DstRegRegion* D0 = builder.createDst(saveVar, 0, 0, 1, Ty);
            G4_INST* I0 = builder.createMov(g4::SIMD1, D0, I0S0, InstOpt_WriteEnable, false);
            currBB->insertBefore(currII, I0);

            auto nextII = currII;
            ++nextII;
            G4_SrcRegRegion* I1S0 = builder.createSrc(saveVar,
                0, 0, builder.getRegionScalar(), Ty);
            G4_DstRegRegion* D1 = builder.createDst(
                modDcl->getRegVar(), 0, 0, 1, Ty);
            G4_INST* I1 = builder.createMov(g4::SIMD1, D1, I1S0, InstOpt_WriteEnable, false);
            G4_Predicate* flag = builder.createPredicate(
                PredState_Minus, flagVar, 0, getPredCtrl(useAnyh));
            I1->setPredicate(flag);
            currBB->insertBefore(nextII, I1);

            flagVarDefInst->addDefUse(I1, Opnd_pred);
            I0->addDefUse(I1, Opnd_src0);

            if (!condModGlb)
            {
                // Copy condMod uses to I1.
                I->copyUsesTo(I1, false);
            }
            return;
        }

        if (I->getExecSize() == g4::SIMD16 && Ty == Type_UW)
        {   // case 2
            G4_Declare* nPDecl = builder.createTempFlag(1, "nP");
            G4_RegVar* nPVar = nPDecl->getRegVar();
            G4_SrcRegRegion* I0S0 = builder.createSrc(flagVar,
                0, 0, builder.getRegionScalar(), Ty);
            G4_DstRegRegion* D0 = builder.createDst(nPVar, 0, 0, 1, Ty);
            G4_INST* I0 = builder.createMov(g4::SIMD1, D0, I0S0, InstOpt_WriteEnable, false);
            currBB->insertBefore(currII, I0);

            // Use the new flag
            // Note that if useAny is true, nP should use anyh
            G4_Predicate* nP = builder.createPredicate(
                PredState_Plus, nPVar, 0, getPredCtrl(useAnyh));
            G4_CondMod* nM = builder.createCondMod(P->getMod(), nPVar, 0);
            I->setPredicate(nP);
            nP->setSameAsNoMask(true);
            I->setCondMod(nM);

            G4_SrcRegRegion* I1S0 = builder.createSrc(
                nPVar,
                0, 0, builder.getRegionScalar(), Ty);
            G4_DstRegRegion* D1 = builder.createDst(
                modDcl->getRegVar(), 0, 0, 1, Ty);
            G4_Predicate* flag1 = builder.createPredicate(
                PredState_Plus, flagVar, 0, getPredCtrl(useAnyh));
            G4_INST* I1 = builder.createMov(g4::SIMD1, D1, I1S0, InstOpt_WriteEnable, false);
            I1->setPredicate(flag1);
            flag1->setSameAsNoMask(true);

            auto nextII = currII;
            ++nextII;
            currBB->insertBefore(nextII, I1);

            flagVarDefInst->addDefUse(I0, Opnd_src0);
            flagVarDefInst->addDefUse(I1, Opnd_pred);
            I0->addDefUse(I, Opnd_pred);
            I->addDefUse(I1, Opnd_src0);

            if (!condModGlb)
            {
                // Need to transfer condMod uses to I1 only. Here, use
                // copyUsesTo() to achieve that, which is conservative.
                I->copyUsesTo(I1, false);
            }
            return;
        }

        // case 3
        G4_Declare* saveDecl = builder.createTempVar(1, Ty, Any, "saveTmp");
        G4_RegVar* saveVar = saveDecl->getRegVar();
        G4_SrcRegRegion* I0S0 = builder.createSrc(
            modDcl->getRegVar(),
            0, 0, builder.getRegionScalar(), Ty);
        G4_DstRegRegion* D0 = builder.createDst(
            saveVar, 0, 0, 1, Ty);
        G4_INST* I0 = builder.createMov(g4::SIMD1, D0, I0S0, InstOpt_WriteEnable, false);
        currBB->insertBefore(currII, I0);

        G4_DstRegRegion* D1 = builder.createNullDst(Ty);
        G4_SrcRegRegion* I1S0 = builder.createSrc(flagVar,
            0, 0, builder.getRegionScalar(), Ty);
        G4_INST* I1 = builder.createMov(I->getExecSize(),
            D1, I1S0, (InstOpt_WriteEnable | I->getMaskOption()), false);
        G4_CondMod* nM0 = builder.createCondMod(Mod_nz, modDcl->getRegVar(), 0);
        I1->setCondMod(nM0);
        currBB->insertBefore(currII, I1);

        G4_Predicate* nP = builder.createPredicate(
            PredState_Plus, modDcl->getRegVar(), 0, PRED_DEFAULT);
        I->setPredicate(nP);
        // keep I's condMod unchanged

        auto nextII = currII;
        ++nextII;
        G4_SrcRegRegion* I2S0 = builder.createSrc(saveVar,
            0, 0, builder.getRegionScalar(), Ty);
        G4_DstRegRegion* D2 = builder.createDst(
            modDcl->getRegVar(), 0, 0, 1, Ty);
        G4_INST* I2 = builder.createMov(g4::SIMD1, D2, I2S0, InstOpt_WriteEnable, false);
        G4_Predicate* flag2 = builder.createPredicate(
            PredState_Minus, flagVar, 0, getPredCtrl(useAnyh));
        I2->setPredicate(flag2);
        currBB->insertBefore(nextII, I2);

        flagVarDefInst->addDefUse(I1, Opnd_src0);
        flagVarDefInst->addDefUse(I2, Opnd_pred);
        I0->addDefUse(I2, Opnd_src0);

        if (!condModGlb)
        {
            I1->addDefUse(I, Opnd_pred);
            // Need to copy uses of I's condMod to I2 only, here
            // conservatively use copyUsesTo().
            I->copyUsesTo(I2, false);
        }
    };

    //  Predicated inst with flagModifier.
    //  flagVar : emask for this BB:
    //
    //    Before:
    //       I:  (W&[-]P)  and (16|M0) (ne)P  ....
    //
    //    After:
    //       I0: (W)           mov (1|M0) save:uw  P
    //       I1: (W&-flagVar)  mov (1|M0) P<1>:uw  0:uw | 0xFFFF (for -P)
    //       I:  (W&P)         and (16|M0) (ne)P  ....
    //       I2: (W&-flagVar)  mov (1|M0)  P   save:uw
    //
    auto doPredicateAndFlagModifierInstWA = [&](
        G4_INST* flagVarDefInst,  // inst that defines flagVar
        G4_RegVar* flagVar,
        G4_BB* currBB,
        INST_LIST_ITER& currII) -> void
    {
        G4_INST* I = *currII;
        G4_Predicate* P = I->getPredicate();
        G4_CondMod* M = I->getCondMod();
        assert((P && M) && "ICE: expect both predicate and flagModifier!");
        assert(P->getTopDcl() == M->getTopDcl() &&
            "ICE: both predicate and flagMod must be the same flag!");

        bool condModGlb = fg.globalOpndHT.isOpndGlobal(M);

        G4_Declare* modDcl = M->getTopDcl();
        G4_Type Ty = (modDcl->getWordSize() > 1) ? Type_UD : Type_UW;

        G4_Declare* saveDecl = builder.createTempVar(1, Ty, Any, "saveTmp");
        G4_RegVar* saveVar = saveDecl->getRegVar();
        G4_SrcRegRegion* I0S0 = builder.createSrc(modDcl->getRegVar(),
            0, 0, builder.getRegionScalar(), Ty);
        G4_DstRegRegion* D0 = builder.createDst(
            saveVar, 0, 0, 1, Ty);
        G4_INST* I0 = builder.createMov(g4::SIMD1, D0, I0S0, InstOpt_WriteEnable, false);
        currBB->insertBefore(currII, I0);

        G4_DstRegRegion* D1 = builder.createDst(
            modDcl->getRegVar(), 0, 0, 1, Ty);
        G4_Imm* immS0;
        if (P->getState() == PredState_Plus) {
            immS0 = builder.createImm(0, Ty);
        }
        else {
            immS0 = builder.createImm(I->getExecLaneMask(), Ty);
        }
        G4_INST* I1 = builder.createMov(g4::SIMD1, D1, immS0, InstOpt_WriteEnable, false);
        G4_Predicate* flag1 = builder.createPredicate(
            PredState_Minus, flagVar, 0, getPredCtrl(useAnyh));
        I1->setPredicate(flag1);
        currBB->insertBefore(currII, I1);

        // No change to I

        auto nextII = currII;
        ++nextII;
        G4_SrcRegRegion* I2S0 = builder.createSrc(saveVar,
            0, 0, builder.getRegionScalar(), Ty);
        G4_DstRegRegion* D2 = builder.createDst(
            modDcl->getRegVar(), 0, 0, 1, Ty);
        G4_INST* I2 = builder.createMov(g4::SIMD1, D2, I2S0, InstOpt_WriteEnable, false);
        G4_Predicate* flag2 = builder.createPredicate(
            PredState_Minus, flagVar, 0, getPredCtrl(useAnyh));
        I2->setPredicate(flag2);
        currBB->insertBefore(nextII, I2);

        flagVarDefInst->addDefUse(I1, Opnd_pred);
        flagVarDefInst->addDefUse(I2, Opnd_pred);
        I0->addDefUse(I2, Opnd_src0);

        if (!condModGlb)
        {
            I1->addDefUse(I, Opnd_pred);
            // Need to copy uses of I's condMod to I2 only, here
            // conservatively use copyUsesTo().
            I->copyUsesTo(I2, false);
        }
    };

    // Scan all insts and apply WA on NoMask candidate insts
    for (auto BI : fg)
    {
        G4_BB* BB = BI;
        if (nestedDivergentBBs.count(BB) == 0)
        {
            continue;
        }
        if (((builder.getuint32Option(vISA_noMaskWA) & 0x3) >= 2) &&
            nestedDivergentBBs[BB] < 2)
        {
            continue;
        }

        if ((builder.getuint32Option(vISA_noMaskWA) & 0x4) != 0)
        {
            // simple flag insertion: every time a flag is needed, calcalate it.
            for (auto II = BB->begin(), IE = BB->end(); II != IE; ++II)
            {
                G4_INST* I = *II;
                if (!isCandidateInst(I, fg))
                {
                    continue;
                }

                uint32_t flagbits = 16;
                if ((I->getExecSize() + I->getMaskOffset()) > 16)
                {
                    flagbits = 32;
                }
                useAnyh = enableAnyh;
                if (enableAnyh)
                {
                    if (I->getExecSize() > simdsize || I->getMaskOffset() != 0)
                    {
                        useAnyh = false;
                    }
                }

                G4_INST* flagDefInst = nullptr;
                G4_RegVar* flagVar = createFlagFromCmp(flagDefInst, flagbits, BB, II);

                G4_Predicate* pred = I->getPredicate();
                G4_CondMod* condmod = I->getCondMod();
                if (!condmod && !pred)
                {
                    // case 1: no predicate, no flagModifier (common case)
                    G4_Predicate* newPred = builder.createPredicate(
                        PredState_Plus, flagVar, 0, getPredCtrl(useAnyh));
                    newPred->setSameAsNoMask(true);
                    I->setPredicate(newPred);

                    // update defUse
                    flagDefInst->addDefUse(I, Opnd_pred);
                }
                else if (pred && !condmod)
                {
                    // case 2: has predicate, no flagModifier
                    doPredicateInstWA(flagDefInst, flagVar, BB, II);
                }
                else if (!pred && condmod)
                {
                    // case 3: has flagModifier, no predicate
                    doFlagModifierInstWA(flagDefInst, flagVar, BB, II);
                }
                else
                {
                    // case 4: both predicate and flagModifier are present
                    //         (rare or never happen)
                    doPredicateAndFlagModifierInstWA(flagDefInst, flagVar, BB, II);
                }
            }
            continue;
        }

        // For each BB that needs to modify NoMask, create a flag once right
        // prior to the first NoMask inst to be modified like the following:
        //
        //    BB:
        //      Before:
        //             (W&f0.0) inst0 (16|M0) ...
        //             ......
        //             (W&f0.0) inst0 (16|M0) ...
        //      After:
        //         (W) mov (1|M0)           f0.0:uw   0:uw  // or ud
        //             cmp (16|M0) (eq)f0.0 null:uw  r0.0<0;1,0::uw  r0.0<0;1,0>:uw
        //         if (!useAnyh)
        //             (W&f0.0.any16h)  mov (1|M0) f0.0  0xFFFF:uw
        //
        //             (W&f0.0) inst0 (16|M0) ...
        //             ......
        //             (W&f0.0) inst0 (16|M0) ...
        //         else
        //             (W&f0.0.any16h) inst0 (16|M0) ...
        //             ......
        //             (W&f0.0.any16h) inst0 (16|M0) ...
        //

        //  1. Collect all candidates and check if 32 bit flag is needed
        //     and if useAnyh can be set to true.
        bool need32BitFlag = false;
        useAnyh = enableAnyh; // need to reset for each BB
        for (auto II = BB->begin(), IE = BB->end(); II != IE; ++II)
        {
            G4_INST* I = *II;
            if (isCandidateInst(I, fg))
            {
                NoMaskCandidates.push_back(II);
                if ((I->getExecSize() + I->getMaskOffset()) > 16)
                {
                    need32BitFlag = true;
                }
                if (enableAnyh)
                {
                    if (I->getExecSize() > simdsize || I->getMaskOffset() != 0)
                    {
                        useAnyh = false;
                    }
                }
            }
        }
        if (NoMaskCandidates.empty())
        {
            continue;
        }

        // 2. Do initialization for per-BB flag.
        INST_LIST_ITER& II0 = NoMaskCandidates[0];

        G4_INST* flagDefInst = nullptr;
        uint32_t flagBits = need32BitFlag ? 32 : 16;
        G4_RegVar* flagVarForBB = createFlagFromCmp(flagDefInst, flagBits, BB, II0);

        // 3. Do WA by adding predicate to each candidate
        for (int i = 0, sz = (int)NoMaskCandidates.size(); i < sz; ++i)
        {
            INST_LIST_ITER& II = NoMaskCandidates[i];
            G4_INST* I = *II;

            G4_CondMod* condmod = I->getCondMod();
            G4_Predicate* pred = I->getPredicate();
            if (!condmod && !pred)
            {
                // case 1: no predicate, no flagModifier (common case)
                G4_Predicate* newPred = builder.createPredicate(
                    PredState_Plus, flagVarForBB, 0, getPredCtrl(useAnyh));
                newPred->setSameAsNoMask(true);
                I->setPredicate(newPred);

                // update defUse
                flagDefInst->addDefUse(I, Opnd_pred);
            }
            else if (pred && !condmod)
            {
                // case 2: has predicate, no flagModifier
                doPredicateInstWA(flagDefInst, flagVarForBB, BB, II);
            }
            else if (!pred && condmod)
            {
                // case 3: has flagModifier, no predicate
                doFlagModifierInstWA(flagDefInst, flagVarForBB, BB, II);
            }
            else
            {
                // case 4: both predicate and flagModifier are present
                //         (rare or never happen)
                doPredicateAndFlagModifierInstWA(flagDefInst, flagVarForBB, BB, II);
            }
        }
        // Clear it to prepare for the next BB
        NoMaskCandidates.clear();
    }
}

// Convert vISA MULH dst:d src0:d src1:d into
//    mul acc0.0<1>:d src0:d src1:w
//    mach dst:d src0:d src1:d
// convert vISA mul dst:d src0:d src1:d into
//    mul acc0.0<1>:d src0:d src1:w
//    macl dst:d src0:d src1:d
void Optimizer::expandMulPostSchedule()
{
    if (!builder.noMulExpandingBeforeScheduler())
    {
        return;
    }

    for (auto bb : kernel.fg)
    {
        for (INST_LIST_ITER it = bb->begin(); it != bb->end(); it++)
        {
            G4_INST* inst = *it;
            if (inst->opcode() != G4_mul && inst->opcode() != G4_mulh)
            {
                continue;
            }

            G4_Operand* src0 = inst->getSrc(0);
            G4_Operand* src1 = inst->getSrc(1);
            G4_DstRegRegion* dst = inst->getDst();

            if (dst->isAccReg())
            {
                continue;
            }

            if (!IS_DTYPE(src0->getType()) || !IS_DTYPE(src1->getType()) || !IS_DTYPE(dst->getType()))
            {
                continue;
            }

            MUST_BE_TRUE(inst->getSaturate() == g4::NOSAT, "NOSAT is expected in mul expanding");
            MUST_BE_TRUE(!src0->isSrcRegRegion() || src0->asSrcRegRegion()->getModifier() == Mod_src_undef, "no src0 modifier is expected in mul expanding");
            MUST_BE_TRUE(!src1->isSrcRegRegion() || src1->asSrcRegRegion()->getModifier() == Mod_src_undef, "no src1 modifier is expected in mul expanding");

            uint32_t origOptions = inst->getOption();
            inst->setOptionOn(InstOpt_WriteEnable);

            G4_Predicate* predicate = inst->getPredicate();
            if (predicate != nullptr)
            {
                // move pred to mach/macl
                inst->setPredicate(nullptr);
            }

            if (inst->getCondMod() != nullptr)
            {
                // conditional modifier cannot be used
                // when the MUL source operand is of dword type.
                MUST_BE_TRUE(false, "Dw multiply does not support conditional modifiers");
                inst->setCondMod(nullptr);
            }

            // Change dst of MUL to acc0
            G4_Type accType = (IS_UNSIGNED_INT(src0->getType()) && IS_UNSIGNED_INT(src1->getType())) ? Type_UD : Type_D;
            G4_DstRegRegion* accDstOpnd = builder.createDst(builder.phyregpool.getAcc0Reg(), 0, 0, 1, accType);
            inst->setDest(accDstOpnd);

            // Change the src1 of MUL from :d to :w
            if (src1->isImm())
            {
                uint64_t truncVal = src1->asImm()->getImm() & 0xFFFF;
                G4_Imm* new_src1 = builder.createImm(truncVal, Type_UW);
                inst->setSrc(new_src1, 1);
            }
            else
            {
                assert(src1->isSrcRegRegion() && "region expected");

                G4_SrcRegRegion* srcRegion = src1->asSrcRegRegion();
                const RegionDesc* rd = srcRegion->getRegion();
                assert(rd->horzStride < 4 && "invalid horz stride");

                unsigned short scale = TypeSize(Type_D) / TypeSize(Type_UW);
                unsigned short newHS = rd->horzStride * scale;
                unsigned short newVS = rd->vertStride * scale;
                const RegionDesc* new_rd = builder.createRegionDesc(newVS, rd->width, newHS);
                short subRegOff = srcRegion->getSubRegOff();
                if (srcRegion->getRegAccess() == Direct)
                {
                    subRegOff *= scale;
                }
                auto new_src1 = builder.createSrcRegRegion(
                    srcRegion->getModifier(), srcRegion->getRegAccess(),
                    srcRegion->getBase(), srcRegion->getRegOff(), subRegOff, new_rd,
                    Type_UW);
                inst->setSrc(new_src1, 1);
                if (srcRegion->getRegAccess() != Direct)
                {
                    new_src1->setImmAddrOff(srcRegion->getAddrImm());
                }
            }

            G4_INST* maclOrMachInst = nullptr;
            if (inst->opcode() == G4_mul)
            {
                // create a macl inst
                maclOrMachInst = builder.createMacl(inst->getExecSize(),
                    dst, builder.duplicateOperand(src0), builder.duplicateOperand(src1), origOptions, accType);
            }
            else
            {
                // create a mach inst
                inst->setOpcode(G4_mul);
                maclOrMachInst = builder.createMach(inst->getExecSize(),
                    dst, builder.duplicateOperand(src0), builder.duplicateOperand(src1), origOptions, accType);
            }

            maclOrMachInst->setPredicate(predicate);

            // maintain du chain as fixAccDst uses it later
            inst->addDefUse(maclOrMachInst, Opnd_implAccSrc);

            // Add mach/macl after mul
            auto maclOrMachInstIt = bb->insertAfter(it, maclOrMachInst);

            // Always add a dummy mov after mach/macl for HW read suppresion W/A
            auto dummyMovSrc = builder.createSrc(dst->getBase(),
                0, 0, builder.getRegionScalar(), Type_D);
            G4_INST* dummyMov = builder.createMov(g4::SIMD1, builder.createNullDst(Type_D),
                dummyMovSrc, InstOpt_WriteEnable, false);
            bb->insertAfter(maclOrMachInstIt, dummyMov);
        }
    }
}
