/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#include "websiteframes.hh"
#include "articlebridge.hh"
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <algorithm>
#include <cmath>
#include <optional>
#include <limits>

namespace {
constexpr double MaxMeasurement = 16777216;
constexpr int TextLimit = 65536, MaxDepth = 32;
QString json( QVariant const & value )
{
  auto bytes = QJsonDocument( QJsonArray{ QJsonValue::fromVariant( value ) } ).toJson( QJsonDocument::Compact );
  return QString::fromUtf8( bytes.mid( 1, bytes.size() - 2 ) );
}
bool numeric( QVariant const & value )
{
  auto type = value.metaType().id();
  return ( type == QMetaType::Double || type == QMetaType::Int || type == QMetaType::UInt ||
           type == QMetaType::LongLong || type == QMetaType::ULongLong ) && std::isfinite( value.toDouble() );
}
bool integer( QVariant const & value, int maximum )
{
  return numeric( value ) && value.toDouble() >= 0 && value.toDouble() <= maximum &&
    std::floor( value.toDouble() ) == value.toDouble();
}
bool measurement( QVariant const & value )
{
  return numeric( value ) && value.toDouble() > 0 && value.toDouble() <= MaxMeasurement;
}
QString readScript( char const * path )
{
  QFile file( QString::fromLatin1( path ) );
  if( !file.open( QIODevice::ReadOnly ) ) { qWarning( "WebsiteFrames: missing %s", path ); return {}; }
  return QString::fromUtf8( file.readAll() );
}
QWebEngineFrame atPath( QWebEnginePage * page, QVector<int> const & path )
{
  if( !page ) return {};
  auto frame = page->mainFrame();
  for( int index : path )
  {
    if( !frame.isValid() ) return {};
    auto children = frame.children();
    if( index < 0 || index >= children.size() ) return {};
    frame = children.at( index );
  }
  return frame;
}
}

struct WebsiteFrames::State
{
  struct Node
  {
    QWebEngineFrame frame;
    QVector<int> path;
    int parent = -1;
    QVariantMap data;
  };
  struct Scan
  {
    QVector<Node> nodes;
    quint64 epoch;
    int remaining;
    bool valid = true;
  };
  struct Entry
  {
    WebsiteFrameKey key;
    QVariantList route;
    QVector<Node> chain;
    QString dictionary;
    std::unique_ptr<QTimer> timer;
    bool sampling = false, pending = false;
    int equal = 0;
    quint64 run = 0;
    double height = 0;
    qint64 started = 0;
  };
  struct Root
  {
    QVariantMap route;
    qint64 started = 0;
    bool measured = false, reported = false;
  };
  struct Word
  {
    std::function<void( QString const & )> callback;
    quint64 epoch;
    QVector<Node> chain;
    bool done = false;
    void finish( QString const & value = {} )
    {
      if( done ) return;
      done = true;
      auto deliver = std::move( callback );
      if( deliver ) deliver( value );
    }
  };
  WebsiteFrames * owner;
  QPointer<QWebEnginePage> page;
  QPointer<ArticleBridge> bridge;
  QTimer tick;
  QElapsedTimer clock;
  int generation = 0;
  quint64 epoch = 0;
  bool active = false, scanning = false, closing = false;
  QString installedScript, topDoc;
  QHash<QString, std::shared_ptr<Entry>> entries;
  QHash<QString, Root> roots;
  QHash<QString, QVariantMap> ready;
  QVector<std::shared_ptr<Word>> words;

