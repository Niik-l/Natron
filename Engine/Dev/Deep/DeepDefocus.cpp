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

#include "DeepDefocus.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../PostFX/DefocusCore.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct DeepDefocusPrivate
{
    KnobDoubleWPtr focalDistance;
    KnobDoubleWPtr fStop;
    KnobDoubleWPtr focalLength;
    KnobDoubleWPtr filmBack;
    KnobDoubleWPtr unitScale;
    KnobDoubleWPtr blurScale;
    KnobIntWPtr numLayers;      // kept for old projects; the engine slices by itself
    KnobDoubleWPtr maxBlur;
    KnobBoolWPtr defocusForeground;
    KnobBoolWPtr defocusBackground;

    // Focus picker (as LensDefocus); the old integer point and button are kept hidden
    KnobChoiceWPtr focusMode;
    KnobBoolWPtr pickFocusMode;
    KnobDoubleWPtr focusPoint;
    bool pickDragging = false;
    KnobIntWPtr focusPointX;
    KnobIntWPtr focusPointY;
    KnobButtonWPtr pickFocusBtn;

    // Bokeh
    KnobIntWPtr apertureBlades; // 0 = circular
    KnobDoubleWPtr apertureRotation;

    KnobStringWPtr info;
};


DeepDefocus::DeepDefocus(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepDefocusPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepDefocus::~DeepDefocus()
{
}

std::string
DeepDefocus::getPluginDescription() const
{
    return tr("Depth of field on deep compositing data.\n\n"
              "Every deep sample blurs at its own depth with a thin-lens circle of confusion (focus "
              "distance, f-stop, focal length, film back), so layers hidden behind the foreground show "
              "through its blur as they really are, transparent and anti-aliased edges keep their own "
              "depths, and volume samples blur along their depth range. Uses the LensDefocus engine "
              "(disc / blade bokeh, occlusion-correct layering); LensDefocus itself takes a deep Source "
              "too and adds real lens designs, camera input and viewer focus picking.\n\n"
              "Output is a flat RGBA image.").toStdString();
}

void
DeepDefocus::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepDefocus::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepDefocus::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepDefocus::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr fd = AppManager::createKnob<KnobDouble>(this, tr("Focal Distance"));
    fd->setName("focalDistance");
    fd->setHintToolTip(tr("Distance of the plane in focus, in the deep image's Z units."));
    fd->setDefaultValue(5.0);
    fd->setMinimum(1e-4);
    fd->setDisplayMinimum(0.1);
    fd->setDisplayMaximum(100.0);
    fd->setAnimationEnabled(true);
    page->addKnob(fd);
    _imp->focalDistance = fd;

    KnobDoublePtr fs = AppManager::createKnob<KnobDouble>(this, tr("F-Stop"));
    fs->setName("fStop");
    fs->setHintToolTip(tr("Aperture f-stop. Lower values = shallower depth of field (more blur)."));
    fs->setDefaultValue(2.8);
    fs->setMinimum(0.1);
    fs->setDisplayMinimum(0.5);
    fs->setDisplayMaximum(22.0);
    fs->setAnimationEnabled(true);
    page->addKnob(fs);
    _imp->fStop = fs;

    KnobDoublePtr fl = AppManager::createKnob<KnobDouble>(this, tr("Focal Length"));
    fl->setName("focalLength");
    fl->setHintToolTip(tr("Lens focal length (mm) of the camera that rendered the deep image."));
    fl->setDefaultValue(50.0);
    fl->setMinimum(1.0);
    fl->setDisplayMinimum(8.0);
    fl->setDisplayMaximum(300.0);
    fl->setAnimationEnabled(true);
    page->addKnob(fl);
    _imp->focalLength = fl;

    KnobDoublePtr fb = AppManager::createKnob<KnobDouble>(this, tr("Film Back"));
    fb->setName("filmBack");
    fb->setHintToolTip(tr("Horizontal film back / sensor width (mm) of that camera."));
    fb->setDefaultValue(36.0);
    fb->setMinimum(1.0);
    fb->setDisplayMinimum(4.0);
    fb->setDisplayMaximum(70.0);
    page->addKnob(fb);
    _imp->filmBack = fb;

    KnobDoublePtr us = AppManager::createKnob<KnobDouble>(this, tr("Scene Unit (mm)"));
    us->setName("unitScale");
    us->setHintToolTip(tr("Millimetres per deep Z unit: 1000 = metres, 10 = centimetres, 1 = millimetres."));
    us->setDefaultValue(1000.0);
    us->setMinimum(1e-6);
    us->setDisplayMinimum(1.0);
    us->setDisplayMaximum(1000.0);
    page->addKnob(us);
    _imp->unitScale = us;

    KnobDoublePtr bs = AppManager::createKnob<KnobDouble>(this, tr("Blur Scale"));
    bs->setName("blurScale");
    bs->setHintToolTip(tr("Artistic multiplier on the blur radius. 1.0 = physically based."));
    bs->setDefaultValue(1.0);
    bs->setMinimum(0.0);
    bs->setDisplayMinimum(0.0);
    bs->setDisplayMaximum(4.0);
    bs->setAnimationEnabled(true);
    page->addKnob(bs);
    _imp->blurScale = bs;

    KnobDoublePtr mb = AppManager::createKnob<KnobDouble>(this, tr("Max Blur"));
    mb->setName("maxBlur");
    mb->setHintToolTip(tr("Maximum blur radius in pixels."));
    mb->setDefaultValue(50.0);
    mb->setMinimum(1.0);
    mb->setDisplayMinimum(1.0);
    mb->setDisplayMaximum(500.0);
    page->addKnob(mb);
    _imp->maxBlur = mb;

    KnobIntPtr nl = AppManager::createKnob<KnobInt>(this, tr("Num Layers"));
    nl->setName("numLayers");
    nl->setHintToolTip(tr("Unused: depth slices are chosen automatically."));
    nl->setDefaultValue(15);
    nl->setSecret(true);
    page->addKnob(nl);
    _imp->numLayers = nl;

    KnobBoolPtr dfg = AppManager::createKnob<KnobBool>(this, tr("Defocus Foreground"));
    dfg->setName("defocusForeground");
    dfg->setDefaultValue(true);
    dfg->setHintToolTip(tr("Blur samples in front of the focal plane."));
    page->addKnob(dfg);
    _imp->defocusForeground = dfg;

    KnobBoolPtr dbg = AppManager::createKnob<KnobBool>(this, tr("Defocus Background"));
    dbg->setName("defocusBackground");
    dbg->setDefaultValue(true);
    dbg->setHintToolTip(tr("Blur samples behind the focal plane."));
    page->addKnob(dbg);
    _imp->defocusBackground = dbg;

    // --- Focus (same controls as LensDefocus) ---
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Focus"));
        k->setName("focusMode");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("distance", "Distance", "Focus at Focal Distance. With Pick Focus on, click in the "
                                                         "viewer to set it from the deep sample under the cursor."));
        o.push_back(ChoiceOption("point", "Point (autofocus)", "Focus on whatever is under Focus Point, every frame. "
                                                               "Link the point to a tracker for a follow-focus."));
        k->populateChoices(o);
        k->setDefaultValue(0);
        page->addKnob(k);
        _imp->focusMode = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Pick Focus"));
        k->setName("pickFocusMode");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setIsPersistent(false);
        k->setHintToolTip(tr("While on (with this panel open), clicking in the viewer picks focus from the nearest "
                             "deep sample under the cursor, and dragging scrubs it: Distance sets Focal Distance, "
                             "Point moves the Focus Point. Picks set keyframes when the knob is animated. Turn it "
                             "off to use the viewer normally. Ctrl+middle-click picks without it."));
        page->addKnob(k);
        _imp->pickFocusMode = k;
    }
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
        k->setHintToolTip(tr("Autofocus point in pixels: drag the crosshair, or click anywhere with Pick Focus on (or "
                             "Ctrl+middle-click). To animate it, set a key first (right-click > Set Key); then each "
                             "drag or pick on another frame adds a key. The nearest deep sample in a small window "
                             "around it is used, so it doesn't land on an anti-aliased edge."));
        page->addKnob(k);
        _imp->focusPoint = k;
    }

    // Old focus picker (integer point + button), kept so saved projects load.
    KnobPagePtr pickPage = AppManager::createKnob<KnobPage>(this, tr("Focus Picker"));

    KnobIntPtr fpx = AppManager::createKnob<KnobInt>(this, tr("Focus Point X"));
    fpx->setName("focusPointX"); fpx->setDefaultValue(640);
    fpx->setSecret(true);
    pickPage->addKnob(fpx);
    _imp->focusPointX = fpx;

    KnobIntPtr fpy = AppManager::createKnob<KnobInt>(this, tr("Focus Point Y"));
    fpy->setName("focusPointY"); fpy->setDefaultValue(360);
    fpy->setSecret(true);
    pickPage->addKnob(fpy);
    _imp->focusPointY = fpy;

    KnobButtonPtr pickBtn = AppManager::createKnob<KnobButton>(this, tr("Set Focus From Point"));
    pickBtn->setName("pickFocus");
    pickBtn->setSecret(true);
    pickPage->addKnob(pickBtn);
    _imp->pickFocusBtn = pickBtn;
    pickPage->setSecret(true);

    // --- Bokeh ---
    KnobPagePtr bokehPage = AppManager::createKnob<KnobPage>(this, tr("Bokeh"));

    KnobIntPtr blades = AppManager::createKnob<KnobInt>(this, tr("Aperture Blades"));
    blades->setName("apertureBlades"); blades->setDefaultValue(0);
    blades->setHintToolTip(tr("Number of aperture blades. 0 = circular (disc), 6 = hexagonal, 8 = octagonal."));
    blades->setMinimum(0);
    blades->setMaximum(16);
    bokehPage->addKnob(blades);
    _imp->apertureBlades = blades;

    KnobDoublePtr rot = AppManager::createKnob<KnobDouble>(this, tr("Aperture Rotation"));
    rot->setName("apertureRotation"); rot->setDefaultValue(0.0);
    rot->setHintToolTip(tr("Rotation of the aperture shape in degrees."));
    rot->setDisplayMinimum(0.0);
    rot->setDisplayMaximum(360.0);
    bokehPage->addKnob(rot);
    _imp->apertureRotation = rot;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAsLabel();
    info->setDefaultValue("Connect a deep node to the input.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepDefocus::depthNear(double cx, double cy, double* depth) const
{
    DeepImagePtr srcDeep = getDeepImageFromEffect(getInput(0).get());
    if (!srcDeep) {
        return false;
    }
    const int zIdx = srcDeep->findChannelIndex("Z");
    const int aIdx = srcDeep->findChannelIndex("A");
    if (zIdx < 0) {
        return false;
    }
    QReadLocker l(&srcDeep->getLock());
    const RectI& dw = srcDeep->getDataWindow();
    const int nc = srcDeep->getNumChannels();
    const int px = (int)std::floor(cx), py = (int)std::floor(cy);
    double best = std::numeric_limits<double>::infinity();
    for (int y = py - 2; y <= py + 2; ++y) {
        for (int x = px - 2; x <= px + 2; ++x) {
            if (x < dw.x1 || x >= dw.x2 || y < dw.y1 || y >= dw.y2) {
                continue;
            }
            const int yd = dw.y1 + dw.y2 - 1 - y;   // deep rows are top-down
            const int n = srcDeep->getSampleCount(x, yd);
            const float* d = n > 0 ? srcDeep->getSampleData(x, yd) : nullptr;
            for (int s = 0; s < n; ++s) {
                const float* q = d + (size_t)s * nc;
                if ((aIdx < 0 || q[aIdx] > 0.f) && q[zIdx] > 0.f && q[zIdx] < best) {
                    best = q[zIdx];
                }
            }
        }
    }
    *depth = best;
    return std::isfinite(best);
}

namespace {
void
setMaybeKeyed(const KnobDoublePtr& k, double time, double v, int dim)
{
    if (k->isAnimated(dim)) {
        k->setValueAtTime(time, v, ViewSpec::all(), dim);   // keep focus pulls keyframed
    } else {
        k->setValue(v, ViewSpec::all(), dim);
    }
}
}

void
DeepDefocus::pickFocusAt(double time, const QPointF& pos)
{
    KnobDoublePtr fp = _imp->focusPoint.lock();
    setMaybeKeyed(fp, time, pos.x(), 0);
    setMaybeKeyed(fp, time, pos.y(), 1);
    if (_imp->focusMode.lock()->getValue() == 1) {
        return;   // autofocus follows the point from the next render
    }
    double depth = 0.;
    if (depthNear(pos.x(), pos.y(), &depth)) {
        setMaybeKeyed(_imp->focalDistance.lock(), time, depth, 0);
    }
}

bool
DeepDefocus::onOverlayPenDown(double time, const RenderScale& /*renderScale*/, ViewIdx /*view*/,
                              const QPointF& /*viewportPos*/, const QPointF& pos, double /*pressure*/,
                              double /*timestamp*/, PenType pen)
{
    // Overlays get no modifier keys, so picking is a mode: with Pick Focus on, a left click picks
    // (and a drag scrubs). Ctrl+middle-click reaches overlays as the middle pen (plain middle-drag
    // pans before overlays see it), so it picks without the mode.
    const bool pickMode = _imp->pickFocusMode.lock()->getValue();
    if (!((pickMode && pen == ePenTypeLMB) || pen == ePenTypeMMB)) {
        return false;
    }
    _imp->pickDragging = pickMode && pen == ePenTypeLMB;
    pickFocusAt(time, pos);
    return true;
}

bool
DeepDefocus::onOverlayPenMotion(double time, const RenderScale& /*renderScale*/, ViewIdx /*view*/,
                                const QPointF& /*viewportPos*/, const QPointF& pos, double /*pressure*/,
                                double /*timestamp*/)
{
    if (!_imp->pickDragging) {
        return false;
    }
    pickFocusAt(time, pos);
    return true;
}

bool
DeepDefocus::onOverlayPenUp(double /*time*/, const RenderScale& /*renderScale*/, ViewIdx /*view*/,
                            const QPointF& /*viewportPos*/, const QPointF& /*pos*/, double /*pressure*/,
                            double /*timestamp*/)
{
    const bool was = _imp->pickDragging;
    _imp->pickDragging = false;
    return was;
}

void
DeepDefocus::refreshFocusKnobs()
{
    KnobChoicePtr m = _imp->focusMode.lock();
    if (!m) {
        return;
    }
    const bool point = m->getValue() == 1;
    _imp->focalDistance.lock()->setSecret(point);
    _imp->focusPoint.lock()->setSecret(!point);
}

void
DeepDefocus::onKnobsLoaded()
{
    refreshFocusKnobs();
}

bool
DeepDefocus::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/, double time, bool /*originatedFromMainThread*/)
{
    if (_imp->focusMode.lock().get() == k) {
        refreshFocusKnobs();
        return true;
    }
    if (_imp->pickFocusBtn.lock().get() == k) {
        DeepImagePtr srcDeep = getDeepImageFromEffect(getInput(0).get());
        if (!srcDeep) {
            setPersistentMessage(eMessageTypeError, "No deep image connected.");
            return true;
        }
        const int zIdx = srcDeep->findChannelIndex("Z");
        if (zIdx < 0) {
            setPersistentMessage(eMessageTypeError, "No Z channel in deep image.");
            return true;
        }
        const RectI& dw = srcDeep->getDataWindow();
        const int fpx = std::max(dw.x1, std::min(dw.x2 - 1, (int)_imp->focusPointX.lock()->getValue()));
        const int fpy = std::max(dw.y1, std::min(dw.y2 - 1, (int)_imp->focusPointY.lock()->getValue()));
        const int yd = dw.y1 + dw.y2 - 1 - fpy;   // deep rows are top-down
        QReadLocker l(&srcDeep->getLock());
        const int n = srcDeep->getSampleCount(fpx, yd);
        if (n == 0) {
            setPersistentMessage(eMessageTypeWarning, "No deep samples at focus point.");
            return true;
        }
        const float* data = srcDeep->getSampleData(fpx, yd);
        const int nc = srcDeep->getNumChannels();
        float frontZ = data[zIdx];
        for (int s = 1; s < n; ++s) {
            frontZ = std::min(frontZ, data[s * nc + zIdx]);
        }
        KnobDoublePtr fd = _imp->focalDistance.lock();
        if (fd->isAnimated(0)) {
            fd->setValueAtTime(time, (double)frontZ, ViewSpec::all(), 0);
        } else {
            fd->setValue((double)frontZ);
        }
        clearPersistentMessage(false);
        return true;
    }
    return false;
}

