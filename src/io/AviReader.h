#pragma once

#include "core/FrameSource.h"
#include <vector>
#include <cstdint>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
}

namespace ls {

// Reader for AVI captures via FFmpeg (libavformat/libavcodec/libswscale).
// Planetary/lunar capture AVIs are, in practice, almost always either
// uncompressed DIB frames (mono Y800/Y8 or RGB/BGR24) or simple intra-only
// codecs (MJPEG) from older webcam-style capture software; FFmpeg's rawvideo
// path and demuxer handle the legacy BITMAPINFOHEADER quirks (bottom-up
// rows, YUY2, etc.) that historically tripped up naive AVI parsers, so we
// lean on it rather than hand-rolling a RIFF parser. AVI output is always
// 8-bit (Mono8 or RGB24) since 16-bit AVI is not a real convention in this
// domain -- 16-bit capture goes through SER or FITS instead.
//
// Random-access reads are implemented via pts-based seeking recorded during
// an initial scan pass at open time, since AVI's own index is not always
// frame-accurate and every frame in this domain is effectively a keyframe.
// Sequential reads (index == last read + 1, the common case for both the
// quality pass and stacking) skip the seek entirely and just keep decoding
// forward -- seeking before every frame measured at ~50ms/frame on a real
// capture, most of which was pure seek/flush overhead.
class AviReader : public FrameSourceBase {
public:
    explicit AviReader(const std::string& path);
    ~AviReader() override;

    RawFrame readFrame(size_t index) override;

private:
    void scanFrames();

    AVFormatContext* fmtCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    int videoStreamIndex_ = -1;
    bool mono_ = true;
    std::vector<int64_t> ptsList_;
    int64_t lastDecodedIndex_ = -1; // -1 means "not positioned anywhere yet"
};

} // namespace ls
