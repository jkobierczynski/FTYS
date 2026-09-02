#pragma once

#include "proc/FrameSelector.h"

#include <QWidget>
#include <vector>

namespace ls {

// Bar chart of per-frame quality scores, sorted best-to-worst (left to
// right), with the "keep best %" boundary and a scrub cursor drawn on top.
// The caller (QualityInspectorDialog) owns the sort -- this widget just
// draws whatever vector it's handed, in the order it's handed, so there's
// one place (the dialog) that decides what "sorted" and "kept" mean rather
// than two copies of that logic risking disagreement.
class QualityGraphWidget : public QWidget {
    Q_OBJECT
public:
    explicit QualityGraphWidget(QWidget* parent = nullptr);

    // `sortedScores` must already be sorted however the caller wants it
    // drawn left-to-right (best-to-worst, i.e. descending by score, is the
    // convention QualityInspectorDialog uses). `keptCount` is how many of
    // the leading entries count as "kept" (drawn in the kept color) --
    // typically PipelineController::selectedCount(), since selectTopPercent
    // keeps exactly the top-scoring `keptCount` frames by the same
    // descending-score ordering.
    void setData(std::vector<FrameQuality> sortedScores, int keptCount);
    // Position (index into the vector passed to setData()) to highlight
    // with a bright vertical cursor line, or -1 for none.
    void setCursorRank(int rank);
    // Draws bar heights on a log10 scale instead of linear. Quality scores
    // (Laplacian variance) commonly span a couple of orders of magnitude
    // between the sharpest and softest frames -- on a linear scale that
    // squashes everything below the sharpest handful of frames down near
    // the axis, making the "keep best %" cutoff region (usually well below
    // the very best frames) hard to read. Persists across setData() calls,
    // i.e. toggling it doesn't require re-supplying the data.
    void setLogScale(bool enabled);
    bool logScale() const { return logScale_; }

    QSize sizeHint() const override { return QSize(400, 180); }

signals:
    // Emitted when the user clicks or drags across the graph, with the
    // rank (index into the last setData() vector) under the cursor --
    // lets the dialog's slider follow a direct click on the graph, not
    // just the other way around.
    void rankClicked(int rank);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    int rankAt(int xPixel) const;

    std::vector<FrameQuality> sorted_;
    int keptCount_ = 0;
    int cursorRank_ = -1;
    bool logScale_ = false;
    static constexpr int kMargin = 12;
};

} // namespace ls
