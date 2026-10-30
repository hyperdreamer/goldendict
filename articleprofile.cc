/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#include "articleprofile.hh"
#include "articleschemes.hh"
#include "article_netmgr.hh"
#include "config.hh"
#include "ui_authentication.h"

#include <QAuthenticator>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QNetworkDiskCache>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QThread>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWidget>

#include <atomic>
#include <limits>
#include <optional>

namespace {
using Credentials = QPair< QString, QString >;

std::optional< Credentials > askCredentials( QWidget * parent, QString const & title,
    QString const & label, Credentials const & initial = {} )
{
  QPointer< QDialog > dialog = new QDialog( parent );
  Ui::Dialog ui;
  ui.setupUi( dialog );
  dialog->setWindowTitle( title );
  ui.label->setTextFormat( Qt::PlainText );
  ui.label->setText( label );
  ui.userEdit->setText( initial.first );
  ui.passwordEdit->setText( initial.second );
  dialog->adjustSize();
  int const result = dialog->exec();
  std::optional< Credentials > credentials;
  if( dialog && result == QDialog::Accepted )
    credentials = Credentials{ ui.userEdit->text(), ui.passwordEdit->text() };
  delete dialog.data();
  return credentials;
}

void applyProxy( Config::ProxyServer const & preference )
{
  QNetworkProxy proxy( QNetworkProxy::NoProxy );
  if( preference.enabled && preference.useSystemProxy )
  {
    auto const proxies = QNetworkProxyFactory::systemProxyForQuery();
    if( proxies.isEmpty() )
      qWarning( "ArticleProxy: system query empty; using NoProxy" );
    else
    {
      proxy = proxies.first();
      if( !preference.systemProxyUser.isEmpty() )
      {
        proxy.setUser( preference.systemProxyUser );
        proxy.setPassword( preference.systemProxyPassword );
      }
    }
  }
  else if( preference.enabled )
  {
    switch( preference.type )
    {
      case Config::ProxyServer::HttpConnect: proxy.setType( QNetworkProxy::HttpProxy ); break;
      case Config::ProxyServer::Socks5: proxy.setType( QNetworkProxy::Socks5Proxy ); break;
      case Config::ProxyServer::HttpGet: proxy.setType( QNetworkProxy::HttpCachingProxy ); break;
    }
    proxy.setHostName( preference.host );
    proxy.setPort( quint16( preference.port ) );
    proxy.setUser( preference.user );
    proxy.setPassword( preference.password );
  }
  QNetworkProxy::setApplicationProxy( proxy );
}

void fixedSettings( QWebEngineSettings * settings )
{
  settings->setAttribute( QWebEngineSettings::LocalContentCanAccessRemoteUrls, true );
  settings->setAttribute( QWebEngineSettings::LocalContentCanAccessFileUrls, true );
  settings->setAttribute( QWebEngineSettings::JavascriptEnabled, true );
  settings->setAttribute( QWebEngineSettings::LocalStorageEnabled, true );
  settings->setAttribute( QWebEngineSettings::PluginsEnabled, false );
  settings->setAttribute( QWebEngineSettings::BackForwardCacheEnabled, false );
}
} // namespace

ProxyAuthAdapter::ProxyAuthAdapter( Config::Class & config_, QWidget * dialogParent_, QObject * parent ):
  QObject( parent ), config( config_ ), dialogParent( dialogParent_ ) {}

void ProxyAuthAdapter::authenticate( QUrl const & requestUrl, QAuthenticator * auth,
                                     QString const & proxyHost )
{
  Q_ASSERT( QThread::currentThread() == thread() );
  if( !auth ) return;
  QString const realm = auth->realm();
  // Qt pre-populates the realm, so returning without resetting would submit
  // empty credentials and repeatedly re-challenge instead of cancelling.
  *auth = QAuthenticator();
  QPointer< ProxyAuthAdapter > alive( this );
  auto const & preference = config.preferences.proxyServer;
  Credentials const saved = preference.useSystemProxy
      ? Credentials{ preference.systemProxyUser, preference.systemProxyPassword }
      : Credentials{ preference.user, preference.password };
  QString const key = proxyHost + ':' + QString::number( QNetworkProxy::applicationProxy().port() )
                    + '/' + realm;
  Credentials chosen;
  if( !saved.first.isEmpty() && ( !attempted.contains( key ) || attempted.value( key ) != saved ) )
    chosen = saved;
  else
  {
    auto const answer = askCredentials( dialogParent, tr( "Proxy authentication required" ),
        tr( "Proxy %1\nRealm: %2" ).arg( proxyHost, realm ), saved );
    if( !alive || !answer ) return;
    chosen = *answer;
    auto & p = config.preferences.proxyServer;
    if( p.useSystemProxy ) { p.systemProxyUser = chosen.first; p.systemProxyPassword = chosen.second; }
    else { p.user = chosen.first; p.password = chosen.second; }
  }
  Q_UNUSED( requestUrl );
  attempted.insert( key, chosen );
  auth->setUser( chosen.first );
  auth->setPassword( chosen.second );
  // This is deliberately proxy-only. OriginAuthAdapter has no config reference.
  auto proxy = QNetworkProxy::applicationProxy();
  proxy.setUser( chosen.first );
  proxy.setPassword( chosen.second );
  QNetworkProxy::setApplicationProxy( proxy );
}

