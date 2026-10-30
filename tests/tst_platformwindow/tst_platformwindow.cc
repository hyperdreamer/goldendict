/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Platform-safe window geometry, flags and activation (specification §4.13,
 * design §6.6.3).
 *
 * The suite links the production platformwindow.cc, never a copy. Three
 * properties make every claim below provable on the xcb gate run:
 *
 *   * popupFlags() is a pure function of the QPA name it is given, so the
 *     Wayland policy is asserted on an xcb build instead of faking
 *     QGuiApplication::platformName() or needing a compositor;
 *   * the screen/geometry decisions that cannot be reached on a live QPA (a
 *     session that reports no screen at all) are driven through the module's
 *     pure detail rules;
 *   * the xcb gate can install a panel-like desktop reservation on its bare X
 *     server, which is the only state in which a screen's available geometry
 *     differs from its full geometry — without it "available, not full" would
 *     be unobservable.
 *
 * Activation is asserted as what §4.13 specifies: a best-effort *request*. No
 * test asserts that focus was granted; the request is observed through the
 * module's own log line, the token is asserted to never appear in it, and the
 * missing native handle is asserted to be left alone rather than invented.
 */

#include <QtTest>

#include <QApplication>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QStringList>
#include <QWidget>
#include <QWindow>

#include "support/fixtures.hh"

#include "config.hh"
#include "hotkeywrapper.hh"
#include "platformwindow.hh"

#ifdef HAVE_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

#if defined( HAVE_X11 ) && QT_CONFIG( xcb )
#include <QtGui/qguiapplication_platform.h>
#endif

namespace {

/// Message sink of the activation log assertions. Only messages emitted while
/// the handler below is installed are collected, and the collecting list is read
/// after the previous handler is restored.
QStringList * messageCaptureTarget = nullptr;

void captureMessage( QtMsgType type, QMessageLogContext const & context, QString const & message )
{
    Q_UNUSED( type )
    Q_UNUSED( context )

    if ( messageCaptureTarget )
        *messageCaptureTarget << message;
}

/// Installs the capture handler for one scope and restores the previous one.
class MessageCapture
{
public:
    explicit MessageCapture( QStringList & messages )
      : previous( qInstallMessageHandler( captureMessage ) )
    {
        messageCaptureTarget = &messages;
    }

    ~MessageCapture()
    {
        messageCaptureTarget = nullptr;
        qInstallMessageHandler( previous );
    }

    MessageCapture( MessageCapture const & ) = delete;
    MessageCapture & operator=( MessageCapture const & ) = delete;

private:
    QtMessageHandler previous;
};

#ifdef HAVE_X11
/// Reads an X11 window property as a boolean presence test.
bool rootWindowHasProperty( Display * display, char const * name )
{
    Atom const atom = XInternAtom( display, name, False );

    if ( atom == None )
        return false;

    Atom type = None;
    int format = 0;
    unsigned long items = 0;
    unsigned long remaining = 0;
    unsigned char * data = nullptr;

    bool present = false;

    if ( XGetWindowProperty( display, DefaultRootWindow( display ), atom, 0, 1, False, XA_WINDOW,
                             &type, &format, &items, &remaining, &data )
         == Success )
    {
        present = type != None && items > 0;

        if ( data )
            XFree( data );
    }

    return present;
}

/// True when a window manager owns the session. A real desktop owns its work
/// area; the bare Xvfb server of the test gate does not, so this is what decides
/// whether the suite may install a reservation.
bool sessionHasWindowManager( Display * display )
{
    return rootWindowHasProperty( display, "_NET_SUPPORTING_WM_CHECK" );
}

/// Publishes the desktop reservation a panel publishes: the usable area becomes
/// smaller than the screen's full geometry.
bool writeWorkArea( Display * display, QRect const & area )
{
    Atom const atom = XInternAtom( display, "_NET_WORKAREA", False );

    if ( atom == None )
        return false;

    long const values[4] = { area.x(), area.y(), area.width(), area.height() };

    XChangeProperty( display, DefaultRootWindow( display ), atom, XA_CARDINAL, 32, PropModeReplace,
                     reinterpret_cast< unsigned char const * >( values ), 4 );
    XFlush( display );

    return true;
}

/// Restores the work area when the fixture leaves its scope, including the path
/// where an assertion fails and QVERIFY2 returns early: a failed reservation
/// must never leak into the tests that follow. The restore is also awaited, so a
/// later test never reads a half-restored screen.
class WorkAreaRestoration
{
public:
    WorkAreaRestoration( Display * display, QScreen * screen, QRect const & original )
      : display( display )
      , screen( screen )
      , original( original )
    {
    }

