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

#include "DeepToPoints.h"

#include <cassert>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"
#include "../Scene3D/Camera3DNode.h"
#include "../Scene3D/RotationConventions.h"
#include "../DotUtils.h"

NATRON_NAMESPACE_ENTER


struct DeepToPointsPrivate
{
    KnobDoubleWPtr pointSize;
    KnobDoubleWPtr zScale;
    KnobDoubleWPtr density;
    KnobStringWPtr info;
};


DeepToPoints::DeepToPoints(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepToPointsPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepToPoints::~DeepToPoints()
{
    _lastPointCloud.reset();
}

std::string
DeepToPoints::getPluginDescription() const
{
    return tr("Convert deep image samples into a 3D point cloud for visualization "
              "in the 3D Viewport panel.\n\n"
              "Each deep sample becomes a colored point at position (pixel_x, pixel_y, depth) "
              "in 3D space. The Z Scale parameter controls the depth axis scaling.\n\n"
              "To view: open a 3D Viewport panel (right-click tab header > New 3D viewport), "
              "then select this node.\n\n"
              "Density controls what fraction of points are displayed (1.0 = all, 0.1 = 10%).").toStdString();
}

void
DeepToPoints::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepToPoints::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepToPoints::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepToPoints::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr zScale = AppManager::createKnob<KnobDouble>(this, tr("Z Scale"));
    zScale->setName("zScale");
    zScale->setHintToolTip(tr("Scale factor for the depth axis in the point cloud. "
                               "Adjust to make the depth range visible relative to the image dimensions."));
    zScale->setAnimationEnabled(true);
    zScale->setDefaultValue(1.0);
    zScale->setMinimum(0.001);
    zScale->setMaximum(100.0);
    zScale->setDisplayMinimum(0.01);
    zScale->setDisplayMaximum(10.0);
    page->addKnob(zScale);
    _imp->zScale = zScale;

    KnobDoublePtr pointSize = AppManager::createKnob<KnobDouble>(this, tr("Point Size"));
    pointSize->setName("pointSize");
    pointSize->setHintToolTip(tr("Size of each point in the 3D viewport (in pixels)."));
    pointSize->setAnimationEnabled(false);
    pointSize->setDefaultValue(2.0);
    pointSize->setMinimum(1.0);
    pointSize->setMaximum(20.0);
    pointSize->setDisplayMinimum(1.0);
    pointSize->setDisplayMaximum(10.0);
    page->addKnob(pointSize);
    _imp->pointSize = pointSize;

    KnobDoublePtr density = AppManager::createKnob<KnobDouble>(this, tr("Density"));
    density->setName("density");
    density->setHintToolTip(tr("Fraction of points to display (1.0 = all points, 0.1 = 10%). "
                                "Reduce this if the point cloud is too dense and slow to render."));
    density->setAnimationEnabled(false);
    density->setDefaultValue(1.0);
    density->setMinimum(0.001);
    density->setMaximum(1.0);
    density->setDisplayMinimum(0.01);
    density->setDisplayMaximum(1.0);
    page->addKnob(density);
    _imp->density = density;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Connect a deep node to the input.");
    page->addKnob(info);
    _imp->info = info;
}

void
DeepToPoints::onInputChanged(int inputNo)
{
    // Drop the cached point cloud when the Deep input is disconnected,
    // otherwise the 3D viewport keeps drawing the last converted cloud.
    if (inputNo == 0 && !getInput(0)) {
        _lastPointCloud.reset();
        KnobStringPtr infoKnob = _imp->info.lock();
        if (infoKnob) infoKnob->setValue("Connect a deep node to the input.");
        if (getApp()) {
            getApp()->redrawAllViewers();
        }
    }
    EffectInstance::onInputChanged(inputNo);
}

