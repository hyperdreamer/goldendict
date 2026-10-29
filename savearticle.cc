/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file. */
#include "savearticle.hh"

#include "articlebridge.hh"
#include "article_netmgr.hh"
#include "articleview.hh"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QPointer>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QWebEnginePage>

namespace
{
QString canonicalResourceKey( QUrl url )
{
  // Fragments identify content inside one byte stream. Queries identify
  // different streams and remain part of both the key and the local filename.
  url.setFragment( QString() );
  return QString::fromLatin1( url.toEncoded( QUrl::FullyEncoded ) );
}

QString resourceRelativePath( QUrl const & url, int & queryNumber )
{
  QString path = url.path( QUrl::FullyDecoded );
  while( path.startsWith( QLatin1Char( '/' ) ) ) path.remove( 0, 1 );
  if( path.isEmpty() ) path = QStringLiteral( "resource" );
  QString const host = url.host();
  QString relative = host.isEmpty() ? path : host + QLatin1Char( '/' ) + path;
  if( url.hasQuery() ) relative += QStringLiteral( "_q%1" ).arg( queryNumber++ );
  return relative;
}

bool pathIsInside( QString const & root, QString const & candidate )
{
  QString const cleanRoot = QDir::cleanPath( QDir( root ).absolutePath() );
  QString const cleanCandidate = QDir::cleanPath( QFileInfo( candidate ).absoluteFilePath() );
  return !candidate.contains( QChar::Null ) &&
         cleanCandidate.startsWith( cleanRoot + QLatin1Char( '/' ) );
}
} // namespace

struct SaveArticle::Private
{
  struct Download
  {
    QUrl url;
    QString path;
    QPointer< ResourceToSaveHandler > handler;
    QPointer< QNetworkReply > reply;
    bool done = false;
  };

  SaveArticle * const owner;
  ArticleNetworkAccessManager & resources;
  QPointer< ArticleView > view;
  QPointer< QWebEnginePage > page;
  QPointer< ArticleBridge > bridge;
  QVector< QMetaObject::Connection > guards;
  State currentState = State::Idle;
  quint64 operation = 0;
  int generation = 0;
  bool active = false, complete = false, assetsPublished = false, htmlPublished = false;
  QString destination, rewritten, destinationParent, staging, stagedAssets, finalAssets;
  QUrl baseUrl;
  QVector< Download > downloads;

  Private( SaveArticle * owner_, ArticleView * view_, ArticleNetworkAccessManager & resources_ ):
    owner( owner_ ), resources( resources_ ), view( view_ ) {}

  bool valid( quint64 token ) const noexcept
  {
    return active && operation == token && view && page && bridge && bridge->generation() == generation;
  }

  void disconnectGuards()
  {
    for( auto const & connection : guards ) QObject::disconnect( connection );
    guards.clear();
  }

  void releaseHandlers( ResourceToSaveHandler * emittingHandler = nullptr )
  {
    auto handles = std::move( downloads );
    downloads.clear();
    QPointer< SaveArticle > guard( owner );
    for( auto const & download : handles )
    {
      if( download.reply )
      {
        if( guard ) QObject::disconnect( download.reply, nullptr, guard, nullptr );
        if( download.reply->isRunning() ) download.reply->abort();
        if( download.reply ) download.reply->deleteLater();
      }
      if( !download.handler ) continue;
      if( guard ) QObject::disconnect( download.handler, nullptr, guard, nullptr );
      // ResourceToSaveHandler continues executing after its done signal.
      if( download.handler == emittingHandler ) download.handler->deleteLater();
      else delete download.handler.data();
    }
  }

  void removeOwnedPaths()
  {
    // A successful rename is the only way we acquire the final directory.
    // Merely choosing its name (including a failed rename collision) is not
    // ownership. The HTML destination is never removed during rollback.
    if( assetsPublished && !htmlPublished ) QDir( finalAssets ).removeRecursively();
    if( !staging.isEmpty() ) QDir( staging ).removeRecursively();
    assetsPublished = false;
    staging.clear(); stagedAssets.clear(); finalAssets.clear();
  }

