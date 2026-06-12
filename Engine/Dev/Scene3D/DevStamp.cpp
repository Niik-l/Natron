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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "DevStamp.h"

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

DevStamp::DevStamp(NodePtr node)
    : EffectInstance(node)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DevStamp::~DevStamp()
{
}

std::string
DevStamp::getPluginDescription() const
{
    return tr("Wireless node connection for the Stamps tool.\n\n"
              "An Anchor taps a source node (input 0 = the source); a Stamp, placed "
              "anywhere, connects back to an Anchor with its input wire hidden. Use "
              "the Stamps menu / hotkey to create and connect them.\n\n"
              "It is a transparent pass-through: the 3D / Cycles / particle systems "
              "see straight through it (input 0 carries the real data — geo, scene, "
              "material, camera, or a 2D image), so it never alters what flows "
              "through.").toStdString();
}

std::string
DevStamp::getInputLabel(int /*inputNb*/) const
{
    return "src";
}

void
DevStamp::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DevStamp::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DevStamp::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

bool
DevStamp::isIdentity(double time,
                     const RenderScale& /*scale*/,
                     const RectI& /*roi*/,
                     ViewIdx view,
                     double* inputTime,
                     ViewIdx* inputView,
                     int* inputNb)
{
    // Pure pass-through when something is connected: hand 2D streams straight to
    // input 0. (3D / scene-graph streams are carried by topology and resolved by
    // the isGraphPassthrough() see-through in DotUtils.h, independent of this.)
    if (getInput(0)) {
        *inputTime = time;
        *inputView = view;
        *inputNb = 0;
        return true;
    }
    return false;
}

void
DevStamp::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Stamp"));

    KnobStringPtr title = AppManager::createKnob<KnobString>(this, tr("Title"));
    title->setName("title");
    title->setAnimationEnabled(false);
    title->setEvaluateOnChange(false);
    title->setHintToolTip(tr("Identifier shared by an Anchor and the Stamps that "
                             "point at it. Stamps reconnect to the Anchor with the "
                             "matching Title."));
    page->addKnob(title);
    _titleKnob = title;

    KnobStringPtr tags = AppManager::createKnob<KnobString>(this, tr("Tags"));
    tags->setName("tags");
    tags->setAnimationEnabled(false);
    tags->setEvaluateOnChange(false);
    tags->setHintToolTip(tr("Optional space-separated tags for filtering Anchors in "
                            "the Stamps selection panel."));
    page->addKnob(tags);
    _tagsKnob = tags;

    // Role is managed by the Stamps tool ("anchor" / "stamp"); hidden from the UI.
    KnobStringPtr role = AppManager::createKnob<KnobString>(this, tr("Role"));
    role->setName("role");
    role->setAnimationEnabled(false);
    role->setEvaluateOnChange(false);
    role->setSecretByDefault(true);
    role->setDefaultValue("stamp");
    page->addKnob(role);
    _roleKnob = role;
}

StatusEnum
DevStamp::getPreferredMetadata(NodeMetadata& metadata)
{
    // Pass through the source's frame-varying flag so scrubbing still invalidates
    // the downstream render cache (mirrors the other routing-aware Dev nodes).
    EffectInstancePtr src = getInput(0);
    if (src && src->getHasAnimation()) {
        metadata.setIsFrameVarying(true);
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
