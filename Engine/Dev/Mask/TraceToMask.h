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

// Bitmap tracing sibling of ReadMask: a raster image goes in, the node's own
// RotoContext Bézier shapes come out (layered mask like ReadMask), rendered as
// alpha like any Roto. Three tracing variants: B/W outline (potrace), color
// k-means clusters (stacked), and centerline (skeleton) for thin strokes.
// Backend: libs/potrace (GPL-2.0+, the Inkscape tracing engine) plus native
// clustering/skeletonization in parsers/PotraceTrace.cpp.

#ifndef NATRON_ENGINE_DEV_MASK_TRACETOMASK_H
#define NATRON_ENGINE_DEV_MASK_TRACETOMASK_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <memory>
#include <list>
#include <vector>

#include "Global/Macros.h"

#include "Engine/RotoPaint.h"
#include "Engine/ViewIdx.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct TraceToMaskPrivate;
namespace VectorMask {
struct VectorDocument;
struct TraceParams;
}

/**
 * @brief "TraceToMask": bitmap tracing that IS a Roto node.
 *
 * Traces input 1 (any raster: renders, scans, rotoscoped mattes) into real
 * Bézier shapes inside this node's own RotoContext and outputs the mask alpha
 * like any Roto. Shapes are project-native: editable in the overlay/curve
 * editor, serialized in the .natron project, and exposed to MaskToGeo /
 * Scene3D / CyclesRender like ReadMask's shapes.
 *
 * Modes:
 *  - B/W: binary outline trace (potrace), optional stacked threshold bands
 *    between Threshold Low and Threshold.
 *  - Color: k-means color clusters, each cluster traced and stacked with its
 *    mean color as the shape fill (VTracer-style stacking).
 *  - Centerline: Zhang-Suen skeletonization of thin strokes into open
 *    Bézier curves (line-art style).
 *
 * Holes (potrace child contours) punch with the Minus or XOR operator, toggle.
 * The Trace button re-runs on demand; Auto Re-trace re-runs whenever a knob
 * changes, exactly like ReadMask's Auto Rebuild. Tracing regenerates the
 * shapes by design - edits to traced shapes are wiped by the next trace.
 */
class TraceToMask
    : public RotoNode
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new TraceToMask(n); }

    TraceToMask(NodePtr node);
    virtual ~TraceToMask();

    // Versions are inherited from RotoPaint (1.0).

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_TRACETOMASK; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "TraceToMask"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /// The bitmap to trace: a single source image. RotoPaint's sealed defaults
    /// cover the rest of the input surface (optional inputs, RGBA/RGB/XY/Alpha
    /// components, float-only bit depths).
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;

    /// Vector-only surface: no brush/pencil/eraser tools (same as ReadMask).
    virtual bool supportsRasterPaintTools() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

private Q_SLOTS:

    /// Connected to the Trace button: re-traces regardless of Auto Re-trace.
    void onTraceClicked();

    /// Connected to the Reset button: removes every traced shape/layer.
    void onResetClicked();

    /// Connected to the Bake Sequence button: keys every traced control point
    /// across the [Bake From, Bake To] frame range so the shapes play back the
    /// traced animation as project-native animated BAcziers, independent of
    /// the input.
    void onBakeClicked();

    /// Connected to the option knobs: re-traces when Auto Re-trace is on.
    void onControlKnobChanged(ViewSpec view, int dimension, int reason);

    /// Connected to the timeline, this node's input connection changes and the
    /// input node's knob edits: re-traces when the input changed underneath the
    /// current shapes (image swap / resize) and Auto Re-trace is on - the
    /// shapes would otherwise sit at the old scale. The check is deferred to a
    /// zero-timer (onInputCheckTimeout): the input node emits knobsAgeChanged
    /// BEFORE recomputing its hash, so any hash-dependent work done in the
    /// direct slot would see the pre-edit state.
    void onInputMayHaveChanged();

    /// Runs the deferred input-change check (age / RoD compare -> trace).
    void onInputCheckTimeout();

private:

    /**
     * @brief Knob creation hook (RotoPaint seals initializeKnobs - same as
     * ReadMask, this is the latest safe hook).
     **/
    virtual void initializeData() OVERRIDE FINAL;

    /// Fetches input 1 at time (plane request, channel extraction, pre-blur,
    /// knob -> TraceParams). Returns false (and leaves outputs untouched) when
    /// nothing was fetched. *bounds receives the plane's canonical integer
    /// bounds - the trace is anchored there so the shapes land on the image.
    bool fetchPlaneAndParams(double time,
                             std::vector<float>* values,
                             std::vector<float>* valuesRGB,
                             int* w, int* h,
                             VectorMask::TraceParams* params,
                             RectI* bounds);

    /// fetchPlaneAndParams + tracePlaneToDocument, synchronous (bake path).
    bool buildTraceDocument(double time, VectorMask::VectorDocument* doc, RectI* bounds);

    /// Effective fit transform for a document of (docW, docH): Fit/Center
    /// modes map into the project format; Original/Custom anchor at the source
    /// plane's bounds origin (bx, by) so the shapes land on the image.
    void computeDocTransform(double docW, double docH, double bx, double by,
                             double* sx, double* sy, double* ox, double* oy,
                             bool* flipY, double* docHeight);

    /// Discards the previous import and imports doc into the RotoContext
    /// (layers from the shape layer names, shapes keyed at time).
    void importDocument(const VectorMask::VectorDocument& doc, const RectI& bounds, double time);

    /// Traces input 1 at time: the plane fetch runs on the main thread, the
    /// heavy potrace/cluster/skeleton compute on a background thread while a
    /// nested event loop keeps the UI responsive, and the RotoContext import
    /// back on the main thread. Coalesces re-entrant requests.
    void trace(double time);
    void removeImportedItems();

    /// (Dis)connects the input-change hooks (input node knob edits) for the
    /// current input.
    void updateInputHooks();

    std::unique_ptr<TraceToMaskPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_MASK_TRACETOMASK_H
