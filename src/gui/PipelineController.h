#pragma once

#include "core/FrameSource.h"
#include "core/ImageBuffer.h"
#include "proc/QualityMetric.h"
#include "proc/FrameSelector.h"
#include "proc/Alignment.h"
#include "proc/MultiPointAlignment.h"
#include "proc/Stacker.h"
#include "proc/Drizzle.h"
#include "proc/WaveletSharpen.h"
#include "proc/RichardsonLucy.h"
#include "proc/ColorStretch.h"
#include "proc/ChromaticAberration.h"

#include <QObject>
#include <QImage>
#include <QString>
#include <memory>
#include <vector>
#include <utility>

namespace ls {

enum class SharpenMode { None, Wavelet, RichardsonLucy };

// Owns the whole processing pipeline's state (loaded sequence, decoded
// frame cache, quality scores, selection, alignment transforms, and each
// stage's output image) and runs the heavy steps on QtConcurrent worker
// threads so the GUI stays responsive. Every stage's completion is reported
// through a signal carrying a ready-to-display QImage; MainWindow doesn't
// touch ImageBuffer/proc types directly.
class PipelineController : public QObject {
    Q_OBJECT
public:
    explicit PipelineController(QObject* parent = nullptr);

    // Shared bound on how many tracking points/boxes may exist at once,
    // whether placed automatically (selectAlignmentPoints, via
    // alignSelected()) or one at a time by hand (the Alignment Point
    // Inspector's manual add/move/delete mode, via realignWithPoints()).
    // One constant so the "Number of boxes" spin box, setAlignMaxPoints()'s
    // clamp, and the manual editor's add-another-box limit can't disagree
    // with each other.
    static constexpr int kAlignMaxPointsMin = 4;
    static constexpr int kAlignMaxPointsMax = 50;

    void openSequence(const QString& path);
    void computeQuality();
    void selectPercent(double percent);
    void alignSelected();
    // Re-runs the per-frame tracking pass (recenter + per-point
    // correlation + sigma-clip robustify) against an explicit point list
    // instead of having selectAlignmentPoints() place one automatically --
    // used after the Alignment Point Inspector's manual box editing
    // (add/move/delete) so the edited boxes get real per-frame shifts
    // rather than sitting at their canonical position untracked. Reuses
    // the already-decoded selected-frame cache and reference frame from
    // the most recent alignSelected() call rather than redecoding
    // anything, so an edit-then-retrack cycle is cheap; requires
    // alignSelected() to have completed at least once this session
    // (reports errorOccurred() otherwise). `points` is clamped to
    // kAlignMaxPointsMax by the caller's own add-limit, but a too-large or
    // empty list here also reports errorOccurred() rather than silently
    // truncating it.
    void realignWithPoints(const std::vector<AlignmentPoint>& points);
    // Tracking-patch ("box") size used by alignSelected() and
    // realignWithPoints(), in pixels. Clamped to a sane range and takes
    // effect on the next call to either. minSpacing between points scales
    // with it (see PipelineController.cpp) to keep the validated ~0.75x
    // ratio between patch size and how far apart candidate points are
    // placed -- realignWithPoints() only uses it for tracking, not spacing,
    // since manual points aren't auto-spaced.
    void setAlignPatchSize(int patchSize);
    int alignPatchSize() const { return alignPatchSize_; }
    // Number of automatically-placed tracking points (see
    // selectAlignmentPoints in MultiPointAlignment.h) to use during the next
    // alignSelected() call. Clamped to [kAlignMaxPointsMin, kAlignMaxPointsMax];
    // how many actually get placed can still come in lower than this if the
    // disk doesn't have enough well-spaced, high-contrast candidates.
    void setAlignMaxPoints(int maxPoints);
    int alignMaxPoints() const { return alignMaxPoints_; }
    // Per-frame outlier-rejection threshold (pixels): a point whose
    // estimated shift differs from that frame's consensus by more than this
    // is replaced by the consensus -- see robustifyPointShifts's doc
    // comment in MultiPointAlignment.h for why. Exposed to the GUI as the
    // "sigma clip" control for deviating boxes. Also used by
    // realignWithPoints().
    void setAlignMaxDeviation(double maxDeviationPx);
    double alignMaxDeviation() const { return alignMaxDeviationPx_; }
    void stackMean(const StackParams& params);
    void stackDrizzle(const DrizzleParams& params);
    void applySharpen(SharpenMode mode, const WaveletParams& wp, const RLParams& rp);
    void applyColor(const LevelsParams& lp, const std::vector<std::pair<float, float>>& curve,
                    const SaturationParams& sp);
    // Corrects lateral chromatic aberration (color fringing) by resampling
    // the red/blue channels of the *color-adjusted* result by their own
    // small shift, green held fixed as the reference -- see
    // proc/ChromaticAberration.h. Runs after applyColor() in the pipeline
    // (requires finalResult_, same as applyColor() requires a prior
    // sharpen), so re-running color adjustments after this has already run
    // doesn't silently drop it -- see MainWindow's cascade, which mirrors
    // the sharpen-then-color one already in place.
    void applyChromaticAberration(const ChromaticAberrationParams& params);
    // Synchronous (a couple of FFT phase correlations, not a per-frame
    // loop) cross-channel correlation against whatever the best currently
    // available result is (final color-adjusted result, falling back to
    // the sharpened or stacked result if color hasn't run yet), used to
    // pre-fill the manual sliders with a reasonable starting point.
    ChromaticAberrationParams detectCA() const;
    bool exportImage(const QString& path) const;

