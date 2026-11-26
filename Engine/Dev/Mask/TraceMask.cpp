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

// TraceMask: bitmap tracing -> shapes.
// See TraceMask.h for the node contract (image + mesh + vector, no roto).

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "TraceMask.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <list>
#include <map>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerInstance.h"
#include "Global/Enums.h"

#include "Dev/Mask/PlaneFetch.h"
#include "Dev/Mask/VectorDocument.h"
#include "Dev/Scene3D/MaskMeshBuilder.h"
#include "Dev/Scene3D/MeshData.h"

NATRON_NAMESPACE_ENTER

struct TraceMaskPrivate
{
    // ---- Mask Trace ----
    KnobChoiceWPtr mapChannel;   // Red / Green / Blue / Alpha / Lumin.
    KnobDoubleWPtr threshold;    // 0..1
    KnobBoolWPtr invert;         // foreground = below threshold
    KnobIntWPtr curvePrecision;  // 1..32 (contour simplification)
    KnobDoubleWPtr writeOnStart; // arc fraction 0..1
    KnobDoubleWPtr writeOnEnd;   // arc fraction 0..1
    KnobStringWPtr info;

    // ---- Mask Trace: Geometry (world normalization of the mesh) ----
    KnobDoubleWPtr worldScale;
    KnobDoubleWPtr extrudeDepth;
    KnobDoubleWPtr bevel;

    // ---- Shading: brush (shape fill) ----
    KnobBoolWPtr brushEnabled;
    KnobColorWPtr brushColor;    // RGBA
    KnobDoubleWPtr brushOpacity;

    // ---- Shading: pen (outline) ----
    KnobBoolWPtr penEnabled;
    KnobColorWPtr penColor;      // RGBA
    KnobDoubleWPtr penOpacity;
    KnobDoubleWPtr penThickness;
    KnobDoubleWPtr penThicknessBias;
    KnobChoiceWPtr penJoinType;  // Miter / Bevel / Round
    KnobChoiceWPtr penEndType;   // Simple / Round
    KnobDoubleWPtr penEndTypeRoundness;
    KnobBoolWPtr penLineEnabled; // pattern on/off
    KnobChoiceWPtr penLineType;  // Solid / Dashed / Dotted / Custom
    KnobDoubleWPtr penLineScale;
    KnobDoubleWPtr penLineDash[4];
    KnobDoubleWPtr penLineSpace[4];

    // ---- Transform (read by name by the Scene3D consumers) ----
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;
    KnobDoubleWPtr uniformScale;

    // ---- Published state (guarded by stateMutex) ----
    mutable QMutex stateMutex;
    VectorMask::VectorDocument doc;   // canonical (project format) space
    bool hasDoc;
    MeshDataPtr mesh;
    double docTime;

    // ---- Trace job state ----
    mutable QMutex renderGate;    // serializes the worker's fetch with renders
    std::atomic<bool> traceRunning;
    bool traceDirty;              // main thread only (coalesce while running)
    bool inputDirty;              // main thread only (deferred input check)
    bool knobDirty;               // main thread only (deferred knob-change trace)
    bool inlineTraced;            // render bootstrap guard (main thread only)
    double lastDegenerateTraceTime; // throttle for not-ready-input retries
    U64 lastTracedInputAge;
    bool hasLastTracedInputAge;
    RectI lastTracedBounds;
    bool hasLastTracedBounds;
    QTimer* inputCheckTimer;
    QFutureWatcher<void>* watcher;
    NodeWPtr hookedInput;

    // ---- Mesh rebuild job (3D knob edits; off the main thread) ----
    std::atomic<bool> meshBusy;
    bool meshDirty;               // guarded by stateMutex

    TraceMaskPrivate()
        : hasDoc(false)
        , docTime(-1.0)
        , traceRunning(false)
        , traceDirty(false)
        , inputDirty(false)
        , knobDirty(false)
        , inlineTraced(false)
        , lastDegenerateTraceTime(-1e9)
        , lastTracedInputAge(0)
        , hasLastTracedInputAge(false)
        , hasLastTracedBounds(false)
        , inputCheckTimer(0)
        , watcher(0)
        , meshBusy(false)
        , meshDirty(false)
    {
    }
};

TraceMask::TraceMask(NodePtr node)
    : EffectInstance(node)
    , _imp(new TraceMaskPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

TraceMask::~TraceMask()
{
    // Join a running trace job before the members it touches go away: the
    // CLI renderer tears the project down right after the render, and the
    // worker (spawned by the auto-trace timer) may still be running.
    if (_imp->watcher && _imp->traceRunning.load()) {
        _imp->watcher->future().waitForFinished();
    }
    // Same for a running mesh rebuild (3D knob edit job): the flag goes
    // false as the worker's LAST statement, so a clear here means it is
    // fully out of this object's state.
    while (_imp->meshBusy.load()) {
        QThread::msleep(10);
    }
}

std::string
TraceMask::getPluginDescription() const
{
    return "TraceMask: bitmap tracing -> shapes.\n\n"
           "Auto-traces the source image on input / parameter change (current frame).\n\n"
           "Outputs:\n"
           "- 2D image: the traced shapes rendered with the brush (shape fill) and the\n"
           "  pen (outline: solid, dashed, dotted or custom). The WriteOn group reveals\n"
           "  the outline progressively along each contour for draw-on effects.\n"
           "- 3D mesh: the same shading as geometry - the brush fill when Brush Enabled\n"
           "  is on, plus the pen outline stroked into a ribbon (thickness, taper, join,\n"
           "  dash pattern, WriteOn window) when Pen Enabled is on. Extrude Depth\n"
           "  extrudes both.\n"
           "- Vector: MaskToGeo reads the traced shapes directly from this node.";
}

std::string
TraceMask::getInputLabel(int inputNb) const
{
    return inputNb == 0 ? "Source" : std::string();
}

bool
TraceMask::isInputOptional(int inputNb) const
{
    return inputNb == 0 ? false : true;
}

void
TraceMask::addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps)
{
    // inputNb == -1 is the output plane: the node produces the traced render
    // (brush fill + pen outline) as RGBA.
    if (inputNb == 0 || inputNb == -1) {
        comps->push_back(ImagePlaneDesc::getRGBAComponents());
    }
}

void
TraceMask::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
TraceMask::getPluginGrouping(std::list<std::string>* grouping) const
{
    grouping->push_back(PLUGIN_GROUP_KEYER);
}

StatusEnum
TraceMask::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                 ViewIdx view, RectD* rod)
{
    NodePtr input = getNode() ? getNode()->getInput(0) : NodePtr();
    if (input && input->getEffectInstance()) {
        bool isProjectFormat = false;
        // GUI hash (getHashValue), not the TLS render hash: during a render
        // pass the TLS hash may be cached before the reader's decoder exists,
        // and the RoD action then returns a token 0,0,1,1 rod that stays
        // cached under that key. The GUI-hash key always re-runs the action.
        const StatusEnum st = input->getEffectInstance()->getRegionOfDefinition_public(
            input->getHashValue(), time, scale, view, rod, &isProjectFormat);
        if (st == eStatusOK && (rod->x2 - rod->x1) >= 1.0 && (rod->y2 - rod->y1) >= 1.0
            && (rod->x2 - rod->x1) * (rod->y2 - rod->y1) > 1.0) {
            return eStatusOK;
        }
    }
    rod->x1 = 0; rod->y1 = 0; rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

// ---------------------------------------------------------------------------
// Knobs (TraceMask / Shading / Transform / 3D)
// ---------------------------------------------------------------------------

static KnobDoublePtr
tmMakeDouble(TraceMask* self, KnobPagePtr page, const QString& label, const char* name,
             double def, double dispMin, double dispMax,
             double hardMin = -1e30, double hardMax = 1e30, double step = -1.0)
{
    KnobDoublePtr k = AppManager::createKnob<KnobDouble>(self, label);
    k->setName(name);
    k->setDefaultValue(def);
    k->setAnimationEnabled(true);
    if (hardMin > -1e29) {
        k->setMinimum(hardMin);
    }
    if (hardMax < 1e29) {
        k->setMaximum(hardMax);
    }
    k->setDisplayMinimum(dispMin);
    k->setDisplayMaximum(dispMax);
    if (step > 0.0) {
        k->setIncrement(step);
    }
    if (page) {
        page->addKnob(k);
    }
    return k;
}

static void
tmConnectValue(TraceMask* self, const KnobIWPtr& knobW, const char* slot)
{
    KnobIPtr k = knobW.lock();
    if (!k) {
        return;
    }
    std::shared_ptr<KnobSignalSlotHandler> handler = k->getSignalSlotHandler();
    if (handler) {
        QObject::connect(handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)), self, slot);
    }
}

