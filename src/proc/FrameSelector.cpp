#include "proc/FrameSelector.h"

#include <algorithm>
#include <cmath>

namespace ls {

std::vector<size_t> selectTopPercent(std::vector<FrameQuality> scores, double percent) {
    if (scores.empty()) return {};

    std::sort(scores.begin(), scores.end(), [](const FrameQuality& a, const FrameQuality& b) {
        return a.score > b.score;
    });

    percent = std::clamp(percent, 0.0, 100.0);
    size_t keep = static_cast<size_t>(std::ceil(scores.size() * percent / 100.0));
    keep = std::clamp<size_t>(keep, 1, scores.size());

    std::vector<size_t> indices;
    indices.reserve(keep);
    for (size_t i = 0; i < keep; ++i) indices.push_back(scores[i].index);

    std::sort(indices.begin(), indices.end());
    return indices;
}

} // namespace ls
