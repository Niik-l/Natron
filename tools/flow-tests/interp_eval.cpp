// Scores FlowCore's frame interpolation (the engine of TimeFlow) on a rendered sequence: every
// frame N is rebuilt from frames N-1 and N+1 alone and compared with the real frame N.
//   g++ -O2 -fopenmp -I Engine/Dev/PostFX tools/flow-tests/interp_eval.cpp Engine/Dev/PostFX/FlowCore.cpp -o interp_eval
//   interp_eval <dir> <first> <last> [temporal strength, default 0] [out.pfm of the middle frame]
// <dir> holds single-channel float PFM files beauty.NNNN.pfm (see seq_eval.cpp for making them).
// The source frames are two apart, so the motion to bridge is twice the shot's own.
// Prints the mean absolute error (in 0..255 display-like units) and the PSNR for: the motion
// interpolation, a plain dissolve of the two neighbours, and the nearer neighbour used as is.
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
frameName(const std::string& dir, int frame)
{
    char buf[64];

    std::snprintf(buf, sizeof(buf), "/beauty.%04d.pfm", frame);

    return dir + buf;
}

struct Acc
{
    double sumAbs, sumSq;
    long n;
    Acc() : sumAbs(0.), sumSq(0.), n(0) {}

    void add(const std::vector<float>& a, const std::vector<float>& truth)
    {
        for (size_t k = 0; k < a.size(); ++k) {
            const double d = a[k] - truth[k];
            sumAbs += std::fabs(d);
            sumSq += d * d;
        }
        n += (long)a.size();
    }

    void print(const char* name) const
    {
        const double mse = sumSq / n;
        std::printf("  %-28s mean error %6.3f   PSNR %5.2f dB\n", name, sumAbs / n, 10. * std::log10(255. * 255. / std::max(mse, 1e-12)));
    }
};

int
main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: interp_eval dir first last [temporal strength] [out.pfm]\n");

        return 2;
    }
    const std::string dir = argv[1];
    const int first = std::atoi(argv[2]), last = std::atoi(argv[3]);
    const double temporal = argc > 4 ? std::atof(argv[4]) : 0.;
    int w = 0, h = 0;
    std::map<int, std::vector<float> > img;
    for (int t = first; t <= last; ++t) {
        if ( !readPFM(frameName(dir, t), &img[t], &w, &h) ) {
            return 2;
        }
        for (size_t k = 0; k < img[t].size(); ++k) {
            img[t][k] = FlowCore::matchEncode(img[t][k]);
        }
    }
    FlowCore::Params p;
    p.vectorDetail = 1.;
    if (const char* s = std::getenv("FLOW_DETAIL")) p.vectorDetail = std::atof(s);
    // fields between frames two apart, both directions: fwd[t] = t -> t+2, bwd[t] = t -> t-2
    std::map<int, std::vector<float> > fwd, bwd;
    for (int t = first; t + 2 <= last; ++t) {
        FlowCore::computeFlow(&img[t][0], &img[t + 2][0], w, h, p, &fwd[t]);
        FlowCore::computeFlow(&img[t + 2][0], &img[t][0], w, h, p, &bwd[t + 2]);
    }
    Acc motion, dissolve, nearest;
    const int mid = (first + last) / 2;
    for (int t = first + 1; t + 1 <= last; ++t) {
        const int a = t - 1, b = t + 1;
        std::vector<float> ab = fwd[a], ba = bwd[b];
        if (temporal > 0.) {
            // neighbours of the same kind are two frames away on this subsampled clip
            FlowCore::temporalFilter(fwd[a], fwd.count(a + 2) ? &fwd[a + 2] : 0, fwd.count(a - 2) ? &fwd[a - 2] : 0, w, h, temporal, &ab);
            FlowCore::temporalFilter(bwd[b], bwd.count(b - 2) ? &bwd[b - 2] : 0, bwd.count(b + 2) ? &bwd[b + 2] : 0, w, h, temporal, &ba);
        }
        std::vector<float> ca, cb, out( (size_t)w * h ), mix( (size_t)w * h );
        FlowCore::flowConsistency(ab, ba, w, h, &ca);
        FlowCore::flowConsistency(ba, ab, w, h, &cb);
        FlowCore::interpolateFrame(&img[a][0], &img[b][0], 1, w, h, ab, ba, ca, cb, 0.5, &out[0]);
        for (size_t k = 0; k < mix.size(); ++k) {
            mix[k] = 0.5f * (img[a][k] + img[b][k]);
        }
        motion.add(out, img[t]);
        dissolve.add(mix, img[t]);
        nearest.add(img[a], img[t]);
        if ( (argc > 5) && (t == mid) ) {
            FILE* f = std::fopen(argv[5], "wb");
            if (f) {
                std::fprintf(f, "Pf\n%d %d\n-1.0\n", w, h);
                std::fwrite(&out[0], 4, out.size(), f);
                std::fclose(f);
            }
        }
    }
    std::printf("frames %d..%d rebuilt from their two neighbours, %dx%d, temporal smoothing %.2f\n", first + 1, last - 1, w, h, temporal);
    motion.print("motion interpolation");
    dissolve.print("plain dissolve");
    nearest.print("nearest frame");

    return 0;
}
