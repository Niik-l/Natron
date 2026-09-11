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

#ifndef NATRON_ENGINE_PROJECT3D_H
#define NATRON_ENGINE_PROJECT3D_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <memory>
#include <mutex>
#include <vector>

#include "../../../Global/Macros.h"
#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "MaterialProvider.h"

NATRON_NAMESPACE_ENTER

struct Project3DPrivate;

/**
 * @class Project3D — camera-projection shader, applied as a material (Nuke Project3D parity).
 *
 * Projects a 2D plate (input 0) through a projection camera (input 1) onto whatever
 * geometry this node's output is connected to (plug into a geo's material / "mat" input,
 * like Material3D). ScanlineRender performs the projection per-fragment: it transforms
 * each geo fragment's world position by the projector camera's view-projection, divides
 * to the plate's UV, and samples the plate.
 *
 * Controls match Nuke's Project3D: project-on (front / back / both), crop, and occlusion
 * (none / self / world). It is a composable material shader — NOT a standalone renderer —
 * so multiple projections compose into a single scene rendered by ScanlineRender.
 *
 * (This replaces the original monolithic Project3D, which was a standalone FBO renderer.
 * The shader architecture matches how Nuke and other DCCs actually do camera projection.)
 */
class Project3D
    : public EffectInstance
    , public MaterialProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new Project3D(n); }

    Project3D(NodePtr node);
    virtual ~Project3D();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }  // 0=plate, 1=cam
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_PROJECT3D; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Project3D"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    // ---- MaterialProvider interface (neutral defaults; the projection is done by
    //      ScanlineRender via the query methods below, not via a flat texture) ----
    virtual void getMaterialBaseColor(double time, double& r, double& g, double& b) const OVERRIDE;
    virtual double getMaterialRoughness(double time) const OVERRIDE;
    virtual double getMaterialMetallic(double time) const OVERRIDE;
    virtual double getMaterialSpecular(double time) const OVERRIDE;
    virtual void getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const OVERRIDE;
    virtual double getMaterialTransmission(double time) const OVERRIDE;
    virtual double getMaterialIOR(double time) const OVERRIDE;
    virtual std::string getMaterialTextureFile() const OVERRIDE { return std::string(); }

    // ---- Projection query (read by ScanlineRender when this is a geo's material) ----
    enum ProjectOn { eProjectFront = 0, eProjectBack = 1, eProjectBoth = 2 };
    enum OcclusionMode { eOcclusionNone = 0, eOcclusionSelf = 1, eOcclusionWorld = 2 };

    /** Read the projector camera (input 1). Returns false if no camera is connected
     *  (projection inactive — the geo keeps its own material/texture). */
    bool getProjectorCamera(double time,
                            double& tx, double& ty, double& tz,
                            double& rx, double& ry, double& rz,
                            double& focal, double& hAperture, double& vAperture) const;

    int getProjectOn(double time) const;       // ProjectOn
    bool getCropToFrame(double time) const;     // true = transparent outside the plate frame
    double getNearClip(double time) const;
    double getFarClip(double time) const;
    int getOcclusionMode(double time) const;    // OcclusionMode

    // ---- 3D-viewport preview (so a geo this material is on shows the live projection) ----
    struct CachedTexture {
        std::vector<float> pixels;  // RGBA float
        int width, height;
        CachedTexture() : width(0), height(0) {}
    };
    typedef std::shared_ptr<const CachedTexture> CachedTexturePtr;
    /** Published immutable snapshot — never null, never mutated after publish.
     *  Hold the returned shared_ptr for as long as you read it: GUI paint and
     *  render workers call this concurrently and a republish must not free a
     *  buffer under a live reader. */
    CachedTexturePtr getCachedTexture() const {
        std::lock_guard<std::mutex> lk(_texMutex);
        return _cachedTexture;
    }
    /** Render the plate (input 0) to a small preview texture for the 3D viewport. */
    void updateCachedTexture(double time);
    /** Build the projector view*projection matrix from the camera (input 1), column-major
     *  float[16]. Returns false if no camera. The viewport uses it for perspective-correct
     *  projected UVs (same STW math as UVProject). */
    bool getProjectorViewProj(double time, float outVP[16]) const;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<Project3DPrivate> _imp;
    mutable std::mutex _texMutex;
    CachedTexturePtr _cachedTexture = std::make_shared<CachedTexture>();
    unsigned long long _texKey = 0;      // materialTextureCacheKey() of _cachedTexture
    bool _texKeyValid = false;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PROJECT3D_H
