#include "HistogramWidget.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kLevels = 4096;
constexpr int kMaxLevel = kLevels - 1;
const QColor kBackground(0x20, 0x20, 0x20);
const QColor kBarColor(0xc8, 0xc8, 0xc8);
const QColor kClippedColor(0xdc, 0x3c, 0x3c);
const QColor kGridColor(0x50, 0x50, 0x50);
const QColor kTextColor(0xe0, 0xe0, 0xe0);
} // namespace

HistogramWidget::HistogramWidget(QWidget *parent) : QWidget(parent) {
	setMinimumHeight(140);
}

void HistogramWidget::setData(const QVector<quint32> &bins) {
	if (bins.size() != kLevels)
		return;
	bins_ = bins;
	update();
}

void HistogramWidget::paintEvent(QPaintEvent *) {
	QPainter painter(this);
	painter.fillRect(rect(), kBackground);

	// Faint quarter-range guides.
	painter.setPen(kGridColor);
	for (int q = 1; q < 4; ++q) {
		const int x = width() * q / 4;
		painter.drawLine(x, 0, x, height());
	}

	if (bins_.size() != kLevels)
		return;

	// One bar per pixel column; each column sums the bins it spans.
	const int columns = std::max(1, width());
	QVector<double> columnSum(columns, 0.0);
	for (int level = 0; level < kLevels; ++level) {
		const int col = std::min(columns - 1, level * columns / kLevels);
		columnSum[col] += bins_[level];
	}
	const double maxSum = *std::max_element(columnSum.begin(), columnSum.end());
	if (maxSum <= 0)
		return;

	const double logMax = std::log1p(maxSum);
	const int clippedColumn = std::min(columns - 1, kMaxLevel * columns / kLevels);
	for (int col = 0; col < columns; ++col) {
		if (columnSum[col] <= 0)
			continue;
		const double barHeight = std::log1p(columnSum[col]) / logMax * (height() - 16);
		const bool clipped = (col == clippedColumn) && bins_[kMaxLevel] > 0;
		painter.fillRect(QRectF(col, height() - barHeight, 1.0, barHeight),
				 clipped ? kClippedColor : kBarColor);
	}

	int highest = 0;
	for (int level = kMaxLevel; level >= 0; --level) {
		if (bins_[level] > 0) {
			highest = level;
			break;
		}
	}
	painter.setPen(bins_[kMaxLevel] > 0 ? kClippedColor : kTextColor);
	painter.drawText(rect().adjusted(6, 2, -6, 0), Qt::AlignTop | Qt::AlignRight,
			 QString("max %1 / 4095   clipped samples: %2")
				 .arg(highest)
				 .arg(bins_[kMaxLevel]));
	painter.setPen(kTextColor);
	painter.drawText(rect().adjusted(6, 2, -6, 0), Qt::AlignTop | Qt::AlignLeft,
			 "raw 12-bit (0 - 4095)");
}
