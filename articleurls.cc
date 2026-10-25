/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * The single URL dispatch matrix of the article view.
 *
 * Everything here is pure: no logging, no widget, no dictionary engine and no
 * application object. A malformed URL is reported through Parsed::error, never
 * through an exception, so a caller can decide between a failed document load
 * and a refused user action with one value in hand.
 */

#include "articleurls.hh"

#include <QBuffer>
#include <QDataStream>
#include <QSet>
#include <QUrlQuery>

namespace
{

using ArticleUrls::Kind;
using ArticleUrls::Lookup;
using ArticleUrls::Parsed;
using ArticleUrls::Use;

/// Pinned on purpose. The contexts map is an ephemeral URL payload, but its
/// producer and its consumer must keep agreeing on the same QDataStream wire
/// format, so the version is never left to the library default.
QDataStream::Version const contextStreamVersion = QDataStream::Qt_5_0;

/// Every query key the URL contract defines. An unknown key is ignored; a
/// repeated known key would let two parsers disagree, so it invalidates the URL.
QStringList const & scalarQueryKeys()
{
  static QStringList const keys = {
    QStringLiteral( "word" ),           QStringLiteral( "punctuation_suffix" ),
    QStringLiteral( "blank" ),          QStringLiteral( "group" ),
    QStringLiteral( "dictionaries" ),   QStringLiteral( "muted" ),
    QStringLiteral( "contexts" ),       QStringLiteral( "ignore_diacritics" ),
    QStringLiteral( "scrollto" ),       QStringLiteral( "dict" ),
    QStringLiteral( "gdanchor" ),       QStringLiteral( "regexp" ),
    QStringLiteral( "matchcase" ),      QStringLiteral( "wildcards" )
  };
  return keys;
}

/// Read-only view of one URL's query, with the duplicate-scalar rule applied up
/// front.
class Query
{
public:
  explicit Query( QUrl const & url ):
    items( url )
  {
    QSet< QString > seen;
    for( QPair< QString, QString > const & item : items.queryItems() )
    {
      if( !scalarQueryKeys().contains( item.first ) )
        continue;
      if( seen.contains( item.first ) )
      {
        duplicateKey = item.first;
        return;
      }
      seen.insert( item.first );
    }
  }

  bool has( QString const & key ) const { return items.hasQueryItem( key ); }

  /// Fully decoded, and a '+' stays a literal plus: none of these values is
  /// form-encoded.
  QString value( QString const & key ) const
  {
    return items.queryItemValue( key, QUrl::FullyDecoded );
  }

  bool isValid() const { return duplicateKey.isEmpty(); }

