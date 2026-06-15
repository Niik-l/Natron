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

#include "ColorChartMatch.h"

#include <cassert>
#include <cmath>
#include <algorithm>
#include <sstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "LinearAlgebra.h"
#include "ChartData.h"
#include "../../../Global/GLIncludes.h"
#include "../../AppInstance.h"
#include "../../CreateNodeArgs.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

using namespace ColorChartMath;
using namespace ChartData;


struct ColorChartMatchPrivate
{
    // Chart settings
    KnobChoiceWPtr chartType;
    KnobBoolWPtr useReferenceValues;
    KnobChoiceWPtr colorspace;
    KnobBoolWPtr normalization;
    KnobChoiceWPtr samplingMethod;  // 0 = Direct (clamp >=0), 1 = No Clip (raw float)

    // Source corner-pin: "to" points (draggable, 2D each)
    KnobDoubleWPtr srcTo[4];  // BL, BR, TR, TL
    // Source corner-pin: "from" points (fixed reference, 2D each)
    KnobDoubleWPtr srcFrom[4];
    // Source enable per corner
    KnobBoolWPtr srcEnable[4];
    // Overlay points display mode
    KnobChoiceWPtr srcOverlayPoints;
    // Interactive mode
    KnobBoolWPtr srcInteractive;

    // Target corner-pin (mirror of source): independent corners on the TARGET image,
    // used when matching to a second chart plate (Use Reference Values off). The
    // target chart can be framed differently from the source, so it needs its own
    // corner-pin instead of reusing the source positions.
    KnobDoubleWPtr tgtTo[4];
    KnobDoubleWPtr tgtFrom[4];
    KnobBoolWPtr tgtEnable[4];
    KnobChoiceWPtr tgtOverlayPoints;
    KnobBoolWPtr tgtInteractive;

    // Sample size
    KnobDoubleWPtr sampleSize;

    // Per-patch enable and color (24 patches)
    KnobBoolWPtr patchEnable[24];
    KnobColorWPtr patchColor[24];  // 3-dimensional (RGB) with color swatch

    // Calculate button
    KnobButtonWPtr calculateButton;

    // Matrix values (3 rows of 3)
    KnobDoubleWPtr matrixR[3];  // row 0: Rr, Rg, Rb
    KnobDoubleWPtr matrixG[3];  // row 1: Gr, Gg, Gb
    KnobDoubleWPtr matrixB[3];  // row 2: Br, Bg, Bb

    // Apply matrix toggle
    KnobBoolWPtr applyMatrix;

    // Current View
    KnobChoiceWPtr currentView;  // 0=source, 1=corrected

    // Export button
    KnobButtonWPtr exportButton;

    // Info display
    KnobStringWPtr info;

    // Overlay drag state
    int draggingCorner;  // -1 = not dragging, 0-3 = dragging that corner
};


// ---- Constructor / Destructor ----

ColorChartMatch::ColorChartMatch(NodePtr node)
    : EffectInstance(node)
    , _imp(new ColorChartMatchPrivate())
{
    _imp->draggingCorner = -1;
    setSupportsRenderScaleMaybe(eSupportsYes);
}

ColorChartMatch::~ColorChartMatch()
{
}

std::string
ColorChartMatch::getPluginDescription() const
{
    return tr("Match image colors using a color reference chart (ColorChecker / Macbeth). "
              "Place corner-pin markers on the chart in the source image, then click "
              "'Calculate Matrix' to compute a best-fit 3x3 color correction matrix. "
              "The matrix is applied to the image in real-time. "
              "Supports ColorChecker 24, ColorChecker Passport Video, and SpyderCHECKR 24. "
              "Inspired by Marco Meyer's mmColorTarget.").toStdString();
}

std::string
ColorChartMatch::getInputLabel(int inputNb) const
{
    return (inputNb == 0) ? "Source" : "Target";
}

bool
ColorChartMatch::isInputOptional(int inputNb) const
{
    return (inputNb == 1);  // Target is optional
}

void
ColorChartMatch::addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
ColorChartMatch::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ColorChartMatch::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}


// ---- Helper: bilinear interpolation on a quad ----
// Given 4 corners (BL, BR, TR, TL) and normalized coords (u, v) in [0,1],
// compute the interpolated position.
static void bilinearInterp(double blx, double bly, double brx, double bry,
                           double trx, double try_, double tlx, double tly,
                           double u, double v,
                           double& ox, double& oy)
{
    double bx = blx + (brx - blx) * u;
    double by = bly + (bry - bly) * u;
    double tx = tlx + (trx - tlx) * u;
    double ty = tly + (try_ - tly) * u;
    ox = bx + (tx - bx) * v;
    oy = by + (ty - by) * v;
}


// ---- Knobs ----

