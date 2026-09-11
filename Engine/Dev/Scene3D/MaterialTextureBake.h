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

#ifndef NATRON_ENGINE_MATERIALTEXTUREBAKE_H
#define NATRON_ENGINE_MATERIALTEXTUREBAKE_H

// Baking a shape's img input to a texture file for Cycles.
//
// The 3D viewport and ScanlineRender read a shape's img input as an image;
// Cycles only textures from files. Material3D bakes its own map inputs to
// Radiance HDR (Material3D::bakeInputTextures), which has no alpha channel.
// The primitives (Card3D, Sphere3D, Cube3D, Cylinder3D) need their img input
// with alpha - a card IS its cutout - so they bake to RGBA EXR through the
// helpers below, keeping the pixels premultiplied the way Natron delivers
// them (CyclesRenderer marks the image node's alpha associated).
//
// Implemented in Material3D.cpp, next to the HDR bake.

#include <mutex>
#include <string>

#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

/** Per-shape state for its baked img-input texture. */
struct BakedInputTexture
{
    mutable std::mutex mutex;
    std::string path;              // empty until the first successful bake
    unsigned long long hash = 0;   // input chain hash at the last bake
    double time = 0.0;             // time of the last bake
    bool baked = false;
};

/** Hash of one input slot's whole upstream chain, mixed the way
 *  Material3D::getMaterialInputsHash mixes its six slots, so the Cycles scene
 *  hash changes when anything upstream of the image is edited. */
unsigned long long materialInputChainHash(const EffectInstancePtr& input, int slot);

// Key for a shape's / material's viewport preview texture: the img input's
// hash, the frame, and (for a shape) the connected material and its diffuse
// input. updateCachedTexture() skips the input render + downscale when this
// has not changed since the last build - the 3D viewport asks for it on
// every paint, for every shape, and a full-res input fetch each time made
// the viewport crawl as soon as an image was attached to geometry.
unsigned long long materialTextureCacheKey(const EffectInstancePtr& input, double time, EffectInstance* material);

/** Render `input` at full resolution, time `time`, to
 *  <TEMP>/natron_<tag>_<owner>.exr (RGBA half, zip, premultiplied).
 *  Returns the path, or an empty string when the render or write failed. */
std::string bakeImageInputToExr(EffectInstance* input, double time, const char* tag, const void* owner);

/** Bake `owner`'s input `inputIdx` unless its chain hash and the time are
 *  unchanged since the last bake, and publish the result into `state`
 *  (cleared when the input is disconnected). */
void bakeShapeImageInput(EffectInstance* owner, int inputIdx, double time, const char* tag, BakedInputTexture& state);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_MATERIALTEXTUREBAKE_H
