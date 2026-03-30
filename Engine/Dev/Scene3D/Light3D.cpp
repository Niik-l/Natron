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
#include "../../KnobFile.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct Light3DPrivate
{
    KnobChoiceWPtr lightType;
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobColorWPtr color;
    KnobDoubleWPtr intensity;
    KnobDoubleWPtr exposure;
    KnobDoubleWPtr spotAngle;
    KnobDoubleWPtr spotSmooth;
    KnobDoubleWPtr areaSizeU, areaSizeV;
    KnobDoubleWPtr spread;
    KnobFileWPtr environmentMap;
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
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate X"));
        k->setName("rotateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        k->setHintToolTip(tr("Rotation around X axis in degrees. Used for Spot and Area lights to aim them."));
        xformPage->addKnob(k); _imp->rotateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y"));
        k->setName("rotateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z"));
        k->setName("rotateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateZ = k;
    }

    KnobPagePtr lightPage = AppManager::createKnob<KnobPage>(this, tr("Light"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Light Type"));
        k->setName("lightType");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Point", "", "Omnidirectional point light"));
        entries.push_back(ChoiceOption("Distant", "", "Directional sun light (parallel rays)"));
        entries.push_back(ChoiceOption("Spot", "", "Cone-shaped spotlight"));
        entries.push_back(ChoiceOption("Area", "", "Rectangular area light (soft shadows)"));
        entries.push_back(ChoiceOption("Dome", "", "Environment dome light (HDRI)"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        lightPage->addKnob(k);
        _imp->lightType = k;
    }
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Light Color"), 3);
        k->setName("lightColor");
        k->setDefaultValue(1.0, 0);
        k->setDefaultValue(0.95, 1);
        k->setDefaultValue(0.9, 2);
        k->setAnimationEnabled(true);
        lightPage->addKnob(k);
        _imp->color = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Intensity"));
        k->setName("intensity"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(10.0);
        lightPage->addKnob(k); _imp->intensity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Exposure"));
        k->setName("exposure"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-5.0); k->setDisplayMaximum(5.0);
        k->setHintToolTip(tr("Exposure adjustment in stops. 0 = no change, 1 = 2x brighter, -1 = half."));
        lightPage->addKnob(k); _imp->exposure = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Spot Angle"));
        k->setName("spotAngle"); k->setDefaultValue(45.0); k->setAnimationEnabled(true);
        k->setMinimum(1.0); k->setMaximum(180.0);
        k->setDisplayMinimum(1.0); k->setDisplayMaximum(180.0);
        k->setHintToolTip(tr("Spot light cone angle in degrees. Only used for Spot light type."));
        lightPage->addKnob(k); _imp->spotAngle = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Spot Smooth"));
        k->setName("spotSmooth"); k->setDefaultValue(0.15); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Softness of spot light edge falloff. 0 = hard edge, 1 = fully soft."));
        lightPage->addKnob(k); _imp->spotSmooth = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Area Width"));
        k->setName("areaSizeU"); k->setDefaultValue(2.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(20.0);
        k->setHintToolTip(tr("Width of area light rectangle. Only used for Area light type."));
        lightPage->addKnob(k); _imp->areaSizeU = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Area Height"));
        k->setName("areaSizeV"); k->setDefaultValue(2.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(20.0);
        k->setHintToolTip(tr("Height of area light rectangle. Only used for Area light type."));
        lightPage->addKnob(k); _imp->areaSizeV = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Spread"));
        k->setName("spread"); k->setDefaultValue(180.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(180.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(180.0);
        k->setHintToolTip(tr("Spread angle for area light falloff in degrees. 180 = full hemisphere, smaller = more focused."));
        lightPage->addKnob(k); _imp->spread = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Environment Map"));
        k->setName("environmentMap");
        k->setHintToolTip(tr("HDR/EXR image for Dome light. Leave empty for solid color.\nSupports .exr, .hdr, .png, .jpg"));
        lightPage->addKnob(k); _imp->environmentMap = k;
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

    // Set initial visibility based on default light type (Point)
    updateKnobVisibility();
}

void
Light3D::getLightParams(double time,
                        double& tx, double& ty, double& tz,
                        double& r, double& g, double& b,
                        double& intensity, double& exposure) const
{
    tx = _imp->translateX.lock()->getValueAtTime(time);
    ty = _imp->translateY.lock()->getValueAtTime(time);
    tz = _imp->translateZ.lock()->getValueAtTime(time);
    KnobColorPtr col = _imp->color.lock();
    if (col) {
        r = col->getValueAtTime(time, 0);
        g = col->getValueAtTime(time, 1);
        b = col->getValueAtTime(time, 2);
    } else {
        r = 1.0; g = 1.0; b = 1.0;
    }
    intensity = _imp->intensity.lock()->getValueAtTime(time);
    KnobDoublePtr expKnob = _imp->exposure.lock();
    exposure = expKnob ? expKnob->getValueAtTime(time) : 0.0;
}

std::string
Light3D::getEnvironmentMap() const
{
    KnobFilePtr k = _imp->environmentMap.lock();
    return k ? k->getValue() : std::string();
}

double Light3D::getSpotAngle(double time) const
{ KnobDoublePtr k = _imp->spotAngle.lock(); return k ? k->getValueAtTime(time) : 45.0; }

double Light3D::getSpotSmooth(double time) const
{ KnobDoublePtr k = _imp->spotSmooth.lock(); return k ? k->getValueAtTime(time) : 0.15; }

double Light3D::getAreaSizeU(double time) const
{ KnobDoublePtr k = _imp->areaSizeU.lock(); return k ? k->getValueAtTime(time) : 2.0; }

double Light3D::getAreaSizeV(double time) const
{ KnobDoublePtr k = _imp->areaSizeV.lock(); return k ? k->getValueAtTime(time) : 2.0; }

double Light3D::getSpread(double time) const
{ KnobDoublePtr k = _imp->spread.lock(); return k ? k->getValueAtTime(time) : 180.0; }

Light3D::LightType
Light3D::getLightType() const
{
    KnobChoicePtr k = _imp->lightType.lock();
    if (!k) return eLightPoint;
    return (LightType)k->getValue();
}

void
Light3D::updateKnobVisibility()
{
    KnobChoicePtr ltKnob = _imp->lightType.lock();
    if (!ltKnob) return;
    LightType lt = (LightType)ltKnob->getValue();

    bool isSpot = (lt == eLightSpot);
    bool isArea = (lt == eLightArea);
    bool isDome = (lt == eLightDome);
    bool needsRotation = (lt == eLightSpot || lt == eLightArea || lt == eLightDistant);

    // Rotation knobs — only for directional lights
    if (KnobDoublePtr k = _imp->rotateX.lock()) k->setSecret(!needsRotation);
    if (KnobDoublePtr k = _imp->rotateY.lock()) k->setSecret(!needsRotation);
    if (KnobDoublePtr k = _imp->rotateZ.lock()) k->setSecret(!needsRotation);

    // Spot-only knobs
    if (KnobDoublePtr k = _imp->spotAngle.lock()) k->setSecret(!isSpot);
    if (KnobDoublePtr k = _imp->spotSmooth.lock()) k->setSecret(!isSpot);

    // Area-only knobs
    if (KnobDoublePtr k = _imp->areaSizeU.lock()) k->setSecret(!isArea);
    if (KnobDoublePtr k = _imp->areaSizeV.lock()) k->setSecret(!isArea);
    if (KnobDoublePtr k = _imp->spread.lock()) k->setSecret(!isArea);

    // Dome-only knobs
    if (KnobFilePtr k = _imp->environmentMap.lock()) k->setSecret(!isDome);
}

bool
Light3D::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/,
                      double /*time*/, bool /*originatedFromMainThread*/)
{
    KnobChoicePtr ltKnob = _imp->lightType.lock();
    if (ltKnob && k == ltKnob.get()) {
        updateKnobVisibility();
        return true;
    }
    return false;
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
