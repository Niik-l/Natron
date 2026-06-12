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

#ifndef NATRON_ENGINE_CYCLESRENDERSETTINGS_H
#define NATRON_ENGINE_CYCLESRENDERSETTINGS_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct CyclesRenderSettingsPrivate;

/**
 * @brief Sink node owning shared Cycles render settings.
 *
 * Holds samples + integrator bounces + DOF + motion blur knobs that both
 * CyclesRender and CyclesRenderPassManager consult. Lets one Settings node
 * drive both the live preview and the final disk render so they can't drift.
 *
 * Consumers `dynamic_cast` their settings-input effect directly to
 * `const CyclesRenderSettings*` and call the getters below. The
 * `CyclesSettingsProvider.h` interface exists as documentation of the
 * intended contract; multi-inheritance off `EffectInstance` + that
 * interface trips Qt6's AUTOMOC silently, so the contract is enforced by
 * duck typing for now (acceptable since CyclesRenderSettings is the only
 * implementer).
 *
 * No inputs, no image output — pure config carrier. Consumers wire to it
 * via an optional input slot and pull values per-frame.
 */
class CyclesRenderSettings
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new CyclesRenderSettings(n); }

    CyclesRenderSettings(NodePtr node);
    virtual ~CyclesRenderSettings();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_CYCLESRENDERSETTINGS; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "CyclesRenderSettings"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

    // getNInputs() returns 0 so isInputOptional is never actually called;
    // we override only because the base declares it pure virtual.
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

    // Shared-settings getters — see CyclesSettingsProvider.h for the
    // intended contract (the abstract interface isn't inherited; see
    // class comment for why).
    int    getSamples(double time) const;
    bool   getDenoise(double time) const;
    int    getMaxBounces(double time) const;
    int    getDiffuseBounces(double time) const;
    int    getGlossyBounces(double time) const;
    int    getTransmissionBounces(double time) const;
    bool   getDOFEnabled(double time) const;
    double getFocusDistance(double time) const;
    int    getBokehBlades(double time) const;
    double getBladeRotation(double time) const;
    bool   getMotionBlurEnabled(double time) const;
    double getShutterTime(double time) const;
    int    getShutterPosition(double time) const;
    // Default output colorspace for files written to disk by consumers
    // (CyclesRenderPassManager). Returns the selected OCIO colorspace id, or
    // empty if unset/unavailable. Per-pass JSON "colorspace" overrides this.
    std::string getOutputColorspace(double time) const;

    // Base output directory for disk renders. A connected CyclesRenderPass appends
    // <Pass Name>/v###/<Pass Name>.####.exr. Empty falls back to the project folder.
    std::string getOutputPath(double time) const;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<CyclesRenderSettingsPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CYCLESRENDERSETTINGS_H
