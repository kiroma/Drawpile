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

#include "paintengine.h"
#include "../../rustpile/rustpile.h"

#include <QPainter>

namespace {
static const int TILE_SIZE = 64; // FIXME

static void updateCacheTile(void *p, int x, int y, const uchar *pixels)
{
	((QPainter*)p)->drawImage(
			x,
			y,
			QImage(pixels,
				TILE_SIZE, TILE_SIZE,
				QImage::Format_ARGB32_Premultiplied
			)
	);
}

}

namespace canvas {

// Callback: notify the view that an area on the canvas is in need of refresh.
// Refreshing is lazy: if that area is not visible at the moment, it doesn't
// need to be repainted now.
void paintEngineAreaChanged(void *pe, rustpile::Rectangle area)
{
	emit reinterpret_cast<PaintEngine*>(pe)->areaChanged(QRect(area.x, area.y, area.w, area.h));
}

// Callback: notify the view that the canvas size has changed.
void paintEngineResized(void *pe, int xoffset, int yoffset, rustpile::Size oldSize)
{
	emit reinterpret_cast<PaintEngine*>(pe)->resized(xoffset, yoffset, QSize{oldSize.width, oldSize.height});
}

void paintEngineLayersChanged(void *pe, const rustpile::LayerInfo *layerInfos, uintptr_t count)
{
	QVector<LayerListItem> layers;
	layers.reserve(count);

	// Note: in the layer list model, layers are listed topmost first,
	// whereas in the actual layerstack, they're bottom-most first so we need
	// to swap the order here.
	for(auto i=count;i>0;--i) {
		const rustpile::LayerInfo &li = layerInfos[i-1];
		layers << LayerListItem {
			uint16_t(li.id), // only internal (non-visible) layers have IDs outside the u16 range
			QString::fromUtf8(reinterpret_cast<const char*>(li.title), li.titlelen),
			li.opacity,
			paintcore::BlendMode::Mode(li.blendmode),
			li.hidden,
			li.censored,
			li.fixed
		};
	}

	emit reinterpret_cast<PaintEngine*>(pe)->layersChanged(layers);
}

void paintEngineAnnotationsChanged(void *pe, rustpile::Annotations *annotations)
{
	// Note: rustpile::Annotations is thread safe
	emit reinterpret_cast<PaintEngine*>(pe)->annotationsChanged(annotations);
}

PaintEngine::PaintEngine(QObject *parent)
	: QObject(parent), m_pe(nullptr)
{
	reset();
}

PaintEngine::~PaintEngine()
{
	rustpile::paintengine_free(m_pe);
}

void PaintEngine::reset()
{
	rustpile::paintengine_free(m_pe);
	m_pe = rustpile::paintengine_new(
		this,
		paintEngineAreaChanged,
		paintEngineResized,
		paintEngineLayersChanged,
		paintEngineAnnotationsChanged
	);

	m_cache = QPixmap();
}

void PaintEngine::receiveMessages(bool local, const net::Envelope &msgs)
{
	rustpile::paintengine_receive_messages(m_pe, local, msgs.data(), msgs.length());
}

void PaintEngine::cleanup()
{
	rustpile::paintengine_cleanup(m_pe);
}

QColor PaintEngine::backgroundColor() const
{
	const auto c = rustpile::paintengine_background_color(m_pe);
	return QColor::fromRgbF(c.r, c.g, c.b, c.a);
}

uint16_t PaintEngine::findAvailableAnnotationId(uint8_t forUser) const
{
	return rustpile::paintengine_get_available_annotation_id(m_pe, forUser);
}

rustpile::AnnotationAt PaintEngine::getAnnotationAt(int x, int y, int expand) const
{
	return rustpile::paintengine_get_annotation_at(m_pe, x, y, expand);
}

const QPixmap& PaintEngine::getPixmap(const QRect &refreshArea)
{
	const auto size = this->size();
	if(!size.isValid())
		return m_cache;

	if(m_cache.isNull() || m_cache.size() != size) {
		m_cache = QPixmap(size);
		m_cache.fill();
	}

	const rustpile::Rectangle r {
		refreshArea.x(),
		refreshArea.y(),
		refreshArea.width(),
		refreshArea.height()
	};

	QPainter painter(&m_cache);
	painter.setCompositionMode(QPainter::CompositionMode_Source);

	rustpile::paintengine_paint_changes(m_pe, &painter, r, &updateCacheTile);

	return m_cache;
}

QSize PaintEngine::size() const
{
	const auto size = rustpile::paintengine_canvas_size(m_pe);
	return QSize(size.width, size.height);
}

}
