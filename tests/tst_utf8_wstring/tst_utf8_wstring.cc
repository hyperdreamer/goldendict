/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Baseline conversion suite: the UTF-8 <-> wide-string helpers (utf8.cc,
 * wstring.cc) and the QString bridges (wstring_qt.cc) that the dictionary
 * backends rely on. These are consumed read-only; the suite only asserts the
 * behaviour that already exists, including the supplementary-plane and
 * embedded-NUL corners the Qt 6 migration must not regress.
 *
 * It is also the home of the harness self-checks: the configuration probe that
 * proves the shared project mirrors the application's feature defines, the
 * display-prerequisite validation and the watchdog termination proof.
 */

#include <QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTcpServer>
#include <QTemporaryDir>

#include <string>

#include "support/fixtures.hh"

#include "tiff.hh"
#include "utf8.hh"
#include "wstring.hh"
#include "wstring_qt.hh"

namespace {

/// Snapshot of one environment variable so a guard test can put it back exactly
/// as it found it, including "was not set at all".
struct EnvironmentValue {
    QByteArray name;
    QByteArray value;
    bool wasSet;

    explicit EnvironmentValue( char const * variableName ):
        name( variableName ),
        value( qgetenv( variableName ) ),
        wasSet( qEnvironmentVariableIsSet( variableName ) )
    {
    }

    void restore() const
    {
        if ( wasSet )
            qputenv( name.constData(), value );
        else
            qunsetenv( name.constData() );
    }
};

} // namespace

class TstUtf8Wstring : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void asciiRoundTrip();
    void embeddedNul();
    void supplementaryConversion();
    void invalidUtf8Throws();

    void guardReportsUnavailableEnvironment();
    void guardAcceptsAvailableFixture();
    void guardValidatesDisplayEndpoints();
    void configurationProbeMirrorsApplicationFeatures();

    void watchdogCanBeRearmedAndDisarmed();
    void watchdogTerminatesStalledProcess();
    void watchdogProbeIsOnlyActiveOnRequest();
};

void TstUtf8Wstring::initTestCase()
{
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );

    // Intentional, target-local probe used to prove the harness reports a real
    // failure. No other suite may read this variable.
    QVERIFY2( qEnvironmentVariable( "GD_TEST_FORCE_FAILURE" ) != "1",
              "GD_TEST_FORCE_FAILURE intentional assertion" );

    TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
}

void TstUtf8Wstring::cleanupTestCase()
{
    TestSupport::clearWatchdog();
}

void TstUtf8Wstring::asciiRoundTrip()
{
    const std::string ascii( "GoldenDict", 10 );
    const gd::wstring wide = Utf8::decode( ascii );

    QCOMPARE( static_cast< int >( wide.size() ), 10 );
    QCOMPARE( Utf8::encode( wide ), ascii );

    const QString text = QString::fromUtf8( ascii.data(), static_cast< int >( ascii.size() ) );
    QCOMPARE( gd::toQString( gd::toWString( text ) ), text );
    QCOMPARE( static_cast< int >( gd::toWString( text ).size() ), 10 );
}

void TstUtf8Wstring::embeddedNul()
{
    // An embedded NUL is data, not a terminator: neither the encoders nor the
    // QString bridges may truncate it. (gd::toWString only drops *trailing*
    // NULs, the non-BMP padding bug workaround it documents.)
    const std::string withNul( "a\0b", 3 );
    const gd::wstring wide = Utf8::decode( withNul );

    QCOMPARE( static_cast< int >( wide.size() ), 3 );
    QCOMPARE( Utf8::encode( wide ), withNul );

    const QString text = QString::fromUtf8( withNul.data(), static_cast< int >( withNul.size() ) );
    QCOMPARE( gd::toQString( gd::toWString( text ) ), text );
}

