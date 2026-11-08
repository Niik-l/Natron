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

#include "VRDepthBlur.h"

#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"

#include "VRFilterCore.h"

NATRON_NAMESPACE_ENTER

// Inputs
enum
{
    eInputSource = 0,
    eInputDepth,
};

// Depth Source choices
enum
{
    eDepthSourcePlane = 0,  // a depth-like channel (plane) of the Source input
    eDepthSourceInput,      // channel of the optional Depth input
};

namespace {

struct DepthOption
{
    ImagePlaneDesc plane;
    int channel;
    std::string label;
};

ImagePlaneDesc
defaultDepthPlane()
{
    std::vector<std::string> channels;
    channels.push_back("Z");
    return ImagePlaneDesc("depth", "depth", "Z", channels);
}

std::string
lower(std::string v)
{
    for (size_t i = 0; i < v.size(); ++i) {
        v[i] = (char)std::tolower((unsigned char)v[i]);
    }
    return v;
}

// Simplified version of LensDefocus' depthScore: how likely a channel is the depth pass,
// from its plane and channel names.
int
depthScore(const std::string& planeId, const std::string& planeLabel, const std::string& channel, int chIndex)
{
    const std::string p = lower(planeLabel.empty() ? planeId : planeLabel);
    const std::string c = lower(channel);
    int score = 0;
    if (p == "depth") {
        score = 100;
    } else if (p.find("depth") != std::string::npos) {
        score = 90;
    } else if (p == "z" || p == "pz" || c == "pz") {
        score = 90;
    } else if (c == "z" && (p == "color" || p == "rgba" || p == "rgb" || p.empty())) {
        score = 88;
    } else if (c == "z") {
        score = 50;
    }
    if (score > 0) {
        if (c == "z" || c == "depth") {
            score += 2;
        } else if (chIndex == 0) {
            score += 1;
        }
    }
    return score;
}

} // anonymous namespace

struct VRDepthBlurPrivate
{
    mutable QMutex optionsMutex;
    std::vector<DepthOption> depthOptions;
    int autoIndex = -1;

    KnobChoiceWPtr depthSource;
    KnobChoiceWPtr depthChannel;
    KnobDoubleWPtr size;
    KnobDoubleWPtr focus;
    KnobDoubleWPtr range;
    KnobChoiceWPtr quality;
    KnobBoolWPtr   latitudeComp;
    KnobDoubleWPtr mix;
};

// The depth channel chosen in the menu ("auto" = the detected one). Returns false if none.
static bool
resolveDepthChannel(const VRDepthBlurPrivate& imp, ImagePlaneDesc* plane, int* channel)
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
    return true;
}

VRDepthBlur::VRDepthBlur(NodePtr node)
    : EffectInstance(node)
    , _imp(new VRDepthBlurPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

VRDepthBlur::~VRDepthBlur()
{
}

std::string
VRDepthBlur::getPluginDescription() const
{
    return "Depth-masked blur for VR / 360 equirectangular footage.\n\n"
           "Pixels at the Focus depth stay sharp; the seam-safe blur sigma ramps up to Size "
           "over Range depth units on either side of focus. The blur wraps horizontally "
           "across the 0/360 seam, clamps at the poles and compensates latitude, so depth "
           "grading near the seam and the poles behaves like the rest of the sphere.\n\n"
           "Depth source: a depth-like channel of the Source input (auto-detected by name: "
           "depth.Z, Z, ZDepth, Pz, Depth, SceneDepth, ...) or the optional Depth input. "
           "Focus and Range are in the depth channel's own units (metres for renderer "
           "depth passes, 0..1 for normalized Z). Without depth the node passes the source "
           "through.";
}

std::string
VRDepthBlur::getInputLabel(int inputNb) const
{
    switch (inputNb) {
    case eInputSource: return "Source";
    case eInputDepth:  return "Depth";
    default: return "";
    }
}

bool
VRDepthBlur::isInputOptional(int inputNb) const
{
    return inputNb == eInputDepth;
}

void
VRDepthBlur::addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps)
{
    if (inputNb == eInputDepth) {
        // Any single-channel plane is a valid depth; the default plane list covers the
        // common cases and the channel menu picks among what the input actually has.
        comps->push_back( ImagePlaneDesc::getRGBAComponents() );
        comps->push_back( ImagePlaneDesc::getRGBComponents() );
        comps->push_back( ImagePlaneDesc::getAlphaComponents() );
        comps->push_back( defaultDepthPlane() );
        return;
    }
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
    comps->push_back( defaultDepthPlane() );
}

