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

#include "BuildIR.h"
#include "DebugInfo.h"
#include "G4_Kernel.hpp"
#include "G4_BB.hpp"
#include "VarSplit.h"
// #include "iga/IGALibrary/api/igaEncoderWrapper.hpp"
#include "iga/IGALibrary/api/kv.hpp"
#include "BinaryEncodingIGA.h"

#include <list>
#include <fstream>
#include <functional>
#include <iomanip>
#include <utility>

using namespace vISA;

void gtPinData::markInsts()
{
    // Take a snapshot of instructions in kernel.
    for (auto bb : kernel.fg)
    {
        for (auto inst : *bb)
        {
            markedInsts.insert(inst);
        }
    }
}

void gtPinData::removeUnmarkedInsts()
{
    if (!kernel.fg.getIsStackCallFunc() &&
        !kernel.fg.getHasStackCalls())
    {
        // Marked instructions correspond to caller/callee save
        // and FP/SP manipulation instructions.
        return;
    }

    MUST_BE_TRUE(whichRAPass == ReRAPass,
        "Unexpectedly removing unmarked instructions in first RA pass");
    // Instructions not seen in "marked" snapshot will be removed by this function.
    for (auto bb : kernel.fg)
    {
        for (auto it = bb->begin(), itEnd = bb->end();
            it != itEnd;)
        {
            auto inst = (*it);

            if (markedInsts.find(inst) == markedInsts.end())
            {
                it = bb->erase(it);
                continue;
            }
            it++;
        }
    }
}

void* gtPinData::getFreeGRFInfo(unsigned& size)
{
    // Here is agreed upon format for reporting free GRFs:
    //struct freeBytes
    //{
    //    unsigned short startByte;
    //    unsigned short numConsecutiveBytes;
    //};

    // Added magic 0xDEADD00D at start and
    // magic 0xDEADBEEF at the end of buffer
    // on request of gtpin team.
    //
    //struct freeGRFInfo
    //{
    //    unsigned short numItems;
    //
    //    freeBytes data[numItems];
    //};
    typedef struct freeBytes
    {
        unsigned short startByte;
        unsigned short numConsecutiveBytes;
    } freeBytes;

    typedef struct freeGRFInfo
    {
        unsigned int magicStart;
        unsigned int numItems;
        freeBytes* data;
    } freeGRFInfo;

    // Compute free register information using vector for efficiency,
    // then convert to POS for passing back to gtpin.
    std::vector<std::pair<unsigned short, unsigned short>> vecFreeBytes;

    for (auto byte : globalFreeRegs)
    {
        if (vecFreeBytes.size() > 0)
        {
            auto& lastFree = vecFreeBytes.back();
            if (byte == (lastFree.first + lastFree.second))
            {
                lastFree.second += 1;
            }
            else
            {
                vecFreeBytes.push_back(std::make_pair(byte, 1));
            }
        }
        else
        {
            vecFreeBytes.push_back(std::make_pair(byte, 1));
        }
    }

    // Now convert vector to POS
    unsigned int numItems = (unsigned int)vecFreeBytes.size();
    freeGRFInfo* buffer = (freeGRFInfo*)malloc(numItems * sizeof(freeBytes) + sizeof(unsigned int)
        + sizeof(unsigned int) + sizeof(unsigned int));
    if (buffer)
    {
        buffer->numItems = numItems;
        buffer->magicStart = 0xDEADD00D;
        memcpy_s((unsigned char*)buffer + sizeof(unsigned int) + sizeof(unsigned int),
            numItems * sizeof(freeBytes), vecFreeBytes.data(), numItems * sizeof(freeBytes));
        unsigned int magicEnd = 0xDEADBEEF;
        memcpy_s((unsigned char*)buffer + sizeof(unsigned int) + sizeof(unsigned int) + (numItems * sizeof(freeBytes)),
            sizeof(magicEnd), &magicEnd, sizeof(magicEnd));

        // numItems - unsigned int
        // magicStart - unsigned int
        // magicEnd - unsigned int
        // data - numItems * sizeof(freeBytes)
        size = sizeof(unsigned int) + sizeof(unsigned int) + sizeof(unsigned int) + (numItems * sizeof(freeBytes));
    }

    return (void*)buffer;
}

void gtPinData::setGTPinInit(void* buffer)
{
    MUST_BE_TRUE(sizeof(gtpin::igc::igc_init_t) <= 200, "Check size of igc_init_t");
    gtpin_init = (gtpin::igc::igc_init_t*)buffer;

    if (gtpin_init->re_ra)
        kernel.getOptions()->setOption(vISA_ReRAPostSchedule, true);
    if (gtpin_init->grf_info)
        kernel.getOptions()->setOption(vISA_GetFreeGRFInfo, true);
}

template<typename T>
static void writeBuffer(
    std::vector<unsigned char>& buffer,
    unsigned& bufferSize,
    const T* t,
    unsigned numBytes)
{
    const unsigned char* data = (const unsigned char*)t;
    for (unsigned i = 0; i != numBytes; i++)
    {
        buffer.push_back(data[i]);
    }
    bufferSize += numBytes;
}

