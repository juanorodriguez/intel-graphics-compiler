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

//
/// GenXEmulate
/// -----------
///
/// GenXEmulate is a mudule pass that emulates certain LLVM IR instructions.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "GENX_EMULATION"

#include "GenX.h"
#include "GenXSubtarget.h"
#include "GenXTargetMachine.h"
#include "GenXUtil.h"
#include "IGC/common/StringMacros.hpp"
#include "Probe/Assertion.h"

#include "llvmWrapper/IR/DerivedTypes.h"

#include "vc/BiF/Tools.h"
#include "vc/GenXOpts/Utils/BiFTools.h"
#include "vc/GenXOpts/Utils/InternalMetadata.h"
#include "vc/Support/BackendConfig.h"

#include "llvm/GenXIntrinsics/GenXIntrinsics.h"
#include "llvm/GenXIntrinsics/GenXMetadata.h"

#include <llvm/Analysis/TargetFolder.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/Module.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Pass.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/raw_ostream.h>

#include "Probe/Assertion.h"

#include <string>

using namespace llvm;
using namespace genx;

static constexpr const char *LibraryFunctionPrefix = "__cm_intrinsic_impl_";
static constexpr const char *EmuLibSDivPrefix = "__cm_intrinsic_impl_sdiv";
static constexpr const char *EmuLibSRemPrefix = "__cm_intrinsic_impl_srem";
static constexpr const char *EmuLibUDivPrefix = "__cm_intrinsic_impl_udiv";
static constexpr const char *EmuLibURemPrefix = "__cm_intrinsic_impl_urem";

static constexpr const char *RoundingRtzSuffix = "__rtz_";
static constexpr const char *RoundingRteSuffix = "__rte_";
static constexpr const char *RoundingRtpSuffix = "__rtp_";
static constexpr const char *RoundingRtnSuffix = "__rtn_";

// TODO: move this to vc-intrinsics`
static constexpr int VCRoundingRTE = 0;
static constexpr int VCRoundingRTP = 1 << 4;
static constexpr int VCRoundingRTN = 2 << 4;
static constexpr int VCRoundingRTZ = 3 << 4;

namespace {

// TODO: we expect this to be turned on by default
static cl::opt<bool> OptStrictEmulationRequests(
    "vc-i64emu-strict-requests", cl::init(false),
    cl::Hidden,
    cl::desc("Explicit emulation requests are subject to stricter checks"));
static cl::opt<bool> OptIcmpEnable("vc-i64emu-icmp-enable", cl::init(true),
                                   cl::Hidden,
                                   cl::desc("enable icmp emulation"));
static cl::opt<bool> OptProcessPtrs("vc-i64emu-ptrs-enable", cl::init(true),
                                    cl::Hidden,
                                    cl::desc("enable icmp emulation"));
static cl::opt<bool> OptConvertPartialPredicates(
    "vc-i64emu-icmp-ppred-lowering", cl::init(true), cl::Hidden,
    cl::desc("if \"partial predicates\" shall be converted to icmp"));

using IRBuilder = IRBuilder<TargetFolder>;

class GenXEmulate : public ModulePass {

  friend Instruction *llvm::genx::emulateI64Operation(const GenXSubtarget *ST,
                                                      Instruction *In,
                                                      EmulationFlag AuxAction);
  std::vector<Instruction *> DiscracedList;
  // Maps <opcode, type> to its corresponding emulation function.
  using OpType = std::pair<unsigned, Type *>;
  std::vector<Instruction *> ToErase;
  std::map<OpType, Function *> EmulationFuns;
  const GenXSubtarget *ST = nullptr;

  class Emu64Expander : public InstVisitor<Emu64Expander, Value *> {

    friend InstVisitor<Emu64Expander, Value *>;

    const GenXSubtarget &ST;
    IVSplitter SplitBuilder;
    Instruction &Inst;

    Value *expandBitwiseOp(BinaryOperator &);
    Value *visitAdd(BinaryOperator &);
    Value *visitSub(BinaryOperator &);
    Value *visitAnd(BinaryOperator &);
    Value *visitOr(BinaryOperator &);
    Value *visitXor(BinaryOperator &);
    Value *visitSelectInst(SelectInst &I);
    Value *visitICmp(ICmpInst &);

    Value *visitShl(BinaryOperator &);
    Value *visitLShr(BinaryOperator &);
    Value *visitAShr(BinaryOperator &);

    Value *buildRightShift(IVSplitter &SplitBuilder, BinaryOperator &Op);

    Value *visitFPToUI(FPToUIInst &);
    Value *visitFPToSI(FPToSIInst &);
    Value *visitUIToFP(UIToFPInst &);
    Value *visitSIToFP(SIToFPInst &);

    Value *visitZExtInst(ZExtInst &I);
    Value *visitSExtInst(SExtInst &I);

    Value *visitPtrToInt(PtrToIntInst &I);
    Value *visitIntToPtr(IntToPtrInst &I);

    Value *visitGenxTrunc(CallInst &CI);
    Value *visitGenxMinMax(CallInst &CI);
    // genx_absi
    Value *visitGenxAbsi(CallInst &CI);
    // handles genx_{XX}add_sat cases
    Value *visitGenxAddSat(CallInst &CI);

    // [+] bitcast
    // [-] genx.constanti ?
    // [-] genx.scatter ?
    // [-] genx.gather ?
    Value *visitCallInst(CallInst &CI);
    Value *visitInstruction(Instruction &I) { return nullptr; }

    // if the value is not an Instruciton (like ConstExpr), return the original
    // value. Return the emulated sequence otherwise
    Value *ensureEmulated(Value *Val);

    static bool isI64PointerOp(const Instruction &I);
    static bool isConvertOfI64(const Instruction &I);
    static bool isI64ToFP(const Instruction &I);
    static bool isI64Cmp(const Instruction &I);
    static Value *detectBitwiseNot(BinaryOperator &);

    struct VectorInfo {
      Value *V;
      VectorType *VTy;
    };
    static VectorInfo toVector(IRBuilder &Builder, Value *In);
    static bool getConstantUI32Values(Value *V,
                                      SmallVectorImpl<uint32_t> &Result);

    // functors to help with shift emulation
    struct LessThan32 {
      bool operator()(uint64_t Val) const { return Val < 32u; }
    };
    struct GreaterThan32 {
      bool operator()(uint64_t Val) const { return Val > 32u; }
    };
    struct Equals32 {
      bool operator()(uint64_t Val) const { return Val == 32u; }
    };

    bool needsEmulation() const {
      return (SplitBuilder.IsI64Operation() || isI64Cmp(Inst) ||
              isConvertOfI64(Inst) || isI64PointerOp(Inst));
    }

    IRBuilder getIRBuilder() {
      return IRBuilder(Inst.getParent(), BasicBlock::iterator(&Inst),
                       TargetFolder(Inst.getModule()->getDataLayout()));
    }

    class ConstantEmitter {
    public:
      ConstantEmitter(Value *V)
          : ElNum(cast<VectorType>(V->getType())->getNumElements()),
            Ty32(Type::getInt32Ty(V->getContext())) {}
      Constant *getSplat(unsigned Val) const {
        auto *KV = Constant::getIntegerValue(Ty32, APInt(32, Val));
        return ConstantDataVector::getSplat(ElNum, KV);
      }
      Constant *getZero() const { return Constant::getNullValue(getVTy()); }
      Constant *getOnes() const { return Constant::getAllOnesValue(getVTy()); }
      Type *getVTy() const {
        return IGCLLVM::FixedVectorType::get(Ty32, ElNum);
      }

    private:
      unsigned ElNum = 0;
      Type *Ty32 = nullptr;
    };

  public:
    Emu64Expander(const GenXSubtarget &ST, Instruction &I)
        : ST(ST), SplitBuilder(I), Inst(I) {}

    Value *tryExpand() {
      if (!needsEmulation())
        return nullptr;
      LLVM_DEBUG(dbgs() << "i64-emu: trying " << Inst << "\n");
      auto *Result = visit(Inst);

      if (Result)
        LLVM_DEBUG(dbgs() << "i64-emu: emulated with " << *Result << "\n");

      return Result;
    }
    using LHSplit = IVSplitter::LoHiSplit;
    Value *buildTernaryAddition(IRBuilder &Builder, Value &A, Value &B,
                                Value &C, const Twine &Name) const;
    struct AddSubExtResult {
      Value *Val; // Main Value
      Value *CB;  // Carry/Borrow
    };
    static AddSubExtResult buildAddc(Module *M, IRBuilder &B, Value &R,
                                     Value &L, const Twine &Prefix);
    static AddSubExtResult buildSubb(Module *M, IRBuilder &B, Value &L,
                                     Value &R, const Twine &Prefix);
    static Value *buildGeneralICmp(IRBuilder &B, CmpInst::Predicate P,
                                   bool IsPartialPredicate, const LHSplit &L,
                                   const LHSplit &R);
    static Value *tryOptimizedShr(IRBuilder &B, IVSplitter &SplitBuilder,
                                  BinaryOperator &Op, ArrayRef<uint32_t> Sa);
    static Value *tryOptimizedShl(IRBuilder &B, IVSplitter &SplitBuilder,
                                  BinaryOperator &Op, ArrayRef<uint32_t> Sa);
    static Value *buildGenericRShift(IRBuilder &B, IVSplitter &SplitBuilder,
                                     BinaryOperator &Op);

    enum Rounding {
      // Not used currenly
    };
    static Value *buildFPToI64(Module &M, IRBuilder &B,
                               IVSplitter &SplitBuilder, Value *V,
                               bool IsSigned, Rounding rnd = Rounding());

    struct ShiftInfo {
      ShiftInfo(Value *ShaIn, Value *Sh32In, Value *Mask1In, Value *Mask0In)
          : Sha{ShaIn}, Sh32{Sh32In}, Mask1{Mask1In}, Mask0{Mask0In} {}
      // Masked Shift Amount
      Value *Sha = nullptr;
      // 32 - Sha
      Value *Sh32 = nullptr;
      // To zero-out the high part (shift >= 32)
      Value *Mask1 = nullptr;
      // To negate results if Sha = 0
      Value *Mask0 = nullptr;
    };
    static Value *buildPartialRShift(IRBuilder &B, Value *SrcLo, Value *SrcHi,
                                     const ShiftInfo &SI);
    static ShiftInfo constructShiftInfo(IRBuilder &B, Value *Base);

  };

public:
  static char ID;
  explicit GenXEmulate() : ModulePass(ID) {}
  virtual StringRef getPassName() const { return "GenX emulation"; }
  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnModule(Module &M);
  void runOnFunction(Function &F);

private:
  Value *emulateInst(Instruction *Inst);
  Function *getEmulationFunction(Instruction *Inst) const;
  void buildDivRemCache(Module &M);

