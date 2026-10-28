/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */
#include "articleview.hh"
#include "articlebridge.hh"
#include "articlewebview.hh"
#include "articleschemes.hh"
#include "websiteframes.hh"
#include "folding.hh"
#include "wstring_qt.hh"
#include "wildcard.hh"
#include "webmultimediadownload.hh"
#include "programs.hh"
#include "gestures.hh"
#include <QApplication>
#include <QClipboard>
#include <QDeadlineTimer>
#include <QDesktopServices>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>
#include <QShowEvent>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrlQuery>
#include <QWebEngineContextMenuRequest>
#include <QWebEngineFindTextResult>
#include <QWebEngineHistory>
#include <algorithm>
#include <cmath>
#include <optional>

namespace {
constexpr int RestoreDeadlineMs = 500;
QString json( QVariant const & value )
{
  auto bytes = QJsonDocument( QJsonArray{QJsonValue::fromVariant( value )} ).toJson( QJsonDocument::Compact );
  return QString::fromUtf8( bytes.mid( 1, bytes.size() - 2 ) );
}
bool finiteNumber( QVariant const & value )
{
  int type = value.metaType().id();
  return ( type == QMetaType::Double || type == QMetaType::Int || type == QMetaType::UInt ||
           type == QMetaType::LongLong || type == QMetaType::ULongLong ) && std::isfinite( value.toDouble() );
}
std::optional<ArticleState> readingState( QVariant const & value )
{
  if( value.metaType().id() != QMetaType::QVariantMap ) return {};
  auto map = value.toMap();
  if( map.value( "currentArticle" ).metaType().id() != QMetaType::QString ||
      !finiteNumber( map.value( "sx" ) ) || !finiteNumber( map.value( "sy" ) ) ) return {};
  return ArticleState{map.value( "currentArticle" ).toString(), map.value( "sx" ).toDouble(),
                      map.value( "sy" ).toDouble(), true};
}
void cancelRequests( std::list<sptr<Dictionary::DataRequest>> & requests, QObject * receiver )
{
  auto pending = std::move( requests );
  requests.clear();
  for( auto & request : pending )
  {
    QObject::disconnect( request.get(), nullptr, receiver, nullptr );
    try { if( !request->isFinished() ) request->cancel(); }
    catch( std::exception const & e ) { qWarning( "Article resource cancellation: %s", e.what() ); }
  }
}
QString host( QString const & url )
{
  auto result = QUrl( url ).host().toLower();
  if( result.endsWith( '.' ) ) result.chop( 1 );
  return result;
}
// Preserve normalization and UTF-16 boundaries while matching PCRE in C++, never
// send Qt's regular-expression syntax to Chromium's different RegExp engine.
struct NormalizedText
{
  QString text;
  QVector<int> starts, ends;
  NormalizedText( QString const & original, bool diacritics )
  {
    for( int i = 0; i < original.size(); )
    {
      int const begin = i;
      int const length = original.at( i ).isHighSurrogate() && i + 1 < original.size() &&
                         original.at( i + 1 ).isLowSurrogate() ? 2 : 1;
      QString piece = original.mid( i, length );
      i += length;
      if( diacritics ) piece = gd::toQString( Folding::applyDiacriticsOnly( gd::toWString( piece ) ) );
      else piece.remove( QChar( 0x301 ) );
      if( piece.isEmpty() )
      {
        if( !ends.isEmpty() ) ends.last() = i;
        continue;
      }
      for( int j = 0; j < piece.size(); ++j ) { starts.append( begin ); ends.append( i ); }
      text += piece;
    }
  }
};
}

struct ArticleView::Capture
{
  QPointer<QWebEnginePage> page;
  QPointer<ArticleBridge> bridge;
  int generation = 0;
  bool done = false;
  std::vector<std::function<void(ArticleState const &)>> callbacks;
};
struct ArticleView::Hit
{
  QPointer<QWebEnginePage> page;
  QPointer<ArticleBridge> bridge;
  int generation = 0;
  QDeadlineTimer deadline{InteractionDeadlineMs};
  bool done = false;
  QVariantMap value;
  std::function<void(QVariantMap const &)> callback;
};
struct ArticleView::State
{
  struct Intent { QUrl url; NavigationKind kind; };
  QPointer<QWebEnginePage> page;
  QPointer<ArticleBridge> bridge;
  QPointer<WebsiteFrames> frames;
  ArticleHistory history;
  std::optional<Intent> pending;
  std::shared_ptr<Capture> capture;
  std::shared_ptr<Hit> hit;
  QTimer * debounce = nullptr;
  std::optional<ArticleState> latest, debounced;
  QStringList articles;
  QString current;
  bool audio = false, scheduled = false, capturing = false, closing = false;
  bool committed = false, loading = false, ready = false, successful = false, restored = false;
  bool initialBlank = true;
  int generation = 0;
  NavigationKind kind = NavigationKind::NewLookup;
  quint64 navigation = 0, findQuery = 0, contextQuery = 0, ftsQuery = 0;
  QMap<QString, QPair<QString, QString>> frameLinks;
  QMap<QString, QString> frameContextText;
  QSet<QString> framedArticles;
  std::shared_ptr<ArticleRequestPolicy const> policy;
  QMap<QUrl, QList<QUrl>> knownFileUrls;
};

