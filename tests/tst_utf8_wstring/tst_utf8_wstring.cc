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

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QAuthenticator>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QSize>
#include <QTcpServer>
#include <QTemporaryDir>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QThread>

#include "support/dictionary_stub.hh"
#include "support/fake_portal.hh"
#include "support/fixtures.hh"
#include "support/http_fixture.hh"

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

/// The request path element a client derives from its own unique bus name.
QString senderElement( QString const & uniqueName )
{
    QString element = uniqueName;

    if ( element.startsWith( QLatin1Char( ':' ) ) )
        element = element.mid( 1 );

    element.replace( QLatin1Char( '.' ), QLatin1Char( '_' ) );
    element.replace( QLatin1Char( '-' ), QLatin1Char( '_' ) );

    return element;
}

QString predictedRequestPath( QString const & uniqueName, QString const & token )
{
    return TestSupport::FakePortal::portalPath() + QStringLiteral( "/request/" )
        + senderElement( uniqueName ) + QLatin1Char( '/' ) + token;
}

QString predictedSessionPath( QString const & uniqueName, QString const & token )
{
    return TestSupport::FakePortal::portalPath() + QStringLiteral( "/session/" )
        + senderElement( uniqueName ) + QLatin1Char( '/' ) + token;
}

QDBusMessage portalMethodCall( QString const & member )
{
    return QDBusMessage::createMethodCall( TestSupport::FakePortal::serviceName(),
                                           TestSupport::FakePortal::portalPath(),
                                           TestSupport::FakePortal::portalInterface(), member );
}

QDBusMessage createSessionCall( QString const & handleToken, QString const & sessionToken )
{
    QDBusMessage call = portalMethodCall( QStringLiteral( "CreateSession" ) );
    call << QVariantMap{ { QStringLiteral( "handle_token" ), handleToken },
                         { QStringLiteral( "session_handle_token" ), sessionToken } };
    return call;
}

QDBusMessage listShortcutsCall( QString const & session, QString const & handleToken )
{
    QDBusMessage call = portalMethodCall( QStringLiteral( "ListShortcuts" ) );
    call << QVariant::fromValue( QDBusObjectPath( session ) )
         << QVariantMap{ { QStringLiteral( "handle_token" ), handleToken } };
    return call;
}

QVector< TestSupport::FakePortal::Shortcut > makeShortcuts( QStringList const & ids )
{
    QVector< TestSupport::FakePortal::Shortcut > entries;

    for ( QString const & id : ids )
    {
        TestSupport::FakePortal::Shortcut entry;
        entry.id = id;
        entry.properties.insert( QStringLiteral( "description" ), QStringLiteral( "fixture " ) + id );
        entry.properties.insert( QStringLiteral( "preferred_trigger" ), QStringLiteral( "CTRL+ALT+" ) + id );
        entries.append( entry );
    }

    return entries;
}

QVariant shortcutListValue( QVector< TestSupport::FakePortal::Shortcut > const & entries )
{
    QList< TestSupport::FakePortal::Shortcut > list;

    for ( auto const & entry : entries )
        list.append( entry );

    return QVariant::fromValue( list );
}

QDBusMessage bindShortcutsCall( QString const & session, QStringList const & ids,
                                QString const & handleToken )
{
    QDBusMessage call = portalMethodCall( QStringLiteral( "BindShortcuts" ) );
    call << QVariant::fromValue( QDBusObjectPath( session ) ) << shortcutListValue( makeShortcuts( ids ) )
         << QString() << QVariantMap{ { QStringLiteral( "handle_token" ), handleToken } };
    return call;
}

QDBusMessage configureShortcutsCall( QString const & session, QString const & activationToken )
{
    QDBusMessage call = portalMethodCall( QStringLiteral( "ConfigureShortcuts" ) );
    QVariantMap options;

    if ( !activationToken.isEmpty() )
        options.insert( QStringLiteral( "activation_token" ), activationToken );

    call << QVariant::fromValue( QDBusObjectPath( session ) ) << QString() << options;
    return call;
}

QDBusMessage closeCall( QString const & path, QString const & interface )
{
    return QDBusMessage::createMethodCall( TestSupport::FakePortal::serviceName(), path, interface,
                                           QStringLiteral( "Close" ) );
}

/// Receives every portal signal. The subscriptions use an empty path match, just
/// like the production request does before it knows its request path.
class PortalSignalReceiver : public QObject
{
    Q_OBJECT

public:
    struct Entry
    {
        QString member;
        QString path;
        QString signature;
        QList< QVariant > arguments;
    };

    QStringList events;
    QVector< Entry > entries;

public slots:
    void handleSignal( QDBusMessage const & message )
    {
        Entry entry;
        entry.member = message.member();
        entry.path = message.path();
        entry.signature = message.signature();
        entry.arguments = message.arguments();
        entries.append( entry );
        events.append( message.member() );
    }
};

void subscribePortalSignals( QDBusConnection bus, PortalSignalReceiver * receiver )
{
    const QString service = TestSupport::FakePortal::serviceName();

    bus.connect( service, QString(), TestSupport::FakePortal::requestInterface(),
                 QStringLiteral( "Response" ), receiver, SLOT( handleSignal( QDBusMessage ) ) );
    bus.connect( service, QString(), TestSupport::FakePortal::sessionInterface(),
                 QStringLiteral( "Closed" ), receiver, SLOT( handleSignal( QDBusMessage ) ) );

    for ( char const * member : { "Activated", "Deactivated", "ShortcutsChanged" } )
    {
        bus.connect( service, QString(), TestSupport::FakePortal::portalInterface(),
                     QString::fromLatin1( member ), receiver, SLOT( handleSignal( QDBusMessage ) ) );
    }
}

/// A single portal method call, driven asynchronously so the fixture's own
/// connection (same thread, same process) keeps dispatching while the test
/// waits. A blocking QDBus::Block call would stall the thread that has to answer
/// it and time out instead.
QDBusMessage callWithEvents( QDBusConnection bus, QDBusMessage const & message,
                             int timeoutMs = 5000 )
{
    QDBusPendingCallWatcher watcher( bus.asyncCall( message, timeoutMs ) );
    const bool finished = QTest::qWaitFor( [ &watcher ]() { return watcher.isFinished(); },
                                          timeoutMs );
    Q_UNUSED( finished )

    return watcher.reply();
}

/// One portal exchange: the method reply plus every portal signal that arrived
/// while it was outstanding, in arrival order.
struct PortalCall
{
    QStringList events;
    QDBusMessage reply;
    QVector< PortalSignalReceiver::Entry > received;
};

PortalCall callPortal( QDBusConnection const & bus, PortalSignalReceiver * receiver,
                       QDBusMessage const & call, int expectedEvents, int timeoutMs = 5000 )
{
    receiver->events.clear();
    receiver->entries.clear();

    PortalCall outcome;

    QDBusPendingCallWatcher watcher( bus.asyncCall( call, timeoutMs ) );

    QObject::connect( &watcher, &QDBusPendingCallWatcher::finished,
                      [ &outcome, receiver ]( QDBusPendingCallWatcher * finished ) {
                          outcome.reply = finished->reply();
                          receiver->events.append( QStringLiteral( "reply" ) );
                      } );

    const bool complete = QTest::qWaitFor( [ receiver, expectedEvents ]() {
        return receiver->events.size() >= expectedEvents;
    }, timeoutMs );
    Q_UNUSED( complete )

    outcome.events = receiver->events;
    outcome.received = receiver->entries;

    return outcome;
}

QString replyPath( QDBusMessage const & reply )
{
    if ( reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty() )
        return QString();

    return reply.arguments().value( 0 ).value< QDBusObjectPath >().path();
}

/// Reads one `a{sv}` argument that arrived on the wire (a QDBusArgument, not a
/// QVariantMap: QVariant::toMap() would silently produce an empty map).
QVariantMap argumentMap( QVariant const & value )
{
    if ( value.metaType() == QMetaType::fromType< QDBusArgument >() )
        return qdbus_cast< QVariantMap >( value );

    return value.toMap();
}

/// The result map of one Request.Response envelope.
QVariantMap resultMap( PortalSignalReceiver::Entry const & entry, int index = 1 )
{
    return argumentMap( entry.arguments.value( index ) );
}

/// Reads a bare `a(sa{sv})` argument (the ShortcutsChanged payload) into ids.
QStringList shortcutIdsOfList( QVariant const & value )
{
    QStringList ids;

    if ( value.metaType() != QMetaType::fromType< QDBusArgument >() )
        return ids;

    const QList< TestSupport::FakePortal::Shortcut > entries =
        qdbus_cast< QList< TestSupport::FakePortal::Shortcut > >( value );

    for ( auto const & entry : entries )
        ids.append( entry.id );

    return ids;
}

/// Reads the `shortcuts` member of a Request.Response result map into ids.
QStringList shortcutIdsOf( QVariantMap const & results )
{
    return shortcutIdsOfList( results.value( QStringLiteral( "shortcuts" ) ) );
}

QByteArray bytesOf( std::vector< char > const & data )
{
    return data.empty() ? QByteArray()
                        : QByteArray( data.data(), static_cast< qsizetype >( data.size() ) );
}

/// One HTTP response, fully read: status, payload, headers and final URL.
struct HttpFetch
{
    int status = 0;
    QByteArray body;
    QUrl url;
    QByteArray contentType;
    QHash< QByteArray, QByteArray > headers;
    bool ok = false;
    QString error;

    QByteArray header( char const * name ) const
    {
        return headers.value( QByteArray( name ).toLower() );
    }
};

HttpFetch httpGet( QNetworkAccessManager & manager, QUrl const & url,
                   QNetworkRequest::RedirectPolicy policy =
                       QNetworkRequest::NoLessSafeRedirectPolicy,
                   int timeoutMs = 5000 )
{
    HttpFetch fetch;

    QNetworkRequest request( url );
    request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, policy );

    QNetworkReply * reply = manager.get( request );

    if ( !QTest::qWaitFor( [ reply ]() { return reply->isFinished(); }, timeoutMs ) )
    {
        fetch.error = QStringLiteral( "the fixture did not answer %1 within %2 ms" )
                          .arg( url.toString() )
                          .arg( timeoutMs );
        reply->abort();
        reply->deleteLater();
        return fetch;
    }

    fetch.ok = reply->error() == QNetworkReply::NoError;
    fetch.error = reply->errorString();
    fetch.status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    fetch.url = reply->url();
    fetch.contentType = reply->header( QNetworkRequest::ContentTypeHeader ).toByteArray();

    for ( auto const & pair : reply->rawHeaderPairs() )
        fetch.headers.insert( pair.first.toLower(), pair.second );

    fetch.body = reply->readAll();
    reply->deleteLater();

    return fetch;
}

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

    // Shared fixture self-tests (tests/support): the deterministic dictionary
    // readers, the loopback HTTP origins and the private portal bus. They live
    // here because this suite is the cheap, dependency-free home of the harness
    // self-checks; no nineteenth suite is added for them.
    void fixtureFilesHoldTheirFixedOracles();
    void fixtureDictionaryProducesDeterministicArticles();
    void controlledRequestObeysDataDiscipline();
    void controlledRequestCompletesInstantly();
    void readerForwardsThroughTheProductionDelegate();
    void readerThrowsOnceWithTheArmedMessage();
    void readerKeepsNoProducer();
    void factoryFallsBackToTheProductionReader();
    void httpOriginsBindOnEveryLoopbackAddress();
    void httpBindFailureIsARequiredFailure();
    void httpServesFixedContentRedirectsAndHost();
    void httpBarrierHoldsUntilReleased();
    void httpProxyChallengesAndForwards();
    void httpProxyTunnelsAConnectRequest();
    void printFixtureHoldsTwoFixedPages();
    void privateBusIsIsolatedFromTheDesktopBus();
    void missingPrivateBusIsARequiredFailure();
    void fakePortalServesThePortalWireInterface();
    void fakePortalEarlyResponsePrecedesTheMethodReply();
    void fakePortalRefusesASecondBind();
    void fakePortalCoversMalformedPayloads();
    void fakePortalCoversResponseAndLossScenarios();
};

