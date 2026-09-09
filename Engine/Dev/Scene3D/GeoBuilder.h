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

#ifndef NATRON_ENGINE_GEOBUILDER_H
#define NATRON_ENGINE_GEOBUILDER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "MaterialProvider.h"
#include "MeshProvider.h"

NATRON_NAMESPACE_ENTER

struct GeoBuilderPrivate;

/**
 * @brief GeoBuilder — build simple geometry for a shot inside Natron (our
 * take on Nuke's ModelBuilder; see Engine/Dev/Research_GeoBuilder.md).
 *
 * Slice 1: the node owns a list of primitive shapes (card, cube, sphere,
 * cylinder), each with its own transform and a few parameters, and hands the
 * scene their union as one mesh (MeshProvider) with one material
 * (MaterialProvider), so Scene3D, the 3D viewport, ScanlineRender and Cycles
 * render it like a ReadGeo. Shapes are added from buttons on the panel, the
 * selected shape is edited through the "Shape" knobs (and the node-level
 * Transform page moves the whole model). The shape list is the node's state,
 * serialised in a hidden knob, so it saves with the project.
 *
 * Later slices: bake to OBJ / Project3D, vertex-edge-face editing, and
 * vertex alignment to the plate over the cam input.
 */
class GeoBuilder
    : public EffectInstance
    , public MaterialProvider
    , public MeshProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static EffectInstance* BuildEffect(NodePtr n) { return new GeoBuilder(n); }

    GeoBuilder(NodePtr node);
    virtual ~GeoBuilder();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 4; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_GEOBUILDER; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "GeoBuilder"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }
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

    // MeshProvider: the union of all visible shapes, in the node's local
    // space (the node-level Transform knobs are applied by the consumers).
    virtual MeshDataPtr getMeshData(double time) const OVERRIDE;

    // MaterialProvider interface (one material for the whole model)
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

    /** One shape in the model. Kept parametric in slice 1 (the mesh is
     *  regenerated from these); a later slice adds an explicit-mesh kind for
     *  edited geometry. */
    struct Shape
    {
        int type = 0;            // 0 card, 1 cube, 2 sphere, 3 cylinder
        std::string name;
        bool visible = true;
        int rows = 1;
        int cols = 1;
        double size = 1.0;       // card side / cube edge / sphere radius / cylinder radius
        double height = 2.0;     // cylinder only
        double tx = 0, ty = 0, tz = 0;
        double rx = 0, ry = 0, rz = 0;
        double sx = 1, sy = 1, sz = 1;
    };

    /** Snapshot of the shape list (for tests / future viewer tools). */
    std::vector<Shape> getShapes() const;
    int getSelectedShapeIndex() const;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    // Shape list <-> hidden knob, selection <-> per-shape knobs, mesh rebuild.
    void loadShapesFromKnob();
    void saveShapesToKnob();
    void refreshShapeChoice();
    void loadSelectedShapeIntoKnobs();
    void storeKnobsIntoSelectedShape();
    void addShape(int type);
    void rebuildMesh();

    std::unique_ptr<GeoBuilderPrivate> _imp;
    mutable std::mutex _meshMutex;
    MeshDataPtr _mesh;   // published snapshot, replaced whole on rebuild
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_GEOBUILDER_H
