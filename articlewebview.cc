/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#include "articlewebview.hh"
#include "articlebridge.hh"
#include "articleinspector.hh"
#include "articleprofile.hh"
#include <QApplication>
#include <QChildEvent>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QWebChannel>
#include <QWebEngineContextMenuRequest>
#include <QWebEngineHistory>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWheelEvent>

namespace {
bool attach( QWebEnginePage * page )
{
  auto controller = qobject_cast<ArticleProfileController *>( page->profile()->parent() );
  if( !controller )
  {
    qWarning( "ArticleWebView: shared profile has no ArticleProfileController" );
    return false;
  }
  controller->attachPage( page );
  return true;
}

// A popup page only observes the first target; it never commits a document.
class LinkCapturePage final : public QWebEnginePage
{
  QPointer<ArticleWebPage> source;
  QPointer<ArticleBridge> sourceBridge;
  int generation = 0;
  bool captured = false;
public:
  explicit LinkCapturePage( ArticleWebPage * owner ):
    QWebEnginePage( owner->profile(), owner ), source( owner )
  {
    settings()->setAttribute( QWebEngineSettings::BackForwardCacheEnabled, false );
    attach( this );
    if( owner->webChannel() )
      sourceBridge = qobject_cast<ArticleBridge *>( owner->webChannel()->registeredObjects().value( "articleview" ) );
    generation = sourceBridge ? sourceBridge->generation() : 0;
  }
protected:
  bool acceptNavigationRequest( QUrl const & url, NavigationType, bool ) override
  {
    if( !captured )
    {
      captured = true;
      QPointer<ArticleWebPage> guard = source; auto bridge = sourceBridge; int const stamp = generation;
      QTimer::singleShot( 0, source, [guard, bridge, stamp, url] {
        if( guard && bridge && bridge->generation() == stamp ) emit guard->userLink( url, true );
      } );
      deleteLater();
    }
    return false;
  }
};
}

ArticleWebPage::ArticleWebPage( QWebEngineProfile * profile, QObject * parent ):
  QWebEnginePage( profile, parent )
{
  settings()->setAttribute( QWebEngineSettings::BackForwardCacheEnabled, false );
}
ArticleWebPage::~ArticleWebPage() = default;

bool ArticleWebPage::acceptNavigationRequest( QUrl const & url, NavigationType type, bool mainFrame )
{
  // One classification decides every branch. An application URL is an action the
  // retained ArticleView handler dispatches; everything else is a navigation
  // Chromium owns.
  bool const action =
    ArticleUrls::parse( url, ArticleUrls::Use::UserAction ).kind != ArticleUrls::Kind::ExternalAction;

  if( !mainFrame )
  {
    // A website frame keeps its real origin: ordinary navigation, forms and
    // redirects stay native. An application URL is not a website navigation, so
    // it never becomes a frame document.
    if( !action ) return true;
    if( type != NavigationTypeLinkClicked ) return false;
  }

  if( type == NavigationTypeRedirect && controlledNavigation ) return true;
  if( type != NavigationTypeLinkClicked && navigationPermit )
  {
    navigationPermit = false;
    controlledNavigation = true;
    return true;
  }
  if( type == NavigationTypeLinkClicked )
  {
    auto view = qobject_cast<ArticleWebView *>( QWebEngineView::forPage( this ) );
    bool const pressed = view && view->bridge && view->inputPage == this &&
                         view->inputGeneration == view->bridge->generation();
    Qt::KeyboardModifiers const modifiers = pressed ? view->pressModifiers : QApplication::keyboardModifiers();
    // Baseline: Alt locks the jump on a link. The engine does not navigate and no
    // action is dispatched.
    if( modifiers.testFlag( Qt::AltModifier ) ) return false;
    bool const newTab = ( pressed && view->pressButton == Qt::MiddleButton ) ||
        modifiers.testFlag( Qt::ControlModifier ) || modifiers.testFlag( Qt::ShiftModifier );
    QPointer<ArticleBridge> bridge = view ? view->bridge : nullptr;
    int const generation = bridge ? bridge->generation() : 0;
    QTimer::singleShot( 0, this, [this, url, newTab, bridge, generation] {
      if( bridge && bridge->generation() == generation ) emit userLink( url, newTab );
    } );
    return false;
  }
  auto kind = type == NavigationTypeReload ? NavigationKind::Reload :
              type == NavigationTypeBackForward ? NavigationKind::Back :
              type == NavigationTypeRedirect ? NavigationKind::Redirect : NavigationKind::NewLookup;
  QTimer::singleShot( 0, this, [this, url, kind] { emit navigationRequested( url, kind ); } );
  return false;
}

