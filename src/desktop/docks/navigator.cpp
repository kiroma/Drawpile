// SPDX-License-Identifier: GPL-3.0-or-later

#include "desktop/docks/navigator.h"
#include "desktop/main.h"
#include "desktop/widgets/groupedtoolbutton.h"
#include "desktop/widgets/kis_slider_spin_box.h"
#include "libclient/canvas/canvasmodel.h"
#include "libclient/canvas/paintengine.h"
#include "libclient/canvas/userlist.h"
#include "libclient/settings.h"
#include "desktop/docks/titlewidget.h"

#include <QIcon>
#include <QMouseEvent>
#include <QTimer>
#include <QPainter>
#include <QPainterPath>
#include <QAction>
#include <QDateTime>
#include <QBoxLayout>
#include <QMenu>

using libclient::settings::zoomMax;
using libclient::settings::zoomMin;

namespace docks {

static QPixmap makeCursorBackground(const int avatarSize)
{
	const int PADDING = 4;
	const int ARROW = 4;
	const QSize s { avatarSize + PADDING*2, avatarSize + PADDING*2 + ARROW };

	QPixmap pixmap(s);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);

	QPainterPath p(QPointF(s.width()/2, s.height()));
	p.lineTo(s.width()/2 + ARROW, s.height() - ARROW);
	p.lineTo(s.width() - PADDING, s.height() - ARROW);
	p.quadTo(s.width(), s.height() - ARROW, s.width(), s.height() - ARROW - PADDING);
	p.lineTo(s.width(), PADDING);
	p.quadTo(s.width(), 0, s.width() - PADDING, 0);
	p.lineTo(PADDING, 0);
	p.quadTo(0, 0, 0, PADDING);
	p.lineTo(0, s.height() - PADDING - ARROW);
	p.quadTo(0, s.height() - ARROW, PADDING, s.height() - ARROW);
	p.lineTo(s.width()/2 - ARROW, s.height() - ARROW);
	p.closeSubpath();

	painter.fillPath(p, Qt::black);

	return pixmap;
}

NavigatorView::NavigatorView(QWidget *parent)
	: QWidget(parent), m_model(nullptr), m_zoomWheelDelta(0),
	  m_showCursors(true)
{
	m_refreshTimer = new QTimer(this);
	m_refreshTimer->setSingleShot(true);
	setRealtimeUpdate(false);
	connect(m_refreshTimer, &QTimer::timeout, this, &NavigatorView::refreshCache);

	// Draw the marker background
	m_cursorBackground = makeCursorBackground(16);
}

void NavigatorView::setCanvasModel(canvas::CanvasModel *model)
{
	m_model = model;
	connect(m_model->paintEngine(), &canvas::PaintEngine::areaChanged, this, &NavigatorView::onChange, Qt::QueuedConnection);
	connect(m_model->paintEngine(), &canvas::PaintEngine::resized, this, &NavigatorView::onResize, Qt::QueuedConnection);
	connect(m_model->paintEngine(), &canvas::PaintEngine::cursorMoved, this, &NavigatorView::onCursorMove);
	m_model->paintEngine()->setRenderOutsideView(isVisible());
	m_refreshAll = true;
	refreshCache();
}

void NavigatorView::setShowCursors(bool show)
{
	m_showCursors = show;
	update();
}

void NavigatorView::setRealtimeUpdate(bool realtime)
{
	m_refreshTimer->setInterval(realtime ? (1000 / 60) : 500);
}

void NavigatorView::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	// Resizes while hidden mean that we're about to be shown.
	if(!m_refreshTimer->isActive()) {
		if(isVisible()) {
			m_refreshTimer->start();
		} else {
			refreshCache();
		}
	}
}

/**
 * Start dragging the view focus
 */
void NavigatorView::mousePressEvent(QMouseEvent *event)
{
	if(event->button() != Qt::RightButton && !m_cache.isNull()) {
		emit focusMoved(getFocusPoint(compat::mousePosition(*event)));
	}
}

/**
 * Drag the view focus
 */
void NavigatorView::mouseMoveEvent(QMouseEvent *event)
{
	mousePressEvent(event);
}

void NavigatorView::wheelEvent(QWheelEvent *event)
{
	// Use scroll wheel for zooming
	m_zoomWheelDelta += event->angleDelta().y();
	const int steps = m_zoomWheelDelta / 120;
	m_zoomWheelDelta -= steps * 120;

	if(steps != 0 && !m_cache.isNull()) {
		emit wheelZoom(steps);
	}
}

void NavigatorView::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if(m_model) {
		m_model->paintEngine()->setRenderOutsideView(true);
		m_refreshTimer->stop();
		m_refreshAll = true;
		refreshCache();
	}
}

