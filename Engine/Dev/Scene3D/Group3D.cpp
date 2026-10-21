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
#include "SceneGraph.h" // buildTRSPivot / multiply
#include "GeoExtract.h" // children bounds for the centroid buttons
#include "../DotUtils.h"

#include <algorithm>
#include <cassert>
#include <cstring>
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
    KnobDoubleWPtr uniformScale;
    KnobDoubleWPtr pivotX, pivotY, pivotZ;
    KnobButtonWPtr movePivotToCentroid, moveCentroidToOrigin;
    KnobBoolWPtr enableTransform;
    // Viewport bounding-box display (read by Viewport3D::drawGroupNode)
    KnobBoolWPtr showBBox;
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
              "Connect ReadGeo, Card3D, lights, other groups or any 3D node to the "
              "numbered inputs to organize them into a group.\n\n"
              "The optional Transform (Translate / Rotate / Scale / Uniform Scale, turning "
              "around Pivot) moves every connected object together in the 3D viewport, "
              "ScanlineRender and Cycles.\n\n"
              "The axis input is an optional parent transform (Nuke TransformGeo-style): "
              "wire any 3D node there and its Translate / Rotate / Scale become the parent "
              "of this group — a ReadAlembicTransform, a Camera3D's keyed position, or another "
              "Group3D / Transform3D for hierarchies.\n\n"
              "Similar to Nuke's Scene node but focused on grouping and organization; "
              "Transform3D is the one-object form.").toStdString();
}

std::string
Group3D::getInputLabel(int inputNb) const
{
    if (inputNb == getAxisInputIndex()) {
        return "axis";
    }
    std::ostringstream ss;
    ss << "Input " << (inputNb + 1);
    return ss.str();
}

std::string
Transform3D::getPluginDescription() const
{
    return tr("Transform a 3D object (Nuke TransformGeo).\n\n"
              "geo: the object to move — a ReadGeo, Card3D, light, or a whole Group3D / "
              "Scene3D branch.\n"
              "axis: optional parent transform. Any 3D node wired here contributes its "
              "Translate / Rotate / Scale (ReadAlembicTransform, a Camera3D's keyed position, "
              "a Sphere3D...); another Transform3D or Group3D contributes its full matrix, so "
              "Transform3D → Transform3D chains build hierarchies.\n\n"
              "Rotate and Scale turn around Pivot X/Y/Z; the viewport gizmo handle sits on the "
              "pivot. Uniform Scale multiplies all three scale axes.").toStdString();
}

