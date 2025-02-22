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


#define MAX_DIM 2
#define BuiltinVector(BuiltinName) \
    (size_t3)(BuiltinName(0),      \
              BuiltinName(1),      \
              BuiltinName(2))

#ifdef NO_ASSUME_SUPPORT
    #define BuiltinAssumeGE0(s)
    #define BuiltinVectorAssumeGE0(v)
#else
    #define BuiltinAssumeGE0(s) __builtin_assume( s >= 0 )
    #define BuiltinVectorAssumeGE0(v) \
        __builtin_assume( (v.x) >= 0 ); \
        __builtin_assume( (v.y) >= 0 ); \
        __builtin_assume( (v.z) >= 0 );
#endif

// Helper functions prefixed with '__intel'

uint __intel_WorkgroupSize()
{
    uint totalWorkGroupSize =
        __builtin_IB_get_local_size(0) *
        __builtin_IB_get_local_size(1) *
        __builtin_IB_get_local_size(2);

    BuiltinAssumeGE0(totalWorkGroupSize);
    return totalWorkGroupSize;
}

size_t __intel_EnqueuedWorkgroupSize()
{
    size_t totalWorkGroupSize =
        __builtin_IB_get_enqueued_local_size(0) *
        __builtin_IB_get_enqueued_local_size(1) *
        __builtin_IB_get_enqueued_local_size(2);

    BuiltinAssumeGE0(totalWorkGroupSize);
    return totalWorkGroupSize;
}

size_t3 OVERLOADABLE __intel_WorkgroupId()
{
    size_t3 v = BuiltinVector(__builtin_IB_get_group_id);
    BuiltinVectorAssumeGE0(v);
    return v;
}

uint OVERLOADABLE __intel_LocalInvocationId(uint dim)
{
    uint v = 0;
    if (dim == 0) {
        v = __builtin_IB_get_local_id_x();
    }
    else if (dim == 1) {
        v = __builtin_IB_get_local_id_y();
    }
    else if (dim == 2) {
        v = __builtin_IB_get_local_id_z();
    }

    // local id is a 16 bit number in curbe.
#ifndef NO_ASSUME_SUPPORT
    __builtin_assume(v >= 0);
    __builtin_assume(v <= 0xffff);
#endif
    return v;
}

size_t3 OVERLOADABLE __intel_LocalInvocationId()
{
    size_t3 v = (size_t3)(__builtin_IB_get_local_id_x(),
                     __builtin_IB_get_local_id_y(),
                     __builtin_IB_get_local_id_z());

    BuiltinVectorAssumeGE0(v);
#ifndef NO_ASSUME_SUPPORT
    __builtin_assume(v.x <= 0xffff);
    __builtin_assume(v.y <= 0xffff);
    __builtin_assume(v.z <= 0xffff);
#endif

    return v;
}

size_t OVERLOADABLE __intel_GlobalInvocationId(uint dim)
{
    if (dim > MAX_DIM)
        return 0;

    size_t v =
        __builtin_IB_get_group_id(dim) * __builtin_IB_get_enqueued_local_size(dim) +
        __intel_LocalInvocationId(dim) + __builtin_IB_get_global_offset(dim);

    BuiltinAssumeGE0(v);
    return v;
}

size_t3 OVERLOADABLE __intel_GlobalInvocationId()
{
    return BuiltinVector(__intel_GlobalInvocationId);
}

uint __intel_LocalInvocationIndex()
{
#if 0
    // This doesn't work right now due to a bug in the runtime.
    // If/when they fix their bug we can experiment if spending the
    // register(s) for get_local_linear_id() is better than spending
    // the math to compute the linear local ID.
    return __builtin_IB_get_local_linear_id();
#else
    uint llid;
    llid = (uint)__intel_LocalInvocationId(2);
    llid *= (uint)__builtin_IB_get_local_size(1);
    llid += (uint)__intel_LocalInvocationId(1);
    llid *= (uint)__builtin_IB_get_local_size(0);
    llid += (uint)__intel_LocalInvocationId(0);

    BuiltinAssumeGE0(llid);
    return llid;
#endif
}

