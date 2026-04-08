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

#include "ParticleSolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

NATRON_NAMESPACE_ENTER

// ============================================================
// Collision math — ported from tools/particle_testbed/main.cpp
// ============================================================

// Bounce a particle off a surface. hitT is the fraction along the
// prev->current segment where the hit occurred (0..1).
static void
bounceParticle(Particle& p, float nx, float ny, float nz, float hitT,
               float elasticity, float friction)
{
    float vDotN = p.vx * nx + p.vy * ny + p.vz * nz;
    if (vDotN >= 0.0f) return; // moving away

    p.collided = true;
    p.bounceCount++;

    // Hit point on the surface
    float hitX = p.prevPx + (p.px - p.prevPx) * hitT;
    float hitY = p.prevPy + (p.py - p.prevPy) * hitT;
    float hitZ = p.prevPz + (p.pz - p.prevPz) * hitT;

    // Reflect velocity
    p.vx -= 2.0f * vDotN * nx;
    p.vy -= 2.0f * vDotN * ny;
    p.vz -= 2.0f * vDotN * nz;

    // Elasticity (energy loss on bounce)
    p.vx *= elasticity;
    p.vy *= elasticity;
    p.vz *= elasticity;

    // Friction (reduce tangential component)
    float vn = p.vx * nx + p.vy * ny + p.vz * nz;
    float tangX = p.vx - vn * nx;
    float tangY = p.vy - vn * ny;
    float tangZ = p.vz - vn * nz;
    tangX *= (1.0f - friction);
    tangY *= (1.0f - friction);
    tangZ *= (1.0f - friction);
    p.vx = vn * nx + tangX;
    p.vy = vn * ny + tangY;
    p.vz = vn * nz + tangZ;

    // Place at hit point + epsilon along normal
    p.px = hitX + nx * 0.001f;
    p.py = hitY + ny * 0.001f;
    p.pz = hitZ + nz * 0.001f;

    // Continue remaining motion in reflected direction
    float remaining = 1.0f - hitT;
    p.px += p.vx * remaining;
    p.py += p.vy * remaining;
    p.pz += p.vz * remaining;

    // Kill if settled
    float speed = std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
    if (speed < 0.0001f) p.life = p.age;
}

// Ray-AABB intersection (slab method).
// Tests segment from (ox,oy,oz) with direction (dx,dy,dz) for t in [0,1].
static bool
rayAABB(float ox, float oy, float oz,
        float dx, float dy, float dz,
        float bMinX, float bMinY, float bMinZ,
        float bMaxX, float bMaxY, float bMaxZ,
        float& outT, float& outNx, float& outNy, float& outNz)
{
    float tMin = 0.0f, tMax = 1.0f;
    float hitNx = 0, hitNy = 0, hitNz = 0;

    // X slab
    if (std::abs(dx) < 1e-8f) {
        if (ox < bMinX || ox > bMaxX) return false;
    } else {
        float invD = 1.0f / dx;
        float t1 = (bMinX - ox) * invD;
        float t2 = (bMaxX - ox) * invD;
        float nxSign = -1.0f;
        if (t1 > t2) { std::swap(t1, t2); nxSign = 1.0f; }
        if (t1 > tMin) { tMin = t1; hitNx = nxSign; hitNy = 0; hitNz = 0; }
        if (t2 < tMax) tMax = t2;
        if (tMin > tMax) return false;
    }

    // Y slab
    if (std::abs(dy) < 1e-8f) {
        if (oy < bMinY || oy > bMaxY) return false;
    } else {
        float invD = 1.0f / dy;
        float t1 = (bMinY - oy) * invD;
        float t2 = (bMaxY - oy) * invD;
        float nySign = -1.0f;
        if (t1 > t2) { std::swap(t1, t2); nySign = 1.0f; }
        if (t1 > tMin) { tMin = t1; hitNx = 0; hitNy = nySign; hitNz = 0; }
        if (t2 < tMax) tMax = t2;
        if (tMin > tMax) return false;
    }

    // Z slab
    if (std::abs(dz) < 1e-8f) {
        if (oz < bMinZ || oz > bMaxZ) return false;
    } else {
        float invD = 1.0f / dz;
        float t1 = (bMinZ - oz) * invD;
        float t2 = (bMaxZ - oz) * invD;
        float nzSign = -1.0f;
        if (t1 > t2) { std::swap(t1, t2); nzSign = 1.0f; }
        if (t1 > tMin) { tMin = t1; hitNx = 0; hitNy = 0; hitNz = nzSign; }
        if (t2 < tMax) tMax = t2;
        if (tMin > tMax) return false;
    }

    if (tMin >= 0.0f && tMin <= 1.0f) {
        outT = tMin;
        outNx = hitNx; outNy = hitNy; outNz = hitNz;
        return true;
    }
    return false;
}

