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

#ifndef NATRON_ENGINE_SCANLINERENDER_H
#define NATRON_ENGINE_SCANLINERENDER_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct ScanlineRenderPrivate;

/**
 * @brief Render a 3D scene through a camera to produce a 2D output image.
 *
 * Input 0 (bg):      Optional background 2D image (composited behind, sets output resolution)
 * Input 1 (obj/scn): 3D geometry (Sphere3D, Card3D, ReadGeo, or Scene/Group3D)
 * Input 2 (cam):     Camera (Camera3D or ReadAlembicCamera)
 *
 * Output: 2D rendered image.
 *
 * The geometry's img input provides the texture. The camera defines the viewpoint.
 *
 * Comparable to scanline-render nodes found in other compositing DCCs.
 */
class ScanlineRender
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ScanlineRender(n); }

    ScanlineRender(NodePtr node);
    virtual ~ScanlineRender();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 3; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_SCANLINERENDER; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ScanlineRender"; }

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
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    // Phase 3D — declare per-pixel AOV planes (Normal/UV/Pref/Velocity) so
    // downstream nodes can shuffle/write them. Beauty stays on the default
    // color plane (-1). Each AOV is produced only when its knob is on.
    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual void getComponentsNeededAndProduced(double time, ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInput) OVERRIDE FINAL;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;

    // Trail-mode knobs are hidden unless Particle Mode = Trail.
    void refreshTrailKnobsVisibility();
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

private:
    // Single source of truth for the Nuke-style bg conform: if input 0 (bg) is connected,
    // returns its output rectangle (format preferred, RoD as fallback) so the output FORMAT,
    // the RoD, and the render framebuffer all agree. Returns false when no bg conforms.
    bool getBgConformRect(double time, ViewIdx view, RectI* outRect);

    std::unique_ptr<ScanlineRenderPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_SCANLINERENDER_H
