/* This file is (c) 2026 GoldenDict contributors.
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file. */

#include "articleschemes.hh"
#include "article_netmgr.hh"
#include "articleurls.hh"

#include <QBuffer>
#include <QDebug>
#include <QHash>
#include <QHostAddress>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>

namespace {

class DictionaryDataRequestReader final : public DataRequestReader
{
public:
  QByteArray fullData( Dictionary::DataRequest & request ) const override
  {
    // getFullData supplies its own lock and preserves exRequestUnfinished.
    std::vector< char > const bytes = request.getFullData();
    return bytes.empty() ? QByteArray() : QByteArray( bytes.data(), qsizetype( bytes.size() ) );
  }

  void dataSlice( Dictionary::DataRequest & request, size_t offset, size_t size,
                  void * destination ) const override
  {
    request.getDataSlice( offset, size, destination );
  }

  void copyStreamBytes( char * destination, char const * source, qint64 count ) const override
  {
    if( count > 0 )
      std::memcpy( destination, source, size_t( count ) );
  }
};

struct Range
{
  std::optional< qint64 > first, last;
};

struct RangeBounds
{
  qint64 start, end, total;
};

QByteArray trimLws( QByteArray value )
{
  while( !value.isEmpty() && ( value.front() == ' ' || value.front() == '\t' ) )
    value.remove( 0, 1 );
  while( !value.isEmpty() && ( value.back() == ' ' || value.back() == '\t' ) )
    value.chop( 1 );
  return value;
}

std::optional< Range > parseRange( QByteArray const & header )
{
  // Match the pinned Chromium HttpUtil::ParseRangeHeader/HttpByteRange rules,
  // because the *loader* parses the same header and its interpretation wins for
  // the response metadata: case-insensitive unit, LWS, signed int64 conversion,
  // empty comma tokens skipped and -1 as the unspecified-last sentinel. Anything
  // this function rejects stays a plain full-body response; anything it accepts
  // must be positioned exactly where Qt will interpret the range, or the reply
  // would carry a Content-Range that does not describe the bytes delivered.
  //
  // Spec §4.5 also asks for "checked nonnegative decimal" fields, which cannot
  // hold together with the loader-acceptance clause: a strict reader would hand
  // Qt position 0 with a suffix Content-Range it resolves itself, i.e. prefix
  // bytes labelled as the representation's suffix (recorded spec defect).
  int const equals = header.indexOf( '=' );
  if( equals < 0 || trimLws( header.left( equals ) ).compare( "bytes", Qt::CaseInsensitive ) != 0 )
    return {};

  static QRegularExpression const integer( QStringLiteral( "\\A[+-]?[0-9]+\\z" ) );
  auto number = []( QByteArray const & text ) -> std::optional< qint64 > {
    if( !integer.match( QString::fromLatin1( text ) ).hasMatch() )
      return {};
    bool ok = false;
    qint64 const value = text.toLongLong( &ok );
    return ok ? std::optional< qint64 >( value ) : std::nullopt;
  };

  std::optional< Range > result;
  for( QByteArray token : header.mid( equals + 1 ).split( ',' ) )
  {
    token = trimLws( token );
    if( token.isEmpty() )
      continue;
    int const dash = token.indexOf( '-' );
    if( result || dash < 0 )
      return {};
    QByteArray const first = trimLws( token.left( dash ) );
    QByteArray const last = trimLws( token.mid( dash + 1 ) );
    Range range;
    if( !first.isEmpty() )
    {
      range.first = number( first );
      if( !range.first || *range.first < 0 )
        return {};
    }
    if( !last.isEmpty() )
    {
      range.last = number( last );
      if( !range.last )
        return {};
      if( range.first && *range.last == -1 )
        range.last.reset();
    }
    if( ( !range.first && ( !range.last || *range.last <= 0 ) )
        || ( range.first && range.last && *range.last < *range.first ) )
      return {};
    result = range;
  }
  return result;
}

QString canonicalHost( QString host )
{
  host = host.toLower();
  if( host.endsWith( '.' ) ) host.chop( 1 );
  QHostAddress address;
  if( address.setAddress( host ) ) return address.toString();
  return QString::fromLatin1( QUrl::toAce( host ) );
}

// Retain the historical short-final-label heuristic. This intentionally is not
// a public-suffix-list implementation; IP addresses never reach it.
QString getHostBase( QString const & host )
{
  QStringList const domains = host.split( '.' );
  int left = domains.size();
  if( left && domains[ left - 1 ].size() <= 3 ) --left;
  if( left && domains[ left - 1 ].size() <= 3 ) --left;
  return left > 1 ? host.mid( domains[ 0 ].size() + 1 ) : host;
}

bool hostBelongsTo( QString const & host, QString const & parent )
{
  if( host.isEmpty() || parent.isEmpty() ) return false;
  QHostAddress a, b;
  bool const aIsIp = a.setAddress( host ), bIsIp = b.setAddress( parent );
  if( aIsIp || bIsIp ) return aIsIp && bIsIp && a == b;
  return host == parent || host.endsWith( '.' + parent );
}

bool sameSite( QString const & a, QString const & b )
{
  if( a.isEmpty() || b.isEmpty() ) return false;
  QHostAddress ipA, ipB;
  if( ipA.setAddress( a ) || ipB.setAddress( b ) )
    return hostBelongsTo( a, b );
  return hostBelongsTo( a, b ) || hostBelongsTo( b, a ) || getHostBase( a ) == getHostBase( b );
}

bool internalScheme( QString const & scheme )
{
  return scheme == QLatin1String( "gdlookup" ) || scheme == QLatin1String( "gdpicture" )
      || scheme == QLatin1String( "bres" ) || scheme == QLatin1String( "gdau" )
      || scheme == QLatin1String( "gdvideo" ) || scheme == QLatin1String( "gico" )
      || scheme == QLatin1String( "qrcx" );
}

} // namespace

