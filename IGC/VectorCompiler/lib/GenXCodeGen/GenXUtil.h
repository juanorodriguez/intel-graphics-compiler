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

#ifndef GENX_UTIL_H
#define GENX_UTIL_H

#include "FunctionGroup.h"
#include "GenXRegion.h"
#include "GenXSubtarget.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

#include "Probe/Assertion.h"

#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace llvm {
namespace genx {

// Utility function to get the integral log base 2 of an integer, or -1 if
// the input is not a power of 2.
inline int exactLog2(unsigned Val)
{
  unsigned CLZ = countLeadingZeros(Val, ZB_Width);
  if (CLZ != 32 && 1U << (31 - CLZ) == Val)
    return 31 - CLZ;
  return -1;
}

// Utility function to get the log base 2 of an integer, truncated to an
// integer, or -1 if the number is 0 or negative.
template<typename T>
inline int log2(T Val)
{
  if (Val <= 0)
    return -1;
  unsigned CLZ = countLeadingZeros((uint32_t)Val, ZB_Width);
  return 31 - CLZ;
}

// Common functionality for media ld/st lowering and CISA builder
template <typename T> inline T roundedVal(T Val, T RoundUp) {
  T RoundedVal = static_cast<T>(1) << genx::log2(Val);
  if (RoundedVal < Val)
    RoundedVal *= 2;
  if (RoundedVal < RoundUp)
    RoundedVal = RoundUp;
  return RoundedVal;
}

// Utility function to get type size in diffrent units.
template<unsigned UnitBitSize = 1>
unsigned getTypeSize(Type *Ty, const DataLayout *DL = nullptr) {
  IGC_ASSERT(Ty && Ty->isSized());
  unsigned BitTypeSize = DL ? DL->getTypeSizeInBits(Ty) : Ty->getPrimitiveSizeInBits();
  IGC_ASSERT_MESSAGE(BitTypeSize, "Consider using DataLayout for retrieving this type size");
  return 1 + (BitTypeSize - 1) / UnitBitSize;
}

// createConvert : create a genx_convert intrinsic call
CallInst *createConvert(Value *In, const Twine &Name, Instruction *InsertBefore,
                        Module *M = nullptr);

// createConvertAddr : create a genx_convert_addr intrinsic call
CallInst *createConvertAddr(Value *In, int Offset, const Twine &Name,
                            Instruction *InsertBefore, Module *M = nullptr);

// createAddAddr : create a genx_add_addr intrinsic call
CallInst *createAddAddr(Value *Lhs, Value *Rhs, const Twine &Name,
                        Instruction *InsertBefore, Module *M = nullptr);

CallInst *createUnifiedRet(Type *Ty, const Twine &Name, Module *M);

// getPredicateConstantAsInt : get a vXi1 constant's value as a single integer
unsigned getPredicateConstantAsInt(const Constant *C);

// getConstantSubvector : get a contiguous region from a vector constant
Constant *getConstantSubvector(const Constant *V, unsigned StartIdx,
                               unsigned Size);

// concatConstants : concatenate two possibly vector constants, giving a vector
// constant
Constant *concatConstants(Constant *C1, Constant *C2);

// findClosestCommonDominator : find latest common dominator of some
// instructions
Instruction *findClosestCommonDominator(DominatorTree *DT,
                                        ArrayRef<Instruction *> Insts);

// convertShlShr : convert Shl followed by AShr/LShr by the same amount into
// trunc+sext/zext
Instruction *convertShlShr(Instruction *Inst);

// splitStructPhis : find struct phi nodes and split them
//
// Return:  whether code modified
//
// Each struct phi node is split into a separate phi node for each struct
// element. This is needed because the GenX backend's liveness and coalescing
// code cannot cope with a struct phi.
//
// This is run in two places: firstly in GenXLowering, so that pass can then
// simplify any InsertElement and ExtractElement instructions added by the
// struct phi splitting. But then it needs to be run again in GenXLiveness,
// because other passes can re-insert a struct phi. The case I saw in
// hevc_speed was something commoning up the struct return from two calls in an
// if..else..endif.
//
// BTW There's also GenXAggregatePseudoLowering pass that does the same.
bool splitStructPhis(Function *F);
bool splitStructPhi(PHINode *Phi);

// normalize g_load with bitcasts.
//
// When a single g_load is being bitcast'ed to different types, clone g_loads.
bool normalizeGloads(Instruction *Inst);

// fold bitcast instruction to store/load pointer operand if possible.
// Return this new instruction or nullptr.
Instruction *foldBitCastInst(Instruction *Inst);

// Return the underlying global variable. Return nullptr if it does not exist.
GlobalVariable *getUnderlyingGlobalVariable(Value *V);
const GlobalVariable *getUnderlyingGlobalVariable(const Value *V);

class Bale;

bool isGlobalStore(StoreInst *ST);

bool isGlobalLoad(LoadInst* LI);

// Check that V is correct as value for global store to StorePtr.
// This implies:
// 1) V is wrregion W;
// 2) Old value of W is result of gload L;
// 3) Pointer operand of L is derived from global variable of StorePtr.
bool isLegalValueForGlobalStore(Value *V, Value *StorePtr);

// Check that global store ST operands meet condition of
// isLegalValueForGlobalStore.
bool isGlobalStoreLegal(StoreInst *ST);

bool isIdentityBale(const Bale &B);

// Check if region of value is OK for baling in to raw operand
//
// Enter:   V = value that is possibly rdregion/wrregion
//          IsWrite = true if caller wants to see wrregion, false for rdregion
//
// The region must be constant indexed, contiguous, and start on a GRF
// boundary.
bool isValueRegionOKForRaw(Value *V, bool IsWrite, const GenXSubtarget *ST);

// Check if region is OK for baling in to raw operand
//
// The region must be constant indexed, contiguous, and start on a GRF
// boundary.
bool isRegionOKForRaw(const genx::Region &R, const GenXSubtarget *ST);

// Skip optimizations on functions with large blocks.
inline bool skipOptWithLargeBlock(const Function &F) {
  return std::any_of(F.begin(), F.end(),
                     [](const BasicBlock &BB) { return BB.size() >= 5000; });
}

bool skipOptWithLargeBlock(FunctionGroup &FG);

// getTwoAddressOperandNum : get operand number of two address operand
int getTwoAddressOperandNum(CallInst *CI);

// isNot : test whether an instruction is a "not" instruction (an xor with
//    constant all ones)
bool isNot(Instruction *Inst);

// isPredNot : test whether an instruction is a "not" instruction (an xor
//    with constant all ones) with predicate (i1 or vector of i1) type
bool isPredNot(Instruction *Inst);

// isIntNot : test whether an instruction is a "not" instruction (an xor
//    with constant all ones) with non-predicate type
bool isIntNot(Instruction *Inst);

// getMaskOperand : get i1 vector type of genx intrinsic, return null
//    if there is no operand of such type or for non genx intrinsic.
//    If there are multiple operands of i1 vector type then return first
//    oparand.
Value *getMaskOperand(const Instruction *Inst);

// invertCondition : Invert the given predicate value, possibly reusing
//    an existing copy.
Value *invertCondition(Value *Condition);

// if V is a function pointer return function it points to,
//    nullptr otherwise
Function *getFunctionPointerFunc(Value *V);

// return true if V is a const vector of function pointers
// considering any casts and extractelems within
bool isFuncPointerVec(Value *V);

// ShuffleVectorAnalyzer : class to analyze a shufflevector
class ShuffleVectorAnalyzer {
  ShuffleVectorInst *SI;

public:
  ShuffleVectorAnalyzer(ShuffleVectorInst *SI) : SI(SI) {}
  // getAsSlice : return start index of slice, or -1 if shufflevector is not
  //  slice
  int getAsSlice();

