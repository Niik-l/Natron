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

// Derived from vectorNuke (https://github.com/erinwnash/vectorNuke), GPL-2.0.

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ReadMask.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <list>
#include <map>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"

#include "Dev/Mask/VectorDocument.h"
#include "Dev/Mask/parsers/SvgParser.h"

NATRON_NAMESPACE_ENTER

struct ReadMaskPrivate
{
    KnobPageWPtr page;
    KnobFileWPtr file;
    KnobButtonWPtr reimport;
    KnobChoiceWPtr fitMode;
    KnobDoubleWPtr customScale;
    KnobDoubleWPtr customOffset;
    KnobBoolWPtr invertY;
    KnobBoolWPtr importColors;
    KnobBoolWPtr groupByLayer;
    KnobBoolWPtr featherSplines;
    KnobBoolWPtr autoRebuild;

    bool knobsCreated = false;

    /// Everything the last import created (shapes + layers), for rebuild removal.
    std::list<std::weak_ptr<RotoItem> > importedItems;
};

namespace {

/**
 * @brief vectorNuke calculate_transform: effective (scaleX, scaleY, offsetX, offsetY)
 * from the fit mode against the project format.
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
    case 0: { // fit_format: uniform scale with a 95% margin, centered
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
    case 1: { // center_format: 1:1 (x user scale), centered
        *sx = userScaleX;
        *sy = userScaleY;
        const double scaledW = docW * (*sx);
        const double scaledH = docH * (*sy);
        *ox = (formatW - scaledW) / 2.0 + userOffsetX;
        *oy = (formatH - scaledH) / 2.0 + userOffsetY;
        break;
    }
    case 2: // original: 1:1 at bottom-left origin
    case 3: // custom
    default:
        *sx = userScaleX;
        *sy = userScaleY;
        *ox = userOffsetX;
        *oy = userOffsetY;
        break;
    }
}

} // anonymous namespace

ReadMask::ReadMask(NodePtr node)
    : RotoNode(node)
    , _imp(new ReadMaskPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

ReadMask::~ReadMask()
{
}

void
ReadMask::initializeData()
{
    if (_imp->knobsCreated) {
        return;
    }
    _imp->knobsCreated = true;

    // Knobs are created here, not in the constructor: node creation runs the effect
    // constructor while Node::_imp->effect is still being assigned, and knob value
    // evaluation (setDefaultValue -> apply -> Node::onEffectKnobValueChanged) needs the
    // node wired up. initializeData runs right after the effect is attached and before
    // loadKnobs; RotoPaint seals initializeKnobs, so this is the latest safe hook.
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("File"));
    _imp->page = page;
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("File"));
        k->setName("file");
        k->setHintToolTip(tr("The vector file to import (SVG in this version: full path "
                             "grammar, rect/circle/ellipse/line/polyline/polygon, nested "
                             "transforms and styles). The file is parsed into real Bézier "
                             "shapes in this node's Roto tree; output is the mask alpha "
                             "like any Roto."));
        page->addKnob(k);
        _imp->file = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Re-import"));
        k->setName("reimport");
        k->setHintToolTip(tr("Re-read the file and rebuild the shapes now, regardless of "
                             "Auto Rebuild. Also use this after editing the file on disk."));
        k->setCheckable(false);
        page->addKnob(k);
        _imp->reimport = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Fit"));
        k->setName("fitMode");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("fit_format", "Fit to Format",
                                    "Uniform scale with a 95% margin, centered in the project format."));
        opts.push_back(ChoiceOption("center_format", "Center to Format",
                                    "Native document size (1:1, times the custom scale), centered."));
        opts.push_back(ChoiceOption("original", "Original Size",
                                    "1:1 at the bottom-left origin."));
        opts.push_back(ChoiceOption("custom", "Custom",
                                    "Custom scale and offset only."));
        k->populateChoices(opts);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("How the document canvas maps onto the project format."));
        page->addKnob(k);
        _imp->fitMode = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Custom Scale"), 2);
        k->setName("customScale");
        k->setDefaultValue(1.0, 0);
        k->setDefaultValue(1.0, 1);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Scale applied by the Fit modes. Fit to Format multiplies it "
                             "with the uniform fit scale; the other modes use it directly."));
        page->addKnob(k);
        _imp->customScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Custom Offset"), 2);
        k->setName("customOffset");
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Offset added after the fit placement (canvas pixels)."));
        page->addKnob(k);
        _imp->customOffset = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Invert Y"));
        k->setName("invertY");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Vector files are top-left; the Roto canvas is bottom-left. "
                             "On (default) the shape points AND tangent handles flip in Y."));
        page->addKnob(k);
        _imp->invertY = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Import Colors"));
        k->setName("importColors");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Apply the vector fill/stroke color (fill first) and opacity "
                             "to each imported shape's color/opacity."));
        page->addKnob(k);
        _imp->importColors = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Group by Layer"));
        k->setName("groupByLayer");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Group imported shapes under a layer named after the file, "
                             "with a sub-layer per SVG group."));
        page->addKnob(k);
        _imp->groupByLayer = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Feather Splines"));
        k->setName("featherSplines");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Off (default): imported shapes get zero feather distance and "
                             "fall-off with feather points locked to the shape curve — the "
                             "vectorNuke clean single-line vectors. On: leave the feather "
                             "attributes at the node defaults."));
        page->addKnob(k);
        _imp->featherSplines = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Auto Rebuild"));
        k->setName("autoRebuild");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Re-import whenever the file or a placement knob changes. "
                             "Importing regenerates the shapes by design — user edits to "
                             "imported shapes are wiped by the next import. Turn off to edit "
                             "freely; the Re-import button still rebuilds on demand."));
        page->addKnob(k);
        _imp->autoRebuild = k;
    }

    // Knob change notifications (knobChanged is sealed by RotoPaint, so connect to the
    // knob signal handlers directly).
    struct ConnectEntry
    {
        KnobIPtr knob;
        bool isFile;
        bool isRefresh;
    };
    const ConnectEntry entries[] = {
        { _imp->file.lock(), true, false },
        { _imp->reimport.lock(), false, true },
        { _imp->fitMode.lock(), false, false },
        { _imp->customScale.lock(), false, false },
        { _imp->customOffset.lock(), false, false },
        { _imp->invertY.lock(), false, false },
        { _imp->importColors.lock(), false, false },
        { _imp->groupByLayer.lock(), false, false },
        { _imp->featherSplines.lock(), false, false },
        { _imp->autoRebuild.lock(), false, false },
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        const KnobSignalSlotHandlerPtr handler = entries[i].knob ? entries[i].knob->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
        if (!handler) {
            continue;
        }
        if (entries[i].isFile) {
            QObject::connect( handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onFileKnobChanged(ViewSpec,int,int)) );
        } else if (entries[i].isRefresh) {
            QObject::connect( handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onRefreshClicked()) );
        } else {
            QObject::connect( handler.get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onControlKnobChanged(ViewSpec,int,int)) );
        }
    }
}

std::string
ReadMask::getPluginDescription() const
{
    return "Vector mask importer that is a Roto node, derived from vectorNuke.\n\n"
           "Parses a vector file (SVG in this version) into real Bézier shapes inside its "
           "own Roto tree and outputs the mask alpha like any Roto. Shapes are project "
           "native: editable in the viewer, serialized with the project.\n\n"
           "Placement reproduces the vectorNuke fit model (Fit to Format / Center to "
           "Format / Original / Custom scale + offset) with the top-left to bottom-left Y "
           "flip on by default. Fill/stroke colors and opacity import onto the shapes; "
           "layers become RotoLayer groups. With Feather Splines off the imported shapes "
           "have zero feather, like vectorNuke's clean single-line vectors.\n\n"
           "While file-driven, a re-import regenerates the shapes by design — bake "
           "(per-frame keyframing) is the persistence path.";
}

void
ReadMask::onFileKnobChanged(ViewSpec /*view*/, int /*dimension*/, int /*reason*/)
{
    const KnobBoolPtr autoRebuild = _imp->autoRebuild.lock();
    const KnobFilePtr file = _imp->file.lock();
    if (!autoRebuild || !file) {
        return;
    }
    const QString path = QString::fromUtf8(file->getValue().c_str());
    if (path.isEmpty()) {
        return;
    }
    if (!autoRebuild->getValue()) {
        return;
    }
    importFile(getNode()->getRotoContext() ? getNode()->getRotoContext()->getTimelineCurrentTime() : 0.0);
}

