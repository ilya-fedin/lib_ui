// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/text/text_stack_engine.h"

#include "ui/text/text_block.h"
#include "styles/style_basic.h"

namespace Ui::Text {
namespace {

// Shaping an item costs more than linear in its length, so a long message is
// much cheaper as many short items than as few long ones - hence a cap far
// below the 4096 the engine used to carry. Past the soft cap an item is only
// cut in front of a space, where a cursive script joins nothing anyway; the
// hard cap catches the scripts that write without spaces at all.
constexpr auto kMaxItemLength = 64;
constexpr auto kMaxItemLengthHard = 1024;

// Ported from QUnicodeTools::initScripts, which is what the engine used to
// call. A character of the Unknown, Inherited or Common script keeps the run it
// is in, so punctuation and spaces belong to the text around them, and a
// combining mark always follows its base character whatever its own script
// says. Anything less - QChar::script() taken per character - would move the
// item boundaries.
void FillScripts(QStringView text, ScriptAnalysis *analysis) {
	const auto size = int(text.size());
	const auto fill = [&](int from, int till, QChar::Script script) {
		for (auto i = from; i != till; ++i) {
			analysis[i].script = script;
		}
	};
	auto start = 0;
	auto script = QChar::Script_Common;
	auto i = 0;
	while (i != size) {
		const auto at = i;
		auto ucs4 = char32_t(text[i].unicode());
		if (text[i].isHighSurrogate()
			&& (i + 1 != size)
			&& text[i + 1].isLowSurrogate()) {
			ucs4 = QChar::surrogateToUcs4(text[i], text[i + 1]);
			i += 2;
		} else {
			++i;
		}
		const auto next = QChar::script(ucs4);
		if (next == script || next <= QChar::Script_Common) {
			continue;
		} else if (script <= QChar::Script_Common) {
			// Also covers a Common base character followed by combining marks
			// that are neither Inherited nor Common.
			script = next;
			continue;
		}
		const auto category = QChar::category(ucs4);
		if (category == QChar::Mark_NonSpacing
			|| category == QChar::Mark_SpacingCombining
			|| category == QChar::Mark_Enclosing) {
			continue;
		}
		fill(start, at, script);
		start = at;
		script = next;
	}
	fill(start, size, script);
}

} // namespace

StackEngine::StackEngine(
	not_null<const String*> t,
	gsl::span<ScriptAnalysis> analysis,
	int from,
	int till,
	int blockIndexHint)
: StackEngine(
	t,
	from,
	((from > 0 || (till >= 0 && till < t->_text.size()))
		? QString::fromRawData(
			t->_text.constData() + from,
			((till < 0) ? int(t->_text.size()) : till) - from)
		: t->_text),
	analysis,
	blockIndexHint) {
}

StackEngine::StackEngine(
	not_null<const String*> t,
	int offset,
	const QString &text,
	gsl::span<ScriptAnalysis> analysis,
	int blockIndexHint,
	int blockIndexLimit)
: _t(t)
, _text(text)
, _analysis(analysis.data())
, _offset(offset)
, _positionEnd(_offset + _text.size())
, _font(_t->_st->font)
, _tBlocks(_t->_blocks)
, _bStart(begin(_tBlocks) + blockIndexHint)
, _bEnd((blockIndexLimit >= 0)
	? (begin(_tBlocks) + blockIndexLimit)
	: end(_tBlocks))
, _bCached(_bStart) {
	Expects(analysis.size() >= _text.size());

	itemize();
}

std::vector<Block>::const_iterator StackEngine::adjustBlock(
		int offset) const {
	Expects(offset < _positionEnd);

	if (blockPosition(_bCached) > offset) {
		_bCached = begin(_tBlocks);
	}
	Assert(_bCached != end(_tBlocks));
	for (auto i = _bCached + 1; blockPosition(i) <= offset; ++i) {
		_bCached = i;
	}
	return _bCached;
}

int StackEngine::blockPosition(std::vector<Block>::const_iterator i) const {
	return (i == _bEnd) ? _positionEnd : (*i)->position();
}

int StackEngine::blockEnd(std::vector<Block>::const_iterator i) const {
	return (i == _bEnd) ? _positionEnd : blockPosition(i + 1);
}

void StackEngine::itemize() {
	if (!_items.empty()) {
		return;
	}

	const auto length = int(_text.size());
	if (!length) {
		return;
	}

	_bStart = adjustBlock(_offset);
	const auto chars = _text.constData();

	FillScripts(_text, _analysis);

	// Override script and flags for object-like blocks.
	const auto end = _offset + length;
	for (auto block = _bStart; blockPosition(block) < end; ++block) {
		const auto type = (*block)->type();
		const auto from = std::max(_offset, int(blockPosition(block)));
		const auto till = std::min(int(end), int(blockEnd(block)));
		if (till > from) {
			if (type == TextBlockType::Emoji
				|| type == TextBlockType::CustomEmoji
				|| type == TextBlockType::Skip) {
				for (auto i = from - _offset, count = till - _offset; i != count; ++i) {
					_analysis[i].script = QChar::Script_Common;
					const auto emojiTrailingSpace =
						(type == TextBlockType::Emoji)
						&& (chars[i] == QChar::Space);
					_analysis[i].flags = emojiTrailingSpace
						? ScriptAnalysis::None
						: ScriptAnalysis::Object;
				}
			} else {
				for (auto i = from - _offset, count = till - _offset; i != count; ++i) {
					if (chars[i] == QChar::LineFeed) {
						_analysis[i].flags = ScriptAnalysis::LineOrParagraphSeparator;
					} else {
						_analysis[i].flags = ScriptAnalysis::None;
					}
				}
			}
		}
	}

	{
		auto &m_string = _text;
		auto m_analysis = _analysis;

		auto start = 0;
		auto startBlock = _bStart;
		auto currentBlock = startBlock;
		auto nextBlock = currentBlock + 1;
		for (int i = 1; i != length; ++i) {
			while (blockPosition(nextBlock) <= _offset + i) {
				currentBlock = nextBlock++;
			}
			// According to the unicode spec we should be treating characters in the Common script
			// (punctuation, spaces, etc) as being the same script as the surrounding text for the
			// purpose of splitting up text. This is important because, for example, a fullstop
			// (0x2E) can be used to indicate an abbreviation and so must be treated as part of a
			// word.  Thus it must be passed along with the word in languages that have to calculate
			// word breaks. For example the thai word "[lookup-in-git]." has no word breaks
			// but the word "[lookup-too]" does.
			// Unfortuntely because we split up the strings for both wordwrapping and for setting
			// the font and because Japanese and Chinese are also aliases of the script "Common",
			// doing this would break too many things.  So instead we only pass the full stop
			// along, and nothing else.
			if (currentBlock != startBlock
				|| m_analysis[i].flags != m_analysis[start].flags) {
				// In emoji blocks we can have one item or two items.
				// First item is the emoji itself,
				// while the second item are the spaces after the emoji,
				// which fall in the same block, but have different flags.
			} else if ((*startBlock)->type() != TextBlockType::Text
				&& m_analysis[i].flags == m_analysis[start].flags) {
				// A non-text block always maps to a single item and can't
				// be split into kMaxItemLength chunks like text is: each
				// Object item paints its block and advances by objectWidth,
				// so an additional item would draw and count it twice.
				//
				// Unlimited length is fine here: Object items are never
				// really shaped (QTextEngine::shape() just reserves a
				// single glyph for them), and the space tail of an emoji
				// block shapes to one glyph per character, so the 16-bit
				// per-item glyph counts can't overflow with the 32k text
				// length limit. A several-thousand-characters Object item
				// is real, e.g. a formula custom emoji covers the whole
				// formula source in a rich message summary text.
				continue;
			} else if (m_analysis[i].bidiLevel == m_analysis[start].bidiLevel
				&& m_analysis[i].flags == m_analysis[start].flags
				&& (m_analysis[i].script == m_analysis[start].script || m_string[i] == u'.')
				//&& m_analysis[i].flags < ScriptAnalysis::SpaceTabOrObject // only emojis are objects here, no tabs
				//
				// Past the length limit the item is only cut in front of a
				// space. Shaping costs more than linear in the item length, so
				// short items are worth having, but a cut inside a word would
				// separate letters that a cursive script joins - while a
				// letter before a space takes its final form either way, so a
				// cut there changes nothing. Scripts that write without spaces
				// would never find that point, hence the hard limit as well;
				// none of them are cursive, and no word runs that long.
				&& (i - start < kMaxItemLength
					|| (!m_string[i].isSpace()
						&& i - start < kMaxItemLengthHard))) {
				continue;
			}
			appendItem(start, i, m_analysis[start]);
			start = i;
			startBlock = currentBlock;
		}
		appendItem(start, length, m_analysis[start]);
	}
}

void StackEngine::appendItem(int from, int till, ScriptAnalysis analysis) {
	auto item = Item();
	// Positions are relative to this engine's string, exactly like
	// QScriptItem::position - the renderer builds an engine per line.
	item.position = from;
	item.length = till - from;
	item.analysis = analysis;
	item.blockIndex = blockIndex(from);
	if (analysis.flags == ScriptAnalysis::Object) {
		// Objects are never shaped, their width is the block's own.
		item.width = Fixed((*adjustBlock(_offset + from))->objectWidth());
	}
	_items.push_back(item);
}

style::font StackEngine::itemFont(const Item &item) const {
	return WithFlags(_t->_st->font, _tBlocks[item.blockIndex]->flags());
}

int StackEngine::itemIndexAt(int position) const {
	for (auto i = 0, count = int(_items.size()); i != count; ++i) {
		const auto &item = _items[i];
		if (position >= item.position
			&& position < item.position + item.length) {
			return i;
		}
	}
	return -1;
}

void StackEngine::updateFont(not_null<const AbstractBlock*> block) {
	const auto flags = block->flags();
	const auto newFont = WithFlags(_t->_st->font, block->flags());
	if (_font != newFont) {
		_font = (newFont->family() == _t->_st->font->family())
			? WithFlags(_t->_st->font, flags, newFont->flags())
			: newFont;
	}
}

std::vector<Block>::const_iterator StackEngine::shapeGetBlock(int item) {
	const auto &si = _items[item];
	const auto blockIt = adjustBlock(_offset + si.position);
	const auto block = blockIt->get();
	updateFont(block);
	return blockIt;
}

int StackEngine::blockIndex(int position) const {
	return int(adjustBlock(_offset + position) - begin(_tBlocks));
}

} // namespace Ui::Text
