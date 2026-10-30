/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Article scheme registration, the dictionary resource reader contract and the
 * declarations of the scheme handler, its streaming device and the request
 * interceptor.
 *
 * The reader, buffered coordinator and live streaming device are implemented
 * here. The request interceptor is supplied by its profile/policy task.
 */

#ifndef GOLDENDICT_ARTICLESCHEMES_HH_INCLUDED
#define GOLDENDICT_ARTICLESCHEMES_HH_INCLUDED

#include <QByteArray>
#include <QIODevice>
#include <QMap>
#include <QSet>
#include <QUrl>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlSchemeHandler>
#include <QtGlobal>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>

#include "sptr.hh"

namespace Dictionary
{
class DataRequest;
}

class ArticleNetworkAccessManager;
class ArticleSchemeHandler;
class ArticleStreamDevice;
struct ArticleRequestPolicy
{
  bool blockExternal = false;
  QSet< QString > dictionaryHosts;
  QMap< QString, QUrl > localFileRedirects;
};

/// The page owns this interceptor. The IO callback sees only one immutable
/// snapshot; publishing and preference refresh never borrow GUI state on IO.
class ArticleRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
  explicit ArticleRequestInterceptor( QObject * parent = nullptr );
  void setPolicy( std::shared_ptr< ArticleRequestPolicy const > policy );
  void interceptRequest( QWebEngineUrlRequestInfo & info ) override;

private:
  friend class ArticleProfileController;
  std::shared_ptr< ArticleRequestPolicy const > policy;
  std::mutex diagnosticMutex;
  std::weak_ptr< ArticleRequestPolicy const > diagnosticPolicy;
  QSet< QUrl > deniedUrls;
};

/// Registers every application URL scheme on the engine's global registry.
///
/// It is a free function taking no object because it has to run before the
/// QApplication/QGuiApplication object exists: the engine reads the scheme
/// registry when it builds its profile, and a late registration is silently
/// ineffective. The call is idempotent and belongs to the main thread only.
void registerArticleSchemes();

/// Access to the bytes of a dictionary DataRequest.
///
/// A reader is a plain C++ object with no QObject affinity and no producer
/// ownership: it is shared between the GUI thread that owns the job and the
/// engine thread that may drain a streaming reply. fullData() and dataSlice()
/// run on the GUI thread only and receive the producer by reference for that
/// call; copyStreamBytes() may run on the engine's IO thread and therefore only
/// copies a stable byte span into a destination, without touching a producer,
/// a handler, a job or a GUI connection. The reader stays alive through every
/// operation, and any mutable reader state has to be synchronized separately.
class DataRequestReader
{
public:
  virtual ~DataRequestReader() noexcept = default;

  /// All bytes of a successfully completed request.
  virtual QByteArray fullData( Dictionary::DataRequest & request ) const = 0;

  /// Forwards directly to the request's slice accessor.
  virtual void dataSlice( Dictionary::DataRequest & request, size_t offset,
                          size_t size, void * destination ) const = 0;

  /// Copies an already-buffered span for a streaming device.
  virtual void copyStreamBytes( char * destination, char const * source,
                                qint64 count ) const = 0;
};

/// The production forwarding reader. Stateless, so one instance can serve every
/// job; it is the only reader the application ever installs.
std::shared_ptr< DataRequestReader const > makeDataRequestReader();

/// Creates the reader of one job. Invoked on the GUI thread, once per job,
/// immediately after the backing resource has been created. A null result is a
/// terminal failure of that job.
using DataRequestReaderFactory = std::function< std::shared_ptr< DataRequestReader const >(
  sptr< Dictionary::DataRequest > const & ) >;

/// Job-owned live reply. Its producer and connections are GUI-only; every IO
/// virtual uses just synchronized byte state and the producer-free reader.
class ArticleStreamDevice : public QIODevice
{
  Q_OBJECT
public:
  ArticleStreamDevice( sptr< Dictionary::DataRequest > const & request,
                       std::shared_ptr< DataRequestReader const > reader, QObject * jobParent );
  ~ArticleStreamDevice() override;
  bool isSequential() const override;
  qint64 size() const override;
  qint64 bytesAvailable() const override;
  bool atEnd() const override;
  void close() override;
  void cancel() noexcept;

signals:
  void completed();
  void failed( QString const & message );

protected:
  qint64 readData( char * data, qint64 maxSize ) override;
  qint64 writeData( char const * data, qint64 maxSize ) override;

private:
  friend class ArticleSchemeHandler;
  struct State;
  struct Producer;
  std::unique_ptr< State > state;
  std::unique_ptr< Producer > producer;
  void pump();
  void publish( bool appended );
};

/// GUI-owned coordinator. The resource manager must outlive this handler;
/// reply devices belong to their jobs and are never deleted by the handler.
class ArticleSchemeHandler : public QWebEngineUrlSchemeHandler
{
  Q_OBJECT
public:
  ArticleSchemeHandler( ArticleNetworkAccessManager & resources, QObject * parent = nullptr );
  ArticleSchemeHandler( ArticleNetworkAccessManager & resources,
                        DataRequestReaderFactory readerFactory, QObject * parent = nullptr );
  ~ArticleSchemeHandler() override;
  void requestStarted( QWebEngineUrlRequestJob * job ) override;

signals:
  void producerFailed( QUrl const & url, QString const & message );

private:
  struct State;
  std::unique_ptr< State > state;
};

#endif
