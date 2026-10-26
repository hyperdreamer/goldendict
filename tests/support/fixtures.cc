/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Test-only support implementation, see tests/support/fixtures.hh.
 */

#include "fixtures.hh"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTcpSocket>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace {

/// Upper bound on the ancestor walk that locates the repository root.
constexpr int MaxRootSearchDepth = 12;

/// Environment variable that switches a test binary into the watchdog expiry
/// probe (see TestSupport::runWatchdogProbe).
char const WatchdogProbeVariable[] = "GD_TEST_WATCHDOG_PROBE";

/// Hard bound the parent-side watchdog test allows a stalled probe process
/// before it declares the expiry path broken and kills it.
constexpr int WatchdogProbeDeadlineMs = 10000;

/// Bounded connect timeout of a display reachability probe. An unreachable or
/// half-open endpoint must fail the gate with a reason, never wedge it.
constexpr int DisplayProbeTimeoutMs = 750;

void appendReason( QStringList * reasons, QString const & reason )
{
    if ( reasons )
        *reasons << reason;
}

QStringList splitList( QString const & value )
{
    QStringList result;

    for ( QString const & item : value.split( QLatin1Char( ':' ) ) )
    {
        const QString trimmed = item.trimmed();
        if ( !trimmed.isEmpty() )
            result << trimmed;
    }

    return result;
}

QString platformOverride()
{
    return qEnvironmentVariable( "QT_QPA_PLATFORM" ).trimmed().toLower();
}

bool headlessPlatformSelected()
{
    const QString platform = platformOverride();

    return platform.startsWith( QLatin1String( "offscreen" ) )
        || platform.startsWith( QLatin1String( "minimal" ) );
}

/// Parsed X11 DISPLAY value: "[protocol/][host]:display[.screen]".
struct X11DisplayAddress {
    bool valid = false;
    QString host;
    int displayNumber = -1;
    QString problem;
};

X11DisplayAddress parseX11Display( QString const & display )
{
    X11DisplayAddress address;

    const QString value = display.trimmed();
    const int colon = value.lastIndexOf( QLatin1Char( ':' ) );

    if ( value.isEmpty() || colon < 0 )
    {
        address.problem = QStringLiteral( "DISPLAY \"%1\" is not a [protocol/][host]:display[.screen] value" )
                              .arg( value.isEmpty() ? QStringLiteral( "(empty)" ) : value );
        return address;
    }

    QString host = value.left( colon );
    const int protocolSeparator = host.lastIndexOf( QLatin1Char( '/' ) );

    if ( protocolSeparator >= 0 )
        host = host.mid( protocolSeparator + 1 );

    address.host = host;

    const QString afterColon = value.mid( colon + 1 );
    const int dot = afterColon.indexOf( QLatin1Char( '.' ) );
    bool numberOk = false;
    const int displayNumber = ( dot >= 0 ? afterColon.left( dot ) : afterColon ).toInt( &numberOk );

    if ( !numberOk || displayNumber < 0 )
    {
        address.problem = QStringLiteral( "DISPLAY \"%1\" has no usable display number" ).arg( value );
        return address;
    }

    address.valid = true;
    address.displayNumber = displayNumber;

    return address;
}

/// Time-bounded connect to a unix domain socket. A missing socket fails
/// immediately; a listening one is reported as soon as the connection is up.
bool unixSocketReachable( QString const & path, QString * reason )
{
    QLocalSocket socket;

    socket.connectToServer( path, QIODevice::ReadWrite );

    if ( socket.waitForConnected( DisplayProbeTimeoutMs ) )
    {
        socket.disconnectFromServer();
        return true;
    }

    if ( reason )
        *reason = QStringLiteral( "the unix socket %1 is not accepting connections (%2)" )
                      .arg( path, socket.errorString() );
    return false;
}

