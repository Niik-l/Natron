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

#include "Volume3D.h"

#include <cmath>
#include <algorithm>
#include <vector>

#include <QtConcurrentMap> // QtCore on Qt4, QtConcurrent on Qt5+

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct Volume3DPrivate
{
    // Transform
    KnobDoubleWPtr centerX, centerY, centerZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;

    // Volume
    KnobChoiceWPtr preset;     // shape preset loader (resets to Custom after applying)
    KnobChoiceWPtr volumeType; // 0=sphere, 1=box (base shape)
    KnobIntWPtr resolution;
    KnobDoubleWPtr density;
    KnobDoubleWPtr baseFlatness;  // flat cloud base (cut at bottom)
    KnobDoubleWPtr topFlatness;   // flat cloud top (cut at top)
    KnobDoubleWPtr heightFalloff; // vertical density gradient
    KnobBoolWPtr enableNoise;   // layer procedural noise on the base shape
    KnobSeparatorWPtr noiseSep; // "Noise" section header
    KnobDoubleWPtr noiseScale;
    KnobDoubleWPtr noiseDetail;
    KnobDoubleWPtr warp;        // domain-warp strength (billowing)
    KnobDoubleWPtr coverage;    // cloud amount (threshold remap)
    KnobDoubleWPtr erosion;     // edge erosion (wispy rim)
    KnobDoubleWPtr edgeDetail;  // fine edge breakup (high-freq fragmenting carve)
    KnobIntWPtr    seed;        // noise variation seed
    KnobDoubleWPtr wind;        // 3-vector drift (noise cells / frame)
    KnobDoubleWPtr stepSize;
    KnobIntWPtr volumeBounces;

    // Color
    KnobDoubleWPtr colorR, colorG, colorB;
};


Volume3D::Volume3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Volume3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
    _cachedVolRes = 0;
    _cachedVolTime = -1;
}

Volume3D::~Volume3D()
{
}

std::string
Volume3D::getPluginDescription() const
{
    return tr("Procedural 3D volume for volumetric rendering.\n\n"
              "Generates a 3D density field (sphere, noise cloud, box). The Noise "
              "Cloud type supports domain Warp, Coverage and Edge Erosion for "
              "natural cloud shapes.\n\n"
              "Primary renderer: connect to a FastVolumeRender (real-time GPU). "
              "Also works with ScanlineRender/Cycles. Wire into the renderer's "
              "scene/obj input (with lights via a Group3D).").toStdString();
}

