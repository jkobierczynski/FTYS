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

ImageBuffer applyColorBalance(const ImageBuffer& src, const ColorBalanceParams& params) {
    if (src.channels() != 3) return src; // no-op on mono, same convention as applySaturation

    ImageBuffer out(src.width(), src.height(), 3);
    for (int y = 0; y < src.height(); ++y) {
        for (int x = 0; x < src.width(); ++x) {
            out.at(x, y, 0) = std::clamp(src.at(x, y, 0) * params.redGain, 0.0f, 1.0f);
            out.at(x, y, 1) = std::clamp(src.at(x, y, 1) * params.greenGain, 0.0f, 1.0f);
            out.at(x, y, 2) = std::clamp(src.at(x, y, 2) * params.blueGain, 0.0f, 1.0f);
        }
    }
    return out;
}

namespace {

// Minimal RGB<->HSV (all channels in [0,1], hue in degrees) -- just enough
// to rotate hue while leaving saturation/value numerically alone. Not a
// general-purpose colorimetric conversion, only used internally here.
void rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    float maxc = std::max({r, g, b});
    float minc = std::min({r, g, b});
    v = maxc;
    float delta = maxc - minc;
    s = maxc <= 1e-6f ? 0.0f : delta / maxc;
    if (delta <= 1e-6f) {
        h = 0.0f; // achromatic (gray/black) -- hue is undefined, pick 0
        return;
    }
    if (maxc == r) h = std::fmod((g - b) / delta, 6.0f);
    else if (maxc == g) h = (b - r) / delta + 2.0f;
    else h = (r - g) / delta + 4.0f;
    h *= 60.0f;
    if (h < 0.0f) h += 360.0f;
}

void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    float c = v * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float m = v - c;
    float r1 = 0, g1 = 0, b1 = 0;
    if (hp < 1.0f) { r1 = c; g1 = x; b1 = 0.0f; }
    else if (hp < 2.0f) { r1 = x; g1 = c; b1 = 0.0f; }
    else if (hp < 3.0f) { r1 = 0.0f; g1 = c; b1 = x; }
    else if (hp < 4.0f) { r1 = 0.0f; g1 = x; b1 = c; }
    else if (hp < 5.0f) { r1 = x; g1 = 0.0f; b1 = c; }
    else { r1 = c; g1 = 0.0f; b1 = x; }
    r = r1 + m;
    g = g1 + m;
    b = b1 + m;
}

} // namespace

ImageBuffer applyHueRotation(const ImageBuffer& src, const HueParams& params) {
    if (src.channels() != 3) return src; // no-op on mono -- hue is meaningless there

    ImageBuffer out(src.width(), src.height(), 3);
    for (int y = 0; y < src.height(); ++y) {
        for (int x = 0; x < src.width(); ++x) {
            float h, s, v;
            rgbToHsv(src.at(x, y, 0), src.at(x, y, 1), src.at(x, y, 2), h, s, v);
            h = std::fmod(h + params.hueDegrees, 360.0f);
            if (h < 0.0f) h += 360.0f;
            float r, g, b;
            hsvToRgb(h, s, v, r, g, b);
            out.at(x, y, 0) = std::clamp(r, 0.0f, 1.0f);
            out.at(x, y, 1) = std::clamp(g, 0.0f, 1.0f);
            out.at(x, y, 2) = std::clamp(b, 0.0f, 1.0f);
        }
    }
    return out;
}

ImageBuffer applyBrightness(const ImageBuffer& src, const BrightnessParams& params) {
    ImageBuffer out(src.width(), src.height(), src.channels());
    size_t n = src.sampleCount();
    for (size_t i = 0; i < n; ++i) out.data()[i] = std::clamp(src.data()[i] + params.brightness, 0.0f, 1.0f);
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
