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


#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <list>

#include "visa_igc_common_header.h"
#include "Common_ISA_util.h"
#include "Common_ISA_framework.h"
#include "JitterDataStruct.h"
#include "BuildIR.h"
#include "common.h"
#include "Timer.h"

using namespace vISA;

DeclarePool::~DeclarePool()
{
    for (unsigned i = 0, size = (unsigned)dcllist.size(); i < size; i++) {
        G4_Declare* dcl = dcllist[i];
        dcl->~G4_Declare();
    }
    dcllist.clear();
}

G4_Declare* DeclarePool::createDeclare(
    const char*    name,
    G4_RegFileKind regFile,
    unsigned short nElems,
    unsigned short nRows,
    G4_Type        ty,
    DeclareType    kind,
    G4_RegVar *    base,
    G4_Operand *   repRegion,
    G4_ExecSize    execSize)
{
    G4_Declare* dcl = new (mem) G4_Declare(name, regFile, nElems * nRows, ty, dcllist);
    G4_RegVar * regVar;
    if (kind == DeclareType::Regular)
        regVar = new (mem) G4_RegVar(dcl, G4_RegVar::RegVarType::Default);
    else if (kind == DeclareType::AddrSpill)
        regVar = new (mem) G4_RegVarAddrSpillLoc(dcl, addrSpillLocCount);
    else if (kind == DeclareType::Tmp)
        regVar = new (mem) G4_RegVarTmp(dcl, base);
    else if (kind == DeclareType::Spill)
        regVar = new (mem) G4_RegVarTransient(dcl, base, repRegion->asDstRegRegion(), execSize, G4_RegVarTransient::TransientType::Spill);
    else if (kind == DeclareType::Fill)
        regVar = new (mem)G4_RegVarTransient(dcl, base, repRegion->asSrcRegRegion(), execSize, G4_RegVarTransient::TransientType::Fill);
    else if (kind == DeclareType::CoalescedFill || kind == DeclareType::CoalescedSpill)
        regVar = new (mem)G4_RegVarCoalesced(dcl, kind == DeclareType::CoalescedFill);
    else
    {
        MUST_BE_TRUE(false, ERROR_INTERNAL_ARGUMENT);
        regVar = NULL;
    }
    dcl->setRegVar(regVar);

    if (regFile == G4_ADDRESS)
    {
        dcl->setSubRegAlign(Any);
    }
    else if (regFile != G4_FLAG)
    {
        if ((unsigned int)nElems * nRows * TypeSize(ty) >= numEltPerGRF<Type_UB>())
        {
            dcl->setSubRegAlign(GRFALIGN);
        }
        else
        {
            // at a minimum subRegAlign has to be at least the type size
            dcl->setSubRegAlign(Get_G4_SubRegAlign_From_Type(ty));
        }
    }
    else
    {
        if (dcl->getNumberFlagElements() == 32)
        {
            dcl->setSubRegAlign(Even_Word);
        }
    }

    return dcl;
}


G4_Declare * IR_Builder::GlobalImmPool::addImmVal(G4_Imm* imm, int numElt)
{
    ImmVal val = { imm, numElt };
    for (int i = 0; i < curSize; ++i)
    {
        if (val == immArray[i])
        {
            return dclArray[i];
        }
    }
    if (curSize == MAX_POOL_SIZE)
    {
        return nullptr;
    }
    immArray[curSize] = val;
    dclArray[curSize] = builder.createTempVar(numElt, imm->getType(), Any);
    return dclArray[curSize++];
}


///////////////////////////////////////////////////////////////////////////////
// IR_Builder functions (except translateXXXX, which should be in VisaToG4)
//

void IR_Builder::dump(std::ostream &os)
{
    os << "DECLARES:\n";
    for (const G4_Declare *dcl : kernel.Declares) {
        dcl->emit(os);
        os  << "\n";
    }
    os << "\n";
    os << "INSTS:\n";
    for (G4_INST *i : instList) {
        i->emit(os, false, false);
        os << "\n";
    }
}


// bind a vISA input variable <dcl> to the GRF byte offset <offset>
void IR_Builder::bindInputDecl(G4_Declare* dcl, int offset)
{    // decide the physical register number and sub register number
    unsigned int regNum = offset / getGRFSize();
    unsigned int subRegNum = (offset % getGRFSize()) / dcl->getElemSize();
    dcl->getRegVar()->setPhyReg(phyregpool.getGreg(regNum), subRegNum);
    dcl->setRegFile(G4_INPUT);
    unsigned int reservedGRFNum = m_options->getuInt32Option(vISA_ReservedGRFNum);
    if (regNum + dcl->getNumRows() > kernel.getNumRegTotal() - reservedGRFNum) {
        MUST_BE_TRUE(false, "INPUT payload execeeds the regsiter number");
    }
}

// check if an operand is aligned to <align_byte>
bool IR_Builder::isOpndAligned(
    G4_Operand *opnd, unsigned short &offset, int align_byte) const
{
    offset = 0;
    bool isAligned = true;

    switch (opnd->getKind())
    {
    case G4_Operand::immediate:
    case G4_Operand::addrExp:
    case G4_Operand::label:
    case G4_Operand::condMod:
    case G4_Operand::predicate:
    {
        isAligned = true;
        break;
    }
    case G4_Operand::srcRegRegion:
    case G4_Operand::dstRegRegion:
    {
        int type_size = opnd->getTypeSize();
        G4_Declare *dcl = NULL;
        if (opnd->getBase()->isRegVar())
        {
            dcl = opnd->getBase()->asRegVar()->getDeclare();
            while (dcl && dcl->getAliasDeclare())
            {
                if (dcl->getSubRegAlign() != Any &&
                    (((dcl->getSubRegAlign() * 2) >= align_byte && (dcl->getSubRegAlign() * 2) % align_byte != 0) ||
                    ((dcl->getSubRegAlign() * 2) < align_byte && align_byte % (dcl->getSubRegAlign() * 2) != 0)))
                {
                    isAligned = false;
                    break;
                }
                offset += (unsigned short) dcl->getAliasOffset();
                dcl = dcl->getAliasDeclare();
            }

            if (dcl && dcl->getRegVar() && dcl->getRegVar()->isPhyRegAssigned())
            {
                offset += static_cast<unsigned short>(dcl->getRegVar()->getByteAddr());
            }
        }
        if (!isAligned)
        {
            return isAligned;
        }

        if (opnd->isDstRegRegion())
        {
            if (opnd->asDstRegRegion()->getRegAccess() != Direct)
            {
                isAligned = false;
            }
            offset += opnd->asDstRegRegion()->getRegOff() * numEltPerGRF<Type_UB>() + opnd->asDstRegRegion()->getSubRegOff() * type_size;
        }
        else if (opnd->isSrcRegRegion())
        {
            if (opnd->asSrcRegRegion()->getRegAccess() != Direct)
            {
                isAligned = false;
            }
            offset += opnd->asSrcRegRegion()->getRegOff() * numEltPerGRF<Type_UB>() + opnd->asSrcRegRegion()->getSubRegOff() * type_size;
        }
        if (offset % align_byte != 0)
        {
            return false;
        }
        // Only alignment of the top dcl can be changed.
        if (dcl && dcl->getRegFile() == G4_GRF)
        {
            if (dcl->getSubRegAlign() == Any ||
                ((dcl->getSubRegAlign() * 2) < align_byte && align_byte % (dcl->getSubRegAlign() * 2) == 0))
            {
                dcl->setSubRegAlign(G4_SubReg_Align(align_byte / 2));
            }
            else if ((dcl->getSubRegAlign() * 2) < align_byte || (dcl->getSubRegAlign() * 2) % align_byte != 0)
            {
                isAligned = false;
            }
        }
        else if (opnd->getKind() == G4_Operand::dstRegRegion &&
            // Only care about GRF or half-GRF alignment.
            (align_byte == numEltPerGRF<Type_UB>() || align_byte == numEltPerGRF<Type_UB>() / 2) &&
            dcl && dcl->getRegFile() == G4_ADDRESS)
        {

            // Get the single definition of the specified operand from the use
            // inst.
            auto getSingleDefInst = [](G4_INST *UI, Gen4_Operand_Number OpndNum)
                -> G4_INST * {
                G4_INST *Def = nullptr;
                for (DEF_EDGE_LIST_ITER I = UI->defInstList.begin(),
                    E = UI->defInstList.end();
                    I != E; ++I) {
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

            G4_INST *inst = opnd->getInst();
            if (inst) {
                // Check address calculation like:
                //
                //    shl (1) V1  V0          imm
                //    add (1) a0  $V2 + off   V1
                //    ...
                //    (use)... r[a0, disp] ...
                //
                // need to check both disp, off, and V1 are aligned.
                //
                // Check acc_use_op's def-list.
                G4_INST *LEA = getSingleDefInst(inst, Opnd_dst);
                if (LEA && LEA->opcode() == G4_add && LEA->getExecSize() == g4::SIMD1) {
                    isAligned = true;
                    G4_Operand *Op0 = LEA->getSrc(0);
                    G4_Operand *Op1 = LEA->getSrc(1);
                    if (Op0->isSrcRegRegion()) {
                        // TODO: Consider MUL as well.
                        G4_INST *Def = getSingleDefInst(LEA, Opnd_src0);
                        if (Def && Def->opcode() == G4_shl &&
                            Def->getSrc(1)->isImm()) {
                            G4_Imm *Imm = Def->getSrc(1)->asImm();
                            unsigned Factor = (1U << Imm->getInt());
                            // TODO: We only perform alignment checking on
                            // component wise and may need to consider checking
                            // the accumulated result.
                            if (Factor % align_byte != 0)
                                isAligned = false;
                        } else if (Def && Def->opcode() == G4_and &&
                            Def->getSrc(1)->isImm()) {
                            G4_Imm *Imm = Def->getSrc(1)->asImm();
                            uint64_t Mask = uint64_t(Imm->getInt());
                            // align_byte could be 32 or 16 guarded previsouly.
                            uint64_t AlignMask = align_byte - 1;
                            if ((Mask & AlignMask) != 0)
                                isAligned = false;
                        } else
                            isAligned = false;
                    }
                    if (isAligned && Op1->isAddrExp()) {
                        G4_AddrExp *AE = Op1->asAddrExp();
                        G4_Declare *Dcl = AE->getRegVar()->getDeclare();
                        unsigned AliasOffset = 0;
                        while (Dcl && Dcl->getAliasDeclare()) {
                            AliasOffset += Dcl->getAliasOffset();
                            Dcl = Dcl->getAliasDeclare();
                        }
                        // TODO: We only perform alignment checking on
                        // component wise and may need to consider checking
                        // the accumulated result.
                        if ((AliasOffset % align_byte) != 0 ||
                            (Dcl && Dcl->getSubRegAlign() != GRFALIGN &&
                                Dcl->getSubRegAlign() != Sixteen_Word &&
                                Dcl->getSubRegAlign() != Eight_Word) ||
                            AE->getOffset() % align_byte != 0) {
                            isAligned = false;
                        }
                    } else
                        isAligned = false;
                    if (isAligned) {
                        // TODO: We only perform alignment checking on
                        // component wise and may need to consider checking
                        // the accumulated result.
                        if (opnd->asDstRegRegion()->getAddrImm() % align_byte != 0)
                            isAligned = false;
                    }
                }
            }
        }
        else if (dcl && dcl->getRegFile() == G4_FLAG)
        {
            // need to make flag even-word aligned if it's used in a setp with dword source
            // ToDo: should we fix input to use 16-bit value instead
            if (align_byte == 4)
            {
                dcl->setSubRegAlign(Even_Word);
            }
        }
        break;
    }
    default:
        break;
    }
    return isAligned;
}


bool IR_Builder::isOpndAligned(G4_Operand* opnd, int alignByte) const
{
    uint16_t offset = 0; // ignored
    return isOpndAligned(opnd, offset, alignByte);
}


void IR_Builder::predefinedVarRegAssignment(uint8_t inputSize)
{
    uint32_t preDefinedStart = ((inputSize + G4_DSIZE - 1) / G4_DSIZE) * G4_DSIZE;
    if (preDefinedStart == 0)
    {
        preDefinedStart = numEltPerGRF<Type_UB>();
    }
    for (PreDefinedVarsInternal i : allPreDefVars)
    {
        if (!predefinedVarNeedGRF(i))
        {
            continue;
        }

        G4_Type ty = GetGenTypeFromVISAType(getPredefinedVarType(i));
        G4_Declare *dcl = preDefVars.getPreDefinedVar((PreDefinedVarsInternal)i);
        if (!isPredefinedVarInR0((PreDefinedVarsInternal)i))
        {
            unsigned short new_offset = preDefinedStart + getPredefinedVarByteOffset(i);
            unsigned int regNum = new_offset / numEltPerGRF<Type_UB>();
            unsigned int subRegNum = (new_offset % numEltPerGRF<Type_UB>()) / TypeSize(ty);
            dcl->getRegVar()->setPhyReg(phyregpool.getGreg(regNum), subRegNum);
        }
        else
        {
            unsigned int regNum = 0;
            unsigned int subRegNum = getPredefinedVarByteOffset(i) / TypeSize(ty);
            dcl->getRegVar()->setPhyReg(phyregpool.getGreg(regNum), subRegNum);
        }
    }
}

// Expand some of the pre-defined variables at kernel entry
// -- replace pre-defined V17 (hw_tid)
// -- replace pre-defined V22 (color)
// -- replace pre-defined V1 (thread_x)
// -- replace pre-defined V2 (thread_y)
void IR_Builder::expandPredefinedVars()
{

    // Use FFTID from msg header
    // and (1) hw_tid, r0.5, 0x3ff
    //
    // 9:0     FFTID. This ID is assigned by TS and is a unique identifier for the thread in
    // comparison to other concurrent root threads. It is used to free up resources used
    // by the thread upon thread completion.
    //
    // [Pre-DevBDW]: Format = U8. Bits 9:8 are Reserved, MBZ.
    //
    // [0:8] For Pre-Gen9
    // [0:9] For Gen10+
    //

    // first non-label instruction
    auto iter = std::find_if(instList.begin(), instList.end(), [](G4_INST* inst) { return !inst->isLabel(); });

    if (preDefVars.isHasPredefined(PreDefinedVarsInternal::HW_TID))
    {
        const unsigned fftid_mask = getPlatform() >= GENX_CNL ? 0x3FF : 0x1FF;
        G4_SrcRegRegion* src = createSrc(realR0->getRegVar(), 0, 5, getRegionScalar(), Type_UD);
        G4_Imm* mask1 = createImm(fftid_mask, Type_UD);
        G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(builtinHWTID, 1);
        G4_INST* inst = createBinOp(G4_and, g4::SIMD1, dst, src, mask1, InstOpt_WriteEnable, false);
        instList.insert(iter, inst);
    }

    if (preDefVars.isHasPredefined(PreDefinedVarsInternal::X))
    {
        if (useNewR0Format())
        {
            // x -> and (1) thread_x<1>:uw r0.1:ud 0xFFF
            G4_SrcRegRegion* r0Dot1UD = createSrc(
                realR0->getRegVar(), 0, 1, getRegionScalar(), Type_UD);
            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::X), 1);
            G4_INST* inst = createBinOp(G4_and, g4::SIMD1, dst, r0Dot1UD,
                createImm(0xFFF, Type_UW), InstOpt_WriteEnable, false);
            instList.insert(iter, inst);
        }
        else
        {
            //  We insert the new instruction
            //  and (1) thread_x<1>:uw, r0.2:uw, 0x01FF
            G4_SrcRegRegion* r0Dot2UW = createSrc(
                realR0->getRegVar(), 0, 2, getRegionScalar(), Type_UW);
            int64_t mask = getThreadIDMask();
            G4_Imm* src1 = createImm(mask, Type_UW);
            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::X), 1);
            G4_INST* inst = createBinOp(G4_and, g4::SIMD1, dst, r0Dot2UW, src1, InstOpt_WriteEnable, false);
            instList.insert(iter, inst);
        }
    }

    if (preDefVars.isHasPredefined(PreDefinedVarsInternal::Y))
    {
        if (useNewR0Format())
        {
            // y -> shr (1) thread_y<1>:uw r0.1:ud 12
            //      and (1) thread_y<1>:uw thread_y:uw 0xFFF
            G4_SrcRegRegion* r0Dot1UD = createSrc(
                realR0->getRegVar(), 0, 1, getRegionScalar(), Type_UD);

            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::Y), 1);
            G4_INST* inst1 = createBinOp(G4_shr, g4::SIMD1, dst, r0Dot1UD,
                createImm(12, Type_UW), InstOpt_WriteEnable, false);
            instList.insert(iter, inst1);
            dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::Y), 1);
            G4_INST* inst2 = createBinOp(G4_and, g4::SIMD1, dst,
                Create_Src_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::Y), getRegionScalar()),
                createImm(0xFFF, Type_UW), InstOpt_WriteEnable, false);
            instList.insert(iter, inst2);
        }
        else
        {
            //  We insert the new instruction
            //  and (1) thread_y<1>:uw, r0.3:uw, 0x01FF
            G4_SrcRegRegion* r0Dot3UW = createSrc(
                realR0->getRegVar(), 0, 3, getRegionScalar(), Type_UW);
            int64_t mask = getThreadIDMask();
            G4_Imm* src1 = createImmWithLowerType(mask, Type_UW);
            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::Y), 1);
            G4_INST* inst = createBinOp(G4_and, g4::SIMD1, dst, r0Dot3UW, src1, InstOpt_WriteEnable, false);
            instList.insert(iter, inst);
        }
    }

    // color bit
    if (preDefVars.isHasPredefined(PreDefinedVarsInternal::COLOR))
    {
        if (useNewR0Format())
        {
            // r0.1[31:24]
            // shr (1) color<2>:uw r0.1<0;1,0>:ud 24
            G4_SrcRegRegion* src = createSrc(realR0->getRegVar(),
                0, 1, getRegionScalar(), Type_UD);
            G4_Imm* shift = createImm(24, Type_UW);
            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::COLOR), 2);
            G4_INST* inst = createBinOp(G4_shr, g4::SIMD1, dst, src, shift,
                InstOpt_WriteEnable, false);
            instList.insert(iter, inst);
        }
        else
        {
            // else: r0.2[3:0]
            // and (1) color<2>:uw r0.2<0;1,0>:ud 0xF
            G4_SrcRegRegion* src = createSrc(realR0->getRegVar(),
                0, 2, getRegionScalar(), Type_UD);
            G4_Imm* mask = createImm(0xF, Type_UW);
            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::COLOR), 2);
            G4_INST* inst = createBinOp(G4_and, g4::SIMD1, dst, src, mask,
                InstOpt_WriteEnable, false);
            instList.insert(iter, inst);
        }
    }
}