void
ColorChartMatch::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("ColorChartMatch"));

    // Current View (top of the panel, like mmColorTarget)
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Current View"));
        k->setName("currentView");
        k->setHintToolTip(tr("Source: show input 0 unchanged (position the Source corner-pin).\n"
                              "Target: show input 1 unchanged (position the Target corner-pin) "
                              "— only meaningful when 'Use Reference Values' is off.\n"
                              "Corrected: show input 0 with the computed color matrix applied."));
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Source", "", "Show source unchanged"));
        entries.push_back(ChoiceOption("Target", "", "Show target unchanged"));
        entries.push_back(ChoiceOption("Corrected", "", "Apply computed color matrix"));
        k->populateChoices(entries);
        k->setDefaultValue(0);  // Source by default
        k->setAnimationEnabled(false);
        mainPage->addKnob(k);
        _imp->currentView = k;
    }

    // Sample size
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Sample Size"));
        k->setName("sampleSize");
        k->setHintToolTip(tr("Size of the sampling area per patch, as a fraction of patch spacing (0.1-1.0). "
                              "Smaller values sample just the center; larger values average more of each patch."));
        k->setDefaultValue(0.5);
        k->setMinimum(0.1);
        k->setMaximum(1.0);
        k->setDisplayMinimum(0.1);
        k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(false);
        mainPage->addKnob(k);
        _imp->sampleSize = k;
    }

    // ---- Source Corner Pin (with draggable overlay) ----
    {
        // "from" points: fixed reference rectangle (chart outline in ideal space)
        const char* fromNames[] = {"srcFrom1", "srcFrom2", "srcFrom3", "srcFrom4"};
        const char* fromLabels[] = {"From BL", "From BR", "From TR", "From TL"};
        double fromDefaults[][2] = {{0, 0}, {720, 0}, {720, 480}, {0, 480}};
        for (int i = 0; i < 4; ++i) {
            KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(fromLabels[i]), 2);
            k->setName(fromNames[i]);
            k->setDefaultValue(fromDefaults[i][0], 0);
            k->setDefaultValue(fromDefaults[i][1], 1);
            k->setAnimationEnabled(false);
            k->setSecret(true);  // Hidden — these are reference points
            mainPage->addKnob(k);
            _imp->srcFrom[i] = k;
        }

        // "to" points: user-draggable corners, laid out two per row (TL TR / BL BR)
        // like mmColorTarget. The srcTo[] index + script-name keep their original
        // semantic corner (srcTo[3]=TL, [2]=TR, [0]=BL, [1]=BR) so saved projects and
        // the overlay/solve still map correctly — only the label + visual order change.
        struct SrcToCorner { int idx; const char* name; const char* label; double x, y; bool sameRow; };
        const SrcToCorner srcToCorners[] = {
            { 3, "srcTo4", "srcTL", 600,  780, true  },  // row 1:  srcTL | srcTR
            { 2, "srcTo3", "srcTR", 1320, 780, false },
            { 0, "srcTo1", "srcBL", 600,  300, true  },  // row 2:  srcBL | srcBR
            { 1, "srcTo2", "srcBR", 1320, 300, false },
        };
        for (int j = 0; j < 4; ++j) {
            const SrcToCorner& c = srcToCorners[j];
            KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(c.label), 2);
            k->setName(c.name);
            k->setDefaultValue(c.x, 0);
            k->setDefaultValue(c.y, 1);
            k->setAnimationEnabled(true);
            if (c.sameRow) k->setAddNewLine(false);
            mainPage->addKnob(k);
            _imp->srcTo[c.idx] = k;
        }

        // Enable per corner
        for (int i = 0; i < 4; ++i) {
            std::string name = "srcEnable" + std::to_string(i + 1);
            KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Enable"));
            k->setName(name.c_str());
            k->setDefaultValue(true);
            k->setSecret(true);  // Hidden — always enabled
            mainPage->addKnob(k);
            _imp->srcEnable[i] = k;
        }

        // Overlay points display mode
        {
            KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Overlay Points"));
            k->setName("srcOverlayPoints");
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("To", "", "Show 'to' points"));
            entries.push_back(ChoiceOption("From", "", "Show 'from' points"));
            k->populateChoices(entries);
            k->setDefaultValue(0);
            k->setSecret(true);
            mainPage->addKnob(k);
            _imp->srcOverlayPoints = k;
        }

        // Interactive mode
        {
            KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Interactive"));
            k->setName("srcInteractive");
            k->setDefaultValue(true);
            k->setSecret(true);
            mainPage->addKnob(k);
            _imp->srcInteractive = k;
        }

        // Register the corner-pin overlay for draggable handles!
        getNode()->addCornerPinInteract(
            _imp->srcFrom[0].lock(), _imp->srcFrom[1].lock(),
            _imp->srcFrom[2].lock(), _imp->srcFrom[3].lock(),
            _imp->srcTo[0].lock(), _imp->srcTo[1].lock(),
            _imp->srcTo[2].lock(), _imp->srcTo[3].lock(),
            _imp->srcEnable[0].lock(), _imp->srcEnable[1].lock(),
            _imp->srcEnable[2].lock(), _imp->srcEnable[3].lock(),
            _imp->srcOverlayPoints.lock(),
            KnobBoolPtr(),  // no invert
            _imp->srcInteractive.lock()
        );
    }

    // ---- Target Corner Pin (mirror of source) ----
    {
        // Target corner-pin is hidden from the panel (the source settings above cover
        // the common case). It stays functional for matching to a SECOND chart plate
        // (Use Reference Values off): the corners sample input 1 and are positioned by
        // dragging the viewer overlay with Current View = "Target".
        const char* fromNames[] = {"tgtFrom1", "tgtFrom2", "tgtFrom3", "tgtFrom4"};
        const char* fromLabels[] = {"T From BL", "T From BR", "T From TR", "T From TL"};
        double fromDefaults[][2] = {{0, 0}, {720, 0}, {720, 480}, {0, 480}};
        for (int i = 0; i < 4; ++i) {
            KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(fromLabels[i]), 2);
            k->setName(fromNames[i]);
            k->setDefaultValue(fromDefaults[i][0], 0);
            k->setDefaultValue(fromDefaults[i][1], 1);
            k->setAnimationEnabled(false);
            k->setSecret(true);
            mainPage->addKnob(k);
            _imp->tgtFrom[i] = k;
        }

        const char* toNames[] = {"tgtTo1", "tgtTo2", "tgtTo3", "tgtTo4"};
        const char* toLabels[] = {"T To BL", "T To BR", "T To TR", "T To TL"};
        double toDefaults[][2] = {{600, 300}, {1320, 300}, {1320, 780}, {600, 780}};
        for (int i = 0; i < 4; ++i) {
            KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(toLabels[i]), 2);
            k->setName(toNames[i]);
            k->setDefaultValue(toDefaults[i][0], 0);
            k->setDefaultValue(toDefaults[i][1], 1);
            k->setAnimationEnabled(true);
            k->setSecret(true);  // hidden from panel; positioned via the viewer overlay
            mainPage->addKnob(k);
            _imp->tgtTo[i] = k;
        }

        for (int i = 0; i < 4; ++i) {
            std::string name = "tgtEnable" + std::to_string(i + 1);
            KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Enable"));
            k->setName(name.c_str());
            k->setDefaultValue(true);
            k->setSecret(true);
            mainPage->addKnob(k);
            _imp->tgtEnable[i] = k;
        }

        {
            KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Overlay Points"));
            k->setName("tgtOverlayPoints");
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("To", "", "Show 'to' points"));
            entries.push_back(ChoiceOption("From", "", "Show 'from' points"));
            k->populateChoices(entries);
            k->setDefaultValue(0);
            k->setSecret(true);
            mainPage->addKnob(k);
            _imp->tgtOverlayPoints = k;
        }

        {
            KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Interactive"));
            k->setName("tgtInteractive");
            // Off by default — the target corner-pin only becomes interactive when the
            // user switches Current View to "Target" (handled in knobChanged).
            k->setDefaultValue(false);
            k->setSecret(true);
            mainPage->addKnob(k);
            _imp->tgtInteractive = k;
        }

        getNode()->addCornerPinInteract(
            _imp->tgtFrom[0].lock(), _imp->tgtFrom[1].lock(),
            _imp->tgtFrom[2].lock(), _imp->tgtFrom[3].lock(),
            _imp->tgtTo[0].lock(), _imp->tgtTo[1].lock(),
            _imp->tgtTo[2].lock(), _imp->tgtTo[3].lock(),
            _imp->tgtEnable[0].lock(), _imp->tgtEnable[1].lock(),
            _imp->tgtEnable[2].lock(), _imp->tgtEnable[3].lock(),
            _imp->tgtOverlayPoints.lock(),
            KnobBoolPtr(),  // no invert
            _imp->tgtInteractive.lock()
        );
    }

    // Chart type
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Chart"));
        k->setName("chartType");
        k->setHintToolTip(tr("Type of color reference chart being used."));
        std::vector<ChoiceOption> entries;
        for (int i = 0; i < eChartCount; ++i) {
            entries.push_back(ChoiceOption(kChartLabels[i], "", ""));
        }
        k->populateChoices(entries);
        k->setDefaultValue(eChartColorChecker24_Post2014);
        k->setAnimationEnabled(false);
        mainPage->addKnob(k);
        _imp->chartType = k;
    }

    // Use reference values
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Use Reference Values as Target"));
        k->setName("useReferenceValues");
        k->setHintToolTip(tr("Use built-in reference sRGB values instead of sampling a target image. "
                              "This effectively calibrates the source to a known standard."));
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        mainPage->addKnob(k);
        _imp->useReferenceValues = k;
    }

    // Colorspace
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Colorspace"));
        k->setName("colorspace");
        k->setHintToolTip(tr("Working colorspace for reference values. "
                              "Reference patch values will be converted from sRGB to this colorspace."));
        std::vector<ChoiceOption> entries;
        for (int i = 0; i < eColorspaceCount; ++i) {
            entries.push_back(ChoiceOption(kColorspaceLabels[i], "", ""));
        }
        k->populateChoices(entries);
        k->setDefaultValue(eColorspaceACEScg);
        k->setAnimationEnabled(false);
        mainPage->addKnob(k);
        _imp->colorspace = k;
    }

    // Sampling Method (mmColorTarget parity) — shares its row with Normalize.
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Sampling Method"));
        k->setName("samplingMethod");
        k->setHintToolTip(tr("How patch samples are conditioned before solving the matrix.\n"
                              "Direct: clamp each sampled value to >= 0 (safe default).\n"
                              "No Clip: keep the full unclamped float (better for HDR / over-range plates)."));
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Direct", "", "Clamp samples to >= 0 before solving"));
        entries.push_back(ChoiceOption("No Clip", "", "Keep full unclamped float samples"));
        k->populateChoices(entries);
        k->setDefaultValue(0);  // Direct
        k->setAnimationEnabled(false);
        k->setAddNewLine(false);  // Normalize sits to the right (mmColorTarget layout)
        mainPage->addKnob(k);
        _imp->samplingMethod = k;
    }

    // Normalize (matches mmColorTarget's "normalize"): pre-scale the source samples
    // by a single Rec.709 luminance factor so the source mid-grey patch's luminance
    // matches the target's, before solving the matrix. Net effect: the chroma is
    // matched to the target while the output keeps the SOURCE luminance/exposure.
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Normalize"));
        k->setName("normalization");
        k->setHintToolTip(tr("Will try to maintain the source luminance: scales the "
                              "source by the Rec.709 luminance ratio of the mid-grey "
                              "patch (target/source) before solving, so the colour "
                              "match doesn't also change your plate's exposure. "
                              "Requires the mid-grey patch to be enabled."));
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        mainPage->addKnob(k);
        _imp->normalization = k;
    }

    // ---- Calculate Button ----
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        mainPage->addKnob(sep);

        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Calculate Matrix"));
        k->setName("calculateMatrix");
        k->setHintToolTip(tr("Sample all enabled patches and compute the best-fit 3x3 color matrix."));
        mainPage->addKnob(k);
        _imp->calculateButton = k;
    }

    // ---- Matrix Display (collapsed group) ----
    {
        KnobGroupPtr grp = AppManager::createKnob<KnobGroup>(this, tr("Color Matrix"));
        grp->setName("matrixGroup");
        grp->setDefaultValue(false);  // collapsed by default
        grp->setAsTab();  // renders as collapsible group
        mainPage->addKnob(grp);

        auto makeRow = [&](const char* name, const char* label, int row,
                           double d0, double d1, double d2,
                           KnobDoubleWPtr out[3]) {
            for (int c = 0; c < 3; ++c) {
                const char* suffix[] = {"r", "g", "b"};
                std::string fullName = std::string(name) + suffix[c];
                std::string fullLabel = std::string(label) + std::string(1, "RGB"[c]);
                KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, QString::fromUtf8(fullLabel.c_str()));
                k->setName(fullName.c_str());
                double def = (c == 0) ? d0 : (c == 1) ? d1 : d2;
                k->setDefaultValue(def);
                k->setAnimationEnabled(true);
                if (c < 2) k->setAddNewLine(false);  // 3 columns per row -> 3x3 grid
                grp->addKnob(k);
                out[c] = k;
            }
        };

        makeRow("matrix_r", "R←", 0, 1.0, 0.0, 0.0, _imp->matrixR);
        makeRow("matrix_g", "G←", 1, 0.0, 1.0, 0.0, _imp->matrixG);
        makeRow("matrix_b", "B←", 2, 0.0, 0.0, 1.0, _imp->matrixB);
    }

    // Apply toggle (hidden — controlled by Current View now)
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Apply Matrix"));
        k->setName("applyMatrix");
        k->setHintToolTip(tr("Apply the color matrix to the output."));
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->applyMatrix = k;
    }

    // Export button
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Export to ColorMatrix Node"));
        k->setName("exportMatrix");
        k->setHintToolTip(tr("Create a ColorMatrix node in the graph with the computed matrix values. "
                              "The matrix is also stored in this node's parameters."));
        mainPage->addKnob(k);
        _imp->exportButton = k;
    }

    // Info
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info");
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setIsPersistent(false);
        k->setDefaultValue("Place corner-pin on chart, then click Calculate Matrix.");
        mainPage->addKnob(k);
        _imp->info = k;
    }

    // ---- Patches Page ----
    KnobPagePtr patchPage = AppManager::createKnob<KnobPage>(this, tr("Patches"));

    {
        KnobStringPtr hdr = AppManager::createKnob<KnobString>(this, tr(""));
        hdr->setName("patchesHeader");
        hdr->setAnimationEnabled(false);
        hdr->setEvaluateOnChange(false);
        hdr->setIsPersistent(false);
        hdr->setDefaultValue("Uncheck patches to exclude them from the calculation.");
        patchPage->addKnob(hdr);
    }

    const double (*defaultRef)[3] = getChartData(eChartColorChecker24_Post2014);

    for (int i = 0; i < 24; ++i) {
        // Add separator between chart rows (every 6 patches)
        if (i % 6 == 0) {
            KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
            std::string sepName = "patchRow_" + std::to_string(i / 6);
            sep->setName(sepName.c_str());
            patchPage->addKnob(sep);
        }

        // Enable checkbox
        std::string enableName = "patchEnable_" + std::to_string(i);
        KnobBoolPtr enableK = AppManager::createKnob<KnobBool>(this, tr(""));
        enableK->setName(enableName.c_str());
        enableK->setDefaultValue(true);
        enableK->setAnimationEnabled(false);
        enableK->setAddNewLine(false);
        patchPage->addKnob(enableK);
        _imp->patchEnable[i] = enableK;

        // RGB color value — no label, just values + swatch (Nuke style)
        std::string colorName = "patchColor_" + std::to_string(i);
        std::string tooltip = std::string(kColorChecker24Names[i]) + " (" + std::to_string(i + 1) + ")";
        KnobColorPtr colorK = AppManager::createKnob<KnobColor>(this, tr(""), 3);
        colorK->setName(colorName.c_str());
        colorK->setHintToolTip(tr(tooltip.c_str()));
        // The chart reference data is stored as linear sRGB; convert it to the
        // DEFAULT working colorspace (matches the colorspace knob's default, ACEScg)
        // so the target patches are in the same space as the (colour-managed) source
        // out of the box. Previously this conversion only ran in knobChanged, so a
        // freshly-created node left the reference in sRGB while the source was ACEScg
        // — a gamut mismatch that skewed the matrix (worst in red). knobChanged still
        // re-converts when the chart / colorspace knob is changed.
        double dcr, dcg, dcb;
        convertSRGBtoColorspace(defaultRef[i][0], defaultRef[i][1], defaultRef[i][2],
                                eColorspaceACEScg, dcr, dcg, dcb);
        colorK->setDefaultValue(dcr, 0);
        colorK->setDefaultValue(dcg, 1);
        colorK->setDefaultValue(dcb, 2);
        colorK->setAnimationEnabled(false);
        patchPage->addKnob(colorK);
        _imp->patchColor[i] = colorK;
    }
}