// Push a particle out of an AABB if it spawned inside (spawn-inside handling).
static void
pushOutOfBox(Particle& p, float bMinX, float bMinY, float bMinZ,
             float bMaxX, float bMaxY, float bMaxZ)
{
    if (p.px > bMinX && p.px < bMaxX &&
        p.py > bMinY && p.py < bMaxY &&
        p.pz > bMinZ && p.pz < bMaxZ) {
        float dists[6] = {
            p.px - bMinX, bMaxX - p.px,
            p.py - bMinY, bMaxY - p.py,
            p.pz - bMinZ, bMaxZ - p.pz
        };
        float normals[6][3] = {
            {-1,0,0}, {1,0,0},
            {0,-1,0}, {0,1,0},
            {0,0,-1}, {0,0,1}
        };
        int closest = 0;
        for (int f = 1; f < 6; ++f) {
            if (dists[f] < dists[closest]) closest = f;
        }
        p.px += normals[closest][0] * (dists[closest] + 0.01f);
        p.py += normals[closest][1] * (dists[closest] + 0.01f);
        p.pz += normals[closest][2] * (dists[closest] + 0.01f);
        float vDotN = p.vx * normals[closest][0] + p.vy * normals[closest][1] + p.vz * normals[closest][2];
        if (vDotN < 0) {
            p.vx -= vDotN * normals[closest][0];
            p.vy -= vDotN * normals[closest][1];
            p.vz -= vDotN * normals[closest][2];
        }
        p.collided = true;
    }
}

// Push a particle out of a sphere if it spawned inside.
static void
pushOutOfSphere(Particle& p, float cx, float cy, float cz, float radius)
{
    float dx = p.px - cx, dy = p.py - cy, dz = p.pz - cz;
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < radius && dist > 0.001f) {
        float nx = dx / dist, ny = dy / dist, nz = dz / dist;
        p.px = cx + nx * (radius + 0.01f);
        p.py = cy + ny * (radius + 0.01f);
        p.pz = cz + nz * (radius + 0.01f);
        float vDotN = p.vx * nx + p.vy * ny + p.vz * nz;
        if (vDotN < 0) {
            p.vx -= vDotN * nx;
            p.vy -= vDotN * ny;
            p.vz -= vDotN * nz;
        }
        p.collided = true;
    }
}

// Collide particle with a plane (ray test: prev -> current).
static void
collidePlane(Particle& p, float planeH, float nx, float ny, float nz,
             float elasticity, float friction)
{
    float distPrev = (p.prevPx * nx + p.prevPy * ny + p.prevPz * nz) - planeH;
    float distCurr = (p.px * nx + p.py * ny + p.pz * nz) - planeH;
    if (distPrev >= 0.0f && distCurr <= 0.0f) {
        float hitT = distPrev / (distPrev - distCurr);
        bounceParticle(p, nx, ny, nz, hitT, elasticity, friction);
    } else if (distCurr <= 0.0f) {
        bounceParticle(p, nx, ny, nz, 0.0f, elasticity, friction);
    }
}