void* gtPinData::getGTPinInfoBuffer(unsigned &bufferSize)
{
    if (!gtpin_init && !gtpinInitFromL0)
    {
        bufferSize = 0;
        return nullptr;
    }
    gtpin::igc::igc_init_t t;
    std::vector<unsigned char> buffer;
    unsigned numTokens = 0;
    auto stackABI = kernel.fg.getIsStackCallFunc() || kernel.fg.getHasStackCalls();
    bufferSize = 0;

    memset(&t, 0, sizeof(t));

    t.version = gtpin::igc::GTPIN_IGC_INTERFACE_VERSION;
    t.igc_init_size = sizeof(t);
    if (gtpinInitFromL0)
    {
        if (kernel.getOption(vISA_GetFreeGRFInfo))
        {
            if (!stackABI)
                t.grf_info = 1;
            numTokens++;
        }

        if (kernel.getOption(vISA_GTPinReRA))
        {
            if (!stackABI)
                t.re_ra = 1;
        }

        if (kernel.getOptions()->getOption(vISA_GenerateDebugInfo))
            t.srcline_mapping = 1;

        if (kernel.getOptions()->getuInt32Option(vISA_GTPinScratchAreaSize) > 0)
        {
            t.scratch_area_size = getNumBytesScratchUse();
            numTokens++;
        }
    }
    else
    {
        t.version = std::min(gtpin_init->version, gtpin::igc::GTPIN_IGC_INTERFACE_VERSION);
        if (gtpin_init->grf_info)
        {
            if (!stackABI)
                t.grf_info = 1;
            numTokens++;
        }

        if (gtpin_init->re_ra)
        {
            if (!stackABI)
                t.re_ra = 1;
        }

        if (gtpin_init->srcline_mapping && kernel.getOptions()->getOption(vISA_GenerateDebugInfo))
            t.srcline_mapping = 1;

        if (gtpin_init->scratch_area_size > 0)
        {
            t.scratch_area_size = gtpin_init->scratch_area_size;
            numTokens++;
        }
    }

    // For payload offsets
    numTokens++;

    // Report #GRFs
    numTokens++;

    writeBuffer(buffer, bufferSize, &t, sizeof(t));
    writeBuffer(buffer, bufferSize, &numTokens, sizeof(uint32_t));

    if (t.grf_info)
    {
        // create token
        void* rerabuffer = nullptr;
        unsigned rerasize = 0;

        rerabuffer = getFreeGRFInfo(rerasize);

        gtpin::igc::igc_token_header_t th;
        th.token = gtpin::igc::GTPIN_IGC_TOKEN::GTPIN_IGC_TOKEN_GRF_INFO;
        th.token_size = sizeof(gtpin::igc::igc_token_header_t) + rerasize;

        // write token and data to buffer
        writeBuffer(buffer, bufferSize, &th, sizeof(th));
        writeBuffer(buffer, bufferSize, rerabuffer, rerasize);

        free(rerabuffer);
    }

    if (t.scratch_area_size)
    {
        gtpin::igc::igc_token_scratch_area_info_t scratchSlotData;
        scratchSlotData.scratch_area_size = t.scratch_area_size;
        scratchSlotData.scratch_area_offset = nextScratchFree;

        // gtpin scratch slots are beyond spill memory
        scratchSlotData.token = gtpin::igc::GTPIN_IGC_TOKEN_SCRATCH_AREA_INFO;
        scratchSlotData.token_size = sizeof(scratchSlotData);

        writeBuffer(buffer, bufferSize, &scratchSlotData, sizeof(scratchSlotData));
    }

    {
        // Write payload offsets
        gtpin::igc::igc_token_kernel_start_info_t offsets;
        offsets.token = gtpin::igc::GTPIN_IGC_TOKEN_KERNEL_START_INFO;
        offsets.per_thread_prolog_size = getPerThreadNextOff();
        offsets.cross_thread_prolog_size = getCrossThreadNextOff() - offsets.per_thread_prolog_size;
        offsets.token_size = sizeof(offsets);
        writeBuffer(buffer, bufferSize, &offsets, sizeof(offsets));
    }

    {
        // Report num GRFs
        gtpin::igc::igc_token_num_grf_regs_t numGRFs;
        numGRFs.token = gtpin::igc::GTPIN_IGC_TOKEN_NUM_GRF_REGS;
        numGRFs.token_size = sizeof(numGRFs);
        numGRFs.num_grf_regs = kernel.getNumRegTotal();
        writeBuffer(buffer, bufferSize, &numGRFs, sizeof(numGRFs));
    }

    void* gtpinBuffer = allocCodeBlock(bufferSize);

    memcpy_s(gtpinBuffer, bufferSize, (const void*)(buffer.data()), bufferSize);

    // Dump buffer with shader dumps
    if (kernel.getOption(vISA_outputToFile))
    {
        auto asmName = kernel.getOptions()->getOptionCstr(VISA_AsmFileName);
        if (asmName)
        {
            std::ofstream ofInit;
            std::stringstream ssInit;
            ssInit << std::string(asmName) << ".gtpin_igc_init";
            ofInit.open(ssInit.str(), std::ofstream::binary);
            if (gtpin_init)
            {
                ofInit.write((const char*)gtpin_init, sizeof(*gtpin_init));
            }
            ofInit.close();

            std::ofstream ofInfo;
            std::stringstream ssInfo;
            ssInfo << std::string(asmName) << ".gtpin_igc_info";
            ofInfo.open(ssInfo.str(), std::ofstream::binary);
            if (gtpinBuffer)
            {
                ofInfo.write((const char*)gtpinBuffer, bufferSize);
            }
            ofInfo.close();
        }
    }

    return gtpinBuffer;
}

uint32_t gtPinData::getNumBytesScratchUse() const
{
    if (gtpin_init)
    {
        return gtpin_init->scratch_area_size;
    }
    else if (isGTPinInitFromL0())
    {
        return kernel.getOptions()->getuInt32Option(vISA_GTPinScratchAreaSize);
    }
    return 0;
}

static unsigned getBinOffsetNextBB(G4_Kernel& kernel, G4_BB* bb)
{
    // Given bb, return binary offset of first
    // non-label of lexically following bb.
    G4_BB* nextBB = nullptr;
    for (auto it = kernel.fg.begin(); it != kernel.fg.end(); it++)
    {
        auto curBB = (*it);
        if (curBB == bb && it != kernel.fg.end())
        {
            it++;
            nextBB = (*it);
        }
    }

    if (!nextBB)
        return 0;

    auto iter = std::find_if(nextBB->begin(), nextBB->end(), [](G4_INST* inst) { return !inst->isLabel(); });
    assert(iter != nextBB->end() && "execpt at least one non-label inst in second BB");
    return (unsigned)(*iter)->getGenOffset();
}

unsigned gtPinData::getCrossThreadNextOff() const
{
    return getBinOffsetNextBB(kernel, crossThreadPayloadBB);
}

unsigned gtPinData::getPerThreadNextOff() const
{
    return getBinOffsetNextBB(kernel, perThreadPayloadBB);
}