  // Replicated slice descriptor.
  // Replicated slice (e.g. 1 2 3 1 2 3) can be parametrized by
  // initial offset (1), slice size (3) and replication count (2).
  struct ReplicatedSlice {
    unsigned InitialOffset;
    unsigned SliceSize;
    unsigned ReplicationCount;
    ReplicatedSlice(unsigned Offset, unsigned Size, unsigned Count)
        : InitialOffset(Offset), SliceSize(Size), ReplicationCount(Count) {}
  };

  // isReplicatedSlice : check whether shufflevector is replicated slice.
  // Example of replicated slice:
  // shufflevector <3 x T> x, undef, <6 x i32> <1, 2, 1, 2, 1, 2>.
  bool isReplicatedSlice() const;

  static bool isReplicatedSlice(ShuffleVectorInst *SI) {
    return ShuffleVectorAnalyzer(SI).isReplicatedSlice();
  }

  // When we have replicated slice, its parameters are ealisy deduced
  // from first and last elements of mask. This function decomposes
  // replicated slice to its parameters.
  ReplicatedSlice getReplicatedSliceDescriptor() const {
    IGC_ASSERT_MESSAGE(isReplicatedSlice(), "Expected replicated slice");
    const unsigned TotalSize = (SI->getType())->getNumElements();
    const unsigned SliceStart = SI->getMaskValue(0);
    const unsigned SliceEnd = SI->getMaskValue(TotalSize - 1);
    const unsigned SliceSize = SliceEnd - SliceStart + 1;
    const unsigned ReplicationCount = TotalSize / SliceSize;
    return ReplicatedSlice(SliceStart, SliceSize, ReplicationCount);
  }

