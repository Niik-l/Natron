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

#include "ContentFill.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

namespace {

enum OutputModeEnum
{
    eOutputResult = 0,
    eOutputMatte  = 1,
};

inline float
smoothstep01(float edge0, float edge1, float x)
{
    if (edge1 <= edge0) return x >= edge0 ? 1.0f : 0.0f;
    float t = (x - edge0) / (edge1 - edge0);
    t = std::max(0.0f, std::min(1.0f, t));
    return t * t * (3.0f - 2.0f * t);
}

inline float toLog(float v)   { return v >= 0.0f ? std::log1p(v) : -std::log1p(-v); }
inline float fromLog(float u) { return u >= 0.0f ? std::expm1(u) : -std::expm1(-u); }

struct Rng
{
    unsigned int s;
    explicit Rng(unsigned int seed) : s(seed * 2654435761u + 0x9E3779B9u) { if (!s) s = 1; next(); next(); }
    unsigned int next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }
    int rangeInt(int a, int b) { return a + (int)(uniform() * (b - a + 1)) % (b - a + 1); } // [a,b]
};

// One pyramid level of the working window.
struct Level
{
    int w = 0, h = 0;
    std::vector<float> img;        // w*h*3, current estimate (hole pixels get synthesised)
    std::vector<char>  hole;       // pixels to synthesise
    std::vector<char>  allowed;    // pixels usable as source patch centres
    std::vector<char>  target;     // hole dilated by the patch radius: patches centred here vote
    std::vector<int>   nnx, nny;   // best source centre per target pixel
    std::vector<float> nnd;        // its distance
    std::vector<int>   allowedList;
    size_t idx(int x, int y) const { return (size_t)y * w + x; }
};

static const float kInf = std::numeric_limits<float>::infinity();

// Mean squared difference between the patch around (px,py) and the one around
// (sx,sy). Target-side pixels use the current estimate (hole included), the
// source side only counts pixels that are known (not hole). Stops early past
// `limit`.
static float
patchDist(const Level& L, int px, int py, int sx, int sy, int r, float limit)
{
    float acc = 0.0f;
    int n = 0;
    for (int dy = -r; dy <= r; ++dy) {
        const int ty = py + dy, qy = sy + dy;
        if (ty < 0 || ty >= L.h || qy < 0 || qy >= L.h) continue;
        for (int dx = -r; dx <= r; ++dx) {
            const int tx = px + dx, qx = sx + dx;
            if (tx < 0 || tx >= L.w || qx < 0 || qx >= L.w) continue;
            const size_t q = L.idx(qx, qy);
            if (L.hole[q]) continue;
            const float* a = &L.img[L.idx(tx, ty) * 3];
            const float* b = &L.img[q * 3];
            const float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
            acc += d0 * d0 + d1 * d1 + d2 * d2;
            ++n;
            if (n >= 4 && acc / n > limit) return kInf;
        }
    }
    const int full = (2 * r + 1) * (2 * r + 1);
    if (n < full / 2) return kInf;      // too little known content to judge
    return acc / n;
}

static inline bool
allowedAt(const Level& L, int x, int y)
{
    return x >= 0 && x < L.w && y >= 0 && y < L.h && L.allowed[L.idx(x, y)];
}

static void
tryCandidate(Level& L, int px, int py, int sx, int sy, int r)
{
    if (!allowedAt(L, sx, sy)) return;
    const size_t p = L.idx(px, py);
    const float d = patchDist(L, px, py, sx, sy, r, L.nnd[p]);
    if (d < L.nnd[p]) { L.nnd[p] = d; L.nnx[p] = sx; L.nny[p] = sy; }
}

