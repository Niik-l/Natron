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
#include <vector>
#include <string>
#include <sstream>
#include <map>

#include "Global/GLIncludes.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Image.h"
#include "Engine/ImagePlaneDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/TrackerFrameAccessor.h"
#include "Engine/ViewIdx.h"

#include <libmv/simple_pipeline/tracks.h>
#include <libmv/simple_pipeline/reconstruction.h>
#include <libmv/simple_pipeline/camera_intrinsics.h>
#include <libmv/simple_pipeline/pipeline.h>
#include <libmv/simple_pipeline/initialize_reconstruction.h>
#include <libmv/simple_pipeline/bundle.h>
#include <libmv/simple_pipeline/keyframe_selection.h>
#include <libmv/simple_pipeline/modal_solver.h>
#include <libmv/simple_pipeline/detect.h>
#include <libmv/tracking/track_region.h>
#include <libmv/image/image.h>
#include <libmv/autotrack/autotrack.h>
#include <libmv/autotrack/frame_accessor.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

// ==================== Private Implementation ====================

struct CameraTrackerNodePrivate
{
    CameraTrackerNode* publicInterface;

    // --- Camera Settings tab ---
    KnobDoubleWPtr focalLengthPx;
    KnobDoubleWPtr sensorWidth;
    KnobDoubleWPtr principalPointX;
    KnobDoubleWPtr principalPointY;

    // --- Lens Distortion tab ---
    KnobChoiceWPtr distortionModel;
    KnobDoubleWPtr k1, k2, k3;
    KnobBoolWPtr refineLensDistortion;

    // --- Tracking tab ---
    KnobChoiceWPtr detectorType;
    KnobIntWPtr maxFeatures;
    KnobIntWPtr minFeatureDistance;
    KnobDoubleWPtr harrisThreshold;
    KnobDoubleWPtr roiX1, roiY1, roiX2, roiY2;
    KnobBoolWPtr useROI;
    KnobBoolWPtr addTrackMode;
    KnobIntWPtr trackRangeStart;
    KnobIntWPtr trackRangeEnd;
    KnobButtonWPtr detectFeaturesBtn;
    KnobButtonWPtr trackFeaturesBtn;
    KnobButtonWPtr clearTracksBtn;

    // --- Solving tab ---
    KnobChoiceWPtr solveMode;
    KnobBoolWPtr refineFocalLength;
    KnobBoolWPtr refinePrincipalPoint;
    KnobIntWPtr keyframe1;
    KnobIntWPtr keyframe2;
    KnobBoolWPtr autoKeyframes;
    KnobButtonWPtr solveBtn;
    KnobDoubleWPtr solveErrorDisplay;
    KnobIntWPtr numCamerasDisplay;
    KnobIntWPtr numPointsDisplay;
    KnobStringWPtr solveStatusDisplay;

    // --- Output tab ---
    KnobButtonWPtr createCameraBtn;
    KnobButtonWPtr exportPointCloudBtn;

    // --- Internal state ---
    struct Track2D {
        int id;
        std::map<int, std::pair<double, double>> markers; // frame -> (x, y)
    };
    std::vector<Track2D> tracks;

    struct SolvedCamera {
        int frame;
        double tx, ty, tz;
        double rx, ry, rz; // Euler XYZ in degrees
    };
    std::vector<SolvedCamera> solvedCameras;

    struct SolvedPoint {
        int track;
        double x, y, z;
    };
    std::vector<SolvedPoint> solvedPoints;
    double lastSolveError;
    bool hasSolution;

    CameraTrackerNodePrivate(CameraTrackerNode* pub)
        : publicInterface(pub)
        , lastSolveError(0)
        , hasSolution(false)
    {
    }

    // Rotation matrix to Euler XYZ (degrees)
    // TODO(xyz-conformance): when this file is re-enabled in the build (currently
    // excluded by Engine/CMakeLists.txt:114 due to libmv/GCC 15 issues), replace
    // this body with a call to RotationConventions::decompose so the extracted
    // angles match Natron's standard extrinsic XYZ convention (M = Rz*Ry*Rx
    // column-vector, Maya/Blender/Houdini default). The current implementation
    // uses ZYX composition despite the function name — mislabeled, not misimplemented
    // for its own use, but it will produce wrong angles when feeding any other
    // extrinsic-XYZ-expecting consumer.
    static void rotationMatrixToEulerXYZ(const libmv::Mat3& R,
                                          double& rx, double& ry, double& rz)
    {
        // R = Rz * Ry * Rx
        double sy = -R(2, 0);
        if (sy > 0.99999) {
            ry = M_PI / 2.0;
            rx = 0.0;
            rz = std::atan2(R(0, 1), R(0, 2));
        } else if (sy < -0.99999) {
            ry = -M_PI / 2.0;
            rx = 0.0;
            rz = std::atan2(-R(0, 1), R(0, 2));
        } else {
            ry = std::asin(sy);
            rx = std::atan2(R(2, 1), R(2, 2));
            rz = std::atan2(R(1, 0), R(0, 0));
        }
        // Convert to degrees
        rx *= 180.0 / M_PI;
        ry *= 180.0 / M_PI;
        rz *= 180.0 / M_PI;
    }

