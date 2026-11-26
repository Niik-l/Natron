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

#ifndef NATRON_ENGINE_DEV_MASK_TRACEMASK_H
#define NATRON_ENGINE_DEV_MASK_TRACEMASK_H

// TraceMask: bitmap tracing -> shapes.
//
// One node, three outputs:
//   2D image   render() rasterizes the traced shapes: brush fill + pen
//              outline (Shading tab: brush = shape, pen = outline).
//   3D mesh    MeshProvider: the same shading as geometry - the brush fill
//              (Brush Enabled) plus the pen outline stroked into a ribbon
//              (thickness, taper, join, dash pattern), tessellated through
//              the shared MaskMeshBuilder (built-in MaskToGeo functionality).
//   Vector     VectorShapeProvider: MaskToGeo reads the traced shapes
//              directly (no RotoContext involved anywhere).
//
// Auto-traces on input / parameter change (current frame). No Roto node
// machinery: no Shape/Stroke/Clone/Transform property pages, no roto
// overlay, no roto serialization.

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <memory>

#include "Global/Macros.h"

#include "Engine/EffectInstance.h"
#include "Engine/ViewIdx.h"
#include "Engine/Dev/Scene3D/MaterialProvider.h"
#include "Engine/Dev/Scene3D/MeshData.h"
#include "Engine/Dev/Scene3D/MeshProvider.h"
#include "Engine/Dev/Scene3D/VectorShapeProvider.h"
#include "Engine/Dev/Mask/VectorDocument.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct TraceMaskPrivate;

/**
 * @brief "TraceMask": bitmap tracing -> shapes. Auto-traces on input change.
 *
 * Tabs:
     *   TraceMask   : Map Channel, Threshold, Invert, Curve Precision, WriteOn
     *                 group (Start / End reveal window),
 *                 Geometry group (World Scale / Extrude Depth / Bevel),
 *                 info line
 *   Shading     : Brush (fill) / Pen (outline) color, opacity, thickness,
 *                 joins, ends, dash patterns - the brush color IS the
 *                 surface color (MaterialProvider); adjust materials
 *                 downstream with a GeoMaterialOverride node
 *   Transform   : whole-model translate/rotate/scale (read by name by the
 *                 Scene3D consumers)
 */
class TraceMask
    : public EffectInstance
    , public MeshProvider
    , public MaterialProvider
    , public VectorShapeProvider
{
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new TraceMask(n); }

    TraceMask(NodePtr node);
    virtual ~TraceMask();

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_TRACEMASK; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "TraceMask"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /// Single source image input
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Required virtuals from EffectInstance
    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL;
    // Unsafe: the trace publish + rasterize write into shared state, so
    // concurrent renders of this node must be serialized by the framework.
    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyUnsafe; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale,
                                             ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // MeshProvider: the union of the traced shapes, in local space (the
    // Transform page knobs are applied by the consumers by name).
    virtual MeshDataPtr getMeshData(double time) const OVERRIDE FINAL;

    // MaterialProvider (one material for the whole model, same knobs as MaskToGeo)
    virtual void getMaterialBaseColor(double time, double& r, double& g, double& b) const OVERRIDE FINAL;
    virtual double getMaterialRoughness(double time) const OVERRIDE FINAL;
    virtual double getMaterialMetallic(double time) const OVERRIDE FINAL;
    virtual double getMaterialSpecular(double time) const OVERRIDE FINAL;
    virtual void getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const OVERRIDE FINAL;
    virtual double getMaterialTransmission(double time) const OVERRIDE FINAL;
    virtual double getMaterialIOR(double time) const OVERRIDE FINAL;
    virtual std::string getMaterialTextureFile() const OVERRIDE FINAL;
    virtual std::string getMaterialDiffuseColorspace() const OVERRIDE FINAL;
    virtual bool hasMaterialInput() const OVERRIDE FINAL;
    virtual MaterialProvider* getConnectedMaterial() const OVERRIDE FINAL;

    // VectorShapeProvider: the traced shapes, already in canonical space.
    virtual VectorMask::VectorDocument getShapeDocument(double time) const OVERRIDE FINAL;

    /// Trace `time` synchronously when the published doc doesn't cover it.
    /// Main thread: fetch + trace (gated). Render threads: NEVER fetch on
    /// the calling thread (a nested render inside a foreign render action
    /// corrupts the caller's render TLS) - run the trace on a dedicated
    /// one-shot thread instead; the consumer picks the result up when the
    /// call returns.
    void ensureTraced(double time) const;

    /// ensureTraced body: RoD query + gated fetch + trace + publish. Runs
    /// on the dedicated worker thread.
    void ensureTracedInternal(const NodePtr& input, double time, int channelIdx,
                              float threshold, bool invert, int precision) const;

