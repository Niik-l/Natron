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

#ifndef NATRON_ENGINE_VOLUME3D_H
#define NATRON_ENGINE_VOLUME3D_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <mutex>
#include <vector>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct Volume3DPrivate;

/**
 * @brief Procedural 3D volume for volumetric rendering.
 *
 * Generates a 3D density field (sphere, noise, cloud) that can be
 * rendered through ScanlineRender using GPU ray marching.
 *
 * No inputs. The volume is defined by procedural parameters.
 * Connect to ScanlineRender's obj/scn input.
 */
class Volume3D
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new Volume3D(n); }

    Volume3D(NodePtr node);
    virtual ~Volume3D();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_VOLUME3D; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Volume3D"; }

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

    // Volume data for rendering
    struct VolumeParams {
        float centerX, centerY, centerZ;
        float scaleX, scaleY, scaleZ;
        float density;
        float colorR, colorG, colorB;
        int resolution;
        int volumeType;  // 0=sphere, 1=box (base shape)
        bool enableNoise; // layer procedural noise on the base shape
        float baseFlatness;  // flat cloud base (vertical cut at bottom), 0=off
        float topFlatness;   // flat cloud top (vertical cut at top, anvil/stratus), 0=off
        float heightFalloff; // vertical density gradient (dense base->wispy top), 0=off
        float noiseScale;
        float noiseDetail;
        float warp;     // domain-warp strength (noise cloud)
        float coverage; // cloud amount: 1=full (default), lower carves puffs
        float erosion;  // edge erosion: high-freq dissolve of the rim into wisps
        float edgeDetail; // fine edge breakup: high-freq carve that fragments the rim
        int   seed;     // noise variation seed (offsets the sampling)
        float windX, windY, windZ;      // drift speed (noise cells / frame)
        float noiseOffX, noiseOffY, noiseOffZ; // seed offset + wind*time (applied to noise)
        float stepSize;
        int volumeBounces;
    };

    VolumeParams getVolumeParams(double time) const;

    // Hash of the shape params (placement/colour excluded) — lets downstream
    // renderers cache the generated field and re-build only when it changes.
    U64 getShapeHash(double time) const;

    // Generate the 3D density data (resolution^3 floats)
    void generateVolumeData(double time, std::vector<float>& outData, int& resolution) const;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    // Show/hide the noise knobs based on the Enable Noise toggle.
    void updateKnobVisibility();

    // Load a cloud shape preset (1=Cumulus, 2=Stratus, 3=Cumulonimbus, 4=Wispy)
    // into the shape/noise/profile knobs. 0 (Custom) is a no-op.
    void applyPreset(int idx);

    std::unique_ptr<Volume3DPrivate> _imp;
    // Guards the volume cache below — generateVolumeData is called from GUI
    // paint and render workers concurrently.
    mutable std::mutex _volCacheMutex;
    mutable std::vector<float> _cachedVolData;
    mutable int _cachedVolRes;
    mutable double _cachedVolTime;
    mutable uint64_t _cachedVolHash = 0;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_VOLUME3D_H