void TstUtf8Wstring::initTestCase()
{
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );

    // Every fixture file the browser/print/portal suites assert against has to be
    // readable here. A missing one is a required failure naming its path; a
    // skipped fixture test would be a lie about coverage.
    const QStringList requiredFixtures = {
        QStringLiteral( "article/article.html" ),
        QStringLiteral( "article/pixel.png" ),
        QStringLiteral( "article/style.css" ),
        QStringLiteral( "article/dom.html" ),
        QStringLiteral( "website/first.html" ),
        QStringLiteral( "website/second.html" ),
        QStringLiteral( "website/nested.html" ),
        QStringLiteral( "website/style.css" ),
        QStringLiteral( "website/relative.js" ),
        QStringLiteral( "website/data.json" ),
        QStringLiteral( "print/two-pages.html" )
    };

    QStringList missingFixtures;

    for ( QString const & fixture : requiredFixtures )
    {
        if ( !TestSupport::fixtureExists( fixture ) )
            missingFixtures << TestSupport::fixturePath( fixture );
    }

    QVERIFY2( missingFixtures.isEmpty(),
              qPrintable( QStringLiteral( "missing fixture files:\n" ) + missingFixtures.join( '\n' ) ) );

    // The three loopback origins the browser fixtures bind are a hard
    // prerequisite: an unbound origin must fail this suite with a reason.
    reasons.clear();
    QVERIFY2( TestSupport::loopbackOriginsBindable( &reasons ), qPrintable( reasons.join( '\n' ) ) );

    // So is a real private D-Bus daemon: the portal fixture never falls back to
    // the user's desktop bus.
    QString busFailure;
    QVERIFY2( TestSupport::prerequisiteAvailable( QStringLiteral( "dbus" ), &busFailure ),
              qPrintable( busFailure ) );

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

////////// Shared fixture self-tests

void TstUtf8Wstring::fixtureFilesHoldTheirFixedOracles()
{
    const auto text = []( QString const & path ) {
        return QString::fromUtf8( TestSupport::HttpFixture::fixtureBytes( path ) );
    };

    // article/article.html: the markers the article suites address by name.
    const QString article = text( QStringLiteral( "article/article.html" ) );
    QVERIFY( !article.isEmpty() );

    for ( char const * marker : { "fixture-headword", "gdfrom-dict-a", "gdfrom-dict-b",
                                 "id=\"early-active-callback\"", "id=\"late-active-callback\"",
                                 "bres://dict-a/", "gdau://dict-a/", "class=\"dsl_opt\"",
                                 "id=\"long-text\"" } )
    {
        QVERIFY2( article.contains( QLatin1String( marker ) ), marker );
    }

    // The early callback really is parser-time (its marker precedes the article
    // body) and the late one is delayed by a timer.
    QVERIFY( article.indexOf( QLatin1String( "id=\"early-active-callback\"" ) )
             < article.indexOf( QLatin1String( "id=\"gdfrom-dict-a\"" ) ) );
    QVERIFY( article.contains( QLatin1String( "window.setTimeout" ) ) );

    // article/style.css: one unique marker plus a relative image rule.
    const QString articleStyle = text( QStringLiteral( "article/style.css" ) );
    QVERIFY( articleStyle.contains( QLatin1String( ".gd-fixture-marker" ) ) );
    QVERIFY( articleStyle.contains( QLatin1String( "url(pixel.png)" ) ) );

    // article/dom.html: every script the bridge has to round-trip.
    const QString dom = text( QStringLiteral( "article/dom.html" ) );

    for ( char const * id : { "word-latin", "word-cyrillic", "word-cjk", "word-combining",
                              "word-apostrophe", "word-surrogate", "context-target",
                              "highlight-target", "zoom-target", "selection-target" } )
    {
        QVERIFY2( dom.contains( QLatin1String( id ) ), id );
    }

    QVERIFY( dom.contains( QStringLiteral( "\u041f\u0440\u0438\u0432\u0435\u0442" ) ) );
    QVERIFY( dom.contains( QStringLiteral( "\u4e2d\u6587\u8bcd\u6761" ) ) );
    QVERIFY( dom.contains( QStringLiteral( "e\u0301" ) ) );            // combining acute
    QVERIFY( dom.contains( QStringLiteral( "rock\u2019n" ) ) );        // typographic apostrophe
    QVERIFY( dom.contains( QStringLiteral( "\U0001F600" ) ) );         // surrogate pair

    // article/pixel.png: a genuinely valid 1x1 image, not just a named file.
    const QByteArray pixel = TestSupport::HttpFixture::pixelPng();
    QVERIFY( !pixel.isEmpty() );
    QCOMPARE( pixel.left( 8 ), QByteArray( "\x89PNG\r\n\x1a\n", 8 ) );

    QImage image = QImage::fromData( pixel, "PNG" );
    QVERIFY2( !image.isNull(), "the fixture PNG does not decode" );
    QCOMPARE( image.size(), QSize( 1, 1 ) );
    QCOMPARE( image.pixelColor( 0, 0 ), QColor( 0xC8, 0x18, 0x18, 0xFF ) );

    // website/: the two page documents differ in their measured height, both use
    // relative resources and both expose the fetch marker filled by relative.js.
    const QString first = text( QStringLiteral( "website/first.html" ) );
    const QString second = text( QStringLiteral( "website/second.html" ) );

    QVERIFY( first.contains( QLatin1String( "id=\"first-marker\"" ) ) );
    QVERIFY( first.contains( QLatin1String( "height:120px" ) ) );
    QVERIFY( first.contains( QLatin1String( "id=\"click-target\"" ) ) );
    QVERIFY( first.contains( QLatin1String( "id=\"context-target\"" ) ) );
    QVERIFY( first.contains( QLatin1String( "id=\"word-target\"" ) ) );
    QVERIFY( first.contains( QLatin1String( "href=\"style.css\"" ) ) );
    QVERIFY( first.contains( QLatin1String( "src=\"pixel.png\"" ) ) );
    QVERIFY( first.contains( QLatin1String( "src=\"relative.js\"" ) ) );
    QVERIFY( first.contains( QLatin1String( "method=\"get\"" ) ) );
    QVERIFY( first.contains( QLatin1String( "action=\"second.html\"" ) ) );

    QVERIFY( second.contains( QLatin1String( "id=\"second-marker\"" ) ) );
    QVERIFY( second.contains( QLatin1String( "height:240px" ) ) );
    QVERIFY( second.contains( QLatin1String( "href=\"style.css\"" ) ) );
    QVERIFY( second.contains( QLatin1String( "src=\"pixel.png\"" ) ) );

    const QString nested = text( QStringLiteral( "website/nested.html" ) );
    QVERIFY( nested.contains( QLatin1String( "id=\"nested-frame\"" ) ) );
    QVERIFY( nested.contains( QLatin1String( "src=\"second.html\"" ) ) );
    QVERIFY( nested.contains( QLatin1String( "margin:10px" ) ) );
    QVERIFY( nested.contains( QLatin1String( "border:3px" ) ) );
    QVERIFY( nested.contains( QLatin1String( "padding:4px" ) ) );
    QVERIFY( nested.contains( QLatin1String( "id=\"nested-word-target\"" ) ) );

    const QString websiteStyle = text( QStringLiteral( "website/style.css" ) );
    QVERIFY( websiteStyle.contains( QLatin1String( ".website-marker" ) ) );
    QVERIFY( websiteStyle.contains( QLatin1String( "url(pixel.png)" ) ) );

    const QString script = text( QStringLiteral( "website/relative.js" ) );
    QVERIFY( script.contains( QLatin1String( "fetch('./data.json')" ) ) );
    QVERIFY( script.contains( QLatin1String( "data-origin-fetch" ) ) );

    QJsonParseError parseError;
    const QJsonDocument data =
        QJsonDocument::fromJson( TestSupport::HttpFixture::fixtureBytes( QStringLiteral( "website/data.json" ) ),
                                 &parseError );
    QCOMPARE( parseError.error, QJsonParseError::NoError );
    QCOMPARE( data.object().value( QStringLiteral( "marker" ) ).toString(),
              QStringLiteral( "real-origin-fetch" ) );
}

void TstUtf8Wstring::fixtureDictionaryProducesDeterministicArticles()
{
    TestSupport::FixtureDictionary dictionary( std::string( "dict-a" ),
                                              QStringLiteral( "Fixture Dictionary A" ) );

    QCOMPARE( QString::fromStdString( dictionary.getId() ), QStringLiteral( "dict-a" ) );
    QCOMPARE( QString::fromStdString( dictionary.getName() ), QStringLiteral( "Fixture Dictionary A" ) );
    QCOMPARE( dictionary.getArticleCount(), 1uL );
    QCOMPARE( dictionary.getWordCount(), 1uL );
    QVERIFY( dictionary.isLocalDictionary() == false );

    const QByteArray body = "<p id=\"fixture-article\">fixture-headword</p>";
    dictionary.setArticleBody( body );

    const gd::wstring word = Utf8::decode( std::string( "fixture-headword" ) );
    const gd::wstring context = Utf8::decode( std::string( "gdfrom-dict-a" ) );
    const std::vector< gd::wstring > alternates = { Utf8::decode( std::string( "alternate" ) ) };

    sptr< Dictionary::DataRequest > article = dictionary.getArticle( word, alternates, context, true );

    QVERIFY( article );
    QCOMPARE( dictionary.articleRequestCount(), 1 );

    const TestSupport::FixtureDictionary::Lookup lookup = dictionary.lastLookup();
    QVERIFY( lookup.word == word );
    QVERIFY( lookup.context == context );
    QCOMPARE( static_cast< int >( lookup.alternates.size() ), 1 );
    QVERIFY( lookup.alternates.front() == alternates.front() );
    QVERIFY( lookup.ignoreDiacritics );

    QVERIFY( article->isFinished() );
    QCOMPARE( article->dataSize(), static_cast< long >( body.size() ) );
    QCOMPARE( bytesOf( article->getFullData() ), body );
    QVERIFY( article->getErrorString().isEmpty() );

    // The fixture hands back the very request a test can keep driving.
    QCOMPARE( dictionary.lastArticleRequest().get(),
              dynamic_cast< TestSupport::ControlledDataRequest * >( article.get() ) );

    // Deferred mode: the test produces the bytes itself.
    dictionary.setArticlePending( true );
    sptr< Dictionary::DataRequest > pending = dictionary.getArticle( word, {}, {}, false );
    QCOMPARE( pending->dataSize(), -1L );
    QVERIFY( !pending->isFinished() );
    QVERIFY( dictionary.lastArticleRequest() );

    dictionary.lastArticleRequest()->append( QByteArray( "abc" ) );
    QCOMPARE( pending->dataSize(), 3L );
    QVERIFY( !pending->isFinished() );
    dictionary.lastArticleRequest()->finishSuccess();
    QVERIFY( pending->isFinished() );
    QCOMPARE( bytesOf( pending->getFullData() ), QByteArray( "abc" ) );

    // Resources: a configured resource succeeds, a missing one is a finished
    // not-found with no error string.
    dictionary.setArticlePending( false );
    const QByteArray resource = TestSupport::HttpFixture::pixelPng();
    dictionary.setResources( { { QStringLiteral( "pixel.png" ), resource } } );

    sptr< Dictionary::DataRequest > found = dictionary.getResource( std::string( "pixel.png" ) );
    QVERIFY( found->isFinished() );
    QCOMPARE( bytesOf( found->getFullData() ), resource );

    sptr< Dictionary::DataRequest > missing = dictionary.getResource( std::string( "missing.png" ) );
    QVERIFY( missing->isFinished() );
    QCOMPARE( missing->dataSize(), -1L );
    QVERIFY( missing->getErrorString().isEmpty() );
    QCOMPARE( dictionary.requestedResources(),
              QStringList( { QStringLiteral( "pixel.png" ), QStringLiteral( "missing.png" ) } ) );

    // Prefix matching stays instantaneous and empty: this fixture is not a word
    // index, and a suite must not believe otherwise.
    sptr< Dictionary::WordSearchRequest > prefix = dictionary.prefixMatch( word, 10 );
    QVERIFY( prefix );
    QVERIFY( prefix->isFinished() );
    QCOMPARE( prefix->matchesCount(), size_t( 0 ) );
    sptr< Dictionary::DataRequest > search =
        dictionary.getSearchResults( QString(), 0, false, 0, 0, false, false, nullptr );
    QVERIFY( search->isFinished() );
    QCOMPARE( search->dataSize(), -1L );
    QCOMPARE( dictionary.searchRequestCount(), 1 );
}

