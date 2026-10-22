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
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

#include <QtCore/QMutex>

#include "DefocusCore.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"
#include "Engine/KnobFile.h"
#include "Engine/Project.h"
#include "../DotUtils.h"
#include "../Scene3D/CameraProvider.h"
#include "../Scene3D/RotationConventions.h"

#ifdef NATRON_HAVE_OPENIMAGEIO
#include <OpenImageIO/imageio.h>
#endif

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
    eFocusWorld,            // a world-space point: stays on the object when the camera moves
};

enum SpaceChoiceEnum
{
    eSpaceAuto = 0,         // from the plane name: "cam" = camera space
    eSpaceChoiceWorld,
    eSpaceChoiceCamera,
};

// How likely a plane is to be the Position (kind 0) or Normal (kind 1) pass, by name. Planes
// need 3 channels. Camera-space variants score lower than world ones.
int
geometryScore(const std::string& planeLabel, int kind)
{
    std::string p;
    for (size_t i = 0; i < planeLabel.size(); ++i) {
        const char c = (char)std::tolower((unsigned char)planeLabel[i]);
        if (c != '_' && c != '-' && c != ' ' && c != '.') {
            p += c;
        }
    }
    if (kind == 0) {
        if (p == "p" || p == "pworld" || p == "worldp" || p == "position" || p == "worldposition" ||
            p == "positionworld" || p == "wp") {
            return 100;
        }
        if (p == "pcamera" || p == "pcam" || p == "camerap" || p == "positioncamera" || p == "cameraposition") {
            return 70;
        }
        return 0;
    }
    if (p == "n" || p == "nworld" || p == "worldn" || p == "normal" || p == "normals" || p == "worldnormal" ||
        p == "normalworld" || p == "nn") {
        return 100;
    }
    if (p == "ncamera" || p == "ncam" || p == "cameran" || p == "normalcamera" || p == "cameranormal") {
        return 70;
    }
    if (p == "ng") {
        return 60;   // geometric normal
    }
    return 0;
}

bool
nameSaysCamera(const std::string& label)
{
    std::string p;
    for (size_t i = 0; i < label.size(); ++i) {
        p += (char)std::tolower((unsigned char)label[i]);
    }
    return p.find("cam") != std::string::npos;
}

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

const double kUnitsToMm[] = {1000., 10., 1., 25.4, 304.8};   // metres, cm, mm, inches, feet; then Custom

std::string
lower(std::string v)
{
    for (size_t i = 0; i < v.size(); ++i) {
        v[i] = (char)std::tolower((unsigned char)v[i]);
    }
    return v;
}

bool
contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

// How likely a channel is to be the depth pass, from its plane and channel names. Covers the names
// renderers use: depth.Z (Nuke, ScanlineRender), depth.z (Karma), Z (Arnold, Redshift, RenderMan),
// ZDepth / Z-depth / z_depth (V-Ray, Octane, Corona), Pz (Mantra), Depth (Blender, Cycles),
// SceneDepth (Unreal), P_camera.z / Pcam.z (camera-space position).
int
depthScore(const std::string& planeId, const std::string& planeLabel, const std::string& channel, int chIndex)
{
    const std::string p = lower(planeLabel.empty() ? planeId : planeLabel);
    const std::string c = lower(channel);
    const bool positionLike = contains(p, "position") || p == "p" || contains(p, "world") || contains(p, "normal") ||
                              p == "n" || contains(p, "motion") || contains(p, "vel") || contains(p, "uv") ||
                              contains(p, "disparity") || contains(p, "vector");
    int score = 0;
    if (p == "depth") {
        score = 100;
    } else if (contains(p, "depth")) {
        score = 90;   // ZDepth, Z-depth, z_depth, SceneDepth, camDepth, Depth layers of multilayer EXRs
    } else if (p == "z" || p == "pz" || c == "pz") {
        score = 90;
    } else if (c == "z" && (p == "color" || p == "rgba" || p == "rgb" || p.empty())) {
        score = 88;   // a bare Z channel next to RGBA
    } else if ((contains(p, "p_camera") || contains(p, "pcam") || contains(p, "camera_p") || contains(p, "camerap")) && c == "z") {
        score = 60;   // camera-space position: |z| is planar depth
    } else if (c == "z" && !positionLike) {
        score = 50;
    }
    if (score > 0) {
        if (c == "z" || c == "depth") {
            score += 2;
        } else if (chIndex == 0) {
            score += 1;   // ZDepth.R / Depth.R etc.: first channel
        }
    }
    return score;
}

} // anonymous namespace

struct DepthOption
{
    ImagePlaneDesc plane;
    int channel;
    std::string label;
};

struct LensDefocusPrivate
{
    // Every channel of every plane of the depth input, refreshed on input/metadata change (main
    // thread); render() reads it under the mutex. autoIndex = the best-scoring one, -1 if none.
    mutable QMutex optionsMutex;
    std::vector<DepthOption> depthOptions;
    int autoIndex = -1;

    // Position / Normal plane candidates of the Source input (3+ channels), with the auto picks.
    std::vector<ImagePlaneDesc> geomPlanes;
    int autoPosition = -1, autoNormal = -1;

    // The last render's depth (and Position / Normal, camera), for picking focus in the viewer
    // and aligning the focus plane (render thread writes, main thread reads).
    QMutex pickMutex;
    std::vector<float> pickDepth;
    RectI pickBounds;           // pixel coordinates at pickScale
    double pickScale = 1.;
    bool pickValid = false;
    std::vector<float> pickP, pickN;            // 3 per pixel, over pickBounds; empty if absent
    defocus::SpaceEnum pickPSpace = defocus::eSpaceWorld, pickNSpace = defocus::eSpaceWorld;
    defocus::CameraModel pickCamera;            // at pickScale
    double pickPivotX = 0., pickPivotY = 0.;    // canonical coordinates of the last focus pivot

    // EXR header camera cache: file -> matrices (empty = none).
    QMutex headerMutex;
    std::string headerFile;
    bool headerValid = false;
    double headerW2C[16], headerW2NDC[16];
    double headerWidth = 0., headerHeight = 0.;

