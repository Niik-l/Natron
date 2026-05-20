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

#include "Blast.h"

#include <cmath>
#include <sstream>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"
#include "Engine/AppInstance.h"
#include "DeepToPoints.h"
#include "../Scene3D/Cube3D.h"
#include "../Scene3D/RotationConventions.h"

NATRON_NAMESPACE_ENTER

struct BlastPrivate
{
    // Mode: 0=BoundingBox, 1=Selection, 2=Expression (future)
    KnobChoiceWPtr mode;

    // Bounding box
    KnobDoubleWPtr bboxMinX, bboxMinY, bboxMinZ;
    KnobDoubleWPtr bboxMaxX, bboxMaxY, bboxMaxZ;

    // Invert: keep matching instead of deleting matching
    KnobBoolWPtr invert;

    // Info
    KnobStringWPtr info;
};

// Get the blast region as an oriented bounding box (OBB).
// Returns false if no valid region (e.g. mode != BoundingBox).
// On success:
//   center[3] — world-space center of the OBB
//   extent[3] — per-axis half-extents in the OBB's LOCAL frame
//   rot[3][3] — 3x3 rotation matrix, mathematical m[row][col] indexing,
//               column-vector convention (extrinsic XYZ from RotationConventions::compose).
//               Columns are the OBB's local axes expressed in world space.
//               Identity when there's no rotation source (knob mode).
static bool
getBlastOBBInternal(EffectInstancePtr boundsInput,
                    BlastPrivate* imp, double time,
                    float center[3], float extent[3], float rot[3][3])
{
    int mode = imp->mode.lock()->getValue();
    if (mode != 0) return false;

    Cube3D* cube = boundsInput ? dynamic_cast<Cube3D*>(boundsInput.get()) : nullptr;

    if (cube) {
        double tx = 0, ty = 0, tz = 0;
        double rx = 0, ry = 0, rz = 0;
        double sx = 1, sy = 1, sz = 1;
        KnobIPtr k;
        EffectInstancePtr eff = boundsInput;
        k = eff->getKnobByName("translateX"); if (k) tx = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
        k = eff->getKnobByName("translateY"); if (k) ty = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
        k = eff->getKnobByName("translateZ"); if (k) tz = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
        k = eff->getKnobByName("rotateX"); if (k) rx = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
        k = eff->getKnobByName("rotateY"); if (k) ry = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
        k = eff->getKnobByName("rotateZ"); if (k) rz = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
        k = eff->getKnobByName("scaleX"); if (k) sx = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
        k = eff->getKnobByName("scaleY"); if (k) sy = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
        k = eff->getKnobByName("scaleZ"); if (k) sz = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);

        center[0] = (float)tx;
        center[1] = (float)ty;
        center[2] = (float)tz;
        extent[0] = (float)std::abs(sx);
        extent[1] = (float)std::abs(sy);
        extent[2] = (float)std::abs(sz);

        // Extrinsic XYZ rotation — matches SceneGraph::buildTRS, so the OBB
        // aligns with the Cube3D's yellow wireframe in the 3D viewport.
        double mRot[3][3];
        RotationConventions::compose(rx, ry, rz, mRot);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                rot[i][j] = (float)mRot[i][j];
    } else {
        // Knob-driven AABB — no rotation source.
        float bMinX = (float)imp->bboxMinX.lock()->getValueAtTime(time);
        float bMinY = (float)imp->bboxMinY.lock()->getValueAtTime(time);
        float bMinZ = (float)imp->bboxMinZ.lock()->getValueAtTime(time);
        float bMaxX = (float)imp->bboxMaxX.lock()->getValueAtTime(time);
        float bMaxY = (float)imp->bboxMaxY.lock()->getValueAtTime(time);
        float bMaxZ = (float)imp->bboxMaxZ.lock()->getValueAtTime(time);
        center[0] = (bMinX + bMaxX) * 0.5f;
        center[1] = (bMinY + bMaxY) * 0.5f;
        center[2] = (bMinZ + bMaxZ) * 0.5f;
        extent[0] = (bMaxX - bMinX) * 0.5f;
        extent[1] = (bMaxY - bMinY) * 0.5f;
        extent[2] = (bMaxZ - bMinZ) * 0.5f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                rot[i][j] = (i == j) ? 1.0f : 0.0f;
    }
    return true;
}

