// Scores Engine/Dev/PostFX/FlowCore on a rendered frame pair against the renderer's own motion
// vectors (ScanlineRender's Velocity AOV), all given as single-channel float PFM files:
//   g++ -O2 -fopenmp -I Engine/Dev/PostFX tools/flow-tests/pfm_eval.cpp Engine/Dev/PostFX/FlowCore.cpp -o pfm_eval
//   oiiotool beauty.0025.exr --ch R -o f0.pfm        (likewise f1 from the other frame)
//   oiiotool vel.0025.exr --ch Velocity.x -o tx.pfm  (likewise ty from Velocity.y)
//   pfm_eval f0.pfm f1.pfm tx.pfm ty.pfm <truth scale> [flow.pfm]
// The flow is computed from f0 to f1 and compared with scale * truth. Velocity holds the motion
// since the previous frame, so with f0 = frame N and f1 = frame N-1 the scale is -1.
// PFM rows run bottom to top, the same way up as Natron, so no sign changes are needed.
// Tuning overrides come from the environment (FLOW_ALPHA, FLOW_ZETA, ...), as in flo_eval.
#include "FlowCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void
applyEnv(FlowCore::Params* p)
{
    if (const char* s = std::getenv("FLOW_ALPHA")) p->refineAlpha = std::atof(s);
    if (const char* s = std::getenv("FLOW_DELTA")) p->refineDelta = std::atof(s);
    if (const char* s = std::getenv("FLOW_GAMMA")) p->refineGamma = std::atof(s);
    if (const char* s = std::getenv("FLOW_ZETA")) p->refineZeta = std::atof(s);
    if (const char* s = std::getenv("FLOW_FP")) p->refineIterations = std::atoi(s);
    if (const char* s = std::getenv("FLOW_SOR")) p->refineSor = std::atoi(s);
    if (const char* s = std::getenv("FLOW_PSMOOTH")) p->patchSmooth = std::atoi(s) != 0;
    if (const char* s = std::getenv("FLOW_PATCH")) p->patchSize = std::atoi(s);
    if (const char* s = std::getenv("FLOW_DETAIL")) p->vectorDetail = std::atof(s);
    if (const char* s = std::getenv("FLOW_SMOOTH")) p->smoothness = std::atof(s);
}

static bool
readPFM(const char* path, std::vector<float>* img, int* w, int* h)
{
    FILE* f = std::fopen(path, "rb");

    if (!f) {
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

    return ok && scale < 0.f; // little-endian only
}

int
main(int argc, char** argv)
{
    if (argc < 6) {
        std::fprintf(stderr, "usage: pfm_eval f0.pfm f1.pfm truthx.pfm truthy.pfm scale [flow.pfm]\n");

        return 2;
    }
    std::vector<float> a, b, tx, ty;
    int w, h, w1, h1;
    if ( !readPFM(argv[1], &a, &w, &h) || !readPFM(argv[2], &b, &w1, &h1) || (w != w1) || (h != h1)
         || !readPFM(argv[3], &tx, &w1, &h1) || (w != w1) || (h != h1) || !readPFM(argv[4], &ty, &w1, &h1) || (w != w1) || (h != h1) ) {
        std::fprintf(stderr, "cannot read inputs or sizes differ\n");

        return 2;
    }
    const float scale = (float)std::atof(argv[5]);
    for (size_t k = 0; k < a.size(); ++k) {
        a[k] = FlowCore::matchEncode(a[k]);
        b[k] = FlowCore::matchEncode(b[k]);
    }
    FlowCore::Params p;
    p.vectorDetail = 1.;
    applyEnv(&p);
    std::vector<float> flow;
    FlowCore::computeFlow(&a[0], &b[0], w, h, p, &flow);
    double sumM = 0., sumS = 0., biasU = 0., biasV = 0.;
    long cntM = 0, cntS = 0, bad1 = 0, bad3 = 0;
    for (size_t k = 0; k < (size_t)w * h; ++k) {
        const float gu = scale * tx[k], gv = scale * ty[k];
        const double e = std::hypot(flow[2 * k] - gu, flow[2 * k + 1] - gv);
        if (std::hypot(gu, gv) > 0.01f) {
            sumM += e;
            biasU += flow[2 * k] - gu;
            biasV += flow[2 * k + 1] - gv;
            ++cntM;
            bad1 += e > 1.;
            bad3 += e > 3.;
        } else {
            sumS += e;
            ++cntS;
        }
    }
    std::printf("moving EPE %.3f px  >1px %5.1f%%  >3px %5.1f%%   still EPE %.3f px\n", cntM ? sumM / cntM : 0.,
                cntM ? 100. * bad1 / cntM : 0., cntM ? 100. * bad3 / cntM : 0., cntS ? sumS / cntS : 0.);
    if (cntM) {
        std::printf("    mean signed error over the moving part: %+.3f, %+.3f px\n", biasU / cntM, biasV / cntM);
    }
    if (argc > 6) {
        // error magnitude as a picture, for viewing
        FILE* f = std::fopen(argv[6], "wb");
        if (f) {
            std::fprintf(f, "Pf\n%d %d\n-1.0\n", w, h);
            for (size_t k = 0; k < (size_t)w * h; ++k) {
                const float e = (float)std::hypot(flow[2 * k] - scale * tx[k], flow[2 * k + 1] - scale * ty[k]);
                std::fwrite(&e, 4, 1, f);
            }
            std::fclose(f);
        }
    }

    return 0;
}