void
Volume3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Volume3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Volume3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Volume3D::initializeKnobs()
{
    // Transform
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
        k->setName("translateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->centerX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
        k->setName("translateY"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->centerY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
        k->setName("translateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->centerZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate X"));
        k->setName("rotateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y"));
        k->setName("rotateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z"));
        k->setName("rotateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale X"));
        k->setName("scaleX"); k->setDefaultValue(2.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Y"));
        k->setName("scaleY"); k->setDefaultValue(2.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Z"));
        k->setName("scaleZ"); k->setDefaultValue(2.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleZ = k;
    }

    // Volume
    KnobPagePtr volPage = AppManager::createKnob<KnobPage>(this, tr("Volume"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Preset"));
        k->setName("preset");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("custom", "Custom", "No change — tweak the knobs yourself"));
        entries.push_back(ChoiceOption("cumulus", "Cumulus", "Puffy fair-weather cloud, flat base"));
        entries.push_back(ChoiceOption("stratus", "Stratus", "Flat, wide, layered slab"));
        entries.push_back(ChoiceOption("cumulonimbus", "Cumulonimbus", "Tall storm cloud with a flat anvil top"));
        entries.push_back(ChoiceOption("wispy", "Wispy", "Thin, scattered, eroded wisps (cirrus-like)"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Loads a starting-point combination of the shape, noise and "
                             "vertical-profile settings (and proportions). The menu keeps "
                             "showing the chosen preset; just fine-tune the knobs from "
                             "there (to re-load, pick Custom then the preset again). Does "
                             "NOT change position or rotation."));
        volPage->addKnob(k); _imp->preset = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Type"));
        k->setName("volumeType");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("sphere", "Sphere", "Spherical density falloff"));
        entries.push_back(ChoiceOption("box", "Box", "Uniform density box"));
        k->populateChoices(entries);
        k->setDefaultValue(0); // sphere by default
        k->setHintToolTip(tr("Base shape. Enable Noise (below) layers procedural cloud "
                             "noise on top of either shape: Sphere + noise = a puffy "
                             "cloud, Box + noise = a noisy cloud-filled box."));
        volPage->addKnob(k); _imp->volumeType = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Resolution"));
        k->setName("resolution"); k->setDefaultValue(64);
        k->setMinimum(8); k->setDisplayMinimum(16); k->setDisplayMaximum(128);
        volPage->addKnob(k); _imp->resolution = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Density"));
        k->setName("density"); k->setDefaultValue(8.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(50.0);
        volPage->addKnob(k); _imp->density = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Base Flatness"));
        k->setName("baseFlatness"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Cuts a flat bottom into the volume (the condensation base "
                             "of a real cumulus). 0 = natural rounded shape; higher values "
                             "raise a flat cut-off plane, removing the lower part of the "
                             "cloud. Works on Sphere and Box, with or without noise."));
        volPage->addKnob(k); _imp->baseFlatness = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Top Flatness"));
        k->setName("topFlatness"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Cuts a flat top into the volume — for a flat-topped stratus "
                             "or the spreading anvil of a storm cloud. 0 = natural rounded "
                             "top; higher values lower a flat cut-off plane, removing the "
                             "upper part of the cloud. Combine with Base Flatness for a "
                             "slab."));
        volPage->addKnob(k); _imp->topFlatness = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Height Falloff"));
        k->setName("heightFalloff"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(4.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Vertical density gradient: density stays full at the base "
                             "and feathers out toward the top. 0 = uniform; 1 = fully "
                             "wispy right at the top; above 1 the fade reaches zero lower "
                             "down, squashing the cloud toward its base (up to 4)."));
        volPage->addKnob(k); _imp->heightFalloff = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Enable Noise"));
        k->setName("enableNoise"); k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Layer procedural cloud noise (domain-warped FBM) onto the "
                             "base shape. Off = a clean Sphere / Box. The Noise settings "
                             "below only apply when this is on."));
        k->setAddNewLine(true);
        volPage->addKnob(k); _imp->enableNoise = k;
    }
    {
        KnobSeparatorPtr k = AppManager::createKnob<KnobSeparator>(this, tr("Noise"));
        k->setName("noiseSep");
        volPage->addKnob(k); _imp->noiseSep = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Noise Scale"));
        k->setName("noiseScale"); k->setDefaultValue(3.0); k->setAnimationEnabled(true);
        k->setMinimum(0.1); k->setDisplayMinimum(0.1); k->setDisplayMaximum(20.0);
        volPage->addKnob(k); _imp->noiseScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Noise Detail"));
        k->setName("noiseDetail"); k->setDefaultValue(4.0); k->setAnimationEnabled(true);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(8.0);
        volPage->addKnob(k); _imp->noiseDetail = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Warp"));
        k->setName("warp"); k->setDefaultValue(0.3); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Domain-warp strength — displaces the noise lookup to "
                             "create billowing, cauliflower-like cloud shapes. "
                             "0 = no warp."));
        volPage->addKnob(k); _imp->warp = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Coverage"));
        k->setName("coverage"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("How much of the volume is filled with cloud. 1 = full "
                             "(default); lower values raise the density floor so only the "
                             "densest cores survive, breaking the cloud into scattered "
                             "puffs. Carving empty space also makes the FastVolumeRender "
                             "brick upload sparser/faster."));
        volPage->addKnob(k); _imp->coverage = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Edge Erosion"));
        k->setName("erosion"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Dissolves the thin/low-density rim of the cloud with a "
                             "high-frequency noise, breaking hard edges into wisps while "
                             "leaving the dense core intact. 0 = off (default). For finer, "
                             "more fragmented edges use Edge Detail below as well."));
        volPage->addKnob(k); _imp->erosion = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Edge Detail"));
        k->setName("edgeDetail"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Fine edge breakup: a much higher-frequency, high-contrast "
                             "noise that punches small gaps and wisps INTO the rim "
                             "(fragments it), where Edge Erosion only thins it. Use both "
                             "together for detailed, ragged cloud edges. 0 = off (default). "
                             "Needs enough Resolution to resolve the fine detail."));
        volPage->addKnob(k); _imp->edgeDetail = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Seed"));
        k->setName("seed"); k->setDefaultValue(0);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(100);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Varies the noise pattern — give copies of this node "
                             "different seeds to get different-looking clouds from the "
                             "same settings."));
        volPage->addKnob(k); _imp->seed = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Wind"), 3);
        k->setName("wind");
        k->setDefaultValue(0.0, 0); k->setDefaultValue(0.0, 1); k->setDefaultValue(0.0, 2);
        k->setAnimationEnabled(true);
        k->setDisplayMinimum(-0.5, 0); k->setDisplayMaximum(0.5, 0);
        k->setDisplayMinimum(-0.5, 1); k->setDisplayMaximum(0.5, 1);
        k->setDisplayMinimum(-0.5, 2); k->setDisplayMaximum(0.5, 2);
        k->setHintToolTip(tr("Drifts/evolves the cloud over time: the noise is scrolled "
                             "by Wind × current frame (units = noise cells per frame). "
                             "The cloud's shape stays put while its internal detail boils "
                             "and drifts. 0 = static. Non-zero re-generates the field each "
                             "frame (and re-uploads to FastVolumeRender)."));
        volPage->addKnob(k); _imp->wind = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Step Size"));
        k->setName("stepSize"); k->setDefaultValue(0.1);
        k->setMinimum(0.001); k->setDisplayMinimum(0.01); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Ray marching step size. Smaller = smoother/slower. 0 = auto.\n"
                             "Legacy ScanlineRender/Cycles only — FastVolumeRender uses its "
                             "own Steps knob and ignores this."));
        volPage->addKnob(k); _imp->stepSize = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Volume Bounces"));
        k->setName("volumeBounces"); k->setDefaultValue(2);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(8);
        k->setHintToolTip(tr("Max light bounces inside volume. 0 = single scatter only.\n"
                             "Legacy ScanlineRender/Cycles only — FastVolumeRender is a "
                             "single-scatter model and ignores this."));
        volPage->addKnob(k); _imp->volumeBounces = k;
    }

    // Color
    KnobPagePtr colorPage = AppManager::createKnob<KnobPage>(this, tr("Color"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color R"));
        k->setName("colorR"); k->setDefaultValue(0.8); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color G"));
        k->setName("colorG"); k->setDefaultValue(0.8); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color B"));
        k->setName("colorB"); k->setDefaultValue(0.9); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorB = k;
    }

    updateKnobVisibility();   // hide the noise knobs until Enable Noise is ticked
}

