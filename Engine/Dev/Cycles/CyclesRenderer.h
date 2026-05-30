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

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <functional>

NATRON_NAMESPACE_ENTER

class SceneGraph;
struct ObjectVisibility;

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
    struct DOFParams {
        bool enabled = false;
        float apertureSize = 0.0f;  // pre-computed: focalLength / (2 * fstop)
        float focusDistance = 10.0f;
        int blades = 0;             // 0=circular, 3+=polygonal
        float bladeRotation = 0.0f; // radians
    };

    struct MotionBlurParams {
        bool enabled = false;
        float shutterTime = 0.5f;   // in frames
        int shutterPosition = 1;    // 0=Start, 1=Center, 2=End
    };

    struct IntegratorParams {
        int maxBounces = 7;
        int diffuseBounces = 7;
        int glossyBounces = 7;
        int transmissionBounces = 7;
        float aoFactor = 0.0f;       // 0 = disabled (proper GI), 1 = full AO
        int aoBounces = 0;
        float aoDistance = 10.0f;
    };

    void syncSceneWithCamera(const SceneGraph& sg,
                             double camTX, double camTY, double camTZ,
                             double camRX, double camRY, double camRZ,
                             double focalLength, double hAperture, double vAperture,
                             double time = 0,
                             const std::vector<std::string>& requestedPasses = std::vector<std::string>(),
                             const std::map<std::string, ObjectVisibility>* visibilityMap = nullptr,
                             const std::set<std::string>* activeLights = nullptr,
                             const DOFParams* dof = nullptr,
                             const MotionBlurParams* motionBlur = nullptr,
                             const IntegratorParams* integrator = nullptr);

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
                                   double focalLength, double hAperture, double vAperture,
                                   std::vector<float>& outPixels,
                                   int width, int height, int samples = 64,
                                   double time = 0);

    /**
     * @brief Render to memory buffers (blocking) with multiple AOV passes and light groups.
     *
     * @param requestedPasses  List of pass names to render (e.g. "Combined", "DiffDir", "Normal").
     *                         "Combined" is always included even if not listed.
     * @param outPassBuffers   Output map: pass name -> RGBA float pixel buffer (width*height*4).
     *                         Light group passes are named "Combined_<groupName>".
     */
    bool renderToBufferWithCameraMultiPass(const SceneGraph& sg,
                                            double camTX, double camTY, double camTZ,
                                            double camRX, double camRY, double camRZ,
                                            double focalLength, double hAperture, double vAperture,
                                            const std::vector<std::string>& requestedPasses,
                                            std::map<std::string, std::vector<float>>& outPassBuffers,
                                            int width, int height, int samples = 64,
                                            double time = 0,
                                            const std::map<std::string, ObjectVisibility>* visibilityMap = nullptr,
                                            const std::set<std::string>* activeLights = nullptr,
                                            const DOFParams* dof = nullptr,
                                            const MotionBlurParams* motionBlur = nullptr,
                                            const IntegratorParams* integrator = nullptr);

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
     * @brief Save pass buffers to a multi-layer EXR file using OIIO.
     * Each entry maps pass name → float buffer (w*h*4 floats).
     */
    /**
     * @brief Output settings overlay for saveMultiLayerEXR.
     * Strings are matched case-insensitively; unknown values fall back
     * to defaults so a user-typed JSON spec can't silently break the
     * write path.
     */
    struct ExrOutputOptions {
        // Pixel type. "32-bit Full" (default), "16-bit Half", or
        // "8-bit Integer" (ignored for EXR — falls back to half).
        std::string bitDepth;
        // EXR compression: ZIP (default), ZIPS, PIZ, DWAA, DWAB, RLE,
        // PXR24, B44, B44A, None.
        std::string compression;
    };

    // Existing 3-arg form kept for backward compatibility with the
    // manual "Save Multi-Layer EXR" button in CyclesRender.
    static bool saveMultiLayerEXR(const std::string& filepath,
                                   const std::map<std::string, std::vector<float>>& passBuffers,
                                   int width, int height);

    // Per-pass form — used by CyclesRenderPassManager so each pass can
    // carry its own bit-depth / compression spec from the JSON.
    static bool saveMultiLayerEXR(const std::string& filepath,
                                   const std::map<std::string, std::vector<float>>& passBuffers,
                                   int width, int height,
                                   const ExrOutputOptions& opts);

    /**
     * @brief Save a single 4-float RGBA buffer to PNG / TIFF / JPEG via OIIO.
     *
     * Dispatch is by file extension on `filepath` (.png / .tif / .tiff /
     * .jpg / .jpeg). The renderer always produces RGBA float; this writer
     * narrows to the destination format:
     *   - PNG  : 8 or 16 bit (per opts.bitDepth), RGBA if isCombined else RGB.
     *   - TIFF : 8/16/32 bit + ZIP/LZW/none compression, RGBA if isCombined else RGB.
     *   - JPEG : 8 bit RGB only; alpha always dropped; compression treated as quality 1-100.
     *
     * Y is flipped (Cycles bottom-up → image top-down). Returns false on
     * any OIIO open / write failure or unknown extension. The CyclesRender
     * Pass Manager uses this as the per-AOV save path when an output
     * filePath doesn't end in .exr.
     */
    static bool saveSingleImage(const std::string& filepath,
                                 const std::vector<float>& rgbaBuffer,
                                 int width, int height,
                                 bool isCombined,
                                 const ExrOutputOptions& opts);

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
