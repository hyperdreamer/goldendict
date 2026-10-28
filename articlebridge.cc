/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#include "articlebridge.hh"
#include "articlewebview.hh"
#include <QFile>
#include <QSet>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QTimer>
#include <cmath>
#include <limits>

namespace {
constexpr int FrameOperationDeadlineMs = 2000;
QString resource( char const * name )
{
  QFile file( QString::fromLatin1( name ) );
  if( !file.open( QIODevice::ReadOnly ) )
  {
    qWarning( "ArticleBridge: cannot read %s", name );
    return {};
  }
  return QString::fromUtf8( file.readAll() );
}

bool finiteNumber( QVariant const & value )
{
  auto type = value.metaType().id();
  return ( type == QMetaType::Double || type == QMetaType::Int || type == QMetaType::LongLong ||
           type == QMetaType::UInt || type == QMetaType::ULongLong ) && std::isfinite( value.toDouble() );
}

bool snapshot( QVariantMap const & value, QStringList & articles )
{
  if( value.value( "currentArticle" ).metaType().id() != QMetaType::QString ||
      !finiteNumber( value.value( "sx" ) ) || !finiteNumber( value.value( "sy" ) ) ||
      value.value( "hasAudio" ).metaType().id() != QMetaType::Bool ) return false;
  auto list = value.value( "articles" );
  if( list.metaType().id() != QMetaType::QVariantList && list.metaType().id() != QMetaType::QStringList )
    return false;
  QSet<QString> seen;
  for( auto const & item : list.toList() )
  {
    if( item.metaType().id() != QMetaType::QString || item.toString().isEmpty() || seen.contains( item.toString() ) )
      return false;
    articles.append( item.toString() );
    seen.insert( item.toString() );
  }
  QString const current = value.value( "currentArticle" ).toString();
  return current.isEmpty() || ( current.startsWith( "gdfrom-" ) && seen.contains( current.mid( 7 ) ) );
}
}

ArticleBridge::ArticleBridge( QWebEnginePage * borrowed, QObject * viewParent ):
  QObject( viewParent ), page( borrowed ), channel( new QWebChannel( this ) )
{
  Q_ASSERT( borrowed );
  channel->registerObject( QStringLiteral( "articleview" ), this );
  page->setWebChannel( channel, QWebEngineScript::MainWorld );
  page->settings()->setAttribute( QWebEngineSettings::BackForwardCacheEnabled, false );
  connect( borrowed, &QObject::destroyed, this, [this] { retire(); } );
  installScripts();
}

ArticleBridge::~ArticleBridge()
{
  retire();
  if( page && page->webChannel() == channel ) page->setWebChannel( nullptr );
  if( page )
    for( auto const & name : installedScripts )
      for( auto const & script : page->scripts().find( name ) ) page->scripts().remove( script );
  channel->deregisterObject( this );
}

int ArticleBridge::generation() const noexcept { return currentGeneration; }

void ArticleBridge::retire()
{
  state = State::Closed;
  queue.clear();
  if( inFlight )
  {
    inFlight->completed = true;
    inFlight->callback = {};
  }
  inFlight.reset();
  pendingActive.clear();
}

bool ArticleBridge::advanceGeneration()
{
  if( currentGeneration == std::numeric_limits<int>::max() ) return false;
  ++currentGeneration;
  return true;
}

void ArticleBridge::reset()
{
  if( state == State::Closed || !page ) return;
  if( !advanceGeneration() )
  {
    retire();
    page->setWebChannel( nullptr );
    for( auto const & name : installedScripts )
      for( auto const & script : page->scripts().find( name ) ) page->scripts().remove( script );
    installedScripts.clear();
    if( auto view = qobject_cast<ArticleWebView *>( parent() ) ) view->recreatePage( this );
    else qWarning( "ArticleBridge: generation exhausted; owning view must recreate page and bridge" );
    return;
  }
  state = State::Installing;
  queue.clear();
  if( inFlight )
  {
    inFlight->completed = true;
    inFlight->callback = {};
  }
  inFlight.reset();
  pendingActive.clear();
  articles.clear();
  hasAudio = false;
  bool const hadActive = !currentArticle.isEmpty(), hadReading = !readingState.isEmpty();
  currentArticle.clear();
  readingState.clear();
  installScripts(); // Before navigation, never from loadStarted.
  QPointer<ArticleBridge> guard( this );
  int const stamp = currentGeneration;
  emit generationChanged( stamp );
  if( !guard || !accepts( stamp ) ) return;
  if( hadActive ) emit activeArticleChanged( {} );
  if( !guard || !accepts( stamp ) ) return;
  if( hadReading ) emit readingStateChanged( {} );
}

