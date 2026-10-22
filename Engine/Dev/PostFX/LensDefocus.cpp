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

#include "LensDefocus.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

#include "DefocusCore.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"
#include "../DotUtils.h"
#include "../Scene3D/CameraProvider.h"

NATRON_NAMESPACE_ENTER

namespace {

enum InputEnum
{
    eInputSource = 0,
    eInputDepth,
    eInputCamera,
    eInputCleanBg,
    eInputMask,
};

enum DepthSourceEnum
{
    eDepthSourcePlane = 0,  // a plane of the Source input
    eDepthSourceInput,      // channel 0 of the Depth input
};

enum FocusModeEnum
{
    eFocusDistance = 0,
    eFocusPoint,            // autofocus: the depth under Focus Point
};

ImagePlaneDesc
defaultDepthPlane()
{
    std::vector<std::string> channels;
    channels.push_back("Z");
    return ImagePlaneDesc("depth", "depth", "Z", channels);
}

ImagePlaneDesc
errorPlane()
{
    std::vector<std::string> channels;
    channels.push_back("guess");
    channels.push_back("clamped");
    channels.push_back("unused");
    return ImagePlaneDesc("defocusError", "defocusError", "", channels);
}

const double kUnitsToMm[] = {1000., 10., 1., 25.4, 304.8};

} // anonymous namespace

struct LensDefocusPrivate
{
    std::vector<ImagePlaneDesc> cachedLayers;   // planes of input 0, refreshed on input/metadata change

    // Depth
    KnobChoiceWPtr depthSource, depthPlane, depthConvention, sceneUnits;
    KnobDoubleWPtr farThreshold, normalizedNear, normalizedFar;
    KnobBoolWPtr zeroIsInfinity, normalizedInvert, depthPremultiplied;
    // Lens
    KnobChoiceWPtr cocMode, focusMode;
    KnobBoolWPtr useCamera;
    KnobDoubleWPtr focalLength, fStop, filmBack, focusDistance, focusPoint, sizeAtInfinity;
    KnobDoubleWPtr nearGain, farGain, maxRadius;
    // Bokeh
    KnobIntWPtr blades;
    KnobDoubleWPtr curvature, rotation, squeeze, catEye, catEyeStart, swirl, sphericalAberration;
    KnobDoubleWPtr onionAmount, onionFrequency, axialCA;
    // Quality
    KnobDoubleWPtr sliceStep, fullResRadius, highlightThreshold, edgeThreshold;
    KnobIntWPtr maxHighlights;
    KnobChoiceWPtr edgeMode, border;
    KnobBoolWPtr fillBehind;
    // Output
    KnobChoiceWPtr output;
    KnobBoolWPtr outputError;
    KnobDoubleWPtr mix;
};

static ImagePlaneDesc
resolveDepthPlane(const KnobChoiceWPtr& knob, const std::vector<ImagePlaneDesc>& cached)
{
    KnobChoicePtr k = knob.lock();
    if (!k) {
        return defaultDepthPlane();
    }
    const std::string id = k->getActiveEntry().id;
    for (size_t i = 0; i < cached.size(); ++i) {
        if (cached[i].getPlaneID() == id) {
            return cached[i];
        }
    }
    return defaultDepthPlane();
}

LensDefocus::LensDefocus(NodePtr node)
    : EffectInstance(node)
    , _imp(new LensDefocusPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

LensDefocus::~LensDefocus()
{
}

std::string
LensDefocus::getPluginDescription() const
{
    return "Physically based depth of field from an image and its depth pass.\n\n"
           "The blur size comes from a real camera: focal length, f-stop, focus distance and film back, "
           "read from a Camera input or typed in. It is signed, so defocus in front of and behind the focus "
           "plane behaves differently, as with a real lens.\n\n"
           "Each pixel spreads its light over its own bokeh shape and depth slices are composited back to "
           "front, so a sharp foreground has no background halo and a blurred foreground spreads over a sharp "
           "background. Background hidden behind blurred foreground edges is filled in, or taken from the "
           "Clean Background input; the defocusError plane shows where it was guessed.\n\n"
           "Work in scene-linear, unclipped colour so highlights become proper bokeh discs.\n\n"
           "Inputs: Source (RGBA + a depth plane), Depth (optional separate depth image), Camera (optional), "
           "Clean Background (optional), Mask.";
}

std::string
LensDefocus::getInputLabel(int inputNb) const
{
    switch (inputNb) {
    case eInputSource: return "Source";
    case eInputDepth: return "Depth";
    case eInputCamera: return "Camera";
    case eInputCleanBg: return "CleanBg";
    case eInputMask: return "Mask";
    default: return "";
    }
}

void
LensDefocus::addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps)
{
    if (inputNb == eInputMask) {
        comps->push_back( ImagePlaneDesc::getAlphaComponents() );
        return;
    }
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
    if (inputNb == eInputSource) {
        comps->push_back( defaultDepthPlane() );
    }
}

