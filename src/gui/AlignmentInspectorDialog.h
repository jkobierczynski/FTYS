#pragma once

#include "gui/PipelineController.h"
#include "gui/PreviewWidget.h"

#include <QDialog>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>
#include <QShowEvent>
#include <vector>

namespace ls {

// Lets you scroll through the aligned frame subset one at a time, with
// each multi-point alignment point drawn as a box (patch-sized, not a
// single pixel -- these track real disk features, not point samples).
// Yellow means this frame's own tracked shift for that box was trusted as
// measured; red means robustifyPointShifts() sigma-clipped it for this
// frame (its own confidence was too low, or its shift deviated too far
// from that frame's consensus) and replaced it with the consensus instead
// -- see Transform2D::clipped's doc comment. Two view modes:
//  - "raw": the frame exactly as decoded, with each box positioned at
//    (point + that frame's own local shift) -- i.e. where the tracked
//    feature was actually found in this frame's own pixels. Watching the
//    boxes "chase" real belt/limb detail frame to frame is the direct
//    visual check that tracking is landing on something real.
//  - "aligned": the frame after this frame's own per-pixel blended warp
//    (exactly what stacking samples), with boxes fixed at each point's
//    canonical position -- if tracking and blending are working, the same
//    real feature should sit under the same box in every frame.
//
// Also hosts manual box editing: "Manual box editing" switches the preview
// into an add/move/delete surface (left-click empty space to add a box up
// to PipelineController::kAlignMaxPointsMax, left-drag an existing box to
// move it, right-click one to delete it), forces the "aligned" view (boxes
// are edited in the reference frame's canonical coordinate space, which is
// only what "aligned" shows consistently across every frame), and disables
// scrubbing while editing so the box positions being edited can't be
// confused with a specific frame's own raw content. Edits are staged in
// editedPoints_ and only take effect -- i.e. get real per-frame tracked
// shifts instead of sitting untracked at their canonical position -- once
// "Re-track with these boxes" is clicked, which calls
// PipelineController::realignWithPoints().
class AlignmentInspectorDialog : public QDialog {
    Q_OBJECT
public:
    explicit AlignmentInspectorDialog(PipelineController* controller, QWidget* parent = nullptr);

    // Call whenever alignSelected()/realignWithPoints() completes (including
    // re-alignment after a changed selection) so an already-open dialog
    // picks up the new frame count / point set without needing to be closed
    // and reopened. Also resyncs editedPoints_ from the controller's
    // now-current point list, discarding any not-yet-retracked manual edits.
    void refresh();

private slots:
    void updateView();
    void onEditModeToggled(bool on);
    void onRetrackClicked();
    void onResetAutomaticClicked();
    void onImagePressed(QPointF pos, Qt::MouseButton button);
    void onImageMoved(QPointF pos);
    void onImageReleased(QPointF pos);

protected:
    void showEvent(QShowEvent* event) override;

private:
    // Index into editedPoints_ under `pos` (within half a patch size in
    // both axes), or -1 if none. Iterates back-to-front so a box added most
    // recently (drawn on top) wins a tie.
    int hitTestPoint(QPointF pos) const;
    void setEditingControlsEnabled(bool editing);

    PipelineController* controller_;
    PreviewWidget* preview_;
    QSlider* slider_;
    QSpinBox* posSpin_;
    QLabel* infoLabel_;
    QCheckBox* alignedCheck_;
    bool fitDone_ = false;

    // --- Manual box editing ---------------------------------------------
    QCheckBox* editModeCheck_;
    QPushButton* retrackButton_;
    QPushButton* resetAutoButton_;
    QLabel* editHintLabel_;
    // Working copy of the point list while editing; resynced from
    // controller_->alignmentPoints() on refresh() and whenever edit mode is
    // turned on. Only pushed to the controller (and given real per-frame
    // shifts) when "Re-track with these boxes" is clicked.
    std::vector<AlignmentPoint> editedPoints_;
    bool editsPending_ = false; // true once editedPoints_ differs from what was last tracked
    int dragIndex_ = -1;        // index in editedPoints_ currently being dragged, or -1
};

} // namespace ls
