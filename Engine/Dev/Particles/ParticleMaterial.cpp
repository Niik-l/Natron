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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ParticleMaterial.h"

#include <cassert>

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"
#include "../Scene3D/MaterialProvider.h"
#include "../DotUtils.h"

NATRON_NAMESPACE_ENTER

struct ParticleMaterialPrivate
{
    KnobDoubleWPtr emissionStrength;
    KnobDoubleWPtr roughness;
    KnobDoubleWPtr metallic;
    KnobBoolWPtr tintWithColor;
};

ParticleMaterial::ParticleMaterial(NodePtr node)
    : EffectInstance(node)
    , _imp(new ParticleMaterialPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ParticleMaterial::~ParticleMaterial()
{
}

std::string
ParticleMaterial::getPluginDescription() const
{
    return tr("Particle shading override for CyclesRender.\n\n"
              "Place anywhere in a particle chain (typically after the solver). "
              "Connect a material (Material3D, MergeMat, ...) to the 'mat' input "
              "and the particles render with the full PBR material — with 'Tint "
              "With Particle Color' on, the per-particle color multiplies the "
              "material's base color and the particle age-fade drives alpha, so "
              "ParticleAttribute ramps still show through.\n\n"
              "Without a material, particles use the default per-particle color "
              "shader with this node's Emission Strength / Roughness / Metallic.\n\n"
              "Emission Strength works in BOTH modes: 0 = scene-lit only (the "
              "node's neutral default), higher = self-emissive glow; with a "
              "material connected it overrides the material's emission. Note: a "
              "chain WITHOUT this node keeps the historic always-glowing look "
              "(emission 1.0).").toStdString();
}

void
ParticleMaterial::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ParticleMaterial::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ParticleMaterial::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ParticleMaterial::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr emis = AppManager::createKnob<KnobDouble>(this, tr("Emission Strength"));
    emis->setName("emissionStrength");
    emis->setHintToolTip(tr("Particle self-emission. 0 = particles are lit only by the "
                            "scene. Higher values glow in the per-particle color.\n\n"
                            "With a material connected, any value above 0 overrides the "
                            "material's own emission (glowing in the per-particle color "
                            "when Tint is on, else in the material base color); at 0 the "
                            "material's emission settings apply untouched."));
    emis->setAnimationEnabled(true);
    emis->setDefaultValue(0.0);
    emis->setMinimum(0.0);
    emis->setDisplayMinimum(0.0);
    emis->setDisplayMaximum(10.0);
    page->addKnob(emis);
    _imp->emissionStrength = emis;

    KnobDoublePtr rough = AppManager::createKnob<KnobDouble>(this, tr("Roughness"));
    rough->setName("roughness");
    rough->setHintToolTip(tr("Roughness of the default per-particle color shader (used "
                             "when no material is connected)."));
    rough->setAnimationEnabled(true);
    rough->setDefaultValue(0.5);
    rough->setMinimum(0.0);
    rough->setMaximum(1.0);
    page->addKnob(rough);
    _imp->roughness = rough;

    KnobDoublePtr metal = AppManager::createKnob<KnobDouble>(this, tr("Metallic"));
    metal->setName("metallic");
    metal->setHintToolTip(tr("Metallic of the default per-particle color shader (used "
                             "when no material is connected)."));
    metal->setAnimationEnabled(true);
    metal->setDefaultValue(0.0);
    metal->setMinimum(0.0);
    metal->setMaximum(1.0);
    page->addKnob(metal);
    _imp->metallic = metal;

    KnobBoolPtr tint = AppManager::createKnob<KnobBool>(this, tr("Tint With Particle Color"));
    tint->setName("tintWithParticleColor");
    tint->setHintToolTip(tr("With a material connected: multiply the per-particle color "
                            "into the material's base color and drive alpha with the "
                            "particle age fade, so emitter colors and ParticleAttribute "
                            "ramps still show through the material. Off = the material "
                            "renders uniformly on every particle."));
    tint->setAnimationEnabled(false);
    tint->setDefaultValue(true);
    page->addKnob(tint);
    _imp->tintWithColor = tint;
}

ParticleDataPtr
ParticleMaterial::getParticleData(double time)
{
    EffectInstancePtr input = skipDots(getInput(0));
    if (!input) return ParticleDataPtr();
    ParticleProvider* provider = dynamic_cast<ParticleProvider*>(input.get());
    if (!provider) return ParticleDataPtr();
    return provider->getParticleData(time);
}

MaterialProvider*
ParticleMaterial::getParticleMaterialProvider() const
{
    EffectInstancePtr matInput = skipDots(const_cast<ParticleMaterial*>(this)->getInput(1));
    if (!matInput) return nullptr;
    return dynamic_cast<MaterialProvider*>(matInput.get());
}

double
ParticleMaterial::getEmissionStrength(double time) const
{
    KnobDoublePtr k = _imp->emissionStrength.lock();
    return k ? k->getValueAtTime(time) : 0.0;
}

double
ParticleMaterial::getRoughness(double time) const
{
    KnobDoublePtr k = _imp->roughness.lock();
    return k ? k->getValueAtTime(time) : 0.5;
}

double
ParticleMaterial::getMetallic(double time) const
{
    KnobDoublePtr k = _imp->metallic.lock();
    return k ? k->getValueAtTime(time) : 0.0;
}

bool
ParticleMaterial::getTintWithParticleColor() const
{
    KnobBoolPtr k = _imp->tintWithColor.lock();
    return k ? k->getValue() : true;
}

StatusEnum
ParticleMaterial::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                        ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

StatusEnum
ParticleMaterial::render(const RenderActionArgs& args)
{
    // Dummy 2D output — consumers use getParticleData / the material getters.
    if (!args.outputPlanes.empty()) {
        ImagePtr outImg = args.outputPlanes.front().second;
        if (outImg) outImg->fillZero(args.roi);
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleMaterial.cpp"