    // Depth
    KnobChoiceWPtr depthSource, depthChannel, depthConvention, sceneUnits;
    KnobChoiceWPtr positionPlane, positionSpace, normalPlane, normalSpace;
    KnobDoubleWPtr unitScale, farThreshold, normalizedNear, normalizedFar;
    KnobBoolWPtr zeroIsInfinity, normalizedInvert, depthPremultiplied;
    // Lens
    KnobChoiceWPtr cocMode, focusMode;
    KnobBoolWPtr useCamera, cameraFromFile, filmBackFromFile;
    KnobDoubleWPtr focalLength, fStop, filmBack, focusDistance, focusPoint, sizeAtInfinity;
    KnobDoubleWPtr focusWorldPoint, focusTilt, focusSwing;
    KnobButtonWPtr alignPlane;
    KnobDoubleWPtr nearGain, farGain, maxRadius;
    // Bokeh
    KnobIntWPtr blades;
    KnobDoubleWPtr curvature, rotation, squeeze, catEye, catEyeStart, swirl, sphericalAberration;
    KnobDoubleWPtr onionAmount, onionFrequency, axialCA;
    // Quality
    KnobDoubleWPtr sliceStep, fullResRadius, highlightThreshold, edgeThreshold;
    KnobIntWPtr maxHighlights;
    KnobChoiceWPtr edgeMode, border, occlusionEdges;
    KnobBoolWPtr fillBehind;
    // Output
    KnobChoiceWPtr output;
    KnobBoolWPtr outputError;
    KnobDoubleWPtr mix;
};

// The depth channel chosen in the menu ("auto" = the detected one). Returns false if there is none.
static bool
resolveDepthChannel(const LensDefocusPrivate& imp, ImagePlaneDesc* plane, int* channel, std::string* label)
{
    KnobChoicePtr k = imp.depthChannel.lock();
    QMutexLocker l(&imp.optionsMutex);
    int idx = imp.autoIndex;
    if (k) {
        const std::string id = k->getActiveEntry().id;
        if (!id.empty() && id != "auto") {
            idx = -1;
            for (size_t i = 0; i < imp.depthOptions.size(); ++i) {
                const DepthOption& o = imp.depthOptions[i];
                if (o.plane.getPlaneID() + "|" + std::to_string(o.channel) == id) {
                    idx = (int)i;
                    break;
                }
            }
        }
    }
    if (idx < 0 || idx >= (int)imp.depthOptions.size()) {
        return false;
    }
    *plane = imp.depthOptions[idx].plane;
    *channel = imp.depthOptions[idx].channel;
    if (label) {
        *label = imp.depthOptions[idx].label;
    }
    return true;
}