QString ArticleView::scrollToFromDictionaryId( QString const & id ) { return "gdfrom-" + id; }
ArticleView::ArticleView( QWidget * parent, QWebEngineProfile & profile, ArticleNetworkAccessManager & nm,
                         AudioPlayerPtr const & player, std::vector<sptr<Dictionary::Class>> const & dictionaries,
                         Instances::Groups const & groups_, bool popup, Config::Class const & config,
                         QAction & search, QAction * bar, GroupComboBox const * combo ):
  QFrame( parent ), d( std::make_unique<State>() ), articleNetMgr( nm ), audioPlayer( player ),
  allDictionaries( dictionaries ), groups( groups_ ), popupView( popup ), cfg( config ),
  pasteAction( this ), articleUpAction( this ), articleDownAction( this ), goBackAction( this ),
  goForwardAction( this ), selectCurrentArticleAction( this ), copyAsTextAction( this ), inspectAction( this ),
  openSearchAction( search ), dictionaryBarToggled( bar ), groupComboBox( combo )
{
  ui.setupUi( this );
  ui.definition->configure( &profile, const_cast<Config::Class *>( &cfg ) );
  d->debounce = new QTimer( this );
  d->debounce->setObjectName( "gdCaptureDebounce" );
  d->debounce->setSingleShot( true );
  d->debounce->setInterval( CaptureDebounceMs );
  connect( d->debounce, &QTimer::timeout, this, [this] { d->debounced = d->latest; } );
  bindDocument();
  auto action = [this]( QAction & a, QString const & name, QKeySequence const & key, auto slot ) {
    a.setText( name ); a.setShortcut( key ); a.setShortcutContext( Qt::WidgetWithChildrenShortcut );
    ui.definition->addAction( &a ); connect( &a, &QAction::triggered, this, slot );
  };
  action( goBackAction, tr( "Back" ), QKeySequence( "Alt+Left" ), &ArticleView::back );
  action( goForwardAction, tr( "Forward" ), QKeySequence( "Alt+Right" ), &ArticleView::forward );
  action( pasteAction, tr( "Paste" ), QKeySequence::Paste, &ArticleView::pasteTriggered );
  action( articleUpAction, tr( "Previous article" ), QKeySequence( "Alt+Up" ), &ArticleView::moveOneArticleUp );
  action( articleDownAction, tr( "Next article" ), QKeySequence( "Alt+Down" ), &ArticleView::moveOneArticleDown );
  action( selectCurrentArticleAction, tr( "Select Current Article" ), QKeySequence( "Ctrl+Shift+A" ), &ArticleView::selectCurrentArticle );
  action( copyAsTextAction, tr( "Copy as text" ), QKeySequence( "Ctrl+Shift+C" ), &ArticleView::copyAsText );
  action( inspectAction, tr( "Inspect" ), QKeySequence( Qt::Key_F12 ), &ArticleView::inspect );
  ui.definition->addAction( &openSearchAction );
  connect( &openSearchAction, &QAction::triggered, this, &ArticleView::openSearch );
  ui.definition->pageAction( QWebEnginePage::Copy )->setShortcut( QKeySequence::Copy );
  ui.definition->addAction( ui.definition->pageAction( QWebEnginePage::Copy ) );
  ui.definition->pageAction( QWebEnginePage::SelectAll )->setShortcut( QKeySequence::SelectAll );
  ui.definition->addAction( ui.definition->pageAction( QWebEnginePage::SelectAll ) );
  ui.definition->setContextMenuPolicy( Qt::CustomContextMenu );
  connect( ui.definition, &QWebEngineView::loadFinished, this, &ArticleView::loadFinished );
  connect( ui.definition, &QWebEngineView::titleChanged, this, &ArticleView::handleTitleChanged );
  connect( ui.definition, &QWebEngineView::urlChanged, this, &ArticleView::handleUrlChanged );
  connect( ui.definition, &QWidget::customContextMenuRequested, this, &ArticleView::contextMenuRequested );
  connect( ui.definition, &ArticleWebView::doubleClicked, this, &ArticleView::doubleClicked );
  connect( ui.definition, &ArticleWebView::userLink, this, [this]( QUrl url, bool newTab ) {
    // Capture the pre-dispatch state once: an emitter's slot cannot change the
    // reference document or the inherited article of this action afterwards.
    Contexts contexts;
    tryMangleWebsiteClickedUrl( url, contexts );
    QUrl const reference = d->page ? d->page->url() : QUrl();
    QString const article = getCurrentArticle();
    if( newTab && !popupView ) emit openLinkInNewTab( url, reference, article, contexts );
    else openLink( url, reference, article, contexts );
  } );
  connect( ui.definition, &ArticleWebView::navigationRequested, this, &ArticleView::requestNavigation );
  ui.definition->installEventFilter( this );
  ui.searchFrame->installEventFilter( this ); ui.ftsSearchFrame->installEventFilter( this );
  ui.searchFrame->hide(); ui.ftsSearchFrame->hide();
  expandOptionalParts = cfg.preferences.alwaysExpandOptionalParts;
  ui.definition->grabGesture( Gestures::GDPinchGestureType );
  ui.definition->grabGesture( Gestures::GDSwipeGestureType );
  ArticleUrls::Lookup blank; blank.blank = true;
  requestNavigation( ArticleUrls::lookupDocument( blank ), NavigationKind::NewLookup );
}
ArticleView::~ArticleView()
{
  d->closing = true;
  ++d->navigation; ++d->findQuery; ++d->contextQuery;
  d->pending.reset(); d->capture.reset(); d->debounce->stop();
  cancelRequests( resourceDownloadRequests, this );
  // Stop frame callbacks first. ArticleWebView retires the bridge, destroys its
  // page while the closed bridge still lives, then destroys the bridge.
  delete d->frames.data();
  delete ui.definition;
  cleanupTemp();
  try { audioPlayer->stop(); } catch( std::exception const & e ) { qWarning( "Audio stop: %s", e.what() ); }
}
QSize ArticleView::minimumSizeHint() const { return ui.searchFrame->minimumSizeHint(); }
void ArticleView::setGroupComboBox( GroupComboBox const * combo ) { groupComboBox = combo; }
void ArticleView::bindDocument()
{
  d->page = ui.definition->page();
  d->bridge = ui.definition->findChild<ArticleBridge *>( "gdArticleBridge", Qt::FindDirectChildrenOnly );
  Q_ASSERT( d->bridge );
  d->generation = d->bridge->generation();
  d->frames = new WebsiteFrames( d->page, d->bridge, this );
  connect( d->page, &QWebEnginePage::linkHovered, this, &ArticleView::linkHovered );
  connect( d->bridge, &ArticleBridge::generationChanged, this, [this]( int generation ) {
    d->generation = generation;
    d->capture.reset(); d->debounce->stop(); d->latest.reset(); d->debounced.reset();
    ++d->findQuery; ++d->contextQuery; ++d->ftsQuery;
    clearDocumentCaches();
  } );
  connect( d->bridge, &ArticleBridge::readingStateChanged, this, &ArticleView::updateReadingState );
  connect( d->bridge, &ArticleBridge::activeArticleChanged, this, [this]( QString const & id ) {
    if( d->current == id ) return;
    d->current = id;
    emit activeArticleChanged( this, getActiveArticleId() );
  } );
  connect( d->bridge, &ArticleBridge::bridgeReady, this, [this]( int generation ) {
    if( generation != d->generation ) return;
    d->ready = true; finishLoad();
  } );
  connect( d->frames, &WebsiteFrames::frameText, this, [this]( QString const & id, QString const & text, bool context ) {
    d->framedArticles.insert( id );
    if( context ) d->frameContextText[ id ] = text;
    else d->frameLinks[ id ] = {text, QStringLiteral( "click" )};
    setCurrentArticle( scrollToFromDictionaryId( id ) );
  } );
}
void ArticleView::run( QString const & script, std::function<void(QVariant const &)> callback )
{
  if( d->closing || !d->bridge || !d->page ) return;
  QPointer<ArticleView> owner( this );
  auto page = d->page; auto bridge = d->bridge; int const generation = d->generation;
  d->bridge->run( script, [owner, page, bridge, generation, callback = std::move( callback )]( QVariant const & value ) {
    if( !owner || owner->d->closing || !page || owner->d->page != page || !bridge ||
        owner->d->bridge != bridge || bridge->generation() != generation ) return;
    if( callback ) callback( value );
  } );
}
void ArticleView::updateReadingState( QVariantMap const & value )
{
  auto state = readingState( value );
  if( !state ) return;
  d->latest = state;
  d->debounce->start();
  auto articles = value.value( "articles" ).toStringList();
  bool audio = value.value( "hasAudio" ).toBool();
  d->framedArticles.clear();
  for( auto const & id : value.value( "framedArticles" ).toStringList() )
    if( articles.contains( id ) ) d->framedArticles.insert( id );
  QPointer<ArticleView> guard( this ); int generation = d->generation;
  if( articles != d->articles ) { d->articles = articles; emit articlesChanged( articles ); }
  if( !guard || d->generation != generation ) return;
  if( audio != d->audio ) { d->audio = audio; emit soundAvailabilityChanged( audio ); }
}
void ArticleView::clearDocumentCaches()
{
  QPointer<ArticleView> guard( this ); int generation = d->generation;
  bool hadArticles = !d->articles.isEmpty(), hadAudio = d->audio;
  d->articles.clear(); d->current.clear(); d->audio = false; d->frameLinks.clear();
  d->frameContextText.clear(); d->framedArticles.clear(); d->ready = false;
  if( d->hit ) { d->hit->done = true; d->hit->callback = {}; d->hit.reset(); }
  if( hadArticles ) emit articlesChanged( {} );
  if( !guard || d->generation != generation ) return;
  if( hadAudio ) emit soundAvailabilityChanged( false );
}
void ArticleView::captureState( std::function<void(ArticleState const &)> callback )
{
  if( d->closing || !d->bridge || !d->page || !callback ) return;
  if( d->capture && d->capture->generation == d->generation && !d->capture->done )
  {
    d->capture->callbacks.push_back( std::move( callback ) );
    return;
  }
  auto capture = std::make_shared<Capture>();
  capture->generation = d->generation; capture->page = d->page; capture->bridge = d->bridge;
  capture->callbacks.push_back( std::move( callback ) ); d->capture = capture;
  QPointer<ArticleView> guard( this );
  // The same queue operation has a deadline too: a held capture must not prevent
  // future actions/captures from running once the public callback has fallen back.
  d->bridge->enqueue( {}, QStringLiteral( "gdCaptureState()" ), [guard, capture]( QVariant const & value ) {
    if( !guard ) return;
    auto state = readingState( value );
    guard->finishCapture( capture, state ? *state : guard->d->debounced.value_or( ArticleState{} ), !state );
  }, CaptureDeadlineMs );
}
void ArticleView::finishCapture( std::shared_ptr<Capture> const & capture, ArticleState const & state, bool timedOut )
{
  if( d->closing || d->capture != capture || capture->done || !capture->page || d->page != capture->page ||
      !capture->bridge || d->bridge != capture->bridge || d->generation != capture->generation ) return;
  capture->done = true;
  d->capture.reset();
  if( timedOut ) qWarning( "ArticleView: capture timed out; using debounced reading state" );
  auto callbacks = std::move( capture->callbacks );
  QPointer<ArticleView> guard( this );
  for( auto & callback : callbacks )
  {
    if( !guard || !capture->page || !capture->bridge || d->generation != capture->generation ||
        d->page != capture->page || d->bridge != capture->bridge ) return;
    callback( state );
  }
}
void ArticleView::requestNavigation( QUrl const & url, NavigationKind kind )
{
  if( d->closing ) return;
  if( kind == NavigationKind::Back && !canGoBack() ) return;
  if( kind == NavigationKind::Forward && !canGoForward() ) return;
  auto parsed = ArticleUrls::parse( url, ArticleUrls::Use::DocumentLoad );
  if( parsed.kind == ArticleUrls::Kind::Invalid )
  { emit statusBarMessage( parsed.error, 10000 ); return; }
  d->pending = State::Intent{url, kind};
  if( d->capturing || d->scheduled ) return;
  d->scheduled = true;
  QTimer::singleShot( 0, this, [this] {
    d->scheduled = false;
    if( !d->pending || d->closing ) return;
    if( !d->committed ) { performNavigation(); return; }
    int const outgoing = d->page->history()->currentItemIndex();
    d->capturing = true;
    captureState( [this, outgoing]( ArticleState const & state ) {
      d->history.setCurrent( outgoing, state );
      d->capturing = false;
      // Return to the event queue: bridge.complete must retire this exact
      // capture before a new generation can install and dispatch its queue.
      QTimer::singleShot( 0, this, [this] { if( d->pending ) performNavigation(); } );
    } );
  } );
}
void ArticleView::performNavigation()
{
  if( d->closing || !d->pending ) return;
  auto intent = *d->pending; d->pending.reset();
  auto outgoing = d->page->history()->currentItemIndex();
  if( intent.kind == NavigationKind::NewLookup || intent.kind == NavigationKind::LinkClick ||
      intent.kind == NavigationKind::Redirect ) d->history.truncateAfter( outgoing );
  d->kind = intent.kind; ++d->navigation;
  d->loading = true; d->committed = false; d->ready = false; d->successful = false; d->restored = false;
  cancelRequests( resourceDownloadRequests, this );
  QPointer<ArticleView> guard( this );
  publishPolicy( intent.url );
  ui.definition->navigate( intent.url, intent.kind );
  if( !guard ) return;
  // Generation exhaustion replaces the page and bridge instead of wrapping.
  if( d->page != ui.definition->page() )
  {
    delete d->frames.data();
    bindDocument();
    publishPolicy( intent.url );
  }
  ui.definition->setCursor( Qt::WaitCursor );
}
void ArticleView::loadFinished( bool ok )
{
  if( !d->loading || !d->page || d->page->isLoading() ) return;
  // An aborted old load may finish after a new intent installed its scripts.
  // Only the committed engine document whose facade carries our generation is
  // eligible to finalize; no load event can release a new queue by itself.
  if( !ok ) { d->loading = false; ui.definition->unsetCursor(); return; }
  d->successful = true;
  finishLoad();
}
void ArticleView::finishLoad()
{
  if( !d->loading || !d->successful || !d->ready || d->restored || !d->page || !d->bridge ) return;
  d->restored = true; d->loading = false; d->committed = true;
  auto history = d->page->history();
  if( d->initialBlank )
  {
    // The initial successful document is the only place the engine is cleared.
    // A lookup may replace the queued constructor blank before it ever loads.
    if( QUrlQuery( d->page->url() ).queryItemValue( "blank" ) == "1" )
    { history->clear(); d->history.reset(); }
    d->initialBlank = false;
  }
  d->history.commit( d->page->url(), history->currentItemIndex(), d->kind );
  ArticleState restore;
  bool const recorded = d->history.stateFor( history->currentItemIndex(), &restore );
  auto parsed = ArticleUrls::parse( d->page->url(), ArticleUrls::Use::DocumentLoad );
  if( !recorded )
  {
    restore.currentArticle = parsed.lookup.scrollTo;
    restore.restoreScroll = false;
  }
  if( recorded || !restore.currentArticle.isEmpty() || d->kind == NavigationKind::Reload || !parsed.lookup.anchor.isEmpty() )
  {
    QVariantMap state{{"currentArticle", restore.currentArticle}, {"sx", restore.sx}, {"sy", restore.sy},
                      {"restoreScroll", restore.restoreScroll}};
    // Install one bounded settle operation in this document. The wall timer
    // covers hidden tabs whose requestAnimationFrame is suspended.
    run( QStringLiteral( R"JS((() => {
      const g = window.__gdDocumentGeneration, s = %1;
      if (window.__gdViewRestoreGeneration === g) return;
      window.__gdViewRestoreGeneration = g;
      if (!Number.isFinite(s.sx) || !Number.isFinite(s.sy)) return;
      if (s.currentArticle && window.gdMakeArticleActive) gdMakeArticleActive(s.currentArticle.slice(7));
      let last = -1, frames = 0, done = false;
      const apply = () => {
        if (done || window.__gdDocumentGeneration !== g) return;
        done = true;
        window.scrollTo(s.restoreScroll ? s.sx : 0, s.restoreScroll ? s.sy : 0);
        if (!s.restoreScroll && s.currentArticle && %2) {
          const article = document.getElementById(s.currentArticle); if (article) article.scrollIntoView();
        }
        const anchor = %3;
        if (anchor) {
          let target = document.getElementById(anchor) || document.getElementsByName(anchor)[0];
          if (!target && anchor.indexOf('_') === 33) {
            const tail = anchor.slice(anchor.indexOf('_', 34) + 1), prefix = anchor.slice(0, 34);
            target = Array.from(document.querySelectorAll('a[id],a[name]')).find(e =>
              [e.id, e.name].some(n => n && n.startsWith(prefix) && n.endsWith('_' + tail)));
          }
          if (target) target.scrollIntoView();
        }
        window.dispatchEvent(new Event('gd-active-article'));
      };
      window.__gdApplyViewRestore = apply;
      const settle = () => {
        if (done || window.__gdDocumentGeneration !== g) return;
        const height = document.documentElement.scrollHeight;
        if (height === last || ++frames >= 10) apply();
        else { last = height; requestAnimationFrame(settle); }
      };
      requestAnimationFrame(settle); return true;
    })())JS" ).arg( json( state ), !recorded && d->kind != NavigationKind::Reload ? "true" : "false", json( parsed.lookup.anchor ) ),
    [this]( QVariant const & installed ) {
      if( !installed.toBool() ) return;
      QPointer<QWebEnginePage> const page = d->page;
      int const generation = d->generation;
      // Chromium throttles timers in hidden pages. Keep the wall deadline on
      // the GUI event loop and re-enter the same apply-once document operation.
      QTimer::singleShot( RestoreDeadlineMs, this, [this, page, generation] {
        if( !page || page != d->page || generation != d->generation ) return;
        run( QStringLiteral( "if (window.__gdViewRestoreGeneration === %1 && window.__gdApplyViewRestore) window.__gdApplyViewRestore()" )
             .arg( generation ) );
      } );
    } );
  }
  run( "if (window.gdCheckArticlesNumber) gdCheckArticlesNumber()" );
  d->frames->discover();
  resolveFileUrls();
  ui.definition->unsetCursor();
  QPointer<ArticleView> guard( this ); int const generation = d->generation;
  emit pageLoaded( this );
  if( !guard || generation != d->generation ) return;
  if( !parsed.lookup.highlight.sourcePattern.isEmpty() ) highlightFTSResults();
}
void ArticleView::publishPolicy( QUrl const & url )
{
  auto object = d->page->findChild<QObject *>( "gdArticleRequestInterceptor", Qt::FindDirectChildrenOnly );
  auto interceptor = dynamic_cast<ArticleRequestInterceptor *>( object );
  if( !interceptor ) { qWarning( "ArticleView: missing page request interceptor" ); return; }
  auto policy = std::make_shared<ArticleRequestPolicy>();
  policy->blockExternal = cfg.preferences.disallowContentFromOtherSites;
  auto parsed = ArticleUrls::parse( url, ArticleUrls::Use::DocumentLoad );
  QSet<QString> contributors;
  if( parsed.kind == ArticleUrls::Kind::LookupDocument && !parsed.lookup.blank )
  {
    if( !parsed.lookup.dictionaries.isEmpty() )
      contributors = QSet<QString>( parsed.lookup.dictionaries.cbegin(), parsed.lookup.dictionaries.cend() );
    else
    {
      auto group = groups.findGroup( parsed.lookup.group );
      auto const & dictionaries = group ? group->dictionaries : allDictionaries;
      for( auto const & dictionary : dictionaries ) contributors.insert( QString::fromStdString( dictionary->getId() ) );
    }
    for( auto const & muted : parsed.lookup.muted ) contributors.remove( muted );
  }
  if( parsed.kind == ArticleUrls::Kind::PictureDocument ) contributors.insert( parsed.dictionaryId );
  for( auto const & website : cfg.webSites )
    if( contributors.contains( website.id ) && !host( website.url ).isEmpty() ) policy->dictionaryHosts.insert( host( website.url ) );
  for( auto const & wiki : cfg.mediawikis )
    if( contributors.contains( wiki.id ) && !host( wiki.url ).isEmpty() ) policy->dictionaryHosts.insert( host( wiki.url ) );
  // Context URLs are explicit replacements for this page's contributing website.
  for( auto it = parsed.lookup.contexts.cbegin(); it != parsed.lookup.contexts.cend(); ++it )
    if( contributors.contains( it.key() ) && !host( it.value() ).isEmpty() ) policy->dictionaryHosts.insert( host( it.value() ) );
  // Known file-valued contexts are resolved on the GUI before publishing. Dynamic
  // file URLs are resolved by the same GUI path when a document reports them.
  auto files = d->knownFileUrls.value( url );
  for( auto const & context : parsed.lookup.contexts )
    if( QUrl( context ).isLocalFile() ) files.append( QUrl( context ) );
  for( auto const & source : files )
  {
    QString path = source.toLocalFile();
    try
    {
      if( ArticleMaker::adjustFilePath( path ) )
        policy->localFileRedirects.insert( source.toString( QUrl::FullyEncoded ), QUrl::fromLocalFile( path ) );
    }
    catch( std::exception const & error ) { qWarning( "ArticleView: file resolution failed: %s", error.what() ); }
  }
  d->policy = policy;
  interceptor->setPolicy( policy );
}
void ArticleView::resolveFileUrls()
{
  toHtml( [this]( QString const & html ) {
    auto const url = d->page->url();
    auto const files = ArticleMaker::emittedFileUrls( html.toUtf8() );
    d->knownFileUrls[ url ] = files;
    if( files.isEmpty() ) return;
    // Complete the filesystem work on GUI, then atomically replace the policy
    // before the explicitly controlled DOM retry. No IO-thread GUI rendezvous.
    publishPolicy( url );
    QVariantMap redirects;
    for( auto it = d->policy->localFileRedirects.cbegin(); it != d->policy->localFileRedirects.cend(); ++it )
      redirects[ it.key() ] = it.value().toString( QUrl::FullyEncoded );
    if( redirects.isEmpty() ) return;
    run( QStringLiteral( R"JS((() => {
      const redirects = %1;
      for (const element of document.querySelectorAll('[src],[href],[poster],[data]')) {
        for (const name of ['src','href','poster','data']) {
          const value = element.getAttribute(name); if (!value) continue;
          let absolute; try { absolute = new URL(value, document.baseURI).href; } catch (_) { continue; }
          if (redirects[absolute]) element.setAttribute(name, redirects[absolute]);
        }
      }
      const replace = value => Object.entries(redirects).reduce((s,[from,to]) => s.split(from).join(to), value);
      for (const element of document.querySelectorAll('[style]')) {
        const original = element.getAttribute('style'), fixed = replace(original);
        if (fixed !== original) element.setAttribute('style', fixed);
      }
      for (const style of document.querySelectorAll('style')) {
        const fixed = replace(style.textContent); if (fixed !== style.textContent) style.textContent = fixed;
      }
    })())JS" ).arg( json( redirects ) ) );
  } );
}
void ArticleView::showDefinition( Config::InputPhrase const & phrase, unsigned group, QString const & scrollTo,
                                  Contexts const & contexts )
{
  audioPlayer->stop();
  ArticleUrls::Lookup lookup;
  lookup.phrase = phrase; lookup.group = group; lookup.groupSpecified = true;
  lookup.scrollTo = scrollTo; lookup.contexts = contexts;
  lookup.ignoreDiacritics = cfg.preferences.ignoreDiacritics;
  lookup.muted = getMutedForGroup( group ).split( ',', Qt::SkipEmptyParts );
  QPointer<ArticleView> guard( this );
  emit sendWordToHistory( phrase.phrase ); if( !guard ) return;
  closeSearch(); ui.highlightAllButton->setChecked( false );
  emit setExpandMode( expandOptionalParts ); if( !guard ) return;
  load( ArticleUrls::lookupDocument( lookup ) );
}
void ArticleView::showDefinition( QString const & word, unsigned group, QString const & scrollTo, Contexts const & contexts )
{ showDefinition( Config::InputPhrase::fromPhrase( word ), group, scrollTo, contexts ); }
void ArticleView::showDefinition( QString const & word, QStringList const & dictionaries, FtsHighlightSpec const & highlight,
                                  unsigned group, bool ignoreDiacritics )
{
  if( dictionaries.isEmpty() ) return;
  audioPlayer->stop();
  ArticleUrls::Lookup lookup;
  lookup.phrase = Config::InputPhrase::fromPhrase( word ); lookup.group = group; lookup.groupSpecified = true;
  lookup.dictionaries = dictionaries; lookup.highlight = highlight; lookup.ignoreDiacritics = ignoreDiacritics;
  QPointer<ArticleView> guard( this );
  emit sendWordToHistory( word ); if( !guard ) return;
  closeSearch(); ui.highlightAllButton->setChecked( false );
  emit setExpandMode( expandOptionalParts ); if( !guard ) return;
  load( ArticleUrls::lookupDocument( lookup ) );
}
void ArticleView::load( QUrl const & url ) { requestNavigation( url, NavigationKind::NewLookup ); }
void ArticleView::showAnticipation()
{ ArticleUrls::Lookup blank; blank.blank = true; load( ArticleUrls::lookupDocument( blank ) ); }
unsigned ArticleView::getGroup( QUrl const & url ) const
{ return url.scheme() == "gdlookup" ? QUrlQuery( url ).queryItemValue( "group" ).toUInt() : 0; }
unsigned ArticleView::getViewGroup() { return getGroup( d->page->url() ); }
QStringList ArticleView::getArticlesList() { return d->articles; }
QString ArticleView::getCurrentArticle() { return d->current; }
QString ArticleView::getActiveArticleId() { return d->current.startsWith( "gdfrom-" ) ? d->current.mid( 7 ) : QString(); }
bool ArticleView::hasSound() { return d->audio; }
QString ArticleView::getTitle() { return d->page ? d->page->title() : QString(); }
Config::InputPhrase ArticleView::getPhrase() const
{
  auto query = QUrlQuery( d->page ? d->page->url() : QUrl() );
  return Config::InputPhrase( query.queryItemValue( "word", QUrl::FullyDecoded ),
                              query.queryItemValue( "punctuation_suffix", QUrl::FullyDecoded ) );
}
void ArticleView::toHtml( std::function<void(QString const &)> callback )
{
  if( !d->page || !callback ) return;
  QPointer<ArticleView> guard( this ); auto page = d->page; auto bridge = d->bridge; int generation = d->generation;
  page->toHtml( [guard, page, bridge, generation, callback = std::move( callback )]( QString const & html ) {
    if( guard && page && bridge && guard->d->page == page && guard->d->bridge == bridge &&
        bridge->generation() == generation && !guard->d->closing ) callback( html );
  } );
}
void ArticleView::handleTitleChanged( QString const & title ) { if( !title.isEmpty() ) emit titleChanged( this, title ); }
void ArticleView::handleUrlChanged( QUrl const & url )
{
  auto group = groups.findGroup( getGroup( url ) );
  emit iconChanged( this, group ? group->makeIcon() : QIcon() );
}
bool ArticleView::canGoBack()
{
  if( !d->page ) return false;
  auto history = d->page->history();
  return history->canGoBack() && QUrlQuery( history->backItem().url() ).queryItemValue( "blank" ) != "1";
}
bool ArticleView::canGoForward() { return d->page && d->page->history()->canGoForward(); }
void ArticleView::back() { if( canGoBack() ) requestNavigation( d->page->history()->backItem().url(), NavigationKind::Back ); }
void ArticleView::forward() { if( canGoForward() ) requestNavigation( d->page->history()->forwardItem().url(), NavigationKind::Forward ); }
void ArticleView::reload() { if( d->page ) requestNavigation( d->page->url(), NavigationKind::Reload ); }
void ArticleView::setSelectionBySingleClick( bool enabled ) { ui.definition->setSelectionBySingleClick( enabled ); }
void ArticleView::jumpToDictionary( QString const & id, bool force )
{ if( force || getActiveArticleId() != id ) setCurrentArticle( scrollToFromDictionaryId( id ), true ); }
bool ArticleView::setCurrentArticle( QString const & id, bool move )
{
  if( !id.startsWith( "gdfrom-" ) || !d->articles.contains( id.mid( 7 ) ) ) return false;
  run( QStringLiteral( R"JS((() => {
    const id = %1, element = document.getElementById(id);
    if (!element) return false;
    if (window.gdMakeArticleActive) gdMakeArticleActive(id.slice(7));
    if (%2) element.scrollIntoView();
    window.dispatchEvent(new Event('gd-active-article')); return true;
  })())JS" ).arg( json( id ), move ? "true" : "false" ) );
  return true;
}
bool ArticleView::isFramedArticle( QString const & id ) const
{ return d->framedArticles.contains( id.startsWith( "gdfrom-" ) ? id.mid( 7 ) : id ); }
void ArticleView::selectCurrentArticle() { run( "if (window.gdSelectArticle) gdSelectArticle(" + json( getActiveArticleId() ) + ")" ); }
void ArticleView::playSound()
{
  run( "(() => { const a=window.gdAudioLinks; return a ? String(a[a.current] || a.first || '') : ''; })()",
       [this]( QVariant const & value ) { if( !value.toString().isEmpty() ) openLink( QUrl::fromEncoded( value.toString().toUtf8() ), d->page->url() ); } );
}
void ArticleView::tryMangleWebsiteClickedUrl( QUrl & url, Contexts & contexts )
{
  if( ( url.scheme() != "http" && url.scheme() != "https" ) || Dictionary::WebMultimediaDownload::isAudioUrl( url ) ) return;
  auto it = d->frameLinks.find( getActiveArticleId() );
  if( it == d->frameLinks.end() ) return;
  contexts[ it.key() ] = QString::fromLatin1( url.toEncoded() );
  QString text = it->first; d->frameLinks.erase( it );
  if( text.isEmpty() ) text = " ";
  url = QUrl( "gdlookup://localhost" ); url.setPath( '/' + text );
}
bool ArticleView::isExternalLink( QUrl const & url ) const
{ return ArticleUrls::parse( url, ArticleUrls::Use::UserAction ).kind == ArticleUrls::Kind::ExternalAction; }
void ArticleView::linkClicked( QUrl const & url )
{ openLink( url, d->page ? d->page->url() : QUrl(), getCurrentArticle() ); }
void ArticleView::openLink( QUrl const & url, QUrl const & ref, QString const & scrollTo, Contexts const & contexts )
{
  // A URL fragment is page navigation, not a lookup: the retained anchor path wins
  // and the fragment never becomes a headword or an anchor query item.
  if( url.hasFragment() && ( url.scheme() == "gdlookup" || url.scheme().isEmpty() ) )
  {
    run( "(() => {const a=" + json( url.fragment( QUrl::FullyDecoded ) ) +
         "; const e=document.getElementById(a)||document.getElementsByName(a)[0]; if(e)e.scrollIntoView();})()" );
    return;
  }
  // The pure parser only classifies and validates the wire form. Resolving the
  // reference view's group, dictionary subset, contexts and scroll state stays
  // here, exactly as the retained handler did.
  auto const parsed = ArticleUrls::parse( url, ArticleUrls::Use::UserAction );
  if( parsed.kind == ArticleUrls::Kind::Invalid ) { emit statusBarMessage( parsed.error, 10000 ); return; }
  if( parsed.kind == ArticleUrls::Kind::LookupAction )
  {
    auto lookup = parsed.lookup;
    auto reference = ArticleUrls::parse( ref, ArticleUrls::Use::DocumentLoad ).lookup;
    if( !lookup.groupSpecified ) { lookup.group = getGroup( ref ); lookup.groupSpecified = true; }
    // A reference FTS result keeps its explicit dictionary subset, which the maker
    // dispatches as the explicit-dictionary branch.
    if( lookup.dictionaries.isEmpty() ) lookup.dictionaries = reference.dictionaries;
    Contexts inherited = reference.contexts;
    for( auto it = contexts.cbegin(); it != contexts.cend(); ++it ) inherited[ it.key() ] = it.value();
    for( auto it = lookup.contexts.cbegin(); it != lookup.contexts.cend(); ++it ) inherited[ it.key() ] = it.value();
    lookup.contexts = inherited;
    // An unknown display name leaves the inherited scroll target in place; only a
    // name that resolves against the current dictionaries replaces it.
    if( lookup.scrollTo.isEmpty() ) lookup.scrollTo = scrollTo;
    if( !lookup.dictionaryName.isEmpty() )
      for( auto const & dictionary : allDictionaries )
        if( QString::fromStdString( dictionary->getName() ) == lookup.dictionaryName )
        { lookup.scrollTo = scrollToFromDictionaryId( QString::fromStdString( dictionary->getId() ) ); break; }
    // An explicit target anchor overrides the inherited context anchor.
    if( !lookup.anchor.isEmpty() ) lookup.contexts[ "gdanchor" ] = lookup.anchor;
    lookup.ignoreDiacritics = reference.ignoreDiacritics || cfg.preferences.ignoreDiacritics;
    lookup.muted = getMutedForGroup( lookup.group ).split( ',', Qt::SkipEmptyParts );
    // Retained effect order: stop playback, record the word, reset the search UI,
    // then change the document through one validated Use::DocumentLoad URL.
    audioPlayer->stop();
    QPointer<ArticleView> guard( this );
    emit sendWordToHistory( lookup.phrase.phrase );
    if( !guard ) return;
    closeSearch();
    ui.highlightAllButton->setChecked( false );
    emit setExpandMode( expandOptionalParts );
    if( !guard ) return;
    requestNavigation( ArticleUrls::lookupDocument( lookup ), NavigationKind::LinkClick );
    return;
  }
  if( parsed.kind == ArticleUrls::Kind::PictureDocument ) { requestNavigation( parsed.url, NavigationKind::LinkClick ); return; }
  if( url.scheme() == "gdprg" )
  {
    for( auto const & program : cfg.programs ) if( program.id == url.host() )
    {
      auto instance = new Programs::RunInstance;
      connect( instance, &Programs::RunInstance::finished, instance, &QObject::deleteLater );
      QString error;
      if( !instance->start( program, url.path().mid( 1 ), error ) ) { delete instance; emit statusBarMessage( error, 10000 ); }
      return;
    }
    emit statusBarMessage( tr( "The referenced audio program doesn't exist." ), 10000 ); return;
  }
  if( url.scheme() == "gdtts" )
  { emit statusBarMessage( tr( "Text to speech is unavailable on this platform." ), 10000 ); return; }
  if( url.scheme() == "bres" || url.scheme() == "gdau" || url.scheme() == "gdvideo" ||
      Dictionary::WebMultimediaDownload::isAudioUrl( url ) )
  {
    cancelRequests( resourceDownloadRequests, this ); resourceDownloadUrl = url;
    auto add = [this]( sptr<Dictionary::DataRequest> request ) {
      if( !request.get() ) return;
      resourceDownloadRequests.push_back( request );
      connect( request.get(), &Dictionary::Request::finished, this, &ArticleView::resourceDownloadFinished );
    };
    try
    {
      if( Dictionary::WebMultimediaDownload::isAudioUrl( url ) ) add( new Dictionary::WebMultimediaDownload( url, articleNetMgr ) );
      else if( url.host() == "search" )
      {
        auto group = groups.findGroup( getGroup( ref ) ); auto dictionaries = group ? group->dictionaries : allDictionaries;
        if( url.hasFragment() ) std::stable_partition( dictionaries.begin(), dictionaries.end(), [&]( auto const & dictionary ) {
          return QString::fromStdString( dictionary->getName() ) == url.fragment();
        } );
        for( auto const & dictionary : dictionaries ) add( dictionary->getResource( ArticleUrls::resourcePath( url ).toUtf8().constData() ) );
      }
      else { QString type; add( articleNetMgr.getResource( url, type ) ); }
      resourceDownloadFinished();
    }
    catch( std::exception const & e ) { emit statusBarMessage( QString::fromUtf8( e.what() ), 10000 ); }
    return;
  }
  if( isExternalLink( url ) ) QDesktopServices::openUrl( url );
}
ResourceToSaveHandler * ArticleView::saveResource( QUrl const & url, QString const & fileName )
{ return saveResource( url, d->page->url(), fileName ); }
ResourceToSaveHandler * ArticleView::saveResource( QUrl const & url, QUrl const & ref, QString const & fileName )
{
  auto handler = new ResourceToSaveHandler( this, fileName );
  try
  {
    if( url.host() == "search" && ( url.scheme() == "gdau" || url.scheme() == "bres" ) )
    {
      auto group = groups.findGroup( getGroup( ref ) ); auto const & dictionaries = group ? group->dictionaries : allDictionaries;
      for( auto const & dictionary : dictionaries ) handler->addRequest( dictionary->getResource( ArticleUrls::resourcePath( url ).toUtf8().constData() ) );
    }
    else if( url.scheme() == "http" || url.scheme() == "https" )
      handler->addRequest( new Dictionary::WebMultimediaDownload( url, articleNetMgr ) );
    else { QString type; handler->addRequest( articleNetMgr.getResource( url, type ) ); }
    handler->downloadFinished();
  }
  catch( std::exception const & e ) { emit statusBarMessage( QString::fromUtf8( e.what() ), 10000 ); handler->deleteLater(); }
  return handler;
}
void ArticleView::resourceDownloadFinished()
{
  bool hadRequests = !resourceDownloadRequests.empty();
  for( auto it = resourceDownloadRequests.begin(); it != resourceDownloadRequests.end(); )
  {
    auto request = *it;
    if( !request->isFinished() ) { ++it; continue; }
    it = resourceDownloadRequests.erase( it );
    if( request->dataSize() < 0 ) continue;
    try
    {
      auto bytes = makeDataRequestReader()->fullData( *request );
      if( resourceDownloadUrl.scheme() == "gdau" || Dictionary::WebMultimediaDownload::isAudioUrl( resourceDownloadUrl ) )
      {
        connect( audioPlayer.data(), &AudioPlayerInterface::error, this, &ArticleView::audioPlayerError, Qt::UniqueConnection );
        auto error = audioPlayer->play( bytes.constData(), int( bytes.size() ) );
        if( !error.isEmpty() ) emit statusBarMessage( tr( "Failed to play sound file: %1" ).arg( error ), 10000 );
      }
      else
      {
        cleanupTemp();
        QTemporaryFile file( QDir::temp().filePath( "XXXXXX-" + resourceDownloadUrl.fileName() ) );
        if( !file.open() || file.write( bytes ) != bytes.size() )
        { emit statusBarMessage( tr( "Failed to create temporary file." ), 10000 ); return; }
        file.setAutoRemove( false ); auto name = file.fileName(); file.close(); desktopOpenedTempFiles.insert( name );
        if( !QDesktopServices::openUrl( QUrl::fromLocalFile( name ) ) ) emit statusBarMessage( tr( "Failed to open %1" ).arg( name ), 10000 );
      }
      cancelRequests( resourceDownloadRequests, this ); return;
    }
    catch( std::exception const & e ) { emit statusBarMessage( QString::fromUtf8( e.what() ), 10000 ); }
  }
  if( hadRequests && resourceDownloadRequests.empty() ) emit statusBarMessage( tr( "The referenced resource failed to download." ), 10000 );
}
void ArticleView::audioPlayerError( QString const & message )
{ emit statusBarMessage( tr( "Audio Player: %1" ).arg( message ), 10000 ); }
void ArticleView::cleanupTemp()
{
  for( auto it = desktopOpenedTempFiles.begin(); it != desktopOpenedTempFiles.end(); )
    if( QFile::remove( *it ) ) it = desktopOpenedTempFiles.erase( it ); else ++it;
}
QString ArticleView::getMutedForGroup( unsigned group )
{
  if( !dictionaryBarToggled || !dictionaryBarToggled->isChecked() ) return {};
  auto configGroup = cfg.getGroup( group );
  Config::MutedDictionaries const * muted = group == Instances::Group::AllGroupId ?
    ( popupView ? &cfg.popupMutedDictionaries : &cfg.mutedDictionaries ) : configGroup ?
    ( popupView ? &configGroup->popupMutedDictionaries : &configGroup->mutedDictionaries ) : nullptr;
  if( !muted ) return {};
  auto instance = groups.findGroup( group );
  QStringList ids;
  for( auto const & dictionary : instance ? instance->dictionaries : allDictionaries )
  { auto id = QString::fromStdString( dictionary->getId() ); if( muted->contains( id ) ) ids.append( id ); }
  return ids.join( ',' );
}
void ArticleView::updateMutedContents()
{
  if( !d->page || d->page->url().scheme() != "gdlookup" ) return;
  auto url = d->page->url(); auto query = QUrlQuery( url ); auto muted = getMutedForGroup( getGroup( url ) );
  if( query.queryItemValue( "muted" ) == muted ) return;
  query.removeAllQueryItems( "muted" ); if( !muted.isEmpty() ) query.addQueryItem( "muted", muted );
  url.setQuery( query ); load( url );
}
void ArticleView::pasteTriggered()
{
  auto phrase = cfg.preferences.sanitizeInputPhrase( QApplication::clipboard()->text() );
  unsigned group = getViewGroup(); if( !group && groupComboBox ) group = groupComboBox->getCurrentGroup();
  if( phrase.isValid() ) showDefinition( phrase, group, getCurrentArticle() );
}
void ArticleView::moveOneArticleUp()
{
  if( d->articles.isEmpty() ) return;
  auto index = d->articles.indexOf( getActiveArticleId() );
  jumpToDictionary( d->articles.at( ( index + d->articles.size() - 1 ) % d->articles.size() ), true );
}
void ArticleView::moveOneArticleDown()
{
  if( d->articles.isEmpty() ) return;
  jumpToDictionary( d->articles.at( ( d->articles.indexOf( getActiveArticleId() ) + 1 ) % d->articles.size() ), true );
}
void ArticleView::linkHovered( QString const & link ) { emit statusBarMessage( link ); }
void ArticleView::requestHit( QPoint const & pos, std::function<void(QVariantMap const &)> callback )
{
  if( d->closing || !d->ready || !d->page || !d->bridge ) return;
  if( d->hit ) { d->hit->done = true; d->hit->callback = {}; }
  auto hit = std::make_shared<Hit>();
  hit->page = d->page; hit->bridge = d->bridge; hit->generation = d->generation;
  hit->callback = std::move( callback ); d->hit = hit;
  QPointer<ArticleView> guard( this );
  // The timer covers the complete top-document + native-frame continuation,
  // rather than starting a second budget after the first result arrives.
  QTimer::singleShot( InteractionDeadlineMs, this, [guard, hit] {
    if( !guard || hit->done ) return;
    auto value = hit->value; value[ "word" ] = QString();
    guard->finishHit( hit, value );
  } );
  auto x = pos.x() / ui.definition->zoomFactor(), y = pos.y() / ui.definition->zoomFactor();
  d->bridge->enqueue( {}, QStringLiteral( R"JS((() => {
    const x=%1, y=%2, e=document.elementFromPoint(x,y);
    if (!e) return {};
    const article=e.closest('[id^="gdfrom-"]'), image=e.closest('img'), link=e.closest('a[href]');
    let background='';
    for (let n=e; n && !background; n=n.parentElement) {
      const match=/^url\(["']?(.*?)["']?\)$/.exec(getComputedStyle(n).backgroundImage);
      if (match) { try { background=new URL(match[1],document.baseURI).href; } catch (_) {} }
    }
    let selection=''; const selected=window.getSelection();
    if (selected && !selected.isCollapsed) for (let i=0; i<selected.rangeCount; ++i)
      if (Array.from(selected.getRangeAt(i).getClientRects()).some(r => x>=r.left && x<=r.right && y>=r.top && y<=r.bottom))
        selection=selected.toString();
    return {article:article ? article.id : '', image:image ? image.currentSrc || image.src : '',
      link:link ? link.href : '', background, selection,
      frame:e.tagName==='IFRAME', word:window.gdWordAtPoint ? gdWordAtPoint(x,y) : ''};
  })())JS" ).arg( x ).arg( y ), [guard, hit, pos]( QVariant const & value ) {
    if( !guard || hit->done || guard->d->hit != hit || !hit->page || !hit->bridge ||
        guard->d->page != hit->page || guard->d->bridge != hit->bridge ||
        hit->generation != hit->bridge->generation() ) return;
    if( hit->deadline.hasExpired() ) { guard->finishHit( hit, {} ); return; }
    hit->value = value.toMap();
    if( !hit->value.value( "frame" ).toBool() ) { guard->finishHit( hit, hit->value ); return; }
    guard->d->frames->wordAtPoint( pos, [guard, hit]( QString const & word ) {
      if( !guard || hit->done ) return;
      auto result = hit->value;
      result[ "word" ] = hit->deadline.hasExpired() ? QString() : word;
      guard->finishHit( hit, result );
    } );
  }, InteractionDeadlineMs );
}
void ArticleView::finishHit( std::shared_ptr<Hit> const & hit, QVariantMap const & value )
{
  if( hit->done || d->hit != hit || d->closing || !hit->page || !hit->bridge ||
      d->page != hit->page || d->bridge != hit->bridge || d->generation != hit->generation ||
      hit->bridge->generation() != hit->generation ) return;
  hit->done = true;
  auto callback = std::move( hit->callback );
  d->hit.reset();
  if( callback ) callback( value );
}
void ArticleView::contextMenuRequested( QPoint const & position )
{
  // lastContextMenuRequest() is only valid during this event: every value used
  // below, including the position and reference document, is copied now.
  auto request = ui.definition->lastContextMenuRequest();
  QPoint const pos = request ? request->position() : position;
  QUrl const link = request ? request->linkUrl() : QUrl();
  QUrl const media = request ? request->mediaUrl() : QUrl();
  QString const linkText = request ? request->linkText() : QString();
  QString const selection = request ? request->selectedText() : ui.definition->selectedText();
  QUrl const reference = d->page ? d->page->url() : QUrl();
  unsigned const group = getGroup( reference );
  quint64 const operation = ++d->contextQuery;
  // Close an older popup even when the second request is in the same document.
  QPointer<ArticleView> owner( this );
  emit closePopupMenu();
  if( !owner ) return;
  requestHit( pos, [this, pos, link, media, linkText, selection, reference, group, operation]( QVariantMap const & value ) {
    if( operation != d->contextQuery ) return;
    QString article = value.value( "article" ).toString();
    if( !article.startsWith( "gdfrom-" ) || !d->articles.contains( article.mid( 7 ) ) ) article = getCurrentArticle();
    QPointer<ArticleView> owner( this );
    if( !article.isEmpty() ) setCurrentArticle( article );
    if( !owner ) return;
    auto menu = new QMenu( this ); menu->setAttribute( Qt::WA_DeleteOnClose );
    int const generation = d->generation; auto page = d->page; auto bridge = d->bridge;
    auto valid = [owner, page, bridge, generation, operation] {
      return owner && !owner->d->closing && page && bridge && owner->d->page == page &&
        owner->d->bridge == bridge && bridge->generation() == generation &&
        owner->d->generation == generation && owner->d->contextQuery == operation;
    };
    auto add = [this, menu, valid]( QString const & text, std::function<void()> callback ) {
      auto action = menu->addAction( text );
      connect( action, &QAction::triggered, this, [valid, callback] { if( valid() ) callback(); } );
      return action;
    };
    QUrl const target = link.isEmpty() ? QUrl( value.value( "link" ).toString() ) : link;
    if( !target.isEmpty() )
    {
      Contexts contexts;
      QUrl lookup = target;
      if( value.value( "frame" ).toBool() && isFramedArticle( article ) &&
          ( target.scheme() == "http" || target.scheme() == "https" ) &&
          !Dictionary::WebMultimediaDownload::isAudioUrl( target ) )
      {
        contexts[ article.mid( 7 ) ] = QString::fromLatin1( target.toEncoded() );
        QString text = linkText.isEmpty() ? d->frameContextText.value( article.mid( 7 ) ) : linkText;
        lookup = QUrl( "gdlookup://localhost" ); lookup.setPath( '/' + ( text.isEmpty() ? QStringLiteral( " " ) : text ) );
      }
      add( tr( "Open Link" ), [this, lookup, reference, article, contexts] { openLink( lookup, reference, article, contexts ); } );
      if( !popupView ) add( tr( "Open Link in New Tab" ), [this, lookup, reference, article, contexts] {
        emit openLinkInNewTab( lookup, reference, article, contexts );
      } );
      add( tr( "Copy Link" ), [target] { QApplication::clipboard()->setText( target.toString( QUrl::FullyEncoded ) ); } );
      if( isExternalLink( target ) ) add( tr( "Open Link in External Browser" ), [target] { QDesktopServices::openUrl( target ); } );
    }
    QString const text = ( selection.isEmpty() ? value.value( "word" ).toString() : selection ).trimmed();
    if( !text.isEmpty() && text.size() < 60 )
    {
      add( tr( "Look up \"%1\"" ).arg( text ), [this, text, group, article] { showDefinition( text, group, article ); } );
      if( !popupView ) add( tr( "Look up in New Tab" ), [this, text, group, article] { emit showDefinitionInNewTab( text, group, article, {} ); } );
      add( tr( "Add to history" ), [this, text] { emit forceAddWordToHistory( text ); } );
      add( tr( "Send to input line" ), [this, text] { emit sendWordToInputLine( text ); } );
      if( groupComboBox && groupComboBox->getCurrentGroup() != group )
      {
        unsigned const selectedGroup = groupComboBox->getCurrentGroup();
        add( tr( "Look up in selected group" ), [this, text, selectedGroup, article] { showDefinition( text, selectedGroup, article ); } );
      }
    }
    auto save = [this, add, valid, reference]( QUrl const & url, QString const & title ) {
      if( url.isEmpty() || popupView ) return;
      add( title, [this, url, reference, valid] {
        auto dialog = new QFileDialog( this, tr( "Save resource" ), cfg.resourceSavePath );
        dialog->setAttribute( Qt::WA_DeleteOnClose );
        dialog->setAcceptMode( QFileDialog::AcceptSave ); dialog->selectFile( url.fileName() );
        connect( dialog, &QFileDialog::fileSelected, this, [this, valid, url, reference]( QString const & destination ) {
          if( !valid() || destination.isEmpty() ) return;
          emit storeResourceSavePath( QFileInfo( destination ).absolutePath() );
          if( valid() ) saveResource( url, reference, destination );
        } );
        connect( this, &ArticleView::closePopupMenu, dialog, &QDialog::reject );
        connect( d->bridge, &ArticleBridge::generationChanged, dialog, &QDialog::reject );
        dialog->open();
      } );
    };
    save( media.isEmpty() ? QUrl( value.value( "image" ).toString() ) : media, tr( "Save resource..." ) );
    save( QUrl( value.value( "background" ).toString() ), tr( "Save background..." ) );
    auto copy = [selection] { if( !selection.isEmpty() ) QApplication::clipboard()->setText( selection ); };
    add( tr( "Copy" ), copy )->setEnabled( !selection.isEmpty() );
    add( tr( "Copy as text" ), copy )->setEnabled( !selection.isEmpty() );
    add( selectCurrentArticleAction.text(), [this, article] {
      run( "if (window.gdSelectArticle) gdSelectArticle(" + json( article.mid( 7 ) ) + ")" );
    } );
    add( tr( "Select All" ), [page] { page->triggerAction( QWebEnginePage::SelectAll ); } );
    add( tr( "Search" ), [this] { openSearch(); } );
    int count = 0;
    for( auto const & id : d->articles )
    {
      if( count++ >= cfg.preferences.maxDictionaryRefsInContextMenu )
      { add( tr( "More dictionaries..." ), [this] { emit showDictsPane(); } ); break; }
      for( auto const & dictionary : allDictionaries ) if( QString::fromStdString( dictionary->getId() ) == id )
        add( QString::fromStdString( dictionary->getName() ), [this, id] { jumpToDictionary( id, true ); } );
    }
    add( inspectAction.text(), [this] { inspect(); } );
    connect( this, &ArticleView::closePopupMenu, menu, &QMenu::close );
    connect( bridge, &ArticleBridge::generationChanged, menu, &QMenu::close );
    menu->popup( ui.definition->mapToGlobal( pos ) );
  } );
}
void ArticleView::doubleClicked( QPoint pos, Qt::KeyboardModifiers modifiers )
{
  auto page = d->page; QUrl const reference = page ? page->url() : QUrl();
  unsigned const group = getGroup( reference );
  requestHit( pos, [this, modifiers, page, reference, group]( QVariantMap const & value ) {
    QString article = value.value( "article" ).toString();
    if( !article.startsWith( "gdfrom-" ) || !d->articles.contains( article.mid( 7 ) ) ) article = getCurrentArticle();
    if( modifiers.testFlag( Qt::AltModifier ) && !value.value( "link" ).toString().isEmpty() ) return;
    if( !value.value( "image" ).toString().isEmpty() )
    { openLink( QUrl( value.value( "image" ).toString() ), reference, article ); return; }
    if( !cfg.preferences.doubleClickTranslates && !modifiers.testFlag( Qt::AltModifier ) ) return;
    QString word = value.value( "selection" ).toString().trimmed();
    if( word.isEmpty() ) word = value.value( "word" ).toString().trimmed();
    if( word.isEmpty() || word.size() >= 60 ) return;
    if( !popupView && ( modifiers & ( Qt::ControlModifier | Qt::ShiftModifier ) ) )
      emit showDefinitionInNewTab( word, group, article, {} );
    else openLink( QUrl( "bword:" + QString::fromLatin1( QUrl::toPercentEncoding( word ) ) ), reference, article );
  } );
}
void ArticleView::openSearch()
{
  if( !isVisible() ) return;
  if( ftsSearchIsOpened ) closeSearch();
  if( !searchIsOpened ) { searchIsOpened = true; ui.searchFrame->show(); ui.searchText->setText( getTitle() ); }
  ui.searchText->setFocus(); ui.searchText->selectAll();
  run( "window.getSelection().removeAllRanges()" );
  ui.searchText->setProperty( "noResults", false ); reloadStyleSheet();
}
void ArticleView::performFindOperation( bool restart, bool backwards, bool checkHighlight )
{
  quint64 const query = ++d->findQuery;
  QString text = ui.searchText->text();
  QWebEnginePage::FindFlags flags;
  if( ui.searchCaseSensitive->isChecked() ) flags |= QWebEnginePage::FindCaseSensitively;
  if( backwards ) flags |= QWebEnginePage::FindBackward;
  if( restart ) { d->page->findText( QString() ); run( "window.getSelection().removeAllRanges()" ); }
  if( checkHighlight || ui.highlightAllButton->isChecked() )
    run( "gdHighlightAll([" + json( text ) + "]," + ( ui.searchCaseSensitive->isChecked() ? "true" : "false" ) + ")" );
  else run( "gdClearHighlights()" );
  QPointer<ArticleView> guard( this ); auto page = d->page; int generation = d->generation;
  page->findText( text, flags, [guard, page, generation, query, text]( QWebEngineFindTextResult const & result ) {
    if( !guard || !page || guard->d->page != page || guard->d->generation != generation || guard->d->findQuery != query ) return;
    bool missing = !text.isEmpty() && result.numberOfMatches() == 0;
    guard->ui.searchText->setProperty( "noResults", missing ); guard->reloadStyleSheet();
    if( !text.isEmpty() ) emit guard->statusBarMessage( missing ? tr( "Phrase not found" ) :
      tr( "%1 of %2 matches" ).arg( result.activeMatch() ).arg( result.numberOfMatches() ) );
  } );
}
void ArticleView::on_searchPrevious_clicked() { if( searchIsOpened ) performFindOperation( false, true ); }
void ArticleView::on_searchNext_clicked() { if( searchIsOpened ) performFindOperation( false, false ); }
void ArticleView::on_searchText_textEdited() { performFindOperation( true, false ); }
void ArticleView::on_searchText_returnPressed() { on_searchNext_clicked(); }
void ArticleView::on_searchCloseButton_clicked() { closeSearch(); }
void ArticleView::on_searchCaseSensitive_clicked() { performFindOperation( true, false ); }
void ArticleView::on_highlightAllButton_clicked() { performFindOperation( false, false, true ); }
bool ArticleView::closeSearch()
{
  ++d->findQuery; ++d->ftsQuery;
  bool wasOpened = searchIsOpened || ftsSearchIsOpened;
  searchIsOpened = false; ftsSearchIsOpened = false; ftsPosition = 0; ftsCount = 0;
  ui.searchFrame->hide(); ui.ftsSearchFrame->hide();
  if( wasOpened )
  {
    if( d->page ) d->page->findText( QString() );
    run( "gdClearHighlights()" ); ui.definition->setFocus();
  }
  return wasOpened;
}
bool ArticleView::isSearchOpened() { return searchIsOpened; }
void ArticleView::reloadStyleSheet()
{
  for( auto widget = parentWidget(); widget; widget = widget->parentWidget() )
    if( !widget->styleSheet().isEmpty() ) { widget->setStyleSheet( widget->styleSheet() ); break; }
}
void ArticleView::highlightFTSResults( int attempt )
{
  auto parsed = ArticleUrls::parse( d->page->url(), ArticleUrls::Use::DocumentLoad );
  auto spec = parsed.lookup.highlight;
  if( spec.sourcePattern.isEmpty() || !spec.expression.isValid() ) return;
  auto const query = attempt == 0 ? ++d->ftsQuery : d->ftsQuery;
  QString pattern = spec.sourcePattern;
  if( parsed.lookup.ignoreDiacritics ) pattern = gd::toQString( Folding::applyDiacriticsOnly( gd::toWString( pattern ) ) );
  else pattern.remove( QChar( 0x301 ) );
  QRegularExpression expression( spec.wildcards ? wildcardsToRegexp( pattern ) : pattern, spec.expression.patternOptions() );
  if( !expression.isValid() ) return;
  run( "(() => { gdHighlightAll([],false); const token=gdSaveSelection(); return {token,text:__gdTextSnapshot.text,version:__gdTextSnapshot.version}; })()",
       [this, expression, query, attempt, ignore = parsed.lookup.ignoreDiacritics]( QVariant const & result ) {
    if( query != d->ftsQuery ) return;
    auto snapshot = result.toMap(); QString original = snapshot.value( "text" ).toString();
    NormalizedText normalized( original, ignore );
    QVariantList spans;
    auto matches = expression.globalMatch( normalized.text );
    while( matches.hasNext() )
    {
      auto match = matches.next(); auto start = match.capturedStart(), end = match.capturedEnd();
      if( start < 0 || end <= start || end > normalized.starts.size() ) continue;
      int first = normalized.starts.at( start ), length = normalized.ends.at( end - 1 ) - first;
      if( length > 500 ) continue;
      spans.append( QVariantMap{{"start", first}, {"length", length}} );
    }
    QVariantMap payload{{"text", original}, {"version", snapshot.value( "version" )}, {"spans", spans}};
    run( "(() => {const n=gdHighlightAll(" + json( payload ) + ",true);gdRestoreSelection(" + json( snapshot.value( "token" ) ) + ");return n;})()",
         [this, query, attempt]( QVariant const & value ) {
      if( query != d->ftsQuery ) return;
      int count = value.toInt();
      if( count < 0 )
      {
        if( attempt == 0 ) highlightFTSResults( 1 );
        else { run( "gdClearHighlights()" ); emit statusBarMessage( tr( "Article changed; run the search again." ), 10000 ); }
        return;
      }
      ftsCount = count; ftsPosition = 0; ftsSearchIsOpened = true;
      ui.ftsSearchFrame->show(); ui.ftsSearchPrevious->setEnabled( false ); ui.ftsSearchNext->setEnabled( count > 1 );
      ui.ftsSearchStatusLabel->setText( count ? tr( "%1 of %2 matches" ).arg( 1 ).arg( count ) : tr( "Phrase not found" ) );
      if( count ) run( "gdScrollToHit(0)" );
    } );
  } );
}
void ArticleView::performFtsFindOperation( bool backwards )
{
  if( !ftsSearchIsOpened || !ftsCount ) return;
  ftsPosition = std::clamp( ftsPosition + ( backwards ? -1 : 1 ), 0, ftsCount - 1 );
  run( QStringLiteral( "gdScrollToHit(%1)" ).arg( ftsPosition ) );
  ui.ftsSearchPrevious->setEnabled( ftsPosition > 0 ); ui.ftsSearchNext->setEnabled( ftsPosition + 1 < ftsCount );
  ui.ftsSearchStatusLabel->setText( tr( "%1 of %2 matches" ).arg( ftsPosition + 1 ).arg( ftsCount ) );
}
void ArticleView::on_ftsSearchPrevious_clicked() { performFtsFindOperation( true ); }
void ArticleView::on_ftsSearchNext_clicked() { performFtsFindOperation( false ); }
void ArticleView::copyAsText() { auto text = ui.definition->selectedText(); if( !text.isEmpty() ) QApplication::clipboard()->setText( text ); }
void ArticleView::inspect() { ui.definition->triggerPageAction( QWebEnginePage::InspectElement ); }
void ArticleView::receiveExpandOptionalParts( bool expand ) { if( expandOptionalParts != expand ) switchExpandOptionalParts(); }
void ArticleView::switchExpandOptionalParts()
{
  expandOptionalParts = !expandOptionalParts;
  QPointer<ArticleView> guard( this );
  emit setExpandMode( expandOptionalParts );
  if( !guard ) return;
  run( QStringLiteral( R"JS((() => {
    const expand=%1;
    for (const node of document.querySelectorAll('.dsl_opt'))
      node.style.setProperty('display', expand ? 'inline' : 'none', 'important');
    for (const node of document.querySelectorAll('.hidden_expand_opt'))
      node.style.setProperty('display', expand ? 'none' : 'inline', 'important');
  })())JS" ).arg( expandOptionalParts ? "true" : "false" ) );
}
void ArticleView::showEvent( QShowEvent * event )
{ QFrame::showEvent( event ); ui.searchFrame->setVisible( searchIsOpened ); ui.ftsSearchFrame->setVisible( ftsSearchIsOpened ); }
bool ArticleView::handleF3( QObject *, QEvent * event )
{
  if( event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride ) return false;
  auto key = static_cast<QKeyEvent *>( event );
  if( key->key() != Qt::Key_F3 || ( !searchIsOpened && !ftsSearchIsOpened ) ||
      ( key->modifiers() != Qt::NoModifier && key->modifiers() != Qt::ShiftModifier ) ) return false;
  if( event->type() == QEvent::KeyPress )
  { if( ftsSearchIsOpened ) performFtsFindOperation( key->modifiers() == Qt::ShiftModifier );
    else performFindOperation( false, key->modifiers() == Qt::ShiftModifier ); }
  event->accept(); return true;
}
bool ArticleView::eventFilter( QObject * object, QEvent * event )
{
  if( event->type() == QEvent::Gesture )
  {
    Gestures::GestureResult result; QPoint point;
    if( Gestures::handleGestureEvent( object, event, result, point ) )
    {
      if( result == Gestures::ZOOM_IN ) emit zoomIn(); else if( result == Gestures::ZOOM_OUT ) emit zoomOut();
      else if( result == Gestures::SWIPE_LEFT ) back(); else if( result == Gestures::SWIPE_RIGHT ) forward();
      else if( result == Gestures::SWIPE_UP || result == Gestures::SWIPE_DOWN )
        run( QStringLiteral( "window.scrollBy(0,%1)" ).arg( result == Gestures::SWIPE_UP ? -120 : 120 ) );
      return true;
    }
  }
  if( handleF3( object, event ) ) return true;
  if( object == ui.definition && event->type() == QEvent::MouseButtonPress )
  {
    auto mouse = static_cast<QMouseEvent *>( event );
    if( mouse->button() == Qt::XButton1 ) { back(); return true; }
    if( mouse->button() == Qt::XButton2 ) { forward(); return true; }
  }
  if( object == ui.definition && event->type() == QEvent::KeyPress )
  {
    auto key = static_cast<QKeyEvent *>( event );
    if( key->modifiers() & ( Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier ) ) return false;
    if( key->key() == Qt::Key_Backspace ) { back(); return true; }
    if( key->key() != Qt::Key_Space && key->key() != Qt::Key_Tab && key->key() != Qt::Key_Backtab &&
        key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter && !key->text().isEmpty() )
    { emit typingEvent( key->text() ); return true; }
  }
  return QFrame::eventFilter( object, event );
}
ResourceToSaveHandler::ResourceToSaveHandler( ArticleView * view, QString const & name ): QObject( view ), fileName( name )
{ connect( this, &ResourceToSaveHandler::statusBarMessage, view, &ArticleView::statusBarMessage ); }
ResourceToSaveHandler::~ResourceToSaveHandler() { cancelRequests( downloadRequests, this ); }
void ResourceToSaveHandler::addRequest( sptr<Dictionary::DataRequest> request )
{
  if( !alreadyDone && request.get() )
  { downloadRequests.push_back( request ); connect( request.get(), &Dictionary::Request::finished, this, &ResourceToSaveHandler::downloadFinished ); }
}
void ResourceToSaveHandler::downloadFinished()
{
  for( auto it = downloadRequests.begin(); it != downloadRequests.end(); )
  {
    auto request = *it;
    if( !request->isFinished() ) { ++it; continue; }
    it = downloadRequests.erase( it );
    if( request->dataSize() < 0 ) continue;
    try
    {
      auto bytes = makeDataRequestReader()->fullData( *request );
      if( !fileName.isEmpty() )
      {
        QDir().mkpath( QFileInfo( fileName ).absolutePath() );
        QSaveFile file( fileName ); file.setDirectWriteFallback( false );
        if( !file.open( QIODevice::WriteOnly ) || file.write( bytes ) != bytes.size() || !file.commit() )
        { emit statusBarMessage( tr( "Resource saving error: %1" ).arg( file.errorString() ), 10000 ); break; }
      }
      alreadyDone = true;
      cancelRequests( downloadRequests, this ); break;
    }
    catch( std::exception const & e ) { emit statusBarMessage( QString::fromUtf8( e.what() ), 10000 ); }
  }
  if( downloadRequests.empty() )
  {
    if( !alreadyDone ) emit statusBarMessage( tr( "The referenced resource failed to download." ), 10000 );
    emit done(); deleteLater();
  }
}
