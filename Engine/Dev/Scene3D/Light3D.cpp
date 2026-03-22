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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Light3D.h"

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct Light3DPrivate
{
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr colorR, colorG, colorB;
    KnobDoubleWPtr intensity;
    KnobDoubleWPtr shadowDensity;
    KnobIntWPtr shadowSteps;
};

Light3D::Light3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Light3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Light3D::~Light3D()
{
}

std::string
Light3D::getPluginDescription() const
{
    return tr("3D point light for illuminating volumes and geometry.\n\n"
              "Connect to ScanlineRender's obj/scn input via a Scene node, "
              "or wire alongside volume/geometry nodes.\n\n"
              "For volume lighting: ScanlineRender will automatically detect "
              "Light3D nodes in the Scene and use them for volume self-shadowing.").toStdString();
}

void
Light3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Light3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Light3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Light3D::initializeKnobs()
{
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
        k->setName("translateX"); k->setDefaultValue(3.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
        k->setName("translateY"); k->setDefaultValue(5.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
        k->setName("translateZ"); k->setDefaultValue(3.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateZ = k;
    }

    KnobPagePtr lightPage = AppManager::createKnob<KnobPage>(this, tr("Light"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color R"));
        k->setName("colorR"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        lightPage->addKnob(k); _imp->colorR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color G"));
        k->setName("colorG"); k->setDefaultValue(0.95); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        lightPage->addKnob(k); _imp->colorG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color B"));
        k->setName("colorB"); k->setDefaultValue(0.9); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        lightPage->addKnob(k); _imp->colorB = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Intensity"));
        k->setName("intensity"); k->setDefaultValue(2.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(10.0);
        lightPage->addKnob(k); _imp->intensity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Shadow Density"));
        k->setName("shadowDensity"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(5.0);
        lightPage->addKnob(k); _imp->shadowDensity = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Shadow Steps"));
        k->setName("shadowSteps"); k->setDefaultValue(16);
        k->setMinimum(1); k->setDisplayMinimum(4); k->setDisplayMaximum(64);
        lightPage->addKnob(k); _imp->shadowSteps = k;
    }
}

void
Light3D::getLightParams(double time,
                        double& tx, double& ty, double& tz,
                        double& r, double& g, double& b,
                        double& intensity) const
{
    tx = _imp->translateX.lock()->getValueAtTime(time);
    ty = _imp->translateY.lock()->getValueAtTime(time);
    tz = _imp->translateZ.lock()->getValueAtTime(time);
    r = _imp->colorR.lock()->getValueAtTime(time);
    g = _imp->colorG.lock()->getValueAtTime(time);
    b = _imp->colorB.lock()->getValueAtTime(time);
    intensity = _imp->intensity.lock()->getValueAtTime(time);
}

StatusEnum
Light3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                               ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Light3D::render(const RenderActionArgs& args)
{
    if (args.outputPlanes.empty()) return eStatusOK;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;

    RectI bounds = outImg->getBounds();
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

#include "moc_Light3D.cpp"
