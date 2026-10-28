/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file. */
#ifndef GOLDENDICT_SAVEARTICLE_HH_INCLUDED
#define GOLDENDICT_SAVEARTICLE_HH_INCLUDED

#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QUrl>
#include <QVector>

#include <algorithm>
#include <memory>

class ArticleNetworkAccessManager;
class ArticleView;

namespace SaveArticleDetail
{
/// A UTF-16 range in an immutable HTML snapshot and its resolved resource URL.
struct ResourceSpan
{
  qsizetype offset = 0;
  qsizetype length = 0;
  QUrl url;
};

namespace detail
{
inline bool isInternalResourceScheme( QString const & scheme )
{
  return scheme == QLatin1String( "bres" ) || scheme == QLatin1String( "gico" ) ||
         scheme == QLatin1String( "gdau" ) || scheme == QLatin1String( "qrcx" ) ||
         scheme == QLatin1String( "gdvideo" );
}

inline QString decodeAttribute( QString const & raw )
{
  // Decode references before QUrl parsing, while keeping the span over the
  // original UTF-16 source (not over the shorter decoded URL).
  static QRegularExpression const entities( QStringLiteral( "&(#x[0-9a-f]+|#[0-9]+|amp|quot|apos|lt|gt);" ),
                                           QRegularExpression::CaseInsensitiveOption );
  QString result;
  qsizetype cursor = 0;
  auto matches = entities.globalMatch( raw );
  while( matches.hasNext() )
  {
    auto const match = matches.next();
    result += raw.mid( cursor, match.capturedStart() - cursor );
    QString const entity = match.captured( 1 );
    if( entity.startsWith( QLatin1Char( '#' ) ) )
    {
      bool hex = entity.size() > 1 && entity.at( 1 ).toLower() == QLatin1Char( 'x' );
      bool ok = false;
      uint const scalar = entity.mid( hex ? 2 : 1 ).toUInt( &ok, hex ? 16 : 10 );
      if( ok && scalar > 0 && scalar <= 0x10ffff && !( scalar >= 0xd800 && scalar <= 0xdfff ) )
      {
        char32_t const character = scalar;
        result += QString::fromUcs4( &character, 1 );
      }
      else result += QChar::ReplacementCharacter;
    }
    else if( entity.compare( QLatin1String( "amp" ), Qt::CaseInsensitive ) == 0 ) result += QLatin1Char( '&' );
    else if( entity.compare( QLatin1String( "quot" ), Qt::CaseInsensitive ) == 0 ) result += QLatin1Char( '"' );
    else if( entity == QLatin1String( "apos" ) ) result += QLatin1Char( '\'' );
    else if( entity == QLatin1String( "lt" ) ) result += QLatin1Char( '<' );
    else if( entity == QLatin1String( "gt" ) ) result += QLatin1Char( '>' );
    else result += match.captured();
    cursor = match.capturedEnd();
  }
  return result + raw.mid( cursor );
}

inline bool resourceAttribute( QString const & tag, QString const & attribute )
{
  if( attribute == QLatin1String( "href" ) ) return tag == QLatin1String( "link" );
  if( attribute != QLatin1String( "src" ) && attribute != QLatin1String( "poster" ) &&
      attribute != QLatin1String( "data" ) ) return false;
  return tag == QLatin1String( "img" ) || tag == QLatin1String( "script" ) ||
         tag == QLatin1String( "source" ) || tag == QLatin1String( "video" ) ||
         tag == QLatin1String( "audio" ) || tag == QLatin1String( "iframe" ) ||
         tag == QLatin1String( "object" ) || tag == QLatin1String( "input" );
}
} // namespace detail

/// Shared with the Core regex target: only Core types and immutable source
/// spans. Every occurrence is retained; the transaction deduplicates downloads.
inline QVector< ResourceSpan > collectResourceSpans( QString const & html, QUrl const & base )
{
  QVector< ResourceSpan > spans;
  auto append = [&]( qsizetype offset, QString const & raw, bool network, bool attribute ) {
    QString const value = ( attribute ? detail::decodeAttribute( raw ) : raw ).trimmed();
    if( value.isEmpty() || value.startsWith( QLatin1Char( '#' ) ) ) return;
    QUrl url( value, QUrl::TolerantMode );
    if( url.isRelative() ) url = base.resolved( url );
    QString const scheme = url.scheme().toLower();
    if( !url.isValid() || url.isRelative() ||
        !( detail::isInternalResourceScheme( scheme ) ||
           ( network && ( scheme == QLatin1String( "http" ) || scheme == QLatin1String( "https" ) ) ) ) ) return;
    spans.append( { offset, raw.size(), url } );
  };

  static QRegularExpression const comments( QStringLiteral( "/\\*.*?\\*/" ),
                                            QRegularExpression::DotMatchesEverythingOption );
  static QRegularExpression const urls(
    QStringLiteral( "\\burl\\s*\\(\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\)\\s]+))\\s*\\)" ),
    QRegularExpression::CaseInsensitiveOption );
  auto css = [&]( QString text, qsizetype offset, bool attribute ) {
    auto ignored = comments.globalMatch( text );
    while( ignored.hasNext() )
    {
      auto const match = ignored.next();
      for( qsizetype i = match.capturedStart(); i < match.capturedEnd(); ++i ) text[ i ] = QLatin1Char( ' ' );
    }
    auto matches = urls.globalMatch( text );
    while( matches.hasNext() )
    {
      auto const match = matches.next();
      int const group = match.capturedStart( 1 ) >= 0 ? 1 : match.capturedStart( 2 ) >= 0 ? 2 : 3;
      append( offset + match.capturedStart( group ), match.captured( group ), true, attribute );
    }
  };

  // Generated audio/video links also occur in script strings and event
  // attributes, as in the legacy collector. Ordinary web links are collected
  // only in resource attributes/CSS, so navigation links remain navigation.
  static QRegularExpression const internalStrings(
    QStringLiteral( "([\"'])((?:bres|gico|gdau|qrcx|gdvideo)://[^\"'<>\\s]+)\\1" ),
    QRegularExpression::CaseInsensitiveOption );
  auto internal = [&]( QString const & text, qsizetype offset, bool attribute ) {
    auto matches = internalStrings.globalMatch( text );
    while( matches.hasNext() )
    {
      auto const match = matches.next();
      append( offset + match.capturedStart( 2 ), match.captured( 2 ), false, attribute );
    }
  };

  // Match complete tags, including > inside quoted values. Attributes are
  // scanned within each tag, so data-src and HTML-looking script text cannot
  // masquerade as an ordinary src attribute.
  static QRegularExpression const tags(
    QStringLiteral( "<([a-z][a-z0-9:-]*)\\b(?:[^>\"']|\"[^\"]*\"|'[^']*')*>" ),
    QRegularExpression::CaseInsensitiveOption );
  static QRegularExpression const attributes(
    QStringLiteral( "\\s+([a-z_:][a-z0-9_:.-]*)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))" ),
    QRegularExpression::CaseInsensitiveOption );
  qsizetype cursor = 0;
  while( ( cursor = html.indexOf( QLatin1Char( '<' ), cursor ) ) >= 0 )
  {
    if( html.mid( cursor, 4 ) == QLatin1String( "<!--" ) )
    {
      qsizetype const end = html.indexOf( QLatin1String( "-->" ), cursor + 4 );
      if( end < 0 ) break;
      cursor = end + 3;
      continue;
    }
    auto const tag = tags.match( html, cursor, QRegularExpression::NormalMatch,
                                 QRegularExpression::AnchorAtOffsetMatchOption );
    if( !tag.hasMatch() ) { ++cursor; continue; }
    QString const name = tag.captured( 1 ).toLower();
    QString const text = tag.captured();
    auto attrs = attributes.globalMatch( text );
    while( attrs.hasNext() )
    {
      auto const attr = attrs.next();
      int const group = attr.capturedStart( 2 ) >= 0 ? 2 : attr.capturedStart( 3 ) >= 0 ? 3 : 4;
      qsizetype const offset = cursor + attr.capturedStart( group );
      QString const key = attr.captured( 1 ).toLower();
      QString const raw = attr.captured( group );
      if( key == QLatin1String( "style" ) ) css( raw, offset, true );
      else if( detail::resourceAttribute( name, key ) ||
               detail::isInternalResourceScheme( QUrl( detail::decodeAttribute( raw ) ).scheme().toLower() ) )
        append( offset, raw, true, true );
      else internal( raw, offset, true );
    }
    cursor = tag.capturedEnd();
    if( name == QLatin1String( "script" ) || name == QLatin1String( "style" ) )
    {
      qsizetype const end = html.indexOf( QStringLiteral( "</%1" ).arg( name ), cursor, Qt::CaseInsensitive );
      if( end < 0 ) break;
      QString const body = html.mid( cursor, end - cursor );
      if( name == QLatin1String( "style" ) ) css( body, cursor, false );
      else internal( body, cursor, false );
      cursor = end;
    }
  }
  std::stable_sort( spans.begin(), spans.end(), []( ResourceSpan const & a, ResourceSpan const & b ) {
    return a.offset < b.offset;
  } );
  spans.erase( std::unique( spans.begin(), spans.end(), []( ResourceSpan const & a, ResourceSpan const & b ) {
    return a.offset == b.offset && a.length == b.length;
  } ), spans.end() );
  return spans;
}
} // namespace SaveArticleDetail

class SaveArticle : public QObject
{
  Q_OBJECT
public:
  enum class State { Idle, Capturing, Collecting, Downloading, Publishing,
                     Complete, Cancelled, Failed };
  Q_ENUM( State )

  SaveArticle( ArticleView * view, ArticleNetworkAccessManager & resources,
               QObject * parent = nullptr );
  ~SaveArticle() override;
  State state() const noexcept;
  void start( QString const & htmlDestination, bool complete );
  void cancel() noexcept;

signals:
  void saved( QString const & htmlDestination );
  void failed( QString const & message );
  void cancelled( QString const & message );

private:
  struct Private;
  std::unique_ptr< Private > d;
};
#endif // GOLDENDICT_SAVEARTICLE_HH_INCLUDED