void ArticleBridge::installScripts()
{
  for( auto const & name : installedScripts )
    for( auto const & script : page->scripts().find( name ) ) page->scripts().remove( script );
  installedScripts.clear();
  auto bootstrap = resource( ":/scripts/article-bootstrap.js" );
  auto dom = resource( ":/scripts/article-dom.js" );
  auto webchannel = resource( ":/qtwebchannel/qwebchannel.js" );
  if( bootstrap.isEmpty() || dom.isEmpty() || webchannel.isEmpty() ) return;
  bootstrap.replace( QStringLiteral( "/* GD_GENERATION */" ), QString::number( currentGeneration ) );
  bootstrap.replace( QStringLiteral( "/* GD_WEBCHANNEL_SOURCE */" ), webchannel );
  for( auto const & entry : { qMakePair( QStringLiteral( "gd-bootstrap-" ), bootstrap ),
                            qMakePair( QStringLiteral( "gd-dom-" ), dom ) } )
  {
    QWebEngineScript script;
    script.setName( entry.first + QString::number( currentGeneration ) );
    script.setInjectionPoint( QWebEngineScript::DocumentCreation );
    script.setWorldId( QWebEngineScript::MainWorld );
    script.setRunsOnSubFrames( false );
    script.setSourceCode( entry.second );
    page->scripts().insert( script );
    installedScripts.append( script.name() );
  }
  state = State::ChannelPending;
}

bool ArticleBridge::accepts( int stamp ) const noexcept
{
  return page && state != State::Closed && stamp == currentGeneration;
}

void ArticleBridge::setActive( QString const & id )
{
  if( currentArticle == id ) return;
  currentArticle = id;
  emit activeArticleChanged( id );
}

void ArticleBridge::onJsActiveArticleChanged( QString id, int stamp )
{
  if( !accepts( stamp ) || !id.startsWith( "gdfrom-" ) ) return;
  if( state != State::Ready ) { pendingActive.append( std::move( id ) ); return; }
  if( articles.contains( id.mid( 7 ) ) ) setActive( id );
}

void ArticleBridge::onJsReadingState( QVariantMap value, int stamp )
{
  if( !accepts( stamp ) || value.value( "kind" ).metaType().id() != QMetaType::QString ) return;
  QString const kind = value.value( "kind" ).toString();
  bool const ready = kind == QLatin1String( "ready" );
  if( ( !ready && kind != QLatin1String( "state" ) ) ||
      ( ready ? state != State::ChannelPending : state != State::Ready ) ) return;
  QStringList members;
  if( !snapshot( value, members ) ) return;
  articles = std::move( members );
  hasAudio = value.value( "hasAudio" ).toBool();
  QPointer<ArticleBridge> guard( this );
  if( ready )
  {
    state = State::Ready;
    auto pending = std::move( pendingActive );
    pendingActive.clear();
    for( auto const & id : pending )
    {
      if( articles.contains( id.mid( 7 ) ) ) setActive( id );
      if( !guard || !accepts( stamp ) || state != State::Ready ) return;
    }
  }
  setActive( value.value( "currentArticle" ).toString() );
  if( !guard || !accepts( stamp ) || state != State::Ready ) return;
  auto canonical = value;
  canonical.remove( "kind" );
  if( canonical != readingState )
  {
    readingState = canonical;
    emit readingStateChanged( value );
    if( !guard || !accepts( stamp ) || state != State::Ready ) return;
  }
  if( ready )
  {
    emit bridgeReady( stamp );
    if( !guard || !accepts( stamp ) || state != State::Ready ) return;
    dispatch();
  }
}

void ArticleBridge::onJsFrameEvent( QVariantMap event, int stamp )
{
  if( accepts( stamp ) && state == State::Ready && !event.isEmpty() ) emit frameEvent( event );
}