ArticleRequestInterceptor::ArticleRequestInterceptor( QObject * parent ):
  QWebEngineUrlRequestInterceptor( parent ), policy( std::make_shared< ArticleRequestPolicy const >() ) {}

void ArticleRequestInterceptor::setPolicy( std::shared_ptr< ArticleRequestPolicy const > replacement )
{
  if( !replacement ) replacement = std::make_shared< ArticleRequestPolicy const >();
  std::atomic_store( &policy, std::move( replacement ) );
}

void ArticleRequestInterceptor::interceptRequest( QWebEngineUrlRequestInfo & info )
{
  auto const snapshot = std::atomic_load( &policy );
  QUrl const url = info.requestUrl();
  QString const scheme = url.scheme();
  bool deny = !url.isValid() || scheme.isEmpty();
  if( !deny && internalScheme( scheme ) )
  {
    auto const kind = ArticleUrls::parse( url, ArticleUrls::Use::DocumentLoad ).kind;
    deny = kind == ArticleUrls::Kind::Invalid || kind == ArticleUrls::Kind::ExternalAction;
    if( !deny ) return;
  }
  else if( !deny && scheme == QLatin1String( "file" ) )
  {
    auto const fixed = snapshot->localFileRedirects.constFind( url.toString( QUrl::FullyEncoded ) );
    if( fixed != snapshot->localFileRedirects.cend() && fixed->isValid() && fixed->isLocalFile()
        && *fixed != url )
      info.redirect( *fixed );
    return;
  }
  else if( !deny && ( scheme == QLatin1String( "data" ) || scheme == QLatin1String( "about" )
                      || scheme == QLatin1String( "blob" ) ) ) return;
  else if( !deny && !snapshot->blockExternal ) return;
  else if( !deny && ( scheme == QLatin1String( "http" ) || scheme == QLatin1String( "https" ) ) )
  {
    QString const requestHost = canonicalHost( url.host() );
    QString const firstPartyHost = canonicalHost( info.firstPartyUrl().host() );
    QUrl const initiator = info.initiator();
    QString const initiatorScheme = initiator.scheme();
    bool const concrete = initiatorScheme == QLatin1String( "http" )
                       || initiatorScheme == QLatin1String( "https" );
    if( sameSite( requestHost, firstPartyHost )
        || ( concrete && sameSite( requestHost, canonicalHost( initiator.host() ) ) ) ) return;
    bool const application = initiatorScheme == QLatin1String( "gdlookup" )
                         || initiatorScheme == QLatin1String( "bres" )
                         || initiatorScheme == QLatin1String( "gico" );
    if( application )
      for( auto const & host : snapshot->dictionaryHosts )
        if( hostBelongsTo( requestHost, canonicalHost( host ) ) ) return;
    deny = true;
  }
  else deny = true;

  if( deny )
  {
    info.block( true );
    std::lock_guard< std::mutex > lock( diagnosticMutex );
    if( diagnosticPolicy.lock() != snapshot )
    {
      diagnosticPolicy = snapshot;
      deniedUrls.clear();
    }
    if( !deniedUrls.contains( url ) )
    {
      deniedUrls.insert( url );
      qWarning() << "ArticlePolicy: blocked external request" << url.adjusted( QUrl::RemoveUserInfo );
    }
  }
}

