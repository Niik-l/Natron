/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef NATRON_ENGINE_CYCLESSETTINGSCONSUMER_H
#define NATRON_ENGINE_CYCLESSETTINGSCONSUMER_H

#include "../../../Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A render node with a CyclesRenderSettings input (CyclesRender, CyclesRenderPass). While
 * one is connected the node hides the knobs it overrides and shows a read-only line with what the
 * settings node sets (device, samples, denoise); CyclesRenderSettings calls refreshSettingsInfo()
 * on the nodes downstream of it (through Dots) when one of its knobs changes.
 */
class CyclesSettingsConsumer
{
public:
    virtual ~CyclesSettingsConsumer() {}

    virtual void refreshSettingsInfo() = 0;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CYCLESSETTINGSCONSUMER_H
