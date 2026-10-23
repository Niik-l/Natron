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

#ifndef NATRON_ENGINE_DEEPDEFOCUS_H
#define NATRON_ENGINE_DEEPDEFOCUS_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../PostFX/LensDefocus.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Depth of field on deep compositing data: LensDefocus with a deep input.
 *
 * Every deep sample blurs at its own depth (hidden layers show through a blurred foreground as
 * they are, edges and volumes keep their own depths), with all of LensDefocus: camera matching
 * (Camera input, camera from file, focal length / film back / f-stop), real lens designs, bokeh
 * shape, image stages, focus picking and the debug outputs. The depth-pass settings are not used.
 * Old DeepDefocus projects carry their knob values over (LensDefocus::onKnobsLoaded).
 */
class DeepDefocus
    : public LensDefocus
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new DeepDefocus(n); }

    DeepDefocus(NodePtr node) : LensDefocus(node, true) {}
    virtual ~DeepDefocus() {}

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_DEEPDEFOCUS; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "DeepDefocus"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back(PLUGIN_GROUP_DEEP); }
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEEPDEFOCUS_H
