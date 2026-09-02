#include "io/ImageWriter.h"

#include <fitsio.h>
#include <tiffio.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ls {

namespace {

// Same clamp-and-rescale convention as ImageConvert's imageBufferToQImage:
// what's already in the buffer (whatever levels/curve/CA stage produced
// it) is what gets written, not a fresh normalization against this
// particular image's own min/max.
uint8_t quantize8(float v) {
    float c = std::clamp(v, 0.0f, 1.0f) * 255.0f;
    return static_cast<uint8_t>(c + 0.5f);
}
uint16_t quantize16(float v) {
    float c = std::clamp(v, 0.0f, 1.0f) * 65535.0f;
    return static_cast<uint16_t>(c + 0.5f);
}

bool writeTiff(const ImageBuffer& buf, const std::string& path, int bitsPerSample) {
    TIFF* tif = TIFFOpen(path.c_str(), "w");
    if (!tif) return false;

    int channels = buf.channels();
    auto w = static_cast<uint32_t>(buf.width());
    auto h = static_cast<uint32_t>(buf.height());

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, w);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, h);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, channels);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bitsPerSample);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, channels == 1 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB);
    // One scanline per strip: simplest correct choice for a lucky-imaging
    // output (typically a few hundred to low thousands of pixels wide,
    // written once per export), not a hot path worth tuning strip size for.
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 1u);

    bool ok = true;
    std::vector<uint8_t> row(static_cast<size_t>(w) * static_cast<size_t>(channels) *
                              static_cast<size_t>(bitsPerSample / 8));
    for (uint32_t y = 0; y < h && ok; ++y) {
        if (bitsPerSample == 16) {
            uint16_t* dst = reinterpret_cast<uint16_t*>(row.data());
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < channels; ++c)
                    dst[x * static_cast<uint32_t>(channels) + static_cast<uint32_t>(c)] =
                        quantize16(buf.at(static_cast<int>(x), static_cast<int>(y), c));
        } else {
            uint8_t* dst = row.data();
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < channels; ++c)
                    dst[x * static_cast<uint32_t>(channels) + static_cast<uint32_t>(c)] =
                        quantize8(buf.at(static_cast<int>(x), static_cast<int>(y), c));
        }
        ok = TIFFWriteScanline(tif, row.data(), y) >= 0;
    }

    TIFFClose(tif);
    return ok;
}

bool writeFits(const ImageBuffer& buf, const std::string& path, int bitsPerSample) {
    // cfitsio refuses to silently overwrite an existing file unless the
    // path is prefixed with '!' (its own "clobber" convention). The
    // export dialog already confirmed the overwrite via the OS file
    // picker before this runs, so this just makes cfitsio agree to what
    // the user already approved.
    std::string clobberPath = "!" + path;
    fitsfile* fptr = nullptr;
    int status = 0;
    fits_create_file(&fptr, clobberPath.c_str(), &status);
    if (status || !fptr) return false;

    int channels = buf.channels();
    int naxis = (channels == 3) ? 3 : 2;
    long naxes[3] = {buf.width(), buf.height(), channels};
    int imgType = (bitsPerSample == 16) ? USHORT_IMG : BYTE_IMG;
    fits_create_img(fptr, imgType, naxis, naxes, &status);

    long nelem = static_cast<long>(buf.width()) * buf.height();
    for (int c = 0; c < channels && status == 0; ++c) {
        // ImageBuffer::channel() extracts plane c in the same row-major
        // (y * width + x) order FitsReader::readPlaneAxis3() expects to
        // read back -- reusing it here, rather than indexing the
        // interleaved buffer directly, keeps that ordering guaranteed to
        // match on both sides instead of two copies of the same logic
        // risking disagreement.
        ImageBuffer plane = buf.channel(c);
        long fpixel[3] = {1, 1, c + 1};
        if (bitsPerSample == 16) {
            std::vector<uint16_t> data(static_cast<size_t>(nelem));
            for (long i = 0; i < nelem; ++i) data[static_cast<size_t>(i)] = quantize16(plane.data()[i]);
            fits_write_pix(fptr, TUSHORT, fpixel, nelem, data.data(), &status);
        } else {
            std::vector<uint8_t> data(static_cast<size_t>(nelem));
            for (long i = 0; i < nelem; ++i) data[static_cast<size_t>(i)] = quantize8(plane.data()[i]);
            fits_write_pix(fptr, TBYTE, fpixel, nelem, data.data(), &status);
        }
    }

    int closeStatus = 0;
    fits_close_file(fptr, &closeStatus);
    return status == 0 && closeStatus == 0;
}

} // namespace

bool writeImage(const ImageBuffer& buf, const std::string& path, ExportFormat format, ExportBitDepth bitDepth) {
    if (buf.empty()) return false;
    if (buf.channels() != 1 && buf.channels() != 3) return false;

    int bits = (bitDepth == ExportBitDepth::Sixteen) ? 16 : 8;
    switch (format) {
        case ExportFormat::TIFF:
            return writeTiff(buf, path, bits);
        case ExportFormat::FITS:
            return writeFits(buf, path, bits);
        case ExportFormat::PNG:
        default:
            return false; // see header comment -- PNG is handled by the caller, not here
    }
}

} // namespace ls
