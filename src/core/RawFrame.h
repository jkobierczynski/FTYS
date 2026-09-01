#pragma once

#include "core/PixelFormat.h"
#include <vector>
#include <cstdint>
#include <cstddef>
#include <chrono>

namespace ls {

// A single frame exactly as decoded from a source file, before conversion to
// the float ImageBuffer working format. Kept as raw bytes so readers can
// stay allocation-light when scanning through thousands of frames just to
// compute quality metrics.
struct RawFrame {
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::Unknown;
    std::vector<uint8_t> bytes;
    std::chrono::system_clock::time_point timestamp{}; // optional, from SER/FITS metadata if present

    size_t expectedBytes() const {
        size_t px = static_cast<size_t>(width) * height;
        bool sixteen = is16Bit(format);
        int ch = (format == PixelFormat::RGB24 || format == PixelFormat::RGB48) ? 3 : 1;
        return px * ch * (sixteen ? 2 : 1);
    }
};

// Forward declaration; implemented in ImageBuffer.cpp
class ImageBuffer;
ImageBuffer imageBufferFromRaw(const RawFrame& raw);

} // namespace ls