// Test if a world-space point is inside the OBB.
// Project (P - center) onto the OBB's local axes (columns of rot), compare
// component magnitudes against extent.
static bool
pointInOBB(float px, float py, float pz,
           const float center[3], const float extent[3], const float rot[3][3])
{
    const float vx = px - center[0];
    const float vy = py - center[1];
    const float vz = pz - center[2];

    // Local coords via R^T * v (dot with each column of R).
    const float lx = rot[0][0] * vx + rot[1][0] * vy + rot[2][0] * vz;
    const float ly = rot[0][1] * vx + rot[1][1] * vy + rot[2][1] * vz;
    const float lz = rot[0][2] * vx + rot[1][2] * vy + rot[2][2] * vz;

    return std::abs(lx) <= extent[0] &&
           std::abs(ly) <= extent[1] &&
           std::abs(lz) <= extent[2];
}

Blast::Blast(NodePtr node)
    : EffectInstance(node)
    , _imp(new BlastPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

Blast::~Blast()
{
}

std::string
Blast::getPluginDescription() const
{
    return "Delete/filter points from a point cloud.\n\n"
           "Bounding Box mode: delete all points inside (or outside) a box.\n"
           "Connect downstream of a DeepToPoints node.\n\n"
           "Like Houdini's Blast SOP for point clouds.";
}

void
Blast::addAcceptedComponents(int /*inputNb*/,
                             std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
}

void
Blast::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Blast::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Blast::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    // Mode
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
        k->setName("mode");
        std::vector<ChoiceOption> modes;
        modes.push_back(ChoiceOption("bbox", "Bounding Box", "Delete points inside/outside a box."));
        modes.push_back(ChoiceOption("selection", "Selection", "Delete viewport-selected points (future)."));
        modes.push_back(ChoiceOption("expression", "Expression", "Delete by attribute expression (future)."));
        k->populateChoices(modes);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("How to select points for deletion.\n"
                              "Bounding Box: delete points inside/outside a box.\n"
                              "Selection: delete viewport-selected points (future).\n"
                              "Expression: delete by attribute expression (future)."));
        page->addKnob(k); _imp->mode = k;
    }

    // Bounding box
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("BBox Min X"));
        k->setName("bboxMinX"); k->setDefaultValue(-1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        page->addKnob(k); _imp->bboxMinX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("BBox Min Y"));
        k->setName("bboxMinY"); k->setDefaultValue(-1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        page->addKnob(k); _imp->bboxMinY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("BBox Min Z"));
        k->setName("bboxMinZ"); k->setDefaultValue(-1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        page->addKnob(k); _imp->bboxMinZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("BBox Max X"));
        k->setName("bboxMaxX"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        page->addKnob(k); _imp->bboxMaxX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("BBox Max Y"));
        k->setName("bboxMaxY"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        page->addKnob(k); _imp->bboxMaxY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("BBox Max Z"));
        k->setName("bboxMaxZ"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        page->addKnob(k); _imp->bboxMaxZ = k;
    }

    // Invert
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Invert"));
        k->setName("invert"); k->setDefaultValue(false);
        k->setHintToolTip(tr("When enabled, keep matching points and delete the rest.\n"
                              "When disabled, delete matching points and keep the rest."));
        page->addKnob(k); _imp->invert = k;
    }

    // Info
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info"); k->setAsLabel();
        k->setEvaluateOnChange(false);
        k->setIsPersistent(false);
        page->addKnob(k); _imp->info = k;
    }
}

bool
Blast::knobChanged(KnobI* /*k*/, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/, double /*time*/, bool /*originatedFromMainThread*/)
{
    // Invalidate cached output — will recompute on next getPointCloud() call
    _lastOutput.reset();
    return true;
}

bool
Blast::getBlastBounds(double time, float outMin[3], float outMax[3]) const
{
    float center[3], extent[3], rot[3][3];
    if (!getBlastOBBInternal(getInput(1), _imp.get(), time, center, extent, rot)) {
        return false;
    }

    // AABB enclosing the rotated OBB: world half-extent along axis i is
    // sum_j |R[i][j]| * extent[j]. Reduces to the OBB itself when R = I.
    const float wx = std::abs(rot[0][0])*extent[0] + std::abs(rot[0][1])*extent[1] + std::abs(rot[0][2])*extent[2];
    const float wy = std::abs(rot[1][0])*extent[0] + std::abs(rot[1][1])*extent[1] + std::abs(rot[1][2])*extent[2];
    const float wz = std::abs(rot[2][0])*extent[0] + std::abs(rot[2][1])*extent[1] + std::abs(rot[2][2])*extent[2];

    outMin[0] = center[0] - wx; outMax[0] = center[0] + wx;
    outMin[1] = center[1] - wy; outMax[1] = center[1] + wy;
    outMin[2] = center[2] - wz; outMax[2] = center[2] + wz;
    return true;
}

