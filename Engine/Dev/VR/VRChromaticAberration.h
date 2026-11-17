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

#ifndef NATRON_ENGINE_DEV_VR_VRCHROMATICABERRATION_H
#define NATRON_ENGINE_DEV_VR_VRCHROMATICABERRATION_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct VRChromaticAberrationPrivate;

/**
 * @brief "ChromaticAberration VR": seam-safe latlong chromatic aberration, modeled on
 * Nukepedia's "More Chromatic Aberration" gizmo (Luan KOLB) with the VR adaptation of
 * the other VR tool nodes.
 *
 * Red, Green and Blue are displaced independently — radially from an adjustable Center,
 * plus a per-channel Shift and a per-channel Rotation around the Center for stylized
 * splits — and each channel can be blurred on its own. A radial Falloff ramp eases the
 * offset from zero at the Center to full strength outward (the gizmo's godrays-driven
 * smooth start-to-end transition, adapted to a latlong radial ramp).
 *
 * A standard 2D chromatic aberration breaks on latlong maps exactly the way a plain blur
 * does: displaced channel samples treat the left/right edges as hard boundaries and tear
 * a vertical seam at the 0/360-degree wrap. This node samples every channel through the
 * seam rules of VRFilterCore:
 *
 *   - horizontal wrap: the bilinear fetch wraps modulo the image width, so displaced
 *     pixels continue smoothly across the seam;
 *   - pole clamp: vertical samples clamp at the top/bottom rows;
 *   - latitude compensation (on by default): horizontal offsets grow by 1/cos(latitude)
 *     toward the poles (capped), keeping the split isotropic ON THE SPHERE.
 *
 * Alpha is never displaced. Expects an equirectangular (2:1) input; that is what the VR
 * viewport and the writers produce/consume. Use SphericalTransform to convert other
 * projections first.
 */
class VRChromaticAberration
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new VRChromaticAberration(n); }

    VRChromaticAberration(NodePtr node);
    virtual ~VRChromaticAberration();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_VRCHROMATICABERRATION; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ChromaticAberration VR"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back(PLUGIN_GROUP_VR); }

    virtual std::string getInputLabel(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Source"; }

    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return false; }

    virtual void addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyFullySafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<VRChromaticAberrationPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_VR_VRCHROMATICABERRATION_H
