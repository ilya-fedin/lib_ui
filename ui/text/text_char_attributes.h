// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <QtCore/QString>

#include <vector>

namespace Ui::Text {

// The three per-character bits the word parser needs out of the eight that
// Qt's private QCharAttributes carries.
struct CharAttribute {
	bool lineBreak = false;
	bool whiteSpace = false;
	bool graphemeBoundary = false;
};

// Reproduces QUnicodeTools::initCharAttributes for those three bits using
// public API only. QTextBoundaryFinder is a thin wrapper over the very same
// initCharAttributes - its isAtBoundary() returns attributes[pos].lineBreak
// and .graphemeBoundary directly - and whiteSpace is literally QChar::isSpace
// per code point (qunicodetools.cpp, getWhiteSpaces).
//
// The result has size() + 1 entries: position size() is a valid line break,
// which is what Qt does as well (LB3).
[[nodiscard]] std::vector<CharAttribute> ComputeCharAttributes(
	const QString &text);

} // namespace Ui::Text
