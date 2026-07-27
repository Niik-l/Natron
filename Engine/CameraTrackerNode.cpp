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

#include "CameraTrackerNode.h"

#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <map>

#include "Global/GLIncludes.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Image.h"
#include "Engine/ImagePlaneDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Lut.h"
#include "Engine/Node.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/Texture.h"
#include "Engine/TextureRect.h"
#include "Engine/NodeGroup.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/TrackerFrameAccessor.h"
#include "Engine/ViewIdx.h"

#include "Engine/Dev/Scene3D/RotationConventions.h"

#include "Global/FStreamsSupport.h"

#include <array>
#include <chrono>
#include <functional>
#include <iomanip>
#include <set>
#include <QtConcurrent>
#include <QFuture>

#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>
#include <Eigen/LU>

#include <libmv/simple_pipeline/tracks.h>
#include <libmv/simple_pipeline/reconstruction.h>
#include <libmv/simple_pipeline/camera_intrinsics.h>
#include <libmv/simple_pipeline/pipeline.h>
#include <libmv/simple_pipeline/initialize_reconstruction.h>
#include <libmv/simple_pipeline/intersect.h>
#include <libmv/simple_pipeline/resect.h>
#include <libmv/simple_pipeline/bundle.h>
#include <libmv/simple_pipeline/keyframe_selection.h>
#include <libmv/simple_pipeline/modal_solver.h>
#include <libmv/multiview/fundamental.h>   // NormalizedEightPointSolver, SampsonDistance (RANSAC init)
#include <libmv/simple_pipeline/detect.h>
#include <libmv/tracking/track_region.h>
#include <libmv/image/image.h>
#include <libmv/autotrack/autotrack.h>
#include <libmv/autotrack/frame_accessor.h>

NATRON_NAMESPACE_ANONYMOUS_ENTER

// Local contrast normalization for feature DETECTION (not tracking): out =
// (I - localMean) / (localStd + eps), computed with integral images in O(N).
// Low-contrast structure (grey consoles, hazy terrain) then scores like
// high-contrast structure, so Harris fires everywhere there is texture instead
// of only on the brightest corners. eps keeps flat areas flat instead of
// amplifying noise.
static libmv::FloatImage
normalizeLocalContrast(const libmv::FloatImage& in, int radius)
{
    const int h = in.Height(), w = in.Width();
    std::vector<double> integ((h + 1) * (w + 1), 0.0);
    std::vector<double> integ2((h + 1) * (w + 1), 0.0);
    const int stride = w + 1;
    for (int y = 0; y < h; ++y) {
        double rowSum = 0.0, rowSum2 = 0.0;
        for (int x = 0; x < w; ++x) {
            const double v = in(y, x, 0);
            rowSum += v; rowSum2 += v * v;
            integ [(y + 1) * stride + (x + 1)] = integ [y * stride + (x + 1)] + rowSum;
            integ2[(y + 1) * stride + (x + 1)] = integ2[y * stride + (x + 1)] + rowSum2;
        }
    }
    libmv::FloatImage out(h, w, 1);
    const double eps = 0.02; // in luminance units (footage is ~0..1)
    for (int y = 0; y < h; ++y) {
        const int y0 = std::max(0, y - radius), y1 = std::min(h, y + radius + 1);
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - radius), x1 = std::min(w, x + radius + 1);
            const double n = (double)(y1 - y0) * (x1 - x0);
            const double s  = integ [y1 * stride + x1] - integ [y0 * stride + x1]
                            - integ [y1 * stride + x0] + integ [y0 * stride + x0];
            const double s2 = integ2[y1 * stride + x1] - integ2[y0 * stride + x1]
                            - integ2[y1 * stride + x0] + integ2[y0 * stride + x0];
            const double mean = s / n;
            const double var = std::max(0.0, s2 / n - mean * mean);
            out(y, x, 0) = (float)((in(y, x, 0) - mean) / (std::sqrt(var) + eps));
        }
    }
    return out;
}

// Exact homography from 4 point correspondences (DLT, Eigen SVD). Used to
// transfer a planar quad's interior grid from the reference frame to every
// tracked frame — projectively correct, unlike bilinear interpolation of the
// corners, which is wrong under foreshortening.
static bool
homographyFrom4Pts(const double* q1, const double* q2, Eigen::Matrix3d* H)
{
    Eigen::Matrix<double, 8, 9> A;
    for (int i = 0; i < 4; ++i) {
        const double x = q1[i * 2], y = q1[i * 2 + 1];
        const double u = q2[i * 2], v = q2[i * 2 + 1];
        A.row(i * 2)     << -x, -y, -1, 0, 0, 0, u * x, u * y, u;
        A.row(i * 2 + 1) << 0, 0, 0, -x, -y, -1, v * x, v * y, v;
    }
    Eigen::JacobiSVD<Eigen::Matrix<double, 8, 9> > svd(A, Eigen::ComputeFullV);
    Eigen::Matrix<double, 9, 1> h = svd.matrixV().col(8);
    if (std::abs(h(8)) < 1e-14) return false;
    (*H) << h(0), h(1), h(2), h(3), h(4), h(5), h(6), h(7), h(8);
    (*H) /= h(8);
    return true;
}

// Least-squares homography over N>=4 correspondences (Hartley-normalized DLT).
// The mandatory RANSAC refit step: keeping the winning 4-point exact sample
// makes the chosen subset flip frame to frame with sub-pixel noise — visible
// quad jitter once manual pins pool across coplanar planars.
static bool
homographyFromNPts(const std::vector<std::pair<std::pair<double,double>, std::pair<double,double> > >& corr,
                   Eigen::Matrix3d* H)
{
    const int n = (int)corr.size();
    if (n < 4) return false;
    // Hartley normalization: centroid to origin, mean distance sqrt(2).
    double cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
    for (const auto& c : corr) {
        cx1 += c.first.first;  cy1 += c.first.second;
        cx2 += c.second.first; cy2 += c.second.second;
    }
    cx1 /= n; cy1 /= n; cx2 /= n; cy2 /= n;
    double d1 = 0, d2 = 0;
    for (const auto& c : corr) {
        d1 += std::hypot(c.first.first - cx1, c.first.second - cy1);
        d2 += std::hypot(c.second.first - cx2, c.second.second - cy2);
    }
    d1 /= n; d2 /= n;
    if (d1 < 1e-9 || d2 < 1e-9) return false;
    const double s1 = std::sqrt(2.0) / d1, s2 = std::sqrt(2.0) / d2;

    Eigen::MatrixXd A(2 * n, 9);
    for (int i = 0; i < n; ++i) {
        const double x = (corr[i].first.first  - cx1) * s1;
        const double y = (corr[i].first.second - cy1) * s1;
        const double u = (corr[i].second.first  - cx2) * s2;
        const double v = (corr[i].second.second - cy2) * s2;
        A.row(i * 2)     << -x, -y, -1, 0, 0, 0, u * x, u * y, u;
        A.row(i * 2 + 1) << 0, 0, 0, -x, -y, -1, v * x, v * y, v;
    }
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeFullV);
    Eigen::Matrix<double, 9, 1> h = svd.matrixV().col(8);
    Eigen::Matrix3d Hn;
    Hn << h(0), h(1), h(2), h(3), h(4), h(5), h(6), h(7), h(8);
    // Denormalize: H = T2^-1 * Hn * T1.
    Eigen::Matrix3d T1 = Eigen::Matrix3d::Identity(), T2i = Eigen::Matrix3d::Identity();
    T1(0,0) = s1; T1(1,1) = s1; T1(0,2) = -s1 * cx1; T1(1,2) = -s1 * cy1;
    T2i(0,0) = 1.0 / s2; T2i(1,1) = 1.0 / s2; T2i(0,2) = cx2; T2i(1,2) = cy2;
    Eigen::Matrix3d Hf = T2i * Hn * T1;
    if (std::abs(Hf(2,2)) < 1e-14) return false;
    Hf /= Hf(2,2);
    *H = Hf;
    return true;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

// Verbose solve diagnostics. Define CAMERATRACKER_DEBUG (e.g. from the build)
// to stream the tracker's internal progress to stderr; silent otherwise.
#ifdef CAMERATRACKER_DEBUG
#define CT_DBG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
#else
#define CT_DBG(...) do {} while (0)
#endif

NATRON_NAMESPACE_ENTER

// ==================== Private Implementation ====================

struct CameraTrackerNodePrivate
{
    CameraTrackerNode* publicInterface;

    // --- Camera Settings tab ---
    KnobDoubleWPtr focalLengthMm;
    KnobDoubleWPtr sensorWidth;
    KnobButtonWPtr estimateFocalBtn;
    KnobButtonWPtr readExifBtn;
    KnobButtonWPtr createUndistortBtn;
    KnobButtonWPtr createRedistortBtn;
    KnobDoubleWPtr principalPointX;
    KnobDoubleWPtr principalPointY;

    // --- Lens Distortion tab ---
    KnobChoiceWPtr distortionModel;
    KnobDoubleWPtr k1, k2, k3;
    KnobBoolWPtr refineLensDistortion;

    // --- Tracking tab ---
    KnobChoiceWPtr detectorType;
    KnobChoiceWPtr featureScale;
    KnobIntWPtr maxFeatures;
    KnobIntWPtr minFeatureDistance;
    KnobDoubleWPtr detectSensitivity;
    KnobDoubleWPtr roiX1, roiY1, roiX2, roiY2;
    KnobBoolWPtr useROI;
    KnobBoolWPtr addTrackMode;
    KnobIntWPtr trackRangeStart;
    KnobIntWPtr trackRangeEnd;
    KnobButtonWPtr detectFeaturesBtn;
    KnobButtonWPtr trackFeaturesBtn;
    KnobButtonWPtr clearTracksBtn;
    KnobDoubleWPtr deleteErrorThreshold;
    KnobButtonWPtr deleteBadBtn;
    KnobButtonWPtr deleteRegionBtn;
    KnobBoolWPtr normalizeContrast;    // local-contrast normalization for detection (low-contrast footage)
    KnobBoolWPtr addPlanarMode;        // drag quads in the viewer to add planar regions
    KnobIntWPtr planarGridSize;        // NxN virtual markers baked per planar quad
    KnobChoiceWPtr planarMotionModel;  // DoF restriction (planar stability lever)
    KnobButtonWPtr trackPlanarsBtn;
    KnobButtonWPtr createCardsFromPlanarBtn;
    KnobButtonWPtr clearPlanarsBtn;
    KnobButtonWPtr deleteManualBtn;    // delete the selected manual track
    KnobButtonWPtr clearManualBtn;     // remove all manual tracks
    KnobChoiceWPtr manualSelector;     // panel-side selection (synced with viewer clicks)
    KnobGroupWPtr grpManualKnob;       // the Manual Tracks group (table follows its open state)
    KnobButtonWPtr trackManualBtn;     // track only the hand-placed tracks
    KnobButtonWPtr trackManualBwdBtn;  // same, tracking BACKWARD (mid-clip placement)
    KnobIntWPtr manualPatchSize;       // pattern box size (px) for manual tracks
    KnobBoolWPtr solveManualOnly;      // solve using only hand-placed tracks
    std::vector<int> manualChoiceIds;  // dropdown index -> track id
    bool manualSelectorGuard = false;  // prevents refresh<->knobChanged recursion
    KnobBoolWPtr redetectEnabled;      // re-detect new features during tracking (continuous replenishment)
    KnobDoubleWPtr redetectMinRatio;   // re-detect when live tracks fall below this fraction of the initial count
    KnobFileWPtr importTracksFile;     // path to a 2D-track .txt to import (validation)
    KnobButtonWPtr importTracksBtn;
    KnobOutputFileWPtr exportTracksFile;  // path to write our 2D tracks (plain .txt)
    KnobButtonWPtr exportTracksBtn;
    KnobOutputFileWPtr exportCameraFile;  // path to write our solved camera (per-frame text)
    KnobButtonWPtr exportCameraBtn;

    // --- Solving tab ---
    KnobChoiceWPtr solveMode;
    KnobBoolWPtr rejectMovingObjects;   // global multi-pair dominant-motion rejection (no mask needed)
    KnobBoolWPtr fixPathSpikes;         // post-solve velocity-outlier lerp (last-resort; default off)
    KnobDoubleWPtr pathSmoothness;      // trajectory-smoothness prior weight in the bundle (0 = off)
    KnobBoolWPtr refineFocalLength;
    KnobBoolWPtr refinePrincipalPoint;
    KnobDoubleWPtr maxTrackError;
    KnobIntWPtr keyframe1;
    KnobIntWPtr keyframe2;
    KnobBoolWPtr autoKeyframes;
    KnobButtonWPtr solveBtn;
    KnobButtonWPtr refineBtn;
    KnobDoubleWPtr solveErrorDisplay;
    KnobIntWPtr numCamerasDisplay;
    KnobIntWPtr numPointsDisplay;
    KnobStringWPtr solveStatusDisplay;

    // --- Output tab ---
    KnobButtonWPtr setOriginBtn;      // scene orientation from viewport selection
    KnobButtonWPtr setGroundBtn;
    KnobDoubleWPtr scaleDistance;     // known real-world distance between 2 selected points
    KnobButtonWPtr setScaleBtn;
    KnobButtonWPtr clearOrientBtn;
    KnobButtonWPtr createCardBtn;     // Card3D at selection centroid, normal-aligned
    KnobButtonWPtr createCameraBtn;
    KnobButtonWPtr exportPointCloudBtn;
    KnobOutputFileWPtr pointCloudFile;
    KnobButtonWPtr exportReportBtn;
    KnobOutputFileWPtr solveReportFile;

    // --- Internal state ---
    struct Track2D {
        int id;
        std::map<int, std::pair<double, double>> markers; // frame -> (x, y)
        double error = -1.0; // mean reprojection error (px) after solve; -1 = unsolved, -2 = rejected outlier
        double patternHalf = -1.0; // manual tracks: per-track pattern half-size (px),
                                   // set by dragging the box corners; -1 = Pattern Size knob
    };
    std::vector<Track2D> tracks;

    struct SolvedCamera {
        int frame;
        double tx, ty, tz;
        double rx, ry, rz; // Euler XYZ in degrees
        double R[3][3];    // world->camera rotation matrix (as extracted, before Euler decompose)
        double residual;   // average reprojection error (px) for this frame
    };
    std::vector<SolvedCamera> solvedCameras;

    struct SolvedPoint {
        int track;
        double x, y, z;
    };
    std::vector<SolvedPoint> solvedPoints;
    double lastSolveError;
    bool hasSolution;
    bool tracksImported = false;  // current tracks came from an external import (curated
                                  // data -> relax the short-track floor in the solve)

    // --- Manual track management ---
    std::set<int> manualTrackIds;    // Track2D ids placed by hand (Add Track mode)
    int selectedManualId = -1;       // currently selected manual track (-1 = none)
    bool manualDragActive = false;   // dragging the selected track's marker
    bool patternResizeActive = false; // dragging a corner of the selected track's pattern box

    // Effective pattern half-size (px) for a manual track: its own override if
    // the box was resized, else the Pattern Size knob.
    double patternHalfFor(int trackId) const
    {
        for (const auto& t : tracks) {
            if (t.id == trackId) {
                if (t.patternHalf > 0) return t.patternHalf;
                break;
            }
        }
        KnobIntPtr pk = manualPatchSize.lock();
        return pk ? std::max(4.0, pk->getValue() / 2.0) : 20.0;
    }

    // --- Drag magnifier (Tracker-node pattern, TrackerNodeInteract: render an
    // ROI around the marker -> sRGB byte GL texture -> zoomed quad drawn next
    // to the cursor while dragging, so sub-pixel placement is possible) ---
    GLTexturePtr magTexture;
    RectI magTextureRoI;             // pixel RoI the texture covers
    int magTextureFrame = -1;        // frame the texture was rendered at
    unsigned int magPboID = 0;       // PBO for texture upload
    bool magVisible = false;

    // Render the input around (cx, cy) at `frame` and upload it as the
    // magnifier texture. Returns false on any failure (no input, render
    // failed, no GL viewer) — the magnifier then simply stays hidden.
    bool refreshMagnifierTexture(int frame, double cx, double cy);
    void uploadMagnifierTexture(const ImagePtr& image, const RectI& roi);

    // Refresh the read-only manual-track list in the panel (id, frames,
    // position, error, selection). Cheap; call after any manual-track change.
    void refreshManualList()
    {
        // Selection dropdown (panel-side selection, synced with viewer clicks)
        manualChoiceIds.clear();
        if (KnobChoicePtr ck = manualSelector.lock()) {
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("none", "(none)", ""));
            manualChoiceIds.push_back(-1);
            int selIndex = 0;
            for (const auto& t : tracks) {
                if (!manualTrackIds.count(t.id)) continue;
                std::stringstream nm;
                nm << "track " << t.id << " (" << t.markers.size() << " fr)";
                entries.push_back(ChoiceOption(nm.str(), nm.str(), ""));
                manualChoiceIds.push_back(t.id);
                if (t.id == selectedManualId) selIndex = (int)manualChoiceIds.size() - 1;
            }
            manualSelectorGuard = true;
            ck->populateChoices(entries);
            ck->setValue(selIndex);
            manualSelectorGuard = false;
        }

        publicInterface->notifyManualTracksChanged();   // the Gui table listens
    }

    // --- Viewport point selection + scene orientation ---
    std::vector<int> viewportSelection;   // indices into solvedPoints, pushed by Viewport3D
    // 2D-viewer rubber-band selection state (post-solve; canonical coords)
    bool selDragActive = false;
    double selDragX0 = 0, selDragY0 = 0, selDragX1 = 0, selDragY1 = 0;

    // --- Planar quad tracks (user-drawn regions tracked with a homography) ---
    struct PlanarTrack {
        int id;
        // frame -> 4 corners (Natron coords, x0,y0..x3,y3; order BL,BR,TR,TL as drawn)
        std::map<int, std::array<double, 8> > quads;
        int refFrame = -1;          // frame the user drew the quad on
        std::set<int> userKeys;     // user-authored frames (drawn or corrected);
                                    // re-tracking propagates BETWEEN these keys
        bool tracked = false;
        std::vector<int> bakedIds;  // Track2D ids synthesized from this quad's grid
    };
    std::vector<PlanarTrack> planarTracks;
    // Interaction state (Add Planar mode)
    bool planarCreating = false;   // dragging out a new quad
    int planarEditQuad = -1;       // quad index being corner-edited
    int planarEditCorner = -1;     // corner index being dragged
    int planarEditFrame = -1;      // frame whose quad is being corner-edited
    double planarStartX = 0, planarStartY = 0;
    // User scene orientation (Set Origin / Set Ground Plane): output space is
    // scale * sceneRot * (X - sceneOrigin), applied IDENTICALLY to cameras,
    // cloud and exports so everything stays registered. Identity by default.
    double sceneRot[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    double sceneOrigin[3] = {0, 0, 0};
    bool sceneOrientSet = false;
    // Real-world scale: output units per solve unit, from Set Scale From
    // Selection (two points + known distance). 0 = automatic gauge (~10 units).
    double sceneScale = 0.0;
    PointCloudDataPtr viewportCloud;   // cached cloud for the 3D viewport (rebuilt only when the solve changes)
    bool cloudDirty;

    CameraTrackerNodePrivate(CameraTrackerNode* pub)
        : publicInterface(pub)
        , lastSolveError(0)
        , hasSolution(false)
        , cloudDirty(true)
    {
    }

    // Output gauge: out = scale * sceneRot * (X - origin).
    // Origin is the first solved camera (or the user's Set-Origin selection),
    // rotation is identity (or the user's Set-Ground-Plane fit), scale puts the
    // largest camera displacement at ~10 viewport units. The SAME transform
    // MUST be applied to cameras, points and exports, or geometry and camera
    // drift out of registration.
    void getOutputNormalization(double& ox, double& oy, double& oz, double& scale) const
    {
        ox = oy = oz = 0.0;
        scale = 1.0;
        if (solvedCameras.empty()) {
            return;
        }
        if (sceneOrientSet) {
            ox = sceneOrigin[0]; oy = sceneOrigin[1]; oz = sceneOrigin[2];
        } else {
            ox = solvedCameras[0].tx;
            oy = solvedCameras[0].ty;
            oz = solvedCameras[0].tz;
        }
        if (sceneScale > 0.0) {
            scale = sceneScale;   // user-set real-world scale wins
            return;
        }
        double maxDisp = 1.0;
        for (const auto& cam : solvedCameras) {
            double dx = cam.tx - ox, dy = cam.ty - oy, dz = cam.tz - oz;
            double disp = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (disp > maxDisp) {
                maxDisp = disp;
            }
        }
        scale = (maxDisp > 1e-6) ? 10.0 / maxDisp : 1.0;
    }

    // Apply the full output transform (rotation included) to a solve-space
    // position. Rotation is isometric so the scale above is unaffected.
    void applyOutputTransform(double x, double y, double z,
                              double ox, double oy, double oz, double scale,
                              double& outX, double& outY, double& outZ) const
    {
        const double dx = x - ox, dy = y - oy, dz = z - oz;
        outX = scale * (sceneRot[0][0]*dx + sceneRot[0][1]*dy + sceneRot[0][2]*dz);
        outY = scale * (sceneRot[1][0]*dx + sceneRot[1][1]*dy + sceneRot[1][2]*dz);
        outZ = scale * (sceneRot[2][0]*dx + sceneRot[2][1]*dy + sceneRot[2][2]*dz);
    }

    // Estimate the focal length from the footage itself (Mendonça-Cipolla
    // self-calibration over a wide-baseline keyframe pair). Writes the focal
    // knob; reports confidence via the status line.
    void estimateFocalFromFootage();
    // Read focal length (and sensor width via the 35mm-equivalent tag) from
    // the input footage's EXIF metadata (JPEG APP1 / TIFF IFD).
    void readFocalFromExif();
    // Create a LensWarp node in the parent graph preloaded with this node's
    // lens model (focal/sensor/pp/k1-k3) — Undistort for the plate branch,
    // Redistort for the end of the comp.
    void createLensWarpNode(bool undistort);
    // Scene-orientation actions (buttons; use the viewport selection)
    void setOriginFromSelection();
    void setGroundPlaneFromSelection();
    void setScaleFromSelection();
    void clearSceneOrientation();
    // Create a Card3D at the selection centroid, oriented to the fitted normal
    // (slip-test / geo-placement helper).
    void createCardAtSelection();

    // Pull the connected clip's frame range into the Frame Range Start/End knobs.
    // Called on input connect (not during project load, so saved ranges survive).
    void refreshFrameRangeFromInput();
    void deleteBadTracks();
    void deleteTracksInRegion(int frame);
    // Import 2D tracks from a text export (track_id view x y) into `tracks`,
    // so our solver can be run on externally-curated clean tracks — isolates whether the
    // remaining quality gap is our tracker or our solver.
    void importTracks2D(const std::string& path);
    // Write our 2D tracks in the same text format as the import (track_id view x y,
    // top-left/Y-down pixels), so ours can be diffed against an external ground truth.
    void exportTracks(const std::string& path);
    // Write our solved camera per-frame as text (R 9, t 3, focal mm),
    // to diff our camera trajectory against an external ground-truth camera.
    void exportCameraTrajectory(const std::string& path);
    void detectFeatures(int frame);
    // Detect fresh features on an already-rendered top-down libmv frame and append
    // them as new tracks starting at `frame`, skipping regions that already hold a
    // live track. Used by trackFeatures() to replenish coverage as the camera pans
    // Returns the number of tracks added.
    int redetectFeatures(const libmv::FloatImage& img, int frame,
                         int imgW, int imgH, double rodX1, double rodY2);
    // manualOnly: track ONLY hand-placed tracks — no auto-detection, no
    // re-detection replenishment (the explicit "Track Manual" button).
    void trackFeatures(bool manualOnly = false, bool backwards = false);
    // Track every user-drawn planar quad across the frame range with a
    // homography warp, then bake an NxN grid of virtual markers per quad into
    // `tracks` (they join the solve as regular, premium-quality markers).
    void trackPlanarRegions();
    // After a solve: create a Card3D per tracked planar quad, fitted to the
    // quad grid's 3D points (position, orientation, size) in output gauge.
    void createCardsFromPlanars();
    void solveCameraMotion();
    void createCamera3DNode();
    void exportPointCloud();
    void exportSolveReport();
};


// ==================== Constructor / Destructor ====================

CameraTrackerNode::CameraTrackerNode(NodePtr node)
    : NodeGroup(node)
    , _imp(new CameraTrackerNodePrivate(this))
{
}

CameraTrackerNode::~CameraTrackerNode()
{
}

std::string
CameraTrackerNode::getPluginID() const
{
    return PLUGINID_NATRON_CAMERATRACKER;
}

std::string
CameraTrackerNode::getPluginLabel() const
{
    return "CameraTracker";
}

std::string
CameraTrackerNode::getPluginDescription() const
{
    return "3D Camera Tracker\n"
           "=================\n\n"
           "Solve 3D camera motion from 2D feature tracks using libmv.\n\n"
           "**Workflow:**\n"
           "1. Connect footage to input\n"
           "2. Set camera focal length (or leave as estimate)\n"
           "3. Click **Detect Features** to find trackable points\n"
           "4. Click **Track Features** to track them across the frame range\n"
           "5. Click **Solve** to compute 3D camera motion\n"
           "6. Click **Create Camera3D** to output a solved camera node\n\n"
           "**Solve Modes:**\n"
           "- **Camera** — Full 6DOF camera solve (translation + rotation)\n"
           "- **Tripod** — Rotation-only solve for locked-off/nodal cameras\n\n"
           "Based on libmv from the Blender open-source software.";
}


// ==================== Knob Creation ====================

void
CameraTrackerNode::initializeKnobs()
{
    // ========== Camera Settings ==========
    KnobPagePtr cameraPage = AppManager::createKnob<KnobPage>(this, tr("Camera"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Focal Length (mm)"));
        k->setName("focalLengthMm");
        k->setHintToolTip(tr("Camera focal length in millimeters.\n"
                              "Common values: 18mm (wide), 35mm (normal), 50mm (standard), 85mm (portrait).\n"
                              "Phone cameras are typically 24-28mm equivalent.\n"
                              "If unknown, try 35mm and enable 'Refine Focal Length' in the Solve tab."));
        k->setDefaultValue(35.0);
        k->setAnimationEnabled(false);
        k->setMinimum(1.0);
        k->setDisplayMinimum(10.0);
        k->setDisplayMaximum(200.0);
        cameraPage->addKnob(k);
        _imp->focalLengthMm = k; // mm; converted to pixels inside solveCameraMotion()
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Sensor Width (mm)"));
        k->setName("sensorWidth");
        k->setHintToolTip(tr("Camera sensor width in millimeters. Used for focal length conversion.\n"
                              "Common values: Super35 = 24.576, Full Frame = 36.0, APS-C = 23.6"));
        k->setDefaultValue(24.576);
        k->setAnimationEnabled(false);
        k->setMinimum(1.0);
        k->setDisplayMinimum(5.0);
        k->setDisplayMaximum(70.0);
        cameraPage->addKnob(k);
        _imp->sensorWidth = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Estimate Focal From Footage"));
        k->setName("estimateFocal");
        k->setHintToolTip(tr("Self-calibrate the focal length from the tracked footage (needs "
                             "tracks: press Detect + Track first). Picks a wide-baseline frame "
                             "pair and finds the focal whose epipolar geometry best satisfies "
                             "the essential-matrix constraint. Writes the Focal Length knob "
                             "(interpreted against the current Sensor Width). Reports a warning "
                             "when the shot has too little parallax to constrain focal (nodal "
                             "pans) — enter a known value instead in that case."));
        k->setEvaluateOnChange(false);
        cameraPage->addKnob(k);
        _imp->estimateFocalBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Read Focal From EXIF"));
        k->setName("readExifFocal");
        k->setHintToolTip(tr("Read the focal length from the input footage's EXIF metadata "
                             "(JPEG/TIFF stills from cameras carry it; converted or stock "
                             "footage usually does not). When the 35mm-equivalent focal tag "
                             "is present, the Sensor Width is derived from it too — the "
                             "exact values, no estimation. Falls back to the focal-plane "
                             "resolution tags for sensor width when available."));
        k->setEvaluateOnChange(false);
        cameraPage->addKnob(k);
        _imp->readExifBtn = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Principal Point X"));
        k->setName("principalPointX");
        k->setHintToolTip(tr("X coordinate of the principal point (optical center) in pixels.\n"
                              "Typically image_width / 2. Set to 0 for centered."));
        k->setDefaultValue(0.0);
        k->setAnimationEnabled(false);
        cameraPage->addKnob(k);
        _imp->principalPointX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Principal Point Y"));
        k->setName("principalPointY");
        k->setHintToolTip(tr("Y coordinate of the principal point (optical center) in pixels.\n"
                              "Typically image_height / 2. Set to 0 for centered."));
        k->setDefaultValue(0.0);
        k->setAnimationEnabled(false);
        cameraPage->addKnob(k);
        _imp->principalPointY = k;
    }

    // ========== Lens Distortion ==========
    KnobPagePtr lensPage = AppManager::createKnob<KnobPage>(this, tr("Lens"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Distortion Model"));
        k->setName("distortionModel");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("none", "None", "No lens distortion correction"));
        entries.push_back(ChoiceOption("polynomial", "Polynomial", "Polynomial radial distortion (K1, K2, K3)"));
        k->populateChoices(entries);
        k->setDefaultValue(1); // Polynomial — model distortion by default (real lenses have it)
        lensPage->addKnob(k);
        _imp->distortionModel = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("K1"));
        k->setName("k1"); k->setDefaultValue(0.0); k->setAnimationEnabled(false);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("First radial distortion coefficient"));
        lensPage->addKnob(k); _imp->k1 = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("K2"));
        k->setName("k2"); k->setDefaultValue(0.0); k->setAnimationEnabled(false);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Second radial distortion coefficient"));
        lensPage->addKnob(k); _imp->k2 = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("K3"));
        k->setName("k3"); k->setDefaultValue(0.0); k->setAnimationEnabled(false);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Third radial distortion coefficient"));
        lensPage->addKnob(k); _imp->k3 = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Refine Lens Distortion"));
        k->setName("refineLensDistortion");
        k->setDefaultValue(true); // solve K1/K2 by default (standard matchmove behaviour). K3 stays manual (libmv refines K1/K2 only).
        k->setHintToolTip(tr("Solve K1/K2 radial distortion during bundle adjustment. Solved values "
                             "are written back to the K1/K2 knobs. (K3 is manual — libmv refines K1/K2 only.)"));
        lensPage->addKnob(k);
        _imp->refineLensDistortion = k;
    }

    // ========== Tracking ==========
    // Organized into task groups (2026-07 UI pass): frame range on top, then
    // Auto Detect / Manual / Planar / Cleanup / Import-Export groups, status at
    // the bottom. Knob NAMES are unchanged — old projects load fine.
    KnobPagePtr trackPage = AppManager::createKnob<KnobPage>(this, tr("Tracking"));

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Range Start"));
        k->setName("trackRangeStart");
        k->setDefaultValue(1);
        k->setAddNewLine(false);   // start/end on one row
        k->setHintToolTip(tr("First frame of the tracking range. Auto-filled from the "
                             "connected clip; edit to track a sub-range."));
        trackPage->addKnob(k);
        _imp->trackRangeStart = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("End"));
        k->setName("trackRangeEnd");
        k->setDefaultValue(100);
        k->setHintToolTip(tr("Last frame of the tracking range. Auto-filled from the "
                             "connected clip; edit to track a sub-range."));
        trackPage->addKnob(k);
        _imp->trackRangeEnd = k;
    }

    // Section separators give each group a visually distinct block (a true
    // boxed background needs a Gui-side group-widget rework — Phase 2).
    auto addSeparator = [this](KnobPagePtr page, const char* name) {
        KnobSeparatorPtr s = AppManager::createKnob<KnobSeparator>(this, QString::fromUtf8(""));
        s->setName(name);
        page->addKnob(s);
    };

    addSeparator(trackPage, "sepAutoDetect");
    KnobGroupPtr grpAutoDetect = AppManager::createKnob<KnobGroup>(this, tr("Auto Detect"));
    grpAutoDetect->setName("grpAutoDetect");
    grpAutoDetect->setDefaultValue(true);   // expanded
    grpAutoDetect->setHintToolTip(tr("Automatic feature detection & tracking"));
    trackPage->addKnob(grpAutoDetect);

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Detector"));
        k->setName("detectorType");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("harris", "Harris", "Harris corner detector — good general-purpose detector"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        grpAutoDetect->addKnob(k);
        _imp->detectorType = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Feature Scale"));
        k->setName("featureScale");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("normal", "Normal", "Detect at full resolution (fine corners)."));
        entries.push_back(ChoiceOption("large",  "Large",  "Detect on a downscaled image — coarse features in soft/distant texture (hills, foliage)."));
        entries.push_back(ChoiceOption("both",   "Both",   "Multi-scale: full + half + quarter resolution, merged. Best background coverage."));
        k->populateChoices(entries);
        k->setDefaultValue(2); // Both — best distribution incl. distant terrain
        k->setHintToolTip(tr("Detection scale. Harris finds only sharp corners at full res; coarser "
                             "scales surface structure in hazy/distant terrain that full-res misses."));
        grpAutoDetect->addKnob(k);
        _imp->featureScale = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Features"));
        k->setName("maxFeatures");
        k->setDefaultValue(1200);   // production-scale density: validation showed our solver
                                    // handles ~2800 tracks cleanly, and density (not the
                                    // solver) was the gap vs the reference solve. Was 400.
        k->setMinimum(10); k->setMaximum(8000);
        k->setDisplayMinimum(10); k->setDisplayMaximum(3000);
        k->setHintToolTip(tr("Maximum number of features to detect per frame. Higher = denser, more "
                             "robust solve (production trackers run thousands), but slower to track and solve."));
        grpAutoDetect->addKnob(k);
        _imp->maxFeatures = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Min Distance"));
        k->setName("minFeatureDistance");
        k->setDefaultValue(10);
        k->setMinimum(1); k->setMaximum(200);
        k->setHintToolTip(tr("Minimum distance in pixels between detected features"));
        grpAutoDetect->addKnob(k);
        _imp->minFeatureDistance = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Detection Sensitivity"));
        k->setName("detectSensitivity");
        k->setDefaultValue(60.0);
        k->setMinimum(0.0); k->setMaximum(100.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Higher = more features (lower Harris threshold). 0-100 scale; maps "
                             "logarithmically to the detector threshold so you don't hand-tune tiny "
                             "values. ~60 ≈ the old 1e-6 default."));
        grpAutoDetect->addKnob(k);
        _imp->detectSensitivity = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Use Detection Region"));
        k->setName("useROI");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Only detect features inside the specified region.\n"
                              "Use this to exclude sky, trees, or other areas that won't track well."));
        grpAutoDetect->addKnob(k);
        _imp->useROI = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Region X1"));
        k->setName("roiX1"); k->setDefaultValue(0.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Left edge of detection region (pixels)"));
        k->setSecretByDefault(true);   // shown only while Use Detection Region is on
        grpAutoDetect->addKnob(k); _imp->roiX1 = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Region Y1"));
        k->setName("roiY1"); k->setDefaultValue(0.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Bottom edge of detection region (pixels)"));
        k->setSecretByDefault(true);
        grpAutoDetect->addKnob(k); _imp->roiY1 = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Region X2"));
        k->setName("roiX2"); k->setDefaultValue(1920.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Right edge of detection region (pixels)"));
        k->setSecretByDefault(true);
        grpAutoDetect->addKnob(k); _imp->roiX2 = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Region Y2"));
        k->setName("roiY2"); k->setDefaultValue(540.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Top edge of detection region (pixels). Set to half image height to exclude sky."));
        k->setSecretByDefault(true);
        grpAutoDetect->addKnob(k); _imp->roiY2 = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Boost Coverage"));
        k->setName("normalizeContrast");
        k->setDefaultValue(false);  // OFF = the proven elite detection (fixed threshold,
                                    // strong corners only — best measured camera accuracy).
        k->setHintToolTip(tr("Dig for extra features on low-contrast footage: adaptively lowers "
                             "the detection threshold and adds contrast-normalized filler "
                             "candidates in regions with no strong corners. Extra tracks are "
                             "down-weighted in the solve, but are less precise — expect a denser "
                             "cloud (better geo line-up) at some cost in solve error. OFF = "
                             "strong corners only (most accurate camera). Planar regions are "
                             "usually the better densification tool."));
        grpAutoDetect->addKnob(k);
        _imp->normalizeContrast = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Re-detect New Features"));
        k->setName("redetectEnabled");
        k->setDefaultValue(true);   // keep the frame populated as the camera moves
        k->setAddNewLine(false);
        k->setHintToolTip(tr("During tracking, automatically detect fresh features on later frames "
                             "when existing tracks drain out of view (e.g. a long pan). New features "
                             "are seeded only in areas that have lost coverage, building a web of "
                             "overlapping tracks that span the whole shot — how production trackers keep a moving "
                             "camera populated. Turn off for detect-once behaviour."));
        grpAutoDetect->addKnob(k); _imp->redetectEnabled = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Below Track %"));
        k->setName("redetectMinRatio"); k->setDefaultValue(0.8); k->setAnimationEnabled(false);
        k->setMinimum(0.1); k->setMaximum(1.0);
        k->setHintToolTip(tr("Re-detect trigger. When the number of tracks still alive on a frame "
                             "drops below this fraction of the initial feature count, new features are "
                             "detected to top the coverage back up. Lower = replenish less often."));
        grpAutoDetect->addKnob(k); _imp->redetectMinRatio = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Detect Features"));
        k->setName("detectFeatures");
        k->setHintToolTip(tr("Detect trackable features (auto-runs at the range start when you "
                             "press Track with no features there)"));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);   // detect / track / clear on one row
        grpAutoDetect->addKnob(k);
        _imp->detectFeaturesBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Track Features"));
        k->setName("trackFeatures");
        k->setHintToolTip(tr("Track detected features across the frame range (predictive tracker: "
                             "global-motion seed + per-track velocity + local refinement)."));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);
        grpAutoDetect->addKnob(k);
        _imp->trackFeaturesBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Clear Tracks"));
        k->setName("clearTracks");
        k->setHintToolTip(tr("Remove all detected features and tracks"));
        k->setEvaluateOnChange(false);
        grpAutoDetect->addKnob(k);
        _imp->clearTracksBtn = k;
    }

    // --- Manual tracks (between Auto Detect and Planar, as requested; the
    // settings panel INSERTS the track table right after this group's rows,
    // and the table shows/hides with the group's expand state) ---
    addSeparator(trackPage, "sepManual");
    KnobGroupPtr grpManual = AppManager::createKnob<KnobGroup>(this, tr("Manual Tracks"));
    grpManual->setName("grpManual");
    grpManual->setDefaultValue(false);   // collapsed — occasional workflow
    grpManual->setHintToolTip(tr("Hand-placed point tracks — solved together with detected features"));
    trackPage->addKnob(grpManual);
    _imp->grpManualKnob = grpManual;
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Add Track Mode"));
        k->setName("addTrackMode");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("When enabled, work with manual tracks in the viewer:\n"
                             "• click empty space — place a new manual track\n"
                             "• click an existing manual track — select it (cyan)\n"
                             "• drag a manual track — reposition its marker on this frame\n"
                             "Manual tracks are tracked and solved together with detected features."));
        grpManual->addKnob(k);
        _imp->addTrackMode = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Selected"));
        k->setName("manualSelector");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("none", "(none)", ""));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);
        k->setHintToolTip(tr("Select a manual track from the panel (synced with clicking one in "
                             "the viewer). Delete Selected Track acts on this."));
        grpManual->addKnob(k);
        _imp->manualSelector = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Pattern Size"));
        k->setName("manualPatchSize");
        k->setDefaultValue(40);
        k->setMinimum(8);
        k->setDisplayMaximum(200);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Pattern box size (pixels) used when TRACKING manual tracks — drawn "
                             "around each manual track in the viewer. Bigger = more texture for "
                             "the matcher (good on soft detail), smaller = tighter lock on a "
                             "crisp corner."));
        grpManual->addKnob(k);
        _imp->manualPatchSize = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Track Manual"));
        k->setName("trackManual");
        k->setHintToolTip(tr("Track ONLY the hand-placed tracks across the frame range — no "
                             "auto-detection, no replenishment. Your placed/dragged frames are "
                             "keyframes the tracker fills between but never overwrites."));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);
        grpManual->addKnob(k);
        _imp->trackManualBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Track Manual Bwd"));
        k->setName("trackManualBwd");
        k->setHintToolTip(tr("Track the hand-placed tracks BACKWARD from the end of the "
                             "range toward the start — for manual tracks placed mid-clip, "
                             "run Track Manual for the frames after the placement and this "
                             "for the frames before it. Placed/dragged frames stay protected "
                             "keyframes in both directions."));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);
        grpManual->addKnob(k);
        _imp->trackManualBwdBtn = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Solve Manual Only"));
        k->setName("solveManualOnly");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Solve using ONLY hand-placed tracks (detected tracks are ignored). "
                             "Needs at least 8 manual tracks sharing frames — the two-view "
                             "initialization minimum."));
        grpManual->addKnob(k);
        _imp->solveManualOnly = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Delete Selected Track"));
        k->setName("deleteManualTrack");
        k->setHintToolTip(tr("Delete the manual track currently selected in the viewer "
                             "(click one in Add Track Mode to select it)."));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);
        grpManual->addKnob(k);
        _imp->deleteManualBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Remove All Manual"));
        k->setName("clearManualTracks");
        k->setHintToolTip(tr("Delete every hand-placed track (detected tracks are untouched)."));
        k->setEvaluateOnChange(false);
        grpManual->addKnob(k);
        _imp->clearManualBtn = k;
    }
    {
        // The track TABLE, nested in this group as a real knob (custom knob
        // type; its Gui hosts the table widget). Non-persistent view — the
        // node's track data is the storage.
        KnobTracksTablePtr k = AppManager::createKnob<KnobTracksTable>(this, tr("Tracks"));
        k->setName("manualTracksTable");
        k->setEvaluateOnChange(false);
        k->setIsPersistent(false);
        grpManual->addKnob(k);
    }

    // --- Planar regions ---
    addSeparator(trackPage, "sepPlanar");
    KnobGroupPtr grpPlanar = AppManager::createKnob<KnobGroup>(this, tr("Planar Regions"));
    grpPlanar->setName("grpPlanar");
    grpPlanar->setDefaultValue(false);   // collapsed — like the other sections
    grpPlanar->setHintToolTip(tr("Region / surface tracking: draw quads on flat features; each is "
                                 "homography-tracked and baked into a grid of premium markers"));
    trackPage->addKnob(grpPlanar);
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Add Planar Mode"));
        k->setName("addPlanarMode");
        k->setDefaultValue(false);
        k->setAddNewLine(false);
        k->setHintToolTip(tr("When enabled, drag a box in the viewer around a FLAT rectangular "
                             "feature (a panel face, window, sign); adjust corners by dragging "
                             "them — on any frame after tracking too (a corrected frame becomes a "
                             "key, and re-tracking propagates between keys). Then press "
                             "'Track Planar Regions'."));
        grpPlanar->addKnob(k);
        _imp->addPlanarMode = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Grid"));
        k->setName("planarGridSize");
        k->setDefaultValue(5);
        k->setMinimum(2);
        k->setDisplayMaximum(9);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Each tracked planar quad is baked into an NxN grid of virtual "
                             "markers (5 = 25 markers per quad)."));
        grpPlanar->addKnob(k);
        _imp->planarGridSize = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Motion Model"));
        k->setName("planarMotionModel");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Translation", "Translation", ""));
        entries.push_back(ChoiceOption("Trans+Rotation", "Trans+Rotation", ""));
        entries.push_back(ChoiceOption("Trans+Scale", "Trans+Scale", ""));
        entries.push_back(ChoiceOption("Trans+Rot+Scale", "Trans+Rot+Scale", ""));
        entries.push_back(ChoiceOption("Affine", "Affine", ""));
        entries.push_back(ChoiceOption("Perspective", "Perspective", ""));
        k->populateChoices(entries);
        k->setDefaultValue(5);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Degrees of freedom the planar tracker may use. "
                             "Full Perspective is correct physics but on low-texture or "
                             "low-perspective content the unconstrained corners 'breathe' — "
                             "restricting to Trans+Rot+Scale or Affine is the classic "
                             "stability lever. Applies to all planar regions."));
        grpPlanar->addKnob(k);
        _imp->planarMotionModel = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Track Planar Regions"));
        k->setName("trackPlanars");
        k->setHintToolTip(tr("Track every drawn planar quad across the frame range (anchored to "
                             "the drawn frame — drift-free; corrected frames become keys and "
                             "re-tracking propagates between them) and bake the marker grids."));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);
        grpPlanar->addKnob(k);
        _imp->trackPlanarsBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Create Cards From Planars"));
        k->setName("createCardsFromPlanars");
        k->setHintToolTip(tr("After solving: create a Card3D per tracked planar quad, positioned, "
                             "oriented and sized to the quad's reconstructed 3D rectangle — geometry "
                             "that lands exactly on the tracked surface."));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);
        grpPlanar->addKnob(k);
        _imp->createCardsFromPlanarBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Clear Planar Regions"));
        k->setName("clearPlanars");
        k->setHintToolTip(tr("Delete all planar quads and their baked markers."));
        k->setEvaluateOnChange(false);
        grpPlanar->addKnob(k);
        _imp->clearPlanarsBtn = k;
    }
    // --- Cleanup (post-solve track surgery) ---
    addSeparator(trackPage, "sepCleanup");
    KnobGroupPtr grpCleanup = AppManager::createKnob<KnobGroup>(this, tr("Cleanup"));
    grpCleanup->setName("grpCleanup");
    grpCleanup->setDefaultValue(false);
    grpCleanup->setHintToolTip(tr("Post-solve track cleanup: inspect (colored crosses) → delete → Refine Solve"));
    trackPage->addKnob(grpCleanup);
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Delete Above Error (px)"));
        k->setName("deleteErrorThreshold"); k->setDefaultValue(3.0); k->setAnimationEnabled(false);
        k->setMinimum(0.0); k->setDisplayMaximum(20.0);
        k->setHintToolTip(tr("Threshold for 'Delete Bad Tracks'. After a solve, tracks whose mean "
                             "reprojection error (red/orange in the viewer) exceeds this are removed."));
        grpCleanup->addKnob(k); _imp->deleteErrorThreshold = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Delete Bad Tracks"));
        k->setName("deleteBad");
        k->setHintToolTip(tr("Remove tracks above the error threshold (and rejected outliers), then "
                             "Refine Solve. The manual cleanup step of the inspect → clean → refine loop."));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);
        grpCleanup->addKnob(k); _imp->deleteBadBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Delete Tracks in Region"));
        k->setName("deleteRegion");
        k->setHintToolTip(tr("Remove tracks whose marker at the current frame falls inside the "
                             "Detection Region rectangle (turn on 'Use Detection Region' to see/set it). "
                             "Useful for culling a moving subject or a bad area."));
        k->setEvaluateOnChange(false);
        grpCleanup->addKnob(k); _imp->deleteRegionBtn = k;
    }

    // --- Import / Export (validation tools) ---
    addSeparator(trackPage, "sepIO");
    KnobGroupPtr grpIO = AppManager::createKnob<KnobGroup>(this, tr("Import / Export"));
    grpIO->setName("grpIO");
    grpIO->setDefaultValue(false);
    grpIO->setHintToolTip(tr("Track/camera exchange in plain text formats — the ground-truth "
                             "validation workflow"));
    trackPage->addKnob(grpIO);
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Import Tracks File"));
        k->setName("importTracksFile");
        k->setHintToolTip(tr("Path to a 2D-track text export (lines of 'track_id view x y'). "
                             "Used to validate the solver against externally-tracked 2D tracks."));
        grpIO->addKnob(k); _imp->importTracksFile = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Import 2D Tracks (.txt)"));
        k->setName("importTracks");
        k->setHintToolTip(tr("Replace the current tracks with those parsed from 'Import Tracks File'. "
                             "View N maps to frame (range start - 1 + N); the source's top-left/Y-down pixel "
                             "coords are converted to ours. Then press Solve to run our solver on them."));
        k->setEvaluateOnChange(false);
        grpIO->addKnob(k); _imp->importTracksBtn = k;
    }
    {
        KnobOutputFilePtr k = AppManager::createKnob<KnobOutputFile>(this, tr("Export Tracks File"));
        k->setName("exportTracksFile");
        k->setHintToolTip(tr("Path to write our 2D tracks (same 'track_id view x y' text format as "
                             "the import), for comparison against an external ground truth."));
        grpIO->addKnob(k); _imp->exportTracksFile = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Export 2D Tracks (.txt)"));
        k->setName("exportTracks");
        k->setHintToolTip(tr("Write the current tracks to 'Export Tracks File' in the same text format "
                             "(top-left/Y-down pixels; view = frame - range start + 1)."));
        k->setEvaluateOnChange(false);
        grpIO->addKnob(k); _imp->exportTracksBtn = k;
    }
    {
        KnobOutputFilePtr k = AppManager::createKnob<KnobOutputFile>(this, tr("Export Camera File"));
        k->setName("exportCameraFile");
        k->setHintToolTip(tr("Path to write our solved camera in a per-frame text format "
                             "(rotation matrix + translation + focal, one line per frame)."));
        grpIO->addKnob(k); _imp->exportCameraFile = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Export Camera (.txt)"));
        k->setName("exportCamera");
        k->setHintToolTip(tr("After solving, write the camera trajectory to 'Export Camera File' in "
                             "a per-frame text format, to compare against an external ground-truth camera."));
        k->setEvaluateOnChange(false);
        grpIO->addKnob(k); _imp->exportCameraBtn = k;
    }

    // Status line — always visible at the bottom of the tab.
    addSeparator(trackPage, "sepStatus");
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Status"));
        k->setName("solveStatus");
        k->setDefaultValue("Not solved");
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setEnabled(0, false);
        trackPage->addKnob(k);
        _imp->solveStatusDisplay = k;
    }

    // ========== Solving ==========
    KnobPagePtr solvePage = AppManager::createKnob<KnobPage>(this, tr("Solve"));

    KnobGroupPtr grpSolver = AppManager::createKnob<KnobGroup>(this, tr("Solver"));
    grpSolver->setName("grpSolver");
    grpSolver->setDefaultValue(true);
    solvePage->addKnob(grpSolver);

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Solve Mode"));
        k->setName("solveMode");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("camera", "Camera", "Full 6DOF camera solve — requires camera translation (parallax)"));
        entries.push_back(ChoiceOption("tripod", "Tripod", "Rotation-only solve — for locked-off or nodal cameras"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        grpSolver->addKnob(k);
        _imp->solveMode = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Reject Moving Objects"));
        k->setName("rejectMovingObjects");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("Automatically reject tracks on moving subjects (a walking/running person, "
                             "cars) WITHOUT a mask. Before solving, the dominant rigid "
                             "camera motion is fit across many frame pairs and tracks that violate it in "
                             "most pairs are dropped. Turn off if the whole frame is moving (e.g. a "
                             "locked shot of flowing water) or you're masking manually."));
        grpSolver->addKnob(k);
        _imp->rejectMovingObjects = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Fix Path Spikes"));
        k->setName("fixPathSpikes");
        k->setDefaultValue(false); // OFF: on smooth solves the speed-outlier test's MAD
                                   // threshold degenerates and the lerp rewrites GOOD
                                   // frames (observed rewriting 2 frames of a 0.34px
                                   // solve). Last-resort cosmetic repair only.
        k->setHintToolTip(tr("Replace camera frames whose speed spikes far above the rest of the "
                             "path with a linear interpolation of their neighbours. Last-resort "
                             "cosmetic repair for a mostly-good solve with a few broken frames — "
                             "the replaced frames no longer match the solved 3D points, so leave "
                             "this off unless you see isolated pops."));
        grpSolver->addKnob(k);
        _imp->fixPathSpikes = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Path Smoothness"));
        k->setName("pathSmoothness");
        k->setDefaultValue(0.0);
        k->setMinimum(0.0);
        k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Camera-path smoothness prior inside the bundle ('optimize "
                             "camera path smoothness'). Penalizes the camera trajectory's frame-to-"
                             "frame acceleration WHILE solving, so weakly-observed per-frame "
                             "translation is pulled toward constant velocity where the tracks don't "
                             "constrain it — the solve still has to explain the 2D tracks. "
                             "0 = off. Start around 0.1-0.3 for handheld, higher for dolly/crane "
                             "moves; too high flattens real motion."));
        grpSolver->addKnob(k);
        _imp->pathSmoothness = k;
    }
    addSeparator(solvePage, "sepIntrinsics");
    KnobGroupPtr grpIntrinsics = AppManager::createKnob<KnobGroup>(this, tr("Refine Intrinsics"));
    grpIntrinsics->setName("grpIntrinsics");
    grpIntrinsics->setDefaultValue(false);
    grpIntrinsics->setHintToolTip(tr("Let the bundle refine camera intrinsics — only with clean, "
                                     "dense tracks (lens distortion refine lives on the Lens tab)"));
    solvePage->addKnob(grpIntrinsics);
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Refine Focal Length"));
        k->setName("refineFocalLength");
        k->setDefaultValue(false);  // OFF by default: refining focal from noisy tracks is
                                    // unstable and runs away (observed 35mm -> 260mm),
                                    // corrupting the camera. Provide a known focal instead;
                                    // enable this only with clean, dense tracks.
        k->setHintToolTip(tr("Let the solver adjust the focal length during bundle adjustment. "
                             "Leave OFF unless your tracks are clean and dense — refining focal on "
                             "noisy tracks can diverge and wreck the solve. Prefer entering the real "
                             "focal length (from the lens / EXIF) on the Camera tab."));
        grpIntrinsics->addKnob(k);
        _imp->refineFocalLength = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Refine Principal Point"));
        k->setName("refinePrincipalPoint");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Allow the solver to adjust the principal point during bundle adjustment"));
        grpIntrinsics->addKnob(k);
        _imp->refinePrincipalPoint = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Max Track Error"));
        // 3px: the 5px default was calibrated when solves sat at ~30px; they
        // now converge at 0.4-0.8px, so 5px kept genuinely-bad tracks alive.
        k->setName("maxTrackError"); k->setDefaultValue(3.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Iterative bad-track rejection: after solving, tracks whose mean "
                             "reprojection error exceeds this (px) are removed and the camera is "
                             "re-bundled (up to 3 passes). 0 disables (standard auto-clean)."));
        grpSolver->addKnob(k);
        _imp->maxTrackError = k;
    }
    addSeparator(solvePage, "sepKeyframes");
    KnobGroupPtr grpKeyframes = AppManager::createKnob<KnobGroup>(this, tr("Keyframes"));
    grpKeyframes->setName("grpKeyframes");
    grpKeyframes->setDefaultValue(false);
    grpKeyframes->setHintToolTip(tr("Two-view initialization pair — auto-selected unless disabled"));
    solvePage->addKnob(grpKeyframes);
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Auto Keyframes"));
        k->setName("autoKeyframes");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("Automatically select keyframes using GRIC analysis.\n"
                              "Disable to manually set keyframe 1 and keyframe 2."));
        grpKeyframes->addKnob(k);
        _imp->autoKeyframes = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Keyframe 1"));
        k->setName("keyframe1");
        k->setDefaultValue(1);
        k->setHintToolTip(tr("First keyframe for two-view initialization (manual mode)"));
        k->setAddNewLine(false);
        grpKeyframes->addKnob(k);
        _imp->keyframe1 = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Keyframe 2"));
        k->setName("keyframe2");
        k->setDefaultValue(50);
        k->setHintToolTip(tr("Second keyframe for two-view initialization (manual mode)"));
        grpKeyframes->addKnob(k);
        _imp->keyframe2 = k;
    }
    addSeparator(solvePage, "sepSolveActions");
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Solve"));
        k->setName("solve");
        k->setHintToolTip(tr("Solve 3D camera motion from 2D tracks"));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);   // Solve / Refine on one row
        solvePage->addKnob(k);
        _imp->solveBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Refine Solve"));
        k->setName("refine");
        k->setHintToolTip(tr("Re-solve using the CURRENT tracks — run after removing bad tracks "
                             "(red in the viewer) or toggling distortion/focal refine. The "
                             "inspect → clean → refine loop. Track colours update each refine."));
        k->setEvaluateOnChange(false);
        solvePage->addKnob(k);
        _imp->refineBtn = k;
    }

    // Solve results (read-only display)
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Solve Error (px)"));
        k->setName("solveError");
        k->setDefaultValue(0.0);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setEnabled(0, false);
        solvePage->addKnob(k);
        _imp->solveErrorDisplay = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Cameras Solved"));
        k->setName("numCameras");
        k->setDefaultValue(0);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setEnabled(0, false);
        solvePage->addKnob(k);
        _imp->numCamerasDisplay = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("3D Points"));
        k->setName("numPoints");
        k->setDefaultValue(0);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setEnabled(0, false);
        solvePage->addKnob(k);
        _imp->numPointsDisplay = k;
    }

    // ========== Export ==========
    KnobPagePtr outputPage = AppManager::createKnob<KnobPage>(this, tr("Export"));

    KnobGroupPtr grpOrient = AppManager::createKnob<KnobGroup>(this, tr("Scene Orientation"));
    grpOrient->setName("grpOrient");
    grpOrient->setDefaultValue(true);
    grpOrient->setHintToolTip(tr("Orient the solved scene from selected points (pick tracks in the "
                                 "2D viewer after a solve, or cloud points in the 3D viewport), "
                                 "then re-run Create Camera3D / exports"));
    outputPage->addKnob(grpOrient);
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Set Origin From Selection"));
        k->setName("setOriginFromSelection");
        k->setHintToolTip(tr("Place the scene origin at the centroid of the cloud points currently "
                             "selected in the 3D viewport (click or box-select points first). "
                             "Re-run Create Camera3D / Export Point Cloud afterwards to output in "
                             "the new orientation."));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);   // origin / ground on one row
        grpOrient->addKnob(k);
        _imp->setOriginBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Set Ground From Selection"));
        k->setName("setGroundFromSelection");
        k->setHintToolTip(tr("Fit a plane to the cloud points currently selected in the 3D viewport "
                             "(3 or more, e.g. box-select a patch of ground) and rotate the scene so "
                             "that plane becomes the X-Z ground (its normal becomes +Y, pointing "
                             "toward the cameras). Re-run Create Camera3D / Export Point Cloud "
                             "afterwards to output in the new orientation."));
        k->setEvaluateOnChange(false);
        grpOrient->addKnob(k);
        _imp->setGroundBtn = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Known Distance"));
        k->setName("scaleDistance");
        k->setDefaultValue(1.0);
        k->setMinimum(1e-6);
        k->setDisplayMaximum(100.0);
        k->setAnimationEnabled(false);
        k->setAddNewLine(false);
        k->setHintToolTip(tr("Real-world distance (in your scene units — meters, feet, ...) "
                             "between the TWO points selected for Set Scale."));
        grpOrient->addKnob(k);
        _imp->scaleDistance = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Set Scale From Selection"));
        k->setName("setScaleFromSelection");
        k->setHintToolTip(tr("Select exactly TWO solved points (click tracks in the 2D viewer "
                             "after a solve, or cloud points in the 3D viewport), enter their "
                             "real-world distance, and press this — the whole scene (camera + "
                             "cloud + exports) is scaled to real units. Re-run Create Camera3D "
                             "afterwards."));
        k->setEvaluateOnChange(false);
        grpOrient->addKnob(k);
        _imp->setScaleBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Clear Scene Orientation"));
        k->setName("clearSceneOrientation");
        k->setHintToolTip(tr("Reset origin/ground/scale back to the automatic gauge "
                             "(first camera at origin, no rotation, ~10-unit span)."));
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);   // clear / card on one row
        grpOrient->addKnob(k);
        _imp->clearOrientBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Create Card At Selection"));
        k->setName("createCardAtSelection");
        k->setHintToolTip(tr("Create a Card3D at the centroid of the cloud points selected in the "
                             "3D viewport, oriented to the plane fitted through them (3+ points; "
                             "with fewer, the card faces the camera path). The quickest slip-test: "
                             "select points on a wall or the ground, create the card, render "
                             "through ScanlineRender and check it sticks."));
        k->setEvaluateOnChange(false);
        grpOrient->addKnob(k);
        _imp->createCardBtn = k;
    }

    addSeparator(outputPage, "sepOutputs");
    KnobGroupPtr grpOutputs = AppManager::createKnob<KnobGroup>(this, tr("Outputs"));
    grpOutputs->setName("grpOutputs");
    grpOutputs->setDefaultValue(true);
    outputPage->addKnob(grpOutputs);
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Create Camera3D"));
        k->setName("createCamera");
        k->setHintToolTip(tr("Create a Camera3D node with animated transforms from the solve.\n"
                              "The camera will have keyframes on every solved frame."));
        k->setEvaluateOnChange(false);
        grpOutputs->addKnob(k);
        _imp->createCameraBtn = k;
    }
    {
        KnobOutputFilePtr k = AppManager::createKnob<KnobOutputFile>(this, tr("Point Cloud File"));
        k->setName("pointCloudFile");
        k->setHintToolTip(tr("Output path for the reconstructed 3D point cloud (Wavefront .obj). "
                             "Written in the same normalized space as the Create Camera3D output, "
                             "so the cloud and the solved camera stay registered."));
        k->setAsOutputImageFile();
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        grpOutputs->addKnob(k);
        _imp->pointCloudFile = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Export Point Cloud (.obj)"));
        k->setName("exportPointCloud");
        k->setHintToolTip(tr("Write the reconstructed 3D points to the Point Cloud File path as a "
                             "Wavefront .obj (vertices + point elements)."));
        k->setEvaluateOnChange(false);
        grpOutputs->addKnob(k);
        _imp->exportPointCloudBtn = k;
    }
    {
        KnobOutputFilePtr k = AppManager::createKnob<KnobOutputFile>(this, tr("Solve Report File"));
        k->setName("solveReportFile");
        k->setHintToolTip(tr("Output path for the per-frame solve report (.csv): "
                             "frame, residual (px), focal (mm), translation, rotation. "
                             "Compare residual against another tracker's per-frame error."));
        k->setAsOutputImageFile();
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        grpOutputs->addKnob(k);
        _imp->solveReportFile = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Export Solve Report (.csv)"));
        k->setName("exportReport");
        k->setHintToolTip(tr("Write a per-frame CSV of residual + camera transform for the solve."));
        k->setEvaluateOnChange(false);
        grpOutputs->addKnob(k);
        _imp->exportReportBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Create Undistort Node"));
        k->setName("createUndistort");
        k->setHintToolTip(tr("Create a LensWarp node preloaded with this solve's lens model, "
                             "set to Undistort. Put it on the plate: downstream pinhole "
                             "renders (ScanlineRender through the solved camera) then match "
                             "exactly. Solve with 'Refine Lens Distortion' on first so "
                             "K1/K2 are meaningful."));
        k->setEvaluateOnChange(false);
        grpOutputs->addKnob(k);
        _imp->createUndistortBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Create Redistort Node"));
        k->setName("createRedistort");
        k->setHintToolTip(tr("Create a LensWarp node preloaded with this solve's lens model, "
                             "set to Redistort. Apply to the finished comp as the LAST step "
                             "so it matches the original (distorted) photography."));
        k->setEvaluateOnChange(false);
        grpOutputs->addKnob(k);
        _imp->createRedistortBtn = k;
    }


    // ========== Create internal Input/Output nodes for the NodeGroup ==========
    NodePtr thisNode = getNode();
    if (thisNode && thisNode->getApp()) {
        NodeGroupPtr thisGroup = std::dynamic_pointer_cast<NodeGroup>(thisNode->getEffectInstance());
        if (thisGroup) {
            try {
                NodePtr output, input;
                {
                    CreateNodeArgs outArgs(PLUGINID_NATRON_OUTPUT, thisGroup);
                    outArgs.setProperty<bool>(kCreateNodeArgsPropOutOfProject, true);
                    outArgs.setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
                    output = thisNode->getApp()->createNode(outArgs);
                }
                {
                    CreateNodeArgs inArgs(PLUGINID_NATRON_INPUT, thisGroup);
                    inArgs.setProperty<bool>(kCreateNodeArgsPropOutOfProject, true);
                    inArgs.setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
                    input = thisNode->getApp()->createNode(inArgs);
                }
                if (output && input) {
                    output->connectInput(input, 0);
                }
            } catch (...) {
                CT_DBG("CameraTracker: Warning — failed to create internal nodes\n");
            }
        }
    }
}


// ==================== Post-init: create internal Input/Output nodes ====================

void
CameraTrackerNode::onKnobsLoaded()
{
    CT_DBG("CameraTracker: onKnobsLoaded called\n");

    // Sync ROI-coordinate visibility with the loaded Use Detection Region value
    // (the knobs are secret-by-default; a project saved with the ROI on must
    // show them again).
    {
        const bool on = _imp->useROI.lock() && _imp->useROI.lock()->getValue();
        KnobDoublePtr rk;
        if ((rk = _imp->roiX1.lock())) rk->setSecret(!on);
        if ((rk = _imp->roiY1.lock())) rk->setSecret(!on);
        if ((rk = _imp->roiX2.lock())) rk->setSecret(!on);
        if ((rk = _imp->roiY2.lock())) rk->setSecret(!on);
    }

    NodePtr thisNode = getNode();
    if (!thisNode || !thisNode->getApp()) return;

    NodeGroupPtr thisGroup = std::dynamic_pointer_cast<NodeGroup>(thisNode->getEffectInstance());
    if (!thisGroup) return;

    // Check if we already have internal nodes (from serialization)
    if (thisGroup->getNInputs() > 0) {
        CT_DBG("CameraTracker: Internal nodes already exist (%d inputs)\n", thisGroup->getNInputs());
        return;
    }

    CT_DBG("CameraTracker: Creating internal Input/Output nodes\n");

    try {
        NodePtr output, input;
        {
            CreateNodeArgs outArgs(PLUGINID_NATRON_OUTPUT, thisGroup);
            outArgs.setProperty<bool>(kCreateNodeArgsPropOutOfProject, true);
            outArgs.setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
            output = thisNode->getApp()->createNode(outArgs);
        }
        {
            CreateNodeArgs inArgs(PLUGINID_NATRON_INPUT, thisGroup);
            inArgs.setProperty<bool>(kCreateNodeArgsPropOutOfProject, true);
            inArgs.setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
            input = thisNode->getApp()->createNode(inArgs);
        }
        if (output && input) {
            output->connectInput(input, 0);
            CT_DBG("CameraTracker: Internal nodes created and connected\n");
        }
    } catch (const std::exception& e) {
        CT_DBG("CameraTracker: Failed to create internal nodes: %s\n", e.what());
    } catch (...) {
        CT_DBG("CameraTracker: Failed to create internal nodes (unknown error)\n");
    }
}

void
CameraTrackerNode::onInputChanged(int inputNo)
{
    NodeGroup::onInputChanged(inputNo);
    if (inputNo == 0) {
        _imp->refreshFrameRangeFromInput();
    }
}

void
CameraTrackerNodePrivate::refreshFrameRangeFromInput()
{
    // Skip during project load so a saved (possibly user-narrowed) range survives;
    // this only fires on live connects, where clip range is the obvious intent.
    if (!publicInterface->getApp() ||
        publicInterface->getApp()->getProject()->isLoadingProject()) {
        return;
    }
    NodePtr node = publicInterface->getNode();
    NodePtr inputNode = node ? node->getInput(0) : NodePtr();
    if (!inputNode || !inputNode->getEffectInstance()) {
        return;
    }
    double first = 0, last = 0;
    inputNode->getEffectInstance()->getFrameRange_public(
        inputNode->getHashValue(), &first, &last, true /*bypasscache*/);
    if (!(last >= first) || (last - first) > 1e6) {
        return; // input advertises no meaningful range
    }
    KnobIntPtr rs = trackRangeStart.lock();
    KnobIntPtr re = trackRangeEnd.lock();
    if (!rs || !re) {
        return;
    }
    const int f = static_cast<int>(std::floor(first + 0.5));
    const int l = static_cast<int>(std::floor(last + 0.5));
    if (rs->getValue() != f) {
        rs->setValue(f);
    }
    if (re->getValue() != l) {
        re->setValue(l);
    }
    CT_DBG("CameraTracker: Frame range auto-set from input clip: %d-%d\n", f, l);
}


// ==================== Button Handlers ====================

bool
CameraTrackerNode::knobChanged(KnobI* k,
                                ValueChangedReasonEnum reason,
                                ViewSpec /*view*/,
                                double time,
                                bool /*originatedFromMainThread*/)
{
    if (reason == eValueChangedReasonNatronGuiEdited ||
        reason == eValueChangedReasonUserEdited) {

        if (k == _imp->detectFeaturesBtn.lock().get()) {
            _imp->detectFeatures(static_cast<int>(time));
            return true;
        }

        if (k == _imp->trackFeaturesBtn.lock().get()) {
            _imp->trackFeatures();
            return true;
        }

        if (k == _imp->trackManualBwdBtn.lock().get()) {
            _imp->trackFeatures(true, true);
            return true;
        }
        if (k == _imp->trackManualBtn.lock().get()) {
            _imp->trackFeatures(true);
            return true;
        }

        if (k == _imp->clearTracksBtn.lock().get()) {
            // Clear DETECTED tracks only — hand-placed tracks are deliberate work
            // and have their own Remove All Manual button.
            {
                std::vector<CameraTrackerNodePrivate::Track2D> keptManual;
                for (const auto& t : _imp->tracks) {
                    if (_imp->manualTrackIds.count(t.id)) keptManual.push_back(t);
                }
                _imp->tracks.swap(keptManual);
            }
            _imp->tracksImported = false;
            _imp->refreshManualList();
            _imp->solvedCameras.clear();
            _imp->solvedPoints.clear();
            _imp->hasSolution = false;
            _imp->cloudDirty = true;
            _imp->solveStatusDisplay.lock()->setValue("Tracks cleared");
            _imp->numCamerasDisplay.lock()->setValue(0);
            _imp->numPointsDisplay.lock()->setValue(0);
            _imp->solveErrorDisplay.lock()->setValue(0.0);
            return true;
        }

        if (k == _imp->deleteBadBtn.lock().get()) {
            _imp->deleteBadTracks();
            return true;
        }

        if (k == _imp->importTracksBtn.lock().get()) {
            _imp->importTracks2D(_imp->importTracksFile.lock()->getValue());
            return true;
        }

        if (k == _imp->exportTracksBtn.lock().get()) {
            _imp->exportTracks(_imp->exportTracksFile.lock()->getValue());
            return true;
        }

        if (k == _imp->exportCameraBtn.lock().get()) {
            _imp->exportCameraTrajectory(_imp->exportCameraFile.lock()->getValue());
            return true;
        }

        if (k == _imp->deleteRegionBtn.lock().get()) {
            _imp->deleteTracksInRegion(static_cast<int>(time));
            return true;
        }

        if (k == _imp->solveBtn.lock().get()) {
            _imp->solveCameraMotion();
            return true;
        }

        if (k == _imp->refineBtn.lock().get()) {
            _imp->solveCameraMotion(); // re-solve using the current (possibly cleaned) tracks
            return true;
        }

        if (k == _imp->grpManualKnob.lock().get()) {
            // Group expanded/collapsed — the settings-panel table follows.
            notifyManualTracksChanged();
            return false;   // let the default group handling run too
        }

        if (k == _imp->manualSelector.lock().get()) {
            if (_imp->manualSelectorGuard) return true;
            const int idx = _imp->manualSelector.lock()->getValue();
            if (idx >= 0 && idx < (int)_imp->manualChoiceIds.size()) {
                _imp->selectedManualId = _imp->manualChoiceIds[idx];
                _imp->refreshManualList();
                getApp()->redrawAllViewers();
            }
            return true;
        }

        if (k == _imp->deleteManualBtn.lock().get()) {
            if (_imp->selectedManualId < 0) {
                _imp->solveStatusDisplay.lock()->setValue(
                    "No manual track selected — click one in the viewer (Add Track Mode) first");
                return true;
            }
            const int id = _imp->selectedManualId;
            std::vector<CameraTrackerNodePrivate::Track2D> kept;
            kept.reserve(_imp->tracks.size());
            for (const auto& t : _imp->tracks) if (t.id != id) kept.push_back(t);
            _imp->tracks.swap(kept);
            _imp->manualTrackIds.erase(id);
            _imp->selectedManualId = -1;
            std::stringstream ss;
            ss << "Deleted manual track " << id << " (" << _imp->manualTrackIds.size() << " manual remain)";
            _imp->solveStatusDisplay.lock()->setValue(ss.str());
            _imp->refreshManualList();
            getApp()->redrawAllViewers();
            return true;
        }

        if (k == _imp->clearManualBtn.lock().get()) {
            if (_imp->manualTrackIds.empty()) {
                _imp->solveStatusDisplay.lock()->setValue("No manual tracks to remove");
                return true;
            }
            std::vector<CameraTrackerNodePrivate::Track2D> kept;
            kept.reserve(_imp->tracks.size());
            for (const auto& t : _imp->tracks) {
                if (!_imp->manualTrackIds.count(t.id)) kept.push_back(t);
            }
            const size_t removed = _imp->tracks.size() - kept.size();
            _imp->tracks.swap(kept);
            _imp->manualTrackIds.clear();
            _imp->selectedManualId = -1;
            std::stringstream ss;
            ss << "Removed " << removed << " manual track(s)";
            _imp->solveStatusDisplay.lock()->setValue(ss.str());
            _imp->refreshManualList();
            getApp()->redrawAllViewers();
            return true;
        }

        if (k == _imp->useROI.lock().get()) {
            // Region coordinates only clutter the panel while the ROI is off.
            const bool on = _imp->useROI.lock()->getValue();
            KnobDoublePtr rk;
            if ((rk = _imp->roiX1.lock())) rk->setSecret(!on);
            if ((rk = _imp->roiY1.lock())) rk->setSecret(!on);
            if ((rk = _imp->roiX2.lock())) rk->setSecret(!on);
            if ((rk = _imp->roiY2.lock())) rk->setSecret(!on);
            return true;
        }

        if (k == _imp->trackPlanarsBtn.lock().get()) {
            _imp->trackPlanarRegions();
            return true;
        }

        if (k == _imp->createCardsFromPlanarBtn.lock().get()) {
            _imp->createCardsFromPlanars();
            return true;
        }

        if (k == _imp->clearPlanarsBtn.lock().get()) {
            // Remove baked markers too, so the solve doesn't keep ghost grids.
            std::set<int> baked;
            for (const auto& p : _imp->planarTracks) {
                baked.insert(p.bakedIds.begin(), p.bakedIds.end());
            }
            if (!baked.empty()) {
                std::vector<CameraTrackerNodePrivate::Track2D> kept;
                kept.reserve(_imp->tracks.size());
                for (const auto& t : _imp->tracks) {
                    if (!baked.count(t.id)) kept.push_back(t);
                }
                _imp->tracks.swap(kept);
            }
            _imp->planarTracks.clear();
            _imp->solveStatusDisplay.lock()->setValue("Planar regions cleared");
            getApp()->redrawAllViewers();
            return true;
        }

        if (k == _imp->readExifBtn.lock().get()) {
            _imp->readFocalFromExif();
            return true;
        }
        if (k == _imp->createUndistortBtn.lock().get()) {
            _imp->createLensWarpNode(true);
            return true;
        }
        if (k == _imp->createRedistortBtn.lock().get()) {
            _imp->createLensWarpNode(false);
            return true;
        }
        if (k == _imp->estimateFocalBtn.lock().get()) {
            _imp->estimateFocalFromFootage();
            return true;
        }

        if (k == _imp->setOriginBtn.lock().get()) {
            _imp->setOriginFromSelection();
            return true;
        }

        if (k == _imp->setGroundBtn.lock().get()) {
            _imp->setGroundPlaneFromSelection();
            return true;
        }

        if (k == _imp->setScaleBtn.lock().get()) {
            _imp->setScaleFromSelection();
            return true;
        }

        if (k == _imp->clearOrientBtn.lock().get()) {
            _imp->clearSceneOrientation();
            return true;
        }

        if (k == _imp->createCardBtn.lock().get()) {
            _imp->createCardAtSelection();
            return true;
        }

        if (k == _imp->createCameraBtn.lock().get()) {
            _imp->createCamera3DNode();
            return true;
        }

        if (k == _imp->exportPointCloudBtn.lock().get()) {
            _imp->exportPointCloud();
            return true;
        }

        if (k == _imp->exportReportBtn.lock().get()) {
            _imp->exportSolveReport();
            return true;
        }
    }

    return false;
}


// ==================== Viewer Overlay ====================

void
CameraTrackerNode::drawOverlay(double time,
                                const RenderScale & /*renderScale*/,
                                ViewIdx /*view*/)
{
    if (!_imp) return;
    if (_imp->tracks.empty() && _imp->planarTracks.empty()) return;

    OverlaySupport* overlay = getCurrentViewportForOverlays();
    if (!overlay) return;

    double pixelScaleX, pixelScaleY;
    overlay->getPixelScale(pixelScaleX, pixelScaleY);

    int curFrame = static_cast<int>(time);
    double crossSize = 6.0 * pixelScaleX;
    int trailLength = 20;

    {
        GLProtectAttrib a(GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_ENABLE_BIT);

        // Draw ROI rectangle if enabled
        KnobBoolPtr roiKnob = _imp->useROI.lock();
        if (roiKnob && roiKnob->getValue()) {
            double rx1 = _imp->roiX1.lock()->getValue();
            double ry1 = _imp->roiY1.lock()->getValue();
            double rx2 = _imp->roiX2.lock()->getValue();
            double ry2 = _imp->roiY2.lock()->getValue();

            glColor4f(1.0f, 1.0f, 0.0f, 0.7f);
            glLineWidth(1.0f);
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(2, 0xAAAA);
            glBegin(GL_LINE_LOOP);
            glVertex2d(rx1, ry1);
            glVertex2d(rx2, ry1);
            glVertex2d(rx2, ry2);
            glVertex2d(rx1, ry2);
            glEnd();
            glDisable(GL_LINE_STIPPLE);
        }

        // Draw "Add Track" cursor indicator
        KnobBoolPtr addMode = _imp->addTrackMode.lock();
        if (addMode && addMode->getValue()) {
            // Draw a small "+" at the cursor position would require mouse position
            // Instead, just indicate mode is active via the status display
        }

        // Planar quads: tracked = green solid at the current frame's tracked
        // position; untracked = orange at the drawn (ref-frame) position, with
        // corner handles while Add Planar mode is on.
        {
            KnobBoolPtr pm = _imp->addPlanarMode.lock();
            const bool planarModeOn = pm && pm->getValue();
            for (const auto& pt : _imp->planarTracks) {
                const std::array<double, 8>* qd = NULL;
                bool atFrame = false;
                auto it = pt.quads.find(curFrame);
                if (it != pt.quads.end()) { qd = &it->second; atFrame = true; }
                else if (!pt.tracked) {
                    auto rf = pt.quads.find(pt.refFrame);
                    if (rf != pt.quads.end()) qd = &rf->second;
                }
                if (!qd) continue;
                if (pt.tracked) glColor4f(0.2f, 1.0f, 0.3f, atFrame ? 0.9f : 0.35f);
                else            glColor4f(1.0f, 0.6f, 0.1f, atFrame ? 0.9f : 0.5f);
                glLineWidth(2.0f);
                glBegin(GL_LINE_LOOP);
                for (int c = 0; c < 4; ++c) glVertex2d((*qd)[c * 2], (*qd)[c * 2 + 1]);
                glEnd();
                if (planarModeOn && !pt.tracked) {
                    const double hs = 5.0 * pixelScaleX;
                    for (int c = 0; c < 4; ++c) {
                        glBegin(GL_LINE_LOOP);
                        glVertex2d((*qd)[c*2] - hs, (*qd)[c*2+1] - hs);
                        glVertex2d((*qd)[c*2] + hs, (*qd)[c*2+1] - hs);
                        glVertex2d((*qd)[c*2] + hs, (*qd)[c*2+1] + hs);
                        glVertex2d((*qd)[c*2] - hs, (*qd)[c*2+1] + hs);
                        glEnd();
                    }
                }
            }
        }

        // Rubber-band selection box (post-solve drag select)
        if (_imp->selDragActive) {
            glColor4f(0.2f, 0.9f, 1.0f, 0.9f);
            glLineWidth(1.0f);
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, 0xAAAA);
            glBegin(GL_LINE_LOOP);
            glVertex2d(_imp->selDragX0, _imp->selDragY0);
            glVertex2d(_imp->selDragX1, _imp->selDragY0);
            glVertex2d(_imp->selDragX1, _imp->selDragY1);
            glVertex2d(_imp->selDragX0, _imp->selDragY1);
            glEnd();
            glDisable(GL_LINE_STIPPLE);
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // After a solve the trails just clutter the stick check — draw crosses
        // only. Trails remain useful pre-solve for judging tracking quality.
        const bool solved = _imp->hasSolution;

        // Track ids currently selected (selection indexes solvedPoints).
        std::set<int> selectedTrackIds;
        for (int idx : _imp->viewportSelection) {
            if (idx >= 0 && idx < (int)_imp->solvedPoints.size()) {
                selectedTrackIds.insert(_imp->solvedPoints[idx].track);
            }
        }

        for (size_t t = 0; t < _imp->tracks.size(); ++t) {
            const auto& track = _imp->tracks[t];
            if (track.markers.empty()) continue;

            auto curIt = track.markers.find(curFrame);

            // Health colour from the track's solved reprojection error:
            //  unsolved (no solve yet) = grey, rejected outlier = red,
            //  good (<1px) = green, ok (1-3px) = yellow, bad (>3px) = orange-red.
            float r, g, b;
            const double e = track.error;
            if (e <= -1.5)      { r = 1.0f; g = 0.15f; b = 0.15f; } // rejected
            else if (e < 0.0)   { r = 1.0f; g = 0.25f; b = 0.7f;  } // not solved yet (pink — visible on plate)
            else if (e <= 1.0)  { r = 0.1f; g = 1.0f;  b = 0.1f;  } // good
            else if (e <= 3.0)  { r = 1.0f; g = 0.9f;  b = 0.1f;  } // ok
            else                { r = 1.0f; g = 0.4f;  b = 0.1f;  } // bad

            // Draw trail (dim, recent path) — pre-solve only
            if (!solved) {
                glLineWidth(1.5f);
                glColor4f(r, g, b, 0.4f);
                glBegin(GL_LINE_STRIP);
                for (int f = curFrame - trailLength; f <= curFrame; ++f) {
                    auto it = track.markers.find(f);
                    if (it != track.markers.end()) {
                        glVertex2d(it->second.first, it->second.second);
                    }
                }
                glEnd();
            }

            // Draw cross at current position, coloured by health
            if (curIt != track.markers.end()) {
                double cx = curIt->second.first;
                double cy = curIt->second.second;
                const bool isManual = _imp->manualTrackIds.count(track.id) != 0;
                const bool isSelected = selectedTrackIds.count(track.id) != 0 ||
                                        (isManual && track.id == _imp->selectedManualId);
                glColor4f(r, g, b, 1.0f);
                glLineWidth(isSelected ? 3.0f : 1.5f);
                glBegin(GL_LINES);
                glVertex2d(cx - crossSize, cy);
                glVertex2d(cx + crossSize, cy);
                glVertex2d(cx, cy - crossSize);
                glVertex2d(cx, cy + crossSize);
                glEnd();
                if (isManual) {
                    // Manual tracks read as a diamond around the cross; the
                    // selected one is cyan.
                    const bool sel = (track.id == _imp->selectedManualId);
                    const double s = crossSize * 1.1;
                    if (sel) glColor4f(0.2f, 0.9f, 1.0f, 1.0f);
                    glLineWidth(sel ? 2.5f : 1.5f);
                    glBegin(GL_LINE_LOOP);
                    glVertex2d(cx - s, cy);
                    glVertex2d(cx, cy - s);
                    glVertex2d(cx + s, cy);
                    glVertex2d(cx, cy + s);
                    glEnd();
                    // Pattern box (the texture window the tracker matches) —
                    // Tracker-node-style; per-track size (corner-drag to resize),
                    // falling back to the Pattern Size knob.
                    const double ph = _imp->patternHalfFor(track.id);
                    glColor4f(sel ? 0.2f : 0.8f, sel ? 0.9f : 0.8f, sel ? 1.0f : 0.8f, 0.7f);
                    glLineWidth(1.0f);
                    glBegin(GL_LINE_LOOP);
                    glVertex2d(cx - ph, cy - ph);
                    glVertex2d(cx + ph, cy - ph);
                    glVertex2d(cx + ph, cy + ph);
                    glVertex2d(cx - ph, cy + ph);
                    glEnd();
                    // Corner handles on the selected track while Add Track mode
                    // is on — grab one to resize the box.
                    KnobBoolPtr am = _imp->addTrackMode.lock();
                    if (sel && am && am->getValue()) {
                        const double hs = 4.0 * pixelScaleX;
                        for (int sx = -1; sx <= 1; sx += 2) {
                            for (int sy = -1; sy <= 1; sy += 2) {
                                const double hx = cx + sx * ph, hy = cy + sy * ph;
                                glBegin(GL_LINE_LOOP);
                                glVertex2d(hx - hs, hy - hs);
                                glVertex2d(hx + hs, hy - hs);
                                glVertex2d(hx + hs, hy + hs);
                                glVertex2d(hx - hs, hy + hs);
                                glEnd();
                            }
                        }
                    }
                }
                if (isSelected) {
                    // Cyan box marks selection (feeds Set Origin / Set Ground /
                    // Create Card just like 3D-viewport selection).
                    const double s = crossSize * 1.4;
                    glColor4f(0.2f, 0.9f, 1.0f, 1.0f);
                    glLineWidth(1.5f);
                    glBegin(GL_LINE_LOOP);
                    glVertex2d(cx - s, cy - s);
                    glVertex2d(cx + s, cy - s);
                    glVertex2d(cx + s, cy + s);
                    glVertex2d(cx - s, cy + s);
                    glEnd();
                }
            }
        }

        // --- Drag magnifier: zoomed pattern view next to the cursor while a
        // manual track is being dragged (Tracker-node magnification window).
        if (_imp->manualDragActive && _imp->magVisible && _imp->magTexture &&
            _imp->selectedManualId >= 0 && _imp->magTextureFrame == curFrame) {
            double mx = 0, my = 0;
            bool found = false;
            for (const auto& tr2 : _imp->tracks) {
                if (tr2.id != _imp->selectedManualId) continue;
                auto it = tr2.markers.find(curFrame);
                if (it != tr2.markers.end()) {
                    mx = it->second.first;
                    my = it->second.second;
                    found = true;
                }
                break;
            }
            if (found) {
                const double patch = 2.0 * _imp->patternHalfFor(_imp->selectedManualId);
                // ~220 screen px window offset up-right of the marker, showing
                // 2.5 pattern-widths of image -> a ~5-10x zoom in practice.
                const double winPx = 220.0;
                const double wx1 = mx + 40.0 * pixelScaleX;
                const double wy1 = my + 40.0 * pixelScaleY;
                const double wx2 = wx1 + winPx * pixelScaleX;
                const double wy2 = wy1 + winPx * pixelScaleY;
                const double zoomHalf = std::max(10.0, patch * 1.25);

                const RectI& roi = _imp->magTextureRoI;
                const double u1 = (mx - zoomHalf - roi.x1) / (double)roi.width();
                const double u2 = (mx + zoomHalf - roi.x1) / (double)roi.width();
                const double v1 = (my - zoomHalf - roi.y1) / (double)roi.height();
                const double v2 = (my + zoomHalf - roi.y1) / (double)roi.height();

                glColor4f(1.f, 1.f, 1.f, 1.f);
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, _imp->magTexture->getTexID());
                glBegin(GL_POLYGON);
                glTexCoord2d(u1, v1); glVertex2d(wx1, wy1);
                glTexCoord2d(u1, v2); glVertex2d(wx1, wy2);
                glTexCoord2d(u2, v2); glVertex2d(wx2, wy2);
                glTexCoord2d(u2, v1); glVertex2d(wx2, wy1);
                glEnd();
                glBindTexture(GL_TEXTURE_2D, 0);
                glDisable(GL_TEXTURE_2D);

                // Border, center crosshair, pattern box scaled into the window.
                glColor4f(0.2f, 0.9f, 1.0f, 0.9f);
                glLineWidth(1.5f);
                glBegin(GL_LINE_LOOP);
                glVertex2d(wx1, wy1);
                glVertex2d(wx1, wy2);
                glVertex2d(wx2, wy2);
                glVertex2d(wx2, wy1);
                glEnd();
                const double wcx = (wx1 + wx2) / 2.0, wcy = (wy1 + wy2) / 2.0;
                const double ch = 12.0 * pixelScaleX;
                glBegin(GL_LINES);
                glVertex2d(wcx - ch, wcy);
                glVertex2d(wcx + ch, wcy);
                glVertex2d(wcx, wcy - ch);
                glVertex2d(wcx, wcy + ch);
                glEnd();
                const double magScale = (wx2 - wx1) / (2.0 * zoomHalf);
                const double pb = (patch / 2.0) * magScale;
                glColor4f(0.2f, 0.9f, 1.0f, 0.6f);
                glLineWidth(1.0f);
                glBegin(GL_LINE_LOOP);
                glVertex2d(wcx - pb, wcy - pb);
                glVertex2d(wcx + pb, wcy - pb);
                glVertex2d(wcx + pb, wcy + pb);
                glVertex2d(wcx - pb, wcy + pb);
                glEnd();
            }
        }
    }
}


// ==================== Manual Track Placement ====================

bool
CameraTrackerNode::onOverlayPenDown(double time,
                                     const RenderScale & /*renderScale*/,
                                     ViewIdx /*view*/,
                                     const QPointF & /*viewportPos*/,
                                     const QPointF & pos,
                                     double /*pressure*/,
                                     double /*timestamp*/,
                                     PenType /*pen*/)
{
    if (!_imp) return false;

    // --- Planar quad creation / editing (Add Planar mode) ---
    KnobBoolPtr planarMode = _imp->addPlanarMode.lock();
    if (planarMode && planarMode->getValue()) {
        const int curFrame = static_cast<int>(time);
        double psX = 1.0, psY = 1.0;
        if (OverlaySupport* ovl = getCurrentViewportForOverlays()) {
            ovl->getPixelScale(psX, psY);
        }
        const double pickR = 12.0 * std::max(psX, psY);

        // Corner hit-test — on ANY frame that has a quad. Editing a tracked
        // frame turns it into a user KEY (keyed correction): re-tracking
        // then propagates between keys, so fixing a broken end-frame repairs
        // the tail without disturbing the good frames before the key.
        for (size_t q = 0; q < _imp->planarTracks.size(); ++q) {
            auto& pt = _imp->planarTracks[q];
            int hitFrame = -1;
            auto it = pt.quads.find(curFrame);
            if (it == pt.quads.end() && !pt.tracked) {
                it = pt.quads.find(pt.refFrame);
                if (it != pt.quads.end()) hitFrame = pt.refFrame;
            } else if (it != pt.quads.end()) {
                hitFrame = curFrame;
            }
            if (hitFrame < 0) continue;
            for (int c = 0; c < 4; ++c) {
                const double d = std::hypot(it->second[c * 2] - pos.x(),
                                            it->second[c * 2 + 1] - pos.y());
                if (d < pickR) {
                    _imp->planarEditQuad = (int)q;
                    _imp->planarEditCorner = c;
                    _imp->planarEditFrame = hitFrame;
                    pt.userKeys.insert(hitFrame);
                    return true;
                }
            }
        }

        // No corner hit: drag out a new quad on this frame.
        CameraTrackerNodePrivate::PlanarTrack pt;
        pt.id = (int)_imp->planarTracks.size();
        pt.refFrame = curFrame;
        std::array<double, 8> qd;
        for (int c = 0; c < 4; ++c) { qd[c * 2] = pos.x(); qd[c * 2 + 1] = pos.y(); }
        pt.quads[curFrame] = qd;
        pt.userKeys.insert(curFrame);
        _imp->planarTracks.push_back(pt);
        _imp->planarCreating = true;
        _imp->planarStartX = pos.x();
        _imp->planarStartY = pos.y();
        return true;
    }

    // --- Solved-track selection (2D viewer) ---
    // After a solve, the plate viewer becomes the picking surface for the
    // shared selection (indices into solvedPoints) that drives Set Origin /
    // Set Ground / Create Card. A click toggles the nearest cross; a drag
    // rubber-bands. The decision happens on pen-UP; pen-down just arms it.
    KnobBoolPtr addMode = _imp->addTrackMode.lock();
    const bool addModeOn = addMode && addMode->getValue();
    if (!addModeOn && _imp->hasSolution && !_imp->solvedPoints.empty()) {
        _imp->selDragActive = true;
        _imp->selDragX0 = _imp->selDragX1 = pos.x();
        _imp->selDragY0 = _imp->selDragY1 = pos.y();
        return true; // consume so we receive motion/up
    }

    // Only handle clicks when Add Track Mode is enabled
    if (!addModeOn) return false;

    int curFrame = static_cast<int>(time);
    double x = pos.x();
    double y = pos.y();

    // Click near an existing manual track: SELECT it and arm a drag (drag
    // repositions its marker on this frame). Click empty space: add a new one.
    {
        double psX = 1.0, psY = 1.0;
        if (OverlaySupport* ovl = getCurrentViewportForOverlays()) {
            ovl->getPixelScale(psX, psY);
        }
        const double pickR = 12.0 * std::max(psX, psY);

        // Corner hit on the SELECTED track's pattern box first: arm a resize
        // drag (Tracker-node-style box sizing) — takes precedence over a grab.
        if (_imp->selectedManualId >= 0) {
            for (const auto& tr2 : _imp->tracks) {
                if (tr2.id != _imp->selectedManualId) continue;
                auto it = tr2.markers.find(curFrame);
                if (it == tr2.markers.end()) break;
                const double ph = _imp->patternHalfFor(tr2.id);
                for (int sx = -1; sx <= 1; sx += 2) {
                    for (int sy = -1; sy <= 1; sy += 2) {
                        if (std::hypot(pos.x() - (it->second.first + sx * ph),
                                       pos.y() - (it->second.second + sy * ph)) < pickR * 0.8) {
                            _imp->patternResizeActive = true;
                            return true;
                        }
                    }
                }
                break;
            }
        }

        int hitId = -1;
        double bestD = pickR;
        for (const auto& tr2 : _imp->tracks) {
            if (!_imp->manualTrackIds.count(tr2.id) || tr2.markers.empty()) continue;
            // marker on this frame, else the nearest earlier one (still grabbable)
            auto it = tr2.markers.find(curFrame);
            if (it == tr2.markers.end()) {
                it = tr2.markers.lower_bound(curFrame);
                if (it == tr2.markers.begin() && it->first > curFrame) { /* keep first */ }
                else if (it == tr2.markers.end() || it->first > curFrame) --it;
            }
            const double d = std::hypot(it->second.first - x, it->second.second - y);
            if (d < bestD) { bestD = d; hitId = tr2.id; }
        }
        if (hitId >= 0) {
            _imp->selectedManualId = hitId;
            _imp->manualDragActive = true;
            _imp->magVisible = _imp->refreshMagnifierTexture(curFrame, x, y);
            std::stringstream ss;
            ss << "Manual track " << hitId << " selected — drag to reposition, "
                  "'Delete Selected Track' to remove";
            _imp->solveStatusDisplay.lock()->setValue(ss.str());
            _imp->refreshManualList();
            getApp()->redrawAllViewers();
            return true;
        }
    }

    // Create a new manual track at the clicked position
    int newId = 0;
    for (const auto& tr2 : _imp->tracks) newId = std::max(newId, tr2.id + 1);
    CameraTrackerNodePrivate::Track2D t;
    t.id = newId;
    t.markers[curFrame] = std::make_pair(x, y);
    _imp->tracks.push_back(t);
    _imp->manualTrackIds.insert(newId);
    _imp->selectedManualId = newId;
    _imp->manualDragActive = true;   // click-drag places precisely in one gesture
    _imp->magVisible = _imp->refreshMagnifierTexture(curFrame, x, y);

    std::stringstream ss;
    ss << "Added manual track " << newId << " at (" << (int)x << ", " << (int)y
       << ") on frame " << curFrame << " (" << _imp->manualTrackIds.size() << " manual)";
    _imp->solveStatusDisplay.lock()->setValue(ss.str());
    _imp->refreshManualList();

    // Trigger overlay redraw
    getApp()->redrawAllViewers();

    return true;
}

bool
CameraTrackerNode::onOverlayPenMotion(double time,
                                       const RenderScale & /*renderScale*/,
                                       ViewIdx /*view*/,
                                       const QPointF & /*viewportPos*/,
                                       const QPointF & pos,
                                       double /*pressure*/,
                                       double /*timestamp*/)
{
    if (!_imp) return false;

    // Pattern-box resize: half-size follows the dragged corner (symmetric,
    // square box — Chebyshev distance from the marker center).
    if (_imp->patternResizeActive && _imp->selectedManualId >= 0) {
        const int curFrame = static_cast<int>(time);
        for (auto& tr2 : _imp->tracks) {
            if (tr2.id != _imp->selectedManualId) continue;
            auto it = tr2.markers.find(curFrame);
            if (it != tr2.markers.end()) {
                tr2.patternHalf = std::max(4.0, std::max(std::abs(pos.x() - it->second.first),
                                                         std::abs(pos.y() - it->second.second)));
            }
            break;
        }
        getApp()->redrawAllViewers();
        return true;
    }

    // Manual track drag: reposition the selected track's marker on this frame.
    if (_imp->manualDragActive && _imp->selectedManualId >= 0) {
        const int curFrame = static_cast<int>(time);
        for (auto& tr2 : _imp->tracks) {
            if (tr2.id == _imp->selectedManualId) {
                tr2.markers[curFrame] = std::make_pair(pos.x(), pos.y());
                break;
            }
        }
        // Keep the magnifier fed: re-render only when the drag leaves the
        // inner half of the cached RoI (or the frame changed).
        if (_imp->magVisible) {
            RectI safe = _imp->magTextureRoI;
            const int mx = safe.width() / 4, my = safe.height() / 4;
            safe.x1 += mx; safe.x2 -= mx; safe.y1 += my; safe.y2 -= my;
            if (curFrame != _imp->magTextureFrame ||
                pos.x() < safe.x1 || pos.x() > safe.x2 ||
                pos.y() < safe.y1 || pos.y() > safe.y2) {
                _imp->magVisible = _imp->refreshMagnifierTexture(curFrame, pos.x(), pos.y());
            }
        }
        getApp()->redrawAllViewers();
        return true;
    }

    // Planar quad creation: live-update the rectangle (BL,BR,TR,TL).
    if (_imp->planarCreating && !_imp->planarTracks.empty()) {
        auto& pt = _imp->planarTracks.back();
        const double x1 = std::min(_imp->planarStartX, pos.x());
        const double x2 = std::max(_imp->planarStartX, pos.x());
        const double y1 = std::min(_imp->planarStartY, pos.y());
        const double y2 = std::max(_imp->planarStartY, pos.y());
        std::array<double, 8>& qd = pt.quads[pt.refFrame];
        qd[0] = x1; qd[1] = y1;   // BL
        qd[2] = x2; qd[3] = y1;   // BR
        qd[4] = x2; qd[5] = y2;   // TR
        qd[6] = x1; qd[7] = y2;   // TL
        getApp()->redrawAllViewers();
        return true;
    }
    // Planar corner drag (on whichever frame the drag started on).
    if (_imp->planarEditQuad >= 0 && _imp->planarEditCorner >= 0 &&
        _imp->planarEditQuad < (int)_imp->planarTracks.size()) {
        auto& pt = _imp->planarTracks[_imp->planarEditQuad];
        const int ef = (_imp->planarEditFrame >= 0) ? _imp->planarEditFrame : pt.refFrame;
        std::array<double, 8>& qd = pt.quads[ef];
        qd[_imp->planarEditCorner * 2] = pos.x();
        qd[_imp->planarEditCorner * 2 + 1] = pos.y();
        getApp()->redrawAllViewers();
        return true;
    }

    if (!_imp->selDragActive) return false;
    _imp->selDragX1 = pos.x();
    _imp->selDragY1 = pos.y();
    getApp()->redrawAllViewers();
    return true;
}

bool
CameraTrackerNode::onOverlayPenUp(double time,
                                   const RenderScale & /*renderScale*/,
                                   ViewIdx /*view*/,
                                   const QPointF & /*viewportPos*/,
                                   const QPointF & pos,
                                   double /*pressure*/,
                                   double /*timestamp*/)
{
    if (!_imp) return false;

    if (_imp->patternResizeActive) {
        _imp->patternResizeActive = false;
        getApp()->redrawAllViewers();
        return true;
    }

    if (_imp->manualDragActive) {
        _imp->manualDragActive = false;
        _imp->magVisible = false;
        _imp->refreshManualList();
        getApp()->redrawAllViewers();
        return true;
    }

    // Planar quad finish: discard degenerate drags (a stray click).
    if (_imp->planarCreating) {
        _imp->planarCreating = false;
        if (!_imp->planarTracks.empty()) {
            const auto& qd = _imp->planarTracks.back().quads[_imp->planarTracks.back().refFrame];
            const double diag = std::hypot(qd[4] - qd[0], qd[5] - qd[1]);
            double psX = 1.0, psY = 1.0;
            if (OverlaySupport* ovl = getCurrentViewportForOverlays()) {
                ovl->getPixelScale(psX, psY);
            }
            if (diag < 12.0 * std::max(psX, psY)) {
                _imp->planarTracks.pop_back();
            } else {
                std::stringstream ss;
                ss << _imp->planarTracks.size() << " planar region(s) drawn — adjust corners, "
                      "then press 'Track Planar Regions'";
                _imp->solveStatusDisplay.lock()->setValue(ss.str());
            }
        }
        getApp()->redrawAllViewers();
        return true;
    }
    if (_imp->planarEditQuad >= 0) {
        _imp->planarEditQuad = -1;
        _imp->planarEditCorner = -1;
        _imp->planarEditFrame = -1;
        _imp->solveStatusDisplay.lock()->setValue(
            "Corner key set — press 'Track Planar Regions' to re-track between keys");
        getApp()->redrawAllViewers();
        return true;
    }

    if (!_imp->selDragActive) return false;
    _imp->selDragActive = false;
    _imp->selDragX1 = pos.x();
    _imp->selDragY1 = pos.y();

    const int curFrame = static_cast<int>(time);
    double psX = 1.0, psY = 1.0;
    if (OverlaySupport* ovl = getCurrentViewportForOverlays()) {
        ovl->getPixelScale(psX, psY);
    }
    const double clickThreshold = 4.0 * std::max(psX, psY);
    const bool isClick =
        std::hypot(_imp->selDragX1 - _imp->selDragX0,
                   _imp->selDragY1 - _imp->selDragY0) < clickThreshold;

    // solvedPoints index per track id, for tracks that survived the solve.
    std::map<int, int> trackToPointIdx;
    for (size_t i = 0; i < _imp->solvedPoints.size(); ++i) {
        trackToPointIdx[_imp->solvedPoints[i].track] = (int)i;
    }

    auto& sel = _imp->viewportSelection;
    if (isClick) {
        // Toggle the nearest cross within ~10 screen px; empty click clears.
        const double pickRadius = 10.0 * std::max(psX, psY);
        int bestIdx = -1;
        double bestDist = pickRadius;
        for (const auto& track : _imp->tracks) {
            auto tp = trackToPointIdx.find(track.id);
            if (tp == trackToPointIdx.end()) continue;
            auto it = track.markers.find(curFrame);
            if (it == track.markers.end()) continue;
            const double d = std::hypot(it->second.first - pos.x(),
                                        it->second.second - pos.y());
            if (d < bestDist) { bestDist = d; bestIdx = tp->second; }
        }
        if (bestIdx >= 0) {
            auto found = std::find(sel.begin(), sel.end(), bestIdx);
            if (found != sel.end()) sel.erase(found);
            else sel.push_back(bestIdx);
        } else {
            sel.clear();
        }
    } else {
        // Rubber band: replace the selection with every solved track whose
        // marker on this frame falls inside the box.
        double x1 = std::min(_imp->selDragX0, _imp->selDragX1);
        double x2 = std::max(_imp->selDragX0, _imp->selDragX1);
        double y1 = std::min(_imp->selDragY0, _imp->selDragY1);
        double y2 = std::max(_imp->selDragY0, _imp->selDragY1);
        sel.clear();
        for (const auto& track : _imp->tracks) {
            auto tp = trackToPointIdx.find(track.id);
            if (tp == trackToPointIdx.end()) continue;
            auto it = track.markers.find(curFrame);
            if (it == track.markers.end()) continue;
            const double mx = it->second.first, my = it->second.second;
            if (mx >= x1 && mx <= x2 && my >= y1 && my <= y2) {
                sel.push_back(tp->second);
            }
        }
    }

    if (!sel.empty()) {
        std::stringstream ss;
        ss << sel.size() << " track(s) selected — Set Origin / Set Ground / "
              "Create Card use this selection";
        _imp->solveStatusDisplay.lock()->setValue(ss.str());
    }
    getApp()->redrawAllViewers();
    return true;
}


// ==================== Manual Track Cleanup ====================

void
CameraTrackerNodePrivate::deleteBadTracks()
{
    const double thr = deleteErrorThreshold.lock()->getValue();
    std::vector<Track2D> kept;
    kept.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const double e = tracks[i].error;
        const bool bad = (e <= -1.5) || (e >= 0.0 && e > thr); // rejected outlier, or high error
        if (!bad) {
            kept.push_back(tracks[i]);
        }
    }
    int removed = (int)(tracks.size() - kept.size());
    tracks.swap(kept);

    std::stringstream ss;
    if (!hasSolution) {
        ss << "Removed " << removed << " tracks. (Solve first to flag bad tracks by error.)";
    } else {
        ss << "Deleted " << removed << " bad tracks (>" << thr << "px / rejected). "
           << tracks.size() << " remain — press Refine Solve.";
    }
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: deleteBadTracks removed %d, %d remain\n", removed, (int)tracks.size());
}

void
CameraTrackerNodePrivate::deleteTracksInRegion(int frame)
{
    double x1 = roiX1.lock()->getValue(), y1 = roiY1.lock()->getValue();
    double x2 = roiX2.lock()->getValue(), y2 = roiY2.lock()->getValue();
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);

    std::vector<Track2D> kept;
    kept.reserve(tracks.size());
    int removed = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        std::map<int, std::pair<double, double> >::const_iterator it = tracks[i].markers.find(frame);
        bool inside = false;
        if (it != tracks[i].markers.end()) {
            double mx = it->second.first, my = it->second.second;
            inside = (mx >= x1 && mx <= x2 && my >= y1 && my <= y2);
        }
        if (inside) { ++removed; } else { kept.push_back(tracks[i]); }
    }
    tracks.swap(kept);

    std::stringstream ss;
    ss << "Deleted " << removed << " tracks inside the region at frame " << frame
       << ". " << tracks.size() << " remain — press Refine Solve.";
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: deleteTracksInRegion removed %d, %d remain\n", removed, (int)tracks.size());
}


// ==================== 2D track import (validation) ====================

void
CameraTrackerNodePrivate::importTracks2D(const std::string& path)
{
    if (path.empty()) {
        solveStatusDisplay.lock()->setValue("Set 'Import Tracks File' to a track .txt first");
        return;
    }
    std::ifstream in(path.c_str());
    if (!in.is_open()) {
        solveStatusDisplay.lock()->setValue("Could not open track file: " + path);
        return;
    }

    // Image height for the Y flip (the source format is top-left/Y-down; ours is Y-up).
    NodePtr node = publicInterface->getNode();
    NodePtr inputNode = node ? node->getInput(0) : NodePtr();
    double imgH = 1080.0, imgW = 1920.0, rodX1 = 0.0, rodY2 = 1080.0;
    if (inputNode) {
        EffectInstancePtr input = inputNode->getEffectInstance();
        RectD rod; RenderScale scale; bool ipf = false;
        int rs = trackRangeStart.lock()->getValue();
        if (input && input->getRegionOfDefinition_public(inputNode->getHashValue(),
                (double)rs, scale, ViewIdx(0), &rod, &ipf) == eStatusOK) {
            imgH = rod.height(); imgW = rod.width(); rodX1 = rod.x1; rodY2 = rod.y2;
        }
    }

    // View N maps to frame (rangeStart - 1 + N): source frame 0 == file rangeStart,
    // and its 2D export numbers views from 1. Keep markers within a margin of the
    // frame (some exports carry ~23% off-frame extrapolations that would skew the solve).
    const int rangeStart = trackRangeStart.lock()->getValue();
    const double margin = 100.0;

    std::map<std::string, int> idMap;    // source track_id -> our track index
    std::map<std::string, int> lastView; // last view seen per source id (reuse detection)
    tracks.clear();
    long parsed = 0, kept = 0, split = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string tid; int view; double bx, by;
        if (!(ss >> tid >> view >> bx >> by)) continue;
        ++parsed;
        // Convert source pixel coords -> our Natron marker coords (Y-up).
        double natronX = bx + rodX1;
        double natronY = (rodY2 - 1.0) - by;
        if (bx < -margin || bx > imgW + margin || by < -margin || by > imgH + margin) continue;
        int frame = rangeStart - 1 + view;
        std::map<std::string, int>::iterator it = idMap.find(tid);
        // Some exports REUSE track ids: when e.g. "auto_57" restarts at a lower/equal view
        // number it is a new physical feature. Merging the runs (the old behavior)
        // built teleporting tracks — one 2D path jumping between two features — which
        // are poisonous solve constraints. Split each reused id into its own track.
        bool fresh = (it == idMap.end());
        if (!fresh && view <= lastView[tid]) {
            fresh = true;
            ++split;
        }
        int idx;
        if (fresh) {
            idx = (int)tracks.size();
            idMap[tid] = idx;
            Track2D t; t.id = idx;
            tracks.push_back(t);
        } else {
            idx = it->second;
        }
        lastView[tid] = view;
        tracks[idx].markers[frame] = std::make_pair(natronX, natronY);
        ++kept;
    }

    // Lock focal refinement (the point of importing curated tracks is isolating the
    // solver, and the source's exact focal should be typed in by the user), but do NOT
    // overwrite focal/sensor: they are PER-CLIP. An earlier version hardcoded the
    // PF0061 values (27.62mm/14.7574mm) here, which silently poisoned every import
    // test on other footage (e.g. the train clip reference solve at 23.68mm/20.12mm
    // — a 59% FOV error -> guaranteed slide).
    refineFocalLength.lock()->setValue(false);
    // Clear stale distortion from any previous solve: with refinement off, leftover
    // K1/K2 would be APPLIED as fixed lens distortion and silently poison this
    // isolation test (observed: 0.34px solve degraded to 4.8px by stale knobs).
    k1.lock()->setValue(0.0);
    k2.lock()->setValue(0.0);
    k3.lock()->setValue(0.0);
    refineLensDistortion.lock()->setValue(false);

    solvedCameras.clear(); solvedPoints.clear(); hasSolution = false; cloudDirty = true;
    tracksImported = true;
    numCamerasDisplay.lock()->setValue(0);
    numPointsDisplay.lock()->setValue(0);

    std::stringstream ss;
    ss << "Imported " << tracks.size() << " tracks (" << kept << "/" << parsed
       << " markers in-frame). NOW SET Focal Length + Sensor Width to the values in "
          "the source's camera export header (#Filmback Size / F(mm) columns), then Solve.";
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: Imported %d tracks (%ld reused ids split), %ld/%ld markers kept; "
           "focal locked 27.62mm, distortion cleared\n",
            (int)tracks.size(), split, kept, parsed);
}


void
CameraTrackerNodePrivate::exportTracks(const std::string& path)
{
    if (path.empty()) {
        solveStatusDisplay.lock()->setValue("Set 'Export Tracks File' to a path first");
        return;
    }
    if (tracks.empty()) {
        solveStatusDisplay.lock()->setValue("No tracks to export");
        return;
    }
    std::ofstream out(path.c_str());
    if (!out.is_open()) {
        solveStatusDisplay.lock()->setValue("Could not open file for writing: " + path);
        return;
    }

    // Same coordinate convention as the exports we compare against: top-left
    // origin, Y-down, pixels; view = frame - rangeStart + 1.
    const int rangeStart = trackRangeStart.lock()->getValue();
    double rodX1 = 0.0, rodY2 = 1080.0;
    NodePtr node = publicInterface->getNode();
    NodePtr inputNode = node ? node->getInput(0) : NodePtr();
    if (inputNode && inputNode->getEffectInstance()) {
        RectD rod; RenderScale sc; bool ipf = false;
        if (inputNode->getEffectInstance()->getRegionOfDefinition_public(
                inputNode->getHashValue(), (double)rangeStart, sc, ViewIdx(0), &rod, &ipf) == eStatusOK) {
            rodX1 = rod.x1; rodY2 = rod.y2;
        }
    }

    out << "# Natron CameraTracker 2d tracks export\n";
    out << "# Format: track_id  view  x  y  (top-left origin, Y-down, pixels)\n";
    out << "# view N = frame " << rangeStart << " + (N-1)\n";
    long markers = 0;
    for (size_t t = 0; t < tracks.size(); ++t) {
        const Track2D& tr = tracks[t];
        for (std::map<int, std::pair<double,double> >::const_iterator it = tr.markers.begin();
             it != tr.markers.end(); ++it) {
            int view = it->first - rangeStart + 1;
            double bx = it->second.first - rodX1;
            double by = (rodY2 - 1.0) - it->second.second;
            out << "track_" << tr.id << "  " << view << "  " << bx << "  " << by << "\n";
            ++markers;
        }
    }
    out.close();

    std::stringstream ss;
    ss << "Exported " << tracks.size() << " tracks (" << markers << " markers) to " << path;
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: Exported %d tracks, %ld markers\n", (int)tracks.size(), markers);
}


void
CameraTrackerNodePrivate::exportCameraTrajectory(const std::string& path)
{
    if (path.empty()) {
        solveStatusDisplay.lock()->setValue("Set 'Export Camera File' to a path first");
        return;
    }
    if (!hasSolution || solvedCameras.empty()) {
        solveStatusDisplay.lock()->setValue("No solution — solve first");
        return;
    }
    std::ofstream out(path.c_str());
    if (!out.is_open()) {
        solveStatusDisplay.lock()->setValue("Could not open file for writing: " + path);
        return;
    }

    // Match the reference camera text line format so the two can be parsed identically:
    //   R00 R01 R02 R10 R11 R12 R20 R21 R22  Tx Ty Tz  F(mm)   (one line per frame)
    // Cameras are already sorted by frame. NOTE: ours are in Natron's world convention
    // (F-flipped, world-flipped) not the reference's, so absolute values won't match — the
    // comparison is on trajectory SHAPE (translation path length, rotation swing,
    // per-frame smoothness), which is convention-independent for the diagnosis.
    out << "# Natron CameraTracker camera export (per-frame R 9, C 3, focal mm)\n";
    out << "# R(9, row-major)  Tx Ty Tz  Focal(mm)   one line per solved frame\n";
    out << "# frames " << solvedCameras.front().frame << " to " << solvedCameras.back().frame << "\n";
    const double focalMm = focalLengthMm.lock()->getValue();
    out.precision(12);
    for (size_t i = 0; i < solvedCameras.size(); ++i) {
        const SolvedCamera& c = solvedCameras[i];
        out << std::fixed
            << c.R[0][0] << "\t" << c.R[0][1] << "\t" << c.R[0][2] << "\t"
            << c.R[1][0] << "\t" << c.R[1][1] << "\t" << c.R[1][2] << "\t"
            << c.R[2][0] << "\t" << c.R[2][1] << "\t" << c.R[2][2] << "\t"
            << c.tx << "\t" << c.ty << "\t" << c.tz << "\t" << focalMm << "\n";
    }
    out.close();

    std::stringstream ss;
    ss << "Exported camera (" << solvedCameras.size() << " frames) to " << path;
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: Exported camera trajectory, %d frames\n", (int)solvedCameras.size());
}


// ==================== Feature Detection ====================

void
CameraTrackerNodePrivate::detectFeatures(int frame)
{
    try {
    NodePtr node = publicInterface->getNode();
    if (!node) return;

    NodePtr inputNode = node->getInput(0);
    if (!inputNode) {
        solveStatusDisplay.lock()->setValue("No input connected");
        return;
    }
    EffectInstancePtr input = inputNode->getEffectInstance();
    if (!input) {
        solveStatusDisplay.lock()->setValue("No input connected");
        return;
    }

    // Get image dimensions
    RectD rod;
    double time = static_cast<double>(frame);
    RenderScale scale;
    bool isProjectFormat = false;
    StatusEnum stat = input->getRegionOfDefinition_public(inputNode->getHashValue(), time, scale, ViewIdx(0), &rod, &isProjectFormat);
    if (stat != eStatusOK) {
        solveStatusDisplay.lock()->setValue("Failed to get input image dimensions");
        return;
    }

    int width = static_cast<int>(rod.width());
    int height = static_cast<int>(rod.height());
    if (width <= 0 || height <= 0) {
        solveStatusDisplay.lock()->setValue("Invalid image dimensions");
        return;
    }

    // Render input frame to get pixel data
    RectI roi;
    roi.x1 = static_cast<int>(rod.x1);
    roi.y1 = static_cast<int>(rod.y1);
    roi.x2 = static_cast<int>(rod.x2);
    roi.y2 = static_cast<int>(rod.y2);

    // Setup render context (required before calling renderRoI)
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
                                              0,      // texture index
                                              node->getApp()->getTimeLine().get(),
                                              NodePtr(), // rotoPaintNode
                                              true,   // isAnalysis
                                              false,  // draftMode
                                              RenderStatsPtr());

    std::list<ImagePlaneDesc> requestedComps;
    requestedComps.push_back(ImagePlaneDesc::getRGBAComponents());

    RenderScale renderScale;
    EffectInstance::RenderRoIArgs args(time,
                                       renderScale,
                                       0, // mipmapLevel
                                       ViewIdx(0),
                                       false, // byPassCache
                                       roi,
                                       rod,   // precomputed RoD
                                       requestedComps,
                                       eImageBitDepthFloat,
                                       true,  // calledFromGetImage
                                       node->getEffectInstance().get(),
                                       eStorageModeRAM,
                                       time);

    std::map<ImagePlaneDesc, ImagePtr> planes;
    EffectInstance::RenderRoIRetCode renderOk = input->renderRoI(args, &planes);

    std::stringstream dbg;
    dbg << "renderRoI returned " << (int)renderOk << ", planes=" << planes.size();

    if (renderOk != EffectInstance::eRenderRoIRetCodeOk || planes.empty()) {
        dbg << " — FAILED. RoD: " << rod.x1 << "," << rod.y1 << " - " << rod.x2 << "," << rod.y2;
        solveStatusDisplay.lock()->setValue(dbg.str());
        return;
    }

    ImagePtr img = planes.begin()->second;
    if (!img) {
        solveStatusDisplay.lock()->setValue("Null image from input");
        return;
    }

    // Convert to libmv FloatImage (single-channel grayscale)
    RectI bounds = img->getBounds();
    int imgW = bounds.width();
    int imgH = bounds.height();
    int nComps = img->getComponentsCount();

    if (imgW <= 0 || imgH <= 0) {
        solveStatusDisplay.lock()->setValue("Image has zero dimensions");
        return;
    }

    libmv::FloatImage mvImage(imgH, imgW, 1);
    Image::ReadAccess ra(img.get());

    float maxVal = 0;
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            const float* pix = (const float*)ra.pixelAt(x, y);
            float lum = 0;
            if (pix) {
                if (nComps >= 3) {
                    lum = 0.2126f * pix[0] + 0.7152f * pix[1] + 0.0722f * pix[2];
                } else if (nComps == 1) {
                    lum = pix[0];
                } else {
                    lum = pix[0];
                }
            }
            // libmv uses (row, col) = (y from top, x)
            int mvRow = (bounds.y2 - 1) - y; // Natron is bottom-up, libmv is top-down
            int mvCol = x - bounds.x1;
            mvImage(mvRow, mvCol, 0) = lum;
            if (lum > maxVal) maxVal = lum;
        }
    }

    if (maxVal < 1e-8f) {
        std::stringstream ss;
        ss << "Image appears to be black (max pixel = " << maxVal
           << "). Dimensions: " << imgW << "x" << imgH
           << ", components: " << nComps;
        solveStatusDisplay.lock()->setValue(ss.str());
        return;
    }

    // Map the 0-100 Detection Sensitivity to a Harris threshold logarithmically
    // (higher sensitivity = lower threshold = more features). ~60 ≈ the old 1e-6.
    const double sens = detectSensitivity.lock()->getValue();
    const double harrisThr = std::pow(10.0, -3.0 - 0.05 * sens); // s=0→1e-3, 60→1e-6, 100→1e-8
    // Resolution normalization (reference 1920px wide): min-distance and margin
    // are spacing quantities (scale with width), the feature BUDGET is an area
    // quantity (scale with width^2) — so knob values keep their HD meaning and
    // UHD footage gets HD-equivalent feature density instead of a quarter of it.
    const double resScale = std::max(0.5, (double)imgW / 1920.0);
    const int baseMinDist = std::max(1, (int)(minFeatureDistance.lock()->getValue() * resScale));

    // --- Multi-scale (Feature Scale) detection ---
    // Harris only fires on sharp corners at full res, so soft/distant terrain (hazy
    // hills, foliage) yields nothing. Detecting on downscaled copies surfaces that
    // coarse structure; features are mapped back to full-res coords and merged.
    auto downsample2x = [](const libmv::FloatImage& in) -> libmv::FloatImage {
        int h = in.Height() / 2, w = in.Width() / 2;
        libmv::FloatImage out(h, w, 1);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out(y, x, 0) = 0.25f * (in(2*y, 2*x, 0) + in(2*y, 2*x+1, 0)
                                      + in(2*y+1, 2*x, 0) + in(2*y+1, 2*x+1, 0));
        return out;
    };

    libmv::vector<libmv::Feature> features;
    auto detectAtLevel = [&](const libmv::FloatImage& img, int level, double thr) {
        const int s = 1 << level;
        libmv::DetectOptions o;
        o.type = libmv::DetectOptions::HARRIS;
        o.margin = std::max(16, (int)(16 * resScale));
        o.min_distance = std::max(1, baseMinDist / s);
        o.harris_threshold = thr;
        libmv::vector<libmv::Feature> f;
        libmv::Detect(img, o, &f);
        for (int i = 0; i < (int)f.size(); ++i) {
            // scale detected coords back to full resolution
            features.push_back(libmv::Feature(f[i].x * s, f[i].y * s, f[i].score, f[i].size * s));
        }
    };

    // Feature budget scales with image AREA so the knob keeps its HD meaning.
    int maxFeat = (int)(maxFeatures.lock()->getValue() * resScale * resScale);

    const int fscale = featureScale.lock()->getValue(); // 0=Normal, 1=Large, 2=Both
    const bool doL0 = (fscale == 0 || fscale == 2);
    const bool doL1 = (fscale == 2);
    const bool doL2 = (fscale == 1 || fscale == 2);

    // Adaptive multi-scale detection on ONE image: retry with a 10x lower
    // threshold until the candidate pool reaches `target` (or floor).
    auto adaptiveDetect = [&](const libmv::FloatImage& img0, int target) -> double {
        libmv::FloatImage half, quarter;
        if (doL1 || doL2) half = downsample2x(img0);
        if (doL2) quarter = downsample2x(half);
        double thr = harrisThr;
        int attempts = 0;
        for (;;) {
            features.clear();
            if (doL0) detectAtLevel(img0, 0, thr);
            if (doL1) detectAtLevel(half, 1, thr);
            if (doL2) detectAtLevel(quarter, 2, thr);
            if ((int)features.size() >= target || ++attempts >= 5 || thr <= 1e-10) break;
            thr *= 0.1;
        }
        return thr;
    };

    // --- Two-pass detection: RAW first, contrast-normalized as FILLER ---
    // Pass 1 (raw image): honest Harris scores — strong corners rank correctly.
    // Pass 2 (normalized image): surfaces low-contrast structure, but its scores
    // are NOT comparable (normalization makes weak corners score like strong
    // ones — an earlier single-pass version let mushy corners beat elite ones in
    // the bucketing and DEGRADED the camera 0.12% -> 9.5% of path vs ground truth).
    // So: normalized-found candidates are dropped near any raw candidate and
    // ranked strictly BELOW every raw one; they only fill cells that have no
    // real corner. Coverage without sacrificing quality.
    // With Boost Coverage OFF: single fixed-threshold pass — byte-identical to
    // the proven elite detection (best measured camera: 0.12% of path vs
    // ground truth). With it ON: dig adaptively toward the budget; the filler pass
    // below adds coverage candidates that can never outrank a raw corner.
    const bool boost = normalizeContrast.lock()->getValue();
    const double thrUsed = adaptiveDetect(mvImage, boost ? maxFeat : 0);
    // (libmv::vector can't copy Feature — no default ctor — so std::vector here.)
    std::vector<libmv::Feature> rawFeatures;
    rawFeatures.reserve(features.size());
    for (int i = 0; i < (int)features.size(); ++i) rawFeatures.push_back(features[i]);
    const bool normC = boost;
    int fillerCount = 0;
    if (normC) {
        libmv::FloatImage detImage = normalizeLocalContrast(mvImage, (int)(32 * resScale));
        adaptiveDetect(detImage, 3 * maxFeat);
        std::vector<libmv::Feature> normFeatures;
        normFeatures.reserve(features.size());
        for (int i = 0; i < (int)features.size(); ++i) normFeatures.push_back(features[i]);

        // Suppression grid over raw candidates
        const double sup = std::max(1.0, (double)baseMinDist);
        std::set<std::pair<int,int> > occ;
        for (size_t i = 0; i < rawFeatures.size(); ++i) {
            occ.insert({(int)(rawFeatures[i].x / sup), (int)(rawFeatures[i].y / sup)});
        }
        double minRawScore = 1e30;
        for (size_t i = 0; i < rawFeatures.size(); ++i) {
            minRawScore = std::min(minRawScore, (double)rawFeatures[i].score);
        }
        features.clear();
        for (size_t i = 0; i < rawFeatures.size(); ++i) features.push_back(rawFeatures[i]);
        for (size_t i = 0; i < normFeatures.size(); ++i) {
            const int cx = (int)(normFeatures[i].x / sup), cy = (int)(normFeatures[i].y / sup);
            bool nearRaw = false;
            for (int dy = -1; dy <= 1 && !nearRaw; ++dy)
                for (int dx = -1; dx <= 1 && !nearRaw; ++dx)
                    if (occ.count({cx + dx, cy + dy})) nearRaw = true;
            if (nearRaw) continue;
            libmv::Feature f = normFeatures[i];
            // rank strictly below every raw corner, preserving relative order
            f.score = (float)(minRawScore * 1e-3) * (1.0f + f.score * 1e-6f);
            features.push_back(f);
            ++fillerCount;
        }
    }
    // (normC off: `features` already holds the raw pass)

    CT_DBG("CameraTracker: Detect at %dpx wide -> resScale %.2f (budget %d, minDist %d, "
           "%d raw + %d filler candidates, thr %.1e)\n",
           imgW, resScale, maxFeat, baseMinDist,
           (int)rawFeatures.size(), fillerCount, thrUsed);

    // ROI filtering — only keep features inside the detection region
    bool doROI = useROI.lock()->getValue();
    double roiLeft = roiX1.lock()->getValue();
    double roiBottom = roiY1.lock()->getValue();
    double roiRight = roiX2.lock()->getValue();
    double roiTop = roiY2.lock()->getValue();

    // --- Spatial bucketing for even feature distribution ---
    // Harris responds hardest on high-contrast regions (e.g. a white building
    // against sky), so picking the globally-strongest N piles every feature onto
    // one cluster and starves the low-contrast areas (distant hills) the solve
    // needs for good conditioning. Instead, grid the frame and keep only the
    // strongest ~targetPerCell features PER CELL.
    const int targetPerCell = 4;
    int desiredCells = std::max(1, maxFeat / targetPerCell);
    int gridCols = std::max(1, static_cast<int>(std::round(std::sqrt(static_cast<double>(desiredCells) * imgW / imgH))));
    int gridRows = std::max(1, static_cast<int>(std::round(static_cast<double>(desiredCells) / gridCols)));
    int numCells = gridCols * gridRows;
    int perCell = std::max(1, maxFeat / numCells);
    double cellW = static_cast<double>(imgW) / gridCols;
    double cellH = static_cast<double>(imgH) / gridRows;

    // Assign each ROI-passing feature to its grid cell (indices into `features`).
    std::vector<std::vector<int> > buckets(numCells);
    for (int i = 0; i < static_cast<int>(features.size()); ++i) {
        double natronX = features[i].x + bounds.x1;
        double natronY = (bounds.y2 - 1) - features[i].y;
        if (doROI && (natronX < roiLeft || natronX > roiRight ||
                      natronY < roiBottom || natronY > roiTop)) {
            continue;
        }
        int cx = std::min(gridCols - 1, std::max(0, static_cast<int>(features[i].x / cellW)));
        int cy = std::min(gridRows - 1, std::max(0, static_cast<int>(features[i].y / cellH)));
        buckets[cy * gridCols + cx].push_back(i);
    }

    // Keep the highest-score `perCell` features in each cell.
    int startTrackId = static_cast<int>(tracks.size());
    int addedCount = 0;
    for (int c = 0; c < numCells && addedCount < maxFeat; ++c) {
        std::vector<int>& idxs = buckets[c];
        std::sort(idxs.begin(), idxs.end(),
                  [&features](int a, int b) { return features[a].score > features[b].score; });
        int take = std::min(static_cast<int>(idxs.size()), perCell);
        for (int k = 0; k < take && addedCount < maxFeat; ++k) {
            const libmv::Feature& f = features[idxs[k]];
            Track2D t;
            t.id = startTrackId + addedCount;
            t.markers[frame] = std::make_pair(f.x + bounds.x1, (bounds.y2 - 1) - f.y);
            tracks.push_back(t);
            addedCount++;
        }
    }

    tracksImported = false;   // tracks are now (at least partly) our own detector's

    std::stringstream ss;
    ss << "Detected " << addedCount << " features on frame " << frame;
    if (doROI) {
        ss << " (ROI filtered from " << features.size() << ")";
    }
    ss << " (" << imgW << "x" << imgH << ", total tracks: " << tracks.size() << ")";
    solveStatusDisplay.lock()->setValue(ss.str());

    } catch (const std::exception& e) {
        solveStatusDisplay.lock()->setValue(std::string("Detect failed: ") + e.what());
    } catch (...) {
        solveStatusDisplay.lock()->setValue("Detect failed: unknown error");
    }
}


// Detect fresh features on an already-rendered frame (top-down libmv coords) and
// append them as new tracks starting at `frame`. Only seeds regions that have lost
// coverage: features near an existing live marker (or an already-accepted new one)
// are suppressed, and each grid cell's budget is reduced by the tracks it already
// holds — so new features flow into the emptied areas the camera just revealed.
// This is the replenishment half of continuous detect-and-track. Returns the
// number of tracks added.
int
CameraTrackerNodePrivate::redetectFeatures(const libmv::FloatImage& img, int frame,
                                           int imgW, int imgH, double rodX1, double rodY2)
{
    const double sens = detectSensitivity.lock()->getValue();
    const double harrisThr = std::pow(10.0, -3.0 - 0.05 * sens);
    // Resolution normalization — must mirror detectFeatures exactly, or initial
    // and re-detected features live at different densities/spacings.
    const double resScale = std::max(0.5, (double)imgW / 1920.0);
    const int baseMinDist = std::max(1, (int)(minFeatureDistance.lock()->getValue() * resScale));
    const int maxFeat = (int)(maxFeatures.lock()->getValue() * resScale * resScale);

    // --- Multi-scale Harris (mirrors detectFeatures) ---
    auto downsample2x = [](const libmv::FloatImage& in) -> libmv::FloatImage {
        int h = in.Height() / 2, w = in.Width() / 2;
        libmv::FloatImage out(h, w, 1);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out(y, x, 0) = 0.25f * (in(2*y, 2*x, 0) + in(2*y, 2*x+1, 0)
                                      + in(2*y+1, 2*x, 0) + in(2*y+1, 2*x+1, 0));
        return out;
    };
    libmv::vector<libmv::Feature> features;
    auto detectAtLevel = [&](const libmv::FloatImage& im, int level, double thr) {
        const int s = 1 << level;
        libmv::DetectOptions o;
        o.type = libmv::DetectOptions::HARRIS;
        o.margin = std::max(16, (int)(16 * resScale));
        o.min_distance = std::max(1, baseMinDist / s);
        o.harris_threshold = thr;
        libmv::vector<libmv::Feature> f;
        libmv::Detect(im, o, &f);
        for (int i = 0; i < (int)f.size(); ++i) {
            features.push_back(libmv::Feature(f[i].x * s, f[i].y * s, f[i].score, f[i].size * s));
        }
    };
    // Two-pass detection (raw priority + normalized filler) — must mirror
    // detectFeatures; see the rationale there.
    const int fscale = featureScale.lock()->getValue();
    const bool doL0 = (fscale == 0 || fscale == 2);
    const bool doL1 = (fscale == 2);
    const bool doL2 = (fscale == 1 || fscale == 2);
    auto adaptiveDetect = [&](const libmv::FloatImage& img0, int target) {
        libmv::FloatImage half, quarter;
        if (doL1 || doL2) half = downsample2x(img0);
        if (doL2) quarter = downsample2x(half);
        double thr = harrisThr;
        int attempts = 0;
        for (;;) {
            features.clear();
            if (doL0) detectAtLevel(img0, 0, thr);
            if (doL1) detectAtLevel(half, 1, thr);
            if (doL2) detectAtLevel(quarter, 2, thr);
            if ((int)features.size() >= target || ++attempts >= 5 || thr <= 1e-10) break;
            thr *= 0.1;
        }
    };
    const bool normC = normalizeContrast.lock()->getValue();
    adaptiveDetect(img, normC ? maxFeat : 0);   // fixed threshold unless boosting (mirror detectFeatures)
    std::vector<libmv::Feature> rawFeatures;
    rawFeatures.reserve(features.size());
    for (int i = 0; i < (int)features.size(); ++i) rawFeatures.push_back(features[i]);
    if (normC) {
        libmv::FloatImage detImage = normalizeLocalContrast(img, (int)(32 * resScale));
        adaptiveDetect(detImage, 3 * maxFeat);
        std::vector<libmv::Feature> normFeatures;
        normFeatures.reserve(features.size());
        for (int i = 0; i < (int)features.size(); ++i) normFeatures.push_back(features[i]);
        const double sup = std::max(1.0, (double)baseMinDist);
        std::set<std::pair<int,int> > occRaw;
        for (size_t i = 0; i < rawFeatures.size(); ++i) {
            occRaw.insert({(int)(rawFeatures[i].x / sup), (int)(rawFeatures[i].y / sup)});
        }
        double minRawScore = 1e30;
        for (size_t i = 0; i < rawFeatures.size(); ++i) {
            minRawScore = std::min(minRawScore, (double)rawFeatures[i].score);
        }
        features.clear();
        for (size_t i = 0; i < rawFeatures.size(); ++i) features.push_back(rawFeatures[i]);
        for (size_t i = 0; i < normFeatures.size(); ++i) {
            const int cx = (int)(normFeatures[i].x / sup), cy = (int)(normFeatures[i].y / sup);
            bool nearRaw = false;
            for (int dy = -1; dy <= 1 && !nearRaw; ++dy)
                for (int dx = -1; dx <= 1 && !nearRaw; ++dx)
                    if (occRaw.count({cx + dx, cy + dy})) nearRaw = true;
            if (nearRaw) continue;
            libmv::Feature f = normFeatures[i];
            f.score = (float)(minRawScore * 1e-3) * (1.0f + f.score * 1e-6f);
            features.push_back(f);
        }
    }
    // (normC off: `features` already holds the raw pass)

    // ROI (same optional Detection Region filter as detectFeatures)
    const bool doROI = useROI.lock()->getValue();
    const double roiLeft = roiX1.lock()->getValue();
    const double roiBottom = roiY1.lock()->getValue();
    const double roiRight = roiX2.lock()->getValue();
    const double roiTop = roiY2.lock()->getValue();

    // --- Suppression grid keyed on the min-feature-distance ---
    // A fine grid of occupied points (existing live markers + accepted new ones);
    // a candidate is rejected if any occupied point sits within `suppress` px.
    const double suppress = std::max((double)baseMinDist, (double)8.0);
    const int sgCols = std::max(1, (int)std::ceil(imgW / suppress));
    const int sgRows = std::max(1, (int)std::ceil(imgH / suppress));
    std::vector<std::vector<std::pair<float,float> > > occ(sgCols * sgRows);
    auto sgIndex = [&](double x, double y) -> int {
        int cx = std::min(sgCols - 1, std::max(0, (int)(x / suppress)));
        int cy = std::min(sgRows - 1, std::max(0, (int)(y / suppress)));
        return cy * sgCols + cx;
    };
    auto occInsert = [&](double x, double y) {
        occ[sgIndex(x, y)].push_back(std::make_pair((float)x, (float)y));
    };
    auto isSuppressed = [&](double x, double y) -> bool {
        int cx = std::min(sgCols - 1, std::max(0, (int)(x / suppress)));
        int cy = std::min(sgRows - 1, std::max(0, (int)(y / suppress)));
        const double r2 = suppress * suppress;
        for (int dy = -1; dy <= 1; ++dy) {
            int ny = cy + dy; if (ny < 0 || ny >= sgRows) continue;
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = cx + dx; if (nx < 0 || nx >= sgCols) continue;
                const std::vector<std::pair<float,float> >& cell = occ[ny * sgCols + nx];
                for (size_t p = 0; p < cell.size(); ++p) {
                    double ddx = x - cell[p].first, ddy = y - cell[p].second;
                    if (ddx * ddx + ddy * ddy < r2) return true;
                }
            }
        }
        return false;
    };

    // --- Coarse bucket grid for even distribution (mirrors detectFeatures) ---
    const int targetPerCell = 4;
    int desiredCells = std::max(1, maxFeat / targetPerCell);
    int gridCols = std::max(1, (int)std::round(std::sqrt((double)desiredCells * imgW / imgH)));
    int gridRows = std::max(1, (int)std::round((double)desiredCells / gridCols));
    int numCells = gridCols * gridRows;
    int perCell = std::max(1, maxFeat / numCells);
    double cellW = (double)imgW / gridCols;
    double cellH = (double)imgH / gridRows;
    std::vector<int> cellBudget(numCells, perCell);

    // Seed suppression + per-cell occupancy from existing live markers at this frame.
    for (size_t t = 0; t < tracks.size(); ++t) {
        std::map<int, std::pair<double,double> >::const_iterator it = tracks[t].markers.find(frame);
        if (it == tracks[t].markers.end()) continue;
        double mvX = it->second.first - rodX1;
        double mvY = (rodY2 - 1) - it->second.second;
        if (mvX < 0 || mvX >= imgW || mvY < 0 || mvY >= imgH) continue;
        occInsert(mvX, mvY);
        int cx = std::min(gridCols - 1, std::max(0, (int)(mvX / cellW)));
        int cy = std::min(gridRows - 1, std::max(0, (int)(mvY / cellH)));
        cellBudget[cy * gridCols + cx] -= 1;  // cell already partly populated
    }

    // Bucket candidate features by cell; drop ROI-outside and self-suppressed ones.
    std::vector<std::vector<int> > buckets(numCells);
    for (int i = 0; i < (int)features.size(); ++i) {
        double mvX = features[i].x, mvY = features[i].y;
        if (mvX < 0 || mvX >= imgW || mvY < 0 || mvY >= imgH) continue;
        double natronX = mvX + rodX1;
        double natronY = (rodY2 - 1) - mvY;
        if (doROI && (natronX < roiLeft || natronX > roiRight ||
                      natronY < roiBottom || natronY > roiTop)) continue;
        int cx = std::min(gridCols - 1, std::max(0, (int)(mvX / cellW)));
        int cy = std::min(gridRows - 1, std::max(0, (int)(mvY / cellH)));
        buckets[cy * gridCols + cx].push_back(i);
    }

    // Fill each cell up to its remaining budget, strongest first, skipping any
    // candidate too close to an already-placed point.
    int nextId = 0;
    for (size_t t = 0; t < tracks.size(); ++t) nextId = std::max(nextId, tracks[t].id + 1);
    int added = 0;
    for (int c = 0; c < numCells; ++c) {
        int budget = cellBudget[c];
        if (budget <= 0) continue;
        std::vector<int>& idxs = buckets[c];
        std::sort(idxs.begin(), idxs.end(),
                  [&features](int a, int b) { return features[a].score > features[b].score; });
        int placed = 0;
        for (size_t k = 0; k < idxs.size() && placed < budget; ++k) {
            const libmv::Feature& f = features[idxs[k]];
            if (isSuppressed(f.x, f.y)) continue;
            occInsert(f.x, f.y);
            Track2D nt;
            nt.id = nextId++;
            nt.markers[frame] = std::make_pair(f.x + rodX1, (rodY2 - 1) - f.y);
            tracks.push_back(nt);
            ++added;
            ++placed;
        }
    }
    return added;
}


// ==================== Feature Tracking ====================

void
CameraTrackerNodePrivate::trackFeatures(bool manualOnly, bool backwards)
{
    try {

    if (tracks.empty()) {
        solveStatusDisplay.lock()->setValue("No features to track — detect features first");
        return;
    }

    NodePtr node = publicInterface->getNode();
    if (!node) return;

    NodePtr inputNode = node->getInput(0);
    if (!inputNode) {
        solveStatusDisplay.lock()->setValue("No input connected");
        return;
    }
    EffectInstancePtr input = inputNode->getEffectInstance();
    if (!input) {
        solveStatusDisplay.lock()->setValue("No input connected");
        return;
    }

    int rangeStart = trackRangeStart.lock()->getValue();
    int rangeEnd = trackRangeEnd.lock()->getValue();
    if (rangeStart >= rangeEnd) {
        solveStatusDisplay.lock()->setValue("Invalid frame range");
        return;
    }

    // Get image dimensions from first frame
    RectD rod;
    RenderScale scale;
    bool isProjectFormat = false;
    StatusEnum stat = input->getRegionOfDefinition_public(inputNode->getHashValue(), static_cast<double>(rangeStart), scale, ViewIdx(0), &rod, &isProjectFormat);
    if (stat != eStatusOK) return;

    int width = static_cast<int>(rod.width());
    int height = static_cast<int>(rod.height());

    // Build a helper to render a frame to a libmv FloatImage
    auto renderFrame = [&](int frame) -> libmv::FloatImage {
        libmv::FloatImage mvImg(height, width, 1);
        RectI roi;
        roi.x1 = static_cast<int>(rod.x1);
        roi.y1 = static_cast<int>(rod.y1);
        roi.x2 = static_cast<int>(rod.x2);
        roi.y2 = static_cast<int>(rod.y2);

        // Setup render context
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        AbortableThread* isAbortable = dynamic_cast<AbortableThread*>(QThread::currentThread());
        if (isAbortable) {
            isAbortable->setAbortInfo(true, abortInfo, node->getEffectInstance());
        }
        ParallelRenderArgsSetter frameRenderArgs(static_cast<double>(frame),
                                                  ViewIdx(0), true, false, abortInfo,
                                                  node, 0,
                                                  node->getApp()->getTimeLine().get(),
                                                  NodePtr(), true, false, RenderStatsPtr());

        std::list<ImagePlaneDesc> comps;
        comps.push_back(ImagePlaneDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args(static_cast<double>(frame),
                                           scale, 0, ViewIdx(0), false,
                                           roi, rod, comps,
                                           eImageBitDepthFloat, true,
                                           node->getEffectInstance().get(), eStorageModeRAM,
                                           static_cast<double>(frame));
        std::map<ImagePlaneDesc, ImagePtr> planes;
        input->renderRoI(args, &planes);
        if (!planes.empty() && planes.begin()->second) {
            ImagePtr img = planes.begin()->second;
            RectI bounds = img->getBounds();
            Image::ReadAccess ra(img.get());
            for (int y = bounds.y1; y < bounds.y2; ++y) {
                for (int x = bounds.x1; x < bounds.x2; ++x) {
                    const float* pix = (const float*)ra.pixelAt(x, y);
                    if (pix) {
                        float lum = 0.2126f * pix[0] + 0.7152f * pix[1] + 0.0722f * pix[2];
                        int mvY = (bounds.y2 - 1) - y;
                        mvImg(mvY, x - bounds.x1, 0) = lum;
                    }
                }
            }
        }
        return mvImg;
    };

    // Detect-at-playhead trap: if the user pressed Detect while parked mid-shot,
    // NO track has a marker on the range's first frame — the whole initial set
    // then dies on the very first tracking step ("tracked 0/N") and the shot is
    // carried entirely by re-detected churn (observed: frames 1-2 unsolvable,
    // position error 15x worse). Auto-detect at range start when that happens.
    {
        bool haveStart = false;
        for (const auto& t : tracks) {
            if (t.markers.count(rangeStart)) { haveStart = true; break; }
        }
        // Manual-only opts out of auto-detection entirely: the explicit Track
        // Manual button, or a session where every track is hand-placed.
        const bool haveAutoTracks = tracks.size() > manualTrackIds.size();
        const bool manualSession = manualOnly || (!manualTrackIds.empty() && !haveAutoTracks);
        if (!haveStart && !manualSession && (haveAutoTracks || manualTrackIds.empty())) {
            CT_DBG("CameraTracker: No tracks on frame %d — auto-detecting at range start\n",
                   rangeStart);
            detectFeatures(rangeStart);
        }
    }

    // Track features using libmv's TrackRegion
    CT_DBG("CameraTracker: Starting tracking %d tracks over frames %d-%d\n",
            (int)tracks.size(), rangeStart, rangeEnd);
    solveStatusDisplay.lock()->setValue("Tracking...");

    // Resolution normalization (reference: 1920px width). The patch, search
    // radius and jump-reject are FIELD-OF-VIEW quantities, not pixel quantities:
    // at UHD the same physical camera move is 2x the pixels, and a 10px patch
    // sees half the scene texture it saw at HD — which is why UHD footage
    // tracked with HD constants produced ~190 usable tracks vs a reference's ~6500.
    const double resScale = std::max(0.5, (double)width / 1920.0);

    const double halfPatchSize = 10.0 * resScale;
    // Manual tracks use the user-set Pattern Size (native pixels) — bigger
    // texture window for deliberate features on soft detail.
    const double manualPatchHalf =
        std::max(4.0, (manualPatchSize.lock() ? manualPatchSize.lock()->getValue() : 40) / 2.0);
    // Per-track resized boxes can exceed the knob size — margin covers the max.
    double maxPatchHalf = std::max(halfPatchSize, manualPatchHalf);
    for (const auto& t : tracks) maxPatchHalf = std::max(maxPatchHalf, t.patternHalf);
    const double edgeMargin = maxPatchHalf + 3.0;
    // Max per-frame motion handled (px) — also the jump-reject bound below.
    const int searchRadius = (int)(100.0 * resScale);
    CT_DBG("CameraTracker: Tracking at %dpx wide -> resScale %.2f (patch %.0f, search %d)\n",
           width, resScale, halfPatchSize * 2, searchRadius);

    // Setup TrackRegion options — fast translation-only mode.
    // NOTE: anchored affine / TRS tracking was tried here (git history) and made this
    // footage WORSE — a fixed reference template can't match self-similar foliage +
    // motion blur across a 25-frame pan, so tracks got less accurate (90% over 5px vs
    // 69% for this frame-to-frame translation tracker). Frame-to-frame wins here
    // because consecutive frames look near-identical. Kept translation as the proven
    // baseline; the real quality lever is bundle robustness / weak-parallax handling.
    libmv::TrackRegionOptions trackOptions;
    trackOptions.mode = libmv::TrackRegionOptions::TRANSLATION;
    trackOptions.minimum_correlation = 0.7;
    trackOptions.max_iterations = 16;
    trackOptions.use_esm = false;
    trackOptions.use_brute_initialization = true;
    trackOptions.attempt_refine_before_brute = true;
    trackOptions.use_normalized_intensities = false;
    trackOptions.sigma = 0.9;
    trackOptions.num_extra_points = 0;
    trackOptions.image1_mask = NULL;

    // Cache previous frame image
    CT_DBG("CameraTracker: Rendering frame %d...\n", rangeStart);
    // Backwards mode (manual tracks placed mid-clip): identical machinery,
    // frames visited in reverse — "previous" is frame+1.
    const int step = backwards ? -1 : 1;
    const int firstFrame = backwards ? rangeEnd : rangeStart;
    const int lastFrame  = backwards ? rangeStart : rangeEnd;
    libmv::FloatImage prevImage = renderFrame(firstFrame);
    CT_DBG("CameraTracker: Frame %d rendered (%dx%d)\n", rangeStart, prevImage.Width(), prevImage.Height());

    // Index list for the parallel map (grows as re-detection adds tracks).
    // Track Manual: only the hand-placed tracks take part.
    std::vector<std::size_t> trackIndexes;
    trackIndexes.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (manualOnly && !manualTrackIds.count(tracks[i].id)) continue;
        trackIndexes.push_back(i);
    }
    if (manualOnly && trackIndexes.empty()) {
        solveStatusDisplay.lock()->setValue("No manual tracks to track — enable Add Track Mode "
                                            "and place some first");
        return;
    }

    // Per-track constant-velocity state: {dx, dy, valid}. Prediction turns the
    // huge brute search into a tiny local refinement (the predictive-search recipe).
    std::vector<std::array<double, 3> > trackVel(tracks.size(), {0.0, 0.0, 0.0});

    // Planar-baked grid markers are geometric products of the planar tracker —
    // Track Features must never overwrite them.
    std::set<int> bakedIds;
    for (const auto& p : planarTracks) bakedIds.insert(p.bakedIds.begin(), p.bakedIds.end());

    // --- Track re-association across occlusions (descriptor matching) ---
    // When a track dies, keep its last appearance (a small luma patch) for a
    // few frames. When re-detection later births a feature NEAR the dead
    // track's predicted position that MATCHES its patch, RESUME the old track
    // id instead of minting a new one — one long track with a gap instead of
    // two fragments. Long tracks carry the accumulated parallax (a reference tracker's low
    // fragmentation comes from exactly this).
    struct DeadTrack {
        std::size_t index;     // index into `tracks`
        int lastFrame;
        double x, y;           // last position (libmv top-down coords)
        double vx, vy;         // last velocity
        std::vector<float> patch;
    };
    std::vector<DeadTrack> deadPool;
    const int kReassocMaxAge = 12;                       // frames a dead track stays matchable
    const int kPatchHalf = 8;                            // 17x17 descriptor patch
    const double kReassocGate = 30.0 * resScale;         // px around the predicted position
    const double kReassocZncc = 0.7;

    auto grabPatch = [&](const libmv::FloatImage& img, double cx, double cy,
                         std::vector<float>* out) -> bool {
        const int ix = (int)std::floor(cx), iy = (int)std::floor(cy);
        if (ix - kPatchHalf < 0 || iy - kPatchHalf < 0 ||
            ix + kPatchHalf >= width || iy + kPatchHalf >= height) return false;
        out->clear();
        out->reserve((2 * kPatchHalf + 1) * (2 * kPatchHalf + 1));
        for (int yy = -kPatchHalf; yy <= kPatchHalf; ++yy)
            for (int xx = -kPatchHalf; xx <= kPatchHalf; ++xx)
                out->push_back(img(iy + yy, ix + xx, 0));
        return true;
    };
    auto zncc = [](const std::vector<float>& a, const std::vector<float>& b) -> double {
        if (a.size() != b.size() || a.empty()) return -1.0;
        double ma = 0, mb = 0;
        for (std::size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
        ma /= a.size(); mb /= b.size();
        double num = 0, da = 0, db = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const double xa = a[i] - ma, xb = b[i] - mb;
            num += xa * xb; da += xa * xa; db += xb * xb;
        }
        const double den = std::sqrt(da * db);
        return den > 1e-12 ? num / den : -1.0;
    };

    // Global frame-to-frame translation estimate: coarse block match on a
    // 1/8-res luma. Seeds the prediction for fresh tracks and pans; costs ~1ms.
    auto estimateGlobalShift = [&](const libmv::FloatImage& a, const libmv::FloatImage& b,
                                   double& gx, double& gy) {
        gx = gy = 0.0;
        const int ds = 8;
        const int sw = width / ds, sh = height / ds;
        if (sw < 48 || sh < 48) return;
        std::vector<float> A((size_t)sw * sh), B((size_t)sw * sh);
        for (int y = 0; y < sh; ++y) {
            for (int x = 0; x < sw; ++x) {
                float sa = 0, sb = 0;
                for (int yy = 0; yy < ds; yy += 2)         // sample 16 of 64 px per block
                    for (int xx = 0; xx < ds; xx += 2) {
                        sa += a(y * ds + yy, x * ds + xx, 0);
                        sb += b(y * ds + yy, x * ds + xx, 0);
                    }
                A[(size_t)y * sw + x] = sa; B[(size_t)y * sw + x] = sb;
            }
        }
        const int rad = std::min(24, std::max(8, searchRadius / ds));
        const int m = rad + 2;                             // valid interior margin
        auto sad = [&](int ox, int oy) -> double {
            double s = 0; int n = 0;
            for (int y = m; y < sh - m; y += 3) {
                for (int x = m; x < sw - m; x += 3) {
                    s += std::abs((double)A[(size_t)y * sw + x]
                                - (double)B[(size_t)(y + oy) * sw + (x + ox)]);
                    ++n;
                }
            }
            return n ? s / n : 1e30;
        };
        int bx = 0, by = 0; double best = 1e30;
        for (int oy = -rad; oy <= rad; oy += 3)            // coarse
            for (int ox = -rad; ox <= rad; ox += 3) {
                const double s = sad(ox, oy);
                if (s < best) { best = s; bx = ox; by = oy; }
            }
        for (int oy = by - 2; oy <= by + 2; ++oy)          // refine
            for (int ox = bx - 2; ox <= bx + 2; ++ox) {
                if (std::abs(ox) > rad || std::abs(oy) > rad) continue;
                const double s = sad(ox, oy);
                if (s < best) { best = s; bx = ox; by = oy; }
            }
        gx = (double)bx * ds; gy = (double)by * ds;
    };

    // --- Adaptive re-detection (continuous replenishment) ---
    // As the camera pans, tracks drain out of frame and are never replaced by the
    // detect-once model, so a long move ends up solving on a handful of survivors.
    // When the live-track count on a frame falls below a fraction of the initial
    // count, detect fresh features in the emptied regions and track them onward,
    // building overlapping tracks that span the whole shot.
    const bool doRedetect = redetectEnabled.lock()->getValue();
    const double redetectRatio = redetectMinRatio.lock()->getValue();
    const int initialTrackCount = (int)tracks.size();
    const int redetectCooldown = 2;   // min frames between re-detects (denser replenishment)
    int lastRedetectFrame = rangeStart;

    for (int frame = firstFrame + step;
         backwards ? (frame >= lastFrame) : (frame <= lastFrame);
         frame += step) {
        const auto tFrame0 = std::chrono::steady_clock::now();
        libmv::FloatImage curImage = renderFrame(frame);
        const auto tRender = std::chrono::steady_clock::now();

        // Dominant camera motion this frame — the prediction seed.
        double gShiftX = 0.0, gShiftY = 0.0;
        estimateGlobalShift(prevImage, curImage, gShiftX, gShiftY);

        // Track one feature from frame-1 to frame. Thread-safe: reads the shared
        // const prev/cur images + trackOptions, and writes ONLY its own track's
        // marker map (distinct element per index). Returns true iff it produced a
        // marker at this frame. `frame` captured by value; everything else by ref
        // (all read-only during the parallel section, which the barrier below joins
        // before the next iteration mutates prevImage).
        std::function<bool(const std::size_t&)> trackOne = [&, frame](const std::size_t& i) -> bool {
            Track2D& track = tracks[i];
            // Manual tracks: a hand-placed marker on this frame is a USER KEY —
            // keep it and count the track as alive (the chain continues from it
            // next frame). Tracking only fills the un-keyed frames.
            if (manualTrackIds.count(track.id) && track.markers.count(frame)) {
                return true;
            }
            // Planar-baked grid markers are geometric — never re-track them.
            if (bakedIds.count(track.id)) {
                return track.markers.count(frame) > 0;
            }
            std::map<int, std::pair<double, double> >::const_iterator prevIt = track.markers.find(frame - step);
            if (prevIt == track.markers.end()) {
                return false;
            }
            double prevX = prevIt->second.first;
            double prevY = prevIt->second.second;
            double mvPrevX = prevX - rod.x1;
            double mvPrevY = (rod.y2 - 1) - prevY;
            if (mvPrevX < edgeMargin || mvPrevX >= width - edgeMargin ||
                mvPrevY < edgeMargin || mvPrevY >= height - edgeMargin) {
                return false;
            }

            // --- Predict, then refine (the predictive-search recipe) ---
            // Prediction: per-track constant velocity when the track moved last
            // frame; otherwise the global (dominant camera) shift. The search
            // then only needs a SMALL window around the prediction — brute cost
            // drops ~100x vs searching around the previous position.
            double predX, predY;
            if (trackVel[i][2] > 0.5) {
                predX = mvPrevX + trackVel[i][0];
                predY = mvPrevY + trackVel[i][1];
            } else {
                predX = mvPrevX + gShiftX;
                predY = mvPrevY + gShiftY;
            }
            predX = std::min((double)width - edgeMargin, std::max((double)edgeMargin, predX));
            predY = std::min((double)height - edgeMargin, std::max((double)edgeMargin, predY));

            // Per-track patch: manual tracks use the user's Pattern Size.
            double hp = manualTrackIds.count(track.id) ? manualPatchHalf : halfPatchSize;
            if (track.patternHalf > 0) hp = track.patternHalf;   // per-track resized box

            // One tracking attempt: template around the previous position in the
            // PREV image; search window of `search` px around the prediction in
            // the CUR image. Returns success + the new position.
            auto attempt = [&](int search, double& outX, double& outY) -> bool {
                const int pm = (int)hp + 4;
                int px0 = std::max(0, (int)std::floor(mvPrevX) - pm);
                int py0 = std::max(0, (int)std::floor(mvPrevY) - pm);
                int px1 = std::min(width,  (int)std::ceil(mvPrevX) + pm);
                int py1 = std::min(height, (int)std::ceil(mvPrevY) + pm);
                const int cm = (int)hp + search;
                int cx0 = std::max(0, (int)std::floor(predX) - cm);
                int cy0 = std::max(0, (int)std::floor(predY) - cm);
                int cx1 = std::min(width,  (int)std::ceil(predX) + cm);
                int cy1 = std::min(height, (int)std::ceil(predY) + cm);
                const int minWin = (int)(2 * hp) + 2;
                if (px1 - px0 < minWin || py1 - py0 < minWin ||
                    cx1 - cx0 < minWin || cy1 - cy0 < minWin) {
                    return false;
                }
                libmv::FloatImage prevWin(py1 - py0, px1 - px0, 1);
                libmv::FloatImage curWin(cy1 - cy0, cx1 - cx0, 1);
                for (int yy = py0; yy < py1; ++yy)
                    for (int xx = px0; xx < px1; ++xx)
                        prevWin(yy - py0, xx - px0, 0) = prevImage(yy, xx, 0);
                for (int yy = cy0; yy < cy1; ++yy)
                    for (int xx = cx0; xx < cx1; ++xx)
                        curWin(yy - cy0, xx - cx0, 0) = curImage(yy, xx, 0);

                const double lpx = mvPrevX - px0, lpy = mvPrevY - py0;
                const double lqx = predX - cx0,   lqy = predY - cy0;
                double x1[4], y1[4], x2[4], y2[4];
                x1[0] = lpx - hp; y1[0] = lpy - hp;
                x1[1] = lpx + hp; y1[1] = lpy - hp;
                x1[2] = lpx + hp; y1[2] = lpy + hp;
                x1[3] = lpx - hp; y1[3] = lpy + hp;
                x2[0] = lqx - hp; y2[0] = lqy - hp;
                x2[1] = lqx + hp; y2[1] = lqy - hp;
                x2[2] = lqx + hp; y2[2] = lqy + hp;
                x2[3] = lqx - hp; y2[3] = lqy + hp;

                libmv::TrackRegionResult result;
                result.termination = libmv::TrackRegionResult::FAILURE;
                result.correlation = 0;
                try {
                    libmv::TrackRegion(prevWin, curWin, x1, y1, trackOptions, x2, y2, &result);
                } catch (...) { return false; }
                if (!result.is_usable()) return false;
                const double nx = (x2[0] + x2[1] + x2[2] + x2[3]) / 4.0 + cx0;
                const double ny = (y2[0] + y2[1] + y2[2] + y2[3]) / 4.0 + cy0;
                // reject results at/beyond the search rim (likely a rim minimum)
                const double ddx = nx - predX, ddy = ny - predY;
                if (ddx * ddx + ddy * ddy > (double)search * search) return false;
                outX = nx; outY = ny;
                return true;
            };

            const int smallSearch = std::max(8, (int)(20.0 * resScale));
            double newMvX = 0, newMvY = 0;
            bool ok = attempt(smallSearch, newMvX, newMvY);
            if (!ok) {
                // prediction missed — full-radius fallback (the old behavior)
                ok = attempt(searchRadius, newMvX, newMvY);
            }
            if (!ok) {
                trackVel[i][2] = 0.0;   // stale velocity — don't trust it next frame
                return false;
            }

            trackVel[i][0] = newMvX - mvPrevX;
            trackVel[i][1] = newMvY - mvPrevY;
            trackVel[i][2] = 1.0;
            track.markers[frame] = std::make_pair(newMvX + rod.x1, (rod.y2 - 1) - newMvY);
            return true;
        };

        // Fan the independent per-track work out over Natron's global thread pool
        // (honours the user's max-thread-count setting), then join. Same pattern as
        // TrackerContext::trackStepFunctor.
        QFuture<bool> future = QtConcurrent::mapped(trackIndexes, trackOne);
        future.waitForFinished();

        int trackedCount = 0;
        for (QFuture<bool>::const_iterator it = future.begin(); it != future.end(); ++it) {
            if (*it) {
                ++trackedCount;
            }
        }

        // Record deaths for re-association: alive last frame, gone this frame.
        // (Patch grabbed from prevImage at the last-known position — still the
        // pre-swap image here.) Manual and baked tracks are never auto-resumed.
        for (std::size_t ti = 0; ti < trackIndexes.size(); ++ti) {
            const std::size_t i = trackIndexes[ti];
            const Track2D& tr2 = tracks[i];
            if (manualTrackIds.count(tr2.id) || bakedIds.count(tr2.id)) continue;
            if (!tr2.markers.count(frame - step) || tr2.markers.count(frame)) continue;
            const std::pair<double, double>& m = tr2.markers.at(frame - step);
            DeadTrack d;
            d.index = i;
            d.lastFrame = frame - step;
            d.x = m.first - rod.x1;
            d.y = (rod.y2 - 1) - m.second;
            d.vx = trackVel[i][0];
            d.vy = trackVel[i][1];
            if (grabPatch(prevImage, d.x, d.y, &d.patch)) {
                deadPool.push_back(d);
            }
        }
        // Age out stale entries.
        for (std::size_t di = deadPool.size(); di > 0; --di) {
            if (frame - deadPool[di - 1].lastFrame > kReassocMaxAge) {
                deadPool.erase(deadPool.begin() + (di - 1));
            }
        }

        // Replenish coverage before advancing: if this frame has lost too many
        // tracks and we're off cooldown (and not the last frame — new tracks need a
        // future frame to track into), detect fresh features in the emptied regions
        // and add them to the parallel map so they track from here onward.
        // Re-detection never fires in manual-only tracking (explicit button OR a
        // session where every track is hand-placed) — a dying manual track must
        // not summon 150 auto features (observed at frame 117 of a 3-track run).
        const bool allowRedetect = !manualOnly && tracks.size() > manualTrackIds.size();
        if (allowRedetect && doRedetect && !backwards && frame < rangeEnd &&
            (frame - lastRedetectFrame) >= redetectCooldown &&
            trackedCount < redetectRatio * initialTrackCount) {
            std::size_t before = tracks.size();
            int added = redetectFeatures(curImage, frame, width, height, rod.x1, rod.y2);
            for (std::size_t i = before; i < tracks.size(); ++i) {
                trackIndexes.push_back(i);
            }
            trackVel.resize(tracks.size(), {0.0, 0.0, 0.0});

            // --- Re-associate new detections with recently-dead tracks ---
            // A new feature born near a dead track's PREDICTED position whose
            // appearance matches the stored patch RESUMES the old track: its
            // marker moves onto the old id and the fresh track becomes an
            // empty husk (skipped everywhere). The resumed track continues
            // tracking next frame from the new marker.
            int reassociated = 0;
            if (!deadPool.empty()) {
                for (std::size_t i = before; i < tracks.size(); ++i) {
                    if (tracks[i].markers.empty()) continue;
                    const int mFrame = tracks[i].markers.begin()->first;
                    const std::pair<double, double>& nm = tracks[i].markers.begin()->second;
                    const double mvX = nm.first - rod.x1;
                    const double mvY = (rod.y2 - 1) - nm.second;

                    int bestDead = -1;
                    double bestScore = kReassocZncc;
                    std::vector<float> candPatch;
                    if (!grabPatch(curImage, mvX, mvY, &candPatch)) continue;
                    for (std::size_t d = 0; d < deadPool.size(); ++d) {
                        const DeadTrack& dt = deadPool[d];
                        const int age = mFrame - dt.lastFrame;
                        if (age <= 0 || age > kReassocMaxAge) continue;
                        const double px = dt.x + dt.vx * age;
                        const double py = dt.y + dt.vy * age;
                        if (std::hypot(mvX - px, mvY - py) > kReassocGate) continue;
                        const double score = zncc(dt.patch, candPatch);
                        if (score > bestScore) { bestScore = score; bestDead = (int)d; }
                    }
                    if (bestDead >= 0) {
                        const DeadTrack& dt = deadPool[bestDead];
                        tracks[dt.index].markers[mFrame] = nm;   // resume old id
                        tracks[i].markers.clear();               // husk out the new one
                        trackVel[dt.index] = {0.0, 0.0, 0.0};    // re-seed from global shift
                        deadPool.erase(deadPool.begin() + bestDead);
                        ++reassociated;
                    }
                }
            }
            if (reassociated > 0) {
                CT_DBG("CameraTracker: Frame %d — re-associated %d track(s) across gaps\n",
                       frame, reassociated);
            }
            lastRedetectFrame = frame;
            CT_DBG("CameraTracker: Frame %d — re-detected %d new features (live was %d/%d)\n",
                    frame, added, trackedCount, initialTrackCount);
        }

        prevImage = curImage;
        const auto tEnd = std::chrono::steady_clock::now();
        const double msRender = std::chrono::duration<double, std::milli>(tRender - tFrame0).count();
        const double msTotal = std::chrono::duration<double, std::milli>(tEnd - tFrame0).count();
        CT_DBG("CameraTracker: Frame %d/%d — tracked %d/%d, shift (%+.0f,%+.0f), "
               "%.0f ms (%.0f render + %.0f track)\n",
                frame, rangeEnd, trackedCount, (int)tracks.size(),
                gShiftX, gShiftY, msTotal, msRender, msTotal - msRender);
    }

    // Report results
    int longTracks = 0;
    int minLength = (rangeEnd - rangeStart) / 4; // At least 25% of range
    for (const auto& t : tracks) {
        if (static_cast<int>(t.markers.size()) > minLength) longTracks++;
    }

    std::stringstream ss;
    ss << "Tracking complete. " << tracks.size() << " total tracks, "
       << longTracks << " long tracks (>" << minLength << " frames)";
    solveStatusDisplay.lock()->setValue(ss.str());
    refreshManualList();

    } catch (const std::exception& e) {
        solveStatusDisplay.lock()->setValue(std::string("Track failed: ") + e.what());
    } catch (...) {
        solveStatusDisplay.lock()->setValue("Track failed: unknown error");
    }
}


// ==================== Planar region tracking ====================

void
CameraTrackerNodePrivate::trackPlanarRegions()
{
    if (planarTracks.empty()) {
        solveStatusDisplay.lock()->setValue("No planar regions drawn — enable Add Planar Mode "
                                            "and drag a box around a flat feature first");
        return;
    }
    NodePtr node = publicInterface->getNode();
    if (!node) return;
    NodePtr inputNode = node->getInput(0);
    EffectInstancePtr input = inputNode ? inputNode->getEffectInstance() : EffectInstancePtr();
    if (!input) {
        solveStatusDisplay.lock()->setValue("No input connected");
        return;
    }
    const int rangeStart = trackRangeStart.lock()->getValue();
    const int rangeEnd = trackRangeEnd.lock()->getValue();

    RectD rod; RenderScale scale; bool ipf = false;
    if (input->getRegionOfDefinition_public(inputNode->getHashValue(),
            (double)rangeStart, scale, ViewIdx(0), &rod, &ipf) != eStatusOK) {
        return;
    }
    const int width = (int)rod.width();
    const int height = (int)rod.height();
    const double resScale = std::max(0.5, (double)width / 1920.0);

    // Optional ROI (mv top-down coords): planar tracking only ever looks at
    // a window around the quad — rendering the full UHD frame per step was
    // where all the time went (~400 full-frame renders per Track press).
    auto renderFrame = [&](int frame, int mvx0 = -1, int mvy0 = -1,
                           int mvx1 = -1, int mvy1 = -1) -> libmv::FloatImage {
        libmv::FloatImage mvImg(height, width, 1);
        mvImg.fill(0.0f);
        RectI roi;
        if (mvx0 < 0) {
            roi.x1 = (int)rod.x1; roi.y1 = (int)rod.y1;
            roi.x2 = (int)rod.x2; roi.y2 = (int)rod.y2;
        } else {
            roi.x1 = std::max((int)rod.x1, (int)rod.x1 + mvx0);
            roi.x2 = std::min((int)rod.x2, (int)rod.x1 + mvx1 + 1);
            roi.y1 = std::max((int)rod.y1, (int)rod.y2 - 1 - mvy1);
            roi.y2 = std::min((int)rod.y2, (int)rod.y2 - mvy0);
            if (roi.x2 <= roi.x1 || roi.y2 <= roi.y1) return mvImg;
        }
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        AbortableThread* isAbortable = dynamic_cast<AbortableThread*>(QThread::currentThread());
        if (isAbortable) {
            isAbortable->setAbortInfo(true, abortInfo, node->getEffectInstance());
        }
        ParallelRenderArgsSetter frameRenderArgs((double)frame, ViewIdx(0), true, false,
                                                 abortInfo, node, 0,
                                                 node->getApp()->getTimeLine().get(),
                                                 NodePtr(), true, false, RenderStatsPtr());
        std::list<ImagePlaneDesc> comps;
        comps.push_back(ImagePlaneDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args((double)frame, scale, 0, ViewIdx(0), false,
                                           roi, rod, comps, eImageBitDepthFloat, true,
                                           node->getEffectInstance().get(), eStorageModeRAM,
                                           (double)frame);
        std::map<ImagePlaneDesc, ImagePtr> planes;
        input->renderRoI(args, &planes);
        if (!planes.empty() && planes.begin()->second) {
            ImagePtr img = planes.begin()->second;
            RectI bounds = img->getBounds();
            Image::ReadAccess ra(img.get());
            for (int y = bounds.y1; y < bounds.y2; ++y) {
                for (int x = bounds.x1; x < bounds.x2; ++x) {
                    const float* pix = (const float*)ra.pixelAt(x, y);
                    if (pix) {
                        float lum = 0.2126f * pix[0] + 0.7152f * pix[1] + 0.0722f * pix[2];
                        mvImg(((int)rod.y2 - 1) - y, x - (int)rod.x1, 0) = lum;
                    }
                }
            }
        }
        return mvImg;
    };

    // Whole-region homography tracking: the big textured patch constrains the
    // 8-DOF warp well (the earlier auto-track affine failure was tiny patches —
    // this is the opposite regime).
    libmv::TrackRegionOptions opts;
    {
        static const libmv::TrackRegionOptions::Mode kModes[6] = {
            libmv::TrackRegionOptions::TRANSLATION,
            libmv::TrackRegionOptions::TRANSLATION_ROTATION,
            libmv::TrackRegionOptions::TRANSLATION_SCALE,
            libmv::TrackRegionOptions::TRANSLATION_ROTATION_SCALE,
            libmv::TrackRegionOptions::AFFINE,
            libmv::TrackRegionOptions::HOMOGRAPHY,
        };
        const int mm = planarMotionModel.lock() ? planarMotionModel.lock()->getValue() : 5;
        opts.mode = kModes[std::max(0, std::min(5, mm))];
    }
    opts.minimum_correlation = 0.6;
    opts.max_iterations = 50;
    opts.use_esm = true;
    opts.use_brute_initialization = false;
    opts.use_normalized_intensities = true;   // robust to lighting drift across the shot
    opts.sigma = 0.9;
    // Anti-jiggle: penalizes warps that change the patch SHAPE dramatically
    // ("the quad corners jiggle around even though the center is well
    // estimated" — libmv's own docs, describing exactly our symptom).
    opts.regularization_coefficient = 3.0;
    opts.num_extra_points = 0;
    opts.image1_mask = NULL;

    // Crop a window around a quad (top-down libmv coords) with margin; returns
    // the window and its origin.
    auto cropQuadWindow = [&](const libmv::FloatImage& img, const double* qx, const double* qy,
                              int margin, int& wx0, int& wy0) -> libmv::FloatImage {
        int x0 = width, y0 = height, x1 = 0, y1 = 0;
        for (int c = 0; c < 4; ++c) {
            x0 = std::min(x0, (int)std::floor(qx[c])); x1 = std::max(x1, (int)std::ceil(qx[c]));
            y0 = std::min(y0, (int)std::floor(qy[c])); y1 = std::max(y1, (int)std::ceil(qy[c]));
        }
        x0 = std::max(0, x0 - margin); y0 = std::max(0, y0 - margin);
        x1 = std::min(width, x1 + margin); y1 = std::min(height, y1 + margin);
        const int w = std::max(0, x1 - x0), h = std::max(0, y1 - y0);
        libmv::FloatImage win(std::max(1, h), std::max(1, w), 1);
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx)
                win(yy, xx, 0) = img(y0 + yy, x0 + xx, 0);
        wx0 = x0; wy0 = y0;
        return win;
    };
    auto toMv = [&](const std::array<double, 8>& q, double* qx, double* qy) {
        for (int c = 0; c < 4; ++c) {
            qx[c] = q[c * 2] - rod.x1;
            qy[c] = (rod.y2 - 1) - q[c * 2 + 1];
        }
    };

    // ==================== Points-first planar tracking ====================
    // Architecture inversion (2026-07-06, after a day of region-first
    // failures): the validated point tracker CARRIES the plane. Homographies
    // are fitted DIRECTLY reference-frame -> current-frame from point tracks
    // inside the quad (manual tracks pin the plane when >=4 qualify), so
    // there is no per-frame chaining and therefore no accumulated drift.
    // When track turnover exhausts the direct share, the reference is
    // PROMOTED to the last solved frame (a handful of hops per shot instead
    // of one per frame). The region matcher is demoted to a bounded POLISH:
    // it may refine the points quad by a few pixels, it can never veto it —
    // every failure mode fought today (out-of-frame, glass reflections, dark
    // holes, template staleness) belonged to the region matcher as PRIMARY.

    typedef std::vector<std::pair<std::pair<double,double>, std::pair<double,double> > > CorrVec;
    // Least-squares affine (6 DOF) from >=3 correspondences.
    auto affineFromCorr = [](const CorrVec& cc, Eigen::Matrix3d* H) -> bool {
        const int n = (int)cc.size();
        if (n < 3) return false;
        Eigen::MatrixXd A(n, 3);
        Eigen::VectorXd bu(n), bv(n);
        for (int i = 0; i < n; ++i) {
            A(i,0) = cc[i].first.first;
            A(i,1) = cc[i].first.second;
            A(i,2) = 1.0;
            bu(i) = cc[i].second.first;
            bv(i) = cc[i].second.second;
        }
        const Eigen::Vector3d ru = A.colPivHouseholderQr().solve(bu);
        const Eigen::Vector3d rv = A.colPivHouseholderQr().solve(bv);
        *H = Eigen::Matrix3d::Identity();
        (*H)(0,0) = ru(0); (*H)(0,1) = ru(1); (*H)(0,2) = ru(2);
        (*H)(1,0) = rv(0); (*H)(1,1) = rv(1); (*H)(1,2) = rv(2);
        return true;
    };
    // Exact similarity (4 DOF: scale/rotation/translation) from 2 correspondences.
    auto similarityFrom2 = [](const CorrVec& cc, Eigen::Matrix3d* H) -> bool {
        if (cc.size() < 2) return false;
        const double x1 = cc[0].first.first,  y1 = cc[0].first.second;
        const double x2 = cc[1].first.first,  y2 = cc[1].first.second;
        const double u1 = cc[0].second.first, v1 = cc[0].second.second;
        const double u2 = cc[1].second.first, v2 = cc[1].second.second;
        const double dx = x2 - x1, dy = y2 - y1;
        const double den = dx*dx + dy*dy;
        if (den < 1e-9) return false;
        const double du = u2 - u1, dv = v2 - v1;
        const double a = (dx*du + dy*dv) / den;   // s*cos
        const double b = (dx*dv - dy*du) / den;   // s*sin
        *H = Eigen::Matrix3d::Identity();
        (*H)(0,0) =  a; (*H)(0,1) = -b; (*H)(0,2) = u1 - (a*x1 - b*y1);
        (*H)(1,0) =  b; (*H)(1,1) =  a; (*H)(1,2) = v1 - (b*x1 + a*y1);
        return true;
    };

    // Plane fit fa->fb from point tracks inside the quad at fa.
    // modeOut: 0 = autos RANSAC homography, 1 = manual-pinned homography (4+),
    // 2 = manual affine (3), 3 = manual similarity (2), 4 = autos affine
    // (extrapolation guard). Manual pinning degrades GRACEFULLY: losing a
    // corner manual off-frame drops the DOF, it never dumps the vote back to
    // autos (which may sit on off-plane content — the doorway-hole pop).
    auto fitPlaneH = [&](int fa, int fb, const std::array<double, 8>& quadAtFa,
                         const std::vector<std::array<double, 8> >& extraRegions,
                         Eigen::Matrix3d* H, int* inliersOut, int* nCorrOut,
                         int* modeOut) -> bool {
        // Harvest region = this quad's bbox plus every COPLANAR group
        // member's — pooled support means no corner is ever far from data.
        std::vector<std::array<double, 4> > boxes;
        auto addBox = [&](const std::array<double, 8>& q) {
            double a0 = 1e18, b0 = 1e18, a1 = -1e18, b1 = -1e18;
            for (int c = 0; c < 4; ++c) {
                a0 = std::min(a0, q[c*2]);   a1 = std::max(a1, q[c*2]);
                b0 = std::min(b0, q[c*2+1]); b1 = std::max(b1, q[c*2+1]);
            }
            const double bm = 0.15 * std::max(a1 - a0, b1 - b0);
            std::array<double, 4> bb = {{a0 - bm, b0 - bm, a1 + bm, b1 + bm}};
            boxes.push_back(bb);
        };
        addBox(quadAtFa);
        for (const auto& q : extraRegions) addBox(q);
        auto inRegion = [&](double x, double y) {
            for (const auto& bb : boxes)
                if (x >= bb[0] && x <= bb[2] && y >= bb[1] && y <= bb[3]) return true;
            return false;
        };
        std::vector<std::pair<std::pair<double,double>, std::pair<double,double> > > corr, manualCorr;
        for (const auto& t : tracks) {
            auto ma = t.markers.find(fa), mb = t.markers.find(fb);
            if (ma == t.markers.end() || mb == t.markers.end()) continue;
            if (!inRegion(ma->second.first, ma->second.second)) continue;
            if (manualTrackIds.count(t.id)) manualCorr.push_back(std::make_pair(ma->second, mb->second));
            corr.push_back(std::make_pair(ma->second, mb->second));
        }
        // Manual tracks pin the plane (deliberate statements of WHERE it is;
        // autos may sit on recessed openings / glass at other depths). With
        // 2-3 manuals left (a corner went off-frame), fit a REDUCED model
        // from them instead of falling back to possibly-polluted autos.
        *modeOut = 0;
        *nCorrOut = (int)corr.size();
        if ((int)manualCorr.size() == 3) {
            if (affineFromCorr(manualCorr, H)) {
                *inliersOut = 3;
                *modeOut = 2;
                return true;
            }
        } else if ((int)manualCorr.size() == 2) {
            if (similarityFrom2(manualCorr, H)) {
                *inliersOut = 2;
                *modeOut = 3;
                return true;
            }
        }
        if ((int)manualCorr.size() >= 4) { corr.swap(manualCorr); *modeOut = 1; }
        *nCorrOut = (int)corr.size();
        if ((int)corr.size() < 4) return false;
        const double thr = 2.0 * resScale;
        unsigned rng = 77u + (unsigned)corr.size() * 2654435761u
                     + (unsigned)fa * 7919u + (unsigned)fb * 104729u;
        auto rnd = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng; };
        int best = -1;
        Eigen::Matrix3d bestH;
        for (int it2 = 0; it2 < 64; ++it2) {
            int idx[4];
            for (int s2 = 0; s2 < 4; ++s2) {
                bool dup;
                do {
                    idx[s2] = (int)(rnd() % (unsigned)corr.size());
                    dup = false;
                    for (int q2i = 0; q2i < s2; ++q2i)
                        if (idx[q2i] == idx[s2]) { dup = true; break; }
                } while (dup);
            }
            double q1[8], q2[8];
            for (int s2 = 0; s2 < 4; ++s2) {
                q1[s2*2] = corr[idx[s2]].first.first;   q1[s2*2+1] = corr[idx[s2]].first.second;
                q2[s2*2] = corr[idx[s2]].second.first;  q2[s2*2+1] = corr[idx[s2]].second.second;
            }
            Eigen::Matrix3d Hc;
            if (!homographyFrom4Pts(q1, q2, &Hc)) continue;
            int nin = 0;
            for (const auto& cp : corr) {
                Eigen::Vector3d pp(cp.first.first, cp.first.second, 1.0);
                Eigen::Vector3d qq = Hc * pp;
                if (std::abs(qq(2)) < 1e-12) continue;
                const double dx2 = qq(0)/qq(2) - cp.second.first;
                const double dy2 = qq(1)/qq(2) - cp.second.second;
                if (dx2*dx2 + dy2*dy2 < thr*thr) ++nin;
            }
            if (nin > best) { best = nin; bestH = Hc; }
        }
        if (best < 4) return false;
        // Collect the winning sample's inliers once.
        CorrVec inl;
        double ix0 = 1e18, iy0 = 1e18, ix1 = -1e18, iy1 = -1e18;
        {
            const double thr2 = thr * thr;
            for (const auto& cp : corr) {
                Eigen::Vector3d pp(cp.first.first, cp.first.second, 1.0);
                Eigen::Vector3d qq = bestH * pp;
                if (std::abs(qq(2)) < 1e-12) continue;
                const double dx2 = qq(0)/qq(2) - cp.second.first;
                const double dy2 = qq(1)/qq(2) - cp.second.second;
                if (dx2*dx2 + dy2*dy2 >= thr2) continue;
                inl.push_back(cp);
                ix0 = std::min(ix0, cp.first.first);  ix1 = std::max(ix1, cp.first.first);
                iy0 = std::min(iy0, cp.first.second); iy1 = std::max(iy1, cp.first.second);
            }
        }
        // LS refit over ALL inliers (the standard RANSAC final step): the
        // 4-point exact sample flips subsets frame-to-frame = quad jitter.
        if ((int)inl.size() >= 4) {
            Eigen::Matrix3d Hr;
            if (homographyFromNPts(inl, &Hr)) bestH = Hr;
        }
        // Extrapolation guard (ALL modes): a quad corner far outside the
        // inliers' extent is an extrapolation — homography perspective terms
        // amplify fit noise with distance (the wobbling off-screen corner).
        // Use the affine fitted from the same inliers out there instead.
        {
            const double gm = 0.25 * std::max(ix1 - ix0, iy1 - iy0);
            bool extrapolated = false;
            for (int c = 0; c < 4; ++c) {
                if (quadAtFa[c*2]   < ix0 - gm || quadAtFa[c*2]   > ix1 + gm ||
                    quadAtFa[c*2+1] < iy0 - gm || quadAtFa[c*2+1] > iy1 + gm) {
                    extrapolated = true;
                    break;
                }
            }
            if (extrapolated && (int)inl.size() >= 3) {
                Eigen::Matrix3d Ha;
                if (affineFromCorr(inl, &Ha)) {
                    *H = Ha;
                    *inliersOut = (int)inl.size();
                    *modeOut = (*modeOut == 1) ? 2 : 4;
                    return true;
                }
            }
        }
        *H = bestH;
        *inliersOut = (int)inl.size();
        return true;
    };

    // Largest centroid-scale keeping a quad (mv coords) inside the image —
    // used only to decide whether the polish step may run.
    auto shrinkToFit = [&](const double* qx, const double* qy) -> double {
        double cx = 0, cy = 0;
        for (int c = 0; c < 4; ++c) { cx += qx[c]; cy += qy[c]; }
        cx /= 4; cy /= 4;
        const double m = 4.0;
        if (cx < m || cx > width - 1 - m || cy < m || cy > height - 1 - m) return 0.0;
        double s = 1.0;
        for (int c = 0; c < 4; ++c) {
            const double dx = qx[c] - cx, dy = qy[c] - cy;
            if (dx < 0 && cx + dx < m)              s = std::min(s, (m - cx) / dx);
            if (dx > 0 && cx + dx > width - 1 - m)  s = std::min(s, (width - 1 - m - cx) / dx);
            if (dy < 0 && cy + dy < m)              s = std::min(s, (m - cy) / dy);
            if (dy > 0 && cy + dy > height - 1 - m) s = std::min(s, (height - 1 - m - cy) / dy);
        }
        return std::max(0.0, s);
    };

    std::vector<int> planeGroup;   // filled before the tracking loop below
    auto trackSegment = [&](size_t selfIdx, PlanarTrack& pt, int from, int to, bool stopAtExisting) {
        if (from == to) return;
        const int dir = (to > from) ? 1 : -1;
        auto ait = pt.quads.find(from);
        if (ait == pt.quads.end()) return;

        auto quadBBoxI = [](const double* qx, const double* qy, int margin,
                            int* x0, int* y0, int* x1, int* y1) {
            double a0 = 1e18, b0 = 1e18, a1 = -1e18, b1 = -1e18;
            for (int c = 0; c < 4; ++c) {
                a0 = std::min(a0, qx[c]); a1 = std::max(a1, qx[c]);
                b0 = std::min(b0, qy[c]); b1 = std::max(b1, qy[c]);
            }
            *x0 = (int)std::floor(a0) - margin; *y0 = (int)std::floor(b0) - margin;
            *x1 = (int)std::ceil(a1) + margin;  *y1 = (int)std::ceil(b1) + margin;
        };
        // Coplanar group members' quads at a frame — their bboxes pool into
        // the plane fit's harvest region.
        auto groupExtras = [&](int frame) {
            std::vector<std::array<double, 8> > ex;
            for (size_t m = 0; m < planarTracks.size(); ++m) {
                if (m == selfIdx || planeGroup[m] != planeGroup[selfIdx]) continue;
                auto q = planarTracks[m].quads.find(frame);
                if (q != planarTracks[m].quads.end()) ex.push_back(q->second);
            }
            return ex;
        };

        // Reference state (promoted when the direct track share thins out).
        int refFrame = from;
        std::array<double, 8> refQuad = ait->second;
        std::vector<std::array<double, 8> > refExtras = groupExtras(refFrame);
        const int patchMargin = (int)(24.0 * resScale);
        double rxf[4], ryf[4];
        toMv(refQuad, rxf, ryf);
        int bx0i, by0i, bx1i, by1i;
        quadBBoxI(rxf, ryf, patchMargin + 8, &bx0i, &by0i, &bx1i, &by1i);
        libmv::FloatImage refImg = renderFrame(refFrame, bx0i, by0i, bx1i, by1i);
        int rwx0 = 0, rwy0 = 0;
        libmv::FloatImage refWin = cropQuadWindow(refImg, rxf, ryf, patchMargin, rwx0, rwy0);
        int polished = 0, promoted = 0;
        int prevMode = -1;
        static const char* kFitModeNames[5] = {
            "autos-H", "manual-H", "manual-affine", "manual-similarity", "autos-affine"
        };

        for (int f = from; f != to; f += dir) {
            const int nf = f + dir;
            if (pt.userKeys.count(nf)) break;  // never overwrite a user key
            if (stopAtExisting && pt.quads.count(nf)) break;
            auto pit = pt.quads.find(f);
            if (pit == pt.quads.end()) break;

            Eigen::Matrix3d H;
            int inl = 0, nc = 0, fitMode = 0;
            bool fitted = fitPlaneH(refFrame, nf, refQuad, refExtras, &H, &inl, &nc, &fitMode);
            if (!fitted && refFrame != f) {
                // Direct share to the reference exhausted — promote the last
                // solved frame to reference (its quad came from a direct fit,
                // so error does not chain per frame).
                refFrame = f;
                refQuad = pit->second;
                refExtras = groupExtras(refFrame);
                toMv(refQuad, rxf, ryf);
                quadBBoxI(rxf, ryf, patchMargin + 8, &bx0i, &by0i, &bx1i, &by1i);
                refImg = renderFrame(refFrame, bx0i, by0i, bx1i, by1i);
                refWin = cropQuadWindow(refImg, rxf, ryf, patchMargin, rwx0, rwy0);
                ++promoted;
                fitted = fitPlaneH(refFrame, nf, refQuad, refExtras, &H, &inl, &nc, &fitMode);
            }
            if (!fitted) {
                CT_DBG("CameraTracker: planar %d lost at frame %d (%d tracks shared "
                       "with ref frame %d — needs >=4; add manual tracks inside the "
                       "quad or a corner key)\n", pt.id, nf, nc, refFrame);
                break;
            }
            if (fitMode != prevMode) {
                CT_DBG("CameraTracker: planar %d frame %d: plane-fit mode -> %s (%d inliers)\n",
                       pt.id, nf, kFitModeNames[fitMode], inl);
                prevMode = fitMode;
            }

            std::array<double, 8> nq;
            bool degenerate = false;
            for (int c = 0; c < 4; ++c) {
                Eigen::Vector3d pp(refQuad[c*2], refQuad[c*2+1], 1.0);
                Eigen::Vector3d qq = H * pp;
                if (std::abs(qq(2)) < 1e-12) { degenerate = true; break; }
                nq[c*2]   = qq(0) / qq(2);
                nq[c*2+1] = qq(1) / qq(2);
                if (!std::isfinite(nq[c*2]) || !std::isfinite(nq[c*2+1]) ||
                    std::abs(nq[c*2]) > 1e7 || std::abs(nq[c*2+1]) > 1e7) {
                    degenerate = true;
                    break;
                }
            }
            if (degenerate) {
                CT_DBG("CameraTracker: planar %d frame %d: degenerate plane fit "
                       "(mode %s, %d inliers) — stopping segment\n",
                       pt.id, nf, kFitModeNames[fitMode], inl);
                break;
            }

            // Bounded region polish: fully-visible quads only; may refine the
            // points geometry by a few pixels, never replace it wholesale.
            {
                double gx[4], gy[4];
                toMv(nq, gx, gy);
                if (std::min(shrinkToFit(rxf, ryf), shrinkToFit(gx, gy)) >= 0.999) {
                    const int motionMargin = (int)(12.0 * resScale);
                    int px0, py0, px1, py1;
                    quadBBoxI(gx, gy, patchMargin + motionMargin + 8, &px0, &py0, &px1, &py1);
                    libmv::FloatImage curImg = renderFrame(nf, px0, py0, px1, py1);
                    int cwx0 = 0, cwy0 = 0;
                    libmv::FloatImage curWin = cropQuadWindow(curImg, gx, gy,
                                                              patchMargin + motionMargin,
                                                              cwx0, cwy0);
                    double x1[4], y1[4], x2[4], y2[4];
                    for (int c = 0; c < 4; ++c) {
                        x1[c] = rxf[c] - rwx0; y1[c] = ryf[c] - rwy0;
                        x2[c] = gx[c] - cwx0;  y2[c] = gy[c] - cwy0;
                    }
                    libmv::TrackRegionResult result;
                    result.termination = libmv::TrackRegionResult::FAILURE;
                    bool ok = false;
                    try {
                        libmv::TrackRegion(refWin, curWin, x1, y1, opts, x2, y2, &result);
                        ok = result.is_usable();
                    } catch (...) { ok = false; }
                    if (ok) {
                        std::array<double, 8> rq;
                        double maxDev = 0.0;
                        for (int c = 0; c < 4; ++c) {
                            rq[c*2]   = (x2[c] + cwx0) + rod.x1;
                            rq[c*2+1] = (rod.y2 - 1) - (y2[c] + cwy0);
                            maxDev = std::max(maxDev, std::hypot(rq[c*2] - nq[c*2],
                                                                 rq[c*2+1] - nq[c*2+1]));
                        }
                        // Blend the polish with a 2px cap so the frame where
                        // polish disengages (quad leaving frame) can't pop.
                        if (maxDev <= 4.0 * resScale) {
                            const double cap = 2.0 * resScale;
                            const double w = (maxDev > cap) ? cap / maxDev : 1.0;
                            for (int c = 0; c < 8; ++c) nq[c] += w * (rq[c] - nq[c]);
                            ++polished;
                        }
                    }
                }
            }

            pt.quads[nf] = nq;
        }
        CT_DBG("CameraTracker: planar %d segment %d->%d: points-first, %d region-polished, "
               "%d reference promotions\n", pt.id, from, to, polished, promoted);
    };

    // Remove previously baked grids (re-track replaces them).
    {
        std::set<int> baked;
        for (const auto& p : planarTracks) baked.insert(p.bakedIds.begin(), p.bakedIds.end());
        if (!baked.empty()) {
            std::vector<Track2D> kept;
            kept.reserve(tracks.size());
            for (const auto& t : tracks) if (!baked.count(t.id)) kept.push_back(t);
            tracks.swap(kept);
        }
    }

    // Keep ONLY the user-keyed quads (drawn + corrected frames) — for ALL
    // planars up front, because the coplanarity probe below needs key quads.
    for (auto& pt : planarTracks) {
        if (pt.userKeys.empty()) pt.userKeys.insert(pt.refFrame);
        std::map<int, std::array<double, 8> > keyQuads;
        for (int k : pt.userKeys) {
            auto it = pt.quads.find(k);
            if (it != pt.quads.end()) keyQuads[k] = it->second;
        }
        pt.quads.swap(keyQuads);
        pt.bakedIds.clear();
    }

    // --- Plane groups: a homography belongs to the PLANE, not the quad.
    // Coplanar quads share one motion, so their point support pools — an
    // off-frame corner of one quad is positioned by measurements made on the
    // other (user insight, 2026-07-07). Detect coplanarity automatically:
    // fit quad i's plane motion over a probe baseline and count how many of
    // quad j's supporters it explains.
    planeGroup.assign(planarTracks.size(), 0);
    for (size_t i = 0; i < planeGroup.size(); ++i) planeGroup[i] = (int)i;
    {
        const std::vector<std::array<double, 8> > noExtras;
        for (size_t i = 0; i < planarTracks.size(); ++i) {
            for (size_t j = i + 1; j < planarTracks.size(); ++j) {
                if (planeGroup[j] != (int)j) continue;   // already grouped
                int f0 = -1;
                for (const auto& kq : planarTracks[i].quads) {
                    if (planarTracks[j].quads.count(kq.first)) { f0 = kq.first; break; }
                }
                if (f0 < 0) continue;
                int f1 = std::min(rangeEnd, f0 + 25);
                if (f1 == f0) f1 = std::max(rangeStart, f0 - 25);
                if (f1 == f0) continue;
                Eigen::Matrix3d Hi;
                int inl = 0, nc = 0, md = 0;
                if (!fitPlaneH(f0, f1, planarTracks[i].quads[f0], noExtras,
                               &Hi, &inl, &nc, &md)) continue;
                const std::array<double, 8>& qj = planarTracks[j].quads[f0];
                double jx0 = 1e18, jy0 = 1e18, jx1 = -1e18, jy1 = -1e18;
                for (int c = 0; c < 4; ++c) {
                    jx0 = std::min(jx0, qj[c*2]);   jx1 = std::max(jx1, qj[c*2]);
                    jy0 = std::min(jy0, qj[c*2+1]); jy1 = std::max(jy1, qj[c*2+1]);
                }
                const double jm = 0.15 * std::max(jx1 - jx0, jy1 - jy0);
                jx0 -= jm; jx1 += jm; jy0 -= jm; jy1 += jm;
                int nj = 0, inlj = 0;
                const double cthr = 2.5 * resScale;
                for (const auto& t : tracks) {
                    auto ma = t.markers.find(f0), mb = t.markers.find(f1);
                    if (ma == t.markers.end() || mb == t.markers.end()) continue;
                    if (ma->second.first  < jx0 || ma->second.first  > jx1 ||
                        ma->second.second < jy0 || ma->second.second > jy1) continue;
                    ++nj;
                    Eigen::Vector3d pp(ma->second.first, ma->second.second, 1.0);
                    Eigen::Vector3d qq = Hi * pp;
                    if (std::abs(qq(2)) < 1e-12) continue;
                    const double dx = qq(0)/qq(2) - mb->second.first;
                    const double dy = qq(1)/qq(2) - mb->second.second;
                    if (dx*dx + dy*dy < cthr*cthr) ++inlj;
                }
                if (nj >= 4 && inlj >= 4 && inlj * 10 >= nj * 6) {
                    planeGroup[j] = planeGroup[i];
                    CT_DBG("CameraTracker: planars %d and %d are COPLANAR "
                           "(%d/%d supporters agree over a %d-frame probe) — "
                           "pooling their plane fits\n",
                           (int)i, (int)j, inlj, nj, std::abs(f1 - f0));
                }
            }
        }
    }

    int totalBaked = 0;
    const int N = std::max(2, planarGridSize.lock()->getValue());
    for (size_t pidx = 0; pidx < planarTracks.size(); ++pidx) {
        PlanarTrack& pt = planarTracks[pidx];
        std::vector<int> keys;
        for (const auto& kq : pt.quads) keys.push_back(kq.first);
        if (keys.empty()) continue;
        // backward from the first key, forward between keys, forward from the last
        if (keys.front() > rangeStart) trackSegment(pidx, pt, keys.front(), rangeStart, false);
        for (size_t i = 0; i < keys.size(); ++i) {
            const int segEnd = (i + 1 < keys.size()) ? keys[i + 1] : rangeEnd;
            if (keys[i] < segEnd) trackSegment(pidx, pt, keys[i], segEnd, false);
            // If the forward pass lost the region partway, fill the remainder
            // of the gap BACKWARD from the next key (stops where coverage
            // resumes) — a mid-shot failure then costs nothing.
            if (i + 1 < keys.size()) {
                trackSegment(pidx, pt, keys[i + 1], keys[i], true);
            }
        }
        // Report coverage gaps honestly so a failed stretch is visible.
        {
            std::vector<std::pair<int,int> > gaps;
            int gapStart = -1;
            for (int f = rangeStart; f <= rangeEnd; ++f) {
                const bool have = pt.quads.count(f) != 0;
                if (!have && gapStart < 0) gapStart = f;
                if (have && gapStart >= 0) { gaps.push_back({gapStart, f - 1}); gapStart = -1; }
            }
            if (gapStart >= 0) gaps.push_back({gapStart, rangeEnd});
            for (const auto& g : gaps) {
                CT_DBG("CameraTracker: planar %d GAP frames %d-%d (add a corner key inside it)\n",
                       pt.id, g.first, g.second);
            }
        }
        pt.tracked = pt.quads.size() > pt.userKeys.size();
        CT_DBG("CameraTracker: planar %d tracked %d/%d frames (%d user keys)\n",
               pt.id, (int)pt.quads.size(), rangeEnd - rangeStart + 1, (int)pt.userKeys.size());
        if (!pt.tracked) continue;

        // --- Bake NxN grid as regular Track2D markers ---
        // Grid points defined INSIDE the ref quad (bilinear in the ref frame is
        // fine — it just chooses which plane points to follow); transferred to
        // every frame by the exact 4-point homography ref->frame (projectively
        // correct under foreshortening).
        const std::array<double, 8> ref = pt.quads[pt.refFrame];
        int nextId = 0;
        for (const auto& t : tracks) nextId = std::max(nextId, t.id + 1);

        std::vector<std::pair<double, double> > refPts;
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) {
                const double s = (double)j / (N - 1), tt = (double)i / (N - 1);
                // bilinear on ref corners BL,BR,TR,TL
                const double bx = (1-s)*(1-tt)*ref[0] + s*(1-tt)*ref[2] + s*tt*ref[4] + (1-s)*tt*ref[6];
                const double by = (1-s)*(1-tt)*ref[1] + s*(1-tt)*ref[3] + s*tt*ref[5] + (1-s)*tt*ref[7];
                refPts.push_back(std::make_pair(bx, by));
            }
        }
        std::vector<Track2D> gridTracks(refPts.size());
        for (size_t g = 0; g < refPts.size(); ++g) {
            gridTracks[g].id = nextId + (int)g;
            pt.bakedIds.push_back(gridTracks[g].id);
        }
        for (const auto& fq : pt.quads) {
            Eigen::Matrix3d H;
            if (!homographyFrom4Pts(ref.data(), fq.second.data(), &H)) continue;
            for (size_t g = 0; g < refPts.size(); ++g) {
                Eigen::Vector3d p = H * Eigen::Vector3d(refPts[g].first, refPts[g].second, 1.0);
                if (std::abs(p(2)) < 1e-12) continue;
                gridTracks[g].markers[fq.first] = std::make_pair(p(0) / p(2), p(1) / p(2));
            }
        }
        for (auto& gt : gridTracks) {
            if (gt.markers.size() >= 3) {
                tracks.push_back(gt);
                ++totalBaked;
            }
        }
    }

    std::stringstream ss;
    ss << "Planar tracking done: " << planarTracks.size() << " region(s), "
       << totalBaked << " grid markers baked. Solve to use them.";
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: %s\n", ss.str().c_str());
    publicInterface->getApp()->redrawAllViewers();
}

void
CameraTrackerNodePrivate::createCardsFromPlanars()
{
    if (!hasSolution) {
        solveStatusDisplay.lock()->setValue("Solve first, then create cards from planars");
        return;
    }
    NodePtr thisNode = publicInterface->getNode();
    AppInstancePtr app = thisNode ? thisNode->getApp() : AppInstancePtr();
    if (!app) return;

    double ox, oy, oz, scaleF;
    getOutputNormalization(ox, oy, oz, scaleF);
    const int N = std::max(2, planarGridSize.lock()->getValue());

    // 3D point per baked track id
    std::map<int, const SolvedPoint*> byId;
    for (const auto& sp : solvedPoints) byId[sp.track] = &sp;

    int made = 0;
    for (const auto& pt : planarTracks) {
        if (!pt.tracked || pt.bakedIds.empty()) continue;
        // grid corners: (row,col) -> index row*N+col; BL=(0,0) BR=(0,N-1) TR=(N-1,N-1) TL=(N-1,0)
        const int idBL = pt.bakedIds[0], idBR = pt.bakedIds[N - 1];
        const int idTR = pt.bakedIds[N * N - 1], idTL = pt.bakedIds[(N - 1) * N];
        auto get = [&](int id) -> const SolvedPoint* {
            std::map<int, const SolvedPoint*>::const_iterator it = byId.find(id);
            return it == byId.end() ? NULL : it->second;
        };
        const SolvedPoint *bl = get(idBL), *br = get(idBR), *tr = get(idTR), *tl = get(idTL);
        if (!bl || !br || !tr || !tl) {
            CT_DBG("CameraTracker: planar %d corners not all solved — skipping card\n", pt.id);
            continue;
        }
        Eigen::Vector3d BL(bl->x, bl->y, bl->z), BR(br->x, br->y, br->z);
        Eigen::Vector3d TR(tr->x, tr->y, tr->z), TL(tl->x, tl->y, tl->z);
        Eigen::Vector3d C = (BL + BR + TR + TL) / 4.0;
        Eigen::Vector3d X = ((BR + TR) - (BL + TL)) / 2.0;   // width vector
        Eigen::Vector3d Y = ((TL + TR) - (BL + BR)) / 2.0;   // height vector
        const double w3d = X.norm(), h3d = Y.norm();
        if (w3d < 1e-9 || h3d < 1e-9) continue;
        Eigen::Vector3d xhat = X.normalized();
        Eigen::Vector3d n = xhat.cross(Y).normalized() * -1.0;  // +Z faces viewer side
        Eigen::Vector3d yhat = n.cross(xhat);

        // Into output gauge: position through the full transform, axes through
        // the scene rotation only.
        double px, py, pz;
        applyOutputTransform(C(0), C(1), C(2), ox, oy, oz, scaleF, px, py, pz);
        Eigen::Matrix3d Rs;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) Rs(r, c) = sceneRot[r][c];
        Eigen::Matrix3d M3;
        M3.col(0) = Rs * xhat; M3.col(1) = Rs * yhat; M3.col(2) = Rs * n;
        double M[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) M[r][c] = M3(r, c);
        double rx, ry, rz;
        RotationConventions::decompose(M, rx, ry, rz);

        CreateNodeArgs cnArgs(PLUGINID_NATRON_CARD3D, thisNode->getGroup());
        cnArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        cnArgs.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);
        NodePtr cardNode = app->createNode(cnArgs);
        if (!cardNode) continue;
        std::stringstream nm; nm << "PlanarCard_" << pt.id;
        cardNode->setLabel(nm.str());
        auto setD = [&cardNode](const char* name, double v) {
            KnobDoublePtr kd = std::dynamic_pointer_cast<KnobDouble>(cardNode->getKnobByName(name));
            if (kd) kd->setValue(v);
        };
        setD("translateX", px); setD("translateY", py); setD("translateZ", pz);
        setD("rotateX", rx);    setD("rotateY", ry);    setD("rotateZ", rz);
        // Card3D mesh (no image input) is (16/9 x 1) units — normalize to the
        // reconstructed rectangle's size in output units.
        setD("scaleX", (w3d * scaleF) / (16.0 / 9.0));
        setD("scaleY", h3d * scaleF);
        ++made;
    }

    std::stringstream ss;
    ss << "Created " << made << " card(s) from planar tracks";
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: %s\n", ss.str().c_str());
}


// ==================== EXIF focal read ====================

namespace {
// The handful of EXIF tags we care about.
struct ExifFocal
{
    double focalMm = 0.0;   // FocalLength (0x920A)
    double focal35 = 0.0;   // FocalLengthIn35mmFilm (0xA405)
    double fpXRes = 0.0;    // FocalPlaneXResolution (0xA20E)
    int fpUnit = 0;         // FocalPlaneResolutionUnit (0xA210): 2 = inch, 3 = cm
    int pixelXDim = 0;      // PixelXDimension (0xA002)
};

unsigned exifRd16(const unsigned char* b, size_t off, size_t n, bool le)
{
    if (off + 2 > n) return 0;
    return le ? (b[off] | (b[off + 1] << 8)) : ((b[off] << 8) | b[off + 1]);
}

unsigned exifRd32(const unsigned char* b, size_t off, size_t n, bool le)
{
    if (off + 4 > n) return 0;
    return le ? (b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | ((unsigned)b[off + 3] << 24))
              : (((unsigned)b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]);
}

double exifRational(const unsigned char* b, size_t off, size_t n, bool le)
{
    const unsigned num = exifRd32(b, off, n, le);
    const unsigned den = exifRd32(b, off + 4, n, le);
    return den ? (double)num / (double)den : 0.0;
}

// Minimal bounds-checked TIFF-IFD walker: b points at the "II"/"MM" header.
// Scans IFD0 (for the Exif sub-IFD pointer, and tolerates focal tags placed
// there directly) then the Exif IFD.
bool exifParseTiff(const unsigned char* b, size_t n, ExifFocal* out)
{
    if (n < 8) return false;
    bool le;
    if (b[0] == 'I' && b[1] == 'I') le = true;
    else if (b[0] == 'M' && b[1] == 'M') le = false;
    else return false;
    if (exifRd16(b, 2, n, le) != 42) return false;

    size_t exifIfd = 0;
    auto scanIfd = [&](size_t ifdOff) {
        const unsigned count = exifRd16(b, ifdOff, n, le);
        for (unsigned i = 0; i < count; ++i) {
            const size_t e = ifdOff + 2 + (size_t)i * 12;
            if (e + 12 > n) return;
            const unsigned tag  = exifRd16(b, e, n, le);
            const unsigned type = exifRd16(b, e + 2, n, le);
            const unsigned cnt  = exifRd32(b, e + 4, n, le);
            const size_t valOff = e + 8;
            static const unsigned typeSize[13] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
            const unsigned ts = (type < 13) ? typeSize[type] : 0;
            // values wider than 4 bytes live at an offset from the TIFF header
            const size_t payload = ((size_t)ts * cnt <= 4)
                                   ? valOff : (size_t)exifRd32(b, valOff, n, le);
            switch (tag) {
                case 0x8769: exifIfd = (size_t)exifRd32(b, valOff, n, le); break;
                case 0x920A: if (type == 5) out->focalMm = exifRational(b, payload, n, le); break;
                case 0xA405: out->focal35 = (type == 3) ? exifRd16(b, payload, n, le)
                                                        : exifRd32(b, payload, n, le); break;
                case 0xA20E: if (type == 5) out->fpXRes = exifRational(b, payload, n, le); break;
                case 0xA210: out->fpUnit = (int)exifRd16(b, payload, n, le); break;
                case 0xA002: out->pixelXDim = (type == 3) ? (int)exifRd16(b, payload, n, le)
                                                          : (int)exifRd32(b, payload, n, le); break;
                default: break;
            }
        }
    };
    const size_t ifd0 = exifRd32(b, 4, n, le);
    if (ifd0 == 0 || ifd0 >= n) return false;
    scanIfd(ifd0);
    if (exifIfd > 0 && exifIfd < n) scanIfd(exifIfd);

    return (out->focalMm > 0.0) || (out->focal35 > 0.0);
}

// Whole-file entry: JPEG (walk markers to the APP1 "Exif\0\0" segment) or a
// bare TIFF container (TIFF/DNG/CR2). EXIF IFDs sit at the head of the file;
// 1MB covers them with a wide margin.
bool exifParseFile(const std::string& path, ExifFocal* out)
{
    FStreamsSupport::ifstream f;
    // binary is essential: text mode on Windows eats CR bytes and stops at
    // 0x1A, both of which JPEG/TIFF data hit within the first IFD.
    FStreamsSupport::open(&f, path, std::ios_base::in | std::ios_base::binary);
    if (!f) return false;
    std::vector<unsigned char> buf(1u << 20);
    f.read((char*)buf.data(), (std::streamsize)buf.size());
    const size_t n = (size_t)f.gcount();
    if (n < 8) return false;
    const unsigned char* b = buf.data();

    if (b[0] == 0xFF && b[1] == 0xD8) {   // JPEG
        size_t off = 2;
        while (off + 4 <= n && b[off] == 0xFF) {
            const unsigned marker = b[off + 1];
            if (marker == 0xDA || marker == 0xD9) break;   // image data / EOI
            const size_t seglen = ((size_t)b[off + 2] << 8) | b[off + 3];
            if (seglen < 2) break;
            if (marker == 0xE1 && off + 10 <= n &&
                std::memcmp(b + off + 4, "Exif\0\0", 6) == 0) {
                const size_t t = off + 10;
                return exifParseTiff(b + t, std::min(n - t, seglen - 8), out);
            }
            off += 2 + seglen;
        }
        return false;
    }
    return exifParseTiff(b, n, out);
}
} // anonymous namespace

void
CameraTrackerNodePrivate::readFocalFromExif()
{
    // Nearest upstream node with a "filename" knob = the Read node.
    NodePtr n = publicInterface->getNode();
    n = n ? n->getInput(0) : NodePtr();
    KnobStringBasePtr fileKnob;
    for (int hop = 0; n && hop < 5; ++hop) {
        KnobIPtr k = n->getKnobByName("filename");
        if (k) {
            fileKnob = std::dynamic_pointer_cast<KnobStringBase>(k);
            if (fileKnob) break;
        }
        n = n->getInput(0);
    }
    if (!fileKnob) {
        publicInterface->message(eMessageTypeWarning,
            "No file-based Read node found upstream — connect the footage "
            "Reader to the CameraTracker input.");
        solveStatusDisplay.lock()->setValue("EXIF: no Read node found upstream");
        return;
    }

    // Resolve the sequence pattern to a concrete file (### padding or a
    // single printf-style %0Nd token), trying the range-start frame.
    const std::string pattern = fileKnob->getValue();
    const int rangeStart = trackRangeStart.lock()->getValue();
    std::vector<std::string> candidates;
    {
        std::string s = pattern;
        const size_t h = s.find('#');
        if (h != std::string::npos) {
            size_t e = h;
            while (e < s.size() && s[e] == '#') ++e;
            std::ostringstream num;
            num << std::setw((int)(e - h)) << std::setfill('0') << rangeStart;
            candidates.push_back(s.substr(0, h) + num.str() + s.substr(e));
        } else if (std::count(s.begin(), s.end(), '%') == 1) {
            char buf[4096];
            snprintf(buf, sizeof(buf), s.c_str(), rangeStart);
            candidates.push_back(buf);
        }
        candidates.push_back(pattern);   // plain single file
    }

    ExifFocal ex;
    bool found = false;
    std::string used;
    for (const auto& c : candidates) {
        ExifFocal tmp;
        if (exifParseFile(c, &tmp)) { ex = tmp; found = true; used = c; break; }
    }
    if (!found || (ex.focalMm <= 0.0 && ex.focal35 <= 0.0)) {
        publicInterface->message(eMessageTypeWarning,
            "No EXIF focal metadata found in the footage. Converted or stock "
            "clips usually lose it — use Estimate Focal From Footage instead.");
        solveStatusDisplay.lock()->setValue("EXIF: no focal metadata in input file");
        return;
    }

    // Focal, and sensor width when derivable: the 35mm-equivalent tag gives
    // it exactly (crop factor against the 36mm full-frame width); otherwise
    // the focal-plane resolution tags.
    double focal = ex.focalMm;
    double sensorW = 0.0;
    if (focal <= 0.0) {   // only the 35mm-equivalent present
        focal = ex.focal35;
        sensorW = 36.0;
    } else if (ex.focal35 > 0.0) {
        sensorW = 36.0 * ex.focalMm / ex.focal35;
    } else if (ex.fpXRes > 0.0 && (ex.fpUnit == 2 || ex.fpUnit == 3) && ex.pixelXDim > 0) {
        sensorW = (double)ex.pixelXDim / ex.fpXRes * (ex.fpUnit == 2 ? 25.4 : 10.0);
    }

    focalLengthMm.lock()->setValue(focal);
    std::stringstream ss;
    ss << "EXIF: focal " << std::fixed << std::setprecision(2) << focal << "mm";
    if (sensorW > 0.0) {
        sensorWidth.lock()->setValue(sensorW);
        ss << ", sensor width " << sensorW << "mm";
        if (ex.focal35 > 0.0 && ex.focalMm > 0.0) {
            ss << " (from 35mm-equiv " << std::setprecision(0) << ex.focal35 << "mm)";
        }
    } else {
        ss << " — sensor width not in EXIF; check the Sensor Width knob "
              "matches the camera";
    }
    solveStatusDisplay.lock()->setValue(ss.str());
    publicInterface->message(eMessageTypeInfo, ss.str());
    CT_DBG("CameraTracker: %s (file %s)\n", ss.str().c_str(), used.c_str());
}


// ==================== Lens distortion workflow ====================

void
CameraTrackerNodePrivate::createLensWarpNode(bool undistort)
{
    NodePtr thisNode = publicInterface->getNode();
    AppInstancePtr app = thisNode ? thisNode->getApp() : AppInstancePtr();
    if (!app) return;

    NodeCollectionPtr group = thisNode->getGroup();
    CreateNodeArgs cnArgs(PLUGINID_NATRON_LENSWARP, group);
    cnArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    cnArgs.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);
    NodePtr warpNode = app->createNode(cnArgs);
    if (!warpNode) {
        solveStatusDisplay.lock()->setValue("Failed to create LensWarp node");
        return;
    }
    warpNode->setLabel(undistort ? "CameraTracker_Undistort" : "CameraTracker_Redistort");

    // Copy this node's lens model over (identical model, identical math).
    auto copyDouble = [&](const char* name, double v) {
        KnobDoublePtr k = std::dynamic_pointer_cast<KnobDouble>(warpNode->getKnobByName(name));
        if (k) k->setValue(v);
    };
    copyDouble("focalLengthMm", focalLengthMm.lock()->getValue());
    copyDouble("sensorWidth", sensorWidth.lock()->getValue());
    copyDouble("k1", k1.lock()->getValue());
    copyDouble("k2", k2.lock()->getValue());
    copyDouble("k3", k3.lock()->getValue());
    {
        KnobDoublePtr pk = std::dynamic_pointer_cast<KnobDouble>(warpNode->getKnobByName("principalPoint"));
        if (pk) {
            pk->setValue(principalPointX.lock()->getValue(), ViewSpec::all(), 0);
            pk->setValue(principalPointY.lock()->getValue(), ViewSpec::all(), 1);
        }
    }
    {
        KnobChoicePtr dk = std::dynamic_pointer_cast<KnobChoice>(warpNode->getKnobByName("direction"));
        if (dk) dk->setValue(undistort ? 0 : 1);
    }

    std::stringstream ss;
    ss << "Created " << (undistort ? "Undistort" : "Redistort")
       << " LensWarp (k1=" << std::setprecision(4) << k1.lock()->getValue()
       << ", k2=" << k2.lock()->getValue() << ")"
       << (undistort ? " — insert on the plate before pinhole comp"
                     : " — apply to the finished comp last");
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: %s\n", ss.str().c_str());
}


// ==================== Drag magnifier ====================

// BGRA packing for the byte texture (same as TrackerNodeInteract).
static unsigned int
magToBGRA(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Linear float image -> sRGB byte texture via PBO. Straight port of
// TrackerNodeInteract::convertImageTosRGBOpenGLTexture (including its
// row-random error-diffusion dither). Caller must have the viewer GL
// context current.
void
CameraTrackerNodePrivate::uploadMagnifierTexture(const ImagePtr& image, const RectI& renderWindow)
{
    RectI bounds;
    RectI roi;
    if (image) {
        bounds = image->getBounds();
        roi = renderWindow.intersect(bounds);
    } else {
        bounds = renderWindow;
        roi = bounds;
    }
    if (roi.isNull()) {
        return;
    }

    std::size_t bytesCount = 4 * sizeof(unsigned char) * roi.area();
    TextureRect region;
    region.x1 = roi.x1;
    region.x2 = roi.x2;
    region.y1 = roi.y1;
    region.y2 = roi.y2;

    GLint currentBoundPBO = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING_ARB, &currentBoundPBO);
    if (magPboID == 0) {
        glGenBuffers(1, &magPboID);
    }
    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, magPboID);
    glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, bytesCount, NULL, GL_DYNAMIC_DRAW_ARB);
    GLvoid *buf = glMapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, GL_WRITE_ONLY_ARB);
    if (buf) {
        if (!image) {
            int pixelsCount = roi.area();
            unsigned int* dstPixels = (unsigned int*)buf;
            for (int i = 0; i < pixelsCount; ++i, ++dstPixels) {
                *dstPixels = magToBGRA(0, 0, 0, 255);
            }
        } else {
            int srcNComps = (int)image->getComponentsCount();
            assert(srcNComps >= 3);
            Image::ReadAccess acc(image.get());
            const float* srcPixels = (const float*)acc.pixelAt(roi.x1, roi.y1);
            unsigned int* dstPixels = (unsigned int*)buf;
            int w = roi.width();
            int srcRowElements = bounds.width() * srcNComps;
            const Color::Lut* lut = Color::LutManager::sRGBLut();
            lut->validate();
            unsigned char alpha = 255;
            for (int y = roi.y1; y < roi.y2; ++y, dstPixels += w, srcPixels += srcRowElements) {
                int start = (int)(rand() % (roi.x2 - roi.x1));
                for (int backward = 0; backward < 2; ++backward) {
                    int index = backward ? start - 1 : start;
                    unsigned error_r = 0x80;
                    unsigned error_g = 0x80;
                    unsigned error_b = 0x80;
                    while (index < w && index >= 0) {
                        float r = srcPixels[index * srcNComps];
                        float g = srcPixels[index * srcNComps + 1];
                        float b = srcPixels[index * srcNComps + 2];
                        error_r = (error_r & 0xff) + lut->toColorSpaceUint8xxFromLinearFloatFast(r);
                        error_g = (error_g & 0xff) + lut->toColorSpaceUint8xxFromLinearFloatFast(g);
                        error_b = (error_b & 0xff) + lut->toColorSpaceUint8xxFromLinearFloatFast(b);
                        dstPixels[index] = magToBGRA((unsigned char)(error_r >> 8),
                                                     (unsigned char)(error_g >> 8),
                                                     (unsigned char)(error_b >> 8),
                                                     alpha);
                        if (backward) { --index; } else { ++index; }
                    }
                }
            }
        }
        glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
    }
    magTexture->fillOrAllocateTexture(region, RectI(), false, 0);
    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, currentBoundPBO);
    glCheckError();
}

bool
CameraTrackerNodePrivate::refreshMagnifierTexture(int frame, double cx, double cy)
{
    NodePtr node = publicInterface->getNode();
    NodePtr inputNode = node ? node->getInput(0) : NodePtr();
    EffectInstancePtr input = inputNode ? inputNode->getEffectInstance() : EffectInstancePtr();
    if (!input) {
        return false;
    }

    // Generous RoI around the marker so small drags reuse the texture; pen
    // motion re-fetches only when the cursor leaves the inner 50%.
    const double patch = 2.0 * patternHalfFor(selectedManualId);
    const double half = std::max(96.0, patch * 3.0);
    RectD roiCanonical(cx - half, cy - half, cx + half, cy + half);
    const double par = input->getAspectRatio(-1);
    RectI roi = roiCanonical.toPixelEnclosing(0, par);

    // Same render path as TrackMarker::getMarkerImage (Tracker node); we are
    // on the main thread inside a pen handler, so render synchronously — the
    // frame is in the viewer cache and the RoI is a few hundred px.
    std::list<ImagePlaneDesc> components;
    components.push_back(ImagePlaneDesc::getRGBComponents());
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameRenderArgs((double)frame,
                                             ViewIdx(0),
                                             true,  // isRenderUserInteraction
                                             false, // isSequential
                                             abortInfo,
                                             node,
                                             0, // texture index
                                             node->getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             true,  // isAnalysis
                                             false, // draftMode
                                             RenderStatsPtr());
    EffectInstance::RenderRoIArgs args((double)frame,
                                       RenderScale::identity,
                                       0, // mipmapLevel
                                       ViewIdx(0),
                                       false,
                                       roi,
                                       RectD(),
                                       components,
                                       eImageBitDepthFloat,
                                       false,
                                       publicInterface,
                                       eStorageModeRAM,
                                       (double)frame);
    std::map<ImagePlaneDesc, ImagePtr> planes;
    EffectInstance::RenderRoIRetCode stat = input->renderRoI(args, &planes);
    if ((stat != EffectInstance::eRenderRoIRetCodeOk) || planes.empty() || !planes.begin()->second) {
        return false;
    }

    // Texture upload needs the viewer GL context (pen handlers don't have it
    // current, unlike drawOverlay).
    OverlaySupport* overlay = publicInterface->getCurrentViewportForOverlays();
    OpenGLViewerI* glViewer = dynamic_cast<OpenGLViewerI*>(overlay);
    if (!glViewer) {
        return false;
    }
    glViewer->makeOpenGLcontextCurrent();

    if (!magTexture) {
        int format, internalFormat, glType;
        Texture::getRecommendedTexParametersForRGBAByteTexture(&format, &internalFormat, &glType);
        magTexture = std::make_shared<Texture>(GL_TEXTURE_2D, GL_LINEAR, GL_NEAREST,
                                               GL_CLAMP_TO_EDGE, Texture::eDataTypeByte,
                                               format, internalFormat, glType);
    }
    magTextureRoI = roi;
    magTextureFrame = frame;
    uploadMagnifierTexture(planes.begin()->second, roi);
    return true;
}

void
CameraTrackerNodePrivate::estimateFocalFromFootage()
{
    if (tracks.empty()) {
        // Dialog, not just the Tracking-tab status line — this button lives on
        // the Camera tab where that line isn't visible.
        publicInterface->message(eMessageTypeWarning,
            "Estimate Focal needs 2D tracks to work from.\n"
            "On the Tracking tab: press Detect Features, then Track Features, "
            "then come back and press Estimate Focal From Footage.");
        solveStatusDisplay.lock()->setValue("No tracks — Detect + Track first, then Estimate Focal");
        return;
    }
    // Image geometry (same source as the solve).
    NodePtr node = publicInterface->getNode();
    NodePtr inputNode = node ? node->getInput(0) : NodePtr();
    EffectInstancePtr input = inputNode ? inputNode->getEffectInstance() : EffectInstancePtr();
    if (!input) {
        solveStatusDisplay.lock()->setValue("No input connected");
        return;
    }
    const int rangeStart = trackRangeStart.lock()->getValue();
    const int rangeEnd = trackRangeEnd.lock()->getValue();
    RectD rod; RenderScale scaleRs; bool ipf = false;
    if (input->getRegionOfDefinition_public(inputNode->getHashValue(),
            (double)rangeStart, scaleRs, ViewIdx(0), &rod, &ipf) != eStatusOK) {
        solveStatusDisplay.lock()->setValue("Could not get the input image size — is the input connected?");
        return;
    }
    const double width = rod.width(), height = rod.height();
    const double cx = width / 2.0, cy = height / 2.0;

    // --- Candidate frame pairs: multiple baselines and start offsets sample
    // DIFFERENT motion geometry. A single pair is fragile (forward motion
    // constrains focal weakly); the median over pairs is far more stable, and
    // the spread across pairs is the honest confidence measure.
    const int span = rangeEnd - rangeStart;
    auto commonCount = [&](int fa, int fb) {
        int c = 0;
        for (const auto& t : tracks) {
            if (t.markers.count(fa) && t.markers.count(fb)) ++c;
        }
        return c;
    };
    std::vector<std::pair<int, int> > pairs;
    {
        const int baselines[3] = { std::max(15, span / 6),
                                   std::max(25, span / 3),
                                   std::max(40, (2 * span) / 3) };
        for (int b = 0; b < 3; ++b) {
            const int baseline = std::min(span, baselines[b]);
            if (baseline < 8) continue;
            // best two disjoint start offsets per baseline
            int best1A = -1, best1C = -1, best2A = -1, best2C = -1;
            const int step = std::max(1, span / 40);
            for (int fa = rangeStart; fa + baseline <= rangeEnd; fa += step) {
                const int c = commonCount(fa, fa + baseline);
                if (c > best1C) {
                    if (best1A >= 0 && std::abs(best1A - fa) > baseline / 2) {
                        best2A = best1A; best2C = best1C;
                    }
                    best1C = c; best1A = fa;
                } else if (c > best2C &&
                           (best1A < 0 || std::abs(best1A - fa) > baseline / 2)) {
                    best2C = c; best2A = fa;
                }
            }
            if (best1C >= 16) pairs.push_back({best1A, best1A + baseline});
            if (best2C >= 16) pairs.push_back({best2A, best2A + baseline});
        }
    }
    if (pairs.empty()) {
        solveStatusDisplay.lock()->setValue("Not enough shared tracks across a wide baseline "
                                            "to estimate focal — track more features");
        return;
    }

    auto sampson = [](const libmv::Mat3& F, const libmv::Vec2& x1, const libmv::Vec2& x2) {
        libmv::Vec3 h1; h1 << x1(0), x1(1), 1.0;
        libmv::Vec3 h2; h2 << x2(0), x2(1), 1.0;
        libmv::Vec3 Fx1 = F * h1;
        libmv::Vec3 Ftx2 = F.transpose() * h2;
        const double num = h2.dot(Fx1);
        const double den = Fx1(0)*Fx1(0) + Fx1(1)*Fx1(1) + Ftx2(0)*Ftx2(0) + Ftx2(1)*Ftx2(1);
        return den > 1e-12 ? (num * num) / den : 1e30;
    };
    auto fovToFpx = [&](double fovDeg) {
        return (width / 2.0) / std::tan(fovDeg * M_PI / 360.0);
    };

    // Estimate the FOV from one frame pair: correspondences -> RANSAC F
    // (8-point + Sampson) -> Mendonça-Cipolla focal sweep -> golden refine.
    struct PairEstimate {
        double fovDeg;
        double costRatio;   // bestCost / secondCost — flat curve -> near 1
        int inliers;
        int kfA, kfB;
    };
    auto estimateFromPair = [&](int kfA, int kfB, PairEstimate* out) -> bool {
        // Correspondences in centered pixel coordinates (principal point at
        // origin simplifies K to diag(f, f, 1)).
        std::vector<libmv::Vec2> p1, p2;
        for (const auto& t : tracks) {
            auto a = t.markers.find(kfA);
            auto b = t.markers.find(kfB);
            if (a == t.markers.end() || b == t.markers.end()) continue;
            // Natron coords -> centered, y-down
            libmv::Vec2 va, vb;
            va << (a->second.first - rod.x1) - cx, ((rod.y2 - 1) - a->second.second) - cy;
            vb << (b->second.first - rod.x1) - cx, ((rod.y2 - 1) - b->second.second) - cy;
            p1.push_back(va);
            p2.push_back(vb);
        }
        const int n = (int)p1.size();
        if (n < 16) return false;

        unsigned rng = 7u + (unsigned)n * 2654435761u + (unsigned)kfA * 7919u;
        auto nextRand = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng; };
        libmv::Mat3 bestF;
        int bestInliers = -1;
        const double thr = 2.0 * 2.0;   // 2px Sampson
        for (int it = 0; it < 256; ++it) {
            int idx[8];
            for (int s = 0; s < 8; ++s) {
                bool dup;
                do {
                    idx[s] = (int)(nextRand() % (unsigned)n);
                    dup = false;
                    for (int q = 0; q < s; ++q) if (idx[q] == idx[s]) { dup = true; break; }
                } while (dup);
            }
            libmv::Mat x1(2, 8), x2(2, 8);
            for (int s = 0; s < 8; ++s) { x1.col(s) = p1[idx[s]]; x2.col(s) = p2[idx[s]]; }
            libmv::Mat3 F;
            libmv::NormalizedEightPointSolver(x1, x2, &F);
            int nIn = 0;
            for (int i = 0; i < n; ++i) if (sampson(F, p1[i], p2[i]) < thr) ++nIn;
            if (nIn > bestInliers) { bestInliers = nIn; bestF = F; }
        }
        if (bestInliers < 16) return false;
        // Re-fit F on all inliers for stability.
        {
            std::vector<int> in;
            for (int i = 0; i < n; ++i) if (sampson(bestF, p1[i], p2[i]) < thr) in.push_back(i);
            libmv::Mat x1(2, (int)in.size()), x2(2, (int)in.size());
            for (size_t i = 0; i < in.size(); ++i) { x1.col(i) = p1[in[i]]; x2.col(i) = p2[in[i]]; }
            libmv::NormalizedEightPointSolver(x1, x2, &bestF);
        }

        // Sweep focal candidates: score E = K^T F K by the essential-matrix
        // constraint (two equal non-zero singular values) — Mendonça-Cipolla
        // cost (s1 - s2) / (s1 + s2). Log-spaced FOV 15°..120°, golden refine.
        auto costForFocal = [&](double fpx) {
            libmv::Mat3 K = libmv::Mat3::Identity();
            K(0,0) = K(1,1) = fpx;
            libmv::Mat3 E = K.transpose() * bestF * K;
            Eigen::JacobiSVD<libmv::Mat3> svd(E);
            const double s1 = svd.singularValues()(0);
            const double s2 = svd.singularValues()(1);
            return (s1 + s2) > 1e-12 ? (s1 - s2) / (s1 + s2) : 1.0;
        };
        double bestFov = 60.0, bestCost = 1e30, secondCost = 1e30;
        const int kSteps = 40;
        for (int i = 0; i <= kSteps; ++i) {
            const double fov = 15.0 * std::pow(120.0 / 15.0, (double)i / kSteps);   // log spacing
            const double c = costForFocal( fovToFpx(fov) );
            if (c < bestCost) { secondCost = bestCost; bestCost = c; bestFov = fov; }
            else if (c < secondCost) { secondCost = c; }
        }
        {
            double lo = bestFov / 1.25, hi = bestFov * 1.25;
            const double gr = 0.6180339887;
            double a = hi - (hi - lo) * gr, b = lo + (hi - lo) * gr;
            double ca = costForFocal(fovToFpx(a)), cb = costForFocal(fovToFpx(b));
            for (int it = 0; it < 24; ++it) {
                if (ca < cb) { hi = b; b = a; cb = ca; a = hi - (hi - lo) * gr; ca = costForFocal(fovToFpx(a)); }
                else         { lo = a; a = b; ca = cb; b = lo + (hi - lo) * gr; cb = costForFocal(fovToFpx(b)); }
            }
            bestFov = (lo + hi) / 2.0;
            bestCost = costForFocal(fovToFpx(bestFov));
        }
        out->fovDeg = bestFov;
        out->costRatio = (secondCost > 1e-12) ? bestCost / secondCost : 1.0;
        out->inliers = bestInliers;
        out->kfA = kfA; out->kfB = kfB;
        return true;
    };

    // Aggregate: median FOV over all usable pairs. Different pairs sample
    // different motion geometry, so the median rides out the degenerate ones
    // (a forward-motion pair barely constrains focal); the spread across pairs
    // is the honest confidence measure.
    std::vector<PairEstimate> est;
    for (const auto& pr : pairs) {
        PairEstimate e;
        if (estimateFromPair(pr.first, pr.second, &e)) {
            est.push_back(e);
            CT_DBG("CameraTracker:   focal pair %d/%d -> FOV %.1f deg "
                   "(inliers %d, cost ratio %.3f)\n",
                   e.kfA, e.kfB, e.fovDeg, e.inliers, e.costRatio);
        }
    }
    if (est.empty()) {
        solveStatusDisplay.lock()->setValue("Epipolar geometry unstable — cannot estimate focal");
        return;
    }
    // Drop under-supported pairs before aggregating: near-minimal inlier counts
    // (a wide baseline where most tracks have died) produce garbage FOVs that
    // poison the spread — observed 12° and 114° from 17-19-inlier pairs on a
    // clip whose real pairs clustered at 42-50°.
    int maxInliers = 0;
    for (const auto& e : est) maxInliers = std::max(maxInliers, e.inliers);
    std::vector<PairEstimate> used;
    for (const auto& e : est) {
        if (e.inliers >= 30 && e.inliers >= (3 * maxInliers) / 10) used.push_back(e);
    }
    if (used.empty()) used = est;
    // Sharpness gate calibrated on ground truth: genuinely constrained pairs
    // (lateral motion) show cost ratios 0.63-0.84; flat/degenerate ones
    // (push-in) sit at 0.97-1.0.
    int sharpPairs = 0;
    for (const auto& e : used) if (e.costRatio < 0.85) ++sharpPairs;
    const bool wellConstrained = sharpPairs >= 2;
    std::vector<double> fovs;
    int totInliers = 0;
    for (const auto& e : used) { fovs.push_back(e.fovDeg); totInliers += e.inliers; }
    std::sort(fovs.begin(), fovs.end());
    const double medFov = fovs[fovs.size() / 2];
    const double spread = fovs.back() - fovs.front();
    const bool consistent = spread < std::max(6.0, 0.20 * medFov);

    const double fpx = fovToFpx(medFov);
    const double sensorW = sensorWidth.lock()->getValue();
    const double focalMm = fpx * sensorW / width;

    focalLengthMm.lock()->setValue(focalMm);
    std::stringstream ss;
    ss << "Estimated focal: " << std::fixed << std::setprecision(2) << focalMm
       << "mm (H-FOV " << std::setprecision(1) << medFov << " deg, "
       << used.size() << "/" << est.size() << " pairs, spread "
       << spread << " deg, " << totInliers << " inliers)";
    if (!wellConstrained || !consistent) {
        ss << " — WARNING: low parallax, focal poorly constrained; prefer a known value";
        publicInterface->message(eMessageTypeWarning,
            "The focal length was estimated, but this shot constrains it weakly "
            "(little parallax, or the frame pairs disagree). If you know the real "
            "focal length, enter it instead.");
    }
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: %s\n", ss.str().c_str());
}


// ==================== Camera Solve ====================

void
CameraTrackerNodePrivate::solveCameraMotion()
{
    if (tracks.empty()) {
        solveStatusDisplay.lock()->setValue("No tracks — detect and track features first");
        return;
    }

    solveStatusDisplay.lock()->setValue("Solving...");

    NodePtr node = publicInterface->getNode();
    NodePtr inputNode = node ? node->getInput(0) : NodePtr();
    if (!inputNode) {
        solveStatusDisplay.lock()->setValue("No input connected");
        return;
    }
    EffectInstancePtr input = inputNode->getEffectInstance();
    if (!input) {
        solveStatusDisplay.lock()->setValue("No input connected");
        return;
    }

    // Get image dimensions
    RectD rod;
    RenderScale scale;
    bool isProjectFormat = false;
    int rangeStart = trackRangeStart.lock()->getValue();
    input->getRegionOfDefinition_public(inputNode->getHashValue(), static_cast<double>(rangeStart), scale, ViewIdx(0), &rod, &isProjectFormat);
    int width = static_cast<int>(rod.width());
    int height = static_cast<int>(rod.height());

    // Build camera intrinsics — convert focal length from mm to pixels
    double focalMm = focalLengthMm.lock()->getValue();
    double sensorW = sensorWidth.lock()->getValue();
    double focalPx = (focalMm / sensorW) * static_cast<double>(width);
    double ppx = principalPointX.lock()->getValue();
    double ppy = principalPointY.lock()->getValue();

    // If principal point is 0,0 use image center
    if (std::abs(ppx) < 0.001 && std::abs(ppy) < 0.001) {
        ppx = width / 2.0;
        ppy = height / 2.0;
    }

    CT_DBG("CameraTracker: Focal %.1fmm -> %.1fpx (sensor %.1fmm, image %dx%d)\n",
            focalMm, focalPx, sensorW, width, height);

    libmv::PolynomialCameraIntrinsics intrinsics;
    intrinsics.SetImageSize(width, height);
    intrinsics.SetFocalLength(focalPx, focalPx);
    intrinsics.SetPrincipalPoint(ppx, ppy);

    // Set distortion if enabled
    int distModel = distortionModel.lock()->getValue();
    if (distModel == 1) { // Polynomial
        double dk1 = k1.lock()->getValue();
        double dk2 = k2.lock()->getValue();
        double dk3 = k3.lock()->getValue();
        intrinsics.SetRadialDistortion(dk1, dk2, dk3);
    }

    // Build libmv Tracks from our 2D data, filtering short tracks
    int rangeEnd = trackRangeEnd.lock()->getValue();
    int totalFrameRange = rangeEnd - rangeStart + 1;
    // A 3D point only needs ~2 views to triangulate; require a modest minimum for a
    // stable track but do NOT scale it with shot length. The old range/4 rule
    // demanded 30-frame tracks on a 120-frame pan, which discarded nearly all the
    // short overlapping tracks that re-detection (§8.13) deliberately creates to span
    // the move (log showed 244 of 551 tracks filtered as "short"). Cap the floor at
    // 10 frames so those re-detected tracks actually contribute points.
    int minTrackLength = std::min(10, std::max(5, totalFrameRange / 4));
    // Imported tracks are already quality-controlled by their source, and
    // half of a reference set is shorter than 10 frames (median lifetime ~10) — the
    // length floor exists to reject OUR tracker's early-dying junk, not curated
    // data. Solving on the full imported set matters: the reference camera diff showed
    // our rotation/translation split drifting from ground truth when we solved on
    // only ~60% of the reference markers.
    if (tracksImported) {
        minTrackLength = 3;
    }

    libmv::Tracks rawTracks;
    int filteredOut = 0;
    int usedTracks = 0;
    std::set<int> outlierTracks;   // track ids rejected by outlier filtering (excluded from bundle/error/output)
    const bool manualOnly = solveManualOnly.lock() && solveManualOnly.lock()->getValue();
    for (const auto& track : tracks) {
        // Solve Manual Only: hand-placed tracks exclusively.
        if (manualOnly && !manualTrackIds.count(track.id)) {
            filteredOut++;
            continue;
        }
        // Filter: skip tracks that are too short. Manual tracks are EXEMPT —
        // the user placed them deliberately (even a 2-frame hand-keyed track
        // is a wanted constraint).
        if (static_cast<int>(track.markers.size()) < minTrackLength &&
            !manualTrackIds.count(track.id)) {
            filteredOut++;
            continue;
        }

        // NOTE: an earlier version also dropped tracks starting within 15% of image
        // center ("minimal parallax under FORWARD motion") — but it ran before the
        // forward/lateral classification, so on lateral pans it just deleted good
        // parallax carriers (209 of the reference tracks on the test shot). Removed.

        // Weight this track's bundle residuals by its QUALITY, proxied by
        // length: long survivors are precise corners carrying accumulated
        // parallax; short filler tracks (from the low-contrast detection pass)
        // still contribute coverage and cloud points but can't drag the camera.
        // Saturates at 30 frames; floor keeps every track a participant.
        // ONLY for our own tracker's output: the length=quality assumption is
        // WRONG for imported tracks, whose short tracks are excellent
        // by design (median lifetime ~10 frames) — weighting them dropped a
        // imported-track solve from 0.4% to 9.5% of path before this guard.
        const double trackWeight = (tracksImported || manualTrackIds.count(track.id))
            ? 1.0   // curated data: imported sets and hand-placed tracks
            : std::min(1.0, std::max(0.25, (double)track.markers.size() / 30.0));
        for (const auto& marker : track.markers) {
            int frame = marker.first;
            double natronX = marker.second.first;
            double natronY = marker.second.second;
            // Convert to libmv image coords (top-left origin)
            double mvX = natronX - rod.x1;
            double mvY = (rod.y2 - 1) - natronY;
            rawTracks.Insert(frame, track.id, mvX, mvY, trackWeight);
        }
        ++usedTracks;
    }
    CT_DBG("CameraTracker: Using %d tracks (filtered %d short, min length %d)\n",
            usedTracks, filteredOut, minTrackLength);

    // Guard: libmv's reconstruction is undefined for near-empty input and will
    // segfault rather than fail gracefully. Two-frame init needs >=8 common
    // markers, so anything under ~8 tracks cannot produce a usable solve.
    const int kMinTracksForSolve = 8;
    if (usedTracks < kMinTracksForSolve) {
        std::stringstream ss;
        ss << "Not enough usable tracks to solve (" << usedTracks << " of "
           << tracks.size() << "; need >= " << kMinTracksForSolve << "). "
           << filteredOut << " were too short (min length " << minTrackLength
           << "). Track more features over a longer range, or lower the frame range.";
        solveStatusDisplay.lock()->setValue(ss.str());
        CT_DBG("CameraTracker: %s\n", ss.str().c_str());
        return;
    }

    // Undistort and normalize tracks through intrinsics
    libmv::Tracks calibratedTracks;
    libmv::InvertIntrinsicsForTracks(rawTracks, intrinsics, &calibratedTracks);

    // --- Global dominant-motion rejection (automatic moving-object handling, no mask) ---
    // Production solvers reject a moving subject without a mask by fitting the DOMINANT rigid camera
    // motion across the whole shot and dropping tracks that don't conform. Our per-init
    // RANSAC only checks ONE keyframe pair, where a mover can briefly pass (log showed just
    // 2/352 rejected while the runner still corrupted the solve). Here we RANSAC the
    // fundamental matrix over MANY frame pairs spanning the range and reject tracks that are
    // epipolar OUTLIERS in a majority of the pairs they appear in: a running person violates
    // the static-scene geometry over most baselines, while background tracks fit everywhere.
    if (rejectMovingObjects.lock()->getValue()) {
        libmv::vector<libmv::Marker> allM0 = rawTracks.AllMarkers();
        int fmin = 0, fmax = 0;
        for (int i = 0; i < allM0.size(); ++i) {
            if (i == 0 || allM0[i].image < fmin) fmin = allM0[i].image;
            if (i == 0 || allM0[i].image > fmax) fmax = allM0[i].image;
        }
        const int span = fmax - fmin;
        if (span >= 12) {
            const int baseline = std::min(50, std::max(20, span / 4));
            const int step = std::max(1, span / 12);
            const double thr = 3.0;   // Sampson inlier threshold (px)
            const int iters = 300;
            unsigned int seed = 2463534242u;   // fixed → repeatable
            std::map<int,int> appear, outlie;  // track id -> (# pairs appeared, # pairs outlier)
            int nPairs = 0;

            for (int f0 = fmin; f0 + baseline <= fmax; f0 += step) {
                const int f1 = f0 + baseline;
                std::vector<int> ct;
                std::vector<libmv::Vec2> p1, p2;
                {
                    libmv::vector<libmv::Marker> mk = rawTracks.MarkersForTracksInBothImages(f0, f1);
                    std::map<int, libmv::Vec2> a, b;
                    for (int i = 0; i < mk.size(); ++i) {
                        libmv::Vec2 v; v << mk[i].x, mk[i].y;
                        if (mk[i].image == f0) a[mk[i].track] = v; else b[mk[i].track] = v;
                    }
                    for (std::map<int, libmv::Vec2>::const_iterator it = a.begin(); it != a.end(); ++it) {
                        std::map<int, libmv::Vec2>::const_iterator jt = b.find(it->first);
                        if (jt == b.end()) continue;
                        ct.push_back(it->first); p1.push_back(it->second); p2.push_back(jt->second);
                    }
                }
                const int nC = (int)ct.size();
                if (nC < 20) continue;

                std::vector<char> bestInl(nC, 0);
                int bestCount = 0;
                for (int it = 0; it < iters; ++it) {
                    int idx[8];
                    for (int s = 0; s < 8; ++s) {
                        int r; bool dup;
                        do { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                             r = (int)(seed % (unsigned)nC);
                             dup = false; for (int t = 0; t < s; ++t) if (idx[t] == r) { dup = true; break; }
                        } while (dup);
                        idx[s] = r;
                    }
                    libmv::Mat x1s(2, 8), x2s(2, 8);
                    for (int s = 0; s < 8; ++s) {
                        x1s(0, s) = p1[idx[s]](0); x1s(1, s) = p1[idx[s]](1);
                        x2s(0, s) = p2[idx[s]](0); x2s(1, s) = p2[idx[s]](1);
                    }
                    libmv::Mat3 F; libmv::NormalizedEightPointSolver(x1s, x2s, &F);
                    libmv::Mat Fm = F;
                    std::vector<char> inl(nC, 0); int cnt = 0;
                    for (int i = 0; i < nC; ++i)
                        if (libmv::SampsonDistance(Fm, p1[i], p2[i]) < thr) { inl[i] = 1; ++cnt; }
                    if (cnt > bestCount) { bestCount = cnt; bestInl.swap(inl); }
                }
                if (bestCount < nC / 2) continue;   // no clear consensus on this pair → skip
                ++nPairs;
                for (int i = 0; i < nC; ++i) { appear[ct[i]]++; if (!bestInl[i]) outlie[ct[i]]++; }
            }

            // A track is a moving-object outlier if it violated the dominant motion in a
            // majority of the pairs it appeared in (and appeared in at least 2).
            std::set<int> movers;
            for (std::map<int,int>::const_iterator it = appear.begin(); it != appear.end(); ++it) {
                const int id = it->first, ap = it->second;
                const int ou = outlie.count(id) ? outlie[id] : 0;
                if (ap >= 2 && (double)ou / ap > 0.5) movers.insert(id);
            }
            if (!movers.empty()) {
                for (std::set<int>::const_iterator it = movers.begin(); it != movers.end(); ++it)
                    outlierTracks.insert(*it);
                { libmv::vector<libmv::Marker> kept, all = rawTracks.AllMarkers();
                  for (int i = 0; i < all.size(); ++i) if (!movers.count(all[i].track)) kept.push_back(all[i]);
                  rawTracks = libmv::Tracks(kept); }
                { libmv::vector<libmv::Marker> kept, all = calibratedTracks.AllMarkers();
                  for (int i = 0; i < all.size(); ++i) if (!movers.count(all[i].track)) kept.push_back(all[i]);
                  calibratedTracks = libmv::Tracks(kept); }
            }
            CT_DBG("CameraTracker: Global dominant-motion rejection: %d pairs (baseline %d), "
                   "rejected %d moving tracks\n", nPairs, baseline, (int)movers.size());
        }
    }

    int mode = solveMode.lock()->getValue();

    libmv::EuclideanReconstruction reconstruction;

    if (mode == 1) {
        // Tripod / rotation-only solve
        CT_DBG("CameraTracker: Using tripod (rotation-only) solver\n");
        libmv::ModalSolver(calibratedTracks, &reconstruction, NULL);
    } else {
        // Full camera solve

        // --- Forward-motion detection ---
        // Analyze flow field between first and last tracked frames to detect radial/forward motion
        bool isForwardMotion = false;
        {
            int firstFrame = calibratedTracks.MaxImage();
            int lastFrame = 0;
            // Find actual frame range from tracks
            libmv::vector<libmv::Marker> allMarkers = calibratedTracks.AllMarkers();
            for (int i = 0; i < allMarkers.size(); ++i) {
                if (allMarkers[i].image < firstFrame) firstFrame = allMarkers[i].image;
                if (allMarkers[i].image > lastFrame) lastFrame = allMarkers[i].image;
            }

            if (lastFrame > firstFrame) {
                libmv::vector<libmv::Marker> commonFL =
                    calibratedTracks.MarkersForTracksInBothImages(firstFrame, lastFrame);
                if (commonFL.size() >= 8) {
                    // Check whether flow vectors point radially away from the
                    // image centre (signature of forward/dolly motion).
                    std::map<int, libmv::Marker> frame1Markers, frameLMarkers;
                    for (int i = 0; i < commonFL.size(); ++i) {
                        if (commonFL[i].image == firstFrame)
                            frame1Markers[commonFL[i].track] = commonFL[i];
                        else
                            frameLMarkers[commonFL[i].track] = commonFL[i];
                    }
                    int radial = 0, total = 0;
                    for (auto& kv : frame1Markers) {
                        auto it = frameLMarkers.find(kv.first);
                        if (it == frameLMarkers.end()) continue;
                        double px = kv.second.x, py = kv.second.y;
                        double dx = it->second.x - px, dy = it->second.y - py;
                        double flowLen = std::sqrt(dx * dx + dy * dy);
                        if (flowLen < 1e-6) continue;
                        // Radial direction from center (0,0 in normalized coords)
                        double posLen = std::sqrt(px * px + py * py);
                        if (posLen < 1e-6) continue;
                        // Dot product of flow direction with radial direction
                        double dot = (dx * px + dy * py) / (flowLen * posLen);
                        if (std::abs(dot) > 0.7) radial++; // mostly radial
                        total++;
                    }
                    // Need a solid sample AND a strong radial majority to call it
                    // forward motion. On a long pan only a handful of features survive
                    // end-to-end (the log showed just 7), far too few to trust — a
                    // spurious "forward" call routes the solve down the wrong bundle
                    // path. Default to the general lateral path unless clearly radial.
                    if (total >= 20) {
                        double radialRatio = static_cast<double>(radial) / total;
                        isForwardMotion = (radialRatio > 0.75);
                        CT_DBG("CameraTracker: Flow analysis: %d/%d radial (%.0f%%) — %s\n",
                                radial, total, radialRatio * 100,
                                isForwardMotion ? "FORWARD MOTION DETECTED" : "lateral motion OK");
                    } else {
                        CT_DBG("CameraTracker: Flow analysis: only %d end-to-end samples — "
                               "assuming lateral motion\n", total);
                    }
                }
            }
        }

        // --- Two-stage solve: re-init from scratch on the cleaned tracks ---
        // The first pass's bundle can collapse the camera TRANSLATION when noisy tracks
        // are present (the reference camera diff proved it: our translation path 0.056 vs
        // the reference's 2.54, rotation inflated to compensate → near-nodal → slides). The
        // refine loop culls those tracks but only RE-BUNDLES (local; can't climb out of
        // the collapse). So we run the whole reconstruction AGAIN on the now-cleaned
        // tracks — a fresh two-frame init re-establishes the baseline like clean tracks
        // do. Pass diagnostics log translation-path vs rotation so we can see recovery.
        const int nSolvePasses = 2;
        // Keep the pass with the LEAST-collapsed camera (most translation for its
        // rotation) so a worse second pass can never degrade the result.
        libmv::EuclideanReconstruction bestRecon;
        libmv::Tracks bestRaw;
        libmv::PolynomialCameraIntrinsics bestIntr;
        double bestErr = 1e30;      // reprojection error of the kept pass (gauge-invariant)
        bool bestOk = false;        // kept pass passed the collapse check
        bool haveBest = false;
        for (int solvePass = 0; solvePass < nSolvePasses; ++solvePass) {
        reconstruction = libmv::EuclideanReconstruction();   // fresh reconstruction each pass

        // --- Keyframe selection ---
        libmv::vector<int> keyframes;
        bool useAutoKeyframes = autoKeyframes.lock()->getValue();

        if (useAutoKeyframes) {
            // A good keyframe pair needs BOTH plentiful shared features AND a WIDE
            // baseline (parallax) for well-conditioned triangulation. libmv's
            // GRIC selector sometimes returns a near-adjacent pair (e.g. 1040/1045):
            // tiny parallax -> ill-conditioned depths -> most points land at absurd
            // distances and get culled by outlier rejection -> a sparse (~40-point),
            // high-error solve that only reprojects well right next to the seed.
            // Instead, scan for the pair that maximises shared tracks at a target
            // wide baseline, and only fall back to GRIC if that fails.
            libmv::vector<libmv::Marker> allM = calibratedTracks.AllMarkers();
            int fmin = 0, fmax = 0;
            for (int i = 0; i < allM.size(); ++i) {
                if (i == 0 || allM[i].image < fmin) fmin = allM[i].image;
                if (i == 0 || allM[i].image > fmax) fmax = allM[i].image;
            }
            int span = fmax - fmin;
            if (span >= 8) {
                // Target ~1/5 of the shot, clamped to a sane parallax window.
                int targetBaseline = std::min(span, std::max(20, span / 5));
                int step = std::max(1, span / 40);   // ~40 sampled start positions
                int bestF1 = fmin, bestF2 = fmin + targetBaseline, bestCommon = -1;
                for (int f1 = fmin; f1 + targetBaseline <= fmax; f1 += step) {
                    int f2 = f1 + targetBaseline;
                    // .size() is markers (≈2× common tracks); fine for ranking.
                    int common = (int)calibratedTracks.MarkersForTracksInBothImages(f1, f2).size();
                    if (common > bestCommon) { bestCommon = common; bestF1 = f1; bestF2 = f2; }
                }
                if (bestCommon >= 16) {   // ≥ ~8 common tracks (8-point minimum)
                    keyframes.clear();
                    keyframes.push_back(bestF1);
                    keyframes.push_back(bestF2);
                    CT_DBG("CameraTracker: Wide-baseline keyframes %d/%d (~%d common markers, "
                           "baseline %d, span %d)\n", bestF1, bestF2, bestCommon, targetBaseline, span);
                }
            }
            // Fall back to GRIC if the wide-baseline scan came up short.
            if (keyframes.size() < 2) {
                CT_DBG("CameraTracker: Wide-baseline scan failed — falling back to GRIC\n");
                libmv::SelectKeyframesBasedOnGRICAndVariance(calibratedTracks, intrinsics, keyframes);
            }
        }

        if (!useAutoKeyframes || keyframes.size() < 2) {
            if (useAutoKeyframes) {
                CT_DBG("CameraTracker: Auto keyframes failed — using first/last frames\n");
            }
            // Use manual keyframes, or fall back to first/last frame
            int kf1 = keyframe1.lock()->getValue();
            int kf2 = keyframe2.lock()->getValue();
            if (useAutoKeyframes) {
                // Auto failed — pick first and last frame with tracks
                libmv::vector<libmv::Marker> allM = calibratedTracks.AllMarkers();
                kf1 = allM[0].image;
                kf2 = allM[0].image;
                for (int i = 0; i < allM.size(); ++i) {
                    if (allM[i].image < kf1) kf1 = allM[i].image;
                    if (allM[i].image > kf2) kf2 = allM[i].image;
                }
            }
            keyframes.clear();
            keyframes.push_back(kf1);
            keyframes.push_back(kf2);
        }

        // Update keyframe display
        keyframe1.lock()->setValue(keyframes[0]);
        keyframe2.lock()->setValue(keyframes[1]);

        CT_DBG("CameraTracker: Keyframes: %d and %d\n", keyframes[0], keyframes[1]);

        // --- RANSAC robust outlier rejection (automatic moving-object handling) ---
        // libmv's two-frame init uses a plain 8-point solve on ALL correspondences,
        // so a moving object (a running person, cars) corrupts the whole solve. Here
        // we robustly estimate the fundamental matrix between the two keyframes with
        // RANSAC and drop tracks that don't fit the dominant (static-scene) epipolar
        // geometry — the automatic rejection production solvers do. Static points satisfy the
        // camera's epipolar geometry (small Sampson distance); moving-object points
        // violate it and become the rejected outliers.
        {
            const int kfa = keyframes[0], kfb = keyframes[1];
            std::vector<int> corrTracks;
            std::vector<libmv::Vec2> p1, p2;
            {
                libmv::vector<libmv::Marker> mk = rawTracks.MarkersForTracksInBothImages(kfa, kfb);
                std::map<int, libmv::Vec2> a, b;
                for (int i = 0; i < mk.size(); ++i) {
                    libmv::Vec2 v; v << mk[i].x, mk[i].y;
                    if (mk[i].image == kfa) a[mk[i].track] = v; else b[mk[i].track] = v;
                }
                for (std::map<int, libmv::Vec2>::const_iterator it = a.begin(); it != a.end(); ++it) {
                    std::map<int, libmv::Vec2>::const_iterator jt = b.find(it->first);
                    if (jt == b.end()) continue;
                    corrTracks.push_back(it->first);
                    p1.push_back(it->second);
                    p2.push_back(jt->second);
                }
            }
            const int nCorr = (int)corrTracks.size();
            if (nCorr >= 20) {
                unsigned int seed = 2463534242u; // fixed seed → repeatable solves
                const double thr = 4.0;          // Sampson inlier threshold (px)
                const int iters = 500;
                std::vector<char> bestInliers(nCorr, 0);
                int bestCount = 0;
                for (int it = 0; it < iters; ++it) {
                    int idx[8];
                    for (int s = 0; s < 8; ++s) {
                        int r; bool dup;
                        do {
                            seed = seed * 1664525u + 1013904223u;
                            r = (int)(seed % (unsigned int)nCorr);
                            dup = false; for (int t = 0; t < s; ++t) if (idx[t] == r) { dup = true; break; }
                        } while (dup);
                        idx[s] = r;
                    }
                    libmv::Mat x1s(2, 8), x2s(2, 8);
                    for (int s = 0; s < 8; ++s) {
                        x1s(0, s) = p1[idx[s]](0); x1s(1, s) = p1[idx[s]](1);
                        x2s(0, s) = p2[idx[s]](0); x2s(1, s) = p2[idx[s]](1);
                    }
                    libmv::Mat3 F;
                    libmv::NormalizedEightPointSolver(x1s, x2s, &F);
                    libmv::Mat Fm = F; // Mat3 -> dynamic Mat for SampsonDistance
                    std::vector<char> inl(nCorr, 0);
                    int count = 0;
                    for (int i = 0; i < nCorr; ++i) {
                        if (libmv::SampsonDistance(Fm, p1[i], p2[i]) < thr) { inl[i] = 1; ++count; }
                    }
                    if (count > bestCount) { bestCount = count; bestInliers.swap(inl); }
                }

                std::set<int> ransacOutliers;
                for (int i = 0; i < nCorr; ++i)
                    if (!bestInliers[i]) ransacOutliers.insert(corrTracks[i]);

                // Only apply when RANSAC found a clear dominant (static) consensus and
                // flagged some outliers — avoids nuking a shot with no real outliers.
                if (bestCount >= nCorr / 2 && bestCount >= 12 && !ransacOutliers.empty()) {
                    for (std::set<int>::const_iterator it = ransacOutliers.begin(); it != ransacOutliers.end(); ++it)
                        outlierTracks.insert(*it);
                    {
                        libmv::vector<libmv::Marker> kept, all = rawTracks.AllMarkers();
                        for (int i = 0; i < all.size(); ++i)
                            if (ransacOutliers.find(all[i].track) == ransacOutliers.end()) kept.push_back(all[i]);
                        rawTracks = libmv::Tracks(kept);
                    }
                    {
                        libmv::vector<libmv::Marker> kept, all = calibratedTracks.AllMarkers();
                        for (int i = 0; i < all.size(); ++i)
                            if (ransacOutliers.find(all[i].track) == ransacOutliers.end()) kept.push_back(all[i]);
                        calibratedTracks = libmv::Tracks(kept);
                    }
                    CT_DBG("CameraTracker: RANSAC init rejected %d/%d moving/outlier tracks (%d inliers, thr %.1fpx)\n",
                           (int)ransacOutliers.size(), nCorr, bestCount, thr);
                }
            }
        }

        // --- Attempt standard two-frame initialization ---
        // Try the selected keyframe pair first, then try other pairs if it fails
        bool initOk = false;
        {
            // Build list of keyframe pairs to try (primary pair first, then alternatives)
            std::vector<std::pair<int, int>> pairsToTry;
            pairsToTry.push_back(std::make_pair(keyframes[0], keyframes[1]));

            // Also try intermediate pairs for better conditioning
            int mid = (keyframes[0] + keyframes[1]) / 2;
            pairsToTry.push_back(std::make_pair(keyframes[0], mid));
            pairsToTry.push_back(std::make_pair(mid, keyframes[1]));

            // Try quarter points
            int q1 = keyframes[0] + (keyframes[1] - keyframes[0]) / 4;
            int q3 = keyframes[0] + 3 * (keyframes[1] - keyframes[0]) / 4;
            pairsToTry.push_back(std::make_pair(q1, q3));
            pairsToTry.push_back(std::make_pair(keyframes[0], q3));
            pairsToTry.push_back(std::make_pair(q1, keyframes[1]));

            for (size_t p = 0; p < pairsToTry.size() && !initOk; ++p) {
                int kf1 = pairsToTry[p].first;
                int kf2 = pairsToTry[p].second;
                if (kf1 == kf2) continue;

                libmv::vector<libmv::Marker> commonMarkers =
                    calibratedTracks.MarkersForTracksInBothImages(kf1, kf2);

                if (commonMarkers.size() < 8) continue;

                // Try reconstruction with this pair
                libmv::EuclideanReconstruction testRecon;
                bool ok = libmv::EuclideanReconstructTwoFrames(commonMarkers, &testRecon);

                if (ok) {
                    // EuclideanReconstructTwoFrames only creates 2 cameras, no 3D points yet.
                    // Validate by checking that both cameras were created.
                    libmv::EuclideanCamera* cam1 = testRecon.CameraForImage(kf1);
                    libmv::EuclideanCamera* cam2 = testRecon.CameraForImage(kf2);

                    CT_DBG("CameraTracker: Pair (%d,%d): %d common markers, "
                            "cam1=%s cam2=%s\n",
                            kf1, kf2, (int)commonMarkers.size(),
                            cam1 ? "OK" : "NULL", cam2 ? "OK" : "NULL");

                    if (cam1 && cam2) {
                        reconstruction = testRecon;
                        keyframes[0] = kf1;
                        keyframes[1] = kf2;
                        initOk = true;
                        CT_DBG("CameraTracker: Two-frame init succeeded with pair (%d,%d)\n",
                                kf1, kf2);
                    }
                }
            }

            if (!initOk) {
                CT_DBG("CameraTracker: All two-frame init attempts FAILED\n");
            }
        }

        // --- Fallback: Rotation-first approach ---
        if (!initOk) {
            CT_DBG("CameraTracker: Falling back to rotation-first approach\n");
            solveStatusDisplay.lock()->setValue("Two-frame init failed — trying rotation-first approach...");

            // Step 1: Solve rotation-only across all frames
            libmv::EuclideanReconstruction rotOnlyRecon;
            libmv::ModalSolver(calibratedTracks, &rotOnlyRecon, NULL);

            // Check if ModalSolver produced cameras
            libmv::vector<libmv::EuclideanCamera> rotCams = rotOnlyRecon.AllCameras();
            CT_DBG("CameraTracker: ModalSolver produced %d cameras\n",
                    (int)rotCams.size());

            if (rotCams.size() >= 2) {
                // Use the rotation-only solution as our reconstruction
                // The ModalSolver projects points onto a unit sphere, which gives us
                // rotation and approximate point directions (but no depth/translation)
                reconstruction = rotOnlyRecon;

                // Try to complete reconstruction by adding translation via resection
                // The rotation is good; incremental reconstruction may recover some translation
                CT_DBG("CameraTracker: Attempting incremental reconstruction from rotation-only base\n");
                libmv::EuclideanCompleteReconstruction(calibratedTracks, &reconstruction, NULL);

                initOk = true;
            }
        }

        if (!initOk) {
            solveStatusDisplay.lock()->setValue(
                "Solve failed — could not initialize reconstruction. "
                "Tips: use footage with lateral camera motion, set accurate focal length, "
                "or try Tripod mode.");
            return;
        }

        // --- Complete reconstruction (if two-frame succeeded, add remaining frames) ---
        if (!isForwardMotion || reconstruction.AllCameras().size() < 3) {
            CT_DBG("CameraTracker: Completing reconstruction...\n");
            libmv::EuclideanCompleteReconstruction(calibratedTracks, &reconstruction, NULL);
        }

        CT_DBG("CameraTracker: After completion: %d cameras, %d points\n",
                (int)reconstruction.AllCameras().size(),
                (int)reconstruction.AllPoints().size());

        // --- Outlier rejection ---
        // Remove 3D points that are behind cameras or at extreme distances
        {
            libmv::vector<libmv::EuclideanPoint> allPts = reconstruction.AllPoints();
            libmv::vector<libmv::EuclideanCamera> allCams = reconstruction.AllCameras();

            // Compute median distance to get a sense of scene scale
            std::vector<double> distances;
            for (int i = 0; i < allPts.size(); ++i) {
                double dist = allPts[i].X.norm();
                distances.push_back(dist);
            }
            if (!distances.empty()) {
                std::sort(distances.begin(), distances.end());
                double medianDist = distances[distances.size() / 2];
                double maxAllowedDist = medianDist * 100.0; // 100x median = outlier

                for (int i = 0; i < allPts.size(); ++i) {
                    double dist = allPts[i].X.norm();
                    bool isBehindCamera = false;

                    // Check if point is behind any camera that sees it
                    libmv::vector<libmv::Marker> trackMarkers = calibratedTracks.MarkersForTrack(allPts[i].track);
                    for (int m = 0; m < trackMarkers.size(); ++m) {
                        libmv::EuclideanCamera* cam = reconstruction.CameraForImage(trackMarkers[m].image);
                        if (cam) {
                            // Point in camera space: P_cam = R * P_world + t
                            libmv::Vec3 pCam = cam->R * allPts[i].X + cam->t;
                            if (pCam(2) < 0) { // behind camera (negative Z in camera space)
                                isBehindCamera = true;
                                break;
                            }
                        }
                    }

                    if (isBehindCamera || dist > maxAllowedDist || dist < 1e-8) {
                        // Flag for removal. (libmv has no RemovePoint; the old code
                        // teleported the point to 1e10, which does NOT remove it --
                        // it stays in the reconstruction and poisons bundle adjustment
                        // and the error metric with enormous residuals. Instead we drop
                        // the track's observations from rawTracks below and skip the
                        // point at extraction.)
                        outlierTracks.insert(allPts[i].track);
                    }
                }
                CT_DBG("CameraTracker: Outlier rejection: flagged %d/%d points (median dist=%.2f, max=%.2f)\n",
                        (int)outlierTracks.size(), (int)allPts.size(), medianDist, maxAllowedDist);
            }
        }

        // Drop flagged outliers from the observations the bundle + error use, so
        // they neither distort the solve nor inflate the reported residual.
        if (!outlierTracks.empty()) {
            libmv::vector<libmv::Marker> kept;
            libmv::vector<libmv::Marker> all = rawTracks.AllMarkers();
            for (int i = 0; i < all.size(); ++i) {
                if (outlierTracks.find(all[i].track) == outlierTracks.end()) {
                    kept.push_back(all[i]);
                }
            }
            rawTracks = libmv::Tracks(kept);
        }

        // --- Bundle adjustment ---
        CT_DBG("CameraTracker: Running bundle adjustment...\n");

        int bundleFlags = libmv::BUNDLE_NO_INTRINSICS;
        // For forward motion, do NOT refine focal length in initial solve
        if (refineFocalLength.lock()->getValue() && !isForwardMotion) {
            bundleFlags |= libmv::BUNDLE_FOCAL_LENGTH;
        }
        if (refinePrincipalPoint.lock()->getValue()) {
            bundleFlags |= libmv::BUNDLE_PRINCIPAL_POINT;
        }
        if (refineLensDistortion.lock()->getValue()) {
            bundleFlags |= libmv::BUNDLE_RADIAL_K1 | libmv::BUNDLE_RADIAL_K2;
        }

        int bundleConstraints = libmv::BUNDLE_NO_CONSTRAINTS;

        // --- Path-smoothness prior weight (scale-normalized) ---
        // The prior residual lives in SCENE units while reprojection residuals are
        // in pixels, and scene scale is an arbitrary gauge — so normalize by the
        // current camera path so the knob behaves the same on every shot:
        //   weight = lambda * 10 * nSteps / pathLength
        // making a typical per-frame step contribute ~10*lambda px-equivalent when
        // fully "bent". Computed from the reconstruction as it stands (post
        // resect/intersect), which is accurate enough for a regularizer scale.
        double smoothWeight = 0.0;
        {
            const double lambda = pathSmoothness.lock()->getValue();
            if (lambda > 0.0) {
                libmv::vector<libmv::EuclideanCamera> cams = reconstruction.AllCameras();
                std::sort(cams.begin(), cams.end(),
                          [](const libmv::EuclideanCamera& a, const libmv::EuclideanCamera& b){ return a.image < b.image; });
                double pathLen = 0.0; int nSteps = 0;
                libmv::Vec3 pc; bool have = false;
                for (int i = 0; i < (int)cams.size(); ++i) {
                    if (cams[i].image < 0) continue;
                    libmv::Vec3 C = -cams[i].R.transpose() * cams[i].t;
                    if (have) { pathLen += (C - pc).norm(); nSteps++; }
                    pc = C; have = true;
                }
                if (nSteps > 2 && pathLen > 1e-12) {
                    smoothWeight = lambda * 10.0 * (double)nSteps / pathLen;
                }
                CT_DBG("CameraTracker: Path smoothness lambda=%.3f -> weight=%.4f (path=%.4f, %d steps)\n",
                       lambda, smoothWeight, pathLen, nSteps);
            }
        }

        // IMPORTANT: the intrinsics-aware bundle re-applies the intrinsics inside
        // its cost functor (projects point -> applies K -> pixels), so it must be
        // given RAW PIXEL tracks, NOT the normalized calibratedTracks. Passing
        // normalized markers here makes every residual ~|principal point| px,
        // which diverges (Ceres "infinite cost") and collapses the reconstruction.
        // The reconstruction itself stays in the normalized metric frame; only the
        // observations need to be pixels. (This is how Blender drives libmv.)
        libmv::EuclideanBundleCommonIntrinsics(
            rawTracks,
            bundleFlags,
            bundleConstraints,
            &reconstruction,
            &intrinsics,
            NULL,
            smoothWeight);

        // For forward motion, do a second bundle pass WITH focal refinement if requested.
        // Tight ±10% trust region: forward motion barely constrains focal (reprojection
        // error is nearly flat in it), so an unbounded refine drifts — measured on the
        // control-room push-in: 58.7° FOV dragged to 65.7° (truth 55.9°) for a 0.02px
        // gain, and the drifted camera was 3.5x worse against ground truth in rotation.
        // The band still lets it rescue a modestly-wrong focal without wandering.
        if (isForwardMotion && refineFocalLength.lock()->getValue()) {
            CT_DBG("CameraTracker: Second bundle pass with focal refinement (forward motion, ±10%% trust region)\n");
            bundleFlags |= libmv::BUNDLE_FOCAL_LENGTH;
            libmv::EuclideanBundleCommonIntrinsics(
                rawTracks,
                bundleFlags,
                bundleConstraints,
                &reconstruction,
                &intrinsics,
                NULL,
                smoothWeight,
                1.10);
        }

        CT_DBG("CameraTracker: Bundle adjustment complete\n");

        // --- Iterative bad-track rejection (standard auto-clean) ---
        // Solve -> drop tracks whose mean reprojection error exceeds the threshold
        // -> re-bundle, a few passes. Removed tracks are excluded from the bundle,
        // the reported error, and the output cloud (via outlierTracks).
        {
            double maxTrackErr = maxTrackError.lock()->getValue();
            for (int pass = 0; maxTrackErr > 0.0 && pass < 3; ++pass) {
                std::map<int, std::pair<double, int> > terr; // track -> (sum err, count)
                libmv::vector<libmv::Marker> mks = rawTracks.AllMarkers();
                for (int i = 0; i < mks.size(); ++i) {
                    libmv::EuclideanCamera* c = reconstruction.CameraForImage(mks[i].image);
                    libmv::EuclideanPoint* pt = reconstruction.PointForTrack(mks[i].track);
                    if (!c || !pt) continue;
                    libmv::Vec3 Pc = c->R * pt->X + c->t;
                    if (std::abs(Pc(2)) < 1e-12) continue;
                    double ix = 0, iy = 0;
                    intrinsics.ApplyIntrinsics(Pc(0) / Pc(2), Pc(1) / Pc(2), &ix, &iy);
                    double ex = ix - mks[i].x, ey = iy - mks[i].y;
                    std::pair<double, int>& a = terr[mks[i].track];
                    a.first += std::sqrt(ex * ex + ey * ey);
                    a.second += 1;
                }
                std::set<int> bad;
                for (std::map<int, std::pair<double, int> >::const_iterator it = terr.begin(); it != terr.end(); ++it) {
                    if (it->second.second > 0 && it->second.first / it->second.second > maxTrackErr) {
                        bad.insert(it->first);
                    }
                }
                if (bad.empty()) break;

                libmv::vector<libmv::Marker> kept;
                libmv::vector<libmv::Marker> all = rawTracks.AllMarkers();
                for (int i = 0; i < all.size(); ++i) {
                    if (bad.find(all[i].track) == bad.end()) kept.push_back(all[i]);
                }
                rawTracks = libmv::Tracks(kept);
                for (std::set<int>::const_iterator it = bad.begin(); it != bad.end(); ++it) {
                    outlierTracks.insert(*it);
                }
                CT_DBG("CameraTracker: refine pass %d removed %d tracks over %.2fpx; re-bundling\n",
                        pass, (int)bad.size(), maxTrackErr);
                // Same tight focal trust as the forward-motion pass: bundleFlags may
                // carry BUNDLE_FOCAL_LENGTH here, and each re-bundle re-anchors the
                // trust region at the current focal — a loose band would let drift
                // compound across outlier-removal passes.
                libmv::EuclideanBundleCommonIntrinsics(rawTracks, bundleFlags, bundleConstraints,
                                                       &reconstruction, &intrinsics, NULL,
                                                       smoothWeight, 1.10);
            }
        }

        // --- Global re-triangulation + final polish bundles (COLMAP-style, §7b P2) ---
        // Incremental reconstruction bakes early-solve error into points that were
        // triangulated from not-yet-converged cameras. Re-intersect every surviving
        // track from the FINAL cameras, then re-bundle with a tightened Huber scale
        // (§7b P3: 2.0px suits the 30px era; the converged solve sits at 0.4-0.8px,
        // so 1.2px stops stragglers from pulling the path). Two rounds.
        {
            for (int rt = 0; rt < 2; ++rt) {
                // EuclideanIntersect wants NORMALIZED markers. Re-normalize from
                // the FILTERED rawTracks with the CURRENT intrinsics each round —
                // the function-level calibratedTracks is stale on both counts
                // (pre-outlier-filter, pre-focal/distortion-refinement).
                libmv::Tracks freshCalibrated;
                libmv::InvertIntrinsicsForTracks(rawTracks, intrinsics, &freshCalibrated);
                libmv::vector<libmv::Marker> nmks = freshCalibrated.AllMarkers();
                std::map<int, libmv::vector<libmv::Marker> > byTrack;
                for (int i = 0; i < nmks.size(); ++i) {
                    const libmv::Marker& m = nmks[i];
                    if (!reconstruction.CameraForImage(m.image)) continue;
                    byTrack[m.track].push_back(m);
                }
                int reTri = 0;
                for (std::map<int, libmv::vector<libmv::Marker> >::iterator it = byTrack.begin();
                     it != byTrack.end(); ++it) {
                    if (it->second.size() < 2) continue;
                    if (libmv::EuclideanIntersect(it->second, &reconstruction)) ++reTri;
                }
                libmv::EuclideanBundleCommonIntrinsics(rawTracks, bundleFlags, bundleConstraints,
                                                       &reconstruction, &intrinsics, NULL,
                                                       smoothWeight, 1.10, 1.2 /*huber*/);
                CT_DBG("CameraTracker: re-triangulation round %d: %d/%d tracks re-intersected, re-bundled (huber 1.2)\n",
                       rt + 1, reTri, (int)byTrack.size());
            }
        }

        // --- Re-resect the gauge-locked first camera (train-clip hinge fix) ---
        // The first camera is locked to identity during bundling to fix the
        // gauge, so any error the two-view init left there can never be
        // optimized away: the rest of the solution converges consistently
        // around the wrong anchor, leaving a rotation/position hinge at frame
        // one (measured: 16.4° jump + 300x step spike on the train clip).
        // With the final geometry in hand, re-estimate that camera like any
        // other frame — the gauge is already carried by the other 100+ cameras.
        {
            libmv::Tracks freshCalibrated;
            libmv::InvertIntrinsicsForTracks(rawTracks, intrinsics, &freshCalibrated);
            int firstImage = -1;
            libmv::vector<libmv::EuclideanCamera> cams = reconstruction.AllCameras();
            for (int i = 0; i < cams.size(); ++i) {
                if (firstImage < 0 || cams[i].image < firstImage) firstImage = cams[i].image;
            }
            libmv::vector<libmv::Marker> mks = freshCalibrated.MarkersInImage(firstImage);
            libmv::vector<libmv::Marker> usable;
            for (int i = 0; i < mks.size(); ++i) {
                if (reconstruction.PointForTrack(mks[i].track)) usable.push_back(mks[i]);
            }
            if (firstImage >= 0 && usable.size() >= 8 &&
                libmv::EuclideanResect(usable, &reconstruction, true)) {
                CT_DBG("CameraTracker: re-resected gauge-locked first camera (frame %d, %d markers)\n",
                       firstImage, (int)usable.size());
            }
        }

        // --- Per-pass diagnostic: translation path vs rotation swing (collapse check) ---
        {
            libmv::vector<libmv::EuclideanCamera> cams = reconstruction.AllCameras();
            std::sort(cams.begin(), cams.end(),
                      [](const libmv::EuclideanCamera& a, const libmv::EuclideanCamera& b){ return a.image < b.image; });
            double tpath = 0.0, rswing = 0.0;
            libmv::Vec3 pc; libmv::Mat3 pr; bool have = false;
            for (int i = 0; i < (int)cams.size(); ++i) {
                if (cams[i].image < 0) continue;
                libmv::Vec3 C = -cams[i].R.transpose() * cams[i].t;
                if (have) {
                    tpath += (C - pc).norm();
                    libmv::Mat3 rel = cams[i].R * pr.transpose();
                    double tr = (rel(0,0) + rel(1,1) + rel(2,2) - 1.0) / 2.0;
                    tr = std::max(-1.0, std::min(1.0, tr));
                    rswing += std::acos(tr);
                }
                pc = C; pr = cams[i].R; have = true;
            }
            // Gauge-invariant pass quality: raw translation path length depends on the
            // reconstruction's arbitrary scale (a bigger-gauge pass previously ALWAYS
            // outscored a smaller one regardless of quality). Normalize the collapse
            // check by scene depth, and rank healthy passes by REPROJECTION ERROR.
            double medianDepth = 0.0;
            {
                libmv::vector<libmv::EuclideanPoint> pts = reconstruction.AllPoints();
                libmv::Vec3 C0 = libmv::Vec3::Zero();
                for (int i = 0; i < (int)cams.size(); ++i) {
                    if (cams[i].image >= 0) { C0 = -cams[i].R.transpose() * cams[i].t; break; }
                }
                std::vector<double> d;
                d.reserve(pts.size());
                for (int i = 0; i < (int)pts.size(); ++i) {
                    if (pts[i].track < 0) continue;
                    d.push_back((pts[i].X - C0).norm());
                }
                if (!d.empty()) {
                    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
                    medianDepth = d[d.size() / 2];
                }
            }
            // Nodal collapse: translation tiny relative to scene depth per radian of
            // rotation (a healthy solve of this shot sits around ~1; collapse ~0.01).
            const double collapseRatio =
                tpath / (std::max(rswing, 0.05) * std::max(medianDepth, 1e-12));
            const double reproErr =
                libmv::EuclideanReprojectionError(rawTracks, reconstruction, intrinsics);
            int nCams = 0; for (int i = 0; i < (int)cams.size(); ++i) if (cams[i].image >= 0) ++nCams;
            const bool passOk = (nCams >= 8) && (collapseRatio > 0.02);

            CT_DBG("CameraTracker: [solve pass %d] translation path=%.4f, rotation swing=%.4f rad, "
                   "%d cams, %d pts, medianDepth=%.3f, collapseRatio=%.4f, reproErr=%.3fpx%s\n",
                    solvePass, tpath, rswing, (int)cams.size(),
                    (int)reconstruction.AllPoints().size(),
                    medianDepth, collapseRatio, reproErr, passOk ? "" : " [COLLAPSED/SPARSE]");

            // Keep the best pass: a healthy pass always beats a collapsed one; among
            // equals, lower reprojection error wins.
            bool better;
            if (!haveBest) {
                better = true;
            } else if (passOk != bestOk) {
                better = passOk;
            } else {
                better = (reproErr < bestErr);
            }
            if (better) {
                bestErr = reproErr; bestOk = passOk; haveBest = true;
                bestRecon = reconstruction; bestRaw = rawTracks; bestIntr = intrinsics;
            }

            // A healthy, sub-1.5px pass 1 makes the re-init pass redundant — the
            // second pass exists to escape the collapse basin, and we're not in it.
            // collapseRatio > 0.5 is the real health bar: passOk's 0.02 floor only
            // detects TOTAL collapse (train solved at 0.085 — translation collapsed,
            // depth blown to 197 — and skipped its rescue). Marginal ratios now run
            // the re-init pass; a genuinely rotation-dominant shot just spends a few
            // extra seconds and best-pass selection keeps whichever scored better.
            if (solvePass == 0 && passOk && reproErr < 1.5 && collapseRatio > 0.5) {
                CT_DBG("CameraTracker: pass 0 healthy (%.3fpx, ratio %.3f) — skipping re-init pass\n",
                       reproErr, collapseRatio);
                break;
            }
        }

        // If the first pass didn't cull anything, a second pass would be identical — stop.
        if (solvePass == 0 && outlierTracks.empty()) break;
        }  // end two-stage solve loop

        // Restore the least-collapsed pass for the error computation + camera extraction.
        if (haveBest) { reconstruction = bestRecon; rawTracks = bestRaw; intrinsics = bestIntr; }

        // Update intrinsics knobs if refined (convert px back to mm). The focal is
        // now bounded INSIDE the bundle (bundle.cc keeps it within 0.5x–2x per pass),
        // so it can no longer run away and corrupt the reconstruction — the old
        // revert-and-relock guard here caused a focal/geometry mismatch (35mm locked
        // onto 139mm-shaped points -> ~34px error) and has been removed.
        if (refineFocalLength.lock()->getValue()) {
            double refinedPx = intrinsics.focal_length();
            double refinedMm = refinedPx * sensorW / static_cast<double>(width);
            focalLengthMm.lock()->setValue(refinedMm);
            CT_DBG("CameraTracker: Refined focal: %.1fpx -> %.1fmm\n", refinedPx, refinedMm);
        }
        if (refinePrincipalPoint.lock()->getValue()) {
            principalPointX.lock()->setValue(intrinsics.principal_point_x());
            principalPointY.lock()->setValue(intrinsics.principal_point_y());
        }
        if (refineLensDistortion.lock()->getValue()) {
            k1.lock()->setValue(intrinsics.k1());
            k2.lock()->setValue(intrinsics.k2());
            k3.lock()->setValue(intrinsics.k3());
        }
    }

    // Compute reprojection error (overall average, px)
    double reproError = libmv::EuclideanReprojectionError(rawTracks, reconstruction, intrinsics);

    // Per-frame residual (px) so it can be compared frame-by-frame against other
    // trackers (e.g. a per-frame solve-residual column). Mirrors
    // libmv's projection: P_cam = R*X + t -> normalize -> apply intrinsics -> px.
    std::map<int, std::pair<double, int> > frameErr; // image -> (sum residual, count)
    std::map<int, std::pair<double, int> > trackErrMap; // track id -> (sum residual, count) for overlay coloring
    // Also split by track WEIGHT tier: the display error used to be comparable
    // across runs, but with quality-weighted markers the plain mean is dominated
    // by the deliberately-included weak filler tracks (down-weighted in the
    // bundle, fully counted in a plain mean). Report the strong-track error —
    // that's the number comparable to the pre-filler era and to what the bundle
    // actually optimizes hardest.
    double strongSum = 0.0; int strongCount = 0;
    {
        libmv::vector<libmv::Marker> mks = rawTracks.AllMarkers();
        for (int i = 0; i < mks.size(); ++i) {
            libmv::EuclideanCamera* cam = reconstruction.CameraForImage(mks[i].image);
            libmv::EuclideanPoint* pt = reconstruction.PointForTrack(mks[i].track);
            if (!cam || !pt) {
                continue;
            }
            libmv::Vec3 Pc = cam->R * pt->X + cam->t;
            if (std::abs(Pc(2)) < 1e-12) {
                continue;
            }
            double ix = 0, iy = 0;
            intrinsics.ApplyIntrinsics(Pc(0) / Pc(2), Pc(1) / Pc(2), &ix, &iy);
            double ex = ix - mks[i].x;
            double ey = iy - mks[i].y;
            double e = std::sqrt(ex * ex + ey * ey);
            std::pair<double, int>& acc = frameErr[mks[i].image];
            acc.first += e; acc.second += 1;
            std::pair<double, int>& tacc = trackErrMap[mks[i].track];
            tacc.first += e; tacc.second += 1;
            if (mks[i].weight >= 0.99) { strongSum += e; strongCount += 1; }
        }
    }
    const double strongErr = strongCount ? strongSum / strongCount : 0.0;
    CT_DBG("CameraTracker: residuals — strong tracks %.3fpx (%d obs), all tracks %.3fpx\n",
           strongErr, strongCount, reproError);

    // Tag each 2D track with its mean reprojection error so the viewer overlay can
    // colour it (green=good → red=bad), and flag rejected outliers. -1 = not solved.
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (outlierTracks.find(tracks[i].id) != outlierTracks.end()) {
            tracks[i].error = -2.0; // rejected by outlier/bad-track filtering
            continue;
        }
        std::map<int, std::pair<double, int> >::const_iterator te = trackErrMap.find(tracks[i].id);
        tracks[i].error = (te != trackErrMap.end() && te->second.second > 0)
                          ? te->second.first / te->second.second : -1.0;
    }
    refreshManualList();   // pick up the fresh per-track errors

    // Extract solved cameras
    solvedCameras.clear();
    libmv::vector<libmv::EuclideanCamera> allCams = reconstruction.AllCameras();
    for (size_t i = 0; i < allCams.size(); ++i) {
        const libmv::EuclideanCamera& cam = allCams[i];
        if (cam.image < 0) continue;

        SolvedCamera sc;
        sc.frame = cam.image;
        {
            std::map<int, std::pair<double, int> >::const_iterator fe = frameErr.find(cam.image);
            sc.residual = (fe != frameErr.end() && fe->second.second > 0)
                          ? fe->second.first / fe->second.second : 0.0;
        }

        // --- Camera world position (camera-to-world translation) ---
        // libmv stores world->camera: P_cam = R*X + t, so the camera centre in
        // world is C = -R^T * t.
        // WORLD FLIP: libmv solves in a computer-vision world frame (Y-down,
        // Z-forward). To present it right-side-up in Natron's Y-up world we rotate
        // the ENTIRE reconstruction (cameras + points) 180 deg about X, i.e. apply
        // F = diag(1,-1,-1) on the LEFT (world space) -> negate world Y and Z.
        // (This is separate from the per-camera local-axis flip below; the same F
        // must be applied to the 3D points at extraction so they stay registered.)
        const libmv::Mat3 Rt = cam.R.transpose();
        const libmv::Vec3 C  = -Rt * cam.t;
        sc.tx =  C(0);
        sc.ty = -C(1);
        sc.tz = -C(2);

        // --- Camera orientation (camera-to-world) ---
        // Local CV->Natron flip:  M_natron = R^T * F   (camera looks down -Z, Y up).
        // World flip (right-side-up): M' = F * M_natron  -> negate rows 1 and 2.
        // First keyframe (R=I) then gives M' = F*F = I -> Euler (0,0,0), the natural
        // upright result.
        double M[3][3];
        for (int r = 0; r < 3; ++r) {
            M[r][0] =  cam.R(0, r);
            M[r][1] = -cam.R(1, r);
            M[r][2] = -cam.R(2, r);
        }
        // F * M : scale rows 1,2 by -1 (world flip)
        M[1][0] = -M[1][0]; M[1][1] = -M[1][1]; M[1][2] = -M[1][2];
        M[2][0] = -M[2][0]; M[2][1] = -M[2][1]; M[2][2] = -M[2][2];

        // Canonical extrinsic-XYZ Euler decomposition (degrees), gimbal-safe.
        RotationConventions::decompose(M, sc.rx, sc.ry, sc.rz);

        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) sc.R[r][c] = M[r][c];

        solvedCameras.push_back(sc);
    }

    // Sort by frame
    std::sort(solvedCameras.begin(), solvedCameras.end(),
              [](const SolvedCamera& a, const SolvedCamera& b) { return a.frame < b.frame; });

    // --- Camera census (FABLE D1) ---
    // Frames that fail resection never get a camera (resect.cc EPnP failure path
    // returns false; the projective fallback is disabled), so solvedCameras can be
    // silently sparser than the tracked range. Downstream keyframing interpolates
    // across the gaps, which reads as "static then jump" in the exported path —
    // so any gap found here is a prime suspect for the lumpy translation.
    if (!solvedCameras.empty()) {
        const int firstF = solvedCameras.front().frame;
        const int lastF  = solvedCameras.back().frame;
        const int span   = lastF - firstF + 1;
        const int missing = span - static_cast<int>(solvedCameras.size());
        CT_DBG("CameraTracker: [diag] camera census: %d cameras over frames %d-%d (%d missing)\n",
               (int)solvedCameras.size(), firstF, lastF, missing);
        if (missing > 0) {
            for (size_t i = 1; i < solvedCameras.size(); ++i) {
                const int gap = solvedCameras[i].frame - solvedCameras[i-1].frame;
                if (gap > 1) {
                    CT_DBG("CameraTracker: [diag]   missing frames %d-%d (resect failed?)\n",
                           solvedCameras[i-1].frame + 1, solvedCameras[i].frame - 1);
                }
            }
        }
    }

    // --- Velocity-based outlier detection on camera path ---
    if (solvedCameras.size() >= 4) {
        // Compute inter-frame speeds
        std::vector<double> speeds;
        for (size_t i = 1; i < solvedCameras.size(); ++i) {
            double dx = solvedCameras[i].tx - solvedCameras[i-1].tx;
            double dy = solvedCameras[i].ty - solvedCameras[i-1].ty;
            double dz = solvedCameras[i].tz - solvedCameras[i-1].tz;
            speeds.push_back(std::sqrt(dx*dx + dy*dy + dz*dz));
        }

        // Robust statistics: median and MAD
        std::vector<double> sortedSpeeds = speeds;
        std::sort(sortedSpeeds.begin(), sortedSpeeds.end());
        double medianSpeed = sortedSpeeds[sortedSpeeds.size() / 2];

        std::vector<double> absDev;
        for (size_t i = 0; i < sortedSpeeds.size(); ++i) {
            absDev.push_back(std::abs(sortedSpeeds[i] - medianSpeed));
        }
        std::sort(absDev.begin(), absDev.end());
        double MAD = absDev[absDev.size() / 2];
        double sigma = 1.4826 * MAD; // scale MAD to match std dev
        double threshold = medianSpeed + 3.0 * sigma;

        // --- Speed-distribution diagnostics (FABLE D2/D3) ---
        // A median of ~0 means the RAW path is a step function (most consecutive
        // cameras identical) and the MAD threshold degenerates to ~0, so the lerp
        // fixup below fires on any frame that moves on both sides — i.e. in that
        // regime it can flatten REAL motion, not just spikes. Log the regime and
        // per-frame track membership so jump frames can be correlated with track
        // churn (re-detection births) vs resection problems.
        auto tracksAliveAt = [this](int frame) {
            int n = 0;
            for (size_t t = 0; t < tracks.size(); ++t)
                if (tracks[t].markers.count(frame)) n++;
            return n;
        };
        auto tracksBornAt = [this](int frame) {
            int n = 0;
            for (size_t t = 0; t < tracks.size(); ++t)
                if (!tracks[t].markers.empty() && tracks[t].markers.begin()->first == frame) n++;
            return n;
        };
        {
            int nearZero = 0;
            for (size_t i = 0; i < speeds.size(); ++i)
                if (speeds[i] < 1e-6) nearZero++;
            CT_DBG("CameraTracker: [diag] speed stats: median=%.6f MAD=%.6f threshold=%.6f "
                   "near-zero steps=%d/%d p90=%.6f max=%.6f\n",
                   medianSpeed, MAD, threshold, nearZero, (int)speeds.size(),
                   sortedSpeeds[(sortedSpeeds.size() * 9) / 10], sortedSpeeds.back());
            if (threshold < 1e-9) {
                CT_DBG("CameraTracker: [diag] WARNING: degenerate speed stats (median~0) — "
                       "the outlier fixup is unreliable in this regime\n");
            }
        }
        // Keep the RAW path so the diagnostics CSV shows what the solver actually
        // produced vs what the lerp fixup rewrote.
        const std::vector<SolvedCamera> rawPath = solvedCameras;

        // Mark outlier frames (high speed in AND high speed out)
        std::vector<bool> outlier(solvedCameras.size(), false);
        for (size_t i = 1; i < solvedCameras.size() - 1; ++i) {
            if (speeds[i-1] > threshold && speeds[i] > threshold) {
                outlier[i] = true;
                CT_DBG("CameraTracker: [diag] jump frame %d: speedIn=%.6f speedOut=%.6f "
                       "alive=%d born=%d residual=%.3fpx\n",
                       solvedCameras[i].frame, speeds[i-1], speeds[i],
                       tracksAliveAt(solvedCameras[i].frame),
                       tracksBornAt(solvedCameras[i].frame),
                       solvedCameras[i].residual);
            }
        }
        // Also check first frame: if speed[0] is extreme but speed[1] is normal
        if (speeds.size() >= 2 && speeds[0] > threshold && speeds[1] < threshold) {
            outlier[0] = true; // frame 0 jumped, frame 1 is fine
        }

        // Replace outliers with linear interpolation — ONLY when the user opts in.
        // The replaced frames stop matching the solved 3D points (the residual shown
        // was computed before this rewrite), so by default we just flag + log them.
        const bool doFixSpikes = fixPathSpikes.lock()->getValue();
        int fixedCount = 0;
        for (size_t i = 0; doFixSpikes && i < solvedCameras.size(); ++i) {
            if (!outlier[i]) continue;

            // Find bracketing valid frames
            int a = static_cast<int>(i) - 1;
            while (a >= 0 && outlier[a]) a--;
            int b = static_cast<int>(i) + 1;
            while (b < static_cast<int>(solvedCameras.size()) && outlier[b]) b++;

            if (a < 0) a = b; // if no valid frame before, use next
            if (b >= static_cast<int>(solvedCameras.size())) b = a; // if no valid frame after, use prev

            if (a != b) {
                double t = static_cast<double>(i - a) / static_cast<double>(b - a);
                solvedCameras[i].tx = solvedCameras[a].tx + t * (solvedCameras[b].tx - solvedCameras[a].tx);
                solvedCameras[i].ty = solvedCameras[a].ty + t * (solvedCameras[b].ty - solvedCameras[a].ty);
                solvedCameras[i].tz = solvedCameras[a].tz + t * (solvedCameras[b].tz - solvedCameras[a].tz);
                solvedCameras[i].rx = solvedCameras[a].rx + t * (solvedCameras[b].rx - solvedCameras[a].rx);
                solvedCameras[i].ry = solvedCameras[a].ry + t * (solvedCameras[b].ry - solvedCameras[a].ry);
                solvedCameras[i].rz = solvedCameras[a].rz + t * (solvedCameras[b].rz - solvedCameras[a].rz);
            }
            fixedCount++;
        }

        if (fixedCount > 0) {
            CT_DBG("CameraTracker: Fixed %d outlier camera frames (median speed=%.4f, threshold=%.4f)\n",
                    fixedCount, medianSpeed, threshold);
        }

        // --- Path diagnostics CSV (FABLE D2/D3) ---
        // Raw vs lerp-fixed path plus per-frame track stats, written on every solve:
        // next to the solve report if that path is set, else to %TEMP%. Columns:
        //   gap_to_prev > 1  -> frames before this one failed resection (H1)
        //   flagged=1        -> rewritten by the lerp fixup (fix_* differ from raw_*)
        //   tracks_born spikes at flagged frames -> track churn / re-detect (H3)
        {
            std::string diagPath;
            KnobOutputFilePtr rk = solveReportFile.lock();
            if (rk) diagPath = rk->getValue();
            if (!diagPath.empty()) {
                diagPath += ".path_diag.csv";
            } else {
                const char* tmp = std::getenv("TEMP");
                diagPath = std::string(tmp ? tmp : ".") + "/cameratracker_path_diag.csv";
            }
            FStreamsSupport::ofstream ofile;
            FStreamsSupport::open(&ofile, diagPath, std::ios_base::out | std::ios_base::trunc);
            if (ofile) {
                ofile.precision(9);
                ofile << "frame,raw_tx,raw_ty,raw_tz,fix_tx,fix_ty,fix_tz,"
                         "speed_in_raw,flagged,tracks_alive,tracks_born,gap_to_prev,residual_px\n";
                for (size_t i = 0; i < solvedCameras.size(); ++i) {
                    const SolvedCamera& r = rawPath[i];
                    const SolvedCamera& f = solvedCameras[i];
                    ofile << f.frame << ','
                          << r.tx << ',' << r.ty << ',' << r.tz << ','
                          << f.tx << ',' << f.ty << ',' << f.tz << ','
                          << (i ? speeds[i-1] : 0.0) << ','
                          << (outlier[i] ? 1 : 0) << ','
                          << tracksAliveAt(f.frame) << ','
                          << tracksBornAt(f.frame) << ','
                          << (i ? f.frame - solvedCameras[i-1].frame : 0) << ','
                          << f.residual << '\n';
                }
                ofile.flush();
                CT_DBG("CameraTracker: [diag] wrote path diagnostics CSV: %s\n", diagPath.c_str());
            } else {
                CT_DBG("CameraTracker: [diag] FAILED to open path diagnostics CSV: %s\n", diagPath.c_str());
            }
        }
    }

    // Extract 3D points
    solvedPoints.clear();
    libmv::vector<libmv::EuclideanPoint> allPts = reconstruction.AllPoints();
    for (size_t i = 0; i < allPts.size(); ++i) {
        if (allPts[i].track < 0) continue;
        if (outlierTracks.find(allPts[i].track) != outlierTracks.end()) continue; // drop flagged outliers
        SolvedPoint sp;
        sp.track = allPts[i].track;
        // Same world flip (F = diag(1,-1,-1)) as the camera extraction, so the
        // sparse cloud stays registered to the (now upright) camera.
        sp.x =  allPts[i].X(0);
        sp.y = -allPts[i].X(1);
        sp.z = -allPts[i].X(2);
        solvedPoints.push_back(sp);
    }

    hasSolution = true;
    lastSolveError = reproError;
    cloudDirty = true;   // solve changed → viewport cloud must be rebuilt next fetch

    // Update display knobs — show the STRONG-track error (comparable across
    // runs; the filler-inclusive mean goes to the status line + log instead).
    solveErrorDisplay.lock()->setValue(strongCount ? strongErr : reproError);
    numCamerasDisplay.lock()->setValue(static_cast<int>(solvedCameras.size()));
    numPointsDisplay.lock()->setValue(static_cast<int>(solvedPoints.size()));

    // --- Coverage report ---
    // The reprojection error is measured ONLY over solved frames, so a small,
    // locally-good island reads as a great error while most of the clip is
    // unsolved. Surface the covered span vs. the requested range so a partial
    // solve is obvious instead of hiding behind a flattering error number.
    int reqStart = trackRangeStart.lock()->getValue();
    int reqEnd = trackRangeEnd.lock()->getValue();
    int reqSpan = std::max(1, reqEnd - reqStart + 1);
    int solvedMin = 0, solvedMax = 0;
    for (size_t i = 0; i < solvedCameras.size(); ++i) {
        if (i == 0 || solvedCameras[i].frame < solvedMin) solvedMin = solvedCameras[i].frame;
        if (i == 0 || solvedCameras[i].frame > solvedMax) solvedMax = solvedCameras[i].frame;
    }
    double coverage = 100.0 * (double)solvedCameras.size() / reqSpan;
    CT_DBG("CameraTracker: Coverage: %d/%d frames (%.0f%%), solved span %d-%d, requested %d-%d\n",
            (int)solvedCameras.size(), reqSpan, coverage,
            solvedCameras.empty() ? 0 : solvedMin, solvedCameras.empty() ? 0 : solvedMax,
            reqStart, reqEnd);

    std::stringstream ss;
    ss << "Solve complete: " << solvedCameras.size() << " cameras, "
       << solvedPoints.size() << " 3D points, error = " << reproError << " px";
    if (!solvedCameras.empty() && (int)solvedCameras.size() < reqSpan - 1) {
        ss << "  |  \xE2\x9A\xA0 PARTIAL: only " << (int)coverage << "% covered (frames "
           << solvedMin << "-" << solvedMax << " of " << reqStart << "-" << reqEnd
           << "). The camera-track chain broke where background features left frame; "
           << "error above is measured on the covered frames only.";
    }
    solveStatusDisplay.lock()->setValue(ss.str());
}


// ==================== Camera3D Output ====================

void
CameraTrackerNodePrivate::createCamera3DNode()
{
    CT_DBG("CameraTracker: createCamera3DNode called\n");

    if (!hasSolution || solvedCameras.empty()) {
        solveStatusDisplay.lock()->setValue("No solution — solve first");
        CT_DBG("CameraTracker: No solution available\n");
        return;
    }

    CT_DBG("CameraTracker: Have %d solved cameras\n", (int)solvedCameras.size());

    NodePtr thisNode = publicInterface->getNode();
    if (!thisNode) {
        CT_DBG("CameraTracker: No node\n");
        return;
    }

    AppInstancePtr app = thisNode->getApp();
    if (!app) {
        CT_DBG("CameraTracker: No app\n");
        return;
    }

    // Create a Camera3D node in the main graph (same group as this node)
    NodeCollectionPtr group = thisNode->getGroup();
    CT_DBG("CameraTracker: Creating Camera3D node in group %p\n", group.get());

    CreateNodeArgs cnArgs(PLUGINID_NATRON_CAMERA3DNODE, group);
    cnArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    cnArgs.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);

    NodePtr camNode = app->createNode(cnArgs);
    if (!camNode) {
        solveStatusDisplay.lock()->setValue("Failed to create Camera3D node");
        CT_DBG("CameraTracker: createNode returned null!\n");
        return;
    }

    CT_DBG("CameraTracker: Camera3D node created successfully\n");
    camNode->setLabel("CameraTracker_Camera");

    // Get Camera3D knobs
    KnobIPtr txKnob = camNode->getKnobByName("translateX");
    KnobIPtr tyKnob = camNode->getKnobByName("translateY");
    KnobIPtr tzKnob = camNode->getKnobByName("translateZ");
    KnobIPtr rxKnob = camNode->getKnobByName("rotateX");
    KnobIPtr ryKnob = camNode->getKnobByName("rotateY");
    KnobIPtr rzKnob = camNode->getKnobByName("rotateZ");
    KnobIPtr focalKnob = camNode->getKnobByName("focalLength");

    if (!txKnob || !tyKnob || !tzKnob || !rxKnob || !ryKnob || !rzKnob) {
        solveStatusDisplay.lock()->setValue("Camera3D node missing expected knobs");
        return;
    }

    KnobDoublePtr txK = std::dynamic_pointer_cast<KnobDouble>(txKnob);
    KnobDoublePtr tyK = std::dynamic_pointer_cast<KnobDouble>(tyKnob);
    KnobDoublePtr tzK = std::dynamic_pointer_cast<KnobDouble>(tzKnob);
    KnobDoublePtr rxK = std::dynamic_pointer_cast<KnobDouble>(rxKnob);
    KnobDoublePtr ryK = std::dynamic_pointer_cast<KnobDouble>(ryKnob);
    KnobDoublePtr rzK = std::dynamic_pointer_cast<KnobDouble>(rzKnob);

    // Shared output gauge — MUST match exportPointCloud() so the camera and the
    // point cloud land in the same normalized space.
    double originTx, originTy, originTz, scaleFactor;
    getOutputNormalization(originTx, originTy, originTz, scaleFactor);

    CT_DBG("CameraTracker: Normalizing cameras — origin=(%.1f, %.1f, %.1f), scale=%.4f\n",
            originTx, originTy, originTz, scaleFactor);

    // Set keyframes for each solved frame (normalized)
    for (const auto& cam : solvedCameras) {
        double t = static_cast<double>(cam.frame);
        double nx, ny, nz;
        applyOutputTransform(cam.tx, cam.ty, cam.tz,
                             originTx, originTy, originTz, scaleFactor,
                             nx, ny, nz);
        txK->setValueAtTime(t, nx, ViewSpec::all(), 0);
        tyK->setValueAtTime(t, ny, ViewSpec::all(), 0);
        tzK->setValueAtTime(t, nz, ViewSpec::all(), 0);
        if (sceneOrientSet) {
            // The scene rotation also rotates the camera's ORIENTATION:
            // M' = sceneRot * M (world-side rotation), then re-decompose.
            double M2[3][3];
            for (int r = 0; r < 3; ++r)
                for (int c2 = 0; c2 < 3; ++c2)
                    M2[r][c2] = sceneRot[r][0]*cam.R[0][c2]
                              + sceneRot[r][1]*cam.R[1][c2]
                              + sceneRot[r][2]*cam.R[2][c2];
            double rx, ry, rz;
            RotationConventions::decompose(M2, rx, ry, rz);
            rxK->setValueAtTime(t, rx, ViewSpec::all(), 0);
            ryK->setValueAtTime(t, ry, ViewSpec::all(), 0);
            rzK->setValueAtTime(t, rz, ViewSpec::all(), 0);
        } else {
            rxK->setValueAtTime(t, cam.rx, ViewSpec::all(), 0);
            ryK->setValueAtTime(t, cam.ry, ViewSpec::all(), 0);
            rzK->setValueAtTime(t, cam.rz, ViewSpec::all(), 0);
        }
    }

    // Set focal length on the output camera — already in mm
    if (focalKnob) {
        KnobDoublePtr focalK = std::dynamic_pointer_cast<KnobDouble>(focalKnob);
        if (focalK) {
            focalK->setValue(focalLengthMm.lock()->getValue());
        }
    }

    // ⭐ Set the camera's FILMBACK (aperture) to match the SENSOR WIDTH the solve used.
    // The FOV is focal/aperture — the solve computes focal_px = focalMm/sensorW * imgW,
    // so unless the Camera3D's hAperture == sensorW its FOV differs and the geometry
    // SLIDES even from a perfect solve. (The Camera3D default hAperture is 24.576mm;
    // if the solve used a different sensor — e.g. a 14.7574mm filmback — the
    // exported camera was ~66% off.) vAperture keeps the image aspect.
    {
        double sensorW = sensorWidth.lock()->getValue();
        double aspect = 1080.0 / 1920.0;   // fallback 16:9
        NodePtr inNode = thisNode->getInput(0);
        if (inNode && inNode->getEffectInstance()) {
            RectD rod; RenderScale sc; bool ipf = false;
            int rs = trackRangeStart.lock()->getValue();
            if (inNode->getEffectInstance()->getRegionOfDefinition_public(
                    inNode->getHashValue(), (double)rs, sc, ViewIdx(0), &rod, &ipf) == eStatusOK
                && rod.width() > 0) {
                aspect = rod.height() / rod.width();
            }
        }
        KnobIPtr hApKnob = camNode->getKnobByName("hAperture");
        KnobIPtr vApKnob = camNode->getKnobByName("vAperture");
        if (KnobDoublePtr hK = std::dynamic_pointer_cast<KnobDouble>(hApKnob)) {
            hK->setValue(sensorW);
        }
        if (KnobDoublePtr vK = std::dynamic_pointer_cast<KnobDouble>(vApKnob)) {
            vK->setValue(sensorW * aspect);
        }
        CT_DBG("CameraTracker: Camera3D filmback set to %.4f x %.4f mm (focal %.2fmm)\n",
                sensorW, sensorW * aspect, focalLengthMm.lock()->getValue());
    }

    // Lock the baked transform so the solve can't be accidentally moved
    // (panel, viewport gizmo, look-through navigation). User can untick
    // Lock Transform on the camera to edit deliberately.
    if (KnobIPtr lockKnob = camNode->getKnobByName("lockTransform")) {
        if (KnobBoolPtr lockB = std::dynamic_pointer_cast<KnobBool>(lockKnob)) {
            lockB->setValue(true);
        }
    }

    std::stringstream ss;
    ss << "Created Camera3D with " << solvedCameras.size() << " keyframes (transform locked)";
    solveStatusDisplay.lock()->setValue(ss.str());
}


// ==================== Point Cloud Export ====================

void
CameraTrackerNodePrivate::exportPointCloud()
{
    if (!hasSolution || solvedPoints.empty()) {
        solveStatusDisplay.lock()->setValue("No 3D points to export — solve first");
        return;
    }

    KnobOutputFilePtr fileKnob = pointCloudFile.lock();
    std::string path = fileKnob ? fileKnob->getValue() : std::string();
    if (path.empty()) {
        solveStatusDisplay.lock()->setValue("Set the Point Cloud File path before exporting");
        return;
    }

    // Apply the SAME gauge as the Camera3D output so the cloud and the solved
    // camera land in one normalized space.
    double ox, oy, oz, scale;
    getOutputNormalization(ox, oy, oz, scale);

    FStreamsSupport::ofstream ofile;
    FStreamsSupport::open(&ofile, path, std::ios_base::out | std::ios_base::trunc);
    if (!ofile) {
        solveStatusDisplay.lock()->setValue(std::string("Failed to open file for writing: ") + path);
        return;
    }
    ofile.precision(9);

    ofile << "# Natron CameraTracker point cloud\n";
    ofile << "# " << solvedPoints.size()
          << " points, normalized to match the Create Camera3D output\n";
    for (const auto& p : solvedPoints) {
        double nx, ny, nz;
        applyOutputTransform(p.x, p.y, p.z, ox, oy, oz, scale, nx, ny, nz);
        ofile << "v " << nx << ' ' << ny << ' ' << nz << '\n';
    }
    // Wavefront point elements (1-based vertex indices) so the file is a valid
    // point cloud rather than an empty (face-less) mesh.
    for (size_t i = 0; i < solvedPoints.size(); ++i) {
        ofile << "p " << (i + 1) << '\n';
    }
    ofile.flush();

    std::stringstream ss;
    ss << "Exported " << solvedPoints.size() << " points to " << path;
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: %s\n", ss.str().c_str());
}


// ==================== Per-frame Solve Report (CSV) ====================

void
CameraTrackerNodePrivate::exportSolveReport()
{
    if (!hasSolution || solvedCameras.empty()) {
        solveStatusDisplay.lock()->setValue("No solution — solve first");
        return;
    }

    KnobOutputFilePtr fileKnob = solveReportFile.lock();
    std::string path = fileKnob ? fileKnob->getValue() : std::string();
    if (path.empty()) {
        solveStatusDisplay.lock()->setValue("Set the Solve Report File path before exporting");
        return;
    }

    FStreamsSupport::ofstream ofile;
    FStreamsSupport::open(&ofile, path, std::ios_base::out | std::ios_base::trunc);
    if (!ofile) {
        solveStatusDisplay.lock()->setValue(std::string("Failed to open file for writing: ") + path);
        return;
    }
    ofile.precision(9);

    const double focalMm = focalLengthMm.lock()->getValue();
    ofile << "# Natron CameraTracker solve report\n";
    ofile << "# overall_residual_px=" << lastSolveError
          << " cameras=" << solvedCameras.size()
          << " points=" << solvedPoints.size() << "\n";
    ofile << "frame,residual_px,focal_mm,tx,ty,tz,rx_deg,ry_deg,rz_deg\n";
    for (size_t i = 0; i < solvedCameras.size(); ++i) {
        const SolvedCamera& c = solvedCameras[i];
        ofile << c.frame << ',' << c.residual << ',' << focalMm << ','
              << c.tx << ',' << c.ty << ',' << c.tz << ','
              << c.rx << ',' << c.ry << ',' << c.rz << '\n';
    }
    ofile.flush();

    std::stringstream ss;
    ss << "Exported solve report (" << solvedCameras.size() << " frames) to " << path;
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: %s\n", ss.str().c_str());
}


// ==================== Panel API (Gui/CameraTrackerPanel) ====================

void
CameraTrackerNode::notifyManualTracksChanged()
{
    Q_EMIT manualTracksChanged();
}

bool
CameraTrackerNode::isManualSectionOpen() const
{
    KnobGroupPtr g = _imp->grpManualKnob.lock();
    return g ? g->getValue() : true;
}

std::vector<CameraTrackerNode::ManualTrackInfo>
CameraTrackerNode::getManualTracksInfo(double time) const
{
    std::vector<ManualTrackInfo> out;
    const int frame = (int)time;
    for (const auto& t : _imp->tracks) {
        if (!_imp->manualTrackIds.count(t.id) || t.markers.empty()) continue;
        ManualTrackInfo info;
        info.id = t.id;
        info.nFrames = (int)t.markers.size();
        info.firstFrame = t.markers.begin()->first;
        info.lastFrame = t.markers.rbegin()->first;
        // marker at the query frame, else the nearest earlier one, else first
        auto it = t.markers.find(frame);
        if (it == t.markers.end()) {
            it = t.markers.lower_bound(frame);
            if (it == t.markers.end() || (it->first > frame && it != t.markers.begin())) --it;
        }
        info.x = it->second.first;
        info.y = it->second.second;
        info.error = t.error;
        info.selected = (t.id == _imp->selectedManualId);
        out.push_back(info);
    }
    return out;
}

void
CameraTrackerNode::panelSelectManualTrack(int id)
{
    if (_imp->selectedManualId == id) return;
    _imp->selectedManualId = id;
    _imp->refreshManualList();
    getApp()->redrawAllViewers();
}

void
CameraTrackerNode::panelDeleteManualTrack(int id)
{
    if (!_imp->manualTrackIds.count(id)) return;
    std::vector<CameraTrackerNodePrivate::Track2D> kept;
    kept.reserve(_imp->tracks.size());
    for (const auto& t : _imp->tracks) if (t.id != id) kept.push_back(t);
    _imp->tracks.swap(kept);
    _imp->manualTrackIds.erase(id);
    if (_imp->selectedManualId == id) _imp->selectedManualId = -1;
    _imp->refreshManualList();
    getApp()->redrawAllViewers();
}

void
CameraTrackerNode::panelSetManualTrackPosition(int id, double time, double x, double y)
{
    for (auto& t : _imp->tracks) {
        if (t.id != id) continue;
        t.markers[(int)time] = std::make_pair(x, y);
        break;
    }
    _imp->refreshManualList();
    getApp()->redrawAllViewers();
}

void
CameraTrackerNode::panelAddManualTrack(double time)
{
    // Place at the input's image center (user drags it into place afterwards).
    double cx = 960.0, cy = 540.0;
    NodePtr thisNode = getNode();
    NodePtr inputNode = thisNode ? thisNode->getInput(0) : NodePtr();
    if (inputNode && inputNode->getEffectInstance()) {
        RectD rod; RenderScale sc; bool ipf = false;
        if (inputNode->getEffectInstance()->getRegionOfDefinition_public(
                inputNode->getHashValue(), time, sc, ViewIdx(0), &rod, &ipf) == eStatusOK) {
            cx = (rod.x1 + rod.x2) / 2.0;
            cy = (rod.y1 + rod.y2) / 2.0;
        }
    }
    int newId = 0;
    for (const auto& t : _imp->tracks) newId = std::max(newId, t.id + 1);
    CameraTrackerNodePrivate::Track2D t;
    t.id = newId;
    t.markers[(int)time] = std::make_pair(cx, cy);
    _imp->tracks.push_back(t);
    _imp->manualTrackIds.insert(newId);
    _imp->selectedManualId = newId;
    _imp->refreshManualList();
    getApp()->redrawAllViewers();
}


// ==================== Scene orientation (viewport-selection driven) ====================

void
CameraTrackerNode::setViewportSelection(const std::vector<int>& indices)
{
    _imp->viewportSelection = indices;
}

void
CameraTrackerNodePrivate::setOriginFromSelection()
{
    if (!hasSolution || viewportSelection.empty()) {
        solveStatusDisplay.lock()->setValue("Select at least 1 cloud point in the 3D viewport first");
        return;
    }
    double cx = 0, cy = 0, cz = 0; int n = 0;
    for (int idx : viewportSelection) {
        if (idx < 0 || idx >= (int)solvedPoints.size()) continue;
        cx += solvedPoints[idx].x; cy += solvedPoints[idx].y; cz += solvedPoints[idx].z;
        ++n;
    }
    if (!n) {
        solveStatusDisplay.lock()->setValue("Selection is stale — reselect points and retry");
        return;
    }
    sceneOrigin[0] = cx / n; sceneOrigin[1] = cy / n; sceneOrigin[2] = cz / n;
    sceneOrientSet = true;
    cloudDirty = true;
    std::stringstream ss;
    ss << "Scene origin set from " << n << " selected point(s). Re-run Create Camera3D to apply.";
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: %s\n", ss.str().c_str());
}

void
CameraTrackerNodePrivate::setGroundPlaneFromSelection()
{
    if (!hasSolution || viewportSelection.size() < 3) {
        solveStatusDisplay.lock()->setValue("Select at least 3 ground points in the 3D viewport first");
        return;
    }
    // Least-squares plane through the selected points: centroid + smallest
    // eigenvector of the scatter matrix (classic total-least-squares fit).
    std::vector<std::array<double,3> > pts;
    for (int idx : viewportSelection) {
        if (idx < 0 || idx >= (int)solvedPoints.size()) continue;
        pts.push_back({solvedPoints[idx].x, solvedPoints[idx].y, solvedPoints[idx].z});
    }
    if (pts.size() < 3) {
        solveStatusDisplay.lock()->setValue("Selection is stale — reselect points and retry");
        return;
    }
    double c[3] = {0,0,0};
    for (const auto& p : pts) { c[0]+=p[0]; c[1]+=p[1]; c[2]+=p[2]; }
    c[0]/=pts.size(); c[1]/=pts.size(); c[2]/=pts.size();
    Eigen::Matrix3d S = Eigen::Matrix3d::Zero();
    for (const auto& p : pts) {
        Eigen::Vector3d d(p[0]-c[0], p[1]-c[1], p[2]-c[2]);
        S += d * d.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(S);
    Eigen::Vector3d n = eig.eigenvectors().col(0);   // smallest eigenvalue = plane normal

    // Orient the normal toward the cameras (cameras fly ABOVE the ground).
    Eigen::Vector3d camMean = Eigen::Vector3d::Zero();
    for (const auto& cam : solvedCameras) camMean += Eigen::Vector3d(cam.tx, cam.ty, cam.tz);
    if (!solvedCameras.empty()) camMean /= (double)solvedCameras.size();
    if (n.dot(camMean - Eigen::Vector3d(c[0], c[1], c[2])) < 0.0) n = -n;

    // Minimal rotation taking the fitted normal to +Y (X-Z becomes the ground).
    const Eigen::Vector3d up(0.0, 1.0, 0.0);
    Eigen::Quaterniond q = Eigen::Quaterniond::FromTwoVectors(n, up);
    Eigen::Matrix3d R = q.toRotationMatrix();
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            sceneRot[r][col] = R(r, col);

    // Put the origin ON the fitted plane too (centroid of the ground points)
    // unless the user explicitly set one before — least surprise: ground-fit
    // then origin-pick works in either order.
    if (!sceneOrientSet) {
        sceneOrigin[0] = c[0]; sceneOrigin[1] = c[1]; sceneOrigin[2] = c[2];
    }
    sceneOrientSet = true;
    cloudDirty = true;
    std::stringstream ss;
    ss << "Ground plane set from " << pts.size() << " points (normal -> +Y). "
          "Re-run Create Camera3D to apply.";
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: %s\n", ss.str().c_str());
}

void
CameraTrackerNodePrivate::createCardAtSelection()
{
    if (!hasSolution || viewportSelection.empty()) {
        solveStatusDisplay.lock()->setValue("Select cloud point(s) in the 3D viewport first");
        return;
    }
    std::vector<std::array<double,3> > pts;
    for (int idx : viewportSelection) {
        if (idx < 0 || idx >= (int)solvedPoints.size()) continue;
        pts.push_back({solvedPoints[idx].x, solvedPoints[idx].y, solvedPoints[idx].z});
    }
    if (pts.empty()) {
        solveStatusDisplay.lock()->setValue("Selection is stale — reselect points and retry");
        return;
    }
    double c[3] = {0,0,0};
    for (const auto& p : pts) { c[0]+=p[0]; c[1]+=p[1]; c[2]+=p[2]; }
    c[0]/=pts.size(); c[1]/=pts.size(); c[2]/=pts.size();

    // Normal: plane fit when we have 3+ points, otherwise face the mean camera
    // position (the useful default for a single tracked feature).
    Eigen::Vector3d n;
    Eigen::Vector3d camMean = Eigen::Vector3d::Zero();
    for (const auto& cam : solvedCameras) camMean += Eigen::Vector3d(cam.tx, cam.ty, cam.tz);
    if (!solvedCameras.empty()) camMean /= (double)solvedCameras.size();
    if (pts.size() >= 3) {
        Eigen::Matrix3d S = Eigen::Matrix3d::Zero();
        for (const auto& p : pts) {
            Eigen::Vector3d d(p[0]-c[0], p[1]-c[1], p[2]-c[2]);
            S += d * d.transpose();
        }
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(S);
        n = eig.eigenvectors().col(0);
        if (n.dot(camMean - Eigen::Vector3d(c[0], c[1], c[2])) < 0.0) n = -n;
    } else {
        n = (camMean - Eigen::Vector3d(c[0], c[1], c[2])).normalized();
    }

    // Everything must land in OUTPUT space (same gauge as Camera3D + cloud):
    // position via the output transform, normal via the scene rotation only.
    double ox, oy, oz, scale;
    getOutputNormalization(ox, oy, oz, scale);
    double px, py, pz;
    applyOutputTransform(c[0], c[1], c[2], ox, oy, oz, scale, px, py, pz);
    Eigen::Matrix3d Rs;
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            Rs(r, col) = sceneRot[r][col];
    Eigen::Vector3d nOut = (Rs * n).normalized();

    // Card3D's plane faces +Z: rotation taking +Z to the fitted normal.
    Eigen::Matrix3d R = Eigen::Quaterniond::FromTwoVectors(
        Eigen::Vector3d(0, 0, 1), nOut).toRotationMatrix();
    double M[3][3];
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            M[r][col] = R(r, col);
    double rx, ry, rz;
    RotationConventions::decompose(M, rx, ry, rz);

    NodePtr thisNode = publicInterface->getNode();
    AppInstancePtr app = thisNode ? thisNode->getApp() : AppInstancePtr();
    if (!app) return;
    CreateNodeArgs cnArgs(PLUGINID_NATRON_CARD3D, thisNode->getGroup());
    cnArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    cnArgs.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);
    NodePtr cardNode = app->createNode(cnArgs);
    if (!cardNode) {
        solveStatusDisplay.lock()->setValue("Failed to create Card3D node");
        return;
    }
    cardNode->setLabel("Tracker_Card");
    auto setD = [&cardNode](const char* name, double v) {
        KnobDoublePtr kd = std::dynamic_pointer_cast<KnobDouble>(cardNode->getKnobByName(name));
        if (kd) kd->setValue(v);
    };
    setD("translateX", px); setD("translateY", py); setD("translateZ", pz);
    setD("rotateX", rx);    setD("rotateY", ry);    setD("rotateZ", rz);

    std::stringstream ss;
    ss << "Created Card3D at selection (" << pts.size() << " point(s), "
       << (pts.size() >= 3 ? "plane-fit normal" : "camera-facing") << ")";
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: %s\n", ss.str().c_str());
}

void
CameraTrackerNodePrivate::setScaleFromSelection()
{
    if (!hasSolution || viewportSelection.size() != 2) {
        solveStatusDisplay.lock()->setValue(
            "Select exactly TWO solved points (2D viewer or 3D viewport), then Set Scale");
        return;
    }
    const int i0 = viewportSelection[0], i1 = viewportSelection[1];
    if (i0 < 0 || i0 >= (int)solvedPoints.size() ||
        i1 < 0 || i1 >= (int)solvedPoints.size() || i0 == i1) {
        solveStatusDisplay.lock()->setValue("Selection is stale — reselect two points and retry");
        return;
    }
    const SolvedPoint& a = solvedPoints[i0];
    const SolvedPoint& b = solvedPoints[i1];
    const double solveDist = std::sqrt((a.x - b.x) * (a.x - b.x) +
                                       (a.y - b.y) * (a.y - b.y) +
                                       (a.z - b.z) * (a.z - b.z));
    const double realDist = scaleDistance.lock()->getValue();
    if (solveDist < 1e-12 || realDist <= 0.0) {
        solveStatusDisplay.lock()->setValue("Degenerate selection — points are coincident");
        return;
    }
    sceneScale = realDist / solveDist;
    cloudDirty = true;
    std::stringstream ss;
    ss << "Scene scale set: " << realDist << " units across the selected pair "
          "(x" << sceneScale << "). Re-run Create Camera3D / exports.";
    solveStatusDisplay.lock()->setValue(ss.str());
    CT_DBG("CameraTracker: %s\n", ss.str().c_str());
}

void
CameraTrackerNodePrivate::clearSceneOrientation()
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            sceneRot[r][c] = (r == c) ? 1.0 : 0.0;
    sceneOrigin[0] = sceneOrigin[1] = sceneOrigin[2] = 0.0;
    sceneOrientSet = false;
    sceneScale = 0.0;
    cloudDirty = true;
    solveStatusDisplay.lock()->setValue("Scene orientation cleared (auto gauge)");
}


// ==================== Locator geometry (ScanlineRender stick-check) ====================

MeshDataPtr
CameraTrackerNode::getLocatorMesh() const
{
    if (!_imp->hasSolution || _imp->solvedPoints.empty()) {
        return MeshDataPtr();
    }
    double ox, oy, oz, scale;
    _imp->getOutputNormalization(ox, oy, oz, scale);

    // One small octahedron per solved point: 6 verts / 8 tris, visible from
    // every angle (unlike axis-aligned quads which vanish edge-on). Size is in
    // output-gauge units (the whole solve spans ~10).
    const double s = 0.04;
    MeshDataPtr mesh(new MeshData());
    mesh->vertices.reserve(_imp->solvedPoints.size() * 6 * 3);
    mesh->faceIndices.reserve(_imp->solvedPoints.size() * 8 * 3);
    mesh->faceCounts.reserve(_imp->solvedPoints.size() * 8);
    static const int kOctaFaces[8][3] = {
        {0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5}
    };
    int base = 0;
    for (const auto& p : _imp->solvedPoints) {
        double nx, ny, nz;
        _imp->applyOutputTransform(p.x, p.y, p.z, ox, oy, oz, scale, nx, ny, nz);
        const float fx = (float)nx, fy = (float)ny, fz = (float)nz;
        const float fs = (float)s;
        const float v[6][3] = {
            {fx+fs, fy, fz}, {fx-fs, fy, fz},
            {fx, fy+fs, fz}, {fx, fy-fs, fz},
            {fx, fy, fz+fs}, {fx, fy, fz-fs}
        };
        for (int i = 0; i < 6; ++i) {
            mesh->vertices.push_back(v[i][0]);
            mesh->vertices.push_back(v[i][1]);
            mesh->vertices.push_back(v[i][2]);
        }
        for (int f = 0; f < 8; ++f) {
            mesh->faceIndices.push_back(base + kOctaFaces[f][0]);
            mesh->faceIndices.push_back(base + kOctaFaces[f][1]);
            mesh->faceIndices.push_back(base + kOctaFaces[f][2]);
            mesh->faceCounts.push_back(3);
        }
        base += 6;
    }
    mesh->numVertices = mesh->vertices.size() / 3;
    mesh->numFaces = mesh->faceCounts.size();
    return mesh;
}


// ==================== PointCloudProvider (3D viewport display) ====================

PointCloudDataPtr
CameraTrackerNode::getPointCloud() const
{
    // Return the cached cloud; only rebuild when the solve changed (cloudDirty).
    // The viewport calls this on every redraw, so rebuilding each time would be a
    // needless per-frame cost.
    if (!_imp->cloudDirty && _imp->viewportCloud) {
        return _imp->viewportCloud;
    }

    PointCloudDataPtr cloud(new PointCloudData());
    if (!_imp->hasSolution || _imp->solvedPoints.empty()) {
        _imp->viewportCloud = cloud;
        _imp->cloudDirty = false;
        return cloud; // empty — nothing solved yet
    }

    // Same gauge as Create Camera3D / the .obj export so the cloud registers
    // with the solved camera in the viewport.
    double ox, oy, oz, scale;
    _imp->getOutputNormalization(ox, oy, oz, scale);

    cloud->reserve(_imp->solvedPoints.size());
    float minX = 1e30f, minY = 1e30f, minZ = 1e30f;
    float maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;
    for (std::size_t i = 0; i < _imp->solvedPoints.size(); ++i) {
        double nx, ny, nz;
        _imp->applyOutputTransform(_imp->solvedPoints[i].x, _imp->solvedPoints[i].y,
                                   _imp->solvedPoints[i].z, ox, oy, oz, scale, nx, ny, nz);
        const float x = static_cast<float>(nx);
        const float y = static_cast<float>(ny);
        const float z = static_cast<float>(nz);
        cloud->addPoint(x, y, z, 0.9f, 0.9f, 0.9f); // flat light-grey points
        minX = std::min(minX, x); minY = std::min(minY, y); minZ = std::min(minZ, z);
        maxX = std::max(maxX, x); maxY = std::max(maxY, y); maxZ = std::max(maxZ, z);
    }
    cloud->setBounds(minX, minY, minZ, maxX, maxY, maxZ);
    _imp->viewportCloud = cloud;
    _imp->cloudDirty = false;
    return cloud;
}


NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_CameraTrackerNode.cpp"