    ~WorkAreaRestoration()
    {
        writeWorkArea( display, original );

        for ( int attempt = 0; attempt < 200 && screen->availableGeometry() != original; ++attempt )
            QTest::qWait( 10 );
    }

    WorkAreaRestoration( WorkAreaRestoration const & ) = delete;
    WorkAreaRestoration & operator=( WorkAreaRestoration const & ) = delete;

private:
    Display * display;
    QScreen * screen;
    QRect original;
};
#endif

/// The geometry of every attached screen united, i.e. the rectangle a point must
/// leave to be outside all of them.
QRect unitedScreenGeometry()
{
    QRect united;

    for ( QScreen * const screen : QGuiApplication::screens() )
        united = united.isNull() ? screen->geometry() : united.united( screen->geometry() );

    return united;
}

} // namespace

/*
 * What this suite can and cannot observe
 * --------------------------------------
 * The popup/geometry/flag decisions and the activation *rule* are pure and are
 * pinned exactly, including the empty-screen branch and the token/request order.
 * The production wiring of applyActivation() is pinned at the callback level
 * (detail::activationOps() must carry a request effect in every build, and the
 * token effect only where KF6WindowSystem is compiled in), so deleting either
 * one is a red build-variant test rather than an invisible behaviour change.
 *
 * Two things remain observable only through the source and the log line: the
 * exact token VALUE handed to KWindowSystem (no public read-back exists, and the
 * environment is not modified by that call) and whether the compositor honoured
 * the request (a helper cannot know, and a fixture asserting focus was granted
 * would be a lie on Wayland).
 */

class TstPlatformWindow: public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void publishedSignaturesAreIntact();

    void popupFlagsOnWaylandAreAlwaysManagedToolWindows_data();
    void popupFlagsOnWaylandAreAlwaysManagedToolWindows();
    void popupFlagsOnXcbRetainStoredCombinations_data();
    void popupFlagsOnXcbRetainStoredCombinations();
    void popupFlagsAttachTheX11BypassHintOnlyOnXcb_data();
    void popupFlagsAttachTheX11BypassHintOnlyOnXcb();

    void screenForPointUsesThePointScreenAndFallsBackToPrimary();
    void screenForPointRuleHandlesAnEmptyScreenList();

    void availableGeometryForPointUsesTheAvailableArea();
    void availableGeometryRuleIsEmptyWithoutAScreen();

    void applyActivationIsNullSafeAndNeverInventsAHandle();
    void applyActivationRequestsWithoutClaimingOrLoggingTheToken();
    void applyActivationNeverClobbersAnExistingActivationToken();
    void activationRuleInstallsTheTokenOnlyForAHandledWindow();
    void activationRuleRequestsActivationAfterTheToken();
    void activationOpsWireTheRequestInEveryBuild();
    void activationRequestReachesThePlatform();

    void x11DisplayIsBorrowedOnlyOnXcb();
    void recorderSessionFollowsTheActivePlatformName();
};

void TstPlatformWindow::initTestCase()
{
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );

    // This suite drives real QScreens and a real QWidget: without a display the
    // required assertions cannot run, so the environment fails with a reason
    // instead of the suite silently passing.
    QString displayReason;
    QVERIFY2( TestSupport::prerequisiteAvailable( QStringLiteral( "display" ), &displayReason ),
              qPrintable( displayReason ) );

    // The activation fixture observes the module's categorized request log. The
    // category is enabled by default; enabling it explicitly keeps the fixture
    // independent of the ambient QT_LOGGING_RULES.
    QLoggingCategory::setFilterRules( QStringLiteral( "goldendict.*.debug=true" ) );

    // The portable variant must genuinely be built without the X11 capability;
    // otherwise it would prove nothing about the capability-free code path.
#ifdef GD_TEST_PLATFORMWINDOW_PORTABLE
    QVERIFY2( !TestSupport::compiledFeatureDefines().contains( QStringLiteral( "HAVE_X11" ) ),
              "the portable variant still defines HAVE_X11" );
#endif

    TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
}

void TstPlatformWindow::cleanupTestCase()
{
    TestSupport::clearWatchdog();
}

