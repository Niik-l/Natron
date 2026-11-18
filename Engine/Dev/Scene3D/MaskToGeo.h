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

#ifndef NATRON_ENGINE_MASKTOGEO_H
#define NATRON_ENGINE_MASKTOGEO_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <mutex>
#include <string>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "MaterialProvider.h"
#include "MeshProvider.h"

NATRON_NAMESPACE_ENTER

struct MaskToGeoPrivate;

/**
 * @brief MaskToGeo — turns the vector shapes of a Roto / RotoPaint / ReadMask
 * node into 3D geometry consumable by the Scene3D pipeline.
 *
 * Connect the mask source to the input; every active shape at the requested
 * time is flattened (Bezier::evaluateAtTime_DeCasteljau), tessellated with the
 * in-tree libtess (same engine as the Roto fill, normal (0,0,1)) and emitted
 * as one MeshData: a flat shape in the XY plane at z=0, centered on the
 * project format, scaled so the format height equals World Scale (1.0 by
 * default — Nuke-style normalized space). Extrude Depth > 0 adds side walls
 * and an offset cap; CCW-from-outside winding everywhere (normals come from
 * winding, per the house convention).
 *
 * The mesh is rebuilt whenever the input node's hash, its Roto context age
 * (curve edits) or this node's knobs change — animated masks (ReadMask file
 * sequences / baked keyframes / hand-animated Roto) become animated geo.
 *
 * Reads the input's RotoContext via Node::getRotoContext(); the node itself
 * produces no image (like GeoBuilder, it is consumed by SceneGraph /
 * ScanlineRender / CyclesRender through the MeshProvider contract).
 */
class MaskToGeo
    : public EffectInstance
    , public MaterialProvider
    , public MeshProvider
{
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new MaskToGeo(n); }

    MaskToGeo(NodePtr node);
    virtual ~MaskToGeo();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_MASKTOGEO; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "MaskToGeo"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;
    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }
    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return false; }

    // MeshProvider: the union of all active shapes, in local space (the
    // Transform page knobs are applied by the consumers by name).
    virtual MeshDataPtr getMeshData(double time) const OVERRIDE;

    // MaterialProvider (one material for the whole model, same knobs as GeoBuilder)
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
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    /// Pure mesh build (no knob writes, safe from any thread).
    MeshDataPtr buildMeshSnapshot(double time) const;

    /// Input hash + input Roto age + own hash: the snapshot is rebuilt when any
    /// of them moves (curve edits bump the Roto age without changing hashes).
    unsigned long long currentInputsHash(double time) const;

    std::unique_ptr<MaskToGeoPrivate> _imp;
    mutable std::mutex _meshMutex;
    mutable MeshDataPtr _mesh;                 // published snapshot
    mutable unsigned long long _meshInputsHash = 0;
    mutable double _meshInputsTime = -1.0;     // time the snapshot was built for
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_MASKTOGEO_H