void TstUtf8Wstring::supplementaryConversion()
{
    // U+1F600 GRINNING FACE: a supplementary-plane code point that needs four
    // UTF-8 bytes and a surrogate pair in QString's UTF-16 storage.
    const QString text = QString::fromUtf8( "A\xf0\x9f\x98\x80", 5 );
    QCOMPARE( gd::toQString( gd::toWString( text ) ), text );

    const gd::wstring wide = Utf8::decode( std::string( "\xf0\x9f\x98\x80", 4 ) );
    QCOMPARE( Utf8::encode( wide ), std::string( "\xf0\x9f\x98\x80", 4 ) );

    QCOMPARE( static_cast< int >( wide.size() ), 1 );
    QCOMPARE( static_cast< int >( wide[ 0 ] ), 0x1F600 );
    // 'A' plus one UTF-16 surrogate pair.
    QCOMPARE( text.size(), qsizetype( 3 ) );
}

void TstUtf8Wstring::invalidUtf8Throws()
{
    // 0xFF can never start a UTF-8 sequence, and a lone continuation byte is
    // equally invalid; both must surface as Utf8::exCantDecode instead of
    // silently producing replacement characters.
    QVERIFY_THROWS_EXCEPTION( Utf8::exCantDecode, Utf8::decode( std::string( "\xff\xff", 2 ) ) );
    QVERIFY_THROWS_EXCEPTION( Utf8::exCantDecode, Utf8::decode( std::string( "\x80", 1 ) ) );

    // The exception must carry the offending input so a caller can report it.
    try
    {
        Utf8::decode( std::string( "\xff\xff", 2 ) );
        QFAIL( "Utf8::decode accepted an invalid sequence" );
    }
    catch ( Utf8::exCantDecode const & error )
    {
        QVERIFY( QByteArray( error.what() ).contains( "Can't decode" ) );
    }
}

void TstUtf8Wstring::guardReportsUnavailableEnvironment()
{
    // The gate is the only thing standing between "environment unavailable"
    // and a silently skipped suite, so its falsification paths are asserted
    // here rather than trusted: each unmet requirement must contribute a
    // diagnostic reason.
    QVERIFY2( TestSupport::requiredEnvironment( nullptr ), "a null reason list must be accepted" );

    qputenv( "GD_TEST_FORCE_ENV_UNAVAILABLE", "1" );
    QStringList reasons;
    QVERIFY( !TestSupport::requiredEnvironment( &reasons ) );
    QVERIFY( reasons.join( '\n' ).contains( QLatin1String( "GD_TEST_FORCE_ENV_UNAVAILABLE" ) ) );
    qunsetenv( "GD_TEST_FORCE_ENV_UNAVAILABLE" );

    reasons.clear();
    qputenv( "GD_TEST_REQUIRED_FIXTURES", "no-such-fixture.bin" );
    QVERIFY( !TestSupport::requiredEnvironment( &reasons ) );
    QVERIFY( reasons.join( '\n' ).contains( QLatin1String( "no-such-fixture.bin" ) ) );
    qunsetenv( "GD_TEST_REQUIRED_FIXTURES" );

    reasons.clear();
    qputenv( "GD_TEST_REQUIRED_PREREQUISITES", "no-such-prerequisite" );
    QVERIFY( !TestSupport::requiredEnvironment( &reasons ) );
    QVERIFY( reasons.join( '\n' ).contains( QLatin1String( "no-such-prerequisite" ) ) );
    qunsetenv( "GD_TEST_REQUIRED_PREREQUISITES" );

    reasons.clear();
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
    QVERIFY( reasons.isEmpty() );
}

void TstUtf8Wstring::guardAcceptsAvailableFixture()
{
    QTemporaryDir fixtureDirectory;
    QVERIFY( fixtureDirectory.isValid() );

    const QString fixtureName = QStringLiteral( "sample-fixture.bin" );
    const QString fixtureFile = fixtureDirectory.filePath( fixtureName );

    {
        QFile file( fixtureFile );
        QVERIFY( file.open( QIODevice::WriteOnly ) );
        QCOMPARE( file.write( "fixture", 7 ), qint64( 7 ) );
    }

    qputenv( "GD_TEST_FIXTURES", fixtureDirectory.path().toUtf8() );
    qputenv( "GD_TEST_REQUIRED_FIXTURES", fixtureName.toUtf8() );

    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
    QCOMPARE( TestSupport::fixturePath( fixtureName ), fixtureFile );
    QVERIFY( TestSupport::fixtureExists( fixtureName ) );

    qunsetenv( "GD_TEST_REQUIRED_FIXTURES" );
    qunsetenv( "GD_TEST_FIXTURES" );
}