void registerArticleSchemes()
{
  static std::once_flag registered;
  std::call_once( registered, [] {
    using Scheme = QWebEngineUrlScheme;
    for( QByteArray const name : { "gdlookup", "gdpicture", "bres", "gdau", "gdvideo", "gico", "qrcx" } )
    {
      Scheme scheme( name );
      scheme.setSyntax( Scheme::Syntax::Host );
      scheme.setFlags( Scheme::SecureScheme | Scheme::LocalScheme | Scheme::LocalAccessAllowed
                       | Scheme::CorsEnabled );
      Scheme::registerScheme( scheme );
    }
  } );
}

std::shared_ptr< DataRequestReader const > makeDataRequestReader()
{
  return std::make_shared< DictionaryDataRequestReader >();
}

struct ArticleStreamDevice::State
{
  // IO methods must never access Producer, even to copy/release its sptr.
  mutable std::mutex mutex;
  QByteArray bytes;
  qint64 readOffset = 0;
  bool completed = false, failed = false, cancelled = false;
  bool failureNotified = false, completionNotified = false;
  QString failureMessage;
  std::shared_ptr< DataRequestReader const > const reader;

  explicit State( std::shared_ptr< DataRequestReader const > reader_ ): reader( std::move( reader_ ) ) {}

  void fail( QString message )
  {
    if( failed || cancelled ) return;
    failed = true;
    failureMessage = std::move( message );
  }
};

struct ArticleStreamDevice::Producer
{
  sptr< Dictionary::DataRequest > request;
  QList< QMetaObject::Connection > connections;
  bool cancelled = false;
  explicit Producer( sptr< Dictionary::DataRequest > const & request_ ): request( request_ ) {}
};

ArticleStreamDevice::ArticleStreamDevice( sptr< Dictionary::DataRequest > const & request,
    std::shared_ptr< DataRequestReader const > reader, QObject * jobParent ):
  QIODevice( jobParent ), state( std::make_unique< State >( std::move( reader ) ) ),
  producer( std::make_unique< Producer >( request ) )
{
  Q_ASSERT( QThread::currentThread() == thread() );
  open( QIODevice::ReadOnly | QIODevice::Unbuffered );
  if( request )
  {
    producer->connections.append( connect( request.get(), &Dictionary::DataRequest::updated,
        this, [this] { pump(); }, Qt::QueuedConnection ) );
    producer->connections.append( connect( request.get(), &Dictionary::DataRequest::finished,
        this, [this] { pump(); }, Qt::QueuedConnection ) );
  }
  // Initial staging is synchronous, before the handler can hand us to Qt.
  // Notifications are queued so the handler can connect before initial failure
  // or successful empty completion is published.
  pump();
}

ArticleStreamDevice::~ArticleStreamDevice()
{
  cancel();
}

bool ArticleStreamDevice::isSequential() const { return true; }
qint64 ArticleStreamDevice::size() const { return 0; }

qint64 ArticleStreamDevice::bytesAvailable() const
{
  std::lock_guard< std::mutex > lock( state->mutex );
  return state->bytes.size() - state->readOffset;
}

bool ArticleStreamDevice::atEnd() const
{
  std::lock_guard< std::mutex > lock( state->mutex );
  return state->cancelled || ( state->completed && !state->failed && state->readOffset == state->bytes.size() );
}