bool x11DisplayReachableImpl( QString const & display, QString * reason )
{
    const X11DisplayAddress address = parseX11Display( display );

    if ( !address.valid )
    {
        if ( reason )
            *reason = address.problem;
        return false;
    }

    // A local display (empty host, "unix", or an explicit "unix/" protocol) is
    // served by the abstract/local socket of the display, a remote one by TCP
    // port 6000+display.
    const bool localDisplay = address.host.isEmpty() || address.host == QLatin1String( "unix" );

    if ( localDisplay )
    {
        const QString socketPath = QStringLiteral( "/tmp/.X11-unix/X%1" ).arg( address.displayNumber );
        QString failure;

        if ( unixSocketReachable( socketPath, &failure ) )
            return true;

        if ( reason )
            *reason = QStringLiteral( "the X server of DISPLAY \"%1\" is not reachable: %2" )
                          .arg( display, failure );
        return false;
    }

    if ( address.displayNumber > 65535 - 6000 )
    {
        if ( reason )
            *reason = QStringLiteral( "DISPLAY \"%1\" cannot address the X11 TCP port 6000+display" )
                          .arg( display );
        return false;
    }

    const QString host = address.host;
    const quint16 port = static_cast< quint16 >( 6000 + address.displayNumber );

    QTcpSocket socket;

    socket.connectToHost( host, port );

    if ( socket.waitForConnected( DisplayProbeTimeoutMs ) )
    {
        socket.disconnectFromHost();
        return true;
    }

    if ( reason )
        *reason = QStringLiteral( "the X server of DISPLAY \"%1\" is not reachable at %2:%3 (%4)" )
                      .arg( display, host )
                      .arg( port )
                      .arg( socket.errorString() );
    return false;
}

bool waylandDisplayReachableImpl( QString const & waylandDisplay, QString * reason )
{
    const QString value = waylandDisplay.trimmed();

    if ( value.isEmpty() )
    {
        if ( reason )
            *reason = QStringLiteral( "WAYLAND_DISPLAY is empty" );
        return false;
    }

    QString socketPath = value;
    QString failure;

    if ( !socketPath.contains( QLatin1Char( '/' ) ) )
    {
        const QString runtimeDirectory = qEnvironmentVariable( "XDG_RUNTIME_DIR" ).trimmed();

        if ( runtimeDirectory.isEmpty() )
        {
            if ( reason )
                *reason = QStringLiteral( "WAYLAND_DISPLAY \"%1\" is relative but XDG_RUNTIME_DIR is unset" )
                              .arg( value );
            return false;
        }

        socketPath = QDir( runtimeDirectory ).absoluteFilePath( value );
    }

    if ( unixSocketReachable( socketPath, &failure ) )
        return true;

    if ( reason )
        *reason = QStringLiteral( "the Wayland compositor of WAYLAND_DISPLAY \"%1\" is not reachable: %2" )
                      .arg( value, failure );
    return false;
}

bool looksLikeRepositoryRoot( QDir const & dir )
{
    return QFileInfo( dir.absoluteFilePath( QStringLiteral( "goldendict.pro" ) ) ).isFile();
}

QString findRepositoryRootUpwards( QString const & startPath )
{
    if ( startPath.isEmpty() )
        return QString();

    QDir dir( startPath );

    for ( int depth = 0; depth < MaxRootSearchDepth; ++depth )
    {
        if ( looksLikeRepositoryRoot( dir ) )
            return dir.absolutePath();

        if ( dir.isRoot() || !dir.cdUp() )
            break;
    }

    return QString();
}

QString computeRepositoryRoot()
{
    const QString overridePath = qEnvironmentVariable( "GD_TEST_REPO_ROOT" ).trimmed();

    if ( !overridePath.isEmpty() )
        return QDir( overridePath ).absolutePath();

    QStringList starts;

    if ( QCoreApplication::instance() )
        starts << QCoreApplication::applicationDirPath();

    starts << QDir::currentPath();

    for ( QString const & start : starts )
    {
        const QString found = findRepositoryRootUpwards( start );

        if ( !found.isEmpty() )
            return found;
    }

#ifdef GD_TEST_SOURCE_ROOT
    // Build-time source root, used when neither the binary nor the working
    // directory sits inside the checkout (for example a shadow build directory).
    const QString sourceRoot = QString::fromUtf8( GD_TEST_SOURCE_ROOT );

    if ( looksLikeRepositoryRoot( QDir( sourceRoot ) ) )
        return QDir( sourceRoot ).absolutePath();
#endif

    return QString();
}