bool
Blast::getBlastOBB(double time,
                   float outCenter[3], float outExtent[3],
                   float outMatrix[16]) const
{
    float center[3], extent[3], rot[3][3];
    if (!getBlastOBBInternal(getInput(1), _imp.get(), time, center, extent, rot)) {
        return false;
    }

    outCenter[0] = center[0]; outCenter[1] = center[1]; outCenter[2] = center[2];
    outExtent[0] = extent[0]; outExtent[1] = extent[1]; outExtent[2] = extent[2];

    // Pack into column-major 4x4 for glMultMatrixf:
    //   out[col*4 + row]
    // Rotation block: out[col*4+row] = rot[row][col].
    // Translation: column 3 = center.
    outMatrix[ 0] = rot[0][0]; outMatrix[ 1] = rot[1][0]; outMatrix[ 2] = rot[2][0]; outMatrix[ 3] = 0.f;
    outMatrix[ 4] = rot[0][1]; outMatrix[ 5] = rot[1][1]; outMatrix[ 6] = rot[2][1]; outMatrix[ 7] = 0.f;
    outMatrix[ 8] = rot[0][2]; outMatrix[ 9] = rot[1][2]; outMatrix[10] = rot[2][2]; outMatrix[11] = 0.f;
    outMatrix[12] = center[0]; outMatrix[13] = center[1]; outMatrix[14] = center[2]; outMatrix[15] = 1.f;
    return true;
}

StatusEnum
Blast::getRegionOfDefinition(U64 /*hash*/,
                             double time,
                             const RenderScale& scale,
                             ViewIdx view,
                             RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProject;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
}

PointCloudDataPtr
Blast::getPointCloud() const
{
    // Compute on demand if not yet available
    if (!_lastOutput) {
        // Use current app time
        double time = 0.0;
        if (getApp()) {
            time = getApp()->getTimeLine()->currentFrame();
        }
        const_cast<Blast*>(this)->computeFilteredCloud(time);
    }
    return _lastOutput;
}

void
Blast::computeFilteredCloud(double time)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return;

    // Find upstream point cloud (DeepToPoints or another Blast)
    PointCloudDataPtr srcCloud;
    DeepToPoints* d2p = dynamic_cast<DeepToPoints*>(input.get());
    Blast* upBlast = dynamic_cast<Blast*>(input.get());
    if (d2p) srcCloud = d2p->getPointCloud();
    else if (upBlast) srcCloud = upBlast->getPointCloud();
    if (!srcCloud || srcCloud->numPoints() == 0) return;

    int mode = _imp->mode.lock()->getValue();
    bool invert = _imp->invert.lock()->getValue();

    const float* srcData = srcCloud->data();
    std::size_t srcCount = srcCloud->numPoints();
    const int stride = 6;

    PointCloudDataPtr outCloud = std::make_shared<PointCloudData>();
    outCloud->reserve(srcCount);

    if (mode == 0) {
        float obbCenter[3], obbExtent[3], obbRot[3][3];
        if (!getBlastOBBInternal(getInput(1), _imp.get(), time, obbCenter, obbExtent, obbRot)) {
            return; // shouldn't happen — mode == 0 here
        }

        float outMin[3] = {1e30f, 1e30f, 1e30f};
        float outMax[3] = {-1e30f, -1e30f, -1e30f};
        int keptCount = 0;

        for (std::size_t i = 0; i < srcCount; ++i) {
            float px = srcData[i * stride + 0];
            float py = srcData[i * stride + 1];
            float pz = srcData[i * stride + 2];

            // Rotation-aware test: point is inside the (possibly rotated) cube
            const bool inside = pointInOBB(px, py, pz, obbCenter, obbExtent, obbRot);

            bool keep = invert ? inside : !inside;
            if (keep) {
                outCloud->addPoint(px, py, pz,
                                   srcData[i * stride + 3],
                                   srcData[i * stride + 4],
                                   srcData[i * stride + 5]);
                if (px < outMin[0]) outMin[0] = px;
                if (py < outMin[1]) outMin[1] = py;
                if (pz < outMin[2]) outMin[2] = pz;
                if (px > outMax[0]) outMax[0] = px;
                if (py > outMax[1]) outMax[1] = py;
                if (pz > outMax[2]) outMax[2] = pz;
                ++keptCount;
            }
        }

        if (keptCount > 0) {
            outCloud->setBounds(outMin[0], outMin[1], outMin[2],
                                outMax[0], outMax[1], outMax[2]);
        }

        std::ostringstream oss;
        oss << "Input: " << srcCount << " points -> Output: " << keptCount
            << " points (" << (srcCount - keptCount) << " deleted)";
        _imp->info.lock()->setValue(oss.str());
    }

    _lastOutput = outCloud;
}

