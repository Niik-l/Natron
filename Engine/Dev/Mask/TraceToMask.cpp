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
#include <vector>

#include <QDateTime>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QString>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/AbortableRenderInfo.h"
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
#include "Engine/TLSHolder.h"
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
    KnobIntWPtr maxShapes;
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
    KnobIntWPtr bakeFrom;
    KnobIntWPtr bakeTo;
    KnobButtonWPtr bakeSequence;
    KnobButtonWPtr traceButton;
    KnobButtonWPtr resetButton;
    KnobStringWPtr info;

    bool knobsCreated = false;

    /// Everything the last trace created (shapes + layers), for rebuild removal.
    std::list<std::weak_ptr<RotoItem> > importedItems;

    /// Result of one background trace job: the plane fetch AND the heavy
    /// compute (potrace/cluster/skeleton) both run on the worker thread, so
    /// the main thread only pumps the GUI and imports the result.
    struct TraceJobResult
    {
        bool ok = false;
        std::string error;
        std::vector<float> values;
        std::vector<float> valuesRGB;
        int w = 0, h = 0;
        VectorMask::TraceParams params;
        RectI bounds;
        VectorMask::VectorDocument doc;
    };

    /// The heavy compute runs on the global thread pool; the nested event loop
    /// below pumps the GUI until this watcher reports the result.
    QFutureWatcher<TraceJobResult>* traceWatcher = NULL;

    /// Re-entrancy: knob edits arriving while a trace runs are coalesced into
    /// one follow-up trace (traceDirty), never nested.
    bool traceRunning = false;
    bool traceDirty = false;

    /// The last successful trace's document, kept so raising Max Shapes can
    /// import it without re-computing.
    VectorMask::VectorDocument lastDoc;
    RectI lastDocBounds;
    bool hasLastDoc = false;

    /// Canonical integer bounds of the plane the current shapes were traced
    /// from - the anchor for the fit transform and the input-change check.
    RectI lastTracedBounds;
    bool hasLastTracedBounds = false;

    /// The input node's knobs age when the current shapes were traced: any
    /// upstream edit (image swap, resize, grade...) bumps it, which is the
    /// change signal for Auto Re-trace.
    U64 lastTracedInputAge = 0;
    bool hasLastTracedInputAge = false;

    /// Input-change detection is deferred to a zero-timer: see
    /// onInputMayHaveChanged. Coalesces bursts of knob edits into one check.
    QTimer* inputCheckTimer = NULL;
    bool inputDirty = false;

    /// The input node currently hooked for knob edits (rewired on re-connect).
    NodeWPtr hookedInput;
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
                   double bx, double by,
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
    case 2: // original: 1:1 anchored at the source plane's position
    case 3: // custom: user scale/offset anchored at the source plane's position
    default:
        *sx = userScaleX;
        *sy = userScaleY;
        *ox = bx + userOffsetX;
        *oy = by + userOffsetY;
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
    _imp->traceWatcher = new QFutureWatcher<TraceToMaskPrivate::TraceJobResult>(this);
    _imp->inputCheckTimer = new QTimer(this);
    _imp->inputCheckTimer->setSingleShot(true);
    QObject::connect( _imp->inputCheckTimer, SIGNAL(timeout()), this, SLOT(onInputCheckTimeout()) );
}

