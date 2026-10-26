/* This file is (c) 2026 GoldenDict contributors.
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file. */

#include "articleschemes.hh"
#include "article_netmgr.hh"
#include "articleurls.hh"

#include <QBuffer>
#include <QHash>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>

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

} // namespace

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
    bool rangeRequested = false;
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
    if( record->terminal || record->replied || record->cancelled || !record->job )
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
      // T14 buffers unfinished producers. T16 can attach ArticleStreamDevice
      // here; no stream methods or Chromium IO-thread callbacks exist yet.
      if( !record->request->isFinished() )
        return;
      if( record->request->dataSize() < 0 )
      {
        fail( record, QWebEngineUrlRequestJob::UrlNotFound );
        return;
      }
      QByteArray const bytes = record->reader->fullData( *record->request );
      qint64 const total = bytes.size();
      qint64 position = 0, last = total - 1;
      if( record->rangeRequested )
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
    auto const headers = job->requestHeaders();
    for( auto const & key : headers.keys() )
    {
      if( key.compare( "Range", Qt::CaseInsensitive ) == 0 )
      {
        record->range = parseRange( headers.value( key ) );
        record->rangeRequested = record->range.has_value();
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
