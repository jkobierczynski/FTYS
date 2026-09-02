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

struct ColorBalanceParams {
    // Independent per-channel multiplicative gain -- the classic "color
    // balance" control (push blue up to cool the image, red up to warm it,
    // etc). 1 = unchanged on that channel.
    float redGain = 1.0f;
    float greenGain = 1.0f;
    float blueGain = 1.0f;
};

struct HueParams {
    // Rotation applied to every pixel's hue angle, in degrees; wraps
    // around at +/-360. Saturation and value/brightness are left alone --
    // this only spins the color wheel.
    float hueDegrees = 0.0f;
};

struct BrightnessParams {
    // Additive offset applied to every channel, then clamped back to
    // [0, 1]. Deliberately simple (not multiplicative/exposure-style) so
    // its effect is easy to reason about alongside the black/white levels
    // stretch, which already does the heavy lifting for overall exposure.
    float brightness = 0.0f;
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

// Independent per-channel gain (color balance / white balance). No-op on
// single-channel (mono) images, same convention as applySaturation.
ImageBuffer applyColorBalance(const ImageBuffer& src, const ColorBalanceParams& params);

// Rotates every pixel's hue by hueDegrees, leaving saturation and value
// numerically unchanged (converts to HSV, adds to H, converts back). No-op
// on single-channel (mono) images -- hue is meaningless there.
ImageBuffer applyHueRotation(const ImageBuffer& src, const HueParams& params);

// Adds a constant offset to every sample (every channel, mono or color
// alike) and clamps back to [0, 1].
ImageBuffer applyBrightness(const ImageBuffer& src, const BrightnessParams& params);

// Histogram of a single channel (or luminance if channel < 0), for driving
// a histogram display widget.
std::vector<int> computeHistogram(const ImageBuffer& src, int channel = -1, int bins = 256);

} // namespace ls