    void detectFeatures(int frame);
    void trackFeatures();
    void solveCameraMotion();
    void createCamera3DNode();
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
        _imp->focalLengthPx = k; // stored as mm, converted to px internally
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
        k->setDefaultValue(0);
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
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Let the solver refine K1/K2 distortion during bundle adjustment"));
        lensPage->addKnob(k);
        _imp->refineLensDistortion = k;
    }

    // ========== Tracking ==========
    KnobPagePtr trackPage = AppManager::createKnob<KnobPage>(this, tr("Tracking"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Detector"));
        k->setName("detectorType");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("harris", "Harris", "Harris corner detector — good general-purpose detector"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        trackPage->addKnob(k);
        _imp->detectorType = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Features"));
        k->setName("maxFeatures");
        k->setDefaultValue(50);
        k->setMinimum(10); k->setMaximum(2000);
        k->setDisplayMinimum(10); k->setDisplayMaximum(500);
        k->setHintToolTip(tr("Maximum number of features to detect per frame"));
        trackPage->addKnob(k);
        _imp->maxFeatures = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Min Distance"));
        k->setName("minFeatureDistance");
        k->setDefaultValue(20);
        k->setMinimum(1); k->setMaximum(200);
        k->setHintToolTip(tr("Minimum distance in pixels between detected features"));
        trackPage->addKnob(k);
        _imp->minFeatureDistance = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Harris Threshold"));
        k->setName("harrisThreshold");
        k->setDefaultValue(1e-5);
        k->setDisplayMinimum(1e-8); k->setDisplayMaximum(1e-2);
        k->setHintToolTip(tr("Threshold for the Harris detector response function"));
        trackPage->addKnob(k);
        _imp->harrisThreshold = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Use Detection Region"));
        k->setName("useROI");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Only detect features inside the specified region.\n"
                              "Use this to exclude sky, trees, or other areas that won't track well."));
        trackPage->addKnob(k);
        _imp->useROI = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Region X1"));
        k->setName("roiX1"); k->setDefaultValue(0.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Left edge of detection region (pixels)"));
        trackPage->addKnob(k); _imp->roiX1 = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Region Y1"));
        k->setName("roiY1"); k->setDefaultValue(0.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Bottom edge of detection region (pixels)"));
        trackPage->addKnob(k); _imp->roiY1 = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Region X2"));
        k->setName("roiX2"); k->setDefaultValue(1920.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Right edge of detection region (pixels)"));
        trackPage->addKnob(k); _imp->roiX2 = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Region Y2"));
        k->setName("roiY2"); k->setDefaultValue(540.0); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Top edge of detection region (pixels). Set to half image height to exclude sky."));
        trackPage->addKnob(k); _imp->roiY2 = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Add Track Mode"));
        k->setName("addTrackMode");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("When enabled, click in the viewer to place a manual track point.\n"
                              "Disable when done adding points."));
        trackPage->addKnob(k);
        _imp->addTrackMode = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Range Start"));
        k->setName("trackRangeStart");
        k->setDefaultValue(1);
        k->setHintToolTip(tr("First frame of the tracking range"));
        trackPage->addKnob(k);
        _imp->trackRangeStart = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Range End"));
        k->setName("trackRangeEnd");
        k->setDefaultValue(100);
        k->setHintToolTip(tr("Last frame of the tracking range"));
        trackPage->addKnob(k);
        _imp->trackRangeEnd = k;
    }
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
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Detect Features"));
        k->setName("detectFeatures");
        k->setHintToolTip(tr("Detect trackable features on the current frame"));
        k->setEvaluateOnChange(false);
        trackPage->addKnob(k);
        _imp->detectFeaturesBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Track Features"));
        k->setName("trackFeatures");
        k->setHintToolTip(tr("Track detected features across the frame range.\n"
                              "This uses the KLT tracker from libmv."));
        k->setEvaluateOnChange(false);
        trackPage->addKnob(k);
        _imp->trackFeaturesBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Clear Tracks"));
        k->setName("clearTracks");
        k->setHintToolTip(tr("Remove all detected features and tracks"));
        k->setEvaluateOnChange(false);
        trackPage->addKnob(k);
        _imp->clearTracksBtn = k;
    }

    // ========== Solving ==========
    KnobPagePtr solvePage = AppManager::createKnob<KnobPage>(this, tr("Solve"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Solve Mode"));
        k->setName("solveMode");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("camera", "Camera", "Full 6DOF camera solve — requires camera translation (parallax)"));
        entries.push_back(ChoiceOption("tripod", "Tripod", "Rotation-only solve — for locked-off or nodal cameras"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        solvePage->addKnob(k);
        _imp->solveMode = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Refine Focal Length"));
        k->setName("refineFocalLength");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Allow the solver to adjust the focal length during bundle adjustment"));
        solvePage->addKnob(k);
        _imp->refineFocalLength = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Refine Principal Point"));
        k->setName("refinePrincipalPoint");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Allow the solver to adjust the principal point during bundle adjustment"));
        solvePage->addKnob(k);
        _imp->refinePrincipalPoint = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Auto Keyframes"));
        k->setName("autoKeyframes");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("Automatically select keyframes using GRIC analysis.\n"
                              "Disable to manually set keyframe 1 and keyframe 2."));
        solvePage->addKnob(k);
        _imp->autoKeyframes = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Keyframe 1"));
        k->setName("keyframe1");
        k->setDefaultValue(1);
        k->setHintToolTip(tr("First keyframe for two-view initialization (manual mode)"));
        solvePage->addKnob(k);
        _imp->keyframe1 = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Keyframe 2"));
        k->setName("keyframe2");
        k->setDefaultValue(50);
        k->setHintToolTip(tr("Second keyframe for two-view initialization (manual mode)"));
        solvePage->addKnob(k);
        _imp->keyframe2 = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Solve"));
        k->setName("solve");
        k->setHintToolTip(tr("Solve 3D camera motion from 2D tracks"));
        k->setEvaluateOnChange(false);
        solvePage->addKnob(k);
        _imp->solveBtn = k;
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

    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Create Camera3D"));
        k->setName("createCamera");
        k->setHintToolTip(tr("Create a Camera3D node with animated transforms from the solve.\n"
                              "The camera will have keyframes on every solved frame."));
        k->setEvaluateOnChange(false);
        outputPage->addKnob(k);
        _imp->createCameraBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Export Point Cloud Info"));
        k->setName("exportPointCloud");
        k->setHintToolTip(tr("Print reconstructed 3D point positions to the script editor log"));
        k->setEvaluateOnChange(false);
        outputPage->addKnob(k);
        _imp->exportPointCloudBtn = k;
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
                fprintf(stderr, "CameraTracker: Warning — failed to create internal nodes\n");
                fflush(stderr);
            }
        }
    }
}


