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

#include "ParticleMerge.h"

#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

ParticleMerge::ParticleMerge(NodePtr node)
    : EffectInstance(node)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ParticleMerge::~ParticleMerge()
{
}

std::string
ParticleMerge::getPluginDescription() const
{
    return tr("Merges multiple particle streams into one.\n\n"
              "Connect different particle chains to combine their particles.").toStdString();
}

std::string
ParticleMerge::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "A";
        case 1: return "B";
        case 2: return "C";
        case 3: return "D";
        default: return "";
    }
}

void
ParticleMerge::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ParticleMerge::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ParticleMerge::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ParticleMerge::initializeKnobs()
{
    // No knobs needed — ParticleMerge just combines inputs
}

ParticleDataPtr
ParticleMerge::getParticleData(double time)
{
    ParticleDataPtr result = std::make_shared<ParticleData>();

    for (int i = 0; i < 4; ++i) {
        EffectInstancePtr input = getInput(i);
        if (!input) continue;
        ParticleProvider* provider = dynamic_cast<ParticleProvider*>(input.get());
        if (!provider) continue;
        ParticleDataPtr data = provider->getParticleData(time);
        if (!data) continue;

        // Append all particles from this input
        result->particles.insert(result->particles.end(),
                                 data->particles.begin(),
                                 data->particles.end());
    }

    return result;
}

StatusEnum
ParticleMerge::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                     ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ParticleMerge::render(const RenderActionArgs& args)
{
    // Fill the dummy 1x1 output with black to avoid NaN warnings
    std::list< std::pair<ImagePlaneDesc, ImagePtr> > outputPlanes = args.outputPlanes;
    for (auto& plane : outputPlanes) {
        if (plane.second) {
            plane.second->fillZero(args.roi);
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleMerge.cpp"
