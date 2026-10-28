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
  return scheme.compare( QStringLiteral( "bres" ), Qt::CaseInsensitive ) == 0 ||
         scheme.compare( QStringLiteral( "gico" ), Qt::CaseInsensitive ) == 0 ||
         scheme.compare( QStringLiteral( "gdau" ), Qt::CaseInsensitive ) == 0 ||
         scheme.compare( QStringLiteral( "qrcx" ), Qt::CaseInsensitive ) == 0 ||
         scheme.compare( QStringLiteral( "gdvideo" ), Qt::CaseInsensitive ) == 0;
}

inline bool isSkippableReference( QString const & value )
{
  QString const trimmed = value.trimmed();
  if( trimmed.isEmpty() || trimmed.startsWith( QLatin1Char( '#' ) ) ) return true;

  QString const lower = trimmed.left( trimmed.indexOf( QLatin1Char( ':' ) ) ).toLower();
  return lower == QStringLiteral( "data" ) || lower == QStringLiteral( "javascript" ) ||
         lower == QStringLiteral( "mailto" ) || lower == QStringLiteral( "tel" );
}

inline QString tagNameAt( QString const & html, qsizetype position )
{
  qsizetype const open = html.lastIndexOf( QLatin1Char( '<' ), position );
  qsizetype const close = html.lastIndexOf( QLatin1Char( '>' ), position );
  if( open < 0 || close > open ) return {};

  qsizetype cursor = open + 1;
  while( cursor < html.size() && ( html.at( cursor ).isSpace() || html.at( cursor ) == QLatin1Char( '/' ) ) )
    ++cursor;
  qsizetype const begin = cursor;
  while( cursor < html.size() && !html.at( cursor ).isSpace() && html.at( cursor ) != QLatin1Char( '>' ) &&
         html.at( cursor ) != QLatin1Char( '/' ) )
    ++cursor;
  return html.mid( begin, cursor - begin ).toLower();
}

inline bool isResourceTagAttribute( QString const & tag, QString const & attribute )
{
  if( attribute == QStringLiteral( "href" ) )
    return tag == QStringLiteral( "link" );

  return tag == QStringLiteral( "img" ) || tag == QStringLiteral( "script" ) ||
         tag == QStringLiteral( "source" ) || tag == QStringLiteral( "video" ) ||
         tag == QStringLiteral( "audio" ) || tag == QStringLiteral( "iframe" ) ||
         tag == QStringLiteral( "object" ) || tag == QStringLiteral( "input" );
}

inline void appendCandidate( QVector< QPair< qsizetype, QString > > & candidates,
                             QString const & html, qsizetype offset, QString const & raw,
                             QUrl const & base, bool resourceContext )
{
  if( isSkippableReference( raw ) ) return;

  QUrl url( raw, QUrl::TolerantMode );
  if( !url.isValid() ) return;
  if( url.isRelative() )
  {
    if( !base.isValid() || base.isRelative() ) return;
    url = base.resolved( url );
  }
  if( !url.isValid() || url.isRelative() || url.scheme().isEmpty() ) return;

  bool const internal = isInternalResourceScheme( url.scheme() );
  bool const network = url.scheme().compare( QStringLiteral( "http" ), Qt::CaseInsensitive ) == 0 ||
                       url.scheme().compare( QStringLiteral( "https" ), Qt::CaseInsensitive ) == 0;
  if( !internal && ( !network || !resourceContext ) ) return;

  candidates.append( qMakePair( offset, raw ) );
  Q_UNUSED( html );
}
} // namespace detail

