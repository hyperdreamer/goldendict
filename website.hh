/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef __WEBSITE_HH_INCLUDED__
#define __WEBSITE_HH_INCLUDED__

#include "dictionary.hh"
#include "config.hh"
#include <QNetworkAccessManager>
#include <QNetworkReply>

/// Support for any web sites via a templated url.

#include "encoding.hh"
#include "gddebug.hh"

namespace WebSite {


/// Template URL substitutions. Kept here (and nowhere else) because they are the
/// only part of the website engine that the encoding migration has to pin: a
/// placeholder whose charset is unavailable, or whose text cannot be represented,
/// is left untouched with one diagnostic instead of being replaced by escapes.
namespace detail {

/// Replaces one template placeholder. A placeholder whose charset is unavailable,
/// or whose text has no mapping in it, is left untouched with one diagnostic --
/// the behaviour the encoding contract pins.
inline void replaceUrlPlaceholder( QByteArray & result, QString const & word,
                                   QByteArray const & placeholder, QString const & name )
{
  if ( !result.contains( placeholder ) ) return;
  const auto warn = [&] {
    qWarning( "Encoding: URL placeholder %s not representable in %s",
              placeholder.constData(), qPrintable( name ) );
  };
  const auto codec = Encoding::byName( name );
  if ( !codec ) { warn(); return; }
  try { result.replace( placeholder, codec->encode( word ).toPercentEncoding() ); }
  catch ( Encoding::exUnrepresentable const & ) { warn(); }
}

inline QByteArray substituteUrlPlaceholders( QByteArray const & urlTemplate, QString const & word )
{
  QByteArray result = urlTemplate;
  result.replace( "%25GDWORD%25", word.toUtf8().toPercentEncoding() );
  const auto replace = [&]( QByteArray const & placeholder, QString const & name ) {
    replaceUrlPlaceholder( result, word, placeholder, name );
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


/// Baseline website rewriting, extracted so the retained behaviour stays testable:
/// relative link targets become absolute against the response URL, absolute
/// stylesheet links are remapped to the dictionary resource scheme, and unclosed
/// span/div wrappers are closed. This is the shipped behaviour, not a browser
/// URL parser: no base-tag handling and no character-reference decoding.
inline QString rewriteWebsiteLinks( QString html, QUrl const & pageUrl, QString const & dictId )
{
  QString root = pageUrl.scheme() + QStringLiteral( "://" ) + pageUrl.host();
  QString base = root + pageUrl.path();
  while( !base.isEmpty() && !base.endsWith( QLatin1Char( '/' ) ) )
    base.chop( 1 );

  QRegularExpression tags( QStringLiteral( "<\\s*(a|link|img|script)\\s+[^>]*(src|href)\\s*=\\s*['\"][^>]+>" ),
                           QRegularExpression::CaseInsensitiveOption );
  QRegularExpression links( QStringLiteral( "\\b(src|href)\\s*=\\s*(['\"])([^'\"]+['\"])" ),
                            QRegularExpression::CaseInsensitiveOption );
  int pos = 0;
  QString articleNewString;
  QRegularExpressionMatchIterator it = tags.globalMatch( html );
  while( it.hasNext() )
  {
    QRegularExpressionMatch match = it.next();
    articleNewString += html.mid( pos, match.capturedStart() - pos );
    pos = match.capturedEnd();

    QString tag = match.captured();
    QRegularExpressionMatch matchLinks = links.match( tag );
    if( !matchLinks.hasMatch() )
    {
      articleNewString += tag;
      continue;
    }

    QString url = matchLinks.captured( 3 );
    if( url.indexOf( QStringLiteral( ":/" ) ) >= 0 || url.indexOf( QStringLiteral( "data:" ) ) >= 0
        || url.indexOf( QStringLiteral( "mailto:" ) ) >= 0 || url.startsWith( QLatin1Char( '#' ) )
        || url.startsWith( QStringLiteral( "javascript:" ) ) )
    {
      articleNewString += tag;
      continue;
    }

    QString newUrl = matchLinks.captured( 1 ) + QStringLiteral( "=" ) + matchLinks.captured( 2 );
    if( url.startsWith( QStringLiteral( "//" ) ) )
      newUrl += pageUrl.scheme() + QLatin1Char( ':' );
    else if( url.startsWith( QLatin1Char( '/' ) ) )
      newUrl += root;
    else
      newUrl += base;
    newUrl += matchLinks.captured( 3 );

    tag.replace( matchLinks.capturedStart(), matchLinks.capturedLength(), newUrl );
    articleNewString += tag;
  }
  if( pos )
  {
    articleNewString += html.mid( pos );
    html = articleNewString;
    articleNewString.clear();
  }

  QString const prefix = QStringLiteral( "bres://" ) + dictId + QLatin1Char( '/' );
  QRegularExpression linkTags( QStringLiteral( "(<\\s*link\\s[^>]*rel\\s*=\\s*['\"]stylesheet['\"]\\s+[^>]*href\\s*=\\s*['\"])([^'\"]+)://([^'\"]+['\"][^>]+>)" ),
                               QRegularExpression::CaseInsensitiveOption );
  pos = 0;
  it = linkTags.globalMatch( html );
  while( it.hasNext() )
  {
    QRegularExpressionMatch match = it.next();
    articleNewString += html.mid( pos, match.capturedStart() - pos );
    pos = match.capturedEnd();
    articleNewString += match.captured( 1 ) + prefix + match.captured( 2 ) + QLatin1Char( '/' ) + match.captured( 3 );
  }
  if( pos )
  {
    articleNewString += html.mid( pos );
    html = articleNewString;
  }

  // The baseline counted these with QRegExp, whose classes are Qt's own, not
  // PCRE's: \s is QChar::isSpace(), and \b splits on QRegExp's word characters,
  // which are letters, numbers, marks (accents and combining characters) and
  // underscore. Emulating that with QRegularExpression options does not hold for
  // every Unicode input, so the counting uses those classes directly.
  auto isWordChar = []( QChar c )
  {
    return c.isLetterOrNumber() || c.isMark() || c == QLatin1Char( '_' );
  };
  auto countOpen = [&]( QChar const * name, int length ) {
    int count = 0;
    for ( qsizetype i = 0; i + length < html.size(); ++i )
    {
      if ( html[i] != QLatin1Char( '<' ) ) continue;
      qsizetype j = i + 1;
      while ( j < html.size() && html[j].isSpace() ) ++j;
      qsizetype k = 0;
      while ( k < length && j + k < html.size()
              && html[j + k].toLower() == name[k].toLower() ) ++k;
      if ( k != length ) continue;
      qsizetype const after = j + length;
      if ( after >= html.size() || !isWordChar( html[after] ) ) ++count;
    }
    return count;
  };
  auto countClose = [&]( QChar const * name, int length ) {
    int count = 0;
    for ( qsizetype i = 0; i + length < html.size(); ++i )
    {
      if ( html[i] != QLatin1Char( '<' ) ) continue;
      qsizetype j = i + 1;
      while ( j < html.size() && html[j].isSpace() ) ++j;
      if ( j >= html.size() || html[j] != QLatin1Char( '/' ) ) continue;
      ++j; // no whitespace allowed between '/' and the name, as in the baseline
      qsizetype k = 0;
      while ( k < length && j + k < html.size()
              && html[j + k].toLower() == name[k].toLower() ) ++k;
      if ( k != length ) continue;
      j += length;
      while ( j < html.size() && html[j].isSpace() ) ++j;
      if ( j < html.size() && html[j] == QLatin1Char( '>' ) ) ++count;
    }
    return count;
  };

  QString const spanName = QStringLiteral( "span" );
  QString const divName = QStringLiteral( "div" );
  int openTags = countOpen( spanName.constData(), 4 );
  int closedTags = countClose( spanName.constData(), 4 );
  while( openTags > closedTags ) { html += QStringLiteral( "</span>" ); closedTags += 1; }

  openTags = countOpen( divName.constData(), 3 );
  closedTags = countClose( divName.constData(), 3 );
  while( openTags > closedTags ) { html += QStringLiteral( "</div>" ); closedTags += 1; }

  return html;
}

/// The inline frame GoldenDict inserts for a website dictionary. Both id and name
/// carry the frame identity because the article view locates the frame by name.
inline QString websiteIframeMarkup( QString const & dictId, QString const & url )
{
  QString const frame = QStringLiteral( "gdexpandframe-" ) + dictId;
  return QStringLiteral( "<div class=\"website_padding\"></div>" )
       + QStringLiteral( "<iframe id=\"" ) + frame
       + QStringLiteral( "\" name=\"" ) + frame
       + QStringLiteral( "\" src=\"" ) + url
       + QStringLiteral( "\" onmouseover=\"processIframeMouseOver('" ) + frame
       + QStringLiteral( "');\" onmouseout=\"processIframeMouseOut();\" "
                         "scrolling=\"no\" marginwidth=\"0\" marginheight=\"0\" "
                         "frameborder=\"0\" vspace=\"0\" hspace=\"0\" "
                         "style=\"overflow:visible; width:100%; display:none;\">"
                         "</iframe>" );
}


/// Decode and rewrite one fetched website response. Kept as one seam so the
/// response path itself is testable: the previous attempt regressed by leaving
/// the decoded body unassigned, which a rewriting-only test cannot catch.
inline QString decodeAndRewriteWebsiteResponse( QByteArray const & body,
                                                QByteArray const & contentType,
                                                QUrl const & pageUrl,
                                                QString const & dictId )
{
  Encoding const codec = Encoding::forHtml( body, contentType );
  QString const decoded = codec.isValid() ? codec.decode( body ) : QString::fromUtf8( body );
  return rewriteWebsiteLinks( decoded, pageUrl, dictId );
}

} // namespace detail


using std::vector;
using std::string;

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
