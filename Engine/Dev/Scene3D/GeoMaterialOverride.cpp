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

#include "GeoMaterialOverride.h"

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

GeoMaterialOverride::GeoMaterialOverride(NodePtr node)
    : EffectInstance(node)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

GeoMaterialOverride::~GeoMaterialOverride()
{
}

std::string
GeoMaterialOverride::getPluginDescription() const
{
    return tr("Assigns a different material to specific sub-objects of an upstream "
              "geometry source (e.g. individual meshes inside a ReadAlembicArchive), "
              "without creating new geometry.\n\n"
              "Connect the geo (the archive) to input 0 and a Material3D to the Mat "
              "input, then list the full archive paths to override in the Surfaces "
              "field (one per line). Chain several to assign several materials.\n\n"
              "Precedence: a downstream / per-pass material override still wins; "
              "this overrides the archive's own material; unlisted surfaces keep the "
              "archive's base material.").toStdString();
}

std::string
GeoMaterialOverride::getInputLabel(int inputNb) const
{
    if (inputNb == 1) return "Mat";
    return "Geo";
}

void
GeoMaterialOverride::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
GeoMaterialOverride::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
GeoMaterialOverride::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
GeoMaterialOverride::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobStringPtr surfaces = AppManager::createKnob<KnobString>(this, tr("Surfaces"));
    surfaces->setName("surfaces");
    surfaces->setAsMultiLine();
    surfaces->setAnimationEnabled(false);
    surfaces->setEvaluateOnChange(true);
    surfaces->setHintToolTip(tr("Full archive paths of the sub-objects to override, "
                                "one per line (e.g. industrial_wall_lamp/polySurface2/"
                                "polySurfaceShape2). Matches the entry's full path as "
                                "shown in the Alembic archive's tree. The connected Mat "
                                "is applied to exactly these surfaces."));
    page->addKnob(surfaces);
    _surfacesKnob = surfaces;
}

void
GeoMaterialOverride::getSurfacePaths(std::set<std::string>& out) const
{
    KnobStringPtr k = _surfacesKnob.lock();
    if (!k) return;
    const std::string raw = k->getValue();

    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) nl = raw.size();
        std::string line = raw.substr(pos, nl - pos);
        // Trim trailing CR (Windows line endings) + surrounding whitespace.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
        if (lead) line.erase(0, lead);
        if (!line.empty()) out.insert(line);
        if (nl == raw.size()) break;
        pos = nl + 1;
    }
}

StatusEnum
GeoMaterialOverride::getPreferredMetadata(NodeMetadata& metadata)
{
    // Pass through the geo input's frame-varying flag so scrubbing invalidates
    // the downstream render cache (mirrors Scene3D).
    EffectInstancePtr geo = getInput(0);
    if (geo && geo->getHasAnimation()) {
        metadata.setIsFrameVarying(true);
    }
    return eStatusOK;
}

StatusEnum
GeoMaterialOverride::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                           ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
GeoMaterialOverride::render(const RenderActionArgs& args)
{
    // Scene-graph-only node — produces no image (black placeholder, like Scene3D).
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
