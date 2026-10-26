/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Hotkey text conversions (§4.11, SD-15). The application continues to store the
 * existing QKeySequence PortableText strings; the XDG Desktop Portal
 * GlobalShortcuts interface wants its own `preferred_trigger` format. These are
 * pure functions usable from any thread and they never rewrite the stored text:
 * a sequence that cannot be represented is reported as unsupported with a reason
 * and the caller keeps the user's value byte-for-byte.
 */

#ifndef HOTKEYSYNTAX_HH_INCLUDED
#define HOTKEYSYNTAX_HH_INCLUDED

#include <QString>

#include "config.hh"

namespace HotkeySyntax {

/// Parses stored PortableText into a Config::HotKey using QKeySequence's count
/// and indexed QKeyCombination accessors -- never a comma split and never the
/// removed Qt5 bitmask on the indexed return type. The existing one- and
/// two-key combinations are accepted; the first combination carries the
/// modifiers, as the stored value always has.
///
/// Returns false for empty text, unparseable text, a key QKeySequence cannot
/// name, or a sequence longer than two combinations, leaving *out untouched and
/// setting *error to the reason. `out` and `error` may be null.
bool parse( QString const & stored, Config::HotKey * out, QString * error );

/// Converts one Config::HotKey into the portal's preferred_trigger format:
/// canonical modifier order CTRL, ALT, SHIFT, LOGO joined with '+', followed by
/// the case-sensitive X keysym name. A bindable value is exactly one ordinary
/// non-modifier key with only Ctrl/Alt/Shift/Meta modifiers. A two-key
/// combination, a modifier-only value, a keypad or other unsupported modifier and
/// a key without a specified X keysym all return false with a reason, leaving
/// *trigger untouched. NUM is never produced.
bool preferredTrigger( Config::HotKey const & key, QString * trigger, QString * error );

/// PortableText of the same combination, for a value newly produced by the
/// recorder. Writing such a value clears the stored raw-text marker, so the new
/// PortableText -- not the obsolete original -- is what a later save writes.
QString serialize( Config::HotKey const & key );

} // namespace HotkeySyntax

#endif // HOTKEYSYNTAX_HH_INCLUDED
