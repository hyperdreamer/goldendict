/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Test-only loopback HTTP origin/proxy, see tests/support/http_fixture.hh.
 */

#include "http_fixture.hh"

#include "fixtures.hh"

#include <QFile>
#include <QHash>
#include <QSet>
#include <QHostAddress>
#include <QPair>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <optional>
#include <utility>

namespace TestSupport {

namespace {

/// Upper bound on the request head the fixture buffers before rejecting it.
constexpr int MaxRequestHeadBytes = 64 * 1024;

QByteArray requestHeader( QByteArray const & name,
                          QVector< QPair< QByteArray, QByteArray > > const & headers )
{
    for ( auto const & entry : headers )
    {
        if ( entry.first.compare( name, Qt::CaseInsensitive ) == 0 )
            return entry.second;
    }

    return QByteArray();
}

QByteArray contentTypeFor( QByteArray const & path )
{
    if ( path.endsWith( ".html" ) )
        return "text/html; charset=utf-8";
    if ( path.endsWith( ".css" ) )
        return "text/css; charset=utf-8";
    if ( path.endsWith( ".js" ) )
        return "text/javascript; charset=utf-8";
    if ( path.endsWith( ".json" ) )
        return "application/json; charset=utf-8";
    if ( path.endsWith( ".png" ) )
        return "image/png";
    if ( path.endsWith( ".svg" ) )
        return "image/svg+xml";
    if ( path.endsWith( ".ogg" ) )
        return "audio/ogg";

    return "application/octet-stream";
}

/// Decoded `user:password` of a Basic credential header, or a null QByteArray.
QByteArray basicCredential( QByteArray const & value )
{
    const QByteArray trimmed = value.trimmed();

    if ( trimmed.left( 6 ).toLower() != QByteArray( "basic " ) )
        return QByteArray();

    return QByteArray::fromBase64( trimmed.mid( 6 ) );
}

bool credentialMatches( QByteArray const & value, QString const & user, QString const & password )
{
    const QByteArray decoded = basicCredential( value );

    if ( decoded.isNull() )
        return false;

    return decoded == ( user + QLatin1Char( ':' ) + password ).toUtf8();
}

/// One request head, parsed strictly enough to reject garbage instead of
/// answering it.
struct ParsedRequest
{
    bool complete = false;
    bool valid = false;
    QByteArray method;
    QByteArray target;
    QVector< QPair< QByteArray, QByteArray > > headers;
};

ParsedRequest parseRequestHead( QByteArray const & buffer )
{
    ParsedRequest parsed;

    const int end = buffer.indexOf( "\r\n\r\n" );

    if ( end < 0 )
    {
        parsed.valid = buffer.size() <= MaxRequestHeadBytes;
        return parsed;
    }

    parsed.complete = true;

    const QList< QByteArray > lines = buffer.left( end ).split( '\n' );

    if ( lines.isEmpty() )
        return parsed;

    const QList< QByteArray > requestLine = lines.first().trimmed().split( ' ' );

    if ( requestLine.size() != 3 || !requestLine.at( 2 ).startsWith( "HTTP/1." ) )
        return parsed;

    parsed.method = requestLine.at( 0 );
    parsed.target = requestLine.at( 1 );

    for ( int line = 1; line < lines.size(); ++line )
    {
        const QByteArray current = lines.at( line ).trimmed();

        if ( current.isEmpty() )
            continue;

        const int colon = current.indexOf( ':' );

        if ( colon <= 0 )
            return parsed;

        parsed.headers.append( qMakePair( current.left( colon ).trimmed(),
                                          current.mid( colon + 1 ).trimmed() ) );
    }

    parsed.valid = parsed.method == "GET" || parsed.method == "HEAD"
        || parsed.method == "POST" || parsed.method == "CONNECT";

    return parsed;
}

QByteArray statusReason( int status )
{
    switch ( status )
    {
        case 200: return "200 OK";
        case 302: return "302 Found";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 404: return "404 Not Found";
        case 407: return "407 Proxy Authentication Required";
        default: return "500 Internal Server Error";
    }
}

// File-local aliases: this translation unit is the only consumer.
using Request = HttpFixture::Request;

/// What a route answers. `barrier` holds the answer until release() is called.
struct RouteResult
{
    int status = 200;
    QByteArray contentType = "text/plain; charset=utf-8";
    QByteArray body;
    QVector< QPair< QByteArray, QByteArray > > headers;
    bool barrier = false;
};

} // namespace

QStringList defaultLoopbackHosts()
{
    return { QStringLiteral( "127.0.0.1" ), QStringLiteral( "127.0.0.2" ),
             QStringLiteral( "127.0.0.3" ) };
}

struct HttpFixturePrivate
{
    struct Waiting
    {
        QTcpSocket * socket = nullptr;
        QByteArray path;
    };