void ArticleStreamDevice::publish( bool appended )
{
  // Called after releasing the state mutex, including on Chromium's IO thread.
  // Only this GUI continuation emits device signals; no producer is captured.
  QMetaObject::invokeMethod( this, [this, appended] {
    QString message;
    bool failure = false, completion = false, wake = false;
    {
      std::lock_guard< std::mutex > lock( state->mutex );
      if( state->failed )
      {
        // Failure selected before loader close still has to be reported.
        if( state->failureNotified ) return;
        state->failureNotified = true;
        failure = true;
        message = state->failureMessage;
      }
      else
      {
        wake = appended && !state->cancelled;
        // A successful producer completion selected before the loader drains
        // an empty body still notifies once, even if IO close wins the race
        // with this queued GUI delivery. Cancelling an unfinished producer
        // never sets completed.
        if( state->completed && !state->completionNotified )
        {
          state->completionNotified = true;
          completion = true;
        }
        else if( state->failed && !state->failureNotified )
        {
          // A failure that latched after the completion was selected wins; the
          // completion observer must not report success afterwards.
          state->failureNotified = true;
          failure = true;
          message = state->failureMessage;
          completion = false;
        }
      }
    }
    // Slots may reenter the device or cancel it: never emit under the mutex.
    QPointer< ArticleStreamDevice > alive( this );

    // Between two emissions a slot can drive the producer far enough for a copy
    // failure to latch. Success notifications are therefore re-validated against
    // the terminal state before each emission: failure wins over a completion that
    // was merely selected earlier.
    auto failureWon = [ this ] {
      std::lock_guard< std::mutex > lock( state->mutex );
      return state->failed;
    };

    if( failure )
    {
      emit readyRead();
      if( alive ) emit failed( message );
      return;
    }
    if( wake ) emit readyRead();
    if( !alive ) return;
    if( completion )
    {
      emit readyRead();
      if( !alive || failureWon() ) return;
      emit completed();
      if( alive && !failureWon() ) emit readChannelFinished();
    }
  }, Qt::QueuedConnection );
}

void ArticleStreamDevice::pump()
{
  Q_ASSERT( QThread::currentThread() == thread() );
  bool appended = false, changed = false;
  {
    std::lock_guard< std::mutex > lock( state->mutex );
    if( state->cancelled || state->failed || state->completed ) return;
    try
    {
      if( !producer->request || !state->reader )
        state->fail( QStringLiteral( "no article stream reader or producer" ) );
      else
      {
        // The producer may publish its final bytes, its error or its completion
        // while a slice is being copied, so the terminal state is sampled first
        // and the loop re-samples after every append: a slice that raced with the
        // producer's final append is followed by another iteration that copies the
        // rest, instead of marking the stream complete with stale bytes.
        for( int guard = 0; guard < 1000000; ++guard )
        {
          bool const finished = producer->request->isFinished();

          QString const error = producer->request->getErrorString();
          if( !error.isEmpty() )
          {
            state->fail( error );
            break;
          }

          qint64 const available = producer->request->dataSize();
          if( available < 0 )
          {
            // No bytes and no more coming: a finished producer with a negative
            // size is the resource-not-found case.
            if( finished )
              state->fail( QStringLiteral( "article stream resource not found" ) );
            break;
          }

          if( available > state->bytes.size() )
          {
            qint64 const count = available - state->bytes.size();
            QByteArray delta( qsizetype( count ), Qt::Uninitialized );
            state->reader->dataSlice( *producer->request, size_t( state->bytes.size() ),
                                      size_t( count ), delta.data() );
            // Neither append length nor read offset changes on a thrown slice.
            state->bytes.append( delta );
            appended = true;
            // The slice may have taken long enough for the producer to finish and
            // publish more bytes; re-sample rather than trusting `available`.
            continue;
          }

          if( finished )
            state->completed = true;
          break;
        }
      }
    }
    catch( std::exception const & error ) { state->fail( QString::fromUtf8( error.what() ) ); }
    catch( ... ) { state->fail( QStringLiteral( "article stream reader failed" ) ); }
    changed = appended || state->failed || state->completed;
  }
  if( changed ) publish( appended );
}