////////////////////////

#if defined(OLD_SPIRV_BUILTINS)

size_t3 __builtin_spirv_BuiltInNumWorkgroups()
{
    size_t3 v = BuiltinVector(__builtin_IB_get_num_groups);
    BuiltinVectorAssumeGE0(v);
    return v;
}

size_t3 __builtin_spirv_BuiltInWorkgroupSize()
{
   size_t3 v = BuiltinVector(__builtin_IB_get_local_size);
   BuiltinVectorAssumeGE0(v);
   return v;
}

size_t3 __builtin_spirv_BuiltInWorkgroupId()
{
    return __intel_WorkgroupId();
}

size_t3 __builtin_spirv_BuiltInLocalInvocationId()
{
    return __intel_LocalInvocationId();
}

size_t3 __builtin_spirv_BuiltInGlobalInvocationId()
{
    return __intel_GlobalInvocationId();
}

size_t3 __builtin_spirv_BuiltInGlobalSize()
{
    size_t3 v = BuiltinVector(__builtin_IB_get_global_size);
    BuiltinVectorAssumeGE0(v);
    return v;
}

size_t3 __builtin_spirv_BuiltInEnqueuedWorkgroupSize()
{
    size_t3 v = BuiltinVector(__builtin_IB_get_enqueued_local_size);
    BuiltinVectorAssumeGE0(v);
    return v;
}

size_t3 __builtin_spirv_BuiltInGlobalOffset()
{
    return BuiltinVector(__builtin_IB_get_global_offset);
}

#else // not defined(OLD_SPIRV_BUILTINS)

size_t OVERLOADABLE __spirv_BuiltInNumWorkgroups(int dimindx)
{
    size_t v = __builtin_IB_get_num_groups(dimindx);
    BuiltinAssumeGE0(v);
    return v;
}

size_t OVERLOADABLE __spirv_BuiltInWorkgroupSize(int dimindx)
{
    size_t v = __builtin_IB_get_local_size(dimindx);
    BuiltinAssumeGE0(v);
    return v;
}

size_t OVERLOADABLE __spirv_BuiltInWorkgroupId(int dimindx)
{
    size_t v = __builtin_IB_get_group_id(dimindx);
    BuiltinAssumeGE0(v);
    return v;
}

size_t OVERLOADABLE __spirv_BuiltInLocalInvocationId(int dimindx)
{
    return __intel_LocalInvocationId(dimindx);
}

size_t OVERLOADABLE __spirv_BuiltInGlobalInvocationId(int dimindx)
{
    return __intel_GlobalInvocationId(dimindx);
}

size_t OVERLOADABLE __spirv_BuiltInGlobalSize(int dimindx)
{
    size_t v = __builtin_IB_get_global_size(dimindx);
    BuiltinAssumeGE0(v);
    return v;
}

size_t OVERLOADABLE __spirv_BuiltInEnqueuedWorkgroupSize(int dimindx)
{
    size_t v = __builtin_IB_get_enqueued_local_size(dimindx);
    BuiltinAssumeGE0(v);
    return v;
}

size_t OVERLOADABLE __spirv_BuiltInGlobalOffset(int dimindx)
{
    return __builtin_IB_get_global_offset(dimindx);
}

#endif // defined(OLD_SPIRV_BUILTINS)

