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

#ifndef NATRON_ENGINE_CYCLESRENDERER_H
#define NATRON_ENGINE_CYCLESRENDERER_H

#include "../../../Global/Macros.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>

NATRON_NAMESPACE_ENTER

class SceneGraph;

/**
 * @brief Bridge between Natron's SceneGraph and Blender's Cycles renderer.
 *
 * Owns a ccl::Session and translates SceneGraph nodes into Cycles geometry.
 * Rendering happens on a background thread managed by Cycles internally.
 *
 * Usage:
 *   CyclesRenderer renderer;
 *   renderer.renderToBufferWithCamera(sceneGraph, camT/R/S, pixels, w, h, samples, time);
 */
class CyclesRenderer
{
public:
    CyclesRenderer();
    ~CyclesRenderer();

    /**
     * @brief Initialize the render session with given resolution.
     * Must be called before syncScene or startRender.
     */
    bool initialize(int width, int height, int samples = 64);

    /**
     * @brief Sync scene with raw camera parameters (used by CyclesRender node).
     * Position and rotation in world space, focal length and aperture for FOV.
     */
    void syncSceneWithCamera(const SceneGraph& sg,
                             double camTX, double camTY, double camTZ,
                             double camRX, double camRY, double camRZ,
                             double focalLength, double hAperture,
                             double time = 0);

    /**
     * @brief Start rendering (non-blocking). Cycles renders on its own thread.
     */
    void startRender();

    /**
     * @brief Cancel an in-progress render.
     */
    void cancelRender();

    /**
     * @brief Block until the current render finishes.
     */
    void waitForRender();

    /**
     * @brief Is a render currently in progress?
     */
    bool isRendering() const;

    /**
     * @brief Get the current render progress (0.0 - 1.0).
     */
    float getProgress() const;

    /**
     * @brief Get current sample count.
     */
    int getCurrentSample() const;

    /**
     * @brief Copy rendered pixels to an RGBA float buffer.
     * Buffer must be at least width * height * 4 floats.
     * Returns true if pixels were available.
     */
    bool getPixels(float* rgba, int width, int height) const;

    /**
     * @brief Render to a memory buffer (blocking) using raw camera parameters.
     */
    bool renderToBufferWithCamera(const SceneGraph& sg,
                                   double camTX, double camTY, double camTZ,
                                   double camRX, double camRY, double camRZ,
                                   double focalLength, double hAperture,
                                   std::vector<float>& outPixels,
                                   int width, int height, int samples = 64,
                                   double time = 0);

    /**
     * @brief Set the number of samples for the next render.
     */
    void setSamples(int samples);

    /**
     * @brief Enable/disable denoising.
     */
    void setDenoise(bool enabled);

    /**
     * @brief Get render width/height.
     */
    int getWidth() const;
    int getHeight() const;

    /**
     * @brief Quick smoke test — create and destroy a session.
     * Returns true if Cycles is functional.
     */
    static bool smokeTest();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CYCLESRENDERER_H
