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

#include "Scene3D.h"

#include <sstream>

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

Scene3D::Scene3D(NodePtr node)
    : EffectInstance(node)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Scene3D::~Scene3D()
{
}

std::string
Scene3D::getPluginDescription() const
{
    return tr("Aggregates multiple 3D objects into a single scene.\n\n"
              "Connect geometry nodes (Sphere3D, Card3D, Cube3D, Cylinder3D, ReadGeo) "
              "to the inputs, then connect this Scene to ScanlineRender's obj/scn input.\n\n"
              "All connected objects will be rendered together.\n\n"
              "Equivalent to Nuke's Scene node.").toStdString();
}

std::string
Scene3D::getInputLabel(int inputNb) const
{
    std::ostringstream ss;
    ss << (inputNb + 1);
    return ss.str();
}

void
Scene3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Scene3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Scene3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Scene3D::initializeKnobs()
{
    // Scene node has no knobs — it's purely a pass-through aggregator
}

int
Scene3D::getNumConnectedInputs() const
{
    int count = 0;
    for (int i = 0; i < SCENE3D_MAX_INPUTS; ++i) {
        if (getInput(i)) ++count;
    }
    return count;
}

EffectInstancePtr
Scene3D::getConnectedInput(int index) const
{
    // Return the Nth connected input (skipping empty slots)
    int count = 0;
    for (int i = 0; i < SCENE3D_MAX_INPUTS; ++i) {
        EffectInstancePtr inp = getInput(i);
        if (inp) {
            if (count == index) return inp;
            ++count;
        }
    }
    return EffectInstancePtr();
}

StatusEnum
Scene3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                               ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Scene3D::render(const RenderActionArgs& args)
{
    // Scene produces no image — just a black placeholder
    if (args.outputPlanes.empty()) return eStatusOK;

    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;

    RectI bounds = outImg->getBounds();
    Image::WriteAccess wa(outImg.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* pix = (float*)wa.pixelAt(x, y);
            if (pix) { pix[0] = pix[1] = pix[2] = pix[3] = 0.0f; }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Scene3D.cpp"