void
TraceMask::initializeData()
{
    // ---------------- TraceMask ----------------
    KnobPagePtr tracePage = AppManager::createKnob<KnobPage>(this, QString::fromUtf8("TraceMask"));
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, QString::fromUtf8("Map Channel"));
        k->setName("mapChannel");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("Red", "", "Trace the red channel"));
        opts.push_back(ChoiceOption("Green", "", "Trace the green channel"));
        opts.push_back(ChoiceOption("Blue", "", "Trace the blue channel"));
        opts.push_back(ChoiceOption("Alpha", "", "Trace the alpha channel"));
        opts.push_back(ChoiceOption("Lumin.", "", "Trace the luminance (default)"));
        k->populateChoices(opts);
        k->setDefaultValue(4);
        k->setAnimationEnabled(false);
        tracePage->addKnob(k);
        _imp->mapChannel = k;
    }
    {
        KnobDoublePtr k = tmMakeDouble(this, tracePage, QString::fromUtf8("Threshold"), "threshold",
                                       0.5, 0.0, 1.0, 0.0, 1.0, 0.01);
        k->setHintToolTip(std::string("Foreground = channel value above this threshold "
                                      "(0 = black, 1 = white). With Invert on, the "
                                      "foreground is what is below it."));
        _imp->threshold = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, QString::fromUtf8("Invert"));
        k->setName("invert");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip(std::string("Foreground = channel value below the threshold: "
                                      "traces a dark subject on a bright background."));
        tracePage->addKnob(k);
        _imp->invert = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, QString::fromUtf8("Curve Precision"));
        k->setName("curvePrecision");
        k->setDefaultValue(8);
        k->setMinimum(1);
        k->setMaximum(32);
        k->setAnimationEnabled(false);
        k->setHintToolTip(std::string("How finely the traced contours are simplified before "
                                      "curve fitting. Higher = more points, closer to the raw "
                                      "pixel outline (tolerance = plane size / 2000 / precision * 4 "
                                      "pixels, e.g. ~1.25px at 2500px and the default 8)."));
        tracePage->addKnob(k);
        _imp->curvePrecision = k;
    }
    {
        // WriteOn: reveal the pen outline progressively along each contour
        // (arc fraction from Start to End). Both the 2D outline and the 3D
        // ribbon are clipped to the window; the brush fill is not affected.
        KnobGroupPtr writeOnGroup = AppManager::createKnob<KnobGroup>(this, QString::fromUtf8("WriteOn"));
        writeOnGroup->setName("writeOnGroup");
        writeOnGroup->setDefaultValue(true);
        tracePage->addKnob(writeOnGroup);
        KnobDoublePtr kS = tmMakeDouble(this, KnobPagePtr(), QString::fromUtf8("Start"), "writeOnStart",
                                        0.0, 0.0, 1.0, 0.0, 1.0, 0.01);
        kS->setHintToolTip(std::string("Arc fraction where the outline starts being drawn "
                                       "(0 = the first contour point). Outside [Start, End] "
                                       "the outline is not drawn; animatable for a draw-on "
                                       "effect. The brush fill is not affected."));
        writeOnGroup->addKnob(kS);
        _imp->writeOnStart = kS;
        KnobDoublePtr kE = tmMakeDouble(this, KnobPagePtr(), QString::fromUtf8("End"), "writeOnEnd",
                                        1.0, 0.0, 1.0, 0.0, 1.0, 0.01);
        kE->setHintToolTip(std::string("Arc fraction where the outline stops being drawn "
                                       "(1 = the full contour)."));
        writeOnGroup->addKnob(kE);
        _imp->writeOnEnd = kE;
    }
    {
        KnobGroupPtr geoGroup = AppManager::createKnob<KnobGroup>(this, QString::fromUtf8("Geometry"));
        geoGroup->setName("geometryGroup");
        geoGroup->setDefaultValue(true);
        tracePage->addKnob(geoGroup);
        KnobDoublePtr k = tmMakeDouble(this, KnobPagePtr(), QString::fromUtf8("World Scale"), "worldScale",
                                       1.0, 0.01, 10.0, 0.0, 1e30, 0.01);
        k->setHintToolTip(std::string("World size of the project format height. The shape "
                                      "pixel coordinates are normalized so the format height "
                                      "equals this value (1.0 = Nuke-style normalized space), "
                                      "then centered on the format rect."));
        geoGroup->addKnob(k);
        _imp->worldScale = k;
        k = tmMakeDouble(this, KnobPagePtr(), QString::fromUtf8("Extrude Depth"), "extrudeDepth",
                         0.0, 0.0, 5.0, 0.0, 1e30, 0.01);
        k->setHintToolTip(std::string("0 (default): a pure flat shape mesh in the XY plane at z=0. "
                                      "Greater than 0: side walls between the contours at z=0 and "
                                      "an offset cap at z=depth, plus both caps. Traced holes punch "
                                      "through both caps and their walls line the hole."));
        geoGroup->addKnob(k);
        _imp->extrudeDepth = k;
        k = tmMakeDouble(this, KnobPagePtr(), QString::fromUtf8("Bevel"), "bevel",
                         0.0, 0.0, 1.0, 0.0, 25.0, 0.01);
        // Hidden for now: the chamfer quality needs more thought before it
        // is user-facing (the knob stays persistent so saved values and the
        // builder support remain). Re-enable by clearing the secret.
        k->setSecret(true);
        k->setHintToolTip(std::string("Bevels the extruded edges with a 45-degree chamfer of this "
                                      "depth (world units, like Extrude Depth). The caps keep their "
                                      "silhouette, the side walls inset by the bevel over the middle "
                                      "band, and hole edges countersink. Needs Extrude Depth > 0; "
                                      "ignored on the flat mesh. EXPERIMENTAL - hidden."));
        geoGroup->addKnob(k);
        _imp->bevel = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, QString::fromUtf8("Info"));
        k->setName("info");
        k->setAsLabel();
        k->setWordWrap(true);
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        k->setDefaultValue("0 shapes");
        tracePage->addKnob(k);
        _imp->info = k;
    }

    // ---------------- Shading (brush = shape, pen = outline) ----------------
    KnobPagePtr shadingPage = AppManager::createKnob<KnobPage>(this, QString::fromUtf8("Shading"));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, QString::fromUtf8("Brush Enabled"));
        k->setName("brushEnabled");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        shadingPage->addKnob(k);
        _imp->brushEnabled = k;
    }
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, QString::fromUtf8("Brush Color"), 4);
        k->setName("brushColor");
        k->setDefaultValue(0.8, 0); k->setDefaultValue(0.2, 1);
        k->setDefaultValue(0.298, 2); k->setDefaultValue(1.0, 3);
        shadingPage->addKnob(k);
        _imp->brushColor = k;
    }
    {
        KnobDoublePtr k = tmMakeDouble(this, shadingPage, QString::fromUtf8("Brush Opacity"), "brushOpacity",
                                       1.0, 0.0, 1.0, 0.0, 1.0, 0.01);
        _imp->brushOpacity = k;
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, QString::fromUtf8("Pen"));
        shadingPage->addKnob(sep);
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, QString::fromUtf8("Pen Enabled"));
        k->setName("penEnabled");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        shadingPage->addKnob(k);
        _imp->penEnabled = k;
    }
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, QString::fromUtf8("Pen Color"), 4);
        k->setName("penColor");
        k->setDefaultValue(1.0, 0); k->setDefaultValue(1.0, 1);
        k->setDefaultValue(1.0, 2); k->setDefaultValue(1.0, 3);
        shadingPage->addKnob(k);
        _imp->penColor = k;
    }
    {
        KnobDoublePtr k = tmMakeDouble(this, shadingPage, QString::fromUtf8("Pen Opacity"), "penOpacity",
                                       1.0, 0.0, 1.0, 0.0, 1.0, 0.01);
        _imp->penOpacity = k;
    }
    {
        KnobDoublePtr k = tmMakeDouble(this, shadingPage, QString::fromUtf8("Pen Thickness"), "penThickness",
                                       5.0, 0.1, 100.0, 0.0, 1000.0, 0.1);
        k->setHintToolTip(std::string("Outline width in pixels: the 2D outline width "
                                      "and the ribbon width of the 3D mesh (scaled by "
                                      "World Scale, so the geometry matches the image)."));
        _imp->penThickness = k;
    }
    {
        KnobDoublePtr k = tmMakeDouble(this, shadingPage, QString::fromUtf8("Pen Thickness Bias"), "penThicknessBias",
                                       0.0, -1.0, 1.0, -1.0, 1.0, 0.01);
        k->setHintToolTip(std::string("Tapers the outline along each contour: negative = thin "
                                      "towards the end, positive = thick towards the end. "
                                      "Applies to the 2D outline and the 3D ribbon."));
        _imp->penThicknessBias = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, QString::fromUtf8("Pen Join Type"));
        k->setName("penJoinType");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("Miter", "", "Sharp corner join, bevels beyond 4x thickness"));
        opts.push_back(ChoiceOption("Bevel", "", "Flat corner join"));
        opts.push_back(ChoiceOption("Round", "", "Rounded corner join"));
        k->populateChoices(opts);
        k->setDefaultValue(1); // Bevel: the safe default (miter spikes on traced corners)
        k->setAnimationEnabled(false);
        shadingPage->addKnob(k);
        _imp->penJoinType = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, QString::fromUtf8("Pen End Type"));
        k->setName("penEndType");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("Simple", "", "Flat (butt) line ends"));
        opts.push_back(ChoiceOption("Round", "", "Rounded line ends"));
        k->populateChoices(opts);
        k->setDefaultValue(1);
        k->setAnimationEnabled(false);
        shadingPage->addKnob(k);
        _imp->penEndType = k;
    }
    {
        KnobDoublePtr k = tmMakeDouble(this, shadingPage, QString::fromUtf8("Pen End Type Roundness"), "penEndTypeRoundness",
                                       1.0, 0.0, 1.0, 0.0, 1.0, 0.01);
        _imp->penEndTypeRoundness = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, QString::fromUtf8("Pen Line"));
        k->setName("penLineEnabled");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(std::string("When checked the outline follows a dash pattern; "
                                      "unchecked draws a solid outline."));
        shadingPage->addKnob(k);
        _imp->penLineEnabled = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, QString::fromUtf8("Pen Line Type"));
        k->setName("penLineType");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("Solid", "", "No pattern"));
        opts.push_back(ChoiceOption("Dashed", "", "Dash 1 / Space 1 pattern"));
        opts.push_back(ChoiceOption("Dotted", "", "Round dots spaced by Dash 1 x Line Scale"));
        opts.push_back(ChoiceOption("Custom", "", "All four dash / space pairs"));
        k->populateChoices(opts);
        k->setDefaultValue(0);
        k->setAnimationEnabled(false);
        shadingPage->addKnob(k);
        _imp->penLineType = k;
    }
    {
        KnobDoublePtr k = tmMakeDouble(this, shadingPage, QString::fromUtf8("Pen Line Scale"), "penLineScale",
                                       1.0, 0.1, 10.0, 0.0, 100.0, 0.05);
        k->setHintToolTip(std::string("Scales every dash and space length."));
        _imp->penLineScale = k;
    }
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr kd = tmMakeDouble(this, shadingPage, QString::fromUtf8("Pen Line Dash %1").arg(i + 1),
                                        QString::fromUtf8("penLineDash%1").arg(i + 1).toStdString().c_str(),
                                        2.0, 0.0, 100.0, 0.0, 1000.0, 0.1);
        _imp->penLineDash[i] = kd;
        KnobDoublePtr ks = tmMakeDouble(this, shadingPage, QString::fromUtf8("Pen Line Space %1").arg(i + 1),
                                        QString::fromUtf8("penLineSpace%1").arg(i + 1).toStdString().c_str(),
                                        2.0, 0.0, 100.0, 0.0, 1000.0, 0.1);
        _imp->penLineSpace[i] = ks;
    }
    // Pair 1 is visible for Dashed / Dotted / Custom; pairs 2-4 for Custom only.
    updatePenLineSecrets();

    // ---------------- Transform (whole model; read by name) ----------------
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, QString::fromUtf8("Transform"));
    _imp->translateX = tmMakeDouble(this, xformPage, QString::fromUtf8("Translate X"), "translateX", 0.0, -100.0, 100.0);
    _imp->translateY = tmMakeDouble(this, xformPage, QString::fromUtf8("Translate Y"), "translateY", 0.0, -100.0, 100.0);
    _imp->translateZ = tmMakeDouble(this, xformPage, QString::fromUtf8("Translate Z"), "translateZ", 0.0, -100.0, 100.0);
    _imp->rotateX = tmMakeDouble(this, xformPage, QString::fromUtf8("Rotate X"), "rotateX", 0.0, -180.0, 180.0);
    _imp->rotateY = tmMakeDouble(this, xformPage, QString::fromUtf8("Rotate Y"), "rotateY", 0.0, -180.0, 180.0);
    _imp->rotateZ = tmMakeDouble(this, xformPage, QString::fromUtf8("Rotate Z"), "rotateZ", 0.0, -180.0, 180.0);
    _imp->scaleX = tmMakeDouble(this, xformPage, QString::fromUtf8("Scale X"), "scaleX", 1.0, 0.1, 10.0, 0.0, 1e30, 0.01);
    _imp->scaleY = tmMakeDouble(this, xformPage, QString::fromUtf8("Scale Y"), "scaleY", 1.0, 0.1, 10.0, 0.0, 1e30, 0.01);
    _imp->scaleZ = tmMakeDouble(this, xformPage, QString::fromUtf8("Scale Z"), "scaleZ", 1.0, 0.1, 10.0, 0.0, 1e30, 0.01);
    _imp->uniformScale = tmMakeDouble(this, xformPage, QString::fromUtf8("Uniform Scale"), "uniformScale", 1.0, 0.001, 10.0, 0.0, 1e30, 0.0001);
    _imp->uniformScale.lock()->setHintToolTip(std::string("Multiplies Scale X/Y/Z, so the whole model resizes from one knob."));

    // ---------------- Trace triggers ----------------
    tmConnectValue(this, _imp->mapChannel, SLOT(onControlKnobChanged()));
    tmConnectValue(this, _imp->threshold, SLOT(onControlKnobChanged()));
    tmConnectValue(this, _imp->invert, SLOT(onControlKnobChanged()));
    tmConnectValue(this, _imp->curvePrecision, SLOT(onControlKnobChanged()));
    tmConnectValue(this, _imp->penLineType, SLOT(onPenLineTypeChanged()));

    tmConnectValue(this, _imp->worldScale, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->extrudeDepth, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->bevel, SLOT(onMeshKnobChanged()));

    // Shading drives the mesh too (brush fill on/off + pen stroke ribbon).
    tmConnectValue(this, _imp->brushEnabled, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->penEnabled, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->penThickness, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->penThicknessBias, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->penJoinType, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->penEndType, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->penEndTypeRoundness, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->penLineEnabled, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->penLineScale, SLOT(onMeshKnobChanged()));
    for (int i = 0; i < 4; ++i) {
        tmConnectValue(this, _imp->penLineDash[i], SLOT(onMeshKnobChanged()));
        tmConnectValue(this, _imp->penLineSpace[i], SLOT(onMeshKnobChanged()));
    }

    // WriteOn reshapes the pen geometry (2D outline + 3D ribbon).
    tmConnectValue(this, _imp->writeOnStart, SLOT(onMeshKnobChanged()));
    tmConnectValue(this, _imp->writeOnEnd, SLOT(onMeshKnobChanged()));

    // ---------------- Timers + watcher ----------------
    _imp->inputCheckTimer = new QTimer(this);
    _imp->inputCheckTimer->setSingleShot(true);
    QObject::connect(_imp->inputCheckTimer, SIGNAL(timeout()), this, SLOT(onInputCheckTimeout()));

    _imp->watcher = new QFutureWatcher<void>(this);
    QObject::connect(_imp->watcher, SIGNAL(finished()), this, SLOT(onTraceFinished()));

    // Timeline scrub: re-trace the input frame by frame (always-on here:
    // this node is always in auto mode).
    const TimeLinePtr timeline = getApp() ? getApp()->getTimeLine() : TimeLinePtr();
    if (timeline) {
        QObject::connect( timeline.get(), SIGNAL(frameChanged(SequenceTime,int)), this, SLOT(scheduleTrace()) );
    }

    updateHooks();
}

void
TraceMask::updatePenLineSecrets()
{
    const KnobChoicePtr typeKnob = _imp->penLineType.lock();
    const int lineType = typeKnob ? typeKnob->getValue() : 1; // Dashed
    // Solid: no pairs. Dashed / Dotted: pair 1. Custom: all four pairs.
    const int visiblePairs = (lineType == 3) ? 4 : ((lineType == 1 || lineType == 2) ? 1 : 0);
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr kd = _imp->penLineDash[i].lock();
        KnobDoublePtr ks = _imp->penLineSpace[i].lock();
        if (kd) {
            kd->setSecret(i >= visiblePairs);
        }
        if (ks) {
            ks->setSecret(i >= visiblePairs);
        }
    }
}

void
TraceMask::onPenLineTypeChanged()
{
    updatePenLineSecrets();
    // The line pattern is geometry too (dashes/dots): rebuild the mesh.
    rebuildMesh(getApp()->getTimeLine()->currentFrame());
}

// ---------------------------------------------------------------------------
// Auto-trace triggers
// ---------------------------------------------------------------------------

void
TraceMask::updateHooks()
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
        // bumping its age but BEFORE recomputing its hash.
        QObject::connect( input.get(), SIGNAL(knobsAgeChanged(U64)), this, SLOT(onInputMayHaveChanged()) );
    }
}

void
TraceMask::onInputChanged(int /*inputNo*/)
{
    updateHooks();
    _imp->inlineTraced = false;
    _imp->inputDirty = true;
    if (_imp->inputCheckTimer && !_imp->inputCheckTimer->isActive()) {
        _imp->inputCheckTimer->start(0);
    }
}

void
TraceMask::onInputMayHaveChanged()
{
    updateHooks();
    _imp->inlineTraced = false;
    _imp->inputDirty = true;
    if (_imp->inputCheckTimer && !_imp->inputCheckTimer->isActive()) {
        _imp->inputCheckTimer->start(0);
    }
}

