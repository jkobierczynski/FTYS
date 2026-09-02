#include "gui/QualityGraphWidget.h"

#include <QPainter>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ls {

QualityGraphWidget::QualityGraphWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(300, 140);
    setMouseTracking(false);
}

void QualityGraphWidget::setData(std::vector<FrameQuality> sortedScores, int keptCount) {
    sorted_ = std::move(sortedScores);
    keptCount_ = std::clamp(keptCount, 0, static_cast<int>(sorted_.size()));
    update();
}

void QualityGraphWidget::setCursorRank(int rank) {
    cursorRank_ = rank;
    update();
}

void QualityGraphWidget::setLogScale(bool enabled) {
    if (logScale_ == enabled) return;
    logScale_ = enabled;
    update();
}

int QualityGraphWidget::rankAt(int xPixel) const {
    if (sorted_.empty()) return -1;
    double w = std::max(1, width() - 2 * kMargin);
    double frac = (xPixel - kMargin) / w;
    int rank = static_cast<int>(std::floor(frac * sorted_.size()));
    return std::clamp(rank, 0, static_cast<int>(sorted_.size()) - 1);
}

void QualityGraphWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(24, 24, 24));

    QRectF plotRect(kMargin, kMargin, width() - 2 * kMargin, height() - 2 * kMargin);

    // Grid, same convention as CurvesWidget.
    p.setPen(QPen(QColor(70, 70, 70), 1, Qt::DotLine));
    for (int i = 1; i < 4; ++i) {
        double x = plotRect.left() + plotRect.width() * i / 4.0;
        double y = plotRect.top() + plotRect.height() * i / 4.0;
        p.drawLine(QPointF(x, plotRect.top()), QPointF(x, plotRect.bottom()));
        p.drawLine(QPointF(plotRect.left(), y), QPointF(plotRect.right(), y));
    }
    p.setPen(QPen(QColor(120, 120, 120), 1));
    p.drawRect(plotRect);

    if (sorted_.empty()) {
        p.setPen(QColor(160, 160, 160));
        p.drawText(plotRect, Qt::AlignCenter, "Run \"Assess Quality\" to see the graph");
        return;
    }

    double maxScore = 0.0;
    double minPositiveScore = std::numeric_limits<double>::max();
    for (const auto& s : sorted_) {
        maxScore = std::max(maxScore, s.score);
        if (s.score > 0.0) minPositiveScore = std::min(minPositiveScore, s.score);
    }
    if (maxScore <= 0.0) maxScore = 1.0; // degenerate (all-zero scores): avoid dividing by zero

    // Log scale needs a positive floor to map to the bottom of the chart --
    // zero (and anything below it) has no log10. Use the data's own
    // smallest positive score so the chart uses its full height for
    // whatever dynamic range this capture actually has; fall back to a
    // fixed three-decade floor below the max when every score is
    // non-positive or all scores are identical (minPositiveScore never got
    // set below maxScore).
    double logFloor = 0.0, logMax = 0.0, logDenom = 1.0;
    if (logScale_) {
        double floor = (minPositiveScore < maxScore) ? minPositiveScore : maxScore * 1e-3;
        if (floor <= 0.0) floor = maxScore * 1e-3;
        logFloor = std::log10(floor);
        logMax = std::log10(maxScore);
        logDenom = logMax - logFloor;
        if (logDenom < 1e-9) logDenom = 1.0; // avoid dividing by zero if floor==max
    }

    int n = static_cast<int>(sorted_.size());
    double barW = plotRect.width() / n;

    for (int i = 0; i < n; ++i) {
        double score = sorted_[static_cast<size_t>(i)].score;
        double frac;
        if (logScale_) {
            double v = std::max(score, std::pow(10.0, logFloor));
            frac = std::clamp((std::log10(v) - logFloor) / logDenom, 0.0, 1.0);
        } else {
            frac = std::clamp(score / maxScore, 0.0, 1.0);
        }
        double x0 = plotRect.left() + barW * i;
        double h = frac * plotRect.height();
        QColor color = (i < keptCount_) ? QColor(120, 200, 120) : QColor(100, 100, 100);
        p.fillRect(QRectF(x0, plotRect.bottom() - h, std::max(1.0, barW), h), color);
    }

    if (logScale_) {
        p.setPen(QColor(160, 160, 160));
        p.drawText(plotRect.adjusted(4, 2, -4, 0), Qt::AlignTop | Qt::AlignRight, "log scale");
    }

    // "Keep best %" boundary: a dashed line right at the cutoff between
    // kept (green) and not-kept (gray) bars, so the effect of the Frame
    // Selection percent slider is visible directly on the distribution,
    // not just as a count.
    if (keptCount_ > 0 && keptCount_ < n) {
        double xBoundary = plotRect.left() + barW * keptCount_;
        p.setPen(QPen(QColor(255, 255, 255, 160), 1, Qt::DashLine));
        p.drawLine(QPointF(xBoundary, plotRect.top()), QPointF(xBoundary, plotRect.bottom()));
    }

    // Scrub cursor: a bright vertical line over whichever rank the dialog's
    // slider currently points at.
    if (cursorRank_ >= 0 && cursorRank_ < n) {
        double xCursor = plotRect.left() + barW * (cursorRank_ + 0.5);
        p.setPen(QPen(QColor(255, 220, 0), 2));
        p.drawLine(QPointF(xCursor, plotRect.top() - 4), QPointF(xCursor, plotRect.bottom() + 4));
    }
}

void QualityGraphWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    int rank = rankAt(event->pos().x());
    if (rank >= 0) emit rankClicked(rank);
}

void QualityGraphWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton)) return;
    int rank = rankAt(event->pos().x());
    if (rank >= 0) emit rankClicked(rank);
}

} // namespace ls
