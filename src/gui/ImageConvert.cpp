#include "gui/ImageConvert.h"

#include <algorithm>
#include <cstdint>

namespace ls {

namespace {
uint8_t to8(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}
} // namespace

QImage imageBufferToQImage(const ImageBuffer& buf) {
    int w = buf.width(), h = buf.height();
    if (buf.channels() == 1) {
        QImage img(w, h, QImage::Format_Grayscale8);
        for (int y = 0; y < h; ++y) {
            uint8_t* row = img.scanLine(y);
            for (int x = 0; x < w; ++x) row[x] = to8(buf.at(x, y, 0));
        }
        return img;
    }

    QImage img(w, h, QImage::Format_RGB888);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = to8(buf.at(x, y, 0));
            row[x * 3 + 1] = to8(buf.at(x, y, 1));
            row[x * 3 + 2] = to8(buf.at(x, y, 2));
        }
    }
    return img;
}

} // namespace ls