OriginAuthAdapter::OriginAuthAdapter( QWidget * dialogParent_, QObject * parent ):
  QObject( parent ), dialogParent( dialogParent_ ) {}

void OriginAuthAdapter::authenticate( QUrl const & requestUrl, QAuthenticator * auth )
{
  Q_ASSERT( QThread::currentThread() == thread() );
  if( !auth ) return;
  QString const realm = auth->realm();
  *auth = QAuthenticator();
  QPointer< OriginAuthAdapter > alive( this );
  QUrl origin;
  origin.setScheme( requestUrl.scheme() );
  origin.setHost( requestUrl.host() );
  origin.setPort( requestUrl.port( requestUrl.scheme() == QLatin1String( "https" ) ? 443 : 80 ) );
  auto const answer = askCredentials( dialogParent, tr( "Website authentication required" ),
      tr( "%1\nRealm: %2" ).arg( origin.toString(), realm ) );
  if( !alive || !answer ) return;
  auth->setUser( answer->first );
  auth->setPassword( answer->second );
}

struct ArticleProfileController::State
{
  Config::Class & config;
  ArticleNetworkAccessManager & resources;
  QWebEngineProfile * profile = nullptr;
  ArticleSchemeHandler * handler = nullptr;
  ProxyAuthAdapter * proxyAuth = nullptr;
  OriginAuthAdapter * originAuth = nullptr;
  QMap< QWebEnginePage *, QPointer< ArticleRequestInterceptor > > pages;
  QString defaultUserAgent;
  QTimer * exitTimer = nullptr;
  QMetaObject::Connection clearConnection;
  bool exitStarted = false, exitFinished = false;
  State( Config::Class & c, ArticleNetworkAccessManager & r ): config( c ), resources( r ) {}
};

ArticleProfileController::ArticleProfileController( Config::Class & config,
    ArticleNetworkAccessManager & resources, QWidget * windowParent ):
  QObject( windowParent ), state( std::make_unique< State >( config, resources ) )
{
  Q_ASSERT( QThread::currentThread() == thread() );
  // Chromium samples the application proxy at construction, before any page.
  applyProxy( config.preferences.proxyServer );
  state->profile = new QWebEngineProfile( QStringLiteral( "goldendict-articles" ), this );
  state->profile->setPersistentCookiesPolicy( QWebEngineProfile::NoPersistentCookies );
  QString const root = Config::getNetworkCacheDir();
  state->profile->setCachePath( QDir( root ).filePath( QStringLiteral( "webengine/cache" ) ) );
  state->profile->setPersistentStoragePath( QDir( root ).filePath( QStringLiteral( "webengine/storage" ) ) );
  state->defaultUserAgent = state->profile->httpUserAgent();
  fixedSettings( state->profile->settings() );
  state->handler = new ArticleSchemeHandler( resources, this );
  for( QByteArray const name : { "gdlookup", "gdpicture", "bres", "gdau", "gdvideo", "gico", "qrcx" } )
    state->profile->installUrlSchemeHandler( name, state->handler );
  state->proxyAuth = new ProxyAuthAdapter( config, windowParent, this );
  state->originAuth = new OriginAuthAdapter( windowParent, this );
  connect( &resources, &QNetworkAccessManager::proxyAuthenticationRequired, state->proxyAuth,
      [adapter = state->proxyAuth]( QNetworkProxy const & proxy, QAuthenticator * auth ) {
        // QNAM's authenticator also carries its HTTP protocol/phase state.
        // Leave it unchanged on cancellation; only WebEngine interprets a
        // freshly null authenticator as rejection.
        QAuthenticator answer;
        answer.setRealm( auth->realm() );
        adapter->authenticate( {}, &answer, proxy.hostName() );
        if( !answer.isNull() )
        {
          auth->setUser( answer.user() );
          auth->setPassword( answer.password() );
        }
      } );
  connect( &resources, &QNetworkAccessManager::authenticationRequired, state->originAuth,
      [adapter = state->originAuth]( QNetworkReply * reply, QAuthenticator * auth ) {
        QAuthenticator answer;
        answer.setRealm( auth->realm() );
        adapter->authenticate( reply->url(), &answer );
        if( !answer.isNull() )
        {
          auth->setUser( answer.user() );
          auth->setPassword( answer.password() );
        }
      } );
  state->exitTimer = new QTimer( this );
  state->exitTimer->setSingleShot( true );
  state->exitTimer->setTimerType( Qt::PreciseTimer );
  state->exitTimer->setInterval( 5000 );
  connect( state->exitTimer, &QTimer::timeout, this, [this] { finishExit( true ); } );
  applyPreferences();
}

