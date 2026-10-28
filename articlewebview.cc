/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#include "articlewebview.hh"
#include "articlebridge.hh"
#include "articleprofile.hh"
#include <QApplication>
#include <QChildEvent>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QMouseEvent>
#include <QTimer>
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
  bool captured = false;
public:
  explicit LinkCapturePage( ArticleWebPage * owner ):
    QWebEnginePage( owner->profile(), owner ), source( owner )
  {
    settings()->setAttribute( QWebEngineSettings::BackForwardCacheEnabled, false );
    attach( this );
  }
protected:
  bool acceptNavigationRequest( QUrl const & url, NavigationType, bool ) override
  {
    if( !captured )
    {
      captured = true;
      QPointer<ArticleWebPage> guard = source;
      QTimer::singleShot( 0, source, [guard, url] { if( guard ) emit guard->userLink( url, true ); } );
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
    // The navigation callback carries no modifier state of its own; the press that
    // produced this link click is what the application's keyboard state reports.
    Qt::KeyboardModifiers const modifiers = QApplication::keyboardModifiers();
    // Baseline: Alt locks the jump on a link. The engine does not navigate and no
    // action is dispatched.
    if( modifiers.testFlag( Qt::AltModifier ) ) return false;
    bool const newTab = ( view && view->isMidButtonPressed() ) ||
        modifiers.testFlag( Qt::ControlModifier ) || modifiers.testFlag( Qt::ShiftModifier );
    QTimer::singleShot( 0, this, [this, url, newTab] { emit userLink( url, newTab ); } );
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
  if( inspector && config ) config->inspectorGeometry = inspector->saveGeometry();
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
    if( !inspector )
    {
      // Keep the Qt6 DevTools page on this controller's shared profile. The
      // legacy ArticleInspector widget is migrated with the ArticleView consumer.
      inspector = new QWebEngineView( this );
      inspector->setWindowFlag( Qt::Window );
      inspector->setAttribute( Qt::WA_DeleteOnClose );
      auto toolsPage = new QWebEnginePage( ownedPage->profile(), inspector );
      attach( toolsPage );
      inspector->setPage( toolsPage );
      ownedPage->setDevToolsPage( toolsPage );
      if( config ) inspector->restoreGeometry( config->inspectorGeometry );
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

bool ArticleWebView::eventFilter( QObject * watched, QEvent * event )
{
  if( event->type() == QEvent::ChildAdded && watched != this )
  {
    QPointer<QObject> child( static_cast<QChildEvent *>( event )->child() );
    QTimer::singleShot( 0, this, [this, child] { if( child ) watchChildren( child ); } );
  }
  auto widget = qobject_cast<QWidget *>( watched );
  if( !widget || ( widget != this && !isAncestorOf( widget ) ) ) return false;
  if( event->type() == QEvent::MouseButtonPress )
  {
    auto mouse = static_cast<QMouseEvent *>( event );
    if( mouse->button() == Qt::MiddleButton ) midButtonPressed = true;
    if( selectionBySingleClick && !syntheticClick && mouse->button() == Qt::LeftButton )
    {
      QPointer<QWidget> child( widget );
      QPointF const local = mouse->position(), global = mouse->globalPosition();
      auto modifiers = mouse->modifiers();
      QTimer::singleShot( 0, this, [this, child, local, global, modifiers] {
        if( !child ) return;
        syntheticClick = true;
        QMouseEvent click( QEvent::MouseButtonDblClick, local, global,
                           Qt::LeftButton, Qt::LeftButton, modifiers );
        QApplication::sendEvent( child, &click );
        syntheticClick = false;
      } );
    }
  }
  else if( event->type() == QEvent::MouseButtonRelease )
  {
    auto mouse = static_cast<QMouseEvent *>( event );
    // Chromium consumes the event after this filter. Keep the modifier through
    // its navigation callback, then retire the pressed state on the event queue.
    if( mouse->button() == Qt::MiddleButton ) QTimer::singleShot( 0, this, [this] { midButtonPressed = false; } );
  }
  else if( event->type() == QEvent::MouseButtonDblClick )
  {
    QPoint const position = widget->mapTo( this, static_cast<QMouseEvent *>( event )->position().toPoint() );
    QTimer::singleShot( 0, this, [this, position] { emit doubleClicked( position ); } );
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
  if( auto request = lastContextMenuRequest() )
  {
    // The request is ephemeral; retain only plain values copied in this event.
    context = {request->position(), request->selectedText(), request->linkText(),
               request->linkUrl(), request->mediaUrl(), bridge ? bridge->generation() : 0};
  }
  QWebEngineView::contextMenuEvent( event );
}