    // Quick synchronous decode of a single frame, used only to show
    // something in the preview immediately after opening a sequence, before
    // any other stage has run.
    QImage previewFrame(int index) const;

    int frameCount() const { return source_ ? static_cast<int>(source_->frameCount()) : 0; }
    int selectedCount() const { return static_cast<int>(selectedIndices_.size()); }
    std::vector<int> preColorHistogram(int channel = -1) const;

    // --- Alignment inspector support -----------------------------------
    // Everything the "scroll through aligned frames with alignment-point
    // boxes" GUI view needs, valid once alignSelected() has completed.
    // Positions here are indices into the *selected* subset (0..alignedFrameCount()-1),
    // not original sequence frame numbers -- use originalFrameIndex() to
    // recover the latter for display.
    int alignedFrameCount() const { return static_cast<int>(selectedFrameCache_.size()); }
    int originalFrameIndex(int pos) const {
        return (pos >= 0 && pos < static_cast<int>(selectedIndices_.size()))
                   ? static_cast<int>(selectedIndices_[pos])
                   : -1;
    }
    const std::vector<AlignmentPoint>& alignmentPoints() const { return alignment_.points; }
    int alignmentPatchSize() const { return alignment_.patchSize; }
    std::vector<Transform2D> pointShiftsForFrame(int pos) const {
        return (pos >= 0 && pos < static_cast<int>(alignment_.perFramePointShifts.size()))
                   ? alignment_.perFramePointShifts[pos]
                   : std::vector<Transform2D>{};
    }
    // The raw, unaligned decoded frame at selected-subset position `pos`
    // (same pixels the tracking points were actually measured against).
    QImage rawSelectedFramePreview(int pos) const;
    // The same frame after applying this frame's own per-pixel blended
    // local warp (see MultiPointAlignment.h's blendShiftAt) -- i.e.
    // exactly what stacking/drizzle sample from at every pixel, made
    // visible one frame at a time.
    QImage alignedFramePreview(int pos) const;