ArticleProfileController::~ArticleProfileController()
{
  // Handler cancellation must run while the borrowed resource manager is alive.
  state->profile->removeUrlSchemeHandler( state->handler );
  delete state->handler;
  for( auto page : state->pages.keys() )
  {
    disconnect( page, nullptr, this, nullptr );
    disconnect( page, nullptr, state->proxyAuth, nullptr );
    disconnect( page, nullptr, state->originAuth, nullptr );
  }
  disconnect( state->clearConnection );
  state->exitTimer->stop();
  if( !state->pages.isEmpty() )
  {
    // A caller violated normal page-before-controller shutdown. Stack pages
    // cannot be deleted here. Retain ONLY the profile until their destruction;
    // the handler/auth adapters are retired with the controller.
    auto keeper = new QObject( QCoreApplication::instance() );
    state->profile->setParent( keeper );
    auto remaining = std::make_shared< int >( state->pages.size() );
    for( auto page : state->pages.keys() )
      connect( page, &QObject::destroyed, keeper, [keeper, remaining] {
        if( --*remaining == 0 ) keeper->deleteLater();
      } );
  }
}

QWebEngineProfile * ArticleProfileController::profile() const noexcept { return state->profile; }

void ArticleProfileController::attachPage( QWebEnginePage * page )
{
  Q_ASSERT( QThread::currentThread() == thread() );
  if( !page || page->profile() != state->profile )
  {
    qWarning( "ArticleProfile: cannot attach a page from another profile" );
    return;
  }
  auto it = state->pages.find( page );
  if( it != state->pages.end() && *it ) return;
  bool const first = it == state->pages.end();
  auto interceptor = new ArticleRequestInterceptor( page );
  interceptor->setObjectName( QStringLiteral( "gdArticleRequestInterceptor" ) );
  auto policy = std::make_shared< ArticleRequestPolicy >();
  policy->blockExternal = state->config.preferences.disallowContentFromOtherSites;
  interceptor->setPolicy( policy );
  state->pages.insert( page, interceptor );
  page->setUrlRequestInterceptor( interceptor );
  // Qt does not own the installed interceptor; reset the borrowed attachment if
  // a caller explicitly deletes the page-owned child before deleting the page.
  connect( interceptor, &QObject::destroyed, page, [page] { page->setUrlRequestInterceptor( nullptr ); } );
  if( !first ) return;
  fixedSettings( page->settings() );
  connect( page, &QObject::destroyed, this, [this, page] { state->pages.remove( page ); } );
  connect( page, &QWebEnginePage::proxyAuthenticationRequired, state->proxyAuth,
           &ProxyAuthAdapter::authenticate );
  connect( page, &QWebEnginePage::authenticationRequired, state->originAuth,
           &OriginAuthAdapter::authenticate );
}

void ArticleProfileController::applyPreferences()
{
  Q_ASSERT( QThread::currentThread() == thread() );
  auto const & preference = state->config.preferences;
  applyProxy( preference.proxyServer );
  // DefaultProxy makes retained QNAM requests follow subsequent app changes.
  state->resources.setProxy( QNetworkProxy( QNetworkProxy::DefaultProxy ) );
  qint64 const bytes = preference.maxNetworkCacheSize > 0
      ? qMin( qint64( std::numeric_limits< int >::max() ), qint64( preference.maxNetworkCacheSize ) << 20 ) : 0;
  if( bytes > 0 )
  {
    state->profile->setHttpCacheType( QWebEngineProfile::DiskHttpCache );
    state->profile->setHttpCacheMaximumSize( int( bytes ) );
    auto cache = qobject_cast< QNetworkDiskCache * >( state->resources.cache() );
    if( !cache )
    {
      cache = new QNetworkDiskCache( &state->resources );
      cache->setCacheDirectory( Config::getNetworkCacheDir() );
      state->resources.setCache( cache );
    }
    cache->setMaximumCacheSize( bytes );
  }
  else
  {
    state->profile->setHttpCacheType( QWebEngineProfile::NoCache );
    state->resources.setCache( nullptr );
  }
  QString userAgent = state->defaultUserAgent;
  if( preference.hideGoldenDictHeader ) userAgent.remove( QCoreApplication::applicationName() );
  state->profile->setHttpUserAgent( userAgent );
  for( auto const & interceptor : state->pages )
    if( interceptor )
    {
      auto replacement = std::make_shared< ArticleRequestPolicy >( *std::atomic_load( &interceptor->policy ) );
      replacement->blockExternal = preference.disallowContentFromOtherSites;
      interceptor->setPolicy( std::move( replacement ) );
    }
}

void ArticleProfileController::clearForExit()
{
  Q_ASSERT( QThread::currentThread() == thread() );
  if( state->exitStarted ) return;
  state->exitStarted = true;
  if( auto cache = state->resources.cache() ) cache->clear();
  state->clearConnection = connect( state->profile, &QWebEngineProfile::clearHttpCacheCompleted,
                                    this, [this] { finishExit( false ); } );
  state->exitTimer->start();
  state->profile->clearHttpCache();
}

void ArticleProfileController::finishExit( bool timedOut )
{
  if( state->exitFinished ) return;
  state->exitFinished = true;
  state->exitTimer->stop();
  disconnect( state->clearConnection );
  if( timedOut ) qWarning( "ArticleCache: exit clear timed out after 5000 ms" );
  emit exitCachesReady( timedOut );
}
