// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/text/text_shaper.h"

#include "ui/text/text_block.h"
#include "ui/text/text_stack_engine.h"

#include <QtGui/QFontMetrics>
#include <QtGui/QGlyphRun>
#include <QtGui/QPainter>
#include <QtGui/QRawFont>
#include <QtCore/QTextBoundaryFinder>
#include <QtCore/QtMath>

#include <list>
#include <unordered_map>

namespace Ui::Text {

// Everything a ShapedItem points at. Entries are held by unique_ptr so the
// addresses handed out stay valid while the cache evicts around them.
struct ShapeEntry {
	QTextLayout layout;
	QTextLine line;
	std::vector<Fixed> x;
	std::vector<char> clusterStart;

	// The glyph each character is drawn with, filled on the first bearing the
	// line breaker asks for. Empty when the item is one this cannot answer for.
	std::vector<QRawFont> fonts;
	std::vector<quint32> glyphAt;
	std::vector<quint8> fontAt;
	bool glyphsTried = false;
	bool rtl = false;
	bool simpleScript = false;
	qreal origin = 0.;
	qreal underlineOffset = 0.;
	qreal ascent = 0.;
	qreal descent = 0.;
	qreal lineThickness = 0.;
	bool underline = false;
	bool overline = false;
	bool strikeOut = false;
};

namespace {

// Wide enough that a single line always holds the whole item, but still far
// inside what Fixed can hold: setLineWidth() converts to 26.6 fixed point, so
// anything above INT_MAX/64 silently overflows and the line comes out empty.
constexpr auto kUnboundedWidth = qreal(kQFixedMax);

// A screenful of messages is a few dozen items, the rest is head room for
// scrolling. Entries are a couple of KB each, so this stays in the low
// single-digit megabytes at worst.
constexpr auto kShapeCacheLimit = 512;

// Keyed by the QFont itself rather than by the style::font it came from: a
// FontData address can be reused after a style reload, and the resolved font
// is what the shaping actually depends on anyway.
struct ShapeKey {
	QString text;
	QFont font;
	bool rtl = false;

	friend bool operator==(const ShapeKey &a, const ShapeKey &b) {
		return (a.rtl == b.rtl)
			&& (a.text == b.text)
			&& (a.font == b.font);
	}
};

struct ShapeKeyHash {
	[[nodiscard]] size_t operator()(const ShapeKey &key) const {
		return qHash(key.text)
			^ (qHash(key.font) << 1)
			^ (key.rtl ? 0x9e3779b9 : 0);
	}
};

class ShapeCache final {
public:
	[[nodiscard]] ShapeEntry *find(const ShapeKey &key);
	ShapeEntry *add(ShapeKey key, std::unique_ptr<ShapeEntry> entry);
	void clear();

private:
	using Entry = std::pair<ShapeKey, std::unique_ptr<ShapeEntry>>;
	using List = std::list<Entry>;

	List _list; // Most recently used first.
	std::unordered_map<ShapeKey, List::iterator, ShapeKeyHash> _map;

};

ShapeEntry *ShapeCache::find(const ShapeKey &key) {
	const auto i = _map.find(key);
	if (i == end(_map)) {
		return nullptr;
	}
	_list.splice(begin(_list), _list, i->second);
	return i->second->second.get();
}

ShapeEntry *ShapeCache::add(ShapeKey key, std::unique_ptr<ShapeEntry> entry) {
	const auto result = entry.get();
	_list.push_front(Entry(key, std::move(entry)));
	_map.emplace(std::move(key), begin(_list));

	// Eviction takes from the back, so the entry just handed out survives:
	// the caller keeps pointing at it for the rest of the item it paints.
	while (int(_list.size()) > kShapeCacheLimit) {
		_map.erase(_list.back().first);
		_list.pop_back();
	}
	return result;
}

void ShapeCache::clear() {
	_map.clear();
	_list.clear();
}

[[nodiscard]] ShapeCache &Cache() {
	static auto result = ShapeCache();
	return result;
}

// The bearing before a character comes from the glyph its cluster is drawn
// with, and a long unbreakable word asks for one per grapheme - each one a call
// to the line for a single character's worth of runs, which costs about what
// the shaping did. Glyphs come out in logical order, so when an item shaped one
// per cluster the k-th glyph belongs to the k-th cluster and a single pass
// answers all of them. Left to the first question, because an item that never
// has to break never asks.
[[nodiscard]] quint8 AddFont(
		not_null<ShapeEntry*> entry,
		const QRawFont &raw) {
	for (auto i = 0, count = int(entry->fonts.size()); i != count; ++i) {
		if (entry->fonts[i] == raw) {
			return quint8(i);
		}
	}
	entry->fonts.push_back(raw);
	return quint8(entry->fonts.size() - 1);
}

void FillGlyphs(not_null<ShapeEntry*> entry) {
	entry->glyphsTried = true;
	const auto line = entry->line;
	if (!line.isValid()) {
		return;
	}
	const auto length = int(entry->clusterStart.size());

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)