private Q_SLOTS:

    /// Any trace knob changed: re-trace.
    void onControlKnobChanged();

    /// World Scale / Extrude Depth changed: rebuild the mesh, no re-trace.
    void onMeshKnobChanged();

    /// Pen Line Type changed: update the dash/space knob visibility.
    void onPenLineTypeChanged();

    /// The input node may have changed (knobsAgeChanged hook).
    void onInputMayHaveChanged();

    /// 0ms deferred input check (knobsAgeChanged fires before the input's
    /// hash is recomputed - tracing here would fetch the pre-edit image).
    void onInputCheckTimeout();

    /// Trace request that can be queued from any thread (getMeshData /
    /// getShapeDocument kick this when nothing was traced yet).
    void scheduleTrace();

    /// Async trace job finished (main thread): refresh UI + invalidate.
    void onTraceFinished();

private:

    virtual void initializeData() OVERRIDE FINAL;

    /// Input connection changes (re-hook the input's knobsAgeChanged).
    virtual void onInputChanged(int inputNo) OVERRIDE;

    /// Show Dash/Space pairs per Pen Line Type (Solid none, Dashed/Dotted 1,
    /// Custom all four).
    void updatePenLineSecrets();

    /// Kick an async trace job (coalesced; main thread).
    void trace(double time);

    /// Worker body: fetch + border trace + hole detection + publish.
    void traceInternal(double time, const RectD& inputRod, int channelIdx, float threshold, bool invert, int precision);

    /// Trace synchronously from an already-fetched image (render bootstrap;
    /// pure math on the provided image, no nested render).
    void traceFromImage(const ImagePtr& img, double time, int channelIdx, float threshold, bool invert, int precision);

    /// Binary border trace of a packed float plane (row 0 = image BOTTOM,
    /// Natron buffer order) into doc-space contours with the enclosing-region
    /// hierarchy (connected components + Moore boundary tracing: outer
    /// borders, enclosed background holes and nested islands, always closed).
    /// Invert flips the foreground test (value below the threshold).
    struct TraceResult {
        std::vector<std::vector<std::pair<double, double> > > contours;
        std::vector<int> depths; // 0 = outer, odd = hole, even = island
    };
    TraceResult doTraceBinary(const std::vector<float>& image, int w, int h, float threshold, bool invert);

    /// Simplify one contour (Douglas-Peucker; tolerance scales with the
    /// plane's reference size and Curve Precision).
    void simplifyContour(std::vector<std::pair<double, double> >* contour, int precision, double refSize) const;

    /// Build the canonical document from traced contours (tracer hierarchy
    /// for hole operators, Curve Precision, Catmull-Rom tangents, buffer ->
    /// canonical transform).
    bool buildCanonicalDoc(const std::vector<std::vector<std::pair<double, double> > >& contours,
                           const std::vector<int>& depths,
                           const ImagePtr& srcImg,
                           VectorMask::VectorDocument* doc,
                           int precision) const;

    /// Publish doc + mesh under the state mutex.
    void publish(const VectorMask::VectorDocument& doc, const MeshDataPtr& mesh, double time);

    /// Mesh build from the published canonical doc (MaskMeshBuilder; same
    /// normalization as MaskToGeo).
    MeshDataPtr buildMeshFromDoc(const VectorMask::VectorDocument& doc, double time) const;

    /// Rebuild the mesh from the published doc (3D knob edits; main thread).
    void rebuildMesh(double time);

    /// Drop the published shapes (no input / failed fetch).
    void clearPublished();

    /// Main-thread post-publish: info line + hash bump + viewer re-render.
    void refreshAfterTrace(bool updateInfo);

    void updateHooks();

    /// Fill + stroke rasterization of the published shapes into the output
    /// plane (Krokodove Shading: brush then pen).
    void rasterize(const VectorMask::VectorDocument& doc, const RenderActionArgs& args) const;

    std::unique_ptr<TraceMaskPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_MASK_TRACEMASK_H
