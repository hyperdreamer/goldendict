/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Test-only fake desktop portal, see tests/support/fake_portal.hh.
 */

#include "fake_portal.hh"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QHash>
#include <QMetaType>
#include <QVariant>

#include <functional>
#include <utility>

#include <QPair>

namespace TestSupport {

QDBusArgument &operator<<( QDBusArgument & out, FakePortal::Shortcut const & value )
{
    out.beginStructure();
    out << value.id << value.properties;
    out.endStructure();
    return out;
}

QDBusArgument const & operator>>( QDBusArgument const & in, FakePortal::Shortcut & value )
{
    in.beginStructure();
    in >> value.id >> value.properties;
    in.endStructure();
    return in;
}

// File-local aliases: this translation unit is the only consumer.
using Shortcut = FakePortal::Shortcut;
using ShortcutList = QList< Shortcut >;
using Call = FakePortal::Call;

namespace {

/// The request path a client predicts for its own handle token: the sender's
/// unique bus name without the leading ':' and with '.' replaced by '_'.
QString senderElement( QString const & uniqueName )
{
    QString element = uniqueName;

    if ( element.startsWith( QLatin1Char( ':' ) ) )
        element = element.mid( 1 );

    element.replace( QLatin1Char( '.' ), QLatin1Char( '_' ) );
    element.replace( QLatin1Char( '-' ), QLatin1Char( '_' ) );

    return element;
}

QString requestPathFor( QString const & sender, QString const & token )
{
    return FakePortal::portalPath() + QStringLiteral( "/request/" ) + senderElement( sender ) + '/'
        + token;
}

QString sessionPathFor( QString const & sender, QString const & token )
{
    return FakePortal::portalPath() + QStringLiteral( "/session/" ) + senderElement( sender ) + '/'
        + token;
}

QVariant shortcutListVariant( QVector< Shortcut > const & entries )
{
    ShortcutList list;

    for ( Shortcut const & entry : entries )
        list.append( entry );

    return QVariant::fromValue( list );
}

QVariant stringListVariant( QStringList const & values )
{
    return QVariant::fromValue( values );
}

/// Reads one `a{sv}` argument. A QVariantMap that arrived on the wire is a
/// QDBusArgument, not a QVariantMap: toMap() would silently yield an empty map.
QVariantMap variantToMap( QVariant const & value )
{
    if ( value.metaType() == QMetaType::fromType< QDBusArgument >() )
        return qdbus_cast< QVariantMap >( value );

    return value.toMap();
}

QString nextGenerated( QString const & prefix, int & counter )
{
    return prefix + QString::number( ++counter );
}

} // namespace

////////// Private

struct FakePortalPrivate
{
    QDBusConnection connection{ QString() };
    uint configuredVersion = 0;
    QString scenario = QStringLiteral( "fast" );
    QString error;
    bool started = false;
    bool registered = false;
    QString owner;

    QVector< Call > calls;
    QVector< Shortcut > listEntries;
    QVector< Shortcut > boundEntries;
    QStringList boundIds;
    QStringList requestPaths;
    QStringList violations;
    QHash< QString, int > acceptedBinds;
    QString sessionPath;

    QVector< std::function< void() > > deferred;
    int generatedTokens = 0;

    QDBusVirtualObject * root = nullptr;

    void sendReply( QDBusMessage const & message, QString const & path );
    void sendError( QDBusMessage const & message, QString const & name, QString const & text );
    void sendResponse( QString const & path, uint code, QVariantMap const & results );
    void sendSessionClosed( QString const & path, QString const & reason );
    void sendShortcutsChanged( QString const & path, QVector< Shortcut > const & entries );
    void sendActivation( QString const & session, QString const & id, QString const & token,
                         bool deactivated );

    QVector< Shortcut > requestedShortcuts( QDBusMessage const & message ) const;
    QVector< Shortcut > bindPayload( QVector< Shortcut > const & requested ) const;
    QVariant listPayload() const;

    void handleCreateSession( QDBusMessage const & message, QDBusConnection const & sender );
    void handleListShortcuts( QDBusMessage const & message, QDBusConnection const & sender,
                              QString const & senderUnique );
    void handleBindShortcuts( QDBusMessage const & message, QDBusConnection const & sender,
                              QString const & senderUnique );
    void handleConfigureShortcuts( QDBusMessage const & message, QDBusConnection const & sender );
    void handleProperties( QDBusMessage const & message, QDBusConnection const & sender );
    void handleClose( QDBusMessage const & message, QDBusConnection const & sender );

    bool handleMessage( QDBusMessage const & message, QDBusConnection const & sender,
                        bool * handled );

    /// Sends (or defers) one Response according to the scenario. Returns false
    /// when the scenario holds the response until releasePending().
    bool emitResponse( QString const & path, uint code, QVariantMap const & results );