// Collide particle with an AABB (solid obstacle — ray-AABB test).
static void
collideGeoBox(Particle& p, float bMinX, float bMinY, float bMinZ,
              float bMaxX, float bMaxY, float bMaxZ,
              float elasticity, float friction)
{
    float dx = p.px - p.prevPx;
    float dy = p.py - p.prevPy;
    float dz = p.pz - p.prevPz;
    float hitT, hitNx, hitNy, hitNz;
    if (rayAABB(p.prevPx, p.prevPy, p.prevPz,
                dx, dy, dz,
                bMinX, bMinY, bMinZ,
                bMaxX, bMaxY, bMaxZ,
                hitT, hitNx, hitNy, hitNz)) {
        bounceParticle(p, hitNx, hitNy, hitNz, hitT, elasticity, friction);
    }
}

// Collide particle with a sphere (solid obstacle — ray-sphere test).
static void
collideGeoSphere(Particle& p, float cx, float cy, float cz, float radius,
                 float elasticity, float friction)
{
    float ox = p.prevPx - cx, oy = p.prevPy - cy, oz = p.prevPz - cz;
    float dx = p.px - p.prevPx, dy = p.py - p.prevPy, dz = p.pz - p.prevPz;
    float a = dx * dx + dy * dy + dz * dz;
    float b = 2.0f * (ox * dx + oy * dy + oz * dz);
    float c = ox * ox + oy * oy + oz * oz - radius * radius;
    if (a < 1e-10f) {
        // Stationary inside sphere
        if (c < 0) {
            float dist = std::sqrt(ox * ox + oy * oy + oz * oz);
            if (dist > 0.001f)
                bounceParticle(p, ox / dist, oy / dist, oz / dist, 0.0f, elasticity, friction);
        }
        return;
    }
    float disc = b * b - 4.0f * a * c;
    if (disc < 0) return;
    float sqrtDisc = std::sqrt(disc);
    float t = (-b - sqrtDisc) / (2.0f * a);
    if (t >= 0.0f && t <= 1.0f) {
        float hx = p.prevPx + dx * t - cx;
        float hy = p.prevPy + dy * t - cy;
        float hz = p.prevPz + dz * t - cz;
        float len = std::sqrt(hx * hx + hy * hy + hz * hz);
        if (len > 0.001f)
            bounceParticle(p, hx / len, hy / len, hz / len, t, elasticity, friction);
    }
}

// ============================================================
// ParticleSolver node implementation
// ============================================================

struct ParticleSolverPrivate
{
    KnobDoubleWPtr elasticity;
    KnobDoubleWPtr friction;
    KnobIntWPtr maxBounces;
    KnobIntWPtr substeps;
    KnobBoolWPtr showCollisions;

    // Solver cache — the single authoritative particle state.
    // All forces, integration, and collision happen on this data.
    ParticleDataPtr cachedData;
    double cachedFrame;
    std::unordered_set<uint32_t> knownIDs;

    ParticleSolverPrivate() : cachedFrame(-1e9) {}
};

ParticleSolver::ParticleSolver(NodePtr node)
    : ParticleModifier(node)
    , _imp(new ParticleSolverPrivate())
{
}

ParticleSolver::~ParticleSolver()
{
}

std::string
ParticleSolver::getPluginDescription() const
{
    return tr("Particle solver — runs the simulation loop (forces, integration, collision).\n\n"
              "Place at the end of the particle chain. Walks upstream to gather all force nodes "
              "(Gravity, Wind, etc.) and the emitter, then runs the integrated simulation.\n\n"
              "Optional: connect 3D geometry (Cube3D, Sphere3D) to the 'geo' input for collision.\n"
              "Without geo, acts as a pure solver (forces + integration only).\n\n"
              "Elasticity controls bounce energy (1 = perfect bounce, 0 = stick).\n"
              "Friction removes tangential velocity on bounce (0 = slide, 1 = full stop).").toStdString();
}

