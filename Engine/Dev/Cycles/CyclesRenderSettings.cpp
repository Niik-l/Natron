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

#include "CyclesRenderSettings.h"

#include "../../AppManager.h"
#include "../../AppInstance.h"
#include "../../Project.h"
#include "../../Image.h"
#include "../../KnobTypes.h"
#include "../../OCIOColorSpaceUtils.h"

NATRON_NAMESPACE_ENTER

// Defaults intentionally mirror CyclesRender's own knob defaults so that
// disconnecting a CyclesRender's local knobs and routing through a fresh
// Settings node produces identical output.
struct CyclesRenderSettingsPrivate
{
    KnobIntWPtr    samples;
    KnobBoolWPtr   denoise;

    KnobIntWPtr    maxBounces;
    KnobIntWPtr    diffuseBounces;
    KnobIntWPtr    glossyBounces;
    KnobIntWPtr    transmissionBounces;

    KnobBoolWPtr   dofEnabled;
    KnobDoubleWPtr focusDistance;
    KnobIntWPtr    bokehBlades;
    KnobDoubleWPtr bladeRotation;

    KnobBoolWPtr   motionBlur;
    KnobDoubleWPtr shutterTime;
    KnobChoiceWPtr shutterPosition;

    KnobChoiceWPtr outputColorspace;
    KnobStringWPtr outputPath;
};