void TstPlatformWindow::publishedSignaturesAreIntact()
{
    // A compile-time pin of the published helper contract: taking each address
    // with its complete type fails to build if a return type or a parameter list
    // changed. applyActivation's void return is the "no boolean focus report"
    // rule stated as a type.
    QScreen *( *screenForPoint )( QPoint const & ) = &PlatformWindow::screenForPoint;
    QRect ( *availableGeometryForPoint )( QPoint const & ) = &PlatformWindow::availableGeometryForPoint;
    Qt::WindowFlags ( *popupFlags )( QString const &, Config::ScanPopupWindowFlags, bool ) =
      &PlatformWindow::popupFlags;
    void ( *applyActivation )( QWidget *, QString const & ) = &PlatformWindow::applyActivation;

    QVERIFY( screenForPoint );
    QVERIFY( availableGeometryForPoint );
    QVERIFY( popupFlags );
    QVERIFY( applyActivation );

#ifdef HAVE_X11
    Display *( *x11Display )() = &PlatformWindow::x11Display;
    QVERIFY( x11Display );
#endif
}

void TstPlatformWindow::popupFlagsOnWaylandAreAlwaysManagedToolWindows_data()
{
    QTest::addColumn< QString >( "platform" );
    QTest::addColumn< int >( "configured" );
    QTest::addColumn< bool >( "bypass" );

    // The QPA name is compared case-insensitively; the capitalised entry is the
    // row that distinguishes a folded comparison from a raw contains().
    const QVector< QString > platforms = { QStringLiteral( "wayland" ),
                                           QStringLiteral( "Wayland" ),
                                           QStringLiteral( "WAYLAND" ),
                                           QStringLiteral( "wayland-egl" ),
                                           QStringLiteral( "wayland/xcb" ),
                                           QStringLiteral( "wayland-egl-streams" ) };
    const Config::ScanPopupWindowFlags preferences[] = { Config::SPWF_default,
                                                         Config::SPWF_Popup,
                                                         Config::SPWF_Tool };

    for ( QString const & platform : platforms )
    {
        for ( Config::ScanPopupWindowFlags const preference : preferences )
        {
            for ( bool const bypass : { false, true } )
            {
                QTest::newRow( qPrintable( QStringLiteral( "%1 preference=%2 bypass=%3" )
                                             .arg( platform )
                                             .arg( int( preference ) )
                                             .arg( int( bypass ) ) ) )
                  << platform << int( preference ) << bypass;
            }
        }
    }
}

void TstPlatformWindow::popupFlagsOnWaylandAreAlwaysManagedToolWindows()
{
    QFETCH( QString, platform );
    QFETCH( int, configured );
    QFETCH( bool, bypass );

    Qt::WindowFlags const flags = PlatformWindow::popupFlags(
      platform, static_cast< Config::ScanPopupWindowFlags >( configured ), bypass );

    // The literal assertions the plan requires, and they hold for every row.
    QVERIFY( flags.testFlag( Qt::Tool ) );
    QVERIFY( flags.testFlag( Qt::FramelessWindowHint ) );
    QVERIFY( flags.testFlag( Qt::WindowStaysOnTopHint ) );
    QVERIFY( !flags.testFlag( Qt::X11BypassWindowManagerHint ) );

    // An exact set: Qt::Tool already carries the Qt::Popup bits, so the exact
    // comparison is what pins "no extra window type was added" on top of the
    // three required flags.
    Qt::WindowFlags const expected = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
    QCOMPARE( flags.toInt(), expected.toInt() );
}

void TstPlatformWindow::popupFlagsOnXcbRetainStoredCombinations_data()
{
    QTest::addColumn< QString >( "platform" );
    QTest::addColumn< int >( "configured" );
    QTest::addColumn< bool >( "bypass" );
    QTest::addColumn< int >( "expected" );

    Qt::WindowFlags const popup = Qt::Popup;
    Qt::WindowFlags const popupBypass = Qt::Popup | Qt::X11BypassWindowManagerHint;
    Qt::WindowFlags const tool = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
    Qt::WindowFlags const toolBypass = tool | Qt::X11BypassWindowManagerHint;

    QTest::newRow( "xcb default, no bypass" )
      << QStringLiteral( "xcb" ) << int( Config::SPWF_default ) << false << int( popup.toInt() );
    // The default choice keeps the platform default and ignores the bypass
    // option, exactly like the replaced popup code and the preference UI, which
    // disables that option for this choice.
    QTest::newRow( "xcb default, bypass ignored" )
      << QStringLiteral( "xcb" ) << int( Config::SPWF_default ) << true << int( popup.toInt() );
    QTest::newRow( "xcb popup, no bypass" )
      << QStringLiteral( "xcb" ) << int( Config::SPWF_Popup ) << false << int( popup.toInt() );
    QTest::newRow( "xcb popup, bypass" )
      << QStringLiteral( "xcb" ) << int( Config::SPWF_Popup ) << true << int( popupBypass.toInt() );
    QTest::newRow( "xcb tool, no bypass" )
      << QStringLiteral( "xcb" ) << int( Config::SPWF_Tool ) << false << int( tool.toInt() );
    QTest::newRow( "xcb tool, bypass" )
      << QStringLiteral( "xcb" ) << int( Config::SPWF_Tool ) << true << int( toolBypass.toInt() );
}