// ---- knobChanged ----

bool
ColorChartMatch::knobChanged(KnobI* k,
                              ValueChangedReasonEnum /*reason*/,
                              ViewSpec /*view*/,
                              double /*time*/,
                              bool /*originatedFromMainThread*/)
{
    // Calculate button
    KnobButtonPtr calcBtn = _imp->calculateButton.lock();
    if (calcBtn.get() == k) {
        calculateMatrix();
        // Auto-switch to Corrected view (index 2) after computing
        _imp->currentView.lock()->setValue(2);
        _imp->applyMatrix.lock()->setValue(true);
        return true;
    }

    // Current View changed (0 = Source, 1 = Target, 2 = Corrected). Toggle which
    // corner-pin is interactive so only the visible chart's handles are draggable,
    // and apply the matrix only in Corrected view.
    KnobChoicePtr viewK = _imp->currentView.lock();
    if (viewK.get() == k) {
        int view = viewK->getValue();
        _imp->applyMatrix.lock()->setValue(view == 2);
        if (KnobBoolPtr si = _imp->srcInteractive.lock()) si->setValue(view != 1);
        if (KnobBoolPtr ti = _imp->tgtInteractive.lock()) ti->setValue(view == 1);
        return true;
    }

    // Export button — create a ColorMatrix node in the graph with the computed matrix
    KnobButtonPtr exportBtn = _imp->exportButton.lock();
    if (exportBtn.get() == k) {
        double m[3][3];
        for (int c = 0; c < 3; ++c) {
            m[0][c] = _imp->matrixR[c].lock()->getValue();
            m[1][c] = _imp->matrixG[c].lock()->getValue();
            m[2][c] = _imp->matrixB[c].lock()->getValue();
        }

        // Check that matrix is not identity/empty (at least one non-diagonal element nonzero or diagonal != 1)
        bool isIdentity = true;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                double expected = (r == c) ? 1.0 : 0.0;
                if (std::abs(m[r][c] - expected) > 1e-9) {
                    isIdentity = false;
                    break;
                }
            }
            if (!isIdentity) break;
        }
        if (isIdentity) {
            _imp->info.lock()->setValue("Matrix is identity — calculate first before exporting.");
            return true;
        }

        NodePtr thisNode = getNode();
        if (!thisNode || !thisNode->getApp()) {
            _imp->info.lock()->setValue("Error: cannot access application to create node.");
            return true;
        }

        try {
            CreateNodeArgs args(PLUGINID_NATRON_COLORMATRIX,
                                thisNode->getGroup());
            args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
            args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);

            NodePtr cmNode = thisNode->getApp()->createNode(args);
            if (!cmNode) {
                _imp->info.lock()->setValue("Error: failed to create ColorMatrix node.");
                return true;
            }

            // Set matrix values on our built-in ColorMatrix knobs
            const char* rowNames[3] = {"outputRed", "outputGreen", "outputBlue"};
            for (int r = 0; r < 3; ++r) {
                KnobIPtr knob = cmNode->getKnobByName(rowNames[r]);
                if (!knob) continue;
                KnobColorPtr colorK = std::dynamic_pointer_cast<KnobColor>(knob);
                if (!colorK) continue;
                colorK->setValue(m[r][0], ViewSpec::all(), 0);
                colorK->setValue(m[r][1], ViewSpec::all(), 1);
                colorK->setValue(m[r][2], ViewSpec::all(), 2);
            }

            // Connect ColorMatrix input to whatever is connected to our input
            NodePtr inputNode = thisNode->getInput(0);
            if (inputNode) {
                cmNode->connectInput(inputNode, 0);
            }

            std::ostringstream oss;
            oss << "Created ColorMatrix node: " << cmNode->getScriptName() << "\n"
                << "R: " << m[0][0] << "  " << m[0][1] << "  " << m[0][2] << "\n"
                << "G: " << m[1][0] << "  " << m[1][1] << "  " << m[1][2] << "\n"
                << "B: " << m[2][0] << "  " << m[2][1] << "  " << m[2][2];
            _imp->info.lock()->setValue(oss.str());
        } catch (const std::exception& e) {
            std::string msg = "Error creating ColorMatrix node: ";
            msg += e.what();
            _imp->info.lock()->setValue(msg);
        }
        return true;
    }

    // Chart type or colorspace changed — update patch reference colors
    KnobChoicePtr chartK = _imp->chartType.lock();
    KnobChoicePtr csK = _imp->colorspace.lock();
    if (chartK.get() == k || csK.get() == k) {
        ChartType chart = (ChartType)chartK->getValue();
        Colorspace cs = (Colorspace)csK->getValue();
        const double (*refData)[3] = getChartData(chart);
        for (int i = 0; i < 24; ++i) {
            double r, g, b;
            convertSRGBtoColorspace(refData[i][0], refData[i][1], refData[i][2],
                                    cs, r, g, b);
            KnobColorPtr colorK = _imp->patchColor[i].lock();
            colorK->setValue(r, ViewSpec::all(), 0);
            colorK->setValue(g, ViewSpec::all(), 1);
            colorK->setValue(b, ViewSpec::all(), 2);
        }
        return true;
    }

    return false;
}


