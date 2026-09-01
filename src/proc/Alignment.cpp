#include "proc/Alignment.h"

#include <fftw3.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <limits>

namespace ls {

DetailedShift estimateShiftDetailed(const ImageBuffer& reference, const ImageBuffer& target) {
    if (reference.width() != target.width() || reference.height() != target.height())
        throw std::runtime_error("estimateShiftDetailed: reference and target size mismatch");

    const int w = reference.width();
    const int h = reference.height();
    const size_t n = static_cast<size_t>(w) * h;
    const size_t cplxN = static_cast<size_t>(h) * (w / 2 + 1);

    std::vector<double> refIn(n), tgtIn(n);
    for (size_t i = 0; i < n; ++i) {
        refIn[i] = reference.data()[i];
        tgtIn[i] = target.data()[i];
    }

    fftw_complex* refF = fftw_alloc_complex(cplxN);
    fftw_complex* tgtF = fftw_alloc_complex(cplxN);
    fftw_complex* crossF = fftw_alloc_complex(cplxN);
    std::vector<double> corr(n);

    fftw_plan pRef = fftw_plan_dft_r2c_2d(h, w, refIn.data(), refF, FFTW_ESTIMATE);
    fftw_plan pTgt = fftw_plan_dft_r2c_2d(h, w, tgtIn.data(), tgtF, FFTW_ESTIMATE);
    fftw_execute(pRef);
    fftw_execute(pTgt);

    for (size_t i = 0; i < cplxN; ++i) {
        double aRe = refF[i][0], aIm = refF[i][1];
        double bRe = tgtF[i][0], bIm = -tgtF[i][1]; // conj(target)
        double re = aRe * bRe - aIm * bIm;
        double im = aRe * bIm + aIm * bRe;
        double mag = std::sqrt(re * re + im * im);
        if (mag < 1e-12) mag = 1e-12;
        crossF[i][0] = re / mag;
        crossF[i][1] = im / mag;
    }

    fftw_plan pInv = fftw_plan_dft_c2r_2d(h, w, crossF, corr.data(), FFTW_ESTIMATE);
    fftw_execute(pInv);

    size_t peakIdx = 0;
    double peakVal = corr[0];
    for (size_t i = 1; i < n; ++i) {
        if (corr[i] > peakVal) { peakVal = corr[i]; peakIdx = i; }
    }
    int py = static_cast<int>(peakIdx / w);
    int px = static_cast<int>(peakIdx % w);

    auto at = [&](int x, int y) -> double {
        x = ((x % w) + w) % w;
        y = ((y % h) + h) % h;
        return corr[static_cast<size_t>(y) * w + x];
    };
    double c = at(px, py);
    double xm = at(px - 1, py), xp = at(px + 1, py);
    double ym = at(px, py - 1), yp = at(px, py + 1);
    double denomX = xm - 2 * c + xp;
    double denomY = ym - 2 * c + yp;
    double subX = std::fabs(denomX) > 1e-9 ? 0.5 * (xm - xp) / denomX : 0.0;
    double subY = std::fabs(denomY) > 1e-9 ? 0.5 * (ym - yp) / denomY : 0.0;

    double peakX = px + subX;
    double peakY = py + subY;
    if (peakX > w / 2.0) peakX -= w;
    if (peakY > h / 2.0) peakY -= h;

    // Per-axis "did this axis actually find a dominant difference": the
    // ratio of the primary peak to the tallest *other* local peak along
    // the row/column running through it, excluding a small neighborhood
    // around the primary peak itself (so a rival peak has to be a genuine
    // separate candidate, not just the same peak's own shoulder).
    //
    // The local-curvature check tried first here didn't work -- checked
    // directly against real out2_2.avi data: phase-only correlation (the
    // whitened/normalized spectrum used above) produces a sharp, near-
    // impulse-like peak almost regardless of whether the match is genuine,
    // because every frequency bin contributes equal magnitude once
    // normalized. Measured curvature-based "sharpness" values of 1-2.5 for
    // points whose residual shift was later confirmed wrong just as often
    // as for points that were right -- it doesn't discriminate on this
    // data. A peak-to-second-peak ratio does: a real, unambiguous match
    // has one peak that clearly dominates its whole row/column, while an
    // aliased or noise-driven one has multiple comparable candidates (this
    // is the standard peak-to-sidelobe idea used to judge correlation
    // tracker reliability).
    constexpr int kExcludeRadius = 3;
    auto secondPeak1D = [&](int size, int peakPos, auto valueAt) -> double {
        double second = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < size; ++i) {
            int d = std::abs(i - peakPos);
            int dw = std::min(d, size - d); // circular distance (corr wraps)
            if (dw <= kExcludeRadius) continue;
            second = std::max(second, valueAt(i));
        }
        return second;
    };
    double rowSecond = secondPeak1D(w, px, [&](int x) { return at(x, py); });
    double colSecond = secondPeak1D(h, py, [&](int y) { return at(px, y); });
    double eps = std::max(1e-9, 1e-6 * std::fabs(c));
    double sharpnessX = c / (std::max(rowSecond, 0.0) + eps);
    double sharpnessY = c / (std::max(colSecond, 0.0) + eps);

    fftw_destroy_plan(pRef);
    fftw_destroy_plan(pTgt);
    fftw_destroy_plan(pInv);
    fftw_free(refF);
    fftw_free(tgtF);
    fftw_free(crossF);

    // corr[] here is the un-normalized inverse FFT (FFTW convention), so
    // divide by n to get a peak height comparable across frame sizes.
    //
    // The raw phase-correlation peak (peakX, peakY) comes out as the
    // negative of the shift we want: empirically (see tests/test_proc.cpp)
    // it points from target back to reference, whereas applyShift's
    // contract is out(x,y) = src(x + dx, y + dy) aligning target onto
    // reference. Negate here so callers get the latter.
    DetailedShift result;
    result.transform.dx = -peakX;
    result.transform.dy = -peakY;
    result.transform.confidence = std::clamp(peakVal / static_cast<double>(n), 0.0, 1.0);
    result.sharpnessX = sharpnessX;
    result.sharpnessY = sharpnessY;
    return result;
}

Transform2D estimateShift(const ImageBuffer& reference, const ImageBuffer& target) {
    return estimateShiftDetailed(reference, target).transform;
}

ImageBuffer applyShift(const ImageBuffer& src, const Transform2D& t) {
    ImageBuffer out(src.width(), src.height(), src.channels());
    for (int y = 0; y < src.height(); ++y) {
        for (int x = 0; x < src.width(); ++x) {
            double sx = x + t.dx;
            double sy = y + t.dy;
            for (int c = 0; c < src.channels(); ++c)
                out.at(x, y, c) = src.sampleBilinear(sx, sy, c);
        }
    }
    return out;
}

} // namespace ls
