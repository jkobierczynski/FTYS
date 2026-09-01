#pragma once

#include "core/ImageBuffer.h"
#include <vector>

namespace ls {

struct WaveletParams {
    // Per-scale gain, finest detail first. A gain of 1.0 leaves that scale
    // unchanged, >1 boosts it (sharpening), <1 (including 0) suppresses it
    // (useful for damping the finest, noisiest scale). The number of
    // entries sets the number of wavelet scales decomposed.
    std::vector<double> layerGains = {1.3, 1.2, 1.0, 1.0};
};

// A trous (starlet) wavelet decomposition/reconstruction for sharpening.
// Operates on each channel independently so color images sharpen without
// hue shifts.
ImageBuffer waveletSharpen(const ImageBuffer& src, const WaveletParams& params);

} // namespace ls
