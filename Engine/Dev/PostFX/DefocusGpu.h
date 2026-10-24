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

#ifndef NATRON_ENGINE_DEFOCUSGPU_H
#define NATRON_ENGINE_DEFOCUSGPU_H

/**
 * DefocusGpu: DefocusCore's scatter step (5 in DefocusCore.h) on the GPU through wgpu (Vulkan /
 * DX12 / Metal), the same way FastVolumeRender uses it. Design: complex_tools/zdefocus/plan/
 * 03_gpu_defocus.md.
 *
 * The CPU engine still decodes, slices and fills, and builds every sample's kernel shape
 * (ProceduralLens::shape covers every lens type); this module draws the shapes and composites the
 * slices, and returns the accumulator DefocusCore's last step mixes into the output. Only compiled
 * with wgpu (DEFOCUS_HAVE_WGPU); otherwise available() says so and scatter() returns false.
 *
 * Pure C++17 without Natron or Qt, like DefocusCore (the testbed compiles both).
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace defocus {
namespace gpu {

// One kernel to draw (a sample, or one channel group of it with axial colour). 128 bytes, the
// layout of `struct Shape` in the WGSL (every member 4-byte aligned).
struct Shape
{
    float bx, by;           // centre in the target's cells (offset (coma) included)
    float ext;              // half height (KernelShape::yExtent)
    float norm;             // set on the GPU: energy / kernel area
    float inv[4];           // pixel -> unit disc
    float v[7];             // value: RGBA, guess, alpha for red, alpha for blue
    std::int32_t kx;        // the sample's cell column (sort key)
    float clip[12];         // up to 4 clip discs: x, y, r (unit coordinates)
    float energy;           // vignetting energy (1 = all the light)
    std::uint32_t flags;    // bit 0 point, bits 1-3 clip disc count, bit 4 offset (bilinear point)
    std::uint32_t ringOff;  // first (scale, weight) pair in Job::rings
    std::uint32_t nRings;
};
static_assert(sizeof(Shape) == 128, "gpu::Shape must match the WGSL struct");

enum ShapeFlags : std::uint32_t
{
    eShapePoint = 1u,
    eShapeOffset = 16u
};

// The shapes scattered into one buffer (full resolution or a level grid) for one slice.
struct Target
{
    int w = 0, h = 0;                   // cells
    int keyY0 = 0;                      // row key of rowStart[0]
    int reachX = 0, reachY = 0;         // furthest any shape here reaches from its cell
    std::vector<Shape> shapes;          // sorted by (row key, kx)
    std::vector<std::uint32_t> rowStart; // per row key: first shape; one extra entry at the end

    bool empty() const { return shapes.empty(); }
};

struct Slice
{
    int level = 0;                      // 2^level downsample of the level grid
    Target full, fullFill;              // full-res samples (level 0 cells, highlights), fills
    Target lvl, lvlFill;                // level-grid cells and fills (level > 0)
    int ilo = 0, jlo = 0;               // the level grid's first cell
    std::vector<float> lvlInside;       // mean source alpha per level cell (fills), lvl.w x lvl.h
};

struct Job
{
    int ow = 0, oh = 0;                 // output size
    int oxD = 0, oyD = 0;               // output origin minus the depth grid origin (level mapping)
    std::vector<float> inside;          // source alpha (0..1) per output pixel, ow x oh
    std::vector<float> constraints;     // the aperture: 8 floats each (nx, ny, h, disc, cx, cy, R, 0)
    std::vector<float> rings;           // (scale, weight) pairs
    std::vector<Slice> slices;          // nearest first, as DefocusCore's slices
};

// Whether a GPU can be used (creates the device on first use). why: the adapter name, or the
// reason there is none.
bool available(std::string* why);

// Draws and composites every slice back to front. acc: ow x oh x 7 floats (DefocusCore's kNumCh
// channels), premultiplied, as the CPU scatter leaves its accumulator. Returns false (err set)
// when the GPU can't do it; the caller then scatters on the CPU.
bool scatter(const Job& job, std::vector<float>& acc, std::string& err,
             const std::function<bool()>& aborted);

} // namespace gpu
} // namespace defocus

#endif // NATRON_ENGINE_DEFOCUSGPU_H