// Randomised nearest-neighbour search over source centres (PatchMatch):
// propagation from the scan-previous neighbours + exponentially shrinking
// random search, alternating scan directions.
static void
nnfIterate(Level& L, int r, int iterations, Rng& rng)
{
    const int R0 = std::max(L.w, L.h);
    for (int it = 0; it < iterations; ++it) {
        const bool forward = (it % 2) == 0;
        const int y0 = forward ? 0 : L.h - 1, y1 = forward ? L.h : -1, ys = forward ? 1 : -1;
        const int x0 = forward ? 0 : L.w - 1, x1 = forward ? L.w : -1, xs = forward ? 1 : -1;
        const int step = forward ? -1 : 1;    // neighbour already visited in this scan
        for (int y = y0; y != y1; y += ys) {
            for (int x = x0; x != x1; x += xs) {
                const size_t p = L.idx(x, y);
                if (!L.target[p]) continue;
                // propagation
                const int nx = x + step, ny = y + step;
                if (nx >= 0 && nx < L.w && L.target[L.idx(nx, y)]) {
                    const size_t q = L.idx(nx, y);
                    tryCandidate(L, x, y, L.nnx[q] - step, L.nny[q], r);
                }
                if (ny >= 0 && ny < L.h && L.target[L.idx(x, ny)]) {
                    const size_t q = L.idx(x, ny);
                    tryCandidate(L, x, y, L.nnx[q], L.nny[q] - step, r);
                }
                // random search around the current best
                for (int R = R0; R >= 1; R /= 2) {
                    const int cx = L.nnx[p], cy = L.nny[p];
                    const int sx = cx + rng.rangeInt(-R, R), sy = cy + rng.rangeInt(-R, R);
                    tryCandidate(L, x, y, sx, sy, r);
                }
            }
        }
    }
}

// Every target patch votes its matched source pixels into the hole pixels it
// covers, weighted by how good the match is.
static void
vote(Level& L, int r)
{
    // weight scale: the typical distance among the targets
    double dsum = 0.0; int dn = 0;
    for (size_t p = 0; p < L.target.size(); ++p) {
        if (L.target[p] && L.nnd[p] < kInf) { dsum += L.nnd[p]; ++dn; }
    }
    const float sigma = (float)(dn ? dsum / dn : 1.0) + 1e-6f;

    std::vector<float> acc((size_t)L.w * L.h * 3, 0.0f), wsum((size_t)L.w * L.h, 0.0f);
    for (int py = 0; py < L.h; ++py) {
        for (int px = 0; px < L.w; ++px) {
            const size_t p = L.idx(px, py);
            if (!L.target[p] || L.nnd[p] >= kInf) continue;
            const float w = std::exp(-L.nnd[p] / sigma);
            const int sx = L.nnx[p], sy = L.nny[p];
            for (int dy = -r; dy <= r; ++dy) {
                const int ty = py + dy, qy = sy + dy;
                if (ty < 0 || ty >= L.h || qy < 0 || qy >= L.h) continue;
                for (int dx = -r; dx <= r; ++dx) {
                    const int tx = px + dx, qx = sx + dx;
                    if (tx < 0 || tx >= L.w || qx < 0 || qx >= L.w) continue;
                    const size_t t = L.idx(tx, ty);
                    if (!L.hole[t]) continue;
                    const size_t q = L.idx(qx, qy);
                    if (L.hole[q]) continue;
                    acc[t * 3]     += w * L.img[q * 3];
                    acc[t * 3 + 1] += w * L.img[q * 3 + 1];
                    acc[t * 3 + 2] += w * L.img[q * 3 + 2];
                    wsum[t] += w;
                }
            }
        }
    }
    for (size_t t = 0; t < wsum.size(); ++t) {
        if (L.hole[t] && wsum[t] > 0.0f) {
            L.img[t * 3]     = acc[t * 3] / wsum[t];
            L.img[t * 3 + 1] = acc[t * 3 + 1] / wsum[t];
            L.img[t * 3 + 2] = acc[t * 3 + 2] / wsum[t];
        }
    }
}

