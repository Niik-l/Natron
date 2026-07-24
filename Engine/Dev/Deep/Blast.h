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

#ifndef NATRON_ENGINE_BLAST_H
#define NATRON_ENGINE_BLAST_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "PointCloudData.h"
#include "PointCloudProvider.h"

NATRON_NAMESPACE_ENTER

struct BlastPrivate;

/**
 * @brief Filter/delete points from a point cloud by bounding box,
 * selection, or expression.
 *
 * Similar to Houdini's Blast SOP. Operates on PointCloudData from
 * upstream DeepToPoints nodes.
 *
 * Input 0: Source (deep node producing a point cloud)
 */
class Blast
    : public EffectInstance
    , public PointCloudProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new Blast(n); }

    Blast(NodePtr node);
    virtual ~Blast();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_BLAST; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Blast"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (inputNb == 0) return "points";
        if (inputNb == 1) return "bounds";
        return std::string();
    }

    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 1; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    // Point cloud access for downstream / viewport — implements PointCloudProvider.
    virtual PointCloudDataPtr getPointCloud() const OVERRIDE;
    void computeFilteredCloud(double time);

    /** Deep output: rebuilds the originating DeepToPoints' source deep image
     *  minus the samples whose points this Blast chain deleted, so a blasted
     *  cloud can go straight back into deep compositing (DeepRecolor,
     *  DeepMerge, DeepFlatten, DeepWrite...). Point IDs carried through the
     *  chain map surviving points onto deep samples; samples that were never
     *  in the cloud (alpha≈0 skipped, density-thinned) are kept untouched.
     *  Returns null when the cloud didn't originate from a DeepToPoints. */
    DeepImagePtr getDeepImage() const;

    // Get current blast bounds as an axis-aligned bbox (encloses the rotated
    // OBB when a Cube3D is used as bounds input). Useful for ROI / culling.
    bool getBlastBounds(double time, float outMin[3], float outMax[3]) const;

    // --- Selection mode (Mode 1) API ---
    // Selected source-cloud indices are stored as a comma-separated string
    // on a hidden knob, so they persist across project save/load and survive
    // undo/redo.

    /** Set the selection (replace whatever was there). Used by the viewport
     *  "Blast: Set as Selection" context-menu action. */
    void setSelectedIndices(const std::vector<int>& indices);

    /** Add indices to the existing selection (set union). Used by "Blast: Add
     *  Selected". */
    void addToSelection(const std::vector<int>& indices);

    /** Remove indices from the existing selection (set difference). Used by
     *  "Blast: Remove Selected". */
    void removeFromSelection(const std::vector<int>& indices);

    /** Empty the selection. Used by "Blast: Clear Selection". */
    void clearSelection();

    /** Get the current selection (parsed from the knob). */
    std::vector<int> getSelectedIndices() const;

    // Get current blast region as an oriented bounding box.
    //   outCenter[3]    — world-space center
    //   outExtent[3]    — per-axis half-extents in the OBB's local frame
    //   outMatrix[16]   — column-major 4x4: world = matrix * local. Includes
    //                     translation + rotation only (NOT scale — scale is
    //                     baked into extent). Suitable for glMultMatrixf.
    // Returns false if no valid region (e.g. mode != BoundingBox).
    bool getBlastOBB(double time,
                     float outCenter[3], float outExtent[3],
                     float outMatrix[16]) const;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<BlastPrivate> _imp;
    mutable PointCloudDataPtr _lastOutput;
    mutable DeepImagePtr _lastDeepImage;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_BLAST_H
