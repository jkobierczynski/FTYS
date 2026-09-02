#pragma once

#include "gui/PipelineController.h"
#include "gui/PreviewWidget.h"
#include "gui/QualityGraphWidget.h"

#include <QDialog>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QCheckBox>
#include <QShowEvent>
#include <vector>

namespace ls {

// Shows every scored frame's sharpness/quality as a sorted bar graph (best
// to worst, left to right -- green bars are currently kept by "keep best
// %", gray ones aren't, with a dashed line at the cutoff), and lets you
// scrub through that same sorted order with a slider or by clicking/
// dragging directly on the graph, previewing the actual decoded frame at
// each position alongside its original frame index and score. Meant to
// answer "how sharp is frame N really, and where does it fall relative to
// everything else" at a glance, and to make the effect of the "keep best
// %" slider visible on the real score distribution rather than just as a
// kept-count number.
class QualityInspectorDialog : public QDialog {
    Q_OBJECT
public:
    explicit QualityInspectorDialog(PipelineController* controller, QWidget* parent = nullptr);

    // Call after computeQuality() completes (a genuinely new set of scores).
    // Resets the scrub position back to the sharpest frame (rank 0) and
    // re-fits the preview, same convention as AlignmentInspectorDialog::
    // refresh() -- appropriate here since the whole score distribution
    // (and therefore what each rank even refers to) may have changed.
    void refresh();
    // Call whenever "keep best %" changes but the scores themselves
    // haven't (i.e. selectPercent() ran, not a new computeQuality()) --
    // recolors the graph's kept/not-kept boundary and updates the info
    // line, without resetting the current scrub position or re-fitting the
    // preview, so dragging the percent slider moves the cutoff live
    // without yanking the view back to rank 0.
    void refreshSelection();

private slots:
    void updateView();
    void onGraphRankClicked(int rank);
    void onLogScaleToggled(bool checked);

protected:
    void showEvent(QShowEvent* event) override;

private:
    PipelineController* controller_;
    QualityGraphWidget* graph_;
    PreviewWidget* preview_;
    QSlider* slider_;
    QSpinBox* rankSpin_;
    QCheckBox* logScaleCheck_;
    QLabel* infoLabel_;
    // Quality scores sorted descending by score (best first) -- the single
    // ordering shared by the graph, the slider/spin box, and the preview,
    // so "rank" always means the same position in all three.
    std::vector<FrameQuality> sorted_;
    bool fitDone_ = false;
};

} // namespace ls