  QUrlQuery items;
  QString duplicateKey;
};

/// Application schemes are portless and credential-free: they name a dictionary
/// and a resource, nothing else.
bool hasRejectedAuthority( QUrl const & url )
{
  return !url.userInfo().isEmpty() || url.port() != -1;
}

/// Exactly one structural leading slash is removed; a resource name may itself
/// start with a slash afterwards.
QString stripOneLeadingSlash( QString path )
{
  if( path.startsWith( QLatin1Char( '/' ) ) )
    return path.mid( 1 );
  return path;
}

/// The decoded resource name a resource URL carries. The query suffix belongs
/// to the name (the legacy fullPath contract included it), the fragment does
/// not.
QString decodedResourceName( QUrl const & url )
{
  QString name = stripOneLeadingSlash( url.path( QUrl::FullyDecoded ) );
  if( url.hasQuery() )
    name += QLatin1Char( '?' ) + QUrlQuery( url ).toString( QUrl::FullyDecoded );
  return name;
}

QStringList splitNames( QString const & value )
{
  return value.split( QLatin1Char( ',' ), Qt::SkipEmptyParts );
}

/// Compiles the user's FTS pattern.
///
/// A wildcard pattern is converted with NonPathWildcardConversion, so `*` is
/// not restricted to a path segment, and the original source syntax is kept in
/// the spec because the compiled expression cannot reconstruct it. The pattern
/// options are the ones the baseline highlight path used; `matchCase` only
/// decides whether the case-insensitive option is added.
QRegularExpression compileHighlight( QString const & sourcePattern, bool wildcards, bool matchCase )
{
  QString const pattern =
    wildcards ? QRegularExpression::wildcardToRegularExpression(
                  sourcePattern, QRegularExpression::NonPathWildcardConversion )
              : sourcePattern;

  QRegularExpression expression( pattern );
  QRegularExpression::PatternOptions options = expression.patternOptions()
                                                | QRegularExpression::DotMatchesEverythingOption
                                                | QRegularExpression::UseUnicodePropertiesOption
                                                | QRegularExpression::MultilineOption
                                                | QRegularExpression::InvertedGreedinessOption;
  if( !matchCase )
    options |= QRegularExpression::CaseInsensitiveOption;
  expression.setPatternOptions( options );
  return expression;
}

Parsed invalidParsed( Parsed parsed, QString const & error )
{
  parsed.kind = Kind::Invalid;
  parsed.lookup = Lookup();
  parsed.dictionaryId.clear();
  parsed.resourceName.clear();
  parsed.error = error;
  return parsed;
}

/// Fills every mode-independent field of a lookup URL and reports the first
/// reason the wire form is unusable.
bool fillLookup( Query const & query, Lookup * lookup, QString * error )
{
  if( !query.isValid() )
  {
    *error = QStringLiteral( "duplicate query parameter: " ) + query.duplicateKey;
    return false;
  }

  if( query.has( QStringLiteral( "contexts" ) ) )
  {
    QMap< QString, QString > contexts;
    QString contextError;
    if( !ArticleUrls::decodeContexts( query.value( QStringLiteral( "contexts" ) ).toLatin1(), &contexts,
                                      &contextError ) )
    {
      *error = QStringLiteral( "invalid contexts: " ) + contextError;
      return false;
    }

    lookup->contexts = contexts;
    // A document URL embeds the anchor in the context map; retaining it here
    // keeps the value addressable for a caller that only received contexts.
    lookup->anchor = contexts.value( QStringLiteral( "gdanchor" ) );
  }

  if( query.has( QStringLiteral( "gdanchor" ) ) )
    lookup->anchor = query.value( QStringLiteral( "gdanchor" ) );
  if( query.has( QStringLiteral( "dict" ) ) )
    lookup->dictionaryName = query.value( QStringLiteral( "dict" ) );
  if( query.has( QStringLiteral( "scrollto" ) ) )
    lookup->scrollTo = query.value( QStringLiteral( "scrollto" ) );

  lookup->ignoreDiacritics = query.value( QStringLiteral( "ignore_diacritics" ) ) == QLatin1String( "1" );
  lookup->dictionaries = splitNames( query.value( QStringLiteral( "dictionaries" ) ) );
  lookup->muted = splitNames( query.value( QStringLiteral( "muted" ) ) );

  lookup->highlight.sourcePattern = query.value( QStringLiteral( "regexp" ) );
  lookup->highlight.wildcards = query.value( QStringLiteral( "wildcards" ) ) == QLatin1String( "1" );
  lookup->highlight.matchCase = query.value( QStringLiteral( "matchcase" ) ) == QLatin1String( "1" );

  if( lookup->highlight.sourcePattern.isEmpty() )
  {
    // An absent or empty pattern disables highlighting; no flag may claim one.
    lookup->highlight = FtsHighlightSpec();
  }
  else
  {
    lookup->highlight.expression = compileHighlight( lookup->highlight.sourcePattern,
                                                     lookup->highlight.wildcards,
                                                     lookup->highlight.matchCase );
  }

  return true;
}

/// Applies the checked unsigned group.
///
/// The explicit-dictionary branch of a document load never required a usable
/// group and dispatches with group 0, exactly as the baseline did.
bool applyGroup( Query const & query, Use use, Lookup * lookup, QString * error )
{
  if( !query.has( QStringLiteral( "group" ) ) )
    return true;

  QString const text = query.value( QStringLiteral( "group" ) );
  bool valid = false;
  unsigned const group = text.toUInt( &valid );
  if( valid )
  {
    lookup->group = group;
    lookup->groupSpecified = true;
    return true;
  }

  if( use == Use::DocumentLoad && !lookup->dictionaries.isEmpty() )
    return true;

  *error = QStringLiteral( "invalid group: " ) + text;
  return false;
}

Parsed parseLookup( QUrl const & url, Use use, Parsed parsed )
{
  if( hasRejectedAuthority( url ) )
    return invalidParsed( parsed, QStringLiteral( "credentials or port not allowed on an application URL" ) );

  QString const host = url.host();
  if( !host.isEmpty() && host != QLatin1String( "localhost" ) )
    return invalidParsed( parsed, QStringLiteral( "unexpected gdlookup host: " ) + host );

  Query const query( url );
  Lookup lookup;
  QString error;
  if( !fillLookup( query, &lookup, &error ) )
    return invalidParsed( parsed, error );
  if( !applyGroup( query, use, &lookup, &error ) )
    return invalidParsed( parsed, error );

  if( query.value( QStringLiteral( "blank" ) ) == QLatin1String( "1" ) )
  {
    // The empty page is a document form; an action never routes it.
    if( use == Use::UserAction )
      return invalidParsed( parsed, QStringLiteral( "blank is a document form, not a lookup action" ) );

    lookup.blank = true;
    parsed.kind = Kind::LookupDocument;
    parsed.lookup = lookup;
    return parsed;
  }

  if( use == Use::UserAction )
  {
    // The clicked headword is the decoded path; `word` is only a fallback for
    // an action that has no path headword. A URL fragment is page navigation
    // and is never folded into it.
    QString headword = stripOneLeadingSlash( url.path( QUrl::FullyDecoded ) );
    if( headword.isEmpty() )
      headword = query.value( QStringLiteral( "word" ) );

    lookup.phrase = Config::InputPhrase( headword, query.value( QStringLiteral( "punctuation_suffix" ) ) );
    parsed.kind = Kind::LookupAction;
    parsed.lookup = lookup;
    return parsed;
  }

  lookup.phrase = Config::InputPhrase( query.value( QStringLiteral( "word" ) ).trimmed(),
                                       query.value( QStringLiteral( "punctuation_suffix" ) ) );

  if( !lookup.dictionaries.isEmpty() )
  {
    if( !lookup.phrase.isValid() )
      return invalidParsed( parsed, QStringLiteral( "word required for DocumentLoad" ) );
  }
  else if( !lookup.groupSpecified )
  {
    return invalidParsed( parsed, QStringLiteral( "group required for DocumentLoad" ) );
  }
  else if( !lookup.phrase.isValid() )
  {
    return invalidParsed( parsed, QStringLiteral( "word required for DocumentLoad" ) );
  }

  parsed.kind = Kind::LookupDocument;
  parsed.lookup = lookup;
  return parsed;
}

Parsed parseBword( QUrl const & url, Use use, Parsed parsed )
{
  if( use == Use::DocumentLoad )
    return invalidParsed( parsed, QStringLiteral( "bword is a user action form" ) );
  if( hasRejectedAuthority( url ) )
    return invalidParsed( parsed, QStringLiteral( "credentials or port not allowed on an application URL" ) );

  Query const query( url );
  Lookup lookup;
  QString error;
  if( !fillLookup( query, &lookup, &error ) )
    return invalidParsed( parsed, error );
  if( !applyGroup( query, use, &lookup, &error ) )
    return invalidParsed( parsed, error );

  // The opaque, path and authority spellings all normalize to one headword.
  QString headword = url.host().isEmpty() ? stripOneLeadingSlash( url.path( QUrl::FullyDecoded ) )
                                          : url.host();
  if( headword.isEmpty() )
    headword = query.value( QStringLiteral( "word" ) );

  lookup.phrase = Config::InputPhrase( headword, query.value( QStringLiteral( "punctuation_suffix" ) ) );
  parsed.kind = Kind::LookupAction;
  parsed.lookup = lookup;
  return parsed;
}

Parsed parsePicture( QUrl const & url, Parsed parsed )
{
  if( hasRejectedAuthority( url ) )
    return invalidParsed( parsed, QStringLiteral( "credentials or port not allowed on an application URL" ) );
  if( url.host().isEmpty() )
    return invalidParsed( parsed, QStringLiteral( "dictionary id required" ) );
  if( stripOneLeadingSlash( url.path( QUrl::FullyDecoded ) ).isEmpty() )
    return invalidParsed( parsed, QStringLiteral( "resource name required" ) );

  parsed.kind = Kind::PictureDocument;
  parsed.dictionaryId = url.host();
  parsed.resourceName = decodedResourceName( url );
  return parsed;
}

Parsed parseResource( QUrl const & url, Kind kind, Parsed parsed )
{
  if( hasRejectedAuthority( url ) )
    return invalidParsed( parsed, QStringLiteral( "credentials or port not allowed on an application URL" ) );

  // qrcx maps onto a qrc resource; its host is discarded by that mapping, so it
  // is not required to be a dictionary id.
  bool const qrcResource = url.scheme() == QLatin1String( "qrcx" );
  if( !qrcResource && url.host().isEmpty() )
    return invalidParsed( parsed, QStringLiteral( "dictionary id required" ) );

  QString const name = decodedResourceName( url );
  if( name.isEmpty() )
    return invalidParsed( parsed, QStringLiteral( "resource name required" ) );

  parsed.kind = kind;
  parsed.dictionaryId = url.host();
  parsed.resourceName = name;
  return parsed;
}

} // namespace

