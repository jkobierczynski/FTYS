#pragma once

#include "core/ImageBuffer.h"
#include <string>

namespace ls {

// Every format the Export panel offers. PNG is included here so GUI code
// only has to juggle one enum for "what the user picked", even though PNG
// itself isn't written by writeImage() below -- see its comment.
enum class ExportFormat { PNG, TIFF, FITS };

// PNG is always written 8-bit regardless of this (Qt's own PNG writer,
// via imageBufferToQImage(), doesn't offer 16-bit output through the
// QImage formats this app otherwise uses) -- TIFF and FITS honor it.
enum class ExportBitDepth { Eight, Sixteen };

// Writes `buf` (the internal float [0,1] working buffer, 1 or 3 channels)
// to `path` as TIFF or FITS at the requested bit depth, quantizing by
// clamping to [0,1] and rescaling to the target integer range -- the same
// "what you see is what gets written" convention as ImageConvert's
// imageBufferToQImage, not a renormalization against the data's own min/max.
//
// FITS output mirrors FitsReader's own read convention (see FitsReader.h)
// so a round trip through FTYS reads back correctly: NAXIS=2 for a mono
// image, or NAXIS=3 with NAXIS3=3 for RGB stored as three separate planes
// (not interleaved). 16-bit FITS is written via cfitsio's USHORT_IMG/
// TUSHORT, which sets BZERO=32768/BSCALE=1 automatically -- the standard
// "unsigned 16-bit" FITS convention -- rather than the native signed
// BITPIX=16 interpretation.
//
// PNG is deliberately NOT handled here -- passing ExportFormat::PNG
// returns false. It stays on the existing QImage-based path in
// gui/ImageConvert.h/PipelineController::exportImage(), since Qt's PNG
// writer already does that correctly; this module's job is only to add
// the two formats QImage doesn't give full bit-depth control over.
//
// Returns false (never throws) on any failure -- unwritable path, an
// unsupported channel count, a library error -- same convention as the
// QImage::save()-based path it sits alongside.
bool writeImage(const ImageBuffer& buf, const std::string& path, ExportFormat format, ExportBitDepth bitDepth);

} // namespace ls
