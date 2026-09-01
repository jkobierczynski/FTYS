#include "proc/MultiPointAlignment.h"

#include <algorithm>
#include <cmath>

namespace ls {

namespace {

// Simple local contrast score (sum of squared horizontal + vertical finite
// differences over a small window) used only to rank candidate point
// positions. Deliberately not the same code path as QualityMetric's
// OpenCV-based Laplacian score -- this needs to run over dozens of small
// candidate windows per frame-pair, and a couple of image-gradient sums are
// enough to tell "this is on a belt edge" from "this is smooth atmosphere/
// disk interior" without pulling in another dependency here.
double localContrastScore(const ImageBuffer& lum, int cx, int cy, int half) {
    int x0 = std::max(1, cx - half);
    int x1 = std::min(lum.width() - 2, cx + half);
    int y0 = std::max(1, cy - half);
    int y1 = std::min(lum.height() - 2, cy + half);
    double sum = 0.0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            double gx = lum.at(x + 1, y, 0) - lum.at(x - 1, y, 0);
            double gy = lum.at(x, y + 1, 0) - lum.at(x, y - 1, 0);
            sum += gx * gx + gy * gy;
        }
    }
    return sum;
}

// Fraction of pixels in the FULL patchSize x patchSize window centered at
// (cx, cy) that fall below `threshold` -- i.e. how much of the actual
// tracking patch (not just the smaller window localContrastScore samples)
// would be background rather than disk. A point placed right on the limb
// scores extremely well on pure gradient contrast (the sharpest edge in
// the whole frame is the disk-to-sky transition itself), but a patch that
// straddles it is mostly tracking motionless black sky, not the planet --
// confirmed on real data: such points reported near-zero shift on every
// frame with anomalously high confidence, because a static background
// "aligns perfectly" while saying nothing about the planet's actual
// motion. This is checked separately from contrast scoring so it can
// reject a candidate outright rather than just nudge its ranking.
double backgroundFraction(const ImageBuffer& lum, int cx, int cy, int patchSize, double threshold) {
    int half = patchSize / 2;
    int x0 = std::max(0, cx - half), x1 = std::min(lum.width() - 1, cx + half);
    int y0 = std::max(0, cy - half), y1 = std::min(lum.height() - 1, cy + half);
    int total = 0, dark = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            ++total;
            if (lum.at(x, y, 0) < threshold) ++dark;
        }
    }
    return total > 0 ? static_cast<double>(dark) / total : 1.0;
}

} // namespace

std::vector<AlignmentPoint> selectAlignmentPoints(const ImageBuffer& referenceLuminance, const Roi& roi,
                                                   int maxPoints, int patchSize, int minSpacing) {
    int half = patchSize / 2;
    struct Candidate { double x, y, score; };
    std::vector<Candidate> candidates;

    // Same object/background split convention as detectObjectRoi (see
    // QualityMetric.h): a pixel below 15% of the disk's own peak
    // brightness counts as background. Used below to reject any candidate
    // whose full tracking patch would touch it.
    float frameMax = 0.0f;
    for (int y = roi.y; y < roi.y + roi.h; ++y)
        for (int x = roi.x; x < roi.x + roi.w; ++x) frameMax = std::max(frameMax, referenceLuminance.at(x, y, 0));
    double bgThreshold = 0.15 * frameMax;

    for (int cy = roi.y + half; cy < roi.y + roi.h - half; cy += minSpacing) {
        for (int cx = roi.x + half; cx < roi.x + roi.w - half; cx += minSpacing) {
            // Reject candidates whose patch would include more than a
            // sliver of background -- these end up tracking motionless
            // sky rather than the planet, however sharp their contrast
            // score looks (see backgroundFraction's comment above).
            if (backgroundFraction(referenceLuminance, cx, cy, patchSize, bgThreshold) > 0.02) continue;
            double score = localContrastScore(referenceLuminance, cx, cy, half / 2);
            candidates.push_back({static_cast<double>(cx), static_cast<double>(cy), score});
        }
    }

    // Degenerate ROI (too small for even one spaced-out candidate, or
    // every candidate touched background -- a very small/thin disk):
    // fall back to the ROI's center so the caller still gets one point
    // rather than none.
    if (candidates.empty()) {
        candidates.push_back({roi.x + roi.w / 2.0, roi.y + roi.h / 2.0, 0.0});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.score > b.score;
    });

    std::vector<AlignmentPoint> points;
    points.reserve(std::min<size_t>(candidates.size(), static_cast<size_t>(maxPoints)));
    for (const auto& c : candidates) {
        if (static_cast<int>(points.size()) >= maxPoints) break;
        points.push_back({c.x, c.y});
    }
    return points;
}