namespace ArticleUrls
{

Parsed parse( QUrl const & url, Use use )
{
  Parsed parsed;
  parsed.url = url;

  if( !url.isValid() )
    return invalidParsed( parsed, QStringLiteral( "invalid url" ) );

  QString const scheme = url.scheme();
  if( scheme.isEmpty() )
    return invalidParsed( parsed, QStringLiteral( "missing scheme" ) );

  if( scheme == QLatin1String( "gdlookup" ) )
    return parseLookup( url, use, parsed );
  if( scheme == QLatin1String( "bword" ) )
    return parseBword( url, use, parsed );
  if( scheme == QLatin1String( "gdpicture" ) )
    return parsePicture( url, parsed );
  if( scheme == QLatin1String( "gdau" ) )
    return parseResource( url, Kind::AudioAction, parsed );
  if( scheme == QLatin1String( "gdvideo" ) )
    return parseResource( url, Kind::VideoAction, parsed );
  if( scheme == QLatin1String( "bres" ) || scheme == QLatin1String( "gico" )
      || scheme == QLatin1String( "qrcx" ) )
    return parseResource( url, Kind::Resource, parsed );

  // Everything else belongs to the engine or to the desktop. Its own authority
  // rules are not this layer's business, so it is classified and left alone.
  parsed.kind = Kind::ExternalAction;
  return parsed;
}

QUrl lookupDocument( Lookup const & lookup )
{
  QUrl url;
  url.setScheme( QStringLiteral( "gdlookup" ) );
  url.setHost( QStringLiteral( "localhost" ) );

  if( lookup.blank )
  {
    // The empty page bypasses the word/group requirements, so it carries
    // nothing else.
    QUrlQuery blank;
    blank.addQueryItem( QStringLiteral( "blank" ), QStringLiteral( "1" ) );
    url.setQuery( blank );
    return url;
  }

  QUrlQuery query;

  if( !lookup.phrase.phrase.isEmpty() )
    query.addQueryItem( QStringLiteral( "word" ), lookup.phrase.phrase );
  if( !lookup.phrase.punctuationSuffix.isEmpty() )
    query.addQueryItem( QStringLiteral( "punctuation_suffix" ), lookup.phrase.punctuationSuffix );
  // A document URL carries a group; the explicit-dictionary form keeps the
  // group omission it is allowed to have.
  if( lookup.groupSpecified || lookup.dictionaries.isEmpty() )
    query.addQueryItem( QStringLiteral( "group" ), QString::number( lookup.group ) );
  if( !lookup.dictionaries.isEmpty() )
    query.addQueryItem( QStringLiteral( "dictionaries" ),
                        lookup.dictionaries.join( QLatin1Char( ',' ) ) );
  if( !lookup.muted.isEmpty() )
    query.addQueryItem( QStringLiteral( "muted" ), lookup.muted.join( QLatin1Char( ',' ) ) );
  if( lookup.ignoreDiacritics )
    query.addQueryItem( QStringLiteral( "ignore_diacritics" ), QStringLiteral( "1" ) );
  if( !lookup.scrollTo.isEmpty() )
    query.addQueryItem( QStringLiteral( "scrollto" ), lookup.scrollTo );
  if( !lookup.highlight.sourcePattern.isEmpty() )
  {
    query.addQueryItem( QStringLiteral( "regexp" ), lookup.highlight.sourcePattern );
    if( lookup.highlight.matchCase )
      query.addQueryItem( QStringLiteral( "matchcase" ), QStringLiteral( "1" ) );
    if( lookup.highlight.wildcards )
      query.addQueryItem( QStringLiteral( "wildcards" ), QStringLiteral( "1" ) );
  }

  // The anchor is a query item, not a member of the context map, exactly like
  // the baseline showDefinition producer: a context-carried value is extracted
  // and the map is serialized without it.
  QMap< QString, QString > contexts = lookup.contexts;
  QString anchor = lookup.anchor;
  QMap< QString, QString >::iterator const anchorContext = contexts.find( QStringLiteral( "gdanchor" ) );
  if( anchorContext != contexts.end() )
  {
    if( anchor.isEmpty() )
      anchor = anchorContext.value();
    contexts.erase( anchorContext );
  }
  if( !anchor.isEmpty() )
    query.addQueryItem( QStringLiteral( "gdanchor" ), anchor );
  if( !contexts.isEmpty() )
    query.addQueryItem( QStringLiteral( "contexts" ),
                        QString::fromLatin1( encodeContexts( contexts ) ) );

  if( !query.isEmpty() )
    url.setQuery( query );
  return url;
}

QUrl pictureDocument( QString const & dictionaryId, QString const & resourceName )
{
  QUrl url;
  url.setScheme( QStringLiteral( "gdpicture" ) );
  url.setHost( dictionaryId );

  QString path = stripOneLeadingSlash( resourceName );
  QString query;
  int const queryStart = path.indexOf( QLatin1Char( '?' ) );
  if( queryStart >= 0 )
  {
    query = path.mid( queryStart + 1 );
    path = path.left( queryStart );
  }

  url.setPath( QLatin1Char( '/' ) + path, QUrl::DecodedMode );
  if( !query.isEmpty() )
    url.setQuery( query, QUrl::DecodedMode );
  return url;
}

QByteArray encodeContexts( QMap< QString, QString > const & contexts )
{
  QByteArray payload;
  QBuffer buffer( &payload );
  buffer.open( QIODevice::WriteOnly );

  QDataStream stream( &buffer );
  stream.setVersion( contextStreamVersion );
  stream << contexts;

  buffer.close();
  return payload.toBase64();
}

bool decodeContexts( QByteArray const & base64, QMap< QString, QString > * out, QString * error )
{
  if( out )
    out->clear();
  if( error )
    error->clear();

  auto const fail = [ error ]( QString const & message )
  {
    if( error )
      *error = message;
    return false;
  };

  if( base64.isEmpty() )
    return fail( QStringLiteral( "empty contexts payload" ) );

  QByteArray::FromBase64Result const decoded =
    QByteArray::fromBase64Encoding( base64, QByteArray::AbortOnBase64DecodingErrors );
  if( !decoded )
    return fail( QStringLiteral( "invalid base64 payload" ) );

  QByteArray payload = decoded.decoded;
  QBuffer buffer( &payload );
  if( !buffer.open( QIODevice::ReadOnly ) )
    return fail( QStringLiteral( "cannot open the contexts payload" ) );

  QDataStream stream( &buffer );
  stream.setVersion( contextStreamVersion );

  QMap< QString, QString > contexts;
  stream >> contexts;

  if( stream.status() != QDataStream::Ok )
    return fail( QStringLiteral( "invalid contexts stream" ) );
  if( !buffer.atEnd() )
    return fail( QStringLiteral( "trailing bytes in contexts" ) );

  if( out )
    *out = contexts;
  return true;
}

QString resourcePath( QUrl const & url )
{
  return decodedResourceName( url );
}

} // namespace ArticleUrls