// ---- Calculate Matrix ----

void
ColorChartMatch::calculateMatrix()
{
    // Get source image at current time
    double currentTime = getCurrentTime();
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, currentTime, RenderScale(), ViewIdx(0),
                               NULL, NULL, false, true,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) {
        _imp->info.lock()->setValue("Error: Could not fetch source image. Make sure an image is connected and the viewer is active.");
        return;
    }

    bool useRef = _imp->useReferenceValues.lock()->getValue();

    // Get target image if not using reference values
    ImagePtr tgtImg;
    if (!useRef) {
        RectI tgtRoi;
        tgtImg = getImage(1, currentTime, RenderScale(), ViewIdx(0),
                          NULL, NULL, false, true,
                          eStorageModeRAM, 0, &tgtRoi);
        if (!tgtImg) {
            _imp->info.lock()->setValue("Error: No target image connected. Either connect a target or enable 'Use Reference Values'.");
            return;
        }
    }

    // Source corner-pin: read "to" points (the user-draggable corners)
    // Order: BL(0), BR(1), TR(2), TL(3)
    double srcBLx = _imp->srcTo[0].lock()->getValue(0), srcBLy = _imp->srcTo[0].lock()->getValue(1);
    double srcBRx = _imp->srcTo[1].lock()->getValue(0), srcBRy = _imp->srcTo[1].lock()->getValue(1);
    double srcTRx = _imp->srcTo[2].lock()->getValue(0), srcTRy = _imp->srcTo[2].lock()->getValue(1);
    double srcTLx = _imp->srcTo[3].lock()->getValue(0), srcTLy = _imp->srcTo[3].lock()->getValue(1);

    // Target corner-pin: independent corners on the target image (only used when
    // sampling a target chart, i.e. Use Reference Values is off).
    double tgtBLx = _imp->tgtTo[0].lock()->getValue(0), tgtBLy = _imp->tgtTo[0].lock()->getValue(1);
    double tgtBRx = _imp->tgtTo[1].lock()->getValue(0), tgtBRy = _imp->tgtTo[1].lock()->getValue(1);
    double tgtTRx = _imp->tgtTo[2].lock()->getValue(0), tgtTRy = _imp->tgtTo[2].lock()->getValue(1);
    double tgtTLx = _imp->tgtTo[3].lock()->getValue(0), tgtTLy = _imp->tgtTo[3].lock()->getValue(1);

    double sampleSz = _imp->sampleSize.lock()->getValue();

    // Normalize (mmColorTarget parity): mid-grey patch is index 21 on the standard
    // 24-patch charts, 15 on the ColorChecker Passport Video. We track where that
    // patch lands in the enabled list, then scale all source samples by the Rec.709
    // luminance ratio (target/source) of that patch before solving.
    bool normalize = _imp->normalization.lock()->getValue();
    // Sampling Method: Direct (0) clamps each sampled patch value to >= 0 before the
    // solve (safe default); No Clip (1) keeps the full unclamped float (HDR/over-range).
    bool clampSamples = (_imp->samplingMethod.lock()->getValue() == 0);
    int chart = _imp->chartType.lock()->getValue();
    const int midGreyPatch = (chart == eChartColorCheckerPassportVideo) ? 15 : 21;
    int midGreyEnabled = -1;

    // Sample patches — 4 rows x 6 cols
    int enabledCount = 0;
    double enabledSrc[24][3], enabledTgt[24][3];

    RectI srcBounds = srcImg->getBounds();
    Image::ReadAccess srcRa(srcImg.get());
    int srcNComp = srcImg->getComponents().getNumComponents();

    // Target image access (if sampling from target)
    Image::ReadAccess* tgtRa = nullptr;
    RectI tgtBounds;
    int tgtNComp = 0;
    if (tgtImg) {
        tgtRa = new Image::ReadAccess(tgtImg.get());
        tgtBounds = tgtImg->getBounds();
        tgtNComp = tgtImg->getComponents().getNumComponents();
    }

    // Helper: sample a rectangular region from an image
    auto sampleRegion = [](Image::ReadAccess& ra, RectI& bounds, int nComp,
                           double cx, double cy, int halfW, int halfH,
                           double& outR, double& outG, double& outB) -> bool {
        double sumR = 0, sumG = 0, sumB = 0;
        int count = 0;
        for (int dy = -halfH; dy <= halfH; ++dy) {
            for (int dx = -halfW; dx <= halfW; ++dx) {
                int px = (int)(cx + dx);
                int py = (int)(cy + dy);
                if (px >= bounds.x1 && px < bounds.x2 &&
                    py >= bounds.y1 && py < bounds.y2) {
                    const float* pix = (const float*)ra.pixelAt(px, py);
                    if (pix) {
                        sumR += pix[0];
                        sumG += (nComp >= 2) ? pix[1] : pix[0];
                        sumB += (nComp >= 3) ? pix[2] : pix[0];
                        count++;
                    }
                }
            }
        }
        if (count == 0) return false;
        outR = sumR / count;
        outG = sumG / count;
        outB = sumB / count;
        return true;
    };

    for (int i = 0; i < 24; ++i) {
        if (!_imp->patchEnable[i].lock()->getValue()) {
            // mmColorTarget: if normalize is on but the mid-grey patch is disabled,
            // it can't be done — warn and turn it off (matches the gizmo).
            if (normalize && i == midGreyPatch) {
                normalize = false;
                _imp->normalization.lock()->setValue(false);
                _imp->info.lock()->setValue("Normalize needs the mid-grey patch "
                                            "(enabled) — it was disabled, so Normalize "
                                            "was turned off.");
            }
            continue;
        }

        if (i == midGreyPatch) midGreyEnabled = enabledCount;

        int row = i / 6;
        int col = i % 6;
        double u = (col + 0.5) / 6.0;
        double v = 1.0 - (row + 0.5) / 4.0;  // row 0 at top, row 3 (neutrals) at bottom

        // Source patch pixel position via corner-pin
        double sx, sy;
        bilinearInterp(srcBLx, srcBLy, srcBRx, srcBRy,
                       srcTRx, srcTRy, srcTLx, srcTLy,
                       u, v, sx, sy);

        // Sampling region size
        double patchSpacingX = std::abs(srcBRx - srcBLx) / 6.0;
        double patchSpacingY = std::abs(srcTLy - srcBLy) / 4.0;
        int halfW = (int)(patchSpacingX * sampleSz * 0.5);
        int halfH = (int)(patchSpacingY * sampleSz * 0.5);
        if (halfW < 1) halfW = 1;
        if (halfH < 1) halfH = 1;

        // Sample source
        double sR, sG, sB;
        if (!sampleRegion(srcRa, srcBounds, srcNComp, sx, sy, halfW, halfH, sR, sG, sB)) {
            continue;
        }

        if (clampSamples) { sR = std::max(0.0, sR); sG = std::max(0.0, sG); sB = std::max(0.0, sB); }
        enabledSrc[enabledCount][0] = sR;
        enabledSrc[enabledCount][1] = sG;
        enabledSrc[enabledCount][2] = sB;

        // Target values
        if (useRef) {
            // Use editable patch color values (reference, possibly colorspace-converted)
            KnobColorPtr colorK = _imp->patchColor[i].lock();
            enabledTgt[enabledCount][0] = colorK->getValue(0);
            enabledTgt[enabledCount][1] = colorK->getValue(1);
            enabledTgt[enabledCount][2] = colorK->getValue(2);
        } else {
            // Sample from the target image at the TARGET corner-pin position — the
            // target chart can be framed differently from the source, so it has its
            // own corner-pin (not the source's).
            double tx, ty;
            bilinearInterp(tgtBLx, tgtBLy, tgtBRx, tgtBRy,
                           tgtTRx, tgtTRy, tgtTLx, tgtTLy,
                           u, v, tx, ty);
            double tHalfW = (int)(std::abs(tgtBRx - tgtBLx) / 6.0 * sampleSz * 0.5);
            double tHalfH = (int)(std::abs(tgtTLy - tgtBLy) / 4.0 * sampleSz * 0.5);
            if (tHalfW < 1) tHalfW = 1;
            if (tHalfH < 1) tHalfH = 1;
            double tR, tG, tB;
            if (!sampleRegion(*tgtRa, tgtBounds, tgtNComp, tx, ty,
                              (int)tHalfW, (int)tHalfH, tR, tG, tB)) {
                continue;
            }
            if (clampSamples) { tR = std::max(0.0, tR); tG = std::max(0.0, tG); tB = std::max(0.0, tB); }
            enabledTgt[enabledCount][0] = tR;
            enabledTgt[enabledCount][1] = tG;
            enabledTgt[enabledCount][2] = tB;
        }

        enabledCount++;
    }

    delete tgtRa;

    if (enabledCount < 3) {
        _imp->info.lock()->setValue("Error: Need at least 3 enabled patches with valid samples.");
        return;
    }

    // Normalize (mmColorTarget parity): scale every source sample by the Rec.709
    // luminance ratio of the mid-grey patch (target / source) before the solve, so
    // the resulting matrix matches chroma without changing the source's exposure.
    if (normalize && midGreyEnabled >= 0) {
        const double* sg = enabledSrc[midGreyEnabled];
        const double* tg = enabledTgt[midGreyEnabled];
        double srcLum = sg[0] * 0.2126 + sg[1] * 0.7152 + sg[2] * 0.0722;
        double dstLum = tg[0] * 0.2126 + tg[1] * 0.7152 + tg[2] * 0.0722;
        if (srcLum != 0.0) {
            double lumMult = dstLum / srcLum;
            for (int k = 0; k < enabledCount; ++k) {
                enabledSrc[k][0] *= lumMult;
                enabledSrc[k][1] *= lumMult;
                enabledSrc[k][2] *= lumMult;
            }
        }
    }

    // Compute the matrix
    Mat3 matrix;
    if (!computeColorMatrix(enabledSrc, enabledTgt, enabledCount, matrix)) {
        _imp->info.lock()->setValue("Error: Matrix computation failed (singular system).");
        return;
    }

    // Write to knobs
    for (int c = 0; c < 3; ++c) {
        _imp->matrixR[c].lock()->setValue(matrix.m[0][c]);
        _imp->matrixG[c].lock()->setValue(matrix.m[1][c]);
        _imp->matrixB[c].lock()->setValue(matrix.m[2][c]);
    }

    std::ostringstream oss;
    oss << "Matrix computed from " << enabledCount << " patches."
        << " First patch sampled: R=" << enabledSrc[0][0]
        << " G=" << enabledSrc[0][1]
        << " B=" << enabledSrc[0][2];
    _imp->info.lock()->setValue(oss.str());
}


