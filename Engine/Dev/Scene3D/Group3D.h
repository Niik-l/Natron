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

#ifndef NATRON_ENGINE_GROUP3D_H
#define NATRON_ENGINE_GROUP3D_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

#define GROUP3D_MAX_INPUTS 8

NATRON_NAMESPACE_ENTER

struct Group3DPrivate;

/**
 * @brief Groups multiple 3D objects (cameras, geo, cards, point clouds)
 * into an organizational unit.
 *
 * Inputs [0, getNumChildInputs()) are the grouped objects (any 3D node type:
 * ReadGeo, Card3D, Light3D, another Group3D...). The input after them is an
 * optional **axis**: a parent transform, Nuke TransformGeo-style. Any 3D node
 * works there — its translate / rotate / scale (+ uniformScale) knobs are read
 * by name (ReadAlembicTransform, a Camera3D's keyed position, a Sphere3D...),
 * and another Group3D / TransformGeo contributes its full matrix, so chains
 * build hierarchies.
 *
 * The node's own Translate / Rotate / Scale (+ Uniform Scale) rotate and scale
 * around Pivot X/Y/Z. Local matrix = axis × T(t)·T(P)·R·S·T(−P); see
 * getLocalMatrix(). SceneGraph parents every child under that matrix, so the
 * 3D viewport, ScanlineRender and Cycles all agree.
 *
 * The 3D viewport can filter display to show only objects in a specific Group3D.
 *
 * TransformGeo (below) is the one-object form of the same node.
 */
class Group3D
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new Group3D(n); }

    Group3D(NodePtr node);
    virtual ~Group3D();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    // Children first, then the axis input.
    virtual int getNInputs() const OVERRIDE WARN_UNUSED_RETURN { return getNumChildInputs() + 1; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    /// Inputs [0, getNumChildInputs()) are the grouped objects. Everything that
    /// enumerates a group's children must use this, not getNInputs(), so the
    /// axis node is never mistaken for a grouped object.
    virtual int getNumChildInputs() const WARN_UNUSED_RETURN { return GROUP3D_MAX_INPUTS; }
    int getAxisInputIndex() const WARN_UNUSED_RETURN { return getNumChildInputs(); }

    virtual std::string getPluginID() const OVERRIDE WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_GROUP3D; }

    virtual std::string getPluginLabel() const OVERRIDE WARN_UNUSED_RETURN
    { return "Group3D"; }

    virtual std::string getPluginDescription() const OVERRIDE WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;

    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return true; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyFullySafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    // Knob T/R/S only (Uniform Scale folded into the scale; identity while
    // Enable Transform is off). No pivot, no axis — see getLocalMatrix().
    void getGroupTransform(double time, float& tx, float& ty, float& tz,
                           float& rx, float& ry, float& rz,
                           float& sx, float& sy, float& sz) const;

    // The group's full local matrix (column-major, GL-ready, SceneGraph
    // convention): axis-input matrix × T(t)·T(P)·R·S·T(−P). The knob part is
    // the identity while Enable Transform is off; the axis input applies
    // regardless. pivotOut (may be NULL) receives the Pivot knobs so the
    // viewport gizmo can seed its handle on the pivot.
    void getLocalMatrix(double time, float out[16], float pivotOut[3]) const;

    // Matrix of whatever is wired into the axis input; false when nothing is.
    bool getAxisMatrix(double time, float out[16]) const;

    // Bounding box of everything connected below this node, in the node's own
    // pre-transform space (children placed by their own transforms, nested
    // groups by theirs; this node's T/R/S and axis NOT applied). Lights and
    // cameras contribute nothing. False when no child has geometry.
    bool getChildrenBounds(double time, float bmin[3], float bmax[3]) const;

    // Check if a node is connected to this group (children only)
    bool isNodeInGroup(const std::string& nodeName) const;

protected:

    // TransformGeo turns the knob transform on by default; a Group3D is an
    // organiser first, so it keeps the upstream default (off).
    virtual bool transformEnabledByDefault() const { return false; }

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<Group3DPrivate> _imp;
};

/**
 * @brief Nuke-style TransformGeo: the one-object form of Group3D.
 *
 * Inputs: geo (the object, or a whole Group3D / Scene3D branch) and axis (an
 * optional parent transform, see Group3D). Same knobs, same matrix, same
 * consumers — only the input count, the labels and the Enable default differ.
 */
class TransformGeo
    : public Group3D
{
public:

    static EffectInstance* BuildEffect(NodePtr n) { return new TransformGeo(n); }

    TransformGeo(NodePtr node)
        : Group3D(node)
    {
    }

    virtual ~TransformGeo()
    {
    }

    virtual int getNumChildInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_TRANSFORMGEO; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "TransformGeo"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:

    virtual bool transformEnabledByDefault() const OVERRIDE FINAL { return true; }
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_GROUP3D_H
