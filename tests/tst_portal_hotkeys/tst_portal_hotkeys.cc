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
#include <algorithm>
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
                          [ this ]() {
                              completedAt = elapsed.elapsed();
                              sequence.append( QStringLiteral( "response" ) );
                          } );
        QObject::connect( &request, &PortalRequest::failed, this,
                          [ this ]() {
                              completedAt = elapsed.elapsed();
                              sequence.append( QStringLiteral( "failed" ) );
                          } );
    }
    int count() const { return responses.size() + failures.size(); }
};

QVector< HotkeyBinding > twoBindings()
{
    return { HotkeyBinding{ QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                            QStringLiteral( "CTRL+F11" ) },
             HotkeyBinding{ QStringLiteral( "clipboard-lookup" ), QStringLiteral( "Clipboard lookup" ),
                            QStringLiteral( "CTRL+C" ) } };
}

QVector< HotkeyBinding > mainBindingOnly()
{
    return { twoBindings().first() };
}

TestSupport::FakePortal::Shortcut fakeShortcut( QString const & id, QString const & description,
                                                 QString const & trigger )
{
    TestSupport::FakePortal::Shortcut shortcut;
    shortcut.id = id;
    shortcut.properties.insert( QStringLiteral( "description" ), description );
    shortcut.properties.insert( QStringLiteral( "trigger_description" ), trigger );
    return shortcut;
}

bool waitForState( PortalBackend const & backend, HotkeyBackend::State state, int timeout = 2000 )
{
    return QTest::qWaitFor( [ &backend, state ]() { return backend.state() == state; }, timeout );
}

bool containsDisplayId( QVector< PortalBindingDisplay > const & displays, QString const & id )
{
    for ( PortalBindingDisplay const & display : displays )
        if ( display.id == id )
            return true;
    return false;
}

bool restartPortal( std::unique_ptr< TestSupport::FakePortal > * portal,
                    QDBusConnection const & connection, uint version )
{
    portal->reset();
    if ( !QTest::qWaitFor( [ &connection ]() {
             return connection.interface()->serviceOwner( TestSupport::FakePortal::serviceName() ).value().isEmpty();
         }, 1000 ) )
        return false;
    *portal = std::make_unique< TestSupport::FakePortal >();
    return ( *portal )->start( connection, version );
}

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
    void capabilityIsUnknownUntilProbeCompletes();
    void capabilityDoesNotOutliveItsGeneration();
    void unusablePortalIsUnsupportedAndUnavailable();
    void absentPortalIsUnsupportedAndDoesNoPortalWork();
    void responseHandlerMayDestroyRequest();
    void portalVersionAndCreateFailures_data();
    void portalVersionAndCreateFailures();
    void malformedListPayloadsAreUnavailable_data();
    void malformedListPayloadsAreUnavailable();
    void listResponseFailureInvalidatesSession();
    void historicalListIsDisplayOnlyAndFiltered();
    void displayPolicyIsIndependentOfTheBindInput();
    void reentrantBindingEditDoesNotDuplicatePortalWork();
    void bindingNotificationMayUnbind();
    void bindingNotificationMayEditOrDestroy_data();
    void bindingNotificationMayEditOrDestroy();
    void responseNotificationMayDestroyBackend_data();
    void responseNotificationMayDestroyBackend();
    void unsupportedBindingsAreNotManufactured();
    void changesBeforeBindCannotActivate();
    void closedBeforeBindStillInvalidatesTheSession();
    void backendRetirementDuringPendingWork_data();
    void backendRetirementDuringPendingWork();
    void bindResultsClassifyStateAndGateActivation_data();
    void bindResultsClassifyStateAndGateActivation();
    void shortcutsChangedReplacesActiveSubset();
    void slowListCannotOverwriteNewerBinding();
    void sessionClosedAndOwnerLossInvalidateBackend();
    void configureIsV2OnlyAndHasNoRequestLifecycle();
    void configurePendingReplyIsRetired();
    void deniedSessionCanBeConfiguredWithoutRebinding();
    void reportedTriggersAreDisplayOnly();
    void duplicateBindAttemptIsRefused();
    void portalSignalsMayDestroyBackend_data();
    void portalSignalsMayDestroyBackend();
    void portalUnbindClosesSessionAndRebindsOnce();
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
    qRegisterMetaType< HotkeyBackend::State >();
    qRegisterMetaType< QVector< PortalBindingDisplay > >();
    qRegisterMetaType< QVector< HotkeyBinding > >();
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
    if ( portal )
        QVERIFY2( portal->protocolViolations().isEmpty(),
                  qPrintable( portal->protocolViolations().join( '\n' ) ) );
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

void TstPortalHotkeys::capabilityIsUnknownUntilProbeCompletes()
{
    PortalBackend backend( bus->connection() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    QVERIFY( backend.isSupported() );

    // A new edit starts a new capability generation. The old successful probe
    // must not make the still-pending generation appear supported.
    portal->setScenario( QStringLiteral( "no-version" ) );
    backend.bind( mainBindingOnly() );
    QCOMPARE( backend.state(), HotkeyBackend::State::Creating );
    QVERIFY( !backend.isSupported() );
    QVERIFY( QTest::qWaitFor( [ &backend ]() {
        return backend.state() == HotkeyBackend::State::Unavailable;
    }, 1500 ) );
    QVERIFY( !backend.isSupported() );
}

void TstPortalHotkeys::capabilityDoesNotOutliveItsGeneration()
{
    // F-2: a successful generation describes that generation only. unbind() starts a
    // new one, so nothing may report support again until a fresh probe answers.
    PortalBackend backend( bus->connection() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound, 3000 ) );
    QVERIFY( backend.isSupported() );
    const int probes = portal->count( QStringLiteral( "Properties.Get" ) );

    backend.unbind();
    QVERIFY( waitForState( backend, HotkeyBackend::State::Closed, 2000 ) );
    QVERIFY2( !backend.isSupported(), "a closed session must not present the previous generation's probe" );
    QCOMPARE( portal->count( QStringLiteral( "Properties.Get" ) ), probes );

    // A fresh bind re-probes and may report support again.
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound, 3000 ) );
    QVERIFY( backend.isSupported() );
    QCOMPARE( portal->count( QStringLiteral( "Properties.Get" ) ), probes + 1 );
}

