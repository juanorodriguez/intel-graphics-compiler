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
/// GenXSubtarget : subtarget information
/// -------------------------------------
///
/// GenXSubtarget is the GenX-specific subclass of TargetSubtargetInfo. It takes
/// features detected by the front end (what the Gen architecture is),
/// and exposes flags to the rest of the GenX backend for
/// various features (e.g. whether 64 bit operations are supported).
///
/// Where subtarget features are used is noted in the documentation of GenX
/// backend passes.
///
/// The flags exposed to the rest of the GenX backend are as follows. Most of
/// these are currently not used.
///
//===----------------------------------------------------------------------===//

#ifndef GENXSUBTARGET_H
#define GENXSUBTARGET_H

#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/Pass.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "visa_igc_common_header.h"
#include <string>

#define GET_SUBTARGETINFO_HEADER
#define GET_SUBTARGETINFO_ENUM
#include "GenXGenSubtargetInfo.inc"

namespace llvm {
class GlobalValue;
class Instruction;
class StringRef;
class TargetMachine;

class GenXSubtarget final : public GenXGenSubtargetInfo {

protected:
  // TargetTriple - What processor and OS we're targeting.
  Triple TargetTriple;

  enum GenXTag {
    GENX_GENERIC,
    GENX_HSW,
    GENX_BDW,
    GENX_CHV,
    GENX_SKL,
    GENX_BXT,
    GENX_KBL,
    GENX_GLK,
    GENX_CNL,
    GENX_ICLLP,
    GENX_TGLLP,
    GENX_DG1,
  };

  // GenXVariant - GenX Tag identifying the variant to compile for
  GenXTag GenXVariant;

private:
  // DumpRegAlloc - True if we should dump register allocation information
  bool DumpRegAlloc;

  // EmitCisa Builder - True if we should generate CISA instead of VISA
  bool EmitCisa;

  // HasLongLong - True if subtarget supports long long type
  bool HasLongLong;

  // DisableJmpi - True if jmpi is disabled.
  bool DisableJmpi;

  // DisableVectorDecomposition - True if vector decomposition is disabled.
  bool DisableVectorDecomposition;

  // DisableJumpTables - True if switch to jump tables lowering is disabled.
  bool DisableJumpTables;

  // Only generate warning when callable is used in the middle of the kernel
  bool WarnCallable;
  // Some targets do not support i64 ops natively, we have an option to emulate
  bool EmulateLongLong;

  // True if target supports native 64-bit add
  bool HasAdd64;

  // True if it is profitable to use native DxD->Q multiplication
  bool UseMulDDQ;

  // True if codegenerating for OCL runtime.
  bool OCLRuntime;

  // True if subtarget supports switchjmp visa instruction
  bool HasSwitchjmp;

  // True if subtarget supports integer division
  bool HasIntegerDivision;

  // Shows which surface should we use for stack
  PreDefined_Surface StackSurf;

public:
  // This constructor initializes the data members to match that
  // of the specified triple.
  //
  GenXSubtarget(const Triple &TT, const std::string &CPU,
                const std::string &FS);

  unsigned getGRFWidth() const { return 32; }

  bool isOCLRuntime() const { return OCLRuntime; }

  // ParseSubtargetFeatures - Parses features string setting specified
  // subtarget options.  Definition of function is auto generated by tblgen.
  void ParseSubtargetFeatures(StringRef CPU, StringRef FS);

  // \brief Reset the features for the GenX target.
  void resetSubtargetFeatures(StringRef CPU, StringRef FS);

public:

  /// * isHSW - true if target is HSW
  bool isHSW() const { return GenXVariant == GENX_HSW; }

  /// * isBDW - true if target is BDW
  bool isBDW() const { return GenXVariant == GENX_BDW; }

  /// * isBDWplus - true if target is BDW or later
  bool isBDWplus() const { return GenXVariant >= GENX_BDW; }

  /// * isCHV - true if target is CHV
  bool isCHV() const { return GenXVariant == GENX_CHV; }

  /// * isSKL - true if target is SKL
  bool isSKL() const { return GenXVariant == GENX_SKL; }

  /// * isSKLplus - true if target is SKL or later
  bool isSKLplus() const { return GenXVariant >= GENX_SKL; }

  /// * isBXT - true if target is BXT
  bool isBXT() const { return GenXVariant == GENX_BXT; }


  /// * isKBL - true if target is KBL
  bool isKBL() const { return GenXVariant == GENX_KBL; }

  /// * isGLK - true if target is GLK
  bool isGLK() const { return GenXVariant == GENX_GLK; }

  /// * isCNL - true if target is CNL
  bool isCNL() const { return GenXVariant == GENX_CNL; }

  /// * isCNLplus - true if target is CNL or later
  bool isCNLplus() const { return GenXVariant >= GENX_CNL; }

