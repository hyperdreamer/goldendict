/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Test-only HTTP fixture: a real loopback origin (and HTTP proxy) the browser,
 * print and save suites load the fixed fixture files from.
 *
 * Why a real server: a browser test that only inspects generated HTML strings
 * proves nothing about origins, redirects, framing policy, authentication or
 * request barriers. This fixture binds a real ephemeral port on a real loopback
 * address, records the requests it received and answers with the fixed bytes of
 * tests/fixtures/, so a suite can observe what Chromium actually did.
 *
 * Fixed routes (the path is the oracle; see each fixture file for the markers
 * inside the body):
 *
 *   /                          fixed text/plain index marker
 *   /dir/first.html            tests/fixtures/website/first.html
 *   /dir/second.html           tests/fixtures/website/second.html
 *   /dir/nested.html           tests/fixtures/website/nested.html
 *   /dir/style.css             tests/fixtures/website/style.css
 *   /dir/relative.js           tests/fixtures/website/relative.js
 *   /dir/data.json             tests/fixtures/website/data.json
 *   /article/article.html      tests/fixtures/article/article.html
 *   /article/dom.html          tests/fixtures/article/dom.html
 *   /article/style.css         tests/fixtures/article/style.css
 *   any path ending in .png    tests/fixtures/article/pixel.png (1x1)
 *   /print/two-pages.html      tests/fixtures/print/two-pages.html
 *   /redirect                  302 to /dir/first.html (relative Location)
 *   /redirect-absolute         302 to the absolute URL of /dir/first.html
 *   /slow-resource             200, but only after release("/slow-resource")
 *   /deny-frame                200 + X-Frame-Options: DENY
 *   /deny-csp                  200 + Content-Security-Policy: frame-ancestors 'none'
 *   /auth                      401 Basic challenge, then 200 for the fixture credentials
 *   /missing                   404
 *
 * Proxy use: the same object accepts `CONNECT host:port` and absolute-form
 * request targets. Both challenge with 407 + Proxy-Authenticate until the
 * fixed proxy credentials arrive; CONNECT then opens a real tunnel and an
 * absolute-form request is really forwarded to its target origin.
 *
 * Ports are always ephemeral (port 0) and always read back, and every bind
 * failure is a `false` return with a diagnostic `errorString()`; callers fail
 * their suite with QVERIFY2 instead of skipping.
 */

#ifndef GOLDENDICT_TESTS_SUPPORT_HTTP_FIXTURE_HH_INCLUDED
#define GOLDENDICT_TESTS_SUPPORT_HTTP_FIXTURE_HH_INCLUDED

#include <QByteArray>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <memory>

class QTcpServer;
class QTcpSocket;

namespace TestSupport {

struct HttpFixturePrivate;

class HttpFixture
{
public:
    /// One recorded request. Only the shape of the request is kept: never the
    /// Authorization/Proxy-Authorization value itself, so no test log can leak
    /// a credential.
    struct Request
    {
        QByteArray method;
        QByteArray target; ///< Raw request target (absolute form for proxy requests).
        QByteArray path;   ///< Path component of the target.
        QByteArray query;
        QByteArray host;
        bool proxyRequest = false;    ///< Absolute-form target or CONNECT.
        bool proxied = false;         ///< Carried valid fixture proxy credentials.
        bool originAuthorized = false;///< Carried valid fixture origin credentials.
        bool tunnel = false;          ///< CONNECT request.
    };

    HttpFixture();
    ~HttpFixture();

    HttpFixture( HttpFixture const & ) = delete;
    HttpFixture & operator=( HttpFixture const & ) = delete;

    /// Binds `address` (an IPv4/IPv6 literal, normally one of
    /// 127.0.0.1/127.0.0.2/127.0.0.3) on `port`, or an ephemeral port when 0.
    bool start( QHostAddress const & address, quint16 port = 0 );

    /// The URL of `path` on this server: scheme http, the bound address as host
    /// and the actually bound (non-default) port.
    QUrl url( QString const & path ) const;

    QVector< Request > requests() const;

    /// Releases every request waiting on the `/slow-resource` barrier whose path
    /// matches. Idempotent; releasing a path with no waiter is a no-op.
    void release( QString const & path );

    QHostAddress address() const;
    quint16 port() const;
    QString errorString() const;
    bool isListening() const;

    int requestCount( QString const & path ) const;

    /// Requests currently parked on the barrier for `path`, i.e. received and
    /// deliberately unanswered. This is the state a test waits for instead of
    /// sleeping: an unbarriered server never reports a waiter, and a slow machine
    /// only makes the wait longer.
    int heldRequestCount( QString const & path ) const;
    void clearRequests();

    /// An HTTP proxy object pointing at this server, for the proxy/auth suites.
    QNetworkProxy httpProxy() const;
    QNetworkProxy cachingProxy() const;

    /// Fixed fixture credentials and realms (fixture-only values; never real).
    static QString proxyRealm();
    static QString proxyUser();
    static QString proxyPassword();
    static QString originRealm();
    static QString originUser();
    static QString originPassword();

    /// The shared 1x1 PNG every image route serves.
    static QByteArray pixelPng();

    /// Reads one fixture file (path relative to the fixture root).
    static QByteArray fixtureBytes( QString const & relativePath );

private:
    std::unique_ptr< HttpFixturePrivate > d;
};

/// The three literal loopback addresses the browser fixtures bind.
QStringList defaultLoopbackHosts();

/// Binds every address in `hosts` on an ephemeral port, verifies the address and
/// the read-back port and closes the server again. Appends one reason per
/// failure and returns false when any required address cannot be served, so a
/// suite's initTestCase turns that into a required failure instead of a silent
/// skip.
bool loopbackOriginsBindable( QStringList * reasons,
                              QStringList const & hosts = defaultLoopbackHosts() );

} // namespace TestSupport

#endif // GOLDENDICT_TESTS_SUPPORT_HTTP_FIXTURE_HH_INCLUDED
