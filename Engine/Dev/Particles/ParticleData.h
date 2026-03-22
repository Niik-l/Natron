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

#ifndef NATRON_ENGINE_PARTICLEDATA_H
#define NATRON_ENGINE_PARTICLEDATA_H

#include <memory>
#include <vector>

/**
 * @brief Single particle.
 */
struct Particle {
    float px, py, pz;       // position
    float vx, vy, vz;       // velocity
    float r, g, b, a;       // color + alpha
    float size;             // display size
    float age;              // current age in frames
    float life;             // total lifetime in frames
    float mass;             // mass (affects how forces act)

    Particle()
        : px(0), py(0), pz(0)
        , vx(0), vy(0), vz(0)
        , r(1), g(1), b(1), a(1)
        , size(0.1f)
        , age(0), life(50)
        , mass(1.0f)
    {}
};

/**
 * @brief Container for a particle system's state at a given time.
 * Shared between particle nodes via getParticleData().
 */
class ParticleData {
public:
    std::vector<Particle> particles;

    int numParticles() const { return (int)particles.size(); }

    void removeExpired()
    {
        std::vector<Particle> alive;
        alive.reserve(particles.size());
        for (size_t i = 0; i < particles.size(); ++i) {
            if (particles[i].age < particles[i].life) {
                alive.push_back(particles[i]);
            }
        }
        particles.swap(alive);
    }
};

typedef std::shared_ptr<ParticleData> ParticleDataPtr;

#endif // NATRON_ENGINE_PARTICLEDATA_H