qint64 ArticleStreamDevice::readData( char * data, qint64 maxSize )
{
  {
    std::lock_guard< std::mutex > lock( state->mutex );
    if( state->cancelled ) return -1;
    if( maxSize == 0 ) return 0;
    if( maxSize < 0 ) return -1;
    qint64 const count = std::min( maxSize, qint64( state->bytes.size() ) - state->readOffset );
    if( count > 0 )
    {
      try
      {
        state->reader->copyStreamBytes( data, state->bytes.constData() + state->readOffset, count );
        state->readOffset += count;
        return count;
      }
      catch( std::exception const & error ) { state->fail( QString::fromUtf8( error.what() ) ); }
      catch( ... ) { state->fail( QStringLiteral( "article stream copy failed" ) ); }
    }
    else return state->failed || state->completed ? -1 : 0;
  }
  // Failed copy: offset is unchanged. Queue the wakeup only after unlocking.
  publish( false );
  return -1;
}

qint64 ArticleStreamDevice::writeData( char const *, qint64 ) { return -1; }

void ArticleStreamDevice::cancel() noexcept
{
  Q_ASSERT( QThread::currentThread() == thread() );
  {
    std::lock_guard< std::mutex > lock( state->mutex );
    state->cancelled = true;
  }
  if( producer->cancelled ) return;
  producer->cancelled = true;
  for( auto const & connection : producer->connections ) disconnect( connection );
  producer->connections.clear();
  // Producer cancellation may synchronously emit finished and reenter cancel.
  // Keep a local owning handle on GUI, after dropping the state lock.
  auto request = producer->request;
  producer->request.reset(); // sptr has no move constructor; keep every ref change on GUI.
  if( request )
    try { request->cancel(); } catch( ... ) {}
}

void ArticleStreamDevice::close()
{
  {
    std::lock_guard< std::mutex > lock( state->mutex );
    state->cancelled = true;
  }
  QIODevice::close();
  // This closure owns no request/connection/job/handler reference. Qt removes
  // it on device destruction; the job remains the sole QObject owner.
  QMetaObject::invokeMethod( this, [this] { cancel(); }, Qt::QueuedConnection );
}

struct ArticleSchemeHandler::State
{
  struct Job
  {
    QPointer< QWebEngineUrlRequestJob > job;
    sptr< Dictionary::DataRequest > request;
    std::shared_ptr< DataRequestReader const > reader;
    QPointer< QIODevice > device;
    QList< QMetaObject::Connection > connections;
    QByteArray mime;
    QUrl url;
    bool replied = false, terminal = false, cancelled = false;
    bool rangeRequested = false, live = false;
    std::optional< Range > range;
    std::optional< RangeBounds > rangeBounds;
    QString failureMessage;
    bool failureNotified = false;
  };

  ArticleSchemeHandler & owner;
  ArticleNetworkAccessManager & resources;
  DataRequestReaderFactory readerFactory;
  QHash< QWebEngineUrlRequestJob *, std::shared_ptr< Job > > jobs;

  State( ArticleSchemeHandler & owner_, ArticleNetworkAccessManager & resources_,
         DataRequestReaderFactory factory ):
    owner( owner_ ), resources( resources_ ), readerFactory( std::move( factory ) ) {}

  static void disconnectProducer( Job & record )
  {
    for( auto const & connection : record.connections )
      QObject::disconnect( connection );
    record.connections.clear();
  }

  static void cancel( Job & record ) noexcept
  {
    if( record.cancelled || !record.request )
      return;
    // Set before calling a producer: cancel() is allowed to emit finished.
    record.cancelled = true;
    if( auto stream = qobject_cast< ArticleStreamDevice * >( record.device.data() ) )
      stream->cancel();
    else
      try { record.request->cancel(); }
    catch( ... ) { /* Destruction/cancellation cannot propagate producer errors. */ }
  }

  void forget( QWebEngineUrlRequestJob * key, std::shared_ptr< Job > const & record )
  {
    auto const it = jobs.find( key );
    if( it == jobs.end() || *it != record )
      return;
    jobs.erase( it );
    disconnectProducer( *record );
    cancel( *record );
    record->request.reset();
    record->reader.reset();
    // The job owns the device, including when Chromium is still reading it.
  }

