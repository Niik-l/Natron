// Scores Engine/Dev/PostFX/FlowCore against a ground-truth flow file (Middlebury .flo).
//   g++ -O2 -fopenmp -I Engine/Dev/PostFX tools/flow-tests/flo_eval.cpp Engine/Dev/PostFX/FlowCore.cpp -o flo_eval
//   flo_eval <frame0.pgm> <frame1.pgm> <truth.flo> [name]
// The frames are binary 8-bit PGM (P5); convert colour frames with tools/flow-tests/to_pgm.py.
// Prints the mean end-point error in pixels for a few settings.
#include "FlowCore.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static bool
readPGM(const char* path, std::vector<float>* img, int* w, int* h)
{
    FILE* f = std::fopen(path, "rb");

    if (!f) {
        return false;
    }
    int maxv = 0;
    if (std::fscanf(f, "P5 %d %d %d", w, h, &maxv) != 3) {
        std::fclose(f);

        return false;
    }
    std::fgetc(f);
    std::vector<unsigned char> raw( (size_t)*w * *h );
    const bool ok = std::fread(&raw[0], 1, raw.size(), f) == raw.size();
    std::fclose(f);
    img->resize( raw.size() );
    for (size_t i = 0; i < raw.size(); ++i) {
        (*img)[i] = (float)raw[i]; // already display-encoded 0..255
    }

    return ok;
}

static bool
readFlo(const char* path, std::vector<float>* flow, int* w, int* h)
{
    FILE* f = std::fopen(path, "rb");

    if (!f) {
        return false;
    }
    float tag = 0.f;
    bool ok = std::fread(&tag, 4, 1, f) == 1 && std::fread(w, 4, 1, f) == 1 && std::fread(h, 4, 1, f) == 1 && tag == 202021.25f;
    if (ok) {
        flow->resize( (size_t)*w * *h * 2 );
        ok = std::fread(&(*flow)[0], 4, flow->size(), f) == flow->size();
    }
    std::fclose(f);

    return ok;
}

int
main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: flo_eval frame0.pgm frame1.pgm truth.flo [name]\n");

        return 2;
    }
    std::vector<float> a, b, gt;
    int w, h, w1, h1, wf, hf;
    if ( !readPGM(argv[1], &a, &w, &h) || !readPGM(argv[2], &b, &w1, &h1) || !readFlo(argv[3], &gt, &wf, &hf)
         || (w != w1) || (h != h1) || (w != wf) || (h != hf) ) {
        std::fprintf(stderr, "cannot read inputs or sizes differ\n");

        return 2;
    }
    const char* name = argc > 4 ? argv[4] : "";
    const double settings[][2] = { { 1., 0.5 }, { 0.5, 0.5 }, { 1., 0.15 }, { 1., 1. } }; // detail, smoothness
    for (unsigned s = 0; s < sizeof(settings) / sizeof(settings[0]); ++s) {
        FlowCore::Params p;
        p.vectorDetail = settings[s][0];
        p.smoothness = settings[s][1];
        std::vector<float> flow;
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        FlowCore::computeFlow(&a[0], &b[0], w, h, p, &flow);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        double sum = 0., sumZero = 0.;
        long cnt = 0, bad1 = 0, bad3 = 0;
        for (size_t k = 0; k < (size_t)w * h; ++k) {
            const float gu = gt[2 * k], gv = gt[2 * k + 1];
            if ( (std::fabs(gu) > 1e9f) || (std::fabs(gv) > 1e9f) ) {
                continue; // unknown in the ground truth
            }
            const double e = std::hypot(flow[2 * k] - gu, flow[2 * k + 1] - gv);
            sum += e;
            sumZero += std::hypot(gu, gv);
            ++cnt;
            bad1 += e > 1.;
            bad3 += e > 3.;
        }
        std::printf("%-12s %dx%d detail %.2f smooth %.2f  EPE %.3f px  (zero-flow EPE %.2f)  >1px %5.1f%%  >3px %5.1f%%  %5.0f ms\n",
                    name, w, h, p.vectorDetail, p.smoothness, sum / cnt, sumZero / cnt, 100. * bad1 / cnt, 100. * bad3 / cnt, ms);
    }

    return 0;
}
