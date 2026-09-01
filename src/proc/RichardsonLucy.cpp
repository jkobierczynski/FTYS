#include "proc/RichardsonLucy.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ls {

namespace {

std::vector<float> makeGaussianKernel(double sigma, int& radius) {
    radius = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    int size = 2 * radius + 1;
    std::vector<float> k(static_cast<size_t>(size) * size);
    double sum = 0.0;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            double v = std::exp(-(x * x + y * y) / (2.0 * sigma * sigma));
            k[static_cast<size_t>(y + radius) * size + (x + radius)] = static_cast<float>(v);
            sum += v;
        }
    }
    for (auto& v : k) v = static_cast<float>(v / sum);
    return k;
}

std::vector<float> convolve2D(const std::vector<float>& img, int w, int h,
                               const std::vector<float>& kernel, int radius) {
    int size = 2 * radius + 1;
    std::vector<float> out(img.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double sum = 0.0;
            for (int ky = -radius; ky <= radius; ++ky) {
                int sy = std::clamp(y + ky, 0, h - 1);
                for (int kx = -radius; kx <= radius; ++kx) {
                    int sx = std::clamp(x + kx, 0, w - 1);
                    sum += img[static_cast<size_t>(sy) * w + sx] *
                           kernel[static_cast<size_t>(ky + radius) * size + (kx + radius)];
                }
            }
            out[static_cast<size_t>(y) * w + x] = static_cast<float>(sum);
        }
    }
    return out;
}

} // namespace

ImageBuffer richardsonLucyDeconvolve(const ImageBuffer& src, const RLParams& params) {
    const int w = src.width(), h = src.height(), c = src.channels();
    int radius = 1;
    auto kernel = makeGaussianKernel(params.psfSigma, radius);
    ImageBuffer out(w, h, c);
    const size_t n = static_cast<size_t>(w) * h;

    for (int ch = 0; ch < c; ++ch) {
        std::vector<float> observed(n);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) observed[static_cast<size_t>(y) * w + x] = src.at(x, y, ch);

        std::vector<float> estimate = observed;
        for (int it = 0; it < params.iterations; ++it) {
            auto conv = convolve2D(estimate, w, h, kernel, radius);
            std::vector<float> ratio(conv.size());
            for (size_t i = 0; i < ratio.size(); ++i) ratio[i] = observed[i] / std::max(conv[i], 1e-6f);
            // Gaussian PSF is symmetric, so correlating with the flipped
            // kernel is the same convolution as the forward step.
            auto corr = convolve2D(ratio, w, h, kernel, radius);
            for (size_t i = 0; i < estimate.size(); ++i)
                estimate[i] = std::clamp(estimate[i] * corr[i], 0.0f, 1.0f);
        }

        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) out.at(x, y, ch) = estimate[static_cast<size_t>(y) * w + x];
    }
    return out;
}

} // namespace ls