void TstPlatformWindow::popupFlagsOnXcbRetainStoredCombinations()
{
    QFETCH( QString, platform );
    QFETCH( int, configured );
    QFETCH( bool, bypass );
    QFETCH( int, expected );

    Qt::WindowFlags const flags = PlatformWindow::popupFlags(
      platform, static_cast< Config::ScanPopupWindowFlags >( configured ), bypass );

    QVERIFY2( int( flags.toInt() ) == expected,
              qPrintable( QStringLiteral( "popupFlags(%1, %2, %3) = 0x%4, expected 0x%5" )
                            .arg( platform )
                            .arg( configured )
                            .arg( int( bypass ) )
                            .arg( int( flags.toInt() ), 0, 16 )
                            .arg( expected, 0, 16 ) ) );

    // The stored bypass option reaches the X11 flags only for the preferences
    // the option applies to.
    const bool bypassExpected = bypass && configured != int( Config::SPWF_default );
    QCOMPARE( flags.testFlag( Qt::X11BypassWindowManagerHint ), bypassExpected );
}

void TstPlatformWindow::popupFlagsAttachTheX11BypassHintOnlyOnXcb_data()
{
    QTest::addColumn< QString >( "platform" );
    QTest::addColumn< int >( "configured" );

    const QVector< QString > platforms = { QStringLiteral( "offscreen" ),
                                          QStringLiteral( "minimal" ),
                                          QStringLiteral( "windows" ),
                                          QStringLiteral( "cocoa" ) };

    for ( QString const & platform : platforms )
    {
        for ( Config::ScanPopupWindowFlags const preference :
              { Config::SPWF_Popup, Config::SPWF_Tool } )
        {
            QTest::newRow( qPrintable( QStringLiteral( "%1 preference=%2" )
                                         .arg( platform )
                                         .arg( int( preference ) ) ) )
              << platform << int( preference );
        }
    }
}

void TstPlatformWindow::popupFlagsAttachTheX11BypassHintOnlyOnXcb()
{
    QFETCH( QString, platform );
    QFETCH( int, configured );

    Qt::WindowFlags const requested = PlatformWindow::popupFlags(
      platform, static_cast< Config::ScanPopupWindowFlags >( configured ), true );
    Qt::WindowFlags const notRequested = PlatformWindow::popupFlags(
      platform, static_cast< Config::ScanPopupWindowFlags >( configured ), false );

    // X11BypassWindowManagerHint is an X11 instruction: no other platform may
    // inherit it from a stored preference.
    QVERIFY2( !requested.testFlag( Qt::X11BypassWindowManagerHint ), qPrintable( platform ) );
    QCOMPARE( requested.toInt(), notRequested.toInt() );
}

void TstPlatformWindow::screenForPointUsesThePointScreenAndFallsBackToPrimary()
{
    QScreen * const primary = QGuiApplication::primaryScreen();
    QVERIFY2( primary, "the selected QPA reports no primary screen" );

    QPoint const insidePrimary = primary->geometry().center();

    // The expectation is derived from the screens' own rectangles, not from the
    // production lookup, so this pins "the screen containing the point".
    QScreen * expected = nullptr;

    for ( QScreen * const screen : QGuiApplication::screens() )
    {
        if ( screen->geometry().contains( insidePrimary ) )
        {
            expected = screen;
            break;
        }
    }

    QVERIFY2( expected, "no reported screen contains its own primary centre" );
    QCOMPARE( PlatformWindow::screenForPoint( insidePrimary ), expected );

    QRect const united = unitedScreenGeometry();
    QVERIFY( united.isValid() );

    QPoint const outsideEverything = united.topLeft() - QPoint( 4000, 4000 );

    // The fixture point is genuinely outside every attached screen, so Qt's own
    // lookup answers nothing and the primary fallback is the only screen source.
    QVERIFY2( !QGuiApplication::screenAt( outsideEverything ),
              "the fixture point is inside an attached screen" );
    QCOMPARE( PlatformWindow::screenForPoint( outsideEverything ), primary );
}