void
ParticleSolver::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("Collide"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Elasticity"));
        k->setName("elasticity"); k->setDefaultValue(0.5); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("How much velocity is preserved on bounce. 1.0 = perfect bounce, 0 = stick."));
        mainPage->addKnob(k); _imp->elasticity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Friction"));
        k->setName("friction"); k->setDefaultValue(0.2); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("How much tangential velocity is removed on bounce. 0 = no friction, 1 = full stop tangentially."));
        mainPage->addKnob(k); _imp->friction = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Bounces"));
        k->setName("maxBounces"); k->setDefaultValue(0); k->setAnimationEnabled(false);
        k->setMinimum(0); k->setMaximum(10);
        k->setDisplayMinimum(0); k->setDisplayMaximum(10);
        k->setHintToolTip(tr("Maximum number of bounces before killing the particle. 0 = unlimited."));
        mainPage->addKnob(k); _imp->maxBounces = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Substeps"));
        k->setName("substeps"); k->setDefaultValue(4); k->setAnimationEnabled(false);
        k->setMinimum(1); k->setMaximum(32);
        k->setDisplayMinimum(1); k->setDisplayMaximum(16);
        k->setHintToolTip(tr("Substeps per frame. Higher = smoother collision and less tunneling. 1 = one step per frame, 4 = default."));
        mainPage->addKnob(k); _imp->substeps = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Show Collisions"));
        k->setName("showCollisions"); k->setDefaultValue(false); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Debug: tint collided particles red to visualize collision hits."));
        mainPage->addKnob(k); _imp->showCollisions = k;
    }
}