void G4_Kernel::computeChannelSlicing()
{
    G4_ExecSize simdSize = getSimdSize();
    channelSliced = true;

    if (simdSize == g4::SIMD8 || simdSize == g4::SIMD16)
    {
        // SIMD8/16 kernels are not sliced
        channelSliced = false;
        return;
    }

    // .dcl V1 size = 128 bytes
    // op (16|M0) V1(0,0)     ..
    // op (16|M16) V1(2,0)    ..
    // For above sequence, return 32. Instruction
    // is broken in to 2 only due to hw restriction.
    // Allocation of dcl is still as if it were a
    // SIMD32 kernel.

    // Store emask bits that are ever used to define a variable
    std::unordered_map<G4_Declare*, std::bitset<32>> emaskRef;
    for (auto bb : fg)
    {
        for (auto inst : *bb)
        {
            if (inst->isSend())
                continue;

            auto dst = inst->getDst();
            if (!dst || !dst->getTopDcl() ||
                dst->getHorzStride() != 1)
                continue;

            if (inst->isWriteEnableInst())
                continue;

            auto regFileKind = dst->getTopDcl()->getRegFile();
            if (regFileKind != G4_RegFileKind::G4_GRF && regFileKind != G4_RegFileKind::G4_INPUT)
                continue;

            if (dst->getTopDcl()->getByteSize() <= dst->getTypeSize() * (unsigned)simdSize)
                continue;

            auto emaskOffStart = inst->getMaskOffset();

            // Reset all bits on first encounter of dcl
            if (emaskRef.find(dst->getTopDcl()) == emaskRef.end())
                emaskRef[dst->getTopDcl()].reset();

            // Set bits based on which EM bits are used in the def
            for (unsigned i = emaskOffStart; i != (emaskOffStart + inst->getExecSize()); i++)
            {
                emaskRef[dst->getTopDcl()].set(i);
            }
        }
    }

    // Check whether any variable's emask usage straddles across lower and upper 16 bits
    for (auto& emRefs : emaskRef)
    {
        auto& bits = emRefs.second;
        auto num = bits.to_ulong();

        // Check whether any lower 16 and upper 16 bits are set
        if (((num & 0xffff) != 0) && ((num & 0xffff0000) != 0))
        {
            channelSliced = false;
            return;
        }
    }

    return;
}

void G4_Kernel::calculateSimdSize()
{
    // Iterate over all instructions in kernel to check
    // whether default execution size of kernel is
    // SIMD8/16. This is required for knowing alignment
    // to use for GRF candidates.

    // only do it once per kernel, as we should not introduce inst with larger simd size than in the input
    if (simdSize.value != 0)
    {
        return;
    }

    // First, get simdsize from attribute (0 : not given)
    // If not 0|8|16|32, wrong value from attribute.
    simdSize = G4_ExecSize((unsigned)m_kernelAttrs->getInt32KernelAttr(Attributes::ATTR_SimdSize));
    if (simdSize != g4::SIMD8 && simdSize != g4::SIMD16 && simdSize != g4::SIMD32)
    {
        assert(simdSize.value == 0 && "vISA: wrong value for SimdSize attribute");
        simdSize = g4::SIMD8;

        for (auto bb : fg)
        {
            for (auto inst : *bb)
            {
                // do not consider send since for certain messages we have to set its execution size
                // to 16 even in simd8 shaders
                if (!inst->isLabel() && !inst->isSend())
                {
                    uint32_t size = inst->getMaskOffset() + inst->getExecSize();
                    if (size > 16)
                    {
                        simdSize = g4::SIMD32;
                        break;
                    }
                    else if (size > 8)
                    {
                        simdSize = g4::SIMD16;
                    }
                }
            }
            if (simdSize == g4::SIMD32)
                break;
        }
    }

    if (GlobalRA::useGenericAugAlign())
        computeChannelSlicing();
}

//
// Updates kernel's related structures based on number of threads.
//
void G4_Kernel::updateKernelByNumThreads(int nThreads)
{
    if (numThreads == nThreads)
        return;

    numThreads = nThreads;

    // Scale number of GRFs, Acc, SWSB tokens.
    setKernelParameters();

    // Update physical register pool
    fg.builder->rebuildPhyRegPool(getNumRegTotal());
}

//
// Evaluate AddrExp/AddrExpList to Imm
//
void G4_Kernel::evalAddrExp()
{
    for (std::list<G4_BB*>::iterator it = fg.begin(), itEnd = fg.end();
        it != itEnd; ++it)
    {
        G4_BB* bb = (*it);

        for (INST_LIST_ITER i = bb->begin(), iEnd = bb->end(); i != iEnd; i++)
        {
            G4_INST* inst = (*i);

            //
            // process each source operand
            //
            for (unsigned j = 0; j < G4_MAX_SRCS; j++)
            {
                G4_Operand* opnd = inst->getSrc(j);

                if (opnd == NULL) continue;

                if (opnd->isAddrExp())
                {
                    int val = opnd->asAddrExp()->eval();
                    G4_Type ty = opnd->asAddrExp()->getType();

                    G4_Imm* imm = fg.builder->createImm(val, ty);
                    inst->setSrc(imm, j);
                }
            }
        }
    }
}

void G4_Kernel::dump() const
{
    fg.print(std::cerr);
}

void G4_Kernel::dumptofile(const char* Filename) const
{
    fg.dumptofile(Filename);
}

void G4_Kernel::dumpDotFileImportant(const char* suffix)
{
    if (m_options->getOption(vISA_DumpDot))
        dumpDotFileInternal(suffix);
    if (m_options->getOption(vISA_DumpPasses) || m_options->getuInt32Option(vISA_DumpPassesSubset) >= 1)
        dumpPassInternal(suffix);
}

void G4_Kernel::dumpDotFile(const char* suffix)
{
    if (m_options->getOption(vISA_DumpDot))
        dumpDotFileInternal(suffix);
    if (m_options->getOption(vISA_DumpPasses) || m_options->getuInt32Option(vISA_DumpPassesSubset) >= 2)
        dumpPassInternal(suffix);
}

// FIX: this needs to here because of the above static thread-local variable
extern _THREAD const char* g4_prevFilename;
extern _THREAD int g4_prevSrcLineNo;

static std::vector<std::string> split(
    const std::string & str, const char * delimiter)
{
    std::vector<std::string> v;
    std::string::size_type start = 0;

    for (auto pos = str.find_first_of(delimiter, start);
        pos != std::string::npos;
        start = pos + 1, pos = str.find_first_of(delimiter, start))
    {
        if (pos != start)
        {
            v.emplace_back(str, start, pos - start);
        }
    }

    if (start < str.length())
        v.emplace_back(str, start, str.length() - start);
    return v;
}

