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

#include "VRViewer.h"

#include "../../AppManager.h"

NATRON_NAMESPACE_ENTER

VRViewer::VRViewer(NodePtr node)
    : ViewerInstance(node)
{
}

VRViewer::~VRViewer()
{
}

std::string
VRViewer::getPluginID() const
{
    return PLUGINID_NATRON_VRVIEWER;
}

std::string
VRViewer::getPluginLabel() const
{
    return "VR Viewer";
}

void
VRViewer::getPluginGrouping(std::list<std::string>* grouping) const
{
    grouping->push_back(PLUGIN_GROUP_VR);
}

std::string
VRViewer::getPluginDescription() const
{
    return tr("The VR viewer: a real Viewer dedicated to the "
              "equirectangular / 360 presentation. Wire footage into its "
              "inputs and a VR viewer pane opens bound to it. Everything "
              "works exactly like the 2D Viewer (image cache, channels, "
              "gain/gamma, color transform, A/B inputs, proxy scale, playback) "
              "— the pane samples the rendered texture on a sphere.").toStdString();
}

NATRON_NAMESPACE_EXIT
