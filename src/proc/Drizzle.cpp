#include "proc/Drizzle.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ls {

ImageBuffer drizzleStack(const std::vector<ImageBuffer>& frames,
                          const MultiPointAlignmentResult& alignment,
                          const DrizzleParams& params,
                          const std::function<void(int)>& progressCallback) {
    if (frames.empty()) return ImageBuffer();
    if (frames.size() != alignment.perFramePointShifts.size())
        throw std::runtime_error("drizzleStack: frames/alignment size mismatch");
    if (params.scale <= 0.0 || params.dropFraction <= 0.0)
        throw std::runtime_error("drizzleStack: scale and dropFraction must be positive");

    const int w = frames[0].width();
    const int h = frames[0].height();
    const int c = frames[0].channels();
    const int outW = std::max(1, static_cast<int>(std::round(w * params.scale)));
    const int outH = std::max(1, static_cast<int>(std::round(h * params.scale)));
    const double half = 0.5 * params.dropFraction * params.scale;

    std::vector<double> accum(static_cast<size_t>(outW) * outH * c, 0.0);
    std::vector<double> weight(static_cast<size_t>(outW) * outH, 0.0);

    for (size_t fi = 0; fi < frames.size(); ++fi) {
        const ImageBuffer& src = frames[fi];
        if (src.width() != w || src.height() != h || src.channels() != c)
            throw std::runtime_error("drizzleStack: all frames must share dimensions/channels");
        const std::vector<Transform2D>& pointShifts = alignment.perFramePointShifts[fi];

        for (int sy = 0; sy < h; ++sy) {
            for (int sx = 0; sx < w; ++sx) {
                // sx,sy are already native integer pixel coordinates, the
                // same resolution blendWeights was built at, so this is a
                // direct lookup -- no resampling of the weight table
                // itself is needed here.
                Transform2D t = blendShiftAt(sx, sy, w, alignment.blendWeights, pointShifts);
                // Same coordinate convention as applyShift: reference-frame
                // position of this source pixel is (sx - dx, sy - dy).
                double cx = (sx - t.dx) * params.scale;
                double cy = (sy - t.dy) * params.scale;

                int oMinX = std::max(0, static_cast<int>(std::floor(cx - half - 0.5)));
                int oMaxX = std::min(outW - 1, static_cast<int>(std::ceil(cx + half + 0.5)));
                int oMinY = std::max(0, static_cast<int>(std::floor(cy - half - 0.5)));
                int oMaxY = std::min(outH - 1, static_cast<int>(std::ceil(cy + half + 0.5)));

                for (int oy = oMinY; oy <= oMaxY; ++oy) {
                    double overlapY = std::min(cy + half, oy + 0.5) - std::max(cy - half, oy - 0.5);
                    if (overlapY <= 0.0) continue;
                    for (int ox = oMinX; ox <= oMaxX; ++ox) {
                        double overlapX = std::min(cx + half, ox + 0.5) - std::max(cx - half, ox - 0.5);
                        if (overlapX <= 0.0) continue;
                        double area = overlapX * overlapY;
                        size_t outIdx = static_cast<size_t>(oy) * outW + ox;
                        weight[outIdx] += area;
                        for (int ch = 0; ch < c; ++ch)
                            accum[outIdx * c + ch] += area * src.at(sx, sy, ch);
                    }
                }
            }
        }
        if (progressCallback && !frames.empty() && (fi % std::max<size_t>(1, frames.size() / 100) == 0))
            progressCallback(static_cast<int>(100.0 * (fi + 1) / frames.size()));
    }
    if (progressCallback) progressCallback(100);

    ImageBuffer out(outW, outH, c);
    for (size_t p = 0; p < static_cast<size_t>(outW) * outH; ++p) {
        double wgt = weight[p];
        for (int ch = 0; ch < c; ++ch)
            out.data()[p * c + ch] = wgt > 1e-9 ? static_cast<float>(accum[p * c + ch] / wgt) : 0.0f;
    }
    return out;
}

} // namespace ls