// ============================================================
// THE SOLVER — owns the single authoritative particle state.
//
// Each frame: get new particles from emitter, apply all upstream
// forces in order, integrate positions, apply collision, expire.
//
// This is the standard pattern used by Blender, Unity, Unreal,
// and Houdini: forces are stateless velocity modifiers, one solver
// owns position/velocity and runs the integrated loop.
// ============================================================
ParticleDataPtr
ParticleSolver::getParticleData(double time)
{
    // Return cached if already at this frame
    if (_imp->cachedData && time == _imp->cachedFrame) {
        return _imp->cachedData;
    }

    // Walk upstream: collect force nodes and find the emitter
    std::vector<ParticleModifier*> forces;
    ParticleProvider* emitter = nullptr;
    EffectInstancePtr input0 = getInput(0);
    if (!input0) return ParticleDataPtr();

    ParticleModifier* inputMod = dynamic_cast<ParticleModifier*>(input0.get());
    if (inputMod) {
        ParticleModifier::collectUpstreamForces(input0.get(), forces, emitter);
    } else {
        emitter = dynamic_cast<ParticleProvider*>(input0.get());
    }
    if (!emitter) return ParticleDataPtr();

    int endFrame = (int)std::floor(time);
    if (endFrame < 1) endFrame = 1;

    // Determine start frame — use cache if moving forward
    int startFrame = 1;
    if (_imp->cachedData && _imp->cachedFrame > 0 && time > _imp->cachedFrame) {
        startFrame = (int)_imp->cachedFrame + 1;
    } else {
        _imp->cachedData.reset();
        _imp->knownIDs.clear();
    }

    if (!_imp->cachedData) {
        _imp->cachedData = std::make_shared<ParticleData>();
    }

    for (int frame = startFrame; frame <= endFrame; ++frame) {
        // 1. Get emitter output to discover new particles
        ParticleDataPtr emitterData = emitter->getParticleData((double)frame);
        if (!emitterData) continue;

        // 2. Add NEW particles from emitter
        for (size_t i = 0; i < emitterData->particles.size(); ++i) {
            const Particle& up = emitterData->particles[i];
            if (_imp->knownIDs.find(up.id) == _imp->knownIDs.end()) {
                Particle p = up;
                // Undo emitter's integration — we integrate ourselves
                p.px -= p.vx;
                p.py -= p.vy;
                p.pz -= p.vz;
                p.prevPx = p.px;
                p.prevPy = p.py;
                p.prevPz = p.pz;
                p.age -= 1.0f;
                if (p.age < 0) p.age = 0;
                _imp->cachedData->particles.push_back(p);
                _imp->knownIDs.insert(up.id);
            }
        }

        // 3-5. Substep loop: forces → integrate → collide
        int numSubsteps = _imp->substeps.lock() ? _imp->substeps.lock()->getValue() : 4;
        if (numSubsteps < 1) numSubsteps = 1;
        float dt = 1.0f / (float)numSubsteps;

        for (int sub = 0; sub < numSubsteps; ++sub) {
            // 3. Apply all upstream forces (scaled by dt)
            //    Forces add a per-frame velocity delta. For substeps we want
            //    1/N of that delta per substep. Snapshot velocity before each
            //    force, apply it (full delta), then scale the delta by dt.
            for (size_t f = 0; f < forces.size(); ++f) {
                // Snapshot velocities
                std::vector<std::array<float, 3>> preVel(_imp->cachedData->particles.size());
                for (size_t j = 0; j < _imp->cachedData->particles.size(); ++j) {
                    const Particle& p = _imp->cachedData->particles[j];
                    preVel[j] = {p.vx, p.vy, p.vz};
                }
                forces[f]->applyForce(_imp->cachedData, (double)frame);
                // Scale velocity delta by dt
                for (size_t j = 0; j < _imp->cachedData->particles.size(); ++j) {
                    Particle& p = _imp->cachedData->particles[j];
                    p.vx = preVel[j][0] + (p.vx - preVel[j][0]) * dt;
                    p.vy = preVel[j][1] + (p.vy - preVel[j][1]) * dt;
                    p.vz = preVel[j][2] + (p.vz - preVel[j][2]) * dt;
                }
            }

            // 4. Integrate positions (scaled by dt)
            for (size_t j = 0; j < _imp->cachedData->particles.size(); ++j) {
                Particle& p = _imp->cachedData->particles[j];
                p.prevPx = p.px;
                p.prevPy = p.py;
                p.prevPz = p.pz;
                p.px += p.vx * dt;
                p.py += p.vy * dt;
                p.pz += p.vz * dt;
            }

            // 5. Apply collision AFTER integration
            for (size_t j = 0; j < _imp->cachedData->particles.size(); ++j) {
                Particle& p = _imp->cachedData->particles[j];
                p.collided = false;
                applyCollision(p, (double)frame);
            }
        }

        // Age once per frame (not per substep)
        for (size_t j = 0; j < _imp->cachedData->particles.size(); ++j) {
            _imp->cachedData->particles[j].age += 1.0f;
        }

        // 6. Sync appearance from emitter (color, size, alpha are age-based)
        std::unordered_map<uint32_t, const Particle*> emitterByID;
        for (size_t i = 0; i < emitterData->particles.size(); ++i) {
            emitterByID[emitterData->particles[i].id] = &emitterData->particles[i];
        }
        for (size_t j = 0; j < _imp->cachedData->particles.size(); ++j) {
            Particle& p = _imp->cachedData->particles[j];
            auto it = emitterByID.find(p.id);
            if (it != emitterByID.end()) {
                p.r = it->second->r;
                p.g = it->second->g;
                p.b = it->second->b;
                p.a = it->second->a;
                p.size = it->second->size;
                p.life = it->second->life;
            }
        }

        // 7. Remove expired particles
        std::unordered_set<uint32_t> emitterIDs;
        for (size_t i = 0; i < emitterData->particles.size(); ++i) {
            emitterIDs.insert(emitterData->particles[i].id);
        }
        std::vector<Particle> alive;
        alive.reserve(_imp->cachedData->particles.size());
        for (size_t i = 0; i < _imp->cachedData->particles.size(); ++i) {
            const Particle& p = _imp->cachedData->particles[i];
            if (p.age < p.life && emitterIDs.count(p.id) > 0) {
                alive.push_back(p);
            } else {
                _imp->knownIDs.erase(p.id);
            }
        }
        _imp->cachedData->particles.swap(alive);
    }

    // Debug: tint collided particles red
    bool showCol = _imp->showCollisions.lock() ? _imp->showCollisions.lock()->getValue() : false;
    if (showCol) {
        for (size_t i = 0; i < _imp->cachedData->particles.size(); ++i) {
            Particle& p = _imp->cachedData->particles[i];
            if (p.collided) { p.r = 1.0f; p.g = 0.15f; p.b = 0.1f; }
        }
    }

    _imp->cachedFrame = time;
    return _imp->cachedData;
}

