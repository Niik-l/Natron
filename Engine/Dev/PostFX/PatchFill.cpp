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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "PatchFill.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

namespace {

enum OutputModeEnum
{
    eOutputResult = 0, // source with the fill blended in through the matte
    eOutputFill   = 1, // the lattice fill everywhere (inspect the ramp)
    eOutputMatte  = 2, // the blend matte
    eOutputDetail = 3, // the detail layer alone over mid grey
};

enum DetailSourceEnum
{
    eDetailSourceBand   = 0, // the ring outside the grown shape
    eDetailSourceRegion = 1, // a user rectangle in the source
    eDetailSourceInput  = 2, // the Detail input, at the hole's own position
};

// Small deterministic RNG (xorshift) so a Seed gives the same stamps every render.
struct Rng
{
    unsigned int s;
    explicit Rng(unsigned int seed) : s(seed * 2654435761u + 0x9E3779B9u) { if (!s) s = 1; next(); next(); }
    unsigned int next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }           // [0,1)
    float range(float a, float b) { return a + (b - a) * uniform(); }
};

inline float
smoothstep01(float edge0, float edge1, float x)
{
    if (edge1 <= edge0) return x >= edge0 ? 1.0f : 0.0f;
    float t = (x - edge0) / (edge1 - edge0);
    t = std::max(0.0f, std::min(1.0f, t));
    return t * t * (3.0f - 2.0f * t);
}

// Signed log so negative (and zero) HDR values survive the round trip.
inline float toLog(float v)   { return v >= 0.0f ? std::log1p(v) : -std::log1p(-v); }
inline float fromLog(float u) { return u >= 0.0f ? std::expm1(u) : -std::expm1(-u); }

// One band sample: position normalised to the lattice box, colour and weight.
struct Sample
{
    float fx, fy;
    float r, g, b;
    float w;
};

// A coarse RGB grid: known cells carry the average of the band samples that
// fell into them; unknown cells get solved.
struct Lattice
{
    int rows = 0, cols = 0;
    std::vector<float> v;      // rows*cols*3
    std::vector<char>  known;  // rows*cols
};

static void
accumulate(Lattice& L, const std::vector<Sample>& samples)
{
    const int n = L.rows * L.cols;
    L.v.assign(n * 3, 0.0f);
    L.known.assign(n, 0);
    std::vector<float> wsum(n, 0.0f);
    for (const Sample& s : samples) {
        int j = std::min(L.cols - 1, std::max(0, (int)(s.fx * L.cols)));
        int i = std::min(L.rows - 1, std::max(0, (int)(s.fy * L.rows)));
        int c = i * L.cols + j;
        L.v[c * 3 + 0] += s.r * s.w;
        L.v[c * 3 + 1] += s.g * s.w;
        L.v[c * 3 + 2] += s.b * s.w;
        wsum[c] += s.w;
    }
    for (int c = 0; c < n; ++c) {
        if (wsum[c] > 0.0f) {
            L.v[c * 3 + 0] /= wsum[c];
            L.v[c * 3 + 1] /= wsum[c];
            L.v[c * 3 + 2] /= wsum[c];
            L.known[c] = 1;
        }
    }
}

// Bilinear read of a lattice at normalised coordinates (cell centres at (j+0.5)/cols).
static void
sampleLattice(const Lattice& L, float fx, float fy, float* out)
{
    float u = fx * L.cols - 0.5f;
    float v = fy * L.rows - 0.5f;
    u = std::max(0.0f, std::min((float)(L.cols - 1), u));
    v = std::max(0.0f, std::min((float)(L.rows - 1), v));
    int j0 = (int)u, i0 = (int)v;
    int j1 = std::min(L.cols - 1, j0 + 1), i1 = std::min(L.rows - 1, i0 + 1);
    float tu = u - j0, tv = v - i0;
    const float* a = &L.v[(i0 * L.cols + j0) * 3];
    const float* b = &L.v[(i0 * L.cols + j1) * 3];
    const float* c = &L.v[(i1 * L.cols + j0) * 3];
    const float* d = &L.v[(i1 * L.cols + j1) * 3];
    for (int k = 0; k < 3; ++k) {
        float top = a[k] + (b[k] - a[k]) * tu;
        float bot = c[k] + (d[k] - c[k]) * tu;
        out[k] = top + (bot - top) * tv;
    }
}

// Gauss-Seidel relaxation of the unknown cells: each becomes the mean of its
// 4-neighbours (fewer at the grid border, which acts as a free edge). Known
// cells are fixed. Over-relaxed a little for speed; stops when nothing moves.
static void
relax(Lattice& L, int maxIters, float tol)
{
    const float omega = 1.5f;
    for (int it = 0; it < maxIters; ++it) {
        float maxDelta = 0.0f;
        for (int i = 0; i < L.rows; ++i) {
            for (int j = 0; j < L.cols; ++j) {
                int c = i * L.cols + j;
                if (L.known[c]) continue;
                float sum[3] = {0, 0, 0};
                int cnt = 0;
                if (i > 0)          { const float* p = &L.v[(c - L.cols) * 3]; sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; ++cnt; }
                if (i < L.rows - 1) { const float* p = &L.v[(c + L.cols) * 3]; sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; ++cnt; }
                if (j > 0)          { const float* p = &L.v[(c - 1) * 3];      sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; ++cnt; }
                if (j < L.cols - 1) { const float* p = &L.v[(c + 1) * 3];      sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; ++cnt; }
                if (!cnt) continue;
                float* q = &L.v[c * 3];
                for (int k = 0; k < 3; ++k) {
                    float target = sum[k] / cnt;
                    float nv = q[k] + omega * (target - q[k]);
                    maxDelta = std::max(maxDelta, std::fabs(nv - q[k]));
                    q[k] = nv;
                }
            }
        }
        if (maxDelta < tol) break;
    }
}

