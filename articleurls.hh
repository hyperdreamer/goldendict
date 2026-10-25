/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * The single URL dispatch matrix of the article view, expressed as values and
 * pure functions.
 *
 * Everything declared here is independent of widgets, of the dictionary
 * engines and of the application object: the article scheme handler, the URL
 * producer used by the view and the navigation/history bookkeeping all agree on
 * one parse result instead of re-deriving flags from raw query strings.
 */

#ifndef GOLDENDICT_ARTICLEURLS_HH_INCLUDED
#define GOLDENDICT_ARTICLEURLS_HH_INCLUDED

#include <QByteArray>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "config.hh"

/// The single navigation vocabulary of the article page and of its
/// application-owned history. Defined here so that the page and the history can
/// share it without a circular header dependency.
enum class NavigationKind
{
  NewLookup,
  LinkClick,
  Back,
  Forward,
  Reload,
  Redirect
};

/// One full-text-search highlight request.
///
/// `sourcePattern` is the pattern exactly as the user wrote it; `expression` is
/// that pattern compiled with `wildcards` applied through
/// QRegularExpression::NonPathWildcardConversion and with `matchCase` folded
/// into the pattern options. Both the original syntax and the compiled PCRE are
/// kept because a QRegularExpression cannot remember wildcard syntax, and a
/// consumer that still has to transform the source (diacritics folding) must
/// rebuild the expression from `sourcePattern` instead of from `expression`.
struct FtsHighlightSpec
{
  QString sourcePattern;
  QRegularExpression expression;
  bool wildcards = false;
  bool matchCase = false;
};

namespace ArticleUrls
{

/// What the caller is about to do with the URL. A document load is what the
/// engine is asked to display; a user action is a clicked link that the
/// application dispatches itself, with the reference view's context.
enum class Use
{
  DocumentLoad,
  UserAction
};

/// What the URL denotes.
///
/// LookupDocument/PictureDocument are documents served by the scheme handler,
/// Resource/AudioAction/VideoAction are dictionary resources, LookupAction is a
/// lookup the application performs on the user's behalf, and ExternalAction is
/// everything the engine or the desktop handles itself.
enum class Kind
{
  LookupDocument,
  PictureDocument,
  Resource,
  LookupAction,
  AudioAction,
  VideoAction,
  ExternalAction,
  Invalid
};

/// The lookup request carried by a `gdlookup`/`bword` URL.
struct Lookup
{
  bool blank = false;
  Config::InputPhrase phrase;
  unsigned group = 0;
  bool groupSpecified = false;
  QStringList dictionaries, muted;
  QMap<QString, QString> contexts;
  bool ignoreDiacritics = false;
  QString scrollTo, anchor;
  QString dictionaryName; // decoded `dict` target, retained for UserAction dispatch
  FtsHighlightSpec highlight;
};

/// A parse result. `error` is non-empty exactly when `kind` is Invalid.
struct Parsed
{
  Kind kind = Kind::Invalid;
  QUrl url;
  Lookup lookup;
  QString dictionaryId, resourceName, error;
};

/// Classifies `url` and validates its wire form for the requested use.
///
/// Syntax and wire validation happen here; a UserAction result deliberately
/// keeps fields the reference view still has to resolve (an absent group, the
/// `dict` target, the `gdanchor` override) so that the action handler, and not
/// this function, decides them. Parser failures are reported through
/// Parsed::error/Parsed::kind; nothing throws.
Parsed parse( QUrl const & url, Use use );

/// Produces the validated document URL of `lookup`.
///
/// The result always names `localhost` and always carries a group for an
/// ordinary lookup, so it re-parses as Use::DocumentLoad. `dictionaryName` is
/// not emitted: `dict` is an action-only field that the action handler consumes
/// before the document URL is built, and a document request never passes it to
/// the article maker. Every field is decoded on the way in and is
/// percent-encoded exactly once on the way out, so a value that itself contains
/// a valid `%HH` sequence keeps its level and re-parses unchanged.
QUrl lookupDocument( Lookup const & lookup );

/// Produces the picture document URL of a dictionary resource.
///
/// `resourceName` is the decoded resource name (path, plus `?` and the decoded
/// query when the resource carries one), exactly the value ArticleUrls::parse
/// reports for a resource scheme. The name's structural slash is already gone,
/// so a leading slash in it is resource data and only one structural slash is
/// added. A `?` is kept even when the query suffix behind it is empty.
QUrl pictureDocument( QString const & dictionaryId, QString const & resourceName );

/// Serializes the reference-view contexts for a URL query item.
///
/// The payload is a QMap<QString,QString> written with QDataStream version
/// Qt_5_0 and then Base64-encoded. The version is pinned: it is an ephemeral
/// URL payload whose producer and consumer must keep the same wire format.
QByteArray encodeContexts( QMap<QString, QString> const & contexts );

/// Deserializes what encodeContexts() produced.
///
/// Strict by construction: invalid Base64, a missing pad character, non-zero
/// unused padding bits, a truncated or failing stream and trailing bytes all
/// make the decode fail (the canonical padded Base64 of the decoded bytes has to
/// reproduce the input). `out` is cleared first and is left empty on failure, so
/// a caller never applies half a map. `error` receives a human-readable reason;
/// both output pointers may be null.
bool decodeContexts( QByteArray const & base64, QMap<QString, QString> * out,
                     QString * error );

/// The dictionary resource name encoded in a resource URL: the decoded path
/// with exactly one structural leading slash removed, followed by `?` and the
/// decoded query when the URL has one. The query suffix is part of the resource
/// name, and the fragment is not.
QString resourcePath( QUrl const & url );

} // namespace ArticleUrls

#endif
