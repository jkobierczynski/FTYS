#include "proc/Stacker.h"

#include <algorithm>
#include <stdexcept>
#include <cmath>

namespace ls {

ImageBuffer stackFrames(const std::vector<ImageBuffer>& frames,
                         const MultiPointAlignmentResult& alignment,
                         const StackParams& params,
                         const std::function<void(int)>& progressCallback) {
    if (frames.empty()) return ImageBuffer();
    if (frames.size() != alignment.perFramePointShifts.size())
        throw std::runtime_error("stackFrames: frames/alignment size mismatch");

    const int w = frames[0].width();
    const int h = frames[0].height();
    const int c = frames[0].channels();
    const size_t n = frames.size();

    // Deliberately does NOT pre-build a second "aligned" copy of every
    // frame: with a large kept-frame count that doubled peak memory right
    // at the stage where it's already highest (confirmed against a real
    // ~850-frame planetary AVI -- see tests/manual_pipeline_run.cpp).
    // sampleBilinear(x + dx, y + dy, c) on the original frame is exactly
    // what applyShift(frame, t).at(x, y, c) would have produced, so this
    // is not an approximation -- just not materializing the intermediate.
    //
    // dx/dy here are no longer one constant per frame: blendShiftAt blends
    // together the frame's per-point local shifts, weighted by each
    // pixel's precomputed nearest-point weights (see
    // MultiPointAlignment.h), so different regions of the disk can (and,
    // per real capture data, do) carry genuinely different local motion.
    ImageBuffer out(w, h, c);

    if (params.mode == StackMode::Mean) {
        std::vector<double> sums(c);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                std::fill(sums.begin(), sums.end(), 0.0);
                for (size_t i = 0; i < n; ++i) {
                    // Blended once per (frame, pixel), reused across all
                    // channels -- dx/dy don't depend on channel, so there's
                    // no reason to redo the K-term blend for each one.
                    Transform2D t = blendShiftAt(x, y, w, alignment.blendWeights, alignment.perFramePointShifts[i]);
                    for (int ch = 0; ch < c; ++ch)
                        sums[ch] += frames[i].sampleBilinear(x + t.dx, y + t.dy, ch);
                }
                for (int ch = 0; ch < c; ++ch) out.at(x, y, ch) = static_cast<float>(sums[ch] / n);
            }
            if (progressCallback && h > 0 && (y % std::max(1, h / 100) == 0))
                progressCallback(static_cast<int>(100.0 * (y + 1) / h));
        }
        if (progressCallback) progressCallback(100);
        return out;
    }

    // Sigma-clip: per-pixel iterative outlier rejection across the stack.
    std::vector<double> tdx(n), tdy(n);
    std::vector<double> vals(n);
    std::vector<double> active;
    active.reserve(n);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Blended once per (frame, pixel) and reused for every
            // channel below, same reasoning as the mean-mode branch.
            for (size_t i = 0; i < n; ++i) {
                Transform2D t = blendShiftAt(x, y, w, alignment.blendWeights, alignment.perFramePointShifts[i]);
                tdx[i] = t.dx;
                tdy[i] = t.dy;
            }
            for (int ch = 0; ch < c; ++ch) {
                for (size_t i = 0; i < n; ++i)
                    vals[i] = frames[i].sampleBilinear(x + tdx[i], y + tdy[i], ch);
                active.assign(vals.begin(), vals.end());

                for (int iter = 0; iter < params.sigmaIterations && active.size() >= 3; ++iter) {
                    double mean = 0.0;
                    for (double v : active) mean += v;
                    mean /= active.size();
                    double var = 0.0;
                    for (double v : active) var += (v - mean) * (v - mean);
                    var /= active.size();
                    double sd = std::sqrt(var);

                    std::vector<double> next;
                    next.reserve(active.size());
                    for (double v : active) {
                        if (v >= mean - params.sigmaLow * sd && v <= mean + params.sigmaHigh * sd) next.push_back(v);
                    }
                    if (next.empty() || next.size() == active.size()) break;
                    active = std::move(next);
                }

                double sum = 0.0;
                for (double v : active) sum += v;
                out.at(x, y, ch) = static_cast<float>(active.empty() ? vals[0] : sum / active.size());
            }
        }
        if (progressCallback && h > 0 && (y % std::max(1, h / 100) == 0))
            progressCallback(static_cast<int>(100.0 * (y + 1) / h));
    }
    if (progressCallback) progressCallback(100);
    return out;
}

} // namespace ls