QWebEnginePage * ArticleWebPage::createWindow( WebWindowType ) { return new LinkCapturePage( this ); }

ArticleWebView::ArticleWebView( QWidget * parent ): QWebEngineView( parent )
{
  watchChildren( this );
}

ArticleWebView::~ArticleWebView()
{
  delete inspector.data();
  if( bridge ) bridge->retire();
  // WebEngine can run final JS callbacks here. The bridge is still present,
  // Closed, and only deleted once those callbacks can no longer reach the page.
  delete ownedPage.data();
  delete bridge.data();
}

void ArticleWebView::configure( QWebEngineProfile * profile, Config::Class * cfg )
{
  if( ownedPage || !profile ) { qWarning( "ArticleWebView: configure requires one shared profile, once" ); return; }
  if( !qobject_cast<ArticleProfileController *>( profile->parent() ) )
  {
    qWarning( "ArticleWebView: configure requires the owning profile controller" );
    return;
  }
  config = cfg;
  installPage( profile );
}

void ArticleWebView::installPage( QWebEngineProfile * profile )
{
  auto replacement = new ArticleWebPage( profile, this );
  attach( replacement ); // Auth/settings/policy are attached before any load.
  auto replacementBridge = new ArticleBridge( replacement, this );
  replacementBridge->setObjectName( QStringLiteral( "gdArticleBridge" ) );
  connect( replacement, &ArticleWebPage::userLink, this, &ArticleWebView::userLink );
  connect( replacement, &ArticleWebPage::navigationRequested, this, &ArticleWebView::navigationRequested );
  QPointer<ArticleWebPage> previous = ownedPage;
  ownedPage = replacement;
  bridge = replacementBridge;
  selection.clear(); selectionReady = false; inputPage.clear(); inputGeneration = 0;
  connect( replacementBridge, &ArticleBridge::generationChanged, this, [this] {
    selectionReady = false; selection.clear(); inputPage.clear(); inputGeneration = 0;
  } );
  connect( replacementBridge, &ArticleBridge::bridgeReady, this, [this, replacement, replacementBridge]( int generation ) {
    if( ownedPage != replacement || bridge != replacementBridge || bridge->generation() != generation ) return;
    selectionReady = true;
    selection = replacement->selectedText();
  } );
  connect( replacement, &QWebEnginePage::selectionChanged, this, [this, replacement, replacementBridge] {
    if( selectionReady && ownedPage == replacement && bridge == replacementBridge )
      selection = replacement->selectedText();
  } );
  setPage( replacement );
  if( previous ) delete previous.data();
  watchChildren( this );
}

void ArticleWebView::recreatePage( ArticleBridge * exhausted )
{
  if( bridge != exhausted || !ownedPage ) return;
  auto profile = ownedPage->profile();
  delete inspector.data();
  installPage( profile );
  navigated = false;
  exhausted->deleteLater(); // reset() may still be returning on its stack.
}

