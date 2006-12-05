/*
   DrawPile - a collaborative drawing program.

   Copyright (C) 2006 Calle Laakkonen

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#ifndef INTERFACES_H
#define INTERFACES_H

#include <QColor>
#include "brush.h"

//! Interface classes
//
namespace interface {

//! Interface for brush sources
class BrushSource {
	public:
		virtual ~BrushSource() {}
		virtual drawingboard::Brush getBrush(const QColor& foreground,
				const QColor& background) const = 0;
};

//! Interface for color sources
class ColorSource {
	public:
		virtual ~ColorSource() {}

		//! Get the foreground color
		virtual QColor foreground() const = 0;

		//! Get the background color
		virtual QColor background() const = 0;

		//! Set foreground color
        virtual void setForeground(const QColor &c) = 0;

        //! Set background color
        virtual void setBackground(const QColor &c) = 0;

};

class Global {
	public:
		static void setBrushSource(BrushSource *src) { brush_ = src; }
		static void setColorSource(ColorSource *src) { color_ = src; }

		static BrushSource *brushSource() { return brush_; }
		static ColorSource *colorSource() { return color_; }
	private:
		static BrushSource *brush_;
		static ColorSource *color_;
};

}

#endif

