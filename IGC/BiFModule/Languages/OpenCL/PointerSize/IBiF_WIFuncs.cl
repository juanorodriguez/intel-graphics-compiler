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
// Work-Item functions
//*****************************************************************************/
#define MAX_DIM 2


INLINE size_t OVERLOADABLE get_enqueued_local_size(uint dim) {
  if (dim > MAX_DIM) {
    return 1;
  }
  return __builtin_IB_get_enqueued_local_size(dim);
}

INLINE size_t OVERLOADABLE get_global_id(uint dim) {
  if (dim > MAX_DIM) {
    return 0;
  }

  return get_group_id(dim) * get_enqueued_local_size(dim) + get_local_id(dim) + get_global_offset(dim);
}

INLINE size_t OVERLOADABLE get_group_id(uint dim) {
  if (dim > MAX_DIM) {
    return 0;
  }
  return __builtin_IB_get_group_id(dim);
}

INLINE size_t OVERLOADABLE get_local_id(uint dim) {
  if (dim > MAX_DIM) {
    return 0;
  } else if (dim == 0) {
    return __builtin_IB_get_local_id_x();
  } else if (dim == 1) {
    return __builtin_IB_get_local_id_y();
  } else if (dim == 2) {
    return __builtin_IB_get_local_id_z();
  }
}

INLINE size_t OVERLOADABLE get_num_groups(uint dim) {
  if (dim > MAX_DIM) {
    return 1;
  }
  return __builtin_IB_get_num_groups(dim);
}

INLINE size_t OVERLOADABLE get_global_size(uint dim) {
  if (dim > MAX_DIM) {
    return 1;
  }
  return __builtin_IB_get_global_size(dim);
}

INLINE size_t OVERLOADABLE get_local_size(uint dim) {
  if (dim > MAX_DIM) {
    return 1;
  }
  return __builtin_IB_get_local_size(dim);
}

INLINE size_t OVERLOADABLE get_global_offset(uint dim) {
  if (dim > MAX_DIM) {
    return 0;
  }
  return __builtin_IB_get_global_offset(dim);
}

INLINE size_t OVERLOADABLE get_global_linear_id( void ) {
  uint dim = get_work_dim();
  size_t result = 0;

  switch (dim) {
    default:
    case 1:
      result = get_global_id(0) - get_global_offset(0);
      break;
    case 2:
      result = (get_global_id(1) - get_global_offset(1))*
                get_global_size (0) + (get_global_id(0) - get_global_offset(0));
      break;
    case 3:
      result = ((get_global_id(2) - get_global_offset(2)) * get_global_size(1) * get_global_size(0)) +
               ((get_global_id(1) - get_global_offset(1)) * get_global_size (0)) +
               (get_global_id(0) - get_global_offset(0));
      break;
  }

  return result;
}

INLINE size_t OVERLOADABLE get_local_linear_id( void ) {
#if 0
    // This doesn't work right now due to a bug in the runtime.
    // If/when they fix their bug we can experiment if spending the
    // register(s) for get_local_linear_id() is better than spending
    // the math to compute the linear local ID.
    return __builtin_IB_get_local_linear_id();
#else
    uint    llid;

    llid  = (uint)get_local_id(2);
    llid *= (uint)get_local_size(1);
    llid += (uint)get_local_id(1);
    llid *= (uint)get_local_size(0);
    llid += (uint)get_local_id(0);

    return llid;
#endif
}

uint __intel_get_local_size( void )
{
    uint    totalWorkGroupSize =
                (uint)get_local_size(0) *
                (uint)get_local_size(1) *
                (uint)get_local_size(2);
    return totalWorkGroupSize;
}

uint __intel_get_enqueued_local_size( void )
{
    uint    totalWorkGroupSize =
                (uint)get_enqueued_local_size(0) *
                (uint)get_enqueued_local_size(1) *
                (uint)get_enqueued_local_size(2);
    return totalWorkGroupSize;
}

uint __intel_get_local_linear_id( void )
{
    return get_local_linear_id();
}

bool __intel_is_first_work_group_item( void )
{
    return get_local_id(0) == 0 &
           get_local_id(1) == 0 &
           get_local_id(2) == 0;
}

