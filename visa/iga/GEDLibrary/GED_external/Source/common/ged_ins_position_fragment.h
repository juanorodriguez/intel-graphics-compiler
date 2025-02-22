/*========================== begin_copyright_notice ============================

Copyright (c) 2015-2021 Intel Corporation

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

#ifndef GED_INS_POSITION_FRAGMENT_H
#define GED_INS_POSITION_FRAGMENT_H

#include "common/ged_int_utils.h"


extern const uint32_t rightShiftedMasks[];

/*!
 * Structure for describing one fragment of an instruction field's bit-position in the instruction bits.
 */
struct ged_ins_field_position_fragment_t
{
    uint8_t _lowBit;        // first bit of the field in the instruction - not necessary, but nice to have
    uint8_t _highBit;       // last bit of the field in the instruction  - not necessary, but nice to have
    uint8_t _dwordIndex;    // dword in which this field resides
    int8_t _shift;          // number of bits to shift (positive _shift means - right shift) in order to align the bits
                            //   to their correct position in the total value (calculated within the current dword)
    uint32_t _bitMask;      // bit mask for extracting the field from within the dword

    bool operator==(const ged_ins_field_position_fragment_t& rhs) const;
    inline bool operator!=(const ged_ins_field_position_fragment_t& rhs) const { return !(*this == rhs); }
};


/*!
 * String representation of the ged_ins_field_position_fragment_t type name.
 */
extern const char* ged_ins_field_position_fragment_t_str;


/*!
 * Properly fills all fields of the given position fragment.
 *
 * @param[out]  fragment    The fragment to be filled.
 * @param[in]   lowBit      The lowest bit (inclusive) of the given fragment.
 * @param[in]   highBit     The highest bit (inclusive) of the given fragment.
 */
extern void FillPositionFragment(ged_ins_field_position_fragment_t* fragment, const uint8_t lowBit, const uint8_t highBit);


/*!
 * Get the size (in bits) of the given fragment.
 *
 * @param[in]   fragmentPtr     Pointer to the fragment for which to calculate the size.
 *
 * @return      The number of bits in the given fragment.
 */
extern uint8_t FragmentSize(const ged_ins_field_position_fragment_t* fragmentPtr);


/*!
 * Get the size (in bits) of the given fragment.
 *
 * @param[in]   fragment    The fragment for which to calculate the size.
 *
 * @return      The number of bits in the given fragment.
 */
extern uint8_t FragmentSize(const ged_ins_field_position_fragment_t& fragment);


/*!
 * Get the maximum possible value for a given position fragment.
 *
 * @param[in]   fragment    The fragment for which to calculate the maximum value.
 *
 * @return      The maximum possible value.
 */
extern uint32_t MaxFragmentValue(const ged_ins_field_position_fragment_t& fragment);


/*!
 * Get the maximum possible value for a given position fragment.
 *
 * @param[in]   fragment    Pointer to the fragment for which to calculate the maximum value.
 *
 * @return      The maximum possible value.
 */
extern uint32_t MaxFragmentValue(const ged_ins_field_position_fragment_t* fragment);

#endif // GED_INS_POSITION_FRAGMENT_H
