#pragma once

#include "core/FrameSource.h"
#include <fitsio.h>
#include <vector>

namespace ls {

// Reader for FITS single images and cubes (fits/fit/fts). Two shapes are
// recognized:
//   NAXIS=2                       -> a single 2D mono frame (frameCount=1)
//   NAXIS=3, NAXIS3 == 3          -> a single RGB frame stored as 3 planes
//   NAXIS=3, NAXIS3 == N (N != 3) -> a cube of N mono frames (frameCount=N)
//
// The NAXIS3==3 heuristic for "this is RGB, not a 3-frame sequence" is a
// genuine ambiguity in how FITS cubes get used across tools; it matches the
// common convention but isn't bulletproof for a 3-frame planetary capture.
//
// Pixel values are read through cfitsio as physical floats (BSCALE/BZERO
// applied automatically), then rescaled to 8/16-bit integer working buffers.
// For >8-bit data there is no universal FITS convention for dynamic range,
// so the min/max of the *first* frame establishes a fixed scale used for
// every subsequent frame in the file -- this keeps relative brightness
// consistent across the sequence for quality scoring and stacking.
class FitsReader : public FrameSourceBase {
public:
    explicit FitsReader(const std::string& path);
    ~FitsReader() override;

    RawFrame readFrame(size_t index) override;

private:
    std::vector<float> readPlaneAxis3(int axis3Index0based) const;

    fitsfile* fptr_ = nullptr;
    int naxis_ = 0;
    long naxes_[3] = {0, 0, 0};
    int bitpix_ = 0;
    bool rgbCube_ = false;
    bool sixteenBit_ = false;
    float normOffset_ = 0.0f;
    float normScale_ = 1.0f;
};

} // namespace ls