  static ReplicatedSlice getReplicatedSliceDescriptor(ShuffleVectorInst *SI) {
    return ShuffleVectorAnalyzer(SI).getReplicatedSliceDescriptor();
  }

  // getAsUnslice : see if the shufflevector is an
  //     unslice where the "old value" is operand 0 and operand 1 is another
  //     shufflevector and operand 0 of that is the "new value" Returns start
  //     index, or -1 if it is not an unslice
  int getAsUnslice();
  // getAsSplat : if shufflevector is a splat, get the splatted input, with the
  //  element's vector index if the input is a vector
  struct SplatInfo {
    Value *Input;
    unsigned Index;
    SplatInfo(Value *Input, unsigned Index) : Input(Input), Index(Index) {}
  };
  SplatInfo getAsSplat();

  // Serialize this shuffulevector instruction.
  Value *serialize();

  // Compute the cost in terms of number of insertelement instructions needed.
  unsigned getSerializeCost(unsigned i);

  // To describe the region of one of two shufflevector instruction operands.
  struct OperandRegionInfo {
    Value *Op;
    Region R;
  };
  OperandRegionInfo getMaskRegionPrefix(int StartIdx);
};

// class for splitting i64 (both vector and scalar) to subregions of i32 vectors
// Used in GenxLowering and emulation routines
class IVSplitter {
  Instruction &Inst;

  Type *ETy = nullptr;
  Type *VI32Ty = nullptr;
  size_t Len = 0;

  enum class RegionType { LoRegion, HiRegion, FirstHalf, SecondHalf };
  Region createSplitRegion(Type *Ty, RegionType RT);

  std::pair<Value*, Value*> splitValue(Value& Val, RegionType RT1,
                                       const Twine& Name1, RegionType RT2,
                                       const Twine& Name2);
  Value* combineSplit(Value &V1, Value &V2, RegionType RT1, RegionType RT2,
                      const Twine& Name, bool Scalarize);

public:

  struct LoHiSplit {
    Value *Lo;
    Value *Hi;
  };
  struct HalfSplit {
    Value *Left;
    Value *Right;
  };

