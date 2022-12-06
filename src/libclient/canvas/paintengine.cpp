/*
   Drawpile - a collaborative drawing program.

   Copyright (C) 2021 Calle Laakkonen

   Drawpile is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Drawpile is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Drawpile.  If not, see <http://www.gnu.org/licenses/>.
*/

extern "C" {
#include <dpengine/layer_routes.h>
#include <dpengine/paint_engine.h>
#include <dpengine/recorder.h>
#include <dpengine/tile.h>
#include <dpmsg/msg_internal.h>
}

#include "paintengine.h"
#include "drawdance/layercontent.h"
#include "drawdance/layerpropslist.h"
#include "drawdance/message.h"

#include <QPainter>
#include <QSet>
#include <QTimer>
#include <QtEndian>

namespace canvas {

/*
// Callback: notify the view that an area on the canvas is in need of refresh.
// Refreshing is lazy: if that area is not visible at the moment, it doesn't
// need to be repainted now.
void paintEngineAreaChanged(void *pe, rustpile::Rectangle area)
{
	emit reinterpret_cast<PaintEngine*>(pe)->areaChanged(QRect(area.x, area.y, area.w, area.h));
}

void paintEngineLayersChanged(void *pe, const rustpile::LayerInfo *layerInfos, uintptr_t count)
{
	QVector<LayerListItem> layers;
	layers.reserve(count);

	for(uintptr_t i=0;i<count;++i) {
		const rustpile::LayerInfo &li = layerInfos[i];
		layers << LayerListItem {
			uint16_t(li.id), // only internal (non-visible) layers have IDs outside the u16 range
			uint16_t(li.frame_id),
			QString::fromUtf8(reinterpret_cast<const char*>(li.title), li.titlelen),
			li.opacity,
			li.blendmode,
			li.hidden,
			li.censored,
			li.isolated,
			li.group,
			li.children,
			li.rel_index,
			li.left,
			li.right
		};
	}

	emit reinterpret_cast<PaintEngine*>(pe)->layersChanged(layers);
}

void paintEngineAnnotationsChanged(void *pe, rustpile::Annotations *annotations)
{
	// Note: rustpile::Annotations is thread safe
	emit reinterpret_cast<PaintEngine*>(pe)->annotationsChanged(annotations);
}

void paintEngineMetadataChanged(void *pe)
{
	emit reinterpret_cast<PaintEngine*>(pe)->metadataChanged();
}

void paintEngineTimelineChanged(void *pe)
{
	emit reinterpret_cast<PaintEngine*>(pe)->timelineChanged();
}

void paintEngineFrameVisbilityChanged(void *pe, const rustpile::Frame *frame, bool frameMode)
{
	QVector<int> layers;
	if(frameMode) {
		for(unsigned long i=0;i<sizeof(rustpile::Frame)/sizeof(rustpile::LayerID);++i) {
			if((*frame)[i] != 0)
				layers << (*frame)[i];
			else
				break;
		}
	}
	emit reinterpret_cast<PaintEngine*>(pe)->frameVisibilityChanged(layers, frameMode);
}

void paintEngineCursors(void *pe, uint8_t user, uint16_t layer, int32_t x, int32_t y)
{
	// This may be emitted from either the main thread or the paint engine thread
	emit reinterpret_cast<PaintEngine*>(pe)->cursorMoved(user, layer, x, y);
}

void paintEnginePlayback(void *pe, int64_t pos, uint32_t interval)
{
	emit reinterpret_cast<PaintEngine*>(pe)->playbackAt(pos, interval);
}
*/

PaintEngine::PaintEngine(QObject *parent)
	: QObject(parent)
	, m_acls{}
	, m_snapshotQueue{5, 10000} // TODO: make these configurable
	, m_paintEngine{m_acls, m_snapshotQueue}
	, m_timerId{0}
	, m_changedTileBounds{}
	, m_lastRefreshAreaTileBounds{}
	, m_lastRefreshAreaTileBoundsTouched{false}
	, m_cache{}
	, m_painter{}
	, m_painterMutex{}
	, m_sampleColorLastDiameter(-1)
{
	start();
}

PaintEngine::~PaintEngine()
{
}

void PaintEngine::start()
{
	// TODO make this configurable
	m_timerId = startTimer(1000 / 60, Qt::PreciseTimer);
}

void PaintEngine::reset(const drawdance::CanvasState &canvasState)
{
	if(m_timerId != 0) {
		killTimer(m_timerId);
	}
	m_paintEngine.reset(m_acls, m_snapshotQueue, canvasState);
	m_cache = QPixmap{};
	m_lastRefreshAreaTileBounds = QRect{};
	m_lastRefreshAreaTileBoundsTouched = false;
	start();
	emit aclsChanged(m_acls, DP_ACL_STATE_CHANGE_MASK);
}

void PaintEngine::timerEvent(QTimerEvent *)
{
	m_changedTileBounds = QRect{};
	DP_paint_engine_tick(
		m_paintEngine.get(), &PaintEngine::onCatchup,
		&PaintEngine::onRecorderStateChanged, &PaintEngine::onResized,
		&PaintEngine::onTileChanged, &PaintEngine::onLayerPropsChanged,
		&PaintEngine::onAnnotationsChanged,
		&PaintEngine::onDocumentMetadataChanged,
		&PaintEngine::onTimelineChanged,
		&PaintEngine::onCursorMoved, this);

	if(m_changedTileBounds.isValid()) {
		QRect changedArea{
			m_changedTileBounds.x() * DP_TILE_SIZE,
			m_changedTileBounds.y() * DP_TILE_SIZE,
			m_changedTileBounds.width() * DP_TILE_SIZE,
			m_changedTileBounds.height() * DP_TILE_SIZE};
		emit areaChanged(changedArea);
	}
}

int PaintEngine::receiveMessages(bool local, int count, const drawdance::Message *msgs)
{
	return DP_paint_engine_handle_inc(m_paintEngine.get(), local, count,
		drawdance::Message::asRawMessages(msgs), &PaintEngine::onAclsChanged,
		&PaintEngine::onLaserTrail, &PaintEngine::onMovePointer,
		&PaintEngine::onDefaultLayer, this);
}

void PaintEngine::enqueueReset()
{
	drawdance::Message msg = drawdance::Message::makeInternalReset(0);
	receiveMessages(false, 1, &msg);
}

void PaintEngine::enqueueLoadBlank(const QSize &size, const QColor &backgroundColor)
{
	drawdance::Message messages[] = {
		drawdance::Message::makeInternalReset(0),
		drawdance::Message::makeCanvasBackground(0, backgroundColor),
		drawdance::Message::makeCanvasResize(0, 0, size.width(), size.height(), 0),
		drawdance::Message::makeLayerCreate(0, 0x100, 0, 0, 0, 0, tr("Layer %1").arg(1)),
		drawdance::Message::makeInternalSnapshot(0),
	};
	receiveMessages(false, DP_ARRAY_LENGTH(messages), messages);
}

void PaintEngine::enqueueCatchupProgress(int progress)
{
	drawdance::Message msg = drawdance::Message::makeInternalCatchup(0, progress);
	receiveMessages(false, 1, &msg);
}

void PaintEngine::resetAcl(uint8_t localUserId)
{
	m_acls.reset(localUserId);
	emit aclsChanged(m_acls, DP_ACL_STATE_CHANGE_MASK);
}

void PaintEngine::cleanup()
{
	drawdance::Message msg = drawdance::Message::makeInternalCleanup(0);
	receiveMessages(false, 1, &msg);
}

QColor PaintEngine::backgroundColor() const
{
	DP_Pixel15 pixel;
	if (canvasState().backgroundTile().samePixel(&pixel)) {
		DP_UPixelFloat color = DP_upixel15_to_float(DP_pixel15_unpremultiply(pixel));
		return QColor::fromRgbF(color.r, color.g, color.b, color.a);
	} else {
		return Qt::transparent;
	}
}

uint16_t PaintEngine::findAvailableAnnotationId(uint8_t forUser) const
{
	QSet<int> usedIds;
	int idMask = forUser << 8;
	drawdance::AnnotationList annotations = canvasState().annotations();
	int count = annotations.count();
	for(int i = 0; i < count; ++i) {
		int id = annotations.at(i).id();
		if((id & 0xff00) == idMask) {
			usedIds.insert(id & 0xff);
		}
	}

	for(int i = 0; i < 256; ++i) {
		if(!usedIds.contains(i)) {
			return idMask | i;
		}
	}

	qWarning("No available annotation id for user %d", forUser);
	return 0;
}

drawdance::Annotation PaintEngine::getAnnotationAt(int x, int y, int expand) const
{
	QPoint point{x, y};
	QMargins margins{expand, expand, expand, expand};

	drawdance::AnnotationList annotations = canvasState().annotations();
	int count = annotations.count();
	int closestIndex = -1;
	int closestDistance = INT_MAX;

	for(int i = 0; i < count; ++i) {
		drawdance::Annotation annotation = annotations.at(i);
		QRect bounds = annotation.bounds().marginsAdded(margins);
		if(bounds.contains(point)) {
			int distance = (point - bounds.center()).manhattanLength();
			if(closestIndex == -1 || distance < closestDistance) {
				closestIndex = i;
				closestDistance = distance;
			}
		}
	}

	return closestIndex == -1 ? drawdance::Annotation::null() : annotations.at(closestIndex);
}

bool PaintEngine::needsOpenRaster() const
{
	drawdance::CanvasState cs = canvasState();
	return cs.backgroundTile().isNull() && cs.layers().count() > 1 && cs.annotations().count() != 0;
}

void PaintEngine::setLocalDrawingInProgress(bool localDrawingInProgress)
{
	m_paintEngine.setLocalDrawingInProgress(localDrawingInProgress);
}

void PaintEngine::setLayerVisibility(int layerId, bool hidden)
{
	m_paintEngine.setLayerVisibility(layerId, hidden);
}

void PaintEngine::setViewMode(DP_LayerViewMode mode, bool censor)
{
	m_paintEngine.setViewMode(mode);
	m_paintEngine.setRevealCensored(!censor);
}

bool PaintEngine::isCensored() const
{
	return m_paintEngine.revealCensored();
}

void PaintEngine::setOnionskinOptions(int skinsBelow, int skinsAbove, bool tint)
{
	// rustpile::paintengine_set_onionskin_opts(m_pe, skinsBelow, skinsAbove, tint);
	qDebug("FIXME Dancepile: %s %d not implemented", __FILE__, __LINE__);
}

void PaintEngine::setViewLayer(int id)
{
	m_paintEngine.setActiveLayerId(id);
}

void PaintEngine::setViewFrame(int frame)
{
	m_paintEngine.setActiveFrameIndex(frame - 1); // 1-based to 0-based index.
}

void PaintEngine::setInspectContextId(unsigned int contextId)
{
	m_paintEngine.setInspectContextId(contextId);
}

QColor PaintEngine::sampleColor(int x, int y, int layerId, int diameter)
{
	drawdance::LayerContent lc = layerId == 0
		? m_paintEngine.renderContent()
		: canvasState().searchLayerContent(layerId);
	if(lc.isNull()) {
		return Qt::transparent;
	} else {
		return lc.sampleColorAt(m_sampleColorStampBuffer, x, y, diameter, m_sampleColorLastDiameter);
	}
}

drawdance::RecordStartResult PaintEngine::startRecording(const QString &path)
{
	return m_paintEngine.startRecorder(path);
}

bool PaintEngine::stopRecording()
{
	return m_paintEngine.stopRecorder();
}

bool PaintEngine::isRecording() const
{
	return m_paintEngine.recorderIsRecording();
}

void PaintEngine::previewCut(int layerId, const QRect &bounds, const QImage &mask)
{
	m_paintEngine.previewCut(layerId, bounds, mask);
}

void PaintEngine::previewDabs(int layerId, const drawdance::MessageList &msgs)
{
	m_paintEngine.previewDabs(layerId, msgs.count(), msgs.constData());
}

void PaintEngine::clearPreview()
{
	m_paintEngine.clearPreview();
}

const QPixmap &PaintEngine::getPixmapView(const QRect &refreshArea)
{
	QRect refreshAreaTileBounds{
		QPoint{refreshArea.left() / DP_TILE_SIZE, refreshArea.top() / DP_TILE_SIZE},
		QPoint{refreshArea.right() / DP_TILE_SIZE, refreshArea.bottom() / DP_TILE_SIZE}};
	if(refreshAreaTileBounds == m_lastRefreshAreaTileBounds) {
		if(m_lastRefreshAreaTileBoundsTouched) {
			renderTileBounds(refreshAreaTileBounds);
			m_lastRefreshAreaTileBoundsTouched = false;
		}
	} else {
		renderTileBounds(refreshAreaTileBounds);
		m_lastRefreshAreaTileBounds = refreshAreaTileBounds;
		m_lastRefreshAreaTileBoundsTouched = false;
	}
	return m_cache;
}

const QPixmap &PaintEngine::getPixmap()
{
	renderEverything();
	m_lastRefreshAreaTileBoundsTouched = false;
	return m_cache;
}

void PaintEngine::renderTileBounds(const QRect &tileBounds)
{
	DP_PaintEngine *pe = m_paintEngine.get();
	DP_paint_engine_prepare_render(pe, &PaintEngine::onRenderSize, this);
	if(!m_cache.isNull() && m_painter.begin(&m_cache)) {
		m_painter.setCompositionMode(QPainter::CompositionMode_Source);
		DP_paint_engine_render_tile_bounds(
			pe, tileBounds.left(), tileBounds.top(), tileBounds.right(),
			tileBounds.bottom(), &PaintEngine::onRenderTile, this);
		m_painter.end();
	}
}

void PaintEngine::renderEverything()
{
	DP_PaintEngine *pe = m_paintEngine.get();
	DP_paint_engine_prepare_render(pe, &PaintEngine::onRenderSize, this);
	if(!m_cache.isNull() && m_painter.begin(&m_cache)) {
		m_painter.setCompositionMode(QPainter::CompositionMode_Source);
		DP_paint_engine_render_everything(pe, &PaintEngine::onRenderTile, this);
		m_painter.end();
	}
}

int PaintEngine::frameCount() const
{
	drawdance::CanvasState cs = canvasState();
	if(cs.documentMetadata().useTimeline()) {
		return cs.timeline().frameCount();
	} else {
		return cs.layers().count();
	}
}

QImage PaintEngine::getLayerImage(int id, const QRect &rect) const
{
	drawdance::CanvasState cs = canvasState();
	QRect area = rect.isNull() ? QRect{0, 0, cs.width(), cs.height()} : rect;
	if (area.isEmpty()) {
		return QImage{};
	}

	if(id <= 0) {
		bool includeBackground = id == 0;
		return cs.toFlatImageArea(area, includeBackground);
	} else {
		return cs.layerToFlatImage(id, area);
	}
}

QImage PaintEngine::getFrameImage(int index, const QRect &rect) const
{
	/*
	rustpile::Rectangle r;
	if(rect.isEmpty()) {
		const auto size = rustpile::paintengine_canvas_size(m_pe);
		r = {0, 0, size.width, size.height};
	} else {
		r = {rect.x(), rect.y(), rect.width(), rect.height()};
	}

	QImage img(r.w, r.h, QImage::Format_ARGB32_Premultiplied);

	if(!rustpile::paintengine_get_frame_content(m_pe, index, r, img.bits())) {
		return QImage();
	}

	return img;
	*/
	qDebug("FIXME Dancepile: %s %d not implemented", __FILE__, __LINE__);
	return QImage{};
}


void PaintEngine::onAclsChanged(void *user, int aclChangeFlags)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->aclsChanged(pe->m_acls, aclChangeFlags);
}