void
LensDefocus::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
LensDefocus::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
LensDefocus::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                            EffectInstance::ComponentsNeededMap* comps,
                                            double* passThroughTime,
                                            int* passThroughView,
                                            int* passThroughInput)
{
    (*comps)[eInputSource].push_back( ImagePlaneDesc::getRGBAComponents() );
    KnobChoicePtr src = _imp->depthSource.lock();
    if (!src || src->getValue() == eDepthSourcePlane) {
        (*comps)[eInputSource].push_back( resolveDepthPlane(_imp->depthPlane, _imp->cachedLayers) );
    } else {
        (*comps)[eInputDepth].push_back( ImagePlaneDesc::getRGBAComponents() );
    }
    (*comps)[eInputCleanBg].push_back( ImagePlaneDesc::getRGBAComponents() );
    (*comps)[eInputMask].push_back( ImagePlaneDesc::getAlphaComponents() );

    (*comps)[-1].push_back( ImagePlaneDesc::getRGBAComponents() );
    KnobBoolPtr err = _imp->outputError.lock();
    if (err && err->getValue()) {
        (*comps)[-1].push_back( errorPlane() );
    }

    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = eInputSource;
}

void
LensDefocus::onInputChanged(int inputNo)
{
    if (inputNo == eInputSource) {
        refreshLayerChoices();
    }
}

void
LensDefocus::onMetadataRefreshed(const NodeMetadata& /*metadata*/)
{
    refreshLayerChoices();
}

void
LensDefocus::refreshLayerChoices()
{
    _imp->cachedLayers.clear();

    std::list<ImagePlaneDesc> availableLayers;
    EffectInstancePtr input = getInput(eInputSource);
    if (input) {
        double time = 0;
        if (getApp() && getApp()->getTimeLine()) {
            time = getApp()->getTimeLine()->currentFrame();
        }
        input->getAvailableLayers(time, ViewIdx(0), -1, &availableLayers);
    }

    std::vector<ChoiceOption> entries;
    entries.push_back(ChoiceOption("depth", "depth.Z", "The depth plane written by ScanlineRender and most renderers."));
    for (std::list<ImagePlaneDesc>::const_iterator it = availableLayers.begin(); it != availableLayers.end(); ++it) {
        const ImagePlaneDesc& layer = *it;
        const std::string id = layer.getPlaneID();
        if (id == "Color" || id == "Alpha" || id == "depth") {
            continue;
        }
        std::string label = layer.getPlaneLabel();
        std::string chans;
        const std::vector<std::string>& channels = layer.getChannels();
        for (size_t c = 0; c < channels.size(); ++c) {
            chans += channels[c];
        }
        const std::string displayName = (label.empty() ? id : label) + "." + chans;
        entries.push_back(ChoiceOption(id, displayName, "Read depth from the first channel of " + displayName));
        _imp->cachedLayers.push_back(layer);
    }

    KnobChoicePtr k = _imp->depthPlane.lock();
    if (k) {
        k->populateChoices(entries);
    }
}

void
LensDefocus::refreshKnobVisibility()
{
    KnobChoicePtr conv = _imp->depthConvention.lock();
    KnobChoicePtr src = _imp->depthSource.lock();
    KnobChoicePtr cocM = _imp->cocMode.lock();
    KnobChoicePtr focM = _imp->focusMode.lock();
    if (!conv || !src || !cocM || !focM) {
        return;
    }
    const int c = conv->getValue();
    const bool normalized = c == defocus::eDepthNormalized;
    const bool planar = c == defocus::eDepthPlanarZ || c == defocus::eDepthRadial;
    const bool direct = c == defocus::eDepthDirectCoC;
    _imp->depthPlane.lock()->setSecret(src->getValue() != eDepthSourcePlane);
    _imp->normalizedNear.lock()->setSecret(!normalized);
    _imp->normalizedFar.lock()->setSecret(!normalized);
    _imp->normalizedInvert.lock()->setSecret(!normalized);
    _imp->farThreshold.lock()->setSecret(!planar);
    _imp->zeroIsInfinity.lock()->setSecret(!planar);

    const bool camera = cocM->getValue() == defocus::eCocCamera;
    _imp->focalLength.lock()->setSecret(!camera || direct);
    _imp->fStop.lock()->setSecret(!camera || direct);
    _imp->filmBack.lock()->setSecret(!camera || direct);
    _imp->useCamera.lock()->setSecret(!camera || direct);
    _imp->sizeAtInfinity.lock()->setSecret(camera || direct);
    _imp->focusMode.lock()->setSecret(direct);
    _imp->focusDistance.lock()->setSecret(direct || focM->getValue() != eFocusDistance);
    _imp->focusPoint.lock()->setSecret(direct || focM->getValue() != eFocusPoint);
}