size_t SPIRV_OVERLOADABLE SPIRV_BUILTIN_NO_OP(BuiltInGlobalLinearId, , )()
{
  uint dim = SPIRV_BUILTIN_NO_OP(BuiltInWorkDim, , )();
  size_t result = 0;

  switch (dim) {
    default:
    case 1:
      result = __intel_GlobalInvocationId(0) - __builtin_IB_get_global_offset(0);
      break;
    case 2:
      result = (__intel_GlobalInvocationId(1) - __builtin_IB_get_global_offset(1))*
                __builtin_IB_get_global_size(0) + (__intel_GlobalInvocationId(0) - __builtin_IB_get_global_offset(0));
      break;
    case 3:
      result = ((__intel_GlobalInvocationId(2) - __builtin_IB_get_global_offset(2)) *
                __builtin_IB_get_global_size(1) * __builtin_IB_get_global_size(0)) +
               ((__intel_GlobalInvocationId(1) - __builtin_IB_get_global_offset(1)) * __builtin_IB_get_global_size(0)) +
               (__intel_GlobalInvocationId(0) - __builtin_IB_get_global_offset(0));
      break;
  }

  BuiltinAssumeGE0(result);
  return result;
}

size_t SPIRV_OVERLOADABLE SPIRV_BUILTIN_NO_OP(BuiltInLocalInvocationIndex, , )()
{
    return __intel_LocalInvocationIndex();
}

uint SPIRV_OVERLOADABLE SPIRV_BUILTIN_NO_OP(BuiltInWorkDim, , )()
{
    uint dim = __builtin_IB_get_work_dim();

#ifndef NO_ASSUME_SUPPORT
    __builtin_assume(dim >= 0);
    __builtin_assume(dim <= 3);
#endif

    return dim;
}

uint SPIRV_OVERLOADABLE SPIRV_BUILTIN_NO_OP(BuiltInSubgroupMaxSize, , )()
{
    uint v = __builtin_IB_get_simd_size();

#ifndef NO_ASSUME_SUPPORT
    __builtin_assume(v >= 8);
    __builtin_assume(v <= 32);
#endif

    return v;
}

uint SPIRV_OVERLOADABLE SPIRV_BUILTIN_NO_OP(BuiltInSubgroupId, , )()
{
    uint v = (uint)SPIRV_BUILTIN_NO_OP(BuiltInLocalInvocationIndex, , )() / SPIRV_BUILTIN_NO_OP(BuiltInSubgroupMaxSize, , )();

#ifndef NO_ASSUME_SUPPORT
    __builtin_assume(v >= 0);
    __builtin_assume(v < 32);
#endif

    return v;
}

uint SPIRV_OVERLOADABLE SPIRV_BUILTIN_NO_OP(BuiltInNumSubgroups, , )()
{
    uint totalWorkGroupSize = __intel_WorkgroupSize() + SPIRV_BUILTIN_NO_OP(BuiltInSubgroupMaxSize, , )() - 1;
    return totalWorkGroupSize / SPIRV_BUILTIN_NO_OP(BuiltInSubgroupMaxSize, , )();
}

uint SPIRV_OVERLOADABLE SPIRV_BUILTIN_NO_OP(BuiltInSubgroupSize, , )()
{
    uint    remainder =
                __intel_WorkgroupSize() & ( SPIRV_BUILTIN_NO_OP(BuiltInSubgroupMaxSize, , )() - 1 );
    bool    fullSubGroup =
                ( remainder == 0 ) ||
                ( SPIRV_BUILTIN_NO_OP(BuiltInSubgroupId, , )() < SPIRV_BUILTIN_NO_OP(BuiltInNumSubgroups, , )() - 1 );

    return fullSubGroup ? SPIRV_BUILTIN_NO_OP(BuiltInSubgroupMaxSize, , )() : remainder;
}

uint SPIRV_OVERLOADABLE SPIRV_BUILTIN_NO_OP(BuiltInNumEnqueuedSubgroups, , )()
{
    uint totalEnqueuedWorkGroupSize = __intel_EnqueuedWorkgroupSize() + SPIRV_BUILTIN_NO_OP(BuiltInSubgroupMaxSize, , )() - 1;
    return totalEnqueuedWorkGroupSize / SPIRV_BUILTIN_NO_OP(BuiltInSubgroupMaxSize, , )();
}

uint SPIRV_OVERLOADABLE SPIRV_BUILTIN_NO_OP(BuiltInSubgroupLocalInvocationId, , )()
{
    return __builtin_IB_get_simd_id();
}