  void finishFailure( QString const & message, ResourceToSaveHandler * emittingHandler = nullptr )
  {
    if( !active ) return;
    active = false;
    disconnectGuards();
    QPointer< SaveArticle > guard( owner );
    releaseHandlers( emittingHandler );
    if( !guard ) return;
    removeOwnedPaths();
    currentState = State::Failed;
    emit owner->failed( message );
  }

  void resourceFailure( int index, QString const & reason, ResourceToSaveHandler * handler = nullptr )
  {
    QString const url = downloads.at( index ).url.toString( QUrl::RemovePassword );
    finishFailure( QStringLiteral( "Unable to save resource %1: %2" ).arg( url, reason ), handler );
  }

  void finishCancelled()
  {
    if( !active ) return;
    active = false; // Retire all continuations before cancelling their producers.
    disconnectGuards();
    QPointer< SaveArticle > guard( owner );
    releaseHandlers();
    if( !guard ) return;
    removeOwnedPaths();
    currentState = State::Cancelled;
    emit owner->cancelled( QStringLiteral( "Save cancelled because the article changed/closed." ) );
  }

  void publish( quint64 token )
  {
    if( !valid( token ) ) return;
    if( complete )
    {
      QDir parent( destinationParent );
      if( !parent.rename( stagedAssets, finalAssets ) )
      {
        finishFailure( QStringLiteral( "Unable to publish the saved asset directory." ) );
        return;
      }
      assetsPublished = true;
    }

    QSaveFile file( destination );
    file.setDirectWriteFallback( false );
    if( !file.open( QIODevice::WriteOnly ) )
    {
      finishFailure( QStringLiteral( "Unable to open the HTML destination: %1" ).arg( file.errorString() ) );
      return;
    }
    QByteArray const bytes = rewritten.toUtf8();
    if( file.write( bytes ) != bytes.size() || !file.commit() )
    {
      finishFailure( QStringLiteral( "Unable to commit the HTML destination: %1" ).arg( file.errorString() ) );
      return;
    }
    // HTML publication is the commit point: cancellation can no longer retract
    // either this HTML or the immutable directory it references.
    htmlPublished = true;
    active = false;
    disconnectGuards();
    releaseHandlers();
    if( !staging.isEmpty() ) QDir( staging ).removeRecursively();
    staging.clear(); stagedAssets.clear();
    currentState = State::Complete;
    emit owner->saved( destination );
  }

  void maybePublish( quint64 token )
  {
    if( !valid( token ) || currentState != State::Downloading ) return;
    if( !std::all_of( downloads.cbegin(), downloads.cend(), []( Download const & item ) { return item.done; } ) )
      return;
    currentState = State::Publishing;
    QMetaObject::invokeMethod( owner, [this, token] { publish( token ); }, Qt::QueuedConnection );
  }

  void downloaded( quint64 token, int index )
  {
    if( !valid( token ) || index >= downloads.size() ) return;
    Download & download = downloads[ index ];
    QFileInfo const file( download.path );
    if( !file.isFile() || file.isSymLink() )
    {
      resourceFailure( index, QStringLiteral( "The download or resource write failed." ), download.handler );
      return;
    }
    download.done = true; // A successfully committed zero-byte resource is valid.
    maybePublish( token );
  }