void TstPlatformWindow::screenForPointRuleHandlesAnEmptyScreenList()
{
    QScreen * const primary = QGuiApplication::primaryScreen();
    QVERIFY2( primary, "the selected QPA reports no primary screen" );

    // A live QPA always reports a screen, so the no-screen branch is driven
    // through the pure rule with an empty list and the best pointers a platform
    // could still hand out (a session losing its last screen at teardown).
    QCOMPARE( PlatformWindow::detail::screenForPointIn( QList< QScreen * >(), primary, primary ),
              static_cast< QScreen * >( nullptr ) );
    QCOMPARE( PlatformWindow::detail::screenForPointIn( QList< QScreen * >(), nullptr, nullptr ),
              static_cast< QScreen * >( nullptr ) );

    QList< QScreen * > const screens = QGuiApplication::screens();
    QVERIFY( !screens.isEmpty() );

    // With a screen list the point's screen wins over the primary fallback. The
    // live QPA has exactly one screen here, so a real pointer cannot distinguish
    // "the point's screen" from "the primary screen": these two rows use distinct
    // sentinel pointers that are compared, never dereferenced.
    QScreen * const sentinelPoint = reinterpret_cast< QScreen * >( quintptr( 0x1000 ) );
    QScreen * const sentinelPrimary = reinterpret_cast< QScreen * >( quintptr( 0x2000 ) );
    // Compared as void* so a failing assertion prints addresses instead of
    // asking QDebug to render a QScreen that was never a real object.
    QCOMPARE( static_cast< void * >( PlatformWindow::detail::screenForPointIn( screens, sentinelPoint,
                                                                              sentinelPrimary ) ),
              static_cast< void * >( sentinelPoint ) );
    QCOMPARE( static_cast< void * >(
                PlatformWindow::detail::screenForPointIn( screens, nullptr, sentinelPrimary ) ),
              static_cast< void * >( sentinelPrimary ) );

    QCOMPARE( PlatformWindow::detail::screenForPointIn( screens, primary, nullptr ), primary );
    QCOMPARE( PlatformWindow::detail::screenForPointIn( screens, nullptr, primary ), primary );
    QCOMPARE( PlatformWindow::detail::screenForPointIn( screens, nullptr, nullptr ),
              static_cast< QScreen * >( nullptr ) );
}

void TstPlatformWindow::availableGeometryForPointUsesTheAvailableArea()
{
    QScreen * const screen = QGuiApplication::primaryScreen();
    QVERIFY2( screen, "the selected QPA reports no primary screen" );

    QPoint const inside = screen->geometry().center();
    QRect const full = screen->geometry();

    // The contract in a session without a desktop reservation: the available
    // rectangle of the chosen screen, contained in that screen.
    QCOMPARE( PlatformWindow::availableGeometryForPoint( inside ), screen->availableGeometry() );
    QVERIFY( full.contains( PlatformWindow::availableGeometryForPoint( inside ) ) );

    // A point outside every screen keeps the window on a real, usable area
    // instead of collapsing to an empty rectangle.
    QRect const united = unitedScreenGeometry();
    QVERIFY( united.isValid() );

    QPoint const outsideEverything = united.topLeft() - QPoint( 4000, 4000 );
    QCOMPARE( PlatformWindow::availableGeometryForPoint( outsideEverything ),
              screen->availableGeometry() );

#ifdef HAVE_X11
    Display * const display = PlatformWindow::x11Display();

    // A bare X server (the xvfb gate) has no window manager, so a reservation
    // can be installed exactly like a panel installs one. That is the state in
    // which a screen's available geometry differs from its full geometry and
    // therefore the only state that can tell "available" apart from "full". A
    // session with a real window manager owns that property and is never touched
    // here; the gate run always takes the strict branch.
    if ( display && !sessionHasWindowManager( display ) )
    {
        QRect const reserved( full.x(),
                              full.y(),
                              qMax( 1, full.width() - 120 ),
                              qMax( 1, full.height() - 90 ) );

        {
            WorkAreaRestoration const restoration( display, screen, full );

            QVERIFY2( writeWorkArea( display, reserved ),
                      "could not install a desktop reservation on the test display" );
            QTRY_VERIFY( screen->availableGeometry() != full );

            QCOMPARE( PlatformWindow::availableGeometryForPoint( inside ),
                      screen->availableGeometry() );
            QVERIFY2( PlatformWindow::availableGeometryForPoint( inside ) != full,
                      "availableGeometryForPoint reported the full screen geometry instead of the "
                      "available area" );
        }

        // The reservation is gone again, so a later suite never inherits it.
        QTRY_COMPARE( screen->availableGeometry(), full );
    }
#endif
}

