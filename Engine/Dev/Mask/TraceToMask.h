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

#include "Global/Macros.h"

#include "Engine/RotoPaint.h"
#include "Engine/ViewIdx.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct TraceToMaskPrivate;

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

    /// Connected to the option knobs: re-traces when Auto Re-trace is on.
    void onControlKnobChanged(ViewSpec view, int dimension, int reason);

private:

    /**
     * @brief Knob creation hook (RotoPaint seals initializeKnobs - same as
     * ReadMask, this is the latest safe hook).
     **/
    virtual void initializeData() OVERRIDE FINAL;

    void trace(double time);
    void removeImportedItems();

    std::unique_ptr<TraceToMaskPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_MASK_TRACETOMASK_H