// ---- Region of Definition ----

StatusEnum
ColorChartMatch::getRegionOfDefinition(U64 /*hash*/,
                                        double time,
                                        const RenderScale& scale,
                                        ViewIdx view,
                                        RectD* rod)
{
    // Match the viewed input (Target view shows input 1, else input 0).
    int viewMode = _imp->currentView.lock()->getValue();
    EffectInstancePtr input = (viewMode == 1 && getInput(1)) ? getInput(1) : getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}


// ---- Render: apply 3x3 matrix to every pixel ----

StatusEnum
ColorChartMatch::render(const RenderActionArgs& args)
{
    // Current View: 0 = Source (input 0), 1 = Target (input 1), 2 = Corrected.
    // In Target view we show input 1 unchanged so the target corner-pin can be
    // positioned on it; otherwise we show input 0 (with or without the matrix).
    // Don't rely on getInput(1) here (unreliable on render-clone threads) — just
    // try to fetch the target image and fall back to the source if it isn't there.
    int view = _imp->currentView.lock()->getValue();

    RectI srcRoi;
    ImagePtr srcImg;
    bool showingTarget = false;
    if (view == 1) {
        srcImg = getImage(1, args.time, args.mappedScale, args.view,
                          NULL, NULL, false, false,
                          eStorageModeRAM, 0, &srcRoi);
        showingTarget = (srcImg != NULL);
    }
    if (!srcImg) {
        srcImg = getImage(0, args.time, args.mappedScale, args.view,
                          NULL, NULL, false, false,
                          eStorageModeRAM, 0, &srcRoi);
    }
    if (!srcImg) return eStatusFailed;

    // Get output image
    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    // Only apply the matrix in Corrected view (never when previewing the target).
    bool apply = (view == 2) && !showingTarget;

    // Read matrix
    double M[3][3];
    for (int c = 0; c < 3; ++c) {
        M[0][c] = _imp->matrixR[c].lock()->getValue();
        M[1][c] = _imp->matrixG[c].lock()->getValue();
        M[2][c] = _imp->matrixB[c].lock()->getValue();
    }

    RectI srcBounds = srcImg->getBounds();
    RectI outBounds = outImg->getBounds();
    int srcNComp = srcImg->getComponents().getNumComponents();
    int nComp = std::min(srcNComp, 4);

    Image::ReadAccess srcRa(srcImg.get());
    Image::WriteAccess wa(outImg.get());

    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;

            const float* src = nullptr;
            if (x >= srcBounds.x1 && x < srcBounds.x2 &&
                y >= srcBounds.y1 && y < srcBounds.y2) {
                src = (const float*)srcRa.pixelAt(x, y);
            }

            if (!src) {
                for (int c = 0; c < nComp; ++c) dst[c] = 0.0f;
                continue;
            }

            if (apply && nComp >= 3) {
                float r = src[0], g = src[1], b = src[2];
                dst[0] = (float)(M[0][0] * r + M[0][1] * g + M[0][2] * b);
                dst[1] = (float)(M[1][0] * r + M[1][1] * g + M[1][2] * b);
                dst[2] = (float)(M[2][0] * r + M[2][1] * g + M[2][2] * b);
                if (nComp >= 4) dst[3] = src[3];  // Pass alpha through
            } else {
                for (int c = 0; c < nComp; ++c) dst[c] = src[c];
            }
        }
    }

    return eStatusOK;
}


