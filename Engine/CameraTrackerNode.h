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

#ifndef CAMERATRACKERNODE_H
#define CAMERATRACKERNODE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/NodeGroup.h"
#include "Engine/Dev/Deep/PointCloudProvider.h"
#include "Engine/Dev/Scene3D/MeshData.h"

NATRON_NAMESPACE_ENTER

struct CameraTrackerNodePrivate;

class CameraTrackerNode
    : public NodeGroup
    , public PointCloudProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new CameraTrackerNode(n);
    }

    CameraTrackerNode(NodePtr node);
    virtual ~CameraTrackerNode();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isPluginDescriptionInMarkdown() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    {
        grouping->push_back("3D");
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafeFrame;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool isOutput() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostMaskingEnabled() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostMixingEnabled() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool isSubGraphUserVisible() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool hasOverlay() const OVERRIDE FINAL { return true; }

    // PointCloudProvider: expose the solved sparse 3D points to the 3D viewport
    // as a point cloud, in the same normalized space as the
    // Create Camera3D output so cloud and camera register.
    virtual PointCloudDataPtr getPointCloud() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Receives the 3D viewport's point selection (indices into getPointCloud()
    // order == solvedPoints order). Powers the scene-orientation tools
    // (Set Origin / Set Ground Plane) and snap-to-selection.
    virtual void setViewportSelection(const std::vector<int>& indices) OVERRIDE FINAL;

    // Solved 3D points as renderable locator geometry (a small octahedron per
    // point, in the same output gauge as Create Camera3D / the viewport cloud).
    // Consumed by ScanlineRender so track stick can be verified by rendering
    // the locators over the plate through the solved camera. Null if unsolved.
    MeshDataPtr getLocatorMesh() const;

    // ==== Panel API (Gui/CameraTrackerPanel — the Tracker-style track table) ====
    struct ManualTrackInfo
    {
        int id;
        int nFrames, firstFrame, lastFrame;
        double x, y;      // marker at (or nearest before) the query time
        double error;     // mean reprojection px; -1 unsolved, -2 rejected
        bool selected;
    };
    std::vector<ManualTrackInfo> getManualTracksInfo(double time) const;
    void panelSelectManualTrack(int id);
    void panelDeleteManualTrack(int id);
    void panelSetManualTrackPosition(int id, double time, double x, double y);
    void panelAddManualTrack(double time);
    // Re-emits manualTracksChanged (signals are protected; the private impl and
    // the Gui panel trigger refreshes through this).
    void notifyManualTracksChanged();
    // Open state of the Manual Tracks group — the settings-panel table shows
    // and hides with it so group + table read as one section.
    bool isManualSectionOpen() const;

Q_SIGNALS:
    // Manual track set / positions / selection / errors changed — the settings
    // panel's table listens to this.
    void manualTracksChanged();

public:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;

    // Auto-fill the tracking frame range from the connected clip on connect.
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

private:

    virtual void drawOverlay(double time, const RenderScale & renderScale, ViewIdx view) OVERRIDE FINAL;
    virtual bool onOverlayPenDown(double time, const RenderScale & renderScale, ViewIdx view,
                                  const QPointF & viewportPos, const QPointF & pos,
                                  double pressure, double timestamp, PenType pen) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenMotion(double time, const RenderScale & renderScale, ViewIdx view,
                                    const QPointF & viewportPos, const QPointF & pos,
                                    double pressure, double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenUp(double time, const RenderScale & renderScale, ViewIdx view,
                                const QPointF & viewportPos, const QPointF & pos,
                                double pressure, double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::unique_ptr<CameraTrackerNodePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // CAMERATRACKERNODE_H
