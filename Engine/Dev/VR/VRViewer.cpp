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
#include "../../ImagePlaneDesc.h"

NATRON_NAMESPACE_ENTER

VRViewer::VRViewer(NodePtr node)
    : EffectInstance(node)
{
    // Default render-scale support keeps makePreviewImage's mipmapped fetches
    // legal; with the identity pass-through the upstream scales anyway.
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

std::string
VRViewer::getPluginDescription() const
{
    return tr("The VR viewport's sink node. Wire your equirectangular footage "
              "(or any processed node) into it; creating it opens a bound VR "
              "viewport pane. It never alters the image — a pure pass-through.").toStdString();
}

void
VRViewer::getPluginGrouping(std::list<std::string>* grouping) const
{
    grouping->push_back(PLUGIN_GROUP_VR);
}

std::string
VRViewer::getInputLabel(int /*inputNb*/) const
{
    return "src";
}

void
VRViewer::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
VRViewer::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
VRViewer::isIdentity(double time,
                     const RenderScale& /*scale*/,
                     const RectI& /*roi*/,
                     ViewIdx view,
                     double* inputTime,
                     ViewIdx* inputView,
                     int* inputNb)
{
    if (getInput(0)) {
        *inputTime = time;
        *inputView = view;
        *inputNb = 0;
        return true;
    }
    return false;
}

NATRON_NAMESPACE_EXIT