bool
LensDefocus::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/, double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    if (k == _imp->depthConvention.lock().get() || k == _imp->depthSource.lock().get() ||
        k == _imp->cocMode.lock().get() || k == _imp->focusMode.lock().get()) {
        refreshKnobVisibility();
        return true;
    }
    return false;
}

void
LensDefocus::initializeKnobs()
{
    auto dbl = [this](const KnobPagePtr& page, const char* label, const char* name, double def,
                      double dmin, double dmax, const QString& tip, bool animated = true) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(label));
        k->setName(name);
        k->setDefaultValue(def);
        k->setDisplayMinimum(dmin);
        k->setDisplayMaximum(dmax);
        k->setAnimationEnabled(animated);
        k->setHintToolTip(tip);
        page->addKnob(k);
        return k;
    };

    // ---------------------------------------------------------------- Lens
    KnobPagePtr lens = AppManager::createKnob<KnobPage>(this, tr("Lens"));
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Blur Size From"));
        k->setName("cocMode");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("camera", "Camera", "Thin-lens blur from focal length, f-stop, focus distance and film back."));
        o.push_back(ChoiceOption("infinity", "Size at Infinity", "You set the blur radius of objects at infinity; the focus distance sets where it is zero."));
        k->populateChoices(o);
        k->setDefaultValue(defocus::eCocCamera);
        k->setHintToolTip(tr("How the blur size is computed from depth."));
        lens->addKnob(k); _imp->cocMode = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Use Camera Input"));
        k->setName("useCamera");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("When a Camera3D (or Alembic camera) is connected, take focal length, film back "
                             "and f-stop from it every frame. The knobs below are used otherwise."));
        lens->addKnob(k); _imp->useCamera = k;
    }
    _imp->focalLength = dbl(lens, "Focal Length", "focalLength", 50., 8., 300.,
                            tr("Lens focal length in mm."));
    _imp->focalLength.lock()->setMinimum(0.1);
    _imp->fStop = dbl(lens, "F-Stop", "fStop", 2.8, 0.7, 22.,
                      tr("Geometric f-number (not the T-stop: a T2.1 cine lens is about f/1.9). Smaller = more blur."));
    _imp->fStop.lock()->setMinimum(0.1);
    _imp->filmBack = dbl(lens, "Film Back Width", "filmBack", 36., 4., 70.,
                         tr("Width in mm of the sensor area that maps to the full width of the image format. "
                            "If the plate was cropped, use the width of the crop. Matches Camera3D's Horizontal Aperture."));
    _imp->filmBack.lock()->setMinimum(0.1);
    _imp->sizeAtInfinity = dbl(lens, "Size at Infinity", "sizeAtInfinity", 20., 0., 200.,
                               tr("Blur radius in pixels (at full resolution) of objects at infinity."));
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Focus"));
        k->setName("focusMode");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("distance", "Distance", "Focus at Focus Distance."));
        o.push_back(ChoiceOption("point", "Point (autofocus)", "Focus on whatever is under Focus Point, every frame. Link the point to a tracker for a follow-focus."));
        k->populateChoices(o);
        k->setDefaultValue(eFocusDistance);
        lens->addKnob(k); _imp->focusMode = k;
    }
    _imp->focusDistance = dbl(lens, "Focus Distance", "focusDistance", 5., 0.1, 100.,
                              tr("Distance to the focus plane, in scene units (see Depth > Scene Units)."));
    _imp->focusDistance.lock()->setMinimum(1e-4);
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Focus Point"), 2);
        k->setName("focusPoint");
        k->setDefaultValue(960., 0);
        k->setDefaultValue(540., 1);
        k->setDimensionName(0, "x");
        k->setDimensionName(1, "y");
        k->setDisplayMinimum(0., 0); k->setDisplayMaximum(4096., 0);
        k->setDisplayMinimum(0., 1); k->setDisplayMaximum(4096., 1);
        k->setAnimationEnabled(true);
        k->setHasHostOverlayHandle(true);
        k->setHintToolTip(tr("Autofocus point in pixels. The nearest depth in a small window around it is used, "
                             "so it doesn't land on an anti-aliased edge."));
        lens->addKnob(k); _imp->focusPoint = k;
    }
    _imp->nearGain = dbl(lens, "Near Gain", "nearGain", 1., 0., 2.,
                         tr("Multiplies the blur in front of the focus plane."));
    _imp->farGain = dbl(lens, "Far Gain", "farGain", 1., 0., 2.,
                        tr("Multiplies the blur behind the focus plane."));
    _imp->maxRadius = dbl(lens, "Max Radius", "maxRadius", 256., 8., 1024.,
                          tr("Largest blur radius in pixels (full resolution). Larger blurs are clamped and "
                             "flagged in the defocusError plane."));
    _imp->maxRadius.lock()->setMinimum(1.);

    // ---------------------------------------------------------------- Bokeh
    KnobPagePtr bokeh = AppManager::createKnob<KnobPage>(this, tr("Bokeh"));
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Blades"));
        k->setName("blades");
        k->setDefaultValue(0);
        k->setMinimum(0); k->setMaximum(16);
        k->setDisplayMinimum(0); k->setDisplayMaximum(16);
        k->setHintToolTip(tr("Iris blades. 0 = circular aperture; 3-16 = polygon."));
        bokeh->addKnob(k); _imp->blades = k;
    }
    _imp->curvature = dbl(bokeh, "Blade Curvature", "curvature", 0., 0., 1.,
                          tr("0 = straight blades, 1 = round. Real irises are rounder wide open."));
    _imp->rotation = dbl(bokeh, "Rotation", "rotation", 0., -180., 180., tr("Iris rotation in degrees."));
    _imp->squeeze = dbl(bokeh, "Anamorphic Squeeze", "squeeze", 1., 0.5, 2.,
                        tr("Oval bokeh: > 1 makes discs taller (anamorphic look), < 1 wider. Area is preserved."));
    _imp->catEye = dbl(bokeh, "Cat's Eye", "catEye", 0., 0., 1.5,
                       tr("Optical vignetting: discs towards the frame edge are clipped into lemon shapes and "
                          "lose light. The strength is at the frame corner."));
    _imp->catEyeStart = dbl(bokeh, "Cat's Eye Start", "catEyeStart", 0.2, 0., 1.,
                            tr("Distance from the centre (0 = centre, 1 = corner) where the clipping begins."));
    _imp->swirl = dbl(bokeh, "Swirl", "swirl", 0., -1., 1.,
                      tr("Petzval-style swirl: discs stretch tangentially towards the frame edge."));
    _imp->sphericalAberration = dbl(bokeh, "Spherical Aberration", "sphericalAberration", 0., -1., 1.,
                                    tr("> 0: bright-rimmed (soap bubble) bokeh behind focus and soft-centred "
                                       "bokeh in front of it; < 0 swaps them."));
    _imp->onionAmount = dbl(bokeh, "Onion Rings", "onionAmount", 0., 0., 1.,
                            tr("Concentric rings inside the discs, as from moulded aspherical elements."));
    _imp->onionFrequency = dbl(bokeh, "Onion Frequency", "onionFrequency", 6., 1., 16.,
                               tr("Number of rings across the disc radius."));
    _imp->axialCA = dbl(bokeh, "Axial Chromatic Aberration", "axialCA", 0., -4., 4.,
                        tr("Longitudinal colour fringing in pixels: red and blue focus at different distances, "
                           "giving magenta/green fringes that swap sides across the focus plane."));

    // ---------------------------------------------------------------- Depth
    KnobPagePtr depth = AppManager::createKnob<KnobPage>(this, tr("Depth"));
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Depth Source"));
        k->setName("depthSource");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("plane", "Source Plane", "A plane of the Source input (pick it below)."));
        o.push_back(ChoiceOption("input", "Depth Input", "The first channel of the Depth input."));
        k->populateChoices(o);
        k->setDefaultValue(eDepthSourcePlane);
        depth->addKnob(k); _imp->depthSource = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Depth Plane"));
        k->setName("depthPlane");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("depth", "depth.Z", "The depth plane written by ScanlineRender and most renderers."));
        k->populateChoices(o);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Which Source plane holds depth (its first channel is used; CyclesRender's 3-channel "
                             "Depth plane works too)."));
        depth->addKnob(k); _imp->depthPlane = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Depth Is"));
        k->setName("depthConvention");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("planar", "Distance along camera axis (Z)", "ScanlineRender depth.Z, CyclesRender Depth, most renderers."));
        o.push_back(ChoiceOption("radial", "Distance along the ray", "FastVolumeRender Depth; converted using the lens."));
        o.push_back(ChoiceOption("inverse", "1/Z (far = 0)", "Nuke-style inverse depth."));
        o.push_back(ChoiceOption("normalized", "Normalized 0..1", "A relative depth map (AI depth, 2.5D), remapped between Near and Far."));
        o.push_back(ChoiceOption("direct", "Blur radius (px)", "The value is the signed blur radius in pixels: negative in front of focus."));
        k->populateChoices(o);
        k->setDefaultValue(defocus::eDepthPlanarZ);
        depth->addKnob(k); _imp->depthConvention = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Scene Units"));
        k->setName("sceneUnits");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("m", "Metres", ""));
        o.push_back(ChoiceOption("cm", "Centimetres", ""));
        o.push_back(ChoiceOption("mm", "Millimetres", ""));
        o.push_back(ChoiceOption("in", "Inches", ""));
        o.push_back(ChoiceOption("ft", "Feet", ""));
        k->populateChoices(o);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Units of the depth values and Focus Distance. Our 3D nodes and Cycles use metres."));
        depth->addKnob(k); _imp->sceneUnits = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Zero Is Infinity"));
        k->setName("zeroIsInfinity");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("CyclesRender and FastVolumeRender write 0 where nothing was hit."));
        depth->addKnob(k); _imp->zeroIsInfinity = k;
    }
    _imp->farThreshold = dbl(depth, "Infinity Beyond", "farThreshold", 9999., 0., 100000.,
                             tr("Depths at or beyond this are treated as infinity (ScanlineRender writes the camera's "
                                "far clip, 10000 by default, for the background). 0 = off. With a Camera input, "
                                "its far clip is used."), false);
    _imp->normalizedNear = dbl(depth, "Normalized Near", "normalizedNear", 1., 0., 100.,
                               tr("Distance (scene units) that a normalized value of 0 maps to."));
    _imp->normalizedFar = dbl(depth, "Normalized Far", "normalizedFar", 100., 0., 1000.,
                              tr("Distance (scene units) that a normalized value of 1 maps to."));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Normalized: 1 Is Near"));
        k->setName("normalizedInvert");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Most AI depth maps are white = near."));
        depth->addKnob(k); _imp->normalizedInvert = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Depth Premultiplied"));
        k->setName("depthPremultiplied");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("The depth was multiplied by alpha upstream; divide it back out."));
        depth->addKnob(k); _imp->depthPremultiplied = k;
    }

    // ---------------------------------------------------------------- Quality
    KnobPagePtr quality = AppManager::createKnob<KnobPage>(this, tr("Quality"));
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Edge Reconstruction"));
        k->setName("edgeMode");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("off", "Off", "Trust the depth pass as is."));
        o.push_back(ChoiceOption("minmax", "Snap", "Snap edge pixels to the nearer of the front/back depth."));
        o.push_back(ChoiceOption("unmix", "Unmix", "Split anti-aliased edge pixels into a front and a back part by colour. "
                                                   "Best for rendered (aliased) depth passes."));
        k->populateChoices(o);
        k->setDefaultValue(defocus::eEdgeUnmix);
        k->setHintToolTip(tr("Renderers write depth without anti-aliasing while the beauty is anti-aliased, so edge "
                             "pixels mix two objects but carry one depth."));
        quality->addKnob(k); _imp->edgeMode = k;
    }
    _imp->edgeThreshold = dbl(quality, "Edge Threshold", "edgeThreshold", 1., 0.1, 10.,
                              tr("Blur-radius difference (px) across a pixel's neighbours that counts as a depth edge."), false);
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Fill Behind Foreground"));
        k->setName("fillBehind");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("Extend the background behind foreground objects where their blur reveals it. "
                             "Off shows the dark seams other defocus tools have."));
        quality->addKnob(k); _imp->fillBehind = k;
    }
    _imp->sliceStep = dbl(quality, "Slice Step", "sliceStep", 0.15, 0.05, 0.5,
                          tr("Relative blur-size step between depth slices. Smaller = more slices, better "
                             "ordering of overlapping objects, slower."), false);
    _imp->fullResRadius = dbl(quality, "Full-Res Radius", "fullResRadius", 32., 8., 128.,
                              tr("Blurs larger than this (px) are computed at reduced resolution. Larger = sharper "
                                 "large discs, slower."), false);
    _imp->highlightThreshold = dbl(quality, "Highlight Threshold", "highlightThreshold", 8., 0., 100.,
                                   tr("Pixels brighter than this always scatter at full resolution so bokeh discs stay "
                                      "crisp. 0 = off."));
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Highlights"));
        k->setName("maxHighlights");
        k->setDefaultValue(50000);
        k->setMinimum(0);
        k->setDisplayMinimum(0); k->setDisplayMaximum(500000);
        k->setHintToolTip(tr("Cap on full-resolution highlight pixels (the brightest are kept)."));
        quality->addKnob(k); _imp->maxHighlights = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Frame Border"));
        k->setName("border");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("black", "Black", "Nothing outside the frame (darkens the edges)."));
        o.push_back(ChoiceOption("repeat", "Repeat", "Extend the edge pixels, as a real lens sees past the frame."));
        k->populateChoices(o);
        k->setDefaultValue(defocus::eBorderRepeat);
        quality->addKnob(k); _imp->border = k;
    }

    // ---------------------------------------------------------------- Output
    KnobPagePtr out = AppManager::createKnob<KnobPage>(this, tr("Output"));
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Output"));
        k->setName("output");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("result", "Result", "The defocused image."));
        o.push_back(ChoiceOption("focus", "Focus Map", "Red = in front of focus, green = in focus, blue = behind."));
        o.push_back(ChoiceOption("coc", "Blur Radius", "R = signed blur radius (px), G = |radius|, B = clamped."));
        o.push_back(ChoiceOption("slices", "Slices", "False-colour depth slices."));
        o.push_back(ChoiceOption("edge", "Edge Split", "Front coverage of edge pixels split by Edge Reconstruction."));
        k->populateChoices(o);
        k->setDefaultValue(defocus::eOutputResult);
        out->addKnob(k); _imp->output = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Error Plane"));
        k->setName("outputError");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Also output the defocusError plane: guess = how much of the pixel came from filled "
                             "(guessed) background, clamped = the blur hit Max Radius."));
        out->addKnob(k); _imp->outputError = k;
    }
    _imp->mix = dbl(out, "Mix", "mix", 1., 0., 1., tr("Blend between the source (0) and the result (1)."));
    _imp->mix.lock()->setMinimum(0.);
    _imp->mix.lock()->setMaximum(1.);

    refreshKnobVisibility();
}