  void captured( quint64 token, QString const & html )
  {
    if( !valid( token ) ) return;
    currentState = State::Collecting;
    auto const spans = complete ? SaveArticleDetail::collectResourceSpans( html, baseUrl ) :
                                 QVector< SaveArticleDetail::ResourceSpan >();
    QFileInfo const destinationInfo( destination );
    destinationParent = destinationInfo.absolutePath();
    if( !QDir( destinationParent ).exists() )
    {
      finishFailure( QStringLiteral( "The destination directory does not exist." ) );
      return;
    }
    rewritten = html;
    if( complete )
    {
      QDir const parent( destinationParent );
      QString const stem = destinationInfo.completeBaseName().isEmpty() ? QStringLiteral( "article" ) :
                                                                         destinationInfo.completeBaseName();
      QString const id = QUuid::createUuid().toString( QUuid::WithoutBraces );
      QString const stagingName = QStringLiteral( ".%1.goldendict-save-%2" ).arg( stem, id );
      // mkdir succeeds only for a newly created directory. Do not remember a
      // colliding path as ours, even if its name looks like a save operation.
      if( !parent.mkdir( stagingName ) )
      {
        finishFailure( QStringLiteral( "Unable to create a unique save staging directory." ) );
        return;
      }
      staging = parent.filePath( stagingName );
      stagedAssets = QDir( staging ).filePath( QStringLiteral( "assets" ) );
      if( !QDir().mkdir( stagedAssets ) )
      {
        finishFailure( QStringLiteral( "Unable to create the save asset directory." ) );
        return;
      }
      finalAssets = parent.filePath( QStringLiteral( "%1_files-%2" ).arg( stem, id ) );

      struct Replacement { qsizetype offset, length; QString text; };
      QVector< Replacement > replacements;
      QMap< QString, int > jobsByUrl;
      QSet< QString > usedPaths;
      int queryNumber = 1;
      for( auto const & span : spans )
      {
        QString const key = canonicalResourceKey( span.url );
        int index = jobsByUrl.value( key, -1 );
        if( index < 0 )
        {
          QString const relative = resourceRelativePath( span.url, queryNumber );
          QString path = QDir( stagedAssets ).filePath( relative );
          if( !pathIsInside( stagedAssets, path ) )
          {
            finishFailure( QStringLiteral( "A resource path escapes the save staging directory: %1" )
                             .arg( span.url.toString( QUrl::RemovePassword ) ) );
            return;
          }
          QString const original = path;
          for( int suffix = 1; usedPaths.contains( QDir::cleanPath( path ) ); ++suffix )
            path = original + QStringLiteral( "_r%1" ).arg( suffix );
          usedPaths.insert( QDir::cleanPath( path ) );
          index = downloads.size();
          jobsByUrl.insert( key, index );
          downloads.append( { span.url, path, nullptr, nullptr, false } );
        }
        QString const relative = QDir( stagedAssets ).relativeFilePath( downloads.at( index ).path );
        QString encoded = QString::fromLatin1( QUrl::toPercentEncoding(
          QDir( finalAssets ).dirName() + QLatin1Char( '/' ) + relative, "/" ) );
        if( span.url.hasFragment() ) encoded += QLatin1Char( '#' ) + span.url.fragment( QUrl::FullyEncoded );
        replacements.append( { span.offset, span.length, encoded } );
      }
      std::sort( replacements.begin(), replacements.end(), []( Replacement const & a, Replacement const & b ) {
        return a.offset > b.offset;
      } );
      for( auto const & replacement : replacements )
        rewritten.replace( replacement.offset, replacement.length, replacement.text );
    }

    currentState = State::Downloading;
    QPointer< SaveArticle > guard( owner );
    for( int index = 0; index < downloads.size(); ++index )
    {
      QUrl const url = downloads.at( index ).url;
      QString const path = downloads.at( index ).path;
      if( url.scheme() == QLatin1String( "http" ) || url.scheme() == QLatin1String( "https" ) )
      {
        QNetworkRequest request( url );
        request.setMaximumRedirectsAllowed( 10 );
        auto * reply = resources.get( request );
        if( !guard ) { reply->abort(); reply->deleteLater(); return; }
        if( !valid( token ) ) { reply->abort(); reply->deleteLater(); return; }
        downloads[ index ].reply = reply;
        QObject::connect( reply, &QNetworkReply::finished, owner, [this, token, index] {
          if( !valid( token ) || index >= downloads.size() ) return;
          auto reply = downloads.at( index ).reply;
          if( !reply || reply->error() != QNetworkReply::NoError )
          {
            resourceFailure( index, reply ? reply->errorString() : QStringLiteral( "Download interrupted." ) );
            return;
          }
          QSaveFile file( downloads.at( index ).path );
          file.setDirectWriteFallback( false );
          QByteArray const bytes = reply->readAll();
          if( !QDir().mkpath( QFileInfo( file.fileName() ).absolutePath() ) ||
              !file.open( QIODevice::WriteOnly ) || file.write( bytes ) != bytes.size() || !file.commit() )
          {
            resourceFailure( index, QStringLiteral( "Resource write failed: %1" ).arg( file.errorString() ) );
            return;
          }
          downloaded( token, index );
        } );
        QObject::connect( reply, &QObject::destroyed, owner, [this, token, index] {
          if( valid( token ) && index < downloads.size() && !downloads.at( index ).done )
            resourceFailure( index, QStringLiteral( "The HTTP download was interrupted." ) );
        } );
        continue;
      }
      // Keep the view's real dictionary/group-search routing; the controller
      // owns every resulting handler until its transaction terminates.
      auto * handler = view->saveResource( url, baseUrl, path );
      if( !guard ) { if( handler ) handler->deleteLater(); return; }
      if( !valid( token ) ) { delete handler; return; }
      if( !handler )
      {
        resourceFailure( index, QStringLiteral( "Unable to create a download handler." ) );
        return;
      }
      handler->setParent( owner );
      downloads[ index ].handler = handler;
      QObject::connect( handler, &ResourceToSaveHandler::done, owner, [this, token, index] {
        downloaded( token, index );
      } );
      QObject::connect( handler, &QObject::destroyed, owner, [this, token, index] {
        if( valid( token ) && index < downloads.size() && !downloads.at( index ).done )
          resourceFailure( index, QStringLiteral( "The resource download was interrupted." ) );
      } );
      // Instant requests finish inside saveResource, before connections can be
      // installed. Their committed file is the same success marker used above.
      if( handler->isEmpty() )
      {
        downloaded( token, index );
        if( !guard || !valid( token ) ) return;
      }
    }
    maybePublish( token );
  }
};

