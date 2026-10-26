/* This file is part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#include "portalhotkeys.hh"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QElapsedTimer>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QVector>

namespace {
const QString Service = QStringLiteral( "org.freedesktop.portal.Desktop" );
const QString RequestInterface = QStringLiteral( "org.freedesktop.portal.Request" );
const QString SessionInterface = QStringLiteral( "org.freedesktop.portal.Session" );
const QString RequestRoot = QStringLiteral( "/org/freedesktop/portal/desktop/request/" );
// Leave dispatch headroom inside the specified five-second wall-clock bound.
constexpr int DeadlineMs = 4900;
constexpr int CleanupMs = 5000;
constexpr int MaxCandidates = 64;

bool validElement( QString const & value )
{
    if ( value.isEmpty() )
        return false;
    for ( QChar ch : value )
    {
        const ushort c = ch.unicode();
        if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' )
                || ( c >= '0' && c <= '9' ) || c == '_' ) )
            return false;
    }
    return true;
}

bool validPath( QString const & value )
{
    if ( value == QLatin1String( "/" ) )
        return true;
    if ( !value.startsWith( QLatin1Char( '/' ) ) )
        return false;
    const QStringList elements = value.mid( 1 ).split( QLatin1Char( '/' ) );
    for ( QString const & element : elements )
        if ( !validElement( element ) )
            return false;
    return true;
}

QString uniqueToken()
{
    return QStringLiteral( "gd_" ) + QUuid::createUuid().toString( QUuid::Id128 );
}
}

/// The small, bounded cleanup observer can outlive the public request. In
/// particular, destroying a cancelled CreateSession must not leak its late session.
class PortalRequestState final : public QObject
{
    Q_OBJECT
public:
    PortalRequestState( QDBusConnection const & connection, quint64 generation_, PortalRequest * target_ ):
        QObject( target_ ), bus( connection ), generation( generation_ ),
        target( target_ ), serviceWatcher( Service, bus, QDBusServiceWatcher::WatchForOwnerChange, this )
    {
        timer.setSingleShot( true );
        timer.setTimerType( Qt::PreciseTimer );
        connect( &timer, &QTimer::timeout, this, [ this ]() {
            if ( retired )
                dispose();
            else
                fail( QStringLiteral( "Portal request deadline exceeded" ) );
        } );
        connect( &serviceWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                 [ this ]( QString const &, QString const &, QString const & ) {
            if ( !started || disposed )
                return;
            owner.clear();
            candidates.clear();
            const bool notify = !retired;
            retired = true;
            auto receiver = target;
            dispose();
            if ( notify && receiver )
                emit receiver->failed( generation, QStringLiteral( "Portal service owner changed" ) );
        } );
    }

    void start( QDBusMessage message, QString const & requestedToken )
    {
        if ( started || retired || disposed )
            return;
        started = true;
        elapsed.start();
        timer.start( DeadlineMs );
        createSession = message.member() == QLatin1String( "CreateSession" );

        const QString token = validElement( requestedToken ) ? requestedToken : uniqueToken();
        QString sender = bus.baseService();
        if ( sender.startsWith( QLatin1Char( ':' ) ) )
            sender.remove( 0, 1 );
        sender.replace( QLatin1Char( '.' ), QLatin1Char( '_' ) );
        sender.replace( QLatin1Char( '-' ), QLatin1Char( '_' ) );
        predictedPath = RequestRoot + sender + QLatin1Char( '/' ) + token;

        auto arguments = message.arguments();
        if ( arguments.isEmpty() || arguments.last().metaType() != QMetaType::fromType< QVariantMap >() )
        {
            fail( QStringLiteral( "Portal request has invalid options" ) );
            return;
        }
        QVariantMap options = arguments.last().toMap();
        options.insert( QStringLiteral( "handle_token" ), token );
        if ( createSession && !validElement( options.value( QStringLiteral( "session_handle_token" ) ).toString() ) )
            options.insert( QStringLiteral( "session_handle_token" ), uniqueToken() );
        arguments.last() = options;
        message.setArguments( arguments );

        // Deliberately receive all senders: identity is checked on every envelope,
        // including candidates buffered before the returned path is known.
        subscribed = bus.connect( QString(), QString(), RequestInterface, QStringLiteral( "Response" ),
                                  this, SLOT( receiveResponse( QDBusMessage ) ) );
        if ( !subscribed )
        {
            fail( QStringLiteral( "Cannot subscribe to portal responses" ) );
            return;
        }

        // Never use the blocking serviceOwner()/pendingCall wait helpers here.
        QDBusMessage lookup = QDBusMessage::createMethodCall(
            QStringLiteral( "org.freedesktop.DBus" ), QStringLiteral( "/org/freedesktop/DBus" ),
            QStringLiteral( "org.freedesktop.DBus" ), QStringLiteral( "GetNameOwner" ) );
        lookup << Service;
        auto watcher = new QDBusPendingCallWatcher( bus.asyncCall( lookup, DeadlineMs ), this );
        connect( watcher, &QDBusPendingCallWatcher::finished, this,
                 [ this, message ]( QDBusPendingCallWatcher * finished ) {
            const QDBusMessage reply = finished->reply();
            finished->deleteLater();
            if ( disposed || retired )
                return;
            if ( expired() )
                return;
            if ( reply.type() != QDBusMessage::ReplyMessage || reply.signature() != QLatin1String( "s" )
                 || reply.arguments().size() != 1 )
            {
                fail( QStringLiteral( "Portal service is unavailable" ) );
                return;
            }
            owner = reply.arguments().first().toString();
            if ( !owner.startsWith( QLatin1Char( ':' ) ) )
            {
                fail( QStringLiteral( "Portal service has no unique owner" ) );
                return;
            }
            // Pin the invocation and all Close calls to this unique service owner.
            QDBusMessage call = QDBusMessage::createMethodCall(
                owner, message.path(), message.interface(), message.member() );
            call.setArguments( message.arguments() );
            invoked = true;
            auto pending = new QDBusPendingCallWatcher(
                bus.asyncCall( call, DeadlineMs + CleanupMs ), this );
            connect( pending, &QDBusPendingCallWatcher::finished, this,
                     [ this ]( QDBusPendingCallWatcher * done ) {
                const QDBusMessage result = done->reply();
                done->deleteLater();
                methodFinished( result );
            } );
        } );
    }

    void cancel() noexcept
    {
        if ( disposed || retired )
            return;
        retired = true;
        closeRequests();
        if ( !invoked )
            dispose();
        else
            timer.start( CleanupMs );
    }

    void detach() noexcept
    {
        target.clear();
        setParent( QCoreApplication::instance() );
        cancel();
    }

private:
    struct Envelope
    {
        QString path;
        uint code;
        QVariantMap results;
    };

    bool expired()
    {
        if ( !retired && elapsed.elapsed() >= DeadlineMs )
        {
            fail( QStringLiteral( "Portal request deadline exceeded" ) );
            return true;
        }
        return false;
    }

    void dispose()
    {
        if ( disposed )
            return;
        disposed = true;
        timer.stop();
        candidates.clear();
        if ( subscribed )
        {
            bus.disconnect( QString(), QString(), RequestInterface, QStringLiteral( "Response" ),
                            this, SLOT( receiveResponse( QDBusMessage ) ) );
            subscribed = false;
        }
        serviceWatcher.setWatchedServices( {} );
        deleteLater();
    }

    void close( QString const & path, QString const & interface )
    {
        if ( owner.isEmpty() || path.isEmpty() || closed.contains( path ) )
            return;
        closed.insert( path );
        QDBusMessage message = QDBusMessage::createMethodCall(
            owner, path, interface, QStringLiteral( "Close" ) );
        bus.send( message );
    }

    void closeRequests()
    {
        if ( !invoked )
            return;
        close( predictedPath, RequestInterface );
        close( returnedPath, RequestInterface );
    }

    void fail( QString const & explanation )
    {
        if ( disposed || retired )
            return;
        // Retire before calling user code; cancel/delete from a signal is safe.
        auto receiver = target;
        cancel();
        if ( receiver )
            emit receiver->failed( generation, explanation );
    }

    bool matches( QString const & path ) const
    {
        return path == predictedPath || path == returnedPath;
    }

    void methodFinished( QDBusMessage const & reply )
    {
        if ( disposed )
            return;
        expired();
        if ( reply.type() == QDBusMessage::ErrorMessage )
        {
            fail( QStringLiteral( "Portal method failed: " ) + reply.errorMessage() );
            dispose();
            return;
        }
        if ( reply.service() != owner )
            return;
        if ( reply.type() != QDBusMessage::ReplyMessage || reply.signature() != QLatin1String( "o" )
             || reply.arguments().size() != 1
             || reply.arguments().first().metaType() != QMetaType::fromType< QDBusObjectPath >() )
        {
            fail( QStringLiteral( "Portal method returned an invalid request path" ) );
            dispose();
            return;
        }
        returnedPath = reply.arguments().first().value< QDBusObjectPath >().path();
        if ( !validPath( returnedPath ) )
        {
            fail( QStringLiteral( "Portal method returned an invalid request path" ) );
            dispose();
            return;
        }
        if ( retired )
            closeRequests();
        const auto buffered = candidates;
        candidates.clear();
        for ( Envelope const & envelope : buffered )
        {
            if ( matches( envelope.path ) )
            {
                deliver( envelope );
                return;
            }
        }
        if ( retired && !createSession )
            dispose();
    }

    void deliver( Envelope const & envelope )
    {
        QString session;
        if ( createSession && envelope.code == 0 )
        {
            const QVariant value = envelope.results.value( QStringLiteral( "session_handle" ) );
            if ( value.metaType() != QMetaType::fromType< QString >() || !validPath( value.toString() ) )
            {
                fail( QStringLiteral( "Portal response has an invalid session_handle" ) );
                return;
            }
            session = value.toString();
        }
        // This gate suppresses every completion from a retired generation while
        // still reclaiming a session created after its public request disappeared.
        if ( retired )
        {
            if ( !session.isEmpty() )
                close( session, SessionInterface );
            dispose();
            return;
        }
        auto receiver = target;
        retired = true;
        dispose();
        if ( receiver )
            emit receiver->response( generation, returnedPath, envelope.code, envelope.results );
    }

private slots:
    void receiveResponse( QDBusMessage const & message )
    {
        if ( disposed || message.service() != owner )
            return;
        expired();
        if ( message.type() != QDBusMessage::SignalMessage || message.interface() != RequestInterface
             || message.member() != QLatin1String( "Response" )
             || message.signature() != QLatin1String( "ua{sv}" ) || message.arguments().size() != 2 )
            return;
        const auto arguments = message.arguments();
        if ( arguments.first().metaType() != QMetaType::fromType< uint >()
             || arguments.last().metaType() != QMetaType::fromType< QDBusArgument >()
             || arguments.last().value< QDBusArgument >().currentSignature() != QLatin1String( "a{sv}" ) )
            return;
        Envelope envelope{ message.path(), arguments.first().toUInt(),
                           qdbus_cast< QVariantMap >( arguments.last() ) };
        if ( returnedPath.isEmpty() )
        {
            for ( Envelope const & candidate : candidates )
                if ( candidate.path == envelope.path )
                    return;
            if ( candidates.size() == MaxCandidates )
                candidates.removeFirst();
            candidates.append( envelope );
            return;
        }
        if ( matches( envelope.path ) )
            deliver( envelope );
    }

private:
    QDBusConnection bus;
    const quint64 generation;
    QPointer< PortalRequest > target;
    QDBusServiceWatcher serviceWatcher;
    QTimer timer;
    QElapsedTimer elapsed;
    QString owner, predictedPath, returnedPath;
    QVector< Envelope > candidates;
    QSet< QString > closed;
    bool started = false;
    bool invoked = false;
    bool subscribed = false;
    bool createSession = false;
    bool retired = false;
    bool disposed = false;
};

PortalRequest::PortalRequest( QDBusConnection const & bus, quint64 generation, QObject * parent ):
    QObject( parent ), d( new PortalRequestState( bus, generation, this ) )
{
}

PortalRequest::~PortalRequest()
{
    if ( d )
        d->detach();
}

void PortalRequest::start( QDBusMessage message, QString const & handleToken )
{
    if ( d )
        d->start( message, handleToken );
}

void PortalRequest::cancel() noexcept
{
    if ( d )
        d->cancel();
}

#include "portalhotkeys.moc"