StatusEnum
DeepToPoints::getRegionOfDefinition(U64 /*hash*/,
                                    double time,
                                    const RenderScale& scale,
                                    ViewIdx view,
                                    RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

PointCloudDataPtr
DeepToPoints::getPointCloud() const
{
    return _lastPointCloud;
}

StatusEnum
DeepToPoints::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) return eStatusFailed;

    double zScaleVal = _imp->zScale.lock()->getValue();
    double densityVal = _imp->density.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");

    if (zIdx < 0) return eStatusFailed;

    // Center the point cloud around origin
    float centerX = (dw.x1 + dw.x2) * 0.5f;
    float centerY = (dw.y1 + dw.y2) * 0.5f;
    float imageScale = 1.0f / std::max(dw.width(), dw.height());

    // Check for camera input (input 1) for world-space unprojection
    EffectInstancePtr camInput = skipDots(getInput(1));
    Camera3DNode* camera = camInput ? dynamic_cast<Camera3DNode*>(camInput.get()) : nullptr;
    bool hasCamera = (camera != nullptr);

    // Camera parameters for unprojection
    double camTx = 0, camTy = 0, camTz = 0;
    double camRx = 0, camRy = 0, camRz = 0;
    float hAperture = 24.576f, vAperture = 18.672f, focalLength = 50.0f;
    float imgW = (float)dw.width(), imgH = (float)dw.height();
    double camRot[3][3] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };
    if (hasCamera) {
        camera->getCameraPosition(args.time, camTx, camTy, camTz, camRx, camRy, camRz);
        focalLength = (float)camera->getCameraFocalLength(args.time);
        hAperture = (float)camera->getCameraHAperture(args.time);
        vAperture = (float)camera->getCameraVAperture(args.time);
        RotationConventions::compose(camRx, camRy, camRz, camRot);
    }

    // Density threshold: use deterministic hash-based thinning
    unsigned int densityThreshold = (unsigned int)(densityVal * 4294967295.0);

    // Build point cloud
    PointCloudDataPtr cloud = std::make_shared<PointCloudData>();
    std::size_t estimatedPoints = (std::size_t)(srcDeep->totalSamples() * densityVal);
    cloud->reserve(estimatedPoints);

    // Track bounding box for the viewport to frame
    float bboxMin[3] = { 1e30f,  1e30f,  1e30f};
    float bboxMax[3] = {-1e30f, -1e30f, -1e30f};

    unsigned int sampleIndex = 0;
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;

            const float* srcData = srcDeep->getSampleData(x, y);

            for (int s = 0; s < nSamples; ++s) {
                const float* sample = srcData + s * nChannels;

                // Skip fully transparent samples
                float alpha = (aIdx >= 0) ? sample[aIdx] : 1.0f;
                if (alpha < 0.001f) {
                    ++sampleIndex;
                    continue;
                }

                // Density filter: simple hash-based thinning
                if (densityVal < 0.999) {
                    // Simple hash: multiply by large prime, compare to threshold
                    unsigned int hash = sampleIndex * 2654435761u;
                    if (hash > densityThreshold) {
                        ++sampleIndex;
                        continue;
                    }
                }

                // Position from depth
                float depth = sample[zIdx] * (float)zScaleVal;
                float px, py, pz;

                if (hasCamera) {
                    // Unproject pixel + depth to world space using camera intrinsics
                    // NDC: map pixel coords to [-1,1] range
                    float ndcX = (x - centerX) / (imgW * 0.5f);
                    float ndcY = -(y - centerY) / (imgH * 0.5f);
                    // Camera space: NDC * (aperture/2) * (depth / focalLength)
                    float camX = ndcX * (hAperture * 0.5f) * (depth / focalLength);
                    float camY = ndcY * (vAperture * 0.5f) * (depth / focalLength);
                    float camZ = -depth; // camera looks down -Z
                    // World space: rotate by camera orientation, then translate
                    px = (float)(camRot[0][0] * camX + camRot[0][1] * camY + camRot[0][2] * camZ + camTx);
                    py = (float)(camRot[1][0] * camX + camRot[1][1] * camY + camRot[1][2] * camZ + camTy);
                    pz = (float)(camRot[2][0] * camX + camRot[2][1] * camY + camRot[2][2] * camZ + camTz);
                } else {
                    // Fallback: screen-projected space (original behavior)
                    px = (x - centerX) * imageScale;
                    py = -(y - centerY) * imageScale;
                    pz = -depth;
                }

                // Color: unpremultiply
                float r = (rIdx >= 0) ? sample[rIdx] : 0.5f;
                float g = (gIdx >= 0) ? sample[gIdx] : 0.5f;
                float b = (bIdx >= 0) ? sample[bIdx] : 0.5f;
                if (alpha > 0.001f) {
                    r /= alpha;
                    g /= alpha;
                    b /= alpha;
                }
                // Clamp colors
                r = std::max(0.0f, std::min(1.0f, r));
                g = std::max(0.0f, std::min(1.0f, g));
                b = std::max(0.0f, std::min(1.0f, b));

                // The running sample index is the point's source ID — Blast
                // chains carry it through so the surviving set can be mapped
                // back onto the deep image (Blast::getDeepImage()).
                cloud->addPoint(px, py, pz, r, g, b, (unsigned long long)sampleIndex);

                // Update bounding box
                if (px < bboxMin[0]) bboxMin[0] = px;
                if (py < bboxMin[1]) bboxMin[1] = py;
                if (pz < bboxMin[2]) bboxMin[2] = pz;
                if (px > bboxMax[0]) bboxMax[0] = px;
                if (py > bboxMax[1]) bboxMax[1] = py;
                if (pz > bboxMax[2]) bboxMax[2] = pz;

                ++sampleIndex;
            }
        }
    }

    // Store bounding box in the cloud for viewport framing
    cloud->setBounds(bboxMin[0], bboxMin[1], bboxMin[2],
                     bboxMax[0], bboxMax[1], bboxMax[2]);

    _lastPointCloud = cloud;

    // Update info
    std::ostringstream ss;
    ss << "Points: " << cloud->numPoints()
       << " | Total samples: " << srcDeep->totalSamples()
       << " | Density: " << (int)(densityVal * 100) << "%"
       << " | Image: " << dw.width() << "x" << dw.height();
    KnobStringPtr infoKnob = _imp->info.lock();
    if (infoKnob) infoKnob->setValue(ss.str());

    // Also produce flattened preview for 2D Viewer
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;

    if (outImg) {
        srcDeep->flattenToImage(outImg.get());
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepToPoints.cpp"
