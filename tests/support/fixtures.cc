/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Test-only support implementation, see tests/support/fixtures.hh.
 */

#include "fixtures.hh"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

/// Exit code a watchdog expiry reports; the convention GNU timeout uses, so a
/// harness can tell a wedged binary from an ordinary assertion failure.
constexpr int WatchdogExitCode = 124;

/// Upper bound on the ancestor walk that locates the repository root.
constexpr int MaxRootSearchDepth = 12;

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

bool displayAvailable()
{
    return !qEnvironmentVariable( "DISPLAY" ).isEmpty()
        || !qEnvironmentVariable( "WAYLAND_DISPLAY" ).isEmpty()
        || headlessPlatformSelected();
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

            // The suite's own event loop is presumed wedged: report and leave
            // without running destructors, atexit handlers or atexit statics.
            std::fprintf( stderr,
                          "\n*** TEST WATCHDOG: the binary exceeded its %d s budget and is being terminated ***\n",
                          seconds );
            std::fflush( stderr );

            std::_Exit( WatchdogExitCode );
        } );
    }
};

Watchdog & watchdog()
{
    static Watchdog instance;

    return instance;
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
        if ( displayAvailable() )
            return true;

        if ( reason )
            *reason = QStringLiteral( "no display server reachable (DISPLAY, WAYLAND_DISPLAY and a "
                                      "headless QPA are all absent)" );
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

        if ( qEnvironmentVariable( "DISPLAY" ).isEmpty() )
        {
            if ( reason )
                *reason = QStringLiteral( "DISPLAY is unset" );
            return false;
        }

        return true;
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

        if ( qEnvironmentVariable( "WAYLAND_DISPLAY" ).isEmpty() )
        {
            if ( reason )
                *reason = QStringLiteral( "WAYLAND_DISPLAY is unset" );
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
                                  "wayland, webengine)" );
    return false;
}

} // namespace TestSupport