	// Qt fills the string indexes by walking the item's clusters forward and
	// only steps when a cluster's glyph is the one it is looking at, so a run
	// that does not start at the item's first glyph leaves every glyph with
	// the item's first character. Right to left is that case in full - its
	// clusters count down the glyph array, so the walk never starts - and is
	// left with the same lookups it had before. Left to right only breaks
	// where one item is split between fonts, and those runs then all claim
	// the same characters, which is what is checked below.
	if (entry->rtl) {
		return;
	}
	auto claimed = std::vector<char>(length, 0);
	auto indexed = std::vector<std::pair<int, std::pair<quint32, quint8>>>();
	indexed.reserve(length);
	auto ordered = true;
	auto broken = false;
	auto run = 0;
	for (const auto &one : line.glyphRuns(
			0,
			length,
			QTextLayout::RetrieveGlyphIndexes
				| QTextLayout::RetrieveStringIndexes)) {
		++run;
		const auto indexes = one.glyphIndexes();
		const auto strings = one.stringIndexes();
		if (indexes.size() != strings.size()) {
			broken = true;
			break;
		}
		const auto font = AddFont(entry, one.rawFont());
		for (auto i = 0; i != indexes.size(); ++i) {
			const auto at = int(strings[i]);
			if (at < 0 || at >= length) {
				broken = true;
				break;
			} else if (claimed[at] && claimed[at] != run) {
				broken = true;
				break;
			}
			claimed[at] = char(run);
			if (!indexed.empty() && indexed.back().first > at) {
				ordered = false;
			}
			indexed.push_back({ at, { indexes[i], font } });
		}
		if (broken) {
			break;
		}
	}
	if (broken || indexed.empty()) {
		entry->fonts.clear();
		return;
	}
	if (!ordered) {
		// Glyphs come in visual order and a matra is drawn before the
		// consonant it follows in the text, so this puts them back in the
		// order the characters are in.
		ranges::stable_sort(indexed, ranges::less(), [](const auto &value) {
			return value.first;
		});
	}
	entry->glyphAt.assign(length, 0);
	entry->fontAt.assign(length, 0);
	for (auto i = 0, count = int(indexed.size()); i != count;) {
		auto last = i;
		while (last + 1 < count
			&& indexed[last + 1].first == indexed[i].first) {
			++last;
		}
		const auto from = indexed[i].first;
		const auto till = (last + 1 < count)
			? indexed[last + 1].first
			: length;
		for (auto c = from; c != till; ++c) {
			entry->glyphAt[c] = indexed[last].second.first;
			entry->fontAt[c] = indexed[last].second.second;
		}
		i = last + 1;
	}

#else // Qt >= 6.10.0

