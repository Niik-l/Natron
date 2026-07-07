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

#ifndef POINTCLOUDGENERATORNODE_H
#define POINTCLOUDGENERATORNODE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EffectInstance.h"
#include "Engine/Dev/Deep/PointCloudData.h"
#include "Engine/Dev/Deep/PointCloudProvider.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct PointCloudGeneratorNodePrivate;

/**
 * @brief Generate a dense(r) point cloud from footage + an already-solved camera.
 *
 * A *second pass* dense-reconstruction stage
 * that runs after a camera solve. Given the known per-frame camera poses (read
 * from a connected Camera3D via CameraProvider) it detects a dense grid of
 * features in the footage, tracks them, and triangulates each track with the
 * known cameras (N-view DLT) -- no re-solve needed. The result is much denser
 * than the sparse solve cloud (CameraTracker), bounded by trackable texture.
 *
 * Image-wise it is a pass-through of the Source input (input 0). The cloud is
 * exposed via PointCloudProvider for the 3D viewport.
 */
class PointCloudGeneratorNode
    : public EffectInstance
    , public PointCloudProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new PointCloudGeneratorNode(n); }

    PointCloudGeneratorNode(NodePtr node);
    virtual ~PointCloudGeneratorNode();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_POINTCLOUDGEN; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "PointCloudGenerator"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isPluginDescriptionInMarkdown() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (inputNb == 0) return "Source";
        if (inputNb == 1) return "Camera";
        return std::string();
    }

    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 1; }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyFullySafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN
    { return false; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    // PointCloudProvider — expose the generated dense cloud to the 3D viewport.
    virtual PointCloudDataPtr getPointCloud() const OVERRIDE WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view,
                             double time, bool originatedFromMainThread) OVERRIDE FINAL;

private:

    // Image pass-through of the Source input.
    virtual bool isIdentity(double time, const RenderScale& scale, const RectI& roi,
                            ViewIdx view, double* inputTime, ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale,
                                             ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::unique_ptr<PointCloudGeneratorNodePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // POINTCLOUDGENERATORNODE_H
