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

#ifndef NATRON_ENGINE_GEOMATERIALOVERRIDE_H
#define NATRON_ENGINE_GEOMATERIALOVERRIDE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <set>
#include <string>

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Assigns a different material to specific sub-objects of an upstream geo
 * source (currently ReadAlembicArchive) without creating new geometry.
 *
 * Sits in the geo chain: ReadAlembicArchive -> GeoMaterialOverride -> Scene ->
 * CyclesRender. Input 0 is the geo (passed through), input 1 is the Material3D.
 * The `surfaces` knob lists the full archive paths to override (one per line).
 *
 * It is a pure decorator: SceneGraph::rebuild reads the listed paths + the Mat
 * input and tags the matching SceneNodes with this override's material; the
 * renderer then shades just those sub-objects with it. Precedence is:
 * downstream/per-pass override > this per-part override > archive base material.
 *
 * Not a MaterialProvider itself — rebuild resolves input 1 directly.
 */
// No Q_OBJECT: this node declares no signals/slots/properties, so it needs no
// meta-object of its own (it inherits QObject via EffectInstance). Avoiding
// Q_OBJECT means no moc file to generate — node identification is done via
// dynamic_cast, not qobject_cast.
class GeoMaterialOverride
    : public EffectInstance
{
public:

    static EffectInstance* BuildEffect(NodePtr n) { return new GeoMaterialOverride(n); }

    GeoMaterialOverride(NodePtr node);
    virtual ~GeoMaterialOverride();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_GEOMATERIALOVERRIDE; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Material Override"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Input 0 (geo) is required; input 1 (mat) is optional.
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return (inputNb == 1); }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    /** @brief Parsed set of full archive paths this override targets (from the
     *  `surfaces` knob, one per line, trimmed, blanks skipped). */
    void getSurfacePaths(std::set<std::string>& out) const;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    KnobStringWPtr _surfacesKnob;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_GEOMATERIALOVERRIDE_H