	// Without a string index per glyph a count is the only thing to go by:
	// when the item shaped one glyph per cluster, the k-th glyph is the k-th
	// cluster.
	if (entry->rtl || !entry->simpleScript) {
		return;
	}
	auto glyphs = std::vector<std::pair<quint32, quint8>>();
	for (const auto &one : line.glyphRuns(0, length)) {
		const auto font = AddFont(entry, one.rawFont());
		for (const auto index : one.glyphIndexes()) {
			glyphs.push_back({ index, font });
		}
	}
	auto clusters = 0;
	for (auto i = 0; i != length; ++i) {
		if (entry->clusterStart[i]) {
			++clusters;
		}
	}
	if (clusters != int(glyphs.size())) {
		// A ligature or a combining mark: the k-th glyph is not the k-th
		// cluster any more, so every bearing is asked for the old way.
		entry->fonts.clear();
		return;
	}
	entry->glyphAt.assign(length, 0);
	entry->fontAt.assign(length, 0);
	auto cluster = -1;
	for (auto i = 0; i != length; ++i) {
		if (entry->clusterStart[i]) {
			++cluster;
		}
		if (cluster >= 0) {
			entry->glyphAt[i] = glyphs[cluster].first;
			entry->fontAt[i] = glyphs[cluster].second;
		}
	}

#endif // Qt < 6.10.0
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)

// The table for an item whose glyphs do not line up with its characters one to
// one - a ligature, a combining mark, a matra drawn before the consonant it
// follows in the text. Walking it with cursorToX() costs a pass over the line
// per character, while the same numbers are in the run: where a cluster starts,
// its glyph says, and inside one the position is interpolated the way the
// engine does it, by the cluster's own share of that glyph's advance.
[[nodiscard]] bool FillClusteredX(
		not_null<ShapeEntry*> result,
		const QTextLine &line,
		const QGlyphRun &run,
		qreal origin,
		const QString &text) {
	const auto positions = run.positions();
	const auto strings = run.stringIndexes();
	const auto count = int(positions.size());
	const auto length = int(text.size());
	if (!count || count != int(strings.size())) {
		return false;
	}

	// Glyphs come in visual order, so a cluster's characters are recovered by
	// sorting: the smallest glyph of a cluster is the one it starts at, which
	// is what the engine's own map holds and takes the advance from.
	auto clusters = std::vector<std::pair<int, int>>();
	clusters.reserve(count);
	for (auto i = 0; i != count; ++i) {
		const auto at = int(strings[i]);
		if (at < 0 || at >= length) {
			return false;
		}
		clusters.push_back({ at, i });
	}
	ranges::stable_sort(clusters);
	clusters.erase(
		ranges::unique(clusters, ranges::equal_to(), &std::pair<int, int>::first),
		clusters.end());
	if (clusters.front().first != 0) {
		return false;
	}

	// Not naturalTextWidth(): that one leaves out trailing spaces.
	auto last = length;
	const auto width = Fixed::FromReal(
		std::abs(line.cursorToX(&last, QTextLine::Leading) - origin));
	const auto at = [&](int glyph) {
		return (glyph < count)
			? Fixed::FromReal(std::abs(positions[glyph].x() - origin))
			: width;
	};
	for (auto i = 0, total = int(clusters.size()); i != total; ++i) {
		const auto [from, glyph] = clusters[i];
		const auto till = (i + 1 < total) ? clusters[i + 1].first : length;
		const auto base = at(glyph);
		const auto advance = at(glyph + 1) - base;
		const auto cluster = till - from;
		for (auto c = from; c != till; ++c) {
			result->x[c] = base + (advance * (c - from) / cluster);
		}
	}
	result->x[length] = width;

	// A glyph the engine hides - a soft hyphen, a separator - is left out of
	// the positions but still counted in the indexes, and everything past it
	// shifts. Nothing in the run says so, so the line is asked about two of
	// the clusters: two passes over it instead of one per character, and an
	// item it disagrees with is walked the old way.
	for (const auto index : { int(clusters.size()) / 2, int(clusters.size()) - 1 }) {
		auto position = clusters[index].first;
		const auto x = line.cursorToX(&position, QTextLine::Leading);
		if (position != clusters[index].first
			|| Fixed::FromReal(std::abs(x - origin)) != result->x[position]) {
			return false;
		}
	}

	// cursorToX() answers a position that is not a grapheme boundary for the
	// next one that is, so every character in between repeats what follows it.
	auto finder = QTextBoundaryFinder(QTextBoundaryFinder::Grapheme, text);
	auto boundaries = std::vector<char>(length, 0);
	for (auto position = finder.position()
		; position >= 0
		; position = finder.toNextBoundary()) {
		if (position < length) {
			boundaries[position] = 1;
		}
	}
	for (auto i = length - 1; i >= 0; --i) {
		if (boundaries[i]) {
			result->clusterStart[i] = 1;
		} else {
			result->x[i] = result->x[i + 1];
		}
	}
	return true;
}

#endif // Qt >= 6.10.0

[[nodiscard]] std::unique_ptr<ShapeEntry> Build(
		const QString &text,
		const style::font &font,
		bool rtl,
		bool simple) {
	auto result = std::make_unique<ShapeEntry>();
	const auto length = int(text.size());
	result->x.assign(length + 1, Fixed());
	result->clusterStart.assign(length, 0);

	result->underline = font->f.underline();
	result->overline = font->f.overline();
	result->strikeOut = font->f.strikeOut();
	if (result->underline || result->overline || result->strikeOut) {
		const auto metrics = QFontMetricsF(font->f);
		result->underlineOffset = metrics.underlinePos();
		result->ascent = metrics.ascent();
		result->descent = metrics.descent();
		result->lineThickness = metrics.lineWidth();
	}

	result->rtl = rtl;
	result->simpleScript = simple;
	result->layout.setText(text);
	result->layout.setFont(font->f);
	auto option = QTextOption();
	option.setTextDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
	option.setWrapMode(QTextOption::NoWrap);
	result->layout.setTextOption(option);

	result->layout.beginLayout();
	auto line = result->layout.createLine();
	if (line.isValid()) {
		line.setLineWidth(kUnboundedWidth);
		line.setPosition(QPointF());
	}
	result->layout.endLayout();
	result->line = line;
	if (!line.isValid()) {
		return result;
	}

	// cursorToX() snaps its argument to the nearest valid cursor position and
	// reports it back, which recovers the cluster boundaries without any
	// private array. Distances are taken from the item start so that xAt()
	// stays a logical-order width in both directions.
	// The line must cover the whole item: with NoWrap it normally does, but a
	// separator inside the text ends it early, and walking past its end would
	// index out of the engine's arrays.
	const auto covered = std::min(int(line.textLength()), length);
	if (covered < length) {
		qWarning("Ui::Text::Shaper: line covers %d of %d characters.",
			covered,
			length);
	}
	auto zero = 0;
	const auto origin = line.cursorToX(&zero, QTextLine::Leading);
	result->origin = origin;

	// cursorToX() walks the line from its start on every call, so filling the
	// table with it costs more than the shaping did. Where the item came out as
	// one run of one glyph per character - which is most text that is not
	// Arabic, Indic or ligated - the same numbers are the run's own positions,
	// which is also what the glyphs are drawn at.
	if (!rtl && covered == length) {
		const auto runs = line.glyphRuns(
			0,
			length,
			QTextLayout::RetrieveGlyphPositions
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
				| QTextLayout::RetrieveStringIndexes
#endif // Qt >= 6.10.0
			);
		if (runs.size() == 1) {
			const auto &run = runs.front();
			const auto positions = run.positions();

			// A glyph for every character is not a glyph *per* character: a
			// matra takes one of them and is drawn before the consonant it
			// follows, so the counts can agree while the order does not. Only
			// the scripts that never reorder can be read off position by
			// position; the rest are mapped below or asked one by one.
			if (simple && positions.size() == length) {
				for (auto i = 0; i != length; ++i) {
					result->x[i] = Fixed::FromReal(
						std::abs(positions[i].x() - origin));
					result->clusterStart[i] = 1;
				}
				// Not naturalTextWidth(): it leaves out trailing spaces.
				auto last = length;
				const auto width = line.cursorToX(&last, QTextLine::Leading);
				result->x[length] = Fixed::FromReal(std::abs(width - origin));
				return result;
			}
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
			if (FillClusteredX(result.get(), line, run, origin, text)) {
				return result;
			}
#endif // Qt >= 6.10.0
		}
	}
	for (auto i = 0; i <= covered; ++i) {
		auto snapped = i;
		const auto x = line.cursorToX(&snapped, QTextLine::Leading);
		result->x[i] = Fixed::FromReal(std::abs(x - origin));
		if (i < length && snapped == i) {
			result->clusterStart[i] = 1;
		}
	}
	for (auto i = covered + 1; i <= length; ++i) {
		result->x[i] = result->x[covered];
		if (i < length) {
			result->clusterStart[i] = 1;
		}
	}
	return result;
}

} // namespace