// Cascadic solve: relax a chain of coarser lattices first and use each as the
// starting guess for the next, so a large fine grid converges in a few passes.
static void
solve(Lattice& fine, const std::vector<Sample>& samples)
{
    std::vector<std::pair<int, int> > sizes; // finest first
    int r = fine.rows, c = fine.cols;
    sizes.push_back(std::make_pair(r, c));
    while (r > 2 || c > 2) {
        r = std::max(1, (r + 1) / 2);
        c = std::max(1, (c + 1) / 2);
        sizes.push_back(std::make_pair(r, c));
    }

    // Scale for the convergence tolerance: the spread of the known values.
    float vmax = 0.0f;
    for (const Sample& s : samples) {
        vmax = std::max(vmax, std::max(std::fabs(s.r), std::max(std::fabs(s.g), std::fabs(s.b))));
    }
    const float tol = 1e-5f * (vmax + 1e-3f);

    Lattice prev;
    for (int lv = (int)sizes.size() - 1; lv >= 0; --lv) {
        Lattice L;
        L.rows = sizes[lv].first;
        L.cols = sizes[lv].second;
        accumulate(L, samples);
        if (prev.rows == 0) {
            // Coarsest level: start unknowns at the mean of the knowns.
            double mean[3] = {0, 0, 0};
            int nk = 0;
            for (int k = 0; k < L.rows * L.cols; ++k) {
                if (L.known[k]) { mean[0] += L.v[k * 3]; mean[1] += L.v[k * 3 + 1]; mean[2] += L.v[k * 3 + 2]; ++nk; }
            }
            if (nk) { mean[0] /= nk; mean[1] /= nk; mean[2] /= nk; }
            for (int k = 0; k < L.rows * L.cols; ++k) {
                if (!L.known[k]) { L.v[k * 3] = (float)mean[0]; L.v[k * 3 + 1] = (float)mean[1]; L.v[k * 3 + 2] = (float)mean[2]; }
            }
        } else {
            for (int i = 0; i < L.rows; ++i) {
                for (int j = 0; j < L.cols; ++j) {
                    int k = i * L.cols + j;
                    if (L.known[k]) continue;
                    sampleLattice(prev, (j + 0.5f) / L.cols, (i + 0.5f) / L.rows, &L.v[k * 3]);
                }
            }
        }
        int iters = (lv == 0) ? std::min(4000, 200 + 20 * std::max(L.rows, L.cols)) : 400;
        relax(L, iters, tol);
        prev.rows = L.rows; prev.cols = L.cols;
        prev.v.swap(L.v); prev.known.swap(L.known);
    }
    fine.v.swap(prev.v);
    fine.known.swap(prev.known);
}


// A float RGB buffer over a pixel rectangle, with a per-pixel validity flag.
struct Plane
{
    int x1 = 0, y1 = 0, w = 0, h = 0;
    std::vector<float> rgb;    // w*h*3
    std::vector<char>  valid;  // w*h
    bool inside(int x, int y) const { return x >= x1 && x < x1 + w && y >= y1 && y < y1 + h; }
    size_t idx(int x, int y) const { return (size_t)(y - y1) * w + (x - x1); }
};

// Separable box blur of a plane that ignores invalid pixels (normalised by
// the count of valid taps), so a hole never leaks into the low-pass.
static void
boxBlurValid(const Plane& in, int r, std::vector<float>& out)
{
    const int w = in.w, h = in.h;
    std::vector<float> tmp((size_t)w * h * 3, 0.0f), tw((size_t)w * h, 0.0f);
    out.assign((size_t)w * h * 3, 0.0f);
    // horizontal: running sums of valid taps
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float acc[3] = {0, 0, 0}; int n = 0;
            for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k) {
                const size_t i = (size_t)y * w + k;
                if (!in.valid[i]) continue;
                acc[0] += in.rgb[i * 3]; acc[1] += in.rgb[i * 3 + 1]; acc[2] += in.rgb[i * 3 + 2]; ++n;
            }
            const size_t o = (size_t)y * w + x;
            tmp[o * 3] = acc[0]; tmp[o * 3 + 1] = acc[1]; tmp[o * 3 + 2] = acc[2]; tw[o] = (float)n;
        }
    }
    // vertical
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            float acc[3] = {0, 0, 0}, n = 0;
            for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k) {
                const size_t i = (size_t)k * w + x;
                acc[0] += tmp[i * 3]; acc[1] += tmp[i * 3 + 1]; acc[2] += tmp[i * 3 + 2]; n += tw[i];
            }
            const size_t o = (size_t)y * w + x;
            if (n > 0) { out[o * 3] = acc[0] / n; out[o * 3 + 1] = acc[1] / n; out[o * 3 + 2] = acc[2] / n; }
        }
    }
}

struct DetailParams
{
    int   source = eDetailSourceBand;
    float stampSize = 32.0f;     // px, diameter
    float density = 2.0f;        // stamps per stamp-area of fill
    float rotVar = 180.0f;       // degrees, +/-
    float scaleVar = 0.3f;       // fraction, +/-
    bool  flip = true;
    int   freq = 8;              // high-pass box radius, px
    int   seed = 1;
    bool  nearest = false;       // sample stamps with nearest-neighbour (crisp grain) instead of bilinear
    RectI region;                // eDetailSourceRegion: source rectangle (pixels)
};