void TstPortalHotkeys::unusablePortalIsUnsupportedAndUnavailable()
{
    // The capability contract the factory rows cannot state on a bus shared with a
    // nested compositor: a portal that answers the version probe with an error is not
    // usable, and nothing may claim support for that generation.
    portal->setScenario( QStringLiteral( "no-version" ) );
    PortalBackend backend( bus->connection() );
    QVERIFY( !backend.isSupported() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Unavailable, 3000 ) );
    QVERIFY2( !backend.isSupported(), "an unusable portal must not report support" );
    QCOMPARE( portal->count( QStringLiteral( "Properties.Get" ) ), 1 );
    QCOMPARE( portal->count( QStringLiteral( "CreateSession" ) ), 0 );
}

void TstPortalHotkeys::absentPortalIsUnsupportedAndDoesNoPortalWork()
{
    // F-1: a connected bus with no portal owner takes readVersion()'s
    // GetNameOwner/NameHasNoOwner path. This bus belongs to the test and the compositor
    // is not on it, so nothing can auto-activate the name behind our back.
    portal->loseService();
    QVERIFY2( QTest::qWaitFor( [ this ]() {
                 return bus->fixtureConnection().interface()
                          ->serviceOwner( TestSupport::FakePortal::serviceName() )
                          .value()
                          .isEmpty();
             }, 2000 ), "the portal name did not withdraw" );

    PortalBackend backend( bus->connection() );
    QVERIFY( !backend.isSupported() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Unavailable, 3000 ) );
    QVERIFY2( !backend.isSupported(), "an absent portal must not report support" );

    // No portal work may follow the failed probe.
    QCOMPARE( portal->count( QStringLiteral( "CreateSession" ) ), 0 );
    QCOMPARE( portal->count( QStringLiteral( "ListShortcuts" ) ), 0 );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 0 );
    QCOMPARE( portal->count( QStringLiteral( "ConfigureShortcuts" ) ), 0 );
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

void TstPortalHotkeys::portalVersionAndCreateFailures_data()
{
    QTest::addColumn< QString >( "scenario" );
    QTest::addColumn< uint >( "version" );
    QTest::addColumn< int >( "expectedState" );
    QTest::addColumn< bool >( "expectedSupported" );

    QTest::newRow( "version-zero" ) << QStringLiteral( "fast" ) << uint( 0 )
                                     << int( HotkeyBackend::State::Unavailable ) << false;
    QTest::newRow( "no-version" ) << QStringLiteral( "no-version" ) << uint( 2 )
                                   << int( HotkeyBackend::State::Unavailable ) << false;
    QTest::newRow( "version-wrong-type" ) << QStringLiteral( "version-wrong-type" ) << uint( 2 )
                                           << int( HotkeyBackend::State::Unavailable ) << false;
    QTest::newRow( "create-denied" ) << QStringLiteral( "denied" ) << uint( 2 )
                                      << int( HotkeyBackend::State::Denied ) << true;
    QTest::newRow( "create-failed" ) << QStringLiteral( "failed" ) << uint( 2 )
                                      << int( HotkeyBackend::State::Unavailable ) << true;
    for ( char const * scenario : { "invalid-session-handle", "missing-session-handle" } )
        QTest::newRow( scenario ) << QString::fromLatin1( scenario ) << uint( 2 )
                                  << int( HotkeyBackend::State::Unavailable ) << true;
}

void TstPortalHotkeys::portalVersionAndCreateFailures()
{
    QFETCH( QString, scenario );
    QFETCH( uint, version );
    QFETCH( int, expectedState );
    QFETCH( bool, expectedSupported );

    if ( version != 2 )
        QVERIFY2( restartPortal( &portal, bus->fixtureConnection(), version ),
                  qPrintable( portal->errorString() ) );
    portal->setScenario( scenario );

    PortalBackend backend( bus->connection() );
    backend.bind( twoBindings() );
    QVERIFY( QTest::qWaitFor( [ &backend, expectedState ]() {
                 return int( backend.state() ) == expectedState;
             }, 2500 ) );
    QCOMPARE( int( backend.state() ), expectedState );
    QCOMPARE( backend.isSupported(), expectedSupported );
    QCOMPARE( portal->count( QStringLiteral( "Properties.Get" ) ), 1 );
    if ( scenario == QLatin1String( "no-version" )
         || scenario == QLatin1String( "version-wrong-type" ) || version == 0 )
        QCOMPARE( portal->count( QStringLiteral( "CreateSession" ) ), 0 );
    else
        QCOMPARE( portal->count( QStringLiteral( "CreateSession" ) ), 1 );
}

void TstPortalHotkeys::malformedListPayloadsAreUnavailable_data()
{
    QTest::addColumn< QString >( "scenario" );
    for ( char const * scenario : { "list-malformed-tuple", "list-malformed-map",
                                    "list-duplicate-ids", "list-property-wrong-type" } )
        QTest::newRow( scenario ) << QString::fromLatin1( scenario );
}

void TstPortalHotkeys::malformedListPayloadsAreUnavailable()
{
    QFETCH( QString, scenario );
    QVERIFY( TestSupport::FakePortal::scenarios().contains( scenario ) );
    portal->setScenario( scenario );

    PortalBackend backend( bus->connection() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Unavailable, 2500 ) );
    QVERIFY( backend.isSupported() );
    QCOMPARE( portal->count( QStringLiteral( "ListShortcuts" ) ), 1 );
}

void TstPortalHotkeys::listResponseFailureInvalidatesSession()
{
    bool ready = false;
    PortalBackend backend( bus->connection() );
    connect( &backend, &HotkeyBackend::stateChanged, this,
             [ this, &ready ]( HotkeyBackend::State state, QString const & ) {
                 if ( state == HotkeyBackend::State::Ready )
                 {
                     ready = true;
                     portal->setScenario( QStringLiteral( "failed" ) );
                 }
             } );

    backend.bind( mainBindingOnly() );
    QVERIFY( QTest::qWaitFor( [ &backend ]() {
                 return backend.state() == HotkeyBackend::State::Unavailable;
             }, 2500 ) );
    QVERIFY( ready );
    QVERIFY( backend.isSupported() );
    QCOMPARE( portal->count( QStringLiteral( "ListShortcuts" ) ), 1 );
}