// The Position (kind 0) or Normal (kind 1) plane chosen in the menu, and its space. Returns false
// for None or when nothing was found.
static bool
resolveGeomPlane(const LensDefocusPrivate& imp, int kind, ImagePlaneDesc* plane, defocus::SpaceEnum* space)
{
    KnobChoicePtr k = (kind == 0 ? imp.positionPlane : imp.normalPlane).lock();
    KnobChoicePtr sk = (kind == 0 ? imp.positionSpace : imp.normalSpace).lock();
    if (!k || !sk) {
        return false;
    }
    QMutexLocker l(&imp.optionsMutex);
    const std::string id = k->getActiveEntry().id;
    int idx = -1;
    if (id.empty() || id == "auto") {
        idx = kind == 0 ? imp.autoPosition : imp.autoNormal;
    } else if (id != "none") {
        for (size_t i = 0; i < imp.geomPlanes.size(); ++i) {
            if (imp.geomPlanes[i].getPlaneID() == id) {
                idx = (int)i;
                break;
            }
        }
    }
    if (idx < 0 || idx >= (int)imp.geomPlanes.size()) {
        return false;
    }
    *plane = imp.geomPlanes[idx];
    const int sp = sk->getValue();
    if (sp == eSpaceChoiceWorld) {
        *space = defocus::eSpaceWorld;
    } else if (sp == eSpaceChoiceCamera) {
        *space = defocus::eSpaceCamera;
    } else {
        const std::string label = plane->getPlaneLabel().empty() ? plane->getPlaneID() : plane->getPlaneLabel();
        *space = nameSaysCamera(label) ? defocus::eSpaceCamera : defocus::eSpaceWorld;
    }
    return true;
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
    const int depthInput = (src && src->getValue() == eDepthSourceInput) ? eInputDepth : eInputSource;
    ImagePlaneDesc dPlane;
    int dCh = 0;
    if (resolveDepthChannel(*_imp, &dPlane, &dCh, NULL)) {
        (*comps)[depthInput].push_back(dPlane);
    } else {
        (*comps)[depthInput].push_back( defaultDepthPlane() );
    }
    for (int kind = 0; kind < 2; ++kind) {
        ImagePlaneDesc g;
        defocus::SpaceEnum sp;
        if (resolveGeomPlane(*_imp, kind, &g, &sp)) {
            (*comps)[eInputSource].push_back(g);
        }
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
    if (inputNo == eInputSource || inputNo == eInputDepth) {
        refreshLayerChoices();
    }
    if (inputNo == eInputSource) {
        QMutexLocker l(&_imp->headerMutex);
        _imp->headerFile.clear();   // re-read the camera from the (new) upstream file
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
    KnobChoicePtr src = _imp->depthSource.lock();
    const int inputNb = (src && src->getValue() == eDepthSourceInput) ? eInputDepth : eInputSource;
    std::list<ImagePlaneDesc> availableLayers, sourceLayers;
    double time = 0;
    if (getApp() && getApp()->getTimeLine()) {
        time = getApp()->getTimeLine()->currentFrame();
    }
    EffectInstancePtr input = getInput(inputNb);
    if (input) {
        input->getAvailableLayers(time, ViewIdx(0), -1, &availableLayers);
    }
    if (inputNb == eInputSource) {
        sourceLayers = availableLayers;
    } else if (EffectInstancePtr srcInput = getInput(eInputSource)) {
        srcInput->getAvailableLayers(time, ViewIdx(0), -1, &sourceLayers);
    }

    // Position / Normal candidates: Source planes with 3+ channels.
    std::vector<ImagePlaneDesc> geom;
    int autoP = -1, autoN = -1, bestP = 0, bestN = 0;
    for (std::list<ImagePlaneDesc>::const_iterator it = sourceLayers.begin(); it != sourceLayers.end(); ++it) {
        if (it->getNumComponents() < 3) {
            continue;
        }
        const std::string id = it->getPlaneID();
        if (id.find("StereoDisparity") != std::string::npos || id.find("MotionVector") != std::string::npos ||
            id == ImagePlaneDesc::getRGBAComponents().getPlaneID()) {
            continue;
        }
        const std::string label = it->getPlaneLabel().empty() ? id : it->getPlaneLabel();
        const int sp = geometryScore(label, 0), sn = geometryScore(label, 1);
        if (sp > bestP) {
            bestP = sp;
            autoP = (int)geom.size();
        }
        if (sn > bestN) {
            bestN = sn;
            autoN = (int)geom.size();
        }
        geom.push_back(*it);
    }
    for (int kind = 0; kind < 2; ++kind) {
        const int a = kind == 0 ? autoP : autoN;
        std::vector<ChoiceOption> e;
        const std::string what = kind == 0 ? "Position" : "Normal";
        e.push_back(ChoiceOption("auto",
                                 a >= 0 ? "Auto (" + (geom[a].getPlaneLabel().empty() ? geom[a].getPlaneID() : geom[a].getPlaneLabel()) + ")"
                                        : std::string("Auto (none found)"),
                                 kind == 0 ? "Pick the position pass by name: P, Pworld, position, P_camera ..."
                                           : "Pick the normal pass by name: N, Nworld, normal, N_camera ..."));
        e.push_back(ChoiceOption("none", "None", "Don't use a " + what + " pass."));
        for (size_t i = 0; i < geom.size(); ++i) {
            e.push_back(ChoiceOption(geom[i].getPlaneID(), geom[i].getPlaneLabel().empty() ? geom[i].getPlaneID() : geom[i].getPlaneLabel(), ""));
        }
        KnobChoicePtr k = (kind == 0 ? _imp->positionPlane : _imp->normalPlane).lock();
        if (k) {
            k->populateChoices(e);
        }
    }
    {
        QMutexLocker l(&_imp->optionsMutex);
        _imp->geomPlanes.swap(geom);
        _imp->autoPosition = autoP;
        _imp->autoNormal = autoN;
    }

    std::vector<DepthOption> options;
    int best = -1, bestScore = 0;
    for (std::list<ImagePlaneDesc>::const_iterator it = availableLayers.begin(); it != availableLayers.end(); ++it) {
        const ImagePlaneDesc& layer = *it;
        if (layer.getNumComponents() == 0) {
            continue;
        }
        const std::string id = layer.getPlaneID();
        // OpenFX's standard stereo-disparity and motion-vector planes are always listed by Natron
        // even when the file has no such data; they would only clutter the menu.
        if (id.find("StereoDisparity") != std::string::npos || id.find("MotionVector") != std::string::npos) {
            continue;
        }
        const std::string label = layer.getPlaneLabel().empty() ? id : layer.getPlaneLabel();
        const std::vector<std::string>& channels = layer.getChannels();
        for (size_t c = 0; c < channels.size(); ++c) {
            DepthOption o;
            o.plane = layer;
            o.channel = (int)c;
            o.label = label + "." + channels[c];
            const int score = depthScore(id, label, channels[c], (int)c);
            if (score > bestScore) {
                bestScore = score;
                best = (int)options.size();
            }
            options.push_back(o);
        }
    }

    // On the separate Depth input a plain image (a greyscale depth Read, a ramp) has no depth-like
    // names: its first channel is the depth.
    if (best < 0 && inputNb == eInputDepth && !options.empty()) {
        best = 0;
    }

    std::vector<ChoiceOption> entries;
    entries.push_back(ChoiceOption("auto",
                                   best >= 0 ? "Auto (" + options[best].label + ")" : std::string("Auto (none found)"),
                                   "Pick the depth channel by name: depth.Z/z, Z, ZDepth, Z-depth, Pz, Depth, SceneDepth, "
                                   "P_camera.z ... Choose a channel below if the guess is wrong."));
    for (size_t i = 0; i < options.size(); ++i) {
        entries.push_back(ChoiceOption(options[i].plane.getPlaneID() + "|" + std::to_string(options[i].channel),
                                       options[i].label, ""));
    }
    {
        QMutexLocker l(&_imp->optionsMutex);
        _imp->depthOptions.swap(options);
        _imp->autoIndex = best;
    }
    KnobChoicePtr k = _imp->depthChannel.lock();
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
    _imp->unitScale.lock()->setSecret(_imp->sceneUnits.lock()->getValue() != 5);
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
    const int fm = focM->getValue();
    const bool tilted = _imp->focusTilt.lock()->getValue() != 0. || _imp->focusSwing.lock()->getValue() != 0. ||
                        _imp->focusTilt.lock()->isAnimated(0) || _imp->focusSwing.lock()->isAnimated(0);
    _imp->focusDistance.lock()->setSecret(direct || fm != eFocusDistance);
    // In Distance mode the point is the tilted plane's pivot.
    _imp->focusPoint.lock()->setSecret(direct || !(fm == eFocusPoint || (fm == eFocusDistance && tilted)));
    _imp->focusWorldPoint.lock()->setSecret(direct || fm != eFocusWorld);
    _imp->focusTilt.lock()->setSecret(direct);
    _imp->focusSwing.lock()->setSecret(direct);
    _imp->alignPlane.lock()->setSecret(direct);
}

bool
LensDefocus::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/, double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    if (k == _imp->depthSource.lock().get()) {
        refreshLayerChoices();
    }
    if (k == _imp->alignPlane.lock().get()) {
        alignFocusPlane();
        return true;
    }
    if (k == _imp->depthConvention.lock().get() || k == _imp->depthSource.lock().get() ||
        k == _imp->cocMode.lock().get() || k == _imp->focusMode.lock().get() || k == _imp->sceneUnits.lock().get() ||
        k == _imp->focusTilt.lock().get() || k == _imp->focusSwing.lock().get()) {
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
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Camera From File"));
        k->setName("cameraFromFile");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("Without a Camera input, read the render camera (position, direction, field of view) "
                             "from the upstream Read's EXR header (worldToCamera / worldToNDC, as Karma, RenderMan "
                             "and Arnold write them). Needed for World Point focus and for a world-space Position "
                             "pass; the lens knobs still set the blur."));
        lens->addKnob(k); _imp->cameraFromFile = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Film Back From Render"));
        k->setName("filmBackFromFile");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Set the film back so that Focal Length gives the render camera's field of view (from "
                             "the file's camera). EXR headers carry the field of view but not the focal length: type "
                             "the focal length, and the blur matches the render."));
        lens->addKnob(k); _imp->filmBackFromFile = k;
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
        o.push_back(ChoiceOption("distance", "Distance", "Focus at Focus Distance. Ctrl+Alt+click in the viewer sets it from the depth under the cursor."));
        o.push_back(ChoiceOption("point", "Point (autofocus)", "Focus on whatever is under Focus Point, every frame. Link the point to a tracker for a follow-focus."));
        o.push_back(ChoiceOption("world", "World Point", "Focus on a point in the scene: Ctrl+Alt+click it once and focus "
                                                         "stays on it while the camera moves. Needs a camera (Camera input "
                                                         "or the file's) and a Position pass or depth."));
        k->populateChoices(o);
        k->setDefaultValue(eFocusDistance);
        lens->addKnob(k); _imp->focusMode = k;
    }
    _imp->focusDistance = dbl(lens, "Focus Distance", "focusDistance", 5., 0.1, 100.,
                              tr("Distance to the focus plane, in scene units (see Depth > Scene Units). Ctrl+Alt+click "
                                 "(or Ctrl+middle-click) in the viewer picks it from the depth under the cursor; keyframed "
                                 "if the knob is animated."));
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
        k->setHintToolTip(tr("Autofocus point in pixels: drag the crosshair, or Ctrl+Alt+click (Ctrl+middle-click) "
                             "anywhere in the viewer. The nearest depth in a small window around it is used, so it "
                             "doesn't land on an anti-aliased edge."));
        lens->addKnob(k); _imp->focusPoint = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Focus World Point"), 3);
        k->setName("focusWorldPoint");
        k->setDefaultValue(0., 0);
        k->setDefaultValue(0., 1);
        k->setDefaultValue(0., 2);
        k->setDimensionName(0, "x");
        k->setDimensionName(1, "y");
        k->setDimensionName(2, "z");
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("World-space point that stays in focus (scene units). Ctrl+Alt+click (or Ctrl+middle-click) "
                             "in the viewer picks it from the Position pass, or from depth through the camera."));
        lens->addKnob(k); _imp->focusWorldPoint = k;
    }
    _imp->focusTilt = dbl(lens, "Focus Tilt", "focusTilt", 0., -89., 89.,
                          tr("Tilts the focus plane (tilt-shift / Scheimpflug), in degrees, about the focus point: "
                             "> 0 moves focus further away towards the top of the frame, so a floor or a table can "
                             "be sharp from front to back; < 0 tilts it the other way. Align Focus Plane to Surface "
                             "sets it from the Normal pass."));
    _imp->focusTilt.lock()->setMinimum(-89.9);
    _imp->focusTilt.lock()->setMaximum(89.9);
    _imp->focusSwing = dbl(lens, "Focus Swing", "focusSwing", 0., -89., 89.,
                           tr("Turns the focus plane sideways, in degrees: > 0 moves focus further away towards the "
                              "right of the frame."));
    _imp->focusSwing.lock()->setMinimum(-89.9);
    _imp->focusSwing.lock()->setMaximum(89.9);
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Align Focus Plane to Surface"));
        k->setName("alignPlane");
        k->setHintToolTip(tr("Set Focus Tilt and Swing so the focus plane lies along the surface under the focus "
                             "point (from the Normal pass), e.g. the whole floor sharp. In Distance mode Focus "
                             "Distance is set to that point too."));
        lens->addKnob(k); _imp->alignPlane = k;
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
        o.push_back(ChoiceOption("plane", "Source Input", "Depth is a channel of the Source input (a multi-layer EXR or a renderer node)."));
        o.push_back(ChoiceOption("input", "Depth Input", "Depth comes from the separate Depth input (pick the channel below)."));
        k->populateChoices(o);
        k->setDefaultValue(eDepthSourcePlane);
        depth->addKnob(k); _imp->depthSource = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Depth Channel"));
        k->setName("depthChannel");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("auto", "Auto", ""));
        k->populateChoices(o);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("The channel that holds depth. Every channel of every layer is listed; Auto picks one by name "
                             "and shows which.\n\n"
                             "Typical names and settings:\n"
                             "- ScanlineRender, Nuke: depth.Z (Z, scene units)\n"
                             "- CyclesRender: Depth (Z, 0 = nothing hit)\n"
                             "- Karma / Mantra: depth.z or Pz (Z, metres)\n"
                             "- Arnold, RenderMan, Redshift: Z (Z; Arnold/Maya scenes often in cm)\n"
                             "- Blender: Depth.Z or ViewLayer.Depth.Z (Z, metres)\n"
                             "- V-Ray: ZDepth (usually Normalized, white = near, with its min/max depth)\n"
                             "- Octane, Corona: Z-depth (often Normalized against a max depth)\n"
                             "- Unreal: SceneDepth (Z, centimetres)\n"
                             "- P_camera.z / Pcam.z: camera-space position, |z| = Z\n"
                             "- FastVolumeRender: Depth (Distance along the ray)\n"
                             "- AI / 2.5D depth maps: Normalized, usually 1 = near"));
        depth->addKnob(k); _imp->depthChannel = k;
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
        o.push_back(ChoiceOption("position", "From Position (P)", "Compute depth from the Position pass below (camera-space, "
                                                                  "or world-space with a camera). No depth pass needed."));
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
        o.push_back(ChoiceOption("custom", "Custom", "Millimetres per scene unit given below."));
        k->populateChoices(o);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Units of the depth values and Focus Distance. Our 3D nodes, Cycles, Karma and Blender use "
                             "metres; Maya/Arnold and Unreal scenes are usually centimetres."));
        depth->addKnob(k); _imp->sceneUnits = k;
    }
    _imp->unitScale = dbl(depth, "Millimetres per Unit", "unitScale", 1000., 0.01, 10000.,
                          tr("Custom scene scale: how many millimetres one depth unit is."), false);
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
    for (int kind = 0; kind < 2; ++kind) {
        const bool pos = kind == 0;
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, pos ? tr("Position Pass") : tr("Normal Pass"));
        k->setName(pos ? "positionPlane" : "normalPlane");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("auto", "Auto", ""));
        o.push_back(ChoiceOption("none", "None", ""));
        k->populateChoices(o);
        k->setDefaultValue(0);
        k->setHintToolTip(pos ? tr("Optional position pass of the Source (P, Pworld, P_camera ...). With a Normal pass it "
                                   "tells real depth edges from steep surfaces, removing the faint seams at object "
                                   "outlines; it also gives World Point focus and Depth Is = From Position.")
                              : tr("Optional normal pass of the Source (N, Nworld, N_camera ...). Used with the Position "
                                   "pass, and by Align Focus Plane to Surface."));
        depth->addKnob(k);
        (pos ? _imp->positionPlane : _imp->normalPlane) = k;

        KnobChoicePtr sk = AppManager::createKnob<KnobChoice>(this, pos ? tr("Position Space") : tr("Normal Space"));
        sk->setName(pos ? "positionSpace" : "normalSpace");
        std::vector<ChoiceOption> so;
        so.push_back(ChoiceOption("auto", "Auto", "Camera space if the pass name says so (P_camera, Ncam ...), else world."));
        so.push_back(ChoiceOption("world", "World", ""));
        so.push_back(ChoiceOption("camera", "Camera", "Either direction of the camera axis is fine."));
        sk->populateChoices(so);
        sk->setDefaultValue(eSpaceAuto);
        depth->addKnob(sk);
        (pos ? _imp->positionSpace : _imp->normalSpace) = sk;
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
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Occlusion Edges"));
        k->setName("occlusionEdges");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("split", "Split", "Every object outline goes between depth slices: no seams, slower "
                                                   "(about 1.5-3x with many objects at many depths)."));
        o.push_back(ChoiceOption("fast", "Fast", "Only outlines with a clear blur difference split, as without the passes."));
        k->populateChoices(o);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Only with Position and Normal passes."));
        quality->addKnob(k); _imp->occlusionEdges = k;
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