StatusEnum
LensDefocus::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale, ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(eInputSource);
    if (!input) {
        return eStatusFailed;
    }
    bool isProject;
    StatusEnum st = input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
    if (st != eStatusOK && st != eStatusReplyDefault) {
        return st;
    }
    // With Frame Border = Black, out-of-focus objects at the edge spread past it (a CG element
    // over nothing grows its bbox). With Repeat the frame continues outside, so keep the input RoD.
    if (_imp->border.lock()->getValue() != defocus::eBorderBlack) {
        return eStatusOK;
    }
    defocus::Settings s;
    s.maxRadiusPx = _imp->maxRadius.lock()->getValueAtTime(time);
    s.renderScale = 1.;
    s.lens.squeeze = _imp->squeeze.lock()->getValueAtTime(time);
    s.lens.swirl = _imp->swirl.lock()->getValueAtTime(time);
    s.lens.axialCA = _imp->axialCA.lock()->getValueAtTime(time);
    const double pad = defocus::maxPaddingPx(s);
    rod->x1 -= pad; rod->x2 += pad;
    rod->y1 -= pad; rod->y2 += pad;
    return eStatusOK;
}

namespace {

// Wraps a Natron image as a core view. Natron float images are contiguous rows.
defocus::ConstImageView
viewOf(const ImagePtr& img, const Image::ReadAccess& ra)
{
    defocus::ConstImageView v;
    if (!img) {
        return v;
    }
    const RectI b = img->getBounds();
    if (b.isNull()) {
        return v;
    }
    v.data = (const float*)ra.pixelAt(b.x1, b.y1);
    v.bounds = defocus::Rect(b.x1, b.y1, b.x2, b.y2);
    v.channels = img->getComponents().getNumComponents();
    v.rowStride = (std::ptrdiff_t)img->getRowElements();
    return v;
}

} // anonymous namespace