void
Volume3D::updateKnobVisibility()
{
    KnobBoolPtr en = _imp->enableNoise.lock();
    const bool noise = en ? en->getValue() : false;
    if (KnobSeparatorPtr k = _imp->noiseSep.lock()) k->setSecret(!noise);
    if (KnobDoublePtr k = _imp->noiseScale.lock())  k->setSecret(!noise);
    if (KnobDoublePtr k = _imp->noiseDetail.lock()) k->setSecret(!noise);
    if (KnobDoublePtr k = _imp->warp.lock())        k->setSecret(!noise);
    if (KnobDoublePtr k = _imp->coverage.lock())    k->setSecret(!noise);
    if (KnobDoublePtr k = _imp->erosion.lock())     k->setSecret(!noise);
    if (KnobDoublePtr k = _imp->edgeDetail.lock())  k->setSecret(!noise);
    if (KnobIntPtr    k = _imp->seed.lock())         k->setSecret(!noise);
    if (KnobDoublePtr k = _imp->wind.lock())         k->setSecret(!noise);
}

void
Volume3D::applyPreset(int idx)
{
    // Shape/noise/profile + proportions for each preset (position & rotation are
    // left untouched). Order: vtype, scaleX/Y/Z, density, noiseScale, noiseDetail,
    // warp, coverage, erosion, edgeDetail, baseFlatness, topFlatness, heightFalloff.
    struct P {
        int vtype; double sx, sy, sz, density, nscale, ndetail, warp, cov, ero,
            edge, baseF, topF, hfall;
    };
    static const P presets[] = {
        // Cumulus — puffy fair-weather cloud, flat base, slightly wispy top
        { 0, 2.2, 1.7, 2.2,  8.0, 3.0, 5.0, 0.40, 0.85, 0.30, 0.35, 0.55, 0.00, 0.40 },
        // Stratus — flat, wide, fairly solid layered slab
        { 0, 3.5, 0.65, 3.5, 9.0, 2.5, 4.0, 0.20, 0.80, 0.25, 0.25, 0.50, 0.50, 0.00 },
        // Cumulonimbus — tall storm cloud with a flattened anvil top
        { 0, 1.9, 2.6, 1.9, 10.0, 3.0, 6.0, 0.50, 0.90, 0.25, 0.40, 0.40, 0.30, 0.30 },
        // Wispy — thin, scattered, eroded wisps (cirrus-like) but still visible
        { 0, 3.0, 0.9, 3.0,  7.0, 4.0, 6.0, 0.60, 0.58, 0.55, 0.70, 0.00, 0.00, 0.45 },
    };
    if (idx < 1 || idx > (int)(sizeof(presets) / sizeof(presets[0]))) return;
    const P& p = presets[idx - 1];

    if (KnobChoicePtr k = _imp->volumeType.lock())   k->setValue(p.vtype);
    if (KnobBoolPtr   k = _imp->enableNoise.lock())  k->setValue(true);
    if (KnobDoublePtr k = _imp->scaleX.lock())       k->setValue(p.sx);
    if (KnobDoublePtr k = _imp->scaleY.lock())       k->setValue(p.sy);
    if (KnobDoublePtr k = _imp->scaleZ.lock())       k->setValue(p.sz);
    if (KnobDoublePtr k = _imp->density.lock())      k->setValue(p.density);
    if (KnobDoublePtr k = _imp->noiseScale.lock())   k->setValue(p.nscale);
    if (KnobDoublePtr k = _imp->noiseDetail.lock())  k->setValue(p.ndetail);
    if (KnobDoublePtr k = _imp->warp.lock())         k->setValue(p.warp);
    if (KnobDoublePtr k = _imp->coverage.lock())     k->setValue(p.cov);
    if (KnobDoublePtr k = _imp->erosion.lock())      k->setValue(p.ero);
    if (KnobDoublePtr k = _imp->edgeDetail.lock())   k->setValue(p.edge);
    if (KnobDoublePtr k = _imp->baseFlatness.lock()) k->setValue(p.baseF);
    if (KnobDoublePtr k = _imp->topFlatness.lock())  k->setValue(p.topF);
    if (KnobDoublePtr k = _imp->heightFalloff.lock())k->setValue(p.hfall);
}

