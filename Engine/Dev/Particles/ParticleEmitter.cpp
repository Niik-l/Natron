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

#include "ParticleEmitter.h"
#include "../DotUtils.h"

#include <cmath>
#include <random>
#include <algorithm>
#include <vector>

#include "../Scene3D/Card3D.h"
#include "../Scene3D/RotationConventions.h"

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../NodeMetadata.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../TimeLine.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct ParticleEmitterPrivate
{
    // Emission
    KnobIntWPtr rate;
    KnobDoubleWPtr lifetime;
    KnobDoubleWPtr lifetimeVariance;
    KnobDoubleWPtr velocity;
    KnobDoubleWPtr velocityVariance;
    KnobDoubleWPtr spread;
    KnobDoubleWPtr startSize;
    KnobDoubleWPtr sizeVariance;
    KnobIntWPtr seed;
    KnobChoiceWPtr emitterShape;
    KnobDoubleWPtr shapeSize;

    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr emitDirX, emitDirY, emitDirZ;

    // Color
    KnobDoubleWPtr startColorR, startColorG, startColorB, startColorA;
    KnobDoubleWPtr colorVariance;

    // Image Mask
    KnobDoubleWPtr maskThreshold;
    KnobDoubleWPtr maskPlaneScale;
    KnobChoiceWPtr maskPlaneOrientation;
    KnobBoolWPtr maskColorFromImage;

    // Age-based appearance
    KnobDoubleWPtr endSize;
    KnobDoubleWPtr endColorR, endColorG, endColorB;
    KnobDoubleWPtr fadeIn, fadeOut;
};


ParticleEmitter::ParticleEmitter(NodePtr node)
    : EffectInstance(node)
    , _imp(new ParticleEmitterPrivate())
    , _lastSimFrame(-1e9)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ParticleEmitter::~ParticleEmitter()
{
}

std::string
ParticleEmitter::getPluginDescription() const
{
    return tr("Particle emitter — spawns particles each frame and simulates them forward.\n\n"
              "Optional mask input: connect an image to use as an emission mask.\n"
              "Spawns particles with randomized velocity within an emission cone.\n"
              "Connect downstream to ParticleForce, ParticleMerge, or ParticleRender nodes.\n\n"
              "Comparable to particle-emitter nodes found in other compositing DCCs.").toStdString();
}

std::string
ParticleEmitter::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "mask";
    if (inputNb == 1) return "transform";
    return "";
}

void
ParticleEmitter::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ParticleEmitter::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ParticleEmitter::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ==================== Knobs ====================

