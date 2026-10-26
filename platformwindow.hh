/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Platform-dependent window decisions shared by the scan popup, the scan flag
 * and the main window (specification §4.13, design §6.6.3).
 *
 * HAVE_X11 records that this build was compiled with the X11 backend; it is
 * never evidence that the running session is X11. Every runtime decision below
 * is derived from the active QPA platform name (or from the name it is given),
 * so a Wayland session never executes an X11-specific path and an explicitly
 * chosen X11 session keeps the existing X11 behaviour. In particular the X11
 * bypass window-manager hint is never handed to a Wayland surface: it can leave
 * the surface unmanaged or unmapped there.
 *
 * No native resource is owned here. x11Display() borrows the display Qt already
 * opened; nothing is opened, closed or reconfigured.
 */

#ifndef GOLDENDICT_PLATFORMWINDOW_HH_INCLUDED
#define GOLDENDICT_PLATFORMWINDOW_HH_INCLUDED

#include <QList>
#include <QRect>
#include <QString>
#include <Qt>

#include "config.hh"

class QPoint;
class QScreen;
class QWidget;

#ifdef HAVE_X11
// Xlib's display type, forward declared so a consumer that only borrows the
// running session's display does not have to include Xlib. The declaration is
// identical to Xlib's own and to the one Qt's native-interface header repeats,
// so either may be included before or after this one.
struct _XDisplay;
typedef struct _XDisplay Display;
#endif

class QWindow;

#include <functional>

namespace PlatformWindow {

/// The screen `point` belongs to. Null only when no screen is attached at all:
/// Qt's answer for the point wins, otherwise the primary screen is used, so a
/// popup is always clamped to a real screen instead of a virtual desktop.
QScreen * screenForPoint( QPoint const & point );

/// The area a popup may use for `point`: the *available* geometry of the screen
/// screenForPoint() selects (shrunk by desktop reservations such as panels),
/// never that screen's full geometry. Empty when there is no screen to place a
/// window on. A consumer that needs the full geometry takes it from
/// screenForPoint()->geometry().
QRect availableGeometryForPoint( QPoint const & point );

/// Window flags for an unpinned popup on the named QPA platform.
///
/// `platformName` is the platform to decide for (QGuiApplication::platformName()
/// at the call site, or any name a test wants to ask about). The decision is a
/// pure function of its arguments, so both branches are provable on one build.
///
/// Wayland: always Tool|FramelessWindowHint|WindowStaysOnTopHint and never
/// X11BypassWindowManagerHint, whatever the stored preference says — the X11
/// shapes are not merely useless there, they can leave the surface unmanaged.
///
/// Everywhere else the stored preference mapping is preserved
/// (scanpopup.cc:448-468): SPWF_Popup keeps Qt::Popup, SPWF_Tool keeps
/// Tool|FramelessWindowHint|WindowStaysOnTopHint, and SPWF_default keeps the
/// platform default while ignoring the bypass option, exactly like the
/// preference UI, which disables that option for the default choice
/// (preferences.cc:600). X11BypassWindowManagerHint is attached only when
/// `bypass` is set *and* the named platform is an actual xcb session, because
/// the hint is an X11-only instruction.
Qt::WindowFlags popupFlags( QString const & platformName,
                            Config::ScanPopupWindowFlags configured,
                            bool bypass );

/// Best-effort activation request for `window`, forwarding `token` (an XDG
/// activation token) when this build has the KDE windowing-system helper and a
/// nonempty token was supplied. `window` may be null; a window without a native
/// handle is left alone, because showing it is the caller's normal path.
///
/// The return type is void on purpose: only the compositor decides whether the
/// request is honoured, so this helper never reports (or logs) a granted focus.
/// The token value is never logged.
void applyActivation( QWidget * window, QString const & token );

#ifdef HAVE_X11
/// The Xlib display of the running session, borrowed from Qt. Null unless the
/// active QPA is xcb and Qt exposes its X11 native interface; a non-X11 session
/// therefore never hands out a display and no Xlib call can be reached there.
Display * x11Display();
#endif

/// Implementation detail, exposed so tst_platformwindow can prove the rules the
/// public helpers apply. The suite runs on a live QPA (xcb or offscreen), where
/// a screen always exists; a session really can lose its last screen at
/// teardown, and that branch has to be provable rather than assumed.
namespace detail {

/// Pure screen decision of screenForPoint(): `screens` is the screen list,
/// `screenAtPoint` the platform's answer for the queried point and
/// `primaryScreen` the platform's primary screen. An empty screen list means no
/// screen, whatever the point lookup reported; otherwise the point's screen
/// wins, and the primary screen is the fallback.
QScreen * screenForPointIn( QList< QScreen * > const & screens,
                            QScreen * screenAtPoint,
                            QScreen * primaryScreen );

/// Pure geometry decision of availableGeometryForPoint(): a screen's available
/// area (never its full area), or an empty rectangle when there is no screen.
QRect availableGeometryIn( QScreen * screen );

/// What applyActivation() did, decided without touching a real window manager.
enum class ActivationOutcome
{
  NoWindow,  ///< A null widget: nothing to activate, nothing recorded.
  NoHandle,  ///< The widget has no native handle yet; the token is not installed.
  Requested  ///< The token was installed (unless empty) and activation requested.
};

/// The two platform effects, injected so the order and the conditions can be
/// observed. A null callback means "this build has no such backend step".
struct ActivationOps
{
  /// Invoked with the token, once, when a handled window is being activated.
  std::function< void( QString const & ) > setToken;
  /// Invoked with the handle whose activation is being requested.
  std::function< void( QWindow * ) > requestActivate;
};

/// Pure activation rule: no window manager is involved.
ActivationOutcome activateIn( QWidget * window, QString const & token, ActivationOps const & ops );

/// The production effects for this build. The activation *request* is wired in
/// every build (spec §4.13: without the KDE helper requestActivate is still
/// called); only the token hand-off depends on KF6WindowSystem. Exposed so the
/// wiring can be pinned instead of assumed.
ActivationOps activationOps();

} // namespace detail

} // namespace PlatformWindow

#endif // GOLDENDICT_PLATFORMWINDOW_HH_INCLUDED