// ==================== Post-init: create internal Input/Output nodes ====================

void
CameraTrackerNode::onKnobsLoaded()
{
    fprintf(stderr, "CameraTracker: onKnobsLoaded called\n"); fflush(stderr);

    NodePtr thisNode = getNode();
    if (!thisNode || !thisNode->getApp()) return;

    NodeGroupPtr thisGroup = std::dynamic_pointer_cast<NodeGroup>(thisNode->getEffectInstance());
    if (!thisGroup) return;

    // Check if we already have internal nodes (from serialization)
    if (thisGroup->getNInputs() > 0) {
        fprintf(stderr, "CameraTracker: Internal nodes already exist (%d inputs)\n", thisGroup->getNInputs());
        fflush(stderr);
        return;
    }

    fprintf(stderr, "CameraTracker: Creating internal Input/Output nodes\n"); fflush(stderr);

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
            fprintf(stderr, "CameraTracker: Internal nodes created and connected\n"); fflush(stderr);
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "CameraTracker: Failed to create internal nodes: %s\n", e.what()); fflush(stderr);
    } catch (...) {
        fprintf(stderr, "CameraTracker: Failed to create internal nodes (unknown error)\n"); fflush(stderr);
    }
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

        if (k == _imp->clearTracksBtn.lock().get()) {
            _imp->tracks.clear();
            _imp->solvedCameras.clear();
            _imp->solvedPoints.clear();
            _imp->hasSolution = false;
            _imp->solveStatusDisplay.lock()->setValue("Tracks cleared");
            _imp->numCamerasDisplay.lock()->setValue(0);
            _imp->numPointsDisplay.lock()->setValue(0);
            _imp->solveErrorDisplay.lock()->setValue(0.0);
            return true;
        }

        if (k == _imp->solveBtn.lock().get()) {
            _imp->solveCameraMotion();
            return true;
        }

        if (k == _imp->createCameraBtn.lock().get()) {
            _imp->createCamera3DNode();
            return true;
        }

        if (k == _imp->exportPointCloudBtn.lock().get()) {
            if (_imp->solvedPoints.empty()) {
                _imp->solveStatusDisplay.lock()->setValue("No 3D points to export — solve first");
            } else {
                std::stringstream ss;
                ss << "CameraTracker: " << _imp->solvedPoints.size() << " reconstructed 3D points:\n";
                for (size_t i = 0; i < _imp->solvedPoints.size(); ++i) {
                    const auto& p = _imp->solvedPoints[i];
                    ss << "  Track " << p.track << ": ("
                       << p.x << ", " << p.y << ", " << p.z << ")\n";
                }
                // Log to console via qDebug — visible in script editor
                fprintf(stderr, "%s", ss.str().c_str()); fflush(stderr);
                _imp->solveStatusDisplay.lock()->setValue("Point cloud info exported to console");
            }
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
    if (_imp->tracks.empty()) return;

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

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        for (size_t t = 0; t < _imp->tracks.size(); ++t) {
            const auto& track = _imp->tracks[t];
            if (track.markers.empty()) continue;

            auto curIt = track.markers.find(curFrame);

            // Draw trail (green line showing recent path)
            glLineWidth(1.5f);
            glColor4f(0.0f, 0.8f, 0.0f, 0.5f);
            glBegin(GL_LINE_STRIP);
            for (int f = curFrame - trailLength; f <= curFrame; ++f) {
                auto it = track.markers.find(f);
                if (it != track.markers.end()) {
                    glVertex2d(it->second.first, it->second.second);
                }
            }
            glEnd();

            // Draw cross at current position
            if (curIt != track.markers.end()) {
                double cx = curIt->second.first;
                double cy = curIt->second.second;
                glColor4f(0.0f, 1.0f, 0.0f, 1.0f);
                glLineWidth(1.5f);
                glBegin(GL_LINES);
                glVertex2d(cx - crossSize, cy);
                glVertex2d(cx + crossSize, cy);
                glVertex2d(cx, cy - crossSize);
                glVertex2d(cx, cy + crossSize);
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

    // Only handle clicks when Add Track Mode is enabled
    KnobBoolPtr addMode = _imp->addTrackMode.lock();
    if (!addMode || !addMode->getValue()) return false;

    int curFrame = static_cast<int>(time);
    double x = pos.x();
    double y = pos.y();

    // Create a new track at the clicked position
    CameraTrackerNodePrivate::Track2D t;
    t.id = static_cast<int>(_imp->tracks.size());
    t.markers[curFrame] = std::make_pair(x, y);
    _imp->tracks.push_back(t);

    std::stringstream ss;
    ss << "Added manual track " << t.id << " at (" << (int)x << ", " << (int)y
       << ") on frame " << curFrame << " (total: " << _imp->tracks.size() << ")";
    _imp->solveStatusDisplay.lock()->setValue(ss.str());

    // Trigger overlay redraw
    getApp()->redrawAllViewers();

    return true;
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

    // Setup detection options — use defaults from libmv and override
    libmv::DetectOptions options;
    options.type = libmv::DetectOptions::HARRIS;
    options.margin = 16;
    options.min_distance = minFeatureDistance.lock()->getValue();
    options.harris_threshold = harrisThreshold.lock()->getValue();

    // Detect
    libmv::vector<libmv::Feature> features;
    libmv::Detect(mvImage, options, &features);

    // Limit to maxFeatures
    int maxFeat = maxFeatures.lock()->getValue();
    int numToUse = std::min(static_cast<int>(features.size()), maxFeat);

    // ROI filtering — only keep features inside the detection region
    bool doROI = useROI.lock()->getValue();
    double roiLeft = roiX1.lock()->getValue();
    double roiBottom = roiY1.lock()->getValue();
    double roiRight = roiX2.lock()->getValue();
    double roiTop = roiY2.lock()->getValue();

    // Convert detected features to tracks (each feature = one new track starting at this frame)
    int startTrackId = static_cast<int>(tracks.size());
    int addedCount = 0;
    for (int i = 0; i < static_cast<int>(features.size()) && addedCount < maxFeat; ++i) {
        // Convert from libmv (top-left origin) back to Natron (bottom-left)
        double natronX = features[i].x + bounds.x1;
        double natronY = (bounds.y2 - 1) - features[i].y;

        // Filter by ROI if enabled
        if (doROI) {
            if (natronX < roiLeft || natronX > roiRight ||
                natronY < roiBottom || natronY > roiTop) {
                continue;
            }
        }

        Track2D t;
        t.id = startTrackId + addedCount;
        t.markers[frame] = std::make_pair(natronX, natronY);
        tracks.push_back(t);
        addedCount++;
    }

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


// ==================== Feature Tracking ====================

void
CameraTrackerNodePrivate::trackFeatures()
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

    // Track features using libmv's TrackRegion
    fprintf(stderr, "CameraTracker: Starting tracking %d tracks over frames %d-%d\n",
            (int)tracks.size(), rangeStart, rangeEnd); fflush(stderr);
    solveStatusDisplay.lock()->setValue("Tracking...");

    const double halfPatchSize = 10.0;
    const double edgeMargin = halfPatchSize + 3.0;

    // Setup TrackRegion options — fast translation-only mode
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
    fprintf(stderr, "CameraTracker: Rendering frame %d...\n", rangeStart); fflush(stderr);
    libmv::FloatImage prevImage = renderFrame(rangeStart);
    fprintf(stderr, "CameraTracker: Frame %d rendered (%dx%d)\n", rangeStart, prevImage.Width(), prevImage.Height()); fflush(stderr);

    for (int frame = rangeStart + 1; frame <= rangeEnd; ++frame) {
        fprintf(stderr, "CameraTracker: Frame %d/%d — rendering...\n", frame, rangeEnd); fflush(stderr);
        libmv::FloatImage curImage = renderFrame(frame);

        int trackedCount = 0;
        int skippedBounds = 0;
        int failedTrack = 0;

        for (auto& track : tracks) {
            auto prevIt = track.markers.find(frame - 1);
            if (prevIt == track.markers.end()) continue;

            double prevX = prevIt->second.first;
            double prevY = prevIt->second.second;

            // Convert to libmv coords (top-down)
            double mvPrevX = prevX - rod.x1;
            double mvPrevY = (rod.y2 - 1) - prevY;

            // Check bounds
            if (mvPrevX < edgeMargin || mvPrevX >= width - edgeMargin ||
                mvPrevY < edgeMargin || mvPrevY >= height - edgeMargin) {
                skippedBounds++;
                continue;
            }

            // Define quad corners around the feature center
            double x1[4], y1[4], x2[4], y2[4];
            x1[0] = mvPrevX - halfPatchSize; y1[0] = mvPrevY - halfPatchSize;
            x1[1] = mvPrevX + halfPatchSize; y1[1] = mvPrevY - halfPatchSize;
            x1[2] = mvPrevX + halfPatchSize; y1[2] = mvPrevY + halfPatchSize;
            x1[3] = mvPrevX - halfPatchSize; y1[3] = mvPrevY + halfPatchSize;
            for (int c = 0; c < 4; ++c) { x2[c] = x1[c]; y2[c] = y1[c]; }

            libmv::TrackRegionResult result;
            result.termination = libmv::TrackRegionResult::FAILURE;
            result.correlation = 0;

            try {
                libmv::TrackRegion(prevImage, curImage, x1, y1, trackOptions, x2, y2, &result);
            } catch (...) {
                failedTrack++;
                continue;
            }

            if (result.is_usable()) {
                double newMvX = (x2[0] + x2[1] + x2[2] + x2[3]) / 4.0;
                double newMvY = (y2[0] + y2[1] + y2[2] + y2[3]) / 4.0;

                // Sanity check: reject if tracked position jumped too far (> 100px)
                double dx = newMvX - mvPrevX;
                double dy = newMvY - mvPrevY;
                if (dx * dx + dy * dy > 10000.0) {
                    failedTrack++;
                    continue;
                }

                double natronX = newMvX + rod.x1;
                double natronY = (rod.y2 - 1) - newMvY;
                track.markers[frame] = std::make_pair(natronX, natronY);
                trackedCount++;
            } else {
                failedTrack++;
            }
        }

        prevImage = curImage;

        fprintf(stderr, "CameraTracker: Frame %d — tracked=%d, bounds=%d, failed=%d\n",
                frame, trackedCount, skippedBounds, failedTrack); fflush(stderr);
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

    } catch (const std::exception& e) {
        solveStatusDisplay.lock()->setValue(std::string("Track failed: ") + e.what());
    } catch (...) {
        solveStatusDisplay.lock()->setValue("Track failed: unknown error");
    }
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
    double focalMm = focalLengthPx.lock()->getValue(); // knob stores mm despite the member name
    double sensorW = sensorWidth.lock()->getValue();
    double focalPx = (focalMm / sensorW) * static_cast<double>(width);
    double ppx = principalPointX.lock()->getValue();
    double ppy = principalPointY.lock()->getValue();

    // If principal point is 0,0 use image center
    if (std::abs(ppx) < 0.001 && std::abs(ppy) < 0.001) {
        ppx = width / 2.0;
        ppy = height / 2.0;
    }

    fprintf(stderr, "CameraTracker: Focal %.1fmm -> %.1fpx (sensor %.1fmm, image %dx%d)\n",
            focalMm, focalPx, sensorW, width, height); fflush(stderr);

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
    int minTrackLength = std::max(5, totalFrameRange / 4); // at least 25% of range or 5 frames

    // Image center (Focus of Expansion for forward motion)
    double imgCenterX = width / 2.0;
    double imgCenterY = height / 2.0;
    double maxDistFromCenter = std::sqrt(imgCenterX * imgCenterX + imgCenterY * imgCenterY);

    libmv::Tracks rawTracks;
    int filteredOut = 0;
    int centerFiltered = 0;
    for (const auto& track : tracks) {
        // Filter: skip tracks that are too short
        if (static_cast<int>(track.markers.size()) < minTrackLength) {
            filteredOut++;
            continue;
        }

        // Skip tracks too close to image center — they have minimal
        // parallax for forward motion and generally degrade the solve
        {
            auto firstIt = track.markers.begin();
            double natronX = firstIt->second.first;
            double natronY = firstIt->second.second;
            double mvX = natronX - rod.x1;
            double mvY = (rod.y2 - 1) - natronY;
            double distFromCenter = std::sqrt(std::pow(mvX - imgCenterX, 2) + std::pow(mvY - imgCenterY, 2));
            // Skip if within 15% of image center
            if (distFromCenter < maxDistFromCenter * 0.15) {
                centerFiltered++;
                continue;
            }
        }

        for (const auto& marker : track.markers) {
            int frame = marker.first;
            double natronX = marker.second.first;
            double natronY = marker.second.second;
            // Convert to libmv image coords (top-left origin)
            double mvX = natronX - rod.x1;
            double mvY = (rod.y2 - 1) - natronY;
            rawTracks.Insert(frame, track.id, mvX, mvY);
        }
    }
    fprintf(stderr, "CameraTracker: Using %d tracks (filtered %d short, %d center, min length %d)\n",
            rawTracks.MaxTrack() + 1, filteredOut, centerFiltered, minTrackLength); fflush(stderr);

    // Undistort and normalize tracks through intrinsics
    libmv::Tracks calibratedTracks;
    libmv::InvertIntrinsicsForTracks(rawTracks, intrinsics, &calibratedTracks);

    int mode = solveMode.lock()->getValue();

    libmv::EuclideanReconstruction reconstruction;

    if (mode == 1) {
        // Tripod / rotation-only solve
        fprintf(stderr, "CameraTracker: Using tripod (rotation-only) solver\n"); fflush(stderr);
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
                    // Compute flow vectors and check radiality
                    double cx = 0, cy = 0; // normalized coords center at 0,0
                    int radialCount = 0, totalCount = 0;
                    for (int i = 0; i < commonFL.size(); i += 2) {
                        // Markers come in pairs (image1, image2) for same track
                        // Find matching pair
                    }
                    // Simpler approach: check if flow vectors point away from center
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
                    if (total > 5) {
                        double radialRatio = static_cast<double>(radial) / total;
                        isForwardMotion = (radialRatio > 0.7);
                        fprintf(stderr, "CameraTracker: Flow analysis: %d/%d radial (%.0f%%) — %s\n",
                                radial, total, radialRatio * 100,
                                isForwardMotion ? "FORWARD MOTION DETECTED" : "lateral motion OK");
                        fflush(stderr);
                    }
                }
            }
        }

        // --- Keyframe selection ---
        libmv::vector<int> keyframes;
        bool useAutoKeyframes = autoKeyframes.lock()->getValue();

        if (useAutoKeyframes) {
            libmv::SelectKeyframesBasedOnGRICAndVariance(calibratedTracks, intrinsics, keyframes);
        }

        if (!useAutoKeyframes || keyframes.size() < 2) {
            if (useAutoKeyframes) {
                fprintf(stderr, "CameraTracker: Auto keyframes failed — using first/last frames\n");
                fflush(stderr);
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

        fprintf(stderr, "CameraTracker: Keyframes: %d and %d\n", keyframes[0], keyframes[1]);
        fflush(stderr);

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

                    fprintf(stderr, "CameraTracker: Pair (%d,%d): %d common markers, "
                            "cam1=%s cam2=%s\n",
                            kf1, kf2, (int)commonMarkers.size(),
                            cam1 ? "OK" : "NULL", cam2 ? "OK" : "NULL");
                    fflush(stderr);

                    if (cam1 && cam2) {
                        reconstruction = testRecon;
                        keyframes[0] = kf1;
                        keyframes[1] = kf2;
                        initOk = true;
                        fprintf(stderr, "CameraTracker: Two-frame init succeeded with pair (%d,%d)\n",
                                kf1, kf2); fflush(stderr);
                    }
                }
            }

            if (!initOk) {
                fprintf(stderr, "CameraTracker: All two-frame init attempts FAILED\n"); fflush(stderr);
            }
        }

        // --- Fallback: Rotation-first approach ---
        if (!initOk) {
            fprintf(stderr, "CameraTracker: Falling back to rotation-first approach\n"); fflush(stderr);
            solveStatusDisplay.lock()->setValue("Two-frame init failed — trying rotation-first approach...");

            // Step 1: Solve rotation-only across all frames
            libmv::EuclideanReconstruction rotOnlyRecon;
            libmv::ModalSolver(calibratedTracks, &rotOnlyRecon, NULL);

            // Check if ModalSolver produced cameras
            libmv::vector<libmv::EuclideanCamera> rotCams = rotOnlyRecon.AllCameras();
            fprintf(stderr, "CameraTracker: ModalSolver produced %d cameras\n",
                    (int)rotCams.size()); fflush(stderr);

            if (rotCams.size() >= 2) {
                // Use the rotation-only solution as our reconstruction
                // The ModalSolver projects points onto a unit sphere, which gives us
                // rotation and approximate point directions (but no depth/translation)
                reconstruction = rotOnlyRecon;

                // Try to complete reconstruction by adding translation via resection
                // The rotation is good; incremental reconstruction may recover some translation
                fprintf(stderr, "CameraTracker: Attempting incremental reconstruction from rotation-only base\n");
                fflush(stderr);
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
            fprintf(stderr, "CameraTracker: Completing reconstruction...\n"); fflush(stderr);
            libmv::EuclideanCompleteReconstruction(calibratedTracks, &reconstruction, NULL);
        }

        fprintf(stderr, "CameraTracker: After completion: %d cameras, %d points\n",
                (int)reconstruction.AllCameras().size(),
                (int)reconstruction.AllPoints().size()); fflush(stderr);

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

                int removedCount = 0;
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
                        // Remove this point by setting it to a huge value
                        // (libmv doesn't have a RemovePoint, so we move it far away)
                        reconstruction.InsertPoint(allPts[i].track, libmv::Vec3(1e10, 1e10, 1e10));
                        removedCount++;
                    }
                }
                fprintf(stderr, "CameraTracker: Outlier rejection: removed %d/%d points (median dist=%.2f, max=%.2f)\n",
                        removedCount, (int)allPts.size(), medianDist, maxAllowedDist); fflush(stderr);
            }
        }

        // --- Bundle adjustment ---
        fprintf(stderr, "CameraTracker: Running bundle adjustment...\n"); fflush(stderr);

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
        libmv::EuclideanBundleCommonIntrinsics(
            calibratedTracks,
            bundleFlags,
            bundleConstraints,
            &reconstruction,
            &intrinsics,
            NULL);

        // For forward motion, do a second bundle pass WITH focal refinement if requested
        if (isForwardMotion && refineFocalLength.lock()->getValue()) {
            fprintf(stderr, "CameraTracker: Second bundle pass with focal refinement (forward motion)\n");
            fflush(stderr);
            bundleFlags |= libmv::BUNDLE_FOCAL_LENGTH;
            libmv::EuclideanBundleCommonIntrinsics(
                calibratedTracks,
                bundleFlags,
                bundleConstraints,
                &reconstruction,
                &intrinsics,
                NULL);
        }

        fprintf(stderr, "CameraTracker: Bundle adjustment complete\n"); fflush(stderr);

        // Update intrinsics knobs if refined (convert px back to mm)
        if (refineFocalLength.lock()->getValue()) {
            double refinedPx = intrinsics.focal_length();
            double refinedMm = refinedPx * sensorW / static_cast<double>(width);
            focalLengthPx.lock()->setValue(refinedMm);
            fprintf(stderr, "CameraTracker: Refined focal: %.1fpx -> %.1fmm\n", refinedPx, refinedMm);
            fflush(stderr);
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

    // Compute reprojection error
    double reproError = libmv::EuclideanReprojectionError(rawTracks, reconstruction, intrinsics);

    // Extract solved cameras
    solvedCameras.clear();
    libmv::vector<libmv::EuclideanCamera> allCams = reconstruction.AllCameras();
    for (size_t i = 0; i < allCams.size(); ++i) {
        const libmv::EuclideanCamera& cam = allCams[i];
        if (cam.image < 0) continue;

        SolvedCamera sc;
        sc.frame = cam.image;

        // Camera world position = -R^T * t
        libmv::Mat3 Rt = cam.R.transpose();
        libmv::Vec3 pos = -Rt * cam.t;
        sc.tx = pos(0);
        sc.ty = pos(1);
        sc.tz = pos(2);

        // Convert rotation matrix to Euler angles
        rotationMatrixToEulerXYZ(cam.R, sc.rx, sc.ry, sc.rz);

        solvedCameras.push_back(sc);
    }

    // Sort by frame
    std::sort(solvedCameras.begin(), solvedCameras.end(),
              [](const SolvedCamera& a, const SolvedCamera& b) { return a.frame < b.frame; });

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

        // Mark outlier frames (high speed in AND high speed out)
        std::vector<bool> outlier(solvedCameras.size(), false);
        for (size_t i = 1; i < solvedCameras.size() - 1; ++i) {
            if (speeds[i-1] > threshold && speeds[i] > threshold) {
                outlier[i] = true;
            }
        }
        // Also check first frame: if speed[0] is extreme but speed[1] is normal
        if (speeds.size() >= 2 && speeds[0] > threshold && speeds[1] < threshold) {
            outlier[0] = true; // frame 0 jumped, frame 1 is fine
        }

        // Replace outliers with linear interpolation
        int fixedCount = 0;
        for (size_t i = 0; i < solvedCameras.size(); ++i) {
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
            fprintf(stderr, "CameraTracker: Fixed %d outlier camera frames (median speed=%.4f, threshold=%.4f)\n",
                    fixedCount, medianSpeed, threshold); fflush(stderr);
        }
    }

    // Extract 3D points
    solvedPoints.clear();
    libmv::vector<libmv::EuclideanPoint> allPts = reconstruction.AllPoints();
    for (size_t i = 0; i < allPts.size(); ++i) {
        if (allPts[i].track < 0) continue;
        SolvedPoint sp;
        sp.track = allPts[i].track;
        sp.x = allPts[i].X(0);
        sp.y = allPts[i].X(1);
        sp.z = allPts[i].X(2);
        solvedPoints.push_back(sp);
    }

    hasSolution = true;
    lastSolveError = reproError;

    // Update display knobs
    solveErrorDisplay.lock()->setValue(reproError);
    numCamerasDisplay.lock()->setValue(static_cast<int>(solvedCameras.size()));
    numPointsDisplay.lock()->setValue(static_cast<int>(solvedPoints.size()));

    std::stringstream ss;
    ss << "Solve complete: " << solvedCameras.size() << " cameras, "
       << solvedPoints.size() << " 3D points, error = " << reproError << " px";
    solveStatusDisplay.lock()->setValue(ss.str());
}


