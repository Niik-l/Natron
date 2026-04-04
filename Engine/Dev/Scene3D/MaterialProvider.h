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

#ifndef NATRON_ENGINE_MATERIALPROVIDER_H
#define NATRON_ENGINE_MATERIALPROVIDER_H

#include <string>

/**
 * @brief Interface for nodes that provide Principled BSDF material parameters.
 *
 * Implemented by geometry nodes (Sphere3D, Card3D, etc.) for inline materials,
 * and by Material3D for standalone reusable materials.
 *
 * CyclesRenderer queries this interface to create per-object ccl::Shader instances.
 */
class MaterialProvider
{
public:
    virtual ~MaterialProvider() {}

    virtual void getMaterialBaseColor(double time, double& r, double& g, double& b) const = 0;
    virtual double getMaterialRoughness(double time) const = 0;
    virtual double getMaterialMetallic(double time) const = 0;
    virtual double getMaterialSpecular(double time) const = 0;
    virtual void getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const = 0;
    virtual double getMaterialTransmission(double time) const = 0;
    virtual double getMaterialIOR(double time) const = 0;
    virtual std::string getMaterialTextureFile() const = 0;

    // PBR texture map slots (default: empty = no texture, use scalar value)
    virtual std::string getMaterialNormalMapFile() const { return std::string(); }
    virtual std::string getMaterialRoughnessMapFile() const { return std::string(); }
    virtual std::string getMaterialMetallicMapFile() const { return std::string(); }
    virtual std::string getMaterialEmissionMapFile() const { return std::string(); }
    virtual double getMaterialNormalStrength(double /*time*/) const { return 1.0; }

    // Texture colorspace (for color textures — diffuse, emission)
    // Returns Cycles-compatible colorspace string: "sRGB", "Linear", "ACEScg", "Raw", "Non-Color"
    virtual std::string getMaterialDiffuseColorspace() const { return "sRGB"; }
    virtual std::string getMaterialEmissionColorspace() const { return "sRGB"; }

    /** @brief Whether this node has a Material3D connected to its material input. */
    virtual bool hasMaterialInput() const { return false; }

    /** @brief Get the connected Material3D provider (if any). */
    virtual MaterialProvider* getConnectedMaterial() const { return nullptr; }
};

#endif // NATRON_ENGINE_MATERIALPROVIDER_H
