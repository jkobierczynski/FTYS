#pragma once

namespace ls {

// Native pixel format as stored in the source file, before conversion to the
// internal float working buffer used by the rest of the pipeline.
enum class PixelFormat {
    Mono8,
    Mono16,
    RGB24,        // interleaved 8-bit R,G,B
    RGB48,        // interleaved 16-bit R,G,B
    BayerRGGB8,
    BayerBGGR8,
    BayerGRBG8,
    BayerGBRG8,
    BayerRGGB16,
    BayerBGGR16,
    BayerGRBG16,
    BayerGBRG16,
    Unknown
};

inline bool isBayer(PixelFormat f) {
    switch (f) {
        case PixelFormat::BayerRGGB8:
        case PixelFormat::BayerBGGR8:
        case PixelFormat::BayerGRBG8:
        case PixelFormat::BayerGBRG8:
        case PixelFormat::BayerRGGB16:
        case PixelFormat::BayerBGGR16:
        case PixelFormat::BayerGRBG16:
        case PixelFormat::BayerGBRG16:
            return true;
        default:
            return false;
    }
}

inline bool is16Bit(PixelFormat f) {
    switch (f) {
        case PixelFormat::Mono16:
        case PixelFormat::RGB48:
        case PixelFormat::BayerRGGB16:
        case PixelFormat::BayerBGGR16:
        case PixelFormat::BayerGRBG16:
        case PixelFormat::BayerGBRG16:
            return true;
        default:
            return false;
    }
}

// Number of channels once debayered / converted to a working buffer (1 = mono, 3 = RGB).
inline int workingChannels(PixelFormat f) {
    switch (f) {
        case PixelFormat::Mono8:
        case PixelFormat::Mono16:
            return 1;
        default:
            return 3; // Bayer and RGB formats all become 3-channel after debayer/expand
    }
}

} // namespace ls
