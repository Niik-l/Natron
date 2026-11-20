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

// TraceToMask implementation - see TraceToMask.h. The import half mirrors
// ReadMask::importFile (same RotoContext::addItem contract so the RotoPanel
// stays in sync); the tracing half lives in parsers/PotraceTrace.cpp.

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "TraceToMask.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <list>
#include <map>
#include <sstream>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/TimeLine.h"
#include "Global/Enums.h"

#include "Dev/Mask/VectorDocument.h"
#include "Dev/Mask/parsers/PotraceTrace.h"

#include "moc_TraceToMask.cpp"

NATRON_NAMESPACE_ENTER

struct TraceToMaskPrivate
{
    KnobPageWPtr page;
    KnobChoiceWPtr mode;
    KnobChoiceWPtr channel;
    KnobBoolWPtr invert;
    KnobDoubleWPtr preBlur;
    KnobDoubleWPtr threshold;
    KnobDoubleWPtr thresholdLow;
    KnobIntWPtr bands;
    KnobIntWPtr colorCount;
    KnobDoubleWPtr despeckle;
    KnobDoubleWPtr cornerThreshold;
    KnobBoolWPtr curveOptimization;
    KnobDoubleWPtr optTolerance;
    KnobDoubleWPtr simplify;
    KnobBoolWPtr holesAsXor;
    KnobChoiceWPtr fitMode;
    KnobDoubleWPtr customScale;
    KnobDoubleWPtr customOffset;
    KnobBoolWPtr invertY;
    KnobBoolWPtr importColors;
    KnobBoolWPtr autoRetrace;
    KnobButtonWPtr traceButton;
    KnobStringWPtr info;

    bool knobsCreated = false;

    /// Everything the last trace created (shapes + layers), for rebuild removal.
    std::list<std::weak_ptr<RotoItem> > importedItems;
};

namespace {

/**
 * @brief ReadMask's fit model (same 4 modes against the project format).
 */
void
calculateTransform(int fitModeIdx,
                   double userScaleX, double userScaleY,
                   double userOffsetX, double userOffsetY,
                   double docW, double docH,
                   double formatW, double formatH,
                   double* sx, double* sy, double* ox, double* oy)
{
    if (docW <= 0.0) {
        docW = formatW;
    }
    if (docH <= 0.0) {
        docH = formatH;
    }
    switch (fitModeIdx) {
    case 0: { // fit_format
        const double sxm = formatW / docW;
        const double sym = formatH / docH;
        const double uniform = std::min(sxm, sym) * 0.95;
        *sx = uniform * userScaleX;
        *sy = uniform * userScaleY;
        const double scaledW = docW * (*sx);
        const double scaledH = docH * (*sy);
        *ox = (formatW - scaledW) / 2.0 + userOffsetX;
        *oy = (formatH - scaledH) / 2.0 + userOffsetY;
        break;
    }
    case 1: { // center_format
        *sx = userScaleX;
        *sy = userScaleY;
        const double scaledW = docW * (*sx);
        const double scaledH = docH * (*sy);
        *ox = (formatW - scaledW) / 2.0 + userOffsetX;
        *oy = (formatH - scaledH) / 2.0 + userOffsetY;
        break;
    }
    case 2: // original
    case 3: // custom
    default:
        *sx = userScaleX;
        *sy = userScaleY;
        *ox = userOffsetX;
        *oy = userOffsetY;
        break;
    }
}

/**
 * @brief 3-pass box blur (approximates a gaussian) on a w*h float plane.
 */
void
blurPlane(std::vector<float>* values, int w, int h, int radius)
{
    if (radius <= 0 || values->size() != (std::size_t)w * h) {
        return;
    }
    std::vector<float> tmp((std::size_t)w * h, 0.0f);
    for (int pass = 0; pass < 3; ++pass) {
        // horizontal
        for (int y = 0; y < h; ++y) {
            float sum = 0.0f;
            const int span = radius * 2 + 1;
            for (int x = -radius; x <= radius; ++x) {
                const int cx = std::max(0, std::min(w - 1, x));
                sum += (*values)[(std::size_t)y * w + cx];
            }
            for (int x = 0; x < w; ++x) {
                tmp[(std::size_t)y * w + x] = sum / span;
                const int xOut = std::max(0, std::min(w - 1, x - radius));
                const int xIn = std::max(0, std::min(w - 1, x + radius + 1));
                sum += (*values)[(std::size_t)y * w + xIn] - (*values)[(std::size_t)y * w + xOut];
            }
        }
        // vertical
        for (int x = 0; x < w; ++x) {
            float sum = 0.0f;
            const int span = radius * 2 + 1;
            for (int y = -radius; y <= radius; ++y) {
                const int cy = std::max(0, std::min(h - 1, y));
                sum += tmp[(std::size_t)cy * w + x];
            }
            for (int y = 0; y < h; ++y) {
                (*values)[(std::size_t)y * w + x] = sum / span;
                const int yOut = std::max(0, std::min(h - 1, y - radius));
                const int yIn = std::max(0, std::min(h - 1, y + radius + 1));
                sum += tmp[(std::size_t)yIn * w + x] - tmp[(std::size_t)yOut * w + x];
            }
        }
    }
}

} // anonymous namespace

