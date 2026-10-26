/* This file is (c) 2008-2011 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "article_netmgr.hh"
#include "articleurls.hh"

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QMimeDatabase>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>

#include <cstring>
#include <exception>

namespace {

QString mimeForName( QString const & name )
{
  QString const mime = QMimeDatabase().mimeTypeForFile( name, QMimeDatabase::MatchExtension ).name();
  return mime.isEmpty() ? QStringLiteral( "application/octet-stream" ) : mime;
}

sptr< Dictionary::DataRequest > notFound()
{
  return new Dictionary::DataRequestInstant( false );
}

sptr< Dictionary::DataRequest > bytesRequest( QByteArray const & bytes )
{
  auto request = sptr< Dictionary::DataRequestInstant >( new Dictionary::DataRequestInstant( true ) );
  request->getData().assign( bytes.begin(), bytes.end() );
  return request;
}

} // namespace

sptr< Dictionary::DataRequest > ArticleNetworkAccessManager::getResource( QUrl const & url,
                                                                       QString & contentType )
{
  contentType.clear();
  ArticleUrls::Parsed const parsed = ArticleUrls::parse( url, ArticleUrls::Use::DocumentLoad );
  if( parsed.kind == ArticleUrls::Kind::Invalid || parsed.kind == ArticleUrls::Kind::ExternalAction )
    return {};

  if( parsed.kind == ArticleUrls::Kind::LookupDocument )
  {
    contentType = QStringLiteral( "text/html" );
    auto const & lookup = parsed.lookup;
    if( lookup.blank )
      return articleMaker.makeEmptyPage();
    // These two producer branches deliberately retain their different legacy
    // argument contracts; explicit dictionary lookups are the FTS path.
    if( !lookup.dictionaries.isEmpty() )
      return articleMaker.makeDefinitionFor( lookup.phrase, 0, {}, {}, lookup.dictionaries );
    QSet< QString > const muted( lookup.muted.cbegin(), lookup.muted.cend() );
    return articleMaker.makeDefinitionFor( lookup.phrase, lookup.group, lookup.contexts, muted, {},
                                          lookup.ignoreDiacritics );
  }

  if( parsed.kind == ArticleUrls::Kind::PictureDocument )
  {
    contentType = QStringLiteral( "text/html" );
    QUrl picture = url;
    picture.setScheme( QStringLiteral( "bres" ) );
    return articleMaker.makePicturePage( picture.toEncoded().toStdString() );
  }

  if( url.scheme() == QLatin1String( "qrcx" ) )
  {
    // qrcx discards its host when mapping to the compiled resource tree.
    QString const path = QStringLiteral( ":" ) + url.path( QUrl::FullyDecoded );
    contentType = mimeForName( path );
    QFile file( path );
    if( !file.open( QIODevice::ReadOnly ) )
      return notFound();
    QByteArray const bytes = file.readAll();
    if( file.error() != QFileDevice::NoError )
      return new Dictionary::DataRequestInstant( file.errorString() );
    return bytesRequest( bytes );
  }

  bool const icon = url.scheme() == QLatin1String( "gico" );
  contentType = icon ? QStringLiteral( "image/png" ) : mimeForName( url.path( QUrl::FullyDecoded ) );
  if( icon && parsed.resourceName != QLatin1String( "dicticon.png" )
      && parsed.resourceName != QLatin1String( "icon.png" ) )
    return notFound();
  for( auto const & dictionary : dictionaries )
  {
    if( parsed.dictionaryId.toUtf8().toStdString() != dictionary->getId() )
      continue;
    if( !icon )
      return dictionary->getResource( parsed.resourceName.toUtf8().toStdString() );
    QByteArray png;
    QBuffer buffer( &png );
    buffer.open( QIODevice::WriteOnly );
    if( !dictionary->getIcon().pixmap( 16 ).save( &buffer, "PNG" ) )
      return notFound();
    return bytesRequest( png );
  }
  return notFound();
}

QNetworkReply * ArticleNetworkAccessManager::createRequest( Operation op, QNetworkRequest const & req,
                                                           QIODevice * outgoingData )
{
  QNetworkRequest request( req );
  if( ( request.url().scheme() == QLatin1String( "gdlookup" )
        || request.url().scheme() == QLatin1String( "http" ) )
      && request.url().host() == QLatin1String( "upload.wikimedia.org" ) )
  {
    QUrl url = request.url();
    url.setScheme( QStringLiteral( "https" ) );
    request.setUrl( url );
  }

  if( op == GetOperation )
  {
    QString contentType;
    try
    {
      auto resource = getResource( request.url(), contentType );
      if( resource )
        return new ArticleResourceReply( this, request, resource, contentType );
    }
    catch( std::exception const & error )
    {
      return new ArticleResourceReply( this, request,
        new Dictionary::DataRequestInstant( QString::fromUtf8( error.what() ) ), contentType );
    }
    catch( ... )
    {
      return new ArticleResourceReply( this, request,
        new Dictionary::DataRequestInstant( QStringLiteral( "resource creation failed" ) ), contentType );
    }
  }

  if( request.url().isLocalFile() && request.url().host().isEmpty() )
  {
    QString path = request.url().toLocalFile();
    if( articleMaker.adjustFilePath( path ) )
    {
      QUrl url = request.url();
      QUrl const local = QUrl::fromLocalFile( path );
      url.setHost( local.host() );
      url.setPath( local.path() );
      request.setUrl( url );
    }
  }
  if( hideHeader && request.url().scheme().startsWith( QLatin1String( "http" ), Qt::CaseInsensitive ) )
    request.setRawHeader( "User-Agent", request.rawHeader( "User-Agent" ).replace(
        QCoreApplication::applicationName().toUtf8(), "" ) );

  // Explicit QNAM requests retain their Referer and the server's frame headers.
  // Renderer policy is applied by the WebEngine page interceptor.
  return QNetworkAccessManager::createRequest( op, request, outgoingData );
}

void ArticleNetworkAccessManager::ignoreSSLErrors( QNetworkReply * reply, QList< QSslError > const & )
{
  reply->ignoreSslErrors();
}

ArticleResourceReply::ArticleResourceReply( QObject * parent, QNetworkRequest const & request,
    sptr< Dictionary::DataRequest > const & req_, QString const & contentType ):
  QNetworkReply( parent ), req( req_ )
{
  setRequest( request );
  setUrl( request.url() );
  setOperation( QNetworkAccessManager::GetOperation );
  if( !contentType.isEmpty() )
    setHeader( QNetworkRequest::ContentTypeHeader, contentType );
  open( ReadOnly );
  connect( req.get(), &Dictionary::DataRequest::updated, this, &ArticleResourceReply::reqUpdated, Qt::QueuedConnection );
  connect( req.get(), &Dictionary::DataRequest::finished, this, &ArticleResourceReply::reqFinished, Qt::QueuedConnection );
  if( req->isFinished() )
    QTimer::singleShot( 0, this, &ArticleResourceReply::reqFinished );
  else if( req->dataSize() > 0 )
    QTimer::singleShot( 0, this, &ArticleResourceReply::reqUpdated );
}

ArticleResourceReply::~ArticleResourceReply()
{
  disconnect( req.get(), nullptr, this, nullptr );
  if( !cancelled )
  {
    cancelled = true;
    try { req->cancel(); } catch( ... ) {}
  }
}

void ArticleResourceReply::abort()
{
  if( cancelled )
    return;
  cancelled = true;
  disconnect( req.get(), nullptr, this, nullptr );
  try { req->cancel(); } catch( ... ) {}
  if( !isFinished() )
  {
    setError( QNetworkReply::OperationCanceledError, QStringLiteral( "Request cancelled" ) );
    setFinished( true );
    emit errorOccurred( error() );
    emit finished();
  }
}

qint64 ArticleResourceReply::bytesAvailable() const
{
  qint64 const size = req->dataSize();
  return ( size < 0 ? 0 : qMax< qint64 >( 0, size - alreadyRead ) ) + QNetworkReply::bytesAvailable();
}

qint64 ArticleResourceReply::readData( char * data, qint64 maxSize )
{
  if( maxSize == 0 )
    return 0;
  if( maxSize < 0 || cancelled )
    return -1;
  qint64 const size = req->dataSize();
  if( size < 0 )
    return req->isFinished() ? -1 : 0;
  qint64 const count = qMin( qMax< qint64 >( 0, size - alreadyRead ), maxSize );
  if( count <= 0 )
    return req->isFinished() ? -1 : 0;
  try
  {
    req->getDataSlice( size_t( alreadyRead ), size_t( count ), data );
    alreadyRead += count;
    return count;
  }
  catch( std::exception const & error )
  {
    setError( QNetworkReply::UnknownContentError, QString::fromUtf8( error.what() ) );
    return -1;
  }
}

void ArticleResourceReply::reqUpdated()
{
  if( !isFinished() && !cancelled )
    emit readyRead();
}

void ArticleResourceReply::reqFinished()
{
  if( isFinished() || cancelled )
    return;
  QString const error = req->getErrorString();
  if( !error.isEmpty() )
    setError( QNetworkReply::UnknownContentError, error );
  else if( req->dataSize() < 0 )
    setError( QNetworkReply::ContentNotFoundError, QStringLiteral( "Content not found" ) );
  setFinished( true );
  if( this->error() != QNetworkReply::NoError )
    emit errorOccurred( this->error() );
  emit readyRead();
  emit finished();
}