void
TraceMask::onInputCheckTimeout()
{
    _imp->inputDirty = false;
    if (_imp->knobDirty) {
        // A trace knob changed: retrace regardless of the input state (the
        // change detection below only tracks the INPUT).
        _imp->knobDirty = false;
        trace(getApp()->getTimeLine()->currentFrame());
        return;
    }

    NodePtr input = getNode() ? getNode()->getInput(0) : NodePtr();
    if (!input || !input->getEffectInstance()) {
        clearPublished();
        refreshAfterTrace(true);
        return;
    }

    // Change detection, cheapest first: any upstream edit retraces (the
    // shapes follow the image - this node is always in auto mode).
    bool changed = false;
    if (!_imp->hasLastTracedInputAge) {
        // Never traced: the first trace happens right away.
        changed = true;
    } else if (input->getKnobsAge() != _imp->lastTracedInputAge) {
        changed = true;
    } else if (_imp->hasLastTracedBounds) {
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
    trace(getApp()->getTimeLine()->currentFrame());
}

void
TraceMask::onControlKnobChanged()
{
    // Defer: the RoD action must not run inside the knob evaluation bracket
    // (a nested RoD query there returns a token 0,0,1,1 rod); the 0ms timer
    // also coalesces slider drags into a single trace.
    _imp->knobDirty = true;
    if (_imp->inputCheckTimer && !_imp->inputCheckTimer->isActive()) {
        _imp->inputCheckTimer->start(0);
    }
}

void
TraceMask::onMeshKnobChanged()
{
    // World Scale / Extrude Depth: rebuild the mesh from the published doc
    // (no re-trace). The knob change itself re-renders downstream.
    rebuildMesh(getApp()->getTimeLine()->currentFrame());
}

void
TraceMask::scheduleTrace()
{
    trace(getApp()->getTimeLine()->currentFrame());
}

// ---------------------------------------------------------------------------
// Tracing
// ---------------------------------------------------------------------------

void
TraceMask::trace(double time)
{
    assert( QThread::currentThread() == qApp->thread() );
    NodePtr input = getNode() ? getNode()->getInput(0) : NodePtr();
    if (!input || !input->getEffectInstance()) {
        clearPublished();
        refreshAfterTrace(true);
        return;
    }
    if (_imp->traceRunning.load()) {
        _imp->traceDirty = true;
        return;
    }

    // Query the input RoD on the main thread: the worker must not race the
    // hash recomputation.
    RectD inputRod;
    {
        bool isProjectFormat = false;
        const StatusEnum st = input->getEffectInstance()->getRegionOfDefinition_public(
            input->getHashValue(), time, RenderScale::identity, ViewIdx(0), &inputRod, &isProjectFormat);
        if (st != eStatusOK || inputRod.isNull()) {
            clearPublished();
            refreshAfterTrace(true);
            return;
        }
    }

    _imp->inlineTraced = false;
    _imp->traceRunning.store(true);

    // Knob values are read HERE on the main thread and passed to the worker:
    // the worker must not race the knob evaluation that triggered it.
    const KnobChoicePtr channelKnob = _imp->mapChannel.lock();
    const int channelIdx = channelKnob ? channelKnob->getValue() : (int)VectorMask::ePlaneChannelLuminance;
    const KnobDoublePtr thresholdKnob = _imp->threshold.lock();
    const float threshold = thresholdKnob ? (float)thresholdKnob->getValue() : 0.5f;
    const KnobBoolPtr invertKnob = _imp->invert.lock();
    const bool invert = invertKnob ? invertKnob->getValue() : false;
    const KnobIntPtr precKnob = _imp->curvePrecision.lock();
    const int precision = precKnob ? std::max(1, precKnob->getValue()) : 8;

    const double t = time;
    _imp->watcher->setFuture(QtConcurrent::run([this, t, inputRod, channelIdx, threshold, invert, precision]() {
        traceInternal(t, inputRod, channelIdx, threshold, invert, precision);
    }));
}

void
TraceMask::traceInternal(double time, const RectD& inputRod, int channelIdx, float threshold, bool invert, int precision)
{
    NodePtr input = getNode() ? getNode()->getInput(0) : NodePtr();
    if (!input || !input->getEffectInstance()) {
        return;
    }

    // Request the plane explicitly: alpha for the alpha channel (alpha-only
    // sources included), RGBA otherwise.
    const ImagePlaneDesc reqComps = (channelIdx == (int)VectorMask::ePlaneChannelAlpha)
                                    ? ImagePlaneDesc::getAlphaComponents()
                                    : ImagePlaneDesc::getRGBAComponents();

    // Never overlap the fetch with a render of this node: the fetch renders
    // the input while a concurrent render may be writing into output images
    // that share the same RAM cache entries (image entry-lock deadlock).
    QMutexLocker gate(&_imp->renderGate);
    const ImagePtr img = VectorMask::fetchInputPlane(input, this, time, inputRod, reqComps);
    if (!img) {
        return;
    }
    traceFromImage(img, time, channelIdx, threshold, invert, precision);
    {
        QMutexLocker l(&_imp->stateMutex);
    }
}

// Moore-neighbor boundary trace of one labeled region (component or
// enclosed hole region), starting at the region's first pixel in scan
// order. Terminates when the walk re-enters the start pixel (the boundary
// loop is closed).
static bool
traceMooreBoundary(const std::vector<int>& id, int pw, int ph, int regionId,
                   int sx, int sy, std::vector<std::pair<int, int> >* out)
{
    const int dx8[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }; // E SE S SW W NW N NE
    const int dy8[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    auto at = [&](int x, int y) -> int {
        if (x < 0 || x >= pw || y < 0 || y >= ph) {
            return -999;
        }
        return id[(std::size_t)y * pw + x];
    };

    int bx = sx, by = sy;
    int backDir = 4; // came from the west (scan order)
    out->push_back(std::make_pair(bx, by));
    const std::size_t maxSteps = (std::size_t)(pw * ph) * 2 + 64;
    std::size_t steps = 0;
    while (steps++ < maxSteps) {
        int foundDir = -1;
        for (int k = 1; k <= 8; ++k) {
            const int dir = (backDir + k) % 8;
            if (at(bx + dx8[dir], by + dy8[dir]) == regionId) {
                foundDir = dir;
                break;
            }
        }
        if (foundDir < 0) {
            return false; // isolated pixel
        }
        bx += dx8[foundDir];
        by += dy8[foundDir];
        backDir = (foundDir + 5) % 8; // opposite + 1
        if (bx == sx && by == sy) {
            return true; // boundary loop closed
        }
        out->push_back(std::make_pair(bx, by));
    }
    return false;
}

TraceMask::TraceResult
TraceMask::doTraceBinary(const std::vector<float>& imageIn, int w, int h, float threshold, bool invert)
{
    TraceResult result;
    if (w < 2 || h < 2) {
        return result;
    }

    // Binary border tracing (connected components + Moore boundary walks):
    // outer borders, enclosed background holes and nested islands, all closed
    // by construction, with the exact enclosing hierarchy (odd depth = hole).
    // The padded frame (1px black border) keeps the packed plane's row order:
    // rows run y-up (row 0 = image bottom, Natron buffer order), so traced
    // points map back with a plain -1 offset on both axes (no flip).
    const int pw = w + 2;
    const int ph = h + 2;
    std::vector<unsigned char> bin((std::size_t)pw * ph, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float v = imageIn[(std::size_t)y * w + x];
            const bool fg = invert ? (v < threshold) : (v > threshold);
            bin[(std::size_t)(y + 1) * pw + (x + 1)] = fg ? 1 : 0;
        }
    }

    // id grid: -1 unassigned, -2 exterior background, >= 0 component or
    // enclosed hole region id.
    std::vector<int> id((std::size_t)pw * ph, -1);
    std::vector<int> depthOf;
    std::vector<unsigned char> isRegion; // 1 = hole-region (background) mark
    std::vector<std::vector<std::pair<int, int> > > borders;
    std::vector<std::pair<int, int> > stack;

    const int dx4[4] = { 1, -1, 0, 0 };
    const int dy4[4] = { 0, 0, 1, -1 };

    // Pass A: exterior background flood from the padded frame.
    for (int x = 0; x < pw; ++x) {
        stack.push_back(std::make_pair(x, 0));
        stack.push_back(std::make_pair(x, ph - 1));
    }
    for (int y = 0; y < ph; ++y) {
        stack.push_back(std::make_pair(0, y));
        stack.push_back(std::make_pair(pw - 1, y));
    }
    while (!stack.empty()) {
        const std::pair<int, int> p = stack.back();
        stack.pop_back();
        if (p.first < 0 || p.first >= pw || p.second < 0 || p.second >= ph) {
            continue;
        }
        const std::size_t idx = (std::size_t)p.second * pw + p.first;
        if (bin[idx] || id[idx] != -1) {
            continue;
        }
        id[idx] = -2;
        for (int k = 0; k < 4; ++k) {
            stack.push_back(std::make_pair(p.first + dx4[k], p.second + dy4[k]));
        }
    }

    // Pass B: raster scan; each unassigned foreground pixel starts a component.
    std::vector<unsigned char> interiorBg((std::size_t)pw * ph, 0);
    std::vector<std::pair<int, int> > interiorPixels;
    for (int y = 1; y < ph - 1; ++y) {
        for (int x = 1; x < pw - 1; ++x) {
            const std::size_t idx = (std::size_t)y * pw + x;
            if (!bin[idx] || id[idx] != -1) {
                continue;
            }

            // Parent: the enclosing hole region's mark must sit on the
            // background run to the left of the component's first scan row.
            // Another component's foreground or the exterior means top-level.
            int parent = -1;
            for (int lx = x - 1; lx >= 0; --lx) {
                const std::size_t lidx = (std::size_t)y * pw + lx;
                if (bin[lidx]) {
                    break;
                }
                if (id[lidx] == -2) {
                    break;
                }
                if (id[lidx] == -1) {
                    continue;
                }
                if (isRegion[id[lidx]]) {
                    parent = id[lidx];
                }
                break;
            }
            const int compId = (int)depthOf.size();
            const int parentDepth = (parent >= 0) ? depthOf[parent] : -1;
            depthOf.push_back(parentDepth + 1);
            isRegion.push_back(0);

            // Flood fill the component (4-connectivity); track its first
            // pixel in scan order for the boundary walk start.
            stack.push_back(std::make_pair(x, y));
            id[idx] = compId;
            int topY = y, topX = x;
            while (!stack.empty()) {
                const std::pair<int, int> p = stack.back();
                stack.pop_back();
                if (p.second < topY || (p.second == topY && p.first < topX)) {
                    topY = p.second;
                    topX = p.first;
                }
                for (int k = 0; k < 4; ++k) {
                    const int nx = p.first + dx4[k];
                    const int ny = p.second + dy4[k];
                    if (nx < 1 || nx >= pw - 1 || ny < 1 || ny >= ph - 1) {
                        continue;
                    }
                    const std::size_t nidx = (std::size_t)ny * pw + nx;
                    if (bin[nidx] && id[nidx] == -1) {
                        id[nidx] = compId;
                        stack.push_back(std::make_pair(nx, ny));
                    }
                }
            }

            // Outer boundary.
            borders.push_back(std::vector<std::pair<int, int> >());
            std::vector<std::pair<int, int> > border;
            if (traceMooreBoundary(id, pw, ph, compId, topX, topY, &border)) {
                borders.back().swap(border);
            }

            // Scanline-fill the component's interior background with the
            // component id. Foreground pixels inside (child components) stay
            // unassigned so the scan finds them later.
            const std::vector<std::pair<int, int> >& useBorder = borders.back();
            if (!useBorder.empty()) {
                for (int fy = 1; fy < ph - 1; ++fy) {
                    std::vector<int> xs;
                    for (std::size_t k = 0; k < useBorder.size(); ++k) {
                        const std::pair<int, int>& a = useBorder[k];
                        const std::pair<int, int>& b2 = useBorder[(k + 1) % useBorder.size()];
                        if (a.second == b2.second) {
                            continue;
                        }
                        const int yLo = std::min(a.second, b2.second);
                        const int yHi = std::max(a.second, b2.second);
                        if (fy >= yLo && fy < yHi) {
                            xs.push_back(a.first);
                        }
                    }
                    if (xs.size() < 2) {
                        continue;
                    }
                    std::sort(xs.begin(), xs.end());
                    for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
                        for (int fx = xs[k] + 1; fx < xs[k + 1]; ++fx) {
                            if (fx < 0 || fx >= pw) {
                                continue;
                            }
                            const std::size_t fidx = (std::size_t)fy * pw + fx;
                            if (!bin[fidx] && id[fidx] == -1) {
                                id[fidx] = compId;
                                interiorBg[fidx] = 1;
                                interiorPixels.push_back(std::make_pair(fx, fy));
                            }
                        }
                    }
                }
            }

            // Enclosed background hole regions of this component: connected
            // sets of interior background. Trace each as a minus contour and
            // mark its pixels with a region id so islands inside find it as
            // their parent.
            for (std::size_t k = 0; k < interiorPixels.size(); ++k) {
                const std::pair<int, int>& hp = interiorPixels[k];
                const std::size_t hidx = (std::size_t)hp.second * pw + hp.first;
                if (!interiorBg[hidx] || id[hidx] != compId) {
                    continue; // claimed by an earlier hole region
                }
                const int holeId = (int)depthOf.size();
                depthOf.push_back(depthOf[compId] + 1);
                isRegion.push_back(1);

                stack.push_back(hp);
                id[hidx] = holeId;
                interiorBg[hidx] = 0;
                int hTopY = hp.second, hTopX = hp.first;
                while (!stack.empty()) {
                    const std::pair<int, int> p = stack.back();
                    stack.pop_back();
                    if (p.second < hTopY || (p.second == hTopY && p.first < hTopX)) {
                        hTopY = p.second;
                        hTopX = p.first;
                    }
                    for (int kk = 0; kk < 4; ++kk) {
                        const int nx = p.first + dx4[kk];
                        const int ny = p.second + dy4[kk];
                        if (nx < 0 || nx >= pw || ny < 0 || ny >= ph) {
                            continue;
                        }
                        const std::size_t nidx = (std::size_t)ny * pw + nx;
                        if (interiorBg[nidx] && id[nidx] == compId) {
                            id[nidx] = holeId;
                            interiorBg[nidx] = 0;
                            stack.push_back(std::make_pair(nx, ny));
                        }
                    }
                }

                borders.push_back(std::vector<std::pair<int, int> >());
                std::vector<std::pair<int, int> > holeBorder;
                if (traceMooreBoundary(id, pw, ph, holeId, hTopX, hTopY, &holeBorder)) {
                    borders.back().swap(holeBorder);
                }
            }
            interiorPixels.clear();
        }
    }

    // Emit: map padded coords back to buffer coords (plain -1 offset; rows
    // are y-up), drop specks.
    const double minArea = std::max(4.0, (double)w * (double)h / 50000.0);
    for (std::size_t i = 0; i < borders.size(); ++i) {
        const std::vector<std::pair<int, int> >& bp = borders[i];
        if (bp.size() < 3) {
            continue;
        }
        std::vector<std::pair<double, double> > contour;
        contour.reserve(bp.size());
        for (std::size_t k = 0; k < bp.size(); ++k) {
            contour.push_back(std::make_pair((double)(bp[k].first - 1),
                                             (double)(bp[k].second - 1)));
        }
        double area = 0.0;
        for (std::size_t k = 0; k < contour.size(); ++k) {
            const std::size_t j = (k + 1) % contour.size();
            area += contour[k].first * contour[j].second - contour[j].first * contour[k].second;
        }
        if (std::fabs(area) * 0.5 < minArea) {
            continue;
        }
        result.contours.push_back(contour);
        result.depths.push_back(depthOf[i]);
    }

    return result;
}

