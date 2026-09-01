#pragma once

#include "core/ImageBuffer.h"
#include <vector>
#include <utility>

namespace ls {

struct LevelsParams {
    float blackPoint = 0.0f; // input value mapped to 0
    float whitePoint = 1.0f; // input value mapped to 1
    float gamma = 1.0f;      // applied after the black/white stretch: out = clamped^(1/gamma)
};

struct SaturationParams {
    float saturation = 1.0f; // 1 = unchanged, 0 = grayscale, >1 = boosted
};

// Applies a levels stretch (black/white point + gamma) identically to every
// channel -- the standard first step before any color-specific adjustment.
ImageBuffer applyLevels(const ImageBuffer& src, const LevelsParams& params);

// Applies a monotonic curve given as control points (x,y in [0,1], sorted
// by x) via piecewise-linear interpolation between them, identically to
// every channel.
ImageBuffer applyCurve(const ImageBuffer& src, const std::vector<std::pair<float, float>>& controlPoints);

// Saturation adjustment: scales each pixel's color components around its
// own luminance (0 = grayscale, 1 = unchanged, >1 = more saturated). No-op
// on single-channel (mono) images.
ImageBuffer applySaturation(const ImageBuffer& src, const SaturationParams& params);

// Histogram of a single channel (or luminance if channel < 0), for driving
// a histogram display widget.
std::vector<int> computeHistogram(const ImageBuffer& src, int channel = -1, int bins = 256);

} // namespace ls
