#include "popupdismiss.hh"

#include <QApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QWidget>
#include <QWindow>
#include <utility>

Q_LOGGING_CATEGORY( popupDismissLc, "goldendict.popup.dismiss" )

namespace PopupDismiss {

Action onFocusMoved( bool visible, bool pinned, bool armed,
                     FocusDestination destination, bool applicationActive )
{
  return visible && !pinned && armed &&
         ( destination == FocusDestination::Foreign ||
           ( destination == FocusDestination::None && !applicationActive ) )
           ? Action::Hide : Action::Ignore;
}

bool needsExplicitActivation( Qt::WindowFlags flags )
{
  return ( flags & Qt::WindowType_Mask ) != Qt::Popup;
}

bool armedAfterShow( QWindow const * ownWindow, QWindow const * focusWindow )
{
  return ownWindow != nullptr && ownWindow == focusWindow;
}

bool isOwnedPopup( QWidget const * owner, QWidget const * candidate )
{
  if( !owner || !candidate || owner == candidate ) return false;
  for( QWidget const * parent = candidate->parentWidget(); parent;
       parent = parent->parentWidget() )
    if( parent == owner ) return true;
  return false;
}

bool isPopupWindowType( Qt::WindowType type ) { return type == Qt::Popup; }

FocusLossCloser::FocusLossCloser( QWidget * observedWindow, QObject * parent )
  : QObject( parent ), window( observedWindow )
{
  if( this->window ) this->window->installEventFilter( this );
  focusConnection = QObject::connect(
    qApp, &QGuiApplication::focusWindowChanged, this,
    [this]( QWindow * focusWindow ) {
      if( !window ) return;
      FocusDestination const destination = destinationOf( focusWindow );
      if( destination == FocusDestination::OwnWindow )
      {
        armed = true;
        qCDebug( popupDismissLc ) << "onFocusMoved OwnWindow Ignore" << "own focus";
        return;
      }
      if( destination == FocusDestination::OwnPopup )
      {
        qCDebug( popupDismissLc ) << "onFocusMoved OwnPopup Ignore" << "own popup";
        return;
      }
      bool const applicationActive = qApp->applicationState() == Qt::ApplicationActive;
      bool const visible = window->isVisible();
      if( onFocusMoved( visible, pinned, armed, destination, applicationActive ) == Action::Ignore )
      {
        char const * const why = !visible ? "hidden" : pinned ? "pinned" :
                                !armed ? "unarmed" : "None with active application";
        qCDebug( popupDismissLc ) << "onFocusMoved"
          << ( destination == FocusDestination::Foreign ? "Foreign" : "None" ) << "Ignore" << why;
        return;
      }
      dismiss( destination == FocusDestination::Foreign ? "focus moved to another window"
                                                       : "focus left the application" );
      return;
    } );
  stateConnection = QObject::connect(
    qApp, &QGuiApplication::applicationStateChanged, this,
    [this]( Qt::ApplicationState state ) {
      if( state != Qt::ApplicationInactive ) return;
      if( !window ) return;
      if( onFocusMoved( window->isVisible(), pinned, armed, FocusDestination::None, false ) == Action::Ignore )
      {
        char const * const why = !window->isVisible() ? "hidden" : pinned ? "pinned" : "unarmed";
        qCDebug( popupDismissLc ) << "applicationStateChanged None Ignore" << why;
        return;
      }
      dismiss( "application inactive" );
      return;
    } );
}
FocusLossCloser::~FocusLossCloser()
{
  QObject::disconnect( focusConnection );
  QObject::disconnect( stateConnection );
  if( window ) window->removeEventFilter( this );
}
void FocusLossCloser::setPinned( bool pinned ) { this->pinned = pinned; }
bool FocusLossCloser::isPinned() const { return pinned; }
void FocusLossCloser::setDismissHandler( std::function< void() > handler )
{
  dismissHandler = std::move( handler );
}
bool FocusLossCloser::eventFilter( QObject * watched, QEvent * event )
{
  if( window && watched == window.data() )
  {
    if( event->type() == QEvent::Show )
      armed = PopupDismiss::armedAfterShow( window->windowHandle(), QGuiApplication::focusWindow() );
    else if( event->type() == QEvent::Hide )
      armed = false;
  }
  return QObject::eventFilter( watched, event );
}
FocusDestination FocusLossCloser::destinationOf( QWindow const * focusWindow ) const
{
  if( !window ) return FocusDestination::Foreign;
  if( !focusWindow ) return FocusDestination::None;
  if( focusWindow == window->windowHandle() ) return FocusDestination::OwnWindow;
  QWidget * const activePopup = QApplication::activePopupWidget();
  if( isOwnedPopup( window, activePopup ) )
  {
    QWindow * const popupHandle = activePopup->windowHandle();
    if( popupHandle && focusWindow == popupHandle && isPopupWindowType( focusWindow->type() ) )
      return FocusDestination::OwnPopup;
  }
  return FocusDestination::Foreign;
}
void FocusLossCloser::dismiss( char const * reason )
{
  if( dismissing ) return;
  dismissing = true;
  QPointer< FocusLossCloser > self( this );
  bool const fromState = qstrcmp( reason, "application inactive" ) == 0;
  bool const fromForeign = qstrcmp( reason, "focus moved to another window" ) == 0;
  qCDebug( popupDismissLc ) << ( fromState ? "applicationStateChanged" : "onFocusMoved" )
    << ( fromForeign ? "Foreign" : "None" ) << "Hide" << reason;
  auto handler = dismissHandler;
  if( handler ) handler();
  if( !self ) return;
  dismissing = false;
  armed = false;
}

} // namespace PopupDismiss