Fixed ShapedItem::width() const {
	return _length ? (*_x)[_length] : Fixed();
}

Fixed ShapedItem::xAt(int charOffset) const {
	Expects(charOffset >= 0 && charOffset <= _length);

	return (*_x)[charOffset];
}

int ShapedItem::charAtX(Fixed x) const {
	auto result = 0;
	for (auto i = 1; i <= _length; ++i) {
		if (!clusterStart(i) && i != _length) {
			continue;
		} else if ((*_x)[i] > x) {
			break;
		}
		result = i;
	}
	return result;
}

bool ShapedItem::clusterStart(int charOffset) const {
	Expects(charOffset >= 0 && charOffset <= _length);

	return (charOffset == _length) || (*_clusterStart)[charOffset];
}

Fixed ShapedItem::rightBearingBefore(int charOffset) const {
	if (charOffset <= 0 || !_line.isValid()) {
		return Fixed();
	}
	const auto at = charOffset - 1;
	if (_entry && !_entry->glyphsTried) {
		FillGlyphs(const_cast<ShapeEntry*>(_entry));
	}
	const auto known = _entry
		&& !_entry->fonts.empty()
		&& (at < int(_entry->glyphAt.size()));

	// The glyph just before the offset: from the table when the item filled
	// one, and as one character's worth of runs when it did not.
	const auto runs = known ? QList<QGlyphRun>() : _line.glyphRuns(at, 1);
	if (!known && runs.isEmpty()) {
		return Fixed();
	}
	const auto indexes = known ? QList<quint32>() : runs.back().glyphIndexes();
	if (!known && indexes.isEmpty()) {
		return Fixed();
	}
	const auto raw = known
		? _entry->fonts[_entry->fontAt[at]]
		: runs.back().rawFont();
	const auto glyph = known ? _entry->glyphAt[at] : indexes.back();
	auto advance = QPointF();
	if (!raw.advancesForGlyphIndexes(&glyph, &advance, 1)) {
		return Fixed();
	}
	// QFontEngine::getGlyphBearings defines the right bearing as
	// advance - leftBearing - inkWidth, that is the slack after the ink, so
	// it is positive when the glyph fits and negative when it overflows.
	// boundingRect() gives the ink box, hence the subtraction this way round.
	//
	// Floored to a whole pixel, which is what the private path got for free:
	// its advance came from the hinted glyph metrics, so any overhang at all
	// counted as a pixel. Rounding the other way would let a line take one
	// more word than it has room for, and the ink would leave the widget.
	const auto bearing = advance.x() - raw.boundingRect(glyph).right();
	return Fixed(qFloor(std::min(bearing, 0.)));
}