SaveArticle::SaveArticle( ArticleView * view, ArticleNetworkAccessManager & resources, QObject * parent ):
  QObject( parent ), d( std::make_unique< Private >( this, view, resources ) ) {}

SaveArticle::~SaveArticle()
{
  d->active = false;
  d->disconnectGuards();
  d->releaseHandlers();
  d->removeOwnedPaths();
}

SaveArticle::State SaveArticle::state() const noexcept { return d->currentState; }
void SaveArticle::cancel() noexcept { d->finishCancelled(); }

void SaveArticle::start( QString const & htmlDestination, bool complete )
{
  QPointer< SaveArticle > guard( this );
  quint64 const previous = d->operation;
  if( d->active )
  {
    d->finishCancelled();
    // A cancellation observer may destroy us or start a replacement itself.
    if( !guard || d->operation != previous || d->active ) return;
  }
  quint64 const token = ++d->operation;
  d->active = true;
  d->complete = complete;
  d->assetsPublished = d->htmlPublished = false;
  d->destination = htmlDestination;
  d->currentState = State::Capturing;
  if( !d->view )
  {
    d->finishFailure( QStringLiteral( "Cannot save an article after its view was closed." ) );
    return;
  }
  d->page = d->view->findChild< QWebEnginePage * >();
  d->bridge = d->view->findChild< ArticleBridge * >( QStringLiteral( "gdArticleBridge" ) );
  if( !d->page || !d->bridge )
  {
    d->finishFailure( QStringLiteral( "Cannot save an article without its live renderer bridge." ) );
    return;
  }
  d->baseUrl = d->page->url();
  d->generation = d->bridge->generation();
  auto cancel = [guard, token] {
    if( guard && guard->d->active && guard->d->operation == token ) guard->cancel();
  };
  d->guards.append( connect( d->view, &QObject::destroyed, this, cancel ) );
  d->guards.append( connect( d->page, &QObject::destroyed, this, cancel ) );
  d->guards.append( connect( d->bridge, &QObject::destroyed, this, cancel ) );
  d->guards.append( connect( d->bridge, &ArticleBridge::generationChanged, this,
    [guard, token]( int generation ) {
      if( guard && guard->d->active && guard->d->operation == token && generation != guard->d->generation )
        guard->cancel();
    } ) );
  d->view->toHtml( [guard, token]( QString const & html ) {
    if( guard ) guard->d->captured( token, html );
  } );
}