  void shutdown()
  {
    auto remaining = std::move( jobs );
    jobs.clear();
    for( auto const & record : remaining )
    {
      disconnectProducer( *record );
      if( record->job && !record->terminal && !record->replied )
      {
        record->terminal = true;
        record->job->fail( QWebEngineUrlRequestJob::RequestAborted );
      }
      cancel( *record );
    }
  }

  void fail( std::shared_ptr< Job > const & record, QWebEngineUrlRequestJob::Error error,
             QString message = {} )
  {
    if( record->terminal || record->replied || record->cancelled || !record->job )
      return;
    record->terminal = true;
    record->failureMessage = std::move( message );
    bool const notify = error == QWebEngineUrlRequestJob::RequestFailed && !record->failureNotified;
    record->failureNotified = notify || record->failureNotified;
    record->job->fail( error );
    // Emitting last permits a GUI receiver to dispose of the handler.
    if( notify )
      emit owner.producerFailed( record->url, record->failureMessage );
  }

  void inspect( std::shared_ptr< Job > const & record )
  {
    Q_ASSERT( QThread::currentThread() == owner.thread() );
    if( record->terminal || record->replied || record->cancelled || !record->job || record->device )
      return;

    try
    {
      // Error wins even when a producer has already published partial bytes.
      QString const error = record->request->getErrorString();
      if( !error.isEmpty() )
      {
        fail( record, QWebEngineUrlRequestJob::RequestFailed, error );
        return;
      }
      bool const finished = record->request->isFinished();
      qint64 const available = record->request->dataSize();
      if( finished && available < 0 )
      {
        fail( record, QWebEngineUrlRequestJob::UrlNotFound );
        return;
      }
      if( record->live && !record->rangeRequested )
      {
        if( available <= 0 && !finished ) return;
        auto stream = new ArticleStreamDevice( record->request, record->reader, record->job );
        record->device = stream;
        record->connections.append( QObject::connect( stream, &ArticleStreamDevice::failed, &owner,
          [this, record]( QString const & message ) {
            if( record->failureNotified || record->cancelled || !record->job ) return;
            if( !record->replied )
            {
              fail( record, QWebEngineUrlRequestJob::RequestFailed, message );
              return;
            }
            record->terminal = true;
            record->failureNotified = true;
            record->failureMessage = message;
            emit owner.producerFailed( record->url, message );
          } ) );
        bool initiallyFailed;
        {
          // This is still before handoff. GUI never calls device IO virtuals
          // after reply; only the queued failure/cancellation protocol remains.
          std::lock_guard< std::mutex > lock( stream->state->mutex );
          initiallyFailed = stream->state->failed;
        }
        // The initial pump already queued failed(). Let that notification
        // select RequestFailed before Qt can destroy the job-owned device.
        // inspect() ignores this installed device while delivery is pending.
        if( initiallyFailed ) return;
        record->replied = true;
        record->job->reply( record->mime, stream );
        return;
      }
      if( !finished ) return;
      QByteArray const bytes = record->reader->fullData( *record->request );
      qint64 const total = bytes.size();
      qint64 position = 0, last = total - 1;
      if( record->range )
      {
        Range const & range = *record->range;
        // A parsed range is unsatisfiable against an empty representation: the
        // spec requires one RequestFailed, never a truncated 200 and never a
        // buffer exposed for a range that selects nothing.
        if( total == 0 )
        {
          fail( record, QWebEngineUrlRequestJob::RequestFailed,
                QStringLiteral( "unsatisfiable byte range for an empty resource" ) );
          return;
        }
        if( range.first )
        {
          position = *range.first;
          if( position >= total )
          {
            fail( record, QWebEngineUrlRequestJob::RequestFailed, QStringLiteral( "unsatisfiable byte range" ) );
            return;
          }
          if( range.last ) last = qMin( *range.last, last );
        }
        else
          position = total - qMin( total, *range.last );
        record->rangeBounds = RangeBounds{ position, last, total };
      }
      auto buffer = new QBuffer( record->job );
      record->device = buffer;
      // Preserve full representation size. Chromium enforces the requested
      // end itself; slicing/rebasing the QBuffer corrupts partial responses.
      buffer->setData( bytes );
      if( !buffer->open( QIODevice::ReadOnly ) || !buffer->seek( position ) )
      {
        fail( record, QWebEngineUrlRequestJob::RequestFailed, QStringLiteral( "cannot open article buffer" ) );
        return;
      }
      record->replied = true;
      record->terminal = true;
      record->job->reply( record->mime, buffer );
    }
    catch( std::exception const & error )
    {
      fail( record, QWebEngineUrlRequestJob::RequestFailed, QString::fromUtf8( error.what() ) );
    }
    catch( ... )
    {
      fail( record, QWebEngineUrlRequestJob::RequestFailed, QStringLiteral( "article reader failed" ) );
    }
  }