FCPatchingInfo* IR_Builder::getFCPatchInfo()
{
    // Create new instance of FC patching class if one is not
    // yet created.
    if (fcPatchInfo == NULL)
    {
        FCPatchingInfo* instance;
        instance = (FCPatchingInfo*)mem.alloc(sizeof(FCPatchingInfo));
        fcPatchInfo = new (instance) FCPatchingInfo();
    }

    return fcPatchInfo;
}

const char* IR_Builder::getNameString(
    Mem_Manager& mem, size_t size, const char* format, ...)
{
#ifdef _DEBUG
    char* name = (char*) mem.alloc(size);
    va_list args;
    va_start(args, format);
    std::vsnprintf(name, size, format, args);
    va_end(args);
    return name;
#else
    return "";
#endif
}

G4_FCALL* IR_Builder::getFcallInfo(const G4_INST* inst) const {
    auto it = m_fcallInfo.find(inst);
    if (m_fcallInfo.end() == it) {
        return nullptr;
    } else {
        return it->second;
    }
}

void IR_Builder::createPreDefinedVars()
{
    for (PreDefinedVarsInternal i : allPreDefVars)
    {
        G4_Declare* dcl = nullptr;

        if (predefinedVarNeedGRF(i))
        {
            // work item id variables are handled uniformly
            G4_Type ty = GetGenTypeFromVISAType(getPredefinedVarType(i));
            dcl = createPreVar(getPredefinedVarID(i), 1, ty);
        }
        else
        {
            const char* name = getPredefinedVarString(i);
            switch (i)
            {
            case PreDefinedVarsInternal::VAR_NULL:
                dcl = createDeclareNoLookup(name, G4_GRF, 1, 1, Type_UD);
                dcl->getRegVar()->setPhyReg(phyregpool.getNullReg(), 0);
                break;
            case PreDefinedVarsInternal::TSC:
            {
                G4_Declare* tscDcl = createPreVar(i, 5, Type_UD);
                tscDcl->getRegVar()->setPhyReg(phyregpool.getTm0Reg(), 0);
                dcl = tscDcl;
                break;
            }
            case PreDefinedVarsInternal::R0:
            {
                dcl = getBuiltinR0();
                break;
            }
            case PreDefinedVarsInternal::SR0:
            {
                G4_Declare* sr0Dcl = createPreVar(i, 4, Type_UD);
                sr0Dcl->getRegVar()->setPhyReg(phyregpool.getSr0Reg(), 0);
                dcl = sr0Dcl;
                break;
            }
            case PreDefinedVarsInternal::CR0:
            {
                G4_Declare* cr0Dcl = createPreVar(i, 3, Type_UD);
                cr0Dcl->getRegVar()->setPhyReg(phyregpool.getCr0Reg(), 0);
                dcl = cr0Dcl;
                break;
            }
            case PreDefinedVarsInternal::CE0:
            {
                G4_Declare* ce0Dcl = createPreVar(i, 1, Type_UD);
                ce0Dcl->getRegVar()->setPhyReg(phyregpool.getMask0Reg(), 0);
                dcl = ce0Dcl;
                break;
            }
            case PreDefinedVarsInternal::DBG:
            {
                G4_Declare* dbgDcl = createPreVar(i, 2, Type_UD);
                dbgDcl->getRegVar()->setPhyReg(phyregpool.getDbgReg(), 0);
                dcl = dbgDcl;
                break;
            }
            case PreDefinedVarsInternal::ARG:
            {
                dcl = createDeclareNoLookup(name, G4_INPUT, numEltPerGRF<Type_UD>(), 32, Type_UD);
                dcl->getRegVar()->setPhyReg(phyregpool.getGreg(ArgRet_Stackcall::Arg), 0);
                break;
            }
            case PreDefinedVarsInternal::RET:
            {
                dcl = createDeclareNoLookup(name, G4_GRF, numEltPerGRF<Type_UD>(), 12, Type_UD);
                dcl->getRegVar()->setPhyReg(phyregpool.getGreg(ArgRet_Stackcall::Ret), 0);
                dcl->setLiveOut();
                break;
            }
            case PreDefinedVarsInternal::FE_SP:
            {
                unsigned int startReg = kernel.getFPSPGRF();
                dcl = createDeclareNoLookup(name, G4_GRF, 1, 1, Type_UQ);
                dcl->getRegVar()->setPhyReg(phyregpool.getGreg(startReg), SubRegs_Stackcall::FE_SP);
                break;
            }
            case PreDefinedVarsInternal::FE_FP:
            {
                // PREDEFINED_FE_FP
                unsigned int startReg = kernel.getFPSPGRF();
                dcl = createDeclareNoLookup(name, G4_GRF, 1, 1, Type_UQ);
                dcl->getRegVar()->setPhyReg(phyregpool.getGreg(startReg), SubRegs_Stackcall::FE_FP);
                break;
            }
            case PreDefinedVarsInternal::HW_TID:
            {
                // PREDEFINED_HW_TID
                dcl = getBuiltinHWTID();
                break;
            }
            case PreDefinedVarsInternal::X:
            case PreDefinedVarsInternal::Y:
            case PreDefinedVarsInternal::COLOR:
            {
                // these three are size 1 UW
                dcl = createDeclareNoLookup(name, G4_GRF, 1, 1,
                    GetGenTypeFromVISAType(getPredefinedVarType(i)));
                break;
            }
            default:
            {
                break;
            }
            }
        }
        preDefVars.setPredefinedVar(i, dcl);
    }
}