// Picking from the last render's caches (caller holds pickMutex). Canonical (cx, cy) -> the pixel
// with the nearest depth in a 5x5 window (so a click on an anti-aliased edge lands on the object).
static bool
pickNearest(const LensDefocusPrivate& imp, double cx, double cy, int* bx, int* by, double* depth)
{
    if (!imp.pickValid) {
        return false;
    }
    const RectI& b = imp.pickBounds;
    const int px = (int)std::floor(cx * imp.pickScale), py = (int)std::floor(cy * imp.pickScale);
    double best = std::numeric_limits<double>::infinity();
    for (int y = py - 2; y <= py + 2; ++y) {
        for (int x = px - 2; x <= px + 2; ++x) {
            if (x >= b.x1 && x < b.x2 && y >= b.y1 && y < b.y2) {
                const double d = imp.pickDepth[(size_t)(y - b.y1) * b.width() + (x - b.x1)];
                if (d > 0. && d < best) {
                    best = d;
                    *bx = x;
                    *by = y;
                }
            }
        }
    }
    *depth = best;
    return std::isfinite(best);
}

// World position of a cached pixel: the Position pass, else its depth through the camera.
static bool
pickWorldPoint(const LensDefocusPrivate& imp, int x, int y, double depth, double w[3])
{
    const RectI& b = imp.pickBounds;
    const size_t i = (size_t)(y - b.y1) * b.width() + (x - b.x1);
    const defocus::CameraModel& cam = imp.pickCamera;
    double c[3];
    if (!imp.pickP.empty()) {
        const float* p = &imp.pickP[i * 3];
        if (imp.pickPSpace == defocus::eSpaceWorld) {
            w[0] = p[0]; w[1] = p[1]; w[2] = p[2];
            return true;
        }
        if (!cam.valid) {
            return false;
        }
        c[0] = p[0]; c[1] = p[1]; c[2] = -std::fabs(p[2]);   // either camera-axis sign
    } else {
        if (!cam.valid) {
            return false;
        }
        c[0] = (x + 0.5 - cam.centreX) / cam.focalPx * depth;
        c[1] = (y + 0.5 - cam.centreY) / cam.focalPx * depth;
        c[2] = -depth;
    }
    for (int r = 0; r < 3; ++r) {
        w[r] = cam.origin[r] + cam.rot[r * 3 + 0] * c[0] + cam.rot[r * 3 + 1] * c[1] + cam.rot[r * 3 + 2] * c[2];
    }
    return true;
}