/// Best-effort expiry notice. It must never delay the unconditional process
/// exit, so it deliberately does not use a stdio stream: a stalled thread that
/// holds flockfile(stderr) would block fprintf/fflush forever, while a raw
/// descriptor write is unaffected by the stream lock. The descriptor is switched
/// to non-blocking for the attempt, so a full, unread stderr pipe cannot block
/// either; a diagnostic that cannot be delivered is dropped.
void writeStderrBestEffort( char const * message, std::size_t size )
{
    const int originalFlags = ::fcntl( STDERR_FILENO, F_GETFL );

    if ( originalFlags == -1 )
        return;

    if ( ( originalFlags & O_NONBLOCK ) == 0
         && ::fcntl( STDERR_FILENO, F_SETFL, originalFlags | O_NONBLOCK ) == -1 )
        return;

    // A partial or refused write is intentionally ignored: the exit follows.
    const ssize_t written = ::write( STDERR_FILENO, message, size );
    (void)written;

    ::fcntl( STDERR_FILENO, F_SETFL, originalFlags );
}

/// Process-lifetime watchdog. The destructor disarms the helper thread so a
/// suite that forgets clearWatchdog() still exits cleanly.
struct Watchdog {
    std::mutex mutex;
    std::condition_variable wakeup;
    std::thread thread;
    bool stop = false;

    ~Watchdog()
    {
        disarm();
    }

    void disarm()
    {
        if ( !thread.joinable() )
            return;

        {
            std::lock_guard< std::mutex > lock( mutex );
            stop = true;
        }

        wakeup.notify_all();
        thread.join();
    }

    void arm( int seconds )
    {
        disarm();

        if ( seconds <= 0 )
            return;

        {
            std::lock_guard< std::mutex > lock( mutex );
            stop = false;
        }

        thread = std::thread( [ this, seconds ]() {
            std::unique_lock< std::mutex > lock( mutex );

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( seconds );

            if ( wakeup.wait_until( lock, deadline, [ this ]() { return stop; } ) )
                return;

            lock.unlock();

            // The suite's own event loop is presumed wedged. The exit is
            // unconditional and immediate: no destructors, no atexit handlers,
            // and nothing that can block behind whatever the stalled thread
            // holds.
            char notice[ 192 ];
            const int noticeLength =
                std::snprintf( notice,
                               sizeof( notice ),
                               "\n*** TEST WATCHDOG: the binary exceeded its %d s budget and is being terminated ***\n",
                               seconds );

            if ( noticeLength > 0 )
                writeStderrBestEffort( notice, static_cast< std::size_t >( noticeLength ) );

            std::_Exit( TestSupport::WatchdogExitCode );
        } );
    }
};

Watchdog & watchdog()
{
    static Watchdog instance;

    return instance;
}

/// Replaces stderr with an already full pipe whose read end is never read, so
/// that any blocking write or stdio flush to stderr can never return. Used by
/// the watchdog expiry probe to emulate a wedged suite with a blocked log sink.
void blockStderrWithFullPipe()
{
    int descriptors[ 2 ];

    if ( ::pipe( descriptors ) != 0 )
        return;

    const int originalFlags = ::fcntl( descriptors[ 1 ], F_GETFL );

    if ( originalFlags == -1 )
        return;

    ::fcntl( descriptors[ 1 ], F_SETFL, originalFlags | O_NONBLOCK );

    char buffer[ 4096 ] = {};

    while ( ::write( descriptors[ 1 ], buffer, sizeof buffer ) > 0 )
        ;

    ::fcntl( descriptors[ 1 ], F_SETFL, originalFlags );
    ::dup2( descriptors[ 1 ], STDERR_FILENO );
    ::close( descriptors[ 1 ] );

    // descriptors[ 0 ] stays open on purpose: the pipe remains full instead of
    // turning into a broken pipe that would raise SIGPIPE on the next write.
}

} // namespace

