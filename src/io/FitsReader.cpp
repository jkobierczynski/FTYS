#include "io/FitsReader.h"

#include <algorithm>
#include <stdexcept>
#include <cstring>

namespace ls {

namespace {

void checkStatus(int status, const std::string& context) {
    if (status) {
        char errText[FLEN_ERRMSG] = {0};
        fits_get_errstatus(status, errText);
        throw std::runtime_error("FitsReader: " + context + ": " + errText);
    }
}

} // namespace

FitsReader::FitsReader(const std::string& path) : FrameSourceBase(path) {
    int status = 0;
    fits_open_file(&fptr_, path.c_str(), READONLY, &status);
    checkStatus(status, "opening '" + path + "'");

    fits_get_img_dim(fptr_, &naxis_, &status);
    checkStatus(status, "reading NAXIS");
    if (naxis_ < 2 || naxis_ > 3) {
        fits_close_file(fptr_, &status);
        throw std::runtime_error("FitsReader: only 2D images or 3D cubes are supported (NAXIS=" +
                                  std::to_string(naxis_) + ")");
    }

    fits_get_img_size(fptr_, 3, naxes_, &status);
    checkStatus(status, "reading NAXISn");

    fits_get_img_type(fptr_, &bitpix_, &status);
    checkStatus(status, "reading BITPIX");

    width_ = static_cast<int>(naxes_[0]);
    height_ = static_cast<int>(naxes_[1]);

    if (naxis_ == 3 && naxes_[2] == 3) {
        rgbCube_ = true;
        frameCount_ = 1;
    } else if (naxis_ == 3) {
        frameCount_ = static_cast<size_t>(naxes_[2]);
    } else {
        frameCount_ = 1;
    }

    sixteenBit_ = (bitpix_ != 8);
    format_ = rgbCube_ ? (sixteenBit_ ? PixelFormat::RGB48 : PixelFormat::RGB24)
                        : (sixteenBit_ ? PixelFormat::Mono16 : PixelFormat::Mono8);

    if (sixteenBit_) {
        std::vector<float> probe = readPlaneAxis3(0);
        auto [minIt, maxIt] = std::minmax_element(probe.begin(), probe.end());
        float mn = *minIt, mx = *maxIt;
        if (mx <= mn) mx = mn + 1.0f;
        normOffset_ = mn;
        normScale_ = 65535.0f / (mx - mn);
    }
}

FitsReader::~FitsReader() {
    if (fptr_) {
        int status = 0;
        fits_close_file(fptr_, &status);
    }
}

std::vector<float> FitsReader::readPlaneAxis3(int axis3Index0based) const {
    long fpixel[3] = {1, 1, 1};
    if (naxis_ == 3) fpixel[2] = axis3Index0based + 1;

    long nelem = static_cast<long>(width_) * height_;
    std::vector<float> out(static_cast<size_t>(nelem));
    int anynul = 0;
    int status = 0;
    fits_read_pix(fptr_, TFLOAT, fpixel, nelem, nullptr, out.data(), &anynul, &status);
    checkStatus(status, "reading pixel data");
    return out;
}

RawFrame FitsReader::readFrame(size_t index) {
    if (index >= frameCount_) throw std::runtime_error("FitsReader: frame index out of range");

    RawFrame out;
    out.width = width_;
    out.height = height_;
    out.format = format_;

    auto toByte = [](float v) -> uint8_t {
        v = std::clamp(v, 0.0f, 255.0f);
        return static_cast<uint8_t>(v + 0.5f);
    };
    auto toWord = [this](float v) -> uint16_t {
        float n = (v - normOffset_) * normScale_;
        n = std::clamp(n, 0.0f, 65535.0f);
        return static_cast<uint16_t>(n + 0.5f);
    };

    if (rgbCube_) {
        std::vector<float> r = readPlaneAxis3(0);
        std::vector<float> g = readPlaneAxis3(1);
        std::vector<float> b = readPlaneAxis3(2);
        size_t px = static_cast<size_t>(width_) * height_;
        if (sixteenBit_) {
            out.bytes.resize(px * 3 * 2);
            uint16_t* dst = reinterpret_cast<uint16_t*>(out.bytes.data());
            for (size_t i = 0; i < px; ++i) {
                dst[i * 3 + 0] = toWord(r[i]);
                dst[i * 3 + 1] = toWord(g[i]);
                dst[i * 3 + 2] = toWord(b[i]);
            }
        } else {
            out.bytes.resize(px * 3);
            uint8_t* dst = out.bytes.data();
            for (size_t i = 0; i < px; ++i) {
                dst[i * 3 + 0] = toByte(r[i]);
                dst[i * 3 + 1] = toByte(g[i]);
                dst[i * 3 + 2] = toByte(b[i]);
            }
        }
    } else {
        std::vector<float> plane = readPlaneAxis3(static_cast<int>(index));
        size_t px = plane.size();
        if (sixteenBit_) {
            out.bytes.resize(px * 2);
            uint16_t* dst = reinterpret_cast<uint16_t*>(out.bytes.data());
            for (size_t i = 0; i < px; ++i) dst[i] = toWord(plane[i]);
        } else {
            out.bytes.resize(px);
            uint8_t* dst = out.bytes.data();
            for (size_t i = 0; i < px; ++i) dst[i] = toByte(plane[i]);
        }
    }

    return out;
}

} // namespace ls
