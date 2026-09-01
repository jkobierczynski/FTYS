#pragma once

#include "core/RawFrame.h"
#include "core/PixelFormat.h"
#include <memory>
#include <string>
#include <cstddef>

namespace ls {

// Abstract interface every format reader (SER/AVI/FITS) implements. The rest
// of the pipeline (quality scoring, alignment, stacking) only ever talks to
// this interface, so adding a new capture format later doesn't touch
// anything downstream.
class FrameSource {
public:
    virtual ~FrameSource() = default;

    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual PixelFormat format() const = 0;
    virtual size_t frameCount() const = 0;

    // Decode frame at `index` (0-based). Throws std::runtime_error on
    // corrupt/short data rather than returning a half-filled frame, so
    // callers can decide whether to skip or abort.
    virtual RawFrame readFrame(size_t index) = 0;

    virtual const std::string& sourcePath() const = 0;
};

// Common bookkeeping shared by all concrete readers.
class FrameSourceBase : public FrameSource {
public:
    explicit FrameSourceBase(std::string path) : path_(std::move(path)) {}

    int width() const override { return width_; }
    int height() const override { return height_; }
    PixelFormat format() const override { return format_; }
    size_t frameCount() const override { return frameCount_; }
    const std::string& sourcePath() const override { return path_; }

protected:
    std::string path_;
    int width_ = 0;
    int height_ = 0;
    PixelFormat format_ = PixelFormat::Unknown;
    size_t frameCount_ = 0;
};

// Opens the right reader based on file extension (.ser / .avi / .fits,.fit,.fts).
// Throws std::runtime_error if the extension is unrecognized or the file
// can't be opened/parsed.
std::unique_ptr<FrameSource> openFrameSource(const std::string& path);

} // namespace ls
