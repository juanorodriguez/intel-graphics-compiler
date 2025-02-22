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

#include "../../Headers/spirv.h"

#ifndef __EXP_HYPERBOLIC_CL__
#define __EXP_HYPERBOLIC_CL__

/*
//               INTEL CORPORATION PROPRIETARY INFORMATION
//  This software is supplied under the terms of a license agreement or
//  nondisclosure agreement with Intel Corporation and may not be copied
//  or disclosed except in accordance with the terms of that agreement.
//    Copyright (C) 1996-2010 Intel Corporation. All Rights Reserved.
//
*/

// These versions of exp are only used for hyperbolic functions:
//      sinh, cosh and tanh.

// There are two main differences between them and the regular old
// version of exp:
//  - This version uses a polynomial approximation for the
//    fractional part of 2^x, vs. the native version.  As such
//    it is slightly more accurate.
//  - This version accepts an optional "scale factor" that can
//    be used to scale the result by 2^scale, without losing
//    accuracy.

// This version is used for sinh and cosh, and part of tanh:
float __intel_exp_for_hyper(float x, float scale)
{
    // e^x = 2^(log2(e^x)) = 2^(x * log2(e))
    // We'll compute 2^(x * log2(e)) by splitting x * log2(e)
    //   into a whole part and fractional part.

    // Compute the whole part of x * log2(e)
    // This part is easy!
    // Note: floor rounds to negative infinity.
    float w = __builtin_spirv_OpenCL_floor_f32( x * M_LOG2E_F + 0.5f );

    // Compute the fractional part of x * log2(e)
    // We have to do this carefully, so we don't lose precision.
    // Compute as:
    //   fract( x * log2(e) ) = ( x - w * C1 - w * C2 ) * log2(e)
    // C1 is the "Cephes Constant", and is close to 1/log2(e)
    // C2 is the difference between the "Cephes Constant" and 1/log2(e)
    const float C1 = as_float( 0x3F317200 );    // 0.693145751953125
    const float C2 = as_float( 0x35BFBE8E );    // 0.000001428606765330187
    float f = x;
    f = __builtin_spirv_OpenCL_fma_f32_f32_f32( w, -C1, f );
    f = __builtin_spirv_OpenCL_fma_f32_f32_f32( w, -C2, f );

    // Do a polynomial approximation for 2^fractional.
    float f2 = f * f;
    f = ((((( 1.9875691500E-4f  * f
            + 1.3981999507E-3f) * f
            + 8.3334519073E-3f) * f
            + 4.1665795894E-2f) * f
            + 1.6666665459E-1f) * f
            + 5.0000001201E-1f) * f2
            + f
            + 1.0f;

    // By doing this computation as 2^(w - 2) * 2^2 we can avoid an
    // overflow case for very large values of w.
    w = __builtin_spirv_OpenCL_native_exp2_f32( w + scale );   // this should be exact

    float res = w * f;
    res = ( x < as_float( 0xC2D20000 ) ) ? as_float( 0x00000000 ) : res;
    res = ( x > as_float( 0x42D20000 ) ) ? as_float( 0x7F800000 ) : res;

    return res;
}

float __intel_exp_for_tanh(float x, float scale)
{
    float px = __builtin_spirv_OpenCL_fabs_f32(x);

    // e^x = 2^(log2(e^x)) = 2^(x * log2(e))
    // We'll compute 2^(x * log2(e)) by splitting x * log2(e)
    //   into a whole part and fractional part.

    // Compute the whole part of x * log2(e)
    // This part is easy!
    // Note: floor rounds to negative infinity.
    float w = __builtin_spirv_OpenCL_floor_f32( px * M_LOG2E_F + 0.5f );

    // Compute the fractional part of x * log2(e)
    // We have to do this carefully, so we don't lose precision.
    // Compute as:
    //   fract( x * log2(e) ) = ( x - w * C1 - w * C2 ) * log2(e)
    // C1 is the "Cephes Constant", and is close to 1/log2(e)
    // C2 is the difference between the "Cephes Constant" and 1/log2(e)

    const float C1 = as_float( 0x3F317200 );    // 0.693145751953125
    const float C2 = as_float( 0x35BFBE8E );    // 0.000001428606765330187
    float f = px;
    f = __builtin_spirv_OpenCL_fma_f32_f32_f32( w, -C1, f );
    f = __builtin_spirv_OpenCL_fma_f32_f32_f32( w, -C2, f );

    // Do a polynomial approximation for 2^fractional.
    float tf =
        ((((( 1.9875691500E-4f  * f
            + 1.3981999507E-3f) * f
            + 8.3334519073E-3f) * f
            + 4.1665795894E-2f) * f
            + 1.6666665459E-1f) * f
            + 5.0000001201E-1f) * f * f
            + f
            + 1.0f;

    float ns = -scale;
    scale = ( x >= 0.0f ) ? scale : ns;
    w = __builtin_spirv_OpenCL_native_exp2_f32( w + scale );  // this should be exact

    float res = w * tf;
    res = ( px < as_float( 0xC2D20000 ) ) ? as_float( 0x00000000 ) : res;
    res = ( px > as_float( 0x42D20000 ) ) ? as_float( 0x7F800000 ) : res;

    float rx = __builtin_spirv_OpenCL_native_recip_f32( res );
    res = ( x >= 0.0f ) ? res : rx;

    return res;
}

#endif  //__EXP_HYPERBOLIC_CL__
