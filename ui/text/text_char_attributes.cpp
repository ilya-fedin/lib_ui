// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/text/text_char_attributes.h"

#include <QtCore/QTextBoundaryFinder>

namespace Ui::Text {

std::vector<CharAttribute> ComputeCharAttributes(const QString &text) {
	const auto count = int(text.size());
	auto result = std::vector<CharAttribute>(count + 1);
	if (!count) {
		result[0].lineBreak = true; // LB3.
		return result;
	}

	{
		auto finder = QTextBoundaryFinder(QTextBoundaryFinder::Line, text);
		for (auto position = finder.toNextBoundary()
			; position >= 0
			; position = finder.toNextBoundary()) {
			result[position].lineBreak = true;
		}
		// LB2 and LB3, the way Qt sets them after running the algorithm.
		result[0].lineBreak = false;
		result[count].lineBreak = true;
	}
	{
		auto finder = QTextBoundaryFinder(QTextBoundaryFinder::Grapheme, text);
		for (auto position = finder.toNextBoundary()
			; position >= 0
			; position = finder.toNextBoundary()) {
			result[position].graphemeBoundary = true;
		}
		result[0].graphemeBoundary = true;
	}

	// getWhiteSpaces(): QChar::isSpace() per code point, marking the unit the
	// code point starts at - so a space outside the BMP marks its high
	// surrogate only.
	for (auto i = 0; i != count; ++i) {
		auto code = char32_t(text[i].unicode());
		if (QChar::isHighSurrogate(code)
			&& (i + 1 != count)
			&& text[i + 1].isLowSurrogate()) {
			code = QChar::surrogateToUcs4(
				ushort(code),
				text[i + 1].unicode());
			if (QChar::isSpace(code)) {
				result[i].whiteSpace = true;
			}
			++i;
			continue;
		}
		if (QChar::isSpace(code)) {
			result[i].whiteSpace = true;
		}
	}
	return result;
}

} // namespace Ui::Text