  void start( QWebEngineUrlRequestJob * job )
  {
    Q_ASSERT( QThread::currentThread() == owner.thread() );
    if( !job || jobs.contains( job ) )
      return;
    auto record = std::make_shared< Job >();
    record->job = job;
    record->url = job->requestUrl();
    jobs.insert( job, record );
    record->connections.append( QObject::connect( job, &QObject::destroyed, &owner,
      [this, job, record] { forget( job, record ); } ) );

    if( job->requestMethod() != "GET" )
    {
      fail( record, QWebEngineUrlRequestJob::RequestDenied );
      return;
    }
    auto const parsed = ArticleUrls::parse( record->url, ArticleUrls::Use::DocumentLoad );
    if( parsed.kind == ArticleUrls::Kind::Invalid || parsed.kind == ArticleUrls::Kind::ExternalAction )
    {
      fail( record, QWebEngineUrlRequestJob::UrlInvalid );
      return;
    }
    record->live = parsed.kind == ArticleUrls::Kind::AudioAction || parsed.kind == ArticleUrls::Kind::VideoAction;
    auto const headers = job->requestHeaders();
    for( auto const & key : headers.keys() )
    {
      if( key.compare( "Range", Qt::CaseInsensitive ) == 0 )
      {
        record->range = parseRange( headers.value( key ) );
        record->rangeRequested = true;
        break;
      }
    }
    try
    {
      QString mime;
      record->request = resources.getResource( record->url, mime );
      record->mime = mime.toLatin1();
      if( !record->request )
      {
        fail( record, QWebEngineUrlRequestJob::UrlNotFound );
        return;
      }
      record->reader = readerFactory ? readerFactory( record->request ) : nullptr;
      if( !record->reader )
      {
        fail( record, QWebEngineUrlRequestJob::RequestFailed, QStringLiteral( "no article request reader" ) );
        return;
      }
      // Connect before inspecting immediate state. Queued delivery confines
      // producer notifications to the handler's GUI thread; every callback
      // validates the record again in inspect().
      record->connections.append( QObject::connect( record->request.get(), &Dictionary::DataRequest::updated, &owner,
        [this, record] { inspect( record ); }, Qt::QueuedConnection ) );
      record->connections.append( QObject::connect( record->request.get(), &Dictionary::DataRequest::finished, &owner,
        [this, record] { inspect( record ); }, Qt::QueuedConnection ) );
      inspect( record );
    }
    catch( std::exception const & error )
    {
      fail( record, QWebEngineUrlRequestJob::RequestFailed, QString::fromUtf8( error.what() ) );
    }
    catch( ... )
    {
      fail( record, QWebEngineUrlRequestJob::RequestFailed, QStringLiteral( "article resource creation failed" ) );
    }
  }
};

ArticleSchemeHandler::ArticleSchemeHandler( ArticleNetworkAccessManager & resources, QObject * parent ):
  ArticleSchemeHandler( resources, []( sptr< Dictionary::DataRequest > const & ) {
    return makeDataRequestReader();
  }, parent ) {}

ArticleSchemeHandler::ArticleSchemeHandler( ArticleNetworkAccessManager & resources,
    DataRequestReaderFactory readerFactory, QObject * parent ):
  QWebEngineUrlSchemeHandler( parent ), state( std::make_unique< State >( *this, resources, std::move( readerFactory ) ) ) {}

ArticleSchemeHandler::~ArticleSchemeHandler()
{
  state->shutdown();
}

void ArticleSchemeHandler::requestStarted( QWebEngineUrlRequestJob * job )
{
  state->start( job );
}
