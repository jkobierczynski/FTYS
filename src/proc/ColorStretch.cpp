#include "proc/ColorStretch.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ls {

ImageBuffer applyLevels(const ImageBuffer& src, const LevelsParams& params) {
    ImageBuffer out(src.width(), src.height(), src.channels());
    float range = params.whitePoint - params.blackPoint;
    if (range <= 1e-6f) range = 1e-6f;
    double invGamma = params.gamma > 1e-6f ? 1.0 / params.gamma : 1.0;

    size_t n = src.sampleCount();
    for (size_t i = 0; i < n; ++i) {
        float t = (src.data()[i] - params.blackPoint) / range;
        t = std::clamp(t, 0.0f, 1.0f);
        out.data()[i] = static_cast<float>(std::pow(t, invGamma));
    }
    return out;
}

ImageBuffer applyCurve(const ImageBuffer& src, const std::vector<std::pair<float, float>>& controlPoints) {
    if (controlPoints.size() < 2)
        throw std::runtime_error("applyCurve: need at least two control points");

    ImageBuffer out(src.width(), src.height(), src.channels());
    size_t n = src.sampleCount();
    for (size_t i = 0; i < n; ++i) {
        float v = std::clamp(src.data()[i], 0.0f, 1.0f);

        // Find the bracketing segment; control points are assumed sorted by x.
        size_t seg = 0;
        while (seg + 1 < controlPoints.size() - 1 && controlPoints[seg + 1].first < v) ++seg;
        const auto& p0 = controlPoints[seg];
        const auto& p1 = controlPoints[seg + 1];
        float dx = p1.first - p0.first;
        float t = dx > 1e-6f ? (v - p0.first) / dx : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        out.data()[i] = p0.second + t * (p1.second - p0.second);
    }
    return out;
}

ImageBuffer applySaturation(const ImageBuffer& src, const SaturationParams& params) {
    if (src.channels() != 3) return src; // no-op on mono data

    ImageBuffer out(src.width(), src.height(), 3);
    for (int y = 0; y < src.height(); ++y) {
        for (int x = 0; x < src.width(); ++x) {
            float r = src.at(x, y, 0), g = src.at(x, y, 1), b = src.at(x, y, 2);
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            out.at(x, y, 0) = std::clamp(lum + params.saturation * (r - lum), 0.0f, 1.0f);
            out.at(x, y, 1) = std::clamp(lum + params.saturation * (g - lum), 0.0f, 1.0f);
            out.at(x, y, 2) = std::clamp(lum + params.saturation * (b - lum), 0.0f, 1.0f);
        }
    }
    return out;
}

std::vector<int> computeHistogram(const ImageBuffer& src, int channel, int bins) {
    std::vector<int> hist(bins, 0);
    int w = src.width(), h = src.height();

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float v;
            if (channel < 0) {
                if (src.channels() == 1) {
                    v = src.at(x, y, 0);
                } else {
                    v = 0.2126f * src.at(x, y, 0) + 0.7152f * src.at(x, y, 1) + 0.0722f * src.at(x, y, 2);
                }
            } else {
                v = src.at(x, y, std::min(channel, src.channels() - 1));
            }
            int bin = static_cast<int>(std::clamp(v, 0.0f, 1.0f) * (bins - 1) + 0.5f);
            hist[bin]++;
        }
    }
    return hist;
}

} // namespace ls