void NavigatorView::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	m_refreshTimer->stop();
	if(m_model) {
		m_model->paintEngine()->setRenderOutsideView(false);
	}
}

/**
 * The focus rectangle represents the visible area in the
 * main viewport.
 */
void NavigatorView::setViewFocus(const QPolygonF& rect)
{
	m_focusRect = rect;
	update();
}


void NavigatorView::onChange(const QRect &rect)
{
	if(isVisible()) {
		if(rect.isValid()) {
			m_refreshArea |= rect;
		}
		if(!m_refreshTimer->isActive()) {
			m_refreshTimer->start();
		}
	}
}

void NavigatorView::onResize()
{
	m_cachedSize = QSize();
	onChange();
}

void NavigatorView::refreshCache()
{
	if(!m_model) {
		return;
	}

	m_model->paintEngine()->withPixmap([this](const QPixmap &pixmap) {
		if(!pixmap.isNull()) {
			QSize navigatorSize = size() * devicePixelRatioF();
			if(navigatorSize != m_cachedSize) {
				m_cachedSize = navigatorSize;
				m_cache = QPixmap{
					pixmap.size().scaled(navigatorSize, Qt::KeepAspectRatio)};
				m_refreshAll = true;
			}

			if(m_refreshAll) {
				QPainter painter(&m_cache);
				painter.drawPixmap(m_cache.rect(), pixmap);
				m_refreshAll = false;
				m_refreshArea = QRect{};
			} else if(m_refreshArea.isValid()) {
				QSizeF ratioSize{m_cache.size()};
				qreal xr = ratioSize.width() / qreal(pixmap.width());
				qreal yr = ratioSize.height() / qreal(pixmap.height());
				QRectF sourceArea = QRectF{m_refreshArea}.adjusted(
					-1.0 / xr, -1.0 / yr, 1.0 / xr, 1.0 / yr);
				QRectF targetArea{
					QPointF{sourceArea.left() * xr, sourceArea.top() * yr},
					QPointF{sourceArea.right() * xr, sourceArea.bottom() * yr}};
				QPainter painter(&m_cache);
				painter.drawPixmap(targetArea, pixmap, sourceArea);
				m_refreshArea = QRect{};
			}
		}
	});

	update();
}

void NavigatorView::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.fillRect(rect(), QColor(100,100,100));

	if(!m_model)
		return;

	// Draw downscaled canvas
	if(m_cache.isNull())
		return;

	const QSize scaledSize = m_cache.size().scaled(size(), Qt::KeepAspectRatio);
	const QRect canvasRect {
		width()/2 - scaledSize.width()/2,
		height()/2 - scaledSize.height()/2,
		scaledSize.width(),
		scaledSize.height()
	};
	painter.drawPixmap(canvasRect, m_cache);

	// Draw main viewport rectangle
	painter.save();

	QPen pen(QColor(96, 191, 96));
	pen.setCosmetic(true);
	pen.setWidth(2.0 * devicePixelRatioF());
	painter.setPen(pen);
	painter.setCompositionMode(QPainter::RasterOp_SourceXorDestination);

	const auto canvasSize = m_model->size();
	const qreal xscale = scaledSize.width() / qreal(canvasSize.width());
	const qreal yscale = scaledSize.height() / qreal(canvasSize.height());
	painter.translate(canvasRect.topLeft());
	painter.scale(xscale, yscale);
	painter.drawPolygon(m_focusRect);

	// Draw top-side marker line
	if(qAbs(m_focusRect[0].y() - m_focusRect[1].y()) >= 1.0 || m_focusRect[0].x() > m_focusRect[1].x()) {
		const QLineF normal { m_focusRect[3], m_focusRect[0] };
		QLineF top {m_focusRect[0], m_focusRect[1]};
		const auto s = (5.0 / xscale) / normal.length();
		top.translate(
		    (normal.x2() - normal.x1()) * s,
		    (normal.y2() - normal.y1()) * s
		);
		painter.drawLine(top);
	}

	painter.restore();
	// Draw user cursors
	if(m_showCursors) {
		QMutableVectorIterator<UserCursor> ci(m_cursors);

		const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - 1000;

		while(ci.hasNext()) {
			const auto &cursor = ci.next();
			if(cursor.lastMoved < cutoff) {
				ci.remove();
				continue;
			}

			const QPoint viewPoint = QPoint(
				cursor.pos.x() * xscale + canvasRect.x() - m_cursorBackground.width() / 2,
				cursor.pos.y() * yscale + canvasRect.y() - m_cursorBackground.height()
			);

			painter.drawPixmap(viewPoint, m_cursorBackground);
			painter.setRenderHint(QPainter::SmoothPixmapTransform);
			painter.drawPixmap(
				QRect(
					viewPoint + QPoint(
						m_cursorBackground.width()/2 - cursor.avatar.width()/4,
						m_cursorBackground.width()/2 - cursor.avatar.height()/4
					),
					cursor.avatar.size() / 2
				),
				cursor.avatar
			);
		}
	}
}

