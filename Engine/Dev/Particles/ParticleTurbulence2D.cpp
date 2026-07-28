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

#include "ParticleTurbulence2D.h"

#include "ParticleParallel.h"

#include <cmath>

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

NATRON_NAMESPACE_ENTER

// ---------------------------------------------------------------------------
// Inline Perlin improved noise (Ken Perlin, 2002)
// ---------------------------------------------------------------------------

namespace {

static const int perm[512] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
    140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
    247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
    57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
    74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
    60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
    65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
    200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
    52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
    207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
    119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
    129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
    218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
    81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
    184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
    222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,
    // Repeat
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
    140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
    247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
    57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
    74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
    60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
    65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
    200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
    52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
    207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
    119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
    129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
    218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
    81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
    184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
    222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
};

inline double fade(double t)
{
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

inline double lerp(double t, double a, double b)
{
    return a + t * (b - a);
}

inline double grad(int hash, double x, double y, double z)
{
    int h = hash & 15;
    double u = h < 8 ? x : y;
    double v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

double noise3(double x, double y, double z)
{
    int X = (int)std::floor(x) & 255;
    int Y = (int)std::floor(y) & 255;
    int Z = (int)std::floor(z) & 255;

    x -= std::floor(x);
    y -= std::floor(y);
    z -= std::floor(z);

    double u = fade(x);
    double v = fade(y);
    double w = fade(z);

    int A  = perm[X] + Y;
    int AA = perm[A] + Z;
    int AB = perm[A + 1] + Z;
    int B  = perm[X + 1] + Y;
    int BA = perm[B] + Z;
    int BB = perm[B + 1] + Z;

    return lerp(w,
        lerp(v,
            lerp(u, grad(perm[AA], x, y, z),
                    grad(perm[BA], x - 1, y, z)),
            lerp(u, grad(perm[AB], x, y - 1, z),
                    grad(perm[BB], x - 1, y - 1, z))),
        lerp(v,
            lerp(u, grad(perm[AA + 1], x, y, z - 1),
                    grad(perm[BA + 1], x - 1, y, z - 1)),
            lerp(u, grad(perm[AB + 1], x, y - 1, z - 1),
                    grad(perm[BB + 1], x - 1, y - 1, z - 1))));
}

double fbm3(double x, double y, double z, int octaves, double lacunarity, double gain)
{
    double sum = 0.0;
    double amp = 1.0;
    double freq = 1.0;

    for (int i = 0; i < octaves; ++i) {
        sum += amp * noise3(x * freq, y * freq, z * freq);
        freq *= lacunarity;
        amp *= gain;
    }

    return sum;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ParticleTurbulence2D implementation
// ---------------------------------------------------------------------------

struct ParticleTurbulence2DPrivate
{
    KnobDoubleWPtr strength;
    KnobDoubleWPtr scale;
    KnobDoubleWPtr speed;
    KnobIntWPtr octaves;
    KnobDoubleWPtr lacunarity;
    KnobDoubleWPtr gain;
};

ParticleTurbulence2D::ParticleTurbulence2D(NodePtr node)
    : ParticleModifier(node)
    , _imp(new ParticleTurbulence2DPrivate())
{
}

ParticleTurbulence2D::~ParticleTurbulence2D()
{
}

std::string
ParticleTurbulence2D::getPluginDescription() const
{
    return tr("Applies planar/2D-biased curl noise turbulence for sheet-like flowing motion.\n\n"
              "Connect to a ParticleEmitter or another particle modifier.\n"
              "A 2D variant that creates sheet-like flowing motion using a single scalar noise field.").toStdString();
}

void
ParticleTurbulence2D::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("Turbulence"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Strength"));
        k->setName("strength"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Force magnitude of the turbulence."));
        mainPage->addKnob(k); _imp->strength = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale"));
        k->setName("scale"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.01); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Spatial scale of the noise. Larger values produce bigger swirls."));
        mainPage->addKnob(k); _imp->scale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Speed"));
        k->setName("speed"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(5.0);
        k->setHintToolTip(tr("How fast the noise field evolves over time."));
        mainPage->addKnob(k); _imp->speed = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Octaves"));
        k->setName("octaves"); k->setDefaultValue(3); k->setAnimationEnabled(false);
        k->setMinimum(1); k->setDisplayMinimum(1); k->setDisplayMaximum(8);
        k->setHintToolTip(tr("Number of fractal detail layers."));
        mainPage->addKnob(k); _imp->octaves = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Lacunarity"));
        k->setName("lacunarity"); k->setDefaultValue(2.0); k->setAnimationEnabled(false);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(4.0);
        k->setHintToolTip(tr("Frequency multiplier per octave."));
        mainPage->addKnob(k); _imp->lacunarity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Gain"));
        k->setName("gain"); k->setDefaultValue(0.5); k->setAnimationEnabled(false);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Amplitude multiplier per octave."));
        mainPage->addKnob(k); _imp->gain = k;
    }
}

void
ParticleTurbulence2D::applyForce(ParticleDataPtr data, double time)
{
    if (!data) return;

    float strength = (float)_imp->strength.lock()->getValueAtTime(time);
    if (strength <= 0.0f) return;

    float scaleVal = (float)_imp->scale.lock()->getValueAtTime(time);
    float speed = (float)_imp->speed.lock()->getValueAtTime(time);
    int octaves = _imp->octaves.lock()->getValueAtTime(time);
    float lacunarity = (float)_imp->lacunarity.lock()->getValueAtTime(time);
    float gain = (float)_imp->gain.lock()->getValueAtTime(time);

    float invScale = 1.0f / std::max(scaleVal, 0.01f);
    float timeOffset = (float)time * speed * 0.1f;
    float eps = 0.01f;

    // Curl noise is by far the most expensive force (dozens of Perlin
    // evaluations per particle per substep) and each particle is fully
    // independent — fan out across cores.
    forEachParticleParallel(data->particles, [&](Particle& p) {

        double x = (double)p.px * invScale + timeOffset;
        double y = (double)p.py * invScale;
        double z = (double)p.pz * invScale;

        // Curl noise via finite differences of ONE scalar field — only 6
        // distinct sample points exist; the previous code evaluated each of
        // them exactly twice (12 fbm3 calls for 6 values, a strict 2x waste).
        const double invTwoEps = 1.0 / (2.0 * eps);
        const double dX = (fbm3(x + eps, y, z, octaves, lacunarity, gain) -
                           fbm3(x - eps, y, z, octaves, lacunarity, gain)) * invTwoEps;
        const double dY = (fbm3(x, y + eps, z, octaves, lacunarity, gain) -
                           fbm3(x, y - eps, z, octaves, lacunarity, gain)) * invTwoEps;
        const double dZ = (fbm3(x, y, z + eps, octaves, lacunarity, gain) -
                           fbm3(x, y, z - eps, octaves, lacunarity, gain)) * invTwoEps;

        float curlX = (float)(dY - dZ);
        float curlY = (float)(dZ - dX);
        float curlZ = (float)(dX - dY);

        p.vx += curlX * strength * 0.1f;
        p.vy += curlY * strength * 0.1f;
        p.vz += curlZ * strength * 0.1f;
    });
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleTurbulence2D.cpp"