// Scatter high-passed clone stamps over the fill pixels of `work`. `src` is the
// plane to clone from (its validity already excludes the hole). `alpha` and
// `dist` are the work-region matte and distance maps. Writes rgb detail
// (work-region sized, 3 floats) and a coverage per pixel.
static void
buildDetailLayer(const DetailParams& P, const Plane& src, const RectI& work,
                 const std::vector<float>& alpha, const std::vector<float>& dist, float grow,
                 std::vector<float>& detail, std::vector<float>& cover)
{
    const int WW = work.width(), WH = work.height();
    detail.assign((size_t)WW * WH * 3, 0.0f);
    cover.assign((size_t)WW * WH, 0.0f);
    if (src.w <= 0 || src.h <= 0) return;

    // high-pass source: rgb - box blur
    std::vector<float> lp;
    boxBlurValid(src, std::max(1, P.freq), lp);

    // candidate target pixels (fill) and, for Band, candidate source centres
    std::vector<int> targets;
    targets.reserve((size_t)WW * WH / 4);
    for (int y = 0; y < WH; ++y)
        for (int x = 0; x < WW; ++x)
            if (alpha[(size_t)y * WW + x] > 0.0f) targets.push_back(y * WW + x);
    if (targets.empty()) return;

    std::vector<int> bandCentres;
    if (P.source == eDetailSourceBand) {
        // a rotated, down-scaled stamp reaches ~0.85 stamp widths from its centre:
        // keep the centres a full width clear of the hole so patches stay valid
        const float inner = grow + P.stampSize * 1.1f;
        const float outer = grow + P.stampSize * 3.0f;
        for (int y = 0; y < WH; ++y)
            for (int x = 0; x < WW; ++x) {
                const float d = dist[(size_t)y * WW + x];
                if (d > inner && d <= outer) bandCentres.push_back(y * WW + x);
            }
        if (bandCentres.empty()) {
            // thin margins: fall back to anything outside the hole
            for (int y = 0; y < WH; ++y)
                for (int x = 0; x < WW; ++x)
                    if (dist[(size_t)y * WW + x] > grow) bandCentres.push_back(y * WW + x);
        }
        if (bandCentres.empty()) return;
    }

    const float R = std::max(1.0f, P.stampSize * 0.5f);
    const int nStamps = std::min(200000, std::max(1, (int)std::ceil(P.density * targets.size() / (M_PI * R * R))));
    Rng rng((unsigned int)P.seed);

    auto sampleHP = [&](float sx, float sy, float* out) -> float {
        // high-pass sample of the source plane; weight 0 when any tap is invalid
        if (P.nearest) {
            const int px = (int)std::floor(sx), py = (int)std::floor(sy);
            if (!src.inside(px, py)) return 0.0f;
            const size_t k = src.idx(px, py);
            if (!src.valid[k]) return 0.0f;
            out[0] = src.rgb[k * 3] - lp[k * 3];
            out[1] = src.rgb[k * 3 + 1] - lp[k * 3 + 1];
            out[2] = src.rgb[k * 3 + 2] - lp[k * 3 + 2];
            return 1.0f;
        }
        const int x0 = (int)std::floor(sx - 0.5f), y0 = (int)std::floor(sy - 0.5f);
        const float tx = sx - 0.5f - x0, ty = sy - 0.5f - y0;
        float acc[3] = {0, 0, 0}, wsum = 0.0f;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const int px = x0 + i, py = y0 + j;
                if (!src.inside(px, py)) continue;
                const size_t k = src.idx(px, py);
                if (!src.valid[k]) return 0.0f;
                const float w = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                acc[0] += w * (src.rgb[k * 3]     - lp[k * 3]);
                acc[1] += w * (src.rgb[k * 3 + 1] - lp[k * 3 + 1]);
                acc[2] += w * (src.rgb[k * 3 + 2] - lp[k * 3 + 2]);
                wsum += w;
            }
        if (wsum < 0.999f) return 0.0f;
        out[0] = acc[0]; out[1] = acc[1]; out[2] = acc[2];
        return 1.0f;
    };

    auto placeStamp = [&](int t) {
        const float tcx = work.x1 + (t % WW) + 0.5f, tcy = work.y1 + (t / WW) + 0.5f;
        float scx, scy;
        if (P.source == eDetailSourceBand) {
            const int c = bandCentres[(size_t)(rng.uniform() * bandCentres.size()) % bandCentres.size()];
            scx = work.x1 + (c % WW) + 0.5f; scy = work.y1 + (c / WW) + 0.5f;
        } else if (P.source == eDetailSourceRegion) {
            scx = P.region.x1 + rng.uniform() * std::max(1, P.region.width());
            scy = P.region.y1 + rng.uniform() * std::max(1, P.region.height());
        } else {
            // Detail input: the hole's own position, jittered by up to a stamp
            scx = tcx + rng.range(-R, R); scy = tcy + rng.range(-R, R);
        }
        const float ang = rng.range(-P.rotVar, P.rotVar) * (float)(M_PI / 180.0);
        const float sc  = std::max(0.1f, 1.0f + rng.range(-P.scaleVar, P.scaleVar));
        const float fx  = (P.flip && rng.uniform() < 0.5f) ? -1.0f : 1.0f;
        const float ca = std::cos(ang), sa = std::sin(ang);
        const float Rt = R * rng.range(0.8f, 1.2f);                 // stamp footprint on the target
        const int bx0 = (int)std::floor(tcx - Rt), bx1 = (int)std::ceil(tcx + Rt);
        const int by0 = (int)std::floor(tcy - Rt), by1 = (int)std::ceil(tcy + Rt);
        for (int py = by0; py <= by1; ++py) {
            if (py < work.y1 || py >= work.y2) continue;
            for (int px = bx0; px <= bx1; ++px) {
                if (px < work.x1 || px >= work.x2) continue;
                const size_t k = (size_t)(py - work.y1) * WW + (px - work.x1);
                if (alpha[k] <= 0.0f) continue;
                const float dx = px + 0.5f - tcx, dy = py + 0.5f - tcy;
                const float rr = (dx * dx + dy * dy) / (Rt * Rt);
                if (rr >= 1.0f) continue;
                // flat core, soft rim: most of the stamp replaces outright so overlapping
                // stamps don't average each other's texture away
                const float w = rr <= 0.45f ? 1.0f : smoothstep01(1.0f, 0.45f, rr);
                // target offset -> source offset: flip, rotate, scale
                const float ox = fx * dx, oy = dy;
                const float sx = scx + (ca * ox - sa * oy) / sc;
                const float sy = scy + (sa * ox + ca * oy) / sc;
                float hp[3];
                if (sampleHP(sx, sy, hp) <= 0.0f) continue;
                float* d = &detail[k * 3];
                d[0] += (hp[0] - d[0]) * w;                                 // "over": later stamps win
                d[1] += (hp[1] - d[1]) * w;
                d[2] += (hp[2] - d[2]) * w;
                cover[k] += (1.0f - cover[k]) * w;
            }
        }
    };

    // pass 1: random scatter
    for (int n = 0; n < nStamps; ++n) {
        placeStamp(targets[(size_t)(rng.uniform() * targets.size()) % targets.size()]);
    }
    // pass 2: coverage. Random placement leaves Poisson gaps and a stamp whose
    // source patch touched the hole was dropped, so visit the fill pixels in a
    // random order and stamp wherever the detail is still thin.
    std::vector<int> order(targets);
    for (size_t i = order.size(); i > 1; --i) {
        std::swap(order[i - 1], order[(size_t)(rng.uniform() * i) % i]);
    }
    int extra = 0;
    const int extraCap = nStamps * 4 + 1000;
    for (size_t i = 0; i < order.size() && extra < extraCap; ++i) {
        if (cover[(size_t)order[i]] < 0.6f) {
            placeStamp(order[i]);
            ++extra;
        }
    }
}

} // anonymous namespace

