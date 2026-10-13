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

#ifndef NATRON_ENGINE_SUBDIVISIONPROVIDER_H
#define NATRON_ENGINE_SUBDIVISIONPROVIDER_H

#include "../../AppManager.h"
#include "../../EffectInstance.h"
#include "../../KnobTypes.h"

#include <algorithm>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * Render-time subdivision of a mesh node, consumed by CyclesRenderer only.
 *
 * With it on, the node's polygons are handed to Cycles as SUBDIVISION faces
 * (Mesh::add_subd_face) instead of fixed triangles, and Cycles dices them
 * adaptively against the render camera: every edge is split until it covers
 * at most Dicing Rate pixels on screen, capped at Max Level splits. That is
 * what gives a 2-triangle Card3D or a coarse scan enough vertices for true
 * displacement (Material3D > Displacement Method) to carve real relief.
 *
 * Render-only: the 3D viewport and ScanlineRender keep drawing the base mesh.
 * The geometry node's own hash covers these knobs, so CyclesRender's scene
 * hash sees a change without extra plumbing.
 */
struct SubdivisionParams
{
    bool   enabled    = false;
    int    type       = 0;     // 0 = Linear (just adds vertices), 1 = Catmull-Clark (also smooths)
    double dicingRate = 1.0;   // target edge length in PIXELS at render size (before the global scale)
    int    maxLevel   = 6;     // cap on the number of edge splits
};

class SubdivisionProvider
{
public:
    virtual ~SubdivisionProvider() {}

    /** Fill `out` from the node's Subdivision knobs. Returns false when the
     *  node has no such knobs (the renderer then uses plain triangles). */
    virtual bool getSubdivisionParams(double time, SubdivisionParams* out) const = 0;
};

/** The knob set every mesh node creates. Weak pointers, like the nodes' other
 *  knob storage. */
struct SubdivisionKnobs
{
    KnobGroupWPtr  group;
    KnobBoolWPtr   enabled;
    KnobChoiceWPtr type;
    KnobDoubleWPtr dicingRate;
    KnobIntWPtr    maxLevel;
};

/** Create the "Subdivision (Cycles)" group on `page`. Call from initializeKnobs. */
inline void
createSubdivisionKnobs(EffectInstance* effect, const KnobPagePtr& page, SubdivisionKnobs& out)
{
    KnobGroupPtr grp = AppManager::createKnob<KnobGroup>(effect, EffectInstance::tr("Subdivision (Cycles)"));
    grp->setName("subdivisionGroup");
    grp->setDefaultValue(false); // collapsed
    grp->setHintToolTip(EffectInstance::tr(
        "Render-time adaptive subdivision in CyclesRender. Dices this mesh "
        "against the render camera so displacement maps can move real geometry. "
        "The 3D viewport and ScanlineRender draw the base mesh regardless."));
    page->addKnob(grp);
    out.group = grp;

    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(effect, EffectInstance::tr("Subdivide"));
        k->setName("subdivide");
        k->setDefaultValue(false);
        k->setHintToolTip(EffectInstance::tr(
            "Hand this mesh to Cycles as subdivision faces and dice it adaptively "
            "at render time. Needed for a Card3D or a coarse mesh to show true "
            "displacement; dense scans usually do not need it."));
        grp->addKnob(k);
        out.enabled = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(effect, EffectInstance::tr("Type"));
        k->setName("subdivisionType");
        std::vector<ChoiceOption> entries;
        entries.push_back( ChoiceOption("linear", "Linear",
                                        "Only adds vertices; the shape is unchanged. The safe choice "
                                        "for triangulated meshes (Megascans exports) and for cards.") );
        entries.push_back( ChoiceOption("catmull-clark", "Catmull-Clark",
                                        "Adds vertices AND smooths the surface (rounds a cube, softens "
                                        "a quad cage). Shrinks and ripples triangle-only meshes.") );
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setHintToolTip(EffectInstance::tr("Linear keeps the shape and just adds vertices. "
                                             "Catmull-Clark smooths like a subdivision-surface modifier."));
        grp->addKnob(k);
        out.type = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(effect, EffectInstance::tr("Dicing Rate (px)"));
        k->setName("dicingRate");
        k->setDefaultValue(1.0);
        k->setMinimum(0.1); k->setMaximum(1000.0);
        k->setDisplayMinimum(0.1); k->setDisplayMaximum(16.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(EffectInstance::tr(
            "Target edge length in pixels at render size: edges are split until "
            "they are at most this long on screen, so close geometry gets fine and "
            "distant geometry stays coarse. 1 = one polygon per pixel (final "
            "quality); 4-8 is plenty for look-dev. Multiplied by CyclesRender's "
            "Dicing Scale (preview renders use its Preview Dicing Scale)."));
        grp->addKnob(k);
        out.dicingRate = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(effect, EffectInstance::tr("Max Level"));
        k->setName("maxSubdivisionLevel");
        k->setDefaultValue(6);
        k->setMinimum(1); k->setMaximum(12);
        k->setDisplayMinimum(1); k->setDisplayMaximum(10);
        k->setHintToolTip(EffectInstance::tr(
            "Cap on how many times an edge may be split (each level quadruples "
            "the face count). Keeps memory bounded when the object fills the "
            "frame; 6 is 4096 faces per base face."));
        grp->addKnob(k);
        out.maxLevel = k;
    }
}

/** Read the knobs created above. Returns false when the group was never
 *  created (null weak pointers). */
inline bool
readSubdivisionKnobs(const SubdivisionKnobs& knobs, double time, SubdivisionParams* out)
{
    KnobBoolPtr en = knobs.enabled.lock();
    if (!en || !out) {
        return false;
    }
    out->enabled = en->getValue();
    if (KnobChoicePtr t = knobs.type.lock()) {
        // id-first ChoiceOption: match the stored id, not the index
        out->type = (t->getActiveEntry().id == "catmull-clark") ? 1 : 0;
    }
    if (KnobDoublePtr r = knobs.dicingRate.lock()) {
        out->dicingRate = std::max(0.1, r->getValueAtTime(time));
    }
    if (KnobIntPtr l = knobs.maxLevel.lock()) {
        out->maxLevel = std::max(1, std::min(12, l->getValue()));
    }
    return true;
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_SUBDIVISIONPROVIDER_H
