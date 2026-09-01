#pragma once

#include "core/ImageBuffer.h"

namespace ls {

// Manual per-channel correction for lateral chromatic aberration ("color
// fringing"): the green channel is treated as the fixed reference and the
// red and blue channels are each independently resampled by their own
// small subpixel (dx, dy) shift to bring them back into register with it.
//
// This is a single global 2D shift per channel, not a per-radius/radial
// (barrel/pincushion-style) model -- across the small field of view a
// planetary disk actually occupies, lateral CA from a refractor/mirror-lens
// combination is well approximated by a constant offset, and a full radial
// model would be solving a problem this scale of image doesn't have.
struct ChromaticAberrationParams {
    double redDx = 0.0, redDy = 0.0;
    double blueDx = 0.0, blueDy = 0.0;
};

// Resamples the red/blue channels by (redDx, redDy)/(blueDx, blueDy) via
// bilinear interpolation (same convention as applyShift: out(x,y) =
// src(x + dx, y + dy)); green is copied through unchanged since it's the
// reference the other two are being aligned to. No-op (returns src as-is)
// on non-RGB (mono) images -- there's no channel misregistration to correct
// when there's only one channel -- and when all four offsets are exactly
// zero, so leaving the panel at its default doesn't cost a needless copy.
ImageBuffer correctChromaticAberration(const ImageBuffer& src, const ChromaticAberrationParams& params);

// Estimates a starting point for the manual sliders by reusing inter-frame
// alignment's FFT phase correlation (see Alignment.h) between channels
// instead of between frames -- the underlying problem is identical: find
// the (dx, dy) that best resamples one single-channel image onto another's
// coordinate frame. Green is the "reference" channel and red/blue are each
// treated as a "target", exactly as estimateShift treats a target frame
// against the reference frame. No-op (returns the zero/identity params) on
// non-RGB images.
ChromaticAberrationParams detectChromaticAberration(const ImageBuffer& rgb);

} // namespace ls
