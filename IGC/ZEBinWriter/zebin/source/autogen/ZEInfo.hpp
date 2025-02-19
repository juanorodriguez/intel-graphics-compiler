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

//===- ZEInfo.hpp -----------------------------------------------*- C++ -*-===//
// ZE Binary Utilitis
//
// \file
// This file declares the struct representation of .ze.info section
//===----------------------------------------------------------------------===//

// ******************** DO NOT MODIFY DIRECTLY *********************************
// This file is auto-generated by ZEAutoTool/fileparser.py

#ifndef ZE_INFO_HPP
#define ZE_INFO_HPP

#include <string>
#include <vector>

namespace zebin {

typedef int64_t     zeinfo_int64_t;
typedef int32_t     zeinfo_int32_t;
typedef bool        zeinfo_bool_t;
typedef std::string zeinfo_str_t;
struct zeInfoExecutionEnv
{
    zeinfo_int32_t actual_kernel_start_offset = 0;
    zeinfo_int32_t barrier_count = 0;
    zeinfo_bool_t disable_mid_thread_preemption = false;
    zeinfo_int32_t grf_count = 0;
    zeinfo_bool_t has_4gb_buffers = false;
    zeinfo_bool_t has_device_enqueue = false;
    zeinfo_bool_t has_fence_for_image_access = false;
    zeinfo_bool_t has_global_atomics = false;
    zeinfo_bool_t has_multi_scratch_spaces = false;
    zeinfo_bool_t has_no_stateless_write = false;
    zeinfo_int32_t offset_to_skip_per_thread_data_load = 0;
    zeinfo_int32_t offset_to_skip_set_ffid_gp = 0;
    zeinfo_int32_t required_sub_group_size = 0;
    std::vector<zeinfo_int32_t> required_work_group_size;
    zeinfo_int32_t simd_size = 0;
    zeinfo_int32_t slm_size = 0;
    zeinfo_bool_t subgroup_independent_forward_progress = false;
    std::vector<zeinfo_int32_t> work_group_walk_order_dimensions;
};
struct zeInfoPayloadArgument
{
    zeinfo_str_t arg_type;
    zeinfo_int32_t offset = 0;
    zeinfo_int32_t size = 0;
    zeinfo_int32_t arg_index = -1;
    zeinfo_str_t addrmode;
    zeinfo_str_t addrspace;
    zeinfo_str_t access_type;
};
struct zeInfoPerThreadPayloadArgument
{
    zeinfo_str_t arg_type;
    zeinfo_int32_t offset = 0;
    zeinfo_int32_t size = 0;
};
struct zeInfoBindingTableIndex
{
    zeinfo_int32_t bti_value = 0;
    zeinfo_int32_t arg_index = 0;
};
struct zeInfoPerThreadMemoryBuffer
{
    zeinfo_str_t type;
    zeinfo_str_t usage;
    zeinfo_int32_t size = 0;
    zeinfo_int32_t slot = 0;
    zeinfo_bool_t is_simt_thread = false;
};
struct zeInfoExperimentalProperties
{
    zeinfo_int32_t has_non_kernel_arg_load = -1;
    zeinfo_int32_t has_non_kernel_arg_store = -1;
    zeinfo_int32_t has_non_kernel_arg_atomic = -1;
};
typedef std::vector<zeInfoPayloadArgument> PayloadArgumentsTy;
typedef std::vector<zeInfoPerThreadPayloadArgument> PerThreadPayloadArgumentsTy;
typedef std::vector<zeInfoBindingTableIndex> BindingTableIndicesTy;
typedef std::vector<zeInfoPerThreadMemoryBuffer> PerThreadMemoryBuffersTy;
typedef std::vector<zeInfoExperimentalProperties> ExperimentalPropertiesTy;
struct zeInfoKernel
{
    zeinfo_str_t name;
    zeInfoExecutionEnv execution_env;
    PayloadArgumentsTy payload_arguments;
    PerThreadPayloadArgumentsTy per_thread_payload_arguments;
    BindingTableIndicesTy binding_table_indices;
    PerThreadMemoryBuffersTy per_thread_memory_buffers;
    ExperimentalPropertiesTy experimental_properties;
};
typedef std::vector<zeInfoKernel> KernelsTy;
struct zeInfoContainer
{
    zeinfo_str_t version;
    KernelsTy kernels;
};
struct PreDefinedAttrGetter{
    static zeinfo_str_t getVersionNumber() { return "1.3"; }

