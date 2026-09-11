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

#ifndef NATRON_ENGINE_MATERIAL3D_H
#define NATRON_ENGINE_MATERIAL3D_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <memory>
#include <mutex>
#include <vector>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "MaterialProvider.h"

NATRON_NAMESPACE_ENTER

struct Material3DPrivate;

/**
 * @brief Standalone PBR material node for Cycles rendering.
 *
 * No inputs. Provides Principled BSDF material parameters.
 * Connect to a geometry node's "mat" input to assign the material.
 * Multiple geometry nodes can share the same Material3D.
 *
 * Equivalent to Houdini's Material node or Blender's Material data-block.
 */
class Material3D
    : public EffectInstance
    , public MaterialProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new Material3D(n); }

    Material3D(NodePtr node);
    virtual ~Material3D();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 6; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_MATERIAL3D; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Material3D"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return true; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    // MaterialProvider interface
    virtual void getMaterialBaseColor(double time, double& r, double& g, double& b) const OVERRIDE;
    virtual double getMaterialRoughness(double time) const OVERRIDE;
    virtual double getMaterialMetallic(double time) const OVERRIDE;
    virtual double getMaterialSpecular(double time) const OVERRIDE;
    virtual void getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const OVERRIDE;
    virtual double getMaterialTransmission(double time) const OVERRIDE;
    virtual double getMaterialIOR(double time) const OVERRIDE;
    virtual std::string getMaterialTextureFile() const OVERRIDE;
    virtual std::string getMaterialNormalMapFile() const OVERRIDE;
    virtual std::string getMaterialRoughnessMapFile() const OVERRIDE;
    virtual std::string getMaterialMetallicMapFile() const OVERRIDE;
    virtual std::string getMaterialEmissionMapFile() const OVERRIDE;
    virtual std::string getMaterialTransmissionMapFile() const OVERRIDE;
    virtual double getMaterialNormalStrength(double time) const OVERRIDE;
    virtual unsigned long long getMaterialInputsHash(double time) const OVERRIDE;
    virtual std::string getMaterialSpecularMapFile() const OVERRIDE;
    virtual std::string getMaterialDisplacementMapFile() const OVERRIDE;
    virtual std::string getMaterialOpacityMapFile() const OVERRIDE;
    virtual bool getMaterialOpacityInvert() const OVERRIDE;
    virtual std::string getMaterialTranslucencyMapFile() const OVERRIDE;
    virtual double getMaterialTranslucencyStrength(double time) const OVERRIDE;
    virtual double getMaterialDisplacementScale(double time) const OVERRIDE;
    virtual double getMaterialDisplacementMidlevel(double time) const OVERRIDE;
    virtual std::string getMaterialDiffuseColorspace() const OVERRIDE;
    virtual std::string getMaterialEmissionColorspace() const OVERRIDE;

    /**
     * @brief Render connected 2D inputs to temp files for Cycles.
     * Called by CyclesRender before syncing the scene.
     * Input 0=Diffuse, 1=Metallic, 2=Roughness, 3=Emission, 4=Normal
     */
    void bakeInputTextures(double time);

    // Cached diffuse texture for viewport preview (rendered from input 0). Geo
    // shapes (Card/Sphere/Cube/Cylinder) display this when this Material3D is
    // connected to their material input, so a textured material shows in the 3D
    // view (not just the Cycles render). Same shape as the geo CachedTexture.
    struct CachedTexture {
        std::vector<float> pixels; // RGBA float
        int width, height;
        CachedTexture() : width(0), height(0) {}
    };
    typedef std::shared_ptr<const CachedTexture> CachedTexturePtr;
    /** Published immutable snapshot — never null, never mutated after publish.
     *  Hold the returned shared_ptr for as long as you read it: the GUI paint
     *  and render workers call this concurrently and a republish must not free
     *  a buffer under a live reader. */
    CachedTexturePtr getCachedTexture() const {
        std::lock_guard<std::mutex> lk(_texMutex);
        return _cachedTexture;
    }
    void updateCachedTexture(double time);

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<Material3DPrivate> _imp;
    mutable std::mutex _texMutex;
    CachedTexturePtr _cachedTexture = std::make_shared<CachedTexture>();
    unsigned long long _texKey = 0;      // materialTextureCacheKey() of _cachedTexture
    bool _texKeyValid = false;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_MATERIAL3D_H