namespace TestSupport {

bool requiredEnvironment( QStringList * reasons )
{
    bool available = true;

    if ( qEnvironmentVariable( "GD_TEST_FORCE_ENV_UNAVAILABLE" ) == QLatin1String( "1" ) )
    {
        appendReason( reasons,
                      QStringLiteral( "GD_TEST_FORCE_ENV_UNAVAILABLE=1: the suite requested an "
                                      "unavailable environment" ) );
        available = false;
    }

    for ( QString const & fixture : splitList( qEnvironmentVariable( "GD_TEST_REQUIRED_FIXTURES" ) ) )
    {
        if ( !fixtureExists( fixture ) )
        {
            appendReason( reasons,
                          QStringLiteral( "missing fixture file: %1" ).arg( fixturePath( fixture ) ) );
            available = false;
        }
    }

    for ( QString const & prerequisite : splitList( qEnvironmentVariable( "GD_TEST_REQUIRED_PREREQUISITES" ) ) )
    {
        QString reason;

        if ( !prerequisiteAvailable( prerequisite, &reason ) )
        {
            appendReason( reasons,
                          QStringLiteral( "prerequisite \"%1\" is unavailable: %2" )
                              .arg( prerequisite, reason ) );
            available = false;
        }
    }

    return available;
}

void installWatchdog( int seconds )
{
    watchdog().arm( seconds );
}

void clearWatchdog()
{
    watchdog().disarm();
}

bool isWatchdogProbeProcess()
{
    return !qEnvironmentVariable( WatchdogProbeVariable ).trimmed().isEmpty();
}

void runWatchdogProbe()
{
    const QString mode = qEnvironmentVariable( WatchdogProbeVariable ).trimmed();

    // Make stderr hostile first, exactly like a stalled suite would: the expiry
    // path must not depend on either the stdio lock or a writable descriptor.
    if ( mode == QLatin1String( "held-stderr" ) )
        ::flockfile( stderr );
    else if ( mode == QLatin1String( "blocked-stderr" ) )
        blockStderrWithFullPipe();

    installWatchdog( WatchdogProbeSeconds );

    for (;;)
        std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
}

bool verifyWatchdogExpiry( QString const & binaryPath, QStringList * reasons )
{
    static char const * const modes[] = { "plain", "held-stderr", "blocked-stderr" };

    bool verified = true;

    for ( char const * mode : modes )
    {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert( QLatin1String( WatchdogProbeVariable ), QLatin1String( mode ) );

        QProcess probe;
        probe.setProcessEnvironment( environment );

        QElapsedTimer elapsed;
        elapsed.start();
        probe.start( binaryPath, QStringList() );

        if ( !probe.waitForStarted( WatchdogProbeDeadlineMs ) )
        {
            appendReason( reasons,
                          QStringLiteral( "watchdog probe \"%1\" did not start: %2" )
                              .arg( QLatin1String( mode ), probe.errorString() ) );
            verified = false;
            continue;
        }

        const bool finished = probe.waitForFinished( WatchdogProbeDeadlineMs );
        const qint64 elapsedMs = elapsed.elapsed();

        if ( !finished )
        {
            probe.kill();
            probe.waitForFinished( WatchdogProbeDeadlineMs );
            appendReason( reasons,
                          QStringLiteral( "watchdog probe \"%1\" was still running after %2 ms: "
                                          "the expiry path did not terminate the process" )
                              .arg( QLatin1String( mode ) )
                              .arg( elapsedMs ) );
            verified = false;
            continue;
        }

        if ( probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != WatchdogExitCode )
        {
            appendReason( reasons,
                          QStringLiteral( "watchdog probe \"%1\" exited with code %2 (status %3) "
                                          "instead of the watchdog exit code %4" )
                              .arg( QLatin1String( mode ) )
                              .arg( probe.exitCode() )
                              .arg( int( probe.exitStatus() ) )
                              .arg( WatchdogExitCode ) );
            verified = false;
            continue;
        }

        if ( elapsedMs < WatchdogProbeSeconds * 1000LL * 9 / 10 )
        {
            appendReason( reasons,
                          QStringLiteral( "watchdog probe \"%1\" exited after %2 ms, before its %3 s budget" )
                              .arg( QLatin1String( mode ) )
                              .arg( elapsedMs )
                              .arg( WatchdogProbeSeconds ) );
            verified = false;
        }
    }

    return verified;
}

QStringList compiledFeatureDefines()
{
    QStringList features;

#ifdef HAVE_X11
    features << QStringLiteral( "HAVE_X11" );
#endif
#ifdef MAKE_FFMPEG_PLAYER
    features << QStringLiteral( "MAKE_FFMPEG_PLAYER" );
#endif
#ifdef MAKE_EXTRA_TIFF_HANDLER
    features << QStringLiteral( "MAKE_EXTRA_TIFF_HANDLER" );
#endif
#ifdef MAKE_ZIM_SUPPORT
    features << QStringLiteral( "MAKE_ZIM_SUPPORT" );
#endif
#ifdef NO_EPWING_SUPPORT
    features << QStringLiteral( "NO_EPWING_SUPPORT" );
#endif
#ifdef MAKE_CHINESE_CONVERSION_SUPPORT
    features << QStringLiteral( "MAKE_CHINESE_CONVERSION_SUPPORT" );
#endif
#ifdef OLD_HUNSPELL_INTERFACE
    features << QStringLiteral( "OLD_HUNSPELL_INTERFACE" );
#endif

    return features;
}

bool x11DisplayReachable( QString const & display, QString * reason )
{
    return x11DisplayReachableImpl( display, reason );
}

bool waylandDisplayReachable( QString const & waylandDisplay, QString * reason )
{
    return waylandDisplayReachableImpl( waylandDisplay, reason );
}

QString repositoryRoot()
{
    static const QString root = computeRepositoryRoot();

    return root;
}

QString fixtureRoot()
{
    const QString overridePath = qEnvironmentVariable( "GD_TEST_FIXTURES" ).trimmed();

    if ( !overridePath.isEmpty() )
        return QDir( overridePath ).absolutePath();

    const QString root = repositoryRoot();

    if ( root.isEmpty() )
        return QString();

    return QDir( root ).absoluteFilePath( QStringLiteral( "tests/fixtures" ) );
}

QString fixturePath( QString const & relativePath )
{
    const QString root = fixtureRoot();

    if ( root.isEmpty() )
        return relativePath;

    return QDir( root ).absoluteFilePath( relativePath );
}

bool fixtureExists( QString const & relativePath )
{
    const QFileInfo info( fixturePath( relativePath ) );

    return info.isFile() && info.isReadable();
}

bool prerequisiteAvailable( QString const & token, QString * reason )
{
    const QString normalized = token.trimmed().toLower();

    if ( normalized.isEmpty() )
        return true;

    if ( normalized == QLatin1String( "display" ) )
    {
        // A headless platform plugin needs no display server at all.
        if ( headlessPlatformSelected() )
            return true;

        QStringList failures;
        bool configured = false;

        const QString display = qEnvironmentVariable( "DISPLAY" ).trimmed();
        const QString waylandDisplay = qEnvironmentVariable( "WAYLAND_DISPLAY" ).trimmed();

        if ( !display.isEmpty() )
        {
            configured = true;

            QString failure;

            if ( x11DisplayReachableImpl( display, &failure ) )
                return true;

            failures << failure;
        }

        if ( !waylandDisplay.isEmpty() )
        {
            configured = true;

            QString failure;

            if ( waylandDisplayReachableImpl( waylandDisplay, &failure ) )
                return true;

            failures << failure;
        }

        if ( !configured )
            failures << QStringLiteral( "DISPLAY and WAYLAND_DISPLAY are unset and no headless QPA is "
                                        "selected" );

        if ( reason )
            *reason = failures.join( QStringLiteral( "; " ) );
        return false;
    }

    if ( normalized == QLatin1String( "x11" ) )
    {
        const QString platform = platformOverride();

        if ( !platform.isEmpty() && !platform.startsWith( QLatin1String( "xcb" ) ) )
        {
            if ( reason )
                *reason = QStringLiteral( "the QPA was forced to \"%1\", not xcb" ).arg( platform );
            return false;
        }

        const QString display = qEnvironmentVariable( "DISPLAY" ).trimmed();

        if ( display.isEmpty() )
        {
            if ( reason )
                *reason = QStringLiteral( "DISPLAY is unset" );
            return false;
        }

        // The environment string alone proves nothing: an xcb suite needs a
        // server that answers.
        return x11DisplayReachableImpl( display, reason );
    }

    if ( normalized == QLatin1String( "wayland" ) )
    {
        const QString platform = platformOverride();

        if ( !platform.isEmpty() && !platform.startsWith( QLatin1String( "wayland" ) ) )
        {
            if ( reason )
                *reason = QStringLiteral( "the QPA was forced to \"%1\", not wayland" ).arg( platform );
            return false;
        }

        const QString waylandDisplay = qEnvironmentVariable( "WAYLAND_DISPLAY" ).trimmed();

        if ( waylandDisplay.isEmpty() )
        {
            if ( reason )
                *reason = QStringLiteral( "WAYLAND_DISPLAY is unset" );
            return false;
        }

        return waylandDisplayReachableImpl( waylandDisplay, reason );
    }

    if ( normalized == QLatin1String( "dbus" ) )
    {
        const QString configured = qEnvironmentVariable( "GD_TEST_DBUS_DAEMON" ).trimmed();
        const QString program =
            configured.isEmpty() ? PrivateSessionBus::defaultDaemonProgram() : configured;

        if ( QStandardPaths::findExecutable( program ).isEmpty() )
        {
            if ( reason )
                *reason = QStringLiteral( "the D-Bus daemon executable \"%1\" is not available: "
                                          "portal tests need a private bus" )
                              .arg( program );
            return false;
        }

        // Prove a private bus can really be started and connected to, not just
        // that a binary exists.
        PrivateSessionBus bus;
        QString failure;

        if ( !bus.start( program, &failure ) )
        {
            if ( reason )
                *reason = failure;
            return false;
        }

        return true;
    }

    if ( normalized == QLatin1String( "webengine" ) )
    {
        const QString helper =
            QDir( QLibraryInfo::path( QLibraryInfo::LibraryExecutablesPath ) )
                .absoluteFilePath( QStringLiteral( "QtWebEngineProcess" ) );

        if ( QFileInfo( helper ).isExecutable() )
            return true;

        if ( reason )
            *reason = QStringLiteral( "the QtWebEngineProcess helper is missing at %1" ).arg( helper );
        return false;
    }

    if ( reason )
        *reason = QStringLiteral( "unknown prerequisite token (known tokens: display, x11, "
                                  "wayland, webengine, dbus)" );
    return false;
}


////////// PrivateSessionBus

namespace {

/// Upper bound on the daemon startup/output handshake. A dbus-daemon that does
/// not print an address within it is a required failure, never a skip.
constexpr int PrivateBusStartTimeoutMs = 5000;

/// Upper bound on the daemon's teardown after terminate().
constexpr int PrivateBusStopTimeoutMs = 5000;

} // namespace

struct PrivateSessionBusPrivate
{
    QProcess daemon;
    QDBusConnection connection{ QString() };
    QDBusConnection fixture{ QString() };
    QString address;
    QString error;
    bool started = false;