/// Collect resource-valued HTML spans without modifying the source document.
/// Offsets are QString/UTF-16 offsets, so callers can rewrite from the end
/// without invalidating any later span.
inline QVector< ResourceSpan > collectResourceSpans( QString const & html, QUrl const & base )
{
  QVector< QPair< qsizetype, QString > > candidates;
  struct SpanEnd
  {
    qsizetype offset = 0;
    qsizetype length = 0;
    QString raw;
  };
  QVector< SpanEnd > sourceSpans;

  static QRegularExpression const attributes(
    QStringLiteral( "\\b(src|href|poster|data)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')" ),
    QRegularExpression::CaseInsensitiveOption );
  auto attributeIterator = attributes.globalMatch( html );
  while( attributeIterator.hasNext() )
  {
    QRegularExpressionMatch const match = attributeIterator.next();
    QString const attribute = match.captured( 1 ).toLower();
    qsizetype const valueOffset = match.capturedStart( 2 ) >= 0 ? match.capturedStart( 2 ) : match.capturedStart( 3 );
    QString const raw = match.capturedStart( 2 ) >= 0 ? match.captured( 2 ) : match.captured( 3 );
    QString const tag = detail::tagNameAt( html, match.capturedStart( 1 ) );

    QUrl probe( raw, QUrl::TolerantMode );
    bool const internal = probe.isValid() && detail::isInternalResourceScheme( probe.scheme() );
    bool const resourceContext = internal || detail::isResourceTagAttribute( tag, attribute );
    if( raw.isEmpty() || ( !internal && !detail::isResourceTagAttribute( tag, attribute ) ) ) continue;

    detail::appendCandidate( candidates, html, valueOffset, raw, base, resourceContext );
    if( !candidates.isEmpty() && candidates.constLast().first == valueOffset )
      sourceSpans.append( { valueOffset, qsizetype( raw.size() ), raw } );
  }

  static QRegularExpression const styleBlocks(
    QStringLiteral( "<style\\b[^>]*>(.*?)</style\\s*>" ),
    QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption );
  static QRegularExpression const cssComments(
    QStringLiteral( "/\\*.*?\\*/" ), QRegularExpression::DotMatchesEverythingOption );
  static QRegularExpression const cssUrls(
    QStringLiteral( "\\burl\\s*\\(\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\)\\s]+))\\s*\\)" ),
    QRegularExpression::CaseInsensitiveOption );
  auto styleIterator = styleBlocks.globalMatch( html );
  while( styleIterator.hasNext() )
  {
    QRegularExpressionMatch const styleMatch = styleIterator.next();
    QString css = styleMatch.captured( 1 );
    auto commentIterator = cssComments.globalMatch( css );
    while( commentIterator.hasNext() )
    {
      QRegularExpressionMatch const comment = commentIterator.next();
      for( qsizetype i = comment.capturedStart(); i < comment.capturedEnd(); ++i )
        css[ i ] = QLatin1Char( ' ' );
    }

    auto cssIterator = cssUrls.globalMatch( css );
    while( cssIterator.hasNext() )
    {
      QRegularExpressionMatch const match = cssIterator.next();
      qsizetype const localOffset = match.capturedStart( 1 ) >= 0 ? match.capturedStart( 1 ) :
                                     match.capturedStart( 2 ) >= 0 ? match.capturedStart( 2 ) : match.capturedStart( 3 );
      QString const raw = match.capturedStart( 1 ) >= 0 ? match.captured( 1 ) :
                          match.capturedStart( 2 ) >= 0 ? match.captured( 2 ) : match.captured( 3 );
      int const before = candidates.size();
      detail::appendCandidate( candidates, html, styleMatch.capturedStart( 1 ) + localOffset,
                               raw, base, false );
      if( candidates.size() != before )
        sourceSpans.append( { styleMatch.capturedStart( 1 ) + localOffset, qsizetype( raw.size() ), raw } );
    }
  }

  std::stable_sort( sourceSpans.begin(), sourceSpans.end(), []( SpanEnd const & left, SpanEnd const & right ) {
    return left.offset < right.offset;
  } );

  QVector< ResourceSpan > result;
  result.reserve( sourceSpans.size() );
  for( SpanEnd const & source : sourceSpans )
  {
    QUrl url( source.raw, QUrl::TolerantMode );
    if( url.isRelative() ) url = base.resolved( url );
    if( url.isValid() && !url.isRelative() )
      result.append( { source.offset, source.length, url } );
  }
  return result;
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
