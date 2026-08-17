// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "ui/style/style_core_types.h"
#include "ui/text/text_script_analysis.h"
#include "ui/ui_fixed.h"

#include <QtCore/QPointF>
#include <QtGui/QTextLayout>

#include <memory>
#include <vector>

class QPainter;

namespace Ui::Text {

// One itemized run: a stretch of text with a single font, direction and
// script, or a single object block. Produced by our own itemizer, which never
// asked Qt for it - see StackEngine::itemize().
struct Item {
	int position = 0;
	int length = 0;
	ScriptAnalysis analysis;
	int blockIndex = 0;
	Fixed width; // Object items carry AbstractBlock::objectWidth() here.
	int ascent = 0;
	int descent = 0;

	[[nodiscard]] bool rtl() const {
		return (analysis.bidiLevel % 2) != 0;
	}
	[[nodiscard]] bool object() const {
		return analysis.flags == ScriptAnalysis::Object;
	}
};

class StackEngine;
struct ShapeEntry;

// Geometry is flattened into a per-character table once per item so that xAt()
// is O(1) and the callers keep their linear walks.
//
// Deliberately glyph-free: the callers ask for character offsets, never for
// clusters or glyph indices. That is what lets the implementation below be
// swapped from QTextEngine to QTextLayout without touching them.
class ShapedItem final {
public:
	[[nodiscard]] Fixed width() const;
	[[nodiscard]] Fixed xAt(int charOffset) const;
	[[nodiscard]] int charAtX(Fixed x) const;
	[[nodiscard]] bool clusterStart(int charOffset) const;
	[[nodiscard]] Fixed rightBearingBefore(int charOffset) const;

	void draw(
		QPainter &p,
		QPointF at,
		int fromOffset,
		int tillOffset) const;

private:
	friend class Shaper;

	QTextLine _line;
	int _length = 0;
	bool _underline = false;
	bool _overline = false;
	bool _strikeOut = false;

	// xAt() is a logical-order distance, but glyphRuns() reports visual
	// positions, so drawing needs the line coordinate back: the item start
	// sits on the right in an RTL run, and distances grow leftwards from it.
	qreal _origin = 0.;
	bool _rtl = false;

	// Decorations are drawn by hand rather than through QGlyphRun, which sizes
	// them by the ink bounding box and so stops short of the last advance.
	qreal _underlineOffset = 0.;
	qreal _ascent = 0.;
	qreal _descent = 0.;
	qreal _lineThickness = 0.;

	// _x has _length + 1 entries: the leading edge of every character plus
	// the trailing edge of the item. The buffers live either in the shaping
	// cache or, for object items, in the Shaper itself.
	const std::vector<Fixed> *_x = nullptr;
	const std::vector<char> *_clusterStart = nullptr;
	const ShapeEntry *_entry = nullptr;

};

class Shaper final {
public:
	explicit Shaper(not_null<StackEngine*> engine);
	~Shaper();

	// The font is passed in where the caller knows better than the block
	// flags alone - the renderer applies link underlining, for one.
	[[nodiscard]] const ShapedItem &shape(const Item &item);
	[[nodiscard]] const ShapedItem &shape(
		const Item &item,
		const style::font &font);

	// Painting the same message again asks for exactly the same items, so
	// shaped items are kept in a bounded cache instead of being rebuilt every
	// frame. Only what is drawn ends up there, which bounds it to the visible
	// set; the key does not involve the width, so a resize keeps its entries.
	static void ClearCache();

private:
	const not_null<StackEngine*> _engine;
	ShapedItem _shaped;

	// Object items are not shaped and so are not worth caching: their width
	// comes from the block. These two hold the trivial table for them.
	std::vector<Fixed> _objectX;
	std::vector<char> _objectClusterStart;

};

} // namespace Ui::Text