  static bool isOldEmulationFunction(const Function *F) {
    return F->getName().contains("__cm_intrinsic_impl_");
  }
  // Check if a function is to emulate instructions.
  static bool isEmulationFunction(const Function* F) {
    return F->hasFnAttribute(genx::FunctionMD::VCEmulationRoutine);
  }
};

} // end namespace

bool GenXEmulate::Emu64Expander::isI64PointerOp(const Instruction &I) {
  auto Opcode = I.getOpcode();
  const DataLayout &DL = I.getModule()->getDataLayout();
  if (Opcode == Instruction::ICmp) {
    auto *OpSTy = I.getOperand(0)->getType()->getScalarType();
    if (!OpSTy->isPointerTy())
      return false;
    if (DL.getTypeSizeInBits(OpSTy) < 64)
      return false;
    return true;
  }
  if (Opcode == Instruction::PtrToInt || Opcode == Instruction::IntToPtr) {
    auto *PtrType = I.getType()->getScalarType();
    auto *IntType = I.getOperand(0)->getType()->getScalarType();
    if (Opcode == Instruction::PtrToInt)
      std::swap(PtrType, IntType);
    if (cast<CastInst>(&I)->isNoopCast(DL))
      return false;
    return (DL.getTypeSizeInBits(PtrType) == 64 ||
            DL.getTypeSizeInBits(IntType) == 64);
  }
  return false;
}
bool GenXEmulate::Emu64Expander::isConvertOfI64(const Instruction &I) {

  if (GenXEmulate::Emu64Expander::isI64ToFP(I))
    return true;

  auto IID = GenXIntrinsic::getAnyIntrinsicID(&I);
  switch (IID) {
  case GenXIntrinsic::genx_uutrunc_sat:
  case GenXIntrinsic::genx_sstrunc_sat:
  case GenXIntrinsic::genx_ustrunc_sat:
  case GenXIntrinsic::genx_sutrunc_sat:
    return I.getOperand(0)->getType()->getScalarType()->isIntegerTy(64);
  }
  return false;
}
bool GenXEmulate::Emu64Expander::isI64ToFP(const Instruction &I) {
  if (Instruction::UIToFP != I.getOpcode() &&
      Instruction::SIToFP != I.getOpcode()) {
    return false;
  }
  return I.getOperand(0)->getType()->getScalarType()->isIntegerTy(64);
}
bool GenXEmulate::Emu64Expander::isI64Cmp(const Instruction &I) {
  if (Instruction::ICmp != I.getOpcode())
    return false;
  return I.getOperand(0)->getType()->getScalarType()->isIntegerTy(64);
}