CyclesRenderSettings::CyclesRenderSettings(NodePtr node)
    : EffectInstance(node)
    , _imp(new CyclesRenderSettingsPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

CyclesRenderSettings::~CyclesRenderSettings()
{
}

std::string
CyclesRenderSettings::getPluginDescription() const
{
    return tr("Hold shared Cycles render settings for downstream renderers.\n\n"
              "Connect to a CyclesRender or CyclesRenderPassManager via its "
              "'settings' input. The downstream node ignores its own local "
              "Render / Integrator / DOF / Motion Blur knobs and pulls values "
              "from here instead, so a preview and a final render can share "
              "one source of truth.\n\n"
              "Sink node — no inputs, no image output.").toStdString();
}

void
CyclesRenderSettings::addAcceptedComponents(int /*inputNb*/,
                                             std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
CyclesRenderSettings::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
CyclesRenderSettings::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
CyclesRenderSettings::initializeKnobs()
{
    // --- Render tab ---
    KnobPagePtr renderPage = AppManager::createKnob<KnobPage>(this, tr("Render"));
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Samples"));
        k->setName("samples"); k->setDefaultValue(6);
        k->setMinimum(1); k->setMaximum(8192);
        k->setHintToolTip(tr("Path-trace samples per pixel."));
        renderPage->addKnob(k); _imp->samples = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Denoise"));
        k->setName("denoise"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Run Cycles' built-in denoiser on the result."));
        renderPage->addKnob(k); _imp->denoise = k;
    }

    // --- Integrator tab ---
    KnobPagePtr integPage = AppManager::createKnob<KnobPage>(this, tr("Integrator"));
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Bounces"));
        k->setName("maxBounces"); k->setDefaultValue(8);
        k->setMinimum(0); k->setMaximum(128);
        k->setHintToolTip(tr("Total light bounces across all paths."));
        integPage->addKnob(k); _imp->maxBounces = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Diffuse Bounces"));
        k->setName("diffuseBounces"); k->setDefaultValue(4);
        k->setMinimum(0); k->setMaximum(128);
        integPage->addKnob(k); _imp->diffuseBounces = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Glossy Bounces"));
        k->setName("glossyBounces"); k->setDefaultValue(4);
        k->setMinimum(0); k->setMaximum(128);
        integPage->addKnob(k); _imp->glossyBounces = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Transmission Bounces"));
        k->setName("transmissionBounces"); k->setDefaultValue(8);
        k->setMinimum(0); k->setMaximum(128);
        integPage->addKnob(k); _imp->transmissionBounces = k;
    }

    // --- DOF tab ---
    KnobPagePtr dofPage = AppManager::createKnob<KnobPage>(this, tr("Depth of Field"));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Enable DOF"));
        k->setName("dofEnabled"); k->setDefaultValue(false);
        dofPage->addKnob(k); _imp->dofEnabled = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Focus Distance"));
        k->setName("focusDistance"); k->setDefaultValue(10.0);
        k->setMinimum(0.001); k->setDisplayMinimum(0.1); k->setDisplayMaximum(1000.0);
        k->setHintToolTip(tr("World-space distance from camera to focus plane."));
        k->setAnimationEnabled(true);
        dofPage->addKnob(k); _imp->focusDistance = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Bokeh Blades"));
        k->setName("bokehBlades"); k->setDefaultValue(0);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(16);
        k->setHintToolTip(tr("Aperture blade count. 0 = circular (no polygon)."));
        dofPage->addKnob(k); _imp->bokehBlades = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Blade Rotation"));
        k->setName("bladeRotation"); k->setDefaultValue(0.0);
        k->setHintToolTip(tr("Aperture rotation in degrees."));
        k->setAnimationEnabled(true);
        dofPage->addKnob(k); _imp->bladeRotation = k;
    }

    // --- Motion Blur tab ---
    KnobPagePtr mbPage = AppManager::createKnob<KnobPage>(this, tr("Motion Blur"));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Enable Motion Blur"));
        k->setName("motionBlur"); k->setDefaultValue(false);
        mbPage->addKnob(k); _imp->motionBlur = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Shutter Time"));
        k->setName("shutterTime"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Shutter duration in frames. 0.5 = half-frame blur (common). 1.0 = full frame."));
        k->setAnimationEnabled(true);
        mbPage->addKnob(k); _imp->shutterTime = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Shutter Position"));
        k->setName("shutterPosition");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Start", "", "Shutter opens at current frame"));
        entries.push_back(ChoiceOption("Center", "", "Shutter centered on current frame (most common)"));
        entries.push_back(ChoiceOption("End", "", "Shutter closes at current frame"));
        k->populateChoices(entries);
        k->setDefaultValue(1);
        mbPage->addKnob(k); _imp->shutterPosition = k;
    }

    // --- Output tab ---
    KnobPagePtr outPage = AppManager::createKnob<KnobPage>(this, tr("Output"));
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Output Path"));
        k->setName("outputPath");
        std::string defPath;
        if ( getApp() && getApp()->getProject() ) {
            defPath = getApp()->getProject()->getProjectPath().toStdString();
        }
        k->setDefaultValue(defPath);
        k->setHintToolTip(tr(
            "Base directory for disk renders. A connected CyclesRenderPass writes to\n"
            "    <Output Path>/<Pass Name>/v###/<Pass Name>.####.exr\n"
            "so each pass gets its own subfolder with an auto-incrementing version. "
            "Defaults to the project folder; set it to wherever renders should land."));
        outPage->addKnob(k); _imp->outputPath = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Output Colorspace"));
        k->setName("outputColorspace");
        // Populate from the active OCIO config so the names resolve downstream.
        std::vector<ChoiceOption> entries;
        const std::vector<std::string> spaces = getOcioColorSpaceNames();
        int defIdx = 0;
        const std::string sceneLinear = getOcioSceneLinearName();
        if ( !spaces.empty() ) {
            for (std::size_t i = 0; i < spaces.size(); ++i) {
                if (!sceneLinear.empty() && spaces[i] == sceneLinear) {
                    defIdx = (int)i;  // default to scene-linear (ACEScg)
                }
                entries.push_back( ChoiceOption(spaces[i], "", "") );
            }
        } else {
            entries.push_back( ChoiceOption("ACEScg", "", "") );
            entries.push_back( ChoiceOption("sRGB - Display", "", "") );
        }
        k->populateChoices(entries);
        k->setDefaultValue(defIdx);
        k->setHintToolTip(tr(
            "Default colorspace for files written to disk by a connected "
            "CyclesRenderPassManager.\n\n"
            "• EXR is always written scene-linear (ACEScg) and tagged with the "
            "config's scene_linear space — it stays comp-ready; this setting does "
            "not bake a transform into EXRs (use a Write node for EXR delivery in "
            "another space).\n"
            "• PNG / JPG / TIFF (review) are converted from scene-linear to this "
            "space so they display correctly. If this is left on a scene-linear "
            "space, review images auto-promote to the config's display space.\n"
            "• Data passes (depth / normal / position / vector / id / cryptomatte) "
            "are never converted.\n\n"
            "A per-pass JSON \"colorspace\" field overrides this for that pass."));
        outPage->addKnob(k); _imp->outputColorspace = k;
    }
}

