/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file. */
#include "savearticle.hh"

#include "articlebridge.hh"
#include "articleview.hh"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>
#include <QWebEnginePage>

#include <algorithm>

namespace
{
QString canonicalResourceKey( QUrl url )
{
  // A fragment selects content inside a resource; it does not select another
  // byte stream. Query parameters remain part of the key and the filename.
  url.setFragment( QString() );
  return QString::fromLatin1( url.toEncoded( QUrl::FullyEncoded ) );
}

QString resourceRelativePath( QUrl const & url, int & queryNumber )
{
  QString path = url.path( QUrl::FullyDecoded );
  while( path.startsWith( QLatin1Char( '/' ) ) ) path.remove( 0, 1 );
  if( path.isEmpty() ) path = QStringLiteral( "resource" );

  QString relative = url.host();
  if( !relative.isEmpty() ) relative += QLatin1Char( '/' );
  relative += path;
  if( url.hasQuery() ) relative += QStringLiteral( "_q%1" ).arg( queryNumber++ );
  return relative;
}

bool pathIsInside( QString const & root, QString const & candidate )
{
  QString const cleanRoot = QDir::cleanPath( QDir( root ).absolutePath() );
  QString const cleanCandidate = QDir::cleanPath( QDir( candidate ).absolutePath() );
  return cleanCandidate == cleanRoot || cleanCandidate.startsWith( cleanRoot + QLatin1Char( '/' ) );
}

} // namespace

struct SaveArticle::Private
{
  struct Download
  {
    QUrl url;
    QString path;
    QPointer< ResourceToSaveHandler > handler;
    bool done = false;
  };

  SaveArticle * const owner;
  ArticleNetworkAccessManager & resources;
  QPointer< ArticleView > view;
  QPointer< ArticleBridge > bridge;
  QMetaObject::Connection generationConnection;
  QMetaObject::Connection bridgeDestroyedConnection;
  QMetaObject::Connection viewDestroyedConnection;

  State currentState = State::Idle;
  quint64 operation = 0;
  int generation = 0;
  bool active = false;
  bool complete = false;
  bool assetsPublished = false;
  bool htmlPublished = false;

  QString destination;
  QString snapshot;
  QString rewritten;
  QUrl baseUrl;
  QString destinationParent;
  QString staging;
  QString stagedAssets;
  QString finalAssets;
  QVector< Download > downloads;

  Private( SaveArticle * owner_, ArticleView * view_, ArticleNetworkAccessManager & resources_ ):
    owner( owner_ ), resources( resources_ ), view( view_ )
  {
  }

  bool valid( quint64 token ) const noexcept
  {
    return active && operation == token;
  }

  void disconnectGuards()
  {
    QObject::disconnect( generationConnection );
    QObject::disconnect( bridgeDestroyedConnection );
    QObject::disconnect( viewDestroyedConnection );
    generationConnection = {};
    bridgeDestroyedConnection = {};
    viewDestroyedConnection = {};
  }

  void removeOwnedPaths()
  {
    if( assetsPublished && !htmlPublished && !finalAssets.isEmpty() )
      QDir( finalAssets ).removeRecursively();
    if( !staging.isEmpty() )
      QDir( staging ).removeRecursively();
    assetsPublished = false;
    staging.clear();
    stagedAssets.clear();
    finalAssets.clear();
  }

  void releaseHandlers( ResourceToSaveHandler * protectedHandler = nullptr, bool immediate = true )
  {
    for( Download & download : downloads )
    {
      if( !download.handler ) continue;
      if( download.handler.data() == protectedHandler || !immediate )
        download.handler->deleteLater();
      else
        delete download.handler.data();
      download.handler.clear();
    }
    downloads.clear();
  }

  void finishFailure( QString const & message, ResourceToSaveHandler * protectedHandler = nullptr )
  {
    if( !active || currentState == State::Complete ) return;
    active = false; // Invalidate every queued HTML/resource continuation first.
    disconnectGuards();
    releaseHandlers( protectedHandler );
    removeOwnedPaths();
    currentState = State::Failed;
    emit owner->failed( message );
  }

  void finishCancelled( QString const & message )
  {
    if( !active || currentState == State::Complete ) return;
    active = false;
    disconnectGuards();
    releaseHandlers();
    removeOwnedPaths();
    currentState = State::Cancelled;
    emit owner->cancelled( message );
  }