static iga_gen_t getIGAPlatform()
{
    iga_gen_t platform = IGA_GEN_INVALID;
    switch (getGenxPlatform())
    {
    case GENX_BDW: platform = IGA_GEN8; break;
    case GENX_CHV: platform = IGA_GEN8lp; break;
    case GENX_SKL: platform = IGA_GEN9; break;
    case GENX_BXT: platform = IGA_GEN9lp; break;
    case GENX_ICLLP: platform = IGA_GEN11; break;
    case GENX_TGLLP:platform = IGA_GEN12p1; break;
    default:
        break;
    }

    return platform;
}

void G4_Kernel::emit_asm(
    std::ostream& output, bool beforeRegAlloc,
    void * binary, uint32_t binarySize)
{
    static const char* const RATypeString[] {
        RA_TYPE(STRINGIFY)
    };

    //
    // for GTGPU lib release, don't dump out asm
    //
#ifdef NDEBUG
#ifdef GTGPU_LIB
    return;
#endif
#endif
    bool newAsm = false;
    if (m_options->getOption(vISA_dumpNewSyntax) && !(binary == NULL || binarySize == 0))
    {
        newAsm = true;
    }

    if (!m_options->getOption(vISA_StripComments))
    {
        output << "//.kernel ";
        if (name != NULL)
        {
            // some 3D kernels do not have name
            output << name;
        }

        output << "\n" << "//.platform " << getGenxPlatformString(getGenxPlatform());
        output << "\n" << "//.thread_config " << "numGRF=" << numRegTotal << ", numAcc=" << numAcc;
        if (fg.builder->hasSWSB())
        {
            output << ", numSWSB=" << numSWSBTokens;
        }
        output << "\n" << "//.options_string \"" << m_options->getUserArgString().str() << "\"";
        output << "\n" << "//.full_options \"" << m_options->getFullArgString() << "\"";
        output << "\n" << "//.instCount " << asmInstCount;
        output << "\n//.RA type\t" << RATypeString[RAType];

        if (auto jitInfo = fg.builder->getJitInfo())
        {
            if (jitInfo->numGRFUsed != 0)
            {
                output << "\n" << "//.GRF count " << jitInfo->numGRFUsed;
            }
            if (jitInfo->spillMemUsed > 0)
            {
                output << "\n" << "//.spill size " << jitInfo->spillMemUsed;
            }
            if (jitInfo->numGRFSpillFill > 0)
            {
                output << "\n" << "//.spill GRF ref count " << jitInfo->numGRFSpillFill;
            }
            if (jitInfo->numFlagSpillStore > 0)
            {
                output << "\n//.spill flag store " << jitInfo->numFlagSpillStore;
                output << "\n//.spill flag load " << jitInfo->numFlagSpillLoad;
            }
        }

        auto privateMemSize = getInt32KernelAttr(Attributes::ATTR_SpillMemOffset);
        if (privateMemSize != 0)
        {
            output << "\n.//.private memory size " << privateMemSize;
        }
        output << "\n\n";

        //Step2: emit declares (as needed)
        //
        // firstly, emit RA declare as comments or code depends on Options::symbolReg
        // we check if the register allocation is successful here
        //

        for (auto dcl : Declares)
        {
            dcl->emit(output);
        }
        output << std::endl;

        auto fmtHex = [](int i) {
            std::stringstream ss;
            ss << "0x" << std::hex << std::uppercase << i;
            return ss.str();
        };

        const unsigned inputCount = fg.builder->getInputCount();
        std::vector<std::string> argNames;
        size_t maxNameLen = 8;
        for (unsigned id = 0; id < inputCount; id++)
        {
            const input_info_t* ii = fg.builder->getInputArg(id);
            std::stringstream ss;
            if (ii->dcl && ii->dcl->getName()) {
                ss << ii->dcl->getName();
            } else {
                ss << "__unnamed" << (id + 1);
            }
            argNames.push_back(ss.str());
            maxNameLen = std::max(maxNameLen, argNames.back().size());
        }

        // emit input location and size
        output << "// .inputs" << std::endl;
        const size_t COLW_IDENT = maxNameLen;
        static const size_t COLW_TYPE = 8;
        static const size_t COLW_SIZE = 6;
        static const size_t COLW_AT = 8;
        static const size_t COLW_CLASS = 10;

        std::stringstream bordss;
        bordss << "// ";
        bordss << '+'; bordss << std::setfill('-') << std::setw(COLW_IDENT + 2) << "";
        bordss << '+'; bordss << std::setfill('-') << std::setw(COLW_TYPE + 2) << "";
        bordss << '+'; bordss << std::setfill('-') << std::setw(COLW_SIZE + 2) << "";
        bordss << '+'; bordss << std::setfill('-') << std::setw(COLW_AT + 2) << "";
        bordss << '+'; bordss << std::setfill('-') << std::setw(COLW_CLASS + 2) << "";
        bordss << '+' << std::endl;
        std::string border = bordss.str();

        output << border;
        output <<
            "//" <<
            " | " << std::left << std::setw(COLW_IDENT) << "id" <<
            " | " << std::left << std::setw(COLW_TYPE) << "type" <<
            " | " << std::right << std::setw(COLW_SIZE) << "bytes" <<
            " | " << std::left << std::setw(COLW_AT) << "at" <<
            " | " << std::left << std::setw(COLW_CLASS) << "class" <<
            " |" << std::endl;
        output << border;

        const unsigned grfSize = getGRFSize();
        for (unsigned id = 0; id < inputCount; id++)
        {
            const input_info_t* input_info = fg.builder->getInputArg(id);
            //
            output << "//";
            //
            // id
            output <<
                " | " << std::left << std::setw(COLW_IDENT) << argNames[id];
            //
            // type and length
            //   e.g. :uq x 16
            const G4_Declare *dcl = input_info->dcl;
            std::stringstream sstype;
            if (dcl) {
                switch (dcl->getElemType()) {
                case Type_B: sstype << ":b"; break;
                case Type_W: sstype << ":w"; break;
                case Type_D: sstype << ":d"; break;
                case Type_Q: sstype << ":q"; break;
                case Type_V: sstype << ":v"; break;
                case Type_UB: sstype << ":ub"; break;
                case Type_UW: sstype << ":uw"; break;
                case Type_UD: sstype << ":ud"; break;
                case Type_UQ: sstype << ":uq"; break;
                case Type_UV: sstype << ":uv"; break;
                    //
                case Type_F:  sstype << ":f"; break;
                case Type_HF: sstype << ":hf"; break;
                case Type_DF: sstype << ":df"; break;
                case Type_NF: sstype << ":nf"; break;
                default:
                    sstype << fmtHex((int)dcl->getElemType()) << "?";
                    break;
                }
                if (dcl->getTotalElems() != 1)
                    sstype << " x " << dcl->getTotalElems();
            } else {
                sstype << "?";
            }
            output << " | " << std::left << std::setw(COLW_TYPE) << sstype.str();
            //
            // size
            output << " | " << std::right << std::setw(COLW_SIZE) << std::dec << input_info->size;

            // location
            unsigned reg = input_info->offset / grfSize,
                subRegBytes = input_info->offset % grfSize;
            std::stringstream ssloc;
            ssloc << "r" << reg;
            if (subRegBytes != 0)
                ssloc << "+" << subRegBytes;
            output << " | " << std::left << std::setw(COLW_AT) << ssloc.str();

            // class
            std::string inpcls;
            switch (input_info->getInputClass()) {
            case INPUT_GENERAL: inpcls = "general"; break;
            case INPUT_SAMPLER: inpcls = "sampler"; break;
            case INPUT_SURFACE: inpcls = "surface"; break;
            default: inpcls = fmtHex((int)input_info->getInputClass()); break;
            }
            output << " | " << std::left << std::setw(COLW_CLASS) << inpcls;
            //
            output << " |" << std::endl;
        }
        output << border;
        output << std::endl;

        if (getPlatformGeneration(getGenxPlatform()) < PlatformGen::XE)
        {
            fg.BCStats.clear();
        }
        else
        {
            fg.XeBCStats.clear();
        }
        fg.numRMWs = 0;
    }


    // Set this to NULL to always print filename for each kernel
    g4_prevFilename = nullptr;
    g4_prevSrcLineNo = 0;

    if (!newAsm)
    {
        //Step3: emit code and subroutines
        output << std::endl << ".code";
    }

    if (newAsm)
    {
        char stringBuffer[512];
        uint32_t pc = 0;
        output << std::endl;
        bool dissasemblyFailed = false;
#define ERROR_STRING_MAX_LENGTH 1024*16
        char* errBuf = new char[ERROR_STRING_MAX_LENGTH];

        KernelView kView(
            getIGAPlatform(), binary, binarySize,
            GetIGASWSBEncodeMode(*fg.builder),
            errBuf, ERROR_STRING_MAX_LENGTH);
        dissasemblyFailed = !kView.decodeSucceeded();

        std::string igaErrMsgs;
        std::vector<std::string> igaErrMsgsVector;
        std::map<int, std::string> errorToStringMap;
        if (dissasemblyFailed)
        {
            std::cerr << "Failed to decode binary for asm output. Please report the issue and try disabling IGA disassembler for now." << std::endl;
            igaErrMsgs = std::string(errBuf);
            igaErrMsgsVector = split(igaErrMsgs, "\n");
            for (auto msg : igaErrMsgsVector)
            {
                auto pos = msg.find("ERROR");
                if (pos != std::string::npos)
                {
                    std::cerr << msg.c_str() << std::endl;
                    std::vector<std::string> aString = split(msg, " ");
                    for (auto token : aString)
                    {
                        if (token.find_first_of("0123456789") != std::string::npos)
                        {
                            int errorPC = std::atoi(token.c_str());
                            errorToStringMap[errorPC] = msg;
                            break;
                        }
                    }
                }
            }
        }

        //
        // For label, generate a label with uniqueLabel as prefix (required by some tools).
        // We do so by using labeler callback.  If uniqueLabels is not present, use iga's
        // default label.  For example,
        //   Without option -uniqueLabels:
        //      generating default label,   L1234
        //   With option -uniqueLabels <sth>:
        //      generating label with <sth> as prefix, <sth>_L1234
        //
        const char* labelPrefix = nullptr;
        if (m_options->getOption(vISA_UniqueLabels))
        {
            m_options->getOption(vISA_LabelStr, labelPrefix);
        }
        typedef struct {
            char m_labelString[128]; // label string for uniqueLabels
            char* m_labelPrefix;    // label prefix
            char m_tmpString[64];   // tmp storage, default label
            KernelView *m_pkView;   // handle to KernelView object.
        } lambdaArg_t;
        lambdaArg_t lambdaArg;
        lambdaArg.m_labelPrefix = const_cast<char*>(labelPrefix);
        lambdaArg.m_pkView = &kView;

        // Labeler callback function.
        auto labelerLambda = [](int32_t pc, void *data) -> const char*
        {
            lambdaArg_t *pArg = (lambdaArg_t *)data;
            char* tmpString = pArg->m_tmpString;
            char* labelString = pArg->m_labelString;

            pArg->m_pkView->getDefaultLabelName(pc, tmpString, 64);
            const char *retString;
            if (pArg->m_labelPrefix)
            {
                SNPRINTF(labelString, 128, "%s_%s", (const char*)pArg->m_labelPrefix, tmpString);
                retString = labelString;
            }
            else
            {
                retString = tmpString;
            }
            return retString;
        };

        int suppressRegs[5];
        int lastRegs[3];
        for (int i = 0; i < 3; i++)
        {
            suppressRegs[i] = -1;
            lastRegs[i] = -1;
        }

        suppressRegs[4] = 0;

        uint32_t lastLabelPC = 0;
        for (BB_LIST_ITER itBB = fg.begin(); itBB != fg.end(); ++itBB)
        {
            for (INST_LIST_ITER itInst = (*itBB)->begin(); itInst != (*itBB)->end(); ++itInst)
            {

                bool isInstTarget = kView.isInstTarget(pc);
                if (isInstTarget)
                {
                    const char* stringLabel = labelerLambda(pc, (void *)&lambdaArg);

                    if ((*itInst)->isLabel())
                    {
                        output << "\n\n//" << (*itInst)->getLabelStr() << ":";
                        //handling the case where there is an empty block with just label.
                        //this way we don't print IGA label twice
                        if ((*itBB)->size() == 1)
                        {
                            break;
                        }
                    }

                    //preventing the case where there are two labels in G4 IR so duplicate IGA labels are printed
                    //then parser asserts.
                    /*
                    //label_cf_20_particle:
                    L3152:

                    //label12_particle:
                    L3152:

                    endif (32|M0)                        L3168                            // [3152]: #218 //:$239:%332
                    */
                    if (pc != lastLabelPC || pc == 0)
                    {
                        output << "\n" << stringLabel << ":" << std::endl;
                        lastLabelPC = pc;
                    }

                    if ((*itInst)->isLabel())
                    {
                        ++itInst;
                        //G4_IR has instruction for label.
                        if (itInst == (*itBB)->end())
                        {
                            break;
                        }
                    }
                }
                else if ((*itInst)->isLabel())
                {
                    output << "\n\n//" << (*itInst)->getLabelStr() << ":";
                    continue;
                }

                if (!(getOptions()->getOption(vISA_disableInstDebugInfo)))
                {
                    (*itBB)->emitInstructionInfo(output, itInst);
                }
                output << std::endl;

                auto errString = errorToStringMap.find(pc);
                if (errString != errorToStringMap.end())
                {
                    output << "// " << errString->second.c_str() << std::endl;
                    output << "// Text representation might not be correct" << std::endl;
                }

                static const uint32_t IGA_FMT_OPTS =
                    IGA_FORMATTING_OPT_PRINT_LDST
                    // | IGA_FORMATTING_OPT_SYNTAX_EXTS
                    ;
                kView.getInstSyntax(
                    pc,
                    stringBuffer, 512,
                    IGA_FMT_OPTS,
                    labelerLambda, (void*)&lambdaArg);
                pc += kView.getInstSize(pc);

                (*itBB)->emitBasicInstructionIga(stringBuffer, output, itInst, suppressRegs, lastRegs);
            }
        }

        delete [] errBuf;
    }
    else
    {
        for (BB_LIST_ITER it = fg.begin(); it != fg.end(); ++it)
        {
            output << std::endl;
            (*it)->emit(output);

        }
    }

    if (!newAsm)
    {
        //Step4: emit clean-up.
        output << std::endl;
        output << ".end_code" << std::endl;
        output << ".end_kernel" << std::endl;
        output << std::endl;
    }
    if (newAsm)
    {
        if (getPlatformGeneration(getGenxPlatform()) >= PlatformGen::XE)
        {
            output << "\n\n//.BankConflicts: " <<  fg.XeBCStats.BCNum << "\n";
            output << "//.sameBankConflicts: " <<  fg.XeBCStats.sameBankConflicts << "\n";
            output << "//.simd16ReadSuppression: " <<  fg.XeBCStats.simd16ReadSuppression << "\n";
            output << "//.twoSrcBankConflicts: " <<  fg.XeBCStats.twoSrcBC << "\n";
            output << "//.SIMD8s: " <<  fg.XeBCStats.simd8 << "\n//\n";
            output << "//.RMWs: " << fg.numRMWs << "\n//\n";
        }
        else
        {
            output << "// Bank Conflict Statistics: \n";
            output << "// -- GOOD: " << fg.BCStats.NumOfGoodInsts << "\n";
            output << "// --  BAD: " << fg.BCStats.NumOfBadInsts << "\n";
            output << "// --   OK: " << fg.BCStats.NumOfOKInsts << "\n";
        }
    }
}