  State( WebsiteFrames * object, QWebEnginePage * target, ArticleBridge * channel ):
    owner( object ), page( target ), bridge( channel ), tick( object ) { clock.start(); }
  bool current( quint64 stamp ) const
  {
    return !closing && page && bridge && active && epoch == stamp && bridge->generation() == generation;
  }
  bool unchanged( Node const & node ) const
  {
    if( !node.frame.isValid() ) return false;
    auto currentFrame = atPath( page, node.path );
    return currentFrame.isValid() && currentFrame == node.frame;
  }
  bool unchanged( QVector<Node> const & chain ) const
  {
    return std::all_of( chain.cbegin(), chain.cend(), [this]( Node const & node ) { return unchanged( node ); } );
  }
  void cancelWords()
  {
    auto cancelled = std::move( words );
    words.clear();
    for( auto const & word : cancelled ) word->finish();
  }
  void arm( std::shared_ptr<Entry> const & entry )
  {
    if( !entry->timer )
    {
      entry->timer = std::make_unique<QTimer>();
      entry->timer->setTimerType( Qt::PreciseTimer );
      entry->timer->setInterval( 250 );
      QPointer<WebsiteFrames> guard( owner );
      std::weak_ptr<Entry> weak = entry;
      QObject::connect( entry->timer.get(), &QTimer::timeout, owner, [guard, weak] {
        if( guard ) if( auto target = weak.lock() ) guard->d->sample( target );
      } );
    }
    entry->sampling = true;
    entry->pending = false;
    ++entry->run;
    entry->equal = 0;
    entry->started = clock.elapsed();
    entry->timer->start();
    sample( entry );
  }
  void stop( std::shared_ptr<Entry> const & entry )
  {
    entry->sampling = false;
    if( entry->timer ) entry->timer->stop();
  }
  void sample( std::shared_ptr<Entry> const & entry )
  {
    auto now = clock.elapsed();
    if( !entry->sampling ) return;
    if( now - entry->started >= 10000 || !current( epoch ) || entries.value( entry->key.docId ) != entry )
      { stop( entry ); return; }
    if( entry->pending ) return;
    if( !unchanged( entry->chain ) ) { stop( entry ); owner->discover(); return; }
    entry->pending = true;
    auto nonce = json( entry->key.docId );
    QString script = QStringLiteral(
      "/*gd-frame-sample*/(()=>{const f=window.__gdWebsiteFrame;"
      "if(!f||f.generation!==%1||f.docId!==%2)return false;"
      "const m=gdMeasureFrame();if(m)parent.postMessage({gdFrameEvent:'measure',generation:%1,"
      "docId:f.docId,path:[],height:m.height,width:m.width},'*');return true})()" ).arg( generation ).arg( nonce );
    QPointer<WebsiteFrames> guard( owner );
    auto stamp = epoch, run = entry->run;
    bridge->runInFrame( entry->chain.last().frame, script, [guard, stamp, entry, run]( QVariant const & ) {
      if( !guard || !guard->d->current( stamp ) || entry->run != run ) return;
      entry->pending = false;
      if( !guard->d->unchanged( entry->chain ) ) guard->d->stop( entry );
    } );
  }
  void apply( std::shared_ptr<Entry> const & entry, double height, double width )
  {
    auto parent = entry->chain.at( entry->chain.size() - 2 );
    auto segment = entry->route.last().toMap();
    auto script = QStringLiteral(
      "/*gd-frame-height*/window.gdApplyFrameHeight&&gdApplyFrameHeight(%1,%2,%3,%4,%5)" )
      .arg( json( parent.data.value( "docId" ) ) ).arg( segment.value( "index" ).toInt() )
      .arg( json( entry->key.docId ) ).arg( height, 0, 'g', 17 ).arg( width, 0, 'g', 17 );
    QPointer<WebsiteFrames> guard( owner );
    auto stamp = epoch;
    bridge->runInFrame( parent.frame, script, [guard, stamp, entry]( QVariant const & value ) {
      if( !guard || !guard->d->current( stamp ) || !guard->d->unchanged( entry->chain ) || !value.toBool() ) return;
      auto & state = *guard->d;
      auto root = state.roots.find( entry->route.first().toMap().value( "name" ).toString() );
      if( entry->route.size() == 1 && root != state.roots.end() ) root->measured = true;
    } );
  }
  void finishScan( std::shared_ptr<Scan> const & scan )
  {
    scanning = false;
    if( !scan->valid || !std::all_of( scan->nodes.cbegin(), scan->nodes.cend(),
        [this]( Node const & node ) { return unchanged( node ); } ) ) return;
    auto const & main = scan->nodes.first().data;
    if( main.value( "generation" ).toInt() != generation || main.value( "docId" ).toString().isEmpty() ) return;
    topDoc = main.value( "docId" ).toString();
    QHash<QString, Root> nextRoots;
    for( auto const & value : main.value( "children" ).toList() )
    {
      auto route = value.toMap();
      auto name = route.value( "name" ).toString();
      if( !route.value( "root" ).toBool() || !name.startsWith( "gdexpandframe-" ) ) continue;
      Root root{route, clock.elapsed(), false, false};
      auto previous = roots.constFind( name );
      if( previous != roots.cend() && previous->route.value( "token" ) == route.value( "token" ) ) root = *previous;
      root.route = route;
      nextRoots.insert( name, root );
    }
    roots = std::move( nextRoots );
    QHash<QString, int> counts;
    for( auto const & node : scan->nodes ) ++counts[node.data.value( "docId" ).toString()];
    QHash<QString, std::shared_ptr<Entry>> nextEntries;
    QVector<std::shared_ptr<Entry>> rearm;
    for( int i = 1; i < scan->nodes.size(); ++i )
    {
      auto const & node = scan->nodes.at( i );
      auto nonce = node.data.value( "docId" ).toString();
      if( nonce.isEmpty() || counts.value( nonce ) != 1 || node.data.value( "generation" ).toInt() != generation ||
          !node.data.value( "ready" ).toBool() ) continue;
      auto entry = std::make_shared<Entry>();
      entry->key = {node.path, {}, nonce, generation};
      bool valid = true;
      for( int index = i; index > 0; index = scan->nodes.at( index ).parent )
      {
        auto const & child = scan->nodes.at( index );
        auto const & parent = scan->nodes.at( child.parent );
        QVariantMap segment;
        int matches = 0;
        for( auto const & value : parent.data.value( "children" ).toList() )
        {
          auto route = value.toMap();
          if( route.value( "docId" ) == child.data.value( "docId" ) ) { segment = route; ++matches; }
        }
        if( matches != 1 || parent.data.value( "generation" ).toInt() != generation ||
            counts.value( parent.data.value( "docId" ).toString() ) != 1 ) { valid = false; break; }
        entry->route.prepend( QVariantMap{{"name", segment.value( "name" )}, {"index", segment.value( "index" )}} );
        entry->key.namePath.prepend( segment.value( "name" ).toString() );
        entry->chain.prepend( child );
      }
      if( !valid || entry->route.isEmpty() ) continue;
      entry->chain.prepend( scan->nodes.first() );
      auto rootName = entry->key.namePath.first();
      if( !roots.contains( rootName ) ) continue;
      auto rootUrl = entry->chain.at( 1 ).frame.url();
      if( rootUrl.scheme() != QLatin1String( "http" ) && rootUrl.scheme() != QLatin1String( "https" ) ) continue;
      entry->dictionary = rootName.mid( 14 );
      auto previous = entries.value( nonce );
      bool same = previous && previous->key.childPath == entry->key.childPath && previous->route == entry->route &&
        previous->chain.last().frame == entry->chain.last().frame;
      if( same )
      {
        previous->chain = entry->chain;
        entry = previous;
      }
      auto pendingReady = ready.constFind( nonce );
      bool received = pendingReady != ready.cend() && pendingReady->value( "path" ).toList() == entry->route;
      if( !same || received ) rearm.append( entry );
      nextEntries.insert( nonce, entry );
    }
    for( auto const & previous : entries )
      if( nextEntries.value( previous->key.docId ) != previous ) stop( previous );
    entries = std::move( nextEntries );
    ready.clear();
    for( auto const & entry : rearm ) arm( entry );
  }
  void pulse()
  {
    if( !current( epoch ) ) return;
    auto names = roots.keys();
    for( auto const & name : names )
    {
      auto & root = roots[name];
      if( root.measured || root.reported || clock.elapsed() - root.started < 10000 ) continue;
      root.reported = true;
      auto route = root.route;
      auto script = QStringLiteral( "/*gd-frame-unavailable*/window.gdFrameUnavailable&&gdFrameUnavailable(%1,%2)" )
        .arg( route.value( "index" ).toInt() ).arg( json( route.value( "token" ) ) );
      QPointer<WebsiteFrames> guard( owner );
      auto stamp = epoch;
      bridge->run( script, [guard, stamp, name, route]( QVariant const & value ) {
        if( !guard || !guard->d->current( stamp ) || !value.toBool() ) return;
        auto const & roots = guard->d->roots;
        auto root = roots.constFind( name );
        if( root == roots.cend() || root->measured || root->route.value( "token" ) != route.value( "token" ) ) return;
        QString const id = name.mid( 14 );
        qWarning().noquote() << "WebsiteFrame: unavailable dict=" + id + " url=" + route.value( "url" ).toString();
        emit guard->frameUnavailable( id, QStringLiteral(
          "This site could not be framed. Try inline mode in the website dictionary preferences." ) );
      } );
    }
    words.erase( std::remove_if( words.begin(), words.end(), []( auto const & word ) { return word->done; } ), words.end() );
    owner->discover();
  }
  void verifyChain( std::shared_ptr<Entry> const & entry, std::function<void(bool)> done )
  {
    struct Verification { int remaining; bool valid = true; std::function<void(bool)> done; };
    auto check = std::make_shared<Verification>( Verification{int( entry->chain.size() ), true, std::move( done )} );
    QPointer<WebsiteFrames> guard( owner );
    auto stamp = epoch;
    for( int index = 0; index < entry->chain.size(); ++index )
    {
      auto const node = entry->chain.at( index );
      auto script = QStringLiteral( "/*gd-frame-event-identity*/(()=>{const f=window.__gdWebsiteFrame;"
        "if(!f||f.generation!==%1||f.docId!==%2)return false;" ).arg( generation ).arg( json( node.data.value( "docId" ) ) );
      if( index + 1 < entry->chain.size() )
      {
        auto segment = entry->route.at( index ).toMap();
        script += QStringLiteral( "return gdFrameChildren().some(c=>c.index===%1&&c.name===%2&&c.docId===%3)" )
          .arg( segment.value( "index" ).toInt() ).arg( json( segment.value( "name" ) ) )
          .arg( json( entry->chain.at( index + 1 ).data.value( "docId" ) ) );
      }
      else script += QStringLiteral( "return true" );
      script += QStringLiteral( "})()" );
      bridge->runInFrame( node.frame, script, [guard, stamp, entry, check]( QVariant const & value ) {
        if( !guard || !guard->d->current( stamp ) ) return;
        auto & state = *guard->d;
        check->valid = check->valid && value.toBool() && state.entries.value( entry->key.docId ) == entry && state.unchanged( entry->chain );
        if( --check->remaining == 0 ) check->done( check->valid );
      } );
    }
  }
  void acceptMessage( std::shared_ptr<Entry> const & entry, QVariantMap const & message )
  {
    auto kind = message.value( "gdFrameEvent" ).toString();
    if( kind == QLatin1String( "click" ) || kind == QLatin1String( "context" ) )
    {
      emit owner->frameText( entry->dictionary, message.value( "text" ).toString().left( TextLimit ), kind == QLatin1String( "context" ) );
      return;
    }
    double height = message.value( "height" ).toDouble(), width = message.value( "width" ).toDouble();
    if( kind == QLatin1String( "resize" ) )
    {
      if( height != entry->height && !entry->sampling ) arm( entry );
      return;
    }
    if( !entry->sampling || clock.elapsed() - entry->started >= 10000 ) return;
    entry->equal = height == entry->height ? entry->equal + 1 : 1;
    entry->height = height;
    if( entry->equal >= 3 ) stop( entry );
    apply( entry, height, width );
  }
  void wordStep( std::shared_ptr<Word> const & word, Node const & node, QPointF point, int depth )
  {
    if( word->done ) return;
    if( depth > MaxDepth || !current( word->epoch ) || !unchanged( word->chain ) || !unchanged( node ) )
      { word->finish(); return; }
    word->chain.append( node );
    auto nonce = node.data.value( "docId" ).toString();
    auto script = QStringLiteral(
      "/*gd-frame-word*/(()=>{const f=window.__gdWebsiteFrame;return f&&f.generation===%1&&f.docId===%2"
      "?gdFrameHit(%3,%4):null})()" ).arg( generation ).arg( json( nonce ) )
      .arg( point.x(), 0, 'g', 17 ).arg( point.y(), 0, 'g', 17 );
    QPointer<WebsiteFrames> guard( owner );
    bridge->runInFrame( node.frame, script, [guard, word, nonce, depth, node]( QVariant const & value ) {
      if( word->done ) return;
      if( !guard || !guard->d->current( word->epoch ) || !guard->d->unchanged( word->chain ) )
        { word->finish(); return; }
      auto answer = value.toMap();
      if( answer.value( "docId" ).toString() != nonce ) { word->finish(); return; }
      if( answer.contains( "word" ) ) { word->finish( answer.value( "word" ).toString().left( TextLimit ) ); return; }
      auto & state = *guard->d;
      auto child = state.entries.value( answer.value( "child" ).toString() );
      if( !child || child->chain.at( child->chain.size() - 2 ).frame != node.frame ||
          child->route.last().toMap().value( "index" ) != answer.value( "index" ) ||
          child->route.last().toMap().value( "name" ) != answer.value( "name" ) ||
          !numeric( answer.value( "x" ) ) || !numeric( answer.value( "y" ) ) ) { word->finish(); return; }
      state.wordStep( word, child->chain.last(), {answer.value( "x" ).toDouble(), answer.value( "y" ).toDouble()}, depth + 1 );
    } );
  }
};

