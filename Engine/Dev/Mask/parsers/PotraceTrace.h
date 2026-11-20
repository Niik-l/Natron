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

// Bitmap tracing for the TraceToMask node: binary outline tracing through the
// vendored potrace library (libs/potrace, GPL-2.0+ - the Inkscape backend),
// color quantization + stacked per-cluster traces (VTracer-style stacking,
// reimplemented natively), and Zhang-Suen centerline extraction for thin
// strokes. Output lands in the shared VectorDocument so the ReadMask import
// path (VectorDocument -> RotoContext Beziers) is reused unchanged.

#ifndef NATRON_ENGINE_DEV_MASK_PARSERS_POTRACETRACE_H
#define NATRON_ENGINE_DEV_MASK_PARSERS_POTRACETRACE_H

#include <string>
#include <vector>

#include "Global/Macros.h"

#include "Dev/Mask/VectorDocument.h"

NATRON_NAMESPACE_ENTER

namespace VectorMask {

enum TraceModeEnum
{
    eTraceModeBW = 0,       // binary outline trace (potrace), optional stacked bands
    eTraceModeColor = 1,    // k-means color clusters, each traced and stacked
    eTraceModeCenterline = 2 // skeletonize thin strokes into open Beziers
};

struct TraceParams
{
    TraceModeEnum mode = eTraceModeBW;

    // Binary threshold (bw / centerline; per band for stacked bw)
    double threshold = 0.5;        // foreground = value > threshold (after invert)
    double thresholdLow = 0.0;     // stacked bands: low edge of the band range
    int bands = 1;                 // stacked bw bands (>= 1)

    // potrace parameters
    double despeckle = 2.0;        // potrace turdsize: drop areas < N pixels
    double cornerThreshold = 1.0;  // potrace alphamax 0..1.334 (0 = polygon)
    bool curveOptimization = true; // potrace opticurve
    double optTolerance = 0.2;     // potrace opttolerance

    // color mode
    int colorCount = 4;            // k-means clusters (2..16)

    // centerline
    double simplifyTolerance = 1.0; // RDP tolerance in pixels

    // how holes punch: a Natron::MergingFunctionEnum for child (hole) contours
    // (eMergeMinus or eMergeXOR)
    int holeOperator = 25;         // eMergeMinus by default (see Global/Enums.h)

    bool invert = false;           // invert before thresholding
};

/**
 * @brief Trace a float plane (row-major, row 0 = image TOP, values 0..1)
 * into VectorDocument shapes in doc pixel coordinates (y-down, same frame as
 * the SVG parser output - the standard Invert-Y import transform applies
 * downstream). valuesRGB (w*h*3, 0..1) feeds the color-mode k-means; it may
 * be empty for bw/centerline. Returns false when nothing traced.
 */
bool tracePlaneToDocument(int w,
                          int h,
                          const std::vector<float>& values,
                          const std::vector<float>& valuesRGB,
                          const TraceParams& params,
                          const std::string& layerName,
                          VectorDocument* doc);

} // namespace VectorMask

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_MASK_PARSERS_POTRACETRACE_H