// --- CyclesSettingsProvider impl ---
//
// All getters lock-and-read. KnobX::lock() returns null only if the knob was
// never created or already destroyed; we guard but it should never trigger
// in practice (initializeKnobs always runs once). Default values when
// guarding match the knob defaults above.

int    CyclesRenderSettings::getSamples(double /*time*/) const
{
    KnobIntPtr k = _imp->samples.lock();
    return k ? k->getValue() : 6;
}

bool   CyclesRenderSettings::getDenoise(double /*time*/) const
{
    KnobBoolPtr k = _imp->denoise.lock();
    return k ? k->getValue() : false;
}

int    CyclesRenderSettings::getMaxBounces(double /*time*/) const
{
    KnobIntPtr k = _imp->maxBounces.lock();
    return k ? k->getValue() : 8;
}

int    CyclesRenderSettings::getDiffuseBounces(double /*time*/) const
{
    KnobIntPtr k = _imp->diffuseBounces.lock();
    return k ? k->getValue() : 4;
}

int    CyclesRenderSettings::getGlossyBounces(double /*time*/) const
{
    KnobIntPtr k = _imp->glossyBounces.lock();
    return k ? k->getValue() : 4;
}

int    CyclesRenderSettings::getTransmissionBounces(double /*time*/) const
{
    KnobIntPtr k = _imp->transmissionBounces.lock();
    return k ? k->getValue() : 8;
}

bool   CyclesRenderSettings::getDOFEnabled(double /*time*/) const
{
    KnobBoolPtr k = _imp->dofEnabled.lock();
    return k ? k->getValue() : false;
}

double CyclesRenderSettings::getFocusDistance(double time) const
{
    KnobDoublePtr k = _imp->focusDistance.lock();
    return k ? k->getValueAtTime(time) : 10.0;
}

int    CyclesRenderSettings::getBokehBlades(double /*time*/) const
{
    KnobIntPtr k = _imp->bokehBlades.lock();
    return k ? k->getValue() : 0;
}

double CyclesRenderSettings::getBladeRotation(double time) const
{
    KnobDoublePtr k = _imp->bladeRotation.lock();
    return k ? k->getValueAtTime(time) : 0.0;
}

bool   CyclesRenderSettings::getMotionBlurEnabled(double /*time*/) const
{
    KnobBoolPtr k = _imp->motionBlur.lock();
    return k ? k->getValue() : false;
}

double CyclesRenderSettings::getShutterTime(double time) const
{
    KnobDoublePtr k = _imp->shutterTime.lock();
    return k ? k->getValueAtTime(time) : 0.5;
}

int    CyclesRenderSettings::getShutterPosition(double /*time*/) const
{
    KnobChoicePtr k = _imp->shutterPosition.lock();
    return k ? k->getValue() : 1;  // Center
}

std::string CyclesRenderSettings::getOutputColorspace(double /*time*/) const
{
    KnobChoicePtr k = _imp->outputColorspace.lock();
    return k ? k->getActiveEntry().id : std::string();
}

std::string CyclesRenderSettings::getOutputPath(double /*time*/) const
{
    KnobStringPtr k = _imp->outputPath.lock();
    return k ? k->getValue() : std::string();
}

StatusEnum
CyclesRenderSettings::getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod)
{
    // Sink node — no image output. Use a 1x1 stub so the engine doesn't
    // cache an early failure (see feedback_natron_input_optional_required).
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
CyclesRenderSettings::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_CyclesRenderSettings.cpp"
