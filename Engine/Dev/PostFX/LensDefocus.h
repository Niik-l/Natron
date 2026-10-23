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

#ifndef NATRON_ENGINE_LENSDEFOCUS_H
#define NATRON_ENGINE_LENSDEFOCUS_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

namespace defocus {
struct CameraModel;
}

NATRON_NAMESPACE_ENTER

struct LensDefocusPrivate;

/**
 * @brief Physically based depth-of-field from an image and its depth pass.
 *
 * The blur size comes from a thin-lens model (focal length, f-stop, focus
 * distance, film back), read from a Camera3D input or typed in. Each pixel
 * spreads its light over its own bokeh shape (scatter), depth slices are
 * composited back to front, and the background hidden behind blurred
 * foreground edges is filled in (or taken from a Clean Background input).
 * The engine is DefocusCore (pure C++, shared with the standalone testbed).
 *
 * Inputs: 0 Source (RGBA + depth plane), 1 Depth (optional separate depth
 * image), 2 Camera (optional), 3 Clean Background (optional), 4 Mask.
 * Outputs: Color, plus the optional "defocusError" plane (R = how much of the
 * pixel came from guessed background, G = blur size was clamped).
 */
class LensDefocus
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new LensDefocus(n); }

    // deepNode: the DeepDefocus variant (input 0 must be a deep image; see DeepDefocus).
    LensDefocus(NodePtr node, bool deepNode = false);
    virtual ~LensDefocus();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 5; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_LENSDEFOCUS; }

    virtual std::string getPluginLabel() const OVERRIDE WARN_UNUSED_RETURN
    { return "LensDefocus"; }

    virtual std::string getPluginDescription() const OVERRIDE WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE
    { grouping->push_back(PLUGIN_GROUP_FILTER); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb != 0; }

    virtual bool isInputMask(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 4; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    // The depth-channel list is rebuilt on the main thread and read by render() under a mutex.
    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual PassThroughEnum isPassThroughForNonRenderedPlanes() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return ePassThroughPassThroughNonRenderedPlanes; }

    virtual void getComponentsNeededAndProduced(double time, ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInput) OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onMetadataRefreshed(const NodeMetadata& metadata) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    // Viewer: with Pick Focus on, a click (or drag) picks focus at the cursor from the last rendered
    // depth; Ctrl+middle-click does it any time. Other clicks go on to the Focus Point handle and
    // the viewer (colour pick, pan).
    virtual bool hasOverlay() const OVERRIDE FINAL { return true; }
    virtual bool onOverlayPenDown(double time, const RenderScale& renderScale, ViewIdx view,
                                  const QPointF& viewportPos, const QPointF& pos, double pressure,
                                  double timestamp, PenType pen) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenMotion(double time, const RenderScale& renderScale, ViewIdx view,
                                    const QPointF& viewportPos, const QPointF& pos, double pressure,
                                    double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenUp(double time, const RenderScale& renderScale, ViewIdx view,
                                const QPointF& viewportPos, const QPointF& pos, double pressure,
                                double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:

    const bool _deepNode;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    void refreshLayerChoices();
    void refreshKnobVisibility();
    // Align Focus Plane to Surface: tilt/swing from the normal under the last focus pivot.
    void alignFocusPlane();
    // Sets focus from the cached depth at canonical position pos (per Focus mode).
    void pickFocusAt(double time, const QPointF& pos);
    // Camera from the upstream Read's EXR header at this time (held / nearest frame if missing).
    bool fileCamera(double time, double pxScale, defocus::CameraModel* out);

    std::unique_ptr<LensDefocusPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_LENSDEFOCUS_H
