#=========================== begin_copyright_notice ============================
#
# Copyright (c) 2021-2021 Intel Corporation
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

# SPIRV translator was added with LLVM (source build or prebuild).
if(TARGET LLVMSPIRVLib)
  message(STATUS "[IGC] Using LLVMSPIRVLib that comes with LLVM")

  # Guess location of header files.
  get_target_property(_is_imported LLVMSPIRVLib IMPORTED)
  if(_is_imported)
    # Imported location property for LLVM can have one of these suffixes.
    set(_prop_types
      ""
      "_RELEASE"
      "_DEBUG"
      "_RELWITHDEBINFO"
      "_MINSIZEREL"
      )
    foreach(t IN LISTS _prop_types)
      get_target_property(_lib_loc LLVMSPIRVLib IMPORTED_LOCATION${t})
      if(_lib_loc)
        break()
      endif()
    endforeach()
    # Installed spirv package has the following directory layout:
    # |-lib/LLVMSPIRVLib.a
    # `-include/LLVMSPIRVLib/<headers>
    # So get include directories based on location of imported library.
    get_filename_component(_inc_dir ${_lib_loc} DIRECTORY)
    get_filename_component(_inc_dir ${_inc_dir}/../include/LLVMSPIRVLib ABSOLUTE)
    unset(_lib_loc)
  else()
    # SPIRV sources has the following directory layout:
    # |-lib/SPIRV/CMakeLists.txt with LLVMSPIRVLib target
    # `-include/<headers>
    # Similarly to the imported target get required include dirs.
    get_target_property(_src_dir LLVMSPIRVLib SOURCE_DIR)
    get_filename_component(_inc_dir ${_src_dir}/../../include ABSOLUTE)
    unset(_srcdir)
  endif()
  unset(_is_imported)

  # Add headers. Since target can be imported, use property.
  # Additionally, for in-tree build, do not set install interface
  # since it can be polluted with build directory artifact.
  set_target_properties(LLVMSPIRVLib PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES $<BUILD_INTERFACE:${_inc_dir}>
    )
  unset(_inc_dir)
else()
  message(STATUS "[IGC] Trying to find prebuilt SPIRV library")
  find_package(SPIRVLLVMTranslator ${LLVM_VERSION_MAJOR} REQUIRED)
endif()

# Set additional compile definition for library. Use property instead of
# target_compile_definitions to unify imported and simple library usage
# because command will fail on imported library (this is cmake deficiency
# that was fixed in later versions).
set_target_properties(LLVMSPIRVLib PROPERTIES
  INTERFACE_COMPILE_DEFINITIONS IGC_USE_KHRONOS_SPIRV_TRANSLATOR
  )
