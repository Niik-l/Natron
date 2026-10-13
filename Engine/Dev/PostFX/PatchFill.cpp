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
           "light's halo, and Log Space when a bright rim bleeds into the patch.";
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

    const RectI srcBounds  = srcImg->getBounds();
    const RectI outBounds  = outImg->getBounds();
    const RectI maskBounds = maskImg ? maskImg->getBounds() : RectI();

    const int srcNComp  = srcImg->getComponents().getNumComponents();
    const int outNComp  = std::min(srcNComp, 4);
    const int maskNComp = maskImg ? maskImg->getComponents().getNumComponents() : 0;

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
    const int pad = (int)std::ceil(grow + band + feather) + 2;
    if (haveSeed) {
        work.x1 = seed.x1 - pad; work.x2 = seed.x2 + pad;
        work.y1 = seed.y1 - pad; work.y2 = seed.y2 + pad;
        work = work.intersect(outBounds);
    }
    const int WW = haveSeed ? work.width() : 0;
    const int WH = haveSeed ? work.height() : 0;

    std::vector<float> mval, dist, alpha;
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
            const bool needFill = haveFill && (outMode == eOutputFill || a > 0.0f);
            if (needFill) {
                float f[3];
                sampleLattice(lattice, (x + 0.5f - box.x1) / boxW, (y + 0.5f - box.y1) / boxH, f);
                if (logSpace) { f[0] = fromLog(f[0]); f[1] = fromLog(f[1]); f[2] = fromLog(f[2]); }
                fR = f[0]; fG = f[1]; fB = f[2];
            }

            float oR, oG, oB, oA = sA;
            if (outMode == eOutputMatte) {
                oR = oG = oB = oA = a;
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
