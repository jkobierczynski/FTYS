// Smoke/correctness tests for the proc library, all against small synthetic
// buffers so they run in milliseconds and don't depend on real capture
// files. These check the properties that matter most for a stacking
// pipeline: alignment recovers a known shift with the right sign, stacking
// actually reduces noise, wavelet reconstruction is lossless at unit gain,
// and deconvolution/levels/curves behave the way their contracts promise.

#include "core/ImageBuffer.h"
#include "proc/QualityMetric.h"
#include "proc/Alignment.h"
#include "proc/Stacker.h"
#include "proc/Drizzle.h"
#include "proc/WaveletSharpen.h"
#include "proc/RichardsonLucy.h"
#include "proc/ColorStretch.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>

namespace {

int g_failures = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        g_failures++;
    } else {
        std::cerr << "ok:   " << msg << "\n";
    }
}

constexpr int W = 48, H = 40;

ls::ImageBuffer renderGaussianBlob(double cx, double cy, double sigma, double noiseAmp = 0.0, unsigned seed = 0) {
    ls::ImageBuffer buf(W, H, 1);
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, noiseAmp);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            double dx = x - cx, dy = y - cy;
            double v = std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
            if (noiseAmp > 0.0) v += noise(rng);
            buf.at(x, y, 0) = static_cast<float>(std::clamp(v, 0.0, 1.0));
        }
    }
    return buf;
}

// Stacker.h/Drizzle.h now take a MultiPointAlignmentResult (per-point,
// per-frame shifts blended together at stack time) rather than one plain
// Transform2D per frame -- see MultiPointAlignment.h. These stacking/
// drizzle tests aren't about point placement, so this builds the
// degenerate single-point case, which reduces to exactly the old
// "one global shift per frame" behavior (computeBlendWeights gives that
// one point weight 1.0 everywhere).
ls::MultiPointAlignmentResult makeSingleGlobalAlignment(const std::vector<ls::Transform2D>& transforms, int width,
                                                         int height) {
    ls::MultiPointAlignmentResult a;
    a.points = {{width / 2.0, height / 2.0}};
    a.width = width;
    a.height = height;
    a.perFramePointShifts.reserve(transforms.size());
    for (const auto& t : transforms) a.perFramePointShifts.push_back({t});
    a.blendWeights = ls::computeBlendWeights(a.points, width, height);
    return a;
}

void testQualityMetric() {
    ls::ImageBuffer sharp = renderGaussianBlob(W / 2.0, H / 2.0, 2.0);
    ls::ImageBuffer blurry = renderGaussianBlob(W / 2.0, H / 2.0, 6.0);
    double sSharp = ls::assessFrameQuality(sharp);
    double sBlurry = ls::assessFrameQuality(blurry);
    expect(sSharp > sBlurry, "quality: sharper blob scores higher than blurred blob (" +
                                 std::to_string(sSharp) + " vs " + std::to_string(sBlurry) + ")");
}

void testAlignment() {
    double cx = 20.0, cy = 18.0, sigma = 2.5;
    double sx = 3.4, sy = -2.1;
    ls::ImageBuffer reference = renderGaussianBlob(cx, cy, sigma);
    // target(x,y) = reference(x - sx, y - sy) means target's blob center is
    // reference's center shifted by (sx, sy).
    ls::ImageBuffer target = renderGaussianBlob(cx + sx, cy + sy, sigma);

    ls::Transform2D t = ls::estimateShift(reference, target);
    std::cerr << "  estimated shift dx=" << t.dx << " dy=" << t.dy << " (expected " << sx << ", " << sy << ")\n";
    expect(std::fabs(t.dx - sx) < 0.5, "alignment: dx recovered within 0.5px");
    expect(std::fabs(t.dy - sy) < 0.5, "alignment: dy recovered within 0.5px");

    ls::ImageBuffer realigned = ls::applyShift(target, t);
    double diff = 0.0;
    for (size_t i = 0; i < realigned.sampleCount(); ++i) {
        double d = realigned.data()[i] - reference.data()[i];
        diff += d * d;
    }
    double refEnergy = 0.0;
    for (size_t i = 0; i < reference.sampleCount(); ++i) refEnergy += reference.data()[i] * reference.data()[i];
    expect(diff < 0.1 * refEnergy, "alignment: applyShift(target, t) closely matches reference");
}