void IR_Builder::createBuiltinDecls()
{

    auto numR0DW = numEltPerGRF<Type_UD>();
    builtinR0 = createDeclareNoLookup(
        "BuiltinR0",
        G4_INPUT,
        numR0DW,
        1,
        Type_UD);
    builtinR0->getRegVar()->setPhyReg(phyregpool.getGreg(0), 0);
    realR0 = builtinR0;

    if (m_options->getOption(vISA_enablePreemption))
    {
        G4_Declare *R0CopyDcl = createTempVar(numR0DW, Type_UD, GRFALIGN);
        builtinR0 = R0CopyDcl;
        R0CopyDcl->setDoNotSpill();
    }

    builtinA0 = createDeclareNoLookup(
        "BuiltinA0",
        G4_ADDRESS,
        1,
        1,
        Type_UD);
    builtinA0->getRegVar()->setPhyReg(phyregpool.getAddrReg(), 0);
    builtinA0Dot2 = createDeclareNoLookup(
        "BuiltinA0Dot2",  //a0.2
        G4_ADDRESS,
        1,
        1,
        Type_UD);
    builtinA0Dot2->getRegVar()->setPhyReg(phyregpool.getAddrReg(), 2);

    builtinHWTID = createDeclareNoLookup("hw_tid", G4_GRF, 1, 1, Type_UD);

    builtinT252 = createDeclareNoLookup(vISAPreDefSurf[PREDEFINED_SURFACE_T252].name, G4_GRF, 1, 1, Type_UD);
    builtinBindlessSampler = createDeclareNoLookup("B_S", G4_GRF, 1, 1, Type_UD);

    builtinSamplerHeader = createDeclareNoLookup("samplerHeader", G4_GRF, numEltPerGRF<Type_UD>(), 1, Type_UD);

}


G4_Declare* IR_Builder::getSpillFillHeader()
{
    if (!spillFillHeader)
    {
        spillFillHeader = createTempVar(1, Type_UD, GRFALIGN, "spillHeader");
        spillFillHeader->setLiveOut();
        spillFillHeader->setDoNotSpill();
    }
    return spillFillHeader;
}

G4_Declare* IR_Builder::getOldA0Dot2Temp()
{
    if (!oldA0Dot2Temp)
    {
        oldA0Dot2Temp = createTempVar(1, Type_UD, Any, "OldA0Dot2");
        oldA0Dot2Temp->setLiveOut();
        oldA0Dot2Temp->setLiveIn();
        oldA0Dot2Temp->setDoNotSpill();
    }
    return oldA0Dot2Temp;
}

IR_Builder::IR_Builder(
    TARGET_PLATFORM genPlatform,
    INST_LIST_NODE_ALLOCATOR &alloc,
    G4_Kernel &k,
    Mem_Manager &m,
    Options *options,
    CISA_IR_Builder* parent,
    FINALIZER_INFO *jitInfo,
    const WA_TABLE *pWaTable)
    : platform(genPlatform), curFile(NULL), curLine(0), curCISAOffset(-1), immPool(*this), metaData(jitInfo),
    type(VISA_BUILD_TYPE::KERNEL), parentBuilder(parent),
    builtinSamplerHeaderInitialized(false), m_pWaTable(pWaTable), m_options(options), CanonicalRegionStride0(0, 1, 0),
    CanonicalRegionStride1(1, 1, 0), CanonicalRegionStride2(2, 1, 0), CanonicalRegionStride4(4, 1, 0),
    mem(m), phyregpool(m, k.getNumRegTotal()), hashtable(m), rgnpool(m), dclpool(m),
    instList(alloc), kernel(k), metadataMem(4096)
{
    num_temp_dcl = 0;
    kernel.setBuilder(this); // kernel needs pointer to the builder
    createBuiltinDecls();

    sampler8x8_group_id = 0;

    be_sp = be_fp = tmpFCRet = nullptr;

    arg_size = 0;
    return_var_size = 0;

    if (metaData != NULL)
    {
        memset(metaData, 0, sizeof(FINALIZER_INFO));
    }

    fcPatchInfo = NULL;

    createPreDefinedVars();
}


IR_Builder::~IR_Builder()
{
    // We need to invoke the destructor of every instruction ever allocated
    // so that its members will be freed.
    // Note that we don't delete the instruction itself as it's allocated from
    // the memory manager's pool
    for (unsigned i = 0, size = (unsigned)instAllocList.size(); i != size; i++)
    {
        G4_INST* inst = instAllocList[i];
        inst->~G4_INST();
    }
    instAllocList.clear();

    for (auto MD : allMDs)
    {
        MD->~Metadata();
    }

    for (auto node : allMDNodes)
    {
        node->~MDNode();
    }

    if (fcPatchInfo)
    {
        fcPatchInfo->~FCPatchingInfo();
    }
}

G4_Declare* IR_Builder::createDeclareNoLookup(
    const char*     name,
    G4_RegFileKind  regFile,
    unsigned short  n_elems,
    unsigned short  n_rows,
    G4_Type         ty,
    DeclareType     kind,
    G4_RegVar *     base,
    G4_Operand *    repRegion,
    G4_ExecSize     execSize)
{
    if (regFile == G4_FLAG)
    {
        MUST_BE_TRUE(ty == Type_UW, "flag decl must have type UW");
    }

    G4_Declare* dcl = dclpool.createDeclare(name, regFile, n_elems,
        n_rows, ty, kind, base, repRegion, execSize);

    kernel.Declares.push_back(dcl);

    return dcl;
}


uint32_t IR_Builder::getSplitEMask(unsigned execSize, uint32_t eMask, bool isLo)
{
    const uint32_t qhMasks = InstOpt_M0 | InstOpt_M8 |
        InstOpt_M16 | InstOpt_M24;
    uint32_t other = eMask & ~qhMasks;
    uint32_t qh = eMask & qhMasks;

    switch (execSize) {
    case 16: // Split SIMD16 into SIMD8
        switch (qh) {
        case 0: // instOpt not specified, treat as 1H
        case InstOpt_M0:
            return (isLo ? InstOpt_M0 : InstOpt_M8) | other;
        case InstOpt_M16:
            return (isLo ? InstOpt_M16 : InstOpt_M24) | other;
        }
        break;
    case 32: // Split SIMD32 into SIMD16.
        switch (qh) {
        case 0:
            return (isLo ? InstOpt_M0 : InstOpt_M16) | other;
        }
        break;
    }

    ASSERT_USER(false, "Unhandled cases for EMask splitting!");
    return ~0U;
}



G4_Declare* IR_Builder::createTempVar(
    unsigned int numElements, G4_Type type, G4_SubReg_Align subAlign,
    const char* prefix, bool appendIdToName)
{
    const char* name = appendIdToName ?
        getNameString(mem, 20, "%s%d", prefix, num_temp_dcl++) :
        getNameString(mem, 20, "%s", prefix);

    unsigned short dcl_width = 0, dcl_height = 1;
    const uint16_t typeSize = TypeSize(type);
    int totalByteSize = numElements * typeSize;
    if (totalByteSize <= (int)numEltPerGRF<Type_UB>())
    {
        dcl_width = totalByteSize / typeSize;
    }
    else
    {
        // here we assume that the start point of the var is the beginning of a GRF?
        // so subregister must be 0?
        dcl_width = numEltPerGRF<Type_UB>() / typeSize;
        dcl_height = totalByteSize / numEltPerGRF<Type_UB>();
        if (totalByteSize % numEltPerGRF<Type_UB>() != 0)
        {
            dcl_height++;
        }
    }

    G4_Declare* dcl = createDeclareNoLookup(name, G4_GRF, dcl_width, dcl_height, type);
    dcl->setSubRegAlign(subAlign);
    return dcl;
}

G4_Declare* IR_Builder::createAddrFlagSpillLoc(G4_Declare* dcl)
{
    const char* name = getNameString(mem, 16, "SP_LOC_%d", numAddrFlagSpillLoc++);
    G4_Declare* spillLoc = createDeclareNoLookup(name,
        G4_GRF,
        dcl->getNumElems(),
        1,
        dcl->getElemType(),
        DeclareType::AddrSpill);
    dcl->setSpilledDeclare(spillLoc);
    spillLoc->setSubRegAlign(dcl->getSubRegAlign()); // for simd32 flag the spill loc has to be 2-word aligned since it's accessed as dw
    return spillLoc;
}

G4_Declare* IR_Builder::createHardwiredDeclare(
    uint32_t numElements, G4_Type type, uint32_t regNum, uint32_t regOff)
{
    G4_Declare* dcl = createTempVar(numElements, type, Any);
    unsigned int linearizedStart = (regNum * numEltPerGRF<Type_UB>()) + (regOff * TypeSize(type));
    // since it's called post RA (specifically post computePReg) we have to manually set the GRF's byte offset
    dcl->setGRFBaseOffset(linearizedStart);
    dcl->getRegVar()->setPhyReg(phyregpool.getGreg(regNum), regOff);
    return dcl;
}

G4_INST* IR_Builder::createPseudoKills(
    std::initializer_list<G4_Declare*> dcls, PseudoKillType ty)
{
    G4_INST* inst = nullptr;
    for (auto dcl : dcls)
    {
        inst = createPseudoKill(dcl, ty);
    }

    return inst;
}

G4_INST* IR_Builder::createPseudoKill(G4_Declare* dcl, PseudoKillType ty)
{
    auto dstRgn = createDst(dcl->getRegVar(), 0, 0, 1, Type_UD);
    G4_INST* inst = createIntrinsicInst(nullptr, Intrinsic::PseudoKill, g4::SIMD1,
        dstRgn, createImm((unsigned int)ty, Type_UD), nullptr, nullptr, InstOpt_WriteEnable, true);

    return inst;
}

static const unsigned int HWORD_BYTE_SIZE = 32;

G4_INST* IR_Builder::createSpill(
    G4_DstRegRegion* dst, G4_SrcRegRegion* header, G4_SrcRegRegion* payload,
    G4_ExecSize execSize,
    uint16_t numRows, uint32_t offset, G4_Declare* fp, G4_InstOption option,
    bool addToInstList)
{
    G4_INST* spill = createIntrinsicInst(nullptr, Intrinsic::Spill, execSize, dst,
        header, payload, nullptr, option, addToInstList);
    spill->asSpillIntrinsic()->setFP(fp);
    spill->asSpillIntrinsic()->setOffset((uint32_t)
        (((uint64_t)offset * HWORD_BYTE_SIZE) / numEltPerGRF<Type_UB>()));
    spill->asSpillIntrinsic()->setNumRows(numRows);
    return spill;
}

G4_INST* IR_Builder::createSpill(
    G4_DstRegRegion* dst, G4_SrcRegRegion* payload,
    G4_ExecSize execSize, uint16_t numRows, uint32_t offset,
    G4_Declare* fp, G4_InstOption option, bool addToInstList)
{
    auto builtInR0 = getBuiltinR0();
    auto rd = getRegionStride1();
    auto srcRgnr0 = createSrc(builtInR0->getRegVar(), 0, 0, rd, Type_UD);
    G4_INST* spill = createIntrinsicInst(nullptr, Intrinsic::Spill, execSize, dst,
        srcRgnr0, payload, nullptr, option, addToInstList);
    spill->asSpillIntrinsic()->setFP(fp);
    spill->asSpillIntrinsic()->setOffset((uint32_t)
        (((uint64_t)offset * HWORD_BYTE_SIZE) / numEltPerGRF<Type_UB>()));
    spill->asSpillIntrinsic()->setNumRows(numRows);
    return spill;
}

G4_INST* IR_Builder::createFill(
    G4_SrcRegRegion* header, G4_DstRegRegion* dstData,
    G4_ExecSize execSize,
    uint16_t numRows, uint32_t offset, G4_Declare* fp, G4_InstOption option,
    bool addToInstList)
{
    G4_INST* fill = createIntrinsicInst(nullptr, Intrinsic::Fill, execSize, dstData,
        header, nullptr, nullptr, option, addToInstList);
    fill->asFillIntrinsic()->setFP(fp);
    fill->asFillIntrinsic()->setOffset((uint32_t)
        (((uint64_t)offset * HWORD_BYTE_SIZE) / numEltPerGRF<Type_UB>()));
    fill->asFillIntrinsic()->setNumRows(numRows);
    return fill;
}