TraceToMask::TraceToMask(NodePtr node)
    : RotoNode(node)
    , _imp(new TraceToMaskPrivate())
{
}

TraceToMask::~TraceToMask()
{
}

void
TraceToMask::initializeData()
{
    if (_imp->knobsCreated) {
        return;
    }
    _imp->knobsCreated = true;

    // Knobs live in initializeData for the same reason as ReadMask: knob value
    // evaluation needs the node wired up; RotoPaint seals initializeKnobs.
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Trace"));
    _imp->page = page;
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
        k->setName("traceMode");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("bw", "B/W", "Binary outline trace through potrace (Inkscape's engine)."));
        opts.push_back(ChoiceOption("color", "Color", "k-means color clusters, each traced and stacked with its mean color (VTracer-style stacking)."));
        opts.push_back(ChoiceOption("centerline", "Centerline", "Skeletonize thin strokes into open Bezier curves (line-art)."));
        k->populateChoices(opts);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("The tracing variant. B/W for line-art/mattes, Color for "
                             "posterized multi-color masks, Centerline for thin strokes."));
        page->addKnob(k);
        _imp->mode = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Trace"));
        k->setName("trace");
        k->setHintToolTip(tr("Re-trace input 1 into shapes now, regardless of Auto "
                             "Re-trace. Tracing regenerates the shapes by design - "
                             "edits to traced shapes are wiped by the next trace."));
        k->setCheckable(false);
        page->addKnob(k);
        _imp->traceButton = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Auto Re-trace"));
        k->setName("autoRetrace");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Re-trace whenever a knob changes. Tracing regenerates the "
                             "shapes by design - user edits to traced shapes are wiped "
                             "by the next trace. Turn off to edit freely; the Trace "
                             "button still rebuilds on demand."));
        // Sit on the Trace row, to its right.
        k->setAddNewLine(false);
        page->addKnob(k);
        _imp->autoRetrace = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Channel"));
        k->setName("traceChannel");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("luma", "Luminance", "0.2126 R + 0.7152 G + 0.0722 B"));
        opts.push_back(ChoiceOption("alpha", "Alpha", "The alpha channel."));
        opts.push_back(ChoiceOption("r", "Red", "The red channel."));
        opts.push_back(ChoiceOption("g", "Green", "The green channel."));
        opts.push_back(ChoiceOption("b", "Blue", "The blue channel."));
        k->populateChoices(opts);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("The scalar channel thresholded/traced. Color mode always "
                             "clusters on RGB."));
        page->addKnob(k);
        _imp->channel = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Invert Source"));
        k->setName("invertSource");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Invert the channel before thresholding: on for dark ink "
                             "on a light plate, off for light marks on dark."));
        page->addKnob(k);
        _imp->invert = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Pre-Blur (px)"));
        k->setName("preBlur");
        k->setDefaultValue(0.0);
        k->setMinimum(0.0);
        k->setMaximum(20.0);
        k->setHintToolTip(tr("Gaussian-approximation blur applied before thresholding: "
                             "smooths noise and JPEG ringing so the traced curves stay "
                             "clean. 0 disables."));
        page->addKnob(k);
        _imp->preBlur = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Threshold"));
        k->setName("threshold");
        k->setDefaultValue(0.5);
        k->setMinimum(0.0);
        k->setMaximum(1.0);
        k->setHintToolTip(tr("Foreground = channel > threshold (after invert). B/W and "
                             "Centerline modes; Color mode clusters on RGB instead."));
        page->addKnob(k);
        _imp->threshold = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Threshold Low"));
        k->setName("thresholdLow");
        k->setDefaultValue(0.1);
        k->setMinimum(0.0);
        k->setMaximum(1.0);
        k->setHintToolTip(tr("With B/W Bands > 1: the low edge of the banded range "
                             "[Threshold Low, Threshold]."));
        page->addKnob(k);
        _imp->thresholdLow = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Bands"));
        k->setName("bands");
        k->setDefaultValue(1);
        k->setMinimum(1);
        k->setMaximum(16);
        k->setHintToolTip(tr("B/W mode: stack N threshold bands across "
                             "[Threshold Low, Threshold], like Inkscape's multiple "
                             "scans. 1 = single threshold."));
        page->addKnob(k);
        _imp->bands = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Colors"));
        k->setName("colorCount");
        k->setDefaultValue(4);
        k->setMinimum(2);
        k->setMaximum(16);
        k->setHintToolTip(tr("Color mode: number of k-means color clusters to trace, "
                             "stacked largest-area first with the cluster mean color."));
        page->addKnob(k);
        _imp->colorCount = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Despeckle"));
        k->setName("despeckle");
        k->setDefaultValue(2.0);
        k->setMinimum(0.0);
        k->setMaximum(100.0);
        k->setHintToolTip(tr("Drop traced areas smaller than this many pixels "
                             "(potrace turdsize)."));
        page->addKnob(k);
        _imp->despeckle = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Corner Threshold"));
        k->setName("cornerThreshold");
        k->setDefaultValue(1.0);
        k->setMinimum(0.0);
        k->setMaximum(1.334);
        k->setHintToolTip(tr("potrace alphamax: 0 = polygon (sharp corners), 1.334 = "
                             "smoothest. 1.0 balances corners and smoothing."));
        page->addKnob(k);
        _imp->cornerThreshold = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Curve Optimization"));
        k->setName("curveOptimization");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("potrace opticurve: replace collinear segments with longer "
                             "ones to reduce the control point count."));
        page->addKnob(k);
        _imp->curveOptimization = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Opt Tolerance"));
        k->setName("optTolerance");
        k->setDefaultValue(0.2);
        k->setMinimum(0.0);
        k->setMaximum(5.0);
        k->setHintToolTip(tr("potrace opttolerance: how much the optimized curve may "
                             "deviate from the ideal path (pixels)."));
        k->setAddNewLine(false);
        page->addKnob(k);
        _imp->optTolerance = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Simplify (px)"));
        k->setName("simplify");
        k->setDefaultValue(1.0);
        k->setMinimum(0.0);
        k->setMaximum(5.0);
        k->setHintToolTip(tr("Centerline mode: Ramer-Douglas-Peucker tolerance applied "
                             "to the skeleton chains before Bezier fitting."));
        page->addKnob(k);
        _imp->simplify = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Holes As XOR"));
        k->setName("holesAsXor");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("How traced holes punch their parent shape: off (default) "
                             "= Minus operator (hand-roto semantics, subtracting alpha); "
                             "on = XOR operator (even-odd, both shapes stay visible)."));
        page->addKnob(k);
        _imp->holesAsXor = k;
    }

    // ---------------- Transform page (the ReadMask fit model) ----------------
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Fit"));
        k->setName("fitMode");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("fit_format", "Fit to Format",
                                    "Uniform scale with a 95% margin, centered in the project format."));
        opts.push_back(ChoiceOption("center_format", "Center to Format",
                                    "Native size (1:1, times the custom scale), centered."));
        opts.push_back(ChoiceOption("original", "Original Size",
                                    "1:1 at the bottom-left origin."));
        opts.push_back(ChoiceOption("custom", "Custom",
                                    "Custom scale and offset only."));
        k->populateChoices(opts);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("How the source image maps onto the project format."));
        xformPage->addKnob(k);
        _imp->fitMode = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Custom Scale"), 2);
        k->setName("customScale");
        k->setDefaultValue(1.0, 0);
        k->setDefaultValue(1.0, 1);
        k->setAnimationEnabled(false);
        xformPage->addKnob(k);
        _imp->customScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Custom Offset"), 2);
        k->setName("customOffset");
        k->setDefaultValue(0.0, 0);
        k->setDefaultValue(0.0, 1);
        k->setAnimationEnabled(false);
        xformPage->addKnob(k);
        _imp->customOffset = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Invert Y"));
        k->setName("invertY");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("The traced frame is top-left; the Roto canvas is "
                             "bottom-left. On (default) the shape points AND tangent "
                             "handles flip in Y."));
        xformPage->addKnob(k);
        _imp->invertY = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Import Colors"));
        k->setName("importColors");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Apply the traced fill colors (Color mode cluster means) to "
                             "each imported shape's color knob."));
        xformPage->addKnob(k);
        _imp->importColors = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info");
        k->setAsLabel();
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        page->addKnob(k);
        _imp->info = k;
    }

    // Knob change notifications (knobChanged is sealed by RotoPaint - same as
    // ReadMask, connect to the knob signal handlers directly).
    struct ConnectEntry
    {
        KnobIPtr knob;
        bool isTrace;
    };
    const ConnectEntry entries[] = {
        { _imp->traceButton.lock(), true },
        { _imp->mode.lock(), false },
        { _imp->channel.lock(), false },
        { _imp->invert.lock(), false },
        { _imp->preBlur.lock(), false },
        { _imp->threshold.lock(), false },
        { _imp->thresholdLow.lock(), false },
        { _imp->bands.lock(), false },
        { _imp->colorCount.lock(), false },
        { _imp->despeckle.lock(), false },
        { _imp->cornerThreshold.lock(), false },
        { _imp->curveOptimization.lock(), false },
        { _imp->optTolerance.lock(), false },
        { _imp->simplify.lock(), false },
        { _imp->holesAsXor.lock(), false },
        { _imp->fitMode.lock(), false },
        { _imp->customScale.lock(), false },
        { _imp->customOffset.lock(), false },
        { _imp->invertY.lock(), false },
        { _imp->importColors.lock(), false },
        { _imp->autoRetrace.lock(), false },
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        const KnobSignalSlotHandlerPtr handler = entries[i].knob ? entries[i].knob->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
        if (!handler) {
            continue;
        }
        if (entries[i].isTrace) {
            QObject::connect( handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onTraceClicked()) );
        } else {
            QObject::connect( handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onControlKnobChanged(ViewSpec,int,int)) );
        }
    }
}

