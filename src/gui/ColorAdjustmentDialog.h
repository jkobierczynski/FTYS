#pragma once

#include "gui/PipelineController.h"
#include "gui/PreviewWidget.h"
#include "gui/CurvesWidget.h"

#include <QDialog>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTimer>
#include <QShowEvent>
#include <vector>
#include <utility>

namespace ls {

// Dedicated window for the "Histogram / Color" stage. This used to be a
// handful of spin boxes and a small curve widget squeezed into the side
// panel behind a manual "Apply" button -- not much room to make a
// deliberate decision from a histogram, and no way to touch anything but
// levels/curve/saturation. This dialog gives the histogram+curve editor
// real space to work in, adds color balance (independent per-channel
// gain), hue rotation, and brightness, and updates the preview live as any
// control changes (debounced briefly so a slider drag or curve-point drag
// doesn't re-run the whole stage on every intermediate mouse-move event)
// instead of requiring an explicit Apply click.
//
// Owns none of the pipeline state -- like AlignmentInspectorDialog and
// QualityInspectorDialog, it just drives PipelineController and listens to
// its signals, so MainWindow's own preview/status bar/downstream buttons
// stay in sync with whatever this dialog does automatically (both are
// listening to the same PipelineController::colorDone signal).
class ColorAdjustmentDialog : public QDialog {
    Q_OBJECT
public:
    explicit ColorAdjustmentDialog(PipelineController* controller, QWidget* parent = nullptr);

    // Call whenever the upstream sharpened result changes (a fresh Apply
    // Sharpening run) -- refreshes the histogram against the new data and
    // reapplies whatever settings are currently dialed in immediately
    // (bypassing the debounce), so the preview/export don't silently keep
    // showing a stretch computed against the previous sharpen output. Also
    // what MainWindow calls right after lazily creating the dialog, so it
    // opens already showing real data instead of a blank histogram.
    void refresh();

private slots:
    void onControlChanged();  // any spin box -- (re)starts the debounce timer
    void onCurveChanged(const std::vector<std::pair<float, float>>& points);
    void onDebounceTimeout();  // debounce settled -- actually recompute
    void onReset();
    void onColorResultReady(QImage preview);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void applyNow();

    PipelineController* controller_;
    PreviewWidget* preview_;
    CurvesWidget* curvesWidget_;

    QDoubleSpinBox* blackPointSpin_;
    QDoubleSpinBox* whitePointSpin_;
    QDoubleSpinBox* gammaSpin_;
    QDoubleSpinBox* brightnessSpin_;
    QDoubleSpinBox* redGainSpin_;
    QDoubleSpinBox* greenGainSpin_;
    QDoubleSpinBox* blueGainSpin_;
    QDoubleSpinBox* hueSpin_;
    QDoubleSpinBox* saturationSpin_;
    QPushButton* resetButton_;

    QTimer* debounceTimer_;
    bool fitDone_ = false;
};

} // namespace ls