static void
buildTargets(Level& L, int r)
{
    // hole dilated by r (separable box max)
    std::vector<char> tmp(L.hole.size(), 0);
    for (int y = 0; y < L.h; ++y)
        for (int x = 0; x < L.w; ++x) {
            char v = 0;
            for (int k = std::max(0, x - r); k <= std::min(L.w - 1, x + r) && !v; ++k) v = L.hole[L.idx(k, y)];
            tmp[L.idx(x, y)] = v;
        }
    L.target.assign(L.hole.size(), 0);
    for (int y = 0; y < L.h; ++y)
        for (int x = 0; x < L.w; ++x) {
            char v = 0;
            for (int k = std::max(0, y - r); k <= std::min(L.h - 1, y + r) && !v; ++k) v = tmp[L.idx(x, k)];
            L.target[L.idx(x, y)] = v;
        }
    L.allowedList.clear();
    for (size_t i = 0; i < L.allowed.size(); ++i) if (L.allowed[i]) L.allowedList.push_back((int)i);
}

// Coarse level from a fine one: 2x2 averages that ignore hole pixels, hole =
// any, allowed = all.
static void
downsample(const Level& F, Level& C)
{
    C.w = (F.w + 1) / 2; C.h = (F.h + 1) / 2;
    C.img.assign((size_t)C.w * C.h * 3, 0.0f);
    C.hole.assign((size_t)C.w * C.h, 0);
    C.allowed.assign((size_t)C.w * C.h, 1);
    for (int y = 0; y < C.h; ++y) {
        for (int x = 0; x < C.w; ++x) {
            float accK[3] = {0, 0, 0}, accA[3] = {0, 0, 0};
            int nK = 0, nA = 0;
            char anyHole = 0, allAllowed = 1;
            for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
                const int fx = 2 * x + i, fy = 2 * y + j;
                if (fx >= F.w || fy >= F.h) continue;
                const size_t f = F.idx(fx, fy);
                const float* c = &F.img[f * 3];
                accA[0] += c[0]; accA[1] += c[1]; accA[2] += c[2]; ++nA;
                if (F.hole[f]) anyHole = 1;
                else { accK[0] += c[0]; accK[1] += c[1]; accK[2] += c[2]; ++nK; }
                if (!F.allowed[f]) allAllowed = 0;
            }
            const size_t c = C.idx(x, y);
            if (nK) { C.img[c * 3] = accK[0] / nK; C.img[c * 3 + 1] = accK[1] / nK; C.img[c * 3 + 2] = accK[2] / nK; }
            else if (nA) { C.img[c * 3] = accA[0] / nA; C.img[c * 3 + 1] = accA[1] / nA; C.img[c * 3 + 2] = accA[2] / nA; }
            C.hole[c] = anyHole;
            C.allowed[c] = (allAllowed && !anyHole) ? 1 : 0;
        }
    }
}

// Diffusion start for the coarsest level: hole pixels relax to their
// neighbours with the known pixels fixed.
static void
diffuseInit(Level& L)
{
    double mean[3] = {0, 0, 0}; int n = 0;
    for (size_t i = 0; i < L.hole.size(); ++i) {
        if (!L.hole[i]) { mean[0] += L.img[i * 3]; mean[1] += L.img[i * 3 + 1]; mean[2] += L.img[i * 3 + 2]; ++n; }
    }
    if (n) { mean[0] /= n; mean[1] /= n; mean[2] /= n; }
    for (size_t i = 0; i < L.hole.size(); ++i) {
        if (L.hole[i]) { L.img[i * 3] = (float)mean[0]; L.img[i * 3 + 1] = (float)mean[1]; L.img[i * 3 + 2] = (float)mean[2]; }
    }
    const int iters = std::min(2000, 4 * std::max(L.w, L.h));
    for (int it = 0; it < iters; ++it) {
        float maxDelta = 0.0f;
        for (int y = 0; y < L.h; ++y) {
            for (int x = 0; x < L.w; ++x) {
                const size_t p = L.idx(x, y);
                if (!L.hole[p]) continue;
                float sum[3] = {0, 0, 0}; int cnt = 0;
                if (x > 0)       { const float* c = &L.img[L.idx(x - 1, y) * 3]; sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2]; ++cnt; }
                if (x < L.w - 1) { const float* c = &L.img[L.idx(x + 1, y) * 3]; sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2]; ++cnt; }
                if (y > 0)       { const float* c = &L.img[L.idx(x, y - 1) * 3]; sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2]; ++cnt; }
                if (y < L.h - 1) { const float* c = &L.img[L.idx(x, y + 1) * 3]; sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2]; ++cnt; }
                if (!cnt) continue;
                float* q = &L.img[p * 3];
                for (int k = 0; k < 3; ++k) {
                    const float nv = sum[k] / cnt;
                    maxDelta = std::max(maxDelta, std::fabs(nv - q[k]));
                    q[k] = nv;
                }
            }
        }
        if (maxDelta < 1e-5f) break;
    }
}

