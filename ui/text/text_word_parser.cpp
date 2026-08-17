// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/text/text_word_parser.h"

#include "ui/text/text_bidi_algorithm.h"
#include "styles/style_basic.h"

// COPIED FROM qtextlayout.cpp AND MODIFIED
namespace Ui::Text {

void WordParser::LineBreakHelper::savePreviousOffset() {
	previousOffset = (currentPosition > itemPosition)
		? (currentPosition - itemPosition)
		: -1;
}

void WordParser::LineBreakHelper::calculateRightBearing() {
	rightBearing = (shaped && currentPosition > itemPosition && !whiteSpaceOrObject)
		? shaped->rightBearingBefore(currentPosition - itemPosition)
		: Fixed();
}

void WordParser::LineBreakHelper::calculateRightBearingForPreviousGlyph() {
	rightBearing = (shaped && previousOffset > 0)
		? shaped->rightBearingBefore(previousOffset)
		: Fixed();
}

Fixed WordParser::LineBreakHelper::negativeRightBearing() const {
	//if (rightBearing == RightBearingNotCalculated)
	//	return Fixed(0);

	return abs(rightBearing);
}

void WordParser::addNextCluster(
		int &pos,
		int end,
		ScriptLine &line,
		int &glyphCount) {
	const auto base = _lbh.itemPosition;
	const auto from = pos - base;
	auto till = from + 1;
	while ((base + till < end) && !_lbh.shaped->clusterStart(till)) {
		++till;
	}
	line.length += (till - from);
	line.textWidth += _lbh.shaped->xAt(till) - _lbh.shaped->xAt(from);
	pos = base + till;
	++glyphCount;
}

WordParser::BidiInitedAnalysis::BidiInitedAnalysis(not_null<String*> text)
: list(text->_text.size()) {
	BidiAlgorithm bidi(
		text->_text.constData(),
		list.data(),
		text->_text.size(),
		false, // baseDirectionIsRtl
		begin(text->_blocks),
		end(text->_blocks),
		0); // offsetInBlocks
	bidi.process();
}

WordParser::WordParser(not_null<String*> string)
: _t(string)
, _tText(_t->_text)
, _tBlocks(_t->_blocks)
, _tWords(_t->_words)
, _analysis(_t)
, _engine(_t, _analysis.list)
, _shaper(&_engine) {
	parse();
}

void WordParser::parse() {
	_tWords.clear();
	if (_tText.isEmpty()) {
		return;
	}
	_newItem = _engine.itemIndexAt(0);
	_attributes = ComputeCharAttributes(_tText);

	while (_newItem < int(_engine.items().size())) {
		if (_newItem != _item) {
			moveToNewItem();
		}
		const auto &current = _engine.items()[_item];
		const auto atSpaceBreak = [&] {
			if (!clusterIsWhitespace(_lbh.currentPosition)) {
				return false;
			}
			for (auto index = _lbh.currentPosition; index < _itemEnd; ++index) {
				if (!_attributes[index].whiteSpace) {
					return false;
				} else if (isSpaceBreak(index)) {
					return true;
				}
			}
			return false;
		}();
		if (current.analysis.flags == ScriptAnalysis::LineOrParagraphSeparator) {
			pushAccumulatedWord();
			processSingleGlyphItem();
			pushNewline(_wordStart, _engine.blockIndex(_wordStart));
			wordProcessed(_itemEnd);
		} else if (current.analysis.flags == ScriptAnalysis::Object) {
			pushAccumulatedWord();
			processSingleGlyphItem(current.width);
			_lbh.calculateRightBearing();
			pushFinishedWord(
				_wordStart,
				_lbh.tmpData.textWidth,
				-_lbh.negativeRightBearing());
			wordProcessed(_itemEnd);
		} else if (atSpaceBreak) {
			pushAccumulatedWord();
			accumulateWhitespaces();
			ensureWordForRightPadding();
			_tWords.back().add_rpadding(_lbh.spaceData.textWidth);
			wordProcessed(_lbh.currentPosition, true);
		} else {
			_lbh.whiteSpaceOrObject = false;
			do {
				addNextCluster(
					_lbh.currentPosition,
					_itemEnd,
					_lbh.tmpData,
					_lbh.glyphCount);

				if (_lbh.currentPosition >= _tText.size()
					|| isSpaceBreak(_lbh.currentPosition)
					|| isLineBreak(_lbh.currentPosition)) {
					maybeStartUnfinishedWord();
					_lbh.calculateRightBearing();
					pushFinishedWord(
						_wordStart,
						_lbh.tmpData.textWidth,
						-_lbh.negativeRightBearing());
					wordProcessed(_lbh.currentPosition);
					break;
				} else if (_attributes[_lbh.currentPosition].graphemeBoundary) {
					maybeStartUnfinishedWord();
					if (_addingEachGrapheme) {
						_lbh.calculateRightBearing();
						pushUnfinishedWord(
							_wordStart,
							_lbh.tmpData.textWidth,
							-_lbh.negativeRightBearing());
						wordContinued(_lbh.currentPosition);
					} else {
						_lastGraphemeBoundaryPosition = _lbh.currentPosition;
						_lastGraphemeBoundaryLine = _lbh.tmpData;
						_lbh.savePreviousOffset();
					}
				}
			} while (_lbh.currentPosition < _itemEnd);
		}
		if (_lbh.currentPosition == _itemEnd)
			_newItem = _item + 1;
	}
	if (!_tWords.empty()) {
		_tWords.shrink_to_fit();
	}
}

void WordParser::moveToNewItem() {
	_item = _newItem;
	const auto &item = _engine.items()[_item];
	_lbh.currentPosition = item.position;
	_itemEnd = item.position + item.length;
	_lbh.shaped = &_shaper.shape(item);
	_lbh.itemPosition = item.position;
	_lbh.previousOffset = -1;
}

void WordParser::pushAccumulatedWord() {
	if (_wordStart < _lbh.currentPosition) {
		_lbh.calculateRightBearing();
		pushFinishedWord(
			_wordStart,
			_lbh.tmpData.textWidth,
			-_lbh.negativeRightBearing());
		wordProcessed(_lbh.currentPosition);
	}
}

void WordParser::processSingleGlyphItem(Fixed added) {
	_lbh.whiteSpaceOrObject = true;
	++_lbh.tmpData.length;
	_lbh.tmpData.textWidth += added;

	_newItem = _item + 1;
	++_lbh.glyphCount;
}

void WordParser::wordProcessed(int nextWordStart, bool spaces) {
	wordContinued(nextWordStart, spaces);
	_addingEachGrapheme = false;
	_lastGraphemeBoundaryPosition = -1;
	_lastGraphemeBoundaryLine = ScriptLine();
}

void WordParser::wordContinued(int nextPartStart, bool spaces) {
	if (spaces) {
		_lbh.spaceData.textWidth = 0;
		_lbh.spaceData.length = 0;
	} else {
		_lbh.tmpData.textWidth = 0;
		_lbh.tmpData.length = 0;
	}
	_wordStart = nextPartStart;
}

void WordParser::accumulateWhitespaces() {
	_lbh.whiteSpaceOrObject = true;
	while (_lbh.currentPosition < _itemEnd
		&& _attributes[_lbh.currentPosition].whiteSpace
		&& clusterIsWhitespace(_lbh.currentPosition))
		addNextCluster(
			_lbh.currentPosition,
			_itemEnd,
			_lbh.spaceData,
			_lbh.glyphCount);
}

void WordParser::ensureWordForRightPadding() {
	if (_tWords.empty()) {
		_lbh.calculateRightBearing();
		pushFinishedWord(
			_wordStart,
			_lbh.tmpData.textWidth,
			-_lbh.negativeRightBearing());
	}
}

void WordParser::maybeStartUnfinishedWord() {
	if (!_addingEachGrapheme && _lbh.tmpData.textWidth > _t->_minResizeWidth) {
		if (_lastGraphemeBoundaryPosition >= 0) {
			_lbh.calculateRightBearingForPreviousGlyph();
			pushUnfinishedWord(
				_wordStart,
				_lastGraphemeBoundaryLine.textWidth,
				-_lbh.negativeRightBearing());
			_lbh.tmpData.textWidth -= _lastGraphemeBoundaryLine.textWidth;
			_lbh.tmpData.length -= _lastGraphemeBoundaryLine.length;
			_wordStart = _lastGraphemeBoundaryPosition;
		}
		_addingEachGrapheme = true;
	}
}

void WordParser::pushFinishedWord(
		uint16 position,
		Fixed width,
		Fixed rbearing) {
	const auto unfinished = false;
	_tWords.push_back(Word(position, unfinished, width, rbearing));
}

void WordParser::pushUnfinishedWord(
		uint16 position,
		Fixed width,
		Fixed rbearing) {
	const auto unfinished = true;
	_tWords.push_back(Word(position, unfinished, width, rbearing));
}

void WordParser::pushNewline(uint16 position, int newlineBlockIndex) {
	_tWords.push_back(Word(position, newlineBlockIndex));
}

bool WordParser::isLineBreak(int index) const {
	// Don't break by '/' or '.' in the middle of the word.
	// In case of a line break or white space it'll allow break anyway.
	return _attributes[index].lineBreak
		&& (index <= 0
			|| (_tText[index - 1] != '/' && _tText[index - 1] != '.'));
}

bool WordParser::isSpaceBreak(int index) const {
	// Don't break on &nbsp;
	return _attributes[index].whiteSpace && (_tText[index] != QChar::Nbsp);
}

bool WordParser::clusterIsWhitespace(int index) const {
	// A mark with no letter to sit on is shaped onto the space before it, and
	// the two come out as one cluster, which can not be cut in half. Such a
	// cluster carries ink of its own, so it belongs to a word and not to the
	// padding a run of spaces makes: padding is dropped at the end of a line,
	// and the width the text reports would be short of what it draws.
	for (auto i = index; i != _itemEnd; ++i) {
		if (i != index && _lbh.shaped->clusterStart(i - _lbh.itemPosition)) {
			break;
		} else if (!_attributes[i].whiteSpace) {
			return false;
		}
	}
	return true;
}

} // namespace Ui::Text