void TstUtf8Wstring::controlledRequestObeysDataDiscipline()
{
    // The request is a QObject the production handler connects to, so it is
    // owned by an sptr exactly like a real producer.
    sptr< TestSupport::ControlledDataRequest > request = new TestSupport::ControlledDataRequest();

    // Unknown existence: dataSize() is -1 until the producer publishes bytes.
    QCOMPARE( request->dataSize(), -1L );
    QCOMPARE( request->completion(), TestSupport::ControlledDataRequest::Completion::Pending );
    QVERIFY( !request->isFinished() );
    QCOMPARE( request->terminalEmissions(), 0 );

    char slice[ 3 ] = {};

    // Without any data the base getter refuses a slice instead of handing out
    // uninitialised bytes.
    QVERIFY_THROWS_EXCEPTION( Dictionary::exSliceOutOfRange,
                              request->getDataSlice( 0, 1, slice ) );

    request->append( QByteArray( "AB" ) );
    QCOMPARE( request->dataSize(), 2L );
    QCOMPARE( request->producedBytes(), QByteArray( "AB" ) );
    request->getDataSlice( 0, 2, slice );
    QCOMPARE( QByteArray( slice, 2 ), QByteArray( "AB" ) );

    // Out-of-range slices keep throwing from the production base method.
    QVERIFY_THROWS_EXCEPTION( Dictionary::exSliceOutOfRange,
                              request->getDataSlice( 1, 2, slice ) );

    // A second append extends the produced data in order.
    request->append( QByteArray( "CDE" ) );
    QCOMPARE( request->dataSize(), 5L );
    QCOMPARE( request->producedBytes(), QByteArray( "ABCDE" ) );

    request->finishSuccess();
    QVERIFY( request->isFinished() );
    QCOMPARE( request->terminalEmissions(), 1 );
    QCOMPARE( request->completion(), TestSupport::ControlledDataRequest::Completion::Success );
    QCOMPARE( bytesOf( request->getFullData() ), QByteArray( "ABCDE" ) );

    // Terminal transitions are idempotent and a finished request cannot be
    // resurrected by a late producer callback.
    request->finishSuccess();
    request->finishNotFound();
    request->finishError( QStringLiteral( "late" ) );
    request->append( QByteArray( "F" ) );
    QCOMPARE( request->terminalEmissions(), 1 );
    QCOMPARE( request->dataSize(), 5L );
    QVERIFY( request->getErrorString().isEmpty() );

    // Not-found: finished, still no size, and not an error.
    sptr< TestSupport::ControlledDataRequest > notFound = new TestSupport::ControlledDataRequest();
    notFound->finishNotFound();
    QVERIFY( notFound->isFinished() );
    QCOMPARE( notFound->dataSize(), -1L );
    QVERIFY( notFound->getErrorString().isEmpty() );
    QVERIFY( notFound->getFullData().empty() );

    // Producer error: the error string is the terminal result, and it wins over
    // whatever partial bytes were produced.
    sptr< TestSupport::ControlledDataRequest > failed = new TestSupport::ControlledDataRequest();
    failed->append( QByteArray( "partial" ) );
    failed->finishError( QStringLiteral( "producer exploded" ) );
    QVERIFY( failed->isFinished() );
    QCOMPARE( failed->getErrorString(), QStringLiteral( "producer exploded" ) );
    QCOMPARE( failed->dataSize(), 7L );

    // Cancellation finishes the request (the Request contract) and is counted.
    sptr< TestSupport::ControlledDataRequest > cancelled = new TestSupport::ControlledDataRequest();
    cancelled->cancel();
    QVERIFY( cancelled->isFinished() );
    QCOMPARE( cancelled->cancelCount(), 1 );
    QCOMPARE( cancelled->terminalEmissions(), 1 );
    cancelled->cancel();
    QCOMPARE( cancelled->cancelCount(), 2 );
    QCOMPARE( cancelled->terminalEmissions(), 1 );

    // The nonvirtual base getters are used unchanged: if the fixture ever hid
    // one behind its own declaration these member-pointer types would differ.
    static_assert( std::is_same< decltype( &TestSupport::ControlledDataRequest::dataSize ),
                                 long ( Dictionary::DataRequest::* )() >::value,
                   "ControlledDataRequest must not hide DataRequest::dataSize" );
    static_assert( std::is_same< decltype( &TestSupport::ControlledDataRequest::getDataSlice ),
                                 void ( Dictionary::DataRequest::* )( size_t, size_t, void * ) >::value,
                   "ControlledDataRequest must not hide DataRequest::getDataSlice" );
    static_assert( std::is_same< decltype( &TestSupport::ControlledDataRequest::getFullData ),
                                 std::vector< char > & ( Dictionary::DataRequest::* )() >::value,
                   "ControlledDataRequest must not hide DataRequest::getFullData" );
}

void TstUtf8Wstring::controlledRequestCompletesInstantly()
{
    // Instant-finish construction: isFinished() is already true when the
    // constructor returns, so a consumer that connects afterwards must recheck
    // the terminal state instead of waiting for a signal that already happened.
    TestSupport::ControlledDataRequest instant( QByteArray( "ABCDEFGHIJ" ) );

    QVERIFY( instant.isFinished() );
    QCOMPARE( instant.terminalEmissions(), 1 );
    QCOMPARE( instant.dataSize(), 10L );
    QCOMPARE( bytesOf( instant.getFullData() ), QByteArray( "ABCDEFGHIJ" ) );

    // Empty success is distinguishable from "unknown": 0 bytes versus -1.
    TestSupport::ControlledDataRequest empty{ QByteArray() };

    QVERIFY( empty.isFinished() );
    QCOMPARE( empty.dataSize(), 0L );
    QVERIFY( empty.getErrorString().isEmpty() );
    QVERIFY( empty.getFullData().empty() );

    // A pending request that is finished synchronously behaves the same way, and
    // the production signals really fire exactly once.
    QSignalSpy finishedSpy( &instant, &Dictionary::Request::finished );
    TestSupport::ControlledDataRequest pending;
    QSignalSpy pendingSpy( &pending, &Dictionary::Request::finished );
    pending.append( QByteArray( "x" ) );
    QCOMPARE( pendingSpy.count(), 0 );
    pending.finishSuccess();
    QCOMPARE( pendingSpy.count(), 1 );
    QCOMPARE( finishedSpy.count(), 0 );

    // The probe starts untouched: no reader has looked at any operation yet.
    for ( TestSupport::ReadOperation operation : { TestSupport::ReadOperation::FullData,
                                                  TestSupport::ReadOperation::Slice,
                                                  TestSupport::ReadOperation::StreamCopy } )
    {
        QVERIFY( instant.readProbe( operation ).untouched() );
    }
}

void TstUtf8Wstring::readerForwardsThroughTheProductionDelegate()
{
    // The factory is the one a suite installs; it must hand out a reader that
    // forwards to the real makeDataRequestReader() and record the probe.
    const DataRequestReaderFactory factory = TestSupport::controlledReaderFactory();

    sptr< TestSupport::ControlledDataRequest > request =
        new TestSupport::ControlledDataRequest( QByteArray( "ABCDEFGHIJ" ) );

    std::shared_ptr< DataRequestReader const > reader = factory( request );
    QVERIFY( reader );
    QVERIFY( std::dynamic_pointer_cast< TestSupport::ThrowingDataRequestReader const >( reader ) );

    // Read-only forwarding, exactly as the production buffered path does it.
    QCOMPARE( reader->fullData( *request ), QByteArray( "ABCDEFGHIJ" ) );

    char slice[ 6 ] = {};
    reader->dataSlice( *request, 3, 5, slice );
    QCOMPARE( QByteArray( slice, 5 ), QByteArray( "DEFGH" ) );

    // StreamCopy copies exactly count bytes from the supplied span and touches
    // nothing else.
    char destination[ 5 ] = { 'q', 'q', 'q', 'q', 'q' };
    char source[ 5 ] = { 'x', 'y', 'z', 'w', 'v' };
    reader->copyStreamBytes( destination, source + 1, 3 );

    QCOMPARE( QByteArray( destination, 3 ), QByteArray( "yzw" ) );
    QCOMPARE( destination[ 3 ], 'q' );
    QCOMPARE( destination[ 4 ], 'q' );

    // Every operation was observed exactly once, on this thread, without a throw.
    const TestSupport::ReadProbeSnapshot fullData = request->readProbe( TestSupport::ReadOperation::FullData );
    QCOMPARE( fullData.entered, 1 );
    QCOMPARE( fullData.returned, 1 );
    QCOMPARE( fullData.thrown, 0 );
    QCOMPARE( fullData.callingThreads, QSet< Qt::HANDLE >() << QThread::currentThreadId() );

    const TestSupport::ReadProbeSnapshot sliceProbe = request->readProbe( TestSupport::ReadOperation::Slice );
    QCOMPARE( sliceProbe.entered, 1 );
    QCOMPARE( sliceProbe.returned, 1 );
    QCOMPARE( sliceProbe.thrown, 0 );

    const TestSupport::ReadProbeSnapshot copyProbe =
        request->readProbe( TestSupport::ReadOperation::StreamCopy );
    QCOMPARE( copyProbe.entered, 1 );
    QCOMPARE( copyProbe.returned, 1 );
    QCOMPARE( copyProbe.thrown, 0 );

    // The probe is per operation: a StreamCopy performed on another thread is
    // recorded there and nowhere else.
    std::thread worker( [ &reader, &source ]() {
        char destination[ 2 ] = {};
        reader->copyStreamBytes( destination, source, 2 );
    } );
    worker.join();

    const TestSupport::ReadProbeSnapshot threaded =
        request->readProbe( TestSupport::ReadOperation::StreamCopy );
    QCOMPARE( threaded.entered, 2 );
    QCOMPARE( threaded.returned, 2 );
    // Two distinct caller threads were recorded, which is what proves the probe
    // identifies the boundary rather than assuming it.
    QCOMPARE( threaded.callingThreads.size(), 2 );
    QCOMPARE( request->readProbe( TestSupport::ReadOperation::Slice ).entered, 1 );
}

void TstUtf8Wstring::readerThrowsOnceWithTheArmedMessage()
{
    const DataRequestReaderFactory factory = TestSupport::controlledReaderFactory();

    sptr< TestSupport::ControlledDataRequest > request =
        new TestSupport::ControlledDataRequest( QByteArray( "ABCDEFGHIJ" ) );

    std::shared_ptr< DataRequestReader const > reader = factory( request );

    const QByteArray before = request->producedBytes();
    const long sizeBefore = request->dataSize();

    // Each setter arms exactly one operation, resets only that operation's probe
    // and neither changes producer bytes nor invokes a reader.
    request->setFullDataException( QStringLiteral( "full-data-arm" ) );

    QCOMPARE( request->producedBytes(), before );
    QCOMPARE( request->dataSize(), sizeBefore );
    QVERIFY( request->readProbe( TestSupport::ReadOperation::FullData ).untouched() );
    QVERIFY( request->readProbe( TestSupport::ReadOperation::Slice ).untouched() );
    QVERIFY( request->readProbe( TestSupport::ReadOperation::StreamCopy ).untouched() );

    // An unarmed operation still forwards: arming FullData does not arm Slice.
    char slice[ 2 ] = {};
    reader->dataSlice( *request, 0, 2, slice );
    QCOMPARE( QByteArray( slice, 2 ), QByteArray( "AB" ) );
    QCOMPARE( request->readProbe( TestSupport::ReadOperation::Slice ).returned, 1 );
    QCOMPARE( request->readProbe( TestSupport::ReadOperation::FullData ).entered, 0 );

    // The armed operation throws the configured UTF-8 message before forwarding.
    bool thrown = false;

    try
    {
        reader->fullData( *request );
    }
    catch ( std::runtime_error const & error )
    {
        thrown = true;
        QCOMPARE( QString::fromUtf8( error.what() ), QStringLiteral( "full-data-arm" ) );
    }

    QVERIFY2( thrown, "the armed FullData operation forwarded instead of throwing" );

    const TestSupport::ReadProbeSnapshot armed = request->readProbe( TestSupport::ReadOperation::FullData );
    QCOMPARE( armed.entered, 1 );
    QCOMPARE( armed.thrown, 1 );
    QCOMPARE( armed.returned, 0 );
    QCOMPARE( armed.callingThreads, QSet< Qt::HANDLE >() << QThread::currentThreadId() );
    QCOMPARE( request->producedBytes(), before );

    // One-shot: the very next operation forwards normally again.
    QCOMPARE( reader->fullData( *request ), QByteArray( "ABCDEFGHIJ" ) );

    const TestSupport::ReadProbeSnapshot after = request->readProbe( TestSupport::ReadOperation::FullData );
    QCOMPARE( after.entered, 2 );
    QCOMPARE( after.thrown, 1 );
    QCOMPARE( after.returned, 1 );

    // The other two setters own their own operation and their own message.
    request->setSliceException( QStringLiteral( "slice-arm" ) );
    QVERIFY( request->readProbe( TestSupport::ReadOperation::Slice ).untouched() );
    QVERIFY_THROWS_EXCEPTION( std::runtime_error, reader->dataSlice( *request, 0, 1, slice ) );
    QCOMPARE( request->readProbe( TestSupport::ReadOperation::Slice ).thrown, 1 );

    request->setStreamCopyException( QStringLiteral( "stream-copy-arm" ) );
    char destination[ 2 ] = { 'a', 'b' };
    char source[ 2 ] = { 'x', 'y' };
    QVERIFY_THROWS_EXCEPTION( std::runtime_error, reader->copyStreamBytes( destination, source, 2 ) );

    // The throw happened before the copy: the destination is untouched.
    QCOMPARE( destination[ 0 ], 'a' );
    QCOMPARE( destination[ 1 ], 'b' );
    QCOMPARE( request->readProbe( TestSupport::ReadOperation::StreamCopy ).thrown, 1 );

    // Setters reset their operation's probe: a fresh arm starts from zero.
    request->setFullDataException( QStringLiteral( "second-arm" ) );
    QVERIFY( request->readProbe( TestSupport::ReadOperation::FullData ).untouched() );
}