    bool launch( QString const & program, QString * reason );
    void shutdown();
};

bool PrivateSessionBusPrivate::launch( QString const & program, QString * reason )
{
    const QString executable = QStandardPaths::findExecutable( program );

    if ( executable.isEmpty() )
    {
        error = QStringLiteral( "the D-Bus daemon executable \"%1\" was not found in PATH" )
                    .arg( program );

        if ( reason )
            *reason = error;
        return false;
    }

    daemon.setProcessChannelMode( QProcess::SeparateChannels );
    daemon.start( executable, { QStringLiteral( "--session" ), QStringLiteral( "--nofork" ),
                                QStringLiteral( "--print-address=1" ) } );

    if ( !daemon.waitForStarted( PrivateBusStartTimeoutMs ) )
    {
        error = QStringLiteral( "cannot start the private D-Bus daemon %1: %2" )
                    .arg( executable, daemon.errorString() );

        if ( reason )
            *reason = error;
        return false;
    }

    QElapsedTimer elapsed;
    elapsed.start();

    QByteArray output;

    while ( !output.contains( '\n' ) )
    {
        const qint64 remaining = PrivateBusStartTimeoutMs - elapsed.elapsed();

        if ( remaining <= 0 )
            break;

        if ( !daemon.waitForReadyRead( static_cast< int >( remaining ) ) )
            break;

        output += daemon.readAllStandardOutput();
    }

    const int newline = output.indexOf( '\n' );
    const QByteArray printedAddress = ( newline >= 0 ? output.left( newline ) : output ).trimmed();

    if ( printedAddress.isEmpty() )
    {
        error = QStringLiteral( "the private D-Bus daemon %1 printed no address within %2 ms" )
                    .arg( executable )
                    .arg( PrivateBusStartTimeoutMs );

        if ( reason )
            *reason = error;

        shutdown();
        return false;
    }

    address = QString::fromUtf8( printedAddress );

    connection = QDBusConnection::connectToBus( address, PrivateSessionBus::connectionName() );

    if ( !connection.isConnected() )
    {
        error = QStringLiteral( "the private D-Bus daemon printed %1 but the connection failed" )
                    .arg( address );

        if ( reason )
            *reason = error;

        shutdown();
        return false;
    }

    started = true;
    error.clear();

    return true;
}

void PrivateSessionBusPrivate::shutdown()
{
    if ( fixture.isConnected() )
    {
        QDBusConnection::disconnectFromBus( PrivateSessionBus::fixtureConnectionName() );
        fixture = QDBusConnection( QString() );
    }

    if ( connection.isConnected() )
    {
        QDBusConnection::disconnectFromBus( PrivateSessionBus::connectionName() );
        connection = QDBusConnection( QString() );
    }

    if ( daemon.state() != QProcess::NotRunning )
    {
        daemon.terminate();

        if ( !daemon.waitForFinished( PrivateBusStopTimeoutMs ) )
        {
            daemon.kill();
            daemon.waitForFinished( PrivateBusStopTimeoutMs );
        }
    }

    started = false;
}

PrivateSessionBus::PrivateSessionBus(): d( std::make_unique< PrivateSessionBusPrivate >() )
{
}

PrivateSessionBus::~PrivateSessionBus()
{
    stop();

    d.reset();
}

bool PrivateSessionBus::start( QString * reason )
{
    const QString overrideProgram = qEnvironmentVariable( "GD_TEST_DBUS_DAEMON" ).trimmed();

    return start( overrideProgram.isEmpty() ? defaultDaemonProgram() : overrideProgram, reason );
}

bool PrivateSessionBus::start( QString const & daemonProgram, QString * reason )
{
    if ( !d )
    {
        if ( reason )
            *reason = QStringLiteral( "no private bus implementation" );
        return false;
    }

    if ( d->started )
    {
        if ( reason )
            *reason = QStringLiteral( "the private bus is already running at %1" ).arg( d->address );
        return false;
    }

    return d->launch( daemonProgram, reason );
}

void PrivateSessionBus::stop()
{
    if ( d )
        d->shutdown();
}

QString PrivateSessionBus::address() const
{
    return d ? d->address : QString();
}

QDBusConnection PrivateSessionBus::connection() const
{
    return d ? d->connection : QDBusConnection( QString() );
}

QDBusConnection PrivateSessionBus::fixtureConnection() const
{
    if ( !d || !d->connection.isConnected() )
        return QDBusConnection( QString() );

    if ( !d->fixture.isConnected() )
    {
        d->fixture = QDBusConnection::connectToBus( d->address,
                                                   PrivateSessionBus::fixtureConnectionName() );

        if ( !d->fixture.isConnected() )
            d->fixture = QDBusConnection( QString() );
    }

    return d->fixture;
}

bool PrivateSessionBus::isRunning() const
{
    return d && d->started && d->daemon.state() != QProcess::NotRunning;
}

qint64 PrivateSessionBus::daemonProcessId() const
{
    return d ? d->daemon.processId() : 0;
}

QString PrivateSessionBus::connectionName()
{
    return QStringLiteral( "goldendict-test-private-bus" );
}

QString PrivateSessionBus::fixtureConnectionName()
{
    return QStringLiteral( "goldendict-test-private-bus-fixture" );
}

QString PrivateSessionBus::defaultDaemonProgram()
{
    return QStringLiteral( "dbus-daemon" );
}

} // namespace TestSupport
