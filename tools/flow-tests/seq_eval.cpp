// Scores Engine/Dev/PostFX/FlowCore on a rendered SEQUENCE against the renderer's own motion
// vectors, for accuracy and for steadiness over time (flicker):
//   g++ -O2 -fopenmp -I Engine/Dev/PostFX tools/flow-tests/seq_eval.cpp Engine/Dev/PostFX/FlowCore.cpp -o seq_eval
//   seq_eval <dir> <first> <last> [temporal strength, default 1]
// <dir> holds single-channel float PFM files per frame: beauty.NNNN.pfm for first..last, and
// velx.NNNN.pfm / vely.NNNN.pfm (ScanlineRender's Velocity AOV) for first+1..last, e.g.
//   oiiotool beauty.0025.exr --ch R -o beauty.0025.pfm
//   oiiotool vel.0025.exr --ch Velocity.x -o velx.0025.pfm
// Backward fields are used (frame N to N-1) because their truth is exactly -Velocity(N).
// Flicker = how much a surface point's error changes from one frame to the next: the error at a
// pixel is compared with the error at the place the same surface point was one frame earlier.
// A steady error does not flicker; an error that jumps around does, and shows as shimmer.
// Tuning overrides come from the environment (FLOW_ALPHA, FLOW_ZETA, ...), as in flo_eval.
#include "FlowCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

static void
applyEnv(FlowCore::Params* p)
{
    if (const char* s = std::getenv("FLOW_ALPHA")) p->refineAlpha = std::atof(s);
    if (const char* s = std::getenv("FLOW_DELTA")) p->refineDelta = std::atof(s);
    if (const char* s = std::getenv("FLOW_GAMMA")) p->refineGamma = std::atof(s);
    if (const char* s = std::getenv("FLOW_ZETA")) p->refineZeta = std::atof(s);
    if (const char* s = std::getenv("FLOW_FP")) p->refineIterations = std::atoi(s);
    if (const char* s = std::getenv("FLOW_DETAIL")) p->vectorDetail = std::atof(s);
    if (const char* s = std::getenv("FLOW_SMOOTH")) p->smoothness = std::atof(s);
}

static bool
readPFM(const std::string& path, std::vector<float>* img, int* w, int* h)
{
    FILE* f = std::fopen(path.c_str(), "rb");

    if (!f) {
        std::fprintf( stderr, "cannot open %s\n", path.c_str() );

        return false;
    }
    char tag[3] = { 0, 0, 0 };
    float scale = 0.f;
    if ( (std::fscanf(f, "%2s %d %d %f", tag, w, h, &scale) != 4) || (tag[0] != 'P') ) {
        std::fclose(f);

        return false;
    }
    std::fgetc(f);
    const int nc = tag[1] == 'F' ? 3 : 1;
    std::vector<float> raw( (size_t)*w * *h * nc );
    const bool ok = std::fread(&raw[0], 4, raw.size(), f) == raw.size();
    std::fclose(f);
    img->resize( (size_t)*w * *h );
    for (size_t i = 0; i < img->size(); ++i) {
        (*img)[i] = raw[i * nc];
    }

    return ok;
}

static std::string
frameName(const std::string& dir, const char* base, int frame)
{
    char buf[64];

    std::snprintf(buf, sizeof(buf), "/%s.%04d.pfm", base, frame);

    return dir + buf;
}

struct Score
{
    double epe, flicker, truthFlicker;
};

// fields[t] = backward field of frame t; tx/ty[t] = Velocity of frame t
static Score
score(const std::map<int, std::vector<float> >& fields,
      const std::map<int, std::vector<float> >& tx,
      const std::map<int, std::vector<float> >& ty,
      int w, int h, int first, int last)
{
    double sumE = 0., sumF = 0.;
    long cntE = 0, cntF = 0;

    for (int t = first; t <= last; ++t) {
        const std::vector<float>& f = fields.at(t);
        const std::vector<float>& vx = tx.at(t);
        const std::vector<float>& vy = ty.at(t);
        const bool havePrev = t > first;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t k = (size_t)y * w + x;
                const float gu = -vx[k], gv = -vy[k];
                if (std::hypot(gu, gv) < 0.01f) {
                    continue;
                }
                const float eu = f[2 * k] - gu, ev = f[2 * k + 1] - gv;
                sumE += std::hypot(eu, ev);
                ++cntE;
                if (!havePrev) {
                    continue;
                }
                // where this surface point was one frame earlier
                const int px = (int)std::floor(x + gu + 0.5f), py = (int)std::floor(y + gv + 0.5f);
                if ( (px < 0) || (py < 0) || (px >= w) || (py >= h) ) {
                    continue;
                }
                const size_t q = (size_t)py * w + px;
                const float pgu = -tx.at(t - 1)[q], pgv = -ty.at(t - 1)[q];
                if (std::hypot(pgu, pgv) < 0.01f) {
                    continue;
                }
                const std::vector<float>& pf = fields.at(t - 1);
                sumF += std::hypot( eu - (pf[2 * q] - pgu), ev - (pf[2 * q + 1] - pgv) );
                ++cntF;
            }
        }
    }
    Score s;
    s.epe = cntE ? sumE / cntE : 0.;
    s.flicker = cntF ? sumF / cntF : 0.;
    s.truthFlicker = 0.;

    return s;
}

int
main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: seq_eval dir first last [temporal strength]\n");

        return 2;
    }
    const std::string dir = argv[1];
    const int first = std::atoi(argv[2]), last = std::atoi(argv[3]);
    const double strength = argc > 4 ? std::atof(argv[4]) : 1.;
    int w = 0, h = 0;
    std::map<int, std::vector<float> > img, tx, ty, raw, filtered;
    for (int t = first; t <= last; ++t) {
        int w1, h1;
        if ( !readPFM(frameName(dir, "beauty", t), &img[t], &w1, &h1) ) {
            return 2;
        }
        w = w1; h = h1;
        for (size_t k = 0; k < img[t].size(); ++k) {
            img[t][k] = FlowCore::matchEncode(img[t][k]);
        }
        if ( (t > first) && ( !readPFM(frameName(dir, "velx", t), &tx[t], &w1, &h1) || !readPFM(frameName(dir, "vely", t), &ty[t], &w1, &h1) ) ) {
            return 2;
        }
    }
    FlowCore::Params p;
    p.vectorDetail = 1.;
    applyEnv(&p);
    for (int t = first + 1; t <= last; ++t) {
        FlowCore::computeFlow(&img[t][0], &img[t - 1][0], w, h, p, &raw[t]);
    }
    // a backward field points to the previous frame: "ahead" is frame t-1, "behind" is frame t+1
    for (int t = first + 2; t <= last - 1; ++t) {
        FlowCore::temporalFilter(raw[t], &raw[t - 1], &raw[t + 1], w, h, strength, &filtered[t]);
    }
    const Score a = score(raw, tx, ty, w, h, first + 2, last - 1);
    const Score b = score(filtered, tx, ty, w, h, first + 2, last - 1);
    std::printf("frames %d..%d, %dx%d, moving pixels only\n", first + 2, last - 1, w, h);
    std::printf("  each frame on its own      error %.3f px   flicker %.3f px\n", a.epe, a.flicker);
    std::printf("  temporal filter (%.2f)     error %.3f px   flicker %.3f px\n", strength, b.epe, b.flicker);

    return 0;
}