void
TraceMask::simplifyContour(std::vector<std::pair<double, double> >* contour, int precision, double refSize) const
{
    if (!contour || contour->size() <= 6) {
        return;
    }
    // Curve Precision -> RDP tolerance, scaled with the plane's resolution so
    // the default (8) gives a consistent look across image sizes (tolerance =
    // plane size / 2000 at precision 8: ~0.25px at 512, ~1.25px at 2500).
    const double tol = std::max(0.25, refSize / 2000.0 * (4.0 / std::max(1, precision)));
    const double tol2 = tol * tol;

    const int n = (int)contour->size();
    // Split the closed ring at the point farthest from vertex 0, then run
    // Douglas-Peucker on both halves.
    int anchor = 0;
    double maxD2 = -1.0;
    for (int i = 1; i < n; ++i) {
        const double dx = (*contour)[i].first - (*contour)[0].first;
        const double dy = (*contour)[i].second - (*contour)[0].second;
        const double d2 = dx * dx + dy * dy;
        if (d2 > maxD2) {
            maxD2 = d2;
            anchor = i;
        }
    }
    if (anchor == 0 || anchor == n - 1) {
        return;
    }

    std::vector<char> keep(n, 0);
    keep[0] = 1;
    keep[anchor] = 1;
    keep[n - 1] = 1;

    // Iterative Douglas-Peucker on the two open sub-ranges.
    std::vector<std::pair<int, int> > stack;
    stack.push_back(std::make_pair(0, anchor));
    stack.push_back(std::make_pair(anchor, n - 1));
    while (!stack.empty()) {
        const int i0 = stack.back().first;
        const int i1 = stack.back().second;
        stack.pop_back();
        if (i1 - i0 < 2) {
            continue;
        }
        const double ax = (*contour)[i0].first, ay = (*contour)[i0].second;
        const double bx = (*contour)[i1].first, by = (*contour)[i1].second;
        const double dx = bx - ax, dy = by - ay;
        const double len2 = dx * dx + dy * dy;
        int farIdx = -1;
        double farD2 = tol2;
        for (int i = i0 + 1; i < i1; ++i) {
            double t = len2 > 0.0 ? (((*contour)[i].first - ax) * dx + ((*contour)[i].second - ay) * dy) / len2 : 0.0;
            t = std::max(0.0, std::min(1.0, t));
            const double px = ax + t * dx, py = ay + t * dy;
            const double ex = (*contour)[i].first - px, ey = (*contour)[i].second - py;
            const double d2 = ex * ex + ey * ey;
            if (d2 > farD2) {
                farD2 = d2;
                farIdx = i;
            }
        }
        if (farIdx > 0) {
            keep[farIdx] = 1;
            stack.push_back(std::make_pair(i0, farIdx));
            stack.push_back(std::make_pair(farIdx, i1));
        }
    }

    std::vector<std::pair<double, double> > out;
    out.reserve(contour->size());
    for (int i = 0; i < n; ++i) {
        if (keep[i]) {
            out.push_back((*contour)[i]);
        }
    }
    if ((int)out.size() >= 3) {
        contour->swap(out);
    }
}

bool
TraceMask::buildCanonicalDoc(const std::vector<std::vector<std::pair<double, double> > >& contoursIn,
                             const std::vector<int>& depthsIn,
                             const ImagePtr& srcImg,
                             VectorMask::VectorDocument* doc,
                             int precision) const
{
    if (!srcImg || contoursIn.empty()) {
        return false;
    }
    const RectI bounds = srcImg->getBounds();
    const int w = bounds.width();
    const int h = bounds.height();
    if (w < 2 || h < 2) {
        return false;
    }

    // Buffer coordinates (bx, by) are relative to the bounds with by growing
    // UPWARDS (row 0 = image bottom, Natron buffer order). Canonical mapping
    // is therefore a plain translation + pixel-aspect scale on x.
    const double par = srcImg->getPixelAspectRatio();
    const RectD rod = srcImg->getRoD();

    // Simplify (Curve Precision) + filter degenerate contours.
    const double refSize = (double)std::max(w, h);
    std::vector<std::vector<std::pair<double, double> > > contours;
    contours.reserve(contoursIn.size());
    for (std::size_t i = 0; i < contoursIn.size(); ++i) {
        std::vector<std::pair<double, double> > c = contoursIn[i];
        simplifyContour(&c, precision, refSize);
        if (c.size() >= 3) {
            contours.push_back(c);
        }
    }
    if (contours.empty()) {
        return false;
    }

    // Hole semantics come straight from the border tracer's hierarchy
    // (odd depth = hole -> eMergeMinus).
    doc->shapes.clear();
    doc->width = rod.width();
    doc->height = rod.height();
    for (std::size_t i = 0; i < contours.size(); ++i) {
        const std::size_t n = contours[i].size();
        VectorMask::VectorShape shape;
        shape.name = "Trace" + std::to_string(i);
        shape.closed = true;
        shape.compositingOperator = (i < depthsIn.size() && depthsIn[i] % 2 == 1)
                                    ? (int)eMergeMinus : -1;

        shape.vertices.resize(n);
        for (std::size_t j = 0; j < n; ++j) {
            shape.vertices[j].center = VectorMask::Point2D(contours[i][j].first, contours[i][j].second);
        }
        // Straight segments (zero tangent offsets): the traced contour IS the
        // target outline - a Catmull-Rom spline here overshoots up to ~15% of
        // the edge length at every corner (the 2D fill, the mesh and the
        // stroke ribbon would all bulge past the bitmap). Any smoothing is
        // already applied by simplifyContour (Curve Precision).

        // Doc (buffer) space -> canonical: x scaled by the pixel aspect.
        for (std::size_t j = 0; j < n; ++j) {
            VectorMask::ShapeVertex& v = shape.vertices[j];
            v.center = VectorMask::Point2D(rod.x1 + v.center.x * par, rod.y1 + v.center.y);
            v.leftTangent = VectorMask::Point2D(v.leftTangent.x * par, v.leftTangent.y);
            v.rightTangent = VectorMask::Point2D(v.rightTangent.x * par, v.rightTangent.y);
        }

        doc->shapes.push_back(shape);
    }
    return !doc->shapes.empty();
}

void
TraceMask::traceFromImage(const ImagePtr& img, double time, int channelIdx, float threshold, bool invert, int precision)
{
    if (!img) {
        return;
    }
    const RectI bounds = img->getBounds();
    const int w = bounds.width();
    const int h = bounds.height();
    if (w < 2 || h < 2) {
        return;
    }

    std::vector<float> values;
    int fw = w, fh = h;
    VectorMask::extractPlaneValues(img, channelIdx, &values, (std::vector<float>*)0, &fw, &fh);
    if (values.empty()) {
        return;
    }

    // A degenerate plane is the "input not ready yet" case (a reader still
    // decoding renders black/transparent), not a legit trace: publish
    // nothing here - the previously traced shapes stay visible and a later
    // render (any zoom) retries the inline trace. Publishing a garbage doc
    // from this state stuck the node blank / traced solid blocks on whole
    // zoom levels. Two degenerate forms:
    //   - all values zero (the plain not-ready case),
    //   - with Invert on, foreground = below threshold, so ANY zero/garbage
    //     region becomes foreground: a fully/partially decoded plane traces
    //     as one huge (or tiled) solid blob. Guard by the foreground
    //     coverage: a legit inverted trace (dark subject on bright bg) stays
    //     far below this, a decode-in-flight plane crosses it.
    float vmax = 0.0f;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] > vmax) {
            vmax = values[i];
        }
    }
    std::size_t fgCount = 0;
    if (invert) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i] < threshold) {
                ++fgCount;
            }
        }
    }
    const double fgCoverage = values.empty() ? 0.0 : (double)fgCount / (double)values.size();
    const bool degenerate = (vmax <= 0.0f) || (invert && fgCoverage > 0.9);
    if (degenerate) {
        // Throttle the inline retry: without this every zoom render re-traces
        // the full plane while the input is not ready (the trace itself is the
        // reported slowdown). Half a second keeps recovery quick and stops
        // the hammering.
        const double now = (double)QDateTime::currentMSecsSinceEpoch() / 1000.0;
        QMutexLocker l(&_imp->stateMutex);
        if (now - _imp->lastDegenerateTraceTime >= 0.5) {
            _imp->lastDegenerateTraceTime = now;
            _imp->inlineTraced = false; // retry on a later render
        } else {
            _imp->inlineTraced = true; // stand down until the throttle expires
        }
        return;
    }

    TraceResult result = doTraceBinary(values, fw, fh, threshold, invert);
    if (result.contours.empty()) {
        // Real "nothing above threshold" (the plane has content): publish an
        // empty document so the node shows "0 shapes" instead of re-tracing
        // on every render. The inline-trace guard is reset so a later render
        // retries once if the input state changes underneath; the traced
        // input age is recorded so the async retrace check knows this empty
        // doc already saw the current input.
        VectorMask::VectorDocument emptyDoc;
        {
            QMutexLocker l(&_imp->stateMutex);
            _imp->doc = emptyDoc;
            _imp->hasDoc = true;
            _imp->mesh.reset();
            _imp->docTime = time;
            _imp->inlineTraced = false;
        }
        NodePtr input = getNode() ? getNode()->getInput(0) : NodePtr();
        if (input) {
            QMutexLocker l(&_imp->stateMutex);
            _imp->lastTracedInputAge = input->getKnobsAge();
            _imp->hasLastTracedInputAge = true;
        }
        return;
    }

    VectorMask::VectorDocument doc;
    if (!buildCanonicalDoc(result.contours, result.depths, img, &doc, precision)) {
        return;
    }
    const MeshDataPtr mesh = buildMeshFromDoc(doc, time);
    publish(doc, mesh, time);
}

void
TraceMask::publish(const VectorMask::VectorDocument& doc, const MeshDataPtr& mesh, double time)
{
    QMutexLocker l(&_imp->stateMutex);
    _imp->doc = doc;
    _imp->hasDoc = true;
    _imp->mesh = mesh;
    _imp->docTime = time;
}

void
TraceMask::clearPublished()
{
    QMutexLocker l(&_imp->stateMutex);
    _imp->hasDoc = false;
    _imp->doc.shapes.clear();
    _imp->mesh.reset();
    _imp->docTime = -1.0;
}

void
TraceMask::onTraceFinished()
{
    // Main thread: the async job just completed.
    _imp->traceRunning.store(false);

    NodePtr input = getNode() ? getNode()->getInput(0) : NodePtr();
    if (input && input->getEffectInstance()) {
        _imp->lastTracedInputAge = input->getKnobsAge();
        _imp->hasLastTracedInputAge = true;
        RectD rod;
        bool isProjectFormat = false;
        const StatusEnum st = input->getEffectInstance()->getRegionOfDefinition_public(
            input->getHashValue(), getApp()->getTimeLine()->currentFrame(),
            RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);
        if (st == eStatusOK) {
            _imp->lastTracedBounds = RectI((int)std::floor(rod.x1), (int)std::floor(rod.y1),
                                           (int)std::ceil(rod.x2), (int)std::ceil(rod.y2));
            _imp->hasLastTracedBounds = true;
        }
    }

    refreshAfterTrace(true);

    if (_imp->traceDirty) {
        _imp->traceDirty = false;
        trace(getApp()->getTimeLine()->currentFrame());
    }
}

void
TraceMask::refreshAfterTrace(bool updateInfo)
{
    // Main thread only: UI refresh + invalidation after the published state
    // changed (the async trace or a failed trace).
    assert( QThread::currentThread() == qApp->thread() );

    std::size_t shapeCount = 0;
    std::size_t vertCount = 0;
    std::size_t faceCount = 0;
    bool hasMesh = false;
    {
        QMutexLocker l(&_imp->stateMutex);
        shapeCount = _imp->doc.shapes.size();
        if (_imp->mesh) {
            vertCount = _imp->mesh->numVertices;
            faceCount = _imp->mesh->numFaces;
            hasMesh = true;
        }
    }

    if (updateInfo) {
        if (KnobStringPtr info = _imp->info.lock()) {
            std::ostringstream ss;
            ss << shapeCount << " shapes";
            if (hasMesh) {
                ss << ", " << vertCount << " vertices, " << faceCount << " faces";
            }
            info->setValue(ss.str());
        }
    }

    // Invalidate: the knobs age feeds the node hash, so downstream consumers
    // (MaskToGeo, ScanlineRender, Cycles) rebuild, and connected viewers
    // re-render with the new shapes.
    if (getNode()) {
        getNode()->incrementKnobsAge();
        std::list<ViewerInstance*> viewers;
        getNode()->hasViewersConnected(&viewers);
        for (std::list<ViewerInstance*>::iterator it = viewers.begin(); it != viewers.end(); ++it) {
            (*it)->renderCurrentFrame(true);
        }
        // The 3D viewports render on demand: without a repaint request here the
        // mesh/shading change lands in the published state AFTER the paint the
        // knob edit scheduled (the rebuild runs off-thread), and the viewport
        // shows the previous geometry until the next unrelated event.
        AppInstancePtr app = getApp();
        if (app) {
            app->redrawAllViewers();
        }
    }
}