    // Read-only diagnostic access to the per-frame quality scores computed
    // by the last computeQuality() run -- used by manual_pipeline_run /
    // wavelet_diagnostic to sanity-check the quality metric and selection
    // against real capture files, not needed by MainWindow itself.
    const std::vector<FrameQuality>& qualityScoresDebug() const { return qualityScores_; }
    const MultiPointAlignmentResult& alignmentDebug() const { return alignment_; }

signals:
    void sequenceOpened(int width, int height, int frameCount, QString formatDescription);
    void qualityProgress(int percent);
    void qualityDone(int totalFrames);
    // Same 0-100 percent-complete convention as qualityProgress, emitted
    // during alignSelected()'s per-frame loop and during stackMean()/
    // stackDrizzle()'s underlying stackFrames()/drizzleStack() calls, so the
    // GUI can drive the same progress bar for those stages too.
    void alignProgress(int percent);
    void stackProgress(int percent);
    // estimatedMegabytes is the memory the kept-frame cache will use once
    // alignSelected() decodes it -- shown in the UI so a percentage that
    // would risk exhausting RAM is visible *before* the user hits it,
    // rather than the process silently getting OOM-killed.
    void selectionChanged(int keptCount, int totalScored, double estimatedMegabytes);
    void alignDone(double averageConfidence);
    void stackDone(QImage preview);
    void sharpenDone(QImage preview);
    void colorDone(QImage preview);
    void chromaticAberrationDone(QImage preview);
    void errorOccurred(QString message);

private:
    ImageBuffer decodeFrame(size_t index) const;
    double estimateSelectedMegabytes() const;
    void cropStackedResultToObject();
    // Shared per-frame tracking pass used by both alignSelected() (after
    // automatic placement) and realignWithPoints() (after a manual edit):
    // correlates every selected frame's already-decoded, already-cached
    // luminance against the cached reference for each of `points`, robustifies,
    // and rebuilds alignment_ (points/perFramePointShifts/blendWeights/
    // averageConfidence) in place. Assumes selectedFrameCache_, referencePos_
    // and referenceCenter_ are already valid -- callers are responsible for
    // that precondition. Runs on whatever thread it's called from; both
    // callers wrap it in QtConcurrent::run().
    void trackPoints(const std::vector<AlignmentPoint>& points, int patchSize, double maxDeviationPx);

    std::unique_ptr<FrameSource> source_;
    std::vector<FrameQuality> qualityScores_;   // one score per frame; never holds pixel data
    std::vector<size_t> selectedIndices_;
    size_t referenceIndex_ = 0;
    // Position of the reference frame *within* selectedIndices_/
    // selectedFrameCache_ (as opposed to referenceIndex_, which is its
    // index in the original, full sequence) -- set by alignSelected(),
    // reused by realignWithPoints()/trackPoints() so a manual re-track
    // doesn't need to reselect a reference frame.
    size_t referencePos_ = 0;
    // The reference frame's own detected object center, cached from
    // alignSelected() so realignWithPoints() doesn't need to recompute it
    // (it's identical for every re-track against the same reference).
    Point2D referenceCenter_;

    // Default matches kDefaultAlignPatchSize in PipelineController.cpp
    // (see its comment for how this was validated); setAlignPatchSize()
    // lets the GUI override it per-sequence, since how much local contrast
    // a capture has to correlate on still varies.
    int alignPatchSize_ = 32;

    // Defaults match kDefaultAlignMaxPoints / kDefaultAlignMaxDeviationPx in
    // PipelineController.cpp; setAlignMaxPoints()/setAlignMaxDeviation() let
    // the GUI override them per-sequence.
    int alignMaxPoints_ = 12;
    double alignMaxDeviationPx_ = 12.0;

    // Decoded pixel data for ONLY the selected subset, populated by
    // alignSelected() and reused by the stacking stage. Deliberately never
    // holds the whole sequence -- computeQuality() scores each frame and
    // discards it immediately, and this cache is sized to the kept
    // percentage, not the total frame count. That's the fix for a real
    // out-of-memory kill seen on a several-thousand-frame planetary AVI:
    // the previous version cached every decoded frame during the quality
    // pass regardless of how many would end up selected.
    std::vector<ImageBuffer> selectedFrameCache_; // parallel to selectedIndices_

    // Multiple-alignment-point result: a set of tracking points placed on
    // the reference frame's disk (belt edges, limb, festoons -- whatever
    // has real contrast), each with its own per-frame local shift, plus
    // the shared per-pixel blend-weight table that turns those into a
    // locally-varying warp during stacking/drizzle. Replaces a single
    // global Transform2D per frame: real-data testing showed a whole-frame
    // correlation on this kind of footage can lock onto a shift tens of
    // pixels away from what every local feature actually indicates, once
    // its confidence sits close to the noise floor (soft, low-contrast
    // planetary video with a lot of near-black background).
    MultiPointAlignmentResult alignment_;

    ImageBuffer stackedResult_;
    ImageBuffer sharpenedResult_;
    ImageBuffer finalResult_;
    // Final output once chromatic-aberration correction has been applied;
    // exportImage() prefers this over finalResult_ when it's non-empty.
    ImageBuffer chromaticAberrationResult_;
};

} // namespace ls
