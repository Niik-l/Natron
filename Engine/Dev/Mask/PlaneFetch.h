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

#ifndef NATRON_ENGINE_DEV_MASK_PLANEFETCH_H
#define NATRON_ENGINE_DEV_MASK_PLANEFETCH_H

// PlaneFetch: the worker-safe input-plane fetch used by the tracing node
// (TraceMask). Render the input's plane over its RoD from any
// thread with fresh per-thread render args (the TrackMarker::getMarkerImage
// pattern), then lift pixels into a plain float buffer for the tracer math.

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <list>
#include <map>
#include <vector>

#include "Global/Macros.h"

#include "Engine/AppManager.h"
#include "Engine/AbortableRenderInfo.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/ThreadPool.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace VectorMask {

/**
 * @brief Render `node`'s plane `comps` at `time` over `inputRod` (queried by
 * the caller on the main thread) into a RAM float image. Usable from worker
 * threads: pushes fresh render TLS for this thread (isAnalysis: no stale
 * request pass) and cleans it up afterwards, and suppresses the project's
 * auto-format for the duration so this off-schedule render doesn't move the
 * project format. Returns a null image when the fetch fails.
 */
inline ImagePtr
fetchInputPlane(const NodePtr& node,
                EffectInstance* caller,
                double time,
                const RectD& inputRod,
                const ImagePlaneDesc& comps)
{
    if (!node || !node->getEffectInstance()) {
        return ImagePtr();
    }

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    AbortableThread* isAbortable = dynamic_cast<AbortableThread*>(QThread::currentThread());
    if (isAbortable) {
        isAbortable->setAbortInfo(true, abortInfo, node->getEffectInstance());
    }
    ParallelRenderArgsSetter frameRenderArgs(time,
                                             ViewIdx(0),
                                             true,   // isRenderUserInteraction
                                             false,  // isSequentialRender
                                             abortInfo,
                                             node,
                                             0,      // textureIndex
                                             node->getApp() ? node->getApp()->getTimeLine().get() : (TimeLine*)0,
                                             NodePtr(),
                                             true,   // isAnalysis
                                             false,  // draftMode
                                             RenderStatsPtr());

    // The reader would otherwise consume the project's auto-format on this
    // off-schedule render (renderRoIInternal sets it from non-main threads);
    // suppress + restore so the project format keeps following the first
    // regular render, not our trace fetch.
    const ProjectPtr proj = node->getApp() ? node->getApp()->getProject() : ProjectPtr();
    const bool wasAutoFormat = proj ? proj->isAutoSetProjectFormatEnabled() : false;
    if (wasAutoFormat) {
        proj->setAutoSetProjectFormatEnabled(false);
    }

    std::list<ImagePlaneDesc> compsList;
    compsList.push_back(comps);
    const double par = node->getEffectInstance()->getAspectRatio(0);
    const RectI roiPixel = inputRod.toPixelEnclosing(0, par);
    EffectInstance::RenderRoIArgs args(time,
                                       RenderScale::identity,
                                       0,        // mipmapLevel
                                       ViewIdx(0),
                                       false,    // byPassCache
                                       roiPixel,
                                       inputRod, // preComputedRoD: queried on the main thread
                                       compsList,
                                       eImageBitDepthFloat,
                                       true,     // fetchUserSelectedComponentsUpstream
                                       caller,
                                       eStorageModeRAM,
                                       time);
    std::map<ImagePlaneDesc, ImagePtr> planes;
    const EffectInstance::RenderRoIRetCode stat = node->getEffectInstance()->renderRoI(args, &planes);

    if (wasAutoFormat && proj) {
        proj->setAutoSetProjectFormatEnabled(true);
    }

    appPTR->getAppTLS()->cleanupTLSForThread();
    if (stat != EffectInstance::eRenderRoIRetCodeOk || planes.empty()) {
        return ImagePtr();
    }
    return planes.begin()->second;
}

/// Channel selectors for extractPlaneValues (Krokodove "Map Channel" order).
enum PlaneChannelEnum
{
    ePlaneChannelRed = 0,
    ePlaneChannelGreen = 1,
    ePlaneChannelBlue = 2,
    ePlaneChannelAlpha = 3,
    ePlaneChannelLuminance = 4
};

/**
 * @brief Lift a fetched plane into tightly packed float buffers (row-major,
 * row 0 = image BOTTOM: Natron buffer order, y-up - pixelAt iterates Natron y
 * from bounds.y1). `values` receives the selected channel (RGB sources
 * fall back to the first component; alpha-only sources ignore the selector
 * and yield the scalar value). For RGBA sources, `valuesRGB` (w*h*3) also
 * receives the raw RGB triple for color-mode clustering; pass null otherwise.
 */
inline void
extractPlaneValues(const ImagePtr& srcImg,
                   int channel,
                   std::vector<float>* values,
                   std::vector<float>* valuesRGB,
                   int* wOut, int* hOut)
{
    if (!srcImg) {
        return;
    }
    const RectI bounds = srcImg->getBounds();
    const int w = bounds.width();
    const int h = bounds.height();
    if (w <= 0 || h <= 0 || !values) {
        return;
    }
    values->assign((std::size_t)w * h, 0.0f);

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
                // value regardless of the channel selector.
                (*values)[idx] = p[0];
                continue;
            }
            switch (channel) {
            case ePlaneChannelAlpha:
                (*values)[idx] = (nComps >= 4) ? p[3] : p[0];
                break;
            case ePlaneChannelRed:
                (*values)[idx] = p[0];
                break;
            case ePlaneChannelGreen:
                (*values)[idx] = (nComps >= 2) ? p[1] : p[0];
                break;
            case ePlaneChannelBlue:
                (*values)[idx] = (nComps >= 3) ? p[2] : p[0];
                break;
            case ePlaneChannelLuminance:
            default:
                (*values)[idx] = 0.2126f * p[0] + 0.7152f * ((nComps >= 2) ? p[1] : p[0])
                                 + 0.0722f * ((nComps >= 3) ? p[2] : p[0]);
                break;
            }
        }
    }
    if (valuesRGB && nComps >= 3) {
        valuesRGB->resize((std::size_t)w * h * 3, 0.0f);
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            for (int x = bounds.x1; x < bounds.x2; ++x) {
                const float* p = (const float*)acc.pixelAt(x, y);
                if (!p) {
                    continue;
                }
                const std::size_t idx = (std::size_t)(y - bounds.y1) * w + (x - bounds.x1);
                (*valuesRGB)[idx * 3 + 0] = p[0];
                (*valuesRGB)[idx * 3 + 1] = p[1];
                (*valuesRGB)[idx * 3 + 2] = p[2];
            }
        }
    }
    if (wOut) {
        *wOut = w;
    }
    if (hOut) {
        *hOut = h;
    }
}

} // namespace VectorMask

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_MASK_PLANEFETCH_H
