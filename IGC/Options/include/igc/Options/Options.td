/*========================== begin_copyright_notice ============================

Copyright (c) 2020-2021 Intel Corporation

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

// This file defines the options accepted by vector compiler.
//
// There are two kinds of options: api options and internal options.
//
// Api options are exposed to user via, e.g., openCL clBuildProgram.
//
// Internal options are for passing of additional info of various purposes.
// Among these can be: debug, things that are not exposed to user directly.
//
//===----------------------------------------------------------------------===//

include "llvm/Option/OptParser.td"

// Option kinds {{
// Options accessible using API.
def ApiOption : OptionFlag;

// Api options compatible with igcmc.
// These are used only when -cmc is present in api options.
def IgcmcApiOption : OptionFlag;

// Internal options.
def InternalOption : OptionFlag;
// }} Option kinds

// Api options {{
let Flags = [ApiOption] in {

// Main dispatch option.
def vc_codegen : Flag<["-"], "vc-codegen">,
  HelpText<"Enable vector codegenerator">;

// Generate debug info
def vc_emit_debug : Flag<["-"], "g">,
  HelpText<"Enable generation of debug information and enables kernel debug">;
def ze_kernel_debug_enable : Flag<["-"], "ze-kernel-debug-enable">,
  Alias<vc_emit_debug>, HelpText<"Alias for -g">;
// these are coming from NEO when run under debugger
def cl_opt_disable : Flag<["-"], "cl-opt-disable">,
  Alias<vc_emit_debug>, HelpText<"Alias for -g">;
def ze_opt_disable : Flag<["-"], "ze-opt-disable">,
  Alias<vc_emit_debug>, HelpText<"Alias for -g">;

def optimize : Separate<["-"], "optimize">,
  HelpText<"Set optimization level to either 'none' or 'full'">,
  MetaVarName<"<level>">;
def optimize_eq : Joined<["-"], "optimize=">,
  Alias<optimize>, HelpText<"Alias for -optimize">;
def no_optimize : Flag<["-"], "no-optimize">,
  Alias<optimize>, AliasArgs<["none"]>, HelpText<"Alias for -optimize=none">;

// This option switches off vector splitting in parts to better fit into GRF
// It may have both positive and negative consequences for performance
// so shall be used with extreme caution
def no_vector_decomposition : Flag<["-"], "ze-no-vector-decomposition">,
  HelpText<"Disable vector decomposition pass">;
// Compatibility aliases
def no_vector_decomposition_old : Flag<["-"], "no_vector_decomposition">,
  Alias<no_vector_decomposition>, HelpText<"Alias for -ze-no-vector-decomposition"> {
  let Flags = [ApiOption, IgcmcApiOption];
}
def no_vector_decomposition_old_2 : Flag<["-"], "no-vector-decomposition">,
  Alias<no_vector_decomposition>, HelpText<"Alias for -ze-no-vector-decomposition"> {
  let Flags = [ApiOption, IgcmcApiOption];
}

// this option disables jump table generation
// TODO: we shall add this to L0 extension spec
def fno_jump_tables : Flag<["-"], "fno-jump-tables">,
  HelpText<"Disable the use of jump tables for lowering switches">;

// this option enables legacy dataport legalization
def ftranslate_legacy_memory_intrinsics : Flag<["-"], "ftranslate-legacy-memory-intrinsics">,
  HelpText<"Legalize old memory intrinsics">;

def Xfinalizer : Separate<["-"], "Xfinalizer">,
  HelpText<"Pass <arg> to the visa finalizer">, MetaVarName<"<arg>">;

def gtpin_rera : Flag<["-"], "ze-gtpin-rera">,
  HelpText<"Enables vISA re_ra interface for gtpin structure generation">;

def gtpin_grf_info : Flag<["-"], "ze-gtpin-grf-info">,
  HelpText<"Enable vISA grf-info interface for gtpin structure generation">;

def gtpin_scratch_area_size : Separate<["-"], "ze-gtpin-scratch-area-size">,
  HelpText<"Set gtpin scratch area size">;

def gtpin_scratch_area_size_eq : Joined<["-"], "ze-gtpin-scratch-area-size=">,
  Alias<gtpin_scratch_area_size>, HelpText<"Alias for -ze-gtpin-scratch-area-size">;
}
// }} Api options

// Igcmc compatibility {{
let Flags = [IgcmcApiOption] in {

def igcmc : Flag<["-"], "cmc">,
  HelpText<"Enable igcmc compatible mode; incompatible with -vc-codegen; implies -optimize=none.">;

def igcmc_visaopts : Joined<["-"], "visaopts=">,
  HelpText<"Options for finalizer in form \"opt1 opt2 opt3...\"">;

def igcmc_stack_size : Joined<["-"], "stack-mem-size=">,
  HelpText<"Control stack memory size (in bytes)">;

}
// }} Igcmc compatibility

// Internal options {{
let Flags = [InternalOption] in {

def dump_llvm_ir : Flag<["-"], "dump-llvm-ir">,
  HelpText<"Dump llvm IR after SPIRV reading, optimizations and codegen">;
def dump_isa_binary : Flag<["-"], "dump-isa-binary">,
  HelpText<"Dump isa binary after finalization pass">;
def dump_asm : Flag<["-"], "dump-asm">,
  HelpText<"Dump assembly (visaasm, asm, etc.)">;
def mdump_asm : Flag<["-"], "mdump_asm">,
  Alias<dump_asm>, HelpText<"Alias for -dump-asm">;

def help : Flag<["-"], "help">,
  HelpText<"Display available API options">;
def help_internal : Flag<["-"], "help-internal">,
  HelpText<"Display available internal options">;

def llvm_options : Separate<["-"], "llvm-options">,
  HelpText<"Additional options forwarded to llvm CommandLine global option parser">;
def llvm_options_eq : Joined<["-"], "llvm-options=">,
  Alias<llvm_options>, HelpText<"Alias for -llvm-options">;

def target_features : Separate<["-"], "target-features">,
  HelpText<"Auxilirary target features">;
def target_features_eq : Joined<["-"], "target-features=">,
  Alias<target_features>, HelpText<"Alias for -target_features">;

def binary_format : Separate<["-"], "binary-format">,
  HelpText<"Set in which format should be generated binary; values: 'cm', 'ocl' or 'ze'">;
def binary_format_eq : Joined<["-"], "binary-format=">,
  Alias<binary_format>, HelpText<"Alias for -binary-format <value>">;

def runtime : Separate<["-"], "runtime">, Alias<binary_format>,
  HelpText<"Set runtime for which binary should be generated; values: 'ocl' or 'cm'">;
def runtime_eq : Joined<["-"], "runtime=">,
  Alias<binary_format>, HelpText<"Alias for -runtime <value>">;

def ftime_report : Flag<["-"], "ftime-report">,
  HelpText<"Print timing summary of each stage of compilation">;

}
// }} Internal options
