#pragma once

#include <QLabel>
#include <QPoint>

// A QLabel that emits clicked(pos) on left-click, pos in widget-local
// coordinates - used for the preview pane's click-to-zoom-1:1 feature.
class ClickableLabel : public QLabel {
	Q_OBJECT
public:
	explicit ClickableLabel(QWidget *parent = nullptr);

signals:
	void clicked(QPoint pos);

protected:
	void mousePressEvent(QMouseEvent *event) override;
};