void TstUtf8Wstring::readerKeepsNoProducer()
{
    const DataRequestReaderFactory factory = TestSupport::controlledReaderFactory();

    sptr< TestSupport::ControlledDataRequest > request =
        new TestSupport::ControlledDataRequest( QByteArray( "ABCDEFGHIJ" ) );

    const std::weak_ptr< void const > lifetime = request->lifetimeToken();
    const std::shared_ptr< TestSupport::ReadProbeState > state = request->probeState();

    std::shared_ptr< DataRequestReader const > reader = factory( request );
    QVERIFY( !lifetime.expired() );

    // Arm the producer-free operation, then destroy the producer: the reader must
    // stay valid, keep its armed state and never resurrect the request.
    request->setStreamCopyException( QStringLiteral( "after-producer-death" ) );
    request.reset();

    QVERIFY2( lifetime.expired(), "the reader retained the producer it was built for" );

    char destination[ 3 ] = { 'q', 'q', 'q' };
    char source[ 3 ] = { 'x', 'y', 'z' };

    bool thrown = false;

    try
    {
        reader->copyStreamBytes( destination, source, 3 );
    }
    catch ( std::runtime_error const & error )
    {
        thrown = true;
        QCOMPARE( QString::fromUtf8( error.what() ), QStringLiteral( "after-producer-death" ) );
    }

    QVERIFY( thrown );
    QCOMPARE( destination[ 0 ], 'q' );

    // The shared probe state survived the producer, which is what the streaming
    // device's IO path relies on.
    {
        std::lock_guard< std::mutex > lock( state->mutex );
        QCOMPARE( state->operation( TestSupport::ReadOperation::StreamCopy ).entered, 1 );
        QCOMPARE( state->operation( TestSupport::ReadOperation::StreamCopy ).thrown, 1 );
    }

    // And the reader copies a supplied span without any producer at all.
    reader->copyStreamBytes( destination, source, 3 );
    QCOMPARE( QByteArray( destination, 3 ), QByteArray( "xyz" ) );
}

void TstUtf8Wstring::factoryFallsBackToTheProductionReader()
{
    const DataRequestReaderFactory factory = TestSupport::controlledReaderFactory();

    // A request that is not one of the fixture's producers is read by the
    // production reader, unwrapped: no probe, no throwing, no substitution.
    auto * instantSource = new Dictionary::DataRequestInstant( true );
    instantSource->getData().assign( { 'p', 'r', 'o', 'd' } );
    sptr< Dictionary::DataRequest > instant = instantSource;

    std::shared_ptr< DataRequestReader const > reader = factory( instant );
    QVERIFY( reader );
    QVERIFY( !std::dynamic_pointer_cast< TestSupport::ThrowingDataRequestReader const >( reader ) );
    QCOMPARE( reader->fullData( *instant ), QByteArray( "prod" ) );

    // Arming a fixture producer does not affect the fallback reader.
    sptr< TestSupport::ControlledDataRequest > controlled =
        new TestSupport::ControlledDataRequest( QByteArray( "ctl" ) );
    controlled->setFullDataException( QStringLiteral( "only-for-this-request" ) );

    std::shared_ptr< DataRequestReader const > fallbackReader = factory( instant );
    QCOMPARE( fallbackReader->fullData( *instant ), QByteArray( "prod" ) );

    // ... and the armed request still dies on its own operation.
    std::shared_ptr< DataRequestReader const > armedReader = factory( controlled );
    QVERIFY_THROWS_EXCEPTION( std::runtime_error, armedReader->fullData( *controlled ) );

    // A null producer never yields a reader silently: the factory returns the
    // production reader and the caller fails on its own terms.
    sptr< Dictionary::DataRequest > none;
    QVERIFY( factory( none ) );
}

void TstUtf8Wstring::httpOriginsBindOnEveryLoopbackAddress()
{
    // The three literal origins the browser fixtures require: distinct hosts,
    // ephemeral ports read back from the socket.
    for ( const QString & host : { QStringLiteral( "127.0.0.1" ), QStringLiteral( "127.0.0.2" ),
                                   QStringLiteral( "127.0.0.3" ) } )
    {
        TestSupport::HttpFixture server;
        QVERIFY2( server.start( QHostAddress( host ), 0 ), qPrintable( server.errorString() ) );

        const QUrl address = server.url( QStringLiteral( "/dir/first.html" ) );
        QCOMPARE( address.host(), host );
        QVERIFY( address.port() > 0 );
        QCOMPARE( address.scheme(), QStringLiteral( "http" ) );
        QCOMPARE( address.path(), QStringLiteral( "/dir/first.html" ) );
        QCOMPARE( server.address().toString(), host );
        QVERIFY( server.isListening() );
    }

    // No fixed port: two fixtures on the same host never collide.
    TestSupport::HttpFixture first;
    TestSupport::HttpFixture second;
    QVERIFY( first.start( QHostAddress( QStringLiteral( "127.0.0.1" ) ), 0 ) );
    QVERIFY( second.start( QHostAddress( QStringLiteral( "127.0.0.1" ) ), 0 ) );
    QVERIFY( first.port() != second.port() );

    // The gate helper binds all three and closes them again.
    QStringList reasons;
    QVERIFY2( TestSupport::loopbackOriginsBindable( &reasons ), qPrintable( reasons.join( '\n' ) ) );
    QVERIFY( reasons.isEmpty() );
}

void TstUtf8Wstring::httpBindFailureIsARequiredFailure()
{
    TestSupport::HttpFixture occupied;
    QVERIFY2( occupied.start( QHostAddress( QStringLiteral( "127.0.0.1" ) ), 0 ),
              qPrintable( occupied.errorString() ) );

    // A second bind on the exact address:port really fails, with a diagnostic
    // instead of a silently dead fixture.
    TestSupport::HttpFixture conflict;
    QVERIFY2( !conflict.start( QHostAddress( QStringLiteral( "127.0.0.1" ) ), occupied.port() ),
              "two fixtures bound the same loopback port" );
    QVERIFY( !conflict.errorString().isEmpty() );
    QVERIFY( conflict.errorString().contains( QString::number( occupied.port() ) ) );
    QVERIFY( !conflict.isListening() );

    // A null address cannot be served either.
    TestSupport::HttpFixture invalid;
    QVERIFY2( !invalid.start( QHostAddress(), 0 ), "a null address was reported as a bound server" );
    QVERIFY( !invalid.errorString().isEmpty() );

    // The gate reports a failure as a reason and returns false, which is what a
    // suite's initTestCase turns into a required failure.
    QStringList reasons;
    QVERIFY2( !TestSupport::loopbackOriginsBindable( &reasons, { QStringLiteral( "no-such-host.invalid" ) } ),
              "an unresolvable loopback host was reported as bindable" );
    QVERIFY( !reasons.isEmpty() );
    QVERIFY( reasons.join( '\n' ).contains( QLatin1String( "no-such-host.invalid" ) ) );

    // The real three bind cleanly.
    reasons.clear();
    QVERIFY2( TestSupport::loopbackOriginsBindable( &reasons ), qPrintable( reasons.join( '\n' ) ) );
}

void TstUtf8Wstring::httpServesFixedContentRedirectsAndHost()
{
    TestSupport::HttpFixture server;
    QVERIFY2( server.start( QHostAddress( QStringLiteral( "127.0.0.2" ) ), 0 ),
              qPrintable( server.errorString() ) );

    QNetworkAccessManager manager;

    // A website document is served byte for byte, with the right MIME type and a
    // Host header that really carries the non-default port.
    HttpFetch document = httpGet( manager, server.url( QStringLiteral( "/dir/first.html" ) ) );
    QVERIFY2( document.ok, qPrintable( document.error ) );
    QCOMPARE( document.status, 200 );
    QCOMPARE( document.contentType, QByteArray( "text/html; charset=utf-8" ) );
    QCOMPARE( document.body,
              TestSupport::HttpFixture::fixtureBytes( QStringLiteral( "website/first.html" ) ) );

    QCOMPARE( server.requestCount( QStringLiteral( "/dir/first.html" ) ), 1 );

    const QVector< TestSupport::HttpFixture::Request > requests = server.requests();
    QVERIFY( !requests.isEmpty() );
    QCOMPARE( requests.last().method, QByteArray( "GET" ) );
    QCOMPARE( requests.last().path, QByteArray( "/dir/first.html" ) );
    QCOMPARE( requests.last().host,
              QStringLiteral( "127.0.0.2:%1" ).arg( server.port() ).toUtf8() );
    QVERIFY( !requests.last().proxyRequest );

    // The shared 1x1 PNG is served under every image path.
    HttpFetch image = httpGet( manager, server.url( QStringLiteral( "/images/pixel.png" ) ) );
    QVERIFY2( image.ok, qPrintable( image.error ) );
    QCOMPARE( image.contentType, QByteArray( "image/png" ) );
    QCOMPARE( image.body, TestSupport::HttpFixture::pixelPng() );

    // A few remaining fixed routes.
    QVERIFY( httpGet( manager, server.url( QStringLiteral( "/article/article.html" ) ) ).body
                 == TestSupport::HttpFixture::fixtureBytes( QStringLiteral( "article/article.html" ) ) );
    QVERIFY( httpGet( manager, server.url( QStringLiteral( "/print/two-pages.html" ) ) ).body
                 == TestSupport::HttpFixture::fixtureBytes( QStringLiteral( "print/two-pages.html" ) ) );
    QCOMPARE( httpGet( manager, server.url( QStringLiteral( "/missing" ) ) ).status, 404 );

    // Framing policy headers are real response headers.
    const HttpFetch deniedFrame = httpGet( manager, server.url( QStringLiteral( "/deny-frame" ) ) );
    QCOMPARE( deniedFrame.status, 200 );
    QCOMPARE( deniedFrame.header( "x-frame-options" ), QByteArray( "DENY" ) );

    const HttpFetch deniedCsp = httpGet( manager, server.url( QStringLiteral( "/deny-csp" ) ) );
    QCOMPARE( deniedCsp.status, 200 );
    QCOMPARE( deniedCsp.header( "content-security-policy" ),
              QByteArray( "frame-ancestors 'none'" ) );

    // Redirects: the raw 302 with its Location, and a followed redirect landing
    // on the real target document.
    const HttpFetch rawRedirect = httpGet( manager, server.url( QStringLiteral( "/redirect" ) ),
                                           QNetworkRequest::ManualRedirectPolicy );
    QCOMPARE( rawRedirect.status, 302 );
    QCOMPARE( rawRedirect.header( "location" ), QByteArray( "/dir/first.html" ) );

    const HttpFetch followed = httpGet( manager, server.url( QStringLiteral( "/redirect-absolute" ) ),
                                        QNetworkRequest::NoLessSafeRedirectPolicy );
    QVERIFY2( followed.ok, qPrintable( followed.error ) );
    QCOMPARE( followed.url.path(), QStringLiteral( "/dir/first.html" ) );
    QCOMPARE( followed.body,
              TestSupport::HttpFixture::fixtureBytes( QStringLiteral( "website/first.html" ) ) );
    QCOMPARE( server.requestCount( QStringLiteral( "/redirect" ) ), 1 );
    QCOMPARE( server.requestCount( QStringLiteral( "/redirect-absolute" ) ), 1 );

    // Origin authentication: a genuine 401 challenge, then the fixed body once
    // the fixture credentials are supplied.
    int challenges = 0;

    QObject::connect( &manager, &QNetworkAccessManager::authenticationRequired,
                      [ &challenges ]( QNetworkReply *, QAuthenticator * authenticator ) {
                          ++challenges;
                          authenticator->setUser( TestSupport::HttpFixture::originUser() );
                          authenticator->setPassword( TestSupport::HttpFixture::originPassword() );
                      } );

    const HttpFetch authenticated = httpGet( manager, server.url( QStringLiteral( "/auth" ) ) );
    QCOMPARE( challenges, 1 );
    QVERIFY2( authenticated.status == 200, qPrintable( authenticated.error ) );
    QCOMPARE( authenticated.body, QByteArray( "origin-authenticated\n" ) );

    // The challenge really happened on the wire before the credentials arrived.
    bool sawChallenge = false;
    for ( auto const & request : server.requests() )
    {
        if ( request.path == QByteArray( "/auth" ) && !request.originAuthorized )
            sawChallenge = true;
    }
    QVERIFY( sawChallenge );
}