G4_INST* IR_Builder::createFill(
    G4_DstRegRegion* dstData,
    G4_ExecSize execSize,
    uint16_t numRows, uint32_t offset, G4_Declare* fp , G4_InstOption option,
    bool addToInstList)
{
    auto builtInR0 = getBuiltinR0();
    auto rd = getRegionStride1();
    auto srcRgnr0 = createSrc(builtInR0->getRegVar(), 0, 0, rd, Type_UD);
    G4_INST* fill = createIntrinsicInst(nullptr, Intrinsic::Fill, execSize, dstData,
        srcRgnr0, nullptr, nullptr, option, addToInstList);

    fill->asFillIntrinsic()->setFP(fp);
    fill->asFillIntrinsic()->setOffset((uint32_t)
        (((uint64_t)offset * HWORD_BYTE_SIZE) / numEltPerGRF<Type_UB>()));
    fill->asFillIntrinsic()->setNumRows(numRows);
    return fill;
}


G4_Declare* IR_Builder::createTempFlag(unsigned short numberOfFlags, const char* prefix)
{
    const char* name = getNameString(mem, 20, "%s%d", prefix, num_temp_dcl++);

    G4_Declare* dcl = createDeclareNoLookup(name, G4_FLAG, numberOfFlags, 1, Type_UW);

    return dcl;
}

G4_Declare* IR_Builder::createFlag(uint16_t numFlagElements, const char* name)
{
    uint32_t numWords = (numFlagElements + 15) / 16;
    G4_Declare* dcl = createDeclareNoLookup(name, G4_FLAG, numWords, 1, Type_UW);
    dcl->setNumberFlagElements((uint8_t)numFlagElements);
    return dcl;
}

G4_Declare* IR_Builder::createPreVar(
    PreDefinedVarsInternal preDefVar_index, unsigned short numElements, G4_Type type)
{
    MUST_BE_TRUE(preDefVar_index < PreDefinedVarsInternal::VAR_LAST,
        "illegal predefined var index");
    unsigned short dcl_width = 0, dcl_height = 1;
    auto typeSize = TypeSize(type);
    int totalByteSize = numElements * typeSize;
    if (totalByteSize <= (int)numEltPerGRF<Type_UB>())
    {
        dcl_width = totalByteSize / typeSize;
    }
    else
    {
        // here we assume that the start point of the var is the beginning of a GRF?
        // so subregister must be 0?
        dcl_width = numEltPerGRF<Type_UB>() / typeSize;
        dcl_height = totalByteSize / numEltPerGRF<Type_UB>();
        if (totalByteSize % numEltPerGRF<Type_UB>() != 0)
        {
            dcl_height++;
        }
    }

    G4_Declare* dcl = createPreVarDeclareNoLookup(
        preDefVar_index, dcl_width, dcl_height, type);
    // subAlign has to be type size at the minimum
    dcl->setSubRegAlign(Get_G4_SubRegAlign_From_Type(type));
    return dcl;
}


G4_SrcRegRegion* IR_Builder::createSrcWithNewRegOff(G4_SrcRegRegion* old, short newRegOff)
{
    if (old->getRegAccess() == Direct)
    {
        return createSrcRegRegion(old->getModifier(), Direct, old->getBase(), newRegOff,
            old->getSubRegOff(), old->getRegion(), old->getType(), old->getAccRegSel());
    }
    else
    {
        return createIndirectSrc(old->getModifier(), old->getBase(), newRegOff, old->getSubRegOff(),
            old->getRegion(), old->getType(), old->getAddrImm());
    }
}


G4_SrcRegRegion* IR_Builder::createSrcWithNewSubRegOff(G4_SrcRegRegion* old, short newSubRegOff)
{
    if (old->getRegAccess() == Direct)
    {
        return createSrcRegRegion(old->getModifier(), old->getRegAccess(), old->getBase(), old->getRegOff(),
            newSubRegOff, old->getRegion(), old->getType(), old->getAccRegSel());
    }
    else
    {
        return createIndirectSrc(old->getModifier(), old->getBase(), old->getRegOff(), newSubRegOff,
            old->getRegion(), old->getType(), old->getAddrImm());
    }
}


G4_SrcRegRegion* IR_Builder::createSrcWithNewBase(G4_SrcRegRegion* old, G4_VarBase* newBase)
{
    if (old->getRegAccess() == Direct)
    {
        return createSrcRegRegion(old->getModifier(), Direct, newBase, old->getRegOff(),
            old->getSubRegOff(), old->getRegion(), old->getType(), old->getAccRegSel());
    }
    else
    {
        return createIndirectSrc(old->getModifier(), newBase, old->getRegOff(), old->getSubRegOff(),
            old->getRegion(), old->getType(), old->getAddrImm());
    }
}

G4_DstRegRegion* IR_Builder::createDstWithNewSubRegOff(G4_DstRegRegion* old, short newSubRegOff)
{
    if (old->getRegAccess() == Direct)
    {
        return createDst(old->getBase(), old->getRegOff(), newSubRegOff, old->getHorzStride(), old->getType(), old->getAccRegSel());
    }
    else
    {
        return createIndirectDst(old->getBase(), newSubRegOff, old->getHorzStride(), old->getType(), old->getAddrImm());
    }
}


G4_Imm* IR_Builder::createImm(float fp)
{
    uint32_t imm = *((uint32_t*) &fp);
    G4_Type immType = Type_F;
    if (getPlatform() >= GENX_CHV && m_options->getOption(vISA_FImmToHFImm) &&
        !VISA_WA_CHECK(getPWaTable(), WaSrc1ImmHfNotAllowed))
    {
        // we may be able to lower it to HF
        // ieee32 format: 23-8-1
        // ieee16 format: 10-5-1
        // bit0-22 are fractions
        uint32_t fraction = imm & 0x7FFFFF;
        // bit23-30 are exponents
        uint32_t exponent = (imm >> 23) & 0xFF;
        uint32_t sign = (imm >> 31) & 0x1;
        int expVal = ((int) exponent) - 127;

        if (exponent == 0 && fraction == 0)
        {
            // 0 and -0
            immType = Type_HF;
            imm = sign << 15;
        }
        else if ((fraction & 0x1FFF) == 0 && (expVal <= 15 && expVal >= -16))
        {
            // immediate can be exactly represented in HF.
            // we exclude denormal, infinity, and NaN.
            immType = Type_HF;
            uint32_t newExp = (expVal + 15) & 0x1F;
            imm = (sign << 15) | (newExp << 10) | (fraction >> 13);
        }
    }
    G4_Imm* i = hashtable.lookupImm(imm, immType);
    return (i != NULL)? i : hashtable.createImm(imm, immType);
}

G4_Imm* IR_Builder::createDFImm(double fp)
{
    int64_t val = (int64_t)(*(uint64_t*)&fp);
    G4_Imm* i = hashtable.lookupImm(val, Type_DF);
    return (i != NULL)? i : hashtable.createImm(val, Type_DF);
}

G4_Type IR_Builder::getNewType(int64_t imm, G4_Type ty)
{
    switch (ty)
    {
    case Type_Q:
    case Type_D:
        // It is legal to change a positive imm's type from signed to unsigned if it fits
        // in the unsigned type. We do prefer signed type however for readability.
        if (imm >= MIN_WORD_VALUE && imm <= MAX_WORD_VALUE)
        {
            return Type_W;
        }
        else if (imm >= MIN_UWORD_VALUE && imm <= MAX_UWORD_VALUE)
        {
            return Type_UW;
        }
        else if (imm >= int(MIN_DWORD_VALUE) && imm <= int(MAX_DWORD_VALUE))
        {
            return Type_D;
        }
        else if (imm >= unsigned(MIN_UDWORD_VALUE) && imm <= unsigned(MAX_UDWORD_VALUE))
        {
            return Type_UD;
        }
        break;
    case Type_UQ:
    case Type_UD:
    {
        // unsigned imm must stay as unsigned
        uint64_t immU = static_cast<uint64_t>(imm);
        if (immU <= MAX_UWORD_VALUE)
        {
            return Type_UW;
        }
        else if (immU <= unsigned(MAX_UDWORD_VALUE))
        {
            return Type_UD;
        }
        break;
    }
    case Type_UB:
        return Type_UW;
    case Type_B:
        return Type_W;
    default:
        return ty;
    }
    return ty;
}

//
// look up an imm operand
//
G4_Imm* OperandHashTable::lookupImm(int64_t imm, G4_Type ty)
{
    ImmKey key(imm, ty);
    auto iter = immTable.find(key);
    return iter != immTable.end() ? iter->second : nullptr;
}

//
// create a dst reg region
//
G4_Imm* OperandHashTable::createImm(int64_t imm, G4_Type ty)
{
    G4_Imm* i = new (mem)G4_Imm(imm, ty);
    ImmKey key(imm, ty);
    immTable[key] = i;
    return i;
}


//
// create the region <vstride; width, hstride> if not yet created
//
const RegionDesc* RegionPool::createRegion(
    uint16_t vstride, uint16_t width, uint16_t hstride)
{

    for (unsigned i = 0, size = (unsigned)rgnlist.size(); i < size; i++)
    {
        RegionDesc* region = rgnlist[i];
        if (region->vertStride == vstride &&
            region->width == width &&
            region->horzStride == hstride)
        {
            return region; // exist
        }
    }
    //
    // create one
    //
    RegionDesc* rd = new (mem) RegionDesc(vstride, width, hstride);
    rgnlist.push_back(rd);
    return rd;
}

/*
    Used in IR_Builder::translateVISARawSendInst. All the bits in des and extDesc are already set.
*/
G4_SendMsgDescriptor* IR_Builder::createGeneralMsgDesc(
    uint32_t desc,
    uint32_t extDesc,
    SendAccess access,
    G4_Operand* bti,
    G4_Operand* sti)
{
    return new (mem) G4_SendMsgDescriptor(desc, extDesc, access, bti, sti);
}

G4_SendMsgDescriptor* IR_Builder::createSendMsgDesc(
    SFID sfid,
    uint32_t desc,
    uint32_t extDesc,
    int src1Len,
    SendAccess access,
    G4_Operand *bti,
    bool isValidFuncCtrl)
{
    return new (mem) G4_SendMsgDescriptor(sfid, desc, extDesc, src1Len, access, bti, isValidFuncCtrl);
}

G4_SendMsgDescriptor* IR_Builder::createSendMsgDesc(
    unsigned funcCtrl,
    unsigned regs2rcv,
    unsigned regs2snd,
    SFID funcID,
    unsigned extMsgLength,
    uint16_t extFuncCtrl,
    SendAccess access,
    G4_Operand *bti,
    G4_Operand *sti)
{
    G4_SendMsgDescriptor* msgDesc = new (mem) G4_SendMsgDescriptor(
        funcCtrl, regs2rcv, regs2snd, funcID, (uint16_t) extMsgLength,
        extFuncCtrl, access, bti, sti, *this);
    return msgDesc;
}

// shorthand for read msg desc. Note that extDesc still needs to be explicitly created,
// SendMsgDesc ctor does not program all the bits
G4_SendMsgDescriptor* IR_Builder::createReadMsgDesc(SFID sfid,
    uint32_t desc,
    G4_Operand* bti)
{
    //ToDo: move extDesc into SendMsgDesc ctor
    uint32_t extDesc = G4_SendMsgDescriptor::createExtDesc(sfid);
    return new (mem) G4_SendMsgDescriptor(sfid, desc, extDesc, 0, SendAccess::READ_ONLY, bti, true);
}

G4_SendMsgDescriptor* IR_Builder::createWriteMsgDesc(SFID sfid,
    uint32_t desc,
    int src1Len,
    G4_Operand* bti)
{
    //ToDo: move extDesc into SendMsgDesc ctor
    uint32_t extDesc = G4_SendMsgDescriptor::createExtDesc(sfid, false, src1Len);
    return new (mem) G4_SendMsgDescriptor(sfid, desc, extDesc, src1Len, SendAccess::WRITE_ONLY, bti, true);
}

G4_SendMsgDescriptor* IR_Builder::createSyncMsgDesc(SFID sfid, uint32_t desc)
{
    //ToDo: move extDesc into SendMsgDesc ctor
    uint32_t extDesc = G4_SendMsgDescriptor::createExtDesc(sfid);
    return new (mem) G4_SendMsgDescriptor(sfid, desc, extDesc, 0, SendAccess::READ_WRITE, nullptr, true);
}

G4_SendMsgDescriptor* IR_Builder::createSampleMsgDesc(
    uint32_t desc,
    bool cps,
    int src1Len,
    G4_Operand* bti,
    G4_Operand* sti)
{
#define CPS_LOD_COMPENSATION_ENABLE 11

    uint32_t extDesc = G4_SendMsgDescriptor::createExtDesc(SFID::SAMPLER, false, src1Len);
    if (cps)
    {
        extDesc |= 1 << CPS_LOD_COMPENSATION_ENABLE;
    }
    return new (mem) G4_SendMsgDescriptor(desc, extDesc, SendAccess::READ_ONLY, bti, sti);
}

