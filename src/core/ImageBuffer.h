#pragma once

#include <vector>
#include <cstddef>
#include <stdexcept>
#include <algorithm>

namespace ls {

// Internal working image representation used by every processing stage
// (alignment, stacking, sharpening, color). Always floating point so we
// never lose precision across repeated operations; values are nominally in
// [0, 1] but intermediate stages (e.g. drizzle accumulation) may briefly
// exceed that range before normalization.
//
// Storage is interleaved: data[(y * width + x) * channels + c]
class ImageBuffer {
public:
    ImageBuffer() = default;

    ImageBuffer(int width, int height, int channels)
        : width_(width), height_(height), channels_(channels),
          data_(static_cast<size_t>(width) * height * channels, 0.0f) {}

    void resize(int width, int height, int channels) {
        width_ = width;
        height_ = height;
        channels_ = channels;
        data_.assign(static_cast<size_t>(width) * height * channels, 0.0f);
    }

    bool empty() const { return data_.empty(); }
    int width() const { return width_; }
    int height() const { return height_; }
    int channels() const { return channels_; }
    size_t pixelCount() const { return static_cast<size_t>(width_) * height_; }
    size_t sampleCount() const { return data_.size(); }

    float* data() { return data_.data(); }
    const float* data() const { return data_.data(); }

    float& at(int x, int y, int c = 0) {
        return data_[index(x, y, c)];
    }
    float at(int x, int y, int c = 0) const {
        return data_[index(x, y, c)];
    }

    // Bilinear sample at fractional coordinates; returns 0 outside bounds.
    float sampleBilinear(double x, double y, int c) const {
        if (x < 0.0 || y < 0.0 || x > width_ - 1 || y > height_ - 1) return 0.0f;
        int x0 = static_cast<int>(x);
        int y0 = static_cast<int>(y);
        int x1 = std::min(x0 + 1, width_ - 1);
        int y1 = std::min(y0 + 1, height_ - 1);
        double fx = x - x0;
        double fy = y - y0;
        float v00 = at(x0, y0, c);
        float v10 = at(x1, y0, c);
        float v01 = at(x0, y1, c);
        float v11 = at(x1, y1, c);
        double top = v00 * (1.0 - fx) + v10 * fx;
        double bot = v01 * (1.0 - fx) + v11 * fx;
        return static_cast<float>(top * (1.0 - fy) + bot * fy);
    }

    void fill(float value) { std::fill(data_.begin(), data_.end(), value); }

    // Extracts the sub-rectangle [x, x+w) x [y, y+h) as a new ImageBuffer,
    // clamped to this buffer's bounds. Used to crop a mostly-black full
    // frame down to just the imaged object (planet/moon/sun disk) once its
    // bounding box is known, so the delivered image isn't dominated by
    // empty background.
    ImageBuffer crop(int x, int y, int w, int h) const {
        x = std::clamp(x, 0, width_ - 1);
        y = std::clamp(y, 0, height_ - 1);
        w = std::clamp(w, 1, width_ - x);
        h = std::clamp(h, 1, height_ - y);
        ImageBuffer out(w, h, channels_);
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx)
                for (int c = 0; c < channels_; ++c) out.at(xx, yy, c) = at(x + xx, y + yy, c);
        return out;
    }

    // Extract a single channel as its own mono ImageBuffer (used by
    // per-channel alignment / quality metrics on color data).
    ImageBuffer channel(int c) const {
        ImageBuffer out(width_, height_, 1);
        for (int y = 0; y < height_; ++y)
            for (int x = 0; x < width_; ++x)
                out.at(x, y, 0) = at(x, y, c);
        return out;
    }

    // Luminance (or the mono channel itself if already mono).
    ImageBuffer luminance() const {
        if (channels_ == 1) return *this;
        ImageBuffer out(width_, height_, 1);
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                float r = at(x, y, 0), g = at(x, y, 1), b = at(x, y, 2);
                out.at(x, y, 0) = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            }
        }
        return out;
    }

private:
    size_t index(int x, int y, int c) const {
        return (static_cast<size_t>(y) * width_ + x) * channels_ + c;
    }

    int width_ = 0;
    int height_ = 0;
    int channels_ = 0;
    std::vector<float> data_;
};

} // namespace ls
