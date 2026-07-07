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

#include "PointCloudGeneratorNode.h"

#include <map>
#include <vector>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <functional>
#include <QtConcurrent>
#include <QFuture>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#include "Engine/Dev/Scene3D/CameraProvider.h"
#include "Engine/Dev/Scene3D/RotationConventions.h"

#include <libmv/numeric/numeric.h>
#include <libmv/multiview/nviewtriangulation.h>
#include <libmv/simple_pipeline/detect.h>
#include <libmv/tracking/track_region.h>
#include <libmv/image/image.h>

NATRON_NAMESPACE_ENTER

// ==================== Private ====================

struct PointCloudGeneratorNodePrivate
{
    PointCloudGeneratorNode* _p;

    KnobIntWPtr frameStart;
    KnobIntWPtr frameEnd;
    KnobIntWPtr pointSeparation;
    KnobIntWPtr maxPoints;
    KnobIntWPtr keyframeSpacing;
    KnobDoubleWPtr trackThreshold;
    KnobDoubleWPtr minAngle;
    KnobDoubleWPtr maxReproError;
    KnobDoubleWPtr pointSize;
    KnobStringWPtr status;
    KnobButtonWPtr generateBtn;

    PointCloudDataPtr cloud;

    struct Track {
        std::map<int, std::pair<double, double> > obs; // frame -> (mvX, mvY) libmv top-down px
    };

    PointCloudGeneratorNodePrivate(PointCloudGeneratorNode* p) : _p(p) {}

    void generate();
};


// ==================== ctor / metadata ====================

PointCloudGeneratorNode::PointCloudGeneratorNode(NodePtr node)
    : EffectInstance(node)
    , _imp(new PointCloudGeneratorNodePrivate(this))
{
}

PointCloudGeneratorNode::~PointCloudGeneratorNode() {}

std::string
PointCloudGeneratorNode::getPluginDescription() const
{
    return "Dense Point Cloud Generator\n"
           "===========================\n\n"
           "Generates a dense point cloud from footage using an already-solved camera "
           "(a second pass that reuses the solved camera instead of re-solving).\n\n"
           "**Workflow:**\n"
           "1. Connect footage to **Source** and a solved Camera3D to **Camera**\n"
           "2. Set the frame range and **Point Separation** (lower = denser)\n"
           "3. Click **Generate Cloud**\n\n"
           "Because the camera is already known, features are triangulated directly "
           "(N-view DLT) without re-solving. The cloud appears in the 3D Viewport, "
           "registered to the camera. Image output is a pass-through of Source.";
}

void
PointCloudGeneratorNode::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
PointCloudGeneratorNode::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

StatusEnum
PointCloudGeneratorNode::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                               ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

bool
PointCloudGeneratorNode::isIdentity(double time, const RenderScale& /*scale*/, const RectI& /*roi*/,
                                    ViewIdx view, double* inputTime, ViewIdx* inputView, int* inputNb)
{
    // Pure pass-through of the Source footage.
    *inputTime = time;
    *inputView = view;
    *inputNb = 0;
    return true;
}


// ==================== Knobs ====================