// applyForce is required by the ParticleModifier interface but collision
// is handled in applyCollision() called from getParticleData(). This is
// a no-op since we override the simulation loop entirely.
void
ParticleSolver::applyForce(ParticleDataPtr /*data*/, double /*time*/)
{
}

// Build a 3x3 rotation matrix from Euler angles (degrees, XYZ order).
// Stores as row-major: m[row][col].
static void
buildRotationMatrix(float rxDeg, float ryDeg, float rzDeg, float m[3][3])
{
    float rx = rxDeg * (float)M_PI / 180.0f;
    float ry = ryDeg * (float)M_PI / 180.0f;
    float rz = rzDeg * (float)M_PI / 180.0f;
    float cx = std::cos(rx), sx = std::sin(rx);
    float cy = std::cos(ry), sy = std::sin(ry);
    float cz = std::cos(rz), sz = std::sin(rz);

    // ZYX order (standard for Euler XYZ rotations applied in reverse)
    m[0][0] = cy * cz;
    m[0][1] = sx * sy * cz - cx * sz;
    m[0][2] = cx * sy * cz + sx * sz;
    m[1][0] = cy * sz;
    m[1][1] = sx * sy * sz + cx * cz;
    m[1][2] = cx * sy * sz - sx * cz;
    m[2][0] = -sy;
    m[2][1] = sx * cy;
    m[2][2] = cx * cy;
}

// Apply rotation matrix to a vector
static void rotVec(const float m[3][3], float x, float y, float z,
                   float& ox, float& oy, float& oz)
{
    ox = m[0][0] * x + m[0][1] * y + m[0][2] * z;
    oy = m[1][0] * x + m[1][1] * y + m[1][2] * z;
    oz = m[2][0] * x + m[2][1] * y + m[2][2] * z;
}

