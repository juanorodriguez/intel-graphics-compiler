#=========================== begin_copyright_notice ============================
#
# Copyright (c) 2020-2021 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom
# the Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
#
#============================ end_copyright_notice =============================

include_guard(DIRECTORY)

# Convenience function to get list of LLVM components for
# target_link_library. If LLVM was configured with llvm dylib, then
# included in dylib llvm targets should be replaced with LLVM
# lib. Otherwise, just return passed libraries.
# ret -- name of variable with returned targets list. All other
# arguments are targets to process.
function(igc_get_llvm_targets RET)
  set(TARGETS ${ARGN})

  if (LLVM_LINK_LLVM_DYLIB)
    # Drop all components, though it is probably not right
    # and llvm_map_components_to_libnames should be used as filter.
    # However, in external build it returns empty list for "all"
    # so filtering is not really done.
    if ("${LLVM_DYLIB_COMPONENTS}" STREQUAL "all")
      set(TARGETS "")
    else()
      list(REMOVE_ITEM TARGETS ${LLVM_DYLIB_COMPONENTS})
    endif()
  endif()

  # Expand rest of components names to target names.
  llvm_map_components_to_libnames(TARGETS ${TARGETS})

  if (LLVM_LINK_LLVM_DYLIB)
    set(TARGETS ${TARGETS} LLVM)
  endif()

  set(${RET} ${TARGETS} PARENT_SCOPE)
endfunction()

# Simple wrapper around configure_lit_site_cfg that allows
# generator expressions in configured lit config.
function(igc_configure_lit_site_cfg in out)
  set(temp_cfg ${CMAKE_CURRENT_BINARY_DIR}/temp.cfg.py)

  # Generate temporary site config with LLVM variables filled.
  configure_lit_site_cfg(
    ${in}
    ${temp_cfg}
    ${ARGN}
    )

  # Need to regenerate again to use generator expressions that are not
  # expanded in configure_file.
  file(GENERATE
    OUTPUT ${out}
    INPUT ${temp_cfg}
    )
endfunction()

# Helper macro to set LLVM_EXTERNAL_LIT variable for LLVM lit tests.
# Variable can be overridden from command line to set custom lit tool.
macro(igc_find_external_lit)
  list(APPEND paths_to_lit ${LLVM_SOURCE_DIR}/utils/lit)
  find_file(LLVM_EXTERNAL_LIT
    NAMES llvm-lit llvm-lit.py lit.py lit
    PATHS ${paths_to_lit}
    DOC "Path to lit utility"
    )

  if(NOT LLVM_EXTERNAL_LIT)
    message(FATAL_ERROR "llvm-lit is not found, please specify it with LLVM_EXTERNAL_LIT variable")
  endif()
endmacro()