bool ArticleWebView::prepareNavigation()
{
  if( !ownedPage || !bridge ) { qWarning( "ArticleWebView: configure before loading" ); return false; }
  QPointer<ArticleWebView> guard( this );
  QPointer<ArticleBridge> previous = bridge;
  int const previousGeneration = bridge->generation();
  bool const resetting = navigated;
  if( resetting ) bridge->reset();
  // reset emits application signals: the owner may die or a newer navigation
  // may already have superseded this one before we return to Chromium.
  if( !guard || !bridge || bridge->state == ArticleBridge::State::Closed ) return false;
  if( resetting && bridge == previous &&
      quint64( bridge->generation() ) != quint64( previousGeneration ) + 1 ) return false;
  ownedPage->navigationPermit = true;
  navigated = true;
  return true;
}
void ArticleWebView::load( QUrl const & url ) { navigate( url, NavigationKind::NewLookup ); }
void ArticleWebView::navigate( QUrl const & url, NavigationKind kind )
{
  if( !prepareNavigation() ) return;
  switch( kind )
  {
    case NavigationKind::Back: ownedPage->history()->back(); break;
    case NavigationKind::Forward: ownedPage->history()->forward(); break;
    case NavigationKind::Reload: ownedPage->triggerAction( QWebEnginePage::Reload ); break;
    default: ownedPage->load( url ); break;
  }
}
void ArticleWebView::setHtml( QString const & html, QUrl const & base )
{
  if( prepareNavigation() ) ownedPage->setHtml( html, base );
}
void ArticleWebView::setSelectionBySingleClick( bool enabled ) { selectionBySingleClick = enabled; }
bool ArticleWebView::isMidButtonPressed() const noexcept { return midButtonPressed; }

void ArticleWebView::triggerPageAction( QWebEnginePage::WebAction action, bool checked )
{
  if( !ownedPage ) return;
  if( action == QWebEnginePage::InspectElement )
  {
    // A close may have retired its page while DeferredDelete is still queued.
    if( inspector && !inspector->page() ) delete inspector.data();
    if( !inspector )
    {
      if( !config ) return;
      inspector = new ArticleInspector( ownedPage, config, this );
    }
    inspector->show(); inspector->raise(); inspector->activateWindow();
  }
  QWebEngineView::triggerPageAction( action, checked );
}

void ArticleWebView::watchChildren( QObject * object )
{
  object->installEventFilter( this );
  for( auto child : object->children() )
    if( qobject_cast<QWidget *>( child ) ) watchChildren( child );
}

bool ArticleWebView::event( QEvent * event )
{
  if( event->type() == QEvent::ChildAdded )
  {
    auto child = static_cast<QChildEvent *>( event )->child();
    QPointer<QObject> guard( child );
    QTimer::singleShot( 0, this, [this, guard] { if( guard ) watchChildren( guard ); } );
  }
  return QWebEngineView::event( event );
}

void ArticleWebView::queueLookup( QPoint const & position, Qt::KeyboardModifiers modifiers )
{
  QPointer<ArticleWebPage> page = ownedPage;
  QPointer<ArticleBridge> source = bridge;
  int const generation = source ? source->generation() : 0;
  QTimer::singleShot( 0, this, [this, page, source, generation, position, modifiers] {
    if( page && ownedPage == page && source && bridge == source && source->generation() == generation )
      emit doubleClicked( position, modifiers );
  } );
}