// ==================== Camera3D Output ====================

void
CameraTrackerNodePrivate::createCamera3DNode()
{
    fprintf(stderr, "CameraTracker: createCamera3DNode called\n"); fflush(stderr);

    if (!hasSolution || solvedCameras.empty()) {
        solveStatusDisplay.lock()->setValue("No solution — solve first");
        fprintf(stderr, "CameraTracker: No solution available\n"); fflush(stderr);
        return;
    }

    fprintf(stderr, "CameraTracker: Have %d solved cameras\n", (int)solvedCameras.size()); fflush(stderr);

    NodePtr thisNode = publicInterface->getNode();
    if (!thisNode) {
        fprintf(stderr, "CameraTracker: No node\n"); fflush(stderr);
        return;
    }

    AppInstancePtr app = thisNode->getApp();
    if (!app) {
        fprintf(stderr, "CameraTracker: No app\n"); fflush(stderr);
        return;
    }

    // Create a Camera3D node in the main graph (same group as this node)
    NodeCollectionPtr group = thisNode->getGroup();
    fprintf(stderr, "CameraTracker: Creating Camera3D node in group %p\n", group.get()); fflush(stderr);

    CreateNodeArgs cnArgs(PLUGINID_NATRON_CAMERA3DNODE, group);
    cnArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    cnArgs.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);

    NodePtr camNode = app->createNode(cnArgs);
    if (!camNode) {
        solveStatusDisplay.lock()->setValue("Failed to create Camera3D node");
        fprintf(stderr, "CameraTracker: createNode returned null!\n"); fflush(stderr);
        return;
    }

    fprintf(stderr, "CameraTracker: Camera3D node created successfully\n"); fflush(stderr);
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

    // Normalize camera positions: first camera at origin, scale to reasonable range
    double originTx = 0, originTy = 0, originTz = 0;
    if (!solvedCameras.empty()) {
        originTx = solvedCameras[0].tx;
        originTy = solvedCameras[0].ty;
        originTz = solvedCameras[0].tz;
    }

    // Compute scale: find max displacement from first camera
    double maxDisp = 1.0;
    for (const auto& cam : solvedCameras) {
        double dx = cam.tx - originTx;
        double dy = cam.ty - originTy;
        double dz = cam.tz - originTz;
        double disp = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (disp > maxDisp) maxDisp = disp;
    }

    // Scale so max displacement is ~10 units (reasonable 3D viewport range)
    double scaleFactor = (maxDisp > 1e-6) ? 10.0 / maxDisp : 1.0;

    fprintf(stderr, "CameraTracker: Normalizing cameras — origin=(%.1f, %.1f, %.1f), maxDisp=%.1f, scale=%.4f\n",
            originTx, originTy, originTz, maxDisp, scaleFactor); fflush(stderr);

    // Set keyframes for each solved frame (normalized)
    for (const auto& cam : solvedCameras) {
        double t = static_cast<double>(cam.frame);
        double nx = (cam.tx - originTx) * scaleFactor;
        double ny = (cam.ty - originTy) * scaleFactor;
        double nz = (cam.tz - originTz) * scaleFactor;
        txK->setValueAtTime(t, nx, ViewSpec::all(), 0);
        tyK->setValueAtTime(t, ny, ViewSpec::all(), 0);
        tzK->setValueAtTime(t, nz, ViewSpec::all(), 0);
        rxK->setValueAtTime(t, cam.rx, ViewSpec::all(), 0);
        ryK->setValueAtTime(t, cam.ry, ViewSpec::all(), 0);
        rzK->setValueAtTime(t, cam.rz, ViewSpec::all(), 0);
    }

    // Set focal length on the output camera — already in mm
    if (focalKnob) {
        KnobDoublePtr focalK = std::dynamic_pointer_cast<KnobDouble>(focalKnob);
        if (focalK) {
            double fMm = focalLengthPx.lock()->getValue(); // stored as mm
            focalK->setValue(fMm);
            {
            }
        }
    }

    std::stringstream ss;
    ss << "Created Camera3D with " << solvedCameras.size() << " keyframes";
    solveStatusDisplay.lock()->setValue(ss.str());
}


NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_CameraTrackerNode.cpp"