struct PatchFillPrivate
{
    KnobIntWPtr    rows;
    KnobIntWPtr    cols;
    KnobDoubleWPtr grow;
    KnobDoubleWPtr band;
    KnobDoubleWPtr feather;
    KnobBoolWPtr   logSpace;
    KnobChoiceWPtr outputMode;
    KnobDoubleWPtr mix;
    // Detail page
    KnobDoubleWPtr detailAmount;
    KnobChoiceWPtr detailSource;
    KnobDoubleWPtr stampSize, stampDensity, rotationVar, scaleVar;
    KnobBoolWPtr   flipStamps;
    KnobIntWPtr    detailFrequency, detailSeed;
    KnobChoiceWPtr stampFilter;
    KnobDoubleWPtr regionCenter, regionSize;
};

PatchFill::PatchFill(NodePtr node)
    : EffectInstance(node)
    , _imp(new PatchFillPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

PatchFill::~PatchFill()
{
}

std::string
PatchFill::getPluginDescription() const
{
    return "Paint out a region with a smooth fill matched to its surroundings. "
           "Draw the object with a Roto node, plug it into Mask, and the shape is replaced by a "
           "ramp pinned to the pixels just outside it: a fast way to remove lights from an HDRI, "
           "markers from a wall or dust from a plate.\n\n"
           "The mask's bounding box is divided into a lattice of Rows x Columns cells. Cells touching "
           "the Sample Band (a ring just outside the grown mask) take the average of those pixels; the "
           "inner cells are solved so every one is the mean of its neighbours, which gives a surface that "
           "matches the surroundings on every side. Few cells = a broad ramp, many cells = the fill "
           "follows the surroundings more closely. Nothing is cloned, so texture is not reproduced.\n\n"
           "Works in float, so HDR values are kept. Use Grow Mask to push the sample band past a "
           "light's halo, and Log Space when a bright rim bleeds into the patch.\n\n"
           "Detail tab: the ramp is only ever smooth, so Detail Amount above zero scatters soft "
           "clone stamps over the fill, each a high-passed patch taken at a random position from "
           "the ring outside the shape (or a rectangle you point at, or the Detail input) with "
           "random rotation, scale and flip, like texture bombing. The ramp keeps brightness and "
           "colour, the stamps bring the texture back.";
}

void
PatchFill::addAcceptedComponents(int /*inputNb*/,
                                 std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
}

void
PatchFill::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
PatchFill::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
PatchFill::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Lattice Rows"));
        k->setName("rows");
        k->setDefaultValue(8);
        k->setMinimum(1); k->setMaximum(256);
        k->setDisplayMinimum(1); k->setDisplayMaximum(64);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Number of lattice cells down the mask's bounding box. Fewer = a broader, "
                             "smoother ramp; more = the fill follows the surrounding pixels more closely."));
        page->addKnob(k); _imp->rows = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Lattice Columns"));
        k->setName("columns");
        k->setDefaultValue(8);
        k->setMinimum(1); k->setMaximum(256);
        k->setDisplayMinimum(1); k->setDisplayMaximum(64);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Number of lattice cells across the mask's bounding box."));
        page->addKnob(k); _imp->cols = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Grow Mask"));
        k->setName("growMask");
        k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(10000.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Expand the mask by this many pixels before sampling and filling. Use it to "
                             "push the sample band past a light's glow or a marker's shadow so the fill "
                             "is pinned to clean pixels and the halo is replaced too."));
        page->addKnob(k); _imp->grow = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Sample Band"));
        k->setName("sampleBand");
        k->setDefaultValue(4.0);
        k->setMinimum(1.0); k->setMaximum(10000.0);
        k->setDisplayMinimum(1.0); k->setDisplayMaximum(50.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Width in pixels of the ring just outside the (grown) mask whose pixels are "
                             "averaged to pin the fill. Wider = less sensitive to noise and film grain, "
                             "narrower = tighter match to the edge."));
        page->addKnob(k); _imp->band = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Feather"));
        k->setName("feather");
        k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(10000.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Soften the edge of the fill over this many pixels outside the grown mask. "
                             "The Roto shape's own feather is honoured as well."));
        page->addKnob(k); _imp->feather = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Log Space"));
        k->setName("logSpace");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Interpolate in logarithmic space. A bright rim then falls off like light "
                             "does instead of washing a linear ramp across the patch. Try it when the "
                             "fill looks too bright near a light."));
        page->addKnob(k); _imp->logSpace = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Output"));
        k->setName("outputMode");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("result", "Result", "The source with the fill blended in through the matte."));
        opts.push_back(ChoiceOption("fill",   "Fill",   "The lattice fill on its own, over the whole image, to inspect the ramp."));
        opts.push_back(ChoiceOption("matte",  "Matte",  "The blend matte (grown + feathered mask) in all channels."));
        opts.push_back(ChoiceOption("detail", "Detail", "The detail layer alone over mid grey, to judge the stamps."));
        k->populateChoices(opts);
        k->setDefaultValue(eOutputResult);
        k->setHintToolTip(tr("What to output. Fill and Matte are for checking the setup."));
        page->addKnob(k); _imp->outputMode = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
        k->setName("mix");
        k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Blend between the source (0) and the filled result (1)."));
        page->addKnob(k); _imp->mix = k;
    }

    // ---- Detail page: texture-bombed clone stamps over the ramp
    KnobPagePtr dpage = AppManager::createKnob<KnobPage>(this, tr("Detail"));
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Detail Amount"));
        k->setName("detailAmount");
        k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(10.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("How much high-frequency texture to stamp over the fill. 0 = the smooth ramp "
                             "only, 1 = stamps at their natural contrast, above 1 exaggerates them."));
        dpage->addKnob(k); _imp->detailAmount = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Detail Source"));
        k->setName("detailSource");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("band",   "Band",         "Patches come from the ring just outside the grown shape."));
        opts.push_back(ChoiceOption("region", "Region",       "Patches come from the Region rectangle (a clean area of the source)."));
        opts.push_back(ChoiceOption("input",  "Detail input", "Patches come from the Detail input at the hole's own position (a grain or texture plate)."));
        k->populateChoices(opts);
        k->setDefaultValue(eDetailSourceBand);
        k->setHintToolTip(tr("Where the clone stamps are taken from."));
        dpage->addKnob(k); _imp->detailSource = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Region Center"), 2);
        k->setName("regionCenter");
        k->setDefaultValue(0.0, 0); k->setDefaultValue(0.0, 1);
        k->setDimensionName(0, "x"); k->setDimensionName(1, "y");
        k->setDisplayMinimum(-10000.0, 0); k->setDisplayMaximum(10000.0, 0);
        k->setDisplayMinimum(-10000.0, 1); k->setDisplayMaximum(10000.0, 1);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Region source: centre of the rectangle to clone from, in pixels."));
        dpage->addKnob(k); _imp->regionCenter = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Region Size"), 2);
        k->setName("regionSize");
        k->setDefaultValue(200.0, 0); k->setDefaultValue(200.0, 1);
        k->setDimensionName(0, "w"); k->setDimensionName(1, "h");
        k->setMinimum(1.0, 0); k->setMinimum(1.0, 1);
        k->setDisplayMinimum(1.0, 0); k->setDisplayMaximum(4000.0, 0);
        k->setDisplayMinimum(1.0, 1); k->setDisplayMaximum(4000.0, 1);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Region source: size of the rectangle to clone from, in pixels."));
        dpage->addKnob(k); _imp->regionSize = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Stamp Size"));
        k->setName("stampSize");
        k->setDefaultValue(32.0);
        k->setMinimum(2.0); k->setMaximum(4096.0);
        k->setDisplayMinimum(4.0); k->setDisplayMaximum(256.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Diameter of each clone stamp in pixels. Smaller stamps break up large "
                             "features more; larger ones keep bigger texture elements intact."));
        dpage->addKnob(k); _imp->stampSize = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Stamp Density"));
        k->setName("stampDensity");
        k->setDefaultValue(2.0);
        k->setMinimum(0.1); k->setMaximum(50.0);
        k->setDisplayMinimum(0.5); k->setDisplayMaximum(8.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Stamps per stamp-area of fill. 1 just about covers the shape; 2-4 overlaps "
                             "so no soft stamp edges show."));
        dpage->addKnob(k); _imp->stampDensity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotation Variation"));
        k->setName("rotationVariation");
        k->setDefaultValue(180.0);
        k->setMinimum(0.0); k->setMaximum(180.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(180.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Each stamp is rotated by a random angle up to this many degrees either way. "
                             "0 keeps directional textures (brick, wood grain) aligned."));
        dpage->addKnob(k); _imp->rotationVar = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Variation"));
        k->setName("scaleVariation");
        k->setDefaultValue(0.3);
        k->setMinimum(0.0); k->setMaximum(0.9);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(0.9);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Each stamp is scaled by a random factor within 1 +/- this."));
        dpage->addKnob(k); _imp->scaleVar = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Flip Stamps"));
        k->setName("flipStamps");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("Mirror half of the stamps at random for more variety."));
        dpage->addKnob(k); _imp->flipStamps = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Detail Frequency"));
        k->setName("detailFrequency");
        k->setDefaultValue(8);
        k->setMinimum(1); k->setMaximum(512);
        k->setDisplayMinimum(1); k->setDisplayMaximum(64);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Radius in pixels of the blur that splits texture from tone before stamping. "
                             "Only features smaller than this are cloned; the ramp supplies the rest. "
                             "Raise it to carry larger texture elements, lower it for grain only."));
        dpage->addKnob(k); _imp->detailFrequency = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Stamp Filter"));
        k->setName("stampFilter");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("bilinear", "Bilinear", "Smooth resampling of each stamp. Best for surfaces and textures."));
        opts.push_back(ChoiceOption("nearest",  "Nearest",  "No resampling: pixels are copied whole, so grain stays crisp."));
        k->populateChoices(opts);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("How a rotated / scaled stamp is resampled. Bilinear slightly softens fine grain; Nearest keeps it."));
        dpage->addKnob(k); _imp->stampFilter = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Seed"));
        k->setName("detailSeed");
        k->setDefaultValue(1);
        k->setMinimum(0); k->setMaximum(100000);
        k->setDisplayMinimum(0); k->setDisplayMaximum(100);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Random seed for the stamp placement. Animate it for per-frame variation."));
        dpage->addKnob(k); _imp->detailSeed = k;
    }
}

