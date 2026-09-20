#include "ClickableLabel.h"

#include <QMouseEvent>

ClickableLabel::ClickableLabel(QWidget *parent) : QLabel(parent) {}

void ClickableLabel::mousePressEvent(QMouseEvent *event) {
	if (event->button() == Qt::LeftButton)
		emit clicked(event->pos());
	QLabel::mousePressEvent(event);
}