void PaintEngine::onLaserTrail(void *user, unsigned int contextId, int persistence, uint32_t color)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->laserTrail(contextId, persistence, color);
}

void PaintEngine::onMovePointer(void *user, unsigned int contextId, int x, int y)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->cursorMoved(contextId, 0, x, y);
}

void PaintEngine::onDefaultLayer(void *user, int layerId)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->defaultLayer(layerId);
}

void PaintEngine::onCatchup(void *user, int progress)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->caughtUpTo(progress);
}

void PaintEngine::onRecorderStateChanged(void *user, bool started)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->recorderStateChanged(started);
}

void PaintEngine::onResized(void *user, int offsetX, int offsetY, int prevWidth, int prevHeight)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	pe->resized(offsetX, offsetY, QSize{prevWidth, prevHeight});
}

void PaintEngine::onTileChanged(void *user, int x, int y)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	pe->m_changedTileBounds |= QRect{x, y, 1, 1};
	if(!pe->m_lastRefreshAreaTileBoundsTouched && pe->m_lastRefreshAreaTileBounds.contains(x, y)) {
		pe->m_lastRefreshAreaTileBoundsTouched = true;
	}
}

void PaintEngine::onLayerPropsChanged(void *user, DP_LayerPropsList *lpl)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->layersChanged(drawdance::LayerPropsList::inc(lpl));
}