// Seed the NNF of a level: from the coarser level's field when there is one,
// otherwise at random.
static void
initNNF(Level& L, const Level* coarse, int r, Rng& rng)
{
    const size_t n = (size_t)L.w * L.h;
    L.nnx.assign(n, 0); L.nny.assign(n, 0); L.nnd.assign(n, kInf);
    if (L.allowedList.empty()) return;
    for (int y = 0; y < L.h; ++y) {
        for (int x = 0; x < L.w; ++x) {
            const size_t p = L.idx(x, y);
            if (!L.target[p]) continue;
            int sx = -1, sy = -1;
            if (coarse) {
                const int cx = std::min(coarse->w - 1, x / 2), cy = std::min(coarse->h - 1, y / 2);
                const size_t c = coarse->idx(cx, cy);
                if (coarse->target[c] && coarse->nnd[c] < kInf) {
                    sx = coarse->nnx[c] * 2 + (x & 1);
                    sy = coarse->nny[c] * 2 + (y & 1);
                }
            }
            if (!allowedAt(L, sx, sy)) {
                const int a = L.allowedList[(size_t)(rng.uniform() * L.allowedList.size()) % L.allowedList.size()];
                sx = a % L.w; sy = a / L.w;
            }
            L.nnx[p] = sx; L.nny[p] = sy;
            L.nnd[p] = patchDist(L, x, y, sx, sy, r, kInf);
            if (L.nnd[p] >= kInf) L.nnd[p] = 1e30f;   // keep a finite "bad" value so better ones can replace it
        }
    }
}

// Hole pixels of a fine level from the coarse estimate (bilinear).
static void
upsampleInto(const Level& C, Level& F)
{
    for (int y = 0; y < F.h; ++y) {
        for (int x = 0; x < F.w; ++x) {
            const size_t p = F.idx(x, y);
            if (!F.hole[p]) continue;
            float u = (x + 0.5f) * 0.5f - 0.5f, v = (y + 0.5f) * 0.5f - 0.5f;
            u = std::max(0.0f, std::min((float)(C.w - 1), u));
            v = std::max(0.0f, std::min((float)(C.h - 1), v));
            const int x0 = (int)u, y0 = (int)v, x1 = std::min(C.w - 1, x0 + 1), y1 = std::min(C.h - 1, y0 + 1);
            const float tx = u - x0, ty = v - y0;
            const float* a = &C.img[C.idx(x0, y0) * 3];
            const float* b = &C.img[C.idx(x1, y0) * 3];
            const float* c = &C.img[C.idx(x0, y1) * 3];
            const float* d = &C.img[C.idx(x1, y1) * 3];
            for (int k = 0; k < 3; ++k) {
                const float top = a[k] + (b[k] - a[k]) * tx, bot = c[k] + (d[k] - c[k]) * tx;
                F.img[p * 3 + k] = top + (bot - top) * ty;
            }
        }
    }
}

} // anonymous namespace

struct ContentFillPrivate
{
    KnobIntWPtr    patchSize;
    KnobDoubleWPtr searchRadius;
    KnobIntWPtr    nnfIterations, emIterations, levels;
    KnobDoubleWPtr growMask, feather;
    KnobBoolWPtr   logSpace;
    KnobIntWPtr    seed;
    KnobChoiceWPtr outputMode;
    KnobDoubleWPtr mix;
};

