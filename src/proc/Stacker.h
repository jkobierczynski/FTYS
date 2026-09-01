#pragma once

#include "core/ImageBuffer.h"
#include "proc/Alignment.h"
#include "proc/MultiPointAlignment.h"
#include <functional>
#include <vector>

namespace ls {

enum class StackMode { Mean, SigmaClip };

struct StackParams {
    StackMode mode = StackMode::Mean;
    double sigmaLow = 3.0;
    double sigmaHigh = 3.0;
    int sigmaIterations = 2;
};

// Aligns each frame using a per-pixel shift blended from `alignment`'s
// tracking points (see MultiPointAlignment.h) rather than one single shift
// for the whole frame, and accumulates the results into a single output at
// the frames' native resolution. Mean stacking is a plain running average;
// sigma-clip rejects per-pixel outliers across the stack (cosmic-ray hits,
// satellite trails, transient noise spikes) before averaging what remains.
//
// `progressCallback`, when set, is invoked with a 0-100 percentage as rows
// of the output are completed -- this is the row loop, not the frame loop,
// since every row touches every frame regardless of stack mode. Optional
// so callers that don't care (tests, the diagnostic executables) don't have
// to pass anything.
ImageBuffer stackFrames(const std::vector<ImageBuffer>& frames,
                         const MultiPointAlignmentResult& alignment,
                         const StackParams& params,
                         const std::function<void(int)>& progressCallback = nullptr);

} // namespace ls