void ShapedItem::draw(
		QPainter &p,
		QPointF at,
		int fromOffset,
		int tillOffset) const {
	if (!_line.isValid() || fromOffset >= tillOffset) {
		return;
	}
	// glyphRuns() places the subset relative to the whole line, so the origin
	// moves back to where the requested range starts *visually* - which is the
	// far end of the range in an RTL run. It also bakes the baseline into the
	// glyph positions, while `at` already is the baseline, so the ascent has
	// to come back out or the text lands a line too low.
	const auto lineX = [&](int offset) {
		const auto distance = xAt(offset).toReal();
		return _rtl ? (_origin - distance) : (_origin + distance);
	};
	const auto shift = QPointF(
		std::min(lineX(fromOffset), lineX(tillOffset)),
		_line.ascent());
	const auto runs = _line.glyphRuns(fromOffset, tillOffset - fromOffset);
	for (auto run : runs) {
		// glyphRuns() sets these from the font, and QPainter would then draw
		// its own decoration on top of the one drawn below.
		run.setUnderline(false);
		run.setOverline(false);
		run.setStrikeOut(false);
		p.drawGlyphRun(at - shift, run);
	}
	if (_underline || _overline || _strikeOut) {
		// The geometry QPainter uses for the decoration it draws itself,
		// including the pixel our own qtbase patch shifts the start by, so this
		// keeps the look even where that patch is absent. Delegating is not an
		// option anyway: since Qt 6 QPainter takes the decoration's left edge
		// from the position argument, which the line-relative positions coming
		// out of QTextLine::glyphRuns() do not match.
		const auto width = (xAt(tillOffset) - xAt(fromOffset)).toReal();
		const auto from = qFloor(at.x() + 1);
		const auto till = qFloor(at.x() + width);
		const auto thickness = std::max(_lineThickness, 1.);
		const auto line = [&](qreal y) {
			p.fillRect(
				QRectF(from, y, till - from, thickness),
				p.pen().brush());
		};
		if (_underline) {
			line(at.y() + _underlineOffset);
		}
		if (_overline) {
			line(at.y() - _ascent);
		}
		if (_strikeOut) {
			line(at.y() - _ascent / 3.);
		}
	}
}