    /// Holds the method reply (and the Response that follows it) until
    /// releasePending(), which is how a test reaches "the method result arrived
    /// after the request gave up".
    void deferMethodReply( QDBusMessage const & message, QString const & replyPath,
                          QString const & responsePath, uint code, QVariantMap const & results );
};

void FakePortalPrivate::sendReply( QDBusMessage const & message, QString const & path )
{
    // The fixture answers the call itself, so Qt must not send an automatic
    // empty reply when handleMessage() returns.
    QDBusMessage call = message;
    call.setDelayedReply( true );

    QDBusMessage reply = call.createReply();
    reply << QVariant::fromValue( QDBusObjectPath( path ) );
    connection.send( reply );
}

void FakePortalPrivate::sendError( QDBusMessage const & message, QString const & name,
                                    QString const & text )
{
    QDBusMessage call = message;
    call.setDelayedReply( true );
    connection.send( call.createErrorReply( name, text ) );
}

void FakePortalPrivate::sendResponse( QString const & path, uint code,
                                        QVariantMap const & results )
{
    QDBusMessage signal = QDBusMessage::createSignal( path, FakePortal::requestInterface(),
                                                      QStringLiteral( "Response" ) );
    signal << code << results;
    connection.send( signal );
}

void FakePortalPrivate::sendSessionClosed( QString const & path, QString const & reason )
{
    QDBusMessage signal = QDBusMessage::createSignal( path, FakePortal::sessionInterface(),
                                                      QStringLiteral( "Closed" ) );
    signal << QVariantMap{ { QStringLiteral( "reason" ), reason } };
    connection.send( signal );
}

void FakePortalPrivate::sendShortcutsChanged( QString const & path,
                                                QVector< Shortcut > const & entries )
{
    QDBusMessage signal = QDBusMessage::createSignal( path, FakePortal::portalInterface(),
                                                      QStringLiteral( "ShortcutsChanged" ) );
    signal << QVariant::fromValue( QDBusObjectPath( path ) ) << shortcutListVariant( entries );
    connection.send( signal );
}

void FakePortalPrivate::sendActivation( QString const & session, QString const & id,
                                          QString const & token, bool deactivated )
{
    QDBusMessage signal = QDBusMessage::createSignal(
        FakePortal::portalPath(), FakePortal::portalInterface(),
        deactivated ? QStringLiteral( "Deactivated" ) : QStringLiteral( "Activated" ) );

    QVariantMap options;

    if ( !token.isEmpty() )
        options.insert( QStringLiteral( "activation_token" ), token );

    signal << QVariant::fromValue( QDBusObjectPath( session ) ) << id
           << QVariant::fromValue( qulonglong( 0 ) ) << options;
    connection.send( signal );
}

bool FakePortalPrivate::emitResponse( QString const & path, uint code,
                                        QVariantMap const & results )
{
    if ( scenario == QLatin1String( "delayed-response" )
         || scenario == QLatin1String( "late-create-response" )
         || scenario == QLatin1String( "list-slow" ) )
    {
        deferred.append( [ this, path, code, results ]() { sendResponse( path, code, results ); } );
        return false;
    }

    sendResponse( path, code, results );

    if ( scenario == QLatin1String( "duplicate-response" ) )
        sendResponse( path, code, results );

    return true;
}

void FakePortalPrivate::deferMethodReply( QDBusMessage const & message,
                                            QString const & replyPath,
                                            QString const & responsePath, uint code,
                                            QVariantMap const & results )
{
    QDBusMessage const call = message;

    deferred.append( [ this, call, replyPath, responsePath, code, results ]() {
        sendReply( call, replyPath );
        sendResponse( responsePath, code, results );
    } );
}

QVector< Shortcut > FakePortalPrivate::requestedShortcuts( QDBusMessage const & message ) const
{
    QVector< Shortcut > requested;

    const QList< QVariant > arguments = message.arguments();

    if ( arguments.size() < 2 )
        return requested;

    const QVariant & value = arguments.at( 1 );

    if ( value.metaType() != QMetaType::fromType< QDBusArgument >() )
        return requested;

    const ShortcutList list = qdbus_cast< ShortcutList >( value );
    requested.reserve( list.size() );

    for ( Shortcut const & entry : list )
        requested.append( entry );

    return requested;
}

QVector< Shortcut > FakePortalPrivate::bindPayload( QVector< Shortcut > const & requested ) const
{
    if ( scenario == QLatin1String( "bind-empty" ) )
        return QVector< Shortcut >();

    if ( scenario == QLatin1String( "bind-unknown-id" ) )
    {
        QVector< Shortcut > payload = requested;
        Shortcut unknown;
        unknown.id = QStringLiteral( "unknown-shortcut-id");
        unknown.properties.insert( QStringLiteral( "description" ), QStringLiteral( "unknown" ) );
        payload.append( unknown );
        return payload;
    }

    if ( scenario == QLatin1String( "bind-duplicate-id" ) )
    {
        QVector< Shortcut > payload = requested;
        if ( !payload.isEmpty() )
            payload.append( payload.first() );
        return payload;
    }

    if ( !boundIds.isEmpty() )
    {
        QVector< Shortcut > payload;

        for ( QString const & id : boundIds )
        {
            for ( Shortcut const & entry : requested )
            {
                if ( entry.id == id )
                {
                    payload.append( entry );
                    break;
                }
            }
        }

        return payload;
    }

    if ( !boundEntries.isEmpty() )
        return boundEntries;

    if ( scenario == QLatin1String( "bind-partial" ) )
        return requested.size() > 1 ? QVector< Shortcut >{ requested.first() } : requested;

    return requested;
}

QVariant FakePortalPrivate::listPayload() const
{
    QVector< Shortcut > entries = listEntries;

    if ( scenario == QLatin1String( "list-property-wrong-type" ) )
    {
        if ( entries.isEmpty() )
        {
            Shortcut entry;
            entry.id = QStringLiteral( "main-window" );
            entries.append( entry );
        }

        entries[ 0 ].properties.insert( QStringLiteral( "description" ),
                                        QVariant::fromValue( uint( 1 ) ) );
    }

    if ( scenario == QLatin1String( "list-duplicate-ids" ) )
    {
        if ( entries.isEmpty() )
        {
            Shortcut entry;
            entry.id = QStringLiteral( "main-window" );
            entries.append( entry );
        }

        entries.append( entries.first() );
    }

    if ( scenario == QLatin1String( "list-malformed-tuple" ) )
    {
        // `as`: bare strings where the wire promises (sa{sv}) tuples.
        QStringList ids;
        for ( Shortcut const & entry : entries )
            ids.append( entry.id );
        return stringListVariant( ids );
    }

    if ( scenario == QLatin1String( "list-malformed-map" ) )
    {
        // `aa{sv}`: bare property maps where the wire promises `(sa{sv})`
        // tuples, so the id is not part of a structure at all.
        QList< QVariantMap > maps;
        for ( Shortcut const & entry : entries )
            maps.append( entry.properties );
        return QVariant::fromValue( maps );
    }

    return shortcutListVariant( entries );
}

void FakePortalPrivate::handleCreateSession( QDBusMessage const & message,
                                               QDBusConnection const & sender )
{
    const QString senderUnique = message.service();
    const QVariantMap options = variantToMap( message.arguments().value( 0 ) );

    QString handleToken = options.value( QStringLiteral( "handle_token" ) ).toString();
    if ( handleToken.isEmpty() )
        handleToken = nextGenerated( QStringLiteral( "gd_fixture_request_" ), generatedTokens );

    QString sessionToken = options.value( QStringLiteral( "session_handle_token" ) ).toString();
    if ( sessionToken.isEmpty() )
        sessionToken = nextGenerated( QStringLiteral( "gd_fixture_session_" ), generatedTokens );

    const QString predictedPath = requestPathFor( senderUnique, handleToken );
    const QString alternatePath = requestPathFor( senderUnique, handleToken + QStringLiteral( "_alt" ) );

    const bool alternate = scenario == QLatin1String( "early-alternate-response" )
        || scenario == QLatin1String( "alternate-response" );

    const QString actualPath = alternate ? alternatePath : predictedPath;
    const QString session = sessionPathFor( senderUnique, sessionToken );

    Call record;
    record.member = QStringLiteral( "CreateSession" );
    record.requestPath = actualPath;
    record.options = options;
    record.signature = message.signature().toUtf8();
    calls.append( record );

    requestPaths.append( actualPath );

    QVariantMap results;

    if ( scenario == QLatin1String( "invalid-session-handle" ) )
        results.insert( QStringLiteral( "session_handle" ), QStringLiteral( "not-an-object-path" ) );
    else if ( scenario == QLatin1String( "missing-session-handle" ) )
    {
        // Deliberately empty: the response has no session_handle at all.
    }
    else
    {
        results.insert( QStringLiteral( "session_handle" ), session );
        if ( scenario != QLatin1String( "late-create-response" ) )
            sessionPath = session;
        else
            sessionPath = QString();
    }

    const uint code = scenario == QLatin1String( "denied" ) ? 1
        : scenario == QLatin1String( "failed" )    ? 2
                                                   : 0;

    const bool early = scenario == QLatin1String( "early-response" )
        || scenario == QLatin1String( "early-alternate-response" );

    if ( early )
    {
        // The Response for the *eventual* request path arrives before the method
        // result names it: exactly the Task 30 race.
        emitResponse( actualPath, code, results );
        sendReply( message, actualPath );
        return;
    }

    if ( scenario == QLatin1String( "no-method-reply" ) )
    {
        // Neither the method result nor a Response ever arrives.
        return;
    }

    if ( scenario == QLatin1String( "delayed-method-reply" ) )
    {
        deferMethodReply( message, actualPath, actualPath, code, results );
        return;
    }

    if ( scenario == QLatin1String( "method-error" ) )
    {
        sendError( message, QStringLiteral( "org.freedesktop.portal.Error.Failed" ),
                   QStringLiteral( "fixture method failure" ) );
        return;
    }

    if ( scenario == QLatin1String( "malformed-method-return" ) )
    {
        QDBusMessage reply = message.createReply();
        reply << QStringLiteral( "this-is-not-a-request-path" );
        connection.send( reply );
        return;
    }

    if ( scenario == QLatin1String( "late-create-response" ) )
    {
        // The client is expected to cancel before this session exists; the
        // fixture remembers the path so the test can prove the late session was
        // closed instead of becoming current.
        QString const lateSession = session;
        deferred.append( [ this, predictedPath, lateSession ]() {
            sessionPath = lateSession;
            sendResponse( predictedPath, 0,
                          QVariantMap{ { QStringLiteral( "session_handle" ), lateSession } } );
        } );
        sendReply( message, predictedPath );
        return;
    }

    // Everything that is not deliberately "early" answers the method call first
    // and only then produces its Response, which is what the real portal does.
    sendReply( message, actualPath );

    if ( scenario == QLatin1String( "unrelated-response" ) )
    {
        // A perfect Response on a path that belongs to no request of ours: the
        // client must ignore it instead of completing this request.
        sendResponse( requestPathFor( senderUnique, QStringLiteral( "unrelated_request" ) ),
                      code, results );
        return;
    }

    if ( scenario == QLatin1String( "no-response" ) )
        return;

    emitResponse( actualPath, code, results );
}

void FakePortalPrivate::handleListShortcuts( QDBusMessage const & message,
                                               QDBusConnection const & sender,
                                               QString const & senderUnique )
{
    Q_UNUSED( sender )

    const QList< QVariant > arguments = message.arguments();
    const QString session = arguments.isEmpty()
        ? QString()
        : arguments.at( 0 ).value< QDBusObjectPath >().path();
    const QVariantMap options = arguments.size() > 1 ? variantToMap( arguments.at( 1 ) ) : QVariantMap();

    QString handleToken = options.value( QStringLiteral( "handle_token" ) ).toString();
    if ( handleToken.isEmpty() )
        handleToken = nextGenerated( QStringLiteral( "gd_fixture_list_" ), generatedTokens );

    const QString path = requestPathFor( senderUnique, handleToken );

    Call record;
    record.member = QStringLiteral( "ListShortcuts" );
    record.session = session;
    record.requestPath = path;
    record.options = options;
    record.signature = message.signature().toUtf8();
    calls.append( record );

    requestPaths.append( path );

    QVariantMap results;

    if ( scenario == QLatin1String( "denied" ) || scenario == QLatin1String( "failed" ) )
    {
        const uint code = scenario == QLatin1String( "denied" ) ? 1 : 2;
        sendReply( message, path );
        emitResponse( path, code, results );
        return;
    }

    results.insert( QStringLiteral( "shortcuts" ), listPayload() );

    sendReply( message, path );

    if ( scenario != QLatin1String( "no-response" ) )
        emitResponse( path, 0, results );
}

void FakePortalPrivate::handleBindShortcuts( QDBusMessage const & message,
                                               QDBusConnection const & sender,
                                               QString const & senderUnique )
{
    Q_UNUSED( sender )

    const QList< QVariant > arguments = message.arguments();
    const QString session = arguments.isEmpty()
        ? QString()
        : arguments.at( 0 ).value< QDBusObjectPath >().path();
    const QString parentWindow = arguments.size() > 2 ? arguments.at( 2 ).toString() : QString();
    const QVariantMap options = arguments.size() > 3 ? variantToMap( arguments.at( 3 ) ) : QVariantMap();
    const QVector< Shortcut > requested = requestedShortcuts( message );

    QString handleToken = options.value( QStringLiteral( "handle_token" ) ).toString();
    if ( handleToken.isEmpty() )
        handleToken = nextGenerated( QStringLiteral( "gd_fixture_bind_" ), generatedTokens );

    const QString path = requestPathFor( senderUnique, handleToken );

    Call record;
    record.member = QStringLiteral( "BindShortcuts" );
    record.session = session;
    record.requestPath = path;
    record.parentWindow = parentWindow;
    record.options = options;
    record.signature = message.signature().toUtf8();

    for ( Shortcut const & entry : requested )
    {
        record.shortcutIds.append( entry.id );
        record.shortcuts.append( entry );
    }

    // The one-Bind-per-session invariant is enforced, not merely documented: a
    // second Bind is a protocol violation the fixture refuses and records. The
    // refused attempt stays visible in calls(), but it never counts as a bind.
    if ( acceptedBinds.value( session ) > 0 )
    {
        calls.append( record );
        violations.append( QStringLiteral( "second BindShortcuts on session %1" ).arg( session ) );
        sendError( message, QStringLiteral( "org.freedesktop.DBus.Error.Failed" ),
                   QStringLiteral( "the fixture allows one BindShortcuts per session" ) );
        return;
    }

    ++acceptedBinds[ session ];
    calls.append( record );
    requestPaths.append( path );

    QVariantMap results;

    if ( scenario == QLatin1String( "bind-wrong-type" ) )
    {
        QStringList ids;
        for ( Shortcut const & entry : requested )
            ids.append( entry.id );
        results.insert( QStringLiteral( "shortcuts" ), stringListVariant( ids ) );
    }
    else
        results.insert( QStringLiteral( "shortcuts" ), shortcutListVariant( bindPayload( requested ) ) );

    const uint code = scenario == QLatin1String( "bind-denied" ) ? 1
        : scenario == QLatin1String( "bind-failed" )           ? 2
                                                               : 0;

    sendReply( message, path );

    if ( scenario != QLatin1String( "no-response" ) )
        emitResponse( path, code, results );
}

void FakePortalPrivate::handleConfigureShortcuts( QDBusMessage const & message,
                                                    QDBusConnection const & sender )
{
    Q_UNUSED( sender )

    if ( configuredVersion < 2 )
    {
        Call record;
        record.member = QStringLiteral( "ConfigureShortcuts" );
        record.session = message.arguments().value( 0 ).value< QDBusObjectPath >().path();
        record.signature = message.signature().toUtf8();
        calls.append( record );

        sendError( message, QStringLiteral( "org.freedesktop.DBus.Error.UnknownMethod" ),
                   QStringLiteral( "ConfigureShortcuts requires version 2" ) );
        return;
    }

    const QList< QVariant > arguments = message.arguments();
    const QVariantMap options = arguments.size() > 2 ? variantToMap( arguments.at( 2 ) ) : QVariantMap();

    Call record;
    record.member = QStringLiteral( "ConfigureShortcuts" );
    record.session = arguments.value( 0 ).value< QDBusObjectPath >().path();
    record.parentWindow = arguments.size() > 1 ? arguments.at( 1 ).toString() : QString();
    record.activationToken = options.value( QStringLiteral( "activation_token" ) ).toString();
    record.options = options;
    record.signature = message.signature().toUtf8();
    calls.append( record );

    if ( scenario == QLatin1String( "configure-error" ) )
    {
        sendError( message, QStringLiteral( "org.freedesktop.portal.Error.Failed" ),
                   QStringLiteral( "fixture configure failure" ) );
        return;
    }

    // No Request, no Response: an asynchronous method result is all v2 promises.
    QDBusMessage call = message;
    call.setDelayedReply( true );
    connection.send( call.createReply() );
}

void FakePortalPrivate::handleProperties( QDBusMessage const & message,
                                            QDBusConnection const & sender )
{
    Q_UNUSED( sender )

    const QList< QVariant > arguments = message.arguments();
    const QString name = arguments.value( 1 ).toString();

    Call record;
    record.member = message.member() == QLatin1String( "GetAll" )
        ? QStringLiteral( "Properties.GetAll" )
        : QStringLiteral( "Properties.Get" );
    record.options = QVariantMap{ { QStringLiteral( "property" ), name } };
    record.signature = message.signature().toUtf8();
    calls.append( record );

    if ( name != QLatin1String( "version" ) )
    {
        sendError( message, QStringLiteral( "org.freedesktop.DBus.Error.UnknownProperty" ),
                   QStringLiteral( "No such property \"%1\"" ).arg( name ) );
        return;
    }

    if ( scenario == QLatin1String( "no-version" ) )
    {
        sendError( message, QStringLiteral( "org.freedesktop.DBus.Error.UnknownProperty" ),
                   QStringLiteral( "No such property \"version\"" ) );
        return;
    }

    if ( message.member() == QLatin1String( "GetAll" ) )
    {
        QDBusMessage allCall = message;
        allCall.setDelayedReply( true );

        QDBusMessage reply = allCall.createReply();
        reply << QVariantMap{ { QStringLiteral( "version" ),
                                QVariant::fromValue( configuredVersion ) } };
        connection.send( reply );
        return;
    }

    QVariant value = scenario == QLatin1String( "version-wrong-type" )
        ? QVariant( QString::number( configuredVersion ) )
        : QVariant::fromValue( configuredVersion );

    QDBusMessage call = message;
    call.setDelayedReply( true );

    QDBusMessage reply = call.createReply();
    reply << QVariant::fromValue( QDBusVariant( value ) );
    connection.send( reply );
}

void FakePortalPrivate::handleClose( QDBusMessage const & message,
                                       QDBusConnection const & sender )
{
    Q_UNUSED( sender )

    const QString path = message.path();

    Call record;
    record.signature = message.signature().toUtf8();

    if ( message.interface() == FakePortal::requestInterface() )
        record.member = QStringLiteral( "Request.Close" );
    else
        record.member = QStringLiteral( "Session.Close" );

    record.requestPath = path;
    record.session = path;
    calls.append( record );

    // Neither Close promises a Response.
    QDBusMessage call = message;
    call.setDelayedReply( true );
    connection.send( call.createReply() );
}

bool FakePortalPrivate::handleMessage( QDBusMessage const & message,
                                         QDBusConnection const & sender, bool * handled )
{
    if ( message.type() != QDBusMessage::MethodCallMessage )
        return false;

    const QString path = message.path();
    const QString interface = message.interface();
    const QString member = message.member();
    const QString senderUnique = message.service();

    if ( path == FakePortal::portalPath() )
    {
        if ( interface == QLatin1String( "org.freedesktop.DBus.Properties" ) )
        {
            if ( member == QLatin1String( "Get" ) || member == QLatin1String( "GetAll" ) )
            {
                handleProperties( message, sender );
                *handled = true;
                return true;
            }

            return false;
        }

        if ( interface == FakePortal::portalInterface() )
        {
            if ( member == QLatin1String( "CreateSession" ) )
            {
                handleCreateSession( message, sender );
                *handled = true;
                return true;
            }

            if ( member == QLatin1String( "ListShortcuts" ) )
            {
                handleListShortcuts( message, sender, senderUnique );
                *handled = true;
                return true;
            }

            if ( member == QLatin1String( "BindShortcuts" ) )
            {
                handleBindShortcuts( message, sender, senderUnique );
                *handled = true;
                return true;
            }

            if ( member == QLatin1String( "ConfigureShortcuts" ) )
            {
                handleConfigureShortcuts( message, sender );
                *handled = true;
                return true;
            }
        }

        return false;
    }

    if ( path.startsWith( FakePortal::portalPath() + QStringLiteral( "/request/" ) ) )
    {
        if ( member == QLatin1String( "Close" ) )
        {
            handleClose( message, sender );
            *handled = true;
            return true;
        }

        return false;
    }

    if ( path.startsWith( FakePortal::portalPath() + QStringLiteral( "/session/" ) ) )
    {
        if ( member == QLatin1String( "Close" ) )
        {
            handleClose( message, sender );
            *handled = true;
            return true;
        }

        return false;
    }

    return false;
}

namespace {

/// Minimal introspection: enough to be honest about the exported interfaces
/// without pretending to be the real portal's full XML.
QString introspectionFor( QString const & path )
{
    if ( path == FakePortal::portalPath() )
    {
        return QStringLiteral( R"XML(<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
  <interface name="org.freedesktop.DBus.Properties">
    <method name="Get"><arg direction="in" type="s"/><arg direction="in" type="s"/><arg direction="out" type="v"/></method>
    <method name="GetAll"><arg direction="in" type="s"/><arg direction="out" type="a{sv}"/></method>
  </interface>
  <interface name="org.freedesktop.portal.GlobalShortcuts">
    <method name="CreateSession"><arg direction="in" type="a{sv}" name="options"/><arg direction="out" type="o" name="handle"/></method>
    <method name="ListShortcuts"><arg direction="in" type="o" name="session_handle"/><arg direction="in" type="a{sv}" name="options"/><arg direction="out" type="o" name="handle"/></method>
    <method name="BindShortcuts"><arg direction="in" type="o" name="session_handle"/><arg direction="in" type="a(sa{sv})" name="shortcuts"/><arg direction="in" type="s" name="parent_window"/><arg direction="in" type="a{sv}" name="options"/><arg direction="out" type="o" name="handle"/></method>
    <method name="ConfigureShortcuts"><arg direction="in" type="o" name="session_handle"/><arg direction="in" type="s" name="parent_window"/><arg direction="in" type="a{sv}" name="options"/></method>
    <signal name="Activated"><arg type="o"/><arg type="s"/><arg type="t"/><arg type="a{sv}"/></signal>
    <signal name="Deactivated"><arg type="o"/><arg type="s"/><arg type="t"/><arg type="a{sv}"/></signal>
    <signal name="ShortcutsChanged"><arg type="o"/><arg type="a(sa{sv})"/></signal>
  </interface>
</node>
)XML" );
    }

    if ( path.startsWith( FakePortal::portalPath() + QStringLiteral( "/request/" ) ) )
    {
        return QStringLiteral( R"XML(<node>
  <interface name="org.freedesktop.portal.Request">
    <method name="Close"/>
    <signal name="Response"><arg type="u"/><arg type="a{sv}"/></signal>
  </interface>
</node>
)XML" );
    }

    if ( path.startsWith( FakePortal::portalPath() + QStringLiteral( "/session/" ) ) )
    {
        return QStringLiteral( R"XML(<node>
  <interface name="org.freedesktop.portal.Session">
    <method name="Close"/>
    <signal name="Closed"><arg type="a{sv}"/></signal>
  </interface>
</node>
)XML" );
    }

    return QStringLiteral( "<node/>" );
}

class PortalRoot final : public QDBusVirtualObject
{
public:
    explicit PortalRoot( FakePortalPrivate * state_ ): state( state_ ) {}