TraceToMask::~TraceToMask()
{
    // A running background job may still hold this; wait it out (it only
    // computes - it never touches the RotoContext).
    if (_imp->traceWatcher && _imp->traceWatcher->isRunning()) {
        _imp->traceWatcher->future().waitForFinished();
    }
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
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Reset"));
        k->setName("reset");
        k->setHintToolTip(tr("Remove every traced shape and layer from this node's roto "
                             "tree now (no re-trace; the Trace button rebuilds on demand)."));
        k->setCheckable(false);
        // Sit on the Trace row too.
        k->setAddNewLine(false);
        page->addKnob(k);
        _imp->resetButton = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Bake From"));
        k->setName("bakeFrom");
        k->setDefaultValue(1);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("First frame of the Bake Sequence range."));
        page->addKnob(k);
        _imp->bakeFrom = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Bake To"));
        k->setName("bakeTo");
        k->setDefaultValue(1);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Last frame of the Bake Sequence range."));
        page->addKnob(k);
        _imp->bakeTo = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Bake Sequence"));
        k->setName("bakeSequence");
        k->setHintToolTip(tr("Trace input 1 at every frame in [Bake From, Bake To] and "
                             "key the control points of the current shapes at each frame: "
                             "the traced animation becomes project-native animated "
                             "BAcziers, independent of the input afterwards. Shapes whose "
                             "control point count changes across frames keep their "
                             "previous keys (topology changes cannot be keyed). Auto "
                             "Re-trace is turned off so the baked animation survives."));
        k->setCheckable(false);
        page->addKnob(k);
        _imp->bakeSequence = k;
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
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Shapes"));
        k->setName("maxShapes");
        k->setDefaultValue(8);
        k->setMinimum(1);
        k->setMaximum(100000);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Import valve: a trace of more shapes than this is refused "
                             "with its previous import kept. Shape count is not the number "
                             "of colors/bands: each color cluster's or band's bitmap "
                             "decomposes into ONE shape per connected region (plus one per "
                             "hole), and photographic content scatters every cluster into "
                             "thousands of tiny speckles - 4 Colors on a photo easily means "
                             "tens of thousands of contours. Raise Despeckle to drop small "
                             "speckles, or Max Shapes to allow bigger imports (each shape "
                             "carries two internal roto nodes)."));
        page->addKnob(k);
        _imp->maxShapes = k;
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
                                    "1:1 anchored at the source image's own position - the traces sit exactly on it."));
        opts.push_back(ChoiceOption("custom", "Custom",
                                    "Custom scale and offset, anchored at the source image's position."));
        k->populateChoices(opts);
        // A tracer's default must land the shapes ON the source image: 1:1 at
        // the image's own position (Fit-to-Format is for vector-file imports).
        k->setDefaultValue(2);
        k->setHintToolTip(tr("How the traced document maps onto the project. Original "
                             "Size (default) anchors 1:1 at the source image itself, so "
                             "the traces match its position and aspect; the other modes "
                             "map into the project format like ReadMask's."));
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
        int action; // 0 = option knob (auto re-trace), 1 = Trace, 2 = Bake, 3 = Reset, 4 = Max Shapes
    };
    const ConnectEntry entries[] = {
        { _imp->traceButton.lock(), 1 },
        { _imp->bakeSequence.lock(), 2 },
        { _imp->resetButton.lock(), 3 },
        { _imp->maxShapes.lock(), 4 },
        { _imp->mode.lock(), 0 },
        { _imp->channel.lock(), 0 },
        { _imp->invert.lock(), 0 },
        { _imp->preBlur.lock(), 0 },
        { _imp->threshold.lock(), 0 },
        { _imp->thresholdLow.lock(), 0 },
        { _imp->bands.lock(), 0 },
        { _imp->colorCount.lock(), 0 },
        { _imp->despeckle.lock(), 0 },
        { _imp->cornerThreshold.lock(), 0 },
        { _imp->curveOptimization.lock(), 0 },
        { _imp->optTolerance.lock(), 0 },
        { _imp->simplify.lock(), 0 },
        { _imp->holesAsXor.lock(), 0 },
        { _imp->fitMode.lock(), 0 },
        { _imp->customScale.lock(), 0 },
        { _imp->customOffset.lock(), 0 },
        { _imp->invertY.lock(), 0 },
        { _imp->importColors.lock(), 0 },
        { _imp->autoRetrace.lock(), 0 },
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        const KnobSignalSlotHandlerPtr handler = entries[i].knob ? entries[i].knob->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
        if (!handler) {
            continue;
        }
        switch (entries[i].action) {
        case 1:
            QObject::connect( handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onTraceClicked()) );
            break;
        case 2:
            QObject::connect( handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onBakeClicked()) );
            break;
        case 3:
            QObject::connect( handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onResetClicked()) );
            break;
        case 4:
            QObject::connect( handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onMaxShapesChanged()) );
            break;
        default:
            QObject::connect( handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onControlKnobChanged(ViewSpec,int,int)) );
            break;
        }
    }

    // Timeline scrub / input knob edits: re-trace when the input's region of
    // definition changed underneath the current shapes. Connection changes go
    // through the onInputChanged override.
    const TimeLinePtr timeline = getApp() ? getApp()->getTimeLine() : TimeLinePtr();
    if (timeline) {
        QObject::connect( timeline.get(), SIGNAL(frameChanged(SequenceTime,int)),
                          this, SLOT(onInputMayHaveChanged()) );
    }
    updateInputHooks();
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
TraceToMask::computeDocTransform(double docW, double docH, double bx, double by,
                                 double* sx, double* sy, double* ox, double* oy,
                                 bool* flipY, double* docHeight)
{
    Format fmt;
    getApp()->getProject()->getProjectDefaultFormat(&fmt);
    const double formatW = fmt.x2 - fmt.x1;
    const double formatH = fmt.y2 - fmt.y1;

    const KnobChoicePtr fitKnob = _imp->fitMode.lock();
    const KnobDoublePtr scaleKnob = _imp->customScale.lock();
    const KnobDoublePtr offsetKnob = _imp->customOffset.lock();
    const KnobBoolPtr invertYKnob = _imp->invertY.lock();
    *flipY = invertYKnob ? invertYKnob->getValue() : true;

    calculateTransform(fitKnob ? fitKnob->getValue() : 0,
                       scaleKnob ? scaleKnob->getValue(0) : 1.0,
                       scaleKnob ? scaleKnob->getValue(1) : 1.0,
                       offsetKnob ? offsetKnob->getValue(0) : 0.0,
                       offsetKnob ? offsetKnob->getValue(1) : 0.0,
                       docW, docH, formatW, formatH, bx, by, sx, sy, ox, oy);
    *docHeight = docH > 0.0 ? docH : formatH;
}