void NavigatorView::onCursorMove(unsigned int flags, uint8_t userId, uint16_t layer, int x, int y)
{
	Q_UNUSED(layer);

	if(!m_showCursors || !(flags & DP_USER_CURSOR_FLAG_VALID))
		return;

	// Never show the local user's cursor in the navigator
	if(userId == m_model->localUserId())
		return;

	for(UserCursor &uc : m_cursors) {
		if(uc.id == userId) {
			uc.pos = QPoint(x, y);
			uc.lastMoved = QDateTime::currentMSecsSinceEpoch();
			return;
		}
	}

	const canvas::User user = m_model->userlist()->getUserById(userId);
	m_cursors << UserCursor {
		user.avatar,
		QPoint(x, y),
		QDateTime::currentMSecsSinceEpoch(),
		userId
	};
}

QPointF NavigatorView::getFocusPoint(const QPointF &eventPoint)
{
	QSize s = m_cache.size().scaled(size(), Qt::KeepAspectRatio);
	QSize canvasSize = m_model->size();
	qreal xscale = s.width() / qreal(canvasSize.width());
	qreal yscale = s.height() / qreal(canvasSize.height());
	QPoint offset { width()/2 - s.width()/2, height()/2 - s.height()/2 };
	return QPointF{
		(eventPoint.x() - offset.x()) / xscale,
		(eventPoint.y() - offset.y()) / yscale
	};
}

/**
 * Construct the navigator dock widget.
 */
Navigator::Navigator(QWidget *parent)
	: DockBase(tr("Navigator"), parent), m_updating(false)
{
	setObjectName("navigatordock");

	auto *titlebar = new TitleWidget(this);
	setTitleBarWidget(titlebar);

	m_view = new NavigatorView(this);
	m_view->setFocusPolicy(Qt::ClickFocus);
	setWidget(m_view);

	m_resetZoomButton = new widgets::GroupedToolButton{widgets::GroupedToolButton::NotGrouped};
	m_resetZoomButton->setIcon(QIcon::fromTheme("zoom-original"));
	titlebar->addCustomWidget(m_resetZoomButton);

	m_zoomSlider = new KisDoubleSliderSpinBox{this};
	m_zoomSlider->setMinimumWidth(0);
	m_zoomSlider->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum));
	m_zoomSlider->setMinimum(zoomMin * 100.0);
	m_zoomSlider->setMaximum(zoomMax * 100.0);
	m_zoomSlider->setExponentRatio(4.0);
	m_zoomSlider->setValue(100.0);
	m_zoomSlider->setSuffix("%");
	m_zoomSlider->setFocusPolicy(Qt::ClickFocus);
	titlebar->addCustomWidget(m_zoomSlider, true);

	connect(m_view, &NavigatorView::focusMoved, this, &Navigator::focusMoved);
	connect(m_view, &NavigatorView::wheelZoom, this, &Navigator::wheelZoom);
	connect(m_resetZoomButton, &widgets::GroupedToolButton::clicked, this, [this] { emit zoomChanged(1.0); });
	connect(m_zoomSlider, QOverload<double>::of(&KisDoubleSliderSpinBox::valueChanged), this, &Navigator::updateZoom);

	QAction *showCursorsAction = new QAction(tr("Show Cursors"), m_view);
	showCursorsAction->setCheckable(true);
	m_view->addAction(showCursorsAction);

	QAction *realtimeUpdateAction = new QAction(tr("Realtime Update"), m_view);
	realtimeUpdateAction->setCheckable(true);
	m_view->addAction(realtimeUpdateAction);

	m_view->setContextMenuPolicy(Qt::ActionsContextMenu);

	auto &settings = dpApp().settings();
	settings.bindNavigatorShowCursors(showCursorsAction);
	settings.bindNavigatorShowCursors(m_view, &NavigatorView::setShowCursors);

	settings.bindNavigatorRealtime(realtimeUpdateAction);
	settings.bindNavigatorRealtime(m_view, &NavigatorView::setRealtimeUpdate);
}

Navigator::~Navigator()
{
}

void Navigator::setCanvasModel(canvas::CanvasModel *model)
{
	m_view->setCanvasModel(model);
}

void Navigator::setViewFocus(const QPolygonF& rect)
{
	m_view->setViewFocus(rect);
}

void Navigator::updateZoom(double value)
{
	if(!m_updating) {
		emit zoomChanged(value / 100.0);
	}
}

void Navigator::setViewTransformation(qreal zoom, qreal angle)
{
	Q_UNUSED(angle)
	m_updating = true;
	m_zoomSlider->setValue(zoom * 100.0);
	m_updating = false;
}

}