// Camera-space normal of a cached pixel (our convention: looking down -Z).
static bool
pickCameraNormal(const LensDefocusPrivate& imp, int x, int y, double n[3])
{
    if (imp.pickN.empty()) {
        return false;
    }
    const RectI& b = imp.pickBounds;
    const size_t i = (size_t)(y - b.y1) * b.width() + (x - b.x1);
    const float* q = &imp.pickN[i * 3];
    if (imp.pickNSpace == defocus::eSpaceCamera) {
        n[0] = q[0]; n[1] = q[1]; n[2] = q[2];
        // A camera space looking down +z (Karma's P_camera) flips z; the Position pass shows which.
        if (!imp.pickP.empty() && imp.pickPSpace == defocus::eSpaceCamera && imp.pickP[i * 3 + 2] > 0.f) {
            n[2] = -n[2];
        }
        return true;
    }
    if (!imp.pickCamera.valid) {
        return false;
    }
    const double w[3] = {q[0], q[1], q[2]};
    imp.pickCamera.dirToCamera(w, n);
    return true;
}

static void
setMaybeKeyed(const KnobDoublePtr& k, double time, double v, int dim)
{
    if (k->isAnimated(dim)) {
        k->setValueAtTime(time, v, ViewSpec::all(), dim);   // keep focus pulls keyframed
    } else {
        k->setValue(v, ViewSpec::all(), dim);
    }
}