void
ReadMask::onControlKnobChanged(ViewSpec /*view*/, int /*dimension*/, int /*reason*/)
{
    const KnobBoolPtr autoRebuild = _imp->autoRebuild.lock();
    const KnobFilePtr file = _imp->file.lock();
    if (!autoRebuild || !file) {
        return;
    }
    const QString path = QString::fromUtf8(file->getValue().c_str());
    if (path.isEmpty()) {
        return;
    }
    if (!autoRebuild->getValue()) {
        return;
    }
    importFile(getNode()->getRotoContext() ? getNode()->getRotoContext()->getTimelineCurrentTime() : 0.0);
}

void
ReadMask::onRefreshClicked()
{
    const KnobFilePtr file = _imp->file.lock();
    if (!file) {
        return;
    }
    const QString path = QString::fromUtf8(file->getValue().c_str());
    if (path.isEmpty()) {
        return;
    }
    importFile(getNode()->getRotoContext() ? getNode()->getRotoContext()->getTimelineCurrentTime() : 0.0);
}

void
ReadMask::removeImportedItems()
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
ReadMask::importFile(double time)
{
    RotoContextPtr ctx = getNode()->getRotoContext();
    const KnobFilePtr fileKnob = _imp->file.lock();
    if (!ctx || !fileKnob) {
        return;
    }

    const QString path = QString::fromUtf8(fileKnob->getValue().c_str());
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("ReadMask"), QDateTime::currentDateTime(),
                                        QString::fromUtf8("File not found: %1").arg(path));
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("ReadMask"), QDateTime::currentDateTime(),
                                        QString::fromUtf8("Cannot open %1: %2").arg(path, f.errorString()));
        return;
    }
    const QByteArray raw = f.readAll();
    f.close();
    const QString content = QString::fromUtf8(raw.data(), (int)raw.size());

    VectorMask::VectorDocument doc;
    QString errorString;
    VectorMask::SvgParser parser;
    if (!parser.parseContent(content, &doc, &errorString)) {
        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("ReadMask"), QDateTime::currentDateTime(),
                                        QString::fromUtf8("Failed to parse %1").arg(path));
        return;
    }

    // The parse succeeded: only now discard the previous import.
    removeImportedItems();

    // Project format for the fit model.
    Format fmt;
    getApp()->getProject()->getProjectDefaultFormat(&fmt);
    const double formatW = fmt.x2 - fmt.x1;
    const double formatH = fmt.y2 - fmt.y1;

    const KnobChoicePtr fitKnob = _imp->fitMode.lock();
    const KnobDoublePtr scaleKnob = _imp->customScale.lock();
    const KnobDoublePtr offsetKnob = _imp->customOffset.lock();
    const KnobBoolPtr invertYKnob = _imp->invertY.lock();
    const KnobBoolPtr colorsKnob = _imp->importColors.lock();
    const KnobBoolPtr groupKnob = _imp->groupByLayer.lock();
    const KnobBoolPtr featherKnob = _imp->featherSplines.lock();
    const bool importColors = colorsKnob ? colorsKnob->getValue() : true;
    const bool groupByLayer = groupKnob ? groupKnob->getValue() : true;
    const bool featherSplines = featherKnob ? featherKnob->getValue() : false;
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

    // Layer grouping: main layer named after the file, sub-layers per SVG group.
    //
    // All tree mutations go through RotoContext::addItem: it links the parent,
    // registers the layer, and emits itemInserted — the RotoPanel keeps its tree
    // in sync via those signals. Mutating RotoLayer directly (addItem/insertItem)
    // is silent and leaves the RotoPanel stale (and, on rebuild, was the
    // use-after-free path into QTreeWidgetItem::insertChild).
    RotoLayerPtr baseLayer = ctx->getOrCreateBaseLayer();
    RotoLayerPtr mainLayer;
    std::map<std::string, RotoLayerPtr> subLayers;
    if (groupByLayer) {
        const std::string fileLabel =
            VectorMask::sanitizeName(info.completeBaseName().toStdString(), "Imported_Vector");
        mainLayer.reset(new RotoLayer(ctx, fileLabel, RotoLayerPtr()));
        if (baseLayer) {
            ctx->addItem(baseLayer, (int)baseLayer->getItems().size(), mainLayer, RotoItem::eSelectionReasonOther);
        } else {
            ctx->addItem(RotoLayerPtr(), 0, mainLayer, RotoItem::eSelectionReasonOther);
        }
        _imp->importedItems.push_back(mainLayer);
    }

    beginChanges();

    for (std::vector<VectorMask::VectorShape>::const_iterator it = doc.shapes.begin();
         it != doc.shapes.end(); ++it) {
        if (it->vertices.empty()) {
            continue;
        }
        const VectorMask::VectorShape shape =
            VectorMask::transformShape(*it, sx, sy, ox, oy, flipY, docHeight);

        // Destination layer
        RotoLayerPtr dest = baseLayer;
        if (groupByLayer && mainLayer) {
            const std::string safeLayer =
                VectorMask::sanitizeName(shape.layerName.empty() ? std::string("Layer") : shape.layerName, "Layer");
            if (shape.layerName != "Layer1") {
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
            } else {
                dest = mainLayer;
            }
        }
        if (!dest) {
            continue;
        }

        const std::string shapeName =
            ctx->generateUniqueName(VectorMask::sanitizeName(shape.name.empty() ? std::string("Shape") : shape.name, "Shape"));
        BezierPtr curve(new Bezier(ctx, shapeName, RotoLayerPtr(), !shape.closed));
        curve->createNodes();
        ctx->addItem(dest, 0, curve, RotoItem::eSelectionReasonOther);
        _imp->importedItems.push_back(curve);

        // Build-up: control points (feather points are added at the same positions).
        for (std::vector<VectorMask::ShapeVertex>::const_iterator v = shape.vertices.begin();
             v != shape.vertices.end(); ++v) {
            curve->addControlPoint(v->center.x, v->center.y, time);
        }
        // Tangents: vectorNuke stores offsets; Natron takes absolute handle positions
        // (Natron right bezier point = P1 = outgoing = vectorNuke right_tangent).
        for (size_t i = 0; i < shape.vertices.size(); ++i) {
            const VectorMask::ShapeVertex& v = shape.vertices[i];
            const double lx = v.center.x + v.leftTangent.x;
            const double ly = v.center.y + v.leftTangent.y;
            const double rx = v.center.x + v.rightTangent.x;
            const double ry = v.center.y + v.rightTangent.y;
            curve->setPointAtIndex(false, (int)i, time, v.center.x, v.center.y, lx, ly, rx, ry);
            // Keep the feather curve aligned with the shape curve.
            curve->setPointAtIndex(true, (int)i, time, v.center.x, v.center.y, lx, ly, rx, ry);
        }
        if (shape.closed) {
            curve->setCurveFinished(true);
        }

        // Color + opacity (fill first, then stroke; fold color alpha into opacity).
        if (importColors && (shape.hasFillColor || shape.hasStrokeColor)) {
            const VectorMask::Color& col = shape.hasFillColor ? shape.fillColor : shape.strokeColor;
            curve->setColor(time, col.r, col.g, col.b);
            curve->setOpacity(shape.opacity * col.a, time);
        }

        // vectorNuke feather zeroing: distance + fall-off to 0 (tangents already aligned).
        if (!featherSplines) {
            curve->setFeatherDistance(0.0, time);
            curve->setFeatherFallOff(0.0, time);
        }
    }

    endChanges(true);

    ctx->evaluateChange();
}

NATRON_NAMESPACE_EXIT

#include "moc_ReadMask.cpp"
