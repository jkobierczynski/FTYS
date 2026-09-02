#include "io/SerReader.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace ls {

namespace {

int32_t readI32LE(std::istream& s) {
    unsigned char b[4];
    s.read(reinterpret_cast<char*>(b), 4);
    return static_cast<int32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
}

int64_t readI64LE(std::istream& s) {
    unsigned char b[8];
    s.read(reinterpret_cast<char*>(b), 8);
    int64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}

} // namespace

SerReader::SerReader(const std::string& path) : FrameSourceBase(path) {
    file_.open(path, std::ios::binary);
    if (!file_) throw std::runtime_error("SerReader: cannot open '" + path + "'");
    parseHeader();
}

void SerReader::parseHeader() {
    char fileId[15] = {0};
    file_.read(fileId, 14);
    if (!file_ || std::strncmp(fileId, "LUCAM-RECORDER", 14) != 0) {
        // Some tools write "LUCAM-RECORDER" others just require a 14-byte
        // signature to be present; be lenient about exact vendor string but
        // require *something* readable.
        if (!file_) throw std::runtime_error("SerReader: truncated header in '" + path_ + "'");
    }

    [[maybe_unused]] int32_t luId = readI32LE(file_);
    int32_t colorId = readI32LE(file_);
    int32_t littleEndianFlag = readI32LE(file_);
    int32_t imageWidth = readI32LE(file_);
    int32_t imageHeight = readI32LE(file_);
    int32_t pixelDepth = readI32LE(file_);
    int32_t frameCount = readI32LE(file_);

    char observer[40], instrument[40], telescope[40];
    file_.read(observer, 40);
    file_.read(instrument, 40);
    file_.read(telescope, 40);
    [[maybe_unused]] int64_t dateTime = readI64LE(file_);
    [[maybe_unused]] int64_t dateTimeUtc = readI64LE(file_);

    if (!file_) throw std::runtime_error("SerReader: truncated header in '" + path_ + "'");
    if (imageWidth <= 0 || imageHeight <= 0 || frameCount <= 0)
        throw std::runtime_error("SerReader: invalid dimensions/frame count in '" + path_ + "'");
    if (pixelDepth != 8 && pixelDepth != 16)
        throw std::runtime_error("SerReader: unsupported pixel depth " + std::to_string(pixelDepth));

    width_ = imageWidth;
    height_ = imageHeight;
    frameCount_ = static_cast<size_t>(frameCount);
    bytesPerSample_ = (pixelDepth == 16) ? 2 : 1;

    // NOTE: in practice most SER writers (FireCapture, SharpCap, etc.) emit
    // little-endian 16-bit samples regardless of what this flag says, and
    // several well-known tools historically wrote it backwards. We honor it
    // only if it explicitly claims big-endian AND depth is 16; 8-bit data
    // has no endianness to speak of.
    littleEndianData_ = !(bytesPerSample_ == 2 && littleEndianFlag == 0);

    switch (colorId) {
        case 0: planes_ = 1; format_ = (bytesPerSample_ == 2) ? PixelFormat::Mono16 : PixelFormat::Mono8; break;
        case 8: planes_ = 1; format_ = (bytesPerSample_ == 2) ? PixelFormat::BayerRGGB16 : PixelFormat::BayerRGGB8; break;
        case 9: planes_ = 1; format_ = (bytesPerSample_ == 2) ? PixelFormat::BayerGRBG16 : PixelFormat::BayerGRBG8; break;
        case 10: planes_ = 1; format_ = (bytesPerSample_ == 2) ? PixelFormat::BayerGBRG16 : PixelFormat::BayerGBRG8; break;
        case 11: planes_ = 1; format_ = (bytesPerSample_ == 2) ? PixelFormat::BayerBGGR16 : PixelFormat::BayerBGGR8; break;
        case 100: planes_ = 3; format_ = (bytesPerSample_ == 2) ? PixelFormat::RGB48 : PixelFormat::RGB24; break;
        case 101: planes_ = 3; format_ = (bytesPerSample_ == 2) ? PixelFormat::RGB48 : PixelFormat::RGB24; swapRGBtoBGR_ = true; break;
        default:
            throw std::runtime_error("SerReader: unsupported ColorID " + std::to_string(colorId) +
                                      " (CMYG-family Bayer patterns are not implemented)");
    }

    headerSize_ = 178;
    frameStride_ = static_cast<size_t>(width_) * height_ * planes_ * bytesPerSample_;
}

RawFrame SerReader::readFrame(size_t index) {
    if (index >= frameCount_) throw std::runtime_error("SerReader: frame index out of range");

    RawFrame out;
    out.width = width_;
    out.height = height_;
    out.format = format_;
    out.bytes.resize(frameStride_);

    std::streamoff offset = static_cast<std::streamoff>(headerSize_) +
                             static_cast<std::streamoff>(index) * static_cast<std::streamoff>(frameStride_);
    file_.clear();
    file_.seekg(offset);
    file_.read(reinterpret_cast<char*>(out.bytes.data()), static_cast<std::streamsize>(frameStride_));
    if (static_cast<size_t>(file_.gcount()) != frameStride_)
        throw std::runtime_error("SerReader: short read for frame " + std::to_string(index));

    // Byte-swap 16-bit big-endian samples to native little-endian so the
    // rest of the pipeline can always assume host byte order.
    if (bytesPerSample_ == 2 && !littleEndianData_) {
        uint16_t* samples = reinterpret_cast<uint16_t*>(out.bytes.data());
        size_t n = out.bytes.size() / 2;
        for (size_t i = 0; i < n; ++i) {
            uint16_t v = samples[i];
            samples[i] = static_cast<uint16_t>((v >> 8) | (v << 8));
        }
    }

    // BGR -> RGB channel swap, done in-place per pixel.
    if (swapRGBtoBGR_) {
        if (bytesPerSample_ == 1) {
            uint8_t* p = out.bytes.data();
            size_t px = static_cast<size_t>(width_) * height_;
            for (size_t i = 0; i < px; ++i) std::swap(p[i * 3 + 0], p[i * 3 + 2]);
        } else {
            uint16_t* p = reinterpret_cast<uint16_t*>(out.bytes.data());
            size_t px = static_cast<size_t>(width_) * height_;
            for (size_t i = 0; i < px; ++i) std::swap(p[i * 3 + 0], p[i * 3 + 2]);
        }
    }

    return out;
}

} // namespace ls