void ArticleBridge::run( QString const & script, Result callback )
{
  enqueue( {}, script, std::move( callback ) );
}

void ArticleBridge::runInFrame( QWebEngineFrame const & frame, QString const & script, Result callback )
{
  enqueue( frame, script, std::move( callback ) );
}

void ArticleBridge::retireInFlightFrame()
{
  auto operation = inFlight;
  if( operation && operation->frame ) retireOperation( operation );
}

void ArticleBridge::enqueue( std::optional<QWebEngineFrame> frame, QString const & script, Result callback, int deadlineMs )
{
  if( state == State::Closed || !page ) return;
  if( nextOperation == std::numeric_limits<quint64>::max() )
  {
    qWarning( "ArticleBridge: operation number exhausted" );
    retire();
    return;
  }
  auto operation = std::make_shared<Operation>( Operation{ currentGeneration, ++nextOperation,
                   std::move( frame ), script, std::move( callback ), false } );
  queue.enqueue( operation );
  if( deadlineMs > 0 )
  {
    QTimer::singleShot( deadlineMs, this, [this, operation] {
      if( operation->completed || operation->generation != currentGeneration || state == State::Closed || !page ) return;
      if( inFlight == operation ) { retireOperation( operation ); return; }
      // A capture queued while the channel is installing also has a deadline.
      // Remove only its own slot; never release a different in-flight operation.
      if( !queue.removeOne( operation ) ) return;
      operation->completed = true;
      auto callback = std::move( operation->callback );
      if( callback ) callback( {} );
    } );
  }
  dispatch();
}

void ArticleBridge::dispatch()
{
  if( state != State::Ready || inFlight || queue.isEmpty() || !page ) return;
  auto operation = queue.dequeue();
  if( operation->generation != currentGeneration ) return;
  inFlight = operation;
  QPointer<ArticleBridge> guard( this );
  QPointer<QWebEnginePage> target = page;
  auto done = [guard, target, operation]( QVariant const & value ) {
    if( guard && target && guard->page == target ) guard->complete( operation, value );
  };
  if( operation->frame )
  {
    // A native frame from another page, or one retired by a navigation, must
    // never be run. WebsiteFrames additionally verifies its nonce/child path.
    std::function<bool(QWebEngineFrame const &)> belongs = [&]( QWebEngineFrame const & frame ) {
      if( frame == *operation->frame ) return true;
      for( auto const & child : frame.children() ) if( belongs( child ) ) return true;
      return false;
    };
    if( !operation->frame->isValid() || !belongs( page->mainFrame() ) ) { done( {} ); return; }
    QTimer::singleShot( FrameOperationDeadlineMs, this, [guard, operation] {
      if( guard ) guard->retireOperation( operation );
    } );
    operation->frame->runJavaScript( operation->script, QWebEngineScript::MainWorld, done );
  }
  else page->runJavaScript( operation->script, QWebEngineScript::MainWorld, done );
}

void ArticleBridge::complete( std::shared_ptr<Operation> const & operation, QVariant const & value )
{
  if( state != State::Ready || !page || operation->generation != currentGeneration ||
      !inFlight || operation->number != inFlight->number || operation->completed ) return;
  operation->completed = true;
  QPointer<ArticleBridge> guard( this );
  auto const target = page;
  auto callback = std::move( operation->callback );
  if( callback ) callback( value ); // Invalid QVariant is a completed result too.
  if( !guard || !target || page != target || !accepts( operation->generation ) || inFlight != operation ) return;
  inFlight.reset();
  dispatch();
}

void ArticleBridge::retireOperation( std::shared_ptr<Operation> const & operation )
{
  if( state != State::Ready || !page || !operation || operation->generation != currentGeneration ||
      !inFlight || inFlight->number != operation->number || inFlight != operation || operation->completed ) return;
  operation->completed = true;
  QPointer<ArticleBridge> guard( this );
  auto const target = page;
  auto callback = std::move( operation->callback );
  if( callback ) callback( {} );
  if( !guard || !target || page != target || !accepts( operation->generation ) || inFlight != operation ) return;
  inFlight.reset();
  dispatch();
}