void TstPortalHotkeys::reentrantBindingEditDoesNotDuplicatePortalWork()
{
    // F-1: a stateChanged handler that starts a new binding edit synchronously must
    // not let the suspended older attempt issue portal work tagged with the newer
    // generation. Each bind call owns exactly one version lookup and one session.
    PortalBackend backend( bus->connection() );
    bool reentered = false;
    connect( &backend, &HotkeyBackend::stateChanged, this,
             [ & ]( HotkeyBackend::State state, QString const & ) {
                 if( state == HotkeyBackend::State::Creating && !reentered )
                 {
                     reentered = true;
                     backend.bind( twoBindings() );
                 }
             } );

    backend.bind( mainBindingOnly() );
    QVERIFY( reentered );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound, 3000 ) );

    // A bus barrier drains work already queued by either version watcher.
    QVERIFY( flushWire( bus->fixtureConnection() ) );

    // The superseded attempt must issue no portal work at all: one version lookup,
    // one session, one listing and one bind for the surviving generation.
    QCOMPARE( portal->count( QStringLiteral( "Properties.Get" ) ), 1 );
    QCOMPARE( portal->count( QStringLiteral( "CreateSession" ) ), 1 );
    QCOMPARE( portal->count( QStringLiteral( "ListShortcuts" ) ), 1 );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 1 );
}

void TstPortalHotkeys::bindingNotificationMayUnbind()
{
    QTest::failOnWarning( QRegularExpression( QStringLiteral( ".*(invalid path|Marshalling failed).*" ) ) );
    PortalBackend backend( bus->connection() );
    bool unbound = false;
    connect( &backend, &HotkeyBackend::stateChanged, this,
             [ & ]( HotkeyBackend::State state, QString const & ) {
                 if ( state == HotkeyBackend::State::Binding )
                 {
                     unbound = true;
                     backend.unbind();
                 }
             } );
    backend.bind( mainBindingOnly() );
    QVERIFY( QTest::qWaitFor( [ & ]() { return unbound; }, 2000 ) );
    QVERIFY( QTest::qWaitFor( [ & ]() {
        return portal->count( QStringLiteral( "Session.Close" ) ) == 1;
    }, 1000 ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( backend.state(), HotkeyBackend::State::Closed );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 0 );
}

void TstPortalHotkeys::bindingNotificationMayEditOrDestroy_data()
{
    QTest::addColumn< bool >( "destroy" );
    QTest::newRow( "edit" ) << false;
    QTest::newRow( "destroy" ) << true;
}

void TstPortalHotkeys::bindingNotificationMayEditOrDestroy()
{
    QFETCH( bool, destroy );
    auto backend = std::make_unique< PortalBackend >( bus->connection() );
    bool reentered = false;
    connect( backend.get(), &HotkeyBackend::stateChanged, this,
             [ & ]( HotkeyBackend::State state, QString const & ) {
                 if ( state != HotkeyBackend::State::Binding || reentered )
                     return;
                 reentered = true;
                 if ( destroy )
                     backend.reset();
                 else
                     backend->bind( twoBindings() );
             } );
    backend->bind( mainBindingOnly() );
    QVERIFY( QTest::qWaitFor( [ & ]() { return reentered; }, 2000 ) );
    if ( !destroy )
        QVERIFY( waitForState( *backend, HotkeyBackend::State::Bound ) );
    QVERIFY( QTest::qWaitFor( [ & ]() {
        return portal->count( QStringLiteral( "Session.Close" ) ) == 1;
    }, 1000 ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( portal->count( QStringLiteral( "CreateSession" ) ), destroy ? 1 : 2 );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), destroy ? 0 : 1 );
    QVERIFY( portal->protocolViolations().isEmpty() );
}

void TstPortalHotkeys::responseNotificationMayDestroyBackend_data()
{
    QTest::addColumn< int >( "notification" );
    QTest::newRow( "creating" ) << int( HotkeyBackend::State::Creating );
    QTest::newRow( "ready" ) << int( HotkeyBackend::State::Ready );
    QTest::newRow( "bound" ) << int( HotkeyBackend::State::Bound );
    QTest::newRow( "unavailable" ) << int( HotkeyBackend::State::Unavailable );
}

void TstPortalHotkeys::responseNotificationMayDestroyBackend()
{
    QFETCH( int, notification );
    if ( notification == int( HotkeyBackend::State::Unavailable ) )
        portal->setScenario( QStringLiteral( "bind-wrong-type" ) );
    auto backend = std::make_unique< PortalBackend >( bus->connection() );
    connect( backend.get(), &HotkeyBackend::stateChanged, this,
             [ & ]( HotkeyBackend::State state, QString const & ) {
                 if ( int( state ) == notification )
                     backend.reset();
             } );
    backend->bind( mainBindingOnly() );
    QVERIFY( QTest::qWaitFor( [ & ]() { return !backend; }, 2000 ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
}

void TstPortalHotkeys::unsupportedBindingsAreNotManufactured()
{
    PortalBackend backend( bus->connection() );
    auto bindings = twoBindings();
    bindings[ 1 ].sequence.clear(); // preferredTrigger rejected the stored shortcut.
    backend.bind( bindings );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    const auto calls = portal->calls();
    for ( auto const & call : calls )
        if ( call.member == QLatin1String( "BindShortcuts" ) )
            QCOMPARE( call.shortcutIds, QStringList{ QStringLiteral( "main-window" ) } );
    const int creates = portal->count( QStringLiteral( "CreateSession" ) );
    bindings[ 0 ].sequence.clear();
    backend.bind( bindings );
    QCOMPARE( backend.state(), HotkeyBackend::State::Unavailable );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( portal->count( QStringLiteral( "CreateSession" ) ), creates );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 1 );
}

void TstPortalHotkeys::changesBeforeBindCannotActivate()
{
    portal->setScenario( QStringLiteral( "delayed-response" ) );
    PortalBackend backend( bus->connection() );
    QSignalSpy activated( &backend, &HotkeyBackend::activated );
    backend.bind( mainBindingOnly() );
    QVERIFY( QTest::qWaitFor( [ & ]() {
        return portal->count( QStringLiteral( "CreateSession" ) ) == 1;
    }, 1000 ) );
    connect( &backend, &HotkeyBackend::stateChanged, this,
             [ & ]( HotkeyBackend::State state, QString const & ) {
                 if ( state != HotkeyBackend::State::Ready )
                     return;
                 portal->emitShortcutsChanged( portal->sessionPath(),
                     { fakeShortcut( QStringLiteral( "main-window" ), {}, {} ) } );
                 portal->emitActivation( portal->sessionPath(), QStringLiteral( "main-window" ), {} );
                 QVERIFY( flushWire( bus->fixtureConnection() ) );
                 QCOMPARE( activated.count(), 0 );
                 QCOMPARE( backend.state(), HotkeyBackend::State::Ready );
             } );
    portal->releasePending();
    QVERIFY( QTest::qWaitFor( [ & ]() {
        return portal->count( QStringLiteral( "BindShortcuts" ) ) == 1;
    }, 1000 ) );
    portal->releasePending();
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
}

void TstPortalHotkeys::closedBeforeBindStillInvalidatesTheSession()
{
    // A Closed signal for the live session is authoritative even while BindShortcuts
    // is still in flight: the session exists from CreateSession's reply onward, so
    // the backend must retire it rather than let a late successful bind resurrect it.
    portal->setScenario( QStringLiteral( "delayed-response" ) );
    PortalBackend backend( bus->connection() );
    QVector< HotkeyBackend::State > states;
    connect( &backend, &HotkeyBackend::stateChanged, this,
             [ &states ]( HotkeyBackend::State state, QString const & ) { states.append( state ); } );

    backend.bind( mainBindingOnly() );
    QVERIFY( QTest::qWaitFor( [ & ]() {
        return portal->count( QStringLiteral( "CreateSession" ) ) == 1;
    }, 1000 ) );
    portal->releasePending();
    QVERIFY( QTest::qWaitFor( [ & ]() {
        return portal->count( QStringLiteral( "BindShortcuts" ) ) == 1;
    }, 1000 ) );
    QCOMPARE( backend.state(), HotkeyBackend::State::Binding );

    portal->emitSessionClosed( portal->sessionPath(), {} );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Unavailable ) );

    // The bind response is still pending on the wire: releasing it must not
    // resurrect the closed session.
    portal->releasePending();
    QTest::qWait( 50 );
    QCOMPARE( backend.state(), HotkeyBackend::State::Unavailable );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 1 );
}