  void finishSuccess()
  {
    if( !active ) return;
    active = false;
    disconnectGuards();
    releaseHandlers( nullptr, false );
    if( !staging.isEmpty() ) QDir( staging ).removeRecursively();
    staging.clear();
    stagedAssets.clear();
    currentState = State::Complete;
    emit owner->saved( destination );
  }
};

SaveArticle::SaveArticle( ArticleView * view, ArticleNetworkAccessManager & resources, QObject * parent ):
  QObject( parent ), d( std::make_unique< Private >( this, view, resources ) )
{
  if( view )
  {
    d->viewDestroyedConnection = connect( view, &QObject::destroyed, this, [this] {
      if( d->active ) d->finishCancelled( QStringLiteral( "Save cancelled because the article changed or closed." ) );
    } );
  }
}

SaveArticle::~SaveArticle()
{
  if( d->active )
  {
    d->active = false;
    d->disconnectGuards();
    d->releaseHandlers();
    d->removeOwnedPaths();
  }
}

SaveArticle::State SaveArticle::state() const noexcept
{
  return d->currentState;
}

void SaveArticle::cancel() noexcept
{
  if( d->active )
    d->finishCancelled( QStringLiteral( "Save cancelled because the article changed or closed." ) );
}

void SaveArticle::start( QString const & htmlDestination, bool complete )
{
  if( d->active )
    d->finishCancelled( QStringLiteral( "Save cancelled because a newer save was started." ) );

  ++d->operation;
  quint64 const token = d->operation;
  d->active = true;
  d->complete = complete;
  d->assetsPublished = false;
  d->htmlPublished = false;
  d->destination = htmlDestination;
  d->snapshot.clear();
  d->rewritten.clear();
  d->downloads.clear();
  d->staging.clear();
  d->stagedAssets.clear();
  d->finalAssets.clear();
  d->currentState = State::Capturing;

  if( !d->view )
  {
    d->finishFailure( QStringLiteral( "Cannot save an article after its view was closed." ) );
    return;
  }

  auto * page = d->view->findChild< QWebEnginePage * >();
  d->baseUrl = page ? page->url() : QUrl();
  d->bridge = d->view->findChild< ArticleBridge * >( QStringLiteral( "gdArticleBridge" ),
                                                       Qt::FindChildrenRecursively );
  if( !d->bridge )
  {
    d->finishFailure( QStringLiteral( "Cannot save an article without its live renderer bridge." ) );
    return;
  }
  d->generation = d->bridge->generation();

  QObject::disconnect( d->generationConnection );
  QObject::disconnect( d->bridgeDestroyedConnection );
  d->generationConnection = connect( d->bridge, &ArticleBridge::generationChanged, this,
    [this, token]( int generation ) {
      if( !d->valid( token ) || generation == d->generation ) return;
      d->finishCancelled( QStringLiteral( "Save cancelled because the article changed or closed." ) );
    } );
  d->bridgeDestroyedConnection = connect( d->bridge, &QObject::destroyed, this, [this, token] {
    if( d->valid( token ) )
      d->finishCancelled( QStringLiteral( "Save cancelled because the article changed or closed." ) );
  } );

  // Keep the supplied manager as part of the controller's ownership boundary;
  // ArticleView::saveResource uses this same manager for every real request.
  Q_UNUSED( d->resources );
  d->view->toHtml( [this, token]( QString const & html ) {
    if( !d->valid( token ) ) return;
    d->snapshot = html;
    d->currentState = State::Collecting;

    QVector< SaveArticleDetail::ResourceSpan > const spans =
      d->complete ? SaveArticleDetail::collectResourceSpans( d->snapshot, d->baseUrl ) :
                    QVector< SaveArticleDetail::ResourceSpan >();

    QFileInfo const destinationInfo( d->destination );
    d->destinationParent = destinationInfo.absoluteDir().absolutePath();
    if( !QDir( d->destinationParent ).exists() )
    {
      d->finishFailure( QStringLiteral( "The destination directory does not exist." ) );
      return;
    }

    if( d->complete )
    {
      QDir const parent( d->destinationParent );
      QString const stem = destinationInfo.completeBaseName().isEmpty() ?
                           QStringLiteral( "article" ) : destinationInfo.completeBaseName();
      QString const operationSuffix = QStringLiteral( ".goldendict-save-%1" ).arg( d->operation );
      bool madeStaging = false;
      for( int attempt = 0; attempt != 32; ++attempt )
      {
        QString const name = QStringLiteral( ".%1%2-%3" ).arg( stem, operationSuffix,
                                                               QUuid::createUuid().toString( QUuid::WithoutBraces ) );
        if( parent.exists( name ) ) continue;
        if( parent.mkdir( name ) )
        {
          d->staging = parent.filePath( name );
          madeStaging = true;
          break;
        }
      }
      if( !madeStaging )
      {
        d->finishFailure( QStringLiteral( "Unable to create a unique save staging directory." ) );
        return;
      }

      d->stagedAssets = QDir( d->staging ).filePath( QStringLiteral( "assets" ) );
      if( !QDir().mkpath( d->stagedAssets ) )
      {
        d->finishFailure( QStringLiteral( "Unable to create the save asset directory." ) );
        return;
      }
      d->finalAssets = parent.filePath( QStringLiteral( "%1_files-%2" ).arg(
        stem, QUuid::createUuid().toString( QUuid::WithoutBraces ) ) );
      if( QFileInfo::exists( d->finalAssets ) )
      {
        d->finishFailure( QStringLiteral( "The unique save asset directory already exists." ) );
        return;
      }

      struct Replacement { qsizetype offset; qsizetype length; QString text; };
      QVector< Replacement > replacements;
      QMap< QString, int > jobsByUrl;
      int queryNumber = 1;
      for( SaveArticleDetail::ResourceSpan const & span : spans )
      {
        QString const key = canonicalResourceKey( span.url );
        int index = jobsByUrl.value( key, -1 );
        if( index < 0 )
        {
          QString const relative = resourceRelativePath( span.url, queryNumber );
          QString const resourcePath = QDir( d->stagedAssets ).filePath( relative );
          if( !pathIsInside( d->stagedAssets, resourcePath ) )
          {
            d->finishFailure( QStringLiteral( "A resource path escapes the save staging directory." ) );
            return;
          }
          index = d->downloads.size();
          jobsByUrl.insert( key, index );
          d->downloads.append( { span.url, resourcePath, nullptr, false } );
        }

        QString const relative = QDir( d->stagedAssets ).relativeFilePath( d->downloads.at( index ).path );
        QString const encoded = QString::fromLatin1( QUrl::toPercentEncoding(
          QDir( d->finalAssets ).dirName() + QLatin1Char( '/' ) + relative, "/" ) );
        replacements.append( { span.offset, span.length, encoded } );
      }

      d->rewritten = d->snapshot;
      std::sort( replacements.begin(), replacements.end(), []( Replacement const & left, Replacement const & right ) {
        return left.offset > right.offset;
      } );
      for( Replacement const & replacement : replacements )
        d->rewritten.replace( replacement.offset, replacement.length, replacement.text );
    }
    else
      d->rewritten = d->snapshot;

    d->currentState = State::Downloading;
    for( int index = 0; index < d->downloads.size(); ++index )
    {
      if( !d->valid( token ) ) return;
      Private::Download & download = d->downloads[ index ];
      ResourceToSaveHandler * handler = d->view->saveResource( download.url, d->baseUrl, download.path );
      if( !handler )
      {
        d->finishFailure( QStringLiteral( "Unable to create a resource download handler." ) );
        return;
      }
      download.handler = handler;
      QObject::connect( handler, &ResourceToSaveHandler::done, this, [this, token, index] {
        if( !d->valid( token ) || index < 0 || index >= d->downloads.size() ) return;
        Private::Download & download = d->downloads[ index ];
        if( !QFileInfo::exists( download.path ) )
        {
          d->finishFailure( QStringLiteral( "The referenced resource failed to download." ),
                            download.handler.data() );
          return;
        }
        download.done = true;
        bool allDone = std::all_of( d->downloads.cbegin(), d->downloads.cend(),
                                    []( Private::Download const & item ) { return item.done; } );
        if( allDone )
        {
          d->currentState = State::Publishing;
          QMetaObject::invokeMethod( this, [this, token] {
            if( d->valid( token ) )
            {
              // The asset rename is the publication step immediately before
              // the QSaveFile commit below; it never overwrites a sibling.
              QDir parent( d->destinationParent );
              if( !parent.rename( d->stagedAssets, d->finalAssets ) )
              {
                d->finishFailure( QStringLiteral( "Unable to publish the saved asset directory." ) );
                return;
              }
              d->assetsPublished = true;

              QSaveFile file( d->destination );
              file.setDirectWriteFallback( false );
              if( !file.open( QIODevice::WriteOnly ) )
              {
                d->finishFailure( QStringLiteral( "Unable to open the HTML destination: %1" ).arg( file.errorString() ) );
                return;
              }
              QByteArray const bytes = d->rewritten.toUtf8();
              if( file.write( bytes ) != bytes.size() || !file.commit() )
              {
                d->finishFailure( QStringLiteral( "Unable to commit the HTML destination: %1" ).arg( file.errorString() ) );
                return;
              }
              d->htmlPublished = true;
              d->finishSuccess();
            }
          }, Qt::QueuedConnection );
        }
      } );
      QObject::connect( handler, &QObject::destroyed, this, [this, token, index] {
        if( !d->valid( token ) || index < 0 || index >= d->downloads.size() ) return;
        if( !d->downloads.at( index ).done )
          d->finishFailure( QStringLiteral( "The referenced resource download was interrupted." ) );
      } );

      // Instant DataRequests finish inside saveResource(), before the signal
      // connection above can be installed. The committed file is the durable
      // completion marker and also accepts valid zero-byte resources.
      if( handler->isEmpty() )
      {
        if( !QFileInfo::exists( download.path ) )
        {
          d->finishFailure( QStringLiteral( "The referenced resource failed to download." ), handler );
          return;
        }
        download.done = true;
      }
    }

    if( d->downloads.isEmpty() )
    {
      d->currentState = State::Publishing;
      QMetaObject::invokeMethod( this, [this, token] {
        if( !d->valid( token ) ) return;
        if( d->complete )
        {
          QDir parent( d->destinationParent );
          if( !parent.rename( d->stagedAssets, d->finalAssets ) )
          {
            d->finishFailure( QStringLiteral( "Unable to publish the saved asset directory." ) );
            return;
          }
          d->assetsPublished = true;
        }
        QSaveFile file( d->destination );
        file.setDirectWriteFallback( false );
        if( !file.open( QIODevice::WriteOnly ) )
        {
          d->finishFailure( QStringLiteral( "Unable to open the HTML destination: %1" ).arg( file.errorString() ) );
          return;
        }
        QByteArray const bytes = d->rewritten.toUtf8();
        if( file.write( bytes ) != bytes.size() || !file.commit() )
        {
          d->finishFailure( QStringLiteral( "Unable to commit the HTML destination: %1" ).arg( file.errorString() ) );
          return;
        }
        d->htmlPublished = true;
        d->finishSuccess();
      }, Qt::QueuedConnection );
    }
    else
    {
      bool allDone = std::all_of( d->downloads.cbegin(), d->downloads.cend(),
                                  []( Private::Download const & item ) { return item.done; } );
      if( allDone )
      {
        d->currentState = State::Publishing;
        QMetaObject::invokeMethod( this, [this, token] {
          if( !d->valid( token ) ) return;
          QDir parent( d->destinationParent );
          if( !parent.rename( d->stagedAssets, d->finalAssets ) )
          {
            d->finishFailure( QStringLiteral( "Unable to publish the saved asset directory." ) );
            return;
          }
          d->assetsPublished = true;
          QSaveFile file( d->destination );
          file.setDirectWriteFallback( false );
          if( !file.open( QIODevice::WriteOnly ) )
          {
            d->finishFailure( QStringLiteral( "Unable to open the HTML destination: %1" ).arg( file.errorString() ) );
            return;
          }
          QByteArray const bytes = d->rewritten.toUtf8();
          if( file.write( bytes ) != bytes.size() || !file.commit() )
          {
            d->finishFailure( QStringLiteral( "Unable to commit the HTML destination: %1" ).arg( file.errorString() ) );
            return;
          }
          d->htmlPublished = true;
          d->finishSuccess();
        }, Qt::QueuedConnection );
      }
    }
  } );
}
