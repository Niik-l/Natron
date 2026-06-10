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

#ifndef NATRON_ENGINE_DEV_DOTUTILS_H
#define NATRON_ENGINE_DEV_DOTUTILS_H

#include "../EffectInstance.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Follow input 0 through any chain of Dot routing nodes, returning the
 * first non-Dot effect (or null). Dots are pure pass-throughs in the node graph.
 *
 * Any typed node input resolved via dynamic_cast — a camera (CameraProvider /
 * Camera3DNode), render settings (CyclesRenderSettings), a material
 * (MaterialProvider), a scene/geo node, etc. — MUST run its `getInput(slot)`
 * through this first. Otherwise a Dot wired between the source and the consumer
 * makes the dynamic_cast see the Dot (which is none of those types) and resolve
 * to null, so the input silently does nothing. Scene/geo discovery walks
 * (collectSceneNodes) already see through Dots; this is the single-input analog.
 */
inline EffectInstancePtr
skipDots(EffectInstancePtr eff)
{
    while (eff && eff->getPluginID() == PLUGINID_NATRON_DOT) {
        eff = eff->getInput(0);
    }
    return eff;
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_DOTUTILS_H