std::string
TraceToMask::getPluginDescription() const
{
    return "Bitmap tracing node that is a Roto node: input 1 (any raster) is traced "
           "into real Bezier shapes inside its own Roto tree and the output is the "
           "mask alpha like any Roto. Shapes are project native: editable in the "
           "viewer, serialized with the project, and consumable by MaskToGeo / "
           "Scene3D / CyclesRender.\n\n"
           "Modes: B/W (binary outline trace through the vendored potrace - Inkscape's "
           "engine - with optional stacked threshold bands), Color (k-means color "
           "clusters traced and stacked with their mean color, VTracer-style), and "
           "Centerline (skeletonization of thin strokes into open Beziers).\n\n"
           "Holes punch with Minus (default) or XOR (toggle). The Trace button re-runs "
           "on demand; Auto Re-trace re-runs whenever a knob changes. Tracing "
           "regenerates the shapes by design.";
}

std::string
TraceToMask::getInputLabel(int inputNb) const
{
    return inputNb == 0 ? "Source" : RotoNode::getInputLabel(inputNb);
}

void
TraceToMask::onTraceClicked()
{
    const double time = getNode()->getRotoContext() ? getNode()->getRotoContext()->getTimelineCurrentTime() : 0.0;
    trace(time);
}

void
TraceToMask::onControlKnobChanged(ViewSpec /*view*/, int /*dimension*/, int /*reason*/)
{
    const KnobBoolPtr autoKnob = _imp->autoRetrace.lock();
    if (autoKnob && !autoKnob->getValue()) {
        return;
    }
    const double time = getNode()->getRotoContext() ? getNode()->getRotoContext()->getTimelineCurrentTime() : 0.0;
    trace(time);
}

