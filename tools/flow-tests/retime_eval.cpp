// Scores FlowCore's frame interpolation (the engine of TimeFlow) at quarter speed against frames
// rendered at exactly those in-between times, overall and in a band around a moving object's edge,
// which is where retimers leave ghosts:
//   g++ -O2 -fopenmp -I Engine/Dev/PostFX tools/flow-tests/retime_eval.cpp Engine/Dev/PostFX/FlowCore.cpp -o retime_eval
//   retime_eval <dir> <first> <last> [dump prefix]
// <dir> holds single-channel float PFM files:
//   beauty.NNNN.pfm     the source clip, frames first..last
//   matte.NNNN.pfm      the object's matte for the same frames
//   slow.NNNN.pfm       the same shot rendered four times slower: slow frame 1 + (N - 1) * 4 is source frame N
//   slowmatte.NNNN.pfm  the object's matte for the slow frames
// Every source pair is interpolated at 0.25, 0.5 and 0.75, without and with the matte.
// Errors are mean absolute differences in 0..255 display-like units.
#include "FlowCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

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

// 1 within `radius` pixels of the matte's edge
static void
edgeBand(const std::vector<float>& matte, int w, int h, int radius, std::vector<unsigned char>* band)
{
    std::vector<unsigned char> lo( (size_t)w * h ), hi( (size_t)w * h ), lo2( (size_t)w * h ), hi2( (size_t)w * h );

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char mn = 1, mx = 0;
            for (int dx = -radius; dx <= radius; ++dx) {
                const int xx = std::min(std::max(x + dx, 0), w - 1);
                const unsigned char v = matte[(size_t)y * w + xx] > 0.5f;
                mn = std::min(mn, v); mx = std::max(mx, v);
            }
            lo[(size_t)y * w + x] = mn; hi[(size_t)y * w + x] = mx;
        }
    }
    band->resize( (size_t)w * h );
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char mn = 1, mx = 0;
            for (int dy = -radius; dy <= radius; ++dy) {
                const int yy = std::min(std::max(y + dy, 0), h - 1);
                mn = std::min(mn, lo[(size_t)yy * w + x]); mx = std::max(mx, hi[(size_t)yy * w + x]);
            }
            (*band)[(size_t)y * w + x] = mx != mn;
        }
    }
}

struct Acc
{
    double all, rim;
    long nAll, nRim;
    Acc() : all(0.), rim(0.), nAll(0), nRim(0) {}

    void add(const std::vector<float>& a, const std::vector<float>& truth, const std::vector<unsigned char>& band)
    {
        for (size_t k = 0; k < a.size(); ++k) {
            const double d = std::fabs(a[k] - truth[k]);
            all += d;
            if (band[k]) {
                rim += d;
                ++nRim;
            }
        }
        nAll += (long)a.size();
    }
};

int
main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: retime_eval dir first last [dump prefix]\n");

        return 2;
    }
    const std::string dir = argv[1];
    const int first = std::atoi(argv[2]), last = std::atoi(argv[3]);
    int w = 0, h = 0;
    std::map<int, std::vector<float> > img, matte;
    for (int t = first; t <= last; ++t) {
        if ( !readPFM(frameName(dir, "beauty", t), &img[t], &w, &h) || !readPFM(frameName(dir, "matte", t), &matte[t], &w, &h) ) {
            return 2;
        }
        for (size_t k = 0; k < img[t].size(); ++k) {
            img[t][k] = FlowCore::matchEncode(img[t][k]);
        }
    }
    FlowCore::Params p;
    p.vectorDetail = 1.;
    if (const char* s = std::getenv("FLOW_DETAIL")) p.vectorDetail = std::atof(s);
    const char* names[2] = { "without matte", "with matte" };
    Acc acc[2][3];
    const size_t N = (size_t)w * h;
    for (int t = first; t + 1 <= last; ++t) {
        const std::vector<float>& a = img[t];
        const std::vector<float>& b = img[t + 1];
        std::vector<float> ab, ba, ca, cb, fAB, fBA, bAB, bBA;
        FlowCore::computeFlow(&a[0], &b[0], w, h, p, &ab);
        FlowCore::computeFlow(&b[0], &a[0], w, h, p, &ba);
        FlowCore::flowConsistency(ab, ba, w, h, &ca);
        FlowCore::flowConsistency(ba, ab, w, h, &cb);
        FlowCore::computeFlowLayered(&a[0], &b[0], &matte[t][0], &matte[t + 1][0], w, h, p, &fAB, &bAB);
        FlowCore::computeFlowLayered(&b[0], &a[0], &matte[t + 1][0], &matte[t][0], w, h, p, &fBA, &bBA);
        for (int q = 1; q <= 3; ++q) {
            const int slowFrame = 1 + (t - 1) * 4 + q;
            std::vector<float> truth, tm;
            if ( !readPFM(frameName(dir, "slow", slowFrame), &truth, &w, &h) || !readPFM(frameName(dir, "slowmatte", slowFrame), &tm, &w, &h) ) {
                return 2;
            }
            for (size_t k = 0; k < N; ++k) {
                truth[k] = FlowCore::matchEncode(truth[k]);
            }
            std::vector<unsigned char> band;
            edgeBand(tm, w, h, 8, &band);
            const double s = q / 4.;
            for (int m = 0; m < 2; ++m) {
                std::vector<float> out(N);
                if (m == 0) {
                    FlowCore::interpolateFrame(&a[0], &b[0], 1, w, h, ab, ba, ca, cb, s, &out[0]);
                } else {
                    FlowCore::interpolateFrameLayered(&a[0], &b[0], 1, w, h, fAB, fBA, bAB, bBA, &matte[t][0], &matte[t + 1][0], s, &out[0]);
                }
                acc[m][q - 1].add(out, truth, band);
                if ( (argc > 4) && (t == (first + last) / 2) ) {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf), "%s_%d_%d.pfm", argv[4], m, q);
                    FILE* f = std::fopen(buf, "wb");
                    if (f) {
                        std::fprintf(f, "Pf\n%d %d\n-1.0\n", w, h);
                        std::fwrite(&out[0], 4, out.size(), f);
                        std::fclose(f);
                    }
                }
            }
        }
    }
    std::printf("source frames %d..%d at quarter speed, %dx%d; mean error whole frame / within 8 px of the object's edge\n", first, last, w, h);
    std::printf("  %-14s %17s %17s %17s %17s\n", "", "at 0.25", "at 0.50", "at 0.75", "all");
    for (int m = 0; m < 2; ++m) {
        double sa = 0., sr = 0.; long na = 0, nr = 0;
        std::printf("  %-14s", names[m]);
        for (int q = 0; q < 3; ++q) {
            const Acc& c = acc[m][q];
            std::printf("   %6.3f / %6.3f", c.all / c.nAll, c.nRim ? c.rim / c.nRim : 0.);
            sa += c.all; sr += c.rim; na += c.nAll; nr += c.nRim;
        }
        std::printf("   %6.3f / %6.3f\n", sa / na, nr ? sr / nr : 0.);
    }

    return 0;
}
