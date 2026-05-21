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

#ifndef NATRON_ENGINE_DEVSHUFFLE_H
#define NATRON_ENGINE_DEVSHUFFLE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct DevShufflePrivate;

/**
 * @brief Layer-aware channel shuffle node with two-row routing.
 *
 * Two-row routing:
 * - Row 1: routes channels from input B (primary)
 * - Row 2: routes channels from input A (secondary)
 *
 * Row 2 routing overrides row 1 for the same output channel.
 * Cross-row connections are supported via routing encoding:
 * - Values 0-99 address row 1 (B) input channels
 * - Values 100-199 address row 2 (A) input channels
 *
 * Input 0 (B): Primary input
 * Input 1 (A): Secondary input (optional)
 */
class DevShuffle
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new DevShuffle(n); }

    DevShuffle(NodePtr node);
    virtual ~DevShuffle();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_DEVSHUFFLE; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "DevShuffle"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("Channel"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }   // cachedLayers mutated from main thread; render reads them

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual PassThroughEnum isPassThroughForNonRenderedPlanes() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return ePassThroughPassThroughNonRenderedPlanes; }

    // User-created layer management. Delegates to Node::addUserComponents
    // (the canonical Natron API) so layers survive save/reload via the
    // standard <UserComponents> XML block. targetRow = 0 routes the new
    // layer to Row 1's output combo, 1 to Row 2's; -1 = no row hint.
    void addUserLayer(const ImagePlaneDesc& layer, int targetRow = -1);

    // Access cached layers for GUI channel name display
    const std::vector<ImagePlaneDesc>& getCachedLayers() const;
    const std::vector<ImagePlaneDesc>& getCachedLayers2() const;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getComponentsNeededAndProduced(double time, ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInput) OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    // Post-load hook. Fires after knobs are restored; inputs may not be wired
    // yet. We defer the refresh via QTimer::singleShot so user layers from the
    // <UserComponents> block can land in the output combos.
    virtual void onKnobsLoaded() OVERRIDE FINAL;

    // Metadata-refresh hook (Natron equivalent of OFX getClipPreferences).
    // Fires after input wiring AND upstream metadata is computed — exactly
    // when getAvailableLayers actually returns useful data.
    virtual void onMetadataRefreshed(const NodeMetadata& metadata) OVERRIDE FINAL;

    void refreshLayerChoices();
    void refreshLayerChoices2();

    // Resolve the output ImagePlaneDesc from an output-layer combo value.
    // First N entries map to cached input layers; entries past that index
    // map to user layers (from Node::getUserCreatedComponents).
    ImagePlaneDesc resolveOutputPlane(const KnobChoiceWPtr& outputLayerKnob,
                                       const std::vector<ImagePlaneDesc>& cachedInputLayers) const;

    std::unique_ptr<DevShufflePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEVSHUFFLE_H