void G4_Kernel::emit_RegInfo()
{
    const char* asmName = nullptr;
    getOptions()->getOption(VISA_AsmFileName, asmName);
    const char* asmNameEmpty = "";
    if( !asmName )
    {
        asmName = asmNameEmpty;
    }

    std::string dumpFileName = std::string(asmName) + ".reginfo";
    std::fstream ofile(dumpFileName, std::ios::out);

    emit_RegInfoKernel(ofile);

    ofile.close();
}

void G4_Kernel::emit_RegInfoKernel(std::ostream& output)
{
    output << "//.platform " << getGenxPlatformString(fg.builder->getPlatform());
    output << "\n" << "//.kernel ID 0x" << std::hex << getKernelID() << "\n";
    output << std::dec << "\n";
    int instOffset = 0;

    for (BB_LIST_ITER itBB = fg.begin(); itBB != fg.end(); ++itBB)
    {
        for (INST_LIST_ITER itInst = (*itBB)->begin(); itInst != (*itBB)->end(); ++itInst)
        {
            G4_INST* inst = (*itInst);
            if (inst->isLabel())
            {
                continue;
            }
            if (inst->getLexicalId() == -1)
            {
                continue;
            }

            (*itBB)->emitRegInfo(output, inst, instOffset);
            instOffset += inst->isCompactedInst() ? 8 : 16;
        }
    }
    return;
}