bool
Volume3D::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/,
                      double /*time*/, bool /*originatedFromMainThread*/)
{
    KnobBoolPtr en = _imp->enableNoise.lock();
    if (en && k == en.get()) {
        updateKnobVisibility();
        return true;
    }
    KnobChoicePtr pr = _imp->preset.lock();
    if (pr && k == pr.get()) {
        int idx = pr->getValue();
        if (idx > 0) {
            applyPreset(idx);         // leave the dropdown showing the chosen preset name
            updateKnobVisibility();   // preset enables noise -> reveal the noise knobs
        }
        return true;
    }
    return false;
}

Volume3D::VolumeParams
Volume3D::getVolumeParams(double time) const
{
    VolumeParams vp;
    vp.centerX = (float)_imp->centerX.lock()->getValueAtTime(time);
    vp.centerY = (float)_imp->centerY.lock()->getValueAtTime(time);
    vp.centerZ = (float)_imp->centerZ.lock()->getValueAtTime(time);
    vp.scaleX = (float)_imp->scaleX.lock()->getValueAtTime(time);
    vp.scaleY = (float)_imp->scaleY.lock()->getValueAtTime(time);
    vp.scaleZ = (float)_imp->scaleZ.lock()->getValueAtTime(time);
    vp.density = (float)_imp->density.lock()->getValueAtTime(time);
    vp.colorR = (float)_imp->colorR.lock()->getValueAtTime(time);
    vp.colorG = (float)_imp->colorG.lock()->getValueAtTime(time);
    vp.colorB = (float)_imp->colorB.lock()->getValueAtTime(time);
    vp.resolution = _imp->resolution.lock()->getValueAtTime(time);
    vp.volumeType = _imp->volumeType.lock()->getValueAtTime(time);
    vp.baseFlatness = (float)_imp->baseFlatness.lock()->getValueAtTime(time);
    vp.topFlatness = (float)_imp->topFlatness.lock()->getValueAtTime(time);
    vp.heightFalloff = (float)_imp->heightFalloff.lock()->getValueAtTime(time);
    vp.enableNoise = _imp->enableNoise.lock()->getValueAtTime(time);
    vp.noiseScale = (float)_imp->noiseScale.lock()->getValueAtTime(time);
    vp.noiseDetail = (float)_imp->noiseDetail.lock()->getValueAtTime(time);
    vp.warp = (float)_imp->warp.lock()->getValueAtTime(time);
    vp.coverage = (float)_imp->coverage.lock()->getValueAtTime(time);
    vp.erosion = (float)_imp->erosion.lock()->getValueAtTime(time);
    vp.edgeDetail = (float)_imp->edgeDetail.lock()->getValueAtTime(time);
    vp.seed = _imp->seed.lock()->getValueAtTime(time);
    {
        KnobDoublePtr w = _imp->wind.lock();
        vp.windX = (float)w->getValueAtTime(time, 0);
        vp.windY = (float)w->getValueAtTime(time, 1);
        vp.windZ = (float)w->getValueAtTime(time, 2);
    }
    // Combined noise-sampling offset: the Seed spreads copies far apart in noise
    // space, and Wind scrolls the field by wind*frame so the cloud drifts/evolves.
    {
        const float sx = vp.seed * 53.17f, sy = vp.seed * 91.71f, sz = vp.seed * 28.39f;
        vp.noiseOffX = sx + vp.windX * (float)time;
        vp.noiseOffY = sy + vp.windY * (float)time;
        vp.noiseOffZ = sz + vp.windZ * (float)time;
    }
    vp.stepSize = (float)_imp->stepSize.lock()->getValueAtTime(time);
    vp.volumeBounces = _imp->volumeBounces.lock()->getValueAtTime(time);
    return vp;
}

