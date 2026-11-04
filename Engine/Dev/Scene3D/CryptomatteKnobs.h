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

#ifndef NATRON_ENGINE_CRYPTOMATTEKNOBS_H
#define NATRON_ENGINE_CRYPTOMATTEKNOBS_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <string>

#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The two Cryptomatte strings a 3D node can carry, read by CyclesRender when it writes
 * Cryptomatte layers:
 *  - "cryptoAsset": the name hashed into the CryptoAsset layer (empty = inherit from the nearest
 *    Group3D above that sets one, else the top-most group's name, else the node itself);
 *  - "cryptoCustom": the name hashed into the CryptoCustom layer (empty = inherit from the nearest
 *    group that sets one, else "default").
 * The renderer looks the knobs up by name, so a user-added String parameter with the same script
 * name on any other node works too.
 */
#define kCryptomatteAssetKnobName "cryptoAsset"
#define kCryptomatteCustomKnobName "cryptoCustom"

// Adds a "Cryptomatte" page with the two string knobs to a node (call from initializeKnobs()).
void createCryptomatteKnobs(EffectInstance* effect);

// The value of one of the two knobs on a node, "" if the node has no such string knob.
std::string getCryptomatteKnobValue(const NodePtr& node, const char* knobName);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CRYPTOMATTEKNOBS_H
