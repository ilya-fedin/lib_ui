// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <QtCore/QChar>

namespace Ui::Text {

// Per-character itemization state, laid out exactly like Qt's private
// QScriptAnalysis so that the two can be converted field by field for free.
//
// It exists so that the bidi algorithm and the word parser - which produce and
// consume this state - do not need <private/qtextengine_p.h>. Only the code
// that actually feeds QTextEngine converts to the Qt type.
struct ScriptAnalysis {
	enum Flags {
		None = 0,
		Lowercase = 1,
		Uppercase = 2,
		SmallCaps = 3,
		LineOrParagraphSeparator = 4,
		Space = 5,
		SpaceTabOrObject = Space,
		Nbsp = 6,
		Tab = 7,
		TabOrObject = Tab,
		Object = 8,
	};
	enum BidiFlags {
		BidiBN = 1,
		BidiMaybeResetToParagraphLevel = 2,
		BidiResetToParagraphLevel = 4,
		BidiMirrored = 8,
	};

	unsigned short script : 8;
	unsigned short flags : 4;
	unsigned short bidiFlags : 4;
	unsigned short bidiLevel : 8; // Unicode bidi embedding level (0-125).
	QChar::Direction bidiDirection : 8; // Used while running the algorithm.

	// Same comparison as QScriptAnalysis: itemization splits on these three.
	[[nodiscard]] friend bool operator==(
			const ScriptAnalysis &a,
			const ScriptAnalysis &b) {
		return (a.script == b.script)
			&& (a.bidiLevel == b.bidiLevel)
			&& (a.flags == b.flags);
	}
};

} // namespace Ui::Text
