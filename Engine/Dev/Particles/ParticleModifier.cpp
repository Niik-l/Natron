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

#include "ParticleModifier.h"
#include "../DotUtils.h"

#include <algorithm>
#include <cmath>

#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

ParticleModifier::ParticleModifier(NodePtr node)
    : EffectInstance(node)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ParticleModifier::~ParticleModifier()
{
}

std::string
ParticleModifier::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "particles";
    return "";
}

void
ParticleModifier::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ParticleModifier::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ParticleModifier::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// Stateless pass-through with preview integration.
// Gets upstream data, applies this force, then does a one-frame integration
// so the viewer shows the effect of forces on positions.
// No persistent cache — this is for preview only. The real simulation
// with proper multi-frame accumulation is done by the solver (ParticleSolver).
ParticleDataPtr
ParticleModifier::getParticleData(double time)
{
    EffectInstancePtr input = skipDots(getInput(0));
    if (!input) return ParticleDataPtr();
    ParticleProvider* provider = dynamic_cast<ParticleProvider*>(input.get());
    if (!provider) return ParticleDataPtr();

    ParticleDataPtr upstreamData = provider->getParticleData(time);
    if (!upstreamData) return upstreamData;

    // Deep copy so we don't modify upstream's cache
    ParticleDataPtr result = std::make_shared<ParticleData>();
    result->particles = upstreamData->particles;

    // Apply this force statelessly (modifies velocities)
    applyForce(result, time);

    return result;
}

// Walk upstream from startNode collecting force nodes and finding the emitter.
void
ParticleModifier::collectUpstreamForces(EffectInstance* startNode,
                                        std::vector<ParticleModifier*>& outForces,
                                        ParticleProvider*& outEmitter)
{
    outEmitter = nullptr;
    outForces.clear();

    EffectInstance* current = startNode;
    while (current) {
        // See through routing nodes (Dot / DevStamp) anywhere in the force chain.
        while (current && isGraphPassthrough(current->getPluginID())) {
            current = current->getInput(0).get();
        }
        if (!current) break;
        ParticleModifier* mod = dynamic_cast<ParticleModifier*>(current);
        if (mod) {
            // Only collect if it's a force (applyForce does something),
            // not another solver/collider (applyForce is a no-op).
            outForces.push_back(mod);
            EffectInstancePtr nextInput = current->getInput(0);
            current = nextInput.get();
        } else {
            // Must be the emitter
            outEmitter = dynamic_cast<ParticleProvider*>(current);
            break;
        }
    }
    // Reverse so emitter-adjacent forces come first
    std::reverse(outForces.begin(), outForces.end());
}

StatusEnum
ParticleModifier::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                        ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ParticleModifier::render(const RenderActionArgs& args)
{
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

#include "moc_ParticleModifier.cpp"