std::string
Transform3D::getInputLabel(int inputNb) const
{
    return inputNb == 0 ? "geo" : "axis";
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
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Transform"));

    KnobBoolPtr en = AppManager::createKnob<KnobBool>(this, tr("Enable Transform"));
    en->setName("enableTransform"); en->setDefaultValue(transformEnabledByDefault());
    en->setHintToolTip(tr("Apply this node's Translate / Rotate / Scale to every connected object "
                          "(3D viewport, ScanlineRender, Cycles). The axis input applies regardless."));
    page->addKnob(en); _imp->enableTransform = en;

    // Bounding-box display toggle for the 3D viewport (read by
    // Viewport3D::drawGroupBounds). Off hides the dashed box; on (default)
    // draws the union of the group's mesh children in world space.
    KnobBoolPtr showBox = AppManager::createKnob<KnobBool>(this, tr("Show Bounding Box"));
    showBox->setName("showBBox"); showBox->setDefaultValue(true);
    showBox->setHintToolTip(tr("Draw a dashed bounding box around the group's contents "
                               "in the 3D viewport. The box encloses the actual geometry "
                               "of every mesh connected to the group."));
    page->addKnob(showBox); _imp->showBBox = showBox;

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

    KnobDoublePtr us = AppManager::createKnob<KnobDouble>(this, tr("Uniform Scale"));
    us->setName("uniformScale"); us->setDefaultValue(1); us->setAnimationEnabled(true);
    us->setMinimum(0.01); us->setDisplayMinimum(0.1); us->setDisplayMaximum(10);
    us->setHintToolTip(tr("Multiplies Scale X / Y / Z together."));
    page->addKnob(us); _imp->uniformScale = us;

    KnobDoublePtr px = AppManager::createKnob<KnobDouble>(this, tr("Pivot X"));
    px->setName("pivotX"); px->setDefaultValue(0); px->setAnimationEnabled(true);
    px->setDisplayMinimum(-100); px->setDisplayMaximum(100);
    px->setHintToolTip(tr("Point, in this node's space, that Rotate and Scale turn around. "
                          "The viewport gizmo handle sits on it."));
    page->addKnob(px); _imp->pivotX = px;

    KnobDoublePtr py = AppManager::createKnob<KnobDouble>(this, tr("Pivot Y"));
    py->setName("pivotY"); py->setDefaultValue(0); py->setAnimationEnabled(true);
    py->setDisplayMinimum(-100); py->setDisplayMaximum(100);
    page->addKnob(py); _imp->pivotY = py;

    KnobDoublePtr pz = AppManager::createKnob<KnobDouble>(this, tr("Pivot Z"));
    pz->setName("pivotZ"); pz->setDefaultValue(0); pz->setAnimationEnabled(true);
    pz->setDisplayMinimum(-100); pz->setDisplayMaximum(100);
    page->addKnob(pz); _imp->pivotZ = pz;

    // Houdini-style helpers. "Centroid" = bounding-box centre of everything
    // connected below, measured at the current frame in this node's own
    // pre-transform space.
    KnobButtonPtr mp = AppManager::createKnob<KnobButton>(this, tr("Move Pivot to Centroid"));
    mp->setName("movePivotToCentroid");
    mp->setHintToolTip(tr("Set Pivot X/Y/Z to the bounding-box centre of the connected geometry "
                          "(at the current frame), so Rotate and Scale turn around the object and "
                          "the gizmo handle sits on it."));
    mp->setAddNewLine(false);
    page->addKnob(mp); _imp->movePivotToCentroid = mp;

    KnobButtonPtr mc = AppManager::createKnob<KnobButton>(this, tr("Move Centroid to Origin"));
    mc->setName("moveCentroidToOrigin");
    mc->setHintToolTip(tr("Set Translate to minus the connected geometry's bounding-box centre and "
                          "the Pivot to that centre, so the geometry's centre lands on the origin "
                          "and later Rotate / Scale turn around it there (current frame)."));
    page->addKnob(mc); _imp->moveCentroidToOrigin = mc;

    // Info
    KnobPagePtr infoPage = AppManager::createKnob<KnobPage>(this, tr("Info"));

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info"); info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false); info->setIsPersistent(false);
    info->setDefaultValue(getNumChildInputs() == 1 ? "Connect a 3D node to the geo input."
                                                   : "Connect 3D nodes to group them together.");
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
    const float us = (float)_imp->uniformScale.lock()->getValueAtTime(time);
    sx = (float)_imp->scaleX.lock()->getValueAtTime(time) * us;
    sy = (float)_imp->scaleY.lock()->getValueAtTime(time) * us;
    sz = (float)_imp->scaleZ.lock()->getValueAtTime(time) * us;
}

void
Group3D::getLocalMatrix(double time, float out[16], float pivotOut[3]) const
{
    float tx, ty, tz, rx, ry, rz, sx, sy, sz;
    getGroupTransform(time, tx, ty, tz, rx, ry, rz, sx, sy, sz);

    float px = 0.f, py = 0.f, pz = 0.f;
    if ( _imp->enableTransform.lock()->getValueAtTime(time) ) {
        px = (float)_imp->pivotX.lock()->getValueAtTime(time);
        py = (float)_imp->pivotY.lock()->getValueAtTime(time);
        pz = (float)_imp->pivotZ.lock()->getValueAtTime(time);
    }

    float trs[16];
    SceneGraph::buildTRSPivot(tx, ty, tz, rx, ry, rz, sx, sy, sz, px, py, pz, trs);

    // out = Axis × TRS (column-major): the node's own transform acts on the
    // vertices first, then the axis parent — the same Parent × Local order
    // SceneGraph uses for the world matrix. SceneGraph::multiply() applies its
    // FIRST argument first, hence trs before axis.
    float axis[16];
    if ( getAxisMatrix(time, axis) ) {
        SceneGraph::multiply(trs, axis, out);
    } else {
        std::memcpy(out, trs, sizeof(trs));
    }

    if (pivotOut) {
        pivotOut[0] = px; pivotOut[1] = py; pivotOut[2] = pz;
    }
}

