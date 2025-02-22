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

#include "../include/BiF_Definitions.cl"
#include "spirv.h"


INLINE
char OVERLOADABLE hadd( char x,
                        char y )
{
    return __builtin_spirv_OpenCL_s_hadd_i8_i8( x, y );
}

INLINE
char2 OVERLOADABLE hadd( char2 x,
                         char2 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v2i8_v2i8( x, y );
}

INLINE
char3 OVERLOADABLE hadd( char3 x,
                         char3 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v3i8_v3i8( x, y );
}

INLINE
char4 OVERLOADABLE hadd( char4 x,
                         char4 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v4i8_v4i8( x, y );
}

INLINE
char8 OVERLOADABLE hadd( char8 x,
                         char8 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v8i8_v8i8( x, y );
}

INLINE
char16 OVERLOADABLE hadd( char16 x,
                          char16 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v16i8_v16i8( x, y );
}

INLINE
uchar OVERLOADABLE hadd( uchar x,
                         uchar y )
{
    return __builtin_spirv_OpenCL_u_hadd_i8_i8( x, y );
}

INLINE
uchar2 OVERLOADABLE hadd( uchar2 x,
                          uchar2 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v2i8_v2i8( x, y );
}

INLINE
uchar3 OVERLOADABLE hadd( uchar3 x,
                          uchar3 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v3i8_v3i8( x, y );
}

INLINE
uchar4 OVERLOADABLE hadd( uchar4 x,
                          uchar4 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v4i8_v4i8( x, y );
}

INLINE
uchar8 OVERLOADABLE hadd( uchar8 x,
                          uchar8 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v8i8_v8i8( x, y );
}

INLINE
uchar16 OVERLOADABLE hadd( uchar16 x,
                           uchar16 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v16i8_v16i8( x, y );
}

INLINE
short OVERLOADABLE hadd( short x,
                         short y )
{
    return __builtin_spirv_OpenCL_s_hadd_i16_i16( x, y );
}

INLINE
short2 OVERLOADABLE hadd( short2 x,
                          short2 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v2i16_v2i16( x, y );
}

INLINE
short3 OVERLOADABLE hadd( short3 x,
                          short3 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v3i16_v3i16( x, y );
}

INLINE
short4 OVERLOADABLE hadd( short4 x,
                          short4 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v4i16_v4i16( x, y );
}

INLINE
short8 OVERLOADABLE hadd( short8 x,
                          short8 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v8i16_v8i16( x, y );
}

INLINE
short16 OVERLOADABLE hadd( short16 x,
                           short16 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v16i16_v16i16( x, y );
}

INLINE
ushort OVERLOADABLE hadd( ushort x,
                          ushort y )
{
    return __builtin_spirv_OpenCL_u_hadd_i16_i16( x, y );
}

INLINE
ushort2 OVERLOADABLE hadd( ushort2 x,
                           ushort2 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v2i16_v2i16( x, y );
}

INLINE
ushort3 OVERLOADABLE hadd( ushort3 x,
                           ushort3 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v3i16_v3i16( x, y );
}

INLINE
ushort4 OVERLOADABLE hadd( ushort4 x,
                           ushort4 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v4i16_v4i16( x, y );
}

INLINE
ushort8 OVERLOADABLE hadd( ushort8 x,
                           ushort8 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v8i16_v8i16( x, y );
}

INLINE
ushort16 OVERLOADABLE hadd( ushort16 x,
                            ushort16 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v16i16_v16i16( x, y );
}

INLINE
int OVERLOADABLE hadd( int x,
                       int y )
{
    return __builtin_spirv_OpenCL_s_hadd_i32_i32( x, y );
}

INLINE
int2 OVERLOADABLE hadd( int2 x,
                        int2 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v2i32_v2i32( x, y );
}

INLINE
int3 OVERLOADABLE hadd( int3 x,
                        int3 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v3i32_v3i32( x, y );
}

INLINE
int4 OVERLOADABLE hadd( int4 x,
                        int4 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v4i32_v4i32( x, y );
}

INLINE
int8 OVERLOADABLE hadd( int8 x,
                        int8 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v8i32_v8i32( x, y );
}

INLINE
int16 OVERLOADABLE hadd( int16 x,
                         int16 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v16i32_v16i32( x, y );
}

INLINE
uint OVERLOADABLE hadd( uint x,
                        uint y )
{
    return __builtin_spirv_OpenCL_u_hadd_i32_i32( x, y );
}

INLINE
uint2 OVERLOADABLE hadd( uint2 x,
                         uint2 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v2i32_v2i32( x, y );
}

INLINE
uint3 OVERLOADABLE hadd( uint3 x,
                         uint3 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v3i32_v3i32( x, y );
}

INLINE
uint4 OVERLOADABLE hadd( uint4 x,
                         uint4 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v4i32_v4i32( x, y );
}

INLINE
uint8 OVERLOADABLE hadd( uint8 x,
                         uint8 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v8i32_v8i32( x, y );
}

INLINE
uint16 OVERLOADABLE hadd( uint16 x,
                          uint16 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v16i32_v16i32( x, y );
}

INLINE
long OVERLOADABLE hadd( long x,
                        long y )
{
    return __builtin_spirv_OpenCL_s_hadd_i64_i64( x, y );
}

INLINE
long2 OVERLOADABLE hadd( long2 x,
                         long2 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v2i64_v2i64( x, y );
}

INLINE
long3 OVERLOADABLE hadd( long3 x,
                         long3 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v3i64_v3i64( x, y );
}

INLINE
long4 OVERLOADABLE hadd( long4 x,
                         long4 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v4i64_v4i64( x, y );
}

INLINE
long8 OVERLOADABLE hadd( long8 x,
                         long8 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v8i64_v8i64( x, y );
}

INLINE
long16 OVERLOADABLE hadd( long16 x,
                          long16 y )
{
    return __builtin_spirv_OpenCL_s_hadd_v16i64_v16i64( x, y );
}

INLINE
ulong OVERLOADABLE hadd( ulong x,
                         ulong y )
{
    return __builtin_spirv_OpenCL_u_hadd_i64_i64( x, y );
}

INLINE
ulong2 OVERLOADABLE hadd( ulong2 x,
                          ulong2 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v2i64_v2i64( x, y );
}

INLINE
ulong3 OVERLOADABLE hadd( ulong3 x,
                          ulong3 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v3i64_v3i64( x, y );
}

INLINE
ulong4 OVERLOADABLE hadd( ulong4 x,
                          ulong4 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v4i64_v4i64( x, y );
}

INLINE
ulong8 OVERLOADABLE hadd( ulong8 x,
                          ulong8 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v8i64_v8i64( x, y );
}

INLINE
ulong16 OVERLOADABLE hadd( ulong16 x,
                           ulong16 y )
{
    return __builtin_spirv_OpenCL_u_hadd_v16i64_v16i64( x, y );
}