bool
TraceToMask::fetchPlaneAndParams(double time,
                                 const RectD& inputRod,
                                 std::vector<float>* valuesOut,
                                 std::vector<float>* valuesRGBOut,
                                 int* wOut, int* hOut,
                                 VectorMask::TraceParams* paramsOut,
                                 RectI* boundsOut)
{
    const KnobChoicePtr channelKnob = _imp->channel.lock();
    const int channelIdx = channelKnob ? channelKnob->getValue() : 0;
    const KnobChoicePtr modeKnob = _imp->mode.lock();
    const int modeIdx = modeKnob ? modeKnob->getValue() : 0;
    // Request the plane explicitly: Alpha for the alpha channel (and
    // alpha-only sources like another Roto/mask node), RGBA otherwise (luma/
    // R/G/B channels and Color-mode clustering).
    const ImagePlaneDesc reqComps = (channelIdx == 1 && modeIdx != 1)
                                        ? ImagePlaneDesc::getAlphaComponents()
                                        : ImagePlaneDesc::getRGBAComponents();

    // Fetch the input through renderRoI with fresh per-thread render args
    // (the TrackMarker::getMarkerImage pattern), over the caller-queried RoD.
    // Going through getImage from outside a render action would reuse the
    // thread's leftover render TLS - whose node hash still points at the
    // pre-edit state, so after an image swap/resize the fetch would silently
    // return the OLD image. The setter below pushes fresh args (isAnalysis:
    // no stale request pass) and the thread's TLS is cleaned up afterwards.
    NodePtr inputNode = getNode() ? getNode()->getInput(0) : NodePtr();
    if (!inputNode || !inputNode->getEffectInstance()) {
        return false;
    }

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    AbortableThread* isAbortable = dynamic_cast<AbortableThread*>(QThread::currentThread());
    if (isAbortable) {
        isAbortable->setAbortInfo(true, abortInfo, getNode()->getEffectInstance());
    }
    ParallelRenderArgsSetter frameRenderArgs(time,
                                             ViewIdx(0),
                                             true,   // isRenderUserInteraction
                                             false,  // isSequentialRender
                                             abortInfo,
                                             getNode(),
                                             0,      // textureIndex
                                             getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             true,   // isAnalysis
                                             false,  // draftMode
                                             RenderStatsPtr());

    // The reader would otherwise consume the project's auto-format on this
    // off-schedule render (renderRoIInternal sets it from non-main threads);
    // suppress + restore so the project format keeps following the first
    // regular render, not our trace fetch.
    const ProjectPtr proj = getApp() ? getApp()->getProject() : ProjectPtr();
    const bool wasAutoFormat = proj ? proj->isAutoSetProjectFormatEnabled() : false;
    if (wasAutoFormat) {
        proj->setAutoSetProjectFormatEnabled(false);
    }

    std::list<ImagePlaneDesc> comps;
    comps.push_back(reqComps);
    const double par = getAspectRatio(0);
    const RectI roiPixel = inputRod.toPixelEnclosing(0, par);
    EffectInstance::RenderRoIArgs args(time,
                                       RenderScale::identity,
                                       0,        // mipmapLevel
                                       ViewIdx(0),
                                       false,    // byPassCache
                                       roiPixel,
                                       inputRod, // preComputedRoD: queried on the main thread
                                       comps,
                                       eImageBitDepthFloat,
                                       true,     // fetchUserSelectedComponentsUpstream
                                       this,     // caller
                                       eStorageModeRAM,
                                       time);
    std::map<ImagePlaneDesc, ImagePtr> planes;
    const EffectInstance::RenderRoIRetCode stat = inputNode->getEffectInstance()->renderRoI(args, &planes);

    if (wasAutoFormat && proj) {
        proj->setAutoSetProjectFormatEnabled(true);
    }

    appPTR->getAppTLS()->cleanupTLSForThread();
    if (stat != EffectInstance::eRenderRoIRetCodeOk || planes.empty()) {
        return false;
    }
    const ImagePtr srcImg = planes.begin()->second;
    if (!srcImg) {
        return false;
    }
    const RectI bounds = srcImg->getBounds();
    const int w = bounds.width();
    const int h = bounds.height();
    if (w <= 1 || h <= 1) {
        return false;
    }

    const KnobDoublePtr blurKnob = _imp->preBlur.lock();
    const double preBlur = blurKnob ? blurKnob->getValue() : 0.0;

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

    *valuesOut = std::move(values);
    if (valuesRGBOut) {
        *valuesRGBOut = std::move(valuesRGB);
    }
    *wOut = w;
    *hOut = h;
    *paramsOut = params;
    *boundsOut = bounds;
    return true;
}