StatusEnum
Blast::render(const RenderActionArgs& args)
{
    // Get upstream point cloud
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;

    // Find the DeepToPoints upstream
    DeepToPoints* d2p = dynamic_cast<DeepToPoints*>(input.get());
    if (!d2p) {
        _imp->info.lock()->setValue("Error: input must be a DeepToPoints node.");
        return eStatusFailed;
    }

    PointCloudDataPtr srcCloud = d2p->getPointCloud();
    if (!srcCloud || srcCloud->numPoints() == 0) {
        _imp->info.lock()->setValue("No input points.");
        _lastOutput.reset();
        return eStatusOK;
    }

    int mode = _imp->mode.lock()->getValue();
    bool invert = _imp->invert.lock()->getValue();

    const float* srcData = srcCloud->data();
    std::size_t srcCount = srcCloud->numPoints();
    const int stride = 6;

    PointCloudDataPtr outCloud = std::make_shared<PointCloudData>();
    outCloud->reserve(srcCount);

    int keptCount = 0;

    if (mode == 0) {
        // Bounding Box mode — rotation-aware OBB test (matches Cube3D's
        // T*R*S in the 3D viewport when a Cube3D is wired as bounds input).
        float obbCenter[3], obbExtent[3], obbRot[3][3];
        if (!getBlastOBBInternal(getInput(1), _imp.get(), args.time, obbCenter, obbExtent, obbRot)) {
            return eStatusFailed;
        }

        // Track output bounds
        float outMin[3] = {1e30f, 1e30f, 1e30f};
        float outMax[3] = {-1e30f, -1e30f, -1e30f};

        for (std::size_t i = 0; i < srcCount; ++i) {
            float px = srcData[i * stride + 0];
            float py = srcData[i * stride + 1];
            float pz = srcData[i * stride + 2];

            const bool inside = pointInOBB(px, py, pz, obbCenter, obbExtent, obbRot);

            // invert=false: delete inside (keep outside)
            // invert=true:  keep inside (delete outside)
            bool keep = invert ? inside : !inside;

            if (keep) {
                float r = srcData[i * stride + 3];
                float g = srcData[i * stride + 4];
                float b = srcData[i * stride + 5];
                outCloud->addPoint(px, py, pz, r, g, b);
                ++keptCount;

                // Update bounds
                if (px < outMin[0]) outMin[0] = px;
                if (py < outMin[1]) outMin[1] = py;
                if (pz < outMin[2]) outMin[2] = pz;
                if (px > outMax[0]) outMax[0] = px;
                if (py > outMax[1]) outMax[1] = py;
                if (pz > outMax[2]) outMax[2] = pz;
            }
        }

        if (keptCount > 0) {
            outCloud->setBounds(outMin[0], outMin[1], outMin[2],
                                outMax[0], outMax[1], outMax[2]);
        }
    } else {
        // Selection / Expression modes — pass through for now
        _lastOutput = srcCloud;
        _imp->info.lock()->setValue("Mode not yet implemented — passing through.");
        return eStatusOK;
    }

    _lastOutput = outCloud;

    std::ostringstream oss;
    oss << "Input: " << srcCount << " points → Output: " << keptCount
        << " points (" << (srcCount - keptCount) << " deleted)";
    _imp->info.lock()->setValue(oss.str());

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