void
PointCloudGeneratorNode::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Range Start"));
        k->setName("frameStart"); k->setDefaultValue(1); k->setAnimationEnabled(false);
        page->addKnob(k); _imp->frameStart = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Range End"));
        k->setName("frameEnd"); k->setDefaultValue(50); k->setAnimationEnabled(false);
        page->addKnob(k); _imp->frameEnd = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Point Separation"));
        k->setName("pointSeparation"); k->setDefaultValue(8);
        k->setMinimum(2); k->setMaximum(100); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Min pixel spacing between detected points. Lower = denser cloud."));
        page->addKnob(k); _imp->pointSeparation = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Points"));
        k->setName("maxPoints"); k->setDefaultValue(5000);
        k->setMinimum(100); k->setMaximum(100000); k->setAnimationEnabled(false);
        page->addKnob(k); _imp->maxPoints = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Keyframe Spacing"));
        k->setName("keyframeSpacing"); k->setDefaultValue(10);
        k->setMinimum(1); k->setMaximum(200); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Re-seed the grid every N frames and track each batch BOTH directions. "
                             "Smaller = more keyframes = better coverage + wider baseline on forward "
                             "motion (more parallax to triangulate), at more compute. Max Points is "
                             "split across keyframes."));
        page->addKnob(k); _imp->keyframeSpacing = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Track Threshold"));
        k->setName("trackThreshold"); k->setDefaultValue(0.7); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Minimum correlation to accept a tracked point."));
        page->addKnob(k); _imp->trackThreshold = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Min Triangulation Angle"));
        k->setName("minAngle"); k->setDefaultValue(1.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Reject points whose rays span less than this angle (degrees) — too little parallax."));
        page->addKnob(k); _imp->minAngle = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Max Reprojection Error"));
        k->setName("maxReproError"); k->setDefaultValue(4.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Reject points whose mean reprojection error (px) exceeds this."));
        page->addKnob(k); _imp->maxReproError = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Point Size"));
        k->setName("pointSize"); k->setDefaultValue(2.0); k->setAnimationEnabled(false);
        page->addKnob(k); _imp->pointSize = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Status"));
        k->setName("status"); k->setDefaultValue("Not generated");
        k->setAsLabel(); k->setAnimationEnabled(false);
        page->addKnob(k); _imp->status = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Generate Cloud"));
        k->setName("generate"); k->setEvaluateOnChange(false);
        page->addKnob(k); _imp->generateBtn = k;
    }
}

bool
PointCloudGeneratorNode::knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec /*view*/,
                                     double /*time*/, bool /*originatedFromMainThread*/)
{
    if (reason == eValueChangedReasonNatronGuiEdited || reason == eValueChangedReasonUserEdited) {
        if (k == _imp->generateBtn.lock().get()) {
            _imp->generate();
            return true;
        }
    }
    return false;
}

PointCloudDataPtr
PointCloudGeneratorNode::getPointCloud() const
{
    if (_imp->cloud) {
        return _imp->cloud;
    }
    return PointCloudDataPtr(new PointCloudData()); // empty
}


// ==================== Generation ====================