WebsiteFrames::WebsiteFrames( QWebEnginePage * page, ArticleBridge * bridge, QObject * parent ):
  QObject( parent ), d( std::make_unique<State>( this, page, bridge ) )
{
  Q_ASSERT( page && bridge );
  connect( bridge, &ArticleBridge::generationChanged, this, &WebsiteFrames::reset );
  connect( bridge, &ArticleBridge::frameEvent, this, &WebsiteFrames::receive );
  connect( bridge, &ArticleBridge::bridgeReady, this, [this]( int generation ) {
    if( generation != d->generation ) return;
    d->active = true;
    d->tick.start( 250 );
    discover();
  } );
  connect( &d->tick, &QTimer::timeout, this, [this] { d->pulse(); } );
  connect( page, &QObject::destroyed, this, [this] {
    d->page = nullptr; d->active = false; ++d->epoch; d->tick.stop(); d->cancelWords();
  } );
  connect( bridge, &QObject::destroyed, this, [this] {
    d->bridge = nullptr; d->active = false; ++d->epoch; d->tick.stop(); d->cancelWords();
  } );
  reset( bridge->generation() );
}
WebsiteFrames::~WebsiteFrames()
{
  d->closing = true;
  for( auto const & entry : d->entries ) d->stop( entry );
  d->tick.stop();
  if( d->page && !d->installedScript.isEmpty() )
    for( auto const & script : d->page->scripts().find( d->installedScript ) ) d->page->scripts().remove( script );
  d->cancelWords();
}
void WebsiteFrames::reset( int generation )
{
  if( !d->bridge || generation != d->bridge->generation() || generation < 1 ) return;
  d->active = false;
  ++d->epoch;
  d->tick.stop();
  d->scanning = false;
  for( auto const & entry : d->entries ) d->stop( entry );
  d->entries.clear(); d->roots.clear(); d->ready.clear(); d->topDoc.clear();
  d->generation = generation;
  QPointer<WebsiteFrames> guard( this );
  d->cancelWords();
  if( !guard || !d->page ) return;
  if( !d->installedScript.isEmpty() )
    for( auto const & script : d->page->scripts().find( d->installedScript ) ) d->page->scripts().remove( script );
  auto frames = readScript( ":/scripts/website-frame.js" );
  auto dom = readScript( ":/scripts/article-dom.js" );
  if( frames.isEmpty() || dom.isEmpty() ) return;
  frames.replace( QStringLiteral( "/* GD_GENERATION */" ), QString::number( generation ) );
  // The pure DOM helpers need no channel. The top document already has them;
  // remote documents receive no bootstrap, facade, or WebChannel transport.
  auto source = QStringLiteral( "if(window!==window.top){window.__gdDocumentGeneration=%1;\n%2\n}\n%3" )
    .arg( generation ).arg( dom, frames );
  QWebEngineScript script;
  d->installedScript = QStringLiteral( "gd-frames-%1" ).arg( generation );
  script.setName( d->installedScript );
  script.setInjectionPoint( QWebEngineScript::DocumentCreation );
  script.setWorldId( QWebEngineScript::MainWorld );
  script.setRunsOnSubFrames( true );
  script.setSourceCode( source );
  d->page->scripts().insert( script );
}
void WebsiteFrames::discover()
{
  if( !d->current( d->epoch ) || d->scanning ) return;
  d->scanning = true;
  auto scan = std::make_shared<State::Scan>();
  scan->epoch = d->epoch;
  std::function<void(QWebEngineFrame const &, QVector<int>, int)> visit = [&]( auto const & frame, auto path, int parent ) {
    if( !frame.isValid() || path.size() > MaxDepth ) return;
    // A provisional child has no document yet. Chromium can discard its JS
    // callback during initial commit, which would strand the bridge queue.
    if( !path.isEmpty() && frame.url().isEmpty() ) return;
    int index = scan->nodes.size();
    scan->nodes.append( {frame, path, parent, {}} );
    auto children = frame.children();
    for( int child = 0; child < children.size(); ++child )
    {
      auto next = path; next.append( child ); visit( children.at( child ), next, index );
    }
  };
  visit( d->page->mainFrame(), {}, -1 );
  scan->remaining = scan->nodes.size();
  if( !scan->remaining ) { d->scanning = false; return; }
  QPointer<WebsiteFrames> guard( this );
  QString const script = QStringLiteral(
    "/*gd-frame-discover*/(()=>{const f=window.__gdWebsiteFrame;return f?"
    "{docId:f.docId,generation:f.generation,ready:f.ready,children:gdFrameChildren()}:null})()" );
  for( int index = 0; index < scan->nodes.size(); ++index )
    d->bridge->runInFrame( scan->nodes.at( index ).frame, script, [guard, scan, index]( QVariant const & value ) {
      if( !guard || !guard->d->current( scan->epoch ) ) return;
      auto & state = *guard->d;
      if( !state.unchanged( scan->nodes.at( index ) ) ) scan->valid = false;
      scan->nodes[index].data = value.toMap();
      if( --scan->remaining == 0 ) state.finishScan( scan );
    } );
}
void WebsiteFrames::receive( QVariantMap const & message )
{
  if( !d->current( d->epoch ) || !integer( message.value( "generation" ), std::numeric_limits<int>::max() ) ||
      message.value( "generation" ).toInt() != d->generation ) return;
  auto kind = message.value( "gdFrameEvent" ).toString();
  if( kind != QLatin1String( "ready" ) && kind != QLatin1String( "click" ) && kind != QLatin1String( "context" ) &&
      kind != QLatin1String( "measure" ) && kind != QLatin1String( "resize" ) ) return;
  if( message.value( "docId" ).metaType().id() != QMetaType::QString || message.value( "name" ).metaType().id() != QMetaType::QString ||
      message.value( "path" ).metaType().id() != QMetaType::QVariantList ) return;
  auto nonce = message.value( "docId" ).toString();
  auto route = message.value( "path" ).toList();
  if( nonce.isEmpty() || nonce.size() > 256 || route.isEmpty() || route.size() > MaxDepth ||
      !message.value( "name" ).toString().startsWith( "gdexpandframe-" ) ) return;
  for( auto const & item : route )
  {
    auto segment = item.toMap();
    if( segment.value( "name" ).metaType().id() != QMetaType::QString || segment.value( "name" ).toString().size() > 1024 ||
        !integer( segment.value( "index" ), 65535 ) ) return;
  }
  if( route.first().toMap().value( "name" ) != message.value( "name" ) ) return;
  if( kind == QLatin1String( "ready" ) )
  {
    if( d->ready.size() < 1024 ) d->ready.insert( nonce, message );
    discover();
    return;
  }
  auto entry = d->entries.value( nonce );
  if( !entry || entry->key.generation != d->generation || entry->route != route || !d->unchanged( entry->chain ) ) return;
  bool const text = kind == QLatin1String( "click" ) || kind == QLatin1String( "context" );
  if( text ? message.value( "text" ).metaType().id() != QMetaType::QString :
      ( !measurement( message.value( "height" ) ) || !measurement( message.value( "width" ) ) ) ) return;
  // A same-process navigation can retain the native value. Re-read the nonce
  // before accepting even a well-shaped event from the previous document.
  QPointer<WebsiteFrames> guard( this );
  auto stamp = d->epoch;
  d->verifyChain( entry, [guard, stamp, entry, message]( bool valid ) {
    if( guard && guard->d->current( stamp ) && valid ) guard->d->acceptMessage( entry, message );
  } );
}
void WebsiteFrames::wordAtPoint( QPointF const & point, std::function<void( QString const & )> callback )
{
  auto word = std::make_shared<State::Word>();
  word->callback = std::move( callback ); word->epoch = d->epoch;
  if( !d->current( d->epoch ) || d->topDoc.isEmpty() || !std::isfinite( point.x() ) || !std::isfinite( point.y() ) ||
      !std::isfinite( d->page->zoomFactor() ) || d->page->zoomFactor() <= 0 ) { word->finish(); return; }
  d->words.append( word );
  QTimer::singleShot( 500, this, [word] { word->finish(); } );
  State::Node main{d->page->mainFrame(), {}, -1, {{"docId", d->topDoc}}};
  d->wordStep( word, main, point / d->page->zoomFactor(), 0 );
}