bool
Group3D::getAxisMatrix(double time, float out[16]) const
{
    EffectInstancePtr axis = skipDots( getInput( getAxisInputIndex() ) );
    if (!axis) {
        return false;
    }

    // Another group / Transform3D: take its whole matrix so chains nest.
    if ( const Group3D* g = dynamic_cast<const Group3D*>( axis.get() ) ) {
        g->getLocalMatrix(time, out, NULL);
        return true;
    }

    // Any other 3D node: its transform knobs by name (the convention every
    // mesh / light / camera node in the 3D system follows).
    auto knob = [&](const char* name, double def) -> float {
        KnobIPtr k = axis->getKnobByName(name);
        KnobDouble* kd = k ? dynamic_cast<KnobDouble*>( k.get() ) : NULL;
        return kd ? (float)kd->getValueAtTime(time) : (float)def;
    };
    const float us = knob("uniformScale", 1.0);
    SceneGraph::buildTRS(knob("translateX", 0.0), knob("translateY", 0.0), knob("translateZ", 0.0),
                         knob("rotateX", 0.0),    knob("rotateY", 0.0),    knob("rotateZ", 0.0),
                         knob("scaleX", 1.0) * us, knob("scaleY", 1.0) * us, knob("scaleZ", 1.0) * us,
                         out);
    return true;
}

// Union the 8 corners of a local-space box, mapped through `m`, into bmin/bmax.
static void
unionTransformedBox(const float m[16], const float lo[3], const float hi[3],
                    float bmin[3], float bmax[3], bool& any)
{
    for (int c = 0; c < 8; ++c) {
        const float p[3] = { (c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2] };
        float w[3];
        geoMat4MulPoint(m, p, w);
        if (!any) {
            bmin[0] = bmax[0] = w[0]; bmin[1] = bmax[1] = w[1]; bmin[2] = bmax[2] = w[2];
            any = true;
        } else {
            for (int i = 0; i < 3; ++i) {
                bmin[i] = std::min(bmin[i], w[i]);
                bmax[i] = std::max(bmax[i], w[i]);
            }
        }
    }
}

// Walk a child: a nested group contributes its children under its own matrix,
// anything else goes through GeoExtract (its localMatrix = the node's own
// T/R/S x embedded transform; bbox in that node's mesh space).
static void
accumulateChildBounds(const EffectInstancePtr& eff, const float parentM[16], double time,
                      float bmin[3], float bmax[3], bool& any)
{
    if (!eff) return;
    if ( const Group3D* g = dynamic_cast<const Group3D*>( eff.get() ) ) {
        float local[16], m[16];
        g->getLocalMatrix(time, local, NULL);
        geoMat4Mul(m, parentM, local); // parent * local: local applied first
        const int n = g->getNumChildInputs();
        for (int i = 0; i < n; ++i) {
            accumulateChildBounds(skipDots( g->getInput(i) ), m, time, bmin, bmax, any);
        }
        return;
    }
    TriMesh tm;
    if ( !extractTriangleMesh(eff, time, tm) ) return;
    float m[16];
    geoMat4Mul(m, parentM, tm.localMatrix);
    unionTransformedBox(m, tm.bboxMin, tm.bboxMax, bmin, bmax, any);
}

// Does `group` have `child` wired (through Dots) into one of its child inputs?
static bool
groupHasChild(const Group3D* group, const EffectInstance* child)
{
    const int n = group->getNumChildInputs();
    for (int i = 0; i < n; ++i) {
        EffectInstancePtr in = skipDots( const_cast<Group3D*>(group)->getInput(i) );
        if (in && in.get() == child) return true;
    }
    return false;
}