void TstUtf8Wstring::httpBarrierHoldsUntilReleased()
{
    TestSupport::HttpFixture server;
    QVERIFY2( server.start( QHostAddress( QStringLiteral( "127.0.0.3" ) ), 0 ),
              qPrintable( server.errorString() ) );

    QNetworkAccessManager manager;
    QNetworkRequest request( server.url( QStringLiteral( "/slow-resource" ) ) );
    QNetworkReply * reply = manager.get( request );

    // The request reaches the server, and the server deliberately does not
    // answer: that is the barrier a save/download test needs to observe.
    QVERIFY( QTest::qWaitFor( [ &server ]() {
        return server.requestCount( QStringLiteral( "/slow-resource" ) ) == 1;
    }, 5000 ) );

    // The request is on the wire and the server is holding it. Waiting for the
    // server's own "held" accounting is deterministic: an unbarriered server
    // never reaches this state, and a slow machine only makes the wait longer.
    QVERIFY( QTest::qWaitFor( [ &server ]() {
        return server.heldRequestCount( QStringLiteral( "/slow-resource" ) ) == 1;
    }, 5000 ) );
    QVERIFY2( !reply->isFinished(), "the barrier answered before release() was called" );

    server.release( QStringLiteral( "/slow-resource" ) );

    QVERIFY( QTest::qWaitFor( [ reply ]() { return reply->isFinished(); }, 5000 ) );
    QCOMPARE( reply->error(), QNetworkReply::NoError );
    QCOMPARE( reply->readAll(), QByteArray( "slow-resource-body\n" ) );
    QCOMPARE( server.requestCount( QStringLiteral( "/slow-resource" ) ), 1 );

    reply->deleteLater();

    // Releasing a path that is not waiting is a harmless no-op.
    server.release( QStringLiteral( "/dir/first.html" ) );
}

void TstUtf8Wstring::httpProxyChallengesAndForwards()
{
    // 127.0.0.2 is the origin, 127.0.0.1 is the proxy: distinct origins, exactly
    // the shape the external-content suite needs.
    TestSupport::HttpFixture origin;
    QVERIFY2( origin.start( QHostAddress( QStringLiteral( "127.0.0.2" ) ), 0 ),
              qPrintable( origin.errorString() ) );

    TestSupport::HttpFixture proxy;
    QVERIFY2( proxy.start( QHostAddress( QStringLiteral( "127.0.0.1" ) ), 0 ),
              qPrintable( proxy.errorString() ) );

    QNetworkAccessManager manager;
    manager.setProxy( proxy.httpProxy() );

    int challenges = 0;

    QObject::connect( &manager, &QNetworkAccessManager::proxyAuthenticationRequired,
                      [ &challenges, &proxy ]( QNetworkProxy const & used, QAuthenticator * authenticator ) {
                          ++challenges;
                          QCOMPARE( used.hostName(), proxy.address().toString() );
                          authenticator->setUser( TestSupport::HttpFixture::proxyUser() );
                          authenticator->setPassword( TestSupport::HttpFixture::proxyPassword() );
                      } );

    const HttpFetch forwarded = httpGet( manager, origin.url( QStringLiteral( "/dir/first.html" ) ) );
    QVERIFY2( forwarded.ok, qPrintable( forwarded.error ) );
    QCOMPARE( forwarded.body,
              TestSupport::HttpFixture::fixtureBytes( QStringLiteral( "website/first.html" ) ) );

    // A genuine 407 was returned and answered with the fixture credentials.
    QCOMPARE( challenges, 1 );

    const QVector< TestSupport::HttpFixture::Request > proxied = proxy.requests();

    QVERIFY( proxied.size() >= 2 );
    QVERIFY( proxied.first().proxyRequest );
    QVERIFY( !proxied.first().proxied );
    QVERIFY( proxied.last().proxyRequest );
    QVERIFY( proxied.last().proxied );
    QCOMPARE( proxied.last().path, QByteArray( "/dir/first.html" ) );

    // The proxy really forwarded to the target origin, with the target Host.
    QCOMPARE( origin.requestCount( QStringLiteral( "/dir/first.html" ) ), 1 );
    QCOMPARE( origin.requests().last().host,
              QStringLiteral( "127.0.0.2:%1" ).arg( origin.port() ).toUtf8() );

    const QNetworkProxy caching = proxy.cachingProxy();
    QCOMPARE( caching.type(), QNetworkProxy::HttpCachingProxy );
    QCOMPARE( caching.hostName(), proxy.address().toString() );
    QCOMPARE( caching.port(), static_cast< quint16 >( proxy.port() ) );
}

void TstUtf8Wstring::httpProxyTunnelsAConnectRequest()
{
    // CONNECT is what an https request goes through. Driving it with a raw
    // socket keeps the assertion on the tunnel itself: challenge, tunnel
    // establishment, and the origin's bytes arriving unchanged through it.
    // Every wait runs through QTest::qWait so the fixture's own sockets keep
    // being served by this thread's event loop.
    TestSupport::HttpFixture origin;
    QVERIFY2( origin.start( QHostAddress( QStringLiteral( "127.0.0.3" ) ), 0 ),
              qPrintable( origin.errorString() ) );

    TestSupport::HttpFixture proxy;
    QVERIFY2( proxy.start( QHostAddress( QStringLiteral( "127.0.0.1" ) ), 0 ),
              qPrintable( proxy.errorString() ) );

    const QByteArray connectTarget =
        QStringLiteral( "127.0.0.3:%1" ).arg( origin.port() ).toUtf8();

    const auto connectToProxy = [ &proxy ]( QTcpSocket & socket ) {
        socket.connectToHost( QStringLiteral( "127.0.0.1" ),
                              static_cast< quint16 >( proxy.port() ) );
        return QTest::qWaitFor(
            [ &socket ]() { return socket.state() == QAbstractSocket::ConnectedState; }, 5000 );
    };

    const auto pumpUntil = []( QTcpSocket & socket, QByteArray & buffer, int timeoutMs,
                               std::function< bool() > done ) {
        QElapsedTimer elapsed;
        elapsed.start();

        while ( !done() && elapsed.elapsed() < timeoutMs )
        {
            QTest::qWait( 20 );
            buffer += socket.readAll();
        }

        return done();
    };

    // 1) Without proxy credentials the tunnel is refused with a real 407.
    {
        QTcpSocket socket;
        QVERIFY( connectToProxy( socket ) );

        socket.write( "CONNECT " + connectTarget + " HTTP/1.1\r\nHost: " + connectTarget
                      + "\r\n\r\n" );

        QByteArray response;
        const bool challenged = pumpUntil( socket, response, 5000, [ &response ]() {
            return response.contains( "\r\n\r\n" );
        } );
        QVERIFY( challenged );
        QVERIFY2( response.startsWith( "HTTP/1.1 407" ), response.constData() );
        QVERIFY( response.contains(
            "Proxy-Authenticate: Basic realm=\"goldendict-fixture-proxy\"" ) );
    }

    // 2) With them, the tunnel opens and the origin answers through it.
    {
        QTcpSocket socket;
        QVERIFY( connectToProxy( socket ) );

        const QByteArray credentials =
            ( TestSupport::HttpFixture::proxyUser() + QLatin1Char( ':' )
              + TestSupport::HttpFixture::proxyPassword() )
                .toUtf8()
                .toBase64();

        socket.write( "CONNECT " + connectTarget + " HTTP/1.1\r\nHost: " + connectTarget
                      + "\r\nProxy-Authorization: Basic " + credentials + "\r\n\r\n" );

        QByteArray response;
        const bool established = pumpUntil( socket, response, 5000, [ &response ]() {
            return response.contains( "\r\n\r\n" );
        } );
        QVERIFY( established );
        QVERIFY2( response.startsWith( "HTTP/1.1 200" ), response.constData() );

        // Real bytes through the tunnel: an origin-form GET inside it.
        socket.write( "GET /dir/second.html HTTP/1.1\r\nHost: " + connectTarget
                      + "\r\nConnection: close\r\n\r\n" );

        const QByteArray expected =
            TestSupport::HttpFixture::fixtureBytes( QStringLiteral( "website/second.html" ) );

        response.clear();
        const bool tunnelled = pumpUntil( socket, response, 5000, [ &response, &expected ]() {
            return response.contains( expected );
        } );
        QVERIFY( tunnelled );
        QVERIFY( response.contains( "200 OK" ) );
        QCOMPARE( origin.requestCount( QStringLiteral( "/dir/second.html" ) ), 1 );
    }

    // Both attempts are recorded as CONNECT requests through the proxy; only the
    // one that carried the fixture credentials actually became a tunnel.
    int attempts = 0;
    int tunnels = 0;

    for ( auto const & request : proxy.requests() )
    {
        if ( request.method == QByteArray( "CONNECT" ) )
            ++attempts;

        if ( request.tunnel )
            ++tunnels;
    }

    QCOMPARE( attempts, 2 );
    QCOMPARE( tunnels, 1 );
}

void TstUtf8Wstring::printFixtureHoldsTwoFixedPages()
{
    const QString print = QString::fromUtf8(
        TestSupport::HttpFixture::fixtureBytes( QStringLiteral( "print/two-pages.html" ) ) );

    QVERIFY( print.contains( QLatin1String( "id=\"snapshot-page-one\"" ) ) );
    QVERIFY( print.contains( QLatin1String( "id=\"snapshot-page-two\"" ) ) );
    QVERIFY( print.contains( QLatin1String( "snapshot-page-one" ) ) );
    QVERIFY( print.contains( QLatin1String( "snapshot-page-two" ) ) );

    // Exactly two page sections, one explicit break after the first one.
    QCOMPARE( print.count( QLatin1String( "class=\"page\"" ) ), 2 );
    QVERIFY( print.contains( QLatin1String( "break-after: page" ) ) );
    QVERIFY( print.contains( QLatin1String( "page-break-after: always" ) ) );
    QVERIFY( print.contains( QLatin1String( ":last-of-type" ) ) );

    // The first section carries the break, the second one must not: a trailing
    // break is what produces the empty third page this fixture exists to avoid.
    const int firstSection = print.indexOf( QLatin1String( "id=\"snapshot-page-one\"" ) );
    const int secondSection = print.indexOf( QLatin1String( "id=\"snapshot-page-two\"" ) );
    const int lastOfTypeRule = print.indexOf( QLatin1String( "section.page:last-of-type" ) );

    QVERIFY( firstSection > 0 );
    QVERIFY( secondSection > firstSection );
    QVERIFY( lastOfTypeRule > 0 );
}