void
TraceMask::rebuildMesh(double time)
{
    // Off the main thread: a rebuild tessellates every contour in the doc
    // (a photographic trace = thousands of them) and must never block the
    // GUI on a knob drag. Coalesced like the trace job: while a rebuild is
    // running, further requests just mark the state dirty and the running
    // job loops once more with the fresh doc + knob values.
    if (_imp->meshBusy.exchange(true)) {
        QMutexLocker l(&_imp->stateMutex);
        _imp->meshDirty = true;
        return;
    }

    const double t = time;
    QThread* worker = QThread::create([this, t]() {
        MeshDataPtr mesh;
        bool again = true;
        while (again) {
            VectorMask::VectorDocument doc;
            bool hasDoc = false;
            {
                QMutexLocker l(&_imp->stateMutex);
                hasDoc = _imp->hasDoc;
                _imp->meshDirty = false;
                if (hasDoc) {
                    doc = _imp->doc;
                }
            }
            mesh = hasDoc ? buildMeshFromDoc(doc, t) : MeshDataPtr();
            {
                QMutexLocker l(&_imp->stateMutex);
                _imp->mesh = mesh;
                again = _imp->meshDirty;
            }
        }
        // Main thread: invalidate so the 3D consumers re-render with the
        // new mesh (no info update - the trace info line is unrelated).
        QMetaObject::invokeMethod(const_cast<TraceMask*>(this), [this]() {
            refreshAfterTrace(false);
        }, Qt::QueuedConnection);
        // LAST statement: the destructor busy-waits on this flag.
        _imp->meshBusy.store(false);
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------

MeshDataPtr
TraceMask::getMeshData(double time) const
{
    {
        QMutexLocker l(&_imp->stateMutex);
        if (_imp->hasDoc && _imp->mesh && _imp->docTime == time) {
            return _imp->mesh;
        }
    }
    // Stale or missing doc: trace this frame synchronously (render threads:
    // the 3D consumer needs the mesh NOW - e.g. a headless ScanlineRender /
    // Cycles render has no main-loop trace queue to lean on). A running
    // async trace makes this a no-op and the stale mesh is returned below
    // (mid-scrub); it is replaced as soon as the async publishes.
    ensureTraced(time);
    QMutexLocker l(&_imp->stateMutex);
    if (_imp->hasDoc && _imp->mesh) {
        return _imp->mesh;
    }
    return MeshDataPtr();
}

VectorMask::VectorDocument
TraceMask::getShapeDocument(double time) const
{
    {
        QMutexLocker l(&_imp->stateMutex);
        if (_imp->hasDoc && _imp->docTime == time) {
            return _imp->doc;
        }
    }
    ensureTraced(time);
    QMutexLocker l(&_imp->stateMutex);
    if (_imp->hasDoc) {
        return _imp->doc;
    }
    return VectorMask::VectorDocument();
}

void
TraceMask::ensureTraced(double time) const
{
    NodePtr input = getNode() ? getNode()->getInput(0) : NodePtr();
    if (!input || !input->getEffectInstance()) {
        return;
    }
    if (_imp->traceRunning.load()) {
        return;
    }

    // NEVER fetch on the calling render thread itself (a nested render
    // inside a foreign render action, on a thread carrying framework render
    // TLS, trips the TLS spawn/copy bookkeeping - observed: the caller's
    // EffectTLSData vanished mid-action). Trace on a DEDICATED one-shot
    // thread instead: pristine TLS, and no shared state with the pool
    // threads the framework recycles.
    // The published doc is also refreshed by this node's own image render
    // (the input-image dependency pass runs TraceMask::render first, whose
    // bootstrap traces inline from the already-fetched input image).
    {
        const KnobChoicePtr channelKnob = _imp->mapChannel.lock();
        const int channelIdx = channelKnob ? channelKnob->getValue() : (int)VectorMask::ePlaneChannelLuminance;
        const KnobDoublePtr thresholdKnob = _imp->threshold.lock();
        const float threshold = thresholdKnob ? (float)thresholdKnob->getValue() : 0.5f;
        const KnobBoolPtr invertKnob = _imp->invert.lock();
        const bool invert = invertKnob ? invertKnob->getValue() : false;
        const KnobIntPtr precKnob = _imp->curvePrecision.lock();
        const int precision = precKnob ? std::max(1, precKnob->getValue()) : 8;
        const NodePtr in = input;
        const double t = time;
        QThread* worker = QThread::create([this, in, t, channelIdx, threshold, invert, precision]() {
            ensureTracedInternal(in, t, channelIdx, threshold, invert, precision);
        });
        QObject::connect(worker, &QThread::finished, worker, &QThread::deleteLater);
        worker->start();
        worker->wait();
    }
}

void
TraceMask::ensureTracedInternal(const NodePtr& input, double time, int channelIdx, float threshold, bool invert, int precision) const
{
    RectD inputRod;
    bool isProjectFormat = false;
    if (input->getEffectInstance()->getRegionOfDefinition_public(
            input->getHashValue(), time, RenderScale::identity, ViewIdx(0),
            &inputRod, &isProjectFormat) != eStatusOK || inputRod.isNull()) {
        return;
    }

    // Serialize with the worker's fetch and this node's own renders (the
    // fetch renders the input - see traceInternal).
    QMutexLocker gate(&_imp->renderGate);
    {
        QMutexLocker l(&_imp->stateMutex);
        // An EMPTY published doc is not an up-to-date state: it is either a
        // legit "nothing above threshold" or the residue of a not-ready
        // input. Retrace once more when the input has changed since the
        // empty trace (decode completed) - guarded by the input age so a
        // genuinely empty trace does not loop forever.
        const bool docStale = !_imp->hasDoc
                              || _imp->docTime != time
                              || _imp->doc.shapes.empty();
        if ( ( _imp->hasDoc && !docStale ) || _imp->traceRunning.load() ) {
            return; // traced meanwhile, or an async trace owns the state
        }
        if (_imp->hasDoc && _imp->doc.shapes.empty() && _imp->hasLastTracedInputAge
            && input->getKnobsAge() == _imp->lastTracedInputAge) {
            return; // the empty trace already saw this exact input
        }
    }

    const ImagePlaneDesc reqComps = (channelIdx == (int)VectorMask::ePlaneChannelAlpha)
                                    ? ImagePlaneDesc::getAlphaComponents()
                                    : ImagePlaneDesc::getRGBAComponents();
    const ImagePtr img = VectorMask::fetchInputPlane(input, const_cast<TraceMask*>(this), time, inputRod, reqComps);
    if (img) {
        const_cast<TraceMask*>(this)->traceFromImage(img, time, channelIdx, threshold, invert, precision);
    }
}

// MaterialProvider: the Shading tab drives the surface color (the brush is
// the paint of the shapes); everything else is a fixed default that a
// GeoMaterialOverride node can adjust downstream.

void
TraceMask::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    KnobColorPtr c = _imp->brushColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 0.8; g = 0.2; b = 0.298; }
}

double
TraceMask::getMaterialRoughness(double /*time*/) const
{
    return 0.5;
}

double
TraceMask::getMaterialMetallic(double /*time*/) const
{
    return 0.0;
}

double
TraceMask::getMaterialSpecular(double /*time*/) const
{
    return 0.5;
}

void
TraceMask::getMaterialEmission(double /*time*/, double& r, double& g, double& b, double& strength) const
{
    r = 0.0; g = 0.0; b = 0.0;
    strength = 0.0;
}

double
TraceMask::getMaterialTransmission(double /*time*/) const
{
    return 0.0;
}

double
TraceMask::getMaterialIOR(double /*time*/) const
{
    return 1.45;
}

std::string
TraceMask::getMaterialTextureFile() const
{
    return std::string();
}

std::string
TraceMask::getMaterialDiffuseColorspace() const
{
    return "sRGB";
}

bool
TraceMask::hasMaterialInput() const
{
    return false; // the single input is the trace source, not a material
}

MaterialProvider*
TraceMask::getConnectedMaterial() const
{
    return (MaterialProvider*)0;
}

// ---------------------------------------------------------------------------
// 2D render: brush fill + pen outline rasterization (Krokodove Shading)
// ---------------------------------------------------------------------------

namespace {

typedef std::pair<double, double> RPoint;

struct RasterCtx
{
    // Region in absolute pixel coordinates + coverage buffer (roi-sized).
    RectI roi;
    int w, h;
    std::vector<float>* cov;

    void addSpan(double x0, double x1, int row, double v)
    {
        if (x1 <= x0) {
            return;
        }
        // Pixel centers at x + 0.5.
        int i0 = (int)std::ceil(x0 - 0.5);
        int i1 = (int)std::ceil(x1 - 0.5);
        i0 = std::max(i0, roi.x1);
        i1 = std::min(i1, roi.x2);
        if (i1 <= i0) {
            return;
        }
        float* rowPix = &(*cov)[ (std::size_t)row * w ];
        for (int x = i0; x < i1; ++x) {
            rowPix[x - roi.x1] = std::max(0.0f, std::min(1.0f, rowPix[x - roi.x1] + (float)v));
        }
    }
};

// Even-odd scanline fill of one closed polygon with 2x2 supersampling.
// `v` > 0 adds coverage, `v` < 0 subtracts it (holes punch through).
void
fillPolygonEvenOdd(const std::vector<RPoint>& poly, RasterCtx& ctx, double v)
{
    if (poly.size() < 3) {
        return;
    }
    const double sub = 0.5;
    std::vector<double> xs;
    for (int row = 0; row < ctx.h; ++row) {
        for (int s = 0; s < 2; ++s) {
            const double ys = ctx.roi.y1 + row + (s + 0.5) * sub;
            xs.clear();
            const std::size_t n = poly.size();
            for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                const double yi = poly[i].second, yj = poly[j].second;
                if ( (yi > ys) != (yj > ys) ) {
                    const double xInt = poly[j].first + (ys - yj) * (poly[i].first - poly[j].first) / (yi - yj + 1e-12);
                    xs.push_back(xInt);
                }
            }
            if (xs.size() < 2) {
                continue;
            }
            std::sort(xs.begin(), xs.end());
            // Even-odd: fill between pairs.
            const std::size_t pairs = xs.size() / 2 * 2;
            for (std::size_t k = 0; k + 1 < pairs; k += 2) {
                ctx.addSpan(xs[k], xs[k + 1], row, v * sub);
            }
        }
    }
}

struct PenStyle
{
    double thickness;     // px (output pixels)
    double thicknessBias; // taper along the contour arc
    int joinType;         // 0 miter 1 bevel 2 round
    int endType;          // 0 simple (butt) 1 round
    double endRoundness;  // 0..1
    std::vector<std::pair<double, double> > pattern; // (on, off) px; empty = solid
    bool forceRoundCaps;  // dotted
};

static double
penWidthAt(const PenStyle& style, double arcFrac)
{
    double w = style.thickness * (1.0 + style.thicknessBias * (2.0 * arcFrac - 1.0));
    if (w < 0.05) {
        w = 0.05;
    }
    return w;
}

/// Fill a PenStyle from the shading knobs at `time`. Shared by the 2D
/// rasterizer and the 3D stroke geometry so both agree.
static PenStyle
readPenStyle(const TraceMaskPrivate* imp, double time)
{
    PenStyle pen;
    pen.thickness = 1.0;
    pen.thicknessBias = 0.0;
    pen.joinType = 0;
    pen.endType = 1;
    pen.endRoundness = 1.0;
    pen.forceRoundCaps = false;

    {
        KnobDoublePtr k = imp->penThickness.lock();
        pen.thickness = k ? std::max(0.0, k->getValueAtTime(time)) : 1.0;
    }
    {
        KnobDoublePtr k = imp->penThicknessBias.lock();
        pen.thicknessBias = k ? k->getValueAtTime(time) : 0.0;
    }
    {
        KnobChoicePtr k = imp->penJoinType.lock();
        pen.joinType = k ? k->getValue() : 0;
    }
    {
        KnobChoicePtr k = imp->penEndType.lock();
        pen.endType = k ? k->getValue() : 1;
    }
    {
        KnobDoublePtr k = imp->penEndTypeRoundness.lock();
        pen.endRoundness = k ? k->getValueAtTime(time) : 1.0;
    }

    const bool lineOn = imp->penLineEnabled.lock() ? imp->penLineEnabled.lock()->getValue() : true;
    const KnobChoicePtr lineTypeKnob = imp->penLineType.lock();
    const int lineType = lineTypeKnob ? lineTypeKnob->getValue() : 1;
    const double lineScale = imp->penLineScale.lock() ? std::max(0.001, imp->penLineScale.lock()->getValueAtTime(time)) : 1.0;
    pen.forceRoundCaps = (lineType == 2); // dotted: round dots

    if (lineOn && lineType != 0) {
        if (lineType == 2) {
            // Dotted: round DOTS, not dashes with round caps. The on-run is
            // the dot diameter (the pen width, so each dot reads as a circle),
            // the gap follows Dash 1 x Line Scale. The same Dash 1 / Space 1
            // pair as Dashed made both types render identical strokes.
            const double d = imp->penLineDash[0].lock() ? imp->penLineDash[0].lock()->getValueAtTime(time) : 2.0;
            const double dot = std::max(1.0, pen.thickness);
            const double gap = std::max(dot, d * lineScale);
            pen.pattern.push_back(std::make_pair(dot, gap));
        } else if (lineType == 3) { // Custom: all four pairs
            for (int i = 0; i < 4; ++i) {
                const double d = imp->penLineDash[i].lock() ? imp->penLineDash[i].lock()->getValueAtTime(time) : 2.0;
                const double s = imp->penLineSpace[i].lock() ? imp->penLineSpace[i].lock()->getValueAtTime(time) : 2.0;
                if (d > 0.0 || s > 0.0) {
                    pen.pattern.push_back(std::make_pair(d * lineScale, s * lineScale));
                }
            }
        } else { // Dashed: pair 1
            const double d = imp->penLineDash[0].lock() ? imp->penLineDash[0].lock()->getValueAtTime(time) : 2.0;
            const double s = imp->penLineSpace[0].lock() ? imp->penLineSpace[0].lock()->getValueAtTime(time) : 2.0;
            pen.pattern.push_back(std::make_pair(d * lineScale, s * lineScale));
        }
        // Degenerate patterns collapse to solid.
        double cycle = 0.0;
        for (std::size_t i = 0; i < pen.pattern.size(); ++i) {
            cycle += pen.pattern[i].first + pen.pattern[i].second;
        }
        if (cycle <= 1e-6) {
            pen.pattern.clear();
        }
    }
    return pen;
}

// Filled disc (round caps / round joins / dots).
void
emitDisc(const RPoint& c, double r, RasterCtx& ctx, double v)
{
    if (r <= 0.0) {
        return;
    }
    const int x0 = std::max(ctx.roi.x1, (int)std::floor(c.first - r - 1.0));
    const int x1 = std::min(ctx.roi.x2, (int)std::ceil(c.first + r + 1.0));
    const int y0 = std::max(ctx.roi.y1, (int)std::floor(c.second - r - 1.0));
    const int y1 = std::min(ctx.roi.y2, (int)std::ceil(c.second + r + 1.0));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double d = std::sqrt((px - c.first) * (px - c.first) + (py - c.second) * (py - c.second));
            const double a = std::max(0.0, std::min(1.0, r + 0.5 - d));
            if (a > 0.0) {
                float& dst = (*ctx.cov)[ (std::size_t)(y - ctx.roi.y1) * ctx.w + (x - ctx.roi.x1) ];
                dst = std::max(dst, (float)(a * v));
            }
        }
    }
}

// Quad / triangle emission through the polygon filler (convex shapes).
void
emitPoly3(const RPoint& a, const RPoint& b, const RPoint& c, RasterCtx& ctx, double v)
{
    std::vector<RPoint> poly;
    poly.push_back(a); poly.push_back(b); poly.push_back(c);
    fillPolygonEvenOdd(poly, ctx, v);
}

void
emitQuad(const RPoint& a, const RPoint& b, const RPoint& c, const RPoint& d, RasterCtx& ctx, double v)
{
    std::vector<RPoint> poly;
    poly.push_back(a); poly.push_back(b); poly.push_back(c); poly.push_back(d);
    fillPolygonEvenOdd(poly, ctx, v);
}

// One stroked segment between a and b with independent end half-widths.
void
emitSegmentQuad(const RPoint& a, const RPoint& b, double ra, double rb, RasterCtx& ctx)
{
    const double dx = b.first - a.first, dy = b.second - a.second;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-9) {
        return;
    }
    const double nx = -dy / len, ny = dx / len;
    emitQuad(RPoint(a.first + nx * ra, a.second + ny * ra),
             RPoint(b.first + nx * rb, b.second + ny * rb),
             RPoint(b.first - nx * rb, b.second - ny * rb),
             RPoint(a.first - nx * ra, a.second - ny * ra),
             ctx, 1.0);
}

void
emitCap(const RPoint& pt, double r, const PenStyle& style, RasterCtx& ctx)
{
    if (style.endType == 1 || style.forceRoundCaps) {
        emitDisc(pt, r * std::max(0.0, std::min(1.0, style.endRoundness)), ctx, 1.0);
    }
    // Simple (butt): the quad edge is already flat.
}