// Apply inverse (transpose) rotation to a vector
static void invRotVec(const float m[3][3], float x, float y, float z,
                      float& ox, float& oy, float& oz)
{
    ox = m[0][0] * x + m[1][0] * y + m[2][0] * z;
    oy = m[0][1] * x + m[1][1] * y + m[2][1] * z;
    oz = m[0][2] * x + m[1][2] * y + m[2][2] * z;
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Collision dispatch — called per-particle after position integration.
// Reads connected geometry node's transform (including rotation) to build
// the collision shape. For boxes, particles are transformed into the geo's
// local space for AABB testing, then results are transformed back (OBB collision).
void
ParticleSolver::applyCollision(Particle& p, double time)
{
    // Check max bounces — kill particle if exceeded (0 = unlimited)
    int maxBouncesVal = _imp->maxBounces.lock() ? _imp->maxBounces.lock()->getValue() : 0;
    if (maxBouncesVal > 0 && p.bounceCount >= maxBouncesVal) {
        p.life = p.age; // kill particle
        return;
    }

    EffectInstancePtr geoEffect = getInput(1);
    if (!geoEffect) return;

    float elasticityVal = (float)_imp->elasticity.lock()->getValueAtTime(time);
    float frictionVal = (float)_imp->friction.lock()->getValueAtTime(time);

    // Read transform + rotation + size knobs from connected geometry node
    float tx = 0, ty = 0, tz = 0;
    float rx = 0, ry = 0, rz = 0;
    float sx = 1, sy = 1, sz = 1;
    float geoSize = 1.0f;
    KnobIPtr k;
    k = geoEffect->getKnobByName("translateX"); if (k) tx = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = geoEffect->getKnobByName("translateY"); if (k) ty = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = geoEffect->getKnobByName("translateZ"); if (k) tz = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = geoEffect->getKnobByName("rotateX"); if (k) rx = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = geoEffect->getKnobByName("rotateY"); if (k) ry = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = geoEffect->getKnobByName("rotateZ"); if (k) rz = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = geoEffect->getKnobByName("scaleX"); if (k) sx = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = geoEffect->getKnobByName("scaleY"); if (k) sy = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = geoEffect->getKnobByName("scaleZ"); if (k) sz = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = geoEffect->getKnobByName("size"); if (k) geoSize = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);

    std::string pluginID = geoEffect->getPluginID();
    bool isSphere = (pluginID.find("Sphere") != std::string::npos);

    if (isSphere) {
        // Sphere is rotation-invariant — no need for OBB
        float radius = geoSize * 0.5f * sx;
        collideGeoSphere(p, tx, ty, tz, radius, elasticityVal, frictionVal);
        pushOutOfSphere(p, tx, ty, tz, radius);
    } else {
        // OBB collision: transform particle into the cube's local space,
        // do AABB test, transform back.
        float hx = geoSize * 0.5f * sx;
        float hy = geoSize * 0.5f * sy;
        float hz = geoSize * 0.5f * sz;

        bool hasRotation = (std::abs(rx) > 0.001f || std::abs(ry) > 0.001f || std::abs(rz) > 0.001f);

        if (!hasRotation) {
            // Fast path: no rotation, standard AABB
            float bMinX = tx - hx, bMaxX = tx + hx;
            float bMinY = ty - hy, bMaxY = ty + hy;
            float bMinZ = tz - hz, bMaxZ = tz + hz;
            collideGeoBox(p, bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ,
                          elasticityVal, frictionVal);
            pushOutOfBox(p, bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ);
        } else {
            // OBB: transform particle positions into local space
            float rot[3][3];
            buildRotationMatrix(rx, ry, rz, rot);

            // Save world-space state
            float worldPx = p.px, worldPy = p.py, worldPz = p.pz;
            float worldPrevPx = p.prevPx, worldPrevPy = p.prevPy, worldPrevPz = p.prevPz;
            float worldVx = p.vx, worldVy = p.vy, worldVz = p.vz;

            // Transform to local space (inverse rotation of position relative to geo center)
            float relX = p.px - tx, relY = p.py - ty, relZ = p.pz - tz;
            float relPrevX = p.prevPx - tx, relPrevY = p.prevPy - ty, relPrevZ = p.prevPz - tz;
            invRotVec(rot, relX, relY, relZ, p.px, p.py, p.pz);
            invRotVec(rot, relPrevX, relPrevY, relPrevZ, p.prevPx, p.prevPy, p.prevPz);
            invRotVec(rot, p.vx, p.vy, p.vz, p.vx, p.vy, p.vz);

            // AABB test in local space (centered at origin)
            collideGeoBox(p, -hx, -hy, -hz, hx, hy, hz, elasticityVal, frictionVal);
            pushOutOfBox(p, -hx, -hy, -hz, hx, hy, hz);

            if (p.collided) {
                // Transform results back to world space
                float localPx = p.px, localPy = p.py, localPz = p.pz;
                float localVx = p.vx, localVy = p.vy, localVz = p.vz;
                rotVec(rot, localPx, localPy, localPz, p.px, p.py, p.pz);
                p.px += tx; p.py += ty; p.pz += tz;
                rotVec(rot, localVx, localVy, localVz, p.vx, p.vy, p.vz);
            } else {
                // No collision — restore world positions
                p.px = worldPx; p.py = worldPy; p.pz = worldPz;
                p.vx = worldVx; p.vy = worldVy; p.vz = worldVz;
            }
            // Always restore prevP (it's for next frame's tracking, stays in world space)
            p.prevPx = worldPrevPx; p.prevPy = worldPrevPy; p.prevPz = worldPrevPz;
        }
    }
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleSolver.cpp"
