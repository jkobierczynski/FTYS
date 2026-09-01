#pragma once

#include "core/ImageBuffer.h"
#include "proc/Alignment.h"
#include "proc/MultiPointAlignment.h"
#include <functional>
#include <vector>

namespace ls {

struct DrizzleParams {
    double scale = 2.0;         // output oversampling factor relative to input resolution
    double dropFraction = 0.65; // "pixfrac": linear shrink factor of each input pixel's footprint
};

// Classic (Fruchter & Hook) drizzle: each source pixel's local (dx, dy) is
// now blended from `alignment`'s tracking points (see
// MultiPointAlignment.h) rather than one global shift for the whole frame,
// so different regions of the disk can carry different local motion. Every
// input pixel is shrunk to `dropFraction` of its size and splatted onto the
// output grid at its sub-pixel-shifted position, weighted by the overlap
// area between its footprint and each output pixel; a running
// weight/coverage accumulator normalizes the result at the end.
//
// This increases effective sampling when frames carry genuine sub-pixel
// dither between them; it does not manufacture resolution the input frames
// don't contain, and a bad dropFraction/scale combination (too small a
// footprint for the amount of dither) can leave gaps of zero coverage.
// `progressCallback`, when set, is invoked with a 0-100 percentage as
// frames are splatted onto the output accumulator (the outer loop is over
// frames here, unlike stackFrames' row loop, since each frame's full
// splat pass is the natural unit of work). Optional for the same reason as
// stackFrames'.
ImageBuffer drizzleStack(const std::vector<ImageBuffer>& frames,
                          const MultiPointAlignmentResult& alignment,
                          const DrizzleParams& params,
                          const std::function<void(int)>& progressCallback = nullptr);

} // namespace ls
