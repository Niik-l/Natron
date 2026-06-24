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

#ifndef NATRON_ENGINE_MERGEMAT_H
#define NATRON_ENGINE_MERGEMAT_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <memory>

#include "../../../Global/Macros.h"
#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "MaterialProvider.h"

NATRON_NAMESPACE_ENTER

struct MergeMatPrivate;

/**
 * @class MergeMat — combine two materials/shaders, like Nuke's MergeMat (classic 3D).
 *
 * Composites a foreground material (input A) over a background material (input B) using a
 * Merge-style operation, and outputs a single material that plugs into a geometry's
 * material ("mat") input. Its primary use is layering multiple Project3D projections onto
 * one piece of geometry — e.g. a front projection 'over' a side projection.
 *
 * MergeMats chain (a MergeMat may feed another MergeMat's A or B input), so any number of
 * materials stack — matching how classic Nuke stacks projections with 2-input MergeMats.
 *
 * The actual per-fragment compositing of the layered projections is performed by
 * ScanlineRender (and approximated in the 3D viewport preview); the MaterialProvider
 * methods below delegate to the foreground material so non-projection consumers (Cycles)
 * still get a sensible single material.
 */
class MergeMat
    : public EffectInstance
    , public MaterialProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new MergeMat(n); }

    MergeMat(NodePtr node);
    virtual ~MergeMat();

    // Merge operations — order matches the Operation knob's choices (and Nuke's MergeMat).
    enum Operation
    {
        eMergeNone = 0,   // B only
        eMergeReplace,    // A only
        eMergeOver,       // A over B (default)
        eMergeStencil,    // B, outside A's alpha
        eMergeMask,       // B, inside A's alpha
        eMergePlus,       // A + B
        eMergeMax,        // max(A, B)
        eMergeMin         // min(A, B)
    };

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }  // 0=A (fg), 1=B (bg)
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_MERGEMAT; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "MergeMat"; }

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
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    // ---- MaterialProvider: delegate to the foreground (A) material, else background (B). ----
    virtual void getMaterialBaseColor(double time, double& r, double& g, double& b) const OVERRIDE;
    virtual double getMaterialRoughness(double time) const OVERRIDE;
    virtual double getMaterialMetallic(double time) const OVERRIDE;
    virtual double getMaterialSpecular(double time) const OVERRIDE;
    virtual void getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const OVERRIDE;
    virtual double getMaterialTransmission(double time) const OVERRIDE;
    virtual double getMaterialIOR(double time) const OVERRIDE;
    virtual std::string getMaterialTextureFile() const OVERRIDE { return std::string(); }

    // ---- MergeMat accessors (read by ScanlineRender / the viewport) ----
    /** Resolve the material connected to input A (ab=0) or B (ab=1), through Dots. May be
     *  another MergeMat (chained) or a Project3D / Material3D leaf. NULL if not connected. */
    MaterialProvider* getInputMaterial(int ab) const;
    int getOperation(double time) const;   // Operation
    double getMix(double time) const;       // 0..1 opacity of the foreground (A)

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<MergeMatPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_MERGEMAT_H
