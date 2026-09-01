#pragma once

#include "gui/PipelineController.h"
#include "gui/PreviewWidget.h"

#include <QDialog>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QCheckBox>
#include <QShowEvent>

namespace ls {

// Lets you scroll through the aligned frame subset one at a time, with
// each multi-point alignment point drawn as a yellow box (patch-sized, not
// a single pixel -- these track real disk features, not point samples).
// Two view modes:
//  - "raw": the frame exactly as decoded, with each box positioned at
//    (point + that frame's own local shift) -- i.e. where the tracked
//    feature was actually found in this frame's own pixels. Watching the
//    boxes "chase" real belt/limb detail frame to frame is the direct
//    visual check that tracking is landing on something real.
//  - "aligned": the frame after this frame's own per-pixel blended warp
//    (exactly what stacking samples), with boxes fixed at each point's
//    canonical position -- if tracking and blending are working, the same
//    real feature should sit under the same box in every frame.
class AlignmentInspectorDialog : public QDialog {
    Q_OBJECT
public:
    explicit AlignmentInspectorDialog(PipelineController* controller, QWidget* parent = nullptr);

    // Call whenever alignSelected() completes (including re-alignment after
    // a changed selection) so an already-open dialog picks up the new
    // frame count / point set without needing to be closed and reopened.
    void refresh();

private slots:
    void updateView();

protected:
    void showEvent(QShowEvent* event) override;

private:
    PipelineController* controller_;
    PreviewWidget* preview_;
    QSlider* slider_;
    QSpinBox* posSpin_;
    QLabel* infoLabel_;
    QCheckBox* alignedCheck_;
    bool fitDone_ = false;
};

} // namespace ls
