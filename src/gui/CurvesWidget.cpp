#include "gui/CurvesWidget.h"

#include <QPainter>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>

namespace ls {

CurvesWidget::CurvesWidget(QWidget* parent) : QWidget(parent) {
    resetToIdentity();
    setMinimumSize(200, 140);
    setMouseTracking(false);
}

void CurvesWidget::resetToIdentity() {
    points_ = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    update();
}

void CurvesWidget::setHistogram(const std::vector<int>& histogram) {
    histogram_ = histogram;
    update();
}

QPointF CurvesWidget::toWidget(std::pair<float, float> p) const {
    double w = width() - 2 * kMargin;
    double h = height() - 2 * kMargin;
    return QPointF(kMargin + p.first * w, height() - kMargin - p.second * h);
}

std::pair<float, float> CurvesWidget::toNormalized(QPointF p) const {
    double w = std::max(1, width() - 2 * kMargin);
    double h = std::max(1, height() - 2 * kMargin);
    float x = static_cast<float>((p.x() - kMargin) / w);
    float y = static_cast<float>((height() - kMargin - p.y()) / h);
    return {std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f)};
}

int CurvesWidget::findNearestPoint(QPointF widgetPos, double maxDist) const {
    int best = -1;
    double bestDist = maxDist;
    for (size_t i = 0; i < points_.size(); ++i) {
        QPointF wp = toWidget(points_[i]);
        double d = std::hypot(wp.x() - widgetPos.x(), wp.y() - widgetPos.y());
        if (d < bestDist) {
            bestDist = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void CurvesWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(24, 24, 24));

    QRectF plotRect(kMargin, kMargin, width() - 2 * kMargin, height() - 2 * kMargin);

    // Histogram backdrop, log-scaled so faint detail stays visible next to
    // a bright peak.
    if (!histogram_.empty()) {
        int maxCount = *std::max_element(histogram_.begin(), histogram_.end());
        if (maxCount > 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(90, 130, 160, 140));
            int bins = static_cast<int>(histogram_.size());
            for (int i = 0; i < bins; ++i) {
                double x0 = plotRect.left() + plotRect.width() * i / bins;
                double x1 = plotRect.left() + plotRect.width() * (i + 1) / bins;
                double logVal = std::log1p(histogram_[i]) / std::log1p(maxCount);
                double barH = logVal * plotRect.height();
                p.drawRect(QRectF(x0, plotRect.bottom() - barH, std::max(1.0, x1 - x0), barH));
            }
        }
    }

    // Grid.
    p.setPen(QPen(QColor(70, 70, 70), 1, Qt::DotLine));
    for (int i = 1; i < 4; ++i) {
        double x = plotRect.left() + plotRect.width() * i / 4.0;
        double y = plotRect.top() + plotRect.height() * i / 4.0;
        p.drawLine(QPointF(x, plotRect.top()), QPointF(x, plotRect.bottom()));
        p.drawLine(QPointF(plotRect.left(), y), QPointF(plotRect.right(), y));
    }
    p.setPen(QPen(QColor(120, 120, 120), 1));
    p.drawRect(plotRect);

    // Curve.
    p.setPen(QPen(QColor(255, 200, 60), 2));
    for (size_t i = 0; i + 1 < points_.size(); ++i) p.drawLine(toWidget(points_[i]), toWidget(points_[i + 1]));

    // Handles.
    p.setPen(QPen(Qt::white, 1));
    p.setBrush(QColor(255, 200, 60));
    for (const auto& pt : points_) {
        QPointF wp = toWidget(pt);
        p.drawEllipse(wp, 4, 4);
    }
}

void CurvesWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    dragIndex_ = findNearestPoint(event->pos(), 12.0);
}

void CurvesWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragIndex_ < 0) return;
    auto norm = toNormalized(event->pos());

    if (dragIndex_ == 0) {
        points_[0].second = norm.second;
    } else if (dragIndex_ == static_cast<int>(points_.size()) - 1) {
        points_[dragIndex_].second = norm.second;
    } else {
        float minX = points_[dragIndex_ - 1].first + 0.01f;
        float maxX = points_[dragIndex_ + 1].first - 0.01f;
        points_[dragIndex_] = {std::clamp(norm.first, minX, maxX), norm.second};
    }
    update();
    emit curveChanged(points_);
}

void CurvesWidget::mouseReleaseEvent(QMouseEvent*) {
    dragIndex_ = -1;
}

void CurvesWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    int near = findNearestPoint(event->pos(), 12.0);
    if (near > 0 && near < static_cast<int>(points_.size()) - 1) {
        points_.erase(points_.begin() + near);
    } else {
        auto norm = toNormalized(event->pos());
        auto it = std::lower_bound(points_.begin(), points_.end(), norm,
                                    [](const std::pair<float, float>& a, const std::pair<float, float>& b) {
                                        return a.first < b.first;
                                    });
        points_.insert(it, norm);
    }
    update();
    emit curveChanged(points_);
}

} // namespace ls
