#include "gui/PipelineController.h"
#include "gui/ImageConvert.h"
#include "core/RawFrame.h"

#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <stdexcept>

namespace ls {

namespace {
// Default/validated patch size for multi-point tracking. History: 56px was
// too small (~50% unreliable outliers); 96px combined with
// selectAlignmentPoints' background-rejection left only 4 of 12 points able
// to place a full patch without touching black background; 64px fixed
// that. All three of those were tuned on a different, higher-contrast test
// capture, though. Once per-frame recentering was added (see
// estimateLocalShift's header comment -- each point's target patch is now
// cropped relative to that frame's own detected object center, so it only
// has to find the *residual* local shift, not the whole disk's bulk
// translation), a fresh sweep against the real out2_2.avi capture that
// motivated recentering showed *smaller* patches now track tighter and
// more consistently across all 12 points (patch=32 clearly beat 48/64,
// which is also what the user reported independently: "a little better
// when I lower the size of the boxes"). 32px is now the default; this is
// still just the GUI's default (see PipelineController::alignPatchSize_),
// since how much local contrast a capture has still varies.
constexpr int kDefaultAlignPatchSize = 32;
constexpr int kAlignPatchSizeMin = 24;
constexpr int kAlignPatchSizeMax = 256;
// Default/range for how many automatically-placed tracking points
// selectAlignmentPoints is asked for. 12 was the fixed value validated
// against out2_2.avi throughout this project's real-data testing; now
// user-configurable since a higher-contrast or larger-disk capture can
// support more independent points, and a very soft/small one may need
// fewer to avoid crowding.
constexpr int kDefaultAlignMaxPoints = 12;
constexpr int kAlignMaxPointsMin = 4;
constexpr int kAlignMaxPointsMax = 40;
// Default/range for robustifyPointShifts' maxDeviationPx -- the per-frame
// "sigma clip" style rejection of a point whose shift deviates too far from
// that frame's consensus. 12px was the value used throughout this
// project's real-data testing; user-configurable so a capture with
// genuinely larger local seeing distortion isn't clipped as if it were an
// outlier, or a very steady one can be clipped more aggressively.
constexpr double kDefaultAlignMaxDeviationPx = 12.0;
constexpr double kAlignMaxDeviationMin = 2.0;
constexpr double kAlignMaxDeviationMax = 50.0;
// 0.75x patch size, same ratio validated (via a spacing/point-count sweep
// on real data) to comfortably fit 12+ candidates once background-
// touching ones are excluded. Kept as a ratio (rather than a fixed pixel
// value) so it still holds when the patch size is overridden.
constexpr double kAlignSpacingRatio = 0.75;
// Per-axis gate on estimateLocalShift's residual (post-recenter)
// correlation: below this peak-to-sidelobe ratio, that axis's correlation
// isn't treated as having found a real, dominant difference, and the gross
// recenter is trusted alone. A first attempt at this gate used peak
// curvature instead of peak-to-sidelobe ratio and turned out, checked
// directly against real data, not to discriminate at all (see
// DetailedShift's header comment in Alignment.h) -- this value is for the
// *replacement* metric, picked by sweeping real out2_2.avi frames and
// choosing where nearly every point's residual collapsed into agreement
// with the independently-measured recenter (see the README's multi-point
// alignment section for the actual sweep numbers).
constexpr double kAlignAxisSharpnessThreshold = 2.0;

QString describeFormat(PixelFormat f) {
    switch (f) {
        case PixelFormat::Mono8: return "Mono 8-bit";
        case PixelFormat::Mono16: return "Mono 16-bit";
        case PixelFormat::RGB24: return "RGB 24-bit";
        case PixelFormat::RGB48: return "RGB 48-bit";
        default: return isBayer(f) ? "Bayer (debayered to RGB)" : "Unknown";
    }
}
} // namespace

PipelineController::PipelineController(QObject* parent) : QObject(parent) {}

ImageBuffer PipelineController::decodeFrame(size_t index) const {
    RawFrame raw = source_->readFrame(index);
    return imageBufferFromRaw(raw);
}

void PipelineController::openSequence(const QString& path) {
    try {
        source_ = openFrameSource(path.toStdString());
        selectedFrameCache_.clear();
        qualityScores_.clear();
        selectedIndices_.clear();
        alignment_ = MultiPointAlignmentResult{};
        chromaticAberrationResult_ = ImageBuffer{};
        emit sequenceOpened(source_->width(), source_->height(), static_cast<int>(source_->frameCount()),
                             describeFormat(source_->format()));
    } catch (const std::exception& e) {
        emit errorOccurred(QString::fromStdString(e.what()));
    }
}

void PipelineController::computeQuality() {
    if (!source_) {
        emit errorOccurred("No sequence loaded");
        return;
    }

    (void)QtConcurrent::run([this]() {
        try {
            size_t n = source_->frameCount();
            qualityScores_.clear();
            qualityScores_.reserve(n);

            // Deliberately does NOT retain decoded frames: each is scored
            // and discarded immediately, so peak memory here is O(1 frame)
            // regardless of sequence length. Only the (index, score) pairs
            // -- a few bytes each -- accumulate. The kept-subset cache is
            // built later, in alignSelected(), once we know which frames
            // are actually needed.
            for (size_t i = 0; i < n; ++i) {
                ImageBuffer buf = decodeFrame(i);
                double score = assessFrameQuality(buf.channels() == 1 ? buf : buf.luminance());
                qualityScores_.push_back({i, score});

                if (n > 0 && (i % std::max<size_t>(1, n / 100) == 0)) {
                    emit qualityProgress(static_cast<int>(100.0 * (i + 1) / n));
                }
            }
            emit qualityProgress(100);
            emit qualityDone(static_cast<int>(n));
        } catch (const std::exception& e) {
            emit errorOccurred(QString::fromStdString(e.what()));
        }
    });
}

double PipelineController::estimateSelectedMegabytes() const {
    if (!source_ || selectedIndices_.empty()) return 0.0;
    int channels = workingChannels(source_->format());
    double bytesPerFrame = static_cast<double>(source_->width()) * source_->height() * channels * sizeof(float);
    return bytesPerFrame * selectedIndices_.size() / (1024.0 * 1024.0);
}

void PipelineController::selectPercent(double percent) {
    if (qualityScores_.empty()) {
        emit errorOccurred("Run quality assessment first");
        return;
    }
    selectedIndices_ = selectTopPercent(qualityScores_, percent);
    selectedFrameCache_.clear(); // stale: belonged to whatever selection existed before
    alignment_ = MultiPointAlignmentResult{}; // stale, same reason
    emit selectionChanged(static_cast<int>(selectedIndices_.size()), static_cast<int>(qualityScores_.size()),
                           estimateSelectedMegabytes());
}

void PipelineController::setAlignPatchSize(int patchSize) {
    alignPatchSize_ = std::clamp(patchSize, kAlignPatchSizeMin, kAlignPatchSizeMax);
}

void PipelineController::setAlignMaxPoints(int maxPoints) {
    alignMaxPoints_ = std::clamp(maxPoints, kAlignMaxPointsMin, kAlignMaxPointsMax);
}

void PipelineController::setAlignMaxDeviation(double maxDeviationPx) {
    alignMaxDeviationPx_ = std::clamp(maxDeviationPx, kAlignMaxDeviationMin, kAlignMaxDeviationMax);
}

void PipelineController::alignSelected() {
    if (selectedIndices_.empty()) {
        emit errorOccurred("No frames selected");
        return;
    }

    const int patchSize = alignPatchSize_;
    const int minSpacing = std::max(1, static_cast<int>(std::round(kAlignSpacingRatio * patchSize)));
    const int maxPoints = alignMaxPoints_;
    const double maxDeviationPx = alignMaxDeviationPx_;

    (void)QtConcurrent::run([this, patchSize, minSpacing, maxPoints, maxDeviationPx]() {
        try {
            // Decode exactly the selected subset now, once, and hold onto
            // it for the stacking stage that follows -- this is the only
            // point where more than one frame's pixel data is resident at
            // once, and it's bounded by the kept percentage rather than
            // the whole sequence.
            selectedFrameCache_.clear();
            selectedFrameCache_.reserve(selectedIndices_.size());
            for (size_t idx : selectedIndices_) selectedFrameCache_.push_back(decodeFrame(idx));

            // Reference = best-scoring frame among the selected ones.
            size_t bestPos = 0;
            double bestScore = -1.0;
            for (size_t pos = 0; pos < selectedIndices_.size(); ++pos) {
                size_t idx = selectedIndices_[pos];
                double score = 0.0;
                for (const auto& q : qualityScores_)
                    if (q.index == idx) { score = q.score; break; }
                if (score > bestScore) { bestScore = score; bestPos = pos; }
            }
            referenceIndex_ = selectedIndices_[bestPos];

            ImageBuffer refLum = selectedFrameCache_[bestPos].luminance();

            // Multiple alignment points, placed automatically on whatever
            // real contrast the reference frame's disk has (belt edges,
            // limb, festoons), rather than one single whole-frame shift.
            // A whole-frame correlation on real planetary AVI turned out
            // to be unreliable -- its confidence sits close to the noise
            // floor once a lot of near-black background and soft, low-
            // contrast disk content dilute the signal, and it was
            // measured locking onto shifts tens of pixels away from what
            // every local feature actually agreed on. Small patches
            // centered on real contrast correlate far more reliably.
            Roi roi = detectObjectRoi(refLum, 0.15f, 20);
            alignment_ = MultiPointAlignmentResult{};
            alignment_.points = selectAlignmentPoints(refLum, roi, maxPoints, patchSize, minSpacing);
            alignment_.width = refLum.width();
            alignment_.height = refLum.height();
            alignment_.perFramePointShifts.assign(selectedIndices_.size(), {});

            // The reference frame's own gross object-center position.
            // Every other frame's center gets compared against this to
            // find that frame's bulk translation *before* any per-point
            // patch correlation runs -- see estimateLocalShift's header
            // comment (MultiPointAlignment.h) for why this ordering
            // matters.
            Point2D refCenter = detectObjectCenter(refLum, 0.15f);

            double confSum = 0.0;
            size_t confCount = 0;
            for (size_t pos = 0; pos < selectedIndices_.size(); ++pos) {
                std::vector<Transform2D> pointShifts;
                pointShifts.reserve(alignment_.points.size());
                if (pos == bestPos) {
                    // The reference against itself: identity everywhere,
                    // full confidence.
                    pointShifts.assign(alignment_.points.size(), Transform2D{0.0, 0.0, 1.0});
                } else {
                    ImageBuffer tgtLum = selectedFrameCache_[pos].luminance();
                    Point2D tgtCenter = detectObjectCenter(tgtLum, 0.15f);
                    Point2D recenterOffset{tgtCenter.x - refCenter.x, tgtCenter.y - refCenter.y};
                    for (const auto& pt : alignment_.points)
                        pointShifts.push_back(estimateLocalShift(refLum, tgtLum, pt, patchSize, recenterOffset,
                                                                  kAlignAxisSharpnessThreshold));
                    robustifyPointShifts(pointShifts, maxDeviationPx);
                }
                for (const auto& t : pointShifts) { confSum += t.confidence; ++confCount; }
                alignment_.perFramePointShifts[pos] = std::move(pointShifts);

                if (!selectedIndices_.empty() && (pos % std::max<size_t>(1, selectedIndices_.size() / 100) == 0))
                    emit alignProgress(static_cast<int>(100.0 * (pos + 1) / selectedIndices_.size()));
            }
            emit alignProgress(100);
            alignment_.blendWeights = computeBlendWeights(alignment_.points, alignment_.width, alignment_.height);
            alignment_.patchSize = patchSize;
            alignment_.averageConfidence = confCount > 0 ? confSum / confCount : 0.0;
            emit alignDone(alignment_.averageConfidence);
        } catch (const std::exception& e) {
            emit errorOccurred(QString::fromStdString(e.what()));
        }
    });
}

void PipelineController::cropStackedResultToObject() {
    if (stackedResult_.empty()) return;
    ImageBuffer lum = stackedResult_.channels() == 1 ? stackedResult_ : stackedResult_.luminance();
    // Margin scales with the buffer's own size, so this works the same
    // whether stackedResult_ is at native resolution or drizzle-upsampled.
    int marginPx = std::max(20, static_cast<int>(0.12 * std::min(stackedResult_.width(), stackedResult_.height())));
    Roi roi = detectObjectRoi(lum, 0.15f, marginPx);
    stackedResult_ = stackedResult_.crop(roi.x, roi.y, roi.w, roi.h);
}

void PipelineController::stackMean(const StackParams& params) {
    if (alignment_.perFramePointShifts.empty()) {
        emit errorOccurred("Align frames first");
        return;
    }
    (void)QtConcurrent::run([this, params]() {
        try {
            stackedResult_ = stackFrames(selectedFrameCache_, alignment_, params,
                                          [this](int percent) { emit stackProgress(percent); });
            // Frames come in at full sensor resolution, mostly empty
            // background around a comparatively small disk; crop down to
            // the object (with margin) so what's previewed/exported isn't
            // dominated by black padding, matching how dedicated planetary
            // stacking tools frame their output.
            cropStackedResultToObject();
            emit stackDone(imageBufferToQImage(stackedResult_));
        } catch (const std::exception& e) {
            emit errorOccurred(QString::fromStdString(e.what()));
        }
    });
}

void PipelineController::stackDrizzle(const DrizzleParams& params) {
    if (alignment_.perFramePointShifts.empty()) {
        emit errorOccurred("Align frames first");
        return;
    }
    (void)QtConcurrent::run([this, params]() {
        try {
            stackedResult_ = drizzleStack(selectedFrameCache_, alignment_, params,
                                           [this](int percent) { emit stackProgress(percent); });
            cropStackedResultToObject();
            emit stackDone(imageBufferToQImage(stackedResult_));
        } catch (const std::exception& e) {
            emit errorOccurred(QString::fromStdString(e.what()));
        }
    });
}

void PipelineController::applySharpen(SharpenMode mode, const WaveletParams& wp, const RLParams& rp) {
    if (stackedResult_.empty()) {
        emit errorOccurred("Stack frames first");
        return;
    }
    (void)QtConcurrent::run([this, mode, wp, rp]() {
        try {
            switch (mode) {
                case SharpenMode::Wavelet: sharpenedResult_ = waveletSharpen(stackedResult_, wp); break;
                case SharpenMode::RichardsonLucy: sharpenedResult_ = richardsonLucyDeconvolve(stackedResult_, rp); break;
                case SharpenMode::None: sharpenedResult_ = stackedResult_; break;
            }
            emit sharpenDone(imageBufferToQImage(sharpenedResult_));
        } catch (const std::exception& e) {
            emit errorOccurred(QString::fromStdString(e.what()));
        }
    });
}

void PipelineController::applyColor(const LevelsParams& lp, const std::vector<std::pair<float, float>>& curve,
                                     const SaturationParams& sp) {
    if (sharpenedResult_.empty()) {
        emit errorOccurred("Apply sharpening first (choose \"None\" to pass the stack through unchanged)");
        return;
    }
    (void)QtConcurrent::run([this, lp, curve, sp]() {
        try {
            ImageBuffer leveled = applyLevels(sharpenedResult_, lp);
            ImageBuffer curved = curve.size() >= 2 ? applyCurve(leveled, curve) : leveled;
            finalResult_ = applySaturation(curved, sp);
            emit colorDone(imageBufferToQImage(finalResult_));
        } catch (const std::exception& e) {
            emit errorOccurred(QString::fromStdString(e.what()));
        }
    });
}

void PipelineController::applyChromaticAberration(const ChromaticAberrationParams& params) {
    if (finalResult_.empty()) {
        emit errorOccurred("Apply color adjustments first (defaults are fine if none are needed)");
        return;
    }
    (void)QtConcurrent::run([this, params]() {
        try {
            chromaticAberrationResult_ = correctChromaticAberration(finalResult_, params);
            emit chromaticAberrationDone(imageBufferToQImage(chromaticAberrationResult_));
        } catch (const std::exception& e) {
            emit errorOccurred(QString::fromStdString(e.what()));
        }
    });
}

ChromaticAberrationParams PipelineController::detectCA() const {
    // Prefer the most-processed result available -- more resolved detail
    // gives the cross-channel correlation more to lock onto -- but don't
    // require the user to have clicked through every stage just to get a
    // starting estimate.
    if (!finalResult_.empty()) return detectChromaticAberration(finalResult_);
    if (!sharpenedResult_.empty()) return detectChromaticAberration(sharpenedResult_);
    if (!stackedResult_.empty()) return detectChromaticAberration(stackedResult_);
    return ChromaticAberrationParams{};
}

bool PipelineController::exportImage(const QString& path) const {
    const ImageBuffer& out = !chromaticAberrationResult_.empty() ? chromaticAberrationResult_ : finalResult_;
    if (out.empty()) return false;
    return imageBufferToQImage(out).save(path);
}

QImage PipelineController::previewFrame(int index) const {
    if (!source_) return QImage();
    try {
        return imageBufferToQImage(decodeFrame(static_cast<size_t>(index)));
    } catch (const std::exception&) {
        return QImage();
    }
}

QImage PipelineController::rawSelectedFramePreview(int pos) const {
    if (pos < 0 || pos >= static_cast<int>(selectedFrameCache_.size())) return QImage();
    return imageBufferToQImage(selectedFrameCache_[pos]);
}

QImage PipelineController::alignedFramePreview(int pos) const {
    if (pos < 0 || pos >= static_cast<int>(selectedFrameCache_.size())) return QImage();
    const ImageBuffer& src = selectedFrameCache_[pos];
    if (alignment_.blendWeights.empty() || pos >= static_cast<int>(alignment_.perFramePointShifts.size()))
        return imageBufferToQImage(src); // not aligned yet -- fall back to raw rather than fail

    const std::vector<Transform2D>& pointShifts = alignment_.perFramePointShifts[pos];
    ImageBuffer warped(src.width(), src.height(), src.channels());
    for (int y = 0; y < src.height(); ++y) {
        for (int x = 0; x < src.width(); ++x) {
            Transform2D t = blendShiftAt(x, y, src.width(), alignment_.blendWeights, pointShifts);
            for (int c = 0; c < src.channels(); ++c)
                warped.at(x, y, c) = src.sampleBilinear(x + t.dx, y + t.dy, c);
        }
    }
    return imageBufferToQImage(warped);
}

std::vector<int> PipelineController::preColorHistogram(int channel) const {
    if (!sharpenedResult_.empty()) return computeHistogram(sharpenedResult_, channel);
    if (!stackedResult_.empty()) return computeHistogram(stackedResult_, channel);
    return {};
}

} // namespace ls