StatusEnum
PatchFill::getRegionOfDefinition(U64 /*hash*/,
                                 double time,
                                 const RenderScale& scale,
                                 ViewIdx view,
                                 RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProject;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
}

StatusEnum
PatchFill::render(const RenderActionArgs& args)
{
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;

    RectI maskRoi;
    ImagePtr maskImg = getInput(1)
                     ? getImage(1, args.time, args.mappedScale, args.view,
                                NULL, NULL, false, false,
                                eStorageModeRAM, 0, &maskRoi)
                     : ImagePtr();

    RectI detRoi;
    ImagePtr detImg = getInput(2)
                    ? getImage(2, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &detRoi)
                    : ImagePtr();

    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    const double pxScale = args.mappedScale.toOfxPointD().x;
    const int   rows     = std::max(1, _imp->rows.lock()->getValueAtTime(args.time));
    const int   cols     = std::max(1, _imp->cols.lock()->getValueAtTime(args.time));
    const float grow     = (float)(std::max(0.0, _imp->grow.lock()->getValueAtTime(args.time)) * pxScale);
    const float band     = (float)std::max(1.0, std::max(1.0, _imp->band.lock()->getValueAtTime(args.time)) * pxScale);
    const float feather  = (float)(std::max(0.0, _imp->feather.lock()->getValueAtTime(args.time)) * pxScale);
    const bool  logSpace = _imp->logSpace.lock()->getValueAtTime(args.time) != 0;
    const int   outMode  = _imp->outputMode.lock()->getValue();
    const float mixVal   = (float)_imp->mix.lock()->getValueAtTime(args.time);

    DetailParams DP;
    const float detailAmount = (float)std::max(0.0, _imp->detailAmount.lock()->getValueAtTime(args.time));
    DP.source    = _imp->detailSource.lock()->getValue();
    DP.stampSize = (float)(std::max(2.0, _imp->stampSize.lock()->getValueAtTime(args.time)) * pxScale);
    DP.density   = (float)std::max(0.1, _imp->stampDensity.lock()->getValueAtTime(args.time));
    DP.rotVar    = (float)_imp->rotationVar.lock()->getValueAtTime(args.time);
    DP.scaleVar  = (float)_imp->scaleVar.lock()->getValueAtTime(args.time);
    DP.flip      = _imp->flipStamps.lock()->getValueAtTime(args.time) != 0;
    DP.freq      = std::max(1, (int)std::lround(_imp->detailFrequency.lock()->getValueAtTime(args.time) * pxScale));
    DP.seed      = _imp->detailSeed.lock()->getValueAtTime(args.time);
    DP.nearest   = _imp->stampFilter.lock()->getValue() == 1;
    {
        KnobDoublePtr rc = _imp->regionCenter.lock(), rs = _imp->regionSize.lock();
        const double cx = rc->getValueAtTime(args.time, 0) * pxScale, cy = rc->getValueAtTime(args.time, 1) * pxScale;
        const double rw = std::max(1.0, rs->getValueAtTime(args.time, 0) * pxScale), rh = std::max(1.0, rs->getValueAtTime(args.time, 1) * pxScale);
        DP.region.x1 = (int)std::floor(cx - rw / 2); DP.region.x2 = (int)std::ceil(cx + rw / 2);
        DP.region.y1 = (int)std::floor(cy - rh / 2); DP.region.y2 = (int)std::ceil(cy + rh / 2);
    }
    const bool wantDetail = detailAmount > 0.0f || outMode == eOutputDetail;

    const RectI srcBounds  = srcImg->getBounds();
    const RectI outBounds  = outImg->getBounds();
    const RectI maskBounds = maskImg ? maskImg->getBounds() : RectI();

    const int srcNComp  = srcImg->getComponents().getNumComponents();
    const int outNComp  = std::min(srcNComp, 4);
    const int maskNComp = maskImg ? maskImg->getComponents().getNumComponents() : 0;
    const RectI detBounds = detImg ? detImg->getBounds() : RectI();
    const int detNComp = detImg ? detImg->getComponents().getNumComponents() : 0;
    std::unique_ptr<Image::ReadAccess> detRa;
    if (detImg) detRa.reset(new Image::ReadAccess(detImg.get()));

    Image::ReadAccess  srcRa(srcImg.get());
    Image::WriteAccess wa(outImg.get());
    std::unique_ptr<Image::ReadAccess> maskRa;
    if (maskImg) maskRa.reset(new Image::ReadAccess(maskImg.get()));

    // ---- 1. The mask over the output window, and the hard shape's bounding box.
    RectI seed; // bbox of pixels with mask >= 0.5
    bool haveSeed = false;
    if (maskImg && maskNComp > 0) {
        RectI scan = outBounds;
        scan = scan.intersect(maskBounds);
        for (int y = scan.y1; y < scan.y2; ++y) {
            for (int x = scan.x1; x < scan.x2; ++x) {
                const float* mp = (const float*)maskRa->pixelAt(x, y);
                if (!mp) continue;
                if (mp[maskNComp - 1] >= 0.5f) {
                    if (!haveSeed) { seed.x1 = seed.x2 = x; seed.y1 = seed.y2 = y; haveSeed = true; }
                    seed.x1 = std::min(seed.x1, x); seed.x2 = std::max(seed.x2, x);
                    seed.y1 = std::min(seed.y1, y); seed.y2 = std::max(seed.y2, y);
                }
            }
        }
    }
    if (haveSeed) { seed.x2 += 1; seed.y2 += 1; } // make it exclusive

    // Working region: the shape grown by everything that reaches outside it.
    RectI work;
    int pad = (int)std::ceil(grow + band + feather) + 2;
    if (wantDetail) pad += (int)std::ceil(DP.stampSize * 3.5f) + DP.freq;
    if (haveSeed) {
        work.x1 = seed.x1 - pad; work.x2 = seed.x2 + pad;
        work.y1 = seed.y1 - pad; work.y2 = seed.y2 + pad;
        work = work.intersect(outBounds);
    }
    const int WW = haveSeed ? work.width() : 0;
    const int WH = haveSeed ? work.height() : 0;

    std::vector<float> mval, dist, alpha;
    std::vector<float> detailRGB, detailCover;
    bool haveDetail = false;
    Lattice lattice;
    RectI box; // lattice box: grown shape + band
    bool haveFill = false;

    if (haveSeed && WW > 0 && WH > 0) {
        mval.assign((size_t)WW * WH, 0.0f);
        dist.assign((size_t)WW * WH, 1e9f);
        for (int y = 0; y < WH; ++y) {
            for (int x = 0; x < WW; ++x) {
                const int px = work.x1 + x, py = work.y1 + y;
                const float* mp = (px >= maskBounds.x1 && px < maskBounds.x2 && py >= maskBounds.y1 && py < maskBounds.y2)
                                ? (const float*)maskRa->pixelAt(px, py) : NULL;
                float m = mp ? std::max(0.0f, std::min(1.0f, mp[maskNComp - 1])) : 0.0f;
                mval[(size_t)y * WW + x] = m;
                if (m >= 0.5f) dist[(size_t)y * WW + x] = 0.0f;
            }
        }

        // ---- 2. Chamfer (3-4) distance to the hard shape, in pixels.
        for (int y = 0; y < WH; ++y) {
            for (int x = 0; x < WW; ++x) {
                float& d = dist[(size_t)y * WW + x];
                if (x > 0)          d = std::min(d, dist[(size_t)y * WW + x - 1] + 3.0f);
                if (y > 0) {
                    d = std::min(d, dist[(size_t)(y - 1) * WW + x] + 3.0f);
                    if (x > 0)      d = std::min(d, dist[(size_t)(y - 1) * WW + x - 1] + 4.0f);
                    if (x < WW - 1) d = std::min(d, dist[(size_t)(y - 1) * WW + x + 1] + 4.0f);
                }
            }
        }
        for (int y = WH - 1; y >= 0; --y) {
            for (int x = WW - 1; x >= 0; --x) {
                float& d = dist[(size_t)y * WW + x];
                if (x < WW - 1)     d = std::min(d, dist[(size_t)y * WW + x + 1] + 3.0f);
                if (y < WH - 1) {
                    d = std::min(d, dist[(size_t)(y + 1) * WW + x] + 3.0f);
                    if (x < WW - 1) d = std::min(d, dist[(size_t)(y + 1) * WW + x + 1] + 4.0f);
                    if (x > 0)      d = std::min(d, dist[(size_t)(y + 1) * WW + x - 1] + 4.0f);
                }
            }
        }
        for (size_t k = 0; k < dist.size(); ++k) dist[k] /= 3.0f;

        // ---- 3. Lattice box, band samples and the blend matte.
        const int reach = (int)std::ceil(grow + band);
        box.x1 = seed.x1 - reach; box.x2 = seed.x2 + reach;
        box.y1 = seed.y1 - reach; box.y2 = seed.y2 + reach;
        box = box.intersect(work);
        const float boxW = (float)std::max(1, box.width());
        const float boxH = (float)std::max(1, box.height());

        std::vector<Sample> samples;
        alpha.assign((size_t)WW * WH, 0.0f);
        for (int y = 0; y < WH; ++y) {
            for (int x = 0; x < WW; ++x) {
                const size_t k = (size_t)y * WW + x;
                const float d = dist[k];
                const float m = mval[k];
                float a;
                if (d <= grow) {
                    a = 1.0f;
                } else if (feather > 0.0f) {
                    a = 1.0f - smoothstep01(grow, grow + feather, d);
                } else {
                    a = 0.0f;
                }
                alpha[k] = std::max(a, m);

                if (d > grow && d <= grow + band) {
                    const int px = work.x1 + x, py = work.y1 + y;
                    const float* sp = (px >= srcBounds.x1 && px < srcBounds.x2 && py >= srcBounds.y1 && py < srcBounds.y2)
                                    ? (const float*)srcRa.pixelAt(px, py) : NULL;
                    if (!sp) continue;
                    Sample s;
                    s.fx = (px + 0.5f - box.x1) / boxW;
                    s.fy = (py + 0.5f - box.y1) / boxH;
                    s.r = srcNComp >= 1 ? sp[0] : 0.0f;
                    s.g = srcNComp >= 2 ? sp[1] : s.r;
                    s.b = srcNComp >= 3 ? sp[2] : s.r;
                    if (logSpace) { s.r = toLog(s.r); s.g = toLog(s.g); s.b = toLog(s.b); }
                    s.w = 1.0f - m;
                    if (s.w <= 0.0f) continue;
                    samples.push_back(s);
                }
            }
        }

        // ---- 4. Solve the lattice.
        if (!samples.empty()) {
            lattice.rows = rows;
            lattice.cols = cols;
            solve(lattice, samples);
            haveFill = true;
        }

        // ---- 4b. Detail: texture-bombed clone stamps over the fill.
        if (haveFill && wantDetail) {
            Plane plane;
            if (DP.source == eDetailSourceInput) {
                if (detImg && detNComp > 0) {
                    RectI r = work;
                    r = r.intersect(detBounds);
                    plane.x1 = r.x1; plane.y1 = r.y1; plane.w = r.width(); plane.h = r.height();
                }
            } else {
                RectI r = work;
                if (DP.source == eDetailSourceRegion) {
                    r = DP.region;
                    r.x1 -= DP.freq; r.y1 -= DP.freq; r.x2 += DP.freq; r.y2 += DP.freq;
                }
                r = r.intersect(srcBounds);
                plane.x1 = r.x1; plane.y1 = r.y1; plane.w = r.width(); plane.h = r.height();
            }
            if (plane.w > 0 && plane.h > 0) {
                plane.rgb.assign((size_t)plane.w * plane.h * 3, 0.0f);
                plane.valid.assign((size_t)plane.w * plane.h, 1);
                for (int y = 0; y < plane.h; ++y) {
                    for (int x = 0; x < plane.w; ++x) {
                        const int px = plane.x1 + x, py = plane.y1 + y;
                        const size_t k = (size_t)y * plane.w + x;
                        const float* sp = NULL;
                        int nc = 0;
                        if (DP.source == eDetailSourceInput) {
                            sp = (const float*)detRa->pixelAt(px, py); nc = detNComp;
                        } else {
                            sp = (const float*)srcRa.pixelAt(px, py); nc = srcNComp;
                            // the hole (and anything inside the grown shape) must never be cloned
                            if (px >= work.x1 && px < work.x2 && py >= work.y1 && py < work.y2) {
                                const size_t wk = (size_t)(py - work.y1) * WW + (px - work.x1);
                                if (dist[wk] <= grow || alpha[wk] > 0.0f) plane.valid[k] = 0;
                            }
                        }
                        if (!sp) { plane.valid[k] = 0; continue; }
                        float r0 = nc >= 1 ? sp[0] : 0.0f, g0 = nc >= 2 ? sp[1] : r0, b0 = nc >= 3 ? sp[2] : r0;
                        if (logSpace) { r0 = toLog(r0); g0 = toLog(g0); b0 = toLog(b0); }
                        plane.rgb[k * 3] = r0; plane.rgb[k * 3 + 1] = g0; plane.rgb[k * 3 + 2] = b0;
                    }
                }
                buildDetailLayer(DP, plane, work, alpha, dist, grow, detailRGB, detailCover);
                haveDetail = !detailRGB.empty();
            }
        }
    }

    // ---- 5. Compose.
    const float boxW = haveFill ? (float)std::max(1, box.width()) : 1.0f;
    const float boxH = haveFill ? (float)std::max(1, box.height()) : 1.0f;

    #ifdef _OPENMP
    #pragma omp parallel for
    #endif
    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;
            const float* sp = (x >= srcBounds.x1 && x < srcBounds.x2 && y >= srcBounds.y1 && y < srcBounds.y2)
                            ? (const float*)srcRa.pixelAt(x, y) : NULL;
            float sR = sp && srcNComp >= 1 ? sp[0] : 0.0f;
            float sG = sp && srcNComp >= 2 ? sp[1] : sR;
            float sB = sp && srcNComp >= 3 ? sp[2] : sR;
            float sA = sp && srcNComp >= 4 ? sp[3] : 1.0f;

            float a = 0.0f;
            if (haveFill && x >= work.x1 && x < work.x2 && y >= work.y1 && y < work.y2) {
                a = alpha[(size_t)(y - work.y1) * WW + (x - work.x1)];
            }

            float fR = sR, fG = sG, fB = sB;
            float dR = 0.0f, dG = 0.0f, dB = 0.0f;
            const bool needFill = haveFill && (outMode == eOutputFill || a > 0.0f);
            if (needFill) {
                float f[3];
                sampleLattice(lattice, (x + 0.5f - box.x1) / boxW, (y + 0.5f - box.y1) / boxH, f);
                if (haveDetail && x >= work.x1 && x < work.x2 && y >= work.y1 && y < work.y2) {
                    const size_t k = (size_t)(y - work.y1) * WW + (x - work.x1);
                    dR = detailRGB[k * 3] * detailAmount;
                    dG = detailRGB[k * 3 + 1] * detailAmount;
                    dB = detailRGB[k * 3 + 2] * detailAmount;
                    f[0] += dR; f[1] += dG; f[2] += dB;   // in log space too when Log Space is on
                }
                if (logSpace) { f[0] = fromLog(f[0]); f[1] = fromLog(f[1]); f[2] = fromLog(f[2]); }
                fR = f[0]; fG = f[1]; fB = f[2];
            }

            float oR, oG, oB, oA = sA;
            if (outMode == eOutputMatte) {
                oR = oG = oB = oA = a;
            } else if (outMode == eOutputDetail) {
                oR = 0.18f + dR; oG = 0.18f + dG; oB = 0.18f + dB;
            } else if (outMode == eOutputFill) {
                oR = fR; oG = fG; oB = fB;
            } else {
                const float t = a * mixVal;
                oR = sR + (fR - sR) * t;
                oG = sG + (fG - sG) * t;
                oB = sB + (fB - sB) * t;
            }

            if (outNComp >= 1) dst[0] = oR;
            if (outNComp >= 2) dst[1] = oG;
            if (outNComp >= 3) dst[2] = oB;
            if (outNComp >= 4) dst[3] = oA;
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
