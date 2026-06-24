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

#include "MergeMat.h"

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Image.h"
#include "../../Node.h"
#include "../DotUtils.h"

NATRON_NAMESPACE_ENTER

struct MergeMatPrivate
{
    KnobChoiceWPtr operation;   // Operation enum
    KnobDoubleWPtr mix;         // 0..1 opacity of the foreground (A)
};

MergeMat::MergeMat(NodePtr node)
    : EffectInstance(node)
    , _imp(new MergeMatPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

MergeMat::~MergeMat()
{
}

std::string
MergeMat::getPluginDescription() const
{
    return tr("Combine two materials, like Nuke's MergeMat (classic 3D).\n\n"
              "Composites a foreground material (input A) over a background material (input B) "
              "using a Merge-style operation, and outputs a single material that plugs into a "
              "geometry node's material ('mat') input.\n\n"
              "Its main use is layering multiple Project3D projections onto one piece of "
              "geometry — e.g. a front-camera projection 'over' a side-camera projection. "
              "MergeMats chain (feed one into another's A or B input) to stack any number of "
              "projections, like classic Nuke.\n\n"
              "Operations: none (B only), replace (A only), over (A over B, default), stencil "
              "(B outside A's alpha), mask (B inside A's alpha), plus, max, min.\n\n"
              "The layered projection is composited per-fragment by ScanlineRender (and "
              "approximated live in the 3D viewport).").toStdString();
}

std::string
MergeMat::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "A";   // foreground material
        case 1: return "B";   // background material
        default: return std::string();
    }
}

bool
MergeMat::isInputOptional(int /*inputNb*/) const
{
    // Both optional so the node never errors in the graph; it simply passes through
    // whichever input is connected until both are.
    return true;
}

void
MergeMat::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
}

void
MergeMat::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
MergeMat::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
MergeMat::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("MergeMat"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Operation"));
        k->setName("operation");
        k->setHintToolTip(tr("How the foreground material (A) combines with the background (B):\n"
                             "none: B only.\n"
                             "replace: A only.\n"
                             "over: A composited over B by A's alpha (default).\n"
                             "stencil: B shown outside A's alpha.\n"
                             "mask: B shown inside A's alpha.\n"
                             "plus / max / min: arithmetic combine."));
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("none", "", "B only"));
        entries.push_back(ChoiceOption("replace", "", "A only"));
        entries.push_back(ChoiceOption("over", "", "A over B (by A's alpha)"));
        entries.push_back(ChoiceOption("stencil", "", "B outside A's alpha"));
        entries.push_back(ChoiceOption("mask", "", "B inside A's alpha"));
        entries.push_back(ChoiceOption("plus", "", "A + B"));
        entries.push_back(ChoiceOption("max", "", "max(A, B)"));
        entries.push_back(ChoiceOption("min", "", "min(A, B)"));
        k->populateChoices(entries);
        k->setDefaultValue(eMergeOver);
        k->setAnimationEnabled(false);
        page->addKnob(k);
        _imp->operation = k;
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
        k->setName("mix");
        k->setHintToolTip(tr("Opacity of the foreground material (A) in the merge. At 0 the "
                             "output is just the background (B); at 1 the operation is applied "
                             "at full strength."));
        k->setDefaultValue(1.0);
        k->setMinimum(0.0);
        k->setMaximum(1.0);
        k->setDisplayMinimum(0.0);
        k->setDisplayMaximum(1.0);
        page->addKnob(k);
        _imp->mix = k;
    }
}

// ---- MaterialProvider: delegate to the foreground (A) material, else background (B). ----

MaterialProvider*
MergeMat::getInputMaterial(int ab) const
{
    EffectInstancePtr inp = skipDots(getInput(ab == 0 ? 0 : 1));
    return inp ? dynamic_cast<MaterialProvider*>(inp.get()) : NULL;
}

namespace {
// The material MergeMat reports for single-material consumers (Cycles): foreground A, else
// background B. NULL → neutral defaults handled by each accessor below.
const MaterialProvider*
representativeMaterial(const MergeMat* self)
{
    if (MaterialProvider* a = self->getInputMaterial(0)) return a;
    if (MaterialProvider* b = self->getInputMaterial(1)) return b;
    return NULL;
}
} // namespace

void
MergeMat::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    if (const MaterialProvider* m = representativeMaterial(this)) { m->getMaterialBaseColor(time, r, g, b); return; }
    r = g = b = 1.0;
}

double MergeMat::getMaterialRoughness(double time) const
{ const MaterialProvider* m = representativeMaterial(this); return m ? m->getMaterialRoughness(time) : 0.5; }

double MergeMat::getMaterialMetallic(double time) const
{ const MaterialProvider* m = representativeMaterial(this); return m ? m->getMaterialMetallic(time) : 0.0; }

double MergeMat::getMaterialSpecular(double time) const
{ const MaterialProvider* m = representativeMaterial(this); return m ? m->getMaterialSpecular(time) : 0.5; }

void
MergeMat::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    if (const MaterialProvider* m = representativeMaterial(this)) { m->getMaterialEmission(time, r, g, b, strength); return; }
    r = g = b = 0.0; strength = 0.0;
}

double MergeMat::getMaterialTransmission(double time) const
{ const MaterialProvider* m = representativeMaterial(this); return m ? m->getMaterialTransmission(time) : 0.0; }

double MergeMat::getMaterialIOR(double time) const
{ const MaterialProvider* m = representativeMaterial(this); return m ? m->getMaterialIOR(time) : 1.45; }

// ---- MergeMat parameters ----

int
MergeMat::getOperation(double time) const
{
    KnobChoicePtr k = _imp->operation.lock();
    return k ? k->getValueAtTime(time) : (int)eMergeOver;
}

double
MergeMat::getMix(double time) const
{
    KnobDoublePtr k = _imp->mix.lock();
    return k ? k->getValueAtTime(time) : 1.0;
}

// ---- EffectInstance plumbing (a material node renders nothing on its own) ----

StatusEnum
MergeMat::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
MergeMat::render(const RenderActionArgs& args)
{
    if (args.outputPlanes.empty()) return eStatusOK;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;

    RectI bounds = outImg->getBounds();
    Image::WriteAccess wa(outImg.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* pix = (float*)wa.pixelAt(x, y);
            if (pix) { pix[0] = pix[1] = pix[2] = pix[3] = 0.f; }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_MergeMat.cpp"
