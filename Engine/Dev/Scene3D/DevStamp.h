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

#ifndef NATRON_ENGINE_DEVSTAMP_H
#define NATRON_ENGINE_DEVSTAMP_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <string>

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A transparent routing node for the "Stamps" wireless-connection tool.
 *
 * One node type plays both roles, distinguished by the `role` knob:
 *   - "anchor": placed on a source node (input 0 = the source). The reference
 *     point that Stamps connect to. Carries a Title (+ optional Tags).
 *   - "stamp" : placed anywhere, connected to an Anchor (input 0 = the anchor),
 *     usually with the node's "Hide inputs" knob on so the wire is hidden.
 *
 * Like a Dot, it is a pure pass-through: `isGraphPassthrough()` (DotUtils.h)
 * reports it so the 3D / Cycles / particle traversals see straight through it
 * (input 0 carries the real data — geo, scene, material, camera, 2D image...).
 * Unlike a Dot it is a full EffectInstance, so it has a settings panel, a label,
 * the "Hide inputs" knob and its own Title/Tags/Role knobs — all of which the
 * Stamps Python tool drives.
 *
 * 2D image streams pass straight through (isIdentity -> input 0). For 3D /
 * scene-graph streams the data is carried by topology, resolved by the traversal
 * see-through above, so this node produces no geometry of its own.
 */
// No Q_OBJECT: declares no signals/slots of its own (inherits QObject via
// EffectInstance). Identified via dynamic_cast / getPluginID, not qobject_cast.
class DevStamp
    : public EffectInstance
{
public:

    static EffectInstance* BuildEffect(NodePtr n) { return new DevStamp(n); }

    DevStamp(NodePtr node);
    virtual ~DevStamp();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_DEVSTAMP; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Stamp"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("Other"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The single input is optional so an unconnected Stamp/Anchor doesn't error.
    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return true; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:

    // Pure pass-through for 2D streams: identity on input 0.
    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL;
    // No getRegionOfDefinition / render overrides: like NoOpBase/Dot, the node is a
    // pure pass-through — isIdentity routes to input 0 and the framework follows it
    // for both RoD and image. The default RoD/render handle the unconnected case.

    KnobStringWPtr _roleKnob;
    KnobStringWPtr _titleKnob;
    KnobStringWPtr _tagsKnob;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEVSTAMP_H