void TstUtf8Wstring::guardValidatesDisplayEndpoints()
{
    // A DISPLAY or WAYLAND_DISPLAY value on its own proves nothing: the
    // environment strings have to be followed by a bounded connect attempt.
    QString reason;
    QVERIFY2( !TestSupport::x11DisplayReachable( QStringLiteral( ":65000" ), &reason ),
              "an X11 display whose socket does not exist was reported reachable" );
    QVERIFY2( !reason.isEmpty(), "an unreachable X11 display must carry a diagnostic reason" );

    QTemporaryDir socketDirectory;
    QVERIFY( socketDirectory.isValid() );
    const QString absentWayland = socketDirectory.filePath( QStringLiteral( "gd-no-such-wayland-socket" ) );

    reason.clear();
    QVERIFY2( !TestSupport::waylandDisplayReachable( absentWayland, &reason ),
              "a Wayland display whose socket does not exist was reported reachable" );
    QVERIFY2( !reason.isEmpty(), "an unreachable Wayland display must carry a diagnostic reason" );

    // Reachable endpoints are accepted: a listening unix socket stands in for a
    // compositor, and a local TCP listener for an X server reached at
    // port 6000+display.
    QLocalServer compositor;
    const QString compositorSocket = socketDirectory.filePath( QStringLiteral( "gd-wayland-0" ) );
    QVERIFY2( compositor.listen( compositorSocket ), qPrintable( compositor.errorString() ) );

    reason.clear();
    QVERIFY2( TestSupport::waylandDisplayReachable( compositor.fullServerName(), &reason ),
              qPrintable( reason ) );
    QVERIFY( reason.isEmpty() );

    QTcpServer xServer;
    int displayNumber = 0;

    for ( int candidate = 4700; candidate < 4800 && !displayNumber; ++candidate )
    {
        if ( xServer.listen( QHostAddress::LocalHost, static_cast< quint16 >( 6000 + candidate ) ) )
            displayNumber = candidate;
    }

    QVERIFY2( displayNumber != 0, qPrintable( xServer.errorString() ) );

    const QString reachableX11 = QStringLiteral( "127.0.0.1:%1" ).arg( displayNumber );
    reason.clear();
    QVERIFY2( TestSupport::x11DisplayReachable( reachableX11, &reason ), qPrintable( reason ) );
    QVERIFY( reason.isEmpty() );

    // The gate itself must reject the reviewer's scenario: xcb is the selected
    // QPA, DISPLAY is set but dead, so a suite selecting x11 must fail instead of
    // running nine green tests.
    const EnvironmentValue savedDisplay( "DISPLAY" );
    const EnvironmentValue savedPlatform( "QT_QPA_PLATFORM" );
    const EnvironmentValue savedWayland( "WAYLAND_DISPLAY" );
    const EnvironmentValue savedPrerequisites( "GD_TEST_REQUIRED_PREREQUISITES" );

    qunsetenv( "WAYLAND_DISPLAY" );
    qputenv( "QT_QPA_PLATFORM", "xcb" );
    qputenv( "DISPLAY", ":65000" );
    qputenv( "GD_TEST_REQUIRED_PREREQUISITES", "x11" );

    QStringList reasons;
    QVERIFY2( !TestSupport::requiredEnvironment( &reasons ),
              "a required x11 prerequisite passed although DISPLAY is unreachable" );
    QVERIFY2( reasons.join( '\n' ).contains( QLatin1String( ":65000" ) ),
              qPrintable( reasons.join( '\n' ) ) );

    // Same for a Wayland socket that does not exist.
    qunsetenv( "DISPLAY" );
    qputenv( "QT_QPA_PLATFORM", "wayland" );
    qputenv( "WAYLAND_DISPLAY", absentWayland.toUtf8() );
    qputenv( "GD_TEST_REQUIRED_PREREQUISITES", "wayland" );

    reasons.clear();
    QVERIFY2( !TestSupport::requiredEnvironment( &reasons ),
              "a required wayland prerequisite passed although the socket is absent" );
    QVERIFY2( reasons.join( '\n' ).contains( absentWayland ),
              qPrintable( reasons.join( '\n' ) ) );

    // A usable endpoint is accepted, and the explicitly selected QPA is never
    // overridden: the reachable X server is honoured through xcb ...
    qputenv( "QT_QPA_PLATFORM", "xcb" );
    qputenv( "DISPLAY", reachableX11.toUtf8() );
    qputenv( "GD_TEST_REQUIRED_PREREQUISITES", "x11" );

    reasons.clear();
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
    QCOMPARE( qEnvironmentVariable( "QT_QPA_PLATFORM" ), QStringLiteral( "xcb" ) );

    // ... a headless QPA needs no display at all ...
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
    qunsetenv( "DISPLAY" );
    qputenv( "GD_TEST_REQUIRED_PREREQUISITES", "display" );

    reasons.clear();
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );

    // ... and an explicitly selected non-xcb QPA is preserved, so an x11
    // prerequisite fails even while a reachable X server exists.
    qputenv( "DISPLAY", reachableX11.toUtf8() );
    qputenv( "GD_TEST_REQUIRED_PREREQUISITES", "x11" );

    reasons.clear();
    QVERIFY2( !TestSupport::requiredEnvironment( &reasons ),
              "an x11 prerequisite passed although the QPA was forced to offscreen" );
    QVERIFY2( reasons.join( '\n' ).contains( QLatin1String( "offscreen" ) ),
              qPrintable( reasons.join( '\n' ) ) );
    QCOMPARE( qEnvironmentVariable( "QT_QPA_PLATFORM" ), QStringLiteral( "offscreen" ) );

    savedPrerequisites.restore();
    savedWayland.restore();
    savedPlatform.restore();
    savedDisplay.restore();
}