void
VRDepthBlur::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
VRDepthBlur::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
VRDepthBlur::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                            EffectInstance::ComponentsNeededMap* comps,
                                            double* /*passThroughTime*/,
                                            int* /*passThroughView*/,
                                            int* /*passThroughInput*/)
{
    (*comps)[eInputSource].push_back( ImagePlaneDesc::getRGBAComponents() );
    KnobChoicePtr src = _imp->depthSource.lock();
    const int depthInput = (src && src->getValue() == eDepthSourceInput) ? eInputDepth : eInputSource;
    ImagePlaneDesc dPlane;
    int dCh = 0;
    if (resolveDepthChannel(*_imp, &dPlane, &dCh)) {
        (*comps)[depthInput].push_back(dPlane);
    } else {
        (*comps)[depthInput].push_back( defaultDepthPlane() );
    }
    (*comps)[-1].push_back( ImagePlaneDesc::getRGBAComponents() );
}

void
VRDepthBlur::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Depth"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Depth Source"));
        k->setName("depthSource");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("srcPlane", "Z-Depth Channel",
                                    "Use a depth-like channel (plane) of the Source input, "
                                    "chosen in Depth Channel below."));
        opts.push_back(ChoiceOption("depthInput", "Depth Input",
                                    "Use the optional Depth input's channel."));
        k->populateChoices(opts);
        k->setDefaultValue(eDepthSourcePlane);
        k->setHintToolTip(tr("Where the depth pass comes from."));
        page->addKnob(k); _imp->depthSource = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Depth Channel"));
        k->setName("depthChannel");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("auto", "Auto (none found)",
                                    "Pick the depth channel by name: depth.Z/z, Z, ZDepth, "
                                    "Z-depth, Pz, Depth, SceneDepth ... Choose a channel "
                                    "below if the guess is wrong."));
        k->populateChoices(opts);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Which channel of the depth source holds the depth."));
        page->addKnob(k); _imp->depthChannel = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size"));
        k->setName("size"); k->setDefaultValue(25.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(200.0);
        k->setHintToolTip(tr("Maximum blur sigma (pixels at the equator) reached at Range "
                             "depth units away from Focus."));
        page->addKnob(k); _imp->size = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Focus"));
        k->setName("focus"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Depth value that stays sharp, in the depth channel's own "
                             "units. Set it beyond the far depth to blur the near "
                             "background instead."));
        page->addKnob(k); _imp->focus = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Range"));
        k->setName("range"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(1e-6);
        k->setHintToolTip(tr("Depth distance over which the blur ramps from 0 at Focus to "
                             "Size, in the depth channel's own units."));
        page->addKnob(k); _imp->range = k;
    }

    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    }

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Quality"));
        k->setName("quality");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("low",    "Low",    "Kernel extent 2*sigma. Fastest."));
        opts.push_back(ChoiceOption("medium", "Medium", "Kernel extent 3*sigma."));
        opts.push_back(ChoiceOption("high",   "High",   "Kernel extent 4*sigma. Smoothest."));
        k->populateChoices(opts);
        k->setDefaultValue(1); // medium
        k->setHintToolTip(tr("How far each gaussian kernel extends before truncation."));
        page->addKnob(k); _imp->quality = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Latitude Compensation"));
        k->setName("latitudeComp"); k->setDefaultValue(true);
        k->setHintToolTip(tr("Scale the horizontal sigma by 1/cos(latitude) toward the "
                             "poles (capped at 8x)."));
        page->addKnob(k); _imp->latitudeComp = k;
    }

    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
        k->setName("mix"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Blend between original (0) and fully blurred (1) output."));
        page->addKnob(k); _imp->mix = k;
    }
}

bool
VRDepthBlur::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    KnobChoicePtr src = _imp->depthSource.lock();
    if (src.get() == k) {
        refreshLayerChoices();
        return true;
    }
    return false;
}

void
VRDepthBlur::onInputChanged(int inputNo)
{
    if (inputNo == eInputSource || inputNo == eInputDepth) {
        refreshLayerChoices();
    }
}