Shaper::Shaper(not_null<StackEngine*> engine)
: _engine(engine) {
}

Shaper::~Shaper() = default;

void Shaper::ClearCache() {
	Cache().clear();
}

const ShapedItem &Shaper::shape(const Item &item) {
	return shape(item, _engine->itemFont(item));
}

const ShapedItem &Shaper::shape(const Item &item, const style::font &font) {
	const auto length = item.length;

	Expects(length > 0);

	_shaped._length = length;
	_shaped._rtl = item.rtl();
	_shaped._origin = 0.;
	_shaped._underline = false;
	_shaped._overline = false;
	_shaped._strikeOut = false;

	if (item.object()) {
		// Objects are never shaped: the block owns their width.
		_objectX.assign(length + 1, Fixed());
		_objectClusterStart.assign(length, 0);
		_objectClusterStart[0] = 1;
		_objectX[length] = item.width;
		_shaped._x = &_objectX;
		_shaped._clusterStart = &_objectClusterStart;
		_shaped._entry = nullptr;
		_shaped._line = QTextLine();
		return _shaped;
	}

	// Latin and the like shape one glyph per cluster, which is what lets the
	// bearings be read off a single pass; the scripts that join their letters
	// or write with combining marks never pay for the attempt.
	const auto script = item.analysis.script;
	const auto simple = (script == QChar::Script_Latin)
		|| (script == QChar::Script_Cyrillic)
		|| (script == QChar::Script_Greek)
		|| (script == QChar::Script_Common);

	auto key = ShapeKey{
		_engine->text().mid(item.position, length),
		font->f,
		item.rtl(),
	};
	auto entry = Cache().find(key);
	if (!entry) {
		auto built = Build(key.text, font, key.rtl, simple);
		entry = Cache().add(std::move(key), std::move(built));
	}
	_shaped._x = &entry->x;
	_shaped._clusterStart = &entry->clusterStart;
	_shaped._entry = entry;
	_shaped._line = entry->line;
	_shaped._origin = entry->origin;
	_shaped._underline = entry->underline;
	_shaped._overline = entry->overline;
	_shaped._strikeOut = entry->strikeOut;
	_shaped._underlineOffset = entry->underlineOffset;
	_shaped._ascent = entry->ascent;
	_shaped._descent = entry->descent;
	_shaped._lineThickness = entry->lineThickness;
	return _shaped;
}

} // namespace Ui::Text
