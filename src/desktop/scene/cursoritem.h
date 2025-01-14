// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DESKTOP_CURSORITEM_H
#define DESKTOP_CURSORITEM_H
#include <QCursor>
#include <QGraphicsItem>

namespace drawingboard {

// Emulated bitmap cursor for platforms like Emscripten that don't support them.
class CursorItem final : public QGraphicsItem {
public:
	enum { Type = UserType + 19 };

	CursorItem(QGraphicsItem *parent = nullptr);

	int type() const override { return Type; }

	QRectF boundingRect() const override;

	void setCursor(const QCursor &cursor);

	void setOnCanvas(bool onCanvas);

protected:
	void paint(
		QPainter *painter, const QStyleOptionGraphicsItem *option,
		QWidget *widget = nullptr) override;

private:
	void updateVisibility();

	QCursor m_cursor;
	QRectF m_bounds;
	bool m_onCanvas = false;
};

}

#endif
