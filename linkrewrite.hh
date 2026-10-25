#ifndef GOLDENDICT_LINKREWRITE_HH_INCLUDED
#define GOLDENDICT_LINKREWRITE_HH_INCLUDED

#include <QString>

/// Common link rewriting rules for the internal URL schemes an article uses.
///
/// The rules used to be duplicated, with subtly different patterns, in the
/// dictionary engines. They live here once and are pinned by
/// tests/fixtures/linkrewrite/cases.json; an engine keeps only its
/// format-specific normalization (StarDict `bword:`/`#anchor`, DICT brace
/// references, Zim title localization, inline-website URL resolution).
///
/// Every rule scans one immutable source, edits only URL value spans (so tag
/// markup, attribute quotes, surrounding whitespace and unmatched bytes are
/// preserved), never rescans what it emitted, leaves absolute targets (a QUrl
/// scheme, a `//` reference or a fragment) intact, and is idempotent.
namespace LinkRewrite
{

/// Normalizes the path headword of existing `gdlookup://localhost/TARGET`
/// links: markup/entity/whitespace cleanup, one percent-decoding pass and one
/// UTF-8 percent-encoding pass applied to the headword only. The query items
/// (`dict`, `gdanchor`, ...) and the fragment keep their bytes, so the action
/// handler still receives the structured target, and a second application is a
/// no-op. It does not interpret dialect syntax such as DICT braces.
QString applyGdLookup( QString const & html );

/// Prefixes relative `img`/`script` `src`, `link` `href` and CSS `url(...)`
/// targets with `bres://DICT/`, retaining the relative spelling. Absolute,
/// protocol-relative, fragment, data, qrcx and already-qualified application
/// URLs are untouched; a root-relative `/path` means the dictionary root.
QString applyBres( QString const & html, QString const & dictId );

/// Qualifies an explicitly unqualified icon target (`gico:/PATH` or
/// `gico:///PATH`) as `gico://DICT/PATH`. An already qualified icon is
/// untouched, and a plain image is never turned into one.
QString applyGico( QString const & html, QString const & dictId );

/// Rewrites a relative `<audio src=...>BODY</audio>` into the existing
/// `gdAudioLinks` script plus the playsound wrapper with `gdau://DICT/NAME`,
/// preserving BODY and adding the play icon only when BODY has no `<img >` tag.
/// Absolute, protocol-relative and already qualified audio is untouched.
QString applyGdau( QString const & html, QString const & dictId );

} // namespace LinkRewrite

#endif // GOLDENDICT_LINKREWRITE_HH_INCLUDED