StatusEnum
DeepDefocus::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                   ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

StatusEnum
DeepDefocus::render(const RenderActionArgs& args)
{
    const double time = args.time;
    DeepImagePtr srcDeep = getDeepImageFromEffect(getInput(0).get());
    if (!srcDeep || args.outputPlanes.empty()) {
        return eStatusFailed;
    }
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) {
        return eStatusFailed;
    }
    QReadLocker deepLock(&srcDeep->getLock());

    defocus::DeepView dv;
    dv.stride = srcDeep->getNumChannels();
    dv.iR = srcDeep->findChannelIndex("R");
    dv.iG = srcDeep->findChannelIndex("G");
    dv.iB = srcDeep->findChannelIndex("B");
    dv.iA = srcDeep->findChannelIndex("A");
    dv.iZ = srcDeep->findChannelIndex("Z");
    dv.iZBack = srcDeep->findChannelIndex("ZBack");
    if (dv.iR < 0 || dv.iA < 0 || dv.iZ < 0) {
        setPersistentMessage(eMessageTypeError, "The deep input needs R, A and Z channels (recolour an alpha-only "
                                                "deep image first, e.g. with DeepRecolor).");
        return eStatusFailed;
    }
    if (dv.iG < 0) dv.iG = dv.iR;
    if (dv.iB < 0) dv.iB = dv.iR;

    // Deep images are top-down within their data window, Natron images bottom-up.
    const RectI dw = srcDeep->getDataWindow();
    const int W = dw.width(), H = dw.height();
    if (W <= 0 || H <= 0) {
        return eStatusFailed;
    }
    const int flipY = dw.y1 + dw.y2 - 1;
    const DeepImage* dimg = srcDeep.get();
    dv.bounds = defocus::Rect(dw.x1, dw.y1, dw.x2, dw.y2);
    dv.at = [dimg, dw, flipY](int x, int y, int* n) -> const float* {
        if (x < dw.x1 || x >= dw.x2 || y < dw.y1 || y >= dw.y2) {
            *n = 0;
            return nullptr;
        }
        *n = dimg->getSampleCount(x, flipY - y);
        return *n > 0 ? dimg->getSampleData(x, flipY - y) : nullptr;
    };

    // Flattened image and front depth (silhouette, slicing outlines, background fill colours).
    std::vector<float> flatTop((size_t)W * H * 4, 0.f), flat((size_t)W * H * 4, 0.f), front((size_t)W * H, 0.f);
    srcDeep->flattenToRGBA(dw, flatTop.data(), W * 4 * (int)sizeof(float));
    for (int y = dw.y1; y < dw.y2; ++y) {
        const size_t rowB = (size_t)(y - dw.y1) * W, rowT = (size_t)(flipY - y - dw.y1) * W;
        std::copy(&flatTop[rowT * 4], &flatTop[rowT * 4] + (size_t)W * 4, &flat[rowB * 4]);
        for (int x = dw.x1; x < dw.x2; ++x) {
            int n = 0;
            const float* d = dv.at(x, y, &n);
            float zf = 0.f;
            for (int k = 0; k < n; ++k) {
                const float* q = d + (size_t)k * dv.stride;
                if (q[dv.iA] > 0.f && q[dv.iZ] > 0.f && (zf == 0.f || q[dv.iZ] < zf)) {
                    zf = q[dv.iZ];
                }
            }
            front[rowB + (x - dw.x1)] = zf;
        }
    }

    defocus::Inputs in;
    in.source.data = flat.data();
    in.source.bounds = dv.bounds;
    in.source.channels = 4;
    in.source.rowStride = (std::ptrdiff_t)W * 4;
    in.depth.data = front.data();
    in.depth.bounds = dv.bounds;
    in.depth.channels = 1;
    in.depth.rowStride = W;
    in.deep = dv;

    defocus::Settings s;
    s.depthConvention = defocus::eDepthPlanarZ;
    s.zeroIsInfinity = true;
    s.cocMode = defocus::eCocCamera;
    s.worldToMm = std::max(1e-6, _imp->unitScale.lock()->getValue());
    s.focalLengthMm = _imp->focalLength.lock()->getValueAtTime(time);
    s.fNumber = std::max(0.1, _imp->fStop.lock()->getValueAtTime(time));
    s.filmbackWidthMm = _imp->filmBack.lock()->getValue();
    s.focusDistance = std::max(1e-4, _imp->focalDistance.lock()->getValueAtTime(time));
    if (_imp->focusMode.lock()->getValue() == 1) {
        // autofocus: the nearest deep sample under Focus Point (unlocked: the render holds the
        // deep image's read lock, and depthNear takes its own)
        double d = 0.;
        KnobDoublePtr fp = _imp->focusPoint.lock();
        deepLock.unlock();
        const bool ok = depthNear(fp->getValueAtTime(time, 0), fp->getValueAtTime(time, 1), &d);
        deepLock.relock();
        if (ok) {
            s.focusDistance = d;
        }
    }
    const double gain = std::max(0., _imp->blurScale.lock()->getValueAtTime(time));
    s.nearGain = _imp->defocusForeground.lock()->getValue() ? gain : 0.;
    s.farGain = _imp->defocusBackground.lock()->getValue() ? gain : 0.;
    s.maxRadiusPx = _imp->maxBlur.lock()->getValue();
    s.formatWidthPx = W;
    s.opticalCentreX = 0.5 * (dw.x1 + dw.x2);
    s.opticalCentreY = 0.5 * (dw.y1 + dw.y2);
    s.fieldRadiusPx = 0.5 * std::sqrt((double)W * W + (double)H * H);
    s.lens.blades = _imp->apertureBlades.lock()->getValue();
    s.lens.rotationDeg = _imp->apertureRotation.lock()->getValue();
    s.border = defocus::eBorderBlack;
    s.renderScale = 1.;

    // Render over the output window, then copy (the output may have more channels or a
    // different window than the deep data).
    const RectI ob = outImg->getBounds();
    std::vector<float> res((size_t)ob.width() * ob.height() * 4, 0.f);
    defocus::ImageView ov;
    ov.data = res.data();
    ov.bounds = defocus::Rect(ob.x1, ob.y1, ob.x2, ob.y2);
    ov.channels = 4;
    ov.rowStride = (std::ptrdiff_t)ob.width() * 4;
    defocus::Stats st;
    if (!defocus::render(in, s, ov, nullptr, &st, [this]() { return aborted(); })) {
        return eStatusFailed;
    }
    {
        Image::WriteAccess wa(outImg.get());
        const int nc = outImg->getComponents().getNumComponents();
        for (int y = ob.y1; y < ob.y2; ++y) {
            for (int x = ob.x1; x < ob.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) {
                    continue;
                }
                const float* r = &res[((size_t)(y - ob.y1) * ob.width() + (x - ob.x1)) * 4];
                for (int c = 0; c < nc && c < 4; ++c) {
                    dst[c] = nc == 1 ? r[3] : r[c];
                }
            }
        }
    }

    std::ostringstream ss;
    ss << st.samples << " deep samples, " << st.slices << " slices, max blur "
       << (int)std::round(st.maxAbsRadius) << " px";
    _imp->info.lock()->setValue(ss.str());
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepDefocus.cpp"
