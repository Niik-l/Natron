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

    /**
     * @brief Hash of the connected 2D input chains (Read -> Grade -> ... into
     * the Diffuse/Roughness/... inputs) at `time`. 0 when nothing is connected.
     *
     * CyclesRender folds this into its scene hash. Hashing the texture PATH is
     * not enough for connected inputs: those are baked to a temp file named
     * after the node, so the path is byte-identical no matter what the upstream
     * nodes do — grading the albedo would leave the hash unchanged and the
     * render cache would serve the previous frame.
     */
    virtual unsigned long long getMaterialInputsHash(double /*time*/) const { return 0; }

    // PBR texture map slots (default: empty = no texture, use scalar value)
    virtual std::string getMaterialNormalMapFile() const { return std::string(); }
    virtual std::string getMaterialRoughnessMapFile() const { return std::string(); }
    virtual std::string getMaterialMetallicMapFile() const { return std::string(); }
    virtual std::string getMaterialEmissionMapFile() const { return std::string(); }
    virtual std::string getMaterialTransmissionMapFile() const { return std::string(); }
    virtual double getMaterialNormalStrength(double /*time*/) const { return 1.0; }

    // Specular level + height map. Height is applied as BUMP (shading normals
    // only) — true displacement needs the mesh path to emit subdivision faces,
    // which it doesn't yet, so a DISPLACE_TRUE shader would render unchanged.
    virtual std::string getMaterialSpecularMapFile() const { return std::string(); }

    // Foliage pair. Opacity drives Principled's Alpha (atlas cutout); the
    // translucency map is mixed in as a Translucent BSDF for backlit thin
    // surfaces — deliberately not subsurface, which is noisy and slow on the
    // single-sided cards Megascans plants are built from.
    virtual std::string getMaterialOpacityMapFile() const { return std::string(); }
    // Some libraries ship the mask inverted and call it "Transparency".
    virtual bool getMaterialOpacityInvert() const { return false; }
    virtual std::string getMaterialTranslucencyMapFile() const { return std::string(); }
    virtual double getMaterialTranslucencyStrength(double /*time*/) const { return 1.0; }
    virtual std::string getMaterialDisplacementMapFile() const { return std::string(); }
    virtual double getMaterialDisplacementScale(double /*time*/) const { return 0.1; }
    virtual double getMaterialDisplacementMidlevel(double /*time*/) const { return 0.5; }

    // Texture colorspace (for color textures — diffuse, emission)
    // Returns Cycles-compatible colorspace string: "sRGB", "Linear", "ACEScg", "Raw", "Non-Color"
    virtual std::string getMaterialDiffuseColorspace() const { return "sRGB"; }
    /** True when the base-color texture's own alpha should cut the surface out
     *  (Cycles links the image node's Alpha output to Principled Alpha). Off by
     *  default: a JPEG albedo has no alpha and a PNG albedo with an alpha channel
     *  should not silently punch holes in a mesh. Card3D turns it on for the
     *  image it bakes from its img input, which is the whole point of a card. */
    virtual bool getMaterialTextureUsesAlpha() const { return false; }
    /** Called once per Cycles render request (CyclesPassRender) before the
     *  scene is built: shapes whose texture comes from an img input bake it to
     *  a file here (see MaterialTextureBake.h). No-op for file-based providers. */
    virtual void bakeImageInput(double /*time*/) {}
    virtual std::string getMaterialEmissionColorspace() const { return "sRGB"; }

    /** @brief Whether this node has a Material3D connected to its material input. */
    virtual bool hasMaterialInput() const { return false; }

    /** @brief Get the connected Material3D provider (if any). */
    virtual MaterialProvider* getConnectedMaterial() const { return nullptr; }
};

#endif // NATRON_ENGINE_MATERIALPROVIDER_H