Transform2D estimateLocalShift(const ImageBuffer& referenceLuminance, const ImageBuffer& targetLuminance,
                                const AlignmentPoint& point, int patchSize,
                                const Point2D& recenterOffset, double axisSharpnessThreshold) {
    int half = patchSize / 2;
    int rx0 = static_cast<int>(std::round(point.x)) - half;
    int ry0 = static_cast<int>(std::round(point.y)) - half;
    // ImageBuffer::crop clamps both origin and extent to the buffer's
    // bounds, so a point near the ROI's edge just gets a slightly smaller
    // (still valid) patch rather than an out-of-range access.
    ImageBuffer refPatch = referenceLuminance.crop(rx0, ry0, patchSize, patchSize);

    // Crop the target patch centered on the point's position *plus* this
    // frame's already-known gross translation, not at the reference's raw
    // position -- see the header comment for why. Track the actual
    // (integer-rounded) center used, not the raw recenterOffset, so the
    // coarse+residual decomposition below stays exact rather than off by
    // up to half a pixel of rounding.
    int tCenterX = static_cast<int>(std::round(point.x + recenterOffset.x));
    int tCenterY = static_cast<int>(std::round(point.y + recenterOffset.y));
    ImageBuffer tgtPatch = targetLuminance.crop(tCenterX - half, tCenterY - half, patchSize, patchSize);

    if (refPatch.width() != tgtPatch.width() || refPatch.height() != tgtPatch.height()) {
        // Patches clamped to different sizes near an edge -- fall back to
        // the coarse recenter alone rather than attempting a correlation
        // that isn't comparing like-for-like.
        return Transform2D{recenterOffset.x, recenterOffset.y, 0.0};
    }

    DetailedShift d = estimateShiftDetailed(refPatch, tgtPatch);

    // Only trust the residual correlation on an axis where it actually
    // found something (see the header comment on sharpnessX/Y) -- a flat
    // axis isn't a real zero, it's a non-measurement, and is left at zero
    // residual (i.e. the coarse recenter alone) rather than folding in
    // whatever the noise happened to produce.
    double residualDx = d.sharpnessX >= axisSharpnessThreshold ? d.transform.dx : 0.0;
    double residualDy = d.sharpnessY >= axisSharpnessThreshold ? d.transform.dy : 0.0;

    double coarseDx = tCenterX - point.x;
    double coarseDy = tCenterY - point.y;
    return Transform2D{coarseDx + residualDx, coarseDy + residualDy, d.transform.confidence};
}

namespace {

// Weighted median of (value, weight) pairs: sorts by value and returns the
// value at which cumulative weight first reaches half the total weight.
// Falls back to a plain (unweighted) median if every weight is zero -- an
// "everyone failed the confidence floor" degenerate case where a weighted
// vote has no meaningful answer, but *some* consensus is still better than
// none.
double weightedMedian(std::vector<std::pair<double, double>> valuesWeights) {
    std::sort(valuesWeights.begin(), valuesWeights.end(),
               [](const auto& a, const auto& b) { return a.first < b.first; });
    double total = 0.0;
    for (const auto& vw : valuesWeights) total += vw.second;
    if (total <= 0.0) {
        return valuesWeights[valuesWeights.size() / 2].first;
    }
    double acc = 0.0;
    for (const auto& vw : valuesWeights) {
        acc += vw.second;
        if (acc >= total / 2.0) return vw.first;
    }
    return valuesWeights.back().first;
}

} // namespace

void robustifyPointShifts(std::vector<Transform2D>& pointShifts, double maxDeviationPx, double minConfidence) {
    if (pointShifts.size() < 3) return; // not enough points for a "consensus" to mean anything

    // Each point's own confidence is its vote weight; anything below the
    // floor is excluded from the vote entirely (weight 0), not just given
    // a small weight, since a barely-above-floor point still being wrong
    // most of the time is exactly the case that broke the old unweighted
    // median.
    std::vector<std::pair<double, double>> dxW, dyW;
    dxW.reserve(pointShifts.size());
    dyW.reserve(pointShifts.size());
    for (const auto& t : pointShifts) {
        double w = t.confidence >= minConfidence ? t.confidence : 0.0;
        dxW.push_back({t.dx, w});
        dyW.push_back({t.dy, w});
    }
    double consensusDx = weightedMedian(dxW);
    double consensusDy = weightedMedian(dyW);

    for (auto& t : pointShifts) {
        double dev = std::sqrt((t.dx - consensusDx) * (t.dx - consensusDx) + (t.dy - consensusDy) * (t.dy - consensusDy));
        // Below the confidence floor: never trusted on its own, however
        // close it happens to land to the consensus -- replace it
        // unconditionally. Otherwise, same deviation check as before, just
        // measured against the (now weighted) consensus.
        if (t.confidence < minConfidence || dev > maxDeviationPx) {
            t.dx = consensusDx;
            t.dy = consensusDy;
            // Leave confidence as-is: it still honestly reports that this
            // particular point's own correlation was unreliable, even
            // though its shift now falls back to the consensus.
        }
    }
}

std::vector<BlendWeights> computeBlendWeights(const std::vector<AlignmentPoint>& points, int width, int height) {
    std::vector<BlendWeights> out(static_cast<size_t>(width) * height);
    const int K = BlendWeights::K;
    std::vector<std::pair<double, int>> dists; // (distance^2, point index), reused per pixel
    dists.reserve(points.size());

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            dists.clear();
            for (int pi = 0; pi < static_cast<int>(points.size()); ++pi) {
                double ddx = x - points[pi].x;
                double ddy = y - points[pi].y;
                dists.push_back({ddx * ddx + ddy * ddy, pi});
            }
            int kUse = std::min<int>(K, static_cast<int>(dists.size()));
            std::partial_sort(dists.begin(), dists.begin() + kUse, dists.end());

            BlendWeights bw;
            double wsum = 0.0;
            for (int k = 0; k < K; ++k) {
                if (k < kUse) {
                    double d2 = dists[k].first;
                    double w = 1.0 / (d2 + 4.0); // +4 avoids a singular weight when a pixel sits on a point
                    bw.idx[k] = dists[k].second;
                    bw.w[k] = static_cast<float>(w);
                    wsum += w;
                } else {
                    // Fewer points than K (a tiny/degenerate point set):
                    // repeat the nearest one with zero extra weight.
                    bw.idx[k] = kUse > 0 ? dists[0].second : 0;
                    bw.w[k] = 0.f;
                }
            }
            if (wsum > 0.0) {
                for (int k = 0; k < K; ++k) bw.w[k] = static_cast<float>(bw.w[k] / wsum);
            }
            out[static_cast<size_t>(y) * width + x] = bw;
        }
    }
    return out;
}

} // namespace ls
