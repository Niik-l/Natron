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

#include "Group3D.h"

#include <cassert>
#include <sstream>

#include "../../AppInstance.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct Group3DPrivate
{
    // Group transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;
    KnobBoolWPtr enableTransform;
    KnobStringWPtr info;
};


Group3D::Group3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Group3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Group3D::~Group3D()
{
}

std::string
Group3D::getPluginDescription() const
{
    return tr("Group multiple 3D objects together.\n\n"
              "Connect ReadGeo, ReadAlembicCamera, Card3DRender, DeepToPoints, "
              "or other 3D nodes to the inputs to organize them into a group.\n\n"
              "The optional Group Transform applies an offset to all connected objects "
              "when displayed in the 3D viewport.\n\n"
              "Use this to organize complex 3D scenes — similar to Nuke's Scene node "
              "but focused on grouping and organization.").toStdString();
}

std::string
Group3D::getInputLabel(int inputNb) const
{
    std::ostringstream ss;
    ss << "Input " << (inputNb + 1);
    return ss.str();
}

void
Group3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Group3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Group3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Group3D::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Group Transform"));

    KnobBoolPtr en = AppManager::createKnob<KnobBool>(this, tr("Enable Group Transform"));
    en->setName("enableTransform"); en->setDefaultValue(false);
    en->setHintToolTip(tr("Apply the group transform to all connected objects in the 3D viewport."));
    page->addKnob(en); _imp->enableTransform = en;

    KnobDoublePtr tx = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
    tx->setName("translateX"); tx->setDefaultValue(0); tx->setAnimationEnabled(true);
    tx->setDisplayMinimum(-100); tx->setDisplayMaximum(100);
    page->addKnob(tx); _imp->translateX = tx;

    KnobDoublePtr ty = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
    ty->setName("translateY"); ty->setDefaultValue(0); ty->setAnimationEnabled(true);
    ty->setDisplayMinimum(-100); ty->setDisplayMaximum(100);
    page->addKnob(ty); _imp->translateY = ty;

    KnobDoublePtr tz = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
    tz->setName("translateZ"); tz->setDefaultValue(0); tz->setAnimationEnabled(true);
    tz->setDisplayMinimum(-100); tz->setDisplayMaximum(100);
    page->addKnob(tz); _imp->translateZ = tz;

    KnobDoublePtr rx = AppManager::createKnob<KnobDouble>(this, tr("Rotate X"));
    rx->setName("rotateX"); rx->setDefaultValue(0); rx->setAnimationEnabled(true);
    rx->setDisplayMinimum(-180); rx->setDisplayMaximum(180);
    page->addKnob(rx); _imp->rotateX = rx;

    KnobDoublePtr ry = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y"));
    ry->setName("rotateY"); ry->setDefaultValue(0); ry->setAnimationEnabled(true);
    ry->setDisplayMinimum(-180); ry->setDisplayMaximum(180);
    page->addKnob(ry); _imp->rotateY = ry;

    KnobDoublePtr rz = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z"));
    rz->setName("rotateZ"); rz->setDefaultValue(0); rz->setAnimationEnabled(true);
    rz->setDisplayMinimum(-180); rz->setDisplayMaximum(180);
    page->addKnob(rz); _imp->rotateZ = rz;

    KnobDoublePtr sx = AppManager::createKnob<KnobDouble>(this, tr("Scale X"));
    sx->setName("scaleX"); sx->setDefaultValue(1); sx->setAnimationEnabled(true);
    sx->setMinimum(0.01); sx->setDisplayMinimum(0.1); sx->setDisplayMaximum(10);
    page->addKnob(sx); _imp->scaleX = sx;

    KnobDoublePtr sy = AppManager::createKnob<KnobDouble>(this, tr("Scale Y"));
    sy->setName("scaleY"); sy->setDefaultValue(1); sy->setAnimationEnabled(true);
    sy->setMinimum(0.01); sy->setDisplayMinimum(0.1); sy->setDisplayMaximum(10);
    page->addKnob(sy); _imp->scaleY = sy;

    KnobDoublePtr sz = AppManager::createKnob<KnobDouble>(this, tr("Scale Z"));
    sz->setName("scaleZ"); sz->setDefaultValue(1); sz->setAnimationEnabled(true);
    sz->setMinimum(0.01); sz->setDisplayMinimum(0.1); sz->setDisplayMaximum(10);
    page->addKnob(sz); _imp->scaleZ = sz;

    // Info
    KnobPagePtr infoPage = AppManager::createKnob<KnobPage>(this, tr("Info"));

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info"); info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false); info->setIsPersistent(false);
    info->setDefaultValue("Connect 3D nodes to group them together.");
    infoPage->addKnob(info); _imp->info = info;
}

void
Group3D::getGroupTransform(double time, float& tx, float& ty, float& tz,
                           float& rx, float& ry, float& rz,
                           float& sx, float& sy, float& sz) const
{
    bool enabled = _imp->enableTransform.lock()->getValueAtTime(time);
    if (!enabled) {
        tx = ty = tz = rx = ry = rz = 0;
        sx = sy = sz = 1;
        return;
    }

    tx = (float)_imp->translateX.lock()->getValueAtTime(time);
    ty = (float)_imp->translateY.lock()->getValueAtTime(time);
    tz = (float)_imp->translateZ.lock()->getValueAtTime(time);
    rx = (float)_imp->rotateX.lock()->getValueAtTime(time);
    ry = (float)_imp->rotateY.lock()->getValueAtTime(time);
    rz = (float)_imp->rotateZ.lock()->getValueAtTime(time);
    sx = (float)_imp->scaleX.lock()->getValueAtTime(time);
    sy = (float)_imp->scaleY.lock()->getValueAtTime(time);
    sz = (float)_imp->scaleZ.lock()->getValueAtTime(time);
}

bool
Group3D::isNodeInGroup(const std::string& nodeName) const
{
    for (int i = 0; i < GROUP3D_MAX_INPUTS; ++i) {
        EffectInstancePtr input = getInput(i);
        if (input && input->getNode()->getScriptName() == nodeName) {
            return true;
        }
    }
    return false;
}

StatusEnum
Group3D::getPreferredMetadata(NodeMetadata& metadata)
{
    // If any connected input has animated knobs, this group is frame-varying.
    for (int i = 0; i < GROUP3D_MAX_INPUTS; ++i) {
        EffectInstancePtr inp = getInput(i);
        if (inp && inp->getHasAnimation()) {
            metadata.setIsFrameVarying(true);
            return eStatusOK;
        }
    }
    // Also check own knobs (group transform might be animated)
    if (getNode()->hasAnimatedKnob()) {
        metadata.setIsFrameVarying(true);
    }
    return eStatusOK;
}

StatusEnum
Group3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                               ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0; rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Group3D::render(const RenderActionArgs& args)
{
    // Update info with connected node count
    int connectedCount = 0;
    std::ostringstream ss;
    ss << "Connected: ";
    for (int i = 0; i < GROUP3D_MAX_INPUTS; ++i) {
        EffectInstancePtr input = getInput(i);
        if (input) {
            if (connectedCount > 0) ss << ", ";
            ss << input->getNode()->getScriptName();
            ++connectedCount;
        }
    }
    if (connectedCount == 0) {
        ss.str("No 3D nodes connected.");
    } else {
        ss << " (" << connectedCount << " objects)";
    }
    _imp->info.lock()->setValue(ss.str());

    Q_UNUSED(args);
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Group3D.cpp"