void
emitJoin(const RPoint& prev, const RPoint& v, const RPoint& next, double r,
         const PenStyle& style, RasterCtx& ctx)
{
    double d0x = v.first - prev.first, d0y = v.second - prev.second;
    double d1x = next.first - v.first, d1y = next.second - v.second;
    const double l0 = std::sqrt(d0x * d0x + d0y * d0y);
    const double l1 = std::sqrt(d1x * d1x + d1y * d1y);
    if (l0 < 1e-9 || l1 < 1e-9) {
        return;
    }
    d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
    const double cross = d0x * d1y - d0y * d1x;
    if (std::fabs(cross) < 1e-6) {
        // Straight (or 180-degree reversal): both quads already cover it;
        // a disc at the vertex makes the reversal gap-free too.
        if (d0x * d1x + d0y * d1y < 0.0) {
            emitDisc(v, r, ctx, 1.0);
        }
        return;
    }
    // Left normals; the joint's outer side is opposite the turn direction.
    const double n0x = -d0y, n0y = d0x;
    const double n1x = -d1y, n1y = d1x;
    const double side = (cross > 0.0) ? -1.0 : 1.0;
    const RPoint c0(v.first + n0x * side * r, v.second + n0y * side * r);
    const RPoint c1(v.first + n1x * side * r, v.second + n1y * side * r);

    if (style.joinType == 2) { // Round
        emitDisc(v, r, ctx, 1.0);
        return;
    }
    if (style.joinType == 1) { // Bevel
        emitPoly3(v, c0, c1, ctx, 1.0);
        return;
    }
    // Miter: intersect the two offset lines; bevel fallback beyond the limit.
    const double t = ( (c1.first - c0.first) * d1y - (c1.second - c0.second) * d1x ) /
                     ( d0x * d1y - d0y * d1x + 1e-12 );
    const RPoint tip(c0.first + t * d0x, c0.second + t * d0y);
    const double miterLen = std::sqrt( (tip.first - v.first) * (tip.first - v.first)
                                       + (tip.second - v.second) * (tip.second - v.second) );
    if (miterLen <= 4.0 * r) {
        emitPoly3(c0, tip, c1, ctx, 1.0);
    } else {
        emitPoly3(v, c0, c1, ctx, 1.0);
    }
}

/// Walk the closed/open polyline and emit the stroke.
void
strokePolyline(const std::vector<RPoint>& pts, bool closed, const PenStyle& style, RasterCtx& ctx)
{
    if (pts.size() < 2) {
        return;
    }
    const int n = (int)pts.size();
    const int segs = closed ? n : (n - 1);

    // Cumulative arc lengths.
    std::vector<double> cum((std::size_t)segs + 1, 0.0);
    double total = 0.0;
    for (int i = 0; i < segs; ++i) {
        const RPoint& a = pts[i];
        const RPoint& b = pts[(i + 1) % n];
        total += std::sqrt( (b.first - a.first) * (b.first - a.first) + (b.second - a.second) * (b.second - a.second) );
        cum[i + 1] = total;
    }
    if (total <= 0.0) {
        return;
    }

    // Point + outward end direction at arc position u.
    auto pointAt = [&](double u, RPoint* p, RPoint* dir) {
        u = std::max(0.0, std::min(total, u));
        int seg = (int)(std::lower_bound(cum.begin(), cum.end(), u) - cum.begin() - 1);
        seg = std::max(0, std::min(segs - 1, seg));
        const RPoint& a = pts[seg];
        const RPoint& b = pts[(seg + 1) % n];
        const double segLen = cum[seg + 1] - cum[seg];
        const double t = segLen > 0.0 ? (u - cum[seg]) / segLen : 0.0;
        p->first = a.first + (b.first - a.first) * t;
        p->second = a.second + (b.second - a.second) * t;
        if (dir) {
            double dx = b.first - a.first, dy = b.second - a.second;
            const double len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-9) {
                dx /= len; dy /= len;
            }
            dir->first = dx; dir->second = dy;
        }
    };

    if (style.pattern.empty()) {
        // Solid: per-segment quads + joins + open-end caps.
        for (int i = 0; i < segs; ++i) {
            const double t0 = cum[i] / total;
            const double t1 = cum[i + 1] / total;
            emitSegmentQuad(pts[i], pts[(i + 1) % n], penWidthAt(style, t0) * 0.5,
                            penWidthAt(style, t1) * 0.5, ctx);
        }
        const int joinCount = closed ? n : (n - 2);
        for (int j = 0; j < joinCount; ++j) {
            const int iPrev = (j - 1 + n) % n;
            const int iNext = (j + 1) % n;
            const double r = penWidthAt(style, (double)j / n) * 0.5;
            emitJoin(pts[iPrev], pts[j], pts[iNext], r, style, ctx);
        }
        if (!closed) {
            RPoint dir;
            pointAt(0.0, (RPoint*)0, &dir);
            dir.first = -dir.first; dir.second = -dir.second; // pointing away from the stroke
            emitCap(pts[0], penWidthAt(style, 0.0) * 0.5, style, ctx);
            pointAt(total, (RPoint*)0, &dir);
            emitCap(pts[n - 1], penWidthAt(style, 1.0) * 0.5, style, ctx);
        }
        return;
    }

    // Patterned: walk the arc, emitting each "on" run as quad + caps.
    std::size_t patIdx = 0;
    bool on = true;
    double u = 0.0;
    std::size_t guard = 0;
    const std::size_t guardMax = (std::size_t)(total * 4.0 + 64.0);
    while (u < total && guard++ < guardMax) {
        double remain = on ? style.pattern[patIdx].first : style.pattern[patIdx].second;
        remain = std::max(0.0, remain);
        const double u1 = std::min(total, u + remain);
        if (on && u1 > u) {
            RPoint a, b, dirA, dirB;
            const double t0 = u / total, t1 = u1 / total;
            pointAt(u, &a, &dirA);
            pointAt(u1, &b, &dirB);
            emitSegmentQuad(a, b, penWidthAt(style, t0) * 0.5, penWidthAt(style, t1) * 0.5, ctx);
            emitCap(a, penWidthAt(style, t0) * 0.5, style, ctx);
            emitCap(b, penWidthAt(style, t1) * 0.5, style, ctx);
        }
        u = u1;
        if (on) {
            // Move to the "off" phase of the same pattern entry.
            on = false;
        } else {
            on = true;
            patIdx = (patIdx + 1) % style.pattern.size();
        }
    }
}

// ---------------------------------------------------------------------------
// Pen stroke -> 3D geometry
//
// The same PenStyle the 2D rasterizer draws, emitted as closed contours in
// canonical (format pixel) space so MaskMesh can union them with the brush
// fill (flat) or extrude them like any other shape. A solid stroke becomes
// ONE band polygon (outer ring forward + inner ring backward, cut once at
// vertex 0) so the flat union keeps the ribbon's hole and the extrusion
// produces a single ring-shaped slab. Patterned strokes become one quad per
// dash plus round caps, mirroring strokePolyline.
// ---------------------------------------------------------------------------

static double
polySignedArea(const std::vector<std::pair<double, double> >& p)
{
    double a = 0.0;
    const std::size_t n = p.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::pair<double, double>& q = p[i];
        const std::pair<double, double>& r = p[(i + 1) % n];
        a += q.first * r.second - r.first * q.second;
    }
    return a * 0.5;
}

/// Offset point(s) at vertex i of a closed polyline, on `side` (+1 = left
/// normal, -1 = right normal), honouring the join type (0 miter, 1 bevel,
/// 2 round). Appends 1..k points to `ring`.
static void
ringJoin(const std::vector<std::pair<double, double> >& pts,
         const std::vector<double>& halfW, std::size_t i,
         int side, int joinType,
         std::vector<std::pair<double, double> >* ring)
{
    const std::size_t n = pts.size();
    const std::pair<double, double>& cur = pts[i];
    const std::pair<double, double>& prv = pts[(i + n - 1) % n];
    const std::pair<double, double>& nxt = pts[(i + 1) % n];
    const double h = halfW[i];
    double d0x = cur.first - prv.first, d0y = cur.second - prv.second;
    double d1x = nxt.first - cur.first, d1y = nxt.second - cur.second;
    const double l0 = std::sqrt(d0x * d0x + d0y * d0y);
    const double l1 = std::sqrt(d1x * d1x + d1y * d1y);
    if (l0 < 1e-12 || l1 < 1e-12) {
        return;
    }
    d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
    const double n0x = -d0y * side, n0y = d0x * side;
    const double n1x = -d1y * side, n1y = d1x * side;
    const double cross = d0x * d1y - d0y * d1x;
    if (std::fabs(cross) < 1e-9) {
        ring->push_back(std::make_pair(cur.first + n0x * h, cur.second + n0y * h));
        return;
    }
    if (joinType == 1) { // bevel
        ring->push_back(std::make_pair(cur.first + n0x * h, cur.second + n0y * h));
        ring->push_back(std::make_pair(cur.first + n1x * h, cur.second + n1y * h));
        return;
    }
    if (joinType == 2) { // round
        const double kPi = 3.14159265358979323846;
        const double a0 = std::atan2(n0y, n0x);
        double da = std::atan2(n1y, n1x) - a0;
        while (da > kPi) da -= 2.0 * kPi;
        while (da < -kPi) da += 2.0 * kPi;
        const int steps = std::max(2, (int)std::ceil(std::fabs(da) / (kPi / 8.0)));
        for (int k = 0; k <= steps; ++k) {
            const double a = a0 + da * (double)k / (double)steps;
            ring->push_back(std::make_pair(cur.first + std::cos(a) * h,
                                           cur.second + std::sin(a) * h));
        }
        return;
    }
    // Miter: intersect the two offset lines, clamp to 4x (bevel beyond).
    double mx = n0x + n1x, my = n0y + n1y;
    const double ml = std::sqrt(mx * mx + my * my);
    if (ml < 1e-9) {
        ring->push_back(std::make_pair(cur.first + n0x * h, cur.second + n0y * h));
        return;
    }
    mx /= ml; my /= ml;
    const double cosHalf = mx * n0x + my * n0y;
    if (cosHalf < 0.25) {
        ring->push_back(std::make_pair(cur.first + n0x * h, cur.second + n0y * h));
        ring->push_back(std::make_pair(cur.first + n1x * h, cur.second + n1y * h));
        return;
    }
    const double s = h / cosHalf;
    ring->push_back(std::make_pair(cur.first + mx * s, cur.second + my * s));
}

/// Closed stroke band as ONE polygon: outer ring forward, inner ring
/// backward, cut once at vertex 0 (the cut edges are radial segments at the
/// same vertex, so the band stays closed).
static bool
strokeBandContour(const std::vector<std::pair<double, double> >& pts,
                  const std::vector<double>& halfW,
                  int outerSide, int joinType,
                  std::vector<std::pair<double, double> >* out)
{
    const std::size_t n = pts.size();
    if (n < 3) {
        return false;
    }
    const int innerSide = -outerSide;
    const std::pair<double, double>& v0 = pts[0];
    double d1x = pts[1].first - v0.first, d1y = pts[1].second - v0.second;
    double d0x = v0.first - pts[n - 1].first, d0y = v0.second - pts[n - 1].second;
    const double l0 = std::sqrt(d0x * d0x + d0y * d0y);
    const double l1 = std::sqrt(d1x * d1x + d1y * d1y);
    if (l0 < 1e-12 || l1 < 1e-12) {
        return false;
    }
    d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
    const double h0 = halfW[0];

    std::vector<std::pair<double, double> > outer, inner;
    // Outer ring: start at vertex 0 along the outgoing normal, full joins at
    // 1..n-1, end at vertex 0 along the incoming normal (the cut).
    outer.push_back(std::make_pair(v0.first + (-d1y * outerSide) * h0,
                                   v0.second + (d1x * outerSide) * h0));
    for (std::size_t i = 1; i < n; ++i) {
        ringJoin(pts, halfW, i, outerSide, joinType, &outer);
    }
    outer.push_back(std::make_pair(v0.first + (-d0y * outerSide) * h0,
                                   v0.second + (d0x * outerSide) * h0));

    // Inner ring: bevel only (a miter on the concave side self-crosses on
    // sharp corners; at sub-pixel scale the difference is invisible).
    inner.push_back(std::make_pair(v0.first + (-d1y * innerSide) * h0,
                                   v0.second + (d1x * innerSide) * h0));
    for (std::size_t i = 1; i < n; ++i) {
        ringJoin(pts, halfW, i, innerSide, 1, &inner);
    }
    inner.push_back(std::make_pair(v0.first + (-d0y * innerSide) * h0,
                                   v0.second + (d0x * innerSide) * h0));

    out->clear();
    out->reserve(outer.size() + inner.size());
    out->insert(out->end(), outer.begin(), outer.end());
    out->insert(out->end(), inner.rbegin(), inner.rend());
    return true;
}

/// Extract the open sub-polyline covering arc fractions [u0, u1] of a closed
/// polyline (WriteOn reveal for the 2D stroke).
static bool
arcSubPolyline(const std::vector<RPoint>& pts, double u0, double u1,
               std::vector<RPoint>* out)
{
    const std::size_t n = pts.size();
    if (n < 2) {
        return false;
    }
    std::vector<double> cum(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const RPoint& a = pts[i];
        const RPoint& b = pts[(i + 1) % n];
        cum[i + 1] = cum[i] + std::sqrt((b.first - a.first) * (b.first - a.first)
                                        + (b.second - a.second) * (b.second - a.second));
    }
    const double total = cum[n];
    if (total <= 0.0) {
        return false;
    }
    u0 = std::max(0.0, std::min(total, u0 * total));
    u1 = std::max(u0, std::min(total, u1 * total));
    if (u1 - u0 < 1e-9) {
        return false;
    }
    out->clear();
    for (std::size_t i = 0; i < n; ++i) {
        if (cum[i + 1] <= u0 || cum[i] >= u1) {
            continue;
        }
        const double s0 = std::max(u0, cum[i]);
        const double s1 = std::min(u1, cum[i + 1]);
        const double segLen = cum[i + 1] - cum[i];
        const RPoint& a = pts[i];
        const RPoint& b = pts[(i + 1) % n];
        if (out->empty() && s0 > cum[i]) {
            const double t0 = segLen > 0.0 ? (s0 - cum[i]) / segLen : 0.0;
            out->push_back(RPoint(a.first + (b.first - a.first) * t0,
                                  a.second + (b.second - a.second) * t0));
        } else if (out->empty()) {
            out->push_back(a);
        }
        if (s1 >= cum[i + 1] - 1e-12) {
            out->push_back(b);
        } else {
            const double t1 = segLen > 0.0 ? (s1 - cum[i]) / segLen : 0.0;
            out->push_back(RPoint(a.first + (b.first - a.first) * t1,
                                  a.second + (b.second - a.second) * t1));
        }
    }
    return out->size() >= 2;
}

