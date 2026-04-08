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

#ifndef NATRON_ENGINE_PARTICLETURBULENCE_H
#define NATRON_ENGINE_PARTICLETURBULENCE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "ParticleModifier.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct ParticleTurbulencePrivate;

/**
 * @brief Applies turbulent noise force to particles for organic, swirling motion.
 *
 * Input 0: Particle source (ParticleEmitter or another particle modifier)
 *
 * Uses curl noise derived from fractal Brownian motion to create
 * divergence-free turbulent flow fields.
 */
class ParticleTurbulence
    : public ParticleModifier
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ParticleTurbulence(n); }

    ParticleTurbulence(NodePtr node);
    virtual ~ParticleTurbulence();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_PARTICLETURBULENCE; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ParticleTurbulence"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void applyForce(ParticleDataPtr data, double time) OVERRIDE FINAL;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;

    std::unique_ptr<ParticleTurbulencePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PARTICLETURBULENCE_H