    QTcpServer * server = nullptr;
    QHostAddress address;
    quint16 port = 0;
    QString error;

    QVector< Request > requests;
    QHash< QTcpSocket *, QByteArray > buffers;

    /// Sockets that became CONNECT tunnels: everything after the request head is
    /// relayed verbatim, so the HTTP parser must never look at them again.
    QSet< QTcpSocket * > tunnelled;
    QVector< Waiting > waiting;

    std::optional< QByteArray > fixtureFile( QByteArray const & path ) const;
    RouteResult route( ParsedRequest const & request, Request const & recorded ) const;

    void writeResponse( QTcpSocket * socket, RouteResult const & response, QByteArray const & method );
    void dispatch( QTcpSocket * socket, ParsedRequest const & request, Request & recorded );
    void handleConnect( QTcpSocket * socket, ParsedRequest const & request );
    void forwardAbsolute( QTcpSocket * socket, ParsedRequest const & request );
    void releaseWaiting( QByteArray const & path );
};

std::optional< QByteArray > HttpFixturePrivate::fixtureFile( QByteArray const & path ) const
{
    QString relative;

    if ( path.startsWith( "/dir/" ) )
        relative = QStringLiteral( "website/" ) + QString::fromUtf8( path.mid( 5 ) );
    else if ( path.startsWith( "/article/" ) )
        relative = QStringLiteral( "article/" ) + QString::fromUtf8( path.mid( 9 ) );
    else if ( path == "/print/two-pages.html" )
        relative = QStringLiteral( "print/two-pages.html" );
    else if ( path.endsWith( ".png" ) )
        relative = QStringLiteral( "article/pixel.png" );

    if ( relative.isEmpty() || relative.contains( QLatin1String( ".." ) ) )
        return std::nullopt;

    const QString absolute = fixturePath( relative );
    QFile file( absolute );

    if ( !file.open( QIODevice::ReadOnly ) )
        return std::nullopt;

    return file.readAll();
}

RouteResult HttpFixturePrivate::route( ParsedRequest const & request,
                                                              Request const & recorded ) const
{
    Q_UNUSED( request )

    RouteResult result;

    const QByteArray & path = recorded.path;

    if ( path == "/" )
    {
        result.body = "goldendict-http-fixture\n";
        return result;
    }

    if ( path == "/redirect" )
    {
        result.status = 302;
        result.headers.append( qMakePair( QByteArray( "Location" ), QByteArray( "/dir/first.html" ) ) );
        result.body = "redirect\n";
        return result;
    }

    if ( path == "/redirect-absolute" )
    {
        result.status = 302;
        result.headers.append(
            qMakePair( QByteArray( "Location" ),
                       QByteArray( "http://" ) + address.toString().toUtf8() + ':'
                           + QByteArray::number( port ) + "/dir/first.html" ) );
        result.body = "redirect\n";
        return result;
    }

    if ( path == "/slow-resource" )
    {
        result.body = "slow-resource-body\n";
        result.barrier = true;
        return result;
    }

    if ( path == "/deny-frame" )
    {
        result.contentType = "text/html; charset=utf-8";
        result.headers.append( qMakePair( QByteArray( "X-Frame-Options" ), QByteArray( "DENY" ) ) );
        result.body = "<!DOCTYPE html><html><body><h1 id=\"deny-marker\">deny-frame</h1>"
                      "<p>This document refuses to be framed.</p></body></html>";
        return result;
    }

    if ( path == "/deny-csp" )
    {
        result.contentType = "text/html; charset=utf-8";
        result.headers.append( qMakePair( QByteArray( "Content-Security-Policy" ),
                                          QByteArray( "frame-ancestors 'none'" ) ) );
        result.body = "<!DOCTYPE html><html><body><h1 id=\"deny-marker\">deny-csp</h1>"
                      "<p>This document refuses to be framed by policy.</p></body></html>";
        return result;
    }

    if ( path == "/auth" && !recorded.originAuthorized )
    {
        result.status = 401;
        result.headers.append(
            qMakePair( QByteArray( "WWW-Authenticate" ),
                       QByteArray( "Basic realm=\"" ) + HttpFixture::originRealm().toUtf8() + '"' ) );
        result.body = "origin credentials required\n";
        return result;
    }

    if ( path == "/auth" )
    {
        result.body = "origin-authenticated\n";
        return result;
    }

    if ( auto file = fixtureFile( path ) )
    {
        result.contentType = contentTypeFor( path );
        result.body = *file;
        return result;
    }

    result.status = 404;
    result.body = "not found\n";
    return result;
}

void HttpFixturePrivate::writeResponse( QTcpSocket * socket, RouteResult const & response,
                                          QByteArray const & method )
{
    if ( !socket || socket->state() == QAbstractSocket::UnconnectedState )
        return;

    QByteArray head = "HTTP/1.1 " + statusReason( response.status ) + "\r\n";

    if ( !response.contentType.isEmpty() )
        head += "Content-Type: " + response.contentType + "\r\n";

    head += "Content-Length: " + QByteArray::number( response.body.size() ) + "\r\n";

    for ( auto const & entry : response.headers )
        head += entry.first + ": " + entry.second + "\r\n";

    head += "Connection: close\r\n\r\n";

    socket->write( head );

    if ( method != "HEAD" )
        socket->write( response.body );

    socket->flush();
    socket->disconnectFromHost();
}

void HttpFixturePrivate::forwardAbsolute( QTcpSocket * socket, ParsedRequest const & request )
{
    const QUrl target( QString::fromUtf8( request.target ) );

    if ( !target.isValid() || target.scheme() != QLatin1String( "http" ) || target.host().isEmpty() )
    {
        RouteResult bad;
        bad.status = 400;
        bad.body = "invalid proxy target\n";
        writeResponse( socket, bad, request.method );
        return;
    }

    const QString host = target.host();
    const quint16 targetPort = static_cast< quint16 >( target.port( 80 ) );

    auto * upstream = new QTcpSocket( server );

    QObject::connect( upstream, &QTcpSocket::connected, upstream,
                      [ upstream, socket, request, target, host, targetPort ]() {
        QByteArray forwarded = request.method + ' ' + target.path().toUtf8();

        if ( !target.query().isEmpty() )
            forwarded += '?' + target.query().toUtf8();

        forwarded += " HTTP/1.1\r\nHost: " + host.toUtf8() + ':'
            + QByteArray::number( targetPort ) + "\r\nConnection: close\r\n\r\n";

        upstream->write( forwarded );
    } );

    QObject::connect( upstream, &QTcpSocket::readyRead, upstream, [ upstream, socket ]() {
        if ( socket->state() == QAbstractSocket::ConnectedState )
            socket->write( upstream->readAll() );
    } );

    QObject::connect( upstream, &QTcpSocket::disconnected, upstream, [ upstream, socket ]() {
        if ( socket->state() == QAbstractSocket::ConnectedState )
            socket->disconnectFromHost();

        upstream->deleteLater();
    } );

    QObject::connect( upstream, &QTcpSocket::errorOccurred, upstream,
                      [ upstream, socket ]( QAbstractSocket::SocketError ) {
                          if ( socket->state() == QAbstractSocket::ConnectedState )
                          {
                              socket->write( "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 21\r\n"
                                             "Connection: close\r\n\r\nproxy upstream error\n" );
                              socket->disconnectFromHost();
                          }

                          upstream->deleteLater();
                      } );

    upstream->connectToHost( host, targetPort );
}

void HttpFixturePrivate::handleConnect( QTcpSocket * socket, ParsedRequest const & request )
{
    // From here on this socket is a byte pipe, not an HTTP connection: the
    // tunneled request (usually a GET or a TLS handshake) belongs to the
    // upstream origin and must never be parsed or answered by this fixture.
    tunnelled.insert( socket );

    const QByteArray target = request.target;
    const int colon = target.lastIndexOf( ':' );

    bool portOk = false;
    const QString host = colon > 0 ? QString::fromUtf8( target.left( colon ) ) : QString();
    const int targetPort = colon > 0 ? target.mid( colon + 1 ).toInt( &portOk ) : 0;

    if ( host.isEmpty() || !portOk || targetPort <= 0 || targetPort > 65535 )
    {
        RouteResult bad;
        bad.status = 400;
        bad.body = "invalid CONNECT target\n";
        writeResponse( socket, bad, "GET" );
        return;
    }

    auto * upstream = new QTcpSocket( server );

    QObject::connect( upstream, &QTcpSocket::connected, upstream, [ upstream, socket ]() {
        socket->write( "HTTP/1.1 200 Connection Established\r\n\r\n" );
        socket->flush();
        upstream->write( socket->readAll() );
    } );

    QObject::connect( upstream, &QTcpSocket::readyRead, upstream, [ upstream, socket ]() {
        if ( socket->state() == QAbstractSocket::ConnectedState )
            socket->write( upstream->readAll() );
    } );

    QObject::connect( socket, &QTcpSocket::readyRead, socket, [ upstream, socket ]() {
        const QByteArray pending = socket->readAll();

        if ( !pending.isEmpty() && upstream->state() == QAbstractSocket::ConnectedState )
            upstream->write( pending );
    } );

    QObject::connect( upstream, &QTcpSocket::disconnected, upstream, [ upstream, socket ]() {
        if ( socket->state() == QAbstractSocket::ConnectedState )
            socket->disconnectFromHost();

        upstream->deleteLater();
    } );

    QObject::connect( upstream, &QTcpSocket::errorOccurred, upstream,
                      [ upstream, socket ]( QAbstractSocket::SocketError ) {
                          if ( socket->state() == QAbstractSocket::ConnectedState )
                          {
                              socket->write( "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 16\r\n"
                                             "Connection: close\r\n\r\ntunnel upstream\n" );
                              socket->disconnectFromHost();
                          }

                          upstream->deleteLater();
                      } );

    upstream->connectToHost( host, static_cast< quint16 >( targetPort ) );
}

void HttpFixturePrivate::dispatch( QTcpSocket * socket, ParsedRequest const & request,
                                   Request & recorded )
{
    const bool proxyRequest = request.method == "CONNECT" || request.target.startsWith( "http://" );
    recorded.proxyRequest = proxyRequest;

    if ( proxyRequest )
    {
        const bool authorized = credentialMatches( requestHeader( "Proxy-Authorization", request.headers ),
                                                   HttpFixture::proxyUser(), HttpFixture::proxyPassword() );
        recorded.proxied = authorized;

        if ( !authorized )
        {
            RouteResult challenge;
            challenge.status = 407;
            challenge.headers.append(
                qMakePair( QByteArray( "Proxy-Authenticate" ),
                           QByteArray( "Basic realm=\"" ) + HttpFixture::proxyRealm().toUtf8() + '"' ) );
            challenge.headers.append( qMakePair( QByteArray( "Cache-Control" ),
                                                 QByteArray( "no-store" ) ) );
            challenge.body = "proxy credentials required\n";
            writeResponse( socket, challenge, "GET" );
            return;
        }
    }

    if ( request.method == "CONNECT" )
    {
        recorded.tunnel = true;
        handleConnect( socket, request );
        return;
    }

    if ( proxyRequest )
    {
        // Absolute-form request through the proxy: really forward it to the
        // target origin instead of inventing a local answer.
        forwardAbsolute( socket, request );
        return;
    }

    if ( recorded.path == "/auth" )
        recorded.originAuthorized =
            credentialMatches( requestHeader( "Authorization", request.headers ), HttpFixture::originUser(),
                               HttpFixture::originPassword() );

    const RouteResult result = route( request, recorded );

    if ( result.barrier )
    {
        // The request is already recorded, so a test can wait for it and then
        // release the answer; until then the connection stays open.
        Waiting entry;
        entry.socket = socket;
        entry.path = recorded.path;
        waiting.append( entry );
        return;
    }

    writeResponse( socket, result, request.method );
}

void HttpFixturePrivate::releaseWaiting( QByteArray const & path )
{
    QVector< Waiting > remaining;

    for ( Waiting const & entry : waiting )
    {
        if ( entry.path != path )
        {
            remaining.append( entry );
            continue;
        }

        RouteResult result;
        result.body = "slow-resource-body\n";
        writeResponse( entry.socket, result, "GET" );
    }

    waiting = remaining;
}

////////// HttpFixture

HttpFixture::HttpFixture(): d( std::make_unique< HttpFixturePrivate >() )
{
    d->server = new QTcpServer();

    QObject::connect( d->server, &QTcpServer::newConnection, d->server, [ this ]() {
        while ( d->server->hasPendingConnections() )
        {
            QTcpSocket * socket = d->server->nextPendingConnection();

            if ( !socket )
                continue;

            QObject::connect( socket, &QTcpSocket::readyRead, socket, [ this, socket ]() {
                if ( d->tunnelled.contains( socket ) )
                    return;

                QByteArray & buffer = d->buffers[ socket ];
                buffer += socket->readAll();

                const ParsedRequest parsed = parseRequestHead( buffer );

                if ( !parsed.complete )
                {
                    if ( !parsed.valid )
                    {
                        d->buffers.remove( socket );
                        socket->disconnectFromHost();
                    }
                    return;
                }

                d->buffers.remove( socket );

                if ( !parsed.valid )
                {
                    socket->disconnectFromHost();
                    return;
                }

                Request recorded;
                recorded.method = parsed.method;
                recorded.target = parsed.target;
                recorded.host = requestHeader( "Host", parsed.headers );

                if ( parsed.method == "CONNECT" )
                    recorded.path = parsed.target;
                else
                {
                    const QUrl target( parsed.target.startsWith( "http://" )
                                           ? QString::fromUtf8( parsed.target )
                                           : QStringLiteral( "http://fixture.invalid" )
                                                 + QString::fromUtf8( parsed.target ) );
                    recorded.path = target.path( QUrl::FullyDecoded ).toUtf8();
                    recorded.query = target.query( QUrl::FullyDecoded ).toUtf8();
                }

                // The record is appended before the answer is decided so a test
                // can wait for a request that the /slow-resource barrier is
                // still holding. dispatch() fills in the proxy/auth flags on the
                // stored record itself.
                d->requests.append( recorded );

                d->dispatch( socket, parsed, d->requests.last() );
            } );

            QObject::connect( socket, &QTcpSocket::disconnected, socket, [ this, socket ]() {
                d->buffers.remove( socket );
                d->tunnelled.remove( socket );

                // A barrier waiter that went away must not be answered later.
                QVector< HttpFixturePrivate::Waiting > remaining;

                for ( HttpFixturePrivate::Waiting const & entry : d->waiting )
                {
                    if ( entry.socket != socket )
                        remaining.append( entry );
                }

                d->waiting = remaining;
            } );
        }
    } );
}

HttpFixture::~HttpFixture()
{
    if ( !d )
        return;

    // Closing the server deletes its child sockets (nextPendingConnection()
    // parents them), which also drops every lambda that referenced them.
    delete d->server;
    d->server = nullptr;

    d.reset();
}

bool HttpFixture::start( QHostAddress const & address, quint16 port )
{
    if ( !d || !d->server )
        return false;

    if ( d->server->isListening() )
    {
        d->error = QStringLiteral( "the fixture is already listening on %1:%2" )
                       .arg( d->address.toString() )
                       .arg( d->port );
        return false;
    }

    if ( !d->server->listen( address, port ) )
    {
        d->error = QStringLiteral( "cannot bind %1:%2: %3" )
                       .arg( address.toString() )
                       .arg( port )
                       .arg( d->server->errorString() );
        d->address = address;
        return false;
    }

    d->address = d->server->serverAddress();
    d->port = d->server->serverPort();

    if ( d->port == 0 )
    {
        d->error = QStringLiteral( "the socket bound %1 but reports no port" ).arg( address.toString() );
        d->server->close();
        return false;
    }

    d->error.clear();
    return true;
}

QUrl HttpFixture::url( QString const & path ) const
{
    QUrl result;

    result.setScheme( QStringLiteral( "http" ) );
    result.setHost( d ? d->address.toString() : QString() );
    result.setPort( d ? static_cast< int >( d->port ) : 0 );
    result.setPath( path );

    return result;
}

QVector< HttpFixture::Request > HttpFixture::requests() const
{
    return d ? d->requests : QVector< Request >();
}

void HttpFixture::release( QString const & path )
{
    if ( d )
        d->releaseWaiting( path.toUtf8() );
}

QHostAddress HttpFixture::address() const
{
    return d ? d->address : QHostAddress();
}

quint16 HttpFixture::port() const
{
    return d ? d->port : 0;
}

QString HttpFixture::errorString() const
{
    return d ? d->error : QStringLiteral( "no fixture implementation" );
}

bool HttpFixture::isListening() const
{
    return d && d->server && d->server->isListening();
}

int HttpFixture::heldRequestCount( QString const & path ) const
{
    int count = 0;

    if ( !d )
        return count;

    const QByteArray encoded = path.toUtf8();

    for ( HttpFixturePrivate::Waiting const & entry : d->waiting )
    {
        if ( entry.path == encoded )
            ++count;
    }

    return count;
}

int HttpFixture::requestCount( QString const & path ) const
{
    int count = 0;

    if ( !d )
        return count;

    const QByteArray encoded = path.toUtf8();

    for ( Request const & request : d->requests )
    {
        if ( request.path == encoded )
            ++count;
    }

    return count;
}

void HttpFixture::clearRequests()
{
    if ( d )
        d->requests.clear();
}

QNetworkProxy HttpFixture::httpProxy() const
{
    return QNetworkProxy( QNetworkProxy::HttpProxy, d ? d->address.toString() : QString(),
                          d ? static_cast< quint16 >( d->port ) : 0 );
}

QNetworkProxy HttpFixture::cachingProxy() const
{
    return QNetworkProxy( QNetworkProxy::HttpCachingProxy, d ? d->address.toString() : QString(),
                          d ? static_cast< quint16 >( d->port ) : 0 );
}

QString HttpFixture::proxyRealm()
{
    return QStringLiteral( "goldendict-fixture-proxy" );
}

QString HttpFixture::proxyUser()
{
    return QStringLiteral( "fixture-proxy" );
}

QString HttpFixture::proxyPassword()
{
    return QStringLiteral( "fixture-proxy-secret" );
}

QString HttpFixture::originRealm()
{
    return QStringLiteral( "goldendict-fixture-origin" );
}

QString HttpFixture::originUser()
{
    return QStringLiteral( "fixture-origin" );
}

QString HttpFixture::originPassword()
{
    return QStringLiteral( "fixture-origin-secret" );
}

QByteArray HttpFixture::pixelPng()
{
    return fixtureBytes( QStringLiteral( "article/pixel.png" ) );
}

QByteArray HttpFixture::fixtureBytes( QString const & relativePath )
{
    QFile file( fixturePath( relativePath ) );

    if ( !file.open( QIODevice::ReadOnly ) )
        return QByteArray();

    return file.readAll();
}

bool loopbackOriginsBindable( QStringList * reasons, QStringList const & hosts )
{
    bool bindable = true;

    for ( QString const & host : hosts )
    {
        const QHostAddress address( host );

        if ( address.isNull() )
        {
            if ( reasons )
                *reasons << QStringLiteral( "\"%1\" is not a usable loopback address" ).arg( host );
            bindable = false;
            continue;
        }

        HttpFixture fixture;

        if ( !fixture.start( address, 0 ) )
        {
            if ( reasons )
                *reasons << fixture.errorString();
            bindable = false;
            continue;
        }

        const QUrl probe = fixture.url( QStringLiteral( "/dir/first.html" ) );

        if ( probe.host() != host || probe.port() <= 0 )
        {
            if ( reasons )
                *reasons << QStringLiteral( "the fixture bound %1 but reported %2:%3" )
                                .arg( host, probe.host() )
                                .arg( probe.port() );
            bindable = false;
        }
    }

    return bindable;
}

} // namespace TestSupport