void
PointCloudGeneratorNodePrivate::generate()
{
    try {
    NodePtr node = _p->getNode();
    if (!node) return;

    NodePtr inputNode = node->getInput(0);
    if (!inputNode) { status.lock()->setValue("Connect footage to Source (input 1)"); return; }
    EffectInstancePtr input = inputNode->getEffectInstance();
    if (!input) { status.lock()->setValue("Connect footage to Source (input 1)"); return; }

    NodePtr camNode = node->getInput(1);
    CameraProvider* cam = camNode ? dynamic_cast<CameraProvider*>(camNode->getEffectInstance().get()) : NULL;
    if (!cam) { status.lock()->setValue("Connect a solved Camera3D to Camera (input 2)"); return; }

    const int rangeStart = frameStart.lock()->getValue();
    const int rangeEnd   = frameEnd.lock()->getValue();
    if (rangeStart >= rangeEnd) { status.lock()->setValue("Invalid frame range"); return; }

    // Image dimensions from the source RoD at the first frame.
    RectD rod;
    RenderScale scale;
    bool isPF = false;
    if (input->getRegionOfDefinition_public(inputNode->getHashValue(), (double)rangeStart, scale, ViewIdx(0), &rod, &isPF) != eStatusOK) {
        status.lock()->setValue("Failed to get source dimensions"); return;
    }
    const int width  = (int)rod.width();
    const int height = (int)rod.height();
    if (width <= 0 || height <= 0) { status.lock()->setValue("Invalid source dimensions"); return; }

    // --- render a frame to a grayscale libmv image ---
    auto renderGray = [&](int frame) -> libmv::FloatImage {
        libmv::FloatImage img(height, width, 1);
        RectI roi; roi.x1 = (int)rod.x1; roi.y1 = (int)rod.y1; roi.x2 = (int)rod.x2; roi.y2 = (int)rod.y2;
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        AbortableThread* abortable = dynamic_cast<AbortableThread*>(QThread::currentThread());
        if (abortable) abortable->setAbortInfo(true, abortInfo, node->getEffectInstance());
        ParallelRenderArgsSetter frameArgs((double)frame, ViewIdx(0), true, false, abortInfo, node, 0,
                                           node->getApp()->getTimeLine().get(), NodePtr(), true, false, RenderStatsPtr());
        std::list<ImagePlaneDesc> comps; comps.push_back(ImagePlaneDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args((double)frame, scale, 0, ViewIdx(0), false, roi, rod, comps,
                                           eImageBitDepthFloat, true, node->getEffectInstance().get(), eStorageModeRAM, (double)frame);
        std::map<ImagePlaneDesc, ImagePtr> planes;
        if (input->renderRoI(args, &planes) != EffectInstance::eRenderRoIRetCodeOk || planes.empty()) return img;
        ImagePtr im = planes.begin()->second;
        if (!im) return img;
        RectI b = im->getBounds();
        Image::ReadAccess ra(im.get());
        for (int y = b.y1; y < b.y2; ++y) {
            for (int x = b.x1; x < b.x2; ++x) {
                const float* pix = (const float*)ra.pixelAt(x, y);
                if (!pix) continue;
                float lum = 0.2126f * pix[0] + 0.7152f * pix[1] + 0.0722f * pix[2];
                int mvY = (b.y2 - 1) - y;
                img(mvY, x - b.x1, 0) = lum;
            }
        }
        return img;
    };

    const int sep = std::max(2, pointSeparation.lock()->getValue());
    const int maxPts = maxPoints.lock()->getValue();
    const int nFrames = rangeEnd - rangeStart + 1;

    // Pre-render every frame to grayscale once. Multi-keyframe bidirectional
    // tracking visits frames repeatedly, so caching avoids re-rendering.
    // (~8 MB/frame at 1080p — fine for typical ranges.)
    status.lock()->setValue("Rendering frames...");
    std::vector<libmv::FloatImage> gray((std::size_t)nFrames);
    for (int f = rangeStart; f <= rangeEnd; ++f) {
        gray[(std::size_t)(f - rangeStart)] = renderGray(f);
    }

    // Keyframes across the range: re-seed the grid every `keyframeSpacing` frames so
    // coverage fills in as the camera moves (seeds from frame 1 alone leave the frame
    // on a forward dolly). Max Points is split across keyframes.
    int kfSpacing = keyframeSpacing.lock()->getValue();
    if (kfSpacing < 1) kfSpacing = nFrames;
    std::vector<int> keyframes;
    for (int f = rangeStart; f <= rangeEnd; f += kfSpacing) keyframes.push_back(f);
    if (keyframes.empty()) keyframes.push_back(rangeStart);
    const int budgetPerKf = std::max(1, maxPts / (int)keyframes.size());

    const double half = 10.0;
    const double edge = half + 3.0;
    const int searchRadius = 100;
    libmv::TrackRegionOptions topt;
    topt.mode = libmv::TrackRegionOptions::TRANSLATION;
    topt.minimum_correlation = trackThreshold.lock()->getValue();
    topt.max_iterations = 16;
    topt.use_brute_initialization = true;
    topt.attempt_refine_before_brute = true;

    std::vector<Track> tracks;

    // One windowed track step: track `track`'s marker at fromF into toF using the
    // cached images. Thread-safe — reads the const images, writes only its own map.
    auto trackStep = [&](Track& track, int fromF, int toF,
                         const libmv::FloatImage& fromImg, const libmv::FloatImage& toImg) -> bool {
        std::map<int, std::pair<double, double> >::const_iterator it = track.obs.find(fromF);
        if (it == track.obs.end()) return false;
        double px = it->second.first, py = it->second.second;
        if (px < edge || px >= width - edge || py < edge || py >= height - edge) return false;
        const int margin = (int)half + searchRadius;
        int wx0 = std::max(0, (int)std::floor(px) - margin);
        int wy0 = std::max(0, (int)std::floor(py) - margin);
        int wx1 = std::min(width,  (int)std::ceil(px) + margin);
        int wy1 = std::min(height, (int)std::ceil(py) + margin);
        int ww = wx1 - wx0, wh = wy1 - wy0;
        const int minWin = (int)(2 * half) + 2;
        if (ww < minWin || wh < minWin) return false;
        libmv::FloatImage pw(wh, ww, 1), cw2(wh, ww, 1);
        for (int yy = 0; yy < wh; ++yy)
            for (int xx = 0; xx < ww; ++xx) {
                pw(yy, xx, 0)  = fromImg(wy0 + yy, wx0 + xx, 0);
                cw2(yy, xx, 0) = toImg(wy0 + yy, wx0 + xx, 0);
            }
        double lpx = px - wx0, lpy = py - wy0;
        double X1[4], Y1[4], X2[4], Y2[4];
        X1[0]=lpx-half; Y1[0]=lpy-half; X1[1]=lpx+half; Y1[1]=lpy-half;
        X1[2]=lpx+half; Y1[2]=lpy+half; X1[3]=lpx-half; Y1[3]=lpy+half;
        for (int c = 0; c < 4; ++c) { X2[c]=X1[c]; Y2[c]=Y1[c]; }
        libmv::TrackRegionResult res; res.termination = libmv::TrackRegionResult::FAILURE;
        try { libmv::TrackRegion(pw, cw2, X1, Y1, topt, X2, Y2, &res); } catch (...) { return false; }
        if (!res.is_usable()) return false;
        double nx = (X2[0]+X2[1]+X2[2]+X2[3])/4.0 + wx0;
        double ny = (Y2[0]+Y2[1]+Y2[2]+Y2[3])/4.0 + wy0;
        double dx = nx - px, dy = ny - py;
        if (dx*dx + dy*dy > 10000.0) return false;
        track.obs[toF] = std::make_pair(nx, ny);
        return true;
    };

    for (std::size_t ki = 0; ki < keyframes.size(); ++ki) {
        const int kf = keyframes[ki];
        {
            std::stringstream ss; ss << "Tracking keyframe " << (ki + 1) << "/" << keyframes.size() << "...";
            status.lock()->setValue(ss.str());
        }

        // Seed a grid at this keyframe (per-keyframe budget, spread over the frame).
        const std::size_t kfStart = tracks.size();
        {
            const int m = 16;
            double availW = (double)std::max(1, width - 2 * m);
            double availH = (double)std::max(1, height - 2 * m);
            double fullG = (availW / sep) * (availH / sep);
            double step = (fullG > budgetPerKf) ? sep * std::sqrt(fullG / (double)budgetPerKf) : (double)sep;
            for (double gy = m; gy < height - m; gy += step) {
                for (double gx = m; gx < width - m; gx += step) {
                    Track tr; tr.obs[kf] = std::make_pair(std::floor(gx), std::floor(gy));
                    tracks.push_back(tr);
                }
            }
        }
        std::vector<std::size_t> idx;
        for (std::size_t i = kfStart; i < tracks.size(); ++i) idx.push_back(i);

        // Track this batch BOTH directions across the range → wide baseline.
        for (int f = kf + 1; f <= rangeEnd; ++f) {
            const libmv::FloatImage& fromImg = gray[(std::size_t)(f - 1 - rangeStart)];
            const libmv::FloatImage& toImg   = gray[(std::size_t)(f - rangeStart)];
            std::function<bool(const std::size_t&)> one = [&, f](const std::size_t& ti) -> bool {
                return trackStep(tracks[ti], f - 1, f, fromImg, toImg);
            };
            QtConcurrent::mapped(idx, one).waitForFinished();
        }
        for (int f = kf - 1; f >= rangeStart; --f) {
            const libmv::FloatImage& fromImg = gray[(std::size_t)(f + 1 - rangeStart)];
            const libmv::FloatImage& toImg   = gray[(std::size_t)(f - rangeStart)];
            std::function<bool(const std::size_t&)> one = [&, f](const std::size_t& ti) -> bool {
                return trackStep(tracks[ti], f + 1, f, fromImg, toImg);
            };
            QtConcurrent::mapped(idx, one).waitForFinished();
        }
    }

    // --- build per-frame projection matrices from the known camera ---
    status.lock()->setValue("Triangulating...");
    std::map<int, libmv::Mat34> Pmap;
    std::map<int, libmv::Vec3> Cmap;
    const double fSign[3] = {1.0, -1.0, -1.0}; // CV<->Natron axis flip: F = diag(1,-1,-1)
    for (int frame = rangeStart; frame <= rangeEnd; ++frame) {
        double tx, ty, tz, rx, ry, rz;
        cam->getCameraPosition((double)frame, tx, ty, tz, rx, ry, rz);
        double focalMm = cam->getCameraFocalLength((double)frame);
        double hAp = cam->getCameraHAperture((double)frame);
        if (hAp <= 1e-6) continue;
        double Rcw[3][3];
        RotationConventions::compose(rx, ry, rz, Rcw); // camera-to-world (extrinsic XYZ)
        // world->camera (CV): R = F * Rcw^T
        libmv::Mat3 R;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                R(i, j) = fSign[i] * Rcw[j][i];
        libmv::Vec3 C; C << tx, ty, tz;
        libmv::Vec3 t = -R * C;
        double fpx = (focalMm / hAp) * (double)width;
        libmv::Mat3 K; K << fpx, 0, width / 2.0,  0, fpx, height / 2.0,  0, 0, 1;
        libmv::Mat34 Rt; Rt.block<3,3>(0,0) = R; Rt.col(3) = t;
        Pmap[frame] = K * Rt;
        Cmap[frame] = C;
    }

    // --- N-view triangulation per track ---
    cloud.reset(new PointCloudData());
    cloud->reserve(tracks.size());
    const double minAng = minAngle.lock()->getValue() * M_PI / 180.0;
    const double maxErr = maxReproError.lock()->getValue();
    float bbMin[3] = { 1e30f, 1e30f, 1e30f}, bbMax[3] = {-1e30f, -1e30f, -1e30f};
    int kept = 0;

    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        std::vector<int> fs;
        for (std::map<int, std::pair<double,double> >::const_iterator it = tracks[ti].obs.begin();
             it != tracks[ti].obs.end(); ++it) {
            if (Pmap.find(it->first) != Pmap.end()) fs.push_back(it->first);
        }
        if ((int)fs.size() < 2) continue;

        libmv::Mat2X x(2, (int)fs.size());
        libmv::vector<libmv::Mat34> Ps;
        for (size_t k = 0; k < fs.size(); ++k) {
            const std::pair<double,double>& o = tracks[ti].obs[fs[k]];
            x(0, (int)k) = o.first;
            x(1, (int)k) = o.second;
            Ps.push_back(Pmap[fs[k]]);
        }
        libmv::Vec4 Xh;
        libmv::NViewTriangulate(x, Ps, &Xh);
        if (std::abs(Xh(3)) < 1e-12) continue;
        libmv::Vec3 X = Xh.head(3) / Xh(3);

        // reprojection error + in-front check
        double sumErr = 0; bool behind = false;
        for (size_t k = 0; k < fs.size(); ++k) {
            libmv::Vec3 p = Ps[k] * Xh;
            if (p(2) <= 0) { behind = true; break; }
            double u = p(0) / p(2), v = p(1) / p(2);
            double ex = u - x(0, (int)k), ey = v - x(1, (int)k);
            sumErr += std::sqrt(ex*ex + ey*ey);
        }
        if (behind) continue;
        if (sumErr / fs.size() > maxErr) continue;

        // triangulation angle between first and last camera rays
        libmv::Vec3 r1 = (X - Cmap[fs.front()]).normalized();
        libmv::Vec3 r2 = (X - Cmap[fs.back()]).normalized();
        double ang = std::acos(std::max(-1.0, std::min(1.0, (double)r1.dot(r2))));
        if (ang < minAng) continue;

        float fx = (float)X(0), fy = (float)X(1), fz = (float)X(2);
        cloud->addPoint(fx, fy, fz, 0.9f, 0.9f, 0.9f);
        bbMin[0]=std::min(bbMin[0],fx); bbMin[1]=std::min(bbMin[1],fy); bbMin[2]=std::min(bbMin[2],fz);
        bbMax[0]=std::max(bbMax[0],fx); bbMax[1]=std::max(bbMax[1],fy); bbMax[2]=std::max(bbMax[2],fz);
        ++kept;
    }

    if (kept > 0) cloud->setBounds(bbMin[0],bbMin[1],bbMin[2], bbMax[0],bbMax[1],bbMax[2]);

    std::stringstream ss;
    ss << "Generated " << kept << " points from " << tracks.size() << " tracks";
    status.lock()->setValue(ss.str());

    } catch (const std::exception& e) {
        status.lock()->setValue(std::string("Generate failed: ") + e.what());
    } catch (...) {
        status.lock()->setValue("Generate failed: unknown error");
    }
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_PointCloudGeneratorNode.cpp"