void TstUtf8Wstring::privateBusIsIsolatedFromTheDesktopBus()
{
    const QByteArray sessionAddress = qgetenv( "DBUS_SESSION_BUS_ADDRESS" );

    TestSupport::PrivateSessionBus bus;
    QString reason;
    QVERIFY2( bus.start( &reason ), qPrintable( reason ) );

    QVERIFY( bus.isRunning() );
    QVERIFY( bus.daemonProcessId() > 0 );
    QVERIFY( bus.address().startsWith( QLatin1String( "unix:" ) ) );
    QVERIFY( bus.connection().isConnected() );
    QVERIFY( bus.connection().baseService().startsWith( QLatin1Char( ':' ) ) );

    // The fixture gets its own peer connection on the same private daemon: a
    // shared connection would turn every portal call into a local call that never
    // reaches the bus.
    QVERIFY( bus.fixtureConnection().isConnected() );
    QVERIFY( bus.fixtureConnection().baseService() != bus.connection().baseService() );

    // The daemon is a private child process on its own socket, not the desktop
    // session bus, and starting it never repointed the desktop bus either.
    QVERIFY( bus.address() != QString::fromUtf8( sessionAddress ) );
    QCOMPARE( qgetenv( "DBUS_SESSION_BUS_ADDRESS" ), sessionAddress );

    {
        TestSupport::FakePortal portal;
        QVERIFY2( portal.start( bus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );

        // The well-known name is owned by the fixture's own peer connection on
        // this private bus, and by nobody else.
        QCOMPARE( portal.serviceOwner(), bus.fixtureConnection().baseService() );
        QVERIFY( portal.serviceOwner() != bus.connection().baseService() );
        QCOMPARE( bus.connection().interface()
                      ->serviceOwner( TestSupport::FakePortal::serviceName() )
                      .value(),
                  bus.fixtureConnection().baseService() );
        QVERIFY( portal.isRegistered() );
        QCOMPARE( portal.serviceName(), TestSupport::FakePortal::serviceName() );

        // If a desktop bus exists at all, our private name must not be resolvable
        // there: the fixture never claims the real portal name on it.
        if ( QDBusConnection::sessionBus().isConnected() )
        {
            QVERIFY( QDBusConnection::sessionBus()
                         .interface()
                         ->serviceOwner( TestSupport::FakePortal::serviceName() )
                         .value()
                     != bus.connection().baseService() );
        }
    }

    // The fixture objects died before the daemon.
    bus.stop();
    QVERIFY( !bus.isRunning() );
    QVERIFY( !bus.connection().isConnected() );
}

void TstUtf8Wstring::missingPrivateBusIsARequiredFailure()
{
    TestSupport::PrivateSessionBus missing;
    QString reason;
    QVERIFY2( !missing.start( QStringLiteral( "/nonexistent/gd-no-such-dbus-daemon" ), &reason ),
              "a nonexistent daemon program was reported as a running bus" );
    QVERIFY( !reason.isEmpty() );
    QVERIFY( !missing.isRunning() );
    QVERIFY( !missing.connection().isConnected() );
    QVERIFY( missing.address().isEmpty() );

    // The environment gate reports the same missing bus as unavailable, which is
    // how a required portal suite fails instead of skipping.
    EnvironmentValue const savedDaemon( "GD_TEST_DBUS_DAEMON" );
    qputenv( "GD_TEST_DBUS_DAEMON", "/nonexistent/gd-no-such-dbus-daemon" );

    QString gateReason;
    QVERIFY( !TestSupport::prerequisiteAvailable( QStringLiteral( "dbus" ), &gateReason ) );
    QVERIFY2( gateReason.contains( QLatin1String( "/nonexistent" ) ), qPrintable( gateReason ) );

    savedDaemon.restore();

    QString available;
    QVERIFY2( TestSupport::prerequisiteAvailable( QStringLiteral( "dbus" ), &available ),
              qPrintable( available ) );
    QVERIFY( available.isEmpty() );

    // Unknown prerequisite tokens stay failures: a typo never passes a gate.
    QVERIFY( !TestSupport::prerequisiteAvailable( QStringLiteral( "dbus-typo" ), nullptr ) );
}

void TstUtf8Wstring::fakePortalServesThePortalWireInterface()
{
    TestSupport::PrivateSessionBus bus;
    QString reason;
    QVERIFY2( bus.start( &reason ), qPrintable( reason ) );

    {
        TestSupport::FakePortal portal;
        QVERIFY2( portal.start( bus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );
        QCOMPARE( portal.version(), 2u );

        PortalSignalReceiver receiver;
        subscribePortalSignals( bus.connection(), &receiver );

        // Wire shape documentation: every scenario the consumers depend on.
        QVERIFY( TestSupport::FakePortal::scenarios().contains(
            QStringLiteral( "early-alternate-response" ) ) );
        QVERIFY( TestSupport::FakePortal::scenarios().contains( QStringLiteral( "bind-partial" ) ) );
        QVERIFY( TestSupport::FakePortal::scenarios().contains( QStringLiteral( "list-slow" ) ) );
        QVERIFY( TestSupport::FakePortal::scenarios().contains( QStringLiteral( "late-create-response" ) ) );

        // Version is read the way the real portal publishes it: Properties.Get.
        QDBusMessage getVersion = QDBusMessage::createMethodCall(
            TestSupport::FakePortal::serviceName(), TestSupport::FakePortal::portalPath(),
            QStringLiteral( "org.freedesktop.DBus.Properties" ), QStringLiteral( "Get" ) );
        getVersion << TestSupport::FakePortal::portalInterface() << QStringLiteral( "version" );

        const QDBusMessage versionReply = callWithEvents( bus.connection(), getVersion );
        QVERIFY2( versionReply.type() != QDBusMessage::ErrorMessage,
                  qPrintable( versionReply.errorMessage() ) );
        QCOMPARE( versionReply.signature(), QStringLiteral( "v" ) );
        QCOMPARE( versionReply.arguments().value( 0 ).value< QDBusVariant >().variant().toUInt(), 2u );

        const QString base = bus.connection().baseService();
        const QString handleToken = QStringLiteral( "gd_test_create" );
        const QString sessionToken = QStringLiteral( "gd_test_session" );

        // CreateSession.
        const PortalCall created =
            callPortal( bus.connection(), &receiver,
                        createSessionCall( handleToken, sessionToken ), 2 );

        QCOMPARE( created.events, QStringList( { QStringLiteral( "reply" ),
                                                 QStringLiteral( "Response" ) } ) );
        QCOMPARE( created.reply.signature(), QStringLiteral( "o" ) );
        QCOMPARE( replyPath( created.reply ), predictedRequestPath( base, handleToken ) );
        QCOMPARE( created.received.size(), 1 );
        QCOMPARE( created.received.at( 0 ).path, predictedRequestPath( base, handleToken ) );
        QCOMPARE( created.received.at( 0 ).signature, QStringLiteral( "ua{sv}" ) );
        QCOMPARE( created.received.at( 0 ).arguments.at( 0 ).toUInt(), 0u );

        const QString session = resultMap( created.received.at( 0 ), 1 )
                                    .value( QStringLiteral( "session_handle" ) )
                                    .toString();
        QCOMPARE( session, predictedSessionPath( base, sessionToken ) );
        QVERIFY( QDBusObjectPath( session ).path() == session );
        QCOMPARE( portal.sessionPath(), session );

        // ListShortcuts: the payload is a(sa{sv}), not a stringified variant.
        const PortalCall listed = callPortal( bus.connection(), &receiver,
                                              listShortcutsCall( session, QStringLiteral( "gd_test_list" ) ),
                                              2 );
        QCOMPARE( listed.received.value( 0 ).signature, QStringLiteral( "ua{sv}" ) );
        QCOMPARE( shortcutIdsOf( resultMap( listed.received.value( 0 ), 1 ) ),
                  QStringList( { QStringLiteral( "main-window" ),
                                 QStringLiteral( "clipboard-lookup" ) } ) );

        // BindShortcuts: the requested (sa{sv}) list arrives in order and exactly
        // one Bind is accepted for this session.
        const PortalCall bound = callPortal(
            bus.connection(), &receiver,
            bindShortcutsCall( session, { QStringLiteral( "main-window" ) },
                               QStringLiteral( "gd_test_bind" ) ),
            2 );

        QCOMPARE( shortcutIdsOf( resultMap( bound.received.value( 0 ), 1 ) ),
                  QStringList( { QStringLiteral( "main-window" ) } ) );

        QCOMPARE( portal.count( QStringLiteral( "BindShortcuts" ) ), 1 );
        QCOMPARE( portal.bindCountForSession( session ), 1 );

        bool sawBindCall = false;

        for ( TestSupport::FakePortal::Call const & call : portal.calls() )
        {
            if ( call.member != QLatin1String( "BindShortcuts" ) )
                continue;

            sawBindCall = true;
            QCOMPARE( call.shortcutIds, QStringList( { QStringLiteral( "main-window" ) } ) );
            QCOMPARE( call.signature, QByteArray( "oa(sa{sv})sa{sv}" ) );
            QVERIFY( call.parentWindow.isEmpty() );
            QCOMPARE( call.options.value( QStringLiteral( "handle_token" ) ).toString(),
                      QStringLiteral( "gd_test_bind" ) );
        }

        QVERIFY( sawBindCall );

        // ConfigureShortcuts on v2: an asynchronous method result with no Request
        // and no Response lifecycle.
        const int requestsBefore = portal.requestPaths().size();
        const PortalCall configured = callPortal(
            bus.connection(), &receiver,
            configureShortcutsCall( session, QStringLiteral( "activation-token-1" ) ), 1 );

        QCOMPARE( configured.reply.type(), QDBusMessage::ReplyMessage );
        QCOMPARE( portal.requestPaths().size(), requestsBefore );
        QCOMPARE( portal.calls().last().activationToken, QStringLiteral( "activation-token-1" ) );
        QVERIFY( portal.calls().last().requestPath.isEmpty() );

        // Signals: ShortcutsChanged (a(sa{sv})), Activated with the token
        // forwarded unchanged, Deactivated, and Session.Closed. The receiver is
        // cleared first so the arrival order below is exactly these four.
        receiver.events.clear();
        receiver.entries.clear();

        portal.emitShortcutsChanged( session, makeShortcuts( { QStringLiteral( "main-window" ) } ) );
        portal.emitActivation( session, QStringLiteral( "main-window" ),
                               QStringLiteral( "token-abc" ) );
        portal.emitDeactivated( session, QStringLiteral( "main-window" ), QString() );
        portal.emitSessionClosed( session, QStringLiteral( "session-closed" ) );

        QVERIFY( QTest::qWaitFor( [ &receiver ]() { return receiver.events.size() >= 4; }, 5000 ) );
        QCOMPARE( receiver.events,
                  QStringList( { QStringLiteral( "ShortcutsChanged" ), QStringLiteral( "Activated" ),
                                 QStringLiteral( "Deactivated" ), QStringLiteral( "Closed" ) } ) );

        QCOMPARE( receiver.entries.at( 0 ).signature, QStringLiteral( "oa(sa{sv})" ) );
        QCOMPARE( shortcutIdsOfList( receiver.entries.at( 0 ).arguments.value( 1 ) ),
                  QStringList( { QStringLiteral( "main-window" ) } ) );

        QCOMPARE( receiver.entries.at( 1 ).signature, QStringLiteral( "osta{sv}" ) );
        QCOMPARE( receiver.entries.at( 1 ).arguments.value( 0 ).value< QDBusObjectPath >().path(),
                  session );
        QCOMPARE( receiver.entries.at( 1 ).arguments.value( 1 ).toString(),
                  QStringLiteral( "main-window" ) );
        QCOMPARE( resultMap( receiver.entries.at( 1 ), 3 )
                      .value( QStringLiteral( "activation_token" ) )
                      .toString(),
                  QStringLiteral( "token-abc" ) );

        QCOMPARE( receiver.entries.at( 3 ).signature, QStringLiteral( "a{sv}" ) );
        QCOMPARE( resultMap( receiver.entries.at( 3 ), 0 )
                      .value( QStringLiteral( "reason" ) )
                      .toString(),
                  QStringLiteral( "session-closed" ) );

        // Closes are recorded with the interface the client used.
        callWithEvents( bus.connection(),
                        closeCall( predictedRequestPath( base, handleToken ),
                                   TestSupport::FakePortal::requestInterface() ) );
        callWithEvents( bus.connection(),
                        closeCall( session, TestSupport::FakePortal::sessionInterface() ) );

        QCOMPARE( portal.count( QStringLiteral( "Request.Close" ) ), 1 );
        QCOMPARE( portal.count( QStringLiteral( "Session.Close" ) ), 1 );
        QVERIFY( portal.protocolViolations().isEmpty() );

        // Service loss is observable.
        portal.loseService();
        QVERIFY( !portal.isRegistered() );
        QVERIFY( portal.serviceOwner().isEmpty() );
    }

    bus.stop();
    QVERIFY( !bus.isRunning() );
}

void TstUtf8Wstring::fakePortalEarlyResponsePrecedesTheMethodReply()
{
    TestSupport::PrivateSessionBus bus;
    QString reason;
    QVERIFY2( bus.start( &reason ), qPrintable( reason ) );

    {
        TestSupport::FakePortal portal;
        QVERIFY2( portal.start( bus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );

        PortalSignalReceiver receiver;
        subscribePortalSignals( bus.connection(), &receiver );

        const QString base = bus.connection().baseService();

        // Baseline: the Response follows the method reply.
        const PortalCall fast = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_fast" ), QStringLiteral( "gd_fast_session" ) ), 2 );
        QCOMPARE( fast.events, QStringList( { QStringLiteral( "reply" ),
                                              QStringLiteral( "Response" ) } ) );

        // early-response: the success Response for the *predicted* path arrives
        // before the method reply. A client that only subscribes after the reply
        // would lose it.
        portal.setScenario( QStringLiteral( "early-response" ) );
        const PortalCall early = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_early" ), QStringLiteral( "gd_early_session" ) ),
            2 );

        QCOMPARE( early.events, QStringList( { QStringLiteral( "Response" ),
                                               QStringLiteral( "reply" ) } ) );
        QCOMPARE( early.received.at( 0 ).path,
                  predictedRequestPath( base, QStringLiteral( "gd_early" ) ) );
        QCOMPARE( early.received.at( 0 ).arguments.at( 0 ).toUInt(), 0u );
        QCOMPARE( replyPath( early.reply ), predictedRequestPath( base, QStringLiteral( "gd_early" ) ) );

        // early-alternate-response: the named Task 30 scenario. The Response for
        // the eventual alternate path is emitted before CreateSession returns
        // that path, so the client must buffer an envelope for a path it has not
        // been told about yet.
        portal.setScenario( QStringLiteral( "early-alternate-response" ) );
        const PortalCall alternate = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_alternate" ),
                               QStringLiteral( "gd_alternate_session" ) ),
            2 );

        QCOMPARE( alternate.events, QStringList( { QStringLiteral( "Response" ),
                                                   QStringLiteral( "reply" ) } ) );

        const QString returned = replyPath( alternate.reply );

        QVERIFY( !returned.isEmpty() );
        QVERIFY( returned != predictedRequestPath( base, QStringLiteral( "gd_alternate" ) ) );
        QCOMPARE( alternate.received.at( 0 ).path, returned );
        QCOMPARE( alternate.received.at( 0 ).arguments.at( 0 ).toUInt(), 0u );
        QVERIFY( !resultMap( alternate.received.at( 0 ), 1 )
                      .value( QStringLiteral( "session_handle" ) )
                      .toString()
                      .isEmpty() );

        // alternate-response: the same alternate path, but the Response follows
        // the reply. A client that discards unknown-path envelopes on the reply
        // still has to accept this one.
        portal.setScenario( QStringLiteral( "alternate-response" ) );
        const PortalCall lateAlternate = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_late_alternate" ),
                               QStringLiteral( "gd_late_alternate_session" ) ),
            2 );

        QCOMPARE( lateAlternate.events, QStringList( { QStringLiteral( "reply" ),
                                                       QStringLiteral( "Response" ) } ) );
        QCOMPARE( lateAlternate.received.at( 0 ).path, replyPath( lateAlternate.reply ) );
        QVERIFY( replyPath( lateAlternate.reply )
                 != predictedRequestPath( base, QStringLiteral( "gd_late_alternate" ) ) );

        // unrelated-response: a perfectly valid Response on a path that belongs
        // to no request of ours must not be mistaken for this request's result.
        portal.setScenario( QStringLiteral( "unrelated-response" ) );
        const PortalCall unrelated = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_unrelated" ),
                               QStringLiteral( "gd_unrelated_session" ) ),
            2 );

        QCOMPARE( unrelated.received.size(), 1 );
        QVERIFY( unrelated.received.at( 0 ).path
                 != predictedRequestPath( base, QStringLiteral( "gd_unrelated" ) ) );
        QVERIFY( unrelated.received.at( 0 ).path != replyPath( unrelated.reply ) );
        QCOMPARE( replyPath( unrelated.reply ),
                  predictedRequestPath( base, QStringLiteral( "gd_unrelated" ) ) );

        // duplicate-response: the same Response twice; completion is idempotent on
        // the client side, and the fixture really did emit two.
        portal.setScenario( QStringLiteral( "duplicate-response" ) );
        const PortalCall duplicate = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_duplicate" ),
                               QStringLiteral( "gd_duplicate_session" ) ),
            3 );

        QCOMPARE( duplicate.events,
                  QStringList( { QStringLiteral( "reply" ), QStringLiteral( "Response" ),
                                 QStringLiteral( "Response" ) } ) );
    }

    bus.stop();
}