static void
appendDiscContour(double cx, double cy, double r,
                  std::vector<MaskMesh::Contour>* out)
{
    if (r <= 0.0) {
        return;
    }
    const int segs = 16;
    const double kPi = 3.14159265358979323846;
    std::vector<std::pair<double, double> > p;
    p.reserve(segs);
    for (int i = 0; i < segs; ++i) {
        const double a = 2.0 * kPi * (double)i / (double)segs;
        p.push_back(std::make_pair(cx + std::cos(a) * r, cy + std::sin(a) * r));
    }
    MaskMesh::Contour c;
    c.points.swap(p);
    c.op = -1;
    out->push_back(c);
}

/// Patterned stroke: one quad per "on" run (a straight segment between the
/// run endpoints, like the 2D rasterizer) plus round caps / dots.
static void
appendDashContours(const std::vector<std::pair<double, double> >& pts,
                   const PenStyle& pen,
                   std::vector<MaskMesh::Contour>* out)
{
    const std::size_t n = pts.size();
    std::vector<double> cum(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::pair<double, double>& a = pts[i];
        const std::pair<double, double>& b = pts[(i + 1) % n];
        cum[i + 1] = cum[i] + std::sqrt((b.first - a.first) * (b.first - a.first)
                                        + (b.second - a.second) * (b.second - a.second));
    }
    const double total = cum[n];
    if (total <= 0.0) {
        return;
    }
    const std::size_t segs = n;
    auto pointAt = [&](double u, std::pair<double, double>* p) {
        u = std::max(0.0, std::min(total, u));
        std::size_t seg = (std::size_t)(std::lower_bound(cum.begin(), cum.end(), u) - cum.begin());
        seg = (seg == 0) ? 0 : seg - 1;
        seg = std::min(segs - 1, seg);
        const std::pair<double, double>& a = pts[seg];
        const std::pair<double, double>& b = pts[(seg + 1) % n];
        const double segLen = cum[seg + 1] - cum[seg];
        const double t = segLen > 0.0 ? (u - cum[seg]) / segLen : 0.0;
        p->first = a.first + (b.first - a.first) * t;
        p->second = a.second + (b.second - a.second) * t;
    };

    const bool roundCap = pen.forceRoundCaps
                          || (pen.endType == 1 && pen.endRoundness > 0.0);
    const double capScale = pen.forceRoundCaps ? 1.0 : pen.endRoundness;

    std::size_t patIdx = 0;
    bool on = true;
    double u = 0.0;
    std::size_t guard = 0;
    const std::size_t guardMax = (std::size_t)(total * 4.0 + 64.0);
    while (u < total && guard++ < guardMax) {
        double remain = on ? pen.pattern[patIdx].first : pen.pattern[patIdx].second;
        remain = std::max(0.0, remain);
        const double u1 = std::min(total, u + remain);
        if (on && u1 > u) {
            std::pair<double, double> a, b;
            pointAt(u, &a);
            pointAt(u1, &b);
            const double ra = penWidthAt(pen, u / total) * 0.5;
            const double rb = penWidthAt(pen, u1 / total) * 0.5;
            const double dx = b.first - a.first, dy = b.second - a.second;
            const double len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-9) {
                const double nx = -dy / len, ny = dx / len;
                std::vector<std::pair<double, double> > q;
                q.push_back(std::make_pair(a.first + nx * ra, a.second + ny * ra));
                q.push_back(std::make_pair(b.first + nx * rb, b.second + ny * rb));
                q.push_back(std::make_pair(b.first - nx * rb, b.second - ny * rb));
                q.push_back(std::make_pair(a.first - nx * ra, a.second - ny * ra));
                MaskMesh::Contour c;
                c.points.swap(q);
                c.op = -1;
                out->push_back(c);
            }
            if (roundCap) {
                appendDiscContour(a.first, a.second, ra * capScale, out);
                appendDiscContour(b.first, b.second, rb * capScale, out);
            }
        }
        u = u1;
        if (on) {
            on = false;
        } else {
            on = true;
            patIdx = (patIdx + 1) % pen.pattern.size();
        }
    }
}

/// Pen stroke clipped to the WriteOn arc window [u0, u1] (fractions of the
/// total arc): an open walk emitting one quad contour per source segment plus
/// round caps at the window ends - the geometry analogue of the 2D
/// strokePolyline on an open sub-polyline (the closed band does not apply:
/// the reveal opens the contour).
static void
appendArcStrokeContours(const std::vector<std::pair<double, double> >& pts,
                        const std::vector<double>& cum, double total,
                        double u0, double u1, const PenStyle& pen,
                        std::vector<MaskMesh::Contour>* out)
{
    const std::size_t n = pts.size();
    if (n < 2 || total <= 0.0) {
        return;
    }
    u0 = std::max(0.0, std::min(total, u0));
    u1 = std::max(u0, std::min(total, u1));
    if (u1 - u0 < 1e-9) {
        return;
    }
    auto pointAt = [&](double u, std::pair<double, double>* p) {
        u = std::max(0.0, std::min(total, u));
        std::size_t seg = (std::size_t)(std::lower_bound(cum.begin(), cum.end(), u) - cum.begin());
        seg = (seg == 0) ? 0 : seg - 1;
        seg = std::min(n - 1, seg);
        const std::pair<double, double>& a = pts[seg];
        const std::pair<double, double>& b = pts[(seg + 1) % n];
        const double segLen = cum[seg + 1] - cum[seg];
        const double t = segLen > 0.0 ? (u - cum[seg]) / segLen : 0.0;
        p->first = a.first + (b.first - a.first) * t;
        p->second = a.second + (b.second - a.second) * t;
    };

    // The dash pattern applies INSIDE the window (a revealed dash pattern
    // shows whole dashes as they come into the window).
    if (!pen.pattern.empty()) {
        std::size_t patIdx = 0;
        bool on = true;
        double u = u0;
        std::size_t guard = 0;
        const std::size_t guardMax = (std::size_t)((u1 - u0) * 4.0 + 64.0);
        while (u < u1 && guard++ < guardMax) {
            double remain = on ? pen.pattern[patIdx].first : pen.pattern[patIdx].second;
            remain = std::max(0.0, remain);
            const double uRun = std::min(u1, u + remain);
            if (on && uRun > u) {
                std::pair<double, double> a, b;
                pointAt(u, &a);
                pointAt(uRun, &b);
                const double ra = penWidthAt(pen, u / total) * 0.5;
                const double rb = penWidthAt(pen, uRun / total) * 0.5;
                const double dx = b.first - a.first, dy = b.second - a.second;
                const double len = std::sqrt(dx * dx + dy * dy);
                if (len > 1e-9) {
                    const double nx = -dy / len, ny = dx / len;
                    std::vector<std::pair<double, double> > q;
                    q.push_back(std::make_pair(a.first + nx * ra, a.second + ny * ra));
                    q.push_back(std::make_pair(b.first + nx * rb, b.second + ny * rb));
                    q.push_back(std::make_pair(b.first - nx * rb, b.second - ny * rb));
                    q.push_back(std::make_pair(a.first - nx * ra, a.second - ny * ra));
                    MaskMesh::Contour c;
                    c.points.swap(q);
                    c.op = -1;
                    out->push_back(c);
                }
                if (pen.forceRoundCaps || (pen.endType == 1 && pen.endRoundness > 0.0)) {
                    const double capScale = pen.forceRoundCaps ? 1.0 : pen.endRoundness;
                    appendDiscContour(a.first, a.second, ra * capScale, out);
                    appendDiscContour(b.first, b.second, rb * capScale, out);
                }
            }
            u = uRun;
            if (on) {
                on = false;
            } else {
                on = true;
                patIdx = (patIdx + 1) % pen.pattern.size();
            }
        }
        return;
    }

    // Solid: one quad per source segment inside the window + end caps.
    bool first = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (cum[i + 1] <= u0 || cum[i] >= u1) {
            continue;
        }
        const double s0 = std::max(u0, cum[i]);
        const double s1 = std::min(u1, cum[i + 1]);
        if (s1 - s0 < 1e-12) {
            continue;
        }
        const double ta = s0 / total, tb = s1 / total;
        const double ra = penWidthAt(pen, ta) * 0.5;
        const double rb = penWidthAt(pen, tb) * 0.5;
        const std::pair<double, double>& a0 = pts[i];
        const std::pair<double, double>& b0 = pts[(i + 1) % n];
        const double segLen = cum[i + 1] - cum[i];
        const double t0 = segLen > 0.0 ? (s0 - cum[i]) / segLen : 0.0;
        const double t1 = segLen > 0.0 ? (s1 - cum[i]) / segLen : 0.0;
        const std::pair<double, double> a(a0.first + (b0.first - a0.first) * t0,
                                          a0.second + (b0.second - a0.second) * t0);
        const std::pair<double, double> b(a0.first + (b0.first - a0.first) * t1,
                                          a0.second + (b0.second - a0.second) * t1);
        const double dx = b.first - a.first, dy = b.second - a.second;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-9) {
            const double nx = -dy / len, ny = dx / len;
            std::vector<std::pair<double, double> > q;
            q.push_back(std::make_pair(a.first + nx * ra, a.second + ny * ra));
            q.push_back(std::make_pair(b.first + nx * rb, b.second + ny * rb));
            q.push_back(std::make_pair(b.first - nx * rb, b.second - ny * rb));
            q.push_back(std::make_pair(a.first - nx * ra, a.second - ny * ra));
            MaskMesh::Contour c;
            c.points.swap(q);
            c.op = -1;
            out->push_back(c);
        }
        // Round the revealed start (once) and end (once).
        const bool roundCap = pen.endType == 1 && pen.endRoundness > 0.0;
        if (roundCap) {
            if (first) {
                appendDiscContour(a.first, a.second, ra * pen.endRoundness, out);
            }
            appendDiscContour(b.first, b.second, rb * pen.endRoundness, out);
        }
        first = false;
    }
}

/// Append the pen stroke of one closed contour as geometry contours.
/// writeOnU0/writeOnU1: the visible arc window (fractions of the total arc;
/// the full contour when WriteOn is off).
static void
appendStrokeContours(const std::vector<std::pair<double, double> >& ptsIn,
                     const PenStyle& pen,
                     double writeOnU0, double writeOnU1,
                     std::vector<MaskMesh::Contour>* out)
{
    if (ptsIn.size() < 3 || pen.thickness <= 0.0) {
        return;
    }
    // Dedupe consecutive points and the closure duplicate: the traced
    // contours carry the closing vertex again, and a zero-length segment
    // breaks the band construction (buildMesh cleans its own fill path).
    std::vector<std::pair<double, double> > pts;
    pts.reserve(ptsIn.size());
    for (std::size_t i = 0; i < ptsIn.size(); ++i) {
        if (!pts.empty()) {
            const double dx = ptsIn[i].first - pts.back().first;
            const double dy = ptsIn[i].second - pts.back().second;
            if (dx * dx + dy * dy < 1e-12) {
                continue;
            }
        }
        pts.push_back(ptsIn[i]);
    }
    while (pts.size() > 3) {
        const double dx = pts.front().first - pts.back().first;
        const double dy = pts.front().second - pts.back().second;
        if (dx * dx + dy * dy < 1e-12) {
            pts.pop_back();
        } else {
            break;
        }
    }
    if (pts.size() < 3) {
        return;
    }
    const std::size_t n = pts.size();
    std::vector<double> cum(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::pair<double, double>& a = pts[i];
        const std::pair<double, double>& b = pts[(i + 1) % n];
        cum[i + 1] = cum[i] + std::sqrt((b.first - a.first) * (b.first - a.first)
                                        + (b.second - a.second) * (b.second - a.second));
    }
    const double total = cum[n];

    const double wU0 = std::max(0.0, std::min(1.0, writeOnU0));
    const double wU1 = std::max(0.0, std::min(1.0, writeOnU1));
    const bool writeOn = (wU0 > 1e-9 || wU1 < 1.0 - 1e-9) && wU1 > wU0;
    if (writeOn) {
        // Revealed arc: an open walk (quads + caps) - the band assumes closed.
        appendArcStrokeContours(pts, cum, total, wU0 * total, wU1 * total, pen, out);
        return;
    }

    if (!pen.pattern.empty()) {
        appendDashContours(pts, pen, out);
        return;
    }
    std::vector<double> halfW(n);
    for (std::size_t i = 0; i < n; ++i) {
        halfW[i] = penWidthAt(pen, total > 0.0 ? cum[i] / total : 0.0) * 0.5;
    }
    // Signed area > 0 (CCW): the interior is on the +normal side, so the
    // outer ring is the -normal side.
    const int outerSide = (polySignedArea(pts) > 0.0) ? -1 : 1;
    std::vector<std::pair<double, double> > band;
    if (strokeBandContour(pts, halfW, outerSide, pen.joinType, &band)) {
        MaskMesh::Contour c;
        c.points.swap(band);
        c.op = -1;
        out->push_back(c);
    }
}

} // anonymous namespace

MeshDataPtr
TraceMask::buildMeshFromDoc(const VectorMask::VectorDocument& doc, double time) const
{
    // The mesh is what the 2D output shows: the brush fill when the brush is
    // enabled, plus the pen outline as a stroke ribbon when the pen is
    // enabled. Both are emitted as canonical-space contours so MaskMesh
    // unions them (flat) or extrudes them (Extrude Depth) like roto shapes.
    const KnobBoolPtr brushKnob = _imp->brushEnabled.lock();
    const bool brushOn = brushKnob ? brushKnob->getValue() : true;
    const KnobBoolPtr penKnob = _imp->penEnabled.lock();
    const bool penOn = penKnob ? penKnob->getValue() : true;
    if (!brushOn && !penOn) {
        return MeshDataPtr();
    }
    const PenStyle pen = readPenStyle(_imp.get(), time);

    // WriteOn window (arc fractions; the full contour at the defaults).
    double writeOnU0 = 0.0, writeOnU1 = 1.0;
    {
        const KnobDoublePtr sK = _imp->writeOnStart.lock();
        const KnobDoublePtr eK = _imp->writeOnEnd.lock();
        writeOnU0 = sK ? std::max(0.0, std::min(1.0, sK->getValueAtTime(time))) : 0.0;
        writeOnU1 = eK ? std::max(0.0, std::min(1.0, eK->getValueAtTime(time))) : 1.0;
    }

    std::vector<MaskMesh::Contour> contours;
    contours.reserve(doc.shapes.size() * 2);
    for (std::size_t i = 0; i < doc.shapes.size(); ++i) {
        std::vector<std::pair<double, double> > poly;
        // Traced shapes carry zero tangents (straight segments): one sample
        // per segment reproduces the polygon EXACTLY and keeps the mesh
        // build (tessellation + extrusion) cheap on dense traced contours.
        if (!VectorMask::flattenShape(doc.shapes[i], &poly, 1)) {
            continue;
        }
        const int op = doc.shapes[i].compositingOperator;
        if (brushOn) {
            MaskMesh::Contour c;
            c.points = poly;
            c.op = op;
            contours.push_back(c);
        }
        if (penOn && !MaskMesh::isSubtractiveOperator(op)) {
            // Hole contours don't get an outline (same rule as the 2D raster).
            appendStrokeContours(poly, pen, writeOnU0, writeOnU1, &contours);
        }
    }
    if (contours.empty()) {
        return MeshDataPtr();
    }

    // Same normalization as MaskToGeo: format height -> World Scale,
    // centered on the project format.
    Format fmt;
    getApp()->getProject()->getProjectDefaultFormat(&fmt);
    const double formatW = fmt.x2 - fmt.x1;
    const double formatH = fmt.y2 - fmt.y1;
    const KnobDoublePtr worldScaleKnob = _imp->worldScale.lock();
    const double worldScale = worldScaleKnob ? worldScaleKnob->getValueAtTime(time) : 1.0;
    const double s = (formatH > 0.0) ? worldScale / formatH : 1.0;
    const double cx = fmt.x1 + formatW / 2.0;
    const double cy = fmt.y1 + formatH / 2.0;
    const KnobDoublePtr depthKnob = _imp->extrudeDepth.lock();
    const double depth = depthKnob ? std::max(0.0, depthKnob->getValueAtTime(time)) : 0.0;
    const KnobDoublePtr bevelKnob = _imp->bevel.lock();
    const double bevel = bevelKnob ? std::max(0.0, bevelKnob->getValueAtTime(time)) : 0.0;

    return MaskMesh::buildMesh(contours, s, cx, cy, formatW, formatH, depth, bevel);
}

