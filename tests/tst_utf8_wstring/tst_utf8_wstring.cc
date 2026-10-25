/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Baseline conversion suite: the UTF-8 <-> wide-string helpers (utf8.cc,
 * wstring.cc) and the QString bridges (wstring_qt.cc) that the dictionary
 * backends rely on. These are consumed read-only; the suite only asserts the
 * behaviour that already exists, including the supplementary-plane and
 * embedded-NUL corners the Qt 6 migration must not regress.
 */

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <string>

#include "support/fixtures.hh"

#include "utf8.hh"
#include "wstring.hh"
#include "wstring_qt.hh"

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
    void watchdogCanBeRearmedAndDisarmed();
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

QTEST_GUILESS_MAIN( TstUtf8Wstring )

#include "tst_utf8_wstring.moc"