void TstUtf8Wstring::fakePortalRefusesASecondBind()
{
    TestSupport::PrivateSessionBus bus;
    QString reason;
    QVERIFY2( bus.start( &reason ), qPrintable( reason ) );

    {
        TestSupport::FakePortal portal;
        QVERIFY2( portal.start( bus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );

        PortalSignalReceiver receiver;
        subscribePortalSignals( bus.connection(), &receiver );

        const PortalCall created = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_bind_create" ),
                               QStringLiteral( "gd_bind_session" ) ),
            2 );

        const QString session = resultMap( created.received.value( 0 ), 1 )
                                    .value( QStringLiteral( "session_handle" ) )
                                    .toString();
        QVERIFY( !session.isEmpty() );

        const PortalCall accepted = callPortal(
            bus.connection(), &receiver,
            bindShortcutsCall( session, { QStringLiteral( "main-window" ) },
                               QStringLiteral( "gd_bind_one" ) ),
            2 );

        QCOMPARE( accepted.reply.type(), QDBusMessage::ReplyMessage );
        QCOMPARE( portal.bindCountForSession( session ), 1 );
        QVERIFY( portal.protocolViolations().isEmpty() );

        // A second Bind on the same session is refused by the portal and recorded
        // as a violation, so a production double-bind can never pass silently.
        const PortalCall refused = callPortal(
            bus.connection(), &receiver,
            bindShortcutsCall( session, { QStringLiteral( "clipboard-lookup" ) },
                               QStringLiteral( "gd_bind_two" ) ),
            1 );

        QCOMPARE( refused.reply.type(), QDBusMessage::ErrorMessage );
        QCOMPARE( portal.bindCountForSession( session ), 1 );
        QCOMPARE( portal.count( QStringLiteral( "BindShortcuts" ) ), 2 );
        QCOMPARE( portal.protocolViolations().size(), 1 );
        QVERIFY( portal.protocolViolations().first().contains( session ) );

        // A fresh session can bind once again: an explicit later edit is legal.
        const PortalCall second = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_bind_create_two" ),
                               QStringLiteral( "gd_bind_session_two" ) ),
            2 );

        const QString secondSession = resultMap( second.received.value( 0 ), 1 )
                                          .value( QStringLiteral( "session_handle" ) )
                                          .toString();

        const PortalCall rebound = callPortal(
            bus.connection(), &receiver,
            bindShortcutsCall( secondSession, { QStringLiteral( "main-window" ) },
                               QStringLiteral( "gd_bind_three" ) ),
            2 );

        QCOMPARE( rebound.reply.type(), QDBusMessage::ReplyMessage );
        QCOMPARE( portal.bindCountForSession( secondSession ), 1 );
    }

    bus.stop();
}

void TstUtf8Wstring::fakePortalCoversMalformedPayloads()
{
    TestSupport::PrivateSessionBus bus;
    QString reason;
    QVERIFY2( bus.start( &reason ), qPrintable( reason ) );

    {
        TestSupport::FakePortal portal;
        QVERIFY2( portal.start( bus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );

        PortalSignalReceiver receiver;
        subscribePortalSignals( bus.connection(), &receiver );

        const auto freshSession = [ & ]( QString const & token ) {
            const PortalCall created = callPortal(
                bus.connection(), &receiver,
                createSessionCall( token + QStringLiteral( "_request" ), token ), 2 );
            return resultMap( created.received.value( 0 ), 1 )
                .value( QStringLiteral( "session_handle" ) )
                .toString();
        };

        // Version: absent and wrong-typed.
        const auto readVersion = [ & ]() {
            QDBusMessage get = QDBusMessage::createMethodCall(
                TestSupport::FakePortal::serviceName(), TestSupport::FakePortal::portalPath(),
                QStringLiteral( "org.freedesktop.DBus.Properties" ), QStringLiteral( "Get" ) );
            get << TestSupport::FakePortal::portalInterface() << QStringLiteral( "version" );
            return callWithEvents( bus.connection(), get );
        };

        portal.setScenario( QStringLiteral( "no-version" ) );
        const QDBusMessage absentVersion = readVersion();
        QCOMPARE( absentVersion.type(), QDBusMessage::ErrorMessage );
        QVERIFY( absentVersion.errorName().contains( QLatin1String( "UnknownProperty" ) ) );

        portal.setScenario( QStringLiteral( "version-wrong-type" ) );
        const QDBusMessage wrongVersion = readVersion();
        QCOMPARE( wrongVersion.type(), QDBusMessage::ReplyMessage );
        QCOMPARE( wrongVersion.signature(), QStringLiteral( "v" ) );
        QVERIFY( wrongVersion.arguments().value( 0 ).value< QDBusVariant >().variant().metaType()
                 == QMetaType( QMetaType::QString ) );

        // CreateSession payload variants.
        const QString session = freshSession( QStringLiteral( "gd_malformed_base" ) );
        QVERIFY( !session.isEmpty() );

        portal.setScenario( QStringLiteral( "invalid-session-handle" ) );
        const PortalCall invalidHandle = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_invalid" ), QStringLiteral( "gd_invalid_session" ) ),
            2 );

        QCOMPARE( resultMap( invalidHandle.received.value( 0 ), 1 )
                      .value( QStringLiteral( "session_handle" ) )
                      .toString(),
                  QStringLiteral( "not-an-object-path" ) );

        portal.setScenario( QStringLiteral( "missing-session-handle" ) );
        const PortalCall missingHandle = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_missing" ), QStringLiteral( "gd_missing_session" ) ),
            2 );

        QVERIFY( resultMap( missingHandle.received.value( 0 ), 1 ).isEmpty() );

        // Response codes: denial and generic failure.
        portal.setScenario( QStringLiteral( "denied" ) );
        const PortalCall denied = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_denied" ), QStringLiteral( "gd_denied_session" ) ),
            2 );
        QCOMPARE( denied.received.value( 0 ).arguments.value( 0 ).toUInt(), 1u );

        portal.setScenario( QStringLiteral( "failed" ) );
        const PortalCall failed = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_failed" ), QStringLiteral( "gd_failed_session" ) ),
            2 );
        QCOMPARE( failed.received.value( 0 ).arguments.value( 0 ).toUInt(), 2u );

        // Method-level failures.
        portal.setScenario( QStringLiteral( "method-error" ) );
        const PortalCall methodError = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_method_error" ),
                               QStringLiteral( "gd_method_error_session" ) ),
            1 );
        QCOMPARE( methodError.reply.type(), QDBusMessage::ErrorMessage );
        QVERIFY( methodError.reply.errorName().contains( QLatin1String( "Failed" ) ) );

        portal.setScenario( QStringLiteral( "malformed-method-return" ) );
        const PortalCall malformedReply = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_malformed_return" ),
                               QStringLiteral( "gd_malformed_return_session" ) ),
            1 );
        QCOMPARE( malformedReply.reply.type(), QDBusMessage::ReplyMessage );
        QCOMPARE( malformedReply.reply.signature(), QStringLiteral( "s" ) );
        QVERIFY( replyPath( malformedReply.reply ).isEmpty() );

        // ListShortcuts payload variants.
        const auto listOnce = [ & ]( QString const & scenario ) {
            // A handle token is an object-path element: hyphens are not allowed,
            // so the fixture's scenarios keep their own token mapping here.
            QString token = QStringLiteral( "gd_list_" ) + scenario;
            token.replace( QLatin1Char( '-' ), QLatin1Char( '_' ) );

            portal.setScenario( scenario );
            return callPortal( bus.connection(), &receiver, listShortcutsCall( session, token ), 2 );
        };

        const PortalCall malformedTuple = listOnce( QStringLiteral( "list-malformed-tuple" ) );
        QVERIFY( resultMap( malformedTuple.received.value( 0 ), 1 )
                     .value( QStringLiteral( "shortcuts" ) )
                     .metaType()
                 == QMetaType::fromType< QStringList >() );
        QVERIFY( shortcutIdsOf( resultMap( malformedTuple.received.value( 0 ), 1 ) ).isEmpty() );

        const PortalCall malformedMap = listOnce( QStringLiteral( "list-malformed-map" ) );
        const QVariant bareMaps = resultMap( malformedMap.received.value( 0 ), 1 )
                                      .value( QStringLiteral( "shortcuts" ) );
        QVERIFY( bareMaps.metaType() == QMetaType::fromType< QDBusArgument >() );

        // `aa{sv}`: the entries are bare property maps, so no (id, map) tuple
        // can be read out of them at all.
        const QList< QVariantMap > maps = qdbus_cast< QList< QVariantMap > >( bareMaps );
        QCOMPARE( maps.size(), 2 );
        QVERIFY( maps.first().contains( QStringLiteral( "description" ) ) );
        QVERIFY( !maps.first().contains( QStringLiteral( "id" ) ) );

        const PortalCall duplicateIds = listOnce( QStringLiteral( "list-duplicate-ids" ) );
        const QStringList duplicated =
            shortcutIdsOf( resultMap( duplicateIds.received.value( 0 ), 1 ) );
        // The default listing has two entries; the malformed scenario repeats
        // the first one, which the client must reject as a duplicate id.
        QCOMPARE( duplicated.size(), 3 );
        QCOMPARE( duplicated.at( 0 ), duplicated.at( 2 ) );

        const PortalCall wrongProperty = listOnce( QStringLiteral( "list-property-wrong-type" ) );
        const QVariant propertyValue = resultMap( wrongProperty.received.value( 0 ), 1 )
                                           .value( QStringLiteral( "shortcuts" ) );
        QVERIFY( propertyValue.isValid() );
        QVERIFY( propertyValue.metaType() == QMetaType::fromType< QDBusArgument >() );

        const QList< TestSupport::FakePortal::Shortcut > wrongEntries =
            qdbus_cast< QList< TestSupport::FakePortal::Shortcut > >( propertyValue );
        QCOMPARE( wrongEntries.size(), 2 );
        QVERIFY( wrongEntries.first().properties.value( QStringLiteral( "description" ) ).metaType()
                 == QMetaType( QMetaType::UInt ) );

        // BindShortcuts payload variants (one fresh session each).
        const auto bindOnce = [ & ]( QString const & scenario, QStringList const & requested ) {
            QString suffix = scenario;
            suffix.replace( QLatin1Char( '-' ), QLatin1Char( '_' ) );

            portal.setScenario( QStringLiteral( "fast" ) );
            const QString fresh = freshSession( QStringLiteral( "gd_bind_case_" ) + suffix );
            portal.setScenario( scenario );
            return callPortal( bus.connection(), &receiver,
                               bindShortcutsCall( fresh, requested,
                                                  QStringLiteral( "gd_bind_" ) + suffix ),
                               2 );
        };

        const PortalCall partial = bindOnce(
            QStringLiteral( "bind-partial" ),
            { QStringLiteral( "main-window" ), QStringLiteral( "clipboard-lookup" ) } );
        QCOMPARE( shortcutIdsOf( resultMap( partial.received.value( 0 ), 1 ) ),
                  QStringList( { QStringLiteral( "main-window" ) } ) );

        const PortalCall empty = bindOnce( QStringLiteral( "bind-empty" ),
                                          { QStringLiteral( "main-window" ) } );
        QVERIFY( shortcutIdsOf( resultMap( empty.received.value( 0 ), 1 ) ).isEmpty() );

        const PortalCall unknown = bindOnce( QStringLiteral( "bind-unknown-id" ),
                                            { QStringLiteral( "main-window" ) } );
        const QStringList unknownIds =
            shortcutIdsOf( resultMap( unknown.received.value( 0 ), 1 ) );
        QCOMPARE( unknownIds.size(), 2 );
        QVERIFY( unknownIds.contains( QStringLiteral( "unknown-shortcut-id" ) ) );

        const PortalCall duplicatedBind = bindOnce( QStringLiteral( "bind-duplicate-id" ),
                                                   { QStringLiteral( "main-window" ) } );
        const QStringList duplicatedBindIds =
            shortcutIdsOf( resultMap( duplicatedBind.received.value( 0 ), 1 ) );
        QCOMPARE( duplicatedBindIds.size(), 2 );
        QCOMPARE( duplicatedBindIds.at( 0 ), duplicatedBindIds.at( 1 ) );

        const PortalCall wrongType = bindOnce( QStringLiteral( "bind-wrong-type" ),
                                              { QStringLiteral( "main-window" ) } );
        QVERIFY( resultMap( wrongType.received.value( 0 ), 1 )
                     .value( QStringLiteral( "shortcuts" ) )
                     .metaType()
                 == QMetaType::fromType< QStringList >() );

        const PortalCall bindDenied = bindOnce( QStringLiteral( "bind-denied" ),
                                               { QStringLiteral( "main-window" ) } );
        QCOMPARE( bindDenied.received.value( 0 ).arguments.value( 0 ).toUInt(), 1u );

        const PortalCall bindFailed = bindOnce( QStringLiteral( "bind-failed" ),
                                               { QStringLiteral( "main-window" ) } );
        QCOMPARE( bindFailed.received.value( 0 ).arguments.value( 0 ).toUInt(), 2u );

        // A configured subset (the returned-ID subset rule) and a portal-assigned
        // description that differs from the requested one.
        portal.setScenario( QStringLiteral( "fast" ) );
        TestSupport::FakePortal::Shortcut assigned;
        assigned.id = QStringLiteral( "main-window" );
        assigned.properties.insert( QStringLiteral( "description" ),
                                    QStringLiteral( "Assigned by portal" ) );
        assigned.properties.insert( QStringLiteral( "trigger_description" ),
                                    QStringLiteral( "portal-chosen" ) );
        portal.setBoundShortcuts( { assigned } );

        const QString subsetSession = freshSession( QStringLiteral( "gd_bound_subset" ) );
        const PortalCall assignedCall = callPortal(
            bus.connection(), &receiver,
            bindShortcutsCall( subsetSession, { QStringLiteral( "main-window" ) },
                               QStringLiteral( "gd_bound_subset_bind" ) ),
            2 );

        QCOMPARE( shortcutIdsOf( resultMap( assignedCall.received.value( 0 ), 1 ) ),
                  QStringList( { QStringLiteral( "main-window" ) } ) );

        const QList< TestSupport::FakePortal::Shortcut > assignedEntries =
            qdbus_cast< QList< TestSupport::FakePortal::Shortcut > >(
                resultMap( assignedCall.received.value( 0 ), 1 ).value(
                    QStringLiteral( "shortcuts" ) ) );

        QCOMPARE( assignedEntries.size(), 1 );
        QCOMPARE( assignedEntries.first().properties.value( QStringLiteral( "description" ) ).toString(),
                  QStringLiteral( "Assigned by portal" ) );
        QCOMPARE( assignedEntries.first()
                      .properties.value( QStringLiteral( "trigger_description" ) )
                      .toString(),
                  QStringLiteral( "portal-chosen" ) );

        // CreateSession payloads are also validated by the fixture itself: the
        // request path always lives under the portal's request subtree.
        QVERIFY( portal.requestPaths().size() > 10 );

        for ( QString const & path : portal.requestPaths() )
        {
            QVERIFY2( path.startsWith( TestSupport::FakePortal::portalPath()
                                       + QStringLiteral( "/request/" ) ),
                      qPrintable( path ) );
        }

        QCOMPARE( portal.sessionPath(), subsetSession );
    }

    bus.stop();
}

