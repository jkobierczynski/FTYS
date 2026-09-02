#pragma once

#include "gui/PipelineController.h"
#include "gui/PreviewWidget.h"
#include "gui/AlignmentInspectorDialog.h"
#include "gui/QualityInspectorDialog.h"
#include "gui/ColorAdjustmentDialog.h"

#include <QMainWindow>
#include <QLabel>
#include <QSlider>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QProgressBar>
#include <QStackedWidget>

namespace ls {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onOpenSequence();
    void onAssessQuality();
    void onPercentChanged(int value);
    void onInspectQuality();
    void onAlign();
    void onInspectAlignment();
    void onStack();
    void onSharpen();
    void onAdjustColor();
    void onDetectCA();
    void onApplyCA();
    void onExportFormatChanged(int index);
    void onExport();

    void onSequenceOpened(int width, int height, int frameCount, QString formatDescription);
    void onQualityProgress(int percent);
    void onQualityDone(int totalFrames);
    void onSelectionChanged(int keptCount, int totalScored, double estimatedMegabytes);
    void onAlignDone(double averageConfidence);
    void onStackDone(QImage preview);
    void onSharpenDone(QImage preview);
    void onColorDone(QImage preview);
    void onChromaticAberrationDone(QImage preview);
    void onError(QString message);

private:
    QWidget* buildControlsPanel();
    void setEnabledStageButtons();

    PipelineController* controller_;
    PreviewWidget* preview_;

    // Sequence.
    QLabel* sequenceInfoLabel_;

    // Frame selection.
    QSlider* percentSlider_;
    QLabel* percentValueLabel_;
    QPushButton* assessButton_;
    QLabel* selectionLabel_;
    bool qualityReady_ = false;
    QPushButton* inspectQualityButton_;
    QualityInspectorDialog* qualityInspector_ = nullptr; // lazily created

    // Alignment.
    QPushButton* alignButton_;
    QLabel* alignLabel_;
    QSpinBox* alignPatchSizeSpin_;
    QSpinBox* alignMaxPointsSpin_;
    QDoubleSpinBox* alignMaxDeviationSpin_;
    QPushButton* inspectAlignmentButton_;
    AlignmentInspectorDialog* alignmentInspector_ = nullptr; // lazily created

    // Stacking.
    QComboBox* stackModeCombo_;
    QStackedWidget* stackParamsStack_;
    QDoubleSpinBox* sigmaLowSpin_;
    QDoubleSpinBox* sigmaHighSpin_;
    QDoubleSpinBox* drizzleScaleSpin_;
    QDoubleSpinBox* drizzleDropSpin_;
    QPushButton* stackButton_;

    // Sharpening.
    QComboBox* sharpenModeCombo_;
    QStackedWidget* sharpenParamsStack_;
    QDoubleSpinBox* waveletGainSpins_[4];
    QSpinBox* rlIterationsSpin_;
    QDoubleSpinBox* rlSigmaSpin_;
    QPushButton* sharpenButton_;

    // Color -- all the actual controls live in ColorAdjustmentDialog now;
    // MainWindow just owns the button that opens it and the flag that
    // drives the sharpen-then-color cascade below.
    QPushButton* adjustColorButton_;
    ColorAdjustmentDialog* colorAdjustDialog_ = nullptr; // lazily created
    // Set once color adjustments have been applied at least once; used to
    // auto-reapply them (via colorAdjustDialog_->refresh(), which reapplies
    // whatever settings are currently dialed in there) after a sharpen
    // (re)run, so redoing Wavelet/Richardson-Lucy doesn't silently drop a
    // previously-applied histogram/color stretch from the preview and
    // export.
    bool colorApplied_ = false;

    // Chromatic aberration (RGB channel alignment).
    QDoubleSpinBox* caRedDxSpin_;
    QDoubleSpinBox* caRedDySpin_;
    QDoubleSpinBox* caBlueDxSpin_;
    QDoubleSpinBox* caBlueDySpin_;
    QPushButton* caDetectButton_;
    QPushButton* caApplyButton_;
    // Same idea as colorApplied_, one stage further downstream: set once CA
    // correction has been applied at least once, so re-running color
    // adjustments (or a sharpen that cascades into color, see
    // onSharpenDone()) also cascades into re-running CA correction with the
    // same offsets, rather than leaving the preview/export on a CA-corrected
    // image computed against a now-stale color/sharpen result.
    bool caApplied_ = false;

    // Export.
    QComboBox* exportFormatCombo_;    // PNG / TIFF / FITS
    QComboBox* exportBitDepthCombo_;  // 8-bit / 16-bit -- disabled (forced 8-bit) when format is PNG
    QPushButton* exportButton_;

    QProgressBar* progressBar_;
};

} // namespace ls