  /// * isICLLP - true if target is ICL LP
  bool isICLLP() const { return GenXVariant == GENX_ICLLP; }
  /// * isTGLLP - true if target is TGL LP
  bool isTGLLP() const { return GenXVariant == GENX_TGLLP; }
  /// * isDG1 - true if target is DG1
  bool isDG1() const { return GenXVariant == GENX_DG1; }

  /// * emulateLongLong - true if i64 emulation is requested
  bool emulateLongLong() const { return EmulateLongLong; }

  /// * dumpRegAlloc - true if we should dump Reg Alloc info
  bool dumpRegAlloc() const { return DumpRegAlloc; }

  /// * hasLongLong - true if target supports long long
  bool hasLongLong() const { return HasLongLong; }

  /// * hasAdd64 - true if target supports native 64-bit add/sub
  bool hasAdd64() const { return HasAdd64; }

  /// * useMulDDQ - true if is desired to emit DxD->Q mul instruction
  bool useMulDDQ() const { return UseMulDDQ; }

  /// * disableJmpi - true if jmpi is disabled.
  bool disableJmpi() const { return DisableJmpi; }

  /// * WaNoA32ByteScatteredStatelessMessages - true if there is no A32 byte
  ///   scatter stateless message.
  bool WaNoA32ByteScatteredStatelessMessages() const { return !isCNLplus(); }

  /// * disableVectorDecomposition - true if vector decomposition is disabled.
  bool disableVectorDecomposition() const { return DisableVectorDecomposition; }

  /// * disableJumpTables - true if switch to jump tables lowering is disabled.
  bool disableJumpTables() const { return DisableJumpTables; }

  /// * has switchjmp instruction
  bool hasSwitchjmp() const { return HasSwitchjmp; }

  /// * has integer div/rem instruction
  bool hasIntegerDivision() const { return HasIntegerDivision; }

  /// * warnCallable() - true if compiler only generate warning for
  ///   callable in the middle
  bool warnCallable() const { return WarnCallable; }

  /// * hasIndirectGRFCrossing - true if target supports an indirect region
  ///   crossing one GRF boundary
  bool hasIndirectGRFCrossing() const { return isSKLplus(); }

  /// * getMaxSlmSize - returns maximum allowed SLM size (in KB)
  unsigned getMaxSlmSize() const {
    return 64;
  }

  bool hasThreadPayloadInMemory() const {
    return false;
  }

  /// * hasSad2Support - returns true if sad2/sada2 are supported by target
  bool hasSad2Support() const {
    if (isICLLP() || isTGLLP())
      return false;
    if (isDG1())
      return false;
    return true;
  }

  /// * hneedsArgPatching - some subtarget require special treatment of
  // certain argument types, returns *true* if this is the case.
  bool needsArgPatching() const {
    if (isOCLRuntime())
      return false;
    return false;
  }

  // Generic helper functions...
  const Triple &getTargetTriple() const { return TargetTriple; }

  bool isTargetDarwin() const { return TargetTriple.isOSDarwin(); }
  bool isTargetLinux() const { return TargetTriple.isOSLinux(); }

  bool isTargetWindowsMSVC() const {
    return TargetTriple.isWindowsMSVCEnvironment();
  }

  bool isTargetKnownWindowsMSVC() const {
    return TargetTriple.isKnownWindowsMSVCEnvironment();
  }

  bool isTargetWindowsCygwin() const {
    return TargetTriple.isWindowsCygwinEnvironment();
  }

  bool isTargetWindowsGNU() const {
    return TargetTriple.isWindowsGNUEnvironment();
  }

  bool isTargetCygMing() const { return TargetTriple.isOSCygMing(); }

  bool isOSWindows() const { return TargetTriple.isOSWindows(); }

  TARGET_PLATFORM getVisaPlatform() const {
    switch (GenXVariant) {
    case GENX_BDW:
      return TARGET_PLATFORM::GENX_BDW;
    case GENX_CHV:
      return TARGET_PLATFORM::GENX_CHV;
    case GENX_SKL:
      return TARGET_PLATFORM::GENX_SKL;
    case GENX_BXT:
      return TARGET_PLATFORM::GENX_BXT;
    case GENX_CNL:
      return TARGET_PLATFORM::GENX_CNL;
    case GENX_ICLLP:
      return TARGET_PLATFORM::GENX_ICLLP;
    case GENX_TGLLP:
      return TARGET_PLATFORM::GENX_TGLLP;
    case GENX_DG1:
      return TARGET_PLATFORM::GENX_TGLLP;
    case GENX_KBL:
      return TARGET_PLATFORM::GENX_SKL;
    case GENX_GLK:
      return TARGET_PLATFORM::GENX_BXT;
    default:
      return TARGET_PLATFORM::GENX_NONE;
    }
  }

  /// * stackSurface - return a surface that should be used for stack.
  PreDefined_Surface stackSurface() const { return StackSurf; }
};

} // End llvm namespace

#endif