// Simple 3D noise function
static float hash3D(float x, float y, float z)
{
    float n = sinf(x * 127.1f + y * 311.7f + z * 74.7f) * 43758.5453f;
    return n - floorf(n);
}

static float smoothNoise3D(float x, float y, float z)
{
    float ix = floorf(x), iy = floorf(y), iz = floorf(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;

    // Smoothstep
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    fz = fz * fz * (3.0f - 2.0f * fz);

    // Trilinear interpolation of hash values
    float v000 = hash3D(ix, iy, iz);
    float v100 = hash3D(ix + 1, iy, iz);
    float v010 = hash3D(ix, iy + 1, iz);
    float v110 = hash3D(ix + 1, iy + 1, iz);
    float v001 = hash3D(ix, iy, iz + 1);
    float v101 = hash3D(ix + 1, iy, iz + 1);
    float v011 = hash3D(ix, iy + 1, iz + 1);
    float v111 = hash3D(ix + 1, iy + 1, iz + 1);

    float v00 = v000 + fx * (v100 - v000);
    float v10 = v010 + fx * (v110 - v010);
    float v01 = v001 + fx * (v101 - v001);
    float v11 = v011 + fx * (v111 - v011);

    float v0 = v00 + fy * (v10 - v00);
    float v1 = v01 + fy * (v11 - v01);

    return v0 + fz * (v1 - v0);
}

static float fbm3D(float x, float y, float z, int octaves)
{
    float value = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        value += amplitude * smoothNoise3D(x * frequency, y * frequency, z * frequency);
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return value;
}

static float smoothstepf(float e0, float e1, float x)
{
    float t = (x - e0) / std::max(1e-6f, e1 - e0);
    t = std::min(1.0f, std::max(0.0f, t));
    return t * t * (3.0f - 2.0f * t);
}

// Vertical (local-Y) density profile: a flat condensation base + a top feather,
// for a cumulus-like silhouette. ly in [-1,1] (bottom..top); returns a multiplier
// in [0,1]. Both effects are neutral (multiplier 1) at their default of 0.
static float
verticalProfileMul(float ly, const Volume3D::VolumeParams& vp)
{
    float m = 1.0f;
    const float t = (ly + 1.0f) * 0.5f;   // 0 at bottom, 1 at top
    // Flat base: raise a soft cut-off plane (removes the lower part of the cloud).
    if (vp.baseFlatness > 1e-4f) {
        const float base = vp.baseFlatness * 0.5f;   // up to mid-height
        m *= smoothstepf(base - 0.04f, base + 0.04f, t);
    }
    // Flat top: lower a soft cut-off plane (removes the upper part — stratus/anvil).
    if (vp.topFlatness > 1e-4f) {
        const float top = 1.0f - vp.topFlatness * 0.5f;  // down to mid-height
        m *= 1.0f - smoothstepf(top - 0.04f, top + 0.04f, t);
    }
    // Height falloff: feather density from full at the base to wispy at the top.
    if (vp.heightFalloff > 1e-4f) {
        m *= std::max(0.0f, 1.0f - vp.heightFalloff * t);
    }
    return m;
}

// Density at one local coordinate in [-1,1]^3. Pure (no shared state) so the
// generation loop can run in parallel across the thread pool. Density scale
// (vp.density) is NOT applied here — the renderer multiplies it in.
static float
volumeDensityAt(const Volume3D::VolumeParams& vp, float lx, float ly, float lz)
{
    // Base shape mask: sphere = soft radial falloff, box = hard interior.
    float shape;
    if (vp.volumeType == 0) {
        float dist = sqrtf(lx * lx + ly * ly + lz * lz);
        shape = std::max(0.0f, 1.0f - dist);
    } else { // box
        shape = (fabsf(lx) < 0.8f && fabsf(ly) < 0.8f && fabsf(lz) < 0.8f) ? 1.0f : 0.0f;
    }

    // Vertical profile (flat base + top feather) multiplies the final density;
    // applies to clean shapes and noise clouds alike.
    const float vprof = verticalProfileMul(ly, vp);

    // Without noise, the base shape IS the density (clean sphere / box).
    if (!vp.enableNoise || shape <= 0.0f) {
        return shape * vprof;
    }

    // Noise modifier: domain-warped, layered FBM that modulates the base shape,
    // then carved by Coverage and dissolved at the edges by Erosion. Sphere +
    // noise = a puffy cloud (the old "Noise Cloud"); Box + noise = a cloud-box.
    // Seed + Wind translate the whole noise field (added to every sample coord).
    const float ox = vp.noiseOffX, oy = vp.noiseOffY, oz = vp.noiseOffZ;

    const float ws = vp.warp;
    float wx = smoothNoise3D(lx * 1.7f + 19.1f + ox, ly * 1.7f + 7.3f + oy,  lz * 1.7f + 3.9f + oz)  - 0.5f;
    float wy = smoothNoise3D(lx * 1.7f + 5.2f + ox,  ly * 1.7f + 23.6f + oy, lz * 1.7f + 11.4f + oz) - 0.5f;
    float wz = smoothNoise3D(lx * 1.7f + 31.8f + ox, ly * 1.7f + 2.1f + oy,  lz * 1.7f + 17.7f + oz) - 0.5f;
    float px = lx + ws * 2.0f * wx;
    float py = ly + ws * 2.0f * wy;
    float pz = lz + ws * 2.0f * wz;

    int oct = std::max(2, (int)vp.noiseDetail);
    float base = fbm3D(px * vp.noiseScale + ox, py * vp.noiseScale + oy, pz * vp.noiseScale + oz, oct);
    float detail = fbm3D(px * vp.noiseScale * 3.1f + 41.0f + ox,
                         py * vp.noiseScale * 3.1f + 41.0f + oy,
                         pz * vp.noiseScale * 3.1f + 41.0f + oz, 3) - 0.5f;
    float n = base + 0.3f * detail;     // raw noise signal, mean ~0.5

    // Coverage and erosion act on the NOISE signal (mean ~0.5, even response)
    // BEFORE the spherical falloff, so the knobs aren't squished by the falloff.

    // Coverage: 1.0 = full cloud (no carving, the default); lower values raise
    // the threshold so only the densest noise survives → scattered puffs.
    float cov = vp.coverage;
    if (cov < 0.999f) {
        n = (n - (1.0f - cov)) / std::max(1e-3f, cov);
    }
    n = std::max(0.0f, n);

    // Edge erosion: a high-frequency noise eats into the thin/low-density rim of
    // the noise, breaking hard edges into wisps while leaving dense cores intact.
    if (vp.erosion > 1e-4f && n > 0.0f) {
        float en = fbm3D(px * vp.noiseScale * 4.7f + 71.3f + ox,
                         py * vp.noiseScale * 4.7f + 17.9f + oy,
                         pz * vp.noiseScale * 4.7f + 53.1f + oz, 3);
        float edgeMask = 1.0f - smoothstepf(0.15f, 0.85f, n); // 1 at rim/mantle, 0 in dense core
        n -= vp.erosion * en * edgeMask;
        n = std::max(0.0f, n);
    }

    // Edge detail: a MUCH finer, high-contrast noise that MULTIPLICATIVELY punches
    // gaps into the rim (fragments it into ragged wisps), rather than just thinning
    // it like erosion. The high contrast (steep smoothstep) makes it break up into
    // discrete tendrils instead of a smooth fade.
    if (vp.edgeDetail > 1e-4f && n > 0.0f) {
        const float ef = vp.noiseScale * 11.0f;   // ~3.5x finer than the erosion band
        float fd = fbm3D(px * ef + 130.0f + ox,
                         py * ef + 70.0f + oy,
                         pz * ef + 200.0f + oz, 2);
        fd = smoothstepf(0.40f, 0.62f, fd);        // high contrast -> discrete gaps
        float edgeMask = 1.0f - smoothstepf(0.10f, 0.80f, n); // 1 at rim, 0 in core
        n *= 1.0f - vp.edgeDetail * edgeMask * (1.0f - fd);   // carve holes at the rim
        n = std::max(0.0f, n);
    }

    return shape * n * 2.0f * vprof;
}

U64
Volume3D::getShapeHash(double time) const
{
    // Hash of the SHAPE params (placement/colour excluded — those don't change
    // the generated density field). Param values are evaluated at `time`, so an
    // animated param changes the hash. Used by downstream renderers to decide
    // when to regenerate/re-upload.
    VolumeParams vp = getVolumeParams(time);
    auto hc = [](U64 s, U64 v) -> U64 { return s ^ (v * 0x9e3779b97f4a7c15ULL + (s << 6) + (s >> 2)); };
    union { double d; U64 u; } conv;
    U64 h = 0;
    h = hc(h, (U64)vp.volumeType);
    h = hc(h, (U64)(vp.enableNoise ? 1 : 0));
    h = hc(h, (U64)vp.resolution);
    conv.d = vp.density;     h = hc(h, conv.u);
    conv.d = vp.baseFlatness;  h = hc(h, conv.u);
    conv.d = vp.topFlatness;   h = hc(h, conv.u);
    conv.d = vp.heightFalloff; h = hc(h, conv.u);
    conv.d = vp.noiseScale;  h = hc(h, conv.u);
    conv.d = vp.noiseDetail; h = hc(h, conv.u);
    conv.d = vp.warp;        h = hc(h, conv.u);
    conv.d = vp.coverage;    h = hc(h, conv.u);
    conv.d = vp.erosion;     h = hc(h, conv.u);
    conv.d = vp.edgeDetail;  h = hc(h, conv.u);
    conv.d = vp.noiseOffX;   h = hc(h, conv.u);  // folds in seed + wind*time
    conv.d = vp.noiseOffY;   h = hc(h, conv.u);
    conv.d = vp.noiseOffZ;   h = hc(h, conv.u);
    return h;
}

void
Volume3D::generateVolumeData(double time, std::vector<float>& outData, int& res) const
{
    VolumeParams vp = getVolumeParams(time);

    // Cache key includes all params, not just time
    U64 paramHash = 0;
    {
        auto hc = [](U64 s, U64 v) -> U64 { return s ^ (v * 0x9e3779b97f4a7c15ULL + (s << 6) + (s >> 2)); };
        union { double d; U64 u; } conv;
        conv.d = time; paramHash = hc(paramHash, conv.u);
        paramHash = hc(paramHash, (U64)vp.volumeType);
        paramHash = hc(paramHash, (U64)(vp.enableNoise ? 1 : 0));
        paramHash = hc(paramHash, (U64)vp.resolution);
        conv.d = vp.density; paramHash = hc(paramHash, conv.u);
        conv.d = vp.baseFlatness; paramHash = hc(paramHash, conv.u);
        conv.d = vp.topFlatness; paramHash = hc(paramHash, conv.u);
        conv.d = vp.heightFalloff; paramHash = hc(paramHash, conv.u);
        conv.d = vp.noiseScale; paramHash = hc(paramHash, conv.u);
        conv.d = vp.noiseDetail; paramHash = hc(paramHash, conv.u);
        conv.d = vp.warp; paramHash = hc(paramHash, conv.u);
        conv.d = vp.coverage; paramHash = hc(paramHash, conv.u);
        conv.d = vp.erosion; paramHash = hc(paramHash, conv.u);
        conv.d = vp.edgeDetail; paramHash = hc(paramHash, conv.u);
        conv.d = vp.noiseOffX; paramHash = hc(paramHash, conv.u);
        conv.d = vp.noiseOffY; paramHash = hc(paramHash, conv.u);
        conv.d = vp.noiseOffZ; paramHash = hc(paramHash, conv.u);
    }
    // Cache accesses are guarded: GUI paint (Viewport3D) and render workers
    // (ScanlineRender / FastVolumeRender) call this concurrently, and copying
    // out of _cachedVolData while another thread reassigns it is a race.
    // The compute below runs unlocked (fills a local buffer).
    {
        std::lock_guard<std::mutex> lk(_volCacheMutex);
        if (paramHash == _cachedVolHash && _cachedVolRes > 0 && !_cachedVolData.empty()) {
            outData = _cachedVolData;
            res = _cachedVolRes;
            return;
        }
    }
    res = std::max(8, std::min(128, vp.resolution));

    outData.resize(res * res * res, 0.0f);

    const float invRes = 1.0f / (float)res;
    const int R = res;
    const VolumeParams vpc = vp;     // copied so the parallel functor stays pure
    float* out = outData.data();

    // Fill one z-slice (disjoint output region → safe to run in parallel).
    auto fillSlice = [out, R, invRes, vpc](int z) {
        const float w = ((float)z + 0.5f) * invRes;
        const float lz = w * 2.0f - 1.0f;
        for (int y = 0; y < R; ++y) {
            const float v = ((float)y + 0.5f) * invRes;
            const float ly = v * 2.0f - 1.0f;
            float* row = out + (z * R + y) * R;
            for (int x = 0; x < R; ++x) {
                const float u = ((float)x + 0.5f) * invRes;
                const float lx = u * 2.0f - 1.0f;
                row[x] = std::max(0.0f, volumeDensityAt(vpc, lx, ly, lz));
            }
        }
    };

    // Parallelize across the engine thread pool (cold-upload / animated-cloud
    // cost is the bottleneck for FastVolumeRender re-uploads). Small volumes
    // run serially to avoid dispatch overhead.
    if (R >= 24) {
        std::vector<int> zs(R);
        for (int z = 0; z < R; ++z) {
            zs[z] = z;
        }
        QtConcurrent::blockingMap(zs, fillSlice);
    } else {
        for (int z = 0; z < R; ++z) {
            fillSlice(z);
        }
    }

    // Cache
    {
        std::lock_guard<std::mutex> lk(_volCacheMutex);
        _cachedVolData = outData;
        _cachedVolRes = res;
        _cachedVolTime = time;
        _cachedVolHash = paramHash;
    }
}

StatusEnum
Volume3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Volume3D::render(const RenderActionArgs& args)
{
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

#include "moc_Volume3D.cpp"