KernelDebugInfo* G4_Kernel::getKernelDebugInfo()
{
    if (kernelDbgInfo == nullptr)
    {
        kernelDbgInfo = new(fg.mem)KernelDebugInfo();
    }

    return kernelDbgInfo;
}

unsigned G4_Kernel::getStackCallStartReg() const
{
    // Last 3 GRFs to be used as scratch
    unsigned totalGRFs = getNumRegTotal();
    unsigned startReg = totalGRFs - numReservedABIGRF();
    return startReg;
}
unsigned G4_Kernel::calleeSaveStart() const
{
    return getCallerSaveLastGRF() + 1;
}
unsigned G4_Kernel::getNumCalleeSaveRegs() const
{
    unsigned totalGRFs = getNumRegTotal();
    return totalGRFs - calleeSaveStart() - numReservedABIGRF();
}

//
// rename non-root declares to their root decl name to make
// it easier to read IR dump
//
void G4_Kernel::renameAliasDeclares()
{
#if _DEBUG
    for (auto dcl : Declares)
    {
        if (dcl->getAliasDeclare())
        {
            uint32_t offset = 0;
            G4_Declare* rootDcl = dcl->getRootDeclare(offset);
            std::string newName(rootDcl->getName());
            if (rootDcl->getElemType() != dcl->getElemType())
            {
                newName += "_";
                newName += TypeSymbol(dcl->getElemType());
            }
            if (offset != 0)
            {
                newName += "_" + std::to_string(offset);
            }
            dcl->setName(fg.builder->getNameString(fg.mem, 64, "%s", newName.c_str()));
        }
    }
#endif
}

//
// perform relocation for every entry in the allocation table
//
void G4_Kernel::doRelocation(void* binary, uint32_t binarySize)
{
    for (auto&& entry : relocationTable)
    {
        entry.doRelocation(*this, binary, binarySize);
    }
}

G4_INST* G4_Kernel::getFirstNonLabelInst() const
{
    for (auto I = fg.cbegin(), E = fg.cend(); I != E; ++I)
    {
        auto bb = *I;
        G4_INST* firstInst = bb->getFirstInst();
        if (firstInst)
        {
            return firstInst;
        }
    }
    // empty kernel
    return nullptr;
}