StatusEnum
TraceMask::render(const RenderActionArgs& args)
{
    // Serialize with the worker's fetch (see traceInternal).
    QMutexLocker gate(&_imp->renderGate);
    VectorMask::VectorDocument doc;
    bool hasDoc = false;
    double docTime = -1.0;
    {
        QMutexLocker l(&_imp->stateMutex);
        hasDoc = _imp->hasDoc;
        docTime = _imp->docTime;
        if (hasDoc) {
            doc = _imp->doc;
        }
    }

    // Inline trace, three cases: nothing traced yet (bootstrap), an EMPTY
    // published doc (an early trace of an input that was not ready yet - the
    // shapes would otherwise be stuck blank until an unrelated event), or a
    // render for a frame the published doc doesn't cover (sequences: the
    // shapes follow the input frame by frame even without a timeline event,
    // e.g. in the CLI renderer). The input image is already at hand in this
    // render - pure math, no nested render. Input/knob changes re-trace
    // through the async path.
    const bool bootstrap = !hasDoc || doc.shapes.empty();
    const bool mayInline = bootstrap
                           ? !_imp->inlineTraced && !_imp->traceRunning.load()
                           : (docTime != args.time && !_imp->traceRunning.load());
    auto it = mayInline ? args.inputImages.find(0) : args.inputImages.end();
    // Only trace from a scale-1 input image: a proxy render passes a
    // downscaled image whose buffer coordinates would trace into a
    // half-size document (the async path always fetches at scale 1).
    if (mayInline && it != args.inputImages.end() && !it->second.empty()
        && it->second.front() && it->second.front()->getMipmapLevel() == 0) {
        if (bootstrap) {
            _imp->inlineTraced = true;
        }
        // Inline bootstrap: read the trace knobs here (a render action is
        // a sanctioned knob-read context, unlike the raw worker thread).
        const KnobChoicePtr channelKnob = _imp->mapChannel.lock();
        const int channelIdx = channelKnob ? channelKnob->getValue() : (int)VectorMask::ePlaneChannelLuminance;
        const KnobDoublePtr thresholdKnob = _imp->threshold.lock();
        const float threshold = thresholdKnob ? (float)thresholdKnob->getValue() : 0.5f;
        const KnobBoolPtr invertKnob = _imp->invert.lock();
        const bool invert = invertKnob ? invertKnob->getValue() : false;
        const KnobIntPtr precKnob = _imp->curvePrecision.lock();
        const int precision = precKnob ? std::max(1, precKnob->getValue()) : 8;
        traceFromImage(it->second.front(), args.time, channelIdx, threshold, invert, precision);
        QMutexLocker l(&_imp->stateMutex);
        hasDoc = _imp->hasDoc;
        if (hasDoc) {
            doc = _imp->doc;
        }
    }

    if (hasDoc && !doc.shapes.empty()) {
        rasterize(doc, args);
    }
    return eStatusOK;
}

void
TraceMask::rasterize(const VectorMask::VectorDocument& doc, const RenderActionArgs& args) const
{
    // Shading knobs.
    const bool brushOn = _imp->brushEnabled.lock() ? _imp->brushEnabled.lock()->getValue() : true;
    const bool penOn = _imp->penEnabled.lock() ? _imp->penEnabled.lock()->getValue() : true;
    if (!brushOn && !penOn) {
        return;
    }
    KnobColorPtr brushCol = _imp->brushColor.lock();
    KnobColorPtr penCol = _imp->penColor.lock();
    const double brushR = brushCol ? brushCol->getValue(0) : 0.8;
    const double brushG = brushCol ? brushCol->getValue(1) : 0.2;
    const double brushB = brushCol ? brushCol->getValue(2) : 0.3;
    const double brushA = brushCol ? brushCol->getValue(3) : 1.0;
    const double penR = penCol ? penCol->getValue(0) : 1.0;
    const double penG = penCol ? penCol->getValue(1) : 1.0;
    const double penB = penCol ? penCol->getValue(2) : 1.0;
    const double penA = penCol ? penCol->getValue(3) : 1.0;
    const double brushOp = _imp->brushOpacity.lock() ? _imp->brushOpacity.lock()->getValue() : 1.0;
    const double penOp = _imp->penOpacity.lock() ? _imp->penOpacity.lock()->getValue() : 1.0;

    const PenStyle pen = readPenStyle(_imp.get(), args.time);

    // Flatten the canonical shapes into output-pixel polylines once.
    std::vector<std::vector<RPoint> > polys;
    std::vector<bool> subtract;
    polys.reserve(doc.shapes.size());
    subtract.reserve(doc.shapes.size());
    for (std::size_t i = 0; i < doc.shapes.size(); ++i) {
        std::vector<std::pair<double, double> > canon;
        // Same as the mesh path: 1 sample per segment (exact for the
        // straight-segment traced shapes, 16x fewer points to rasterize).
        if (!VectorMask::flattenShape(doc.shapes[i], &canon, 1)) {
            continue;
        }
        subtract.push_back( MaskMesh::isSubtractiveOperator(doc.shapes[i].compositingOperator) );
        polys.push_back(std::vector<RPoint>());
        std::vector<RPoint>& poly = polys.back();
        poly.reserve(canon.size());
        for (std::size_t j = 0; j < canon.size(); ++j) {
            poly.push_back(RPoint(canon[j].first, canon[j].second));
        }
    }
    if (polys.empty()) {
        return;
    }

    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin();
         it != args.outputPlanes.end(); ++it) {
        ImagePtr img = it->second;
        if (!img) {
            continue;
        }
        RectI roi = args.roi;
        roi.intersect(img->getBounds());
        if (roi.isNull()) {
            continue;
        }
        const int w = roi.width();
        const int h = roi.height();
        const int dstNComps = img->getComponents().getNumComponents();
        if (dstNComps != 1 && dstNComps != 3 && dstNComps != 4) {
            continue;
        }
        const bool premultOut = img->getPremultiplication() == eImagePremultiplicationPremultiplied;
        // Canonical -> output pixels: the output image's own bounds/RoD
        // ratios fold in both the pixel aspect and the render scale (proxy),
        // so the shapes land 1:1 at scale 1 and stay aligned at any proxy
        // scale (the plane's RoD is in canonical units, its bounds in
        // scaled pixels).
        const RectD imgRod = img->getRoD();
        const RectI imgBounds = img->getBounds();
        const double canonToPixX = (imgRod.x2 - imgRod.x1) > 0.0
                                   ? (double)imgBounds.width() / (imgRod.x2 - imgRod.x1) : 1.0;
        const double canonToPixY = (imgRod.y2 - imgRod.y1) > 0.0
                                   ? (double)imgBounds.height() / (imgRod.y2 - imgRod.y1) : 1.0;

        // Canonical -> pixel polyline cache for this plane.
        std::vector<std::vector<RPoint> > pixPolys(polys.size());
        for (std::size_t i = 0; i < polys.size(); ++i) {
            pixPolys[i].reserve(polys[i].size());
            for (std::size_t j = 0; j < polys[i].size(); ++j) {
                pixPolys[i].push_back(RPoint((polys[i][j].first - imgRod.x1) * canonToPixX + imgBounds.x1,
                                             (polys[i][j].second - imgRod.y1) * canonToPixY + imgBounds.y1));
            }
        }

        // Pen thickness is defined in canonical (format) pixels - the same
        // space the traced contours live in - so the outline covers the same
        // document area at any zoom / proxy scale. The 2D viewer requests
        // downscaled images when zoomed out: a thickness used raw in output
        // pixels would grow the ink as the zoom shrinks. Scale it into this
        // plane's pixel space like the polylines above.
        PenStyle pixPen = pen;
        pixPen.thickness = pen.thickness * (canonToPixX + canonToPixY) * 0.5;
        if (pixPen.thickness < 0.05) {
            pixPen.thickness = 0.05;
        }
        for (std::size_t i = 0; i < pixPen.pattern.size(); ++i) {
            pixPen.pattern[i].first *= canonToPixX;
            pixPen.pattern[i].second *= canonToPixX;
        }

        std::vector<float> fillCov, strokeCov;
        if (brushOn) {
            fillCov.assign((std::size_t)w * h, 0.0f);
            RasterCtx ctx;
            ctx.roi = roi; ctx.w = w; ctx.h = h; ctx.cov = &fillCov;
            for (std::size_t i = 0; i < pixPolys.size(); ++i) {
                fillPolygonEvenOdd(pixPolys[i], ctx, subtract[i] ? -1.0 : 1.0);
            }
        }
        if (penOn) {
            strokeCov.assign((std::size_t)w * h, 0.0f);
            RasterCtx ctx;
            ctx.roi = roi; ctx.w = w; ctx.h = h; ctx.cov = &strokeCov;
            // WriteOn window: reveal the outline between the start/end arc
            // fractions. The sub-polyline walk needs per-contour arc lengths:
            // extract the visible portion as an open polyline and stroke it
            // open (the caps round the reveal ends like a drawn line).
            const KnobDoublePtr sK = _imp->writeOnStart.lock();
            const KnobDoublePtr eK = _imp->writeOnEnd.lock();
            const double writeOnU0 = sK ? std::max(0.0, std::min(1.0, sK->getValueAtTime(args.time))) : 0.0;
            const double writeOnU1 = eK ? std::max(0.0, std::min(1.0, eK->getValueAtTime(args.time))) : 1.0;
            const bool writeOnActive = writeOnU0 > 1e-9 || writeOnU1 < 1.0 - 1e-9;
            for (std::size_t i = 0; i < pixPolys.size(); ++i) {
                if (subtract[i]) {
                    continue; // hole contours don't get an outline
                }
                const std::vector<RPoint>& poly = pixPolys[i];
                if (writeOnActive && poly.size() >= 2) {
                    std::vector<RPoint> sub;
                    if (arcSubPolyline(poly, writeOnU0, writeOnU1, &sub) && sub.size() >= 2) {
                        strokePolyline(sub, false, pixPen, ctx);
                    }
                    continue;
                }
                strokePolyline(poly, true, pixPen, ctx);
            }
        }
        // Rasterize into a private local image first (DiskCacheNode
        // pattern), then copy into the output plane: avoids holding the
        // shared plane's entry lock during the whole rasterization.
        ImagePtr localImg = std::make_shared<Image>(it->first,
                                                    img->getRoD(),
                                                    img->getBounds(),
                                                    img->getMipmapLevel(),
                                                    img->getPixelAspectRatio(),
                                                    img->getBitDepth(),
                                                    img->getPremultiplication(),
                                                    img->getFieldingOrder(),
                                                    false);
        {
            Image::WriteAccess acc(localImg.get());
            for (int y = roi.y1; y < roi.y2; ++y) {
                for (int x = roi.x1; x < roi.x2; ++x) {
                    const std::size_t idx = (std::size_t)(y - roi.y1) * w + (x - roi.x1);
                    const double covF = brushOn ? fillCov[idx] : 0.0;
                    const double covP = penOn ? strokeCov[idx] : 0.0;
                    // over: pen over brush
                    const double aB = brushOn ? (brushA * brushOp * covF) : 0.0;
                    const double aP = penOn ? (penA * penOp * covP) : 0.0;
                    const double aOut = std::max(0.0, std::min(1.0, aP + aB * (1.0 - aP)));
                    double rC = 0.0, gC = 0.0, bC = 0.0;
                    if (aOut > 0.0) {
                        // Premultiplied over: pen over brush. Each source
                        // color is premultiplied by its own ink alpha
                        // (color alpha * opacity * coverage); the brush
                        // contribution is attenuated by the pen coverage
                        // (src-over).
                        const double attenB = 1.0 - aP;
                        rC = penR * aP + brushR * aB * attenB;
                        gC = penG * aP + brushG * aB * attenB;
                        bC = penB * aP + brushB * aB * attenB;
                        if (!premultOut) {
                            rC = aOut > 0.0 ? rC / aOut : 0.0;
                            gC = aOut > 0.0 ? gC / aOut : 0.0;
                            bC = aOut > 0.0 ? bC / aOut : 0.0;
                        }
                    }
                    unsigned char* pix = acc.pixelAt(x, y);
                    float* dst = (float*)pix;
                    switch (dstNComps) {
                    case 4:
                        dst[0] = (float)rC; dst[1] = (float)gC; dst[2] = (float)bC; dst[3] = (float)aOut;
                        break;
                    case 3:
                        dst[0] = (float)rC; dst[1] = (float)gC; dst[2] = (float)bC;
                        break;
                    case 1:
                        dst[0] = (float)aOut;
                        break;
                    default:
                        break;
                    }
                }
            }
        }
        img->pasteFrom(*localImg, roi, false);
    }
}

// Qt6 moc workaround: q20::identity not defined in Natron::q20 namespace
namespace Natron { namespace q20 {
    template<typename T>
    constexpr T&& identity(T&& t) noexcept { return std::forward<T>(t); }
}}

NATRON_NAMESPACE_EXIT

#include "moc_TraceMask.cpp"
