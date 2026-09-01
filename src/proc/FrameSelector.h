#pragma once

#include <cstddef>
#include <vector>

namespace ls {

struct FrameQuality {
    size_t index;
    double score;
};

// Returns the indices of the best `percent` fraction of frames by score
// (e.g. percent=25 keeps the sharpest quarter). Always keeps at least one
// frame. Output indices are returned in ascending order so downstream
// stages (alignment, stacking) can process them in natural sequence order
// regardless of how they scored.
std::vector<size_t> selectTopPercent(std::vector<FrameQuality> scores, double percent);

} // namespace ls
