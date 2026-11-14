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

#ifndef NATRON_ENGINE_FLOWCORE_H
#define NATRON_ENGINE_FLOWCORE_H

#include <vector>

/**
 * @brief Dense optical flow between two frames: the motion engine shared by
 * VectorGenerator and TimeFlow.
 *
 * Deliberately free of any Natron type so it can be compiled and measured on
 * its own (tools/flow-tests). The estimator is in the family of Dense Inverse
 * Search (Kroeger et al. 2016): an image pyramid, and on each level a grid of
 * overlapping patches whose displacement is found by inverse-compositional
 * Lucas-Kanade starting from the coarser level's answer, blended into a dense
 * field and then smoothed across the field but not across image edges.
 */
namespace FlowCore {

struct Params
{
    // Resolution of the vector field relative to the image: 1 = a vector per pixel,
    // 0.5 = computed at half resolution and scaled up, and so on.
    double vectorDetail;

    // 0 = no smoothing of the field (detailed but noisy), 1 = heavy smoothing.
    double smoothness;

    // Side of the matching patch in pixels, at every pyramid level.
    int patchSize;

    // Maximum Lucas-Kanade iterations per patch.
    int iterations;

    // Compare patches after removing their mean, so a change of brightness
    // between the two frames is not mistaken for motion.
    bool meanNormalize;

    // Variational refinement after the patch search at every level: weights of the smoothness,
    // brightness and gradient terms (for 0..255 images; the smoothness weight is scaled by
    // `smoothness`), outer iterations and solver sweeps per iteration. 0 iterations = off.
    double refineAlpha, refineDelta, refineGamma;

    // Gradient strength (0..255 units per pixel) below which the refinement trusts the picture
    // less: soft shading and noise should not outvote real edges.
    double refineZeta;
    int refineIterations, refineSor;

    // Also run the edge-aware smoothing filter on the patch result before refining.
    bool patchSmooth;

    Params()
        : vectorDetail(0.5)
        , smoothness(0.5)
        , patchSize(8)
        , iterations(16)
        , meanNormalize(true)
        , refineAlpha(20.)
        , refineDelta(1.)
        , refineGamma(10.)
        , refineZeta(2.)
        , refineIterations(8)
        , refineSor(5)
        , patchSmooth(true)
    {}
};

/**
 * @brief Tone-compress a scene-linear value for matching: a display-like curve
 * below 1 and a logarithm above it, scaled to roughly 0..255.
 */
float matchEncode(float linear);

/**
 * @brief Computes the flow from image 0 to image 1.
 *
 * @param i0, i1  Single-channel images, width * height floats, row major, already
 *                passed through matchEncode (or any 0..255-like encoding).
 * @param flow    Receives 2 * width * height floats (u, v per pixel) such that
 *                i0(x, y) matches i1(x + u, y + v).
 * @param confidence  Optional, receives width * height values in 0..1.
 * @param abort   Optional, polled between pyramid levels; when it turns true the
 *                function returns false and @p flow is undefined.
 */
bool computeFlow(const float* i0,
                 const float* i1,
                 int width,
                 int height,
                 const Params& params,
                 std::vector<float>* flow,
                 std::vector<float>* confidence = 0,
                 const volatile bool* abort = 0);

/**
 * @brief Steadies a flow field over time using the fields of the neighbouring frames.
 *
 * Each frame pair is solved on its own, so where the picture says little (flat areas) the answer
 * lands somewhere slightly different every frame. This follows each pixel along its own motion
 * into the neighbouring fields and averages the vectors that agree with it; a neighbour that
 * disagrees (an occlusion, a cut) is left out.
 *
 * All fields have the same direction and layout as computeFlow's output. `ahead` is the field of
 * the frame the motion points to (the next frame for a forward field, the previous one for a
 * backward field), `behind` the one on the other side; either may be null.
 * @param strength 0 = `current` unchanged, 1 = neighbours count as much as the frame itself
 */
void temporalFilter(const std::vector<float>& current,
                    const std::vector<float>* ahead,
                    const std::vector<float>* behind,
                    int width,
                    int height,
                    double strength,
                    std::vector<float>* out);

/**
 * @brief How far each vector of a field can be trusted, from the field in the opposite direction.
 *
 * Following a pixel along `ab` into the other frame and then back along `ba` should land where it
 * started. Where it does not, the two solves disagree: typically something is being covered or
 * uncovered there, and there is no true vector to find.
 *
 * @param ab   field from frame A to frame B (computeFlow layout)
 * @param ba   field from frame B to frame A
 * @param confidence  receives width * height values, 1 = the round trip closes, towards 0 = it does not
 */
void flowConsistency(const std::vector<float>& ab,
                     const std::vector<float>& ba,
                     int width,
                     int height,
                     std::vector<float>* confidence);

/**
 * @brief The field in the opposite direction, derived from a field instead of solved from pictures:
 * used when vectors are supplied for one direction only (a renderer's motion pass).
 * @param ba   field from frame B to frame A
 * @param ab   receives the field from frame A to frame B
 */
void invertFlow(const std::vector<float>& ba,
                int width,
                int height,
                std::vector<float>* ab);

/**
 * @brief Builds the picture at a time between two frames by moving both along the motion.
 *
 * @param a, b   the two frames, width * height * nComp floats each (any number of channels)
 * @param ab, ba the fields from A to B and from B to A
 * @param confA, confB  flowConsistency of ab and of ba
 * @param position  0 = frame A, 1 = frame B
 * @param out    receives width * height * nComp floats
 *
 * Every output pixel is looked up in A (a fraction `position` of the way back along ab) and in B
 * (the rest of the way back along ba) and the two are mixed by how close each frame is in time.
 * Where only one of the two lookups can be trusted (the area was hidden in the other frame) that
 * one is used alone; where neither can, the result falls back to a plain dissolve.
 */
void interpolateFrame(const float* a,
                      const float* b,
                      int nComp,
                      int width,
                      int height,
                      const std::vector<float>& ab,
                      const std::vector<float>& ba,
                      const std::vector<float>& confA,
                      const std::vector<float>& confB,
                      double position,
                      float* out);

} // namespace FlowCore

#endif // NATRON_ENGINE_FLOWCORE_H