Value *GenXEmulate::Emu64Expander::detectBitwiseNot(BinaryOperator &Op) {
  if (Instruction::Xor != Op.getOpcode())
    return nullptr;

  auto isAllOnes = [](const Value *V) {
    if (auto *C = dyn_cast<Constant>(V))
      return C->isAllOnesValue();
    return false;
  };

  if (isAllOnes(Op.getOperand(1)))
    return Op.getOperand(0);
  if (isAllOnes(Op.getOperand(0)))
    return Op.getOperand(1);

  return nullptr;
}
Value *GenXEmulate::Emu64Expander::expandBitwiseOp(BinaryOperator &Op) {
  auto Src0 = SplitBuilder.splitOperandHalf(0);
  auto Src1 = SplitBuilder.splitOperandHalf(1);

  auto Builder = getIRBuilder();

  Value *Part1 = Builder.CreateBinOp(Op.getOpcode(), Src0.Left, Src1.Left,
                                     Inst.getName() + ".part1");
  Value *Part2 = Builder.CreateBinOp(Op.getOpcode(), Src0.Right, Src1.Right,
                                     Inst.getName() + ".part2");
  return SplitBuilder.combineHalfSplit(
      {Part1, Part2}, Twine("int_emu.") + Op.getOpcodeName() + ".",
      Inst.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::visitAdd(BinaryOperator &Op) {
  auto Src0 = SplitBuilder.splitOperandLoHi(0);
  auto Src1 = SplitBuilder.splitOperandLoHi(1);

  auto Builder = getIRBuilder();
  // add64 transforms as:
  //    [add_lo, carry] = genx_addc(src0.l0, src1.lo)
  //    add_hi = add(carry, add(src0.hi, src1.hi))
  //    add64  = combine(add_lo,add_hi)
  auto AddcRes = buildAddc(Inst.getModule(), Builder, *Src0.Lo, *Src1.Lo,
                           "int_emu.add64.lo.");
  auto *AddLo = AddcRes.Val;
  auto *AddHi =
      buildTernaryAddition(Builder, *AddcRes.CB, *Src0.Hi, *Src1.Hi, "add_hi");
  return SplitBuilder.combineLoHiSplit(
      {AddLo, AddHi}, Twine("int_emu.") + Op.getOpcodeName() + ".",
      Inst.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::visitSub(BinaryOperator &Op) {
  auto Src0 = SplitBuilder.splitOperandLoHi(0);
  auto Src1 = SplitBuilder.splitOperandLoHi(1);

  auto *SubbFunct = GenXIntrinsic::getGenXDeclaration(
      Inst.getModule(), GenXIntrinsic::genx_subb,
      {Src0.Lo->getType(), Src1.Lo->getType()});

  auto Builder = getIRBuilder();
  // sub64 transforms as:
  //    [sub_lo, borrow] = genx_subb(src0.l0, src1.lo)
  //    sub_hi = add(src0.hi, add(-borrow, -src1.hi))
  //    sub64  = combine(sub_lo, sub_hi)
  using namespace GenXIntrinsic::GenXResult;
  auto *SubbVal = Builder.CreateCall(SubbFunct, {Src0.Lo, Src1.Lo}, "subb");
  auto *SubLo = Builder.CreateExtractValue(SubbVal, {IdxSubb_Sub}, "subb.sub");
  auto *Borrow =
      Builder.CreateExtractValue(SubbVal, {IdxSubb_Borrow}, "subb.borrow");
  auto *MinusBorrow = Builder.CreateNeg(Borrow, "borrow.negate");
  auto *MinusS1Hi = Builder.CreateNeg(Src1.Hi, "negative.src1_hi");
  auto *SubHi = buildTernaryAddition(Builder, *Src0.Hi, *MinusBorrow,
                                     *MinusS1Hi, "sub_hi");
  return SplitBuilder.combineLoHiSplit(
      {SubLo, SubHi}, Twine("int_emu.") + Op.getOpcodeName() + ".",
      Inst.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::visitAnd(BinaryOperator &Op) {
  return expandBitwiseOp(Op);
}
Value *GenXEmulate::Emu64Expander::visitOr(BinaryOperator &Op) {
  return expandBitwiseOp(Op);
}
Value *GenXEmulate::Emu64Expander::visitXor(BinaryOperator &Op) {
  if (auto *NotOperand = detectBitwiseNot(Op)) {
    unsigned OperandIdx = NotOperand == Op.getOperand(0) ? 0 : 1;
    auto Src0 = SplitBuilder.splitOperandHalf(OperandIdx);
    auto *Part1 = BinaryOperator::CreateNot(Src0.Left, ".part1_not", &Inst);
    auto *Part2 = BinaryOperator::CreateNot(Src0.Right, ".part2_not", &Inst);
    return SplitBuilder.combineHalfSplit({Part1, Part2}, "int_emu.not.",
                                         Op.getType()->isIntegerTy());
  }
  return expandBitwiseOp(Op);
}
GenXEmulate::Emu64Expander::VectorInfo
GenXEmulate::Emu64Expander::toVector(IRBuilder &Builder, Value *In) {
  if (In->getType()->isVectorTy())
    return {In, cast<VectorType>(In->getType())};

  if (auto *CIn = dyn_cast<ConstantInt>(In)) {
    uint64_t CVals[] = {CIn->getZExtValue()};
    auto *VectorValue = ConstantDataVector::get(In->getContext(), CVals);
    return {VectorValue, cast<VectorType>(VectorValue->getType())};
  }
  auto *VTy = IGCLLVM::FixedVectorType::get(In->getType(), 1);
  auto *VectorValue = Builder.CreateBitCast(In, VTy);
  return {VectorValue, VTy};
  // Note: alternatively, we could do something like this:
  // Value *UndefVector = UndefValue::get(VTy);
  // return Builder.CreateInsertElement(UndefVector, In, (uint64_t)0, ...
}
bool GenXEmulate::Emu64Expander::getConstantUI32Values(
    Value *V, SmallVectorImpl<uint32_t> &Result) {

  auto FitsUint32 = [](uint64_t V) {
    return V <= std::numeric_limits<uint32_t>::max();
  };
  Result.clear();
  if (auto *Scalar = dyn_cast<ConstantInt>(V)) {
    uint64_t Value = Scalar->getZExtValue();
    if (!FitsUint32(Value))
      return false;
    Result.push_back(Value);
    return true;
  }
  auto *SeqVal = dyn_cast<ConstantDataSequential>(V);
  if (!SeqVal)
    return false;

  Result.reserve(SeqVal->getNumElements());
  for (unsigned i = 0; i < SeqVal->getNumElements(); ++i) {
    auto *CV = dyn_cast_or_null<ConstantInt>(SeqVal->getAggregateElement(i));
    if (!CV)
      return false;
    uint64_t Value = CV->getZExtValue();
    if (!FitsUint32(Value))
      return false;
    Result.push_back(Value);
  }
  return true;
}
Value *GenXEmulate::Emu64Expander::visitSelectInst(SelectInst &I) {
  auto SrcTrue = SplitBuilder.splitOperandLoHi(1);
  auto SrcFalse = SplitBuilder.splitOperandLoHi(2);
  auto *Cond = I.getCondition();

  auto Builder = getIRBuilder();
  // sel from 64-bit values transforms as:
  //    split TrueVal and FalseVal on lo/hi parts
  //    lo_part = self(cond, src0.l0, src1.lo)
  //    hi_part = self(cond, src0.hi, src1.hi)
  //    result  = combine(lo_part, hi_part)
  auto *SelLo = Builder.CreateSelect(Cond, SrcTrue.Lo, SrcFalse.Lo, "sel.lo");
  auto *SelHi = Builder.CreateSelect(Cond, SrcTrue.Hi, SrcFalse.Hi, "sel.hi");
  return SplitBuilder.combineLoHiSplit(
      {SelLo, SelHi}, Twine("int_emu.") + I.getOpcodeName() + ".",
      I.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::visitICmp(ICmpInst &Cmp) {
  if (!OptIcmpEnable)
    return nullptr;

  auto Builder = getIRBuilder();

  if (isI64PointerOp(Cmp)) {

    if (!OptProcessPtrs) {
      LLVM_DEBUG(dbgs() << "i64-emu::WARNING: " << Cmp << " won't be emulated\n");
      return nullptr;
    }

    Type *Ty64 = Builder.getInt64Ty();
    if (Cmp.getType()->isVectorTy()) {
      auto NumElements = cast<VectorType>(Cmp.getType())->getNumElements();
      Ty64 = IGCLLVM::FixedVectorType::get(Ty64, NumElements);
    }
    auto *IL = Builder.CreatePtrToInt(Cmp.getOperand(0), Ty64);
    auto *IR = Builder.CreatePtrToInt(Cmp.getOperand(1), Ty64);
    // Create new 64-bit compare
    auto *NewICMP = Builder.CreateICmp(Cmp.getPredicate(), IL, IR);
    return ensureEmulated(NewICMP);
  }

  unsigned BaseOperand = 0;
  IVSplitter Splitter(Cmp, &BaseOperand);
  auto Src0 = Splitter.splitOperandLoHi(0);
  auto Src1 = Splitter.splitOperandLoHi(1);

  bool PartialPredicate =
      std::any_of(Cmp.user_begin(), Cmp.user_end(), [](const User *U) {
        auto IID = GenXIntrinsic::getAnyIntrinsicID(U);
        return IID == GenXIntrinsic::genx_wrpredregion ||
               IID == GenXIntrinsic::genx_wrpredpredregion;
      });
  Value *Result = buildGeneralICmp(Builder, Cmp.getPredicate(),
                                   PartialPredicate, Src0, Src1);

  if (Cmp.getType()->isIntegerTy() && !Result->getType()->isIntegerTy()) {
    // we expect this cast to be possible
    IGC_ASSERT(Cmp.getType() == Result->getType()->getScalarType());
    Result = Builder.CreateBitCast(Result, Cmp.getType(),
                                   Result->getName() + ".toi");
  }
  return Result;
}
Value *GenXEmulate::Emu64Expander::visitShl(BinaryOperator &Op) {

  auto Builder = getIRBuilder();

  llvm::SmallVector<uint32_t, 8> ShaVals;
  if (getConstantUI32Values(Op.getOperand(1), ShaVals)) {
    auto *Result = tryOptimizedShl(Builder, SplitBuilder, Op, ShaVals);
    if (Result)
      return Result;
  }

  auto L = SplitBuilder.splitOperandLoHi(0);
  auto R = SplitBuilder.splitOperandLoHi(1);

  auto SI = constructShiftInfo(Builder, R.Lo);
  ConstantEmitter K(L.Lo);

  // Shift Left
  // 1. Calculate MASK1. MASK1 is 0 when the shift is >= 32 (large shift)
  // 2. Calculate MASK0. MASK0 is 0 iff the shift is 0
  // 3. Calculate Lo part:
  //    [(L.Lo *SHL* SHA) *AND* MASK1 | MASK1 to ensure zero if large shift
  auto *Lo = Builder.CreateAnd(Builder.CreateShl(L.Lo, SI.Sha), SI.Mask1);
  // 4. Calculate Hi part:
  // Hl1: [L.Lo *SHL* (SHA - 32)] *AND* ~MASK1 | shifted out values, large shift
  // Hl2: [(L.Lo *AND* MASK0) *LSR* (32 - SHA)] *AND* MASK1 | nz for small shift
  // Hh:  [(L.Hi *SHL* Sha)] *AND* MASK1 | MASK1 discards result if large shift
  // Hi:  *OR* the above
  // NOTE: SI.Sh32 == (32 - SHA)
  auto *Hl1 = Builder.CreateShl(L.Lo, Builder.CreateNeg(SI.Sh32));
  Hl1 = Builder.CreateAnd(Hl1, Builder.CreateNot(SI.Mask1));

  auto *Hl2 = Builder.CreateLShr(Builder.CreateAnd(L.Lo, SI.Mask0), SI.Sh32);
  Hl2 = Builder.CreateAnd(Hl2, SI.Mask1);

  auto *Hh = Builder.CreateAnd(Builder.CreateShl(L.Hi, SI.Sha), SI.Mask1);

  auto *Hi = Builder.CreateOr(Hh, Builder.CreateOr(Hl1, Hl2));
  return SplitBuilder.combineLoHiSplit(
      {Lo, Hi}, Twine("int_emu.") + Op.getOpcodeName() + ".",
      Op.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::visitLShr(BinaryOperator &Op) {
  return buildRightShift(SplitBuilder, Op);
}
Value *GenXEmulate::Emu64Expander::visitAShr(BinaryOperator &Op) {
  return buildRightShift(SplitBuilder, Op);
}
Value *GenXEmulate::Emu64Expander::visitFPToUI(FPToUIInst &Op) {

  if (Op.getType()->getScalarType() == Type::getDoubleTy(Op.getContext()))
    report_fatal_error("int_emu: double->UI conversions are not supported");

  // TODO: try to detect the case where operand is a constant expression
  // and do the covertion manually
  auto Builder = getIRBuilder();
  const bool IsSigned = false;
  auto *V = buildFPToI64(*Op.getModule(), Builder, SplitBuilder,
                         Op.getOperand(0), IsSigned);
  return Builder.CreateBitCast(V, Op.getType(),
                               Twine(Op.getOpcodeName()) + ".emu");
}
Value *GenXEmulate::Emu64Expander::visitFPToSI(FPToSIInst &Op) {

  if (Op.getType()->getScalarType() == Type::getDoubleTy(Op.getContext()))
    report_fatal_error("int_emu: double->SI conversions are not supported");

  // TODO: try to detect the case where operand is a constant expression
  // and do the covertion manually
  auto Builder = getIRBuilder();
  const bool IsSigned = true;
  auto *V = buildFPToI64(*Op.getModule(), Builder, SplitBuilder,
                         Op.getOperand(0), IsSigned);
  return Builder.CreateBitCast(V, Op.getType(),
                               Twine(Op.getOpcodeName()) + ".emu");
}
Value *GenXEmulate::Emu64Expander::visitUIToFP(UIToFPInst &Op) {

  if (Op.getType()->getScalarType() == Type::getDoubleTy(Op.getContext()))
    report_fatal_error("int_emu: UI->double conversions are not supported");

  auto Builder = getIRBuilder();
  auto UI64 = SplitBuilder.splitOperandLoHi(0);
  ConstantEmitter K(UI64.Lo);

  Function *LzdF = GenXIntrinsic::getAnyDeclaration(
      Op.getModule(), GenXIntrinsic::genx_lzd, {UI64.Hi->getType()});
  Value *Lz = Builder.CreateCall(LzdF, UI64.Hi, "int_emu.ui2fp.lzd.");
  // sp: 1|8|23
  // we need to get that nice first set bit into bit position 23.
  // thus we shift our nice pair of values by 63 - 23 - clz,
  // some bits will be dropped by shift thus we'll add 1 bits as R bit.
  // uint8_t shift = 39 - lz;
  const unsigned kMaxDroppedMantBits = 39;
  Value *DroppedBits = Builder.CreateSub(K.getSplat(kMaxDroppedMantBits), Lz);
  auto SI = constructShiftInfo(Builder, DroppedBits);
  // mantissa = LoPartOf(shr64(data_h, data_l, shift))
  Value *Mant = buildPartialRShift(Builder, UI64.Lo, UI64.Hi, SI);

  // bool sticky_h = (data_h & ~mask) & ((1 << (shift - 32)) - 1);
  auto *TmpShA = Builder.CreateShl(K.getSplat(1), Builder.CreateNeg(SI.Sh32));
  auto *TmpMask = Builder.CreateSub(TmpShA, K.getSplat(1));
  auto *StickyH = Builder.CreateAnd(UI64.Hi, Builder.CreateNot(SI.Mask1));
  StickyH = Builder.CreateAnd(StickyH, TmpMask);

  // bool sticky_l = (data_l & ~mask) || ((data_l & (mask >> shift));
  auto *SL1 = Builder.CreateAnd(UI64.Lo, Builder.CreateNot(SI.Mask1));
  auto *SL2 = Builder.CreateAnd(UI64.Lo, Builder.CreateLShr(SI.Mask1, SI.Sh32));
  auto *StickyL = Builder.CreateOr(SL1, SL2);

  // Calculate RS
  // bool S = sticky_h | sticky_l;
  auto *S = Builder.CreateOr(StickyH, StickyL);
  S = Builder.CreateICmpEQ(S, K.getZero());

  auto *notS = Builder.CreateSelect(S, K.getOnes(), K.getZero());

  // R = Mant & 1
  auto *R = Builder.CreateAnd(Mant, K.getSplat(1));
  // mant = (mant + 0x1) >> 1;
  Mant =
      Builder.CreateLShr(Builder.CreateAdd(Mant, K.getSplat(1)), K.getSplat(1));
  // mant &= ~(!S & R); // R is set but no S, round to even.
  auto *RoundMask = Builder.CreateNot(Builder.CreateAnd(notS, R));
  Mant = Builder.CreateAnd(Mant, RoundMask);
  // 0xbd - Lz
  const unsigned kMaxValueExp = 0xbd;
  auto *Exp = Builder.CreateSub(K.getSplat(kMaxValueExp), Lz);
  auto *ResultLarge = Builder.CreateShl(Exp, K.getSplat(23));
  ResultLarge = Builder.CreateAdd(ResultLarge, Mant);

  // NOTE: at this point ResultLarge is a integer vector
  // Since we calculate "optimized" route through creating yes another
  // UIToFP instrucion (on i32) and this shall be a vector operation,
  // all further calculatoins assume that we always process vectors
  // The cast to the final type (scalar or vector) shall be done at the end
  auto *VFPTy = Op.getType();
  if (!VFPTy->isVectorTy())
    VFPTy = IGCLLVM::FixedVectorType::get(Builder.getFloatTy(), 1);

  ResultLarge = Builder.CreateBitCast(
      ResultLarge, VFPTy, Twine("int_emu.ui2f.l.") + Op.getOpcodeName());
  auto *ResultSmall = Builder.CreateUIToFP(
      UI64.Lo, VFPTy, Twine("int_emu.ui2f.s.") + Op.getOpcodeName());

  auto *IsSmallPred = Builder.CreateICmpEQ(UI64.Hi, K.getZero());
  auto *Result = Builder.CreateSelect(IsSmallPred, ResultSmall, ResultLarge);
  // Final cast to the requested type (usually <1 x float> -> float)
  if (Op.getType() != VFPTy)
    Result = Builder.CreateBitCast(
        Result, Op.getType(), Twine("int_emu.ui2fp.") + Op.getOpcodeName());
  return Result;
}
Value *GenXEmulate::Emu64Expander::visitSIToFP(SIToFPInst &Op) {

  if (Op.getType()->getScalarType() == Type::getDoubleTy(Op.getContext()))
    report_fatal_error("int_emu: UI->double conversions are not supported");

  // NOTE: SIToFP is special, since it does not do the convert by itself,
  // Instead it just creates a sequence of 64.bit operations which
  // are then expanded. As such some type convertion trickery is involved.
  // Namely, we transform all operands to vector types type as early as possible
  auto Builder = getIRBuilder();
  auto UI64 = SplitBuilder.splitOperandLoHi(0);
  ConstantEmitter K(UI64.Hi);

  auto *SignVal = Builder.CreateAnd(UI64.Hi, K.getSplat(1 << 31));
  auto *PredSigned = Builder.CreateICmpNE(SignVal, K.getZero());

  auto *VOprnd = toVector(Builder, Op.getOperand(0)).V;
  // This would be a 64-bit operation on a vector types
  auto *NegatedOpnd = Builder.CreateNeg(VOprnd);
  // this could be a constexpr - in this case, no emulation necessary
  if (auto *NegOp64 = dyn_cast<Instruction>(NegatedOpnd)) {
    Value *NewInst = Emu64Expander(ST, *NegOp64).tryExpand();
    IGC_ASSERT(NewInst);
    NegOp64->eraseFromParent();
    NegatedOpnd = NewInst;
  }

  auto *AbsOp64 =
      cast<Instruction>(Builder.CreateSelect(PredSigned, NegatedOpnd, VOprnd));
  auto *AbsVal = Emu64Expander(ST, *AbsOp64).tryExpand();
  IGC_ASSERT(AbsVal);
  AbsOp64->eraseFromParent();

  Type *CnvType = Op.getType();
  if (!Op.getType()->isVectorTy()) {
    CnvType = IGCLLVM::FixedVectorType::get(Builder.getFloatTy(), 1);
  }
  auto *Cnv64 = cast<Instruction>(Builder.CreateUIToFP(AbsVal, CnvType));
  // Now the convert holds the <N x float> vector
  auto *Cnv = Emu64Expander(ST, *Cnv64).tryExpand();
  IGC_ASSERT(Cnv);
  Cnv64->eraseFromParent();

  // we want to set a proper sign, so we cast it to <N x int>,
  // set sign bit and cast-away to the final result
  Value *AsInt = Builder.CreateBitCast(Cnv, K.getVTy());
  auto *Result = Builder.CreateOr(AsInt, SignVal);
  return Builder.CreateBitCast(Result, Op.getType());
}
Value *GenXEmulate::Emu64Expander::visitZExtInst(ZExtInst &I) {
  auto Builder = getIRBuilder();
  auto VOp = toVector(Builder, I.getOperand(0));
  Value *LoPart = VOp.V;
  if (VOp.VTy->getScalarType()->getPrimitiveSizeInBits() < 32) {
    auto *ExtendedType =
        IGCLLVM::FixedVectorType::get(Builder.getInt32Ty(), VOp.VTy->getNumElements());
    LoPart = Builder.CreateZExt(LoPart, ExtendedType, ".zext32");
  }
  auto *ZeroValue = Constant::getNullValue(LoPart->getType());
  return SplitBuilder.combineLoHiSplit({LoPart, ZeroValue}, "int_emu.zext64.",
                                       Inst.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::visitSExtInst(SExtInst &I) {
  auto Builder = getIRBuilder();
  auto VOp = toVector(Builder, I.getOperand(0));
  auto *LoPart = VOp.V;
  if (VOp.VTy->getScalarType()->getPrimitiveSizeInBits() < 32) {
    auto *ExtendedType =
        IGCLLVM::FixedVectorType::get(Builder.getInt32Ty(), VOp.VTy->getNumElements());
    LoPart = Builder.CreateSExt(LoPart, ExtendedType, ".sext32");
  }
  auto *HiPart = Builder.CreateAShr(LoPart, 31u, ".sign_hi");
  return SplitBuilder.combineLoHiSplit({LoPart, HiPart}, "int_emu.sext64.",
                                       Inst.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::visitPtrToInt(PtrToIntInst &I) {

  const DataLayout &DL = I.getModule()->getDataLayout();
  // do not emulate noop
  if (cast<CastInst>(&I)->isNoopCast(DL))
    return nullptr;

  if (!OptProcessPtrs) {
    LLVM_DEBUG(dbgs() << "i64-emu::WARNING: " << I << " won't be emulated\n");
    return nullptr;
  }
  // ptr32 -> i64 conversions are not supported
  if (DL.getTypeSizeInBits(I.getOperand(0)->getType()->getScalarType()) <
      DL.getTypeSizeInBits(I.getType()->getScalarType())) {
    LLVM_DEBUG(dbgs() << "i64-emu::ERROR: " << I << " can't be emulated\n");
    report_fatal_error("int_emu: ptr32->i64 extensions are not supported");
  }

  auto Builder = getIRBuilder();
  auto VOp = toVector(Builder, I.getOperand(0));

  auto *VTy64 = IGCLLVM::FixedVectorType::get(Builder.getInt64Ty(),
                                              VOp.VTy->getNumElements());
  auto *Cast = Builder.CreatePtrToInt(VOp.V, VTy64);

  auto *ResTy = I.getType();
  unsigned Stride =
      VTy64->getPrimitiveSizeInBits() / ResTy->getPrimitiveSizeInBits();
  unsigned NumElements = VOp.VTy->getNumElements();

  auto *VElTy = IGCLLVM::FixedVectorType::get(ResTy->getScalarType(),
                                              Stride * NumElements);
  auto *ElCast = Builder.CreateBitCast(Cast, VElTy, "int_emu.ptr2int.elcast.");
  genx::Region R(ElCast);
  R.NumElements = NumElements;
  R.Stride = Stride;
  R.Width = NumElements;
  R.VStride = R.Stride * R.Width;
  auto *Result = (Value *)R.createRdRegion(
      ElCast, "int_emu.trunc." + I.getName() + ".", &I, I.getDebugLoc());
  if (Result->getType() != ResTy) {
    Result = Builder.CreateBitCast(
        Result, ResTy, Twine("int_emu.trunc.") + I.getName() + ".to_s.");
  }
  return Result;
}
Value *GenXEmulate::Emu64Expander::visitIntToPtr(IntToPtrInst &I) {

  const DataLayout &DL = I.getModule()->getDataLayout();
  // do not emulate noop
  if (cast<CastInst>(&I)->isNoopCast(DL))
    return nullptr;

  if (!OptProcessPtrs) {
    LLVM_DEBUG(dbgs() << "i64-emu::WARNING: " << I << " won't be emulated\n");
    return nullptr;
  }
  // i64 -> ptr32 truncations are not supported
  if (DL.getTypeSizeInBits(I.getOperand(0)->getType()->getScalarType()) >
      DL.getTypeSizeInBits(I.getType()->getScalarType())) {
    LLVM_DEBUG(dbgs() << "i64-emu::ERROR: " << I << " can't be emulated\n");
    report_fatal_error("int_emu: i64->ptr32 truncations are not supported");
  }

  auto Builder = getIRBuilder();
  auto VOp = toVector(Builder, I.getOperand(0));

  auto *VTy32 = IGCLLVM::FixedVectorType::get(Builder.getInt32Ty(),
                                              VOp.VTy->getNumElements());
  auto *VTy64 = IGCLLVM::FixedVectorType::get(Builder.getInt64Ty(),
                                              VOp.VTy->getNumElements());
  Value *VI32 = VOp.V;
  if (VOp.VTy != VTy32)
    VI32 = Builder.CreateZExt(VOp.V, VTy32);

  auto *Zext64 = Builder.CreateZExt(VI32, VTy64);
  auto *Zext = ensureEmulated(Zext64);

  Type *ResType = I.getType();
  Type *CnvType = ResType;
  if (!ResType->isVectorTy()) {
    CnvType = IGCLLVM::FixedVectorType::get(ResType, 1);
  }
  auto *Result = Builder.CreateIntToPtr(Zext, CnvType);
  if (ResType != CnvType) {
    Result = Builder.CreateBitCast(Result, ResType,
                                   Twine("int_emu.") + I.getOpcodeName() + ".");
  }
  return Result;
}
Value *GenXEmulate::Emu64Expander::visitGenxTrunc(CallInst &CI) {

  auto IID = GenXIntrinsic::getAnyIntrinsicID(&Inst);
  unsigned DstSize = CI.getType()->getScalarType()->getPrimitiveSizeInBits();
  IGC_ASSERT(DstSize == 8 || DstSize == 16 || DstSize == 32 || DstSize == 64);

  // early exit
  if (IID == GenXIntrinsic::genx_uutrunc_sat ||
      IID == GenXIntrinsic::genx_sstrunc_sat) {
    if (DstSize == 64)
      return CI.getOperand(0);
  }

  auto Builder = getIRBuilder();
  auto VOp = toVector(Builder, CI.getOperand(0));

  auto MakeConstantSplat64 = [](IRBuilder &B, VectorType *VTy, uint64_t Value) {
     auto* KV = Constant::getIntegerValue(B.getInt64Ty(), APInt(64, Value));
     return ConstantDataVector::getSplat(VTy->getNumElements(), KV);
  };
  auto MaxDstSigned   = [&](unsigned DstSize) {
     uint64_t MaxVal = (1ull << (DstSize - 1)) - 1;
     return MakeConstantSplat64(Builder, VOp.VTy, MaxVal);
  };
  auto MinDstSigned   = [&](unsigned DstSize) {
     uint64_t Ones = ~0ull;
     uint64_t MinVal = Ones << (DstSize - 1);
     return MakeConstantSplat64(Builder, VOp.VTy, MinVal);
  };
  auto MaxDstUnsigned = [&](unsigned DstSize) {
     uint64_t MaxVal = ~0ull;
     MaxVal = MaxVal >> (64 - DstSize);
     return MakeConstantSplat64(Builder, VOp.VTy, MaxVal);
  };
  auto MinDstUnsigned = [&](unsigned DstSize) {
     return MakeConstantSplat64(Builder, VOp.VTy, 0);
  };

  Value *Cond1 = nullptr;
  Value *Limit1 = nullptr;
  // optional
  Value *Cond2 = nullptr;
  Value *Limit2 = nullptr;

  switch (IID) {
  case GenXIntrinsic::genx_uutrunc_sat:
    // UGT maxDstUnsigend -> maxDstUnsigned
    Limit1 = MaxDstUnsigned(DstSize);
    Cond1 = ensureEmulated(Builder.CreateICmpUGT(VOp.V, Limit1));
  break;
  case GenXIntrinsic::genx_sstrunc_sat:
    // Result = Operand
    // SGT (maxDstSigned) -> maxDstSigned
    // SLT (minDstSigned) -> minDstSigned
    // trunc
    Limit1 = MaxDstSigned(DstSize);
    Cond1 = ensureEmulated(Builder.CreateICmpSGT(VOp.V, Limit1));
    Limit2 = MinDstSigned(DstSize);
    Cond2 = ensureEmulated(Builder.CreateICmpSLT(VOp.V, Limit2));
  break;
  case GenXIntrinsic::genx_ustrunc_sat: // unsigned result, signed operand
    // UGE (maxDstUnsigned) -> maxDstSigned
    // Operand < 0 -> 0
    // trunc
    Limit1 = MaxDstUnsigned(DstSize);
    Cond1 = ensureEmulated(Builder.CreateICmpUGE(VOp.V, Limit1));
    Limit2 = MinDstUnsigned(DstSize);
    Cond2 = ensureEmulated(Builder.CreateICmpSLT(VOp.V, Limit2));
  break;
  case GenXIntrinsic::genx_sutrunc_sat: // signed result, unsigned operand
    // UGT (maxDstSigned) -> maxDstSigned
    // trunc
    Limit1 = MaxDstSigned(DstSize);
    Cond1 = ensureEmulated(Builder.CreateICmpUGT(VOp.V, Limit1));
  break;
  }
  IGC_ASSERT(Cond1 && Limit1);
  auto *Result = ensureEmulated(Builder.CreateSelect(Cond1, Limit1, VOp.V));
  if (Cond2) {
    Result = ensureEmulated(Builder.CreateSelect(Cond2, Limit2, Result));
  }
  if (DstSize <= 32) {
    auto Splitted = SplitBuilder.splitValueLoHi(*Result);
    if (DstSize == 32) {
      Result = Splitted.Lo;
    } else {
      // DIRTY HACK: since currently our backend does not support
      // llvm trunc instruction, we just build a 32-bit trunc.sat instead
      unsigned ElNum = VOp.VTy->getNumElements();
      auto *CnvType =
          IGCLLVM::FixedVectorType::get(CI.getType()->getScalarType(), ElNum);
      // Result = Builder.CreateTrunc(Result, CnvType);
      Function *TrSatF = GenXIntrinsic::getAnyDeclaration(
              CI.getModule(), IID, {CnvType, Splitted.Lo->getType()});
      Result = Builder.CreateCall(TrSatF, Splitted.Lo, "int_emu.trunc.sat.small.");
    }
  }
  if (Result->getType() == CI.getType())
    return Result;

  return Builder.CreateBitCast(Result, CI.getType());
}
Value *GenXEmulate::Emu64Expander::visitGenxMinMax(CallInst &CI) {

  auto Builder = getIRBuilder();
  Value* Lhs = CI.getOperand(0);
  Value* Rhs = CI.getOperand(1);

  Value* CondVal = nullptr;
  // We create 2 64-bit operations:
  // compare and select.
  // Then we replace those with yet-another expander instance
  auto IID = GenXIntrinsic::getAnyIntrinsicID(&Inst);
  switch (IID) {
  case GenXIntrinsic::genx_umax:
    CondVal = Builder.CreateICmpUGT(Lhs, Rhs);
    break;
  case GenXIntrinsic::genx_smax:
    CondVal = Builder.CreateICmpSGT(Lhs, Rhs);
    break;
  case GenXIntrinsic::genx_umin:
    CondVal = Builder.CreateICmpULT(Lhs, Rhs);
    break;
  case GenXIntrinsic::genx_smin:
    CondVal = Builder.CreateICmpSLT(Lhs, Rhs);
    break;
  }
  IGC_ASSERT(CondVal);
  CondVal = ensureEmulated(CondVal);
  return ensureEmulated(Builder.CreateSelect(CondVal, Lhs, Rhs));
}

Value *GenXEmulate::Emu64Expander::visitGenxAbsi(CallInst &CI) {
  auto Builder = getIRBuilder();
  auto Src = SplitBuilder.splitOperandLoHi(0);
  // we check the sign, and if
  ConstantEmitter K(Src.Hi);
  auto *VOprnd = toVector(Builder, CI.getOperand(0)).V;
  // This would be a 64-bit operation on a vector types
  auto *NegatedOpnd = Builder.CreateNeg(VOprnd);
  // this could be a constexpr - in this case, no emulation necessary
  if (auto *NegOp64 = dyn_cast<Instruction>(NegatedOpnd)) {
    Value *NewInst = Emu64Expander(ST, *NegOp64).tryExpand();
    IGC_ASSERT(NewInst);
    NegOp64->eraseFromParent();
    NegatedOpnd = NewInst;
  }
  auto NegSplit = SplitBuilder.splitValueLoHi(*NegatedOpnd);

  auto *FlagSignSet = Builder.CreateICmpSLT(Src.Hi, K.getZero());
  auto *Lo = Builder.CreateSelect(FlagSignSet, NegSplit.Lo, Src.Lo);
  auto *Hi = Builder.CreateSelect(FlagSignSet, NegSplit.Hi, Src.Hi);

  return SplitBuilder.combineLoHiSplit({Lo, Hi}, "int_emu.genxabsi.",
                                       CI.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::visitGenxAddSat(CallInst &CI) {

  auto Src0 = SplitBuilder.splitOperandLoHi(0);
  auto Src1 = SplitBuilder.splitOperandLoHi(1);

  auto *M = CI.getModule();

  auto Builder = getIRBuilder();
  ConstantEmitter K(Src0.Lo);

  auto IID = GenXIntrinsic::getAnyIntrinsicID(&Inst);
  switch (IID) {
  case GenXIntrinsic::genx_uuadd_sat: {
    auto LoAdd = buildAddc(M, Builder, *Src0.Lo, *Src1.Lo, "int_emu.uuadd.lo");
    auto HiAdd1 =
        buildAddc(M, Builder, *Src0.Hi, *Src1.Hi, "int_emu.uuadd.hi1.");
    // add carry from low part
    auto HiAdd2 =
        buildAddc(M, Builder, *HiAdd1.Val, *LoAdd.CB, "int_emu.uuadd.h2.");

    auto *HiResult = HiAdd2.Val;
    auto *Saturated =
        Builder.CreateICmpNE(Builder.CreateOr(HiAdd1.CB, HiAdd2.CB),
                             K.getZero(), "int_emu.uuadd.sat.");
    auto *Lo = Builder.CreateSelect(Saturated, K.getOnes(), LoAdd.Val);
    auto *Hi = Builder.CreateSelect(Saturated, K.getOnes(), HiResult);
    return SplitBuilder.combineLoHiSplit({Lo, Hi}, "int_emu.uuadd.",
                                         CI.getType()->isIntegerTy());
  } break;
  case GenXIntrinsic::genx_ssadd_sat: {
    auto LoAdd = buildAddc(M, Builder, *Src0.Lo, *Src1.Lo, "int_emu.ssadd.lo");
    auto HiAdd1 =
        buildAddc(M, Builder, *Src0.Hi, *Src1.Hi, "int_emu.ssadd.hi1.");
    // add carry from low part
    auto HiAdd2 =
        buildAddc(M, Builder, *HiAdd1.Val, *LoAdd.CB, "int_emu.ssadd.h2.");
    // auto F
    auto *MaskBit31    = K.getSplat(1 << 31);
    auto *MaxSigned32  = K.getSplat((1u << 31u) - 1u);
    //Overflow = (x >> (os - 1)) == (y >> (os - 1)) &&
    //           (x >> (os - 1)) != (result >> (os - 1)) ? 1 : 0;
    auto *SignOp0 = Builder.CreateAnd(Src0.Hi, MaskBit31);
    auto *SignOp1 = Builder.CreateAnd(Src1.Hi, MaskBit31);
    auto *SignRes = Builder.CreateAnd(HiAdd2.Val, MaskBit31);

    auto *FlagSignOpMatch = Builder.CreateICmpEQ(SignOp0, SignOp1);
    auto *FlagSignResMismatch = Builder.CreateICmpNE(SignOp0, SignRes);
    auto *FlagOverflow = Builder.CreateAnd(FlagSignOpMatch, FlagSignResMismatch);

    // by default we assume that we have positive saturation
    auto *Lo = Builder.CreateSelect(FlagOverflow, K.getOnes(), LoAdd.Val);
    auto *Hi = Builder.CreateSelect(FlagOverflow, MaxSigned32, HiAdd2.Val);
    // if negative, change the saturation value
    auto *FlagNegativeSat = Builder.CreateAnd(FlagOverflow,
                                 Builder.CreateICmpSLT(SignOp0, K.getZero()));
    Lo = Builder.CreateSelect(FlagNegativeSat, K.getZero(), Lo);
    Hi = Builder.CreateSelect(FlagNegativeSat, K.getSplat(1 << 31), Hi);

    return SplitBuilder.combineLoHiSplit({Lo, Hi}, "int_emu.ssadd.",
                                         CI.getType()->isIntegerTy());
  } break;
  case GenXIntrinsic::genx_suadd_sat:
    report_fatal_error(
        "int_emu: genx_suadd_sat is not supported by VC backend");
    break;
  case GenXIntrinsic::genx_usadd_sat:
    report_fatal_error(
        "int_emu: genx_usadd_sat is not supported by VC backend");
    break;
  default:
    IGC_ASSERT_MESSAGE(0, "unknown intrinsic passed to saturation add emu");
  }
  return nullptr;
}
Value *GenXEmulate::Emu64Expander::visitCallInst(CallInst &CI) {
  switch (GenXIntrinsic::getAnyIntrinsicID(&Inst)) {
  case GenXIntrinsic::genx_uutrunc_sat:
  case GenXIntrinsic::genx_sstrunc_sat:
  case GenXIntrinsic::genx_ustrunc_sat:
  case GenXIntrinsic::genx_sutrunc_sat:
    return visitGenxTrunc(CI);
  case GenXIntrinsic::genx_umin:
  case GenXIntrinsic::genx_umax:
  case GenXIntrinsic::genx_smin:
  case GenXIntrinsic::genx_smax:
    return visitGenxMinMax(CI);
  case GenXIntrinsic::genx_absi:
    return visitGenxAbsi(CI);
  case GenXIntrinsic::genx_suadd_sat:
  case GenXIntrinsic::genx_usadd_sat:
  case GenXIntrinsic::genx_uuadd_sat:
  case GenXIntrinsic::genx_ssadd_sat:
    return visitGenxAddSat(CI);
  }
  return nullptr;
}
Value *GenXEmulate::Emu64Expander::ensureEmulated(Value *Val) {
  Instruction *Inst = dyn_cast<Instruction>(Val);
  if (!Inst)
    return Val;
  auto *Emulated = Emu64Expander(ST, *Inst).tryExpand();
  // we expect to always return an emulated sequence
  IGC_ASSERT(Emulated);
  Inst->eraseFromParent();
  return Emulated;
}
Value *GenXEmulate::Emu64Expander::buildTernaryAddition(
    IRBuilder &Builder, Value &A, Value &B, Value &C, const Twine &Name) const {
  auto *SubH = Builder.CreateAdd(&A, &B, Name + ".part");
  return Builder.CreateAdd(SubH, &C, Name);
}
GenXEmulate::Emu64Expander::AddSubExtResult
GenXEmulate::Emu64Expander::buildAddc(Module *M, IRBuilder &Builder, Value &L,
                                      Value &R, const Twine &Prefix) {
  IGC_ASSERT(L.getType() == R.getType());

  auto *AddcFunct = GenXIntrinsic::getGenXDeclaration(
      M, GenXIntrinsic::genx_addc, {L.getType(), R.getType()});

  using namespace GenXIntrinsic::GenXResult;
  auto *AddcVal =
      Builder.CreateCall(AddcFunct, {&L, &R}, Prefix + "aggregate.");
  auto *Add =
      Builder.CreateExtractValue(AddcVal, {IdxAddc_Add}, Prefix + "add.");
  auto *Carry =
      Builder.CreateExtractValue(AddcVal, {IdxAddc_Carry}, Prefix + "carry.");
  return {Add, Carry};
}
GenXEmulate::Emu64Expander::AddSubExtResult
GenXEmulate::Emu64Expander::buildSubb(Module *M, IRBuilder &Builder, Value &L,
                                      Value &R, const Twine &Prefix) {

  IGC_ASSERT(L.getType() == R.getType());

  auto *SubbFunct = GenXIntrinsic::getGenXDeclaration(
      M, GenXIntrinsic::genx_subb, {L.getType(), R.getType()});

  using namespace GenXIntrinsic::GenXResult;
  auto *SubbVal =
      Builder.CreateCall(SubbFunct, {&L, &R}, Prefix + "aggregate.");
  auto *Sub =
      Builder.CreateExtractValue(SubbVal, {IdxSubb_Sub}, Prefix + "sub.");
  auto *Borrow =
      Builder.CreateExtractValue(SubbVal, {IdxSubb_Borrow}, Prefix + "borrow.");
  return {Sub, Borrow};
}
Value *GenXEmulate::Emu64Expander::buildGeneralICmp(IRBuilder &Builder,
                                                    CmpInst::Predicate P,
                                                    bool IsPartialPredicate,
                                                    const LHSplit &Src0,
                                                    const LHSplit &Src1) {

  auto getEmulateCond1 = [](const CmpInst::Predicate P) {
    // For the unsigned predicate the first condition stays the same
    if (CmpInst::isUnsigned(P))
      return P;
    switch (P) {
    // transform signed predicate to an unsigned one
    case CmpInst::ICMP_SGT:
      return CmpInst::ICMP_UGT;
    case CmpInst::ICMP_SGE:
      return CmpInst::ICMP_UGE;
    case CmpInst::ICMP_SLT:
      return CmpInst::ICMP_ULT;
    case CmpInst::ICMP_SLE:
      return CmpInst::ICMP_ULE;
    default:
      llvm_unreachable("unexpected ICMP predicate for first condition");
    }
  };
  auto getEmulateCond2 = [](const CmpInst::Predicate P) {
    // discard EQ part
    switch (P) {
    case CmpInst::ICMP_SGT:
    case CmpInst::ICMP_SGE:
      return CmpInst::ICMP_SGT;
    case CmpInst::ICMP_SLT:
    case CmpInst::ICMP_SLE:
      return CmpInst::ICMP_SLT;
    case CmpInst::ICMP_UGT:
    case CmpInst::ICMP_UGE:
      return CmpInst::ICMP_UGT;
    case CmpInst::ICMP_ULT:
    case CmpInst::ICMP_ULE:
      return CmpInst::ICMP_ULT;
    default:
      llvm_unreachable("unexpected ICMP predicate for second condition");
    }
  };

  std::pair<Value *, Value *> ResultParts = {};
  switch (P) {
  case CmpInst::ICMP_EQ: {
    auto *T0 = Builder.CreateICmpEQ(Src0.Lo, Src1.Lo);
    auto *T1 = Builder.CreateICmpEQ(Src0.Hi, Src1.Hi);
    ResultParts = {T0, T1};
    break;
  }
  case CmpInst::ICMP_NE: {
    auto *T0 = Builder.CreateICmpNE(Src0.Lo, Src1.Lo);
    auto *T1 = Builder.CreateICmpNE(Src0.Hi, Src1.Hi);
    ResultParts = {T0, T1};
    break;
  }
  default: {
    CmpInst::Predicate EmuP1 = getEmulateCond1(P);
    CmpInst::Predicate EmuP2 = getEmulateCond2(P);
    auto *T0 = Builder.CreateICmp(EmuP1, Src0.Lo, Src1.Lo);
    auto *T1 = Builder.CreateICmpEQ(Src0.Hi, Src1.Hi);
    auto *T2 = Builder.CreateAnd(T1, T0);
    auto *T3 = Builder.CreateICmp(EmuP2, Src0.Hi, Src1.Hi);
    ResultParts = {T2, T3};
    break;
  }
  }
  auto ResultCond = (P == CmpInst::ICMP_EQ) ? Instruction::BinaryOps::And
                                            : Instruction::BinaryOps::Or;
  if (!IsPartialPredicate || !OptConvertPartialPredicates) {
    return Builder.CreateBinOp(
        ResultCond, ResultParts.first, ResultParts.second,
        "int_emu.cmp." + CmpInst::getPredicateName(P) + ".");
  }
  // Note:
  // The reason for doing this conversion is that our backend has no
  // convinient way to represent partial updates of predicates with anything
  // except for icmp instructions. In the current codebase we have -
  // we are unable to create a proper visa for the following case ("pseudo" IR):
  // bale {
  //   %ne1 = or <8 x i1> %a, %b
  //   %j = call <16 x i1> wrpredregion(<16 x i1> undef, <8 x i1> %ne1, i32 0)
  // }
  // bale {
  //   %ne2 = or <8 x i1> %c, %d
  //   %joined = call <16 x i1> wrpredregion(<16 x i1> %j, <8 x i1> %ne1, i32 8)
  // }
  // As such we convert such cases to the following sequence: 2xsel->or->cmp
  ConstantEmitter K(Src0.Lo);
  auto *L = Builder.CreateSelect(ResultParts.first, K.getOnes(), K.getZero());
  auto *R = Builder.CreateSelect(ResultParts.second, K.getOnes(), K.getZero());
  auto *IPred = Builder.CreateBinOp(ResultCond, L, R,
                                    "int_emu.cmp.part.int." +
                                        CmpInst::getPredicateName(P) + ".");
  return Builder.CreateICmpEQ(IPred, K.getOnes(),
                              "int_emu.cmp.part.i1" +
                                  CmpInst::getPredicateName(P) + ".");
}
Value *GenXEmulate::Emu64Expander::buildRightShift(IVSplitter &SplitBuilder,
                                                   BinaryOperator &Op) {
  auto Builder = getIRBuilder();

  llvm::SmallVector<uint32_t, 8> ShaVals;
  if (getConstantUI32Values(Op.getOperand(1), ShaVals)) {
    auto *Result = tryOptimizedShr(Builder, SplitBuilder, Op, ShaVals);
    if (Result)
      return Result;
  }
  return buildGenericRShift(Builder, SplitBuilder, Op);
}
Value *GenXEmulate::Emu64Expander::tryOptimizedShr(IRBuilder &Builder,
                                                   IVSplitter &SplitBuilder,
                                                   BinaryOperator &Op,
                                                   ArrayRef<uint32_t> Sa) {
  auto Operand = SplitBuilder.splitOperandLoHi(0);
  Value *LoPart{};
  Value *HiPart{};

  ConstantEmitter K(Operand.Lo);

  bool IsLogical = Op.getOpcode() == Instruction::LShr;

  if (std::all_of(Sa.begin(), Sa.end(), LessThan32())) {
    if (std::find(Sa.begin(), Sa.end(), 0) != Sa.end()) {
      // TODO: for now, we bail-out if zero is encountered. Theoretically
      // we could mask-out potentially poisoned values by inserting
      // [cmp/select] pair at the end of the if branch, but for now bailing
      // out is a more safe choice
      return nullptr;
    }
    auto *ShiftA = ConstantDataVector::get(Builder.getContext(), Sa);
    auto *Lo1 = Builder.CreateLShr(Operand.Lo, ShiftA);
    auto *Hi = (IsLogical) ? Builder.CreateLShr(Operand.Hi, ShiftA)
                           : Builder.CreateAShr(Operand.Hi, ShiftA);
    auto *C32 = K.getSplat(32);
    auto *CShift = ConstantExpr::getSub(C32, ShiftA);
    auto *Lo2 = Builder.CreateShl(Operand.Hi, CShift);
    LoPart = Builder.CreateOr(Lo1, Lo2);
    HiPart = Hi;
  } else if (std::all_of(Sa.begin(), Sa.end(), Equals32())) {
    LoPart = Operand.Hi;
    if (IsLogical) {
      HiPart = K.getZero();
    } else {
      auto *C31 = K.getSplat(31);
      HiPart = Builder.CreateAShr(Operand.Hi, C31);
    }
  } else if (std::all_of(Sa.begin(), Sa.end(), GreaterThan32())) {
    auto *C32 = K.getSplat(32);
    auto *CRawShift = ConstantDataVector::get(Builder.getContext(), Sa);
    auto *CShift = ConstantExpr::getSub(CRawShift, C32);
    if (IsLogical) {
      LoPart = Builder.CreateLShr(Operand.Hi, CShift);
      HiPart = K.getZero();
    } else {
      auto *C31 = K.getSplat(31);
      LoPart = Builder.CreateAShr(Operand.Hi, CShift);
      HiPart = Builder.CreateAShr(Operand.Hi, C31);
    }
  } else {
    return nullptr;
  }
  IGC_ASSERT_MESSAGE(LoPart && HiPart, "could not construct optimized shr");
  return SplitBuilder.combineLoHiSplit(
      {LoPart, HiPart}, Twine("int_emu.") + Op.getOpcodeName() + ".",
      Op.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::tryOptimizedShl(IRBuilder &Builder,
                                                   IVSplitter &SplitBuilder,
                                                   BinaryOperator &Op,
                                                   ArrayRef<uint32_t> Sa) {
  auto Operand = SplitBuilder.splitOperandLoHi(0);
  Value *LoPart{};
  Value *HiPart{};

  ConstantEmitter K(Operand.Lo);

  if (std::all_of(Sa.begin(), Sa.end(), LessThan32())) {
    if (std::find(Sa.begin(), Sa.end(), 0) != Sa.end()) {
      // TODO: for now, we bail-out if zero is encountered. Theoretically
      // we could mask-out potentially poisoned values by inserting
      // [cmp/select] pair at the end of the if branch, but for now bailing
      // out seems like safe choice
      return nullptr;
    }
    auto *CRawShift = ConstantDataVector::get(Builder.getContext(), Sa);
    LoPart = Builder.CreateShl(Operand.Lo, CRawShift);
    auto *C32 = K.getSplat(32);
    auto *CShift = ConstantExpr::getSub(C32, CRawShift);
    auto *Hi1 = Builder.CreateShl(Operand.Hi, CRawShift);
    auto *Hi2 = Builder.CreateLShr(Operand.Lo, CShift);
    HiPart = Builder.CreateOr(Hi1, Hi2);
  } else if (std::all_of(Sa.begin(), Sa.end(), Equals32())) {
    LoPart = K.getZero();
    HiPart = Operand.Lo;
  } else if (std::all_of(Sa.begin(), Sa.end(), GreaterThan32())) {
    LoPart = K.getZero();
    auto *C32 = K.getSplat(32);
    auto *CRawShift = ConstantDataVector::get(Builder.getContext(), Sa);
    auto *CShift = ConstantExpr::getSub(CRawShift, C32);
    HiPart = Builder.CreateShl(Operand.Lo, CShift);
  } else {
    return nullptr;
  }
  IGC_ASSERT_MESSAGE(LoPart && HiPart, "could not construct optimized shl");
  return SplitBuilder.combineLoHiSplit(
      {LoPart, HiPart}, Twine("int_emu.") + Op.getOpcodeName() + ".",
      Op.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::buildGenericRShift(IRBuilder &Builder,
                                                      IVSplitter &SplitBuilder,
                                                      BinaryOperator &Op) {

  auto L = SplitBuilder.splitOperandLoHi(0);
  auto R = SplitBuilder.splitOperandLoHi(1);

  auto SI = constructShiftInfo(Builder, R.Lo);
  ConstantEmitter K(L.Lo);

  // Logical Shift Right
  // 1. Calculate MASK1. MASK1 is 0 when the shift is >= 32 (large shift)
  // 2. Calculate MASK0. MASK0 is 0 iff the shift is 0
  // 3. Calculate High part:
  //    [(L.Hi *LSR* Sha) *AND* MASK1], "&" discards result is large shift
  // 4. Calculate Low part:
  //  [(L.Hi & MASK0) *SHL* (32 - SHA)] & MASK1, bits from HI part shifted-out
  //  to LOW
  //  [(L.HI *LSR* (SHA - 32)] & ~MASK1, in case of large shift, all bits occupy
  //  LOW
  //  [(L.Lo *LSR* Sha) *AND* MASK1], "&" discards result if large shift
  //  *OR* the above
  auto *Lo = buildPartialRShift(Builder, L.Lo, L.Hi, SI);
  auto *Hi = Builder.CreateAnd(Builder.CreateLShr(L.Hi, SI.Sha), SI.Mask1);

  bool IsLogical = Op.getOpcode() == Instruction::LShr;
  if (!IsLogical) {
    // Arithmetic Shift Right
    // Do all the steps form Logical Shift
    // 5. SignedMask = L.Hi *ASR* 31
    //    HIPART |= (SignedMask *SHL* (SH32 & MASK1)) & Mask0
    //      HIPART &= Mask0 => apply full SignedMask for large shifts
    //    LOPART |= (SignedMask *SHL* (63 - Sha)) & ~MASK1 =>
    //      LOPART &= ~Mask1 => do not apply this for small shifts
    auto *SignedMask =
        Builder.CreateAShr(L.Hi, K.getSplat(31), "int_emu.asr.sign.");

    auto *AuxHi =
        Builder.CreateShl(SignedMask, Builder.CreateAnd(SI.Sh32, SI.Mask1));
    AuxHi = Builder.CreateAnd(AuxHi, SI.Mask0);

    auto *AuxLo = Builder.CreateShl(SignedMask,
                                    Builder.CreateSub(K.getSplat(63), SI.Sha));
    AuxLo = Builder.CreateAnd(AuxLo, Builder.CreateNot(SI.Mask1));

    Lo = Builder.CreateOr(Lo, AuxLo);
    Hi = Builder.CreateOr(Hi, AuxHi);
  }
  return SplitBuilder.combineLoHiSplit(
      {Lo, Hi}, Twine("int_emu.") + Op.getOpcodeName() + ".",
      Op.getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::buildFPToI64(Module &M, IRBuilder &Builder,
                                                IVSplitter &SplitBuilder,
                                                Value *V, bool IsSigned,
                                                Rounding rnd) {
  (void)rnd; // Currently, only round to zero is supported

  // NOTE: we should factor-out this code to a dedicated function if
  // we'll ever need to emulate custom rounding facilities.

  auto VFOp = toVector(Builder, V);
  Type *I32VTy = IGCLLVM::FixedVectorType::get(Builder.getInt32Ty(),
                                               VFOp.VTy->getNumElements());
  // vector of floats -> vector of ints
  Value *Operand = Builder.CreateBitCast(VFOp.V, I32VTy);
  ConstantEmitter K(Operand);

  auto *Exp = Builder.CreateAnd(Builder.CreateLShr(Operand, K.getSplat(23)),
                                K.getSplat(0xff));
  // mantissa without hidden bit
  auto *PMant = Builder.CreateAnd(Operand, K.getSplat((1u << 23) - 1));
  auto *Shift = Builder.CreateSub(K.getSplat(0xbe), Exp);
  // take hidden bit into account
  auto *Mant = Builder.CreateOr(PMant, K.getSplat(1 << 23));

  auto *DataH = Builder.CreateShl(Mant, K.getSplat(8));
  auto *DataL = K.getZero();

  // The following 3 statements do Logical Shift Right
  auto SI = constructShiftInfo(Builder, Shift);
  auto *Lo = buildPartialRShift(Builder, DataL, DataH, SI);
  auto *Hi = Builder.CreateAnd(Builder.CreateLShr(DataH, SI.Sha), SI.Mask1);

  // Discard results if shift is greater than 63
  auto *MASK = Builder.CreateSelect(
      Builder.CreateICmpUGT(Shift, K.getSplat(63)), K.getZero(), K.getOnes());
  Lo = Builder.CreateAnd(Lo, MASK);
  Hi = Builder.CreateAnd(Hi, MASK);

  auto PredicatedUpdate = [&Builder](Value *Predicate,
                                     const std::pair<Value *, Value *> &New,
                                     const std::pair<Value *, Value *> &Old) {
    Value *Lo = Builder.CreateSelect(Predicate, New.first, Old.first);
    Value *Hi = Builder.CreateSelect(Predicate, New.second, Old.second);
    return std::make_pair(Lo, Hi);
  };

  auto *SignedBit = Builder.CreateAnd(Operand, K.getSplat(1 << 31));
  auto *FlagSignSet = Builder.CreateICmpNE(SignedBit, K.getZero());
  auto *FlagNoSignSet = Builder.CreateNot(FlagSignSet);
  // check for Exponent overflow (when sign bit set)
  auto *FlagExpO = Builder.CreateICmpUGT(Exp, K.getSplat(0xbe));
  auto *FlagExpUO = Builder.CreateAnd(FlagNoSignSet, FlagExpO);
  // signed bit alterations
  if (IsSigned) {
    // calculate (NOT[Lo, Hi] + 1) (integer sign negation)
    auto *NegLo = Builder.CreateNot(Lo);
    auto *NegHi = Builder.CreateNot(Hi);
    auto AddcRes = buildAddc(&M, Builder, *NegLo, *K.getSplat(1),
                             "int_emu.fp2ui.arg_negate.");
    NegHi = Builder.CreateAdd(NegHi, AddcRes.CB);
    // if sign bit is set, alter the result with negated value
    std::tie(Lo, Hi) =
        PredicatedUpdate(FlagSignSet, {AddcRes.Val, NegHi}, {Lo, Hi});
    // Here we process oveflows
    auto K_SOverflow = std::make_pair(K.getZero(), K.getSplat(1u << 31));
    auto K_UOverflow = std::make_pair(K.getOnes(), K.getSplat((1u << 31) - 1));

    // Overflow processing...
    auto *NZ = Builder.CreateICmpNE(Builder.CreateOr(Lo, Hi), K.getZero());
    // (sign ^ ((result_h  >> 31) & 1)))
    auto *SS = Builder.CreateXor(SignedBit,
                                 Builder.CreateAnd(Hi, K.getSplat(1 << 31)));
    auto *NZ2 = Builder.CreateICmpNE(SS, K.getZero());
    auto *Ovrfl = Builder.CreateAnd(NZ, NZ2);
    // In case of overflow, HW response is : 7fffffffffffffff
    std::tie(Lo, Hi) = PredicatedUpdate(Ovrfl, K_UOverflow, {Lo, Hi});
    std::tie(Lo, Hi) = PredicatedUpdate(FlagExpO, K_SOverflow, {Lo, Hi});
    std::tie(Lo, Hi) = PredicatedUpdate(FlagExpUO, K_UOverflow, {Lo, Hi});
  } else {
    auto *Zero = K.getZero();
    auto *Ones = K.getOnes();
    std::tie(Lo, Hi) = PredicatedUpdate(FlagSignSet, {Zero, Zero}, {Lo, Hi});
    std::tie(Lo, Hi) = PredicatedUpdate(FlagExpUO, {Ones, Ones}, {Lo, Hi});
  }

  return SplitBuilder.combineLoHiSplit({Lo, Hi}, "int_emu.fp2i.combine.",
                                       V->getType()->isIntegerTy());
}
Value *GenXEmulate::Emu64Expander::buildPartialRShift(IRBuilder &B,
                                                      Value *SrcLo,
                                                      Value *SrcHi,
                                                      const ShiftInfo &SI) {
  ConstantEmitter K(SrcLo);
  // calculate part which went from hi part to low
  auto *TmpH1 = B.CreateShl(B.CreateAnd(SrcHi, SI.Mask0), SI.Sh32);
  TmpH1 = B.CreateAnd(TmpH1, SI.Mask1);
  // TmpH2 is for the case when the shift amount is greater than 32
  auto *TmpH2 = B.CreateLShr(SrcHi, B.CreateSub(SI.Sha, K.getSplat(32)));
  // Here we mask out tmph2 is the shift is less than 32
  TmpH2 = B.CreateAnd(TmpH2, B.CreateNot(SI.Mask1));
  // Mask1 will ensure that the result is discarded if the shift is large
  auto *TmpL = B.CreateAnd(B.CreateLShr(SrcLo, SI.Sha), SI.Mask1);

  return B.CreateOr(B.CreateOr(TmpL, TmpH1), TmpH2, "int_emu.shif.r.lo.");
}
GenXEmulate::Emu64Expander::ShiftInfo
GenXEmulate::Emu64Expander::constructShiftInfo(IRBuilder &B, Value *RawSha) {
  ConstantEmitter K(RawSha);

  auto *Sha = B.CreateAnd(RawSha, K.getSplat(0x3f), "int_emu.shift.sha.");
  auto *Sh32 = B.CreateSub(K.getSplat(32), Sha, "int_emu.shift.sh32.");
  auto *FlagLargeShift = B.CreateICmpUGE(Sha, K.getSplat(32));
  auto *FlagZeroShift = B.CreateICmpEQ(Sha, K.getSplat(0));

  auto *Mask1 = B.CreateSelect(FlagLargeShift, K.getZero(), K.getOnes());
  auto *Mask0 = B.CreateSelect(FlagZeroShift, K.getZero(), K.getOnes());

  return ShiftInfo{Sha, Sh32, Mask1, Mask0};
}

void GenXEmulate::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetPassConfig>();
  AU.setPreservesCFG();
}

bool GenXEmulate::runOnModule(Module &M) {
  bool Changed = false;
  ST = &getAnalysis<TargetPassConfig>()
            .getTM<GenXTargetMachine>()
            .getGenXSubtarget();
  buildDivRemCache(M);

  // Process non-builtin functions.
  for (auto &F : M.getFunctionList()) {
    if (!isEmulationFunction(&F) && !isOldEmulationFunction(&F))
      runOnFunction(F);
  }
  Changed |= !ToErase.empty();
  for (auto *I : ToErase)
    I->eraseFromParent();
  ToErase.clear();

  // Delete unuse builtins or make used builtins internal.
  for (auto I = M.begin(); I != M.end();) {
    Function &F = *I++;
    if (isEmulationFunction(&F) || isOldEmulationFunction(&F)) {
      Changed = true;
      if (F.use_empty())
        F.eraseFromParent();
      else
        F.setLinkage(GlobalValue::InternalLinkage);
    }
  }

  if (!DiscracedList.empty()) {
    for (const auto *Insn : DiscracedList) {
      llvm::errs() << "I64EMU-FAILURE: " << *Insn << "\n";
    }
    report_fatal_error("int_emu: strict emulation requirements failure", false);
  }
  return Changed;
}

void GenXEmulate::runOnFunction(Function &F) {
  for (auto &BB : F.getBasicBlockList()) {
    for (auto I = BB.begin(); I != BB.end(); ++I) {

      Instruction *Inst = &*I;
      auto *NewVal = emulateInst(Inst);
      if (NewVal) {
        Inst->replaceAllUsesWith(NewVal);
        ToErase.push_back(Inst);
      }
    }
  }
  return;
}

Function *GenXEmulate::getEmulationFunction(Instruction *Inst) const {

  unsigned Opcode = Inst->getOpcode();
  Type *Ty = Inst->getType();
  OpType OpAndType = std::make_pair(Opcode, Ty);

  auto Iter = EmulationFuns.find(OpAndType);
  if (Iter != EmulationFuns.end()) {
    LLVM_DEBUG(dbgs() << "Emulation function: " << Iter->second->getName()
                      << " shall be used for: " << *Inst << "\n");
    return Iter->second;
  }

  return nullptr;
}

void GenXEmulate::buildDivRemCache(Module &M) {
  EmulationFuns.clear();

  IGC_ASSERT(ST);
  bool HasDivRem = ST->hasIntegerDivision();

  if (HasDivRem)
    return;

  auto UpdateCacheIfMatch = [this](Function &F, StringRef PrefixToMatch,
                                   unsigned OpCode) {
    const auto &Name = F.getName();
    if (!Name.startswith(PrefixToMatch))
      return false;

    Type *Ty = F.getReturnType();
    EmulationFuns.insert({{OpCode, Ty}, &F});
    return true;
  };

  for (Function &F : M.getFunctionList()) {
    if (!isEmulationFunction(&F))
      continue;
    if (UpdateCacheIfMatch(F, EmuLibSDivPrefix, BinaryOperator::SDiv))
      continue;
    if (UpdateCacheIfMatch(F, EmuLibSRemPrefix, BinaryOperator::SRem))
      continue;
    if (UpdateCacheIfMatch(F, EmuLibUDivPrefix, BinaryOperator::UDiv))
      continue;
    if (UpdateCacheIfMatch(F, EmuLibURemPrefix, BinaryOperator::URem))
      continue;
  }
}

Value *GenXEmulate::emulateInst(Instruction *Inst) {
  Function *EmuFn = getEmulationFunction(Inst);
  if (EmuFn) {
    IGC_ASSERT(isEmulationFunction(EmuFn));
    IGC_ASSERT_MESSAGE(!isa<CallInst>(Inst), "call emulation not supported yet");
    llvm::IRBuilder<> Builder(Inst);
    SmallVector<Value *, 8> Args(Inst->operands());
    return Builder.CreateCall(EmuFn, Args);
  }
  IGC_ASSERT(ST);
  if (ST->emulateLongLong()) {
    Value *NewInst = Emu64Expander(*ST, *Inst).tryExpand();
    if (!NewInst) {
    }

    return NewInst;
  }
  return nullptr;
}

Instruction *llvm::genx::emulateI64Operation(const GenXSubtarget *ST,
                                             Instruction *Inst,
                                             EmulationFlag AuxAction) {
  LLVM_DEBUG(dbgs() << "i64-emu::WARNING: direct emulation routine was "
                    << "called for " << *Inst << "\n");
  Instruction *NewInst = nullptr;
  if (!ST->hasLongLong()) {
    Value *EmulatedResult = GenXEmulate::Emu64Expander(*ST, *Inst).tryExpand();
    NewInst = cast_or_null<Instruction>(EmulatedResult);
    // If there is no explicit request to enable i64 emulation - report
    // an error
    if (NewInst && !ST->emulateLongLong() && OptStrictEmulationRequests) {
      report_fatal_error("int_emu: target does not suport i64 types", false);
    }
  }

  // NewInst can be nullptr if the instruction does not need emulation,
  // (like various casts)
  if (!NewInst) {
    // if EmulationFlag::RAUWE was requested, then caller expects that
    // that the returned instruction can be safely used.
    if (AuxAction == EmulationFlag::RAUWE)
      return Inst; // return the original instruction
    return nullptr;
  }

  switch (AuxAction) {
  case EmulationFlag::RAUW:
    Inst->replaceAllUsesWith(NewInst);
    break;
  case EmulationFlag::RAUWE:
    Inst->replaceAllUsesWith(NewInst);
    Inst->eraseFromParent();
    break;
  case EmulationFlag::None:
    // do nothing
    break;
  }
  return NewInst;
}
char GenXEmulate::ID = 0;

namespace llvm {
void initializeGenXEmulatePass(PassRegistry &);
}
INITIALIZE_PASS_BEGIN(GenXEmulate, "GenXEmulate", "GenXEmulate", false, false)
INITIALIZE_PASS_END(GenXEmulate, "GenXEmulate", "GenXEmulate", false, false)

ModulePass *llvm::createGenXEmulatePass() {
  initializeGenXEmulatePass(*PassRegistry::getPassRegistry());
  return new GenXEmulate;
}

namespace {
class GenXEmulationImport : public ModulePass {
public:
  static char ID;

  explicit GenXEmulationImport() : ModulePass(ID) {}
  virtual StringRef getPassName() const { return "GenX Emulation BiF Import"; }
  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<TargetPassConfig>();
    AU.addRequired<GenXBackendConfig>();
  }
  bool runOnModule(Module &M) {
    const GenXSubtarget &ST = getAnalysis<TargetPassConfig>()
                                  .getTM<GenXTargetMachine>()
                                  .getGenXSubtarget();

    if (!ST.hasIntegerDivision()) {
      auto ModDivRem32 = LoadDivRem32Lib(M.getContext(), M.getDataLayout(),
                                         M.getTargetTriple());
      if (ModDivRem32 && Linker::linkModules(M, std::move(ModDivRem32)))
        report_fatal_error("Error linking emulation routines");
    }
    return true;
  }

private:
  static bool IsLibraryFunction(Function &F) {
    const auto &Name = F.getName();
    return Name.startswith(LibraryFunctionPrefix);
  }

  static void DeriveRoundingAttributes(Function &F) {

    IGC_ASSERT(IsLibraryFunction(F));

    const auto &Name = F.getName();
    if (Name.contains(RoundingRtzSuffix)) {
      F.addFnAttr(genx::FunctionMD::CMFloatControl,
                  std::to_string(VCRoundingRTZ));
      return;
    }
    if (Name.contains(RoundingRteSuffix)) {
      F.addFnAttr(genx::FunctionMD::CMFloatControl,
                  std::to_string(VCRoundingRTE));
      return;
    }
    if (Name.contains(RoundingRtpSuffix)) {
      F.addFnAttr(genx::FunctionMD::CMFloatControl,
                  std::to_string(VCRoundingRTP));
      return;
    }
    if (Name.contains(RoundingRtnSuffix)) {
      F.addFnAttr(genx::FunctionMD::CMFloatControl,
                  std::to_string(VCRoundingRTN));
      return;
    }
  }

  std::unique_ptr<Module> LoadDivRem32Lib(LLVMContext &Ctx,
                                          const DataLayout &DL,
                                          const std::string &Triple) {

    MemoryBufferRef EmulationBiFBuffer =
        getAnalysis<GenXBackendConfig>().getBiFModule(BiFKind::VCEmulation);

    // NOTE: to simplify LIT testing it is legal to have an empty buffer
    if (!EmulationBiFBuffer.getBufferSize())
      return nullptr;

    auto BiFModule = getBiFModuleOrReportError(EmulationBiFBuffer, Ctx);

    BiFModule->setDataLayout(DL);
    BiFModule->setTargetTriple(Triple);

    for (Function &F : *BiFModule) {
      if (!IsLibraryFunction(F))
        continue;

      F.addFnAttr(genx::FunctionMD::VCEmulationRoutine);
      DeriveRoundingAttributes(F);
    }

    return BiFModule;
  }
};
} // namespace

char GenXEmulationImport::ID = 0;

namespace llvm {
void initializeGenXEmulationImportPass(PassRegistry &);
}
INITIALIZE_PASS_BEGIN(GenXEmulationImport, "GenXEmulationImport",
                      "GenXEmulationImport", false, false)
INITIALIZE_PASS_END(GenXEmulationImport, "GenXEmulationImport",
                    "GenXEmulationImport", false, false)
ModulePass *llvm::createGenXEmulationImportPass() {
  initializeGenXEmulationImportPass(*PassRegistry::getPassRegistry());
  return new GenXEmulationImport;
}