ContentFill::ContentFill(NodePtr node)
    : EffectInstance(node)
    , _imp(new ContentFillPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

ContentFill::~ContentFill()
{
}

std::string
ContentFill::getPluginDescription() const
{
    return "Content-aware fill. The masked region is rebuilt from patches of its surroundings: a "
           "randomised nearest-neighbour search over patch offsets (PatchMatch, the method behind "
           "Photoshop's classic Content-Aware Fill), alternating with patch voting, run coarse to fine "
           "on an image pyramid so big holes take their structure from the coarse levels and their "
           "texture from the fine ones. No model, no network.\n\n"
           "Input 0 Source, input 1 Mask (the hole, a Roto), input 2 Sample (optional: a mask of where "
           "patches may be borrowed from; default is everything within Search Radius of the hole).\n\n"
           "Best for repeating texture and clutter: foliage, brick, gravel, crowds, sky, ground. "
           "Large single structures (a window the hole swallows whole) may come out as a plausible "
           "mix rather than the real thing; a Sample mask on the part you want copied helps. Use "
           "PatchFill for pure gradients (lights) or when lines must continue straight across.";
}

void
ContentFill::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
}

void
ContentFill::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ContentFill::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ContentFill::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Patch Size"));
        k->setName("patchSize");
        k->setDefaultValue(7);
        k->setMinimum(3); k->setMaximum(31);
        k->setDisplayMinimum(3); k->setDisplayMaximum(21);
        k->setHintToolTip(tr("Side of the square patches that are matched and copied, in pixels (odd). "
                             "Larger patches keep bigger structures intact but match less flexibly; "
                             "5-9 suits texture, 11-15 brick and windows."));
        page->addKnob(k); _imp->patchSize = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Search Radius"));
        k->setName("searchRadius");
        k->setDefaultValue(300.0);
        k->setMinimum(8.0); k->setMaximum(100000.0);
        k->setDisplayMinimum(16.0); k->setDisplayMaximum(2000.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("How far outside the shape, in pixels, patches may be borrowed from (unless a "
                             "Sample mask narrows it). Also bounds the work: the node only looks at this "
                             "window around the hole."));
        page->addKnob(k); _imp->searchRadius = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Grow Mask"));
        k->setName("growMask");
        k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(10000.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Expand the shape by this many pixels so a halo or soft edge of the object is "
                             "replaced too."));
        page->addKnob(k); _imp->growMask = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Feather"));
        k->setName("feather");
        k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(10000.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Soften the edge of the fill over this many pixels outside the grown shape. "
                             "The feathered band is synthesised as well and blended in."));
        page->addKnob(k); _imp->feather = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Search Iterations"));
        k->setName("searchIterations");
        k->setDefaultValue(4);
        k->setMinimum(1); k->setMaximum(20);
        k->setDisplayMinimum(1); k->setDisplayMaximum(10);
        k->setHintToolTip(tr("Nearest-neighbour search passes per refinement step. More = better matches, slower."));
        page->addKnob(k); _imp->nnfIterations = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Refinements"));
        k->setName("refinements");
        k->setDefaultValue(3);
        k->setMinimum(1); k->setMaximum(20);
        k->setDisplayMinimum(1); k->setDisplayMaximum(10);
        k->setHintToolTip(tr("Search + vote rounds per pyramid level. More = the fill settles further, slower."));
        page->addKnob(k); _imp->emIterations = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Pyramid Levels"));
        k->setName("pyramidLevels");
        k->setDefaultValue(0);
        k->setMinimum(0); k->setMaximum(10);
        k->setDisplayMinimum(0); k->setDisplayMaximum(8);
        k->setHintToolTip(tr("Coarse-to-fine levels. 0 = automatic (halve until the hole is about two patches "
                             "wide). Fewer levels = more texture, less large-scale structure."));
        page->addKnob(k); _imp->levels = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Log Space"));
        k->setName("logSpace");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Match and blend in logarithmic space, so HDR highlights don't dominate the "
                             "patch comparison."));
        page->addKnob(k); _imp->logSpace = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Seed"));
        k->setName("seed");
        k->setDefaultValue(1);
        k->setMinimum(0); k->setMaximum(100000);
        k->setDisplayMinimum(0); k->setDisplayMaximum(100);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Random seed for the search. Change it for a different solution."));
        page->addKnob(k); _imp->seed = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Output"));
        k->setName("outputMode");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("result", "Result", "The source with the fill blended in."));
        opts.push_back(ChoiceOption("matte",  "Matte",  "The blend matte (grown + feathered mask)."));
        k->populateChoices(opts);
        k->setDefaultValue(eOutputResult);
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
}

