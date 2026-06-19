/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_FASTVOLUMERENDER_H
#define NATRON_ENGINE_FASTVOLUMERENDER_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct FastVolumeRenderPrivate;

/**
 * @brief Real-time GPU volume renderer for VDBs (compressed brick ray march).
 *
 * Input 0 (vdb):  ReadVDB node providing the volume grids
 * Input 1 (cam):  Optional camera (Camera3D / any CameraProvider).
 *                 Unconnected -> auto-frames the volume bbox.
 *
 * Output: linear premultiplied RGBA + AOV planes
 *   (Emission, SunScatter, AmbientScatter, Depth, Temperature, Shadow).
 *
 * ~15 ms/frame interactive preview counterpart to CyclesRender's
 * path-traced volumes: same scene inputs, swap the node for finals.
 */
class FastVolumeRender
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new FastVolumeRender(n); }

    FastVolumeRender(NodePtr node);
    virtual ~FastVolumeRender();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_FASTVOLUMERENDER; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "FastVolumeRender"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    // Multi-plane AOV support
    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual PassThroughEnum isPassThroughForNonRenderedPlanes() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return ePassThroughPassThroughNonRenderedPlanes; }

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getComponentsNeededAndProduced(double time, ViewIdx view,
                                                 EffectInstance::ComponentsNeededMap* comps,
                                                 double* passThroughTime, int* passThroughView,
                                                 int* passThroughInput) OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<FastVolumeRenderPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_FASTVOLUMERENDER_H