void
VRDepthBlur::refreshLayerChoices()
{
    KnobChoicePtr src = _imp->depthSource.lock();
    const int inputNb = (src && src->getValue() == eDepthSourceInput) ? eInputDepth : eInputSource;
    std::list<ImagePlaneDesc> availableLayers;
    double time = 0;
    if (getApp() && getApp()->getTimeLine()) {
        time = getApp()->getTimeLine()->currentFrame();
    }
    EffectInstancePtr input = getInput(inputNb);
    if (input) {
        input->getAvailableLayers(time, ViewIdx(0), -1, &availableLayers);
    }

    std::vector<DepthOption> options;
    int best = -1, bestScore = 0;
    for (std::list<ImagePlaneDesc>::const_iterator it = availableLayers.begin();
         it != availableLayers.end(); ++it) {
        const ImagePlaneDesc& layer = *it;
        if (layer.getNumComponents() == 0) {
            continue;
        }
        const std::string id = layer.getPlaneID();
        // OpenFX's always-listed stereo/motion planes would only clutter the menu.
        if (id.find("StereoDisparity") != std::string::npos ||
            id.find("MotionVector") != std::string::npos) {
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

    // On the separate Depth input a plain greyscale image has no depth-like names:
    // its first channel is the depth.
    if (best < 0 && inputNb == eInputDepth && !options.empty()) {
        best = 0;
    }

    std::vector<ChoiceOption> entries;
    entries.push_back(ChoiceOption("auto",
                                   best >= 0 ? "Auto (" + options[best].label + ")" : std::string("Auto (none found)"),
                                   "Pick the depth channel by name: depth.Z/z, Z, ZDepth, Z-depth, Pz, Depth, "
                                   "SceneDepth ... Choose a channel below if the guess is wrong."));
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

StatusEnum
VRDepthBlur::render(const RenderActionArgs& args)
{
    RectI srcRoi;
    ImagePtr srcImg = getImage(eInputSource, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;
    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    const double sizeVal    = std::max(0.0, _imp->size.lock()->getValueAtTime(args.time));
    const double focusVal   = _imp->focus.lock()->getValueAtTime(args.time);
    const double rangeVal   = std::max(1e-6, _imp->range.lock()->getValueAtTime(args.time));
    const int    quality    = std::max(1, _imp->quality.lock()->getValue() + 1);
    const bool   latitudeComp = _imp->latitudeComp.lock()->getValue() != 0;
    const double mixVal     = _imp->mix.lock()->getValueAtTime(args.time);

    const RectI srcBounds = srcImg->getBounds();
    const RectI outBounds = outImg->getBounds();
    const int W = srcBounds.x2 - srcBounds.x1;
    const int H = srcBounds.y2 - srcBounds.y1;
    if (W <= 0 || H <= 0) return eStatusOK;

    int nComp = std::min(srcImg->getComponents().getNumComponents(),
                         outImg->getComponents().getNumComponents());
    if (nComp > 4) nComp = 4;
    if (nComp <= 0) return eStatusOK;

    const double pxScale = args.mappedScale.toOfxPointD().x;

    // ---- depth: resolve the plane, fetch, build a 1-channel buffer over srcBounds
    ImagePtr depthImg;
    ImagePlaneDesc dPlane;
    int dChannel = 0;
    const int depthInputNb = _imp->depthSource.lock()->getValue() == eDepthSourceInput
                             ? eInputDepth : eInputSource;
    if (getInput(depthInputNb) && resolveDepthChannel(*_imp, &dPlane, &dChannel)) {
        depthImg = getImage(depthInputNb, args.time, args.mappedScale, args.view,
                            NULL, &dPlane, false, false, eStorageModeRAM, 0, NULL);
    }

    const size_t nPixels = (size_t)W * H;
    std::vector<float> depthNorm;  // 0..1 blur factor per pixel
    bool haveDepth = false;
    if (depthImg) {
        depthNorm.assign(nPixels, 0.0f);
        Image::ReadAccess dra(depthImg.get());
        const RectI dBounds = depthImg->getBounds();
        const std::vector<std::string>& dChans = depthImg->getComponents().getChannels();
        int chanIdx = std::min(dChannel, (int)dChans.size() - 1);
        if (chanIdx < 0) chanIdx = 0;
        haveDepth = true;
        for (int y = 0; y < H; ++y) {
            const int dy = srcBounds.y1 + y;
            const bool yOk = (dy >= dBounds.y1 && dy < dBounds.y2);
            for (int x = 0; x < W; ++x) {
                const int dx = srcBounds.x1 + x;
                float d = 0.0f;
                if (yOk && dx >= dBounds.x1 && dx < dBounds.x2) {
                    const float* p = (const float*)dra.pixelAt(dx, dy);
                    if (p) d = p[chanIdx];
                }
                float t = (float)(std::abs((double)d - focusVal) / rangeVal);
                if (t < 0.0f) t = 0.0f;
                else if (t > 1.0f) t = 1.0f;
                depthNorm[(size_t)y * W + x] = t;
            }
        }
    }

    // Copy the source into an interleaved buffer.
    std::vector<float> src((size_t)nPixels * nComp);
    {
        Image::ReadAccess ra(srcImg.get());
        for (int y = 0; y < H; ++y) {
            const float* srow = (const float*)ra.pixelAt(srcBounds.x1, srcBounds.y1 + y);
            float* drow = &src[(size_t)y * W * nComp];
            if (srow) {
                std::memcpy(drow, srow, (size_t)W * nComp * sizeof(float));
            } else {
                std::memset(drow, 0, (size_t)W * nComp * sizeof(float));
            }
        }
    }

    // No depth: passthrough.
    if (!haveDepth) {
        Image::WriteAccess wa(outImg.get());
        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            const int by = y - srcBounds.y1;
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;
                const int bx = x - srcBounds.x1;
                if (bx < 0 || bx >= W || by < 0 || by >= H) {
                    for (int c = 0; c < nComp; ++c) dst[c] = 0.0f;
                    continue;
                }
                const float* srcPix = &src[((size_t)by * W + bx) * nComp];
                for (int c = 0; c < nComp; ++c) dst[c] = srcPix[c];
            }
        }
        return eStatusOK;
    }

    // ---- slice compositing: blurred copies at nSlices discrete sigmas (half res),
    // per-pixel blended by the depth factor.
    const int nSlices = 6;
    const int hw = std::max(1, W / 2);
    const int hh = std::max(1, H / 2);

    // Half-res source (wrap-aware downsample).
    std::vector<float> srcHalf((size_t)hw * hh * nComp);
    {
        int gw = 0, gh = 0;
        VRFilterCore::downsample2xWrap(&src[0], &srcHalf[0], W, H, nComp, &gw, &gh);
    }

    std::vector<std::vector<float> > slices(nSlices);
    VRFilterCore::BlurParams bp;
    bp.sizeXFactor = 1.0;
    bp.quality = quality;
    bp.latitudeComp = latitudeComp;
    bp.width = hw;
    bp.height = hh;
    for (int s = 1; s < nSlices; ++s) {
        if ( aborted() ) return eStatusOK;
        const double sigmaFull = sizeVal * pxScale * (double)s / (double)(nSlices - 1);
        slices[s].assign((size_t)hw * hh * nComp, 0.0f);
        bp.sigmaX = sigmaFull * 0.5;  // half-res pixels
        bp.sigmaY = sigmaFull * 0.5;
        VRFilterCore::wrapBlur(&srcHalf[0], &slices[s][0], hw, hh, nComp, bp,
                               std::function<bool()>( [this]() { return aborted(); } ));
    }

    // Combine: per-pixel lerp between adjacent slices.
    {
        Image::WriteAccess wa(outImg.get());
        const float fMix = (float)mixVal;
        const float invMix = 1.0f - fMix;
        const float lastSlice = (float)(nSlices - 1);
        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            const int by = y - srcBounds.y1;
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;
                const int bx = x - srcBounds.x1;
                if (bx < 0 || bx >= W || by < 0 || by >= H) {
                    for (int c = 0; c < nComp; ++c) dst[c] = 0.0f;
                    continue;
                }
                const size_t pixIdx = (size_t)by * W + bx;
                const float t = depthNorm[pixIdx] * lastSlice;
                int s0 = (int)t;
                if (s0 > nSlices - 1) s0 = nSlices - 1;
                const int s1 = std::min(s0 + 1, nSlices - 1);
                const float frac = t - (float)s0;

                // Half-res coords for the blurred slices.
                const int hx = std::min(hw - 1, bx >> 1);
                const int hy = std::min(hh - 1, by >> 1);
                const size_t halfIdx = ((size_t)hy * hw + hx) * nComp;

                const float* srcPix = &src[pixIdx * nComp];
                for (int c = 0; c < nComp; ++c) {
                    float blurred;
                    if (s0 == 0) {
                        const float* b1 = &slices[s1][halfIdx];
                        blurred = srcPix[c] * (1.0f - frac) + b1[c] * frac;
                    } else {
                        const float* b0 = &slices[s0][halfIdx];
                        const float* b1 = &slices[s1][halfIdx];
                        blurred = b0[c] * (1.0f - frac) + b1[c] * frac;
                    }
                    dst[c] = blurred * fMix + srcPix[c] * invMix;
                }
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT

#include "moc_VRDepthBlur.cpp"
