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

#ifndef CAMERATRACKERNODE_H
#define CAMERATRACKERNODE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/NodeGroup.h"

NATRON_NAMESPACE_ENTER

struct CameraTrackerNodePrivate;

class CameraTrackerNode
    : public NodeGroup
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new CameraTrackerNode(n);
    }

    CameraTrackerNode(NodePtr node);
    virtual ~CameraTrackerNode();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isPluginDescriptionInMarkdown() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    {
        grouping->push_back("3D");
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafeFrame;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool isOutput() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostMaskingEnabled() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostMixingEnabled() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool isSubGraphUserVisible() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool hasOverlay() const OVERRIDE FINAL { return true; }

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

private:

    virtual void drawOverlay(double time, const RenderScale & renderScale, ViewIdx view) OVERRIDE FINAL;
    virtual bool onOverlayPenDown(double time, const RenderScale & renderScale, ViewIdx view,
                                  const QPointF & viewportPos, const QPointF & pos,
                                  double pressure, double timestamp, PenType pen) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::unique_ptr<CameraTrackerNodePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // CAMERATRACKERNODE_H
