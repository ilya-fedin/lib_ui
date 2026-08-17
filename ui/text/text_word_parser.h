// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "ui/text/text.h"
#include "ui/text/text_char_attributes.h"
#include "ui/text/text_shaper.h"
#include "ui/text/text_script_analysis.h"
#include "ui/text/text_block.h"
#include "ui/text/text_stack_engine.h"
#include "ui/text/text_word.h"

namespace Ui::Text {

class WordParser {
public:
	explicit WordParser(not_null<String*> string);

private:
	struct ScriptLine {
		int length = 0;
		Fixed textWidth;
	};
	struct LineBreakHelper {
		ScriptLine tmpData;
		ScriptLine spaceData;

		// The shaped item currently being walked and where it starts in the
		// whole text, so that absolute positions convert to item offsets.
		const ShapedItem *shaped = nullptr;
		int itemPosition = 0;

		int glyphCount = 0;
		int maxGlyphs = INT_MAX;
		int currentPosition = 0;

		// Offset saved at the last grapheme boundary, to compute the bearing
		// of a word that gets rolled back to it.
		int previousOffset = -1;

		Fixed rightBearing;

		bool whiteSpaceOrObject = true;

		void savePreviousOffset();
		void calculateRightBearing();
		void calculateRightBearingForPreviousGlyph();

		// We always calculate the right bearing right before it is needed.
		// So we don't need caching / optimizations referred to
		// delayed right bearing calculations.

		//static const Fixed RightBearingNotCalculated;

		//inline void resetRightBearing()
		//{
		//	rightBearing = RightBearingNotCalculated;
		//}

		// We express the negative right bearing as an absolute number
		// so that it can be applied to the width using addition.
		Fixed negativeRightBearing() const;

	};
	struct BidiInitedAnalysis {
		explicit BidiInitedAnalysis(not_null<String*> text);

		QVarLengthArray<ScriptAnalysis, 4096> list;
	};

	void parse();

	void moveToNewItem();

	void pushAccumulatedWord();
	void processSingleGlyphItem(Fixed added = 0);
	void wordProcessed(int nextWordStart, bool spaces = false);
	void wordContinued(int nextPartStart, bool spaces = false);
	void accumulateWhitespaces();
	void ensureWordForRightPadding();
	void maybeStartUnfinishedWord();
	void pushFinishedWord(uint16 position, Fixed width, Fixed rbearing);
	void pushUnfinishedWord(uint16 position, Fixed width, Fixed rbearing);
	void pushNewline(uint16 position, int newlineBlockIndex);

	void addNextCluster(int &pos, int end, ScriptLine &line, int &glyphCount);

	[[nodiscard]] bool isLineBreak(int index) const;
	[[nodiscard]] bool isSpaceBreak(int index) const;
	[[nodiscard]] bool clusterIsWhitespace(int index) const;

	const not_null<String*> _t;
	QString &_tText;
	std::vector<Block> &_tBlocks;
	std::vector<Word> &_tWords;
	BidiInitedAnalysis _analysis;
	StackEngine _engine;
	Shaper _shaper;
	LineBreakHelper _lbh;
	std::vector<CharAttribute> _attributes;
	int _wordStart = 0;
	bool _addingEachGrapheme = false;
	int _lastGraphemeBoundaryPosition = -1;
	ScriptLine _lastGraphemeBoundaryLine;
	int _item = -1;
	int _newItem = -1;
	int _itemEnd = 0;

};

} // namespace Ui::Text