// ---- Overlay: draw corner-pin quad and patch grid ----

void
ColorChartMatch::drawOverlay(double /*time*/,
                              const RenderScale& /*renderScale*/,
                              ViewIdx /*view*/)
{
    // In Target view, overlay the TARGET corner-pin (on the target image); otherwise
    // the source corner-pin. This way the patch grid lands on whichever chart the
    // viewer is showing.
    const bool targetView = (_imp->currentView.lock()->getValue() == 1);
    KnobDoubleWPtr* corners = targetView ? _imp->tgtTo : _imp->srcTo;

    // Read corner positions
    double to[4][2];
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr k = corners[i].lock();
        if (!k) return;
        to[i][0] = k->getValue(0);
        to[i][1] = k->getValue(1);
    }

    GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT |
                      GL_COLOR_BUFFER_BIT | GL_POINT_BIT | GL_CURRENT_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);

    // Draw the corner-pin quad outline (yellow for source, cyan for target)
    if (targetView) glColor4f(0.0f, 1.0f, 1.0f, 0.8f);
    else            glColor4f(1.0f, 1.0f, 0.0f, 0.8f);
    glLineWidth(1.5f);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 4; ++i) {
        glVertex2d(to[i][0], to[i][1]);
    }
    glEnd();

    // Draw corner handles (small squares)
    double handleSize = 6.0;
    if (targetView) glColor4f(0.0f, 1.0f, 1.0f, 1.0f);
    else            glColor4f(1.0f, 1.0f, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        double cx = to[i][0], cy = to[i][1];
        glBegin(GL_LINE_LOOP);
        glVertex2d(cx - handleSize, cy - handleSize);
        glVertex2d(cx + handleSize, cy - handleSize);
        glVertex2d(cx + handleSize, cy + handleSize);
        glVertex2d(cx - handleSize, cy + handleSize);
        glEnd();
    }

    // Draw corner labels
    const char* labels[] = {"BL", "BR", "TR", "TL"};
    (void)labels; // Labels need text rendering — skip for now

    // Compute patch radius based on sample size and chart dimensions
    double chartW = std::sqrt(std::pow(to[1][0] - to[0][0], 2) + std::pow(to[1][1] - to[0][1], 2));
    double chartH = std::sqrt(std::pow(to[3][0] - to[0][0], 2) + std::pow(to[3][1] - to[0][1], 2));
    double sampleSz = _imp->sampleSize.lock()->getValue();
    double patchRadius = std::min(chartW / 6.0, chartH / 4.0) * 0.5 * sampleSz;
    if (patchRadius < 2.0) patchRadius = 2.0;

    const int circleSegments = 32;

    // Draw 24 patch centers (4 rows x 6 cols)
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 6; ++c) {
            int patchIdx = r * 6 + c;
            bool enabled = _imp->patchEnable[patchIdx].lock()->getValue();

            double u = (c + 0.5) / 6.0;
            double v = 1.0 - (r + 0.5) / 4.0;  // row 0 at top, row 3 (neutrals) at bottom

            // Bilinear interpolation on the quad
            double bx = to[0][0] + (to[1][0] - to[0][0]) * u;
            double by = to[0][1] + (to[1][1] - to[0][1]) * u;
            double tx = to[3][0] + (to[2][0] - to[3][0]) * u;
            double ty = to[3][1] + (to[2][1] - to[3][1]) * u;
            double px = bx + (tx - bx) * v;
            double py = by + (ty - by) * v;

            // Draw filled circle with patch color (from editable knobs)
            // Apply sRGB gamma so colors display correctly in the viewer
            auto linearToSRGB = [](double c) -> float {
                if (c <= 0.0031308) return (float)(c * 12.92);
                return (float)(1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
            };

            if (enabled) {
                KnobColorPtr colorK = _imp->patchColor[patchIdx].lock();
                float r = linearToSRGB(std::max(0.0, colorK->getValue(0)));
                float g = linearToSRGB(std::max(0.0, colorK->getValue(1)));
                float b = linearToSRGB(std::max(0.0, colorK->getValue(2)));
                glColor4f(r, g, b, 1.0f);
            } else {
                glColor4f(0.3f, 0.3f, 0.3f, 0.3f);
            }

            glBegin(GL_TRIANGLE_FAN);
            glVertex2d(px, py);
            for (int s = 0; s <= circleSegments; ++s) {
                double angle = s * 2.0 * M_PI / circleSegments;
                glVertex2d(px + patchRadius * std::cos(angle),
                           py + patchRadius * std::sin(angle));
            }
            glEnd();
        }
    }
}