  // Instruction is used as an insertion point, debug location source and
  // as a source of operands to split.
  // If BaseOpIdx indexes a scalar/vector operand of i64 type, then
  // IsI64Operation shall return true, otherwise the value type of an
  // instruction is used
  IVSplitter(Instruction &Inst, const unsigned *BaseOpIdx = nullptr);

  // Splitted Operand is expected to be a scalar/vector of i64 type
  LoHiSplit splitOperandLoHi(unsigned SourceIdx);
  HalfSplit splitOperandHalf(unsigned SourceIdx);

  LoHiSplit splitValueLoHi(Value &V);
  HalfSplit splitValueHalf(Value &V);

  // Combined values are expected to be a vector of i32 of the same size
  Value *combineLoHiSplit(const LoHiSplit &Split, const Twine &Name,
                          bool Scalarize);
  Value *combineHalfSplit(const HalfSplit &Split, const Twine &Name,
                          bool Scalarize);

  // convinence method for quick sanity checking
  bool IsI64Operation() const { return ETy->isIntegerTy(64); }
};

// adjustPhiNodesForBlockRemoval : adjust phi nodes when removing a block
void adjustPhiNodesForBlockRemoval(BasicBlock *Succ, BasicBlock *BB);

/***********************************************************************
 * sinkAdd : sink add(s) in address calculation
 *
 * Enter:   IdxVal = the original index value
 *
 * Return:  the new calculation for the index value
 *
 * This detects the case when a variable index in a region or element access
 * is one or more constant add/subs then some mul/shl/truncs. It sinks
 * the add/subs into a single add after the mul/shl/truncs, so the add
 * stands a chance of being baled in as a constant offset in the region.
 *
 * If add sinking is successfully applied, it may leave now unused
 * instructions behind, which need tidying by a later dead code removal
 * pass.
 */
Value *sinkAdd(Value *V);

// Check if this is a mask packing operation, i.e. a bitcast from Vxi1 to
// integer i8, i16 or i32.
static inline bool isMaskPacking(const Value *V) {
  if (auto BC = dyn_cast<BitCastInst>(V)) {
    auto SrcTy = dyn_cast<VectorType>(BC->getSrcTy());
    if (!SrcTy || !SrcTy->getScalarType()->isIntegerTy(1))
      return false;
    unsigned NElts = cast<VectorType>(SrcTy)->getNumElements();
    if (NElts != 8 && NElts != 16 && NElts != 32)
      return false;
    return V->getType()->getScalarType()->isIntegerTy(NElts);
  }
  return false;
}

void LayoutBlocks(Function &func, LoopInfo &LI);
void LayoutBlocks(Function &func);

// Metadata name for inline assemly instruction
constexpr const char *MD_genx_inline_asm_info = "genx.inlasm.constraints.info";

// Inline assembly avaliable constraints
enum class ConstraintType : uint32_t {
  Constraint_r,
  Constraint_rw,
  Constraint_i,
  Constraint_n,
  Constraint_F,
  Constraint_a,
  Constraint_cr,
  Constraint_unknown
};

// Represents info about inline asssembly operand
class GenXInlineAsmInfo {
  genx::ConstraintType CTy = ConstraintType::Constraint_unknown;
  int MatchingInput = -1;
  bool IsOutput = false;

public:
  GenXInlineAsmInfo(genx::ConstraintType Ty, int MatchingInput, bool IsOutput)
      : CTy(Ty), MatchingInput(MatchingInput), IsOutput(IsOutput) {}
  bool hasMatchingInput() const { return MatchingInput != -1; }
  int getMatchingInput() const { return MatchingInput; }
  bool isOutput() const { return IsOutput; }
  genx::ConstraintType getConstraintType() const { return CTy; }
};

// If input input constraint has matched output operand with same constraint
bool isInlineAsmMatchingInputConstraint(const InlineAsm::ConstraintInfo &Info);

// Get matched output operand number for input operand
unsigned getInlineAsmMatchedOperand(const InlineAsm::ConstraintInfo &Info);

// Get joined string representation of constraints
std::string getInlineAsmCodes(const InlineAsm::ConstraintInfo &Info);

// Get constraint type
genx::ConstraintType getInlineAsmConstraintType(StringRef Codes);

// Get vector of inline asm info for inline assembly instruction.
// Return empty vector if no constraint string in inline asm or
// if called before GenXInlineAsmLowering pass.
std::vector<GenXInlineAsmInfo> getGenXInlineAsmInfo(CallInst *CI);

// Get vector of inline asm info from MDNode
std::vector<GenXInlineAsmInfo> getGenXInlineAsmInfo(MDNode *MD);

bool hasConstraintOfType(const std::vector<GenXInlineAsmInfo> &ConstraintsInfo,
                         genx::ConstraintType CTy);

// Get number of outputs for inline assembly instruction
unsigned getInlineAsmNumOutputs(CallInst *CI);

Type *getCorrespondingVectorOrScalar(Type *Ty);

/* scalarVectorizeIfNeeded: scalarize of vectorize \p Inst if it is required
 *
 * Result of some instructions can be both Ty and <1 x Ty> value e.g. rdregion.
 * It is sometimes required to replace uses of instructions with types
 * [\p FirstType, \p LastType) with \p Inst. If types don't
 * correspond this function places BitCastInst <1 x Ty> to Ty, or Ty to <1 x Ty>
 * after \p Inst and returns the pointer to the instruction. If no cast is
 * required, nullptr is returned.
 */
template <
    typename ConstIter,
    typename std::enable_if<
        std::is_base_of<
            Type, typename std::remove_pointer<typename std::iterator_traits<
                      ConstIter>::value_type>::type>::value,
        int>::type = 0>
CastInst *scalarizeOrVectorizeIfNeeded(Instruction *Inst, ConstIter FirstType,
                                       ConstIter LastType) {
  IGC_ASSERT_MESSAGE(Inst, "wrong argument");
  IGC_ASSERT_MESSAGE(std::all_of(FirstType, LastType,
                     [Inst](Type *Ty) {
                       return Ty == Inst->getType() ||
                              Ty == getCorrespondingVectorOrScalar(
                                        Inst->getType());
                     }),
         "wrong arguments: type of instructions must correspond");

  if (Inst->getType()->isVectorTy() &&
      cast<VectorType>(Inst->getType())->getNumElements() > 1)
    return nullptr;
  bool needBitCast = std::any_of(
      FirstType, LastType, [Inst](Type *Ty) { return Ty != Inst->getType(); });
  if (!needBitCast)
    return nullptr;
  auto *CorrespondingTy = getCorrespondingVectorOrScalar(Inst->getType());
  auto *BC = CastInst::Create(Instruction::BitCast, Inst, CorrespondingTy);
  BC->insertAfter(Inst);
  return BC;
}
/* scalarVectorizeIfNeeded: scalarize of vectorize \p Inst if it is required
 *
 * Result of some instructions can be both Ty and <1 x Ty> value e.g. rdregion.
 * It is sometimes required to replace uses of instructions of [\p
 * FirstInstToReplace, \p LastInstToReplace) with \p Inst. If types don't
 * correspond this function places BitCastInst <1 x Ty> to Ty, or Ty to <1 x Ty>
 * after \p Inst and returns the pointer to the instruction. If no cast is
 * required, nullptr is returned.
 */
template <typename ConstIter,
          typename std::enable_if<
              std::is_base_of<
                  Instruction,
                  typename std::remove_pointer<typename std::iterator_traits<
                      ConstIter>::value_type>::type>::value,
              int>::type = 0>
CastInst *scalarizeOrVectorizeIfNeeded(Instruction *Inst,
                                       ConstIter FirstInstToReplace,
                                       ConstIter LastInstToReplace) {
  std::vector<Type *> Types;
  std::transform(FirstInstToReplace, LastInstToReplace,
                 std::back_inserter(Types),
                 [](Instruction *Inst) { return Inst->getType(); });
  return scalarizeOrVectorizeIfNeeded(Inst, Types.begin(), Types.end());
}

CastInst *scalarizeOrVectorizeIfNeeded(Instruction *Inst, Type *RefType);

CastInst *scalarizeOrVectorizeIfNeeded(Instruction *Inst,
                                       Instruction *InstToReplace);


// Returns log alignment for align type and target grf width, because ALIGN_GRF
// must be target-dependent.
unsigned getLogAlignment(VISA_Align Align, unsigned GRFWidth);
// The opposite of getLogAlignment.
VISA_Align getVISA_Align(unsigned LogAlignment, unsigned GRFWidth);
// Some log alignments cannot be transparently transformed to VISA_Align. This
// chooses suitable log alignment which is convertible to VISA_Align.
unsigned ceilLogAlignment(unsigned LogAlignment, unsigned GRFWidth);

// If \p Ty is degenerate vector type <1 x ElTy>,
// ElTy is returned, otherwise original type \p Ty is returned.
const Type &fixDegenerateVectorType(const Type &Ty);
Type &fixDegenerateVectorType(Type &Ty);

// Checks whether provided wrpredregion intrinsic can be encoded
// as legal SETP instruction.
bool isWrPredRegionLegalSetP(const CallInst &WrPredRegion);

// Checks if V is a CallInst representing a direct call to F
// Many of our analyzes do not check whether a function F's user
// which is a CallInst calls exactly F. This may not be true
// when a function pointer is passed as an argument of a call to
// another function, e.g. genx.faddr intrinsic.
// Returns V casted to CallInst if the check is true,
// nullptr otherwise.
CallInst *checkFunctionCall(Value *V, Function *F);

// breakConstantVector : break vector of constexprs into a sequence of
//                       InsertElementInsts.
// CV - vector to break
// CurInst - Instruction CV is a part of
// InsertPt - point to insert new instructions at
// Return the last InsertElementInst in the resulting chain,
// or nullptr if there're no constexprs in CV.
Value *breakConstantVector(ConstantVector *CV, Instruction *CurInst,
                           Instruction *InsertPt);
// breakConstantExprs : break constant expressions in instruction I.
// Return true if any modifications have been made, false otherwise.
bool breakConstantExprs(Instruction *I);
// breakConstantExprs : break constant expressions in function F.
// Return true if any modifications have been made, false otherwise.
bool breakConstantExprs(Function *F);
// Get possible number of GRFs for indirect region
unsigned getNumGRFsPerIndirectForRegion(const genx::Region &R,
                                        const GenXSubtarget *ST, bool Allow2D);
// to control behavior of emulateI64Operation function
enum class EmulationFlag {
  RAUW,
  // RAUW and EraseFromParent, always returns a valid instruction
  // either the original one or the last one from the result emulation sequence
  RAUWE,
  None,
};
// transforms operation on i64 type to an equivalent sequence that do not
// operate on i64 (but rather on i32)
// The implementation is contained in GenXEmulate pass sources
// Note: ideally, i64 emulation should be handled by GenXEmulate pass,
// however, some of our late passes like GetXPostLegalization or GenXCategory
// may introduce additional instructions which violate Subtarget restrictions -
// this function is intended to cope with such cases
Instruction *emulateI64Operation(const GenXSubtarget *ST, Instruction *In,
                                 EmulationFlag AuxAction);
// BinaryDataAccumulator: it's a helper class to accumulate binary data
// in one buffer.
// Information about each stored section can be accessed via the key with
// which it was stored. The key must be unique.
// Accumulated/consolidated binary data can be accesed.
template <typename KeyT> class BinaryDataAccumulator {
public:
  struct SectionInfoT {
    int Offset = 0;
    ArrayRef<char> Data;

    SectionInfoT() = default;
    SectionInfoT(const char *BasePtr, int First, int Last)
        : Offset{First}, Data{BasePtr + First, BasePtr + Last} {}

    int getSize() const { return Data.size(); }
  };

  struct SectionT {
    KeyT Key;
    SectionInfoT Info;
  };

private:
  std::vector<char> Data;
  using SectionSeq = std::vector<SectionT>;
  SectionSeq Sections;

public:
  using value_type = typename SectionSeq::value_type;
  using reference = typename SectionSeq::reference;
  using const_reference = typename SectionSeq::const_reference;
  using iterator = typename SectionSeq::iterator;
  using const_iterator = typename SectionSeq::const_iterator;

  iterator begin() { return Sections.begin(); }
  const_iterator begin() const { return Sections.begin(); }
  const_iterator cbegin() const { return Sections.cbegin(); }
  iterator end() { return Sections.end(); }
  const_iterator end() const { return Sections.end(); }
  const_iterator cend() const { return Sections.cend(); }
  reference front() { return *begin(); }
  const_reference front() const { return *begin(); }
  reference back() { return *std::prev(end()); }
  const_reference back() const { return *std::prev(end()); }

  // Append the data that is referenced by a \p Key and represented
  // in range [\p First, \p Last), to the buffer.
  // The range must consist of char elements.
  template <typename InputIter>
  void append(KeyT Key, InputIter First, InputIter Last) {
    IGC_ASSERT_MESSAGE(
        std::none_of(Sections.begin(), Sections.end(),
                     [&Key](const SectionT &S) { return S.Key == Key; }),
        "There's already a section with such key");
    SectionT Section;
    Section.Key = std::move(Key);
    int Offset = Data.size();
    std::copy(First, Last, std::back_inserter(Data));
    Section.Info =
        SectionInfoT{Data.data(), Offset, static_cast<int>(Data.size())};
    Sections.push_back(std::move(Section));
  }

  void append(KeyT Key, ArrayRef<char> SectionBin) {
    append(std::move(Key), SectionBin.begin(), SectionBin.end());
  }

  // Get information about the section referenced by \p Key.
  SectionInfoT getSectionInfo(const KeyT &Key) const {
    auto SectionIt =
        std::find_if(Sections.begin(), Sections.end(),
                     [&Key](const SectionT &S) { return S.Key == Key; });
    IGC_ASSERT_MESSAGE(SectionIt != Sections.end(),
                       "There must be a section with such key");
    return SectionIt->Info;
  }

  // Get offset of the section referenced by \p Key.
  int getSectionOffset(const KeyT &Key) const {
    return getSectionInfo(Key).Offset;
  }
  // Get size of the section referenced by \p Key.
  int getSectionSize(const KeyT &Key) const { return getSectionInfo(Key).Size; }
  // Get size of the whole collected data.
  int getFullSize() const { return Data.size(); }
  // Data buffer empty.
  bool empty() const { return Data.empty(); }
  // Emit the whole consolidated data.
  std::vector<char> emitConsolidatedData() const & { return Data; }
  std::vector<char> emitConsolidatedData() && { return std::move(Data); }
};

// Not every global variable is a real global variable and should be eventually
// encoded as a global variable.
// GenX volatile and printf strings are exclusion for now.
// Printf strings should be already legalized to make it possible to use this
// function. Which should already be done in middle-end so no problem for
// calling it in codegen.
bool isRealGlobalVariable(const GlobalVariable &GV);

// Get size of an struct field including the size of padding for the next field,
// or the tailing padding.
// For example for the 1st element of { i8, i32 } 4 bytes will be returned
// (likely in the most of layouts).
//
// Arguments:
//    \p ElemIdx - index of a struct field
//    \p NumOperands - the number of fields in struct
//                     (StructLayout doesn't expose it)
//    \p StructLayout - struct layout
//
// Returns the size in bytes.
std::size_t getStructElementPaddedSize(unsigned ElemIdx, unsigned NumOperands,
                                       const StructLayout &Layout);

} // namespace genx
} // namespace llvm

#endif // GENX_UTIL_H
