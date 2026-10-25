/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef __WEBSITE_HH_INCLUDED__
#define __WEBSITE_HH_INCLUDED__

#include "dictionary.hh"
#include "config.hh"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QVector>
#include <QTextDocumentFragment>
#include <algorithm>
#include "encoding.hh"

/// Support for any web sites via a templated url.
namespace WebSite {

using std::vector;
using std::string;

namespace detail {

// Only these value spans may change: markup, quotes, whitespace, comments and
// raw script text are retained from the original input, never serialized again.
struct UrlEdit { qsizetype start, length; QString value; };
struct TextSpan { qsizetype start, length; };
struct Attribute { QString name; qsizetype start, length; };
struct Tag { QString name; QVector< Attribute > attributes; qsizetype start, end; };

inline bool startsAt( QString const & text, qsizetype at, QString const & token )
{
  return text.mid( at, token.size() ).compare( token, Qt::CaseInsensitive ) == 0;
}

inline QString applyEdits( QString text, QVector< UrlEdit > edits )
{
  // Attribute and CSS spans are disjoint and always refer to the original input.
  std::sort( edits.begin(), edits.end(), []( UrlEdit const & a, UrlEdit const & b ) { return a.start < b.start; } );
  for ( auto i = edits.crbegin(); i != edits.crend(); ++i )
    text.replace( i->start, i->length, i->value );
  return text;
}

inline QVector< Tag > htmlTags( QString const & html, QVector< TextSpan > * cssSpans = nullptr )
{
  QVector< Tag > tags;
  qsizetype p = 0;
  while ( p < html.size() )
  {
    const auto nextTag = html.indexOf( '<', p );
    if ( nextTag < 0 ) break;
    p = nextTag;
    if ( html.mid( p, 4 ) == QStringLiteral( "<!--" ) )
    {
      const auto end = html.indexOf( QStringLiteral( "-->" ), p + 4 );
      p = end < 0 ? html.size() : end + 3;
      continue;
    }
    const auto start = p++;
    while ( p < html.size() && html[p].isSpace() ) ++p;
    if ( p >= html.size() ) break;
    // Closing tags and declarations have no editable URL-valued spans.
    if ( html[p] == '/' || html[p] == '!' || html[p] == '?' )
    {
      const auto end = html.indexOf( '>', p );
      p = end < 0 ? html.size() : end + 1;
      continue;
    }
    if ( !html[p].isLetter() ) continue;
    const auto nameStart = p;
    while ( p < html.size() && ( html[p].isLetterOrNumber() || html[p] == '-' || html[p] == ':' ) ) ++p;
    Tag tag{ html.mid( nameStart, p - nameStart ).toLower(), {}, start, html.size() };
    bool closed = false;
    while ( p < html.size() )
    {
      while ( p < html.size() && html[p].isSpace() ) ++p;
      if ( p == html.size() ) break;
      if ( html[p] == '>' ) { tag.end = ++p; closed = true; break; }
      if ( html[p] == '/' ) { ++p; continue; }
      const auto attrStart = p;
      while ( p < html.size() && !html[p].isSpace() && html[p] != '=' && html[p] != '>' ) ++p;
      const QString name = html.mid( attrStart, p - attrStart ).toLower();
      while ( p < html.size() && html[p].isSpace() ) ++p;
      if ( p >= html.size() || html[p] != '=' ) continue;
      ++p;
      while ( p < html.size() && html[p].isSpace() ) ++p;
      if ( p == html.size() ) break;
      const QChar quote = html[p] == '\'' || html[p] == '"' ? html[p++] : QChar();
      const auto valueStart = p;
      while ( p < html.size() && ( !quote.isNull() ? html[p] != quote : !html[p].isSpace() && html[p] != '>' ) ) ++p;
      tag.attributes.append( { name, valueStart, p - valueStart } );
      if ( !quote.isNull() && p < html.size() ) ++p;
    }
    if ( !closed ) break;
    tags.append( tag );
    if ( tag.name == QStringLiteral( "script" ) || tag.name == QStringLiteral( "style" )
         || tag.name == QStringLiteral( "textarea" ) || tag.name == QStringLiteral( "title" ) )
    {
      const QString closing = QStringLiteral( "</" ) + tag.name;
      auto end = html.indexOf( closing, p, Qt::CaseInsensitive );
      while ( end >= 0 && end + closing.size() < html.size()
              && !html[end + closing.size()].isSpace()
              && html[end + closing.size()] != '>' && html[end + closing.size()] != '/' )
        end = html.indexOf( closing, end + closing.size(), Qt::CaseInsensitive );
      const auto bodyEnd = end < 0 ? html.size() : end;
      if ( cssSpans && tag.name == QStringLiteral( "style" ) && bodyEnd > tag.end )
        cssSpans->append( { tag.end, bodyEnd - tag.end } );
      const auto closeEnd = end < 0 ? -1 : html.indexOf( '>', end + closing.size() );
      p = closeEnd < 0 ? html.size() : closeEnd + 1;
    }
  }
  return tags;
}

enum class UrlContext
{
  Css,
  HtmlAttribute
};

inline QString decodeHtmlAttributeValue( QString const & raw )
{
  // QTextDocumentFragment decodes named and numeric character references. The
  // angle brackets are escaped first so a URL value cannot become markup while
  // it is being decoded.
  QString escaped = raw;
  escaped.replace( '<', QStringLiteral( "&lt;" ) ).replace( '>', QStringLiteral( "&gt;" ) );
  return QTextDocumentFragment::fromHtml( QStringLiteral( "<pre>" ) + escaped
                                           + QStringLiteral( "</pre>" ) ).toPlainText();
}

inline QString serializeTarget( QString value, UrlContext context )
{
  if ( context != UrlContext::HtmlAttribute ) return value;

  // URL query separators and other ampersands must survive HTML attribute
  // parsing as data, not as a new character reference. QUrl already fully
  // encodes quote characters in generated URLs.
  value.replace( '&', QStringLiteral( "&amp;" ) )
       .replace( '<', QStringLiteral( "&lt;" ) )
       .replace( '>', QStringLiteral( "&gt;" ) );
  return value;
}

inline QString resolvedTarget( QString const & raw, QUrl const & base,
                               UrlContext context = UrlContext::Css )
{
  const QString rawTarget = raw.trimmed();
  if ( rawTarget.isEmpty() ) return raw;

  const QString target = context == UrlContext::HtmlAttribute
    ? decodeHtmlAttributeValue( rawTarget ).trimmed()
    : rawTarget;
  if ( target.isEmpty() || target.startsWith( '#' ) ) return raw;

  const QUrl relative( target );
  if ( !relative.isValid() || !relative.scheme().isEmpty() ) return raw;
  const QUrl absolute = base.resolved( relative );
  if ( !absolute.isValid() || absolute.isRelative() ) return raw;

  const auto start = raw.indexOf( rawTarget );
  return raw.left( start ) + serializeTarget( absolute.toString( QUrl::FullyEncoded ), context )
       + raw.mid( start + rawTarget.size() );
}

inline QString resolveCss( QString const & css, QUrl const & base,
                           UrlContext context = UrlContext::Css )
{
  QVector< UrlEdit > edits;
  qsizetype p = 0;
  auto quotedEnd = [&]( qsizetype start, QChar quote ) {
    auto end = start;
    while ( end < css.size() )
    {
      if ( css[end] == '\\' ) { end += 2; continue; }
      if ( css[end] == quote ) break;
      ++end;
    }
    return qMin( end, css.size() );
  };
  auto add = [&]( qsizetype start, qsizetype end ) {
    const QString raw = css.mid( start, end - start );
    // Leave CSS-escaped URLs alone instead of misinterpreting escape syntax.
    if ( raw.contains( '\\' ) ) return;
    const QString value = resolvedTarget( raw, base, context );
    if ( value != raw ) edits.append( { start, end - start, value } );
  };
  while ( p < css.size() )
  {
    if ( css.mid( p, 2 ) == QStringLiteral( "/*" ) )
    {
      const auto end = css.indexOf( QStringLiteral( "*/" ), p + 2 );
      p = end < 0 ? css.size() : end + 2;
      continue;
    }
    if ( css[p] == '\'' || css[p] == '"' ) { p = quotedEnd( p + 1, css[p] ) + 1; continue; }
    if ( startsAt( css, p, QStringLiteral( "@import" ) )
         && ( p + 7 == css.size() || css[p + 7].isSpace() ) )
    {
      p += 7;
      while ( p < css.size() && css[p].isSpace() ) ++p;
      if ( p < css.size() && ( css[p] == '\'' || css[p] == '"' ) )
      {
        const auto end = quotedEnd( p + 1, css[p] );
        add( p + 1, end ); p = end + 1; continue;
      }
    }
    if ( p >= css.size() ) break;
    if ( startsAt( css, p, QStringLiteral( "url" ) )
         && ( p == 0 || ( !css[p - 1].isLetterOrNumber() && css[p - 1] != '-' && css[p - 1] != '_' ) ) )
    {
      auto value = p + 3;
      while ( value < css.size() && css[value].isSpace() ) ++value;
      if ( value < css.size() && css[value] == '(' )
      {
        ++value;
        while ( value < css.size() && css[value].isSpace() ) ++value;
        if ( value < css.size() && ( css[value] == '\'' || css[value] == '"' ) )
        {
          const auto end = quotedEnd( value + 1, css[value] );
          if ( end < css.size() ) add( value + 1, end );
          p = end + 1; continue;
        }
        const auto end = css.indexOf( ')', value );
        if ( end >= 0 ) { add( value, end ); p = end + 1; continue; }
      }
    }
    ++p;
  }
  return applyEdits( css, edits );
}

inline QString resolveSrcset( QString const & text, QUrl const & base )
{
  QVector< UrlEdit > edits;
  qsizetype p = 0;
  while ( p < text.size() )
  {
    while ( p < text.size() && ( text[p].isSpace() || text[p] == ',' ) ) ++p;
    const auto start = p;
    // The HTML parser decodes the attribute before srcset tokenization. Do
    // the same for the data: discriminator so an entity-encoded data URL's
    // comma remains part of that candidate rather than starting a new one.
    const bool data = startsAt( text, p, QStringLiteral( "data:" ) )
                   || startsAt( decodeHtmlAttributeValue( text.mid( start ) ), 0,
                                QStringLiteral( "data:" ) );
    while ( p < text.size() && !text[p].isSpace() && ( data || text[p] != ',' ) ) ++p;
    auto targetEnd = p;
    // A data URL may contain commas, but trailing commas delimit candidates
    // without descriptors rather than becoming part of the next candidate.
    while ( targetEnd > start && text[targetEnd - 1] == ',' ) --targetEnd;
    const QString raw = text.mid( start, targetEnd - start );
    const QString value = resolvedTarget( raw, base, UrlContext::HtmlAttribute );
    if ( raw != value ) edits.append( { start, targetEnd - start, value } );
    if ( targetEnd < p ) continue;
    if ( p < text.size() && text[p] == ',' ) { ++p; continue; }
    while ( p < text.size() && text[p] != ',' ) ++p;
  }
  return applyEdits( text, edits );
}

inline QByteArray substituteUrlPlaceholders( QByteArray const & urlTemplate, QString const & word )
{
  QByteArray result = urlTemplate;
  result.replace( "%25GDWORD%25", word.toUtf8().toPercentEncoding() );
  const auto replace = [&]( QByteArray const & placeholder, QString const & name ) {
    if ( !result.contains( placeholder ) ) return;
    const auto codec = Encoding::byName( name );
    if ( !codec ) return;
    try { result.replace( placeholder, codec->encode( word ).toPercentEncoding() ); }
    catch ( Encoding::exUnrepresentable const & ) { /* Leave only this placeholder intact. */ }
  };
  replace( "%25GD1251%25", QStringLiteral( "Windows-1251" ) );
  replace( "%25GDBIG5%25", QStringLiteral( "Big-5" ) );
  replace( "%25GDBIG5HKSCS%25", QStringLiteral( "Big5-HKSCS" ) );
  replace( "%25GDSHIFTJIS%25", QStringLiteral( "Shift-JIS" ) );
  replace( "%25GDGBK%25", QStringLiteral( "GB18030" ) );
  for ( int x = 1; x <= 16; ++x )
  {
    replace( QString( "%25GDISO%1%25" ).arg( x ).toLatin1(), QString( "ISO-8859-%1" ).arg( x ) );
    if ( x == 10 ) x = 12; // Preserve the caller's ISO11/12 skip.
  }
  return result;
}
}

/// Pure production resolver, shared verbatim with the HTTP completion path.
inline QString resolveInlineUrls( QString const & html, QUrl const & documentUrl )
{
  QVector< detail::TextSpan > cssSpans;
  const auto tags = detail::htmlTags( html, &cssSpans );
  QUrl base = documentUrl;
  bool foundBase = false;
  for ( auto const & tag : tags )
  {
    if ( tag.name != QStringLiteral( "base" ) ) continue;
    for ( auto const & attr : tag.attributes )
    {
      if ( attr.name != QStringLiteral( "href" ) ) continue;
      const QString raw = html.mid( attr.start, attr.length ).trimmed();
      const QString decoded = detail::decodeHtmlAttributeValue( raw ).trimmed();
      const QUrl candidate( decoded );
      const QUrl resolved = documentUrl.resolved( candidate );
      if ( candidate.isValid() && resolved.isValid() && !resolved.isRelative()
           && ( resolved.scheme() == QStringLiteral( "http" ) || resolved.scheme() == QStringLiteral( "https" ) ) )
      { base = resolved; foundBase = true; break; }
    }
    if ( foundBase ) break;
  }
  QVector< detail::UrlEdit > edits;
  for ( auto const & tag : tags )
  {
    for ( auto const & attr : tag.attributes )
    {
      const QString raw = html.mid( attr.start, attr.length );
      QString value = raw;
      if ( attr.name == QStringLiteral( "style" ) )
        value = detail::resolveCss( raw, base, detail::UrlContext::HtmlAttribute );
      else if ( attr.name == QStringLiteral( "srcset" ) ) value = detail::resolveSrcset( raw, base );
      else if ( attr.name == QStringLiteral( "src" ) || attr.name == QStringLiteral( "href" )
                || attr.name == QStringLiteral( "action" ) || attr.name == QStringLiteral( "poster" ) )
      {
        if ( tag.name == QStringLiteral( "form" ) && attr.name == QStringLiteral( "action" ) && raw.isEmpty() )
          value = documentUrl.toString( QUrl::FullyEncoded );
        else
          value = detail::resolvedTarget( raw,
                                          tag.name == QStringLiteral( "base" ) ? documentUrl : base,
                                          detail::UrlContext::HtmlAttribute );
      }
      if ( raw != value ) edits.append( { attr.start, attr.length, value } );
    }
  }
  for ( auto const & span : cssSpans )
  {
    const QString raw = html.mid( span.start, span.length );
    const QString value = detail::resolveCss( raw, base );
    if ( raw != value ) edits.append( { span.start, span.length, value } );
  }
  return detail::applyEdits( html, edits );
}

namespace detail {
inline QString mapStylesheets( QString const & html, QString const & dictionaryId )
{
  QVector< UrlEdit > edits;
  for ( auto const & tag : htmlTags( html ) )
  {
    if ( tag.name != QStringLiteral( "link" ) ) continue;
    bool stylesheet = false;
    for ( auto const & attr : tag.attributes )
      if ( attr.name == QStringLiteral( "rel" ) )
      {
        const QString rel = html.mid( attr.start, attr.length ).simplified();
        stylesheet = rel.split( ' ', Qt::SkipEmptyParts )
                         .contains( QStringLiteral( "stylesheet" ), Qt::CaseInsensitive );
      }
    if ( !stylesheet ) continue;
    for ( auto const & attr : tag.attributes )
      if ( attr.name == QStringLiteral( "href" ) )
      {
        const QString raw = html.mid( attr.start, attr.length );
        const QString target = raw.trimmed();
        // The browser normally decodes character references in href. Once the
        // URL is nested in a percent-encoded path, perform that step explicitly
        // so &amp; remains a remote query separator rather than an "amp;" key.
        QString escaped = target;
        escaped.replace( '<', QStringLiteral( "&lt;" ) ).replace( '>', QStringLiteral( "&gt;" ) );
        const QString remote = QTextDocumentFragment::fromHtml( QStringLiteral( "<pre>" ) + escaped
                                                               + QStringLiteral( "</pre>" ) ).toPlainText();
        const QUrl url( remote );
        if ( !url.isValid() || ( url.scheme() != QStringLiteral( "http" ) && url.scheme() != QStringLiteral( "https" ) ) ) continue;
        edits.append( { attr.start + raw.indexOf( target ), target.size(), QStringLiteral( "bres://" ) + dictionaryId + '/'
                        + QString::fromLatin1( QUrl::toPercentEncoding( remote ) ) } );
      }
  }
  return applyEdits( html, edits );
}
}

vector< sptr< Dictionary::Class > > makeDictionaries( Config::WebSites const &,
                                                      QNetworkAccessManager & )
    THROW_SPEC( std::exception );

/// Exposed here for moc
class WebSiteDataRequestSlots: public Dictionary::DataRequest
{
  Q_OBJECT

protected slots:

  virtual void requestFinished( QNetworkReply * )
  {}
};

}

#endif