StatusEnum
LensDefocus::render(const RenderActionArgs& args)
{
    const double time = args.time;
    const double pxScale = args.mappedScale.toOfxPointD().x;

    ImagePtr srcImg = getImage(eInputSource, time, args.mappedScale, args.view,
                               NULL, NULL, false, false, eStorageModeRAM, 0, NULL);
    if (!srcImg) {
        return eStatusFailed;
    }
    ImagePtr depthImg;
    if (_imp->depthSource.lock()->getValue() == eDepthSourceInput) {
        if (getInput(eInputDepth)) {
            depthImg = getImage(eInputDepth, time, args.mappedScale, args.view,
                                NULL, NULL, false, false, eStorageModeRAM, 0, NULL);
        }
    } else {
        ImagePlaneDesc dDesc = resolveDepthPlane(_imp->depthPlane, _imp->cachedLayers);
        depthImg = getImage(eInputSource, time, args.mappedScale, args.view,
                            NULL, &dDesc, false, false, eStorageModeRAM, 0, NULL);
    }
    ImagePtr cleanImg = getInput(eInputCleanBg)
                        ? getImage(eInputCleanBg, time, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, NULL)
                        : ImagePtr();
    ImagePtr maskImg = getInput(eInputMask)
                       ? getImage(eInputMask, time, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, NULL)
                       : ImagePtr();

    // ---- settings
    defocus::Settings s;
    s.renderScale = pxScale;
    s.depthConvention = (defocus::DepthConventionEnum)_imp->depthConvention.lock()->getValue();
    s.zeroIsInfinity = _imp->zeroIsInfinity.lock()->getValue();
    s.farThreshold = _imp->farThreshold.lock()->getValue();
    s.normalizedNear = _imp->normalizedNear.lock()->getValueAtTime(time);
    s.normalizedFar = _imp->normalizedFar.lock()->getValueAtTime(time);
    s.normalizedInvert = _imp->normalizedInvert.lock()->getValue();
    s.depthPremultiplied = _imp->depthPremultiplied.lock()->getValue();
    const int units = std::max(0, std::min(4, _imp->sceneUnits.lock()->getValue()));
    s.worldToMm = kUnitsToMm[units];

    s.cocMode = (defocus::CocModeEnum)_imp->cocMode.lock()->getValue();
    s.focalLengthMm = _imp->focalLength.lock()->getValueAtTime(time);
    s.fNumber = _imp->fStop.lock()->getValueAtTime(time);
    s.filmbackWidthMm = _imp->filmBack.lock()->getValueAtTime(time);
    s.sizeAtInfinityPx = _imp->sizeAtInfinity.lock()->getValueAtTime(time);
    s.focusDistance = _imp->focusDistance.lock()->getValueAtTime(time);
    s.nearGain = _imp->nearGain.lock()->getValueAtTime(time);
    s.farGain = _imp->farGain.lock()->getValueAtTime(time);
    s.maxRadiusPx = _imp->maxRadius.lock()->getValueAtTime(time);

    // Camera input: lens and far clip per frame.
    EffectInstancePtr camEffect = skipDots(getInput(eInputCamera));
    CameraProvider* cam = camEffect ? dynamic_cast<CameraProvider*>(camEffect.get()) : NULL;
    if (cam && _imp->useCamera.lock()->getValue()) {
        s.focalLengthMm = cam->getCameraFocalLength(time);
        s.filmbackWidthMm = cam->getCameraHAperture(time);
        s.fNumber = cam->getCameraFStop(time);
        if (s.farThreshold > 0.) {
            s.farThreshold = cam->getCameraFar(time) * 0.9999;
        }
    }

    const RectI format = getOutputFormat();
    s.formatWidthPx = std::max(1, format.width());
    s.opticalCentreX = 0.5 * (format.x1 + format.x2) * pxScale;
    s.opticalCentreY = 0.5 * (format.y1 + format.y2) * pxScale;
    s.fieldRadiusPx = 0.5 * std::sqrt((double)format.width() * format.width() + (double)format.height() * format.height()) * pxScale;

    s.lens.blades = _imp->blades.lock()->getValue();
    s.lens.curvature = _imp->curvature.lock()->getValueAtTime(time);
    s.lens.rotationDeg = _imp->rotation.lock()->getValueAtTime(time);
    s.lens.squeeze = _imp->squeeze.lock()->getValueAtTime(time);
    s.lens.catEye = _imp->catEye.lock()->getValueAtTime(time);
    s.lens.catEyeStart = _imp->catEyeStart.lock()->getValueAtTime(time);
    s.lens.swirl = _imp->swirl.lock()->getValueAtTime(time);
    s.lens.sphericalAberration = _imp->sphericalAberration.lock()->getValueAtTime(time);
    s.lens.onionAmount = _imp->onionAmount.lock()->getValueAtTime(time);
    s.lens.onionFrequency = _imp->onionFrequency.lock()->getValueAtTime(time);
    s.lens.axialCA = _imp->axialCA.lock()->getValueAtTime(time);

    s.sliceStep = std::max(0.02, _imp->sliceStep.lock()->getValue());
    s.fullResRadius = _imp->fullResRadius.lock()->getValue() * pxScale;
    s.highlightThreshold = _imp->highlightThreshold.lock()->getValueAtTime(time);
    s.maxHighlights = _imp->maxHighlights.lock()->getValue();
    s.edgeMode = (defocus::EdgeModeEnum)_imp->edgeMode.lock()->getValue();
    s.edgeThresholdPx = _imp->edgeThreshold.lock()->getValue();
    s.fillBehind = _imp->fillBehind.lock()->getValue();
    s.border = (defocus::BorderModeEnum)_imp->border.lock()->getValue();
    s.output = (defocus::OutputModeEnum)_imp->output.lock()->getValue();
    s.mix = _imp->mix.lock()->getValueAtTime(time);

    // ---- views
    Image::ReadAccess srcRa(srcImg.get());
    std::unique_ptr<Image::ReadAccess> depthRa, cleanRa, maskRa;
    defocus::Inputs in;
    in.source = viewOf(srcImg, srcRa);
    if (depthImg) {
        depthRa.reset(new Image::ReadAccess(depthImg.get()));
        in.depth = viewOf(depthImg, *depthRa);
    }
    if (cleanImg) {
        cleanRa.reset(new Image::ReadAccess(cleanImg.get()));
        in.cleanBackground = viewOf(cleanImg, *cleanRa);
    }
    if (maskImg) {
        maskRa.reset(new Image::ReadAccess(maskImg.get()));
        in.mask = viewOf(maskImg, *maskRa);
        in.maskChannel = std::max(0, in.mask.channels - 1);
    }

    // Autofocus: the nearest depth in a 5x5 window around the focus point.
    if (_imp->focusMode.lock()->getValue() == eFocusPoint && in.depth.valid() &&
        s.depthConvention != defocus::eDepthDirectCoC) {
        KnobDoublePtr fp = _imp->focusPoint.lock();
        const int px = (int)std::floor(fp->getValueAtTime(time, 0) * pxScale);
        const int py = (int)std::floor(fp->getValueAtTime(time, 1) * pxScale);
        double best = std::numeric_limits<double>::infinity();
        for (int y = py - 2; y <= py + 2; ++y) {
            for (int x = px - 2; x <= px + 2; ++x) {
                if (in.depth.bounds.contains(x, y)) {
                    const double d = defocus::decodeDepthUnits(s, in.depth.pixel(x, y)[0]);
                    if (d > 0. && d < best) {
                        best = d;
                    }
                }
            }
        }
        if (std::isfinite(best)) {
            s.focusDistance = best;
        }
    }

    // ---- outputs: render once into scratch buffers covering the first plane, then copy.
    if (args.outputPlanes.empty()) {
        return eStatusFailed;
    }
    RectI outRect = args.outputPlanes.front().second->getBounds();
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin();
         it != args.outputPlanes.end(); ++it) {
        if (it->second) {
            outRect.merge(it->second->getBounds());
        }
    }
    const int W = outRect.width(), H = outRect.height();
    std::vector<float> rgba((size_t)W * H * 4, 0.f), err;
    defocus::ImageView ov;
    ov.data = rgba.data();
    ov.bounds = defocus::Rect(outRect.x1, outRect.y1, outRect.x2, outRect.y2);
    ov.channels = 4;
    ov.rowStride = (std::ptrdiff_t)W * 4;
    defocus::ImageView ev;
    bool wantErr = false;
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin();
         it != args.outputPlanes.end(); ++it) {
        if (it->first.getPlaneID() == errorPlane().getPlaneID()) {
            wantErr = true;
        }
    }
    if (wantErr) {
        err.assign((size_t)W * H * 3, 0.f);
        ev.data = err.data();
        ev.bounds = ov.bounds;
        ev.channels = 3;
        ev.rowStride = (std::ptrdiff_t)W * 3;
    }

    defocus::Stats stats;
    const bool done = defocus::render(in, s, ov, wantErr ? &ev : NULL, &stats, [this]() { return aborted(); });
    if (!done) {
        return eStatusOK;   // aborted
    }

    if (stats.clamped > 0) {
        std::ostringstream m;
        m << stats.clamped << " pixels hit Max Radius (" << s.maxRadiusPx << " px) and were clamped.";
        setPersistentMessage(eMessageTypeWarning, m.str());
    } else {
        clearPersistentMessage(false);
    }

    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin();
         it != args.outputPlanes.end(); ++it) {
        ImagePtr outImg = it->second;
        if (!outImg) {
            continue;
        }
        const bool isErr = it->first.getPlaneID() == errorPlane().getPlaneID();
        const std::vector<float>& srcBuf = isErr ? err : rgba;
        const int sc = isErr ? 3 : 4;
        const RectI b = outImg->getBounds();
        const int nc = outImg->getComponents().getNumComponents();
        Image::WriteAccess wa(outImg.get());
        for (int y = b.y1; y < b.y2; ++y) {
            for (int x = b.x1; x < b.x2; ++x) {
                float* d = (float*)wa.pixelAt(x, y);
                if (!d) {
                    continue;
                }
                const float* p = &srcBuf[((size_t)(y - outRect.y1) * W + (x - outRect.x1)) * sc];
                if (nc == 1) {
                    d[0] = isErr ? p[0] : p[3];
                } else {
                    for (int c = 0; c < nc; ++c) {
                        d[c] = c < sc ? p[c] : 1.f;
                    }
                }
            }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
