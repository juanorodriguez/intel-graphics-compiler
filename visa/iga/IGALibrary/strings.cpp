/*========================== begin_copyright_notice ============================

Copyright (c) 2017-2021 Intel Corporation

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

#include "bits.hpp"
#include "strings.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>

using namespace iga;

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // omit min()/max() macros (favor std::min/std::max)
#endif
#include <Windows.h>
#include <malloc.h> /* for alloca */
#else
#include <alloca.h> /* for alloca */
#endif


std::ostream &operator <<(std::ostream &os, hex h)
{
    os << fmtHexDigits(h.value, h.cols);
    return os;
}


std::string iga::formatF(const char *pat, ...)
{
    va_list va;
    va_start(va, pat);
    std::string s = vformatF(pat, va);
    va_end(va);
    return s;
}


std::string iga::vformatF(const char *pat, va_list &va)
{
    va_list copy;
    va_copy(copy, va);
    size_t ebuflen = VSCPRINTF(pat, copy) + 1;
    va_end(copy);

    char *buf = (char *)alloca(ebuflen);
    VSPRINTF(buf, ebuflen, pat, va);
    buf[ebuflen - 1] = 0;

    return buf;
}


size_t iga::formatToF(std::ostream &out, const char *pat, ...)
{
    va_list va;
    va_start(va, pat);
    size_t s = vformatToF(out, pat, va);
    va_end(va);
    return s;
}


size_t iga::vformatToF(std::ostream &out, const char *pat, va_list &va)
{
    va_list copy;
    va_copy(copy, va);
    size_t ebuflen = VSCPRINTF(pat, copy) + 1;
    va_end(copy);

    char *buf = (char *)alloca(ebuflen);
    VSPRINTF(buf, ebuflen, pat, va);
    buf[ebuflen - 1] = 0;

    size_t initOff = (size_t)out.tellp();
    out << buf;
    return (size_t)out.tellp() - initOff;
}


size_t iga::formatToF(char *buf, size_t bufLen, const char *pat, ...)
{
    std::stringstream ss;
    va_list va;
    va_start(va, pat);
    vformatToF(ss, pat, va);
    va_end(va);
    return copyOut(buf, bufLen, ss);
}


size_t iga::vformatToF(char *buf, size_t bufLen, const char *pat, va_list &va)
{
    return VSPRINTF(buf, bufLen, pat, va);
}

std::string iga::trimTrailingWs(const std::string &s)
{
    int i = (int)s.size() - 1;
    while (i >= 0) {
        if (!isspace(s[i]))
            break;
        i--;
    }
    return s.substr(0, (size_t)i + 1);
}

size_t iga::copyOut(char *buf, size_t bufCap, std::iostream &ios)
{
    size_t sslen = (size_t)ios.tellp();
    if (!buf || bufCap == 0)
        return sslen + 1;
    ios.read(buf, bufCap);
    size_t eos = bufCap - 1 < sslen ? bufCap - 1 : sslen;
    buf[eos] = '\0';
    return sslen + 1;
}

void iga::fmtBinaryDigits(std::ostream &os, uint64_t val, int w)
{
    if (w == 0) {
        // STL really needs this
        // gcc has __builtin_clzll, but let's ignore the #ifdef nonsense
        w = std::max<int>(1, findLeadingOne(val) + 1);
    }

    for (int i = w - 1; i >= 0; i--) {
        if (val & (1ull<<(uint64_t)i)) {
            os << '1';
        } else {
            os << '0';
        }
    }
}
void iga::fmtBinary(std::ostream &os, uint64_t val, int w)
{
    os << "0b";
    fmtBinaryDigits(os, val, w);
}

void iga::fmtHexDigits(std::ostream &os, uint64_t val, int w)
{
    std::stringstream ss;
    if (w > 0) {
        ss << std::setw(w) << std::setfill('0') <<
            std::hex << std::uppercase << val;
    } else {
        ss << std::hex << std::uppercase << val;
    }
    os << ss.str();
}
std::string iga::fmtHexDigits(uint64_t val, int w)
{
    std::stringstream ss;
    fmtHexDigits(ss, val, w);
    return ss.str();
}

void iga::fmtHex(std::ostream &os, uint64_t val, int w)
{
    os << "0x";
    fmtHexDigits(os, val, w);
}
std::string iga::fmtHex(uint64_t val, int w)
{
    std::stringstream ss;
    fmtHex(ss, val, w);
    return ss.str();
}