void TstUtf8Wstring::configurationProbeMirrorsApplicationFeatures()
{
    // The shared support has to mirror goldendict.pro's feature configuration,
    // otherwise a guarded production implementation compiles to an empty
    // translation unit while every build step still reports success. The
    // GD_TEST_CONFIG_* markers record the CONFIG decision; the feature macros
    // must follow it.
    const QStringList features = TestSupport::compiledFeatureDefines();
    const QString compiled = features.isEmpty() ? QStringLiteral( "(none)" )
                                               : features.join( QStringLiteral( ", " ) );

    QStringList problems;
    const auto check = [ &features, &problems ]( char const * name, bool expected ) {
        const bool defined = features.contains( QString::fromLatin1( name ) );

        if ( defined != expected )
        {
            problems << QStringLiteral( "%1 %2" )
                            .arg( QString::fromLatin1( name ),
                                  expected ? QStringLiteral( "must be defined by the mirrored application "
                                                             "configuration but is missing" )
                                           : QStringLiteral( "must not be defined in this configuration "
                                                             "but is" ) );
        }
    };

#if defined( GD_TEST_CONFIG_HAVE_X11 )
    check( "HAVE_X11", true );
#else
    check( "HAVE_X11", false );
#endif

#if defined( GD_TEST_CONFIG_FFMPEG_PLAYER )
    check( "MAKE_FFMPEG_PLAYER", true );
#else
    check( "MAKE_FFMPEG_PLAYER", false );
#endif

#if defined( GD_TEST_CONFIG_EXTRA_TIFF_HANDLER )
    const bool extraTiffHandler = true;
#else
    const bool extraTiffHandler = false;
#endif
    check( "MAKE_EXTRA_TIFF_HANDLER", extraTiffHandler );

#if defined( GD_TEST_CONFIG_ZIM_SUPPORT )
    check( "MAKE_ZIM_SUPPORT", true );
#else
    check( "MAKE_ZIM_SUPPORT", false );
#endif

#if defined( GD_TEST_CONFIG_NO_EPWING_SUPPORT )
    check( "NO_EPWING_SUPPORT", true );
#else
    check( "NO_EPWING_SUPPORT", false );
#endif

#if defined( GD_TEST_CONFIG_CHINESE_CONVERSION_SUPPORT )
    check( "MAKE_CHINESE_CONVERSION_SUPPORT", true );
#else
    check( "MAKE_CHINESE_CONVERSION_SUPPORT", false );
#endif

#if defined( GD_TEST_CONFIG_OLD_HUNSPELL )
    check( "OLD_HUNSPELL_INTERFACE", true );
#else
    check( "OLD_HUNSPELL_INTERFACE", false );
#endif

    QVERIFY2( problems.isEmpty(),
              qPrintable( QStringLiteral( "%1\nfeatures this binary was compiled with: %2" )
                              .arg( problems.join( '\n' ), compiled ) ) );

#if defined( GD_TEST_CONFIG_EXTRA_TIFF_HANDLER )
    // The default TIFF handler is enabled, so its guarded implementation must
    // really be part of the binary: this reference does not link when tiff.cc
    // was compiled without MAKE_EXTRA_TIFF_HANDLER, and it does not return for a
    // non-TIFF buffer.
    QImage image;
    QVERIFY2( !GdTiff::tiffToQImage( "this is not a TIFF image", 24, image ),
              "GdTiff::tiffToQImage accepted a non-TIFF buffer" );
    QVERIFY( image.isNull() );
#else
    // With the switch configured off the guarded symbol is absent by design; the
    // empty translation unit above is the expected (and respected) result.
    QVERIFY( !extraTiffHandler );
#endif
}