void
ParticleEmitter::initializeKnobs()
{
    // Emission page
    KnobPagePtr emissionPage = AppManager::createKnob<KnobPage>(this, tr("Emission"));

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Rate"));
        k->setName("rate"); k->setDefaultValue(100);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(10000);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->rate = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Lifetime"));
        k->setName("lifetime"); k->setDefaultValue(50.0);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(500.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->lifetime = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Lifetime Variance"));
        k->setName("lifetimeVariance"); k->setDefaultValue(10.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->lifetimeVariance = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Velocity"));
        k->setName("velocity"); k->setDefaultValue(0.5);
        k->setMinimum(0.001); k->setDisplayMinimum(0.001); k->setDisplayMaximum(5.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->velocity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Velocity Variance"));
        k->setName("velocityVariance"); k->setDefaultValue(0.1);
        k->setMinimum(0.001); k->setDisplayMinimum(0.001); k->setDisplayMaximum(5.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->velocityVariance = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Spread (degrees)"));
        k->setName("spread"); k->setDefaultValue(30.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(180.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->spread = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start Size"));
        k->setName("startSize"); k->setDefaultValue(0.1);
        k->setMinimum(0.001); k->setDisplayMinimum(0.001); k->setDisplayMaximum(10.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->startSize = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size Variance"));
        k->setName("sizeVariance"); k->setDefaultValue(0.02);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->sizeVariance = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Seed"));
        k->setName("seed"); k->setDefaultValue(0);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(10000);
        k->setAnimationEnabled(false);
        emissionPage->addKnob(k); _imp->seed = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Emitter Shape"));
        k->setName("emitterShape");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Point", "", "Emit from a single point"));
        entries.push_back(ChoiceOption("Sphere", "", "Emit from random positions within a sphere"));
        entries.push_back(ChoiceOption("Box", "", "Emit from random positions within a box"));
        entries.push_back(ChoiceOption("Disc", "", "Emit from random positions on a flat disc (XZ plane)"));
        entries.push_back(ChoiceOption("ImageMask", "", "Emit from bright regions of a connected image mask"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Shape of the emission region."));
        emissionPage->addKnob(k); _imp->emitterShape = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Shape Size"));
        k->setName("shapeSize"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Radius (Sphere/Disc) or half-extent (Box) of the emitter shape."));
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->shapeSize = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Mask Threshold"));
        k->setName("maskThreshold"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Minimum pixel brightness to allow emission (Image Mask mode)."));
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->maskThreshold = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Plane Scale"));
        k->setName("maskPlaneScale"); k->setDefaultValue(10.0);
        k->setMinimum(0.1); k->setDisplayMinimum(0.1); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Size of the emission plane in world units (Image Mask mode)."));
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->maskPlaneScale = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Plane Orientation"));
        k->setName("maskPlaneOrientation");
        std::vector<ChoiceOption> orientEntries;
        orientEntries.push_back(ChoiceOption("XY", "", "Map image onto XY plane (Z=0)"));
        orientEntries.push_back(ChoiceOption("XZ", "", "Map image onto XZ plane (Y=0)"));
        orientEntries.push_back(ChoiceOption("YZ", "", "Map image onto YZ plane (X=0)"));
        k->populateChoices(orientEntries);
        k->setDefaultValue(1); // XZ default
        k->setHintToolTip(tr("Which plane to map image pixels onto (Image Mask mode)."));
        emissionPage->addKnob(k); _imp->maskPlaneOrientation = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Color From Image"));
        k->setName("maskColorFromImage"); k->setDefaultValue(true);
        k->setHintToolTip(tr("When enabled, particle color is set from the image pixel color (Image Mask mode)."));
        emissionPage->addKnob(k); _imp->maskColorFromImage = k;
    }

    // Transform page
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
        k->setName("translateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
        k->setName("translateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
        k->setName("translateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Emit Direction X"));
        k->setName("emitDirX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        xformPage->addKnob(k); _imp->emitDirX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Emit Direction Y"));
        k->setName("emitDirY"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        xformPage->addKnob(k); _imp->emitDirY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Emit Direction Z"));
        k->setName("emitDirZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        xformPage->addKnob(k); _imp->emitDirZ = k;
    }

    // Color page
    KnobPagePtr colorPage = AppManager::createKnob<KnobPage>(this, tr("Color"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start Color R"));
        k->setName("startColorR"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        colorPage->addKnob(k); _imp->startColorR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start Color G"));
        k->setName("startColorG"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        colorPage->addKnob(k); _imp->startColorG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start Color B"));
        k->setName("startColorB"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        colorPage->addKnob(k); _imp->startColorB = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start Color A"));
        k->setName("startColorA"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        colorPage->addKnob(k); _imp->startColorA = k;
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color Variance"));
        k->setName("colorVariance"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Per-particle random color variation. 0 = all same color, 1 = fully random per particle. Uses particle ID as seed."));
        k->setAnimationEnabled(true);
        colorPage->addKnob(k); _imp->colorVariance = k;
    }

    // Over Life page (age-based appearance)
    KnobPagePtr lifePage = AppManager::createKnob<KnobPage>(this, tr("Over Life"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("End Size"));
        k->setName("endSize"); k->setDefaultValue(0.3); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        lifePage->addKnob(k); _imp->endSize = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("End Color R"));
        k->setName("endColorR"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        lifePage->addKnob(k); _imp->endColorR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("End Color G"));
        k->setName("endColorG"); k->setDefaultValue(0.3); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        lifePage->addKnob(k); _imp->endColorG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("End Color B"));
        k->setName("endColorB"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        lifePage->addKnob(k); _imp->endColorB = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Fade In"));
        k->setName("fadeIn"); k->setDefaultValue(0.1); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(QString::fromUtf8("Fraction of life for fade-in (0-1). 0.1 = first 10% of life."));
        lifePage->addKnob(k); _imp->fadeIn = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Fade Out"));
        k->setName("fadeOut"); k->setDefaultValue(0.3); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(QString::fromUtf8("Fraction of life for fade-out (0-1). 0.3 = last 30% of life."));
        lifePage->addKnob(k); _imp->fadeOut = k;
    }
}

// ==================== Particle Simulation ====================

/**
 * @brief Build an orthonormal basis from a direction vector.
 *
 * Given a normalized direction `dir`, produces two perpendicular vectors `tangent` and `bitangent`
 * such that (tangent, bitangent, dir) forms a right-handed orthonormal basis.
 */
static void
buildBasisFromDirection(float dx, float dy, float dz,
                        float& tx, float& ty, float& tz,
                        float& bx, float& by, float& bz)
{
    // Pick a vector not parallel to dir
    float ax = 0.0f, ay = 0.0f, az = 1.0f;
    if (std::fabs(dz) > 0.9f) {
        ax = 1.0f; ay = 0.0f; az = 0.0f;
    }

    // tangent = cross(dir, a)
    tx = dy * az - dz * ay;
    ty = dz * ax - dx * az;
    tz = dx * ay - dy * ax;
    float tLen = std::sqrt(tx * tx + ty * ty + tz * tz);
    if (tLen > 1e-8f) { tx /= tLen; ty /= tLen; tz /= tLen; }

    // bitangent = cross(dir, tangent)
    bx = dy * tz - dz * ty;
    by = dz * tx - dx * tz;
    bz = dx * ty - dy * tx;
    float bLen = std::sqrt(bx * bx + by * by + bz * bz);
    if (bLen > 1e-8f) { bx /= bLen; by /= bLen; bz /= bLen; }
}

StatusEnum
ParticleEmitter::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setIsFrameVarying(true);
    return eStatusOK;
}

ParticleDataPtr
ParticleEmitter::getParticleData(double time)
{
    // Serialise against concurrent callers (3D viewport paint on the GUI
    // thread vs render workers, or two solvers sharing this emitter). The
    // published snapshot is never mutated after storage, so the cache hit
    // below can hand out the shared pointer directly.
    std::lock_guard<std::mutex> computeLk(_computeMutex);

    // Return cached result if already simulated to this frame
    if (_lastParticleData && time == _lastSimFrame) {
        return _lastParticleData;
    }

    // Incremental simulation: if we have cached data and moving forward,
    // continue from cached state instead of re-simulating from frame 1
    int startFrame = 1;
    ParticleDataPtr data;
    if (_lastParticleData && _lastSimFrame > 0 && time > _lastSimFrame) {
        startFrame = (int)_lastSimFrame + 1;
        data = std::make_shared<ParticleData>();
        data->particles = _lastParticleData->particles; // copy current state
    }

    // Read knob values at current time (most are constant but support animation)
    int rateVal           = _imp->rate.lock()->getValueAtTime(time);
    double lifetimeVal    = _imp->lifetime.lock()->getValueAtTime(time);
    double lifeVarVal     = _imp->lifetimeVariance.lock()->getValueAtTime(time);
    double velocityVal    = _imp->velocity.lock()->getValueAtTime(time);
    double velVarVal      = _imp->velocityVariance.lock()->getValueAtTime(time);
    double spreadVal      = _imp->spread.lock()->getValueAtTime(time);
    double startSizeVal   = _imp->startSize.lock()->getValueAtTime(time);
    double sizeVarVal     = _imp->sizeVariance.lock()->getValueAtTime(time);
    int seedVal           = _imp->seed.lock()->getValueAtTime(time);

    double emPosX = _imp->translateX.lock()->getValueAtTime(time);
    double emPosY = _imp->translateY.lock()->getValueAtTime(time);
    double emPosZ = _imp->translateZ.lock()->getValueAtTime(time);

    int shapeType = _imp->emitterShape.lock() ? _imp->emitterShape.lock()->getValue() : 0;
    float shapeSize = _imp->shapeSize.lock() ? (float)_imp->shapeSize.lock()->getValueAtTime(time) : 1.0f;

    double edx = _imp->emitDirX.lock()->getValueAtTime(time);
    double edy = _imp->emitDirY.lock()->getValueAtTime(time);
    double edz = _imp->emitDirZ.lock()->getValueAtTime(time);

    // Override position + emit direction from transform input (input 1).
    // Each lookup is guarded so a non-KnobDouble knob with a matching name
    // (KnobChoice / KnobInt etc.) doesn't null-deref.
    EffectInstancePtr xformInput = getInput(1);
    if (xformInput) {
        auto readDouble = [&](const char* name, double& out) {
            KnobIPtr k = xformInput->getKnobByName(name);
            if (!k) return;
            if (KnobDouble* kd = dynamic_cast<KnobDouble*>(k.get())) {
                out = kd->getValueAtTime(time);
            }
        };
        readDouble("translateX", emPosX);
        readDouble("translateY", emPosY);
        readDouble("translateZ", emPosZ);

        // Read rotation (degrees) and rotate emit direction by the parent's transform.
        double rxDeg = 0, ryDeg = 0, rzDeg = 0;
        readDouble("rotateX", rxDeg);
        readDouble("rotateY", ryDeg);
        readDouble("rotateZ", rzDeg);

        // Extrinsic XYZ matrix (M = Rz*Ry*Rx column-vector, Maya/Blender/Houdini default).
        double m[3][3];
        RotationConventions::compose(rxDeg, ryDeg, rzDeg, m);

        const double ldx = edx, ldy = edy, ldz = edz;
        edx = m[0][0]*ldx + m[0][1]*ldy + m[0][2]*ldz;
        edy = m[1][0]*ldx + m[1][1]*ldy + m[1][2]*ldz;
        edz = m[2][0]*ldx + m[2][1]*ldy + m[2][2]*ldz;
    }

    float colR = (float)_imp->startColorR.lock()->getValueAtTime(time);
    float colG = (float)_imp->startColorG.lock()->getValueAtTime(time);
    float colB = (float)_imp->startColorB.lock()->getValueAtTime(time);
    float colA = (float)_imp->startColorA.lock()->getValueAtTime(time);

    // Normalize emit direction
    double edLen = std::sqrt(edx * edx + edy * edy + edz * edz);
    if (edLen < 1e-8) { edx = 0; edy = 1; edz = 0; }
    else { edx /= edLen; edy /= edLen; edz /= edLen; }

    float spreadRad = (float)(spreadVal * M_PI / 180.0);

    // Build orthonormal basis from emit direction
    float fdx = (float)edx, fdy = (float)edy, fdz = (float)edz;
    float tangentX, tangentY, tangentZ;
    float bitangentX, bitangentY, bitangentZ;
    buildBasisFromDirection(fdx, fdy, fdz,
                            tangentX, tangentY, tangentZ,
                            bitangentX, bitangentY, bitangentZ);

    // Seeded RNG — use frame-based seed for deterministic results
    // even with incremental simulation
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    // Image Mask: fetch mask image once before the simulation loop
    ImagePtr maskImg;
    RectI maskBounds;
    int maskWidth = 0, maskHeight = 0;
    float maskThresholdVal = 0.5f;
    float maskPlaneScaleVal = 10.0f;
    int maskOrientVal = 1; // XZ
    bool maskColorFromImg = true;

    // Card3D Mask path (Scope A): if input 0 is a Card3D node, particles emit
    // from the Card3D's textured surface using its world transform. The
    // Card3D is visible in the 3D viewport so users see where particles are
    // emerging from. Replaces the legacy XY/YZ/XZ plane projection and the
    // emitter's own translateXYZ for the Card3D case.
    Card3D* card3dMask = nullptr;
    // Holds the snapshot alive for the whole sim — card3dTex points into it.
    Card3D::CachedTexturePtr card3dTexSnap;
    const Card3D::CachedTexture* card3dTex = nullptr;
    float card3dT[3] = { 0.f, 0.f, 0.f };
    double card3dRot[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    float card3dScale[3] = { 1.f, 1.f, 1.f };
    float card3dHalfW = 0.5f, card3dHalfH = 0.5f;

    if (shapeType == 4) {
        maskThresholdVal = (float)_imp->maskThreshold.lock()->getValueAtTime(time);
        maskPlaneScaleVal = (float)_imp->maskPlaneScale.lock()->getValueAtTime(time);
        maskOrientVal = _imp->maskPlaneOrientation.lock() ? _imp->maskPlaneOrientation.lock()->getValue() : 1;
        maskColorFromImg = _imp->maskColorFromImage.lock() ? _imp->maskColorFromImage.lock()->getValue() : true;

        // Detect Card3D upstream (through any Dots)
        EffectInstancePtr maskInput = skipDots(getInput(0));
        if (maskInput) {
            card3dMask = dynamic_cast<Card3D*>(maskInput.get());
        }

        if (card3dMask) {
            // Card3D mask source — pull its cached texture + transform.
            card3dMask->updateCachedTexture(time);
            card3dTexSnap = card3dMask->getCachedTexture();
            card3dTex = card3dTexSnap.get();

            auto readDouble = [&](const char* name, float& out) {
                KnobIPtr k = card3dMask->getKnobByName(name);
                if (!k) return;
                if (KnobDouble* kd = dynamic_cast<KnobDouble*>(k.get())) {
                    out = (float)kd->getValueAtTime(time);
                }
            };
            readDouble("translateX", card3dT[0]);
            readDouble("translateY", card3dT[1]);
            readDouble("translateZ", card3dT[2]);

            float rx = 0.f, ry = 0.f, rz = 0.f;
            readDouble("rotateX", rx);
            readDouble("rotateY", ry);
            readDouble("rotateZ", rz);
            RotationConventions::compose((double)rx, (double)ry, (double)rz, card3dRot);

            readDouble("scaleX", card3dScale[0]);
            readDouble("scaleY", card3dScale[1]);
            readDouble("scaleZ", card3dScale[2]);

            // Uniform Scale multiplies both axes on top of the per-axis Scale
            // (matches the drawn card; previously ignored here, so emission
            // from a uniformly-scaled card came out at the unscaled size).
            float card3dUniform = 1.f;
            readDouble("uniformScale", card3dUniform);
            card3dScale[0] *= card3dUniform;
            card3dScale[1] *= card3dUniform;

            // Card3D local plane spans (-halfW, -halfH, 0) to (+halfW, +halfH, 0).
            // halfW comes from the card's GEOMETRY aspect (getCardAspect — img
            // input aspect, honoring the Image Aspect toggle), matching
            // generateCardMesh exactly. The cached texture's resolution can
            // differ from the geometry aspect, so it is only used for sampling.
            card3dHalfW = card3dMask->getCardAspect(time) * 0.5f;
            card3dHalfH = 0.5f;

            // If the Card3D has no texture loaded, fall back to Point shape.
            if (card3dTex->pixels.empty()) {
                card3dMask = nullptr;
                card3dTex = nullptr;
                shapeType = 0;
            }
        } else {
            // Existing 2D-image mask path: fetch the mask image.
            RectI srcRoi;
            maskImg = getImage(0, time, RenderScale(), ViewIdx(0),
                               NULL, NULL, false, true,
                               eStorageModeRAM, 0, &srcRoi);
            if (maskImg) {
                maskBounds = maskImg->getBounds();
                maskWidth = maskBounds.x2 - maskBounds.x1;
                maskHeight = maskBounds.y2 - maskBounds.y1;
            }
            // If no image, fall back to Point shape
            if (!maskImg || maskWidth <= 0 || maskHeight <= 0) {
                shapeType = 0;
            }
        }
    }

    if (!data) {
        data = std::make_shared<ParticleData>();
    }

    int endFrame = (int)std::floor(time);
    if (endFrame < 1) endFrame = 1;

    // If going backward or to a different timeline position, reset
    if (startFrame > endFrame + 1) {
        startFrame = 1;
        data->particles.clear();
    }

    // Pre-create mask read access outside the loop for efficiency
    std::unique_ptr<Image::ReadAccess> maskRaPtr;
    if (shapeType == 4 && maskImg) {
        maskRaPtr.reset(new Image::ReadAccess(maskImg.get()));
    }

    // Simulate from startFrame to endFrame (incremental when going forward)
    for (int frame = startFrame; frame <= endFrame; ++frame) {

        // Per-frame deterministic RNG (same results regardless of start frame)
        std::mt19937 rng((unsigned int)(seedVal * 73856093 + frame * 19349663));

        // Spawn new particles
        for (int i = 0; i < rateVal; ++i) {
            Particle p;

            // Position based on emitter shape
            float offX = 0, offY = 0, offZ = 0;
            switch (shapeType) {
                case 1: { // Sphere
                    float u = dist01(rng) * 2.0f - 1.0f;
                    float t = dist01(rng) * 2.0f * (float)M_PI;
                    float r = std::cbrt(dist01(rng)) * shapeSize;
                    float s = std::sqrt(1.0f - u * u);
                    offX = r * s * std::cos(t);
                    offY = r * u;
                    offZ = r * s * std::sin(t);
                    break;
                }
                case 2: { // Box
                    offX = (dist01(rng) * 2.0f - 1.0f) * shapeSize;
                    offY = (dist01(rng) * 2.0f - 1.0f) * shapeSize;
                    offZ = (dist01(rng) * 2.0f - 1.0f) * shapeSize;
                    break;
                }
                case 3: { // Disc (XZ plane)
                    float angle = dist01(rng) * 2.0f * (float)M_PI;
                    float rad = std::sqrt(dist01(rng)) * shapeSize;
                    offX = rad * std::cos(angle);
                    offY = 0;
                    offZ = rad * std::sin(angle);
                    break;
                }
                case 4: { // Image Mask (2D image OR Card3D)
                    bool accepted = false;

                    // ----- Card3D mask path -----
                    if (card3dMask) {
                        for (int retry = 0; retry < 10; ++retry) {
                            const float u = dist01(rng);
                            const float v = dist01(rng);

                            int px = (int)(u * (float)card3dTex->width);
                            int py = (int)(v * (float)card3dTex->height);
                            if (px >= card3dTex->width)  px = card3dTex->width  - 1;
                            if (py >= card3dTex->height) py = card3dTex->height - 1;
                            const float* pix = &card3dTex->pixels[(py * card3dTex->width + px) * 4];

                            float lum = 0.2126f * pix[0] + 0.7152f * pix[1] + 0.0722f * pix[2];
                            if (lum < maskThresholdVal) continue;

                            // Local plane position: (-halfW..+halfW, -halfH..+halfH, 0).
                            // Per-axis scale applied, then rotation, then translation.
                            // Then SUBTRACT emPos so the final `p.px = emPosX + offX`
                            // step downstream produces the absolute world position —
                            // Card3D's transform replaces the emitter's translate.
                            float lx = (u - 0.5f) * card3dHalfW * 2.f * card3dScale[0];
                            float ly = (v - 0.5f) * card3dHalfH * 2.f * card3dScale[1];
                            float lz = 0.f;

                            float wx = (float)(card3dRot[0][0]*lx + card3dRot[0][1]*ly + card3dRot[0][2]*lz);
                            float wy = (float)(card3dRot[1][0]*lx + card3dRot[1][1]*ly + card3dRot[1][2]*lz);
                            float wz = (float)(card3dRot[2][0]*lx + card3dRot[2][1]*ly + card3dRot[2][2]*lz);

                            offX = wx + card3dT[0] - (float)emPosX;
                            offY = wy + card3dT[1] - (float)emPosY;
                            offZ = wz + card3dT[2] - (float)emPosZ;

                            if (maskColorFromImg) {
                                colR = pix[0];
                                colG = pix[1];
                                colB = pix[2];
                            }
                            accepted = true;
                            break;
                        }
                        if (!accepted) continue;
                        break;
                    }

                    // ----- Legacy 2D-image mask path -----
                    for (int retry = 0; retry < 10; ++retry) {
                        int px = maskBounds.x1 + (int)(dist01(rng) * (float)maskWidth);
                        int py = maskBounds.y1 + (int)(dist01(rng) * (float)maskHeight);
                        if (px >= maskBounds.x2) px = maskBounds.x2 - 1;
                        if (py >= maskBounds.y2) py = maskBounds.y2 - 1;

                        const float* pix = (const float*)maskRaPtr->pixelAt(px, py);
                        if (!pix) continue;

                        float lum = 0.2126f * pix[0] + 0.7152f * pix[1] + 0.0722f * pix[2];
                        if (lum < maskThresholdVal) continue;

                        // Map pixel to 3D position based on plane orientation
                        float u = ((float)(px - maskBounds.x1) / (float)maskWidth - 0.5f) * maskPlaneScaleVal;
                        float v = ((float)(py - maskBounds.y1) / (float)maskHeight - 0.5f) * maskPlaneScaleVal;

                        switch (maskOrientVal) {
                            case 0: // XY
                                offX = u; offY = v; offZ = 0;
                                break;
                            case 1: // XZ
                                offX = u; offY = 0; offZ = v;
                                break;
                            case 2: // YZ
                                offX = 0; offY = u; offZ = v;
                                break;
                        }

                        // Set color from image if enabled
                        if (maskColorFromImg) {
                            colR = pix[0];
                            colG = pix[1];
                            colB = pix[2];
                        }

                        accepted = true;
                        break;
                    }
                    if (!accepted) continue; // skip this particle
                    break;
                }
                default: // Point
                    break;
            }
            p.px = (float)emPosX + offX;
            p.py = (float)emPosY + offY;
            p.pz = (float)emPosZ + offZ;
            p.prevPx = p.px;
            p.prevPy = p.py;
            p.prevPz = p.pz;

            // Random direction within cone of `spread` degrees around emitDir
            float theta = spreadRad * std::sqrt(dist01(rng)); // uniform on disk
            float phi = 2.0f * (float)M_PI * dist01(rng);

            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            // Local direction in cone: sinTheta*cos(phi)*tangent + sinTheta*sin(phi)*bitangent + cosTheta*dir
            float localX = sinTheta * std::cos(phi) * tangentX + sinTheta * std::sin(phi) * bitangentX + cosTheta * fdx;
            float localY = sinTheta * std::cos(phi) * tangentY + sinTheta * std::sin(phi) * bitangentY + cosTheta * fdy;
            float localZ = sinTheta * std::cos(phi) * tangentZ + sinTheta * std::sin(phi) * bitangentZ + cosTheta * fdz;

            // Normalize the direction
            float dirLen = std::sqrt(localX * localX + localY * localY + localZ * localZ);
            if (dirLen > 1e-8f) { localX /= dirLen; localY /= dirLen; localZ /= dirLen; }

            // Randomize speed
            float speed = (float)velocityVal + (dist01(rng) * 2.0f - 1.0f) * (float)velVarVal;
            if (speed < 0.0f) speed = 0.0f;

            p.vx = localX * speed;
            p.vy = localY * speed;
            p.vz = localZ * speed;

            // Randomize lifetime
            p.life = (float)lifetimeVal + (dist01(rng) * 2.0f - 1.0f) * (float)lifeVarVal;
            if (p.life < 1.0f) p.life = 1.0f;

            // Randomize size
            p.size = (float)startSizeVal + (dist01(rng) * 2.0f - 1.0f) * (float)sizeVarVal;
            if (p.size < 0.001f) p.size = 0.001f;

            // Color
            p.r = colR;
            p.g = colG;
            p.b = colB;
            p.a = colA;

            p.age = 0.0f;
            p.mass = 1.0f;
            p.id = (uint32_t)(frame * 100000 + i + seedVal * 7919);

            // Per-particle color variance (deterministic by ID)
            float cVar = _imp->colorVariance.lock() ? (float)_imp->colorVariance.lock()->getValueAtTime(time) : 0.0f;
            if (cVar > 0.001f) {
                // Hash particle ID to get 3 random values in [0,1]
                uint32_t h = p.id;
                h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16;
                float rndR = (float)(h & 0xFFFF) / 65535.0f;
                h = h * 2654435761u;
                float rndG = (float)(h & 0xFFFF) / 65535.0f;
                h = h * 2654435761u;
                float rndB = (float)(h & 0xFFFF) / 65535.0f;
                p.r = p.r + (rndR - 0.5f) * 2.0f * cVar;
                p.g = p.g + (rndG - 0.5f) * 2.0f * cVar;
                p.b = p.b + (rndB - 0.5f) * 2.0f * cVar;
                // Clamp
                if (p.r < 0) p.r = 0; if (p.r > 1) p.r = 1;
                if (p.g < 0) p.g = 0; if (p.g > 1) p.g = 1;
                if (p.b < 0) p.b = 0; if (p.b > 1) p.b = 1;
            }

            data->particles.push_back(p);
        }

        // Read appearance knobs ONCE per frame (outside particle loop)
        float dt = 1.0f;

        // Over-life knobs (read once per frame, NOT per particle)
        float startSz = (float)_imp->startSize.lock()->getValueAtTime(frame);
        float endSz = (float)_imp->endSize.lock()->getValueAtTime(frame);
        float sr = (float)_imp->startColorR.lock()->getValueAtTime(frame);
        float sg = (float)_imp->startColorG.lock()->getValueAtTime(frame);
        float sb = (float)_imp->startColorB.lock()->getValueAtTime(frame);
        float er = (float)_imp->endColorR.lock()->getValueAtTime(frame);
        float eg = (float)_imp->endColorG.lock()->getValueAtTime(frame);
        float eb = (float)_imp->endColorB.lock()->getValueAtTime(frame);
        float fadeInFrac = (float)_imp->fadeIn.lock()->getValueAtTime(frame);
        float fadeOutFrac = (float)_imp->fadeOut.lock()->getValueAtTime(frame);

        for (size_t j = 0; j < data->particles.size(); ++j) {
            Particle& p = data->particles[j];

            // Save previous position for collision ray tests
            p.prevPx = p.px;
            p.prevPy = p.py;
            p.prevPz = p.pz;

            // Update position
            p.px += p.vx * dt;
            p.py += p.vy * dt;
            p.pz += p.vz * dt;
            p.age += 1.0f;

            // Age-based appearance (interpolate start→end over life)
            float ageFrac = (p.life > 0) ? (p.age / p.life) : 1.0f;
            ageFrac = std::max(0.0f, std::min(1.0f, ageFrac));

            // Size: lerp start→end
            p.size = startSz + (endSz - startSz) * ageFrac;

            // Color: lerp start→end + per-particle variance
            p.r = sr + (er - sr) * ageFrac;
            p.g = sg + (eg - sg) * ageFrac;
            p.b = sb + (eb - sb) * ageFrac;

            float cVar = _imp->colorVariance.lock() ? (float)_imp->colorVariance.lock()->getValueAtTime(frame) : 0.0f;
            if (cVar > 0.001f) {
                uint32_t h = p.id;
                h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16;
                float rndR = (float)(h & 0xFFFF) / 65535.0f;
                h = h * 2654435761u;
                float rndG = (float)(h & 0xFFFF) / 65535.0f;
                h = h * 2654435761u;
                float rndB = (float)(h & 0xFFFF) / 65535.0f;
                p.r += (rndR - 0.5f) * 2.0f * cVar;
                p.g += (rndG - 0.5f) * 2.0f * cVar;
                p.b += (rndB - 0.5f) * 2.0f * cVar;
                if (p.r < 0) p.r = 0; if (p.r > 1) p.r = 1;
                if (p.g < 0) p.g = 0; if (p.g > 1) p.g = 1;
                if (p.b < 0) p.b = 0; if (p.b > 1) p.b = 1;
            }

            // Alpha: fade in/out
            float alpha = 1.0f;
            if (fadeInFrac > 0.0f && ageFrac < fadeInFrac) {
                alpha = ageFrac / fadeInFrac;
            }
            if (fadeOutFrac > 0.0f && ageFrac > (1.0f - fadeOutFrac)) {
                alpha = (1.0f - ageFrac) / fadeOutFrac;
            }
            p.a = std::max(0.0f, std::min(1.0f, alpha));
        }

        // Remove expired particles (age >= life)
        data->removeExpired();
    }

    // Cache
    _lastParticleData = data;
    _lastSimFrame = time;

    return data;
}

// ==================== RoD / Render ====================

StatusEnum
ParticleEmitter::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                       ViewIdx /*view*/, RectD* rod)
{
    // Data node — 1x1 dummy output
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ParticleEmitter::render(const RenderActionArgs& args)
{
    // Data node — no meaningful image output, fill black
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

#include "moc_ParticleEmitter.cpp"
