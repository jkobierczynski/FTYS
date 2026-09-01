#pragma once

#include "core/FrameSource.h"
#include <fstream>

namespace ls {

// Reader for the SER format used by most planetary/lunar/solar capture
// software (FireCapture, SharpCap, GenikaCapture, ...). Format reference:
// a 178-byte fixed header followed by FrameCount raw frames, optionally
// followed by a per-frame timestamp trailer (not required for stacking, so
// not parsed here).
class SerReader : public FrameSourceBase {
public:
    explicit SerReader(const std::string& path);

    RawFrame readFrame(size_t index) override;

private:
    void parseHeader();

    std::ifstream file_;
    size_t headerSize_ = 178;
    size_t bytesPerSample_ = 1; // 1 or 2
    int planes_ = 1;            // 1 = mono/bayer, 3 = rgb/bgr
    bool swapRGBtoBGR_ = false; // ColorID 101 (BGR) needs channel swap to RGB24/48 convention
    bool littleEndianData_ = true;
    size_t frameStride_ = 0;    // bytes per frame in the file
};

} // namespace ls