void TstUtf8Wstring::watchdogCanBeRearmedAndDisarmed()
{
    // The expiry path terminates the process and therefore cannot be observed
    // in-process; the lifecycle must still be safe (no deadlock, no joinable
    // thread left at exit) because every suite calls it around its own run.
    TestSupport::installWatchdog( TestSupport::BrowserWatchdogSeconds );
    TestSupport::clearWatchdog();
    TestSupport::clearWatchdog();
    TestSupport::installWatchdog( 0 );
    TestSupport::clearWatchdog();

    TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );

    QVERIFY( true );
}

void TstUtf8Wstring::watchdogTerminatesStalledProcess()
{
    // The watchdog can only be proven by a process that is actually stuck, so
    // this test re-runs the suite binary as a stalled subprocess (see main() and
    // TestSupport::runWatchdogProbe) and checks its exit code and elapsed bound.
    // The probe variants include a held stdio lock and a full, unread stderr
    // pipe, which used to keep the expiry path alive forever.
    QStringList reasons;
    QVERIFY2( TestSupport::verifyWatchdogExpiry( QCoreApplication::applicationFilePath(), &reasons ),
              qPrintable( reasons.join( '\n' ) ) );
}

void TstUtf8Wstring::watchdogProbeIsOnlyActiveOnRequest()
{
    // The probe mode must never leak into an ordinary run: only the environment
    // variable the parent sets selects it.
    QVERIFY( !TestSupport::isWatchdogProbeProcess() );
}

int main( int argc, char *argv[] )
{
    // This is QTEST_GUILESS_MAIN spelled out: the watchdog expiry test re-runs
    // this very binary as a stalled subprocess, so the probe mode has to be
    // decided before any Qt test machinery exists.
    if ( TestSupport::isWatchdogProbeProcess() )
        TestSupport::runWatchdogProbe();

    QCoreApplication app( argc, argv );
    TstUtf8Wstring testCase;

    return QTest::qExec( &testCase, argc, argv );
}

#include "tst_utf8_wstring.moc"
