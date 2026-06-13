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

#ifndef NATRON_ENGINE_UVPROJECT_H
#define NATRON_ENGINE_UVPROJECT_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <vector>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct UVProjectPrivate;

/**
 * @brief Rewrite UVs on a 3D mesh via projection.
 *
 * Input 0 (geo): Upstream geometry (required).
 * Input 1 (cam): Projection camera (used in Perspective mode).
 * Input 2 (img): Optional projection plate; overrides upstream mesh's texture.
 *
 * Modes:
 *   - Perspective: project from `cam` through the mesh. With Generate Perspective ON,
 *     UVProject emits 3-component (s, t, w) texture coords — ScanlineRender uses
 *     glTexCoord4f for perspective-correct fragment-level divide (Mark Kilgard's
 *     textbook projective-texturing trick).
 *   - PlanarXY/YZ/ZX: world vert into projection-local frame, then U,V are two of
 *     the local axes.
 *   - Spherical: equirectangular u = atan2(z,x)/2π + 0.5, v = asin(y/r)/π + 0.5.
 *   - Cylindrical: u = atan2(z,x)/2π + 0.5, v = (y + 1) / 2 (user scales V to fit).
 *
 * Output: pass-through dummy image — UVProject is a UV rewriter, not a renderer.
 * Its work is consumed by ScanlineRender via the rewriteUVs() public hook.
 */
class UVProject
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    enum ProjectionMode {
        eModeOff = 0,
        eModePerspective,
        eModePlanarXY,
        eModePlanarYZ,
        eModePlanarZX,
        eModeSpherical,
        eModeCylindrical
    };

    static EffectInstance* BuildEffect(NodePtr n) { return new UVProject(n); }

    UVProject(NodePtr node);
    virtual ~UVProject();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 3; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_UVPROJECT; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "UVProject"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    // UVProject only rewrites mesh texture coords — it doesn't combine images.
    // Its own output is a 1x1 dummy. Multi-resolution must be true so users
    // can plug a Read of any size into the 'img' input.
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    // Convenience accessors used by ScanlineRender's extractGeometries.
    EffectInstancePtr getGeoInput() const { return getInput(0); }
    EffectInstancePtr getImgInput() const { return getInput(2); }

    /**
     * Rewrite UVs for the supplied vertex buffer (xyz interleaved) given the
     * mesh's world matrix. Reads knobs at `time`, returns the rewrite via
     * outUVs (2 floats per vertex) OR outSTW (3 floats per vertex). The
     * `outComponents` arg tells the caller which buffer was filled:
     *
     *   0 = mode is Off; both output buffers are cleared; caller keeps the
     *       upstream geo's existing UVs.
     *   2 = standard (u, v); outUVs is populated.
     *   3 = projective (s, t, w); outSTW is populated.
     */
    void rewriteUVs(const std::vector<float>& verts,
                    const float worldMatrix[16],
                    double time,
                    std::vector<float>& outUVs,
                    std::vector<float>& outSTW,
                    int& outComponents) const;

    // Cached projection texture for viewport preview (rendered from the img input,
    // input 2). The 3D viewport displays this on a geo whose UVs this UVProject
    // rewrites, so the projection shows in the view (not only in ScanlineRender).
    struct CachedTexture {
        std::vector<float> pixels; // RGBA float
        int width, height;
        CachedTexture() : width(0), height(0) {}
    };
    const CachedTexture& getCachedTexture() const { return _cachedTexture; }
    void updateCachedTexture(double time);

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<UVProjectPrivate> _imp;
    mutable CachedTexture _cachedTexture;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_UVPROJECT_H