void testStackingReducesNoise() {
    double cx = 24.0, cy = 20.0, sigma = 3.0;
    std::vector<ls::ImageBuffer> frames;
    std::vector<ls::Transform2D> transforms;
    const int N = 12;
    for (int i = 0; i < N; ++i) {
        // Small known per-frame shift + noise; transform undoes the shift
        // exactly (same convention verified in testAlignment).
        double sx = 0.3 * i, sy = -0.2 * i;
        frames.push_back(renderGaussianBlob(cx + sx, cy + sy, sigma, /*noiseAmp=*/0.08, /*seed=*/i + 1));
        ls::Transform2D t;
        t.dx = sx;
        t.dy = sy;
        transforms.push_back(t);
    }

    ls::MultiPointAlignmentResult alignment = makeSingleGlobalAlignment(transforms, W, H);

    ls::StackParams meanParams;
    meanParams.mode = ls::StackMode::Mean;
    ls::ImageBuffer stacked = ls::stackFrames(frames, alignment, meanParams);

    ls::ImageBuffer clean = renderGaussianBlob(cx, cy, sigma);
    double stackedErr = 0.0, singleErr = 0.0;
    for (size_t i = 0; i < clean.sampleCount(); ++i) {
        stackedErr += std::pow(stacked.data()[i] - clean.data()[i], 2);
        singleErr += std::pow(frames[0].data()[i] - clean.data()[i], 2);
    }
    std::cerr << "  stacked RMS err=" << std::sqrt(stackedErr / clean.sampleCount())
              << " single-frame RMS err=" << std::sqrt(singleErr / clean.sampleCount()) << "\n";
    expect(stackedErr < singleErr, "stacking: mean stack is closer to ground truth than any single noisy frame");

    ls::StackParams sigmaParams;
    sigmaParams.mode = ls::StackMode::SigmaClip;
    ls::ImageBuffer stackedSigma = ls::stackFrames(frames, alignment, sigmaParams);
    expect(stackedSigma.width() == W && stackedSigma.height() == H, "stacking: sigma-clip output has correct dimensions");
}

void testDrizzle() {
    double cx = 24.0, cy = 20.0, sigma = 3.0;
    std::vector<ls::ImageBuffer> frames;
    std::vector<ls::Transform2D> transforms;
    const int N = 8;
    for (int i = 0; i < N; ++i) {
        double sx = 0.25 * i, sy = 0.15 * i; // sub-pixel dither
        frames.push_back(renderGaussianBlob(cx + sx, cy + sy, sigma));
        ls::Transform2D t;
        t.dx = sx;
        t.dy = sy;
        transforms.push_back(t);
    }
    ls::MultiPointAlignmentResult alignment = makeSingleGlobalAlignment(transforms, W, H);
    ls::DrizzleParams dp;
    dp.scale = 2.0;
    dp.dropFraction = 0.7;
    ls::ImageBuffer out = ls::drizzleStack(frames, alignment, dp);

    expect(out.width() == static_cast<int>(std::round(W * dp.scale)), "drizzle: output width scaled correctly");
    expect(out.height() == static_cast<int>(std::round(H * dp.scale)), "drizzle: output height scaled correctly");

    bool anyNaN = false;
    for (size_t i = 0; i < out.sampleCount(); ++i)
        if (!std::isfinite(out.data()[i])) anyNaN = true;
    expect(!anyNaN, "drizzle: output contains no NaN/Inf");

    float center = out.at(static_cast<int>(cx * dp.scale), static_cast<int>(cy * dp.scale), 0);
    float corner = out.at(2, 2, 0);
    expect(center > corner, "drizzle: blob center brighter than background corner in output");
}