bool
LensDefocus::onOverlayPenDown(double time, const RenderScale& /*renderScale*/, ViewIdx /*view*/,
                              const QPointF& /*viewportPos*/, const QPointF& pos, double /*pressure*/,
                              double /*timestamp*/, PenType pen)
{
    // Ctrl+Alt+left and Ctrl+middle both reach plugin overlays as the middle pen (the viewer keeps
    // plain Alt+drag and middle-drag for panning, and Ctrl+left for colour picking).
    if (pen != ePenTypeMMB) {
        return false;
    }
    KnobDoublePtr fp = _imp->focusPoint.lock();
    setMaybeKeyed(fp, time, pos.x(), 0);
    setMaybeKeyed(fp, time, pos.y(), 1);
    const int mode = _imp->focusMode.lock()->getValue();
    if (mode == eFocusPoint) {
        return true;    // autofocus follows the point from the next render
    }

    int bx = 0, by = 0;
    double depth = 0., w[3];
    bool okDepth, okWorld = false;
    {
        QMutexLocker l(&_imp->pickMutex);
        okDepth = pickNearest(*_imp, pos.x(), pos.y(), &bx, &by, &depth);
        if (okDepth && mode == eFocusWorld) {
            okWorld = pickWorldPoint(*_imp, bx, by, depth, w);
        }
    }
    if (!okDepth) {
        setPersistentMessage(eMessageTypeWarning, "Pick focus: no depth under the cursor (background, or nothing rendered yet).");
        return true;
    }
    if (mode == eFocusWorld) {
        if (!okWorld) {
            setPersistentMessage(eMessageTypeWarning, "Pick focus: World Point needs a world Position pass or a camera "
                                                      "(Camera input, or Camera From File).");
            return true;
        }
        KnobDoublePtr wp = _imp->focusWorldPoint.lock();
        for (int k = 0; k < 3; ++k) {
            setMaybeKeyed(wp, time, w[k], k);
        }
        return true;
    }
    setMaybeKeyed(_imp->focusDistance.lock(), time, depth, 0);
    return true;
}

void
LensDefocus::alignFocusPlane()
{
    const double time = getApp() && getApp()->getTimeLine() ? getApp()->getTimeLine()->currentFrame() : 0.;
    int bx = 0, by = 0;
    double depth = 0., n[3];
    bool okDepth, okNormal = false;
    {
        QMutexLocker l(&_imp->pickMutex);
        okDepth = pickNearest(*_imp, _imp->pickPivotX, _imp->pickPivotY, &bx, &by, &depth);
        if (okDepth) {
            okNormal = pickCameraNormal(*_imp, bx, by, n);
        }
    }
    double tilt = 0., swing = 0.;
    if (!okDepth || !okNormal || !defocus::focusTiltFromNormal(n, &tilt, &swing)) {
        setPersistentMessage(eMessageTypeWarning, !okDepth ? "Align: nothing under the focus point (render the node once first)."
                                                 : !okNormal ? "Align: needs a Normal pass (world-space normals also need a camera)."
                                                 : "Align: the surface under the focus point is seen edge-on.");
        return;
    }
    setMaybeKeyed(_imp->focusTilt.lock(), time, tilt, 0);
    setMaybeKeyed(_imp->focusSwing.lock(), time, swing, 0);
    if (_imp->focusMode.lock()->getValue() == eFocusDistance) {
        setMaybeKeyed(_imp->focusDistance.lock(), time, depth, 0);
    }
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

// Camera from a Camera3D / Alembic camera input, for an image of the given format at pxScale.
bool
cameraFromProvider(const CameraProvider* cam, double time, const RectI& format, double pxScale, defocus::CameraModel* out)
{
    const double fl = cam->getCameraFocalLength(time), hap = cam->getCameraHAperture(time);
    if (!(fl > 0.) || !(hap > 0.) || format.width() <= 0) {
        return false;
    }
    double rx, ry, rz, R[3][3];
    defocus::CameraModel m;
    cam->getCameraPosition(time, m.origin[0], m.origin[1], m.origin[2], rx, ry, rz);
    RotationConventions::compose(rx, ry, rz, R);   // camera to world; looks down -Z
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m.rot[r * 3 + c] = R[r][c];
        }
    }
    m.focalPx = fl / hap * format.width() * pxScale;
    m.centreX = 0.5 * (format.x1 + format.x2) * pxScale;
    m.centreY = 0.5 * (format.y1 + format.y2) * pxScale;
    m.valid = true;
    *out = m;
    return true;
}

// The file the nearest upstream Read (any node with a "filename" knob, through Source's first
// inputs) reads at this time; empty if none.
std::string
upstreamFile(EffectInstance* self, double time)
{
    NodePtr n = self->getNode() ? self->getNode()->getInput(eInputSource) : NodePtr();
    for (int hop = 0; n && hop < 32; ++hop) {
        KnobFilePtr fk = std::dynamic_pointer_cast<KnobFile>(n->getKnobByName("filename"));
        if (fk) {
            std::string f = fk->getFileName((int)std::floor(time + 0.5), ViewIdx(0));
            if (!f.empty() && self->getApp() && self->getApp()->getProject()) {
                self->getApp()->getProject()->canonicalizePath(f);
            }
            return f;
        }
        n = n->getInput(0);
    }
    return std::string();
}

} // anonymous namespace