void TstPlatformWindow::availableGeometryRuleIsEmptyWithoutAScreen()
{
    // No screen means no placement area; this branch is otherwise unreachable on
    // a live QPA.
    QCOMPARE( PlatformWindow::detail::availableGeometryIn( nullptr ), QRect() );

    QScreen * const screen = QGuiApplication::primaryScreen();
    QVERIFY2( screen, "the selected QPA reports no primary screen" );

    // Never the full geometry: a caller that needs that reads
    // screenForPoint()->geometry() explicitly.
    QCOMPARE( PlatformWindow::detail::availableGeometryIn( screen ), screen->availableGeometry() );
}

void TstPlatformWindow::applyActivationIsNullSafeAndNeverInventsAHandle()
{
    // A null window is not an error: the popup and the main window both call
    // this during state changes, and the helper is best-effort.
    PlatformWindow::applyActivation( nullptr, QString() );
    PlatformWindow::applyActivation( nullptr, QStringLiteral( "activation-token-must-not-crash" ) );

    // A widget that was never shown has no native handle. Activation must not
    // create one (showing the window is the caller's normal path) and must not
    // dereference the missing handle.
    QWidget notShown;
    QVERIFY2( !notShown.windowHandle(), "a freshly constructed widget already has a native window" );

    PlatformWindow::applyActivation( &notShown, QStringLiteral( "activation-token" ) );

    QVERIFY2( !notShown.windowHandle(),
              "applyActivation created a native window; showing it is the caller's path" );
}

void TstPlatformWindow::applyActivationRequestsWithoutClaimingOrLoggingTheToken()
{
    QWidget window;
    window.resize( 200, 120 );
    window.show(); // the caller's normal path creates the native window

    QVERIFY2( window.windowHandle(),
              "the shown window has no native handle to request activation for" );

    QString const token = QStringLiteral( "fixture-xdg-activation-token" );
    QStringList messages;

    {
        MessageCapture const capture( messages );

        PlatformWindow::applyActivation( &window, token );
        PlatformWindow::applyActivation( &window, QString() );
    }

    QString const log = messages.join( QLatin1Char( '\n' ) );

    QVERIFY2( !log.isEmpty(), "applyActivation logged nothing; the request must be logged" );
    QVERIFY2( log.contains( QStringLiteral( "activation" ), Qt::CaseInsensitive ), qPrintable( log ) );

    // The token is a bearer capability: its value never reaches the log, only
    // its presence does.
    QVERIFY2( !log.contains( token ), "applyActivation logged the activation token value" );

    // The helper requests; it never claims that focus was granted.
    QVERIFY2( !log.contains( QStringLiteral( "granted" ), Qt::CaseInsensitive ), qPrintable( log ) );

    // The token argument participates in the request: the first call carries one,
    // the second one does not.
    QVERIFY2( log.contains( QStringLiteral( "present" ) ), qPrintable( log ) );
    QVERIFY2( log.contains( QStringLiteral( "absent" ) ), qPrintable( log ) );

    window.hide();
}

void TstPlatformWindow::applyActivationNeverClobbersAnExistingActivationToken()
{
    // An empty token must not be installed: it would clear a token another
    // component (or Qt) left in place for the next requestActivate(). This holds
    // with and without the KDE helper.
    qputenv( "XDG_ACTIVATION_TOKEN", QByteArrayLiteral( "pre-existing-activation-token" ) );

    PlatformWindow::applyActivation( nullptr, QString() );

    QCOMPARE( qEnvironmentVariable( "XDG_ACTIVATION_TOKEN" ),
              QStringLiteral( "pre-existing-activation-token" ) );

    QWidget window;
    window.show();
    QVERIFY2( window.windowHandle(), "the shown window has no native handle" );

    PlatformWindow::applyActivation( &window, QString() );
    QCOMPARE( qEnvironmentVariable( "XDG_ACTIVATION_TOKEN" ),
              QStringLiteral( "pre-existing-activation-token" ) );

    window.hide();
    qunsetenv( "XDG_ACTIVATION_TOKEN" );
}

