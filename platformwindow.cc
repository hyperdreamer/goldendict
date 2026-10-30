/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Implementation of the platform-safe window helpers, see platformwindow.hh and
 * specification §4.13 / design §6.6.3.
 */

#include "platformwindow.hh"

#include <QDebug>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPoint>
#include <QScreen>
#include <QWidget>
#include <QWindow>

#ifdef HAVE_KWINDOWSYSTEM
#include <KWindowSystem>
#endif

#if defined( HAVE_X11 ) && QT_CONFIG( xcb )
#include <QtGui/qguiapplication_platform.h>
#endif

Q_LOGGING_CATEGORY( platformWindowLc, "goldendict.platform.window" )

namespace {

/// The unpinned form that needs no window-manager special-casing. This is what
/// a Wayland surface gets and what the X11 "tool" preference means today.
Qt::WindowFlags const managedToolFlags =
  Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;

/// What the stored "default" preference means on X11: the platform default the
/// replaced popup code used (scanpopup.cc's defaultUnpinnedWindowFlags on
/// Linux/X11), with the bypass option deliberately not applied.
Qt::WindowFlags const defaultX11Flags = Qt::Popup;

bool isWaylandPlatform( QString const & platformName )
{
  return platformName.toLower().contains( QLatin1String( "wayland" ) );
}

bool isXcbPlatform( QString const & platformName )
{
  return platformName.toLower().startsWith( QLatin1String( "xcb" ) );
}

} // namespace

namespace PlatformWindow {

namespace detail {

QScreen * screenForPointIn( QList< QScreen * > const & screens,
                            QScreen * screenAtPoint,
                            QScreen * primaryScreen )
{
  // A session that reports no screen at all has no screen to borrow, whatever
  // the platform's point lookup still answers: a stale pointer there would be a
  // dangling screen rather than a usable one.
  if ( screens.isEmpty() )
    return nullptr;

  if ( screenAtPoint )
    return screenAtPoint;

  return primaryScreen;
}

QRect availableGeometryIn( QScreen * screen )
{
  return screen ? screen->availableGeometry() : QRect();
}

ActivationOutcome activateIn( QWidget * window, QString const & token, ActivationOps const & ops )
{
  if( !window )
    return ActivationOutcome::NoWindow;

  // The activation token is installed only once there is a handle to activate:
  // setting it earlier would leave a process-wide token behind on the abandoned
  // path, where another component's later requestActivate could consume it.
  QWindow * const handle = window->windowHandle();
  if( !handle )
    return ActivationOutcome::NoHandle;

  if( !token.isEmpty() && ops.setToken )
    ops.setToken( token );

  if( ops.requestActivate )
    ops.requestActivate( handle );

  return ActivationOutcome::Requested;
}

} // namespace detail

QScreen * screenForPoint( QPoint const & point )
{
  return detail::screenForPointIn( QGuiApplication::screens(),
                                   QGuiApplication::screenAt( point ),
                                   QGuiApplication::primaryScreen() );
}

QRect availableGeometryForPoint( QPoint const & point )
{
  return detail::availableGeometryIn( screenForPoint( point ) );
}

Qt::WindowFlags popupFlags( QString const & platformName,
                            Config::ScanPopupWindowFlags configured,
                            bool bypass )
{
  // Wayland first, and unconditionally: the stored X11 preferences do not
  // describe anything a Wayland compositor can honour.
  if ( isWaylandPlatform( platformName ) )
    return managedToolFlags;

  Qt::WindowFlags flags;
  switch ( configured )
  {
    case Config::SPWF_Popup:
      flags = Qt::Popup;
      break;
    case Config::SPWF_Tool:
      flags = managedToolFlags;
      break;
    case Config::SPWF_default:
    default:
      // Preserved behaviour: the replaced code returned the platform default
      // here before the bypass hint was considered, and the preference UI
      // disables the bypass option for exactly this choice.
      return defaultX11Flags;
  }

  // The bypass hint is an X11 instruction. It is attached only on a real xcb
  // session, so a non-X11 platform can never inherit it through this helper.
  if ( bypass && isXcbPlatform( platformName ) )
    flags |= Qt::X11BypassWindowManagerHint;

  return flags;
}

namespace detail {
ActivationOps activationOps()
{
  ActivationOps ops;
  // Requested in every build: the KDE helper only adds the activation token, so
  // a session without it still asks the platform to raise the window.
  ops.requestActivate = []( QWindow * handle ) { handle->requestActivate(); };
#ifdef HAVE_KWINDOWSYSTEM
  ops.setToken = []( QString const & value ) { KWindowSystem::setCurrentXdgActivationToken( value ); };
#endif
  return ops;
}
} // namespace detail

void applyActivation( QWidget * window, QString const & token )
{
  detail::ActivationOutcome const outcome = detail::activateIn( window, token, detail::activationOps() );

  if( outcome == detail::ActivationOutcome::NoWindow )
    return;

  if( outcome == detail::ActivationOutcome::NoHandle )
  {
    // Nothing to ask for yet: showing the window is the caller's normal path,
    // not this helper's job.
    qCDebug( platformWindowLc ) << "activation skipped: the window has no native handle";
    return;
  }

  // A logged request, never a claimed grant: the compositor may ignore it, and
  // this helper cannot observe the outcome. Only the presence of a token is
  // recorded; the token itself is a bearer capability and is never logged.
  qCDebug( platformWindowLc ) << "activation requested on platform"
                              << QGuiApplication::platformName() << "with activation token"
                              << ( token.isEmpty() ? "absent" : "present" );
}

#ifdef HAVE_X11
Display * x11Display()
{
  // HAVE_X11 only says the build can talk to Xlib. The active platform decides
  // whether there is a display to borrow, and Wayland must never be asked.
  if ( !isXcbPlatform( QGuiApplication::platformName() ) )
    return nullptr;

#if QT_CONFIG( xcb )
  if ( auto * const x11 = qGuiApp->nativeInterface< QNativeInterface::QX11Application >() )
    return x11->display();
#endif

  return nullptr;
}
#endif

} // namespace PlatformWindow
