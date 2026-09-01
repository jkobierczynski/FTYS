#pragma once

#include <QWidget>
#include <vector>
#include <utility>

namespace ls {

// Interactive tone curve editor with a histogram backdrop. Control points
// live in normalized [0,1] x [0,1] space and always include fixed endpoints
// at x=0 and x=1 (their y can move, but they can't be deleted or cross
// neighbors). Left-drag moves the nearest point; double-click adds a point,
// or removes the nearest interior point if double-clicked directly on it.
// The piecewise-linear interpretation here matches ColorStretch::applyCurve
// exactly, so what's drawn is what gets applied.
class CurvesWidget : public QWidget {
    Q_OBJECT
public:
    explicit CurvesWidget(QWidget* parent = nullptr);

    void setHistogram(const std::vector<int>& histogram);
    std::vector<std::pair<float, float>> controlPoints() const { return points_; }
    void resetToIdentity();

    QSize sizeHint() const override { return QSize(320, 200); }

signals:
    void curveChanged(const std::vector<std::pair<float, float>>& points);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QPointF toWidget(std::pair<float, float> p) const;
    std::pair<float, float> toNormalized(QPointF p) const;
    int findNearestPoint(QPointF widgetPos, double maxDist) const;

    std::vector<std::pair<float, float>> points_; // sorted by x, size >= 2, endpoints fixed at x=0/x=1
    std::vector<int> histogram_;
    int dragIndex_ = -1;
    static constexpr int kMargin = 12;
};

} // namespace ls
