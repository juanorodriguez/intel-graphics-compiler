/*========================== begin_copyright_notice ============================

Copyright (c) 2016-2021 Intel Corporation

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

//*****************************************************************************/
// Functions to convert enum of memory_fence and cl_mem_fence_flags to
// to corresponding SPIRV equivalents
//*****************************************************************************/

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
static Scope_t get_spirv_mem_scope(memory_scope scope)
{
    switch (scope)
    {
    case memory_scope_work_item:
        return Invocation;
    case memory_scope_sub_group:
        return Subgroup;
    case memory_scope_work_group:
        return Workgroup;
    case memory_scope_device:
        return Device;
    case memory_scope_all_svm_devices:
        return CrossDevice;
    default:
        return CrossDevice;
    }
}
#endif // __OPENCL_C_VERSION__ >= CL_VERSION_2_0

static uint get_spirv_mem_fence(cl_mem_fence_flags flag)
{
    uint result = 0;

    if (flag & CLK_GLOBAL_MEM_FENCE)
    {
        result |= CrossWorkgroupMemory;
    }

    if (flag & CLK_LOCAL_MEM_FENCE)
    {
        result |= WorkgroupMemory;
    }

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
    if (flag & CLK_IMAGE_MEM_FENCE)
    {
        result |= ImageMemory;
    }
#endif // __OPENCL_C_VERSION__ >= CL_VERSION_2_0

    return result;
}

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
static uint get_spirv_mem_order(memory_order order)
{
    switch (order)
    {
    case memory_order_relaxed:
        return Relaxed;
    case memory_order_acquire:
        return Acquire;
    case memory_order_release:
        return Release;
    case memory_order_acq_rel:
        return AcquireRelease;
    case memory_order_seq_cst:
        return SequentiallyConsistent;
    default:
        return 0;
    }
}
#endif // __OPENCL_C_VERSION__ >= CL_VERSION_2_0
