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

#ifndef NATRON_ENGINE_CARD3D_H
#define NATRON_ENGINE_CARD3D_H

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

struct Card3DPrivate;

/**
 * @brief Flat textured card (quad) geometry.
 *
 * Input 0 (img): 2D image to texture onto the card.
 *
 * Generates a single quad with UVs mapped 0-1.
 * Card aspect ratio is derived from the img input dimensions.
 * Connect to ScanlineRender's obj/scn input.
 *
 * Equivalent to Nuke's Card node.
 */
class Card3D
    : public EffectInstance
    , public MaterialProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new Card3D(n); }

    Card3D(NodePtr node);
    virtual ~Card3D();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_CARD3D; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Card3D"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

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

    // Card mesh data for viewport and ScanlineRender
    struct CardVertex {
        float x, y, z;
        float u, v;
        float nx, ny, nz;
    };

    void getCardTransform(double time,
                          double& tx, double& ty, double& tz,
                          double& rx, double& ry, double& rz,
                          double& sx, double& sy) const;

    void generateCardMesh(double time,
                          std::vector<CardVertex>& outVertices,
                          std::vector<int>& outTriIndices) const;

    // Nuke "image aspect": when true (default) the card's shape matches the img
    // input's aspect ratio; when false the card is a unit square regardless of the
    // input, so plugging in a differently-shaped image doesn't resize the geometry.
    bool getImageAspectEnabled() const;

    // Cached texture for viewport preview
    struct CachedTexture {
        std::vector<float> pixels; // RGBA float
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
    void updateCachedTexture(double time);

    // MaterialProvider interface
    virtual void getMaterialBaseColor(double time, double& r, double& g, double& b) const OVERRIDE;
    virtual double getMaterialRoughness(double time) const OVERRIDE;
    virtual double getMaterialMetallic(double time) const OVERRIDE;
    virtual double getMaterialSpecular(double time) const OVERRIDE;
    virtual void getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const OVERRIDE;
    virtual double getMaterialTransmission(double time) const OVERRIDE;
    virtual double getMaterialIOR(double time) const OVERRIDE;
    virtual std::string getMaterialTextureFile() const OVERRIDE;
    virtual std::string getMaterialDiffuseColorspace() const OVERRIDE;
    virtual bool hasMaterialInput() const OVERRIDE;
    virtual MaterialProvider* getConnectedMaterial() const OVERRIDE;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<Card3DPrivate> _imp;
    mutable std::mutex _texMutex;
    CachedTexturePtr _cachedTexture = std::make_shared<CachedTexture>();
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CARD3D_H