void TstPlatformWindow::activationRuleInstallsTheTokenOnlyForAHandledWindow()
{
    // The rule is exercised through the injected effects, so the conditions are
    // observable rather than inferred from a log line: only a window that already
    // has a native handle gets the token, and a null widget or a handle-less
    // widget must leave the process-wide token untouched.
    QStringList recorder;
    PlatformWindow::detail::ActivationOps const ops{
        [ &recorder ]( QString const & value ) { recorder << QStringLiteral( "token:" ) + value; },
        [ &recorder ]( QWindow * ) { recorder << QStringLiteral( "activate" ); }
    };

    // A null widget: nothing happens at all.
    QCOMPARE( PlatformWindow::detail::activateIn( nullptr, QStringLiteral( "T" ), ops ),
              PlatformWindow::detail::ActivationOutcome::NoWindow );
    QVERIFY( recorder.isEmpty() );

    // A constructed but not shown widget has no native handle yet, so the token
    // must not be installed: it would be left behind on the abandoned path.
    QWidget handleless;
    QVERIFY( handleless.windowHandle() == nullptr );
    QCOMPARE( PlatformWindow::detail::activateIn( &handleless, QStringLiteral( "T" ), ops ),
              PlatformWindow::detail::ActivationOutcome::NoHandle );
    QVERIFY2( recorder.isEmpty(), qPrintable( recorder.join( QStringLiteral( "," ) ) ) );

    // A shown widget has a handle: the token is installed once and activation
    // requested once.
    QWidget shown;
    shown.resize( 120, 80 );
    shown.show();
    QVERIFY( QTest::qWaitForWindowExposed( &shown ) );
    QVERIFY( shown.windowHandle() != nullptr );

    QCOMPARE( PlatformWindow::detail::activateIn( &shown, QStringLiteral( "T" ), ops ),
              PlatformWindow::detail::ActivationOutcome::Requested );
    QCOMPARE( recorder, ( QStringList{ QStringLiteral( "token:T" ), QStringLiteral( "activate" ) } ) );

    // An empty token is not forwarded, but activation is still requested.
    recorder.clear();
    QCOMPARE( PlatformWindow::detail::activateIn( &shown, QString(), ops ),
              PlatformWindow::detail::ActivationOutcome::Requested );
    QCOMPARE( recorder, QStringList{ QStringLiteral( "activate" ) } );
}

void TstPlatformWindow::activationRuleRequestsActivationAfterTheToken()
{
    // Order matters: a token installed after the request would be consumed by the
    // next unrelated activation instead of this one.
    QStringList order;
    PlatformWindow::detail::ActivationOps const ops{
        [ &order ]( QString const & ) { order << QStringLiteral( "token" ); },
        [ &order ]( QWindow * ) { order << QStringLiteral( "activate" ); }
    };

    QWidget window;
    window.resize( 120, 80 );
    window.show();
    QVERIFY( QTest::qWaitForWindowExposed( &window ) );

    QCOMPARE( PlatformWindow::detail::activateIn( &window, QStringLiteral( "T" ), ops ),
              PlatformWindow::detail::ActivationOutcome::Requested );
    QCOMPARE( order, ( QStringList{ QStringLiteral( "token" ), QStringLiteral( "activate" ) } ) );

    // A build without a backend step has a null setToken: activation is still
    // requested and no token is installed.
    order.clear();
    PlatformWindow::detail::ActivationOps const portable{
        nullptr, [ &order ]( QWindow * ) { order << QStringLiteral( "activate" ); }
    };
    QCOMPARE( PlatformWindow::detail::activateIn( &window, QStringLiteral( "T" ), portable ),
              PlatformWindow::detail::ActivationOutcome::Requested );
    QCOMPARE( order, QStringList{ QStringLiteral( "activate" ) } );
}

void TstPlatformWindow::activationOpsWireTheRequestInEveryBuild()
{
    // Spec §4.13: the KDE helper only adds the activation token, so a build
    // without it must still request activation. This pins the *wiring*, which a
    // pure-rule fixture cannot see: dropping the request callback turns this red
    // in every build variant.
    PlatformWindow::detail::ActivationOps const ops = PlatformWindow::detail::activationOps();
    QVERIFY2( bool( ops.requestActivate ),
              "applyActivation must request activation even without the KDE helper" );

#ifdef HAVE_KWINDOWSYSTEM
    QVERIFY2( bool( ops.setToken ), "the KDE build must hand the activation token over" );
#else
    QVERIFY2( !ops.setToken, "a build without KDE has no token hand-off" );
#endif
}

