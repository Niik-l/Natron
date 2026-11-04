/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "CryptomatteHash.h"

#include <cstdio>
#include <cstring>

NATRON_NAMESPACE_ENTER

namespace CryptoHash {

namespace {

inline std::uint32_t
rotl32(std::uint32_t x, int r)
{
    return (x << r) | (x >> (32 - r));
}

inline std::uint32_t
fmix32(std::uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

} // anonymous namespace

std::uint32_t
murmurHash3_32(const void* key, int len, std::uint32_t seed)
{
    const std::uint8_t* data = (const std::uint8_t*)key;
    const int nblocks = len / 4;
    std::uint32_t h1 = seed;
    const std::uint32_t c1 = 0xcc9e2d51u;
    const std::uint32_t c2 = 0x1b873593u;

    for (int i = 0; i < nblocks; ++i) {
        std::uint32_t k1;
        std::memcpy(&k1, data + i * 4, 4);   // little-endian blocks, as the reference does on x86
        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;
        h1 ^= k1;
        h1 = rotl32(h1, 13);
        h1 = h1 * 5 + 0xe6546b64u;
    }

    const std::uint8_t* tail = data + nblocks * 4;
    std::uint32_t k1 = 0;
    switch (len & 3) {
    case 3:
        k1 ^= (std::uint32_t)tail[2] << 16;
        // fall through
    case 2:
        k1 ^= (std::uint32_t)tail[1] << 8;
        // fall through
    case 1:
        k1 ^= tail[0];
        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;
        h1 ^= k1;
    }

    h1 ^= (std::uint32_t)len;
    return fmix32(h1);
}

std::uint32_t
hashName(const std::string& name)
{
    return murmurHash3_32(name.data(), (int)name.size(), 0);
}

float
hashToFloat(std::uint32_t hash)
{
    // Exponent 0 (zero / denormal) or 255 (inf / NaN) would not survive a float pipeline:
    // flip the lowest exponent bit, as every encoder does.
    const std::uint32_t exponent = (hash >> 23) & 0xFFu;
    if (exponent == 0 || exponent == 255) {
        hash ^= 1u << 23;
    }
    float f;
    std::memcpy(&f, &hash, 4);
    return f;
}

std::uint32_t
floatToBits(float f)
{
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

std::string
hashToHex(std::uint32_t hash)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", hash);
    return buf;
}

bool
hexToHash(const std::string& hex, std::uint32_t* hash)
{
    if (hex.size() != 8) {
        return false;
    }
    std::uint32_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
        const char c = hex[i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return false;
        }
        v = (v << 4) | (std::uint32_t)d;
    }
    *hash = v;
    return true;
}

void
idToPreviewColor(std::uint32_t idBits, float* r, float* g, float* b)
{
    // Hash the ID bits once more so neighbouring IDs get unrelated colours; the shifted
    // products of the hash give three pseudo-random channels in [0, 1).
    const std::uint32_t h = murmurHash3_32(&idBits, 4, 0);
    const float denom = 4294967295.f;
    *r = (float)(std::uint32_t)(h << 8) / denom;
    *g = (float)(std::uint32_t)(h << 16) / denom;
    *b = (float)(std::uint32_t)(h << 24) / denom;
}

} // namespace CryptoHash

NATRON_NAMESPACE_EXIT
