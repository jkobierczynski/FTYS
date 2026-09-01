#pragma once

#include "core/ImageBuffer.h"

namespace ls {

struct Transform2D {
    double dx = 0.0;         // shift applied when resampling (see applyShift)
    double dy = 0.0;
    double confidence = 0.0; // normalized correlation peak height, roughly in [0,1]
};

// FFT-based phase correlation between `reference` and `target` (both mono
// luminance buffers of identical size). Returns the subpixel shift that,
// passed to applyShift(target, shift), best resamples target onto
// reference's coordinate frame.
//
// Rotation is intentionally not estimated here: frame-to-frame rotation
// from atmospheric seeing within a single capture run is negligible; the
// rotation that does accumulate over a longer alt-az session is a single
// field-rotation correction applied once to the whole run, not per frame,
// and is out of scope for this pass -- translation dominates seeing-induced
// motion, which is what this module targets.
Transform2D estimateShift(const ImageBuffer& reference, const ImageBuffer& target);

// Same correlation as estimateShift, but also exposes, independently for
// each axis, how much the correlation's primary peak actually dominates
// its row/column versus the tallest *other* candidate peak found there
// (a small neighborhood around the primary peak is excluded first, so a
// rival has to be a genuinely separate candidate). A real, unambiguous
// match has one peak that clearly wins; an aliased match (e.g. a
// periodic surface texture a "period" away) or a noise-driven one has
// multiple comparable candidates and a ratio near 1. This is the
// standard peak-to-sidelobe idea used to judge correlation-tracker
// reliability.
//
// An earlier version of this used the peak's local curvature instead,
// normalized by its height -- checked directly against real capture data
// (out2_2.avi) and found not to discriminate at all: phase-only
// correlation (the whitened cross-power spectrum used above) produces a
// sharp, near-impulse-like peak almost regardless of whether the match is
// genuine, since every frequency contributes equal magnitude once
// normalized, so curvature-based "sharpness" came out similarly high
// (1-2.5) whether the resulting shift was later confirmed right or
// wrong. The peak-to-sidelobe ratio actually separates the two on real
// data; see MultiPointAlignment.cpp's recentered per-point tracking,
// which gates dx and dy independently on it.
struct DetailedShift {
    Transform2D transform;
    double sharpnessX = 0.0;
    double sharpnessY = 0.0;
};
DetailedShift estimateShiftDetailed(const ImageBuffer& reference, const ImageBuffer& target);

// Resamples `src` by `t` via bilinear interpolation, producing an image of
// the same size (out-of-bounds samples are 0). out(x,y) = src(x + t.dx, y + t.dy).
ImageBuffer applyShift(const ImageBuffer& src, const Transform2D& t);

} // namespace ls