void TstPlatformWindow::activationRequestReachesThePlatform()
{
    // F-7: the previous fixture only proved a callback exists. This one drives
    // the production effect against a real, shown window and requires the
    // platform to report the window active, so a blackholed effect body (the
    // mutant that survived F-6) is red in every build variant.
    QWidget window;
    window.resize( 160, 100 );
    window.show();
    QVERIFY( QTest::qWaitForWindowExposed( &window ) );

    QWindow * const handle = window.windowHandle();
    QVERIFY( handle != nullptr );

    if( QTest::currentTestFailed() )
        return;

    if( handle->isActive() )
    {
        // Already active: the request cannot be distinguished from the status quo,
        // so state that instead of pretending to have proven it.
        QSKIP( "the window is already active; the platform request cannot be observed" );
    }

    PlatformWindow::detail::ActivationOps const ops = PlatformWindow::detail::activationOps();
    QVERIFY( bool( ops.requestActivate ) );
    ops.requestActivate( handle );

    QTRY_VERIFY_WITH_TIMEOUT( handle->isActive(), 5000 );
}

void TstPlatformWindow::x11DisplayIsBorrowedOnlyOnXcb()
{
#ifdef HAVE_X11
    Display * const display = PlatformWindow::x11Display();

    const bool xcbSession =
      QGuiApplication::platformName().toLower().startsWith( QLatin1String( "xcb" ) );

    if ( xcbSession )
    {
        QVERIFY2( display, "an xcb session must lend Qt's Xlib display" );

#if QT_CONFIG( xcb )
        auto * const native = qGuiApp->nativeInterface< QNativeInterface::QX11Application >();
        QVERIFY2( native, "the xcb session exposes no X11 native interface" );

        // Borrowed, not owned: the handle is the one Qt opened for this process,
        // so it is usable immediately and never closed here.
        QCOMPARE( display, native->display() );
#endif

        QVERIFY( XDefaultScreen( display ) >= 0 );
    }
    else
    {
        // HAVE_X11 is a build capability: on any other QPA there is no display
        // to borrow and none may be opened on the side.
        QVERIFY2( !display, "a non-xcb QPA must not hand out an X11 display" );
    }
#else
    // The portable variant is compiled from this same source without HAVE_X11:
    // the helper does not exist there at all, and initTestCase() already proved
    // that the variant really dropped the capability.
    QVERIFY( !QGuiApplication::platformName().isEmpty() );
#endif
}

void TstPlatformWindow::recorderSessionFollowsTheActivePlatformName()
{
    // The X11 recorder's platform gate is the same runtime rule as the window
    // helpers': this build's X11 capability (HAVE_X11) never makes a session an X11
    // one, and the name comparison is case-insensitive so a capitalised "Wayland"
    // cannot be taken for X11.
    QVERIFY( HotkeyRuntime::isX11Session( QStringLiteral( "xcb" ) ) );
    QVERIFY( HotkeyRuntime::isX11Session( QStringLiteral( "XCB" ) ) );
    QVERIFY( !HotkeyRuntime::isX11Session( QStringLiteral( "Wayland" ) ) );
    QVERIFY( !HotkeyRuntime::isX11Session( QStringLiteral( "WAYLAND" ) ) );
    QVERIFY( !HotkeyRuntime::isX11Session( QStringLiteral( "wayland/xcb" ) ) );
    QVERIFY( !HotkeyRuntime::isX11Session( QStringLiteral( "offscreen" ) ) );

    // The recorder session exists only for an X11 session of the running platform.
#ifdef GD_TEST_PLATFORMWINDOW_PORTABLE
    // This variant is compiled without the X11 capability: the production sources
    // take their disabled paths and no recorder session can be created at all.
    QVERIFY( !createRecorderSession() );
#else
    QCOMPARE( bool( createRecorderSession() ),
              HotkeyRuntime::isX11Session( QGuiApplication::platformName() ) );
#endif
}

int main( int argc, char *argv[] )
{
    if ( TestSupport::isWatchdogProbeProcess() )
        TestSupport::runWatchdogProbe();

    QApplication app( argc, argv );
    TstPlatformWindow testCase;

    return QTest::qExec( &testCase, argc, argv );
}

#include "tst_platformwindow.moc"
