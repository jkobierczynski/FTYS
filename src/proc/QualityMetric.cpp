#include "proc/QualityMetric.h"

#include <opencv2/opencv.hpp>
#include <algorithm>
#include <limits>

namespace ls {

Roi detectObjectRoi(const ImageBuffer& lum, float threshFraction, int margin) {
    int w = lum.width(), h = lum.height();
    float maxVal = 0.0f;
    for (size_t i = 0; i < lum.pixelCount(); ++i) maxVal = std::max(maxVal, lum.data()[i]);

    if (maxVal <= 0.0f) return Roi{0, 0, w, h};

    float thresh = maxVal * threshFraction;
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (lum.at(x, y, 0) >= thresh) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < minX || maxY < minY) return Roi{0, 0, w, h};

    minX = std::max(0, minX - margin);
    minY = std::max(0, minY - margin);
    maxX = std::min(w - 1, maxX + margin);
    maxY = std::min(h - 1, maxY + margin);

    Roi roi;
    roi.x = minX;
    roi.y = minY;
    roi.w = maxX - minX + 1;
    roi.h = maxY - minY + 1;
    return roi;
}

Point2D detectObjectCenter(const ImageBuffer& lum, float threshFraction) {
    int w = lum.width(), h = lum.height();
    float maxVal = 0.0f;
    for (size_t i = 0; i < lum.pixelCount(); ++i) maxVal = std::max(maxVal, lum.data()[i]);
    if (maxVal <= 0.0f) return Point2D{w / 2.0, h / 2.0};

    float thresh = maxVal * threshFraction;
    double sumX = 0.0, sumY = 0.0, sumW = 0.0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float v = lum.at(x, y, 0);
            if (v >= thresh) {
                double weight = v - thresh; // weight by how far above background, not a flat binary mask
                sumX += weight * x;
                sumY += weight * y;
                sumW += weight;
            }
        }
    }
    if (sumW <= 0.0) return Point2D{w / 2.0, h / 2.0};
    return Point2D{sumX / sumW, sumY / sumW};
}

double laplacianVarianceScore(const ImageBuffer& lum, const Roi& roi) {
    // Wrap the ROI directly as a non-owning cv::Mat over the ImageBuffer's
    // own storage (row stride = full image width, so we address the ROI as
    // a sub-rectangle rather than copying).
    cv::Mat full(lum.height(), lum.width(), CV_32F, const_cast<float*>(lum.data()));
    cv::Rect rect(roi.x, roi.y, roi.w, roi.h);
    rect &= cv::Rect(0, 0, lum.width(), lum.height());
    if (rect.width <= 2 || rect.height <= 2) return 0.0;

    cv::Mat region = full(rect);
    cv::Mat lap;
    cv::Laplacian(region, lap, CV_32F, 3);
    cv::Scalar mean, stddev;
    cv::meanStdDev(lap, mean, stddev);
    return stddev[0] * stddev[0];
}

double assessFrameQuality(const ImageBuffer& lum) {
    Roi roi = detectObjectRoi(lum);
    return laplacianVarianceScore(lum, roi);
}

} // namespace ls