// Camera from the EXR header of `file` (cached per file). Returns false if it has none.
static bool
exrHeaderCamera(LensDefocusPrivate& imp, const std::string& file, double pxScale, defocus::CameraModel* out)
{
    if (file.empty()) {
        return false;
    }
    QMutexLocker l(&imp.headerMutex);
    if (file != imp.headerFile) {
        imp.headerFile = file;
        imp.headerValid = false;
#ifdef NATRON_HAVE_OPENIMAGEIO
        std::unique_ptr<OIIO::ImageInput> in = OIIO::ImageInput::open(file);
        if (in) {
            const OIIO::ImageSpec& sp = in->spec();
            float a[16], b[16];
            if (sp.getattribute("worldToCamera", OIIO::TypeMatrix44, a) &&
                sp.getattribute("worldToNDC", OIIO::TypeMatrix44, b)) {
                for (int k = 0; k < 16; ++k) {
                    imp.headerW2C[k] = a[k];
                    imp.headerW2NDC[k] = b[k];
                }
                imp.headerWidth = sp.full_width;
                imp.headerHeight = sp.full_height;
                imp.headerValid = true;
            }
            in->close();
        }
#endif
    }
    if (!imp.headerValid) {
        return false;
    }
    return defocus::cameraFromExrMatrices(imp.headerW2C, imp.headerW2NDC, imp.headerWidth, imp.headerHeight, pxScale, out);
}

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
    ImagePlaneDesc dDesc;
    int dChannel = 0;
    std::string dLabel;
    const int depthInputNb = _imp->depthSource.lock()->getValue() == eDepthSourceInput ? eInputDepth : eInputSource;
    if (getInput(depthInputNb) && resolveDepthChannel(*_imp, &dDesc, &dChannel, &dLabel)) {
        depthImg = getImage(depthInputNb, time, args.mappedScale, args.view,
                            NULL, &dDesc, false, false, eStorageModeRAM, 0, NULL);
    }
    // Position / Normal passes of the Source.
    ImagePtr geomImg[2];
    defocus::SpaceEnum geomSpace[2] = {defocus::eSpaceWorld, defocus::eSpaceWorld};
    for (int kind = 0; kind < 2; ++kind) {
        ImagePlaneDesc g;
        if (resolveGeomPlane(*_imp, kind, &g, &geomSpace[kind])) {
            geomImg[kind] = getImage(eInputSource, time, args.mappedScale, args.view, NULL, &g, false, false, eStorageModeRAM, 0, NULL);
        }
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
    const int units = _imp->sceneUnits.lock()->getValue();
    s.worldToMm = units >= 0 && units <= 4 ? kUnitsToMm[units] : std::max(1e-6, _imp->unitScale.lock()->getValue());

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

    // Camera (for Position-based features): the Camera input, else the file's header.
    std::string cameraSource;
    if (cam && _imp->useCamera.lock()->getValue() && cameraFromProvider(cam, time, format, pxScale, &s.camera)) {
        cameraSource = "Camera input";
    } else if (_imp->cameraFromFile.lock()->getValue() &&
               exrHeaderCamera(*_imp, upstreamFile(this, time), pxScale, &s.camera)) {
        s.camera.centreX += format.x1 * pxScale;
        s.camera.centreY += format.y1 * pxScale;
        cameraSource = "file";
        // EXR headers carry the field of view, not the focal length: match the film back to it.
        if (_imp->filmBackFromFile.lock()->getValue() && s.camera.focalPx > 0.) {
            s.filmbackWidthMm = s.focalLengthMm * format.width() * pxScale / s.camera.focalPx;
        }
    }
    s.focusTiltDeg = _imp->focusTilt.lock()->getValueAtTime(time);
    s.focusSwingDeg = _imp->focusSwing.lock()->getValueAtTime(time);
    s.splitOcclusionEdges = _imp->occlusionEdges.lock()->getValue() == 0;

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
    std::unique_ptr<Image::ReadAccess> depthRa, cleanRa, maskRa, geomRa[2];
    defocus::Inputs in;
    in.source = viewOf(srcImg, srcRa);
    if (depthImg) {
        depthRa.reset(new Image::ReadAccess(depthImg.get()));
        in.depth = viewOf(depthImg, *depthRa);
        in.depthChannel = std::max(0, std::min(in.depth.channels - 1, dChannel));
    }
    for (int kind = 0; kind < 2; ++kind) {
        if (geomImg[kind] && geomImg[kind]->getComponents().getNumComponents() >= 3) {
            geomRa[kind].reset(new Image::ReadAccess(geomImg[kind].get()));
            (kind == 0 ? in.position : in.normal) = viewOf(geomImg[kind], *geomRa[kind]);
            (kind == 0 ? in.positionSpace : in.normalSpace) = geomSpace[kind];
        }
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
    const bool fromPosition = s.depthConvention == defocus::eDepthPosition;

    // Planar depth (scene units) at a render-scale pixel: the depth pass, or the Position pass.
    // 0 = nothing there.
    auto depthAt = [&](int x, int y) -> double {
        if (fromPosition) {
            if (!in.position.bounds.contains(x, y)) {
                return 0.;
            }
            const float* p = in.position.pixel(x, y);
            if (p[0] == 0.f && p[1] == 0.f && p[2] == 0.f) {
                return 0.;
            }
            if (in.positionSpace == defocus::eSpaceCamera) {
                return std::fabs(p[2]);
            }
            const double w[3] = {p[0], p[1], p[2]};
            return s.camera.valid ? std::max(0., s.camera.planarDepth(w)) : 0.;
        }
        if (!in.depth.valid() || !in.depth.bounds.contains(x, y)) {
            return 0.;
        }
        const double d = defocus::decodeDepthUnits(s, in.depth.pixel(x, y)[in.depthChannel]);
        return d > 0. && std::isfinite(d) ? d : 0.;
    };

    // Focus. The pivot of a tilted plane is the focus point (Point, Distance) or the world point's
    // image (World Point).
    std::vector<std::string> warnings;
    const int focusMode = _imp->focusMode.lock()->getValue();
    KnobDoublePtr fp = _imp->focusPoint.lock();
    double pivotX = fp->getValueAtTime(time, 0), pivotY = fp->getValueAtTime(time, 1);   // canonical
    if (focusMode == eFocusWorld && s.depthConvention != defocus::eDepthDirectCoC) {
        KnobDoublePtr wp = _imp->focusWorldPoint.lock();
        const double w[3] = {wp->getValueAtTime(time, 0), wp->getValueAtTime(time, 1), wp->getValueAtTime(time, 2)};
        double c[3];
        if (!s.camera.valid) {
            warnings.push_back("World Point focus needs a camera: connect the Camera input, or read a file with a camera "
                               "in its header (Camera From File). Using Focus Distance.");
        } else {
            s.camera.toCamera(w, c);
            if (-c[2] > 0.) {
                s.focusDistance = -c[2];
                pivotX = (s.camera.centreX + s.camera.focalPx * c[0] / -c[2]) / pxScale;
                pivotY = (s.camera.centreY + s.camera.focalPx * c[1] / -c[2]) / pxScale;
            } else {
                warnings.push_back("Focus World Point is behind the camera.");
            }
        }
    } else if (focusMode == eFocusPoint && s.depthConvention != defocus::eDepthDirectCoC) {
        // Autofocus: the nearest depth in a 5x5 window around the focus point.
        const int px = (int)std::floor(pivotX * pxScale);
        const int py = (int)std::floor(pivotY * pxScale);
        double best = std::numeric_limits<double>::infinity();
        for (int y = py - 2; y <= py + 2; ++y) {
            for (int x = px - 2; x <= px + 2; ++x) {
                const double d = depthAt(x, y);
                if (d > 0. && d < best) {
                    best = d;
                }
            }
        }
        if (std::isfinite(best)) {
            s.focusDistance = best;
        }
    }
    s.focusPivotX = pivotX * pxScale;
    s.focusPivotY = pivotY * pxScale;

    // Keep this render's depth, passes and camera for picking in the viewer.
    {
        QMutexLocker l(&_imp->pickMutex);
        const defocus::Rect& b = in.source.bounds;
        const size_t n = (size_t)b.width() * b.height();
        _imp->pickDepth.assign(n, 0.f);
        _imp->pickP.assign(in.position.valid() ? n * 3 : 0, 0.f);
        _imp->pickN.assign(in.normal.valid() ? n * 3 : 0, 0.f);
        for (int y = b.y1; y < b.y2; ++y) {
            for (int x = b.x1; x < b.x2; ++x) {
                const size_t i = (size_t)(y - b.y1) * b.width() + (x - b.x1);
                _imp->pickDepth[i] = (float)depthAt(x, y);
                if (in.position.valid() && in.position.bounds.contains(x, y)) {
                    std::memcpy(&_imp->pickP[i * 3], in.position.pixel(x, y), 3 * sizeof(float));
                }
                if (in.normal.valid() && in.normal.bounds.contains(x, y)) {
                    std::memcpy(&_imp->pickN[i * 3], in.normal.pixel(x, y), 3 * sizeof(float));
                }
            }
        }
        _imp->pickBounds = RectI(b.x1, b.y1, b.x2, b.y2);
        _imp->pickScale = pxScale;
        _imp->pickPSpace = in.positionSpace;
        _imp->pickNSpace = in.normalSpace;
        _imp->pickCamera = s.camera;
        _imp->pickPivotX = pivotX;
        _imp->pickPivotY = pivotY;
        _imp->pickValid = true;
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

    // Depth sanity checks: a wrong channel or convention is the most common setup mistake.
    const bool haveDepth = fromPosition ? in.position.valid() && (in.positionSpace == defocus::eSpaceCamera || s.camera.valid)
                                        : in.depth.valid() || s.depthConvention == defocus::eDepthDirectCoC;
    if (fromPosition && !in.position.valid()) {
        warnings.push_back("Depth Is = From Position, but no Position pass: pick one in Depth > Position Pass. Output = source.");
    } else if (fromPosition && !haveDepth) {
        warnings.push_back("A world-space Position pass needs a camera to give depth: connect the Camera input or read a "
                           "file with a camera in its header (Camera From File). Output = source.");
    } else if (!fromPosition && !haveDepth) {
        warnings.push_back("No depth channel found: pick one in Depth > Depth Channel. Output = source.");
    } else if (!fromPosition && in.depth.valid()) {
        double vMin = std::numeric_limits<double>::infinity(), vMax = -vMin;
        long nFinite = 0, nNeg = 0, nAtMax = 0;
        const defocus::Rect& b = in.depth.bounds;
        const int step = std::max(1, std::max(b.width(), b.height()) / 256);
        for (int y = b.y1; y < b.y2; y += step) {
            for (int x = b.x1; x < b.x2; x += step) {
                const double v = in.depth.pixel(x, y)[in.depthChannel];
                if (!std::isfinite(v) || v == 0.) {
                    continue;
                }
                ++nFinite;
                nNeg += v < 0.;
                vMin = std::min(vMin, std::fabs(v));
                vMax = std::max(vMax, std::fabs(v));
            }
        }
        for (int y = b.y1; y < b.y2 && nFinite > 0; y += step) {
            for (int x = b.x1; x < b.x2; x += step) {
                nAtMax += std::fabs(in.depth.pixel(x, y)[in.depthChannel]) == vMax;
            }
        }
        const bool planar = s.depthConvention == defocus::eDepthPlanarZ || s.depthConvention == defocus::eDepthRadial;
        if (nFinite == 0) {
            warnings.push_back("Depth channel " + dLabel + " is empty (all 0 or not finite): everything is treated as infinitely far.");
        } else if (planar && vMax <= 1.0001 && vMax > vMin) {
            warnings.push_back("Depth values of " + dLabel + " are all within 0..1: if this is a normalized depth map, set "
                               "Depth Is to Normalized (and its Near/Far).");
        } else if (s.depthConvention == defocus::eDepthNormalized && vMax > 1.0001) {
            warnings.push_back("Depth Is = Normalized but " + dLabel + " has values above 1: is it real distance (Z)?");
        }
        if (planar && s.farThreshold <= 0. && vMax >= 1000. && nAtMax > nFinite / 20) {
            std::ostringstream m;
            m << "Many depth values equal " << vMax << " (a far-clip background?): set Infinity Beyond to treat them as infinity.";
            warnings.push_back(m.str());
        }
    }

    defocus::Stats stats;
    bool done = true;
    if (!haveDepth) {
        // No depth: pass the source through rather than blurring everything as if at infinity.
        for (int y = outRect.y1; y < outRect.y2; ++y) {
            for (int x = outRect.x1; x < outRect.x2; ++x) {
                float* o = &rgba[((size_t)(y - outRect.y1) * W + (x - outRect.x1)) * 4];
                if (in.source.bounds.contains(x, y)) {
                    const float* sp = in.source.pixel(x, y);
                    for (int c = 0; c < 4; ++c) {
                        o[c] = c < in.source.channels ? sp[c] : 1.f;
                    }
                }
            }
        }
    } else {
        done = defocus::render(in, s, ov, wantErr ? &ev : NULL, &stats, [this]() { return aborted(); });
    }
    if (!done) {
        return eStatusOK;   // aborted
    }

    if (stats.clamped > 0) {
        std::ostringstream m;
        m << stats.clamped << " pixels hit Max Radius (" << s.maxRadiusPx << " px) and were clamped.";
        warnings.push_back(m.str());
    }
    if (!warnings.empty()) {
        std::string m;
        for (size_t i = 0; i < warnings.size(); ++i) {
            m += (i ? "\n" : "") + warnings[i];
        }
        setPersistentMessage(eMessageTypeWarning, m);
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