void
TraceToMask::removeImportedItems()
{
    const RotoContextPtr ctx = getNode()->getRotoContext();
    if (!ctx) {
        _imp->importedItems.clear();
        return;
    }
    // Remove in reverse insertion order: shapes first, layers last.
    for (std::list<std::weak_ptr<RotoItem> >::reverse_iterator it = _imp->importedItems.rbegin();
         it != _imp->importedItems.rend(); ++it) {
        const RotoItemPtr item = it->lock();
        if (!item) {
            continue;
        }
        ctx->removeItem(item);
    }
    _imp->importedItems.clear();
}

void
TraceToMask::trace(double time)
{
    RotoContextPtr ctx = getNode()->getRotoContext();
    if (!ctx) {
        return;
    }
    const KnobChoicePtr channelKnob = _imp->channel.lock();
    const int channelIdx = channelKnob ? channelKnob->getValue() : 0;
    const KnobChoicePtr modeKnob = _imp->mode.lock();
    const int modeIdx = modeKnob ? modeKnob->getValue() : 0;
    // Request the plane explicitly: a NULL layer makes getImage fall back to
    // the input's clip-preferred components, which for a Roto node's input is
    // Alpha-only - pixel buffers would then be single-component and the RGBA
    // read below would run off the end of each pixel. Alpha is requested for
    // the alpha channel (and alpha-only sources like another Roto/mask node);
    // RGBA otherwise (luma/R/G/B channels and Color-mode clustering).
    const ImagePlaneDesc reqComps = (channelIdx == 1 && modeIdx != 1)
                                        ? ImagePlaneDesc::getAlphaComponents()
                                        : ImagePlaneDesc::getRGBAComponents();
    const ImagePtr srcImg = getImage(0, time, RenderScale(), ViewIdx(0),
                                     NULL, &reqComps, false, true,
                                     eStorageModeRAM, 0, NULL);
    if (!srcImg) {
        removeImportedItems();
        return;
    }
    const RectI bounds = srcImg->getBounds();
    const int w = bounds.width();
    const int h = bounds.height();
    if (w <= 1 || h <= 1) {
        removeImportedItems();
        return;
    }

    const KnobDoublePtr blurKnob = _imp->preBlur.lock();
    const double preBlur = blurKnob ? blurKnob->getValue() : 0.0;
    const KnobBoolPtr colorKnob = _imp->importColors.lock();
    const bool importColors = colorKnob ? colorKnob->getValue() : true;

    std::vector<float> values((std::size_t)w * h, 0.0f);
    std::vector<float> valuesRGB;
    {
        // The fetched plane may be RGBA (4 comps) or Alpha (1 comp); buffers
        // are tightly packed per component, so index by the actual count -
        // a hardcoded alpha=p[3] would read off the end of an alpha buffer.
        const int nComps = srcImg->getComponents().getNumComponents();
        Image::ReadAccess acc(srcImg.get());
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            for (int x = bounds.x1; x < bounds.x2; ++x) {
                const float* p = (const float*)acc.pixelAt(x, y);
                if (!p) {
                    continue;
                }
                const std::size_t idx = (std::size_t)(y - bounds.y1) * w + (x - bounds.x1);
                if (nComps == 1) {
                    // Alpha-only source: the single component is the scalar
                    // value regardless of the channel knob.
                    values[idx] = p[0];
                    continue;
                }
                switch (channelIdx) {
                case 1:
                    values[idx] = p[3];
                    break;
                case 2:
                    values[idx] = p[0];
                    break;
                case 3:
                    values[idx] = p[1];
                    break;
                case 4:
                    values[idx] = p[2];
                    break;
                case 0:
                default:
                    values[idx] = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
                    break;
                }
            }
        }
        if (modeIdx == 1 && nComps >= 3) {
            valuesRGB.resize((std::size_t)w * h * 3, 0.0f);
            for (int y = bounds.y1; y < bounds.y2; ++y) {
                for (int x = bounds.x1; x < bounds.x2; ++x) {
                    const float* p = (const float*)acc.pixelAt(x, y);
                    if (!p) {
                        continue;
                    }
                    const std::size_t idx = (std::size_t)(y - bounds.y1) * w + (x - bounds.x1);
                    valuesRGB[idx * 3 + 0] = p[0];
                    valuesRGB[idx * 3 + 1] = p[1];
                    valuesRGB[idx * 3 + 2] = p[2];
                }
            }
        }
    }

    if (preBlur > 0.0) {
        blurPlane(&values, w, h, (int)std::max(1.0, preBlur));
    }

    VectorMask::TraceParams params;
    switch (modeIdx) {
    case 1:
        params.mode = VectorMask::eTraceModeColor;
        break;
    case 2:
        params.mode = VectorMask::eTraceModeCenterline;
        break;
    case 0:
    default:
        params.mode = VectorMask::eTraceModeBW;
        break;
    }
    {
        const KnobDoublePtr k = _imp->threshold.lock();
        if (k) params.threshold = k->getValue();
        const KnobDoublePtr kl = _imp->thresholdLow.lock();
        if (kl) params.thresholdLow = kl->getValue();
        const KnobIntPtr kb = _imp->bands.lock();
        if (kb) params.bands = kb->getValue();
        const KnobIntPtr kc = _imp->colorCount.lock();
        if (kc) params.colorCount = kc->getValue();
        const KnobDoublePtr kd = _imp->despeckle.lock();
        if (kd) params.despeckle = kd->getValue();
        const KnobDoublePtr kct = _imp->cornerThreshold.lock();
        if (kct) params.cornerThreshold = kct->getValue();
        const KnobBoolPtr kco = _imp->curveOptimization.lock();
        if (kco) params.curveOptimization = kco->getValue();
        const KnobDoublePtr kot = _imp->optTolerance.lock();
        if (kot) params.optTolerance = kot->getValue();
        const KnobDoublePtr ks = _imp->simplify.lock();
        if (ks) params.simplifyTolerance = ks->getValue();
        const KnobBoolPtr kx = _imp->holesAsXor.lock();
        if (kx) params.holeOperator = kx->getValue() ? (int)eMergeXOR : (int)eMergeMinus;
        const KnobBoolPtr ki = _imp->invert.lock();
        if (ki) params.invert = ki->getValue();
    }

    VectorMask::VectorDocument doc;
    if (!VectorMask::tracePlaneToDocument(w, h, values, valuesRGB, params, "Trace", &doc)) {
        removeImportedItems();
        if (KnobStringPtr info = _imp->info.lock()) {
            info->setValue("0 shapes (nothing traced)");
        }
        return;
    }

    // The trace succeeded: only now discard the previous import.
    removeImportedItems();

    Format fmt;
    getApp()->getProject()->getProjectDefaultFormat(&fmt);
    const double formatW = fmt.x2 - fmt.x1;
    const double formatH = fmt.y2 - fmt.y1;

    const KnobChoicePtr fitKnob = _imp->fitMode.lock();
    const KnobDoublePtr scaleKnob = _imp->customScale.lock();
    const KnobDoublePtr offsetKnob = _imp->customOffset.lock();
    const KnobBoolPtr invertYKnob = _imp->invertY.lock();
    const bool flipY = invertYKnob ? invertYKnob->getValue() : true;

    double sx = 1.0, sy = 1.0, ox = 0.0, oy = 0.0;
    calculateTransform(fitKnob ? fitKnob->getValue() : 0,
                       scaleKnob ? scaleKnob->getValue(0) : 1.0,
                       scaleKnob ? scaleKnob->getValue(1) : 1.0,
                       offsetKnob ? offsetKnob->getValue(0) : 0.0,
                       offsetKnob ? offsetKnob->getValue(1) : 0.0,
                       doc.width, doc.height,
                       formatW, formatH,
                       &sx, &sy, &ox, &oy);

    const double docHeight = doc.height > 0.0 ? doc.height : formatH;

    RotoLayerPtr baseLayer = ctx->getOrCreateBaseLayer();
    RotoLayerPtr mainLayer;
    const std::string mainLabel = "Trace";
    mainLayer.reset(new RotoLayer(ctx, mainLabel, RotoLayerPtr()));
    if (baseLayer) {
        ctx->addItem(baseLayer, (int)baseLayer->getItems().size(), mainLayer, RotoItem::eSelectionReasonOther);
    } else {
        ctx->addItem(RotoLayerPtr(), 0, mainLayer, RotoItem::eSelectionReasonOther);
    }
    _imp->importedItems.push_back(mainLayer);

    beginChanges();

    int shapeIdx = 0;
    for (std::vector<VectorMask::VectorShape>::const_iterator it = doc.shapes.begin();
         it != doc.shapes.end(); ++it) {
        if (it->vertices.empty()) {
            continue;
        }
        const VectorMask::VectorShape shape =
            VectorMask::transformShape(*it, sx, sy, ox, oy, flipY, docHeight);

        // RotoContext::generateUniqueName appends its own counter to the base
        // (Traced1, Traced2, ...), so pass a fixed base rather than the
        // potrace path name.
        const std::string shapeName = ctx->generateUniqueName("Traced");
        BezierPtr curve(new Bezier(ctx, shapeName, RotoLayerPtr(), !shape.closed));
        curve->createNodes();
        ctx->addItem(mainLayer, (int)mainLayer->getItems().size(), curve, RotoItem::eSelectionReasonOther);
        _imp->importedItems.push_back(curve);

        for (std::vector<VectorMask::ShapeVertex>::const_iterator v = shape.vertices.begin();
             v != shape.vertices.end(); ++v) {
            curve->addControlPoint(v->center.x, v->center.y, time);
        }
        for (size_t i = 0; i < shape.vertices.size(); ++i) {
            const VectorMask::ShapeVertex& v = shape.vertices[i];
            const double lx = v.center.x + v.leftTangent.x;
            const double ly = v.center.y + v.leftTangent.y;
            const double rx = v.center.x + v.rightTangent.x;
            const double ry = v.center.y + v.rightTangent.y;
            curve->setPointAtIndex(false, (int)i, time, v.center.x, v.center.y, lx, ly, rx, ry);
            curve->setPointAtIndex(true, (int)i, time, v.center.x, v.center.y, lx, ly, rx, ry);
        }
        if (shape.closed) {
            curve->setCurveFinished(true);
        }
        if (importColors && shape.hasFillColor) {
            curve->setColor(time, shape.fillColor.r, shape.fillColor.g, shape.fillColor.b);
            curve->setOpacity(shape.opacity * shape.fillColor.a, time);
        }
        // Traced holes punch with the configured operator.
        if (shape.compositingOperator >= 0) {
            curve->setCompositingOperator(shape.compositingOperator);
        }
        // vectorNuke feather zeroing.
        curve->setFeatherDistance(0.0, time);
        curve->setFeatherFallOff(0.0, time);
        ++shapeIdx;
    }

    endChanges(true);

    ctx->evaluateChange();

    if (KnobStringPtr info = _imp->info.lock()) {
        std::ostringstream ss;
        ss << shapeIdx << " shapes, " << (int)doc.shapes.size() << " traced paths";
        info->setValue(ss.str());
    }
}

NATRON_NAMESPACE_EXIT
