#pragma once

#include "core/ImageBuffer.h"
#include <QImage>

namespace ls {

// Converts the internal float [0,1] working buffer to an 8-bit QImage for
// display/export. Mono buffers become Format_Grayscale8, 3-channel become
// Format_RGB888. Values are clamped, not rescaled, so what you see reflects
// whatever levels/curve stage has already been applied upstream.
QImage imageBufferToQImage(const ImageBuffer& buf);

} // namespace ls