VarSplitPass* G4_Kernel::getVarSplitPass()
{
    if (varSplitPass)
        return varSplitPass;

    varSplitPass = new VarSplitPass(*this);

    return varSplitPass;
}

void G4_Kernel::setKernelParameters()
{
    unsigned overrideGRFNum = 0;
    unsigned overrideNumThreads = 0;

    TARGET_PLATFORM platform = getGenxPlatform();
    overrideGRFNum = m_options->getuInt32Option(vISA_TotalGRFNum);


    // Set the number of GRFs
    if (overrideGRFNum > 0)
    {
        // User-provided number of GRFs
        unsigned Val = m_options->getuInt32Option(vISA_GRFNumToUse);
        if (Val > 0)
        {
            numRegTotal = std::min(Val, overrideGRFNum);
        }
        else
        {
            numRegTotal = overrideGRFNum;
        }
        callerSaveLastGRF = ((overrideGRFNum - 8) / 2) - 1;
    }
    else
    {
        // Default value for all other platforms
        numRegTotal = 128;
        callerSaveLastGRF = ((numRegTotal - 8) / 2) - 1;
    }
    // For safety update TotalGRFNum, there may be some uses for this vISA option
    m_options->setOption(vISA_TotalGRFNum, numRegTotal);

    // Set the number of SWSB tokens
    unsigned overrideNumSWSB = m_options->getuInt32Option(vISA_SWSBTokenNum);
    if (overrideNumSWSB > 0)
    {
        // User-provided number of SWSB tokens
        numSWSBTokens = overrideNumSWSB;
    }
    else
    {
        // Default value based on platform
        switch (platform)
        {
        default:
            numSWSBTokens = 16;
        }
    }


    // Set the number of Acc. They are in the unit of GRFs (i.e., 1 accumulator is the same size as 1 GRF)
    unsigned overrideNumAcc = m_options->getuInt32Option(vISA_numGeneralAcc);
    if (overrideNumAcc > 0)
    {
        // User-provided number of Acc
        numAcc = overrideNumAcc;
    }
    else
    {
        // Default value based on platform
        switch (platform)
        {
        default:
            numAcc = 2;
        }
    }

    // Set number of threads if it was not defined before
    if (numThreads == 0)
    {
        if (overrideNumThreads > 0)
        {
            numThreads = overrideNumThreads;
        }
        else
        {
            switch (platform)
            {
            default:
                numThreads = 7;
            }
        }
    }
}

G4_Kernel::G4_Kernel(INST_LIST_NODE_ALLOCATOR& alloc,
    Mem_Manager& m, Options* options, Attributes* anAttr,
    unsigned char major, unsigned char minor)
    : m_options(options), m_kernelAttrs(anAttr), RAType(RA_Type::UNKNOWN_RA),
    asmInstCount(0), kernelID(0), fg(alloc, this, m),
    major_version(major), minor_version(minor)
{
    ASSERT_USER(
        major < COMMON_ISA_MAJOR_VER ||
        (major == COMMON_ISA_MAJOR_VER && minor <= COMMON_ISA_MINOR_VER),
        "CISA version not supported by this JIT-compiler");


    name = NULL;
    numThreads = 0;
    hasAddrTaken = false;
    kernelDbgInfo = nullptr;
    if (options->getOption(vISAOptions::vISA_ReRAPostSchedule) ||
        options->getOption(vISAOptions::vISA_GetFreeGRFInfo) ||
        options->getuInt32Option(vISAOptions::vISA_GTPinScratchAreaSize))
    {
        allocGTPinData();
    } else {
        gtPinInfo = nullptr;
    }

    setKernelParameters();
}



// prevent overwriting dump file and indicate compilation order with dump serial number
static _THREAD int dotDumpCount = 0;

