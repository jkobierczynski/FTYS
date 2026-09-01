#pragma once

#include "core/ImageBuffer.h"
#include "proc/Alignment.h"
#include "proc/QualityMetric.h"
#include <vector>

namespace ls {

// A tracking point's position, fixed in the reference frame's native pixel
// coordinates for the whole alignment run (only its per-frame *shift*
// varies -- the point itself doesn't move).
struct AlignmentPoint {
    double x = 0.0, y = 0.0;
};

// Automatic placement of tracking points across the imaged object, mirroring
// AutoStakkert's automatic Alignment Point (AP) placement: candidates are
// laid out on a grid spanning `roi`, scored by local contrast (well-textured
// spots -- belt edges, festoons, the limb -- correlate far more reliably
// than smooth, near-featureless patches), and the highest-scoring
// `maxPoints` are kept. Always returns at least a few points (falling back
// past the contrast threshold if necessary) so a very smooth/low-contrast
// disk still gets *something* to track.
std::vector<AlignmentPoint> selectAlignmentPoints(const ImageBuffer& referenceLuminance, const Roi& roi,
                                                   int maxPoints = 16, int patchSize = 56, int minSpacing = 44);

// One point's local translation between reference and target.
//
// `recenterOffset` is the frame's already-known *gross* object-center
// translation (target's detectObjectCenter minus the reference's -- see
// QualityMetric.h), computed once per frame by the caller and passed to
// every point. Real-data testing (a real Jupiter capture, out2_2.avi) found
// that a large share of what looked like "unreliable per-point tracking"
// was actually the whole disk translating -- something a simple brightness
// centroid measures far more robustly than patch-level phase correlation
// does, since a centroid can't lock onto the wrong periodic peak the way
// correlation can, and it still works when local surface contrast is too
// weak to correlate on at all. Removing that gross translation before
// cropping the target patch means the FFT correlation below only has to
// find whatever's genuinely left over -- local seeing distortion, real
// surface evolution -- rather than tens of pixels of bulk motion on top of
// it.
//
// That residual correction is applied per axis independently, and only
// where the correlation actually found a dominant, unambiguous peak:
// `axisSharpnessThreshold` gates dx and dy separately against
// estimateShiftDetailed's sharpnessX/Y peak-to-sidelobe ratios (see
// Alignment.h). An axis whose top peak doesn't clearly beat the next-best
// rival candidate isn't a real measurement, and folding it in anyway just
// adds noise on top of an already-good coarse answer -- that axis is left
// at the gross recenter alone. The default (2.0) was picked by sweeping
// real out2_2.avi frames and choosing the point where nearly every point's
// residual collapsed to agreement with the independently-measured
// recenter, with only the genuine occasional outlier left for
// robustifyPointShifts to catch (see the README's multi-point alignment
// section for the actual numbers).
//
// Passing a default-constructed `recenterOffset` (the zero vector) skips
// the gross-translation removal but still applies the same per-axis gate
// -- a caller with no gross-translation estimate available should
// probably pass a lower threshold, since without recentering, residuals
// are much larger and this specific default hasn't been validated in that
// regime.
Transform2D estimateLocalShift(const ImageBuffer& referenceLuminance, const ImageBuffer& targetLuminance,
                                const AlignmentPoint& point, int patchSize,
                                const Point2D& recenterOffset = Point2D{}, double axisSharpnessThreshold = 2.0);

// Per-frame robustness pass: a point whose estimated shift differs from
// this frame's consensus shift (across all points) by more than
// `maxDeviationPx` is almost certainly a bad local lock (a patch that
// briefly had too little contrast, a compression artifact, etc.), not a
// real local seeing distortion -- real atmospheric distortion moves
// neighboring points together, it doesn't fling one point off by itself.
// Such a point's shift is replaced by the frame's consensus shift so it
// can't inject a spurious warp into its neighborhood of the blended flow
// field.
//
// The consensus used to be a plain unweighted median. Real-data
// instrumentation against a low-contrast, MJPEG-compressed capture
// (out2_2.avi) showed that breaks down when most points -- not just a
// minority -- carry only marginal signal: an unweighted median is then
// dominated by whatever value that unreliable majority's noise happens to
// average out to (measured landing close to zero, frame after frame, even
// while the disk was actually drifting tens of pixels), which then
// overwrites the few points that *did* find the real answer instead of the
// other way around. The consensus is now a confidence-weighted median for
// dx and dy independently, so a handful of higher-confidence points can
// outvote a larger number of low-confidence ones. Any point whose own
// confidence is below `minConfidence` contributes no weight to that vote
// and is unconditionally replaced by the consensus, regardless of how
// close its own value happens to land to it -- it's never trusted on its
// own.
//
// Caveat worth stating plainly: this does not help (and can even
// slightly worsen) a point whose confidence is high for the *wrong*
// reason -- e.g. a patch dominated by a smooth, static limb-brightness
// gradient rather than real surface texture, which correlates
// "confidently" against itself without reflecting real motion. Weighting
// by confidence gives that kind of point *more* say, not less. That
// failure mode needs a better reliability check than raw confidence
// (tracked separately); this pass only fixes the case where the majority
// is honestly low-confidence noise.
void robustifyPointShifts(std::vector<Transform2D>& pointShifts, double maxDeviationPx = 12.0,
                           double minConfidence = 0.03);

// Precomputed, geometry-only blend weights: for every pixel in a
// width x height frame, the indices of and weights toward the K nearest
// alignment points (inverse-distance weighted, normalized to sum to 1).
// This depends only on where the points *are*, not on any frame's content,
// so it's computed once per alignment run and reused for every frame --
// the actual per-frame cost of turning point shifts into a locally-varying
// warp is then just a K-term weighted sum per pixel.
struct BlendWeights {
    static constexpr int K = 3;
    int idx[K] = {0, 0, 0};
    float w[K] = {0.f, 0.f, 0.f};
};
std::vector<BlendWeights> computeBlendWeights(const std::vector<AlignmentPoint>& points, int width, int height);

// Blends one frame's per-point shifts into a single local (dx, dy) at pixel
// (x, y), using that pixel's precomputed nearest-point weights. This is the
// per-pixel replacement for reading a single global Transform2D -- called
// from Stacker.cpp / Drizzle.cpp wherever they used to read transforms[i].
inline Transform2D blendShiftAt(int x, int y, int width, const std::vector<BlendWeights>& blendWeights,
                                 const std::vector<Transform2D>& pointShifts) {
    const BlendWeights& bw = blendWeights[static_cast<size_t>(y) * width + x];
    double dx = 0.0, dy = 0.0, conf = 0.0;
    for (int k = 0; k < BlendWeights::K; ++k) {
        const Transform2D& t = pointShifts[static_cast<size_t>(bw.idx[k])];
        dx += bw.w[k] * t.dx;
        dy += bw.w[k] * t.dy;
        conf += bw.w[k] * t.confidence;
    }
    return Transform2D{dx, dy, conf};
}

// Everything the stacking/drizzle stages need to turn a set of per-frame,
// per-point shifts into a locally-varying warp -- bundled together so
// PipelineController only has to build and hand over one object.
// perFramePointShifts is parallel to the frame list being stacked
// (perFramePointShifts[i] parallel to `points`); blendWeights is shared,
// sized width*height, in the reference frame's native resolution.
struct MultiPointAlignmentResult {
    std::vector<AlignmentPoint> points;
    std::vector<std::vector<Transform2D>> perFramePointShifts;
    std::vector<BlendWeights> blendWeights;
    int width = 0, height = 0;
    // The patch size points were tracked with -- kept alongside the result
    // so a caller drawing a box around each point (e.g. the GUI's
    // alignment inspector) knows how big to make it without having to
    // separately remember whatever value was passed to
    // selectAlignmentPoints/estimateLocalShift.
    int patchSize = 0;
    double averageConfidence = 0.0;
};

} // namespace ls
