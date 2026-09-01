// One-off diagnostic: loads a PNG, runs it through the wavelet decomposition
// used by waveletSharpen, and prints per-layer amplitude statistics. Used to
// determine whether a weak-looking sharpening result on real data is a bug
// in the algorithm or simply that the source image has little fine-scale
// content left after stacking (e.g. from a soft/out-of-focus capture).

#include "core/ImageBuffer.h"
#include "proc/WaveletSharpen.h"

#include <QImage>
#include <algorithm>
#include <cmath>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <png>\n", argv[0]);
        return 2;
    }
    QImage img(argv[1]);
    if (img.isNull()) {
        fprintf(stderr, "failed to load %s\n", argv[1]);
        return 1;
    }
    img = img.convertToFormat(QImage::Format_Grayscale8);
    int w = img.width(), h = img.height();
    ls::ImageBuffer buf(w, h, 1);
    for (int y = 0; y < h; ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < w; ++x) buf.at(x, y, 0) = row[x] / 255.0f;
    }

    // Reproduce the a trous decomposition inline (mirrors WaveletSharpen.cpp)
    // just to print per-layer stats -- not exported from the library since
    // normal callers only need the final sharpened image.
    auto convolveTrous = [&](const std::vector<float>& in, std::vector<float>& out, int step) {
        static const double k[5] = {1.0 / 16, 4.0 / 16, 6.0 / 16, 4.0 / 16, 1.0 / 16};
        std::vector<float> tmp(in.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double sum = 0.0;
                for (int t = -2; t <= 2; ++t) {
                    int xx = std::clamp(x + t * step, 0, w - 1);
                    sum += k[t + 2] * in[static_cast<size_t>(y) * w + xx];
                }
                tmp[static_cast<size_t>(y) * w + x] = static_cast<float>(sum);
            }
        out.resize(in.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double sum = 0.0;
                for (int t = -2; t <= 2; ++t) {
                    int yy = std::clamp(y + t * step, 0, h - 1);
                    sum += k[t + 2] * tmp[static_cast<size_t>(yy) * w + x];
                }
                out[static_cast<size_t>(y) * w + x] = static_cast<float>(sum);
            }
    };

    std::vector<float> cCur(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) cCur[static_cast<size_t>(y) * w + x] = buf.at(x, y, 0);

    int step = 1;
    for (int j = 0; j < 4; ++j) {
        std::vector<float> cNext;
        convolveTrous(cCur, cNext, step);
        double maxAbs = 0, sumAbs = 0, sumSq = 0;
        for (size_t i = 0; i < cCur.size(); ++i) {
            double d = cCur[i] - cNext[i];
            maxAbs = std::max(maxAbs, std::fabs(d));
            sumAbs += std::fabs(d);
            sumSq += d * d;
        }
        double mean = sumAbs / cCur.size();
        double rms = std::sqrt(sumSq / cCur.size());
        printf("layer %d (step=%d): maxAbs=%.5f meanAbs=%.5f rms=%.5f  (in 0..1 units, x255=%.2f/%.2f/%.2f)\n",
               j, step, maxAbs, mean, rms, maxAbs * 255, mean * 255, rms * 255);
        cCur = cNext;
        step *= 2;
    }
    return 0;
}
