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
 * 2D grids (the part that matters first): with the node's panel open, Add
 * Grid then click four corners over the plate in the 2D viewer; the grid is
 * drawn as a quad subdivided by rows/cols and its corners stay draggable.
 * Grids are 2D only for now — a later step projects them through the cam
 * input onto a plane so they become 3D cards. When the src input is
 * connected the node passes it straight through, so viewing the node shows
 * the plate with the grids over it.
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

    // 2D pass-through of the src input, so the plate shows under the grids.
    virtual bool isIdentity(double time, const RenderScale& scale, const RectI& roi, ViewIdx view,
                            double* inputTime, ViewIdx* inputView, int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // 2D viewer overlay: the grids.
    virtual bool hasOverlay() const OVERRIDE FINAL { return true; }
    virtual void drawOverlay(double time, const RenderScale& renderScale, ViewIdx view) OVERRIDE FINAL;
    virtual bool onOverlayPenDown(double time, const RenderScale& renderScale, ViewIdx view,
                                  const QPointF& viewportPos, const QPointF& pos,
                                  double pressure, double timestamp, PenType pen) OVERRIDE FINAL;
    virtual bool onOverlayPenMotion(double time, const RenderScale& renderScale, ViewIdx view,
                                    const QPointF& viewportPos, const QPointF& pos,
                                    double pressure, double timestamp) OVERRIDE FINAL;
    virtual bool onOverlayPenUp(double time, const RenderScale& renderScale, ViewIdx view,
                                const QPointF& viewportPos, const QPointF& pos,
                                double pressure, double timestamp) OVERRIDE FINAL;

    /** A 2D grid drawn over the plate: four corners in the order they were
     *  clicked (go around the shape), subdivided rows x cols. */
    struct Grid
    {
        std::string name;
        double x[4] = {0, 0, 0, 0};   // corners in plate pixels, as seen at `frame`
        double y[4] = {0, 0, 0, 0};
        int rows = 4;
        int cols = 4;
        bool planar = true;           // project through the cam input onto `plane`
        int plane = 0;                // 0 ground (XZ, y=offset) 1 front (XY, z=offset) 2 side (YZ, x=offset) 3 facing camera at distance `offset`
        double offset = 0.0;
        double frame = 1.0;           // the frame the corners were placed on
    };
    std::vector<Grid> getGrids() const;
    int getSelectedGridIndex() const;

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

    // Grid list <-> hidden knob, selection <-> grid knobs.
    void loadGridsFromKnob();
    void saveGridsToKnob();
    void refreshGridChoice();
    void loadSelectedGridIntoKnobs();
    void storeKnobsIntoSelectedGrid();
    void setGridStatus(const std::string& text);

    // Camera projection of grids (cam input, plate size from src / project).
    struct CamView
    {
        double o[3];          // camera position
        double R[3][3];       // camera -> world rotation
        double tanH, tanV;    // half-angle tangents from focal + apertures
        double x1, y1, w, h;  // plate rectangle in pixels
    };
    bool cameraAt(double time, CamView& out) const;
    bool unprojectToPlane(const CamView& cv, const Grid& g, double px, double py, double out[3]) const;
    bool projectPoint(const CamView& cv, const double p[3], double& px, double& py) const;
    bool solveGrid3D(const Grid& g, double corners[4][3]) const;
    /** Where the grid's corners are drawn at `time`: the stored 2D corners, or
     *  the 3D corners re-projected through the camera when the grid is planar
     *  and a camera is connected. Returns false when it had to fall back. */
    bool displayCorners(const Grid& g, double time, double out[4][2]) const;
    virtual void onInputChanged(int inputNb) OVERRIDE FINAL;

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
