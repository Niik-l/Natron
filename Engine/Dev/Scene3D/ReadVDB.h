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

#ifndef NATRON_ENGINE_READVDB_H
#define NATRON_ENGINE_READVDB_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <vector>
#include <string>

#ifdef NATRON_HAVE_OPENVDB
#ifndef Q_MOC_RUN
#include <openvdb/openvdb.h>
#endif
#endif

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct ReadVDBPrivate;

/**
 * @brief Read OpenVDB (.vdb) files for volumetric rendering.
 *
 * Loads a density grid from a .vdb file and converts it to a dense 3D texture
 * for ray marching in ScanlineRender.
 *
 * Supports animated VDB sequences (frame padding).
 * Connect to ScanlineRender's obj/scn input.
 */
class ReadVDB
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ReadVDB(n); }

    ReadVDB(NodePtr node);
    virtual ~ReadVDB();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_READVDB; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ReadVDB"; }

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

    // Volume data for ScanlineRender
    struct VDBVolumeData {
        std::vector<float> densityData; // dense 3D array, resX*resY*resZ floats, X-major
        int resX, resY, resZ;            // per-axis resolution (non-cubic — preserves VDB aspect)
        float bboxMinX, bboxMinY, bboxMinZ;
        float bboxMaxX, bboxMaxY, bboxMaxZ;
        float colorR, colorG, colorB;
        float density;
    };

    bool getVolumeData(double time, VDBVolumeData& outData);

    // Lightweight bbox-only query — reads VDB grid metadata (no voxel data).
    // Returns world-space bounds of the density grid (or first available grid).
    // Used by the 3D viewport for wireframe display.
    bool getVDBBounds(double time,
                      float& outMinX, float& outMinY, float& outMinZ,
                      float& outMaxX, float& outMaxY, float& outMaxZ);

    // Coarse density (+ fire) sampling for the 3D viewport splat preview (plain
    // floats — no OpenVDB in the API). Samples the density grid on a lattice
    // ~targetN along the longest axis (proportional on the others), normalized to
    // [0,1]. Also samples a fire grid (flames / temperature / heat) on the same
    // lattice if present — `outFire` is left empty for a pure smoke sim. EXPENSIVE
    // (re-reads the file) — the caller MUST cache the result per (path, frame).
    bool getViewportDensitySamples(double time, int targetN,
                                   std::vector<float>& outDensity,
                                   std::vector<float>& outFire,
                                   int& outNx, int& outNy, int& outNz);

    // Direct VDB grid access for Cycles (skips dense conversion)
    // Returns the OpenVDB grid and render params without converting to dense array.
    // REVERT NOTE: if this causes issues, use getVolumeData() instead (dense path).
#ifndef Q_MOC_RUN
#ifdef NATRON_HAVE_OPENVDB
    struct VDBGridInfo {
        openvdb::GridBase::ConstPtr grid;
        std::string name;
    };
    struct VDBDirectData {
        std::vector<VDBGridInfo> grids;  // All grids from the file
        float density;
        float colorR, colorG, colorB;
        float absorptionR, absorptionG, absorptionB;
        float stepSize;
        int volumeBounces;
        float anisotropy;
        float blackbodyIntensity;
        float blackbodyTintR, blackbodyTintG, blackbodyTintB;
        float temperatureScale;
        // Grid name bindings (user-configurable)
        std::string bindDensity, bindTemperature, bindFlame, bindColor, bindVelocity;
        // Remap curves — sampled at 256 points from KnobParametric
        static const int REMAP_SAMPLES = 256;
        std::vector<float> densityRemap;      // 256 floats, maps [0,1] input → output
        std::vector<float> temperatureRemap;  // 256 floats, maps [0,1] input → output
    };
    bool getVDBDirect(double time, VDBDirectData& outData);
#endif
#endif

    void getTransform(double time,
                      double& tx, double& ty, double& tz,
                      double& sx, double& sy, double& sz) const;

    /**
     * @brief Why the last file read failed — the resolved path plus the OpenVDB
     * exception text. Empty when the last read succeeded.
     *
     * The readers (FastVolumeRender, CyclesRenderer) surface this in their own
     * error message: on its own "failed to load VDB grids" gives the user
     * nothing to act on, and the underlying reason previously only reached
     * stderr, which a GUI launch never shows.
     */
    std::string getLastLoadError() const;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    void loadVDBFile(const std::string& path);

    // Resolve the file to read for `time`: applies Frame Offset and substitutes
    // the frame number ONLY when the picked file was detected as part of a real
    // sequence (see detectSequence in the .cpp). A single-frame VDB is returned
    // untouched, whatever digits its name happens to end in.
    std::string resolvePathAtTime(double time) const;

    std::unique_ptr<ReadVDBPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_READVDB_H