//
// This routine dumps out the dot file of the control flow graph along with instructions.
// dot is drawing graph tool from AT&T.
//
void G4_Kernel::dumpDotFileInternal(const char* appendix)
{

    MUST_BE_TRUE(appendix != NULL, ERROR_INTERNAL_ARGUMENT);
    if (!m_options->getOption(vISA_DumpDot))  // skip dumping dot files
        return;

    std::stringstream ss;
    ss << (name ? name : "UnknownKernel") <<
        "." << std::setfill('0') << std::setw(3) <<
        dotDumpCount++ << "." << appendix << ".dot";

    std::string fname(ss.str());
    fname = sanitizePathString(fname);

    std::fstream ofile(fname, std::ios::out);
    if (!ofile)
    {
        MUST_BE_TRUE(false, ERROR_FILE_READ(fname));
    }
    //
    // write digraph KernelName {"
    //          size = "8, 10";
    //
    const char* asmFileName = NULL;
    m_options->getOption(VISA_AsmFileName, asmFileName);
    if (asmFileName == NULL)
        ofile << "digraph UnknownKernel" << " {" << std::endl;
    else
        ofile << "digraph " << asmFileName << " {" << std::endl;
    //
    // keep the graph width 8, estimate a reasonable graph height
    //
    const unsigned itemPerPage = 64;                                        // 60 instructions per Letter page
    unsigned totalItem = (unsigned)Declares.size();
    for (std::list<G4_BB*>::iterator it = fg.begin(); it != fg.end(); ++it)
        totalItem += ((unsigned)(*it)->size());
    totalItem += (unsigned)fg.size();
    float graphHeight = (float)totalItem / itemPerPage;
    graphHeight = graphHeight < 100.0f ? 100.0f : graphHeight;    // minimal size: Letter
    ofile << "\n\t// Setup\n";
    ofile << "\tsize = \"80.0, " << graphHeight << "\";\n";
    ofile << "\tpage= \"80.5, 110\";\n";
    ofile << "\tpagedir=\"TL\";\n";
    //
    // dump out declare information
    //     Declare [label="
    //
    //if (name == NULL)
    //  ofile << "\tDeclares [shape=record, label=\"{kernel:UnknownKernel" << " | ";
    //else
    //  ofile << "\tDeclares [shape=record, label=\"{kernel:" << name << " | ";
    //for (std::list<G4_Declare*>::iterator it = Declares.begin(); it != Declares.end(); ++it)
    //{
    //  (*it)->emit(ofile, true, Options::symbolReg);   // Solve the DumpDot error on representing <>
    //
    //  ofile << "\\l";  // left adjusted
    //}
    //ofile << "}\"];" << std::endl;
    //
    // dump out flow graph
    //
    for (std::list<G4_BB*>::iterator it = fg.begin(); it != fg.end(); ++it)
    {
        G4_BB* bb = (*it);
        //
        // write:   BB0 [shape=plaintext, label=<
        //                      <TABLE BORDER="0" CELLBORDER="1" CELLSPACING="0">
        //                          <TR><TD ALIGN="CENTER">BB0: TestRA_Dot</TD></TR>
        //                          <TR><TD>
        //                              <TABLE BORDER="0" CELLBORDER="0" CELLSPACING="0">
        //                                  <TR><TD ALIGN="LEFT">TestRA_Dot:</TD></TR>
        //                                  <TR><TD ALIGN="LEFT"><FONT color="red">add (8) Region(0,0)[1] Region(0,0)[8;8,1] PAYLOAD(0,0)[8;8,1] [NoMask]</FONT></TD></TR>
        //                              </TABLE>
        //                          </TD></TR>
        //                      </TABLE>>];
        // print out label if the first inst is a label inst
        //
        ofile << "\t";
        bb->writeBBId(ofile);
        ofile << " [shape=plaintext, label=<" << std::endl;
        ofile << "\t\t\t    <TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\">" << std::endl;
        ofile << "\t\t\t\t<TR><TD ALIGN=\"CENTER\">";
        bb->writeBBId(ofile);
        ofile << ": ";

        if (!bb->empty() && bb->front()->isLabel())
        {
            bb->front()->getSrc(0)->emit(ofile);
        }
        ofile << "</TD></TR>" << std::endl;
        //emit all instructions within basic block
        ofile << "\t\t\t\t<TR><TD>" << std::endl;

        if (!bb->empty())
        {
            ofile << "\t\t\t\t\t    <TABLE BORDER=\"0\" CELLBORDER=\"0\" CELLSPACING=\"0\">" << std::endl;
            for (INST_LIST_ITER i = bb->begin(); i != bb->end(); i++)
            {
                //
                // detect if there is spill code first, set different color for it
                //
                std::string fontColor = "black";
                //
                // emit the instruction
                //
                ofile << "\t\t\t\t\t\t<TR><TD ALIGN=\"LEFT\"><FONT color=\"" << fontColor << "\">";
                std::ostringstream os;
                (*i)->emit(os, m_options->getOption(vISA_SymbolReg), true);
                std::string dotStr(os.str());
                //TODO: dot doesn't like '<', '>', '{', or '}' (and '&') this code below is a hack. need to replace with delimiters.
                //std::replace_if(dotStr.begin(), dotStr.end(), bind2nd(equal_to<char>(), '<'), '[');
                std::replace_if(dotStr.begin(), dotStr.end(), std::bind(std::equal_to<char>(), std::placeholders::_1, '<'), '[');
                std::replace_if(dotStr.begin(), dotStr.end(), std::bind(std::equal_to<char>(), std::placeholders::_1, '>'), ']');
                std::replace_if(dotStr.begin(), dotStr.end(), std::bind(std::equal_to<char>(), std::placeholders::_1, '{'), '[');
                std::replace_if(dotStr.begin(), dotStr.end(), std::bind(std::equal_to<char>(), std::placeholders::_1, '}'), ']');
                std::replace_if(dotStr.begin(), dotStr.end(), std::bind(std::equal_to<char>(), std::placeholders::_1, '&'), '$');
                ofile << dotStr;

                ofile << "</FONT></TD></TR>" << std::endl;
                //ofile << "\\l"; // left adjusted
            }
            ofile << "\t\t\t\t\t    </TABLE>" << std::endl;
        }

        ofile << "\t\t\t\t</TD></TR>" << std::endl;
        ofile << "\t\t\t    </TABLE>>];" << std::endl;
        //
        // dump out succ edges
        // BB12 -> BB10
        //
        for (std::list<G4_BB*>::iterator sit = bb->Succs.begin();
            sit != bb->Succs.end(); ++sit)
        {
            bb->writeBBId(ofile);
            ofile << " -> ";
            (*sit)->writeBBId(ofile);
            ofile << std::endl;
        }
    }
    //
    // write "}" to end digraph
    //
    ofile << std::endl << " }" << std::endl;
    //
    // close dot file
    //
    ofile.close();
}


// Dump the instructions into a file
void G4_Kernel::dumpPassInternal(const char* suffix)
{
    MUST_BE_TRUE(suffix != NULL, ERROR_INTERNAL_ARGUMENT);
    if (!m_options->getOption(vISA_DumpPasses) && !m_options->getuInt32Option(vISA_DumpPassesSubset)) {
        return;
    }
    std::stringstream ss;
    ss << (name ? name : "UnknownKernel") << "." << std::setfill('0') << std::setw(3) <<
        dotDumpCount++ << "." << suffix << ".g4";
    std::string fname = ss.str();
    fname = sanitizePathString(fname);

    std::fstream ofile(fname, std::ios::out);
    assert(ofile);

    const char* asmFileName = nullptr;
    m_options->getOption(VISA_AsmFileName, asmFileName);
    if (!asmFileName)
        ofile << "UnknownKernel" << std::endl << std::endl;
    else
        ofile << asmFileName << std::endl << std::endl;

    for (const G4_Declare *d : Declares) {
        // stuff below this is usually builtin-type stuff?
        static const int MIN_DECL = 34;
        if (d->getDeclId() > MIN_DECL) {
            // ofile << d->getDeclId() << "\n";
            d->emit(ofile);
        }
    }

    for (std::list<G4_BB*>::iterator it = fg.begin();
        it != fg.end(); ++it)
    {
        // Emit BB number
        G4_BB* bb = (*it);
        bb->writeBBId(ofile);

        // Emit BB type
        if (bb->getBBType())
        {
            ofile << " [" << bb->getBBTypeStr() << "] ";
        }

        ofile << "\tPreds: ";
        for (auto pred : bb->Preds)
        {
            pred->writeBBId(ofile);
            ofile << " ";
        }
        ofile << "\tSuccs: ";
        for (auto succ : bb->Succs)
        {
            succ->writeBBId(ofile);
            ofile << " ";
        }
        ofile << "\n";

        bb->emit(ofile);
        ofile << "\n\n";
    } // bbs

    ofile.close();
}
