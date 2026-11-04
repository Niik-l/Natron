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

#ifndef NATRON_ENGINE_CRYPTOMATTEHASH_H
#define NATRON_ENGINE_CRYPTOMATTEHASH_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <cstdint>
#include <string>

NATRON_NAMESPACE_ENTER

/**
 * @brief The Cryptomatte ID maths (spec 1.2.0, Psyop):
 *  - an object's ID is MurmurHash3_x86_32 of its UTF-8 name with seed 0,
 *  - the ID is stored in the EXR as the float32 whose bits are that hash, with the exponent
 *    nudged away from 0 and 255 so the value is never a NaN, an infinity or a denormal,
 *  - the manifest lists each name with its hash as 8 lowercase hex digits.
 * Standalone (no Cycles / OIIO dependency) so the decoder builds everywhere.
 */
namespace CryptoHash {

// MurmurHash3_x86_32 (Austin Appleby, public domain).
std::uint32_t murmurHash3_32(const void* key, int len, std::uint32_t seed);

// The hash of a name, as the spec defines it.
std::uint32_t hashName(const std::string& name);

// The float32 that carries this hash in a Cryptomatte layer.
float hashToFloat(std::uint32_t hash);

// The bits of an ID read from a layer, undoing nothing: IDs are compared bitwise, and the
// exponent nudge is part of the stored value on both sides.
std::uint32_t floatToBits(float f);

// Hash -> "%08x" and back ("2ce615c5"; returns false for anything but 8 hex digits).
std::string hashToHex(std::uint32_t hash);
bool hexToHash(const std::string& hex, std::uint32_t* hash);

// The "keyable surface" preview colour of an ID: the hash of the ID's bits spread over
// three channels, as the Blender compositor and the Nuke gizmo do it.
void idToPreviewColor(std::uint32_t idBits, float* r, float* g, float* b);

} // namespace CryptoHash

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CRYPTOMATTEHASH_H