void TstPortalHotkeys::backendRetirementDuringPendingWork_data()
{
    QTest::addColumn< QString >( "stage" );
    QTest::addColumn< QString >( "retirement" );
    for ( QString stage : { QStringLiteral( "create" ), QStringLiteral( "bind" ) } )
        for ( QString retirement : { QStringLiteral( "unbind" ), QStringLiteral( "edit" ),
                  QStringLiteral( "destroy" ), QStringLiteral( "loss" ), QStringLiteral( "timeout" ) } )
            QTest::newRow( qPrintable( stage + '-' + retirement ) ) << stage << retirement;
}

void TstPortalHotkeys::backendRetirementDuringPendingWork()
{
    QFETCH( QString, stage );
    QFETCH( QString, retirement );
    auto backend = std::make_unique< PortalBackend >( bus->connection() );
    portal->setScenario( QStringLiteral( "delayed-response" ) );
    backend->bind( mainBindingOnly() );
    QVERIFY( QTest::qWaitFor( [ & ]() {
        return portal->count( QStringLiteral( "CreateSession" ) ) == 1;
    }, 1000 ) );
    if ( stage == QLatin1String( "bind" ) )
    {
        portal->releasePending();
        QVERIFY( QTest::qWaitFor( [ & ]() {
            return portal->count( QStringLiteral( "BindShortcuts" ) ) == 1;
        }, 1000 ) );
    }
    const QString oldSession = portal->sessionPath();
    QSignalSpy states( backend.get(), &HotkeyBackend::stateChanged );
    QSignalSpy activations( backend.get(), &HotkeyBackend::activated );
    if ( retirement == QLatin1String( "destroy" ) )
        backend.reset();
    else if ( retirement == QLatin1String( "edit" ) )
    {
        portal->setScenario( QStringLiteral( "fast" ) );
        backend->bind( twoBindings() );
        QVERIFY( waitForState( *backend, HotkeyBackend::State::Bound ) );
    }
    else if ( retirement == QLatin1String( "loss" ) )
    {
        portal->loseService();
        QVERIFY( waitForState( *backend, HotkeyBackend::State::Unavailable ) );
        QVERIFY( !backend->isSupported() );
    }
    else if ( retirement == QLatin1String( "timeout" ) )
        QVERIFY( waitForState( *backend, HotkeyBackend::State::Unavailable, 5500 ) );
    else
        backend->unbind();
    const int notifications = states.count();
    portal->releasePending();
    portal->emitActivation( oldSession, QStringLiteral( "main-window" ), QStringLiteral( "stale" ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    if ( retirement != QLatin1String( "loss" ) )
        QVERIFY( QTest::qWaitFor( [ & ]() {
            return portal->count( QStringLiteral( "Session.Close" ) ) >= 1;
        }, 1000 ) );
    QCOMPARE( activations.count(), 0 );
    QCOMPARE( states.count(), notifications );
    QVERIFY( portal->protocolViolations().isEmpty() );
}

void TstPortalHotkeys::displayPolicyIsIndependentOfTheBindInput()
{
    // F-2: an ID can be eligible for display while the current session cannot bind
    // it, and a bound ID can be excluded from display. The two decisions are
    // independent: the policy decides what is shown, the bind input decides what is
    // requested and how Bound/Partial is classified.
    portal->setScenario( QStringLiteral( "list-slow" ) );
    portal->setListShortcuts( { fakeShortcut( QStringLiteral( "main-window" ),
                                               QStringLiteral( "Historical main" ),
                                               QStringLiteral( "historical-trigger" ) ),
                                  fakeShortcut( QStringLiteral( "clipboard-lookup" ),
                                               QStringLiteral( "Old lookup" ),
                                               QStringLiteral( "Ctrl+C C" ) ) } );

    PortalBackend backend( bus->connection() );
    QSignalSpy activated( &backend, &HotkeyBackend::activated );
    QVector< QVector< PortalBindingDisplay > > displays;
    connect( &backend, &HotkeyBackend::bindingsChanged, this,
             [ &displays ]( QVector< PortalBindingDisplay > const & values ) {
                 displays.append( values );
             } );

    // Display both IDs, but only ask the portal to bind one of them.
    backend.setDisplayPolicy( QSet< QString >{ QStringLiteral( "main-window" ),
                                               QStringLiteral( "clipboard-lookup" ) } );
    backend.bind( mainBindingOnly() );

    // "list-slow" defers the Response signals. Release the session response so the
    // backend proceeds to List, then the List response so it proceeds to Bind.
    QVERIFY( QTest::qWaitFor( [ this ]() {
                 return portal->count( QStringLiteral( "Properties.Get" ) ) == 1
                        && portal->count( QStringLiteral( "CreateSession" ) ) == 1;
             }, 2000 ) );
    portal->releasePending();
    QVERIFY( QTest::qWaitFor( [ this ]() {
                 return portal->count( QStringLiteral( "ListShortcuts" ) ) == 1;
             }, 2000 ) );
    portal->releasePending();
    QVERIFY( QTest::qWaitFor( [ this ]() {
                 return portal->count( QStringLiteral( "BindShortcuts" ) ) == 1;
             }, 2000 ) );
    portal->releasePending();
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    QVERIFY( QTest::qWaitFor( [ &displays ]() { return !displays.isEmpty(); }, 2000 ) );

    bool sawUnboundHistorical = false;
    for ( QVector< PortalBindingDisplay > const & values : displays )
    {
        if ( containsDisplayId( values, QStringLiteral( "clipboard-lookup" ) ) )
            sawUnboundHistorical = true;
    }
    QVERIFY2( sawUnboundHistorical,
              "an ID that is display-eligible but not bound must still be displayed" );

    // Bind call still carries only the requested ID.
    QVector< TestSupport::FakePortal::Call > calls = portal->calls();
    auto bindCall = std::find_if( calls.cbegin(), calls.cend(), []( auto const & call ) {
        return call.member == QLatin1String( "BindShortcuts" );
    } );
    QVERIFY( bindCall != calls.cend() );
    QCOMPARE( bindCall->shortcutIds, QStringList{ QStringLiteral( "main-window" ) } );

    // The historical entry never activates: only a portal activation for a
    // *requested* ID reaches the backend.
    portal->emitActivation( portal->sessionPath(), QStringLiteral( "clipboard-lookup" ),
                            QStringLiteral( "stale-token" ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( activated.size(), 0 );

    portal->emitActivation( portal->sessionPath(), QStringLiteral( "main-window" ),
                            QStringLiteral( "live-token" ) );
    QVERIFY( QTest::qWaitFor( [ &activated ]() { return activated.size() == 1; }, 2000 ) );
}

void TstPortalHotkeys::historicalListIsDisplayOnlyAndFiltered()
{
    portal->setScenario( QStringLiteral( "list-slow" ) );
    portal->setListShortcuts( { fakeShortcut( QStringLiteral( "main-window" ),
                                               QStringLiteral( "Historical main" ),
                                               QStringLiteral( "historical-trigger" ) ),
                                  fakeShortcut( QStringLiteral( "clipboard-lookup" ),
                                               QStringLiteral( "Old lookup" ),
                                               QStringLiteral( "Ctrl+C C" ) ) } );

    PortalBackend backend( bus->connection() );
    QSignalSpy activated( &backend, &HotkeyBackend::activated );
    QVector< QVector< PortalBindingDisplay > > displays;
    connect( &backend, &HotkeyBackend::bindingsChanged, this,
             [ &displays ]( QVector< PortalBindingDisplay > const & values ) {
                 displays.append( values );
             } );

    backend.bind( mainBindingOnly() );
    QVERIFY( QTest::qWaitFor( [ this ]() {
                 return portal->count( QStringLiteral( "Properties.Get" ) ) == 1
                        && portal->count( QStringLiteral( "CreateSession" ) ) == 1;
             }, 1000 ) );
    portal->releasePending();
    QVERIFY( QTest::qWaitFor( [ this ]() {
                 return portal->count( QStringLiteral( "ListShortcuts" ) ) == 1
                        && portal->count( QStringLiteral( "BindShortcuts" ) ) == 1;
             }, 1000 ) );
    portal->releasePending();
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    QVERIFY( QTest::qWaitFor( [ &displays ]() { return displays.size() >= 2; }, 1000 ) );
    QVERIFY( !displays.constFirst().isEmpty() );
    for ( QVector< PortalBindingDisplay > const & values : displays )
    {
        QVERIFY( !containsDisplayId( values, QStringLiteral( "clipboard-lookup" ) ) );
        for ( PortalBindingDisplay const & value : values )
            QCOMPARE( value.id, QStringLiteral( "main-window" ) );
    }
    QVERIFY( containsDisplayId( displays.constLast(), QStringLiteral( "main-window" ) ) );

    QVector< TestSupport::FakePortal::Call > calls = portal->calls();
    auto bindCall = std::find_if( calls.cbegin(), calls.cend(), []( auto const & call ) {
        return call.member == QLatin1String( "BindShortcuts" );
    } );
    QVERIFY( bindCall != calls.cend() );
    QCOMPARE( bindCall->signature, QByteArray( "oa(sa{sv})sa{sv}" ) );
    QCOMPARE( bindCall->shortcutIds, QStringList{ QStringLiteral( "main-window" ) } );
    QCOMPARE( bindCall->parentWindow, QString() );
    QCOMPARE( bindCall->shortcuts.constFirst().properties.value( QStringLiteral( "description" ) ).toString(),
              QStringLiteral( "Main window" ) );
    QCOMPARE( bindCall->shortcuts.constFirst().properties.value( QStringLiteral( "preferred_trigger" ) ).toString(),
              QStringLiteral( "CTRL+F11" ) );
    QCOMPARE( bindCall->options.size(), 1 );
    QVERIFY( !bindCall->options.value( QStringLiteral( "handle_token" ) ).toString().isEmpty() );

    const QString session = portal->sessionPath();
    portal->emitActivation( session, QStringLiteral( "clipboard-lookup" ), QStringLiteral( "old-token" ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( activated.count(), 0 );
    portal->emitActivation( session, QStringLiteral( "main-window" ), QStringLiteral( "live-token" ) );
    QVERIFY( QTest::qWaitFor( [ &activated ]() { return activated.count() == 1; }, 1000 ) );
    QCOMPARE( activated.at( 0 ).at( 0 ).toString(), QStringLiteral( "main-window" ) );
    QCOMPARE( activated.at( 0 ).at( 1 ).toString(), QStringLiteral( "live-token" ) );
}

void TstPortalHotkeys::bindResultsClassifyStateAndGateActivation_data()
{
    QTest::addColumn< QString >( "scenario" );
    QTest::addColumn< int >( "expectedState" );
    QTest::addColumn< int >( "expectedActivations" );
    const int bound = int( HotkeyBackend::State::Bound );
    const int partial = int( HotkeyBackend::State::Partial );
    const int denied = int( HotkeyBackend::State::Denied );
    const int unavailable = int( HotkeyBackend::State::Unavailable );
    QTest::newRow( "full" ) << QStringLiteral( "fast" ) << bound << 2;
    QTest::newRow( "partial" ) << QStringLiteral( "bind-partial" ) << partial << 1;
    QTest::newRow( "empty" ) << QStringLiteral( "bind-empty" ) << denied << 0;
    QTest::newRow( "unknown-id" ) << QStringLiteral( "bind-unknown-id" ) << unavailable << 0;
    QTest::newRow( "duplicate-id" ) << QStringLiteral( "bind-duplicate-id" ) << unavailable << 0;
    QTest::newRow( "wrong-type" ) << QStringLiteral( "bind-wrong-type" ) << unavailable << 0;
    QTest::newRow( "denied" ) << QStringLiteral( "bind-denied" ) << denied << 0;
    QTest::newRow( "failed" ) << QStringLiteral( "bind-failed" ) << denied << 0;
}

void TstPortalHotkeys::bindResultsClassifyStateAndGateActivation()
{
    QFETCH( QString, scenario );
    QFETCH( int, expectedState );
    QFETCH( int, expectedActivations );
    portal->setScenario( scenario );

    PortalBackend backend( bus->connection() );
    QSignalSpy activated( &backend, &HotkeyBackend::activated );
    backend.bind( twoBindings() );
    QVERIFY( QTest::qWaitFor( [ &backend, expectedState ]() {
                 return int( backend.state() ) == expectedState;
             }, 2500 ) );
    QCOMPARE( int( backend.state() ), expectedState );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 1 );

    const QString session = portal->sessionPath();
    portal->emitActivation( session, QStringLiteral( "main-window" ), QStringLiteral( "main-token" ) );
    portal->emitActivation( session, QStringLiteral( "clipboard-lookup" ), QStringLiteral( "lookup-token" ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( activated.count(), expectedActivations );
    if ( expectedActivations == 2 )
    {
        QCOMPARE( activated.at( 0 ).at( 1 ).toString(), QStringLiteral( "main-token" ) );
        QCOMPARE( activated.at( 1 ).at( 1 ).toString(), QStringLiteral( "lookup-token" ) );
    }
    else if ( expectedActivations == 1 )
    {
        QCOMPARE( activated.at( 0 ).at( 0 ).toString(), QStringLiteral( "main-window" ) );
    }
}

void TstPortalHotkeys::shortcutsChangedReplacesActiveSubset()
{
    portal->setBoundIds( { QStringLiteral( "main-window" ) } );
    PortalBackend backend( bus->connection() );
    QSignalSpy activated( &backend, &HotkeyBackend::activated );
    int displayChanges = 0;
    connect( &backend, &HotkeyBackend::bindingsChanged, this,
             [ &displayChanges ]( QVector< PortalBindingDisplay > const & ) { ++displayChanges; } );

    backend.bind( twoBindings() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Partial ) );
    const int listCalls = portal->count( QStringLiteral( "ListShortcuts" ) );
    const QString session = portal->sessionPath();
    portal->emitShortcutsChanged( session,
                                  { fakeShortcut( QStringLiteral( "clipboard-lookup" ),
                                                  QStringLiteral( "Changed lookup" ),
                                                  QStringLiteral( "Ctrl+Shift+C" ) ) } );
    QVERIFY( QTest::qWaitFor( [ &displayChanges ]() { return displayChanges >= 2; }, 1000 ) );
    QCOMPARE( int( backend.state() ), int( HotkeyBackend::State::Partial ) );

    portal->emitActivation( session, QStringLiteral( "main-window" ), QStringLiteral( "stale" ) );
    portal->emitActivation( session, QStringLiteral( "clipboard-lookup" ), QStringLiteral( "changed" ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( activated.count(), 1 );
    QCOMPARE( activated.at( 0 ).at( 0 ).toString(), QStringLiteral( "clipboard-lookup" ) );
    QCOMPARE( activated.at( 0 ).at( 1 ).toString(), QStringLiteral( "changed" ) );

    portal->emitDeactivated( session, QStringLiteral( "clipboard-lookup" ), QStringLiteral( "ignored" ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( activated.count(), 1 );
    QCOMPARE( portal->count( QStringLiteral( "ListShortcuts" ) ), listCalls );
}

void TstPortalHotkeys::slowListCannotOverwriteNewerBinding()
{
    portal->setScenario( QStringLiteral( "list-slow" ) );
    portal->setListShortcuts( { fakeShortcut( QStringLiteral( "main-window" ), QStringLiteral( "Historical" ),
                                               QStringLiteral( "historical-trigger" ) ) } );
    PortalBackend backend( bus->connection() );
    QVector< QVector< PortalBindingDisplay > > displays;
    connect( &backend, &HotkeyBackend::bindingsChanged, this,
             [ &displays ]( QVector< PortalBindingDisplay > const & values ) {
                 displays.append( values );
             } );

    backend.bind( mainBindingOnly() );
    QVERIFY( QTest::qWaitFor( [ this ]() {
                 return portal->count( QStringLiteral( "Properties.Get" ) ) == 1
                        && portal->count( QStringLiteral( "CreateSession" ) ) == 1;
             }, 1000 ) );
    portal->releasePending();
    QVERIFY( QTest::qWaitFor( [ this ]() {
                 return portal->count( QStringLiteral( "ListShortcuts" ) ) == 1
                        && portal->count( QStringLiteral( "BindShortcuts" ) ) == 1;
             }, 1000 ) );

    QVector< TestSupport::FakePortal::Call > calls = portal->calls();
    auto bindCall = std::find_if( calls.cbegin(), calls.cend(), []( auto const & call ) {
        return call.member == QLatin1String( "BindShortcuts" );
    } );
    QVERIFY( bindCall != calls.cend() );
    const QString bindPath = bindCall->requestPath;
    PortalWire::Shortcuts bound{ PortalWire::Shortcut{
        QStringLiteral( "main-window" ),
        QVariantMap{ { QStringLiteral( "description" ), QStringLiteral( "Main window" ) },
                     { QStringLiteral( "trigger_description" ), QStringLiteral( "new-trigger" ) } } } };
    QVERIFY( sendResponse( bus->fixtureConnection(), bindPath, uint( 0 ),
                           QVariantMap{ { QStringLiteral( "shortcuts" ), QVariant::fromValue( bound ) } } ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );

    // The fixture also releases the delayed List response here. It was invoked
    // before the manual Bind response, but its captured display revision is old.
    portal->releasePending();
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QVERIFY( !displays.isEmpty() );
    QCOMPARE( displays.constLast().constFirst().triggerDescription, QStringLiteral( "new-trigger" ) );
}

void TstPortalHotkeys::sessionClosedAndOwnerLossInvalidateBackend()
{
    PortalBackend backend( bus->connection() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    const QString firstSession = portal->sessionPath();
    portal->emitSessionClosed( firstSession, QStringLiteral( "fixture closed" ) );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Unavailable ) );
    QVERIFY( backend.isSupported() );

    // An explicit later edit starts a fresh generation; service loss then
    // invalidates that live session and the capability.
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    portal->loseService();
    QVERIFY( QTest::qWaitFor( [ &backend ]() {
                 return backend.state() == HotkeyBackend::State::Unavailable;
             }, 1500 ) );
    QVERIFY( !backend.isSupported() );
}

void TstPortalHotkeys::configureIsV2OnlyAndHasNoRequestLifecycle()
{
    PortalBackend backend( bus->connection() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 1 );
    const int requestPaths = portal->requestPaths().size();
    QSignalSpy states( &backend, &HotkeyBackend::stateChanged );

    backend.configure( QStringLiteral( "configure-token" ) );
    QVERIFY( QTest::qWaitFor( [ this ]() {
                 return portal->count( QStringLiteral( "ConfigureShortcuts" ) ) == 1;
             }, 1000 ) );
    auto calls = portal->calls();
    auto configureCall = std::find_if( calls.cbegin(), calls.cend(), []( auto const & call ) {
        return call.member == QLatin1String( "ConfigureShortcuts" );
    } );
    QVERIFY( configureCall != calls.cend() );
    QCOMPARE( configureCall->parentWindow, QString() );
    QCOMPARE( configureCall->activationToken, QStringLiteral( "configure-token" ) );
    QCOMPARE( configureCall->signature, QByteArray( "osa{sv}" ) );
    QCOMPARE( configureCall->options.size(), 1 );
    QVERIFY( configureCall->options.contains( QStringLiteral( "activation_token" ) ) );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 1 );

    portal->setScenario( QStringLiteral( "configure-error" ) );
    backend.configure( QStringLiteral( "second-token" ) );
    QVERIFY( QTest::qWaitFor( [ this ]() {
                 return portal->count( QStringLiteral( "ConfigureShortcuts" ) ) == 2;
             }, 1000 ) );
    QVERIFY( QTest::qWaitFor( [ & ]() { return states.count() == 1; }, 1000 ) );
    QCOMPARE( states.first().first().value< HotkeyBackend::State >(), HotkeyBackend::State::Bound );
    QVERIFY( states.first().at( 1 ).toString().contains( QStringLiteral( "ConfigureShortcuts failed" ) ) );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 1 );
    QCOMPARE( portal->count( QStringLiteral( "Request.Close" ) ), 0 );
    QCOMPARE( portal->requestPaths().size(), requestPaths );
    portal->setScenario( QStringLiteral( "fast" ) );
    backend.configure( {} );
    QVERIFY( QTest::qWaitFor( [ & ]() {
        return portal->count( QStringLiteral( "ConfigureShortcuts" ) ) == 3;
    }, 1000 ) );
    QCOMPARE( portal->calls().constLast().options.size(), 0 );

    QVERIFY2( restartPortal( &portal, bus->fixtureConnection(), 1 ), qPrintable( portal->errorString() ) );
    PortalBackend versionOne( bus->connection() );
    versionOne.bind( mainBindingOnly() );
    QVERIFY( waitForState( versionOne, HotkeyBackend::State::Bound ) );
    versionOne.configure( QStringLiteral( "ignored-on-v1" ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( portal->count( QStringLiteral( "ConfigureShortcuts" ) ), 0 );
    QCOMPARE( int( versionOne.state() ), int( HotkeyBackend::State::Bound ) );
}

void TstPortalHotkeys::reportedTriggersAreDisplayOnly()
{
    portal->setBoundShortcuts( { fakeShortcut( QStringLiteral( "main-window" ), {},
                                               QStringLiteral( "Portal's chosen trigger" ) ),
                                fakeShortcut( QStringLiteral( "clipboard-lookup" ), {}, {} ) } );
    PortalBackend backend( bus->connection() );
    QVector< PortalBindingDisplay > displayed;
    connect( &backend, &HotkeyBackend::bindingsChanged, this,
             [ & ]( QVector< PortalBindingDisplay > const & entries ) { displayed = entries; } );
    backend.bind( twoBindings() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    QCOMPARE( displayed.size(), 2 );
    QCOMPARE( displayed.at( 0 ).triggerDescription, QStringLiteral( "Portal's chosen trigger" ) );
    QCOMPARE( displayed.at( 1 ).triggerDescription, QStringLiteral( "Assigned by portal" ) );
    const auto calls = portal->calls();
    const auto binding = std::find_if( calls.cbegin(), calls.cend(), []( auto const & call ) {
        return call.member == QLatin1String( "BindShortcuts" );
    } );
    QVERIFY( binding != calls.cend() );
    QCOMPARE( binding->shortcuts.at( 0 ).properties.value( QStringLiteral( "preferred_trigger" ) ).toString(),
              QStringLiteral( "CTRL+F11" ) );
    QCOMPARE( binding->shortcuts.at( 1 ).properties.value( QStringLiteral( "preferred_trigger" ) ).toString(),
              QStringLiteral( "CTRL+C" ) );
}

void TstPortalHotkeys::deniedSessionCanBeConfiguredWithoutRebinding()
{
    portal->setScenario( QStringLiteral( "bind-empty" ) );
    PortalBackend backend( bus->connection() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Denied ) );
    const int paths = portal->requestPaths().size();
    backend.configure( {} );
    QVERIFY( QTest::qWaitFor( [ & ]() {
        return portal->count( QStringLiteral( "ConfigureShortcuts" ) ) == 1;
    }, 1000 ) );
    portal->emitShortcutsChanged( portal->sessionPath(),
        { fakeShortcut( QStringLiteral( "main-window" ), {}, QStringLiteral( "Assigned later" ) ) } );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    QCOMPARE( portal->requestPaths().size(), paths );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 1 );
}

void TstPortalHotkeys::duplicateBindAttemptIsRefused()
{
    PortalBackend backend( bus->connection() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    const QString session = portal->sessionPath();
    const auto calls = portal->calls();
    const auto create = std::find_if( calls.cbegin(), calls.cend(), []( auto const & call ) {
        return call.member == QLatin1String( "CreateSession" );
    } );
    QVERIFY( create != calls.cend() );
    // Replaying completion, configuring or a Changed signal must never retry Bind.
    QVERIFY( sendResponse( bus->fixtureConnection(), create->requestPath, uint( 0 ),
                          QVariantMap{ { QStringLiteral( "session_handle" ), session } } ) );
    backend.configure( {} );
    portal->emitShortcutsChanged( session, { fakeShortcut( QStringLiteral( "main-window" ), {}, {} ) } );
    QVERIFY( QTest::qWaitFor( [ & ]() {
        return portal->count( QStringLiteral( "ConfigureShortcuts" ) ) == 1;
    }, 1000 ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    // Replaying completion, configuring or a Changed signal must never retry Bind.
    // A second bind path for the same live session and generation must be refused
    // by the one-bind guard itself, not merely unreachable: this seam reaches it.
    backend.startBindForCurrentGenerationForTest();
    QVERIFY( flushWire( bus->fixtureConnection() ) );

    QCOMPARE( portal->bindCountForSession( session ), 1 );
    QCOMPARE( backend.state(), HotkeyBackend::State::Bound );
    QVERIFY( portal->protocolViolations().isEmpty() );
}

void TstPortalHotkeys::configurePendingReplyIsRetired()
{
    class ConfigureGate final : public QDBusVirtualObject
    {
    public:
        QDBusMessage pending;
        QString introspect( QString const & ) const override { return {}; }
        bool handleMessage( QDBusMessage const & message, QDBusConnection const & ) override
        {
            if ( message.member() != QLatin1String( "ConfigureShortcuts" ) )
                return false;
            pending = message;
            pending.setDelayedReply( true );
            return true;
        }
    } gate;
    PortalBackend backend( bus->connection() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    bus->fixtureConnection().unregisterObject( TestSupport::FakePortal::portalPath(),
                                               QDBusConnection::UnregisterTree );
    QVERIFY( bus->fixtureConnection().registerVirtualObject( TestSupport::FakePortal::portalPath(),
              &gate, QDBusConnection::SubPath ) );
    backend.configure( QStringLiteral( "pending-token" ) );
    QVERIFY( QTest::qWaitFor( [ & ]() { return gate.pending.type() == QDBusMessage::MethodCallMessage; }, 1000 ) );
    QSignalSpy states( &backend, &HotkeyBackend::stateChanged );
    backend.unbind();
    QCOMPARE( backend.state(), HotkeyBackend::State::Closed );
    const int count = states.count();
    QVERIFY( bus->fixtureConnection().send( gate.pending.createErrorReply(
      QStringLiteral( "org.goldendict.Test" ), QStringLiteral( "late configuration error" ) ) ) );
    QVERIFY( flushWire( bus->fixtureConnection() ) );
    QCOMPARE( states.count(), count );
    bus->fixtureConnection().unregisterObject( TestSupport::FakePortal::portalPath() );
}

void TstPortalHotkeys::portalSignalsMayDestroyBackend_data()
{
    QTest::addColumn< QString >( "signal" );
    for ( QString const & name : { QStringLiteral( "changed" ), QStringLiteral( "closed" ),
                                   QStringLiteral( "owner-loss" ) } )
        QTest::newRow( qPrintable( name ) ) << name;
}

void TstPortalHotkeys::portalSignalsMayDestroyBackend()
{
    QFETCH( QString, signal );
    auto backend = std::make_unique< PortalBackend >( bus->connection() );
    backend->bind( twoBindings() );
    QVERIFY( waitForState( *backend, HotkeyBackend::State::Bound ) );
    connect( backend.get(), &HotkeyBackend::stateChanged, this, [ & ]() { backend.reset(); } );
    if ( signal == QLatin1String( "changed" ) )
        portal->emitShortcutsChanged( portal->sessionPath(), {} );
    else if ( signal == QLatin1String( "closed" ) )
        portal->emitSessionClosed( portal->sessionPath(), {} );
    else
        portal->loseService();
    QVERIFY( QTest::qWaitFor( [ & ]() { return !backend; }, 1000 ) );
}

void TstPortalHotkeys::portalUnbindClosesSessionAndRebindsOnce()
{
    PortalBackend backend( bus->connection() );
    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    const QString firstSession = portal->sessionPath();
    QCOMPARE( portal->bindCountForSession( firstSession ), 1 );

    backend.unbind();
    QVERIFY( waitForState( backend, HotkeyBackend::State::Closed ) );
    QVERIFY( QTest::qWaitFor( [ this ]() {
                 return portal->count( QStringLiteral( "Session.Close" ) ) == 1;
             }, 1000 ) );

    backend.bind( mainBindingOnly() );
    QVERIFY( waitForState( backend, HotkeyBackend::State::Bound ) );
    const QString secondSession = portal->sessionPath();
    QVERIFY( secondSession != firstSession );
    QCOMPARE( portal->bindCountForSession( firstSession ), 1 );
    QCOMPARE( portal->bindCountForSession( secondSession ), 1 );
    QCOMPARE( portal->count( QStringLiteral( "BindShortcuts" ) ), 2 );
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
