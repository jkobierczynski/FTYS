#include "core/ImageBuffer.h"
#include "core/PixelFormat.h"
#include "core/RawFrame.h"

#include <cstdint>
#include <cstring>

namespace ls {

namespace {

// Bayer color at (x, y) for a given Bayer pattern, 0=R,1=G,2=B.
// pattern encodes the color of the top-left 2x2 block reading left-to-right,
// top-to-bottom, e.g. RGGB -> {R,G,G,B}.
int bayerColorAt(PixelFormat fmt, int x, int y) {
    // order[row][col] for a 2x2 tile, row/col in {0,1}
    static const int RGGB[2][2] = {{0, 1}, {1, 2}};
    static const int BGGR[2][2] = {{2, 1}, {1, 0}};
    static const int GRBG[2][2] = {{1, 0}, {2, 1}};
    static const int GBRG[2][2] = {{1, 2}, {0, 1}};

    const int (*table)[2] = nullptr;
    switch (fmt) {
        case PixelFormat::BayerRGGB8:
        case PixelFormat::BayerRGGB16: table = RGGB; break;
        case PixelFormat::BayerBGGR8:
        case PixelFormat::BayerBGGR16: table = BGGR; break;
        case PixelFormat::BayerGRBG8:
        case PixelFormat::BayerGRBG16: table = GRBG; break;
        case PixelFormat::BayerGBRG8:
        case PixelFormat::BayerGBRG16: table = GBRG; break;
        default: return -1;
    }
    return table[y & 1][x & 1];
}

// Simple, robust bilinear demosaic. Not as sharp as an edge-directed
// algorithm but numerically well-behaved and easy to reason about; the
// wavelet/deconvolution sharpening stages recover detail later anyway.
void debayerBilinear(const float* raw, int width, int height, PixelFormat fmt, ImageBuffer& out) {
    out.resize(width, height, 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float sums[3] = {0, 0, 0};
            int counts[3] = {0, 0, 0};
            for (int dy = -1; dy <= 1; ++dy) {
                int sy = y + dy;
                if (sy < 0 || sy >= height) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    int sx = x + dx;
                    if (sx < 0 || sx >= width) continue;
                    int c = bayerColorAt(fmt, sx, sy);
                    sums[c] += raw[static_cast<size_t>(sy) * width + sx];
                    counts[c]++;
                }
            }
            int here = bayerColorAt(fmt, x, y);
            // Keep the native sample exact for its own channel, average neighbors for the rest.
            for (int c = 0; c < 3; ++c) {
                if (c == here) {
                    out.at(x, y, c) = raw[static_cast<size_t>(y) * width + x];
                } else {
                    out.at(x, y, c) = counts[c] > 0 ? sums[c] / counts[c] : 0.0f;
                }
            }
        }
    }
}

} // namespace

ImageBuffer imageBufferFromRaw(const RawFrame& raw) {
    ImageBuffer out;
    const int w = raw.width;
    const int h = raw.height;
    const PixelFormat fmt = raw.format;

    if (isBayer(fmt)) {
        // Normalize raw samples to [0,1] float first, then demosaic.
        std::vector<float> norm(static_cast<size_t>(w) * h);
        if (is16Bit(fmt)) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(raw.bytes.data());
            for (size_t i = 0; i < norm.size(); ++i) norm[i] = src[i] / 65535.0f;
        } else {
            const uint8_t* src = raw.bytes.data();
            for (size_t i = 0; i < norm.size(); ++i) norm[i] = src[i] / 255.0f;
        }
        debayerBilinear(norm.data(), w, h, fmt, out);
        return out;
    }

    switch (fmt) {
        case PixelFormat::Mono8: {
            out.resize(w, h, 1);
            const uint8_t* src = raw.bytes.data();
            for (size_t i = 0; i < out.pixelCount(); ++i) out.data()[i] = src[i] / 255.0f;
            break;
        }
        case PixelFormat::Mono16: {
            out.resize(w, h, 1);
            const uint16_t* src = reinterpret_cast<const uint16_t*>(raw.bytes.data());
            for (size_t i = 0; i < out.pixelCount(); ++i) out.data()[i] = src[i] / 65535.0f;
            break;
        }
        case PixelFormat::RGB24: {
            out.resize(w, h, 3);
            const uint8_t* src = raw.bytes.data();
            for (size_t i = 0; i < out.sampleCount(); ++i) out.data()[i] = src[i] / 255.0f;
            break;
        }
        case PixelFormat::RGB48: {
            out.resize(w, h, 3);
            const uint16_t* src = reinterpret_cast<const uint16_t*>(raw.bytes.data());
            for (size_t i = 0; i < out.sampleCount(); ++i) out.data()[i] = src[i] / 65535.0f;
            break;
        }
        default:
            throw std::runtime_error("imageBufferFromRaw: unsupported pixel format");
    }
    return out;
}

} // namespace ls