    enum class ArgType {
        packed_local_ids,
        local_id,
        local_size,
        group_count,
        global_size,
        enqueued_local_size,
        global_id_offset,
        private_base_stateless,
        buffer_offset,
        printf_buffer,
        arg_byvalue,
        arg_bypointer
    };
    enum class ArgAddrMode {
        stateless,
        stateful,
        bindless,
        slm
    };
    enum class ArgAddrSpace {
        global,
        local,
        constant,
        image,
        sampler
    };
    enum class ArgAccessType {
        readonly,
        writeonly,
        readwrite
    };
    enum class MemBufferType {
        global,
        scratch,
        slm
    };
    enum class MemBufferUsage {
        private_space,
        spill_fill_space,
        single_space
    };
    static zeinfo_str_t get(ArgType val) {
        switch(val) {
        case ArgType::packed_local_ids:
            return "packed_local_ids";
        case ArgType::local_id:
            return "local_id";
        case ArgType::local_size:
            return "local_size";
        case ArgType::group_count:
            return "group_count";
        case ArgType::global_size:
            return "global_size";
        case ArgType::enqueued_local_size:
            return "enqueued_local_size";
        case ArgType::global_id_offset:
            return "global_id_offset";
        case ArgType::private_base_stateless:
            return "private_base_stateless";
        case ArgType::buffer_offset:
            return "buffer_offset";
        case ArgType::printf_buffer:
            return "printf_buffer";
        case ArgType::arg_byvalue:
            return "arg_byvalue";
        case ArgType::arg_bypointer:
            return "arg_bypointer";
        default:
            break;
        }
        return "";
    }
    static zeinfo_str_t get(ArgAddrMode val) {
        switch(val) {
        case ArgAddrMode::stateless:
            return "stateless";
        case ArgAddrMode::stateful:
            return "stateful";
        case ArgAddrMode::bindless:
            return "bindless";
        case ArgAddrMode::slm:
            return "slm";
        default:
            break;
        }
        return "";
    }
    static zeinfo_str_t get(ArgAddrSpace val) {
        switch(val) {
        case ArgAddrSpace::global:
            return "global";
        case ArgAddrSpace::local:
            return "local";
        case ArgAddrSpace::constant:
            return "constant";
        case ArgAddrSpace::image:
            return "image";
        case ArgAddrSpace::sampler:
            return "sampler";
        default:
            break;
        }
        return "";
    }
    static zeinfo_str_t get(ArgAccessType val) {
        switch(val) {
        case ArgAccessType::readonly:
            return "readonly";
        case ArgAccessType::writeonly:
            return "writeonly";
        case ArgAccessType::readwrite:
            return "readwrite";
        default:
            break;
        }
        return "";
    }
    static zeinfo_str_t get(MemBufferType val) {
        switch(val) {
        case MemBufferType::global:
            return "global";
        case MemBufferType::scratch:
            return "scratch";
        case MemBufferType::slm:
            return "slm";
        default:
            break;
        }
        return "";
    }
    static zeinfo_str_t get(MemBufferUsage val) {
        switch(val) {
        case MemBufferUsage::private_space:
            return "private_space";
        case MemBufferUsage::spill_fill_space:
            return "spill_fill_space";
        case MemBufferUsage::single_space:
            return "single_space";
        default:
            break;
        }
        return "";
    }
};
}
#endif
