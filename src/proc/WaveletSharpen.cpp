#include "proc/WaveletSharpen.h"

#include <algorithm>
#include <cstddef>

namespace ls {

namespace {

// One "a trous" pass: 2D separable convolution with the B3-spline kernel
// [1,4,6,4,1]/16, with `step`-pixel holes between taps (step doubles each
// scale, which is what gives the algorithm its name and its dyadic scale
// spacing). Edge samples are clamped rather than wrapped.
void convolveTrous(const std::vector<float>& in, std::vector<float>& out, int w, int h, int step) {
    static const double k[5] = {1.0 / 16, 4.0 / 16, 6.0 / 16, 4.0 / 16, 1.0 / 16};
    std::vector<float> tmp(in.size());

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double sum = 0.0;
            for (int t = -2; t <= 2; ++t) {
                int xx = std::clamp(x + t * step, 0, w - 1);
                sum += k[t + 2] * in[static_cast<size_t>(y) * w + xx];
            }
            tmp[static_cast<size_t>(y) * w + x] = static_cast<float>(sum);
        }
    }

    out.resize(in.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double sum = 0.0;
            for (int t = -2; t <= 2; ++t) {
                int yy = std::clamp(y + t * step, 0, h - 1);
                sum += k[t + 2] * tmp[static_cast<size_t>(yy) * w + x];
            }
            out[static_cast<size_t>(y) * w + x] = static_cast<float>(sum);
        }
    }
}

} // namespace

ImageBuffer waveletSharpen(const ImageBuffer& src, const WaveletParams& params) {
    const int w = src.width(), h = src.height(), c = src.channels();
    ImageBuffer out(w, h, c);
    const int J = static_cast<int>(params.layerGains.size());
    const size_t n = static_cast<size_t>(w) * h;

    for (int ch = 0; ch < c; ++ch) {
        std::vector<float> cCur(n);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) cCur[static_cast<size_t>(y) * w + x] = src.at(x, y, ch);

        std::vector<std::vector<float>> layers(J);
        std::vector<float> cNext;
        int step = 1;
        for (int j = 0; j < J; ++j) {
            convolveTrous(cCur, cNext, w, h, step);
            layers[j].resize(n);
            for (size_t i = 0; i < n; ++i) layers[j][i] = cCur[i] - cNext[i];
            cCur = cNext;
            step *= 2;
        }
        // cCur now holds the residual (smoothest) approximation c_J.
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                size_t i = static_cast<size_t>(y) * w + x;
                double v = cCur[i];
                for (int j = 0; j < J; ++j) v += params.layerGains[j] * layers[j][i];
                out.at(x, y, ch) = static_cast<float>(std::clamp(v, 0.0, 1.0));
            }
        }
    }
    return out;
}

} // namespace ls
