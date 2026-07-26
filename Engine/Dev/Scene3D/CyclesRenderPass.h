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

#ifndef NATRON_ENGINE_CYCLESRENDERPASS_H
#define NATRON_ENGINE_CYCLESRENDERPASS_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

#define RENDERPASS_MAX_OBJECTS 16

struct RenderPassPrivate;

/**
 * @brief Per-object visibility configuration for a render pass.
 */
struct ObjectVisibility
{
    uint32_t rayVisibility = 0;   // PATH_RAY bitmask
    bool isHoldout = false;
    bool isShadowCatcher = false;
    bool isExcluded = false;
    bool reflectionMatte = false; // emit a flat colour into the ReflectionMatte AOV
};

/**
 * @brief Per-light ray-visibility override (per pass). Each flag = "this light /
 * dome environment is VISIBLE to that ray type." Default all-true = inherit the
 * light's natural visibility; turning one off REMOVES the light from that ray type
 * (e.g. uncheck glossy so a dome doesn't appear in reflections — it still lights).
 * Camera is meaningful for the dome (matches its Renderable flag); for area/point
 * lights it's a no-op since they're camera-invisible by default.
 */
struct LightRayVis
{
    bool camera   = true;
    bool glossy   = true;   // reflections
    bool diffuse  = true;
    bool transmit = true;
};

/**
 * @brief Render pass filter node for multi-pass rendering.
 *
 * Sits between Scene3D and CyclesRender. Defines which objects are
 * visible to camera, which are trace-only (phantom), which are holdouts,
 * and which lights are active.
 *
 * Input 0 (scene): Scene3D or Group3D with all objects
 *
 * CyclesRender reads the visibility map from this node and applies
 * per-object ray visibility flags before rendering.
 */
class CyclesRenderPass
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new CyclesRenderPass(n); }

    CyclesRenderPass(NodePtr node);
    virtual ~CyclesRenderPass();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    // 0 = scene (filter input, read by downstream CyclesRender), 1 = camera,
    // 2 = settings. Camera/settings drive this node's own live Cycles preview.
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 3; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_CYCLESRENDERPASS; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "CyclesRenderPass"; }

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

    // Multi-plane AOV support for the live preview (mirrors CyclesRender).
    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual PassThroughEnum isPassThroughForNonRenderedPlanes() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return ePassThroughPassThroughNonRenderedPlanes; }

    /**
     * @brief Get the pass name.
     */
    std::string getPassName() const;

    /**
     * @brief Build the visibility map for all objects in the scene.
     * Call this before rendering to get per-object visibility config.
     */
    std::map<std::string, ObjectVisibility> getObjectVisibilityMap() const;

    /**
     * @brief Get the set of active light names. Empty = all lights.
     */
    std::set<std::string> getActiveLights() const;

    /**
     * @brief Per-light ray-visibility overrides (light name -> flags). Read from the
     * per-light Cam/Refl/Diff/Trans toggles in the Active Lights rows.
     */
    std::map<std::string, LightRayVis> getLightRayVisibility() const;

    /**
     * @brief Discover objects and lights from the connected Scene input.
     */
    void discoverSceneObjects(std::vector<std::string>& outGeo,
                              std::vector<std::string>& outLights) const;

    /**
     * @brief Refresh the object/light checkbox lists from the connected Scene.
     */
    void refreshObjectLists();

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;

public:
    /**
     * @brief Deep output side channel (Deep Output on the AOV tab): the last
     * preview render's deep samples as a DeepImage. Null when disabled.
     * Registered in DeepUtils::getDeepImageFromEffect.
     */
    DeepImagePtr getDeepImage() const { return _lastDeepImage; }

private:
    mutable DeepImagePtr _lastDeepImage;
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL;
    virtual void getComponentsNeededAndProduced(double time, ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime, int* passThroughView,
                                                int* passThroughInput) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    // "Render to Disk" button handler: render this pass over the Output-page frame
    // range to multi-layer EXRs at <Output Path>/<Pass Name>/v###/<Pass Name>.####.exr.
    void renderToDisk();

    // Output-page review/import handlers.
    void openInRV();       // launch RV on the latest rendered version's sequence
    void importRender();   // create (or re-point) a linked Read at the latest version
    void updateRender();   // re-point the linked Read to the latest version + clear stale flag
    void refreshLinkStatus();        // update the linked-Read status + outdated badge
    NodePtr getLinkedReadNode() const; // resolve the linked Read (cache, else by name)

    std::unique_ptr<RenderPassPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CYCLESRENDERPASS_H
