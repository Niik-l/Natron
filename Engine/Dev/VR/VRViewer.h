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

#ifndef NATRON_ENGINE_DEV_VR_VRVIEWER_H
#define NATRON_ENGINE_DEV_VR_VRVIEWER_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"
#include "Engine/EffectInstance.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The VR Viewer node: the graph-side sink of the VR viewport.
 *
 * Wire the (processed) equirectangular footage into its input and create a VR
 * viewport pane (pane corner menu > "New VR viewport"); creating the node in
 * the GUI opens a bound pane automatically, the same way creating a Viewer
 * node opens a 2D viewer. Multiple VR Viewer nodes can coexist, each with its
 * own pane, exactly like viewers.
 *
 * The node is a transparent identity pass-through to input 0 (VR_PLAN.md:
 * "spherical-to-planar is simple math") — it never alters data; it exists so
 * the VR viewport has a proper node-tree wire to attach to instead of a
 * free-floating selector, and so upstream connections survive save/load and
 * reorganize like any other node.
 */
class VRViewer
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new VRViewer(n); }

    explicit VRViewer(NodePtr node);
    virtual ~VRViewer();

    virtual std::string getPluginID() const OVERRIDE FINAL;
    virtual std::string getPluginLabel() const OVERRIDE FINAL;
    virtual std::string getPluginDescription() const OVERRIDE FINAL;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL;

    // NOTE: deliberately NOT isOutput(). getWriters() (the headless render-all
    // path) collects isOutput() nodes and asserts they are OutputEffectInstances
    // (AppInstance.cpp) — true Viewers dodge that via background-mode eviction,
    // which does not apply to this node. The pane renders it through the preview
    // path, which never checks isOutput.
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL { Q_UNUSED(inputNb); return true; }

    virtual int getMajorVersion() const OVERRIDE FINAL { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL { return 0; }

    virtual int getNInputs() const OVERRIDE FINAL { return 1; }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL;

    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE FINAL
    { return false; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL { return false; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL
    { return eRenderSafetyFullySafe; }

    /** Pure pass-through to input 0, the DevStamp way. */
    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_VR_VRVIEWER_H
