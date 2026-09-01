#pragma once

#include "core/ImageBuffer.h"

namespace ls {

struct Roi {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Point2D {
    double x = 0.0, y = 0.0;
};

// Detects a bounding box around the dominant bright object in `lum` by
// thresholding at threshFraction of the frame's max value, then padding by
// `margin` pixels. Falls back to the full frame if nothing crosses the
// threshold (e.g. a near-empty or pathological frame).
Roi detectObjectRoi(const ImageBuffer& lum, float threshFraction = 0.2f, int margin = 8);

// Sub-pixel, brightness-weighted centroid of the same thresholded object
// detectObjectRoi finds (pixels at or above threshFraction of the frame's
// max, weighted by how far above that threshold they sit). Deliberately
// not a correlation-based estimate: a weighted centroid can't lock onto a
// spurious periodic peak the way FFT phase correlation can, and it still
// works when local surface contrast is too weak to correlate on at all --
// only the object's bright/dark footprint matters, not its detail. Used to
// track a frame's *gross* object position before any patch-level
// correlation runs (see MultiPointAlignment.cpp), which real-data testing
// showed was often the dominant source of the large, erratic per-point
// shifts a fixed-position patch grid was trying (and often failing) to
// measure directly. Falls back to the frame's own center if nothing
// crosses the threshold.
Point2D detectObjectCenter(const ImageBuffer& lum, float threshFraction = 0.15f);

// Variance of the Laplacian within `roi` -- a standard, well-behaved focus/
// sharpness proxy: well-resolved fine structure produces a high-variance
// second derivative, while blur flattens it. Restricting to the object's
// own bounding box (rather than the whole frame) keeps a mostly-black
// planetary background from diluting the score with sensor noise.
double laplacianVarianceScore(const ImageBuffer& lum, const Roi& roi);

// Convenience used by the frame selector: detect ROI, then score it.
double assessFrameQuality(const ImageBuffer& lum);

} // namespace ls