    QString introspect( QString const & path ) const override
    {
        return introspectionFor( path );
    }

    bool handleMessage( QDBusMessage const & message, QDBusConnection const & connection ) override
    {
        Q_UNUSED( connection )

        bool handled = false;
        state->handleMessage( message, connection, &handled );

        // Returning true tells Qt the message was ours. A scenario that
        // deliberately never replies (no-method-reply) must still claim the
        // call, otherwise Qt would answer with an "unknown method" error.
        return handled;
    }

private:
    FakePortalPrivate * state;
};

} // namespace

////////// FakePortal

FakePortal::FakePortal(): d( std::make_unique< FakePortalPrivate >() )
{
    qDBusRegisterMetaType< Shortcut >();
    qDBusRegisterMetaType< ShortcutList >();

    // The deliberately malformed `list-malformed-map` payload is a list of bare
    // `a{sv}` maps: registering it gives the exact `aa{sv}` wire signature.
    qDBusRegisterMetaType< QList< QVariantMap > >();
}

FakePortal::~FakePortal()
{
    if ( !d )
        return;

    if ( d->root )
    {
        if ( d->registered )
            d->connection.unregisterService( serviceName() );

        d->connection.unregisterObject( portalPath() );
        delete d->root;
        d->root = nullptr;
        d->registered = false;
    }

    d.reset();
}

bool FakePortal::start( QDBusConnection const & bus, uint version )
{
    if ( !d )
        return false;

    if ( d->started )
    {
        d->error = QStringLiteral( "the fake portal is already started" );
        return false;
    }

    if ( !bus.isConnected() )
    {
        d->error = QStringLiteral( "the supplied bus connection is not connected" );
        return false;
    }

    // registerService/registerVirtualObject are mutating operations, so the copy
    // kept for the fixture has to be mutable.
    QDBusConnection connection = bus;
    d->connection = connection;
    d->configuredVersion = version;
    d->owner = connection.baseService();

    if ( d->listEntries.isEmpty() )
    {
        Shortcut mainWindow;
        mainWindow.id = QStringLiteral( "main-window" );
        mainWindow.properties.insert( QStringLiteral( "description" ),
                                       QStringLiteral( "Show the main window" ) );
        mainWindow.properties.insert( QStringLiteral( "trigger_description" ),
                                       QStringLiteral( "Ctrl+F11" ) );
        d->listEntries.append( mainWindow );

        Shortcut clipboard;
        clipboard.id = QStringLiteral( "clipboard-lookup" );
        clipboard.properties.insert( QStringLiteral( "description" ),
                                      QStringLiteral( "Look up the clipboard" ) );
        clipboard.properties.insert( QStringLiteral( "trigger_description" ),
                                      QStringLiteral( "Ctrl+C C" ) );
        d->listEntries.append( clipboard );
    }

    d->root = new PortalRoot( d.get() );

    if ( !connection.registerVirtualObject( portalPath(), d->root, QDBusConnection::SubPath ) )
    {
        d->error = QStringLiteral( "cannot export %1 on the private bus" ).arg( portalPath() );
        delete d->root;
        d->root = nullptr;
        return false;
    }

    if ( !connection.registerService( serviceName() ) )
    {
        d->error = QStringLiteral( "cannot take the name %1 on the private bus (taken by %2)" )
                       .arg( serviceName(),
                             connection.interface()->serviceOwner( serviceName() ).value() );
        connection.unregisterObject( portalPath() );
        delete d->root;
        d->root = nullptr;
        return false;
    }
    d->started = true;
    d->registered = true;
    d->error.clear();

    return true;
}

void FakePortal::setScenario( QString const & scenario )
{
    if ( d )
        d->scenario = scenario;
}

QString FakePortal::scenario() const
{
    return d ? d->scenario : QString();
}

QVector< FakePortal::Call > FakePortal::calls() const
{
    return d ? d->calls : QVector< Call >();
}

void FakePortal::emitActivation( QString const & session, QString const & id, QString const & token )
{
    if ( d )
        d->sendActivation( session, id, token, false );
}

void FakePortal::loseService()
{
    if ( !d || !d->registered )
        return;

    d->connection.unregisterService( serviceName() );
    d->registered = false;
}

QString FakePortal::serviceName()
{
    return QStringLiteral( "org.freedesktop.portal.Desktop" );
}

QString FakePortal::portalPath()
{
    return QStringLiteral( "/org/freedesktop/portal/desktop" );
}

QString FakePortal::portalInterface()
{
    return QStringLiteral( "org.freedesktop.portal.GlobalShortcuts" );
}

QString FakePortal::requestInterface()
{
    return QStringLiteral( "org.freedesktop.portal.Request" );
}

QString FakePortal::sessionInterface()
{
    return QStringLiteral( "org.freedesktop.portal.Session" );
}

QStringList FakePortal::scenarios()
{
    return { QStringLiteral( "fast" ),
             QStringLiteral( "early-response" ),
             QStringLiteral( "early-alternate-response" ),
             QStringLiteral( "alternate-response" ),
             QStringLiteral( "delayed-response" ),
             QStringLiteral( "delayed-method-reply" ),
             QStringLiteral( "no-response" ),
             QStringLiteral( "no-method-reply" ),
             QStringLiteral( "method-error" ),
             QStringLiteral( "malformed-method-return" ),
             QStringLiteral( "duplicate-response" ),
             QStringLiteral( "unrelated-response" ),
             QStringLiteral( "invalid-session-handle" ),
             QStringLiteral( "missing-session-handle" ),
             QStringLiteral( "late-create-response" ),
             QStringLiteral( "denied" ),
             QStringLiteral( "failed" ),
             QStringLiteral( "list-slow" ),
             QStringLiteral( "list-malformed-tuple" ),
             QStringLiteral( "list-malformed-map" ),
             QStringLiteral( "list-duplicate-ids" ),
             QStringLiteral( "list-property-wrong-type" ),
             QStringLiteral( "bind-partial" ),
             QStringLiteral( "bind-empty" ),
             QStringLiteral( "bind-unknown-id" ),
             QStringLiteral( "bind-duplicate-id" ),
             QStringLiteral( "bind-wrong-type" ),
             QStringLiteral( "bind-denied" ),
             QStringLiteral( "bind-failed" ),
             QStringLiteral( "configure-error" ),
             QStringLiteral( "no-version" ),
             QStringLiteral( "version-wrong-type" ) };
}

void FakePortal::setListShortcuts( QVector< Shortcut > const & entries )
{
    if ( d )
        d->listEntries = entries;
}

void FakePortal::setBoundShortcuts( QVector< Shortcut > const & entries )
{
    if ( d )
        d->boundEntries = entries;
}

void FakePortal::setBoundIds( QStringList const & ids )
{
    if ( d )
        d->boundIds = ids;
}

void FakePortal::emitDeactivated( QString const & session, QString const & id,
                                  QString const & token )
{
    if ( d )
        d->sendActivation( session, id, token, true );
}

void FakePortal::emitShortcutsChanged( QString const & session,
                                       QVector< Shortcut > const & entries )
{
    if ( d )
        d->sendShortcutsChanged( session, entries );
}

void FakePortal::emitSessionClosed( QString const & session, QString const & reason )
{
    if ( d )
        d->sendSessionClosed( session, reason );
}

void FakePortal::releasePending()
{
    if ( !d )
        return;

    const QVector< std::function< void() > > pending = d->deferred;
    d->deferred.clear();

    for ( auto const & action : pending )
        action();
}

int FakePortal::receivedCallCount() const
{
    return d ? d->calls.size() : 0;
}

int FakePortal::count( QString const & member ) const
{
    int matches = 0;

    if ( !d )
        return matches;

    for ( Call const & call : d->calls )
    {
        if ( call.member == member )
            ++matches;
    }

    return matches;
}

int FakePortal::bindCountForSession( QString const & session ) const
{
    if ( !d )
        return 0;

    return d->acceptedBinds.value( session );
}

QStringList FakePortal::requestPaths() const
{
    return d ? d->requestPaths : QStringList();
}

QStringList FakePortal::protocolViolations() const
{
    return d ? d->violations : QStringList();
}

QString FakePortal::sessionPath() const
{
    return d ? d->sessionPath : QString();
}

QString FakePortal::serviceOwner() const
{
    if ( !d )
        return QString();

    return d->connection.interface()->serviceOwner( serviceName() ).value();
}

uint FakePortal::version() const
{
    return d ? d->configuredVersion : 0;
}

bool FakePortal::isStarted() const
{
    return d && d->started;
}

bool FakePortal::isRegistered() const
{
    return d && d->registered;
}

QString FakePortal::errorString() const
{
    return d ? d->error : QStringLiteral( "no fixture implementation" );
}

} // namespace TestSupport