bool ArticleWebView::eventFilter( QObject * watched, QEvent * event )
{
  if( event->type() == QEvent::ChildAdded && watched != this )
  {
    QPointer<QObject> child( static_cast<QChildEvent *>( event )->child() );
    QTimer::singleShot( 0, this, [this, child] { if( child ) watchChildren( child ); } );
  }
  auto widget = qobject_cast<QWidget *>( watched );
  if( !widget || ( widget != this && !isAncestorOf( widget ) ) ) return false;
  if( event->type() == QEvent::MouseButtonPress && !syntheticClick )
  {
    auto mouse = static_cast<QMouseEvent *>( event );
    inputPage = ownedPage; inputGeneration = bridge ? bridge->generation() : 0;
    pressPosition = widget->mapTo( this, mouse->position().toPoint() );
    pressModifiers = mouse->modifiers(); pressButton = mouse->button();
    dragging = doubleClick = false;
    if( mouse->button() == Qt::MiddleButton ) midButtonPressed = true;
  }
  else if( event->type() == QEvent::MouseMove && !syntheticClick )
  {
    auto mouse = static_cast<QMouseEvent *>( event );
    if( mouse->buttons().testFlag( Qt::LeftButton ) &&
        ( widget->mapTo( this, mouse->position().toPoint() ) - pressPosition ).manhattanLength() >= QApplication::startDragDistance() )
      dragging = true;
  }
  else if( event->type() == QEvent::MouseButtonRelease && !syntheticClick )
  {
    auto mouse = static_cast<QMouseEvent *>( event );
    if( mouse->button() == Qt::MiddleButton ) QTimer::singleShot( 0, this, [this] { midButtonPressed = false; } );
    bool const current = inputPage && inputPage == ownedPage && bridge && inputGeneration == bridge->generation();
    if( current && mouse->button() == Qt::LeftButton && !dragging && !doubleClick &&
        ( widget->mapTo( this, mouse->position().toPoint() ) - pressPosition ).manhattanLength() < QApplication::startDragDistance() )
    {
      if( pressModifiers.testFlag( Qt::AltModifier ) ) queueLookup( pressPosition, pressModifiers );
      else if( selectionBySingleClick )
      {
        QPointer<QWidget> child( widget ); auto page = inputPage; auto source = bridge; int generation = inputGeneration;
        QPointF const local = mouse->position(), global = mouse->globalPosition(); auto modifiers = pressModifiers;
        auto const timestamp = mouse->timestamp();
        QTimer::singleShot( 0, this, [this, child, page, source, generation, local, global, modifiers, timestamp] {
          if( !child || !page || ownedPage != page || !source || bridge != source || source->generation() != generation ) return;
          syntheticClick = true;
          QMouseEvent click( QEvent::MouseButtonDblClick, local, global, Qt::LeftButton, Qt::LeftButton, modifiers );
          click.setTimestamp( timestamp + 1 );
          QApplication::sendEvent( child, &click );
          QMouseEvent release( QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, modifiers );
          release.setTimestamp( timestamp + 2 );
          QApplication::sendEvent( child, &release );
          syntheticClick = false;
        } );
      }
    }
  }
  else if( event->type() == QEvent::MouseButtonDblClick )
  {
    auto mouse = static_cast<QMouseEvent *>( event );
    if( mouse->button() == Qt::LeftButton )
    {
      QPoint const position = widget->mapTo( this, mouse->position().toPoint() );
      doubleClick = true;
      queueLookup( position, mouse->modifiers() );
    }
  }
  else if( event->type() == QEvent::Wheel && static_cast<QWheelEvent *>( event )->modifiers().testFlag( Qt::ControlModifier ) )
  {
    event->ignore();
    return true;
  }
  return false;
}

void ArticleWebView::focusInEvent( QFocusEvent * event )
{
  QWebEngineView::focusInEvent( event );
  if( bridge && ( event->reason() == Qt::MouseFocusReason || event->reason() == Qt::TabFocusReason ||
                 event->reason() == Qt::BacktabFocusReason ) ) bridge->run( QStringLiteral( "window.focus()" ) );
}
void ArticleWebView::wheelEvent( QWheelEvent * event )
{
  if( event->modifiers().testFlag( Qt::ControlModifier ) ) event->ignore();
  else QWebEngineView::wheelEvent( event );
}
void ArticleWebView::contextMenuEvent( QContextMenuEvent * event )
{
  // ArticleView uses CustomContextMenu and copies lastContextMenuRequest()
  // synchronously in that signal. A bare view retains WebEngine's default menu.
  QWebEngineView::contextMenuEvent( event );
}
