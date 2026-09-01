#pragma once

#include "core/ImageBuffer.h"

namespace ls {

struct RLParams {
    int iterations = 15;
    double psfSigma = 1.6; // Gaussian PSF sigma in pixels, approximating optics+seeing blur
};

// Richardson-Lucy deconvolution against a Gaussian PSF approximation of the
// combined optical and atmospheric blur. Operates per-channel. This assumes
// a symmetric, spatially-invariant PSF -- a simplification that holds
// reasonably well for a well-aligned, well-stacked planetary image, though
// a measured or asymmetric PSF would do better under extreme seeing.
ImageBuffer richardsonLucyDeconvolve(const ImageBuffer& src, const RLParams& params);

} // namespace ls
