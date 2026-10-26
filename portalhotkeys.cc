/* This file is part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#include "portalhotkeys.hh"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QHash>
#include <QMetaType>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QVector>

namespace PortalWire {
QDBusArgument &operator<<( QDBusArgument & out, Shortcut const & value )
{
    out.beginStructure();
    out << value.id << value.properties;
    out.endStructure();
    return out;
}

QDBusArgument const &operator>>( QDBusArgument const & in, Shortcut & value )
{
    in.beginStructure();
    in >> value.id >> value.properties;
    in.endStructure();
    return in;
}
} // namespace PortalWire

namespace {
const QString Service = QStringLiteral( "org.freedesktop.portal.Desktop" );
const QString PortalPath = QStringLiteral( "/org/freedesktop/portal/desktop" );
const QString GlobalShortcutsInterface = QStringLiteral( "org.freedesktop.portal.GlobalShortcuts" );
const QString PropertiesInterface = QStringLiteral( "org.freedesktop.DBus.Properties" );
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

bool knownBindingId( QString const & id )
{
    return id == QLatin1String( "main-window" ) || id == QLatin1String( "clipboard-lookup" );
}

bool validStringProperty( QVariantMap const & properties, QString const & name )
{
    return !properties.contains( name )
        || properties.value( name ).metaType() == QMetaType::fromType< QString >();
}

bool parseShortcuts( QVariant const & value, PortalWire::Shortcuts * result, QString * error )
{
    if ( value.metaType() != QMetaType::fromType< QDBusArgument >() )
    {
        if ( error )
            *error = QStringLiteral( "shortcuts is not a(sa{sv})" );
        return false;
    }

    const QDBusArgument argument = value.value< QDBusArgument >();
    if ( argument.currentSignature() != QLatin1String( "a(sa{sv})" ) )
    {
        if ( error )
            *error = QStringLiteral( "shortcuts has an invalid wire signature" );
        return false;
    }

    const PortalWire::Shortcuts parsed = qdbus_cast< PortalWire::Shortcuts >( value );
    QSet< QString > ids;
    for ( PortalWire::Shortcut const & entry : parsed )
    {
        if ( entry.id.isEmpty() || ids.contains( entry.id )
             || !validStringProperty( entry.properties, QStringLiteral( "description" ) )
             || !validStringProperty( entry.properties, QStringLiteral( "trigger_description" ) )
             || !validStringProperty( entry.properties, QStringLiteral( "preferred_trigger" ) ) )
        {
            if ( error )
                *error = QStringLiteral( "shortcuts contain an invalid or duplicate entry" );
            return false;
        }
        ids.insert( entry.id );
    }

    if ( result )
        *result = parsed;
    return true;
}

QVector< PortalBindingDisplay > displayEntries( PortalWire::Shortcuts const & entries,
                                                QSet< QString > const & policy )
{
    QVector< PortalBindingDisplay > displayed;
    for ( PortalWire::Shortcut const & entry : entries )
    {
        if ( !policy.contains( entry.id ) )
            continue;

        QString trigger;
        const QVariant value = entry.properties.value( QStringLiteral( "trigger_description" ) );
        if ( value.metaType() == QMetaType::fromType< QString >() )
            trigger = value.toString();
        if ( trigger.isEmpty() )
            trigger = QStringLiteral( "Assigned by portal" );

        displayed.append( PortalBindingDisplay{ entry.id, trigger } );
    }
    return displayed;
}

bool extractOptions( QVariant const & value, QVariantMap * result )
{
    if ( value.metaType() == QMetaType::fromType< QVariantMap >() )
    {
        if ( result )
            *result = value.toMap();
        return true;
    }
    if ( value.metaType() != QMetaType::fromType< QDBusArgument >()
         || value.value< QDBusArgument >().currentSignature() != QLatin1String( "a{sv}" ) )
        return false;
    if ( result )
        *result = qdbus_cast< QVariantMap >( value );
    return true;
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
        // Qt may omit sender metadata from a pending-call reply. The call was
        // addressed to the captured unique owner and Qt correlates its serial;
        // enforce sender identity additionally when that metadata is present.
        if ( !reply.service().isEmpty() && reply.service() != owner )
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


struct PortalBackend::Private
{
    explicit Private( QDBusConnection const & connection ):
        bus( connection )
    {
    }

    QDBusConnection bus;
    HotkeyBackend::State state = HotkeyBackend::State::Unsupported;
    QString explanation;
    QString owner;
    QString sessionPath;
    QVector< HotkeyBinding > desired;
    /// What may be displayed: decided by the config/UI layer, independent of
    /// which IDs the current bind attempt carries. Defaults to the IDs of the
    /// first request when the caller never supplies a policy.
    QSet< QString > displayPolicy;
    bool displayPolicyExplicit = false;
    /// The IDs this attempt actually asked the portal to bind.
    QSet< QString > requestedIds;
    /// The most recent records the portal reported, kept so a display-policy change
    /// can refresh what is shown without touching the session.
    QVector< PortalWire::Shortcut > records;
    QSet< QString > activeIds;
    QVector< QPointer< PortalRequest > > requests;
    QPointer< PortalRequest > createRequest;
    QPointer< PortalRequest > listRequest;
    QPointer< PortalRequest > bindRequest;
    quint64 generation = 0;
    quint64 displayRevision = 0;
    uint version = 0;
    bool versionKnown = false;
    bool supported = false;
    bool bindAttempted = false;
    bool bindCompleted = false;
    bool destroying = false;
};

PortalBackend::PortalBackend( QDBusConnection const & bus, QObject * parent ):
    HotkeyBackend( parent ), d( std::make_unique< Private >( bus ) )
{
    qDBusRegisterMetaType< PortalWire::Shortcut >();
    qDBusRegisterMetaType< PortalWire::Shortcuts >();

    serviceWatcher = new QDBusServiceWatcher( Service, d->bus,
                                               QDBusServiceWatcher::WatchForOwnerChange, this );
    connect( serviceWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
             [ this ]( QString const &, QString const & oldOwner, QString const & newOwner ) {
                 ownerChanged( oldOwner, newOwner );
             } );

    d->bus.connect( Service, PortalPath, GlobalShortcutsInterface, QStringLiteral( "Activated" ),
                    this, SLOT( receivePortalSignal( QDBusMessage ) ) );
    d->bus.connect( Service, PortalPath, GlobalShortcutsInterface,
                    QStringLiteral( "Deactivated" ), this,
                    SLOT( receivePortalSignal( QDBusMessage ) ) );
    d->bus.connect( Service, QString(), GlobalShortcutsInterface,
                    QStringLiteral( "ShortcutsChanged" ), this,
                    SLOT( receivePortalSignal( QDBusMessage ) ) );
    d->bus.connect( Service, QString(), SessionInterface, QStringLiteral( "Closed" ), this,
                    SLOT( receivePortalSignal( QDBusMessage ) ) );

    // Probe capability even before the first edit. A bind before this queued
    // probe runs supersedes it, rather than issuing duplicate version lookups.
    d->state = HotkeyBackend::State::Creating;
    d->explanation = QStringLiteral( "Checking portal capability" );
    const quint64 generation = d->generation;
    QTimer::singleShot( 0, this, [ this, generation ]() {
        if ( generation != d->generation )
            return;
        if ( !d->bus.isConnected() )
            failGeneration( QStringLiteral( "Portal bus is unavailable" ), false );
        else
            readVersion( generation );
    } );
}

PortalBackend::~PortalBackend()
{
    if ( !d )
        return;

    d->destroying = true;
    ++d->generation;
    cancelRequests();
    closeSession();

    d->bus.disconnect( Service, PortalPath, GlobalShortcutsInterface,
                       QStringLiteral( "Activated" ), this,
                       SLOT( receivePortalSignal( QDBusMessage ) ) );
    d->bus.disconnect( Service, PortalPath, GlobalShortcutsInterface,
                       QStringLiteral( "Deactivated" ), this,
                       SLOT( receivePortalSignal( QDBusMessage ) ) );
    d->bus.disconnect( Service, QString(), GlobalShortcutsInterface,
                       QStringLiteral( "ShortcutsChanged" ), this,
                       SLOT( receivePortalSignal( QDBusMessage ) ) );
    d->bus.disconnect( Service, QString(), SessionInterface, QStringLiteral( "Closed" ), this,
                       SLOT( receivePortalSignal( QDBusMessage ) ) );

    // QtDBus can still be delivering this watcher's owner-change metacall when
    // a stateChanged receiver destroys us. Retire it now, delete after dispatch.
    serviceWatcher->disconnect( this );
    serviceWatcher->setWatchedServices( {} );
    serviceWatcher->setParent( QCoreApplication::instance() );
    serviceWatcher->deleteLater();
    serviceWatcher = nullptr;
    d.reset();
}

bool PortalBackend::isSupported() const noexcept
{
    return d && d->supported && d->bus.isConnected();
}

HotkeyBackend::State PortalBackend::state() const noexcept
{
    return d ? d->state : HotkeyBackend::State::Closed;
}

void PortalBackend::bind( QVector< HotkeyBinding > const & bindings )
{
    beginGeneration( bindings );
}

void PortalBackend::unbind()
{
    if ( !d )
        return;

    const quint64 generation = ++d->generation;
    cancelRequests();
    closeSession();
    d->desired.clear();
    d->requestedIds.clear();
    d->activeIds.clear();
    d->bindAttempted = false;
    d->bindCompleted = false;
    ++d->displayRevision;
    QPointer< PortalBackend > alive( this );
    setState( HotkeyBackend::State::Closed, QStringLiteral( "Portal session closed" ) );
    if ( alive && generation == d->generation )
        emit bindingsChanged( {} );
}

void PortalBackend::configure( QString const & activationToken )
{
    if ( !d || d->destroying )
        return;

    if ( d->version < 2 )
    {
        setState( d->state, QStringLiteral( "ConfigureShortcuts requires portal version 2" ) );
        return;
    }
    if ( d->sessionPath.isEmpty() || d->owner.isEmpty() || !isSupported() )
    {
        setState( d->state, QStringLiteral( "ConfigureShortcuts requires an available session" ) );
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        d->owner, PortalPath, GlobalShortcutsInterface, QStringLiteral( "ConfigureShortcuts" ) );
    QVariantMap options;
    if ( !activationToken.isEmpty() )
        options.insert( QStringLiteral( "activation_token" ), activationToken );
    message << QVariant::fromValue( QDBusObjectPath( d->sessionPath ) ) << QString() << options;

    const quint64 generation = d->generation;
    const QString session = d->sessionPath;
    auto watcher = new QDBusPendingCallWatcher( d->bus.asyncCall( message, DeadlineMs ), this );
    connect( watcher, &QDBusPendingCallWatcher::finished, this,
             [ this, generation, session ]( QDBusPendingCallWatcher * finished ) {
                 const QDBusMessage reply = finished->reply();
                 finished->deleteLater();
                 if ( !d || d->destroying || generation != d->generation || session != d->sessionPath )
                     return;
                 if ( reply.type() == QDBusMessage::ErrorMessage )
                     setState( d->state, QStringLiteral( "ConfigureShortcuts failed: " )
                                         + reply.errorMessage() );
                 else if ( reply.type() != QDBusMessage::ReplyMessage )
                     setState( d->state, QStringLiteral( "ConfigureShortcuts returned no reply" ) );
             } );
}

void PortalBackend::setState( HotkeyBackend::State state, QString const & explanation )
{
    if ( !d || ( d->state == state && d->explanation == explanation ) )
        return;
    d->state = state;
    d->explanation = explanation;
    const QString notification = explanation;
    emit stateChanged( state, notification );
}

void PortalBackend::setDisplayPolicy( QSet< QString > const & ids )
{
    if( !d || d->destroying )
        return;
    d->displayPolicy = ids;
    d->displayPolicyExplicit = true;

    // The policy changes what is shown, not what is bound: refresh the display
    // from the records the portal already reported, without touching the session.
    ++d->displayRevision;
    emit bindingsChanged( displayEntries( d->records, d->displayPolicy ) );
}

void PortalBackend::beginGeneration( QVector< HotkeyBinding > const & bindings )
{
    if ( !d || d->destroying )
        return;

    const quint64 generation = ++d->generation;
    cancelRequests();
    closeSession();
    d->desired.clear();
    // The display policy belongs to the config/UI layer and survives a bind edit;
    // only the requested set is rebuilt below.
    d->activeIds.clear();
    d->bindAttempted = false;
    d->bindCompleted = false;
    ++d->displayRevision;

    QSet< QString > ids;
    bool valid = true;
    for ( HotkeyBinding const & binding : bindings )
    {
        if ( binding.id.isEmpty() || !knownBindingId( binding.id ) || ids.contains( binding.id ) )
        {
            valid = false;
            break;
        }
        ids.insert( binding.id );
        // preferredTrigger leaves unsupported shortcuts without an XDG trigger.
        // Keep their display policy, but never manufacture an empty binding.
        if ( !binding.sequence.isEmpty() )
            d->desired.append( binding );
    }
    d->requestedIds.clear();
    for ( HotkeyBinding const & binding : d->desired )
        d->requestedIds.insert( binding.id );
    if( !d->displayPolicyExplicit )
        d->displayPolicy = ids;

    if ( !valid || d->desired.isEmpty() )
    {
        setState( HotkeyBackend::State::Unavailable,
                  QStringLiteral( "No valid portal bindings were requested" ) );
        return;
    }
    if ( !d->bus.isConnected() )
    {
        d->supported = false;
        setState( HotkeyBackend::State::Unavailable, QStringLiteral( "Portal bus is unavailable" ) );
        return;
    }

    d->versionKnown = false;
    d->version = 0;

    // A stateChanged handler may start the next binding edit synchronously, which
    // bumps d->generation before this invocation resumes. The generation of *this*
    // attempt is therefore captured before the notification is emitted; the
    // resumed work then fails the guard instead of issuing a second lookup tagged
    // with the newer generation.
    QPointer< PortalBackend > alive( this );
    setState( HotkeyBackend::State::Creating, QStringLiteral( "Creating portal session" ) );
    if ( !alive || d->destroying || generation != d->generation )
        return;
    readVersion( generation );
}

void PortalBackend::readVersion( quint64 generation )
{
    if ( !d || generation != d->generation || d->destroying )
        return;
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral( "org.freedesktop.DBus" ), QStringLiteral( "/org/freedesktop/DBus" ),
        QStringLiteral( "org.freedesktop.DBus" ), QStringLiteral( "GetNameOwner" ) );
    message << Service;
    auto watcher = new QDBusPendingCallWatcher( d->bus.asyncCall( message, DeadlineMs ), this );
    connect( watcher, &QDBusPendingCallWatcher::finished, this,
             [ this, generation ]( QDBusPendingCallWatcher * finished ) {
                 const QDBusMessage reply = finished->reply();
                 finished->deleteLater();
                 if ( generation != d->generation || d->destroying )
                     return;
                 if ( reply.type() != QDBusMessage::ReplyMessage || reply.signature() != QLatin1String( "s" )
                      || reply.arguments().size() != 1
                      || !reply.arguments().first().toString().startsWith( QLatin1Char( ':' ) ) )
                 {
                     failGeneration( QStringLiteral( "Portal service is unavailable" ), false );
                     return;
                 }
                 readVersionForOwner( generation, reply.arguments().first().toString() );
             } );
}

void PortalBackend::readVersionForOwner( quint64 generation, QString const & owner )
{
    if ( generation != d->generation || d->destroying )
        return;
    d->owner = owner;
    QDBusMessage message = QDBusMessage::createMethodCall(
        owner, PortalPath, PropertiesInterface, QStringLiteral( "Get" ) );
    message.setAutoStartService( false );
    message << GlobalShortcutsInterface << QStringLiteral( "version" );
    auto watcher = new QDBusPendingCallWatcher( d->bus.asyncCall( message, DeadlineMs ), this );
    connect( watcher, &QDBusPendingCallWatcher::finished, this,
             [ this, generation, owner ]( QDBusPendingCallWatcher * finished ) {
                 const QDBusMessage reply = finished->reply();
                 finished->deleteLater();
                 if ( !d || d->destroying || generation != d->generation || owner != d->owner )
                     return;
                 if ( reply.type() == QDBusMessage::ErrorMessage )
                 {
                     failGeneration( QStringLiteral( "Portal version is unavailable: " )
                                         + reply.errorMessage(), false );
                     return;
                 }
                 if ( reply.type() != QDBusMessage::ReplyMessage || reply.signature() != QLatin1String( "v" )
                      || reply.arguments().size() != 1
                      || reply.arguments().first().metaType() != QMetaType::fromType< QDBusVariant >() )
                 {
                     failGeneration( QStringLiteral( "Portal version has an invalid wire type" ), false );
                     return;
                 }

                 const QVariant version = reply.arguments().first().value< QDBusVariant >().variant();
                 if ( version.metaType() != QMetaType::fromType< uint >() )
                 {
                     failGeneration( QStringLiteral( "Portal version has an invalid value type" ), false );
                     return;
                 }

                 d->version = version.toUInt();
                 d->versionKnown = true;
                 if ( !reply.service().isEmpty() && reply.service().startsWith( QLatin1Char( ':' ) ) )
                     d->owner = reply.service();
                 if ( d->version < 1 )
                 {
                     failGeneration( QStringLiteral( "Portal GlobalShortcuts version is too old" ), false );
                     return;
                 }
                 d->supported = true;
                 if ( d->desired.isEmpty() )
                     setState( HotkeyBackend::State::Ready, QStringLiteral( "Portal is available" ) );
                 else
                     createSession( generation );
             } );
}

void PortalBackend::createSession( quint64 generation )
{
    if ( !d || generation != d->generation || d->destroying )
        return;

    const QString token = uniqueToken();
    QDBusMessage message = QDBusMessage::createMethodCall(
        Service, PortalPath, GlobalShortcutsInterface, QStringLiteral( "CreateSession" ) );
    message << QVariantMap{ { QStringLiteral( "handle_token" ), token },
                             { QStringLiteral( "session_handle_token" ), uniqueToken() } };

    auto request = new PortalRequest( d->bus, generation, this );
    d->requests.append( request );
    d->createRequest = request;
    connect( request, &PortalRequest::response, this,
             [ this, request ]( quint64 requestGeneration, QString const &, uint code,
                                QVariantMap const & results ) {
                 forgetRequest( request );
                 if ( d && d->createRequest == request )
                     d->createRequest = nullptr;
                 request->deleteLater();
                 if ( d && !d->destroying && requestGeneration == d->generation )
                     handleCreateResponse( requestGeneration, code, results );
             } );
    connect( request, &PortalRequest::failed, this,
             [ this, request ]( quint64 requestGeneration, QString const & explanation ) {
                 forgetRequest( request );
                 if ( d && d->createRequest == request )
                     d->createRequest = nullptr;
                 request->deleteLater();
                 if ( d && !d->destroying && requestGeneration == d->generation )
                     failGeneration( explanation );
             } );
    request->start( message, token );
}

void PortalBackend::startList( quint64 generation )
{
    if ( !d || generation != d->generation || d->destroying || d->sessionPath.isEmpty() )
        return;

    const quint64 revision = d->displayRevision;
    const QString token = uniqueToken();
    QDBusMessage message = QDBusMessage::createMethodCall(
        Service, PortalPath, GlobalShortcutsInterface, QStringLiteral( "ListShortcuts" ) );
    message << QVariant::fromValue( QDBusObjectPath( d->sessionPath ) )
            << QVariantMap{ { QStringLiteral( "handle_token" ), token } };

    auto request = new PortalRequest( d->bus, generation, this );
    d->requests.append( request );
    d->listRequest = request;
    connect( request, &PortalRequest::response, this,
             [ this, request, revision ]( quint64 requestGeneration, QString const &, uint code,
                                          QVariantMap const & results ) {
                 forgetRequest( request );
                 if ( d && d->listRequest == request )
                     d->listRequest = nullptr;
                 request->deleteLater();
                 if ( d && !d->destroying && requestGeneration == d->generation )
                     handleListResponse( requestGeneration, revision, code, results );
             } );
    connect( request, &PortalRequest::failed, this,
             [ this, request ]( quint64 requestGeneration, QString const & explanation ) {
                 forgetRequest( request );
                 if ( d && d->listRequest == request )
                     d->listRequest = nullptr;
                 request->deleteLater();
                 if ( d && !d->destroying && requestGeneration == d->generation )
                     failGeneration( QStringLiteral( "ListShortcuts failed: " ) + explanation );
             } );
    request->start( message, token );
}

void PortalBackend::startBindForCurrentGenerationForTest()
{
    if( !d || d->destroying )
        return;
    startBind( d->generation );
}

void PortalBackend::startBind( quint64 generation )
{
    if ( !d || generation != d->generation || d->destroying || d->sessionPath.isEmpty()
         || d->bindAttempted )
        return;

    // Set before notifying callers: even reentrant work cannot bind this session twice.
    d->bindAttempted = true;
    QPointer< PortalBackend > alive( this );
    setState( HotkeyBackend::State::Binding, QStringLiteral( "Binding portal shortcuts" ) );
    if ( !alive || generation != d->generation || d->sessionPath.isEmpty() )
        return;

    PortalWire::Shortcuts shortcuts;
    for ( HotkeyBinding const & binding : d->desired )
    {
        PortalWire::Shortcut shortcut;
        shortcut.id = binding.id;
        shortcut.properties.insert( QStringLiteral( "description" ), binding.description );
        shortcut.properties.insert( QStringLiteral( "preferred_trigger" ), binding.sequence );
        shortcuts.append( shortcut );
    }

    const QString token = uniqueToken();
    QDBusMessage message = QDBusMessage::createMethodCall(
        Service, PortalPath, GlobalShortcutsInterface, QStringLiteral( "BindShortcuts" ) );
    message << QVariant::fromValue( QDBusObjectPath( d->sessionPath ) )
            << QVariant::fromValue( shortcuts ) << QString()
            << QVariantMap{ { QStringLiteral( "handle_token" ), token } };

    auto request = new PortalRequest( d->bus, generation, this );
    d->requests.append( request );
    d->bindRequest = request;
    connect( request, &PortalRequest::response, this,
             [ this, request ]( quint64 requestGeneration, QString const &, uint code,
                                QVariantMap const & results ) {
                 forgetRequest( request );
                 if ( d && d->bindRequest == request )
                     d->bindRequest = nullptr;
                 request->deleteLater();
                 if ( d && !d->destroying && requestGeneration == d->generation )
                     handleBindResponse( requestGeneration, code, results );
             } );
    connect( request, &PortalRequest::failed, this,
             [ this, request ]( quint64 requestGeneration, QString const & explanation ) {
                 forgetRequest( request );
                 if ( d && d->bindRequest == request )
                     d->bindRequest = nullptr;
                 request->deleteLater();
                 if ( d && !d->destroying && requestGeneration == d->generation )
                     failGeneration( explanation );
             } );
    request->start( message, token );
}

void PortalBackend::handleCreateResponse( quint64 generation, uint code,
                                          QVariantMap const & results )
{
    if ( !d || generation != d->generation || d->destroying )
        return;
    if ( code == 1 )
    {
        setState( HotkeyBackend::State::Denied, QStringLiteral( "Portal session creation was denied" ) );
        return;
    }
    if ( code != 0 )
    {
        failGeneration( QStringLiteral( "Portal session creation failed" ) );
        return;
    }

    const QVariant value = results.value( QStringLiteral( "session_handle" ) );
    if ( value.metaType() != QMetaType::fromType< QString >() || !validPath( value.toString() ) )
    {
        failGeneration( QStringLiteral( "Portal response has an invalid session_handle" ) );
        return;
    }

    d->sessionPath = value.toString();
    d->activeIds.clear();
    QPointer< PortalBackend > alive( this );
    setState( HotkeyBackend::State::Ready, QStringLiteral( "Portal session is ready" ) );
    if ( !alive || generation != d->generation )
        return;
    startList( generation );
    startBind( generation );
}

void PortalBackend::handleListResponse( quint64 generation, quint64 revision, uint code,
                                        QVariantMap const & results )
{
    if ( !d || d->destroying || generation != d->generation || d->sessionPath.isEmpty() )
        return;
    if ( code != 0 )
    {
        failGeneration( code == 1 ? QStringLiteral( "Portal shortcut listing was denied" )
                                   : QStringLiteral( "Portal shortcut listing failed" ) );
        return;
    }

    PortalWire::Shortcuts entries;
    QString error;
    if ( !parseShortcuts( results.value( QStringLiteral( "shortcuts" ) ), &entries, &error ) )
    {
        failGeneration( QStringLiteral( "ListShortcuts returned invalid data: " ) + error );
        return;
    }
    if ( revision != d->displayRevision )
        return;

    // The listing is the authoritative history: keep it so a later display-policy
    // change (or a bind reply that reports only the bound subset) can still show
    // entries the current session does not bind.
    d->records = entries;

    emit bindingsChanged( displayEntries( entries, d->displayPolicy ) );
}

void PortalBackend::handleBindResponse( quint64 generation, uint code, QVariantMap const & results )
{
    if ( !d || d->destroying || generation != d->generation || d->sessionPath.isEmpty() )
        return;

    ++d->displayRevision;
    d->bindCompleted = true;
    QPointer< PortalBackend > alive( this );
    if ( code == 1 || code == 2 )
    {
        d->activeIds.clear();
        setState( HotkeyBackend::State::Denied, QStringLiteral( "Portal shortcut binding was denied" ) );
        if ( alive && generation == d->generation )
            emit bindingsChanged( {} );
        return;
    }
    if ( code != 0 )
    {
        failGeneration( QStringLiteral( "Portal shortcut binding failed" ) );
        return;
    }

    PortalWire::Shortcuts entries;
    QString error;
    if ( !parseShortcuts( results.value( QStringLiteral( "shortcuts" ) ), &entries, &error ) )
    {
        failGeneration( QStringLiteral( "BindShortcuts returned invalid data: " ) + error );
        return;
    }

    QSet< QString > returned;
    {
        // Replace the records for the IDs this reply carries and keep the rest of
        // the history: a bind attempt reports only what it bound.
        QHash< QString, PortalWire::Shortcut > merged;
        for( PortalWire::Shortcut const & entry : d->records )
            merged.insert( entry.id, entry );
        for( PortalWire::Shortcut const & entry : entries )
            merged.insert( entry.id, entry );
        d->records.clear();
        for( auto const & entry : merged )
            d->records.append( entry );
    }
    for ( PortalWire::Shortcut const & entry : entries )
    {
        if ( !d->requestedIds.contains( entry.id ) )
        {
            failGeneration( QStringLiteral( "BindShortcuts returned an unknown shortcut id" ) );
            return;
        }
        returned.insert( entry.id );
    }

    d->activeIds = returned;
    if ( returned.isEmpty() )
        setState( HotkeyBackend::State::Denied, QStringLiteral( "Portal bound no shortcuts" ) );
    else if ( returned.size() == d->requestedIds.size() )
        setState( HotkeyBackend::State::Bound, QStringLiteral( "Portal shortcuts are bound" ) );
    else
        setState( HotkeyBackend::State::Partial, QStringLiteral( "Portal bound only some shortcuts" ) );
    if ( alive && generation == d->generation )
        emit bindingsChanged( displayEntries( entries, d->displayPolicy ) );
}

void PortalBackend::receivePortalSignal( QDBusMessage const & message )
{
    if ( !d || d->destroying || message.service().isEmpty() )
        return;
    if ( !d->owner.isEmpty() && message.service().startsWith( QLatin1Char( ':' ) )
         && message.service() != d->owner )
        return;
    if ( d->owner.isEmpty() && message.service().startsWith( QLatin1Char( ':' ) ) )
        d->owner = message.service();

    if ( message.interface() == GlobalShortcutsInterface
         && message.member() == QLatin1String( "Activated" ) )
    {
        if ( !d->bindCompleted || message.path() != PortalPath || message.signature() != QLatin1String( "osta{sv}" )
             || message.arguments().size() != 4 || d->sessionPath.isEmpty() )
            return;
        const QList< QVariant > arguments = message.arguments();
        if ( arguments.at( 0 ).metaType() != QMetaType::fromType< QDBusObjectPath >()
             || arguments.at( 1 ).metaType() != QMetaType::fromType< QString >()
             || arguments.at( 2 ).metaType() != QMetaType::fromType< qulonglong >() )
            return;
        if ( arguments.at( 0 ).value< QDBusObjectPath >().path() != d->sessionPath )
            return;
        const QString id = arguments.at( 1 ).toString();
        if ( !d->activeIds.contains( id ) )
            return;
        QVariantMap options;
        if ( !extractOptions( arguments.at( 3 ), &options ) )
            return;
        QString token;
        const QVariant tokenValue = options.value( QStringLiteral( "activation_token" ) );
        if ( tokenValue.metaType() == QMetaType::fromType< QString >() )
            token = tokenValue.toString();
        emit activated( id, token );
        return;
    }

    if ( message.interface() == GlobalShortcutsInterface
         && message.member() == QLatin1String( "ShortcutsChanged" ) )
    {
        if ( !d->bindCompleted || ( message.path() != PortalPath && message.path() != d->sessionPath )
             || message.signature() != QLatin1String( "oa(sa{sv})" )
             || message.arguments().size() != 2 || d->sessionPath.isEmpty()
             || message.arguments().at( 0 ).metaType() != QMetaType::fromType< QDBusObjectPath >()
             || message.arguments().at( 0 ).value< QDBusObjectPath >().path() != d->sessionPath )
            return;

        PortalWire::Shortcuts entries;
        QString error;
        if ( !parseShortcuts( message.arguments().at( 1 ), &entries, &error ) )
        {
            failGeneration( QStringLiteral( "ShortcutsChanged returned invalid data: " ) + error );
            return;
        }
        for ( PortalWire::Shortcut const & entry : entries )
        {
            if ( !d->requestedIds.contains( entry.id ) )
            {
                failGeneration( QStringLiteral( "ShortcutsChanged returned an unknown shortcut id" ) );
                return;
            }
        }

        {
            QHash< QString, PortalWire::Shortcut > merged;
            for( PortalWire::Shortcut const & entry : d->records )
                merged.insert( entry.id, entry );
            for( PortalWire::Shortcut const & entry : entries )
                merged.insert( entry.id, entry );
            d->records.clear();
            for( auto const & entry : merged )
                d->records.append( entry );
        }
        ++d->displayRevision;
        d->activeIds.clear();
        for ( PortalWire::Shortcut const & entry : entries )
            d->activeIds.insert( entry.id );
        const quint64 generation = d->generation;
        QPointer< PortalBackend > alive( this );
        if ( d->activeIds.isEmpty() )
            setState( HotkeyBackend::State::Denied, QStringLiteral( "Portal bound no shortcuts" ) );
        else if ( d->activeIds.size() == d->requestedIds.size() )
            setState( HotkeyBackend::State::Bound, QStringLiteral( "Portal shortcuts are bound" ) );
        else
            setState( HotkeyBackend::State::Partial, QStringLiteral( "Portal bound only some shortcuts" ) );
        if ( alive && generation == d->generation )
            emit bindingsChanged( displayEntries( entries, d->displayPolicy ) );
        return;
    }

    if ( message.interface() == SessionInterface && message.member() == QLatin1String( "Closed" ) )
    {
        // A Closed signal for the live session is authoritative whenever it arrives:
        // the session exists from CreateSession's reply onward, so a bind still in
        // flight must not resurrect it afterwards.
        if( d->sessionPath.isEmpty() || message.path() != d->sessionPath
            || message.signature() != QLatin1String( "a{sv}" ) || message.arguments().size() != 1 )
            return;
        QVariantMap details;
        if ( !extractOptions( message.arguments().first(), &details ) )
            return;
        Q_UNUSED( details )
        d->sessionPath.clear();
        failGeneration( QStringLiteral( "Portal session was closed" ) );
    }
}

void PortalBackend::failGeneration( QString const & explanation, bool supported )
{
    if ( !d || d->destroying )
        return;

    const quint64 generation = ++d->generation;
    // A request's own service watcher may notify before the backend watcher.
    // Publish loss atomically with Unavailable, and never Close a vanished owner.
    if ( explanation.endsWith( QStringLiteral( "Portal service owner changed" ) ) )
    {
        supported = false;
        d->sessionPath.clear();
        d->owner.clear();
    }
    d->supported = supported;
    d->activeIds.clear();
    d->bindCompleted = false;
    ++d->displayRevision;
    cancelRequests();
    closeSession();
    QPointer< PortalBackend > alive( this );
    setState( HotkeyBackend::State::Unavailable, explanation );
    if ( alive && generation == d->generation )
        emit bindingsChanged( {} );
}

void PortalBackend::cancelRequests() noexcept
{
    if ( !d )
        return;

    const auto pending = d->requests;
    d->requests.clear();
    d->createRequest = nullptr;
    d->listRequest = nullptr;
    d->bindRequest = nullptr;
    for ( QPointer< PortalRequest > request : pending )
    {
        if ( request )
        {
            request->cancel();
            request->deleteLater();
        }
    }
}

void PortalBackend::forgetRequest( PortalRequest * request ) noexcept
{
    if ( !d )
        return;
    for ( auto it = d->requests.begin(); it != d->requests.end(); )
    {
        if ( *it == request )
            it = d->requests.erase( it );
        else
            ++it;
    }
}

void PortalBackend::closeSession() noexcept
{
    if ( !d || d->sessionPath.isEmpty() )
        return;

    const QString path = d->sessionPath;
    const QString service = d->owner.isEmpty() ? Service : d->owner;
    d->sessionPath.clear();
    if ( d->bus.isConnected() )
    {
        QDBusMessage message = QDBusMessage::createMethodCall(
            service, path, SessionInterface, QStringLiteral( "Close" ) );
        d->bus.send( message );
    }
}

void PortalBackend::ownerChanged( QString const &, QString const & newOwner )
{
    if ( !d || d->destroying )
        return;

    const bool lost = newOwner.isEmpty()
        || ( !d->owner.isEmpty() && d->owner != newOwner );
    // A vanished owner needs no Close and must never route one to its successor.
    if ( lost )
        d->sessionPath.clear();
    d->owner = newOwner;
    if ( lost )
    {
        d->supported = false;
        if ( d->state != HotkeyBackend::State::Unsupported
             && d->state != HotkeyBackend::State::Closed )
            failGeneration( QStringLiteral( "Portal service owner changed" ), false );
    }
}

#include "portalhotkeys.moc"