G4_Operand* IR_Builder::emitSampleIndexGE16(
    G4_Operand* sampler,
    G4_Declare* headerDecl)
{
    G4_Operand* samplerIdx;

    G4_Declare* t0
        = createTempVar(1, Type_UD, Any);
    G4_DstRegRegion* t0Dst
        = Create_Dst_Opnd_From_Dcl(t0, 1);
    G4_SrcRegRegion* t0Src
        = Create_Src_Opnd_From_Dcl(t0, getRegionScalar());

    G4_Declare* baseAdj
        = createTempVar(1, Type_UD, Any);
    G4_DstRegRegion* baseAdjDst
        = Create_Dst_Opnd_From_Dcl(baseAdj, 1);
    G4_SrcRegRegion* baseAdjSrc
        = Create_Src_Opnd_From_Dcl(baseAdj, getRegionScalar());

    G4_Declare* idxLow
        = createTempVar(1, Type_UD, Any);
    G4_DstRegRegion* idxLowDst
        = Create_Dst_Opnd_From_Dcl(idxLow, 1);
    G4_SrcRegRegion* idxLowSrc
        = Create_Src_Opnd_From_Dcl(idxLow, getRegionScalar());

    // calculate the sampler state base pointer offset based on
    // sample index, for putting to msg header M0.3
    createBinOp(G4_shr, g4::SIMD1,
        t0Dst, sampler, createImm(4, Type_UD),
        InstOpt_WriteEnable, true);
    createBinOp(G4_shl, g4::SIMD1,
        baseAdjDst, t0Src, createImm(8, Type_UD),
        InstOpt_WriteEnable, true);

    // get low 4 bits of sample index for putting into msg descriptor
    G4_SrcRegRegion* sampler2Src
        = createSrc(
        sampler->getTopDcl()->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
    createBinOp(G4_and, g4::SIMD1,
        idxLowDst, sampler2Src, createImm(0xf, Type_UD),
        InstOpt_WriteEnable, true);
    samplerIdx = idxLowSrc;

    // add the base pointer offset with r0.3 and put to M0.3
    G4_DstRegRegion* stateBaseRgn
        = createDst(headerDecl->getRegVar(),
            0, 3, 1, Type_UD);
    G4_SrcRegRegion* src0
        = createSrc(
            builtinR0->getRegVar(), 0, 3, getRegionScalar(), Type_UD);
    createBinOp(G4_add, g4::SIMD1, stateBaseRgn,
        src0, baseAdjSrc, InstOpt_WriteEnable, true);

    return samplerIdx;
}

G4_INST* IR_Builder::createInst(
    G4_Predicate* prd,
    G4_opcode op,
    G4_CondMod* mod,
    G4_Sat sat,
    G4_ExecSize execSize,
    G4_DstRegRegion* dst,
    G4_Operand* src0,
    G4_Operand* src1,
    G4_InstOpts options,
    bool addToInstList)
{
    MUST_BE_TRUE(op != G4_math, "IR_Builder::createInst should not be used to create math instructions");
    G4_INST* i = NULL;

    // ToDo: have separate functions to create call/jmp/ret
    if (G4_Inst_Table[op].instType == InstTypeFlow)
    {
        // TODO: remove this path
        MUST_BE_TRUE(!sat, "saturation not defined on branching ops");
        i = new (mem)G4_InstCF(*this, prd, op, mod, execSize, dst, src0, options);
    }
    else
    {
        i = new (mem)G4_INST(*this, prd, op, mod, sat, execSize, dst, src0, src1, options);
    }

    if (addToInstList)
    {
        i->setCISAOff(curCISAOffset);

        if (m_options->getOption(vISA_EmitLocation))
        {
            i->setLocation(allocateMDLocation(curLine, curFile));
        }

        instList.push_back(i);
    }

    instAllocList.push_back(i);

    return i;
}

// same as above, except we don't add it to the Builder's instList
G4_INST* IR_Builder::createInternalInst(
    G4_Predicate* prd,
    G4_opcode op,
    G4_CondMod* mod,
    G4_Sat sat,
    G4_ExecSize execSize,
    G4_DstRegRegion* dst,
    G4_Operand* src0,
    G4_Operand* src1,
    G4_InstOpts options)
{
    MUST_BE_TRUE(op != G4_math, "IR_Builder::createInternalInst should not be used to create math instructions");

    auto ii = createInst(prd, op, mod, sat, execSize, dst, src0, src1, options, false);

    return ii;
}

G4_INST* IR_Builder::createNop(G4_InstOpts instOpt)
{
    return createInternalInst(
        nullptr, G4_nop, nullptr, g4::NOSAT, g4::SIMD1,
        nullptr, nullptr, nullptr, instOpt);
}

// sync inst are always internal, so no option to append it to instList.
// Also currently don't take any InstOpt
G4_INST* IR_Builder::createSync(G4_opcode syncOp, G4_Operand* src)
{
    assert(G4_INST::isSyncOpcode(syncOp) && "expect a sync op");
    return createInternalInst(
        nullptr, syncOp, nullptr, g4::NOSAT, g4::SIMD1,
        nullptr, src, nullptr, InstOpt_NoOpt);
}

G4_INST* IR_Builder::createMov(
    G4_ExecSize execSize,
    G4_DstRegRegion* dst, G4_Operand* src0, G4_InstOpts options,
    bool appendToInstList)
{
    G4_INST* newInst = nullptr;
    if (appendToInstList)
    {
        newInst = createInst(
            nullptr, G4_mov, nullptr, g4::NOSAT, execSize,
            dst, src0, nullptr, options, true);
    }
    else
    {
        newInst = createInternalInst(
            nullptr, G4_mov, nullptr, g4::NOSAT, execSize,
            dst, src0, nullptr, options);
    }
    return newInst;
}

G4_INST* IR_Builder::createBinOp(
    G4_Predicate *pred, G4_opcode op, G4_ExecSize execSize,
    G4_DstRegRegion* dst, G4_Operand* src0, G4_Operand* src1,
    G4_InstOpts options,
    bool appendToInstList)
{
    if (appendToInstList)
    {
        return createInst(
            pred, op, nullptr, g4::NOSAT, execSize,
            dst, src0, src1, options, true);
    }
    else
    {
        return createInternalInst(
            pred, op, nullptr, g4::NOSAT, execSize,
            dst, src0, src1, options);
    }
}

// mach creates both implicit acc and src using the supplied accType. AccWrCtrl is turned on.
// acc0.0 is always used
G4_INST* IR_Builder::createMach(
    G4_ExecSize execSize,
    G4_DstRegRegion* dst, G4_Operand* src0, G4_Operand* src1,
    G4_InstOpts options, G4_Type accType)
{
    auto machInst = createInternalInst(
        nullptr, G4_mach, nullptr, g4::NOSAT, execSize,
        dst, src0, src1, options);
    const RegionDesc* rd = execSize > g4::SIMD1 ? getRegionStride1() : getRegionScalar();
    auto accSrc = createSrc(phyregpool.getAcc0Reg(), 0, 0, rd, accType);
    machInst->setImplAccSrc(accSrc);
    auto accDSt = createDst(phyregpool.getAcc0Reg(), 0, 0, 1, accType);
    machInst->setImplAccDst(accDSt);
    machInst->setOptionOn(InstOpt_AccWrCtrl);
    return machInst;
}

// macl creates an implicit src using the supplied the accType. AccWrCtrl is not set.
// acc0.0 is always used
G4_INST* IR_Builder::createMacl(
    G4_ExecSize execSize,
    G4_DstRegRegion* dst, G4_Operand* src0, G4_Operand* src1,
    G4_InstOpts options, G4_Type accType)
{
    auto maclInst = createInternalInst(
        nullptr, G4_mach, nullptr, g4::NOSAT, execSize, dst, src0, src1, options);
    const RegionDesc* rd = execSize > g4::SIMD1 ? getRegionStride1() : getRegionScalar();
    auto accSrc = createSrc(phyregpool.getAcc0Reg(), 0, 0, rd, accType);
    maclInst->setImplAccSrc(accSrc);
    return maclInst;
}

G4_INST* IR_Builder::createMadm(
    G4_Predicate* pred,
    G4_ExecSize execSize,
    G4_DstRegRegion* dst,
    G4_SrcRegRegion* src0, G4_SrcRegRegion* src1, G4_SrcRegRegion* src2,
    G4_InstOpts options)
{
    // madm is currently only created in vISA->Gen IR translation
        return createInst(
            pred, G4_madm, nullptr, g4::NOSAT, execSize,
            dst, src0, src1, src2, options, true);
}

G4_INST* IR_Builder::createIf(G4_Predicate* prd, G4_ExecSize execSize, G4_InstOpts options)
{
    auto inst = createCFInst(prd, G4_if, execSize, nullptr, nullptr, options, true);
    return inst;
}

G4_INST* IR_Builder::createElse(G4_ExecSize execSize, G4_InstOpts options)
{
    auto inst = createCFInst(nullptr, G4_else, execSize, nullptr, nullptr, options, true);
    return inst;
}

G4_INST* IR_Builder::createEndif(G4_ExecSize execSize, G4_InstOpts options)
{
    auto inst = createCFInst(nullptr, G4_endif, execSize, nullptr, nullptr, options, true);
    return inst;
}

G4_INST* IR_Builder::createLabelInst(G4_Label* label, bool appendToInstList)
{
    if (appendToInstList)
    {
        return createInst(nullptr, G4_label, nullptr, g4::NOSAT, g4::SIMD_UNDEFINED,
            nullptr, label, nullptr, InstOpt_NoOpt, true);
    }
    else
    {
        return createInternalInst(
            nullptr, G4_label, nullptr, g4::NOSAT, g4::SIMD_UNDEFINED,
            nullptr, label, nullptr, 0,
            0);
    }
}

// jmpTarget may be either a label (direct jmp) or scalar operand (indirect jmp)
G4_INST* IR_Builder::createJmp(
    G4_Predicate* pred,
    G4_Operand* jmpTarget, G4_InstOpts options,
    bool appendToInstList)
{
    if (appendToInstList)
    {
        return createInst(pred, G4_jmpi, nullptr, g4::NOSAT, g4::SIMD1,
            nullptr, jmpTarget, nullptr, options, true);
    }
    else
    {
        return createInternalInst(pred, G4_jmpi, nullptr, g4::NOSAT, g4::SIMD1,
            nullptr, jmpTarget, nullptr, options);
    }
}

G4_INST* IR_Builder::createInternalCFInst(
    G4_Predicate* prd, G4_opcode op, G4_ExecSize execSize,
    G4_Label* jip, G4_Label* uip,
    G4_InstOpts options)
{
    MUST_BE_TRUE(G4_Inst_Table[op].instType == InstTypeFlow,
        "IR_Builder::createInternalCFInst must be used with InstTypeFlow instruction class");

    auto ii = createCFInst(prd, op, execSize, jip, uip, options, false);
    return ii;
}

G4_INST* IR_Builder::createCFInst(
    G4_Predicate* prd, G4_opcode op, G4_ExecSize execSize,
    G4_Label* jip, G4_Label* uip,
    G4_InstOpts options,
    bool addToInstList)
{
    MUST_BE_TRUE(G4_Inst_Table[op].instType == InstTypeFlow,
        "IR_Builder::createCFInst must be used with InstTypeFlow instruction class");

    G4_InstCF* ii = new (mem)G4_InstCF(*this, prd, op, execSize, jip, uip, options);

    if (addToInstList)
    {
        ii->setCISAOff(curCISAOffset);

        if (m_options->getOption(vISA_EmitLocation))
        {
            ii->setLocation(allocateMDLocation(curLine, curFile));
        }
        instList.push_back(ii);
    }

    instAllocList.push_back(ii);

    return ii;
}


G4_INST* IR_Builder::createInst(
    G4_Predicate* prd,
    G4_opcode op,
    G4_CondMod* mod,
    G4_Sat sat,
    G4_ExecSize execSize,
    G4_DstRegRegion* dst,
    G4_Operand* src0,
    G4_Operand* src1,
    G4_Operand* src2,
    G4_InstOpts options,
    bool addToInstList)
{
    MUST_BE_TRUE(op != G4_math && G4_Inst_Table[op].instType != InstTypeFlow,
        "IR_Builder::createInst should not be used to create math/CF instructions");

    G4_INST* i = NULL;

    i = new (mem)G4_INST(*this, prd, op, mod, sat, execSize, dst, src0, src1, src2, options);

    if (addToInstList)
    {
        i->setCISAOff(curCISAOffset);

        if (m_options->getOption(vISA_EmitLocation))
        {
            i->setLocation(allocateMDLocation(curLine, curFile));
        }

        instList.push_back(i);
    }

    instAllocList.push_back(i);

    return i;
}

// same as above, except we don't add it to the Builder's instList
G4_INST* IR_Builder::createInternalInst(
    G4_Predicate* prd,
    G4_opcode op,
    G4_CondMod* mod,
    G4_Sat sat,
    G4_ExecSize execSize,
    G4_DstRegRegion* dst,
    G4_Operand* src0,
    G4_Operand* src1,
    G4_Operand* src2,
    G4_InstOpts options)
{
    auto ii = createInst(
        prd, op, mod, sat, execSize,
        dst, src0, src1, src2, options, false);

    return ii;

}

G4_InstSend* IR_Builder::createSendInst(
    G4_Predicate* prd,
    G4_opcode op,
    G4_ExecSize execSize,
    G4_DstRegRegion* postDst,
    G4_SrcRegRegion* currSrc,
    G4_Operand* msg,
    G4_InstOpts options,
    G4_SendMsgDescriptor *msgDesc,
    bool addToInstList)
{

    assert (msgDesc && "msgDesc must not be null");
    G4_InstSend* m = new (mem)G4_InstSend(
        *this, prd, op, execSize, postDst, currSrc, msg, options, msgDesc);

    if (addToInstList)
    {
        m->setCISAOff(curCISAOffset);

        if (m_options->getOption(vISA_EmitLocation))
        {
            m->setLocation(allocateMDLocation(curLine, curFile));
        }

        instList.push_back(m);
    }

    instAllocList.push_back(m);

    return m;
}

G4_InstSend* IR_Builder::createInternalSendInst(
    G4_Predicate* prd,
    G4_opcode op,
    G4_ExecSize execSize,
    G4_DstRegRegion* postDst,
    G4_SrcRegRegion* currSrc,
    G4_Operand* msg,
    G4_InstOpts options,
    G4_SendMsgDescriptor *msgDesc)
{
    auto ii = createSendInst(prd, op, execSize,
        postDst, currSrc,
        msg, options, msgDesc, false);

    return ii;
}

//
// Create a split send (sends) instruction
// sends (size) dst src0 src1 exDesc msgDesc
//

G4_InstSend* IR_Builder::createSplitSendInst(
    G4_Predicate* prd,
    G4_opcode op,
    G4_ExecSize execSize,
    G4_DstRegRegion* dst,
    G4_SrcRegRegion* src0, // can be header
    G4_SrcRegRegion* src1,
    G4_Operand* msg,       // msg descriptor: imm or vec
    G4_InstOpts options,
    G4_SendMsgDescriptor *msgDesc,
    G4_Operand* src3,      // ext msg desciptor: imm or vec
    bool addToInstList)
{

    if (!src1)
    {
        // src1 may be null if we need to force generate split send (e.g., for bindless surfaces)
        MUST_BE_TRUE(msgDesc->extMessageLength() == 0, "src1 length must be 0 if it is null");
        src1 = createNullSrc(Type_UD);
    }
    if (!src3)
    {
        src3 = createImm(msgDesc->getExtendedDesc(), Type_UD);
    }
    G4_InstSend* m = new (mem) G4_InstSend(
        *this, prd, op, execSize, dst, src0, src1, msg, src3, options, msgDesc);

    if (addToInstList)
    {
        m->setCISAOff(curCISAOffset);

        if (m_options->getOption(vISA_EmitLocation))
        {
            m->setLocation(allocateMDLocation(curLine, curFile));
        }
        instList.push_back(m);
    }

    instAllocList.push_back(m);

    return m;
}

G4_InstSend* IR_Builder::createInternalSplitSendInst(
    G4_ExecSize execSize,
    G4_DstRegRegion* dst,
    G4_SrcRegRegion* src0, // can be header
    G4_SrcRegRegion* src1,
    G4_Operand* msg,       // msg descriptor: imm or vec
    G4_InstOpts options,
    G4_SendMsgDescriptor *msgDesc,
    G4_Operand* src3)     // ext msg desciptor: imm or vec)
{
    auto ii = createSplitSendInst(nullptr, G4_sends, execSize, dst, src0, src1, msg, options,
        msgDesc, src3, false);

    return ii;
}

//
// Math instruction is like a generic one except:
// -- it takes a G4_MathOp to specify the function control
// -- conditional modifier is not allowed
// -- there are additional restrictions on dst/src regions that will be checked in HW conformity
//
G4_INST* IR_Builder::createMathInst(
    G4_Predicate* prd,
    G4_Sat sat,
    G4_ExecSize execSize,
    G4_DstRegRegion* dst,
    G4_Operand* src0,
    G4_Operand* src1,
    G4_MathOp mathOp,
    G4_InstOpts options,
    bool addToInstList)
{
    G4_INST* i = new (mem)G4_InstMath(
        *this, prd, G4_math, NULL, sat, execSize, dst, src0, src1, options, mathOp);

    if (addToInstList)
    {
        i->setCISAOff(curCISAOffset);

        if (m_options->getOption(vISA_EmitLocation))
        {
            i->setLocation(allocateMDLocation(curLine, curFile));
        }
        instList.push_back(i);
    }

    instAllocList.push_back(i);

    return i;
}

G4_INST* IR_Builder::createInternalMathInst(
    G4_Predicate* prd,
    G4_Sat sat,
    G4_ExecSize execSize,
    G4_DstRegRegion* dst,
    G4_Operand* src0,
    G4_Operand* src1,
    G4_MathOp mathOp,
    G4_InstOpts options)
{
    auto ii = createMathInst(prd, sat, execSize, dst, src0, src1, mathOp, options, false);
    return ii;
}

G4_INST* IR_Builder::createIntrinsicInst(
    G4_Predicate* prd, Intrinsic intrinId,
    G4_ExecSize size,
    G4_DstRegRegion* dst, G4_Operand* src0, G4_Operand* src1, G4_Operand* src2,
    G4_InstOpts options, bool addToInstList)
{
    G4_INST* i = nullptr;

    if (intrinId == Intrinsic::Spill)
        i = new (mem) G4_SpillIntrinsic(*this, prd, intrinId, size, dst, src0, src1, src2, options);
    else if (intrinId == Intrinsic::Fill)
        i = new (mem) G4_FillIntrinsic(*this, prd, intrinId, size, dst, src0, src1, src2, options);
    else
        i = new (mem) G4_InstIntrinsic(*this, prd, intrinId, size, dst, src0, src1, src2, options);

    if (addToInstList)
    {
        i->setCISAOff(curCISAOffset);

        if (m_options->getOption(vISA_EmitLocation))
        {
            i->setLocation(allocateMDLocation(curLine, curFile));
        }

        instList.push_back(i);
    }

    instAllocList.push_back(i);

    return i;
}

G4_INST* IR_Builder::createInternalIntrinsicInst(
    G4_Predicate* prd, Intrinsic intrinId, G4_ExecSize execSize,
    G4_DstRegRegion* dst, G4_Operand* src0, G4_Operand* src1, G4_Operand* src2,
    G4_InstOpts options)
{
    auto ii = createIntrinsicInst(prd, intrinId, execSize, dst, src0, src1, src2, options, false);

    return ii;
}

G4_MathOp IR_Builder::Get_MathFuncCtrl(ISA_Opcode op, G4_Type type)
{
    switch (op)
    {
    case ISA_LOG:
        return MATH_LOG;
    case ISA_MOD:   // remainder of IDIV
        return MATH_INT_DIV_REM;
    case ISA_POW:
        return MATH_POW;
    case ISA_SIN:
        return MATH_SIN;
    case ISA_COS:
        return MATH_COS;
    case ISA_SQRT:
        return MATH_SQRT;
    case ISA_RSQRT:
        return MATH_RSQ;
    case ISA_INV:
        return MATH_INV;
    case ISA_DIV:
        return IS_FTYPE(type) || IS_HFTYPE(type) ? MATH_FDIV : MATH_INT_DIV_QUOT;
    case ISA_EXP:
        return MATH_EXP;
    default:
        ASSERT_USER(0, "Illegal math opcode.");
        return MATH_RESERVED;
    }
}

// After building IR total number number of rows required
// for arg and retvar become known, so resize the pre-defined
// vars here to the max required in current compilation unit.
void IR_Builder::resizePredefinedStackVars()
{
    getStackCallArg()->resizeNumRows(this->getArgSize());
    getStackCallRet()->resizeNumRows(this->getRetVarSize());
}

G4_Operand* IR_Builder::duplicateOpndImpl(G4_Operand* opnd)
{
    if (!opnd || opnd->isImm())
        return opnd;
    if (opnd->isSrcRegRegion()) {
        return createSrcRegRegion(*(opnd->asSrcRegRegion()));
    }
    else if (opnd->isDstRegRegion()) {
        return createDstRegRegion(*(opnd->asDstRegRegion()));
    }
    else if (opnd->isPredicate()) {
        return createPredicate(*(opnd->asPredicate()));
    }
    else if (opnd->isCondMod()) {
        return createCondMod(*(opnd->asCondMod()));
    }
    else {
        return opnd;
    }
}

/*
* Create send instruction for specified GenX architecture.
* bti: surface id
* sti: sampler id
*/
G4_InstSend* IR_Builder::Create_Send_Inst_For_CISA(
    G4_Predicate* pred,
    G4_DstRegRegion *postDst,
    G4_SrcRegRegion *payload,
    unsigned regs2snd,
    unsigned regs2rcv,
    G4_ExecSize execSize,
    unsigned fc,
    SFID tf_id,
    bool header_present,
    SendAccess access,
    G4_Operand* bti,
    G4_Operand* sti,
    G4_InstOpts options,
    bool is_sendc)
{
    G4_SendMsgDescriptor* msgDesc =
        createSendMsgDesc(fc, regs2rcv, regs2snd, tf_id, 0, 0, access,
            bti, sti);

    msgDesc->setHeaderPresent(header_present);

    return Create_Send_Inst_For_CISA(
        pred, postDst, payload, execSize, msgDesc, options, is_sendc);
}

//bindless surfaces, write the content of T252 to extended message descriptor
//exdesc holds the value of the extended message descriptor for bit [0:11]
//add (1) a0.2<1>:ud T252<1>:ud exDesc:ud {NoMask}
// returns a0.2<0;1,0>:ud
G4_SrcRegRegion* IR_Builder::createBindlessExDesc(uint32_t exdesc)
{
    // virtual var for each exdesc
    G4_SrcRegRegion* T252 = Create_Src_Opnd_From_Dcl(builtinT252, getRegionScalar());
    const char* buf = getNameString(mem, 20, "ExDesc%d", num_temp_dcl++);
    G4_Declare* exDescDecl = createDeclareNoLookup(buf, G4_ADDRESS, 1, 1, Type_UD);
    exDescDecl->setSubRegAlign(Four_Word);
    G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(exDescDecl, 1);
    if (useNewExtDescFormat())
    {
        createMov(g4::SIMD1, dst, T252, InstOpt_WriteEnable, true);
    }
    else
    {
        createBinOp(G4_add, g4::SIMD1, dst, T252, createImm(exdesc, Type_UD), InstOpt_WriteEnable, true);
    }
    return Create_Src_Opnd_From_Dcl(exDescDecl, getRegionScalar());
}


/*
 *
 *  this does two things:
 *  -- If send has exec size 16, its destination must have Type W.
 *  -- avoid using Q/UQ type on CHV/BXT
 */
static void fixSendDstType(G4_DstRegRegion* dst, G4_ExecSize execSize)
{
    MUST_BE_TRUE(dst->getRegAccess() == Direct, "Send dst must be a direct operand");

    MUST_BE_TRUE(dst->getSubRegOff() == 0, "dst may not have a non-zero subreg offset");

    // normally we should create a new alias for dst's declare, but since it's a send
    // type mismatch between operand and decl should not matter
    if (execSize == g4::SIMD16 && dst->getType() != Type_W && dst->getType() != Type_UW)
    {
        dst->setType(Type_W);
    }

    if (dst->getType() == Type_HF)
    {
        dst->setType(Type_W);
    }
}


G4_InstSend *IR_Builder::Create_Send_Inst_For_CISA(
    G4_Predicate *pred,
    G4_DstRegRegion *postDst,
    G4_SrcRegRegion *payload,
    G4_ExecSize execsize,
    G4_SendMsgDescriptor *msgDesc,
    G4_InstOpts option,
    bool is_sendc)
{
    G4_opcode send_opcode= is_sendc ? G4_sendc : G4_send;

    fixSendDstType(postDst, execsize);

    uint32_t desc = msgDesc->getDesc();
    G4_Operand *bti = msgDesc->getBti();
    G4_Operand *sti = msgDesc->getSti();
    G4_Operand *descOpnd = NULL;

    bool needSamplerMove = sti && !sti->isImm() && !isBindlessSampler(sti);

    if ((bti && !bti->isImm()) || needSamplerMove)
    {
        // use a0.0 directly
        G4_DstRegRegion* addr_dst_opnd = Create_Dst_Opnd_From_Dcl(builtinA0, 1);

        if (bti && !bti->isImm())
        {
            //add (1) a0.0:ud bti:ud desc:ud
            // create source for bti
            createBinOp(
                G4_add,
                g4::SIMD1,
                addr_dst_opnd,
                bti,
                createImm(desc, Type_UD),
                InstOpt_WriteEnable,
                true);
        }

        if (needSamplerMove)
        {
            G4_Declare *dcl1 = createTempVar(1, Type_UD, Any);
            G4_DstRegRegion* tmp_dst_opnd = Create_Dst_Opnd_From_Dcl(dcl1, 1);

            createBinOp(
                G4_shl,
                g4::SIMD1,
                tmp_dst_opnd,
                sti,
                createImm(8, Type_UD),
                InstOpt_WriteEnable,
                true);

            G4_SrcRegRegion* tmp_src_opnd = Create_Src_Opnd_From_Dcl(dcl1, getRegionScalar());

            if (!bti || bti->isImm())
            {
                createBinOp(
                    G4_add,
                    g4::SIMD1,
                    addr_dst_opnd,
                    tmp_src_opnd,
                    createImm(desc, Type_UD),
                    InstOpt_WriteEnable,
                    true);
            }
            else
            {
                G4_SrcRegRegion* addr_src_opnd = Create_Src_Opnd_From_Dcl(builtinA0, getRegionScalar());

                createBinOp(
                    G4_add,
                    g4::SIMD1,
                    duplicateOperand(addr_dst_opnd),
                    addr_src_opnd,
                    tmp_src_opnd,
                    InstOpt_WriteEnable,
                    true);
            }
        }

        descOpnd = Create_Src_Opnd_From_Dcl(builtinA0, getRegionScalar());
    }
    else
    {
        descOpnd = createImm(desc, Type_UD);
    }

    return createSendInst(
        pred,
        send_opcode,
        execsize,
        postDst,
        payload,
        descOpnd,
        option,
        msgDesc,
        true);
}

/*
 * Create split send instruction for specified GenX architecture.
 * bti: surface id
 * sti: sampler id
 * Gen9: sends (execsize)     dst,  src1,  src2,  ex_desc,  desc
 */
G4_InstSend* IR_Builder::Create_SplitSend_Inst_For_CISA(
    G4_Predicate* pred,
    G4_DstRegRegion *dst,
    G4_SrcRegRegion *src1,
    unsigned regs2snd1,
    G4_SrcRegRegion *src2,
    unsigned regs2snd2,
    unsigned regs2rcv,
    G4_ExecSize execSize,
    unsigned fc,
    SFID tf_id,
    bool header_present,
    SendAccess access,
    G4_Operand* bti,
    G4_Operand* sti,
    G4_InstOpts options,
    bool is_sendc)
{
    G4_SendMsgDescriptor *msgDesc =
        createSendMsgDesc(fc, regs2rcv, regs2snd1, tf_id, regs2snd2,
                          0, access, bti, sti);

    msgDesc->setHeaderPresent(header_present);

    return Create_SplitSend_Inst(pred, dst, src1, src2, execSize,
        msgDesc, options, is_sendc);
}

// desc, if indirect, is constructed from the BTI/STI values in msgDesc and is always a0.0
G4_InstSend *IR_Builder::Create_SplitSend_Inst(
    G4_Predicate *pred,
    G4_DstRegRegion *dst,
    G4_SrcRegRegion *src1,
    G4_SrcRegRegion *src2,
    G4_ExecSize execsize,
    G4_SendMsgDescriptor *msgDesc,
    G4_InstOpts option,
    bool is_sendc)
{
    G4_opcode send_opcode = is_sendc ? G4_sendsc : G4_sends;

    fixSendDstType(dst, execsize);

    uint32_t desc = msgDesc->getDesc();
    uint32_t exdesc = msgDesc->getExtendedDesc();
    G4_Operand *bti = msgDesc->getBti();
    G4_Operand *sti = msgDesc->getSti();

    G4_Operand* descOpnd = NULL;
    G4_SrcRegRegion* extDescOpnd = nullptr;

    bool doAlignBindlessSampler = alignBindlessSampler() && sti && isBindlessSampler(sti);
    bool needsSamplerMove = (sti && !sti->isImm() && !isBindlessSampler(sti)) || doAlignBindlessSampler;

    bool needsSurfaceMove = false;
    bool needsA0ExDesc = false;

    if (bti && bti->isSrcRegRegion())
    {
        if (isBindlessSurface(bti))
        {
            needsA0ExDesc = true;
            // set T252 as BTI
            if ((desc & 0xFF) != PREDEF_SURF_252)
            {
                desc = (desc & ~0xFF) | PREDEF_SURF_252;
            }
        }
        else
        {
            needsSurfaceMove = true;
        }
    }

    if (needsSurfaceMove)
    {
        //add (1) a0.0:ud bti:ud desc:ud
        G4_DstRegRegion* addrDstOpnd = Create_Dst_Opnd_From_Dcl(builtinA0, 1);

        createBinOp(G4_add, g4::SIMD1, addrDstOpnd, bti,
            createImm(desc, Type_UD), InstOpt_WriteEnable, true);
    }

    if (needsSamplerMove)
    {
        G4_Declare *dcl1 = createTempVar(1, Type_UD, Any);

        if (doAlignBindlessSampler)
        {
            // check if address is 32-byte aligned
            // use STI = 0 for 32-byte aligned address, STI = 1 otherwise
            // (W) and (1) (nz)f0.0 null S31 0x10:uw
            G4_Declare* tmpFlag = createTempFlag(1);
            G4_CondMod* condMod = createCondMod(Mod_nz, tmpFlag->getRegVar(), 0);
            createInst(nullptr, G4_and, condMod, g4::NOSAT, g4::SIMD1, createNullDst(Type_UD),
                createSrcRegRegion(*(sti->asSrcRegRegion())), createImm(0x10, Type_UW), InstOpt_WriteEnable, true);
            // (W) (f0.0) sel (1) tmp:ud 0x100 0x0
            G4_Predicate* pred = createPredicate(PredState_Plus, tmpFlag->getRegVar(), 0);
            createInst(pred, G4_sel, nullptr, g4::NOSAT, g4::SIMD1, Create_Dst_Opnd_From_Dcl(dcl1, 1),
                createImm(0x100, Type_UW), createImm(0x0, Type_UW), InstOpt_WriteEnable, true);
        }
        else
        {
            // shl (1) tmp:ud sti:ud 0x8:uw
            G4_DstRegRegion* tmpDstOpnd = Create_Dst_Opnd_From_Dcl(dcl1, 1);
            createBinOp(G4_shl, g4::SIMD1, tmpDstOpnd, sti,
                createImm(8, Type_UD), InstOpt_WriteEnable, true);
        }

        G4_SrcRegRegion* tmpSrcOpnd = Create_Src_Opnd_From_Dcl(dcl1, getRegionScalar());
        G4_DstRegRegion* addrDstOpnd = Create_Dst_Opnd_From_Dcl(builtinA0, 1);
        if (!needsSurfaceMove)
        {
            // add (1) a0.0 tmp:ud desc:ud
            createBinOp(G4_add, g4::SIMD1, addrDstOpnd, tmpSrcOpnd,
                createImm(desc, Type_UD),
                InstOpt_WriteEnable,
                true);
        }
        else
        {
            // add (1) a0.0 a0.0:ud tmp:ud
            G4_SrcRegRegion* addrSrcOpnd = Create_Src_Opnd_From_Dcl(builtinA0, getRegionScalar());
            createBinOp(G4_add, g4::SIMD1, addrDstOpnd, addrSrcOpnd,
                tmpSrcOpnd, InstOpt_WriteEnable, true);
        }
    }

    if (needsSurfaceMove || needsSamplerMove)
    {
        descOpnd = Create_Src_Opnd_From_Dcl(builtinA0, getRegionScalar());
    }
    else
    {
        descOpnd = createImm(desc, Type_UD);
    }

    if (needsA0ExDesc)
    {
        extDescOpnd = createBindlessExDesc(exdesc);
    }
    else
    {
        // do nothing as the extended msg desc will just be a null operand
    }

    return createSplitSendInst(pred, send_opcode, execsize,
        dst, src1, src2,
        descOpnd,
        option, msgDesc, extDescOpnd, true);
}



// for reder target messages,
// desc has a constant BTI value (i.e., no bindless) and no STI
// extDesc may be indirect (MRT and other bits) and is passed in
G4_InstSend *IR_Builder::Create_SplitSend_Inst_For_RT(
    G4_Predicate *pred,
    G4_DstRegRegion *dst,
    G4_SrcRegRegion *src1,
    G4_SrcRegRegion *src2,
    G4_SrcRegRegion *extDescOpnd,
    G4_ExecSize execSize,
    G4_SendMsgDescriptor *msgDesc,
    G4_InstOpts option)
{
    G4_opcode send_opcode = G4_sendsc;

    fixSendDstType(dst, execSize);

    uint32_t desc = msgDesc->getDesc();
    G4_Operand* descOpnd = nullptr;
    G4_Operand *bti = msgDesc->getBti();

    if (bti && bti->isSrcRegRegion())
    {
        //add (1) a0.0:ud bti:ud desc:ud
        G4_DstRegRegion* addrDstOpnd = Create_Dst_Opnd_From_Dcl(builtinA0, 1);
        createBinOp(G4_add, g4::SIMD1, addrDstOpnd, bti,
            createImm(desc, Type_UD), InstOpt_WriteEnable, true);
        descOpnd = Create_Src_Opnd_From_Dcl(builtinA0, getRegionScalar());
    }
    else
    {
        descOpnd = createImm(desc, Type_UD);
    }

    return createSplitSendInst(pred, send_opcode, execSize,
        dst, src1, src2, descOpnd,
        option, msgDesc, extDescOpnd, true);
}

// create a declare for send payload
G4_Declare* IR_Builder::createSendPayloadDcl(unsigned num_elt, G4_Type type)
{
    const char* name = getNameString(mem, 16, "M%u", ++num_temp_dcl);
    const uint16_t sizeOfType = TypeSize(type);
    unsigned short numRow = (num_elt * sizeOfType - 1) / numEltPerGRF<Type_UB>() + 1;
    unsigned short numElt = (numRow == 1) ? num_elt : (numEltPerGRF<Type_UB>()/sizeOfType);
    G4_Declare *dcl = createDeclareNoLookup(
        name,
        G4_GRF,
        numElt,
        numRow,
        type);
    return dcl;
}

void IR_Builder::Create_MOVR0_Inst(G4_Declare* dcl, short regOff, short subregOff, bool use_nomask)
{
    G4_DstRegRegion* dst1_opnd = createDst(
        dcl->getRegVar(),
        regOff,
        subregOff,
        1,
        dcl->getElemType());

    // create r0 src
    G4_SrcRegRegion* r0_src_opnd = Create_Src_Opnd_From_Dcl(builtinR0, getRegionStride1());
    // create inst
    createMov(
        G4_ExecSize(GENX_DATAPORT_IO_SZ),
        dst1_opnd,
        r0_src_opnd,
        (use_nomask ? InstOpt_WriteEnable : 0),
        true);
}

void IR_Builder::Create_ADD_Inst(
    G4_Declare* dcl, short regOff, short subregOff, G4_ExecSize execsize,
    G4_Predicate* pred, G4_CondMod* condMod,
    G4_Operand* src0_opnd, G4_Operand* src1_opnd, G4_InstOption options)
{
    auto dst = createDst(dcl->getRegVar(), regOff, subregOff, 1, dcl->getElemType());

    if (src0_opnd->isImm() && src0_opnd->asImm()->isZero())
    {
        createInst(pred, G4_mov, condMod, g4::NOSAT, execsize, dst, src1_opnd, NULL, options, true);
    }
    else if (src1_opnd->isImm() && src1_opnd->asImm()->isZero())
    {
        createInst(pred, G4_mov, condMod, g4::NOSAT, execsize, dst, src0_opnd, NULL, options, true);
    }
    else if (src0_opnd->isImm() && !src1_opnd->isImm())
    {
        createInst(pred, G4_add, condMod, g4::NOSAT, execsize, dst, src1_opnd, src0_opnd, options, true);
    }
    else
    {
        createInst(pred, G4_add, condMod, g4::NOSAT, execsize, dst, src0_opnd, src1_opnd, options, true);
    }
}

// Currently this function is mostly used in dataport intrinsic translation functions.
// If it is used in some other places, Qtrctrl should be added in options if needed.
void IR_Builder::Create_MOV_Inst(
    G4_Declare* dcl,
    short regOff,
    short subregOff,
    G4_ExecSize execSize,
    G4_Predicate* pred,
    G4_CondMod* condMod,
    G4_Operand* src_opnd,
    bool use_nomask,
    G4_InstOpts options)
{
    G4_DstRegRegion* dst2_opnd = createDst(
        dcl->getRegVar(),
        regOff,
        subregOff,
        1,
        dcl->getElemType());

    createInst(
        pred,
        G4_mov,
        condMod,
        g4::NOSAT,
        execSize,
        dst2_opnd,
        src_opnd,
        NULL,
        use_nomask ? (InstOpt_WriteEnable | options) : options,
        true);
}

// send payload preparation.
// dcl: decl for send payload
// num_dword: number of DW to send
// src_opnd: send src, its size may be several GRFs
void IR_Builder::Create_MOV_Send_Src_Inst(
    G4_Declare* dcl,
    short regoff,
    short subregoff,
    unsigned num_dword,
    G4_Operand* src_opnd,
    G4_InstOpts options)
{
    // since src_opnd is raw source in CISA, it is aligned to GRF, so there is no subRegOff.
    unsigned remained_dword = num_dword;
    // if data type of src_opnd is not UD, change it to UD
    // assumption: size of src_opnd is multiple of UD
    short dst_regoff = regoff, dst_subregoff = subregoff;
    G4_ExecSize execsize = g4::SIMD1;
    G4_DstRegRegion* dst = NULL;
    //G4_SrcRegRegion* src = NULL;
    G4_Operand* src = NULL;
    const RegionDesc *rd = NULL;
    G4_Declare *dst_dcl = dcl;
    short src_regoff = 0, src_subregoff = 0;
    bool non_ud_scalar = false;
    bool scalar_src = (src_opnd->isImm() || num_dword == 1);

    if (scalar_src && src_opnd->getType() != Type_UD) {
        // change the type of dst dcl to src type
        remained_dword = num_dword * (TypeSize(Type_UD)/src_opnd->getTypeSize());
        dst_dcl = createSendPayloadDcl(remained_dword, src_opnd->getType());
        dst_dcl->setAliasDeclare(dcl, regoff * numEltPerGRF<Type_UB>() + subregoff * TypeSize(Type_UD));
        dst_regoff = 0;
        dst_subregoff = 0;
        non_ud_scalar = true;
    }

    src_regoff = src_opnd->asSrcRegRegion()->getRegOff();
    src_subregoff = src_opnd->asSrcRegRegion()->getSubRegOff();
    src_subregoff = src_subregoff * src_opnd->getTypeSize() / dst_dcl->getElemSize();

    auto getMaxEsize = [](uint32_t opt)
    {
        unsigned maskOption = (opt & InstOpt_QuarterMasks);
        switch (maskOption)
        {
        case InstOpt_M4:
        case InstOpt_M12:
        case InstOpt_M20:
        case InstOpt_M28:
            return 4;
        case InstOpt_M8:
        case InstOpt_M24:
            return 8;
        case InstOpt_M16:
            return 16;
        default:
            return 32;
        }
    };
    G4_ExecSize maxEsize(getMaxEsize(options));

    // here remained_dword is not the number of DW, but the number of dst data type.
    while (remained_dword)
    {
        if (non_ud_scalar && src_opnd->getTypeSize() != TypeSize(Type_UD))
        {
            if (remained_dword >= 32)
            {
                execsize = g4::SIMD32;
            }
            else if (remained_dword >= 16)
            {
                execsize = g4::SIMD16;
            }
            else
            {
                execsize = G4_ExecSize((uint8_t)Round_Down_Pow2(remained_dword));
            }

            execsize = (execsize > maxEsize) ? maxEsize :  execsize;
            if (execsize == g4::SIMD1)
            {
                rd = getRegionScalar();
            }
            else
            {
                rd = getRegionStride1();
            }
        }
        else
        {
            if (remained_dword >= 16)
            {
                execsize = g4::SIMD16;
            }
            else if (remained_dword >= 8)
            {
                execsize = g4::SIMD8;
            }
            else
            {
                execsize = G4_ExecSize(Round_Down_Pow2(remained_dword));
            }
            execsize = (execsize > maxEsize) ? maxEsize :  execsize;
            if (execsize == g4::SIMD1)
            {
                rd = getRegionScalar();
            }
            else
            {
                rd = getRegionStride1();
            }
        }

        dst = createDst(
            dst_dcl->getRegVar(),
            dst_regoff,
            dst_subregoff,
            1,
            dst_dcl->getElemType());

        if (scalar_src && src_opnd->isImm())
        {
            src = src_opnd->asImm();
        }
        else
        {
            src = createSrc(
                src_opnd->asSrcRegRegion()->getBase(),
                src_regoff,
                src_subregoff,
                rd,
                dst_dcl->getElemType());
        }

        createMov(
            execsize,
            dst,
            src,
            options,
            true);

        // update offset in decl
        if (remained_dword >= execsize) {
            remained_dword -= execsize;
            if (execsize * dst_dcl->getElemSize() == 2 * numEltPerGRF<Type_UB>()) {
                dst_regoff += 2;
                if (!scalar_src) {
                    src_regoff += 2;
                }
            }
            else if (execsize * dst_dcl->getElemSize() == numEltPerGRF<Type_UB>()) {
                dst_regoff += 1;
                if (!scalar_src) {
                    src_regoff += 1;
                }
            }
            else {
                dst_subregoff += execsize;
                if (dst_subregoff > ((int)numEltPerGRF<Type_UB>() / dst_dcl->getElemSize())) {
                    dst_regoff++;
                    dst_subregoff -= numEltPerGRF<Type_UB>() / dst_dcl->getElemSize();
                }
                if (!scalar_src) {
                    src_subregoff += execsize;
                    if (src_subregoff > (short)(numEltPerGRF<Type_UB>() / TypeSize(Type_UD))) {
                        src_regoff++;
                        src_subregoff -= numEltPerGRF<Type_UB>() / TypeSize(Type_UD);
                    }
                }
            }
        }
    }
}
// create an opnd without regpoff and subregoff
G4_DstRegRegion* IR_Builder::Create_Dst_Opnd_From_Dcl(
    G4_Declare* dcl, unsigned short hstride)
{
    return createDst(
        dcl->getRegVar(),
        0,
        0,
        hstride,
        dcl->getElemType());
}
// create an opnd without regpoff and subregoff
G4_SrcRegRegion* IR_Builder::Create_Src_Opnd_From_Dcl(
    G4_Declare* dcl, const RegionDesc* rd)
{
    return createSrcRegRegion(
        Mod_src_undef,
        Direct,
        dcl->getRegVar(),
        0,
        0,
        rd,
        dcl->getElemType());
}

G4_DstRegRegion* IR_Builder::createNullDst(G4_Type dstType)
{
    return createDst(
        phyregpool.getNullReg(),
        0,
        0,
        1,
        dstType);
}

G4_SrcRegRegion* IR_Builder::createNullSrc(G4_Type srcType)
{
    return createSrcRegRegion(Mod_src_undef,
                               Direct,
                               phyregpool.getNullReg(),
                               0,
                               0,
                               getRegionScalar(),
                               srcType);
}

// check if the dst opnd align to GRF.
// if it is not aligned to GRF
// 1. change align of var dcl to GRF if the dst size is smaller than GRF size,
//    no alias or alias offset is 0.
// 2. otherwise, create a temp operand and return it.
G4_DstRegRegion* IR_Builder::Check_Send_Dst(G4_DstRegRegion *dst_opnd)
{
    //FIXME: This function seems to be bogus
    G4_DstRegRegion* d;
    // check if dst is align to GRF

    const unsigned short SIZEOF_DW = 4;
    if (dst_opnd->getTypeSize() > 1)
    {
        d = dst_opnd;
    }
    else
    {
        // change type of dcl and offset in it
        short new_SubRegOff = dst_opnd->getSubRegOff();
        if (dst_opnd->getRegAccess() == Direct)
        {
            new_SubRegOff = dst_opnd->getSubRegOff() / SIZEOF_DW;
        }
        G4_DstRegRegion new_dst(
            dst_opnd->getRegAccess(),
            dst_opnd->getBase(),
            dst_opnd->getRegOff(),
            new_SubRegOff,
            1,
            Type_UD);
        d = createDstRegRegion(new_dst);
    }

    return d;
}

void IR_Builder::addInputArg(input_info_t * inpt)
{
    m_inputVect.push_back(inpt);
}

input_info_t * IR_Builder::getInputArg(unsigned int index)
{
    return m_inputVect[index];
}

unsigned int IR_Builder::getInputCount()
{
    return (uint32_t)m_inputVect.size();
}

input_info_t *IR_Builder::getRetIPArg() {
    // TODO: So far, we assume the last argument of caller of callable kernel
    // or callable kernel is the RetIP argument. If required, extra attribute
    // will be added to specify which QWORD argument is used as RetIP argument
    // and the code will traverse all argument to find that one.
    input_info_t *RetIP = getInputArg(getInputCount() - 1);
    // More sanity check on the argument.
    ASSERT_USER(IS_QTYPE(RetIP->dcl->getElemType()), "RetIP needs to be QWORD!");
    ASSERT_USER(RetIP->dcl->getNumElems() == 1, "RetIP needs to be QWORD!");
    return RetIP;
}

G4_Predicate_Control IR_Builder::vISAPredicateToG4Predicate(
    VISA_PREDICATE_CONTROL control, G4_ExecSize execSize)
{
    switch (control)
    {
    case PRED_CTRL_NON:
        return PRED_DEFAULT;
    case PRED_CTRL_ANY:
    {
        if (!predCtrlHasWidth())
        {
            return PRED_ANY_WHOLE;
        }
        switch (execSize)
        {
        case 1: return PRED_DEFAULT;
        case 2: return PRED_ANY2H;
        case 4: return PRED_ANY4H;
        case 8: return PRED_ANY8H;
        case 16: return PRED_ANY16H;
        case 32: return PRED_ANY32H;
        default:
            MUST_BE_TRUE(0, "Invalid predicate control group size.");
            return PRED_DEFAULT;
        }
    }
    case PRED_CTRL_ALL:
    {
        if (!predCtrlHasWidth())
        {
            return PRED_ALL_WHOLE;
        }
        switch (execSize)
        {
        case 1: return PRED_DEFAULT;
        case 2: return PRED_ALL2H;
        case 4: return PRED_ALL4H;
        case 8: return PRED_ALL8H;
        case 16: return PRED_ALL16H;
        case 32: return PRED_ALL32H;
        default:
            MUST_BE_TRUE(0, "Invalid predicate control group size.");
            return PRED_DEFAULT;
        }
    }
    default:
        MUST_BE_TRUE(0, "Invalid predicate control.");
        return PRED_DEFAULT;
    }
}



