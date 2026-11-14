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

#ifndef NATRON_ENGINE_READSPLATS_H
#define NATRON_ENGINE_READSPLATS_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <memory>
#include <string>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "SplatData.h"

NATRON_NAMESPACE_ENTER

struct ReadSplatsPrivate;

/**
 * @brief Read a 3D Gaussian splat scene (3DGS binary .ply, .splat, or .spz up to version 3).
 *
 * A 3D source node: connect it to a Scene3D or straight to ScanlineRender's
 * obj/scn input. The 3D viewport and ScanlineRender show the scene as a point
 * preview (one opaque round point per splat, coloured with the splat's base
 * colour and sized from its scale). The full splat data (scales, rotations,
 * opacity, spherical-harmonic colour) is kept for a splat renderer to use
 * through getSplatData().
 *
 * Deliberately NOT a ParticleProvider or PointCloudProvider: both would copy
 * every splat into a much larger per-point record and route the scene through
 * draw paths built for thousands of points, not millions.
 */
class ReadSplats
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ReadSplats(n); }

    ReadSplats(NodePtr node);
    virtual ~ReadSplats();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_READSPLATS; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ReadSplats"; }

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

    /**
     * @brief The scene, after the Crop tab's crop and cleanup, reading the file
     * on first use. Null when no file is set, the read failed, or the crop
     * leaves nothing (see getLastLoadError). Thread-safe: the 3D viewport paint
     * and render threads may call it at the same time; the file is read once.
     * The returned object never changes: a reload or a crop change publishes a
     * new one with a new SplatData::version.
     */
    SplatDataPtr getSplatData();

    /** Why the last read failed; empty when it succeeded or nothing was read yet. */
    std::string getLastLoadError() const;

    /** The node's Translate / Rotate / Scale (and Y-down flip) as one column-major
     *  matrix taking the file's space to world space. */
    void getSplatTransform(double time, float out[16]) const;

    /** The Crop tab's shape, for drawing it: mode 0 none / 1 box / 2 sphere, in the scene's
     *  upright space (see SplatFilter). `flipYZ` tells whether that space is the file's space
     *  with Y and Z negated; the matrix of getSplatTransform already contains that flip. */
    void getCropShape(int* mode, float center[3], float size[3], float* radius, bool* flipYZ) const;

    /** Uniform world scale contained in getSplatTransform (sizes multiply by it). */
    float getSplatWorldScale(double time) const;

    // Point-preview settings (the Preview tab), shared by the viewport and ScanlineRender.
    float getPreviewPointScale(double time) const;
    float getPreviewSizeClamp() const;
    float getPreviewOpacityThreshold() const;
    std::size_t getViewportMaxPoints() const;
    std::size_t getRenderMaxPoints() const;
    bool getPreviewLinearize() const;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    void refreshInfo();

    std::unique_ptr<ReadSplatsPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_READSPLATS_H