void TstUtf8Wstring::fakePortalCoversResponseAndLossScenarios()
{
    TestSupport::PrivateSessionBus bus;
    QString reason;
    QVERIFY2( bus.start( &reason ), qPrintable( reason ) );

    {
        TestSupport::FakePortal portal;
        QVERIFY2( portal.start( bus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );

        PortalSignalReceiver receiver;
        subscribePortalSignals( bus.connection(), &receiver );

        const QString base = bus.connection().baseService();

        // no-response: the method reply arrives, the Response never does.
        portal.setScenario( QStringLiteral( "no-response" ) );
        const PortalCall silent = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_silent" ), QStringLiteral( "gd_silent_session" ) ),
            1 );
        QCOMPARE( silent.events, QStringList( { QStringLiteral( "reply" ) } ) );
        QVERIFY( QTest::qWaitFor( [ &receiver ]() { return receiver.events.size() > 1; }, 300 )
                 == false );

        // no-method-reply: neither a reply nor a Response ever arrives, so the
        // client has to enforce its own deadline.
        portal.setScenario( QStringLiteral( "no-method-reply" ) );
        receiver.events.clear();
        receiver.entries.clear();
        const int neverCallsBefore = portal.receivedCallCount();

        QDBusPendingCall pending = bus.connection().asyncCall(
            createSessionCall( QStringLiteral( "gd_never" ), QStringLiteral( "gd_never_session" ) ) );
        // Deterministic: wait until the portal has actually received *this* call,
        // then assert nothing came back. A sleep would only hope it had arrived.
        QVERIFY( QTest::qWaitFor(
            [ &portal, neverCallsBefore ]() { return portal.receivedCallCount() > neverCallsBefore; },
            5000 ) );
        QVERIFY( receiver.events.isEmpty() );
        QVERIFY( !pending.isFinished() );

        // delayed-response: the reply arrives, the Response waits for
        // releasePending().
        portal.setScenario( QStringLiteral( "delayed-response" ) );
        const PortalCall delayed = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_delayed" ), QStringLiteral( "gd_delayed_session" ) ),
            1 );
        QCOMPARE( delayed.events, QStringList( { QStringLiteral( "reply" ) } ) );

        portal.releasePending();
        QVERIFY( QTest::qWaitFor( [ &receiver ]() { return receiver.events.size() >= 2; }, 5000 ) );
        QCOMPARE( receiver.events.last(), QStringLiteral( "Response" ) );
        QCOMPARE( receiver.entries.last().arguments.at( 0 ).toUInt(), 0u );

        // delayed-method-reply: both the reply and the Response are held, so a
        // client can give up first and still reconcile the late answer.
        portal.setScenario( QStringLiteral( "delayed-method-reply" ) );
        receiver.events.clear();
        receiver.entries.clear();

        // The portal has served earlier scenarios, so the predicate counts from
        // the call baseline rather than from zero.
        const int callsBefore = portal.receivedCallCount();

        QDBusPendingCallWatcher deferredWatcher( bus.connection().asyncCall(
            createSessionCall( QStringLiteral( "gd_deferred" ),
                               QStringLiteral( "gd_deferred_session" ) ) ) );

        QVERIFY( QTest::qWaitFor(
            [ &portal, callsBefore ]() { return portal.receivedCallCount() > callsBefore; }, 5000 ) );
        QVERIFY2( receiver.events.isEmpty(), "the delayed method reply arrived before releasePending()" );
        QVERIFY( !deferredWatcher.isFinished() );

        portal.releasePending();
        QVERIFY( QTest::qWaitFor( [ &receiver ]() { return receiver.events.size() >= 1; }, 5000 ) );
        QCOMPARE( receiver.events.last(), QStringLiteral( "Response" ) );
        QCOMPARE( receiver.entries.last().path,
                  predictedRequestPath( base, QStringLiteral( "gd_deferred" ) ) );
        QVERIFY( QTest::qWaitFor( [ &deferredWatcher ]() { return deferredWatcher.isFinished(); },
                                  5000 ) );
        QCOMPARE( replyPath( deferredWatcher.reply() ),
                  predictedRequestPath( base, QStringLiteral( "gd_deferred" ) ) );

        // late-create-response: the session only exists after releasePending(),
        // after the request that asked for it is long gone. The client must close
        // that late session instead of adopting it.
        portal.setScenario( QStringLiteral( "late-create-response" ) );
        const PortalCall late = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_late" ), QStringLiteral( "gd_late_session" ) ),
            1 );

        QCOMPARE( late.events, QStringList( { QStringLiteral( "reply" ) } ) );
        const QString sessionBeforeLate = portal.sessionPath();
        QCOMPARE( portal.sessionPath(), sessionBeforeLate );

        portal.releasePending();
        QVERIFY( QTest::qWaitFor( [ &receiver ]() { return receiver.events.size() >= 2; }, 5000 ) );

        const QString lateSession = portal.sessionPath();
        QVERIFY( !lateSession.isEmpty() );
        QCOMPARE( resultMap( receiver.entries.last() )
                      .value( QStringLiteral( "session_handle" ) )
                      .toString(),
                  lateSession );

        // The late session can be closed by the late cleanup path, and the Close
        // is recorded.
        callWithEvents( bus.connection(),
                        closeCall( lateSession, TestSupport::FakePortal::sessionInterface() ) );
        QCOMPARE( portal.count( QStringLiteral( "Session.Close" ) ), 1 );

        // configure-error: the asynchronous method result carries the failure and
        // still produces no Request lifecycle.
        portal.setScenario( QStringLiteral( "fast" ) );
        const PortalCall sessionCall = callPortal(
            bus.connection(), &receiver,
            createSessionCall( QStringLiteral( "gd_configure" ),
                               QStringLiteral( "gd_configure_session" ) ),
            2 );
        const QString session = resultMap( sessionCall.received.value( 0 ), 1 )
                                    .value( QStringLiteral( "session_handle" ) )
                                    .toString();

        const int requestsBefore = portal.requestPaths().size();
        portal.setScenario( QStringLiteral( "configure-error" ) );
        const PortalCall configureError = callPortal(
            bus.connection(), &receiver,
            configureShortcutsCall( session, QStringLiteral( "token-untouched" ) ), 1 );

        QCOMPARE( configureError.reply.type(), QDBusMessage::ErrorMessage );
        QVERIFY( configureError.reply.errorName().contains( QLatin1String( "Failed" ) ) );
        QCOMPARE( portal.requestPaths().size(), requestsBefore );
        QCOMPARE( portal.calls().last().activationToken, QStringLiteral( "token-untouched" ) );

        // Service loss invalidates every handle on the client side and is
        // observable here.
        portal.loseService();
        QVERIFY( !portal.isRegistered() );
        QVERIFY( portal.serviceOwner().isEmpty() );

        // A late Close after the service disappeared is answered by the daemon
        // with an error, never by the retired fixture.
        const QDBusMessage lateClose =
            callWithEvents( bus.connection(),
                            closeCall( session, TestSupport::FakePortal::sessionInterface() ), 2000 );
        QCOMPARE( lateClose.type(), QDBusMessage::ErrorMessage );
    }

    bus.stop();
    QVERIFY( !bus.isRunning() );
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