StatusEnum
ContentFill::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale, ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProject;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
}

StatusEnum
ContentFill::render(const RenderActionArgs& args)
{
    RectI roi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, &roi);
    if (!srcImg) return eStatusFailed;
    ImagePtr maskImg = getInput(1) ? getImage(1, args.time, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, &roi) : ImagePtr();
    ImagePtr sampImg = getInput(2) ? getImage(2, args.time, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, &roi) : ImagePtr();

    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    const double pxScale = args.mappedScale.toOfxPointD().x;
    int patch = _imp->patchSize.lock()->getValue();
    if ((patch & 1) == 0) ++patch;
    patch = std::max(3, std::min(31, patch));
    const int r = patch / 2;
    const int searchRadius = std::max(8, (int)std::lround(_imp->searchRadius.lock()->getValueAtTime(args.time) * pxScale));
    const float grow    = (float)(std::max(0.0, _imp->growMask.lock()->getValueAtTime(args.time)) * pxScale);
    const float feather = (float)(std::max(0.0, _imp->feather.lock()->getValueAtTime(args.time)) * pxScale);
    const int nnfIters  = std::max(1, _imp->nnfIterations.lock()->getValue());
    const int emIters   = std::max(1, _imp->emIterations.lock()->getValue());
    const int levelsKnob = _imp->levels.lock()->getValue();
    const bool logSpace = _imp->logSpace.lock()->getValue();
    const int seed      = _imp->seed.lock()->getValueAtTime(args.time);
    const int outMode   = _imp->outputMode.lock()->getValue();
    const float mixVal  = (float)_imp->mix.lock()->getValueAtTime(args.time);

    const RectI srcBounds = srcImg->getBounds();
    const RectI outBounds = outImg->getBounds();
    const RectI maskBounds = maskImg ? maskImg->getBounds() : RectI();
    const RectI sampBounds = sampImg ? sampImg->getBounds() : RectI();
    const int srcNComp  = srcImg->getComponents().getNumComponents();
    const int outNComp  = std::min(srcNComp, 4);
    const int maskNComp = maskImg ? maskImg->getComponents().getNumComponents() : 0;
    const int sampNComp = sampImg ? sampImg->getComponents().getNumComponents() : 0;

    Image::ReadAccess  srcRa(srcImg.get());
    Image::WriteAccess wa(outImg.get());
    std::unique_ptr<Image::ReadAccess> maskRa, sampRa;
    if (maskImg) maskRa.reset(new Image::ReadAccess(maskImg.get()));
    if (sampImg) sampRa.reset(new Image::ReadAccess(sampImg.get()));

    auto maskAt = [&](int x, int y) -> float {
        if (!maskImg || maskNComp <= 0 || x < maskBounds.x1 || x >= maskBounds.x2 || y < maskBounds.y1 || y >= maskBounds.y2) return 0.0f;
        const float* mp = (const float*)maskRa->pixelAt(x, y);
        return mp ? std::max(0.0f, std::min(1.0f, mp[maskNComp - 1])) : 0.0f;
    };
    auto sampAt = [&](int x, int y) -> float {
        if (!sampImg || sampNComp <= 0) return 1.0f;
        if (x < sampBounds.x1 || x >= sampBounds.x2 || y < sampBounds.y1 || y >= sampBounds.y2) return 0.0f;
        const float* mp = (const float*)sampRa->pixelAt(x, y);
        return mp ? mp[sampNComp - 1] : 0.0f;
    };

    // ---- 1. hole bbox
    RectI seedBox; bool haveSeed = false;
    if (maskImg) {
        RectI scan = outBounds.intersect(maskBounds);
        for (int y = scan.y1; y < scan.y2; ++y)
            for (int x = scan.x1; x < scan.x2; ++x)
                if (maskAt(x, y) >= 0.5f) {
                    if (!haveSeed) { seedBox.x1 = seedBox.x2 = x; seedBox.y1 = seedBox.y2 = y; haveSeed = true; }
                    seedBox.x1 = std::min(seedBox.x1, x); seedBox.x2 = std::max(seedBox.x2, x);
                    seedBox.y1 = std::min(seedBox.y1, y); seedBox.y2 = std::max(seedBox.y2, y);
                }
    }
    if (haveSeed) { seedBox.x2 += 1; seedBox.y2 += 1; }

    // ---- 2. window, distance, matte, hole, allowed
    RectI win;
    std::vector<float> alpha;
    Level fine;
    bool haveFill = false;
    if (haveSeed) {
        const int pad = searchRadius + (int)std::ceil(grow + feather) + patch;
        win.x1 = seedBox.x1 - pad; win.x2 = seedBox.x2 + pad;
        win.y1 = seedBox.y1 - pad; win.y2 = seedBox.y2 + pad;
        win = win.intersect(outBounds).intersect(srcBounds);
        const int WW = win.width(), WH = win.height();
        if (WW > 2 * patch && WH > 2 * patch) {
            std::vector<float> dist((size_t)WW * WH, 1e9f), mval((size_t)WW * WH, 0.0f);
            for (int y = 0; y < WH; ++y)
                for (int x = 0; x < WW; ++x) {
                    const float m = maskAt(win.x1 + x, win.y1 + y);
                    mval[(size_t)y * WW + x] = m;
                    if (m >= 0.5f) dist[(size_t)y * WW + x] = 0.0f;
                }
            for (int y = 0; y < WH; ++y)
                for (int x = 0; x < WW; ++x) {
                    float& d = dist[(size_t)y * WW + x];
                    if (x > 0) d = std::min(d, dist[(size_t)y * WW + x - 1] + 3.0f);
                    if (y > 0) {
                        d = std::min(d, dist[(size_t)(y - 1) * WW + x] + 3.0f);
                        if (x > 0) d = std::min(d, dist[(size_t)(y - 1) * WW + x - 1] + 4.0f);
                        if (x < WW - 1) d = std::min(d, dist[(size_t)(y - 1) * WW + x + 1] + 4.0f);
                    }
                }
            for (int y = WH - 1; y >= 0; --y)
                for (int x = WW - 1; x >= 0; --x) {
                    float& d = dist[(size_t)y * WW + x];
                    if (x < WW - 1) d = std::min(d, dist[(size_t)y * WW + x + 1] + 3.0f);
                    if (y < WH - 1) {
                        d = std::min(d, dist[(size_t)(y + 1) * WW + x] + 3.0f);
                        if (x < WW - 1) d = std::min(d, dist[(size_t)(y + 1) * WW + x + 1] + 4.0f);
                        if (x > 0) d = std::min(d, dist[(size_t)(y + 1) * WW + x - 1] + 4.0f);
                    }
                }
            for (size_t k = 0; k < dist.size(); ++k) dist[k] /= 3.0f;

            alpha.assign((size_t)WW * WH, 0.0f);
            fine.w = WW; fine.h = WH;
            fine.img.assign((size_t)WW * WH * 3, 0.0f);
            fine.hole.assign((size_t)WW * WH, 0);
            fine.allowed.assign((size_t)WW * WH, 0);
            int nHole = 0, nAllowed = 0;
            for (int y = 0; y < WH; ++y) {
                for (int x = 0; x < WW; ++x) {
                    const size_t k = (size_t)y * WW + x;
                    const float d = dist[k];
                    float a = d <= grow ? 1.0f : (feather > 0.0f ? 1.0f - smoothstep01(grow, grow + feather, d) : 0.0f);
                    a = std::max(a, mval[k]);
                    alpha[k] = a;
                    const int px = win.x1 + x, py = win.y1 + y;
                    const float* sp = (const float*)srcRa.pixelAt(px, py);
                    float r0 = sp && srcNComp >= 1 ? sp[0] : 0.0f;
                    float g0 = sp && srcNComp >= 2 ? sp[1] : r0;
                    float b0 = sp && srcNComp >= 3 ? sp[2] : r0;
                    if (logSpace) { r0 = toLog(r0); g0 = toLog(g0); b0 = toLog(b0); }
                    fine.img[k * 3] = r0; fine.img[k * 3 + 1] = g0; fine.img[k * 3 + 2] = b0;
                    const bool hole = a > 0.0f;
                    fine.hole[k] = hole ? 1 : 0;
                    if (hole) ++nHole;
                    const bool inWindowMargin = x >= r && x < WW - r && y >= r && y < WH - r;
                    const bool allowed = !hole && inWindowMargin && sampAt(px, py) >= 0.5f;
                    fine.allowed[k] = allowed ? 1 : 0;
                    if (allowed) ++nAllowed;
                }
            }
            haveFill = nHole > 0 && nAllowed > 0;
        }
    }

    // ---- 3. pyramid + PatchMatch completion
    if (haveFill) {
        std::vector<Level> pyr;
        pyr.push_back(fine);
        const int maxLevels = levelsKnob > 0 ? levelsKnob : 8;
        for (;;) {
            const Level& L = pyr.back();
            if ((int)pyr.size() >= maxLevels) break;
            // hole extent at this level
            int hx1 = L.w, hx2 = -1, hy1 = L.h, hy2 = -1;
            for (int y = 0; y < L.h; ++y)
                for (int x = 0; x < L.w; ++x)
                    if (L.hole[L.idx(x, y)]) { hx1 = std::min(hx1, x); hx2 = std::max(hx2, x); hy1 = std::min(hy1, y); hy2 = std::max(hy2, y); }
            const int holeDim = std::max(hx2 - hx1 + 1, hy2 - hy1 + 1);
            if (holeDim <= 2 * patch) break;
            if (std::min(L.w, L.h) / 2 < 3 * patch) break;
            Level C;
            downsample(L, C);
            // the coarse level still needs some allowed centres
            int nA = 0; for (size_t i = 0; i < C.allowed.size(); ++i) nA += C.allowed[i];
            if (nA < 4) break;
            pyr.push_back(C);
        }
        Rng rng((unsigned int)seed + 17u);
        for (int lv = (int)pyr.size() - 1; lv >= 0; --lv) {
            Level& L = pyr[lv];
            buildTargets(L, r);
            if (lv == (int)pyr.size() - 1) {
                diffuseInit(L);
                initNNF(L, NULL, r, rng);
            } else {
                upsampleInto(pyr[lv + 1], L);
                initNNF(L, &pyr[lv + 1], r, rng);
            }
            if (L.allowedList.empty()) continue;
            for (int em = 0; em < emIters; ++em) {
                nnfIterate(L, r, nnfIters, rng);
                vote(L, r);
            }
            // free what the finer level no longer needs
            if (lv + 1 < (int)pyr.size()) { Level& C = pyr[lv + 1]; C.img.clear(); C.nnx.clear(); C.nny.clear(); C.nnd.clear(); }
        }
        fine.img.swap(pyr[0].img);
    }

    // ---- 4. compose
    const int WW = haveFill ? win.width() : 0;
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
            float a = 0.0f, fR = sR, fG = sG, fB = sB;
            if (haveFill && x >= win.x1 && x < win.x2 && y >= win.y1 && y < win.y2) {
                const size_t k = (size_t)(y - win.y1) * WW + (x - win.x1);
                a = alpha[k];
                if (a > 0.0f) {
                    fR = fine.img[k * 3]; fG = fine.img[k * 3 + 1]; fB = fine.img[k * 3 + 2];
                    if (logSpace) { fR = fromLog(fR); fG = fromLog(fG); fB = fromLog(fB); }
                }
            }
            float oR, oG, oB, oA = sA;
            if (outMode == eOutputMatte) {
                oR = oG = oB = oA = a;
            } else {
                const float t = a * mixVal;
                oR = sR + (fR - sR) * t; oG = sG + (fG - sG) * t; oB = sB + (fB - sB) * t;
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
