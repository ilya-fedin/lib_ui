// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "ui/text/text.h"
#include "ui/text/text_script_analysis.h"
#include "ui/text/text_shaper.h"

namespace Ui::Text {

class StackEngine final {
public:
	explicit StackEngine(
		not_null<const String*> t,
		gsl::span<ScriptAnalysis> analysis,
		int from = 0,
		int till = -1,
		int blockIndexHint = 0);
	explicit StackEngine(
		not_null<const String*> t,
		int offset,
		const QString &text,
		gsl::span<ScriptAnalysis> analysis,
		int blockIndexHint = 0,
		int blockIndexLimit = -1);

	void itemize();
	std::vector<Block>::const_iterator shapeGetBlock(int item);
	[[nodiscard]] int blockIndex(int position) const;

	// Our own itemization result - the only one there is now.
	[[nodiscard]] const std::vector<Item> &items() const {
		return _items;
	}
	[[nodiscard]] int itemIndexAt(int position) const;
	[[nodiscard]] Item &itemForUpdate(int index) {
		return _items[index];
	}

	// What a QTextLayout-based shaper needs instead of QTextEngine.
	[[nodiscard]] const QString &text() const {
		return _text;
	}
	[[nodiscard]] style::font itemFont(const Item &item) const;

private:
	void appendItem(int from, int till, ScriptAnalysis analysis);
	void updateFont(not_null<const AbstractBlock*> block);
	[[nodiscard]] std::vector<Block>::const_iterator adjustBlock(
		int offset) const;
	[[nodiscard]] int blockPosition(
		std::vector<Block>::const_iterator i) const;
	[[nodiscard]] int blockEnd(std::vector<Block>::const_iterator i) const;

	const not_null<const String*> _t;
	// By value: the delegating constructor passes a conditional expression,
	// which materializes a temporary even in the branch that selects a string
	// already owned by the String, so a reference would dangle the moment the
	// constructor returns. The copy is cheap - it is either shared with the
	// String or a fromRawData() view into it.
	const QString _text;
	ScriptAnalysis *_analysis = nullptr;
	const int _offset = 0;
	const int _positionEnd = 0;
	style::font _font;

	const std::vector<Block> &_tBlocks;
	std::vector<Item> _items;

	std::vector<Block>::const_iterator _bStart;
	std::vector<Block>::const_iterator _bEnd;
	mutable std::vector<Block>::const_iterator _bCached;

};

} // namespace Ui::Text
