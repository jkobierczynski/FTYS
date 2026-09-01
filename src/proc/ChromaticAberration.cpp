#include "proc/ChromaticAberration.h"
#include "proc/Alignment.h"

namespace ls {

ImageBuffer correctChromaticAberration(const ImageBuffer& src, const ChromaticAberrationParams& params) {
    if (src.channels() != 3) return src; // mono: no channels to align against each other
    if (params.redDx == 0.0 && params.redDy == 0.0 && params.blueDx == 0.0 && params.blueDy == 0.0) return src;

    ImageBuffer out(src.width(), src.height(), src.channels());
    for (int y = 0; y < src.height(); ++y) {
        for (int x = 0; x < src.width(); ++x) {
            out.at(x, y, 0) = src.sampleBilinear(x + params.redDx, y + params.redDy, 0);
            out.at(x, y, 1) = src.at(x, y, 1); // green: fixed reference, untouched
            out.at(x, y, 2) = src.sampleBilinear(x + params.blueDx, y + params.blueDy, 2);
        }
    }
    return out;
}

ChromaticAberrationParams detectChromaticAberration(const ImageBuffer& rgb) {
    ChromaticAberrationParams result;
    if (rgb.channels() != 3) return result;

    ImageBuffer green = rgb.channel(1);
    Transform2D redShift = estimateShift(green, rgb.channel(0));
    Transform2D blueShift = estimateShift(green, rgb.channel(2));
    result.redDx = redShift.dx;
    result.redDy = redShift.dy;
    result.blueDx = blueShift.dx;
    result.blueDy = blueShift.dy;
    return result;
}

} // namespace ls