void testWaveletIdentityAtUnitGain() {
    ls::ImageBuffer src = renderGaussianBlob(W / 2.0, H / 2.0, 3.0);
    ls::WaveletParams p;
    p.layerGains = {1.0, 1.0, 1.0, 1.0};
    ls::ImageBuffer out = ls::waveletSharpen(src, p);

    double maxDiff = 0.0;
    for (size_t i = 0; i < src.sampleCount(); ++i)
        maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(out.data()[i] - src.data()[i])));
    expect(maxDiff < 1e-3, "wavelet: unit gains reconstruct the input almost exactly (maxDiff=" +
                               std::to_string(maxDiff) + ")");

    ls::WaveletParams boosted;
    boosted.layerGains = {2.5, 2.0, 1.0, 1.0};
    ls::ImageBuffer sharpened = ls::waveletSharpen(src, boosted);
    double changed = 0.0;
    for (size_t i = 0; i < src.sampleCount(); ++i)
        changed = std::max(changed, static_cast<double>(std::fabs(sharpened.data()[i] - src.data()[i])));
    expect(changed > 1e-3, "wavelet: boosted gains actually change the image");
}

void testRichardsonLucySharpens() {
    ls::ImageBuffer blurred = renderGaussianBlob(W / 2.0, H / 2.0, 3.0);
    ls::RLParams rp;
    rp.iterations = 20;
    rp.psfSigma = 1.8;
    ls::ImageBuffer deconvolved = ls::richardsonLucyDeconvolve(blurred, rp);

    double before = ls::assessFrameQuality(blurred);
    double after = ls::assessFrameQuality(deconvolved);
    std::cerr << "  RL quality before=" << before << " after=" << after << "\n";
    expect(after > before, "richardson-lucy: deconvolved image scores sharper than the blurred input");
}

void testColorStretch() {
    ls::ImageBuffer mono(4, 4, 1);
    mono.fill(0.5f);
    ls::LevelsParams lp;
    lp.blackPoint = 0.2f;
    lp.whitePoint = 0.8f;
    lp.gamma = 1.0f;
    ls::ImageBuffer leveled = ls::applyLevels(mono, lp);
    expect(std::fabs(leveled.data()[0] - 0.5f) < 1e-5f, "levels: (0.5-0.2)/(0.8-0.2) == 0.5 exactly");

    std::vector<std::pair<float, float>> identity = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    ls::ImageBuffer curved = ls::applyCurve(mono, identity);
    expect(std::fabs(curved.data()[0] - 0.5f) < 1e-5f, "curves: identity control points are a no-op");

    ls::ImageBuffer rgb(2, 2, 3);
    for (int i = 0; i < 4; ++i) {
        rgb.data()[i * 3 + 0] = 0.9f;
        rgb.data()[i * 3 + 1] = 0.1f;
        rgb.data()[i * 3 + 2] = 0.1f;
    }
    ls::SaturationParams sp;
    sp.saturation = 0.0f;
    ls::ImageBuffer gray = ls::applySaturation(rgb, sp);
    expect(std::fabs(gray.at(0, 0, 0) - gray.at(0, 0, 1)) < 1e-4f &&
               std::fabs(gray.at(0, 0, 1) - gray.at(0, 0, 2)) < 1e-4f,
           "saturation=0 collapses R/G/B to equal (grayscale) values");

    ls::ImageBuffer constImg(8, 8, 1);
    constImg.fill(0.75f);
    auto hist = ls::computeHistogram(constImg, -1, 256);
    int expectedBin = static_cast<int>(0.75f * 255 + 0.5f);
    expect(hist[expectedBin] == 64, "histogram: constant image bins entirely into the expected bin");
}

} // namespace

int main() {
    testQualityMetric();
    testAlignment();
    testStackingReducesNoise();
    testDrizzle();
    testWaveletIdentityAtUnitGain();
    testRichardsonLucySharpens();
    testColorStretch();

    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cerr << "all proc tests passed\n";
    return 0;
}