void PaintEngine::onAnnotationsChanged(void *user, DP_AnnotationList *al)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->annotationsChanged(drawdance::AnnotationList::inc(al));
}

void PaintEngine::onDocumentMetadataChanged(void *user, DP_DocumentMetadata *dm)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->documentMetadataChanged(drawdance::DocumentMetadata::inc(dm));
}

void PaintEngine::onTimelineChanged(void *user, DP_Timeline *tl)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->timelineChanged(drawdance::Timeline::inc(tl));
}

void PaintEngine::onCursorMoved(void *user, unsigned int contextId, int layerId, int x, int y)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	emit pe->cursorMoved(contextId, layerId, x, y);
}

void PaintEngine::onRenderSize(void *user, int width, int height)
{
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	QSize size{width, height};
	if(pe->m_cache.size() != size) {
		pe->m_cache = QPixmap{size};
	}
}

void PaintEngine::onRenderTile(void *user, int x, int y, DP_Pixel8 *pixels, int threadIndex)
{
	// My initial idea was to use an array of QPainters to spew pixels into the
	// pixmap in parallel, but Qt doesn't support multiple painters on a single
	// pixmap. So we have to use a single painter and lock its usage instead.
	Q_UNUSED(threadIndex);
	QImage image{
		reinterpret_cast<unsigned char *>(pixels), DP_TILE_SIZE, DP_TILE_SIZE,
		QImage::Format_ARGB32_Premultiplied};
	PaintEngine *pe = static_cast<PaintEngine *>(user);
	QMutexLocker lock{&pe->m_painterMutex};
	pe->m_painter.drawImage(x * DP_TILE_SIZE, y * DP_TILE_SIZE, image);
}


}