// First Group3D downstream of `node` (following Dots) that has `child` on a
// child input. `depth` guards against pathological graphs.
static const Group3D*
findParentGroup(const NodePtr& node, const EffectInstance* child, int depth)
{
    if (!node || depth > 32) return NULL;
    const NodesWList& outs = node->getOutputs();
    for (NodesWList::const_iterator it = outs.begin(); it != outs.end(); ++it) {
        NodePtr out = it->lock();
        if (!out) continue;
        EffectInstancePtr eff = out->getEffectInstance();
        if (!eff) continue;
        if ( isGraphPassthrough( eff->getPluginID() ) ) {
            if ( const Group3D* g = findParentGroup(out, child, depth + 1) ) return g;
            continue;
        }
        const Group3D* g = dynamic_cast<const Group3D*>( eff.get() );
        if ( g && groupHasChild(g, child) ) return g;
    }
    return NULL;
}

bool
Group3D::getParentWorldMatrix(const EffectInstance* child, double time, float out[16])
{
    geoMat4Identity(out);
    if (!child) return false;
    const Group3D* g = findParentGroup(child->getNode(), child, 0);
    if (!g) return false;
    float local[16];
    g->getLocalMatrix(time, local, NULL);
    float above[16];
    if ( getParentWorldMatrix(g, time, above) ) {
        geoMat4Mul(out, above, local); // above * local: local applied first
    } else {
        std::memcpy(out, local, sizeof(local));
    }
    return true;
}

bool
Group3D::getChildrenBounds(double time, float bmin[3], float bmax[3]) const
{
    float ident[16];
    geoMat4Identity(ident);
    bool any = false;
    const int n = getNumChildInputs();
    for (int i = 0; i < n; ++i) {
        accumulateChildBounds(skipDots( getInput(i) ), ident, time, bmin, bmax, any);
    }
    return any;
}

bool
Group3D::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/,
                     double time, bool /*originatedFromMainThread*/)
{
    if (!k) return false;
    const bool toCentroid = (k == _imp->movePivotToCentroid.lock().get());
    const bool toOrigin   = (k == _imp->moveCentroidToOrigin.lock().get());
    if (!toCentroid && !toOrigin) return false;

    float bmin[3], bmax[3];
    if ( !getChildrenBounds(time, bmin, bmax) ) {
        setPersistentMessage(eMessageTypeWarning,
                             tr("No geometry connected below this node — nothing to measure.").toStdString());
        return true;
    }
    clearPersistentMessage(false);
    const double c[3] = { 0.5 * (bmin[0] + bmax[0]), 0.5 * (bmin[1] + bmax[1]), 0.5 * (bmin[2] + bmax[2]) };

    beginChanges();
    _imp->pivotX.lock()->setValue(c[0]);
    _imp->pivotY.lock()->setValue(c[1]);
    _imp->pivotZ.lock()->setValue(c[2]);
    if (toOrigin) {
        // Centre to the origin: with pivot = c, a point c maps to c + t, so t = -c.
        _imp->translateX.lock()->setValue(-c[0]);
        _imp->translateY.lock()->setValue(-c[1]);
        _imp->translateZ.lock()->setValue(-c[2]);
        if ( !_imp->enableTransform.lock()->getValue() ) {
            _imp->enableTransform.lock()->setValue(true);
        }
    }
    endChanges();
    return true;
}

bool
Group3D::isNodeInGroup(const std::string& nodeName) const
{
    const int nChildren = getNumChildInputs();
    for (int i = 0; i < nChildren; ++i) {
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
    // If any connected input (children or the axis) has animated knobs, this
    // group is frame-varying.
    const int nInputs = getNInputs();
    for (int i = 0; i < nInputs; ++i) {
        EffectInstancePtr inp = skipDots(getInput(i));
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
    const int nChildren = getNumChildInputs();
    for (int i = 0; i < nChildren; ++i) {
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
    if ( EffectInstancePtr axis = skipDots( getInput( getAxisInputIndex() ) ) ) {
        ss << "  axis: " << axis->getNode()->getScriptName();
    }
    _imp->info.lock()->setValue(ss.str());

    // A group produces no image. Write a black placeholder: an untouched
    // output buffer is uninitialised memory and the viewer flags it as NaN
    // ("rendered rectangle (0,0)-(1,1) contains NaN values").
    if (args.outputPlanes.empty()) return eStatusOK;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;
    const RectI bounds = outImg->getBounds();
    Image::WriteAccess wa(outImg.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* pix = (float*)wa.pixelAt(x, y);
            if (pix) { pix[0] = pix[1] = pix[2] = pix[3] = 0.0f; }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Group3D.cpp"