bool
TraceToMask::buildTraceDocument(double time, VectorMask::VectorDocument* doc, RectI* bounds)
{
    // Main-thread path (bake): query the input's RoD here, fresh.
    NodePtr inputNode = getNode() ? getNode()->getInput(0) : NodePtr();
    if (!inputNode || !inputNode->getEffectInstance()) {
        return false;
    }
    RectD inputRod;
    {
        RectD rod;
        bool isProjectFormat = false;
        const StatusEnum st = inputNode->getEffectInstance()->getRegionOfDefinition_public(
            inputNode->getHashValue(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);
        if (st != eStatusOK || rod.isNull()) {
            return false;
        }
        inputRod = rod;
    }
    std::vector<float> values, valuesRGB;
    int w = 0, h = 0;
    VectorMask::TraceParams params;
    if (!fetchPlaneAndParams(time, inputRod, &values, &valuesRGB, &w, &h, &params, bounds)) {
        return false;
    }
    return VectorMask::tracePlaneToDocument(w, h, values, valuesRGB, params, "Trace", doc);
}

void
TraceToMask::importDocument(const VectorMask::VectorDocument& doc, const RectI& bounds, double time)
{
    RotoContextPtr ctx = getNode()->getRotoContext();
    if (!ctx) {
        return;
    }
    const KnobBoolPtr colorKnob = _imp->importColors.lock();
    const bool importColors = colorKnob ? colorKnob->getValue() : true;

    // The import replaces the previous one only once the new trace succeeded.
    removeImportedItems();

    double sx = 1.0, sy = 1.0, ox = 0.0, oy = 0.0;
    bool flipY = true;
    double docHeight = doc.height;
    // Original/Custom fit modes anchor at the source plane's own position so
    // the traces sit exactly on the image; Fit/Center map into the format.
    computeDocTransform(doc.width, doc.height, bounds.x1, bounds.y1,
                        &sx, &sy, &ox, &oy, &flipY, &docHeight);
    _imp->lastTracedBounds = bounds;
    _imp->hasLastTracedBounds = true;
    {
        NodePtr input = getNode()->getInput(0);
        if (input) {
            _imp->lastTracedInputAge = input->getKnobsAge();
            _imp->hasLastTracedInputAge = true;
        }
    }
    // The input is definitely wired by now: (re)attach the input-change hooks
    // so a later image swap/resize is detected.
    updateInputHooks();

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

    // Stacked bands (and color clusters) each carry their own doc layer name;
    // group their shapes into one sub-layer per band/cluster so the RotoPanel
    // shows the stack and bands can be toggled independently.
    std::map<std::string, RotoLayerPtr> subLayers;

    int shapeIdx = 0;
    for (std::vector<VectorMask::VectorShape>::const_iterator it = doc.shapes.begin();
         it != doc.shapes.end(); ++it) {
        if (it->vertices.empty()) {
            continue;
        }
        const VectorMask::VectorShape shape =
            VectorMask::transformShape(*it, sx, sy, ox, oy, flipY, docHeight);

        RotoLayerPtr dest = mainLayer;
        if (!shape.layerName.empty() && shape.layerName != mainLabel) {
            const std::string safeLayer =
                VectorMask::sanitizeName(shape.layerName, "Trace");
            std::map<std::string, RotoLayerPtr>::iterator found = subLayers.find(safeLayer);
            if (found == subLayers.end()) {
                RotoLayerPtr sub(new RotoLayer(ctx, safeLayer, RotoLayerPtr()));
                ctx->addItem(mainLayer, (int)mainLayer->getItems().size(), sub, RotoItem::eSelectionReasonOther);
                subLayers[safeLayer] = sub;
                _imp->importedItems.push_back(sub);
                dest = sub;
            } else {
                dest = found->second;
            }
        }

        // RotoContext::generateUniqueName appends its own counter to the base
        // (Traced1, Traced2, ...), so pass a fixed base rather than the
        // potrace path name.
        const std::string shapeName = ctx->generateUniqueName("Traced");
        BezierPtr curve(new Bezier(ctx, shapeName, RotoLayerPtr(), !shape.closed));
        curve->createNodes();
        ctx->addItem(dest, (int)dest->getItems().size(), curve, RotoItem::eSelectionReasonOther);
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

void
TraceToMask::trace(double time)
{
    RotoContextPtr ctx = getNode()->getRotoContext();
    if (!ctx) {
        return;
    }
    if (_imp->traceRunning) {
        // A trace is already in flight (its event loop is pumping the GUI):
        // coalesce this request into one follow-up instead of nesting.
        _imp->traceDirty = true;
        return;
    }
    _imp->traceRunning = true;

    for (;;) {
        _imp->traceDirty = false;

        // The input's RoD is queried here on the main thread (cheap, cached,
        // fresh hash) and handed to the worker as the fetch region.
        NodePtr inputNode = getNode() ? getNode()->getInput(0) : NodePtr();
        RectD inputRod;
        bool haveRod = false;
        if (inputNode && inputNode->getEffectInstance()) {
            RectD rod;
            bool isProjectFormat = false;
            const StatusEnum st = inputNode->getEffectInstance()->getRegionOfDefinition_public(
                inputNode->getHashValue(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);
            if (st == eStatusOK && !rod.isNull()) {
                inputRod = rod;
                haveRod = true;
            }
        }

        // Fetch AND heavy compute (render upstream, potrace / k-means /
        // thinning) on the worker thread: the main thread only pumps the GUI
        // (nested event loop below) and imports the result into the
        // RotoContext. The worker carries no stale render TLS, so the fetch
        // sees the input's CURRENT state after a swap/resize.
        if (!haveRod) {
            removeImportedItems();
            if (KnobStringPtr info = _imp->info.lock()) {
                info->setValue("0 shapes (nothing traced)");
            }
        } else {
            _imp->traceWatcher->setFuture(QtConcurrent::run([this, time, inputRod]() -> TraceToMaskPrivate::TraceJobResult {
                TraceToMaskPrivate::TraceJobResult r;
                try {
                    if (!fetchPlaneAndParams(time, inputRod, &r.values, &r.valuesRGB, &r.w, &r.h, &r.params, &r.bounds)) {
                        r.ok = false;
                        return r;
                    }
                    r.ok = VectorMask::tracePlaneToDocument(r.w, r.h, r.values, r.valuesRGB, r.params, "Trace", &r.doc);
                } catch (const std::exception& e) {
                    r.ok = false;
                    r.error = e.what();
                } catch (...) {
                    r.ok = false;
                    r.error = "unknown tracing error";
                }
                return r;
            }));
            QEventLoop loop;
            QObject::connect(_imp->traceWatcher, SIGNAL(finished()), &loop, SLOT(quit()));
            // Exclude user input while pumping: the window keeps painting (the
            // UI does not look frozen) but clicks/keys cannot re-enter the
            // trace or the settings panel mid-import.
            loop.exec(QEventLoop::ExcludeUserInputEvents);
            QObject::disconnect(_imp->traceWatcher, SIGNAL(finished()), &loop, SLOT(quit()));

            const TraceToMaskPrivate::TraceJobResult res = _imp->traceWatcher->result();
            if (!res.ok) {
                removeImportedItems();
                if (KnobStringPtr info = _imp->info.lock()) {
                    info->setValue(res.error.empty() ? std::string("0 shapes (nothing traced)")
                                                     : std::string("trace failed: ") + res.error);
                }
            } else {
                // Safety valve: a photographic source in Color/B/W mode can
                // decompose into thousands of contours; importing that many
                // Beziers (2 internal nodes each) stalls the UI for minutes
                // and can exhaust memory. Refuse and point at Max Shapes /
                // Despeckle instead - the previous import is kept, and the
                // refused document stays cached so raising Max Shapes imports
                // it without re-tracing.
                const KnobIntPtr maxKnob = _imp->maxShapes.lock();
                const int maxShapes = maxKnob ? maxKnob->getValue() : 8;
                const int pathCount = (int)res.doc.shapes.size();
                _imp->lastDoc = res.doc;
                _imp->lastDocBounds = res.bounds;
                _imp->hasLastDoc = true;
                if (pathCount > maxShapes) {
                    if (KnobStringPtr info = _imp->info.lock()) {
                        std::ostringstream ss;
                        ss << pathCount << " shapes traced - too complex to import; raise Max Shapes or Despeckle";
                        info->setValue(ss.str());
                    }
                    appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("TraceToMask"), QDateTime::currentDateTime(),
                                                    QString::fromUtf8("Trace produced %1 shapes (Max Shapes %2) - not imported. "
                                                                      "Raise Despeckle to drop small regions, or Max Shapes to allow bigger imports.")
                                                        .arg(pathCount).arg(maxShapes));
                } else {
                    // An exception escaping a Qt slot aborts the app - the mass
                    // import is exactly where that could happen (node creation,
                    // allocations for hundreds of shapes).
                    bool imported = false;
                    std::string importError;
                    try {
                        importDocument(res.doc, res.bounds, time);
                        imported = true;
                    } catch (const std::exception& e) {
                        importError = e.what();
                    } catch (...) {
                        importError = "unknown import error";
                    }
                    if (!imported) {
                        removeImportedItems();
                        if (KnobStringPtr info = _imp->info.lock()) {
                            info->setValue(std::string("import failed: ") + importError);
                        }
                        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("TraceToMask"), QDateTime::currentDateTime(),
                                                        QString::fromUtf8("Trace import failed: %1").arg(QString::fromUtf8(importError.c_str())));
                    }
                }
            }
        }

        if (!_imp->traceDirty) {
            break;
        }
        // A knob edit landed while tracing: run once more with the latest values.
        time = ctx->getTimelineCurrentTime();
    }
    _imp->traceRunning = false;
}

void
TraceToMask::onResetClicked()
{
    removeImportedItems();
    _imp->hasLastTracedBounds = false;
    if (KnobStringPtr info = _imp->info.lock()) {
        info->setValue("0 shapes (reset)");
    }
}

void
TraceToMask::updateInputHooks()
{
    NodePtr input = getNode() ? getNode()->getInput(0) : NodePtr();
    if (input == _imp->hookedInput.lock()) {
        return;
    }
    const NodePtr old = _imp->hookedInput.lock();
    if (old) {
        QObject::disconnect( old.get(), SIGNAL(knobsAgeChanged(U64)), this, SLOT(onInputMayHaveChanged()) );
    }
    _imp->hookedInput = input;
    if (input) {
        // Direct connection on purpose: the node emits knobsAgeChanged after
        // bumping its age but BEFORE recomputing its hash, so the age compare
        // below is fresh while a hash-based RoD query would be stale here.
        QObject::connect( input.get(), SIGNAL(knobsAgeChanged(U64)), this, SLOT(onInputMayHaveChanged()) );
    }
}

void
TraceToMask::onInputMayHaveChanged()
{
    updateInputHooks();

    const KnobBoolPtr autoKnob = _imp->autoRetrace.lock();
    if (!autoKnob || !autoKnob->getValue()) {
        return;
    }
    if (!_imp->hasLastTracedInputAge) {
        // Never traced: the first trace is the user's explicit one.
        return;
    }
    // Defer the check: the input emits knobsAgeChanged before recomputing its
    // hash, so tracing right here would fetch the pre-edit image. The zero
    // timer fires once the edit's stack (and hash) is done; a running trace's
    // event loop pumps it too, so scripted callers see the re-trace as well.
    _imp->inputDirty = true;
    if (_imp->inputCheckTimer && !_imp->inputCheckTimer->isActive()) {
        _imp->inputCheckTimer->start(0);
    }
}

void
TraceToMask::onMaxShapesChanged()
{
    // A Max Shapes raise can admit the LAST trace's document without
    // re-tracing: it is cached for exactly this purpose.
    const KnobIntPtr maxKnob = _imp->maxShapes.lock();
    const int maxShapes = maxKnob ? maxKnob->getValue() : 8;
    RotoContextPtr ctx = getNode()->getRotoContext();
    if (!ctx || !_imp->hasLastDoc) {
        return;
    }
    const int pathCount = (int)_imp->lastDoc.shapes.size();
    if (pathCount > maxShapes) {
        if (KnobStringPtr info = _imp->info.lock()) {
            std::ostringstream ss;
            ss << pathCount << " shapes traced - too complex to import; raise Max Shapes or Despeckle";
            info->setValue(ss.str());
        }
        return;
    }
    const double time = ctx->getTimelineCurrentTime();
    try {
        importDocument(_imp->lastDoc, _imp->lastDocBounds, time);
    } catch (const std::exception& e) {
        if (KnobStringPtr info = _imp->info.lock()) {
            info->setValue(std::string("import failed: ") + e.what());
        }
    } catch (...) {
        if (KnobStringPtr info = _imp->info.lock()) {
            info->setValue("import failed");
        }
    }
}

void
TraceToMask::onInputCheckTimeout()
{
    if (!_imp->inputDirty) {
        return;
    }
    _imp->inputDirty = false;

    if (_imp->traceRunning) {
        // The running trace's coalescing loop picks this up.
        _imp->traceDirty = true;
        return;
    }

    // Change detection, cheapest first:
    // 1. The input's knobs age - ANY upstream edit (image swap, resize, grade)
    //    retraces. This is the correct semantic for a tracer: the shapes
    //    follow the image.
    bool changed = false;
    NodePtr input = getNode() ? getNode()->getInput(0) : NodePtr();
    if (!input || !input->getEffectInstance()) {
        return;
    }
    if (input->getKnobsAge() != _imp->lastTracedInputAge) {
        changed = true;
    } else if (_imp->hasLastTracedBounds) {
        // 2. Timeline moves with an unchanged age: a sequence input's frame
        //    may resolve to a differently-sized image. The hash is up to date
        //    here (no knob edit in flight), so the RoD query is reliable.
        RectD rod;
        bool isProjectFormat = false;
        const StatusEnum st = input->getEffectInstance()->getRegionOfDefinition_public(
            input->getHashValue(), getApp()->getTimeLine()->currentFrame(),
            RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);
        if (st == eStatusOK) {
            const RectI b((int)std::floor(rod.x1), (int)std::floor(rod.y1),
                          (int)std::ceil(rod.x2), (int)std::ceil(rod.y2));
            changed = !(b == _imp->lastTracedBounds);
        }
    }
    if (!changed) {
        return;
    }
    const double time = getNode()->getRotoContext() ? getNode()->getRotoContext()->getTimelineCurrentTime() : 0.0;
    trace(time);
}

void
TraceToMask::onBakeClicked()
{
    RotoContextPtr ctx = getNode()->getRotoContext();
    if (!ctx) {
        return;
    }

    // The bake keys into the shapes of the current trace, in import order
    // (doc order); make sure there is one.
    std::vector<BezierPtr> curves;
    for (std::list<std::weak_ptr<RotoItem> >::const_iterator it = _imp->importedItems.begin();
         it != _imp->importedItems.end(); ++it) {
        const BezierPtr curve = std::dynamic_pointer_cast<Bezier>(it->lock());
        if (curve) {
            curves.push_back(curve);
        }
    }
    if (curves.empty()) {
        trace(ctx->getTimelineCurrentTime());
        curves.clear();
        for (std::list<std::weak_ptr<RotoItem> >::const_iterator it = _imp->importedItems.begin();
             it != _imp->importedItems.end(); ++it) {
            const BezierPtr curve = std::dynamic_pointer_cast<Bezier>(it->lock());
            if (curve) {
                curves.push_back(curve);
            }
        }
        if (curves.empty()) {
            appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("TraceToMask"), QDateTime::currentDateTime(),
                                            QString::fromUtf8("Bake Sequence: no traced shapes to bake "
                                                              "(is input 1 readable?)."));
            return;
        }
    }

    int start = _imp->bakeFrom.lock() ? _imp->bakeFrom.lock()->getValue() : 1;
    int end = _imp->bakeTo.lock() ? _imp->bakeTo.lock()->getValue() : 1;
    if (end < start) {
        // Unset/invalid range: fall back to the project frame range.
        double first = 0.0, last = 0.0;
        getApp()->getProject()->getFrameRange(&first, &last);
        start = (int)std::floor(first);
        end = (int)std::ceil(last);
    }
    if (end < start) {
        return;
    }
    const int frameCount = end - start + 1;
    if (frameCount > 1000) {
        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("TraceToMask"), QDateTime::currentDateTime(),
                                        QString::fromUtf8("Bake Sequence: %1 frames to bake, this may "
                                                          "take a while.").arg(frameCount));
    }

    beginChanges();

    const KnobIntPtr maxShapesKnob = _imp->maxShapes.lock();
    const int maxShapesBake = maxShapesKnob ? maxShapesKnob->getValue() : 8;
    int bakedFrames = 0;
    int skippedFrames = 0;
    for (int t = start; t <= end; ++t) {
        VectorMask::VectorDocument doc;
        RectI bounds;
        bool traced = false;
        // One bad frame must not throw through the slot and kill the bake.
        try {
            traced = buildTraceDocument((double)t, &doc, &bounds);
        } catch (const std::exception& e) {
            appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("TraceToMask"), QDateTime::currentDateTime(),
                                            QString::fromUtf8("Bake Sequence: frame %1 failed (%2) - skipped.")
                                                .arg(t).arg(QString::fromUtf8(e.what())));
        } catch (...) {
            appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("TraceToMask"), QDateTime::currentDateTime(),
                                            QString::fromUtf8("Bake Sequence: frame %1 failed - skipped.").arg(t));
        }
        if (!traced) {
            // Unreadable/empty frame at t: keep the previous keys.
            ++skippedFrames;
            continue;
        }
        if ((int)doc.shapes.size() > maxShapesBake) {
            // Too complex: keep the previous keys (same valve as the trace).
            ++skippedFrames;
            continue;
        }

        double sx = 1.0, sy = 1.0, ox = 0.0, oy = 0.0;
        bool flipY = true;
        double docHeight = doc.height;
        computeDocTransform(doc.width, doc.height, bounds.x1, bounds.y1,
                            &sx, &sy, &ox, &oy, &flipY, &docHeight);

        // The import skips empty-vertex shapes; the bake matches the same
        // filter so curve i corresponds to doc shape i.
        std::vector<const VectorMask::VectorShape*> shapes;
        shapes.reserve(doc.shapes.size());
        for (std::vector<VectorMask::VectorShape>::const_iterator it = doc.shapes.begin();
             it != doc.shapes.end(); ++it) {
            if (!it->vertices.empty()) {
                shapes.push_back(&*it);
            }
        }
        if (shapes.size() != curves.size()) {
            // Topology changed across frames: leave the previous keys in place.
            ++skippedFrames;
            continue;
        }

        std::vector<VectorMask::VectorShape> transformed;
        transformed.reserve(shapes.size());
        bool topologyChanged = false;
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            transformed.push_back(VectorMask::transformShape(*shapes[i], sx, sy, ox, oy, flipY, docHeight));
            if ((int)transformed.back().vertices.size() != curves[i]->getControlPointsCount()) {
                topologyChanged = true;
                break;
            }
        }
        if (topologyChanged) {
            ++skippedFrames;
            continue;
        }

        for (std::size_t i = 0; i < curves.size(); ++i) {
            const VectorMask::VectorShape& shape = transformed[i];
            for (std::size_t v = 0; v < shape.vertices.size(); ++v) {
                const VectorMask::ShapeVertex& vert = shape.vertices[v];
                const double lx = vert.center.x + vert.leftTangent.x;
                const double ly = vert.center.y + vert.leftTangent.y;
                const double rx = vert.center.x + vert.rightTangent.x;
                const double ry = vert.center.y + vert.rightTangent.y;
                curves[i]->setPointAtIndex(false, (int)v, (double)t, vert.center.x, vert.center.y, lx, ly, rx, ry);
                // Keep the feather curve aligned with the shape curve.
                curves[i]->setPointAtIndex(true, (int)v, (double)t, vert.center.x, vert.center.y, lx, ly, rx, ry);
            }
        }
        ++bakedFrames;
    }

    endChanges(true);
    ctx->evaluateChange();

    // The shapes now carry per-frame keys: project-native animated BAcziers,
    // independent of the input. Turn Auto Re-trace off so the next knob change
    // cannot wipe the baked animation.
    const KnobBoolPtr autoKnob = _imp->autoRetrace.lock();
    if (autoKnob && autoKnob->getValue()) {
        autoKnob->setValue(false);
    }

    if (KnobStringPtr info = _imp->info.lock()) {
        std::ostringstream ss;
        ss << "baked " << bakedFrames << " frames";
        if (skippedFrames) {
            ss << " (" << skippedFrames << " skipped: shape count changed)";
        }
        info->setValue(ss.str());
    }
    appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("TraceToMask"), QDateTime::currentDateTime(),
                                    QString::fromUtf8("Bake Sequence: keyed %1 frames (%2 skipped).")
                                        .arg(bakedFrames).arg(skippedFrames));
}

NATRON_NAMESPACE_EXIT
