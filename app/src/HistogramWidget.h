#pragma once

#include <QVector>
#include <QWidget>

// Paints a log-scale bar-chart of the sensor's raw 12-bit value histogram
// (4096 bins, colour-agnostic - every Bayer photosite counts the same).
// Log scale is used because real-scene histograms are dominated by a few
// tall spikes (e.g. a large flat background) that would flatten every
// other bin to invisibility on a linear scale. The columns covering the
// top code (4095 - a saturated, clipped photosite) are drawn in red when
// populated, since clipping is the main thing you're watching for when
// picking an exposure. The overlay text gives the highest raw value seen
// and how many samples are clipped.
class HistogramWidget : public QWidget {
	Q_OBJECT
public:
	explicit HistogramWidget(QWidget *parent = nullptr);

	// `bins` is the 4096-bin histogram; other sizes are ignored.
	void setData(const QVector<quint32> &bins);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QVector<quint32> bins_;
};
