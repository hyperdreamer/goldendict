/* This file is part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#include <QtTest>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <vector>

#include "portalhotkeys.hh"
#include "support/fake_portal.hh"
#include "support/fixtures.hh"

namespace {
QString const Token = QStringLiteral( "gd_test_request" );
QString const SessionToken = QStringLiteral( "gd_test_session" );

QDBusMessage createSession( QString const & token = Token, QString const & sessionToken = SessionToken )
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        TestSupport::FakePortal::serviceName(), TestSupport::FakePortal::portalPath(),
        TestSupport::FakePortal::portalInterface(), QStringLiteral( "CreateSession" ) );
    message << QVariantMap{ { QStringLiteral( "handle_token" ), token },
                             { QStringLiteral( "session_handle_token" ), sessionToken } };
    return message;
}

QString pathFor( QDBusConnection const & bus, QString const & kind, QString const & token )
{
    QString sender = bus.baseService().mid( 1 );
    sender.replace( QLatin1Char( '.' ), QLatin1Char( '_' ) );
    sender.replace( QLatin1Char( '-' ), QLatin1Char( '_' ) );
    return TestSupport::FakePortal::portalPath() + '/' + kind + '/' + sender + '/' + token;
}

bool sendResponse( QDBusConnection const & sender, QString const & path,
                   QVariant const & code, QVariant const & results )
{
    QDBusMessage message = QDBusMessage::createSignal(
        path, TestSupport::FakePortal::requestInterface(), QStringLiteral( "Response" ) );
    message << code << results;
    return sender.send( message );
}

struct Outcome : QObject
{
    QSignalSpy responses;
    QSignalSpy failures;
    QStringList sequence;
    QElapsedTimer elapsed;
    qint64 completedAt = -1;
    explicit Outcome( PortalRequest & request ):
        responses( &request, &PortalRequest::response ), failures( &request, &PortalRequest::failed )
    {
        elapsed.start();
        QObject::connect( &request, &PortalRequest::response, this,
                          [ this ]() { completedAt = elapsed.elapsed(); sequence.append( QStringLiteral( "response" ) ); } );
        QObject::connect( &request, &PortalRequest::failed, this,
                          [ this ]() { completedAt = elapsed.elapsed(); sequence.append( QStringLiteral( "failed" ) ); } );
    }
    int count() const { return responses.size() + failures.size(); }
};

/// Test-local extension for combinations absent from the shared FakePortal:
/// explicitly release only the method reply, on an arbitrary returned path.
class GatedPortal : public QDBusVirtualObject
{
public:
    explicit GatedPortal( QDBusConnection const & connection ): bus( connection ) {}
    ~GatedPortal() override
    {
        if ( registered )
        {
            bus.unregisterService( TestSupport::FakePortal::serviceName() );
            bus.unregisterObject( TestSupport::FakePortal::portalPath() );
        }
    }
    bool start()
    {
        registered = bus.registerVirtualObject( TestSupport::FakePortal::portalPath(), this,
                                                QDBusConnection::SubPath );
        return registered && bus.registerService( TestSupport::FakePortal::serviceName() );
    }
    QString introspect( QString const & ) const override { return QStringLiteral( "<node/>" ); }
    bool handleMessage( QDBusMessage const & message, QDBusConnection const & ) override
    {
        if ( message.member() == QLatin1String( "CreateSession" ) )
        {
            held = message;
            held.setDelayedReply( true );
            ++creates;
            return true;
        }
        if ( message.member() != QLatin1String( "Close" ) )
            return false;
        if ( message.interface() == TestSupport::FakePortal::requestInterface() )
            requestCloses.append( message.path() );
        else
            sessionCloses.append( message.path() );
        bus.send( message.createReply() );
        return true;
    }
    bool releaseMethod( QString const & path )
    {
        QDBusMessage reply = held.createReply();
        reply << QVariant::fromValue( QDBusObjectPath( path ) );
        return bus.send( reply );
    }
    QDBusConnection bus;
    QDBusMessage held;
    int creates = 0;
    QStringList requestCloses, sessionCloses;
    bool registered = false;
};
}

class TstPortalHotkeys : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void scenarioCompletesExactlyOnce_data();
    void scenarioCompletesExactlyOnce();
    void silentRequestsShareOneWallDeadline();
    void delayedMethodDoesNotRestartDeadline();
    void deadlineClosesLateMethodAndSession();
    void cleanupObserversExpireWithoutResponse();
    void pathMatchingIgnoresUnrelatedResponse();
    void predictedAndReturnedPathsAreBothAccepted();
    void wrongWireTypesCannotComplete();
    void cancelBeforeMethodClosesReturnedPath();
    void cancelBeforeAlternateMethodClosesActualPath();
    void cancelClosesLateSessionWithoutCompletion();
    void destroyedRequestStillClosesLateSession();
    void retiredGenerationCannotCompleteCurrentRequest();
    void staleOwnerCannotCompleteCurrentRequest();
    void ownerChangeFailsOnceAndInvalidatesCleanup();
    void invalidTokensAreReplacedWithValidUniqueTokens();
    void startAndCancelAreIdempotent();
    void missingServiceFailsWithinDeadline();
    void responseHandlerMayDestroyRequest();
    void portalWireTypeSignature();
    void wireBarrier( uint marker ) { seenMarker = marker; }

private:
    bool flushWire( QDBusConnection const & sender );
    bool waitForCall( int total = 1 );
    bool noCleanupObservers() const;
    std::unique_ptr< TestSupport::PrivateSessionBus > bus;
    std::unique_ptr< TestSupport::FakePortal > portal;
    uint nextMarker = 0;
    uint seenMarker = 0;
};

void TstPortalHotkeys::initTestCase()
{
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
    TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
}

void TstPortalHotkeys::cleanupTestCase()
{
    TestSupport::clearWatchdog();
}

void TstPortalHotkeys::init()
{
    bus = std::make_unique< TestSupport::PrivateSessionBus >();
    QString reason;
    QVERIFY2( bus->start( &reason ), qPrintable( reason ) );
    portal = std::make_unique< TestSupport::FakePortal >();
    QVERIFY2( portal->start( bus->fixtureConnection(), 2 ), qPrintable( portal->errorString() ) );
    QVERIFY( bus->connection().baseService() != bus->fixtureConnection().baseService() );
    QVERIFY( bus->address() != qEnvironmentVariable( "DBUS_SESSION_BUS_ADDRESS" ) );
    QVERIFY( bus->connection().connect( QString(), QStringLiteral( "/gd_test" ),
                QStringLiteral( "org.goldendict.Test" ), QStringLiteral( "Barrier" ),
                this, SLOT( wireBarrier( uint ) ) ) );
}

bool TstPortalHotkeys::noCleanupObservers() const
{
    for ( QObject * child : QCoreApplication::instance()->children() )
        if ( QByteArray( child->metaObject()->className() ) == "PortalRequestState" )
            return false;
    return true;
}

void TstPortalHotkeys::cleanup()
{
    // Stack requests are already destroyed. Retained cleanup observers see the
    // owner's loss and die before the private daemon and connections do.
    portal.reset();
    QVERIFY( QTest::qWaitFor( [ this ]() { return noCleanupObservers(); }, 1000 ) );
    bus->stop();
    QVERIFY( !bus->isRunning() );
    QVERIFY( !bus->connection().isConnected() );
    bus.reset();
}

bool TstPortalHotkeys::waitForCall( int total )
{
    return QTest::qWaitFor( [ this, total ]() { return portal->receivedCallCount() >= total; }, 1000 );
}

bool TstPortalHotkeys::flushWire( QDBusConnection const & sender )
{
    const uint marker = ++nextMarker;
    QDBusMessage message = QDBusMessage::createSignal(
        QStringLiteral( "/gd_test" ), QStringLiteral( "org.goldendict.Test" ), QStringLiteral( "Barrier" ) );
    message << marker;
    return sender.send( message ) && QTest::qWaitFor( [ this, marker ]() { return seenMarker == marker; }, 1000 );
}

void TstPortalHotkeys::scenarioCompletesExactlyOnce_data()
{
    QTest::addColumn< QString >( "scenario" );
    QTest::addColumn< int >( "code" );
    QTest::addColumn< QString >( "error" );
    for ( char const * name : { "fast", "early-response", "early-alternate-response",
                                "alternate-response", "duplicate-response" } )
        QTest::newRow( name ) << QString::fromLatin1( name ) << 0 << QString();
    QTest::newRow( "denied" ) << QStringLiteral( "denied" ) << 1 << QString();
    QTest::newRow( "failed" ) << QStringLiteral( "failed" ) << 2 << QString();
    QTest::newRow( "malformed-method-return" ) << QStringLiteral( "malformed-method-return" ) << -1
        << QStringLiteral( "Portal method returned an invalid request path" );
    QTest::newRow( "method-error" ) << QStringLiteral( "method-error" ) << -1
        << QStringLiteral( "Portal method failed: fixture method failure" );
    for ( char const * name : { "invalid-session-handle", "missing-session-handle" } )
        QTest::newRow( name ) << QString::fromLatin1( name ) << -1
            << QStringLiteral( "Portal response has an invalid session_handle" );
}

void TstPortalHotkeys::scenarioCompletesExactlyOnce()
{
    QFETCH( QString, scenario );
    QFETCH( int, code );
    QFETCH( QString, error );
    QVERIFY( TestSupport::FakePortal::scenarios().contains( scenario ) );
    portal->setScenario( scenario );
    PortalRequest request( bus->connection(), 7 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    QVERIFY( QTest::qWaitFor( [ &outcome ]() { return outcome.count() == 1; }, 5500 ) );
    QCOMPARE( portal->count( QStringLiteral( "CreateSession" ) ), 1 );
    if ( code >= 0 )
    {
        QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "response" ) } );
        QCOMPARE( outcome.failures.count(), 0 );
        const auto values = outcome.responses.first();
        QCOMPARE( values.at( 0 ).toULongLong(), qulonglong( 7 ) );
        QCOMPARE( values.at( 1 ).toString(), portal->requestPaths().first() );
        QCOMPARE( values.at( 2 ).toUInt(), uint( code ) );
        if ( code == 0 )
            QCOMPARE( values.at( 3 ).toMap().value( QStringLiteral( "session_handle" ) ).toString(),
                      pathFor( bus->connection(), QStringLiteral( "session" ), SessionToken ) );
    }
    else
    {
        QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "failed" ) } );
        QCOMPARE( outcome.responses.count(), 0 );
        QCOMPARE( outcome.failures.first().at( 0 ).toULongLong(), qulonglong( 7 ) );
        QCOMPARE( outcome.failures.first().at( 1 ).toString(), error );
    }
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    request.cancel();
    request.cancel();
    portal->releasePending();
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( outcome.count(), 1 );
}

void TstPortalHotkeys::silentRequestsShareOneWallDeadline()
{
    // Run silent scenarios together so there is one elapsed bound for the whole
    // test, including owner lookup and method reply, rather than chained waits.
    std::vector< std::unique_ptr< PortalRequest > > requests;
    std::vector< std::unique_ptr< Outcome > > outcomes;
    QElapsedTimer elapsed;
    elapsed.start();
    for ( char const * scenario : { "no-method-reply", "no-response", "unrelated-response" } )
    {
        portal->setScenario( QString::fromLatin1( scenario ) );
        const QString token = QStringLiteral( "gd_silent_%1" ).arg( requests.size() );
        requests.push_back( std::make_unique< PortalRequest >( bus->connection(), 20 + requests.size() ) );
        outcomes.push_back( std::make_unique< Outcome >( *requests.back() ) );
        requests.back()->start( createSession( token ), token );
        QVERIFY( QTest::qWaitFor( [ this, &requests ]() {
            return portal->count( QStringLiteral( "CreateSession" ) ) == int( requests.size() );
        }, 1000 ) );
    }
    QVERIFY( QTest::qWaitFor( [ &outcomes ]() {
        for ( auto const & outcome : outcomes )
            if ( outcome->count() != 1 )
                return false;
        return true;
    }, 5500 ) );
    QVERIFY2( elapsed.elapsed() < 5200, qPrintable( QString::number( elapsed.elapsed() ) ) );
    for ( size_t i = 0; i < outcomes.size(); ++i )
    {
        auto const & outcome = *outcomes[ i ];
        QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "failed" ) } );
        QVERIFY2( outcome.completedAt < 5000, qPrintable( QString::number( outcome.completedAt ) ) );
        QCOMPARE( outcome.responses.count(), 0 );
        QCOMPARE( outcome.failures.first().at( 0 ).toULongLong(), qulonglong( 20 + i ) );
        QCOMPARE( outcome.failures.first().at( 1 ).toString(), QStringLiteral( "Portal request deadline exceeded" ) );
    }
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Request.Close" ) ) == 3; }, 1000 ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    for ( auto const & outcome : outcomes )
        QCOMPARE( outcome->count(), 1 );
}

void TstPortalHotkeys::delayedMethodDoesNotRestartDeadline()
{
    portal.reset();
    GatedPortal gated( bus->fixtureConnection() );
    QVERIFY( gated.start() );
    PortalRequest request( bus->connection(), 31 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    QVERIFY( QTest::qWaitFor( [ &gated ]() { return gated.creates == 1; }, 1000 ) );
    const QString path = pathFor( bus->connection(), QStringLiteral( "request" ), Token );
    bool released = false;
    QTimer release;
    release.setSingleShot( true );
    connect( &release, &QTimer::timeout, this, [ &gated, &released, path ]() {
        released = gated.releaseMethod( path );
    } );
    release.start( 2500 );
    QVERIFY( QTest::qWaitFor( [ &outcome ]() { return outcome.count() == 1; }, 5500 ) );
    QVERIFY( released );
    QVERIFY2( outcome.completedAt < 5000, qPrintable( QString::number( outcome.completedAt ) ) );
    QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "failed" ) } );
    QCOMPARE( outcome.failures.first().at( 0 ).toULongLong(), qulonglong( 31 ) );
    QCOMPARE( outcome.failures.first().at( 1 ).toString(), QStringLiteral( "Portal request deadline exceeded" ) );
    QVERIFY( QTest::qWaitFor( [ &gated ]() { return gated.requestCloses.size() == 1; }, 1000 ) );
    const QString session = pathFor( bus->connection(), QStringLiteral( "session" ), QStringLiteral( "gd_late_deadline" ) );
    const QVariantMap results{ { QStringLiteral( "session_handle" ), session } };
    QVERIFY( sendResponse( bus->fixtureConnection(), path, uint( 0 ), results ) );
    QVERIFY( QTest::qWaitFor( [ &gated ]() { return gated.sessionCloses.size() == 1; }, 1000 ) );
    QCOMPARE( gated.sessionCloses.first(), session );
    QCOMPARE( outcome.count(), 1 );
}

void TstPortalHotkeys::deadlineClosesLateMethodAndSession()
{
    portal->setScenario( QStringLiteral( "delayed-method-reply" ) );
    PortalRequest request( bus->connection(), 32 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    QVERIFY( waitForCall() );
    QVERIFY( QTest::qWaitFor( [ &outcome ]() { return outcome.count() == 1; }, 5500 ) );
    QVERIFY2( outcome.completedAt < 5000, qPrintable( QString::number( outcome.completedAt ) ) );
    QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "failed" ) } );
    QCOMPARE( outcome.failures.first().at( 0 ).toULongLong(), qulonglong( 32 ) );
    QCOMPARE( outcome.failures.first().at( 1 ).toString(), QStringLiteral( "Portal request deadline exceeded" ) );
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Request.Close" ) ) == 1; }, 1000 ) );
    portal->releasePending();
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Session.Close" ) ) == 1; }, 1000 ) );
    QCOMPARE( portal->calls().last().session, portal->sessionPath() );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( portal->count( QStringLiteral( "Request.Close" ) ), 1 );
    QCOMPARE( outcome.count(), 1 );
}

void TstPortalHotkeys::cleanupObserversExpireWithoutResponse()
{
    portal->setScenario( QStringLiteral( "no-method-reply" ) );
    auto noReply = std::make_unique< PortalRequest >( bus->connection(), 33 );
    Outcome noReplyOutcome( *noReply );
    noReply->start( createSession(), Token );
    QVERIFY( waitForCall() );
    portal->setScenario( QStringLiteral( "no-response" ) );
    const QString token = QStringLiteral( "gd_no_response_cleanup" );
    auto noResponse = std::make_unique< PortalRequest >( bus->connection(), 34 );
    Outcome noResponseOutcome( *noResponse );
    noResponse->start( createSession( token ), token );
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "CreateSession" ) ) == 2; }, 1000 ) );
    QElapsedTimer elapsed;
    elapsed.start();
    noReply.reset();
    noResponse.reset();
    QVERIFY( !noCleanupObservers() );
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Request.Close" ) ) == 2; }, 1000 ) );
    // Keep the portal owner alive: cleanup must expire by its own bound even
    // when neither Close nor the original call ever produces another event.
    QVERIFY( QTest::qWaitFor( [ this ]() { return noCleanupObservers(); }, 5500 ) );
    QVERIFY( elapsed.elapsed() < 5200 );
    QCOMPARE( noReplyOutcome.count(), 0 );
    QCOMPARE( noResponseOutcome.count(), 0 );
    QCOMPARE( portal->count( QStringLiteral( "Request.Close" ) ), 2 );
    QCOMPARE( portal->count( QStringLiteral( "Session.Close" ) ), 0 );
}

void TstPortalHotkeys::pathMatchingIgnoresUnrelatedResponse()
{
    portal->setScenario( QStringLiteral( "unrelated-response" ) );
    PortalRequest request( bus->connection(), 40 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    QVERIFY( waitForCall() );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( outcome.count(), 0 );
    const QVariantMap results{ { QStringLiteral( "session_handle" ), portal->sessionPath() } };
    QVERIFY( sendResponse( bus->fixtureConnection(), portal->requestPaths().first(), uint( 0 ), results ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "response" ) } );
    QCOMPARE( outcome.responses.first().at( 0 ).toULongLong(), qulonglong( 40 ) );
    QCOMPARE( outcome.responses.first().at( 1 ).toString(), portal->requestPaths().first() );
    QCOMPARE( outcome.failures.count(), 0 );
}

void TstPortalHotkeys::predictedAndReturnedPathsAreBothAccepted()
{
    portal.reset();
    GatedPortal gated( bus->fixtureConnection() );
    QVERIFY( gated.start() );
    PortalRequest request( bus->connection(), 41 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    QVERIFY( QTest::qWaitFor( [ &gated ]() { return gated.creates == 1; }, 1000 ) );
    const QString predicted = pathFor( bus->connection(), QStringLiteral( "request" ), Token );
    const QString actual = predicted + QStringLiteral( "_alternate" );
    const QVariantMap results{ { QStringLiteral( "session_handle" ), QStringLiteral( "/predicted_session" ) } };
    QVERIFY( sendResponse( bus->fixtureConnection(), predicted, uint( 0 ), results ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( outcome.count(), 0 ); // A predicted signal alone cannot preempt the method reply.
    QVERIFY( gated.releaseMethod( actual ) );
    QVERIFY( QTest::qWaitFor( [ &outcome ]() { return outcome.count() == 1; }, 1000 ) );
    QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "response" ) } );
    QCOMPARE( outcome.responses.first().at( 1 ).toString(), actual );
    QVERIFY( sendResponse( bus->fixtureConnection(), actual, uint( 0 ), results ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( outcome.count(), 1 );
}

void TstPortalHotkeys::wrongWireTypesCannotComplete()
{
    portal->setScenario( QStringLiteral( "no-response" ) );
    PortalRequest request( bus->connection(), 42 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    QVERIFY( waitForCall() );
    const QString path = portal->requestPaths().first();
    const QVariantMap valid{ { QStringLiteral( "session_handle" ), portal->sessionPath() } };
    QVERIFY( sendResponse( bus->fixtureConnection(), path, QStringLiteral( "0" ), valid ) );
    QVERIFY( sendResponse( bus->fixtureConnection(), path, uint( 0 ), QStringLiteral( "not-a-map" ) ) );
    const QVariantMap wrongSession{ { QStringLiteral( "session_handle" ),
                                      QVariant::fromValue( QDBusObjectPath( portal->sessionPath() ) ) } };
    // Wrong envelope types are ignored. A structurally valid success envelope
    // with the wrong session type is a protocol failure, never a success.
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( outcome.count(), 0 );
    QVERIFY( sendResponse( bus->fixtureConnection(), path, uint( 0 ), wrongSession ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "failed" ) } );
    QCOMPARE( outcome.failures.first().at( 1 ).toString(), QStringLiteral( "Portal response has an invalid session_handle" ) );
    QVERIFY( sendResponse( bus->fixtureConnection(), path, uint( 0 ), valid ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( outcome.count(), 1 );
}

void TstPortalHotkeys::cancelBeforeMethodClosesReturnedPath()
{
    portal->setScenario( QStringLiteral( "delayed-method-reply" ) );
    PortalRequest request( bus->connection(), 50 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    QVERIFY( waitForCall() );
    QCOMPARE( outcome.count(), 0 );
    request.cancel();
    request.cancel();
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Request.Close" ) ) == 1; }, 1000 ) );
    QCOMPARE( outcome.count(), 0 ); // Close does not promise a Response.
    portal->releasePending();
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Session.Close" ) ) == 1; }, 1000 ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( portal->count( QStringLiteral( "Request.Close" ) ), 1 );
    QCOMPARE( outcome.count(), 0 );
    for ( auto const & call : portal->calls() )
    {
        if ( call.member == QLatin1String( "Request.Close" ) )
            QCOMPARE( call.requestPath, portal->requestPaths().first() );
        if ( call.member == QLatin1String( "Session.Close" ) )
            QCOMPARE( call.session, portal->sessionPath() );
    }
}

void TstPortalHotkeys::cancelBeforeAlternateMethodClosesActualPath()
{
    portal.reset();
    GatedPortal gated( bus->fixtureConnection() );
    QVERIFY( gated.start() );
    PortalRequest request( bus->connection(), 53 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    QVERIFY( QTest::qWaitFor( [ &gated ]() { return gated.creates == 1; }, 1000 ) );
    const QString predicted = pathFor( bus->connection(), QStringLiteral( "request" ), Token );
    const QString actual = predicted + QStringLiteral( "_late_alternate" );
    request.cancel();
    QVERIFY( QTest::qWaitFor( [ &gated ]() { return gated.requestCloses.size() == 1; }, 1000 ) );
    QCOMPARE( gated.requestCloses.first(), predicted );
    QVERIFY( gated.releaseMethod( actual ) );
    QVERIFY( QTest::qWaitFor( [ &gated ]() { return gated.requestCloses.size() == 2; }, 1000 ) );
    QCOMPARE( gated.requestCloses.last(), actual );
    QCOMPARE( outcome.count(), 0 ); // Closing the actual handle needs no Response.
    const QString session = pathFor( bus->connection(), QStringLiteral( "session" ), QStringLiteral( "gd_late_alternate" ) );
    const QVariantMap results{ { QStringLiteral( "session_handle" ), session } };
    QVERIFY( sendResponse( bus->fixtureConnection(), actual, uint( 0 ), results ) );
    QVERIFY( QTest::qWaitFor( [ &gated ]() { return gated.sessionCloses.size() == 1; }, 1000 ) );
    QCOMPARE( gated.sessionCloses.first(), session );
    QVERIFY( sendResponse( bus->fixtureConnection(), actual, uint( 0 ), results ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( gated.requestCloses.size(), 2 );
    QCOMPARE( gated.sessionCloses.size(), 1 );
    QCOMPARE( outcome.count(), 0 );
}

void TstPortalHotkeys::cancelClosesLateSessionWithoutCompletion()
{
    portal->setScenario( QStringLiteral( "late-create-response" ) );
    PortalRequest request( bus->connection(), 51 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    QVERIFY( waitForCall() );
    QVERIFY( portal->sessionPath().isEmpty() );
    request.cancel();
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Request.Close" ) ) == 1; }, 1000 ) );
    QCOMPARE( outcome.count(), 0 );
    portal->releasePending();
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Session.Close" ) ) == 1; }, 1000 ) );
    QCOMPARE( portal->calls().last().session, portal->sessionPath() );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( outcome.count(), 0 );
}

void TstPortalHotkeys::destroyedRequestStillClosesLateSession()
{
    portal->setScenario( QStringLiteral( "late-create-response" ) );
    auto request = std::make_unique< PortalRequest >( bus->connection(), 52 );
    int completions = 0;
    connect( request.get(), &PortalRequest::response, this, [ &completions ]() { ++completions; } );
    connect( request.get(), &PortalRequest::failed, this, [ &completions ]() { ++completions; } );
    request->start( createSession(), Token );
    QVERIFY( waitForCall() );
    request.reset();
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Request.Close" ) ) == 1; }, 1000 ) );
    portal->releasePending();
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Session.Close" ) ) == 1; }, 1000 ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( completions, 0 );
    QVERIFY( noCleanupObservers() );
}

void TstPortalHotkeys::retiredGenerationCannotCompleteCurrentRequest()
{
    portal->setScenario( QStringLiteral( "late-create-response" ) );
    PortalRequest old( bus->connection(), 60 );
    Outcome retired( old );
    old.start( createSession(), Token );
    QVERIFY( waitForCall() );
    old.cancel();
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Request.Close" ) ) == 1; }, 1000 ) );
    portal->setScenario( QStringLiteral( "early-alternate-response" ) );
    const QString token = QStringLiteral( "gd_current_generation" );
    PortalRequest current( bus->connection(), 61 );
    Outcome active( current );
    current.start( createSession( token, QStringLiteral( "gd_current_session" ) ), token );
    QVERIFY( QTest::qWaitFor( [ &active ]() { return active.count() == 1; }, 1000 ) );
    portal->releasePending();
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( retired.count(), 0 );
    QCOMPARE( active.sequence, QStringList{ QStringLiteral( "response" ) } );
    QCOMPARE( active.responses.first().at( 0 ).toULongLong(), qulonglong( 61 ) );
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Session.Close" ) ) == 1; }, 1000 ) );
    QCOMPARE( portal->calls().last().session,
              pathFor( bus->connection(), QStringLiteral( "session" ), SessionToken ) );
}

void TstPortalHotkeys::staleOwnerCannotCompleteCurrentRequest()
{
    const QString extraName = QStringLiteral( "gd-t30-replacement" );
    // Keep the old owner's unique connection alive after relinquishing its name.
    portal->loseService();
    QDBusConnection replacementBus = QDBusConnection::connectToBus( bus->address(), extraName );
    QVERIFY( replacementBus.isConnected() );
    {
        TestSupport::FakePortal replacement;
        QVERIFY2( replacement.start( replacementBus, 2 ), qPrintable( replacement.errorString() ) );
        replacement.setScenario( QStringLiteral( "no-response" ) );
        PortalRequest current( bus->connection(), 70 );
        Outcome outcome( current );
        current.start( createSession(), Token );
        QVERIFY( QTest::qWaitFor( [ &replacement ]() { return replacement.receivedCallCount() == 1; }, 1000 ) );
        const QString path = replacement.requestPaths().first();
        const QVariantMap forged{ { QStringLiteral( "session_handle" ), QStringLiteral( "/stale_session" ) } };
        QVERIFY( sendResponse( bus->fixtureConnection(), path, uint( 0 ), forged ) );
        QVERIFY( flushWire( bus->fixtureConnection() ) );
        QCOMPARE( outcome.count(), 0 );
        const QVariantMap valid{ { QStringLiteral( "session_handle" ), replacement.sessionPath() } };
        QVERIFY( sendResponse( replacementBus, path, uint( 0 ), valid ) );
        QVERIFY( flushWire( replacementBus ) );
        QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "response" ) } );
        QCOMPARE( outcome.responses.first().at( 0 ).toULongLong(), qulonglong( 70 ) );
        QCOMPARE( outcome.responses.first().at( 3 ).toMap(), valid );
    }
    QDBusConnection::disconnectFromBus( extraName );
}

void TstPortalHotkeys::ownerChangeFailsOnceAndInvalidatesCleanup()
{
    portal->setScenario( QStringLiteral( "delayed-method-reply" ) );
    PortalRequest request( bus->connection(), 71 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    QVERIFY( waitForCall() );
    portal->loseService();
    QVERIFY( QTest::qWaitFor( [ &outcome ]() { return outcome.count() == 1; }, 1000 ) );
    QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "failed" ) } );
    QCOMPARE( outcome.failures.first().at( 0 ).toULongLong(), qulonglong( 71 ) );
    QCOMPARE( outcome.failures.first().at( 1 ).toString(), QStringLiteral( "Portal service owner changed" ) );
    portal->releasePending();
    request.cancel();
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( outcome.count(), 1 );
    QCOMPARE( portal->count( QStringLiteral( "Request.Close" ) ), 0 );
    QCOMPARE( portal->count( QStringLiteral( "Session.Close" ) ), 0 );
}

void TstPortalHotkeys::invalidTokensAreReplacedWithValidUniqueTokens()
{
    for ( int i = 0; i < 2; ++i )
    {
        PortalRequest request( bus->connection(), 80 + i );
        Outcome outcome( request );
        request.start( createSession( QStringLiteral( "not/a/token" ), QStringLiteral( "also/bad" ) ),
                       QStringLiteral( "not/a/token" ) );
        QVERIFY( QTest::qWaitFor( [ &outcome ]() { return outcome.count() == 1; }, 1000 ) );
        QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "response" ) } );
        const auto call = portal->calls().last();
        const QString token = call.options.value( QStringLiteral( "handle_token" ) ).toString();
        const QString session = call.options.value( QStringLiteral( "session_handle_token" ) ).toString();
        const QRegularExpression valid( QStringLiteral( "^[a-zA-Z0-9_]+$" ) );
        QVERIFY( valid.match( token ).hasMatch() );
        QVERIFY( valid.match( session ).hasMatch() );
        QCOMPARE( outcome.responses.first().at( 1 ).toString(),
                  pathFor( bus->connection(), QStringLiteral( "request" ), token ) );
    }
    QCOMPARE( portal->requestPaths().size(), 2 );
    QVERIFY( portal->requestPaths().first() != portal->requestPaths().last() );
}

void TstPortalHotkeys::startAndCancelAreIdempotent()
{
    portal->setScenario( QStringLiteral( "no-response" ) );
    PortalRequest cancelled( bus->connection(), 90 );
    Outcome cancelledOutcome( cancelled );
    cancelled.cancel();
    cancelled.start( createSession(), Token );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( portal->receivedCallCount(), 0 );
    QCOMPARE( cancelledOutcome.count(), 0 );
    PortalRequest request( bus->connection(), 91 );
    Outcome outcome( request );
    request.start( createSession(), Token );
    request.start( createSession(), Token );
    QVERIFY( waitForCall() );
    request.cancel();
    request.cancel();
    QVERIFY( QTest::qWaitFor( [ this ]() { return portal->count( QStringLiteral( "Request.Close" ) ) == 1; }, 1000 ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( portal->count( QStringLiteral( "CreateSession" ) ), 1 );
    QCOMPARE( outcome.count(), 0 );
}

void TstPortalHotkeys::missingServiceFailsWithinDeadline()
{
    portal->loseService();
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    PortalRequest request( bus->connection(), 92 );
    Outcome outcome( request );
    QElapsedTimer elapsed;
    elapsed.start();
    request.start( createSession(), Token );
    QVERIFY( QTest::qWaitFor( [ &outcome ]() { return outcome.count() == 1; }, 1000 ) );
    QVERIFY( elapsed.elapsed() < 5000 );
    QCOMPARE( outcome.sequence, QStringList{ QStringLiteral( "failed" ) } );
    QCOMPARE( outcome.failures.first().at( 1 ).toString(), QStringLiteral( "Portal service is unavailable" ) );
    QCOMPARE( portal->receivedCallCount(), 0 );
}

void TstPortalHotkeys::responseHandlerMayDestroyRequest()
{
    portal->setScenario( QStringLiteral( "duplicate-response" ) );
    QPointer< PortalRequest > request = new PortalRequest( bus->connection(), 93 );
    int completions = 0;
    connect( request, &PortalRequest::response, this, [ &request, &completions ]() {
        ++completions;
        delete request;
    } );
    request->start( createSession(), Token );
    QVERIFY( QTest::qWaitFor( [ &request ]() { return request.isNull(); }, 1000 ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( completions, 1 );
}

void TstPortalHotkeys::portalWireTypeSignature()
{
    qDBusRegisterMetaType< PortalWire::Shortcut >();
    qDBusRegisterMetaType< PortalWire::Shortcuts >();
    QCOMPARE( QByteArray( QDBusMetaType::typeToSignature(
                  QMetaType::fromType< PortalWire::Shortcut >() ) ), QByteArray( "(sa{sv})" ) );
    QCOMPARE( QByteArray( QDBusMetaType::typeToSignature(
                  QMetaType::fromType< PortalWire::Shortcuts >() ) ), QByteArray( "a(sa{sv})" ) );
}


QTEST_GUILESS_MAIN( TstPortalHotkeys )
#include "tst_portal_hotkeys.moc"