// ---- Overlay: pen down (start dragging a corner) ----

bool
ColorChartMatch::onOverlayPenDown(double /*time*/,
                                   const RenderScale& /*renderScale*/,
                                   ViewIdx /*view*/,
                                   const QPointF& /*viewportPos*/,
                                   const QPointF& pos,
                                   double /*pressure*/,
                                   double /*timestamp*/,
                                   PenType /*pen*/)
{
    // Drag the corner-pin matching the current view (Target view -> target corners).
    KnobDoubleWPtr* corners = (_imp->currentView.lock()->getValue() == 1)
                              ? _imp->tgtTo : _imp->srcTo;
    double threshold = 15.0;
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr k = corners[i].lock();
        double cx = k->getValue(0);
        double cy = k->getValue(1);
        double dx = pos.x() - cx;
        double dy = pos.y() - cy;
        if (dx * dx + dy * dy < threshold * threshold) {
            _imp->draggingCorner = i;
            return true;
        }
    }
    return false;
}


// ---- Overlay: pen motion (drag corner) ----

bool
ColorChartMatch::onOverlayPenMotion(double /*time*/,
                                     const RenderScale& /*renderScale*/,
                                     ViewIdx /*view*/,
                                     const QPointF& /*viewportPos*/,
                                     const QPointF& pos,
                                     double /*pressure*/,
                                     double /*timestamp*/)
{
    if (_imp->draggingCorner >= 0 && _imp->draggingCorner < 4) {
        KnobDoubleWPtr* corners = (_imp->currentView.lock()->getValue() == 1)
                                  ? _imp->tgtTo : _imp->srcTo;
        KnobDoublePtr k = corners[_imp->draggingCorner].lock();
        k->setValue(pos.x(), ViewSpec::all(), 0);
        k->setValue(pos.y(), ViewSpec::all(), 1);
        return true;
    }
    return false;
}


// ---- Overlay: pen up (stop dragging) ----

bool
ColorChartMatch::onOverlayPenUp(double /*time*/,
                                 const RenderScale& /*renderScale*/,
                                 ViewIdx /*view*/,
                                 const QPointF& /*viewportPos*/,
                                 const QPointF& /*pos*/,
                                 double /*pressure*/,
                                 double /*timestamp*/)
{
    if (_imp->draggingCorner >= 0) {
        _imp->draggingCorner = -1;
        return true;
    }
    return false;
}


NATRON_NAMESPACE_EXIT

#include "moc_ColorChartMatch.cpp"
