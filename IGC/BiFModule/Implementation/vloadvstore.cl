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

//
// This file defines SPIRV vload/vstore built-ins.
//
//===----------------------------------------------------------------------===//

//*****************************************************************************/
// vload/vstore macros
//*****************************************************************************/

#define ELEM_ARG(addressSpace, scalarType, mang)             \
VLOAD_MACRO(addressSpace, scalarType, 2,  long, i64_##mang) \
VLOAD_MACRO(addressSpace, scalarType, 2,  int,  i32_##mang) \
VLOAD_MACRO(addressSpace, scalarType, 3,  long, i64_##mang) \
VLOAD_MACRO(addressSpace, scalarType, 3,  int,  i32_##mang) \
VLOAD_MACRO(addressSpace, scalarType, 4,  long, i64_##mang) \
VLOAD_MACRO(addressSpace, scalarType, 4,  int,  i32_##mang) \
VLOAD_MACRO(addressSpace, scalarType, 8,  long, i64_##mang) \
VLOAD_MACRO(addressSpace, scalarType, 8,  int,  i32_##mang) \
VLOAD_MACRO(addressSpace, scalarType, 16, long, i64_##mang) \
VLOAD_MACRO(addressSpace, scalarType, 16, int,  i32_##mang)

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
#define TYPE_ARG(TYPE, TYPEMANG)       \
ELEM_ARG(global,   TYPE, p1##TYPEMANG) \
ELEM_ARG(constant, TYPE, p2##TYPEMANG) \
ELEM_ARG(local,    TYPE, p3##TYPEMANG) \
ELEM_ARG(private,  TYPE, p0##TYPEMANG) \
ELEM_ARG(generic,  TYPE, p4##TYPEMANG)
#else
#define TYPE_ARG(TYPE, TYPEMANG)       \
ELEM_ARG(global,   TYPE, p1##TYPEMANG) \
ELEM_ARG(constant, TYPE, p2##TYPEMANG) \
ELEM_ARG(local,    TYPE, p3##TYPEMANG) \
ELEM_ARG(private,  TYPE, p0##TYPEMANG)
#endif // __OPENCL_C_VERSION__ >= CL_VERSION_2_0

TYPE_ARG(char,  i8)
TYPE_ARG(short, i16)
TYPE_ARG(int,   i32)
TYPE_ARG(long,  i64)
TYPE_ARG(half,   f16)
TYPE_ARG(float,  f32)

#undef TYPE_ARG
#undef ELEM_ARG

#define ELEM_ARG(addressSpace, scalarType, typemang, mang)                    \
VSTORE_MACRO(addressSpace, scalarType, 2,  ulong, v2##typemang##_i64_##mang)  \
VSTORE_MACRO(addressSpace, scalarType, 2,  uint,  v2##typemang##_i32_##mang)  \
VSTORE_MACRO(addressSpace, scalarType, 3,  ulong, v3##typemang##_i64_##mang)  \
VSTORE_MACRO(addressSpace, scalarType, 3,  uint,  v3##typemang##_i32_##mang)  \
VSTORE_MACRO(addressSpace, scalarType, 4,  ulong, v4##typemang##_i64_##mang)  \
VSTORE_MACRO(addressSpace, scalarType, 4,  uint,  v4##typemang##_i32_##mang)  \
VSTORE_MACRO(addressSpace, scalarType, 8,  ulong, v8##typemang##_i64_##mang)  \
VSTORE_MACRO(addressSpace, scalarType, 8,  uint,  v8##typemang##_i32_##mang)  \
VSTORE_MACRO(addressSpace, scalarType, 16, ulong, v16##typemang##_i64_##mang) \
VSTORE_MACRO(addressSpace, scalarType, 16, uint,  v16##typemang##_i32_##mang)

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
#define TYPE_ARG(TYPE, TYPEMANG)                 \
ELEM_ARG(global,   TYPE, TYPEMANG, p1##TYPEMANG) \
ELEM_ARG(local,    TYPE, TYPEMANG, p3##TYPEMANG) \
ELEM_ARG(private,  TYPE, TYPEMANG, p0##TYPEMANG) \
ELEM_ARG(generic,  TYPE, TYPEMANG, p4##TYPEMANG)
#else
#define TYPE_ARG(TYPE, TYPEMANG)                 \
ELEM_ARG(global,   TYPE, TYPEMANG, p1##TYPEMANG) \
ELEM_ARG(local,    TYPE, TYPEMANG, p3##TYPEMANG) \
ELEM_ARG(private,  TYPE, TYPEMANG, p0##TYPEMANG)
#endif // __OPENCL_C_VERSION__ >= CL_VERSION_2_0

TYPE_ARG(uchar,  i8)
TYPE_ARG(ushort, i16)
TYPE_ARG(uint,   i32)
TYPE_ARG(ulong,  i64)
TYPE_ARG(half,   f16)
TYPE_ARG(float,  f32)

#undef TYPE_ARG
#undef ELEM_ARG

//*****************************************************************************/
// vload macros
//*****************************************************************************/
static OVERLOADABLE float __intel_spirv_half2float(short h)
{
    return SPIRV_BUILTIN(FConvert, _f32_f16, _Rfloat)(as_half(h));
}

#define VLOAD_SHORT(addressSpace, ASNUM)                                                                 \
INLINE static short SPIRV_OVERLOADABLE SPIRV_OCL_BUILTIN(vload, _i64_p##ASNUM##i16, n_Rshort)(long offset, addressSpace short* p) \
{                                                                                                        \
    return *(p + offset);                                                                                \
}                                                                                                        \
INLINE static short SPIRV_OVERLOADABLE SPIRV_OCL_BUILTIN(vload, _i32_p##ASNUM##i16, n_Rshort)(int offset, addressSpace short* p)  \
{                                                                                                        \
    return *(p + offset);                                                                                \
}

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
VLOAD_SHORT(__generic,  4)
#endif // __OPENCL_C_VERSION__ >= CL_VERSION_2_0
VLOAD_SHORT(__global,   1)
VLOAD_SHORT(__local,    3)
VLOAD_SHORT(__constant, 2)
VLOAD_SHORT(__private,  0)

GENERATE_VECTOR_FUNCTIONS_1ARG_NO_MANG(__intel_spirv_half2float, float, short)

// Two copies for the i32 and i64 size_t offsets.
#define __CLFN_DEF_F_VLOAD_SCALAR_HALF(addressSpace, ASNUM)                                      \
INLINE half SPIRV_OVERLOADABLE SPIRV_OCL_BUILTIN(vload, _i32_p##ASNUM##f16, _Rhalf)(int offset, addressSpace half* p) {   \
  return *(p + offset);                                                                          \
}                                                                                                \
INLINE half SPIRV_OVERLOADABLE SPIRV_OCL_BUILTIN(vload, _i64_p##ASNUM##f16, _Rhalf)(long offset, addressSpace half* p) {  \
  return *(p + offset);                                                                          \
}

#define __CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, MANGSIZE, SIZETYPE, numElements, postfix)                                                        \
INLINE float##numElements SPIRV_OVERLOADABLE SPIRV_OCL_BUILTIN(vload_half, numElements##_##MANGSIZE##_p##ASNUM##f16, postfix##_Rfloat##numElements)(SIZETYPE offset, addressSpace half* p) { \
  return __intel_spirv_half2float(SPIRV_OCL_BUILTIN(vload, numElements##_##MANGSIZE##_p##ASNUM##i16, n_Rshort##numElements)(offset, (addressSpace short*)p));          \
}

#define __CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, MANGSIZE, SIZETYPE, step, numElements)                                                 \
INLINE float##numElements SPIRV_OVERLOADABLE SPIRV_OCL_BUILTIN(vloada_half, numElements##_##MANGSIZE##_p##ASNUM##f16, n_Rfloat##numElements)(SIZETYPE offset, addressSpace half* p) {  \
  const addressSpace short##numElements* pHalf = (const addressSpace short##numElements*)(p + offset * step);  \
  return __intel_spirv_half2float(*pHalf);                                                                     \
}

#define __CLFN_DEF_F_VLOAD_HALFX_AS(addressSpace, ASNUM)             \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i64, long, , )         \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i64, long, 2, n)       \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i64, long, 3, n)       \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i64, long, 4, n)       \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i64, long, 8, n)       \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i64, long, 16, n)      \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i32, int, , )          \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i32, int, 2, n)        \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i32, int, 3, n)        \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i32, int, 4, n)        \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i32, int, 8, n)        \
__CLFN_DEF_F_VLOAD_HALFX(addressSpace, ASNUM, i32, int, 16, n)

#define __CLFN_DEF_F_VLOADA_HALFX_AS(addressSpace, ASNUM)          \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i64, long, 1, )     \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i64, long, 2, 2)    \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i64, long, 4, 3)    \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i64, long, 4, 4)    \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i64, long, 8, 8)    \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i64, long, 16, 16)  \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i32, int, 1, )      \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i32, int, 2, 2)     \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i32, int, 4, 3)     \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i32, int, 4, 4)     \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i32, int, 8, 8)     \
__CLFN_DEF_F_VLOADA_HALFX(addressSpace, ASNUM, i32, int, 16, 16)

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
#define __CLFN_DEF_F_VLOAD_HALF_ALL()       \
__CLFN_DEF_F_VLOAD_HALFX_AS(__generic,  4)  \
__CLFN_DEF_F_VLOAD_HALFX_AS(__global,   1)  \
__CLFN_DEF_F_VLOAD_HALFX_AS(__local,    3)  \
__CLFN_DEF_F_VLOAD_HALFX_AS(__constant, 2)  \
__CLFN_DEF_F_VLOAD_HALFX_AS(__private,  0)
#else
#define __CLFN_DEF_F_VLOAD_HALF_ALL()       \
__CLFN_DEF_F_VLOAD_HALFX_AS(__global,   1)  \
__CLFN_DEF_F_VLOAD_HALFX_AS(__local,    3)  \
__CLFN_DEF_F_VLOAD_HALFX_AS(__constant, 2)  \
__CLFN_DEF_F_VLOAD_HALFX_AS(__private,  0)
#endif // __OPENCL_C_VERSION__ >= CL_VERSION_2_0

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
#define __CLFN_DEF_F_VLOADA_HALF_ALL()      \
__CLFN_DEF_F_VLOADA_HALFX_AS(__generic,  4) \
__CLFN_DEF_F_VLOADA_HALFX_AS(__global,   1) \
__CLFN_DEF_F_VLOADA_HALFX_AS(__local,    3) \
__CLFN_DEF_F_VLOADA_HALFX_AS(__constant, 2) \
__CLFN_DEF_F_VLOADA_HALFX_AS(__private,  0)
#else
#define __CLFN_DEF_F_VLOADA_HALF_ALL()      \
__CLFN_DEF_F_VLOADA_HALFX_AS(__global,   1) \
__CLFN_DEF_F_VLOADA_HALFX_AS(__local,    3) \
__CLFN_DEF_F_VLOADA_HALFX_AS(__constant, 2) \
__CLFN_DEF_F_VLOADA_HALFX_AS(__private,  0)
#endif // __OPENCL_C_VERSION__ >= CL_VERSION_2_0

//*****************************************************************************/
// vstore macros (half)
//*****************************************************************************/
// These helper functions are used to macro-ize the rounding mode built-ins.
static OVERLOADABLE half __intel_spirv_float2half_rtz(float f)
{
    return SPIRV_BUILTIN(FConvert, _RTZ_f16_f32, _rtz_Rhalf)(f);
}

static OVERLOADABLE half __intel_spirv_float2half_rte(float f)
{
    return SPIRV_BUILTIN(FConvert, _RTE_f16_f32, _rte_Rhalf)(f);
}

static OVERLOADABLE half __intel_spirv_float2half_rtp(float f)
{
    return SPIRV_BUILTIN(FConvert, _RTP_f16_f32, _rtp_Rhalf)(f);
}

static OVERLOADABLE half __intel_spirv_float2half_rtn(float f)
{
    return SPIRV_BUILTIN(FConvert, _RTN_f16_f32, _rtn_Rhalf)(f);
}

static OVERLOADABLE half __intel_spirv_float2half(float f)
{
    return __intel_spirv_float2half_rte(f);
}

GENERATE_VECTOR_FUNCTIONS_1ARG_NO_MANG(__intel_spirv_float2half_rtz, half, float)
GENERATE_VECTOR_FUNCTIONS_1ARG_NO_MANG(__intel_spirv_float2half_rte, half, float)
GENERATE_VECTOR_FUNCTIONS_1ARG_NO_MANG(__intel_spirv_float2half_rtp, half, float)
GENERATE_VECTOR_FUNCTIONS_1ARG_NO_MANG(__intel_spirv_float2half_rtn, half, float)
GENERATE_VECTOR_FUNCTIONS_1ARG_NO_MANG(__intel_spirv_float2half,     half, float)

// Two copies for the i32 and i64 size_t offsets.
#define __CLFN_DEF_VSTORE_SCALAR_HALF(addressSpace, ASNUM)       \
INLINE void __builtin_spirv_OpenCL_vstore_f16_i32_p##ASNUM##f16(        \
    half data,                                                   \
    uint offset,                                                 \
    addressSpace half* p) {                                      \
  addressSpace half *pHalf = (addressSpace half *)(p + offset);  \
  *pHalf = data;                                                 \
}                                                                \
INLINE void __builtin_spirv_OpenCL_vstore_f16_i64_p##ASNUM##f16(        \
    half data,                                                   \
    ulong offset,                                                \
    addressSpace half* p) {                                      \
  addressSpace half *pHalf = (addressSpace half *)(p + offset);  \
  *pHalf = data;                                                 \
}

#define __CLFN_DEF_VSTORE_HALF(addressSpace, ASNUM, rnd)                                  \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i64, ulong, ,    rnd, f32, float, )        \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i64, ulong, v,   rnd, f32, float, 2)       \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i64, ulong, v,   rnd, f32, float, 3)       \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i64, ulong, v,   rnd, f32, float, 4)       \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i64, ulong, v,   rnd, f32, float, 8)       \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i64, ulong, v,   rnd, f32, float, 16)      \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i32, uint,  ,    rnd, f32, float, )       \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i32, uint,  v,   rnd, f32, float, 2)      \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i32, uint,  v,   rnd, f32, float, 3)      \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i32, uint,  v,   rnd, f32, float, 4)      \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i32, uint,  v,   rnd, f32, float, 8)      \
  __CLFN_DEF_VSTORE_HALFX(addressSpace, ASNUM, i32, uint,  v,   rnd, f32, float, 16)

#define __CLFN_DEF_VSTOREA_HALF(addressSpace, ASNUM, rnd)                                     \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i64, ulong, ,    rnd, f32, float, 1, )        \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i64, ulong, v,   rnd, f32, float, 2, 2)       \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i64, ulong, v,   rnd, f32, float, 4, 3)       \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i64, ulong, v,   rnd, f32, float, 4, 4)       \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i64, ulong, v,   rnd, f32, float, 8, 8)       \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i64, ulong, v,   rnd, f32, float, 16, 16)     \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i32, uint,  ,    rnd, f32, float, 1, )       \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i32, uint,  v,   rnd, f32, float, 2, 2)      \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i32, uint,  v,   rnd, f32, float, 4, 3)      \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i32, uint,  v,   rnd, f32, float, 4, 4)      \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i32, uint,  v,   rnd, f32, float, 8, 8)      \
  __CLFN_DEF_VSTOREA_HALFX(addressSpace, ASNUM, i32, uint,  v,   rnd, f32, float, 16, 16)

//*****************************************************************************/
// vload/vstore HALF functions
//*****************************************************************************/
#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
__CLFN_DEF_F_VLOAD_SCALAR_HALF(__generic,  4)
#endif // __OPENCL_C_VERSION__ >= CL_VERSION_2_0
__CLFN_DEF_F_VLOAD_SCALAR_HALF(__global,   1)
__CLFN_DEF_F_VLOAD_SCALAR_HALF(__local,    3)
__CLFN_DEF_F_VLOAD_SCALAR_HALF(__constant, 2)
__CLFN_DEF_F_VLOAD_SCALAR_HALF(__private,  0)

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
__CLFN_DEF_VSTORE_SCALAR_HALF(__generic, 4)
#endif // __OPENCL_C_VERSION__ >= CL_VERSION_2_0
__CLFN_DEF_VSTORE_SCALAR_HALF(__global,  1)
__CLFN_DEF_VSTORE_SCALAR_HALF(__local,   3)
__CLFN_DEF_VSTORE_SCALAR_HALF(__private, 0)

__CLFN_DEF_VSTORE_HALF_ALL()
__CLFN_DEF_VSTOREA_HALF_ALL()

__CLFN_DEF_F_VLOAD_HALF_ALL()
__CLFN_DEF_F_VLOADA_HALF_ALL()
