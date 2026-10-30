/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Test-only fake of the desktop portal's GlobalShortcuts interface, served from
 * a private bus a test owns (see TestSupport::PrivateSessionBus).
 *
 * The point of the fake is to be *exactly* shaped like the real portal on the
 * wire while being fully deterministic: every method, request handle, Response,
 * session and signal is driven by a named scenario, so a portal test can pin
 * the early/late/alternate/list/bind/loss races the production backend has to
 * survive without touching the user's desktop bus.
 *
 * Wire shape (spec §4.12):
 *
 *   service    org.freedesktop.portal.Desktop
 *   path       /org/freedesktop/portal/desktop
 *   interface  org.freedesktop.portal.GlobalShortcuts
 *     CreateSession(a{sv} options) -> o request
 *     ListShortcuts(o session, a{sv} options) -> o request
 *     BindShortcuts(o session, a(sa{sv}) shortcuts, s parent_window, a{sv} options) -> o request
 *     ConfigureShortcuts(o session, s parent_window, a{sv} options)          [v2 only]
 *     Activated(o session, s id, t timestamp, a{sv} options)
 *     Deactivated(o session, s id, t timestamp, a{sv} options)
 *     ShortcutsChanged(o session, a(sa{sv}) shortcuts)
 *     org.freedesktop.DBus.Properties.Get(interface, "version") -> v (uint)
 *   request path  /org/freedesktop/portal/desktop/request/<sender>/<handle_token>
 *     interface org.freedesktop.portal.Request
 *       Close(); Response(u response, a{sv} results)
 *   session path  /org/freedesktop/portal/desktop/session/<sender>/<session_handle_token>
 *     interface org.freedesktop.portal.Session
 *       Close(); Closed(a{sv} details)
 *
 * Scenarios (see FakePortal::scenarios()):
 *
 *   fast                     method replies, then a success Response follows.
 *   early-response           success Response on the *predicted* request path
 *                            is sent before the method reply.
 *   early-alternate-response success Response on the *returned alternate* path
 *                            is sent before the method reply names that path
 *                            (the named Task 30 scenario).
 *   alternate-response       method returns an alternate path; the Response
 *                            follows the reply.
 *   delayed-response         method replies; the Response waits for
 *                            releasePending().
 *   no-response              method replies; no Response is ever sent.
 *   no-method-reply          neither a method reply nor a Response ever arrives
 *                            (the client has to hit its own deadline).
 *   delayed-method-reply     the method reply and the Response wait for
 *                            releasePending(), so a test can cancel first.
 *   method-error             method replies with a D-Bus error.
 *   malformed-method-return  method replies with a string instead of a path.
 *   duplicate-response       two identical Responses follow the reply.
 *   unrelated-response       the Response is emitted on an unrelated path.
 *   invalid-session-handle   Response(0) with a non-path session_handle string.
 *   missing-session-handle   Response(0) without a session_handle at all.
 *   late-create-response     CreateSession replies; the session Response waits
 *                            for releasePending(), so a cancelled request can be
 *                            observed closing the late session.
 *   denied                   Response(1, {}) (cancellation/denial).
 *   failed                   Response(2, {}) (other portal failure).
 *   list-slow                ListShortcuts replies; the listing waits for
 *                            releasePending().
 *   list-malformed-tuple     List response payload is `as` (no struct tuple).
 *   list-malformed-map       List response payload is `aa{sv}` (no id tuple).
 *   list-duplicate-ids       List response repeats one shortcut id.
 *   list-property-wrong-type List response carries `description` as a uint.
 *   bind-partial             Bind reports a proper nonempty subset.
 *   bind-empty               Bind reports an empty bound set.
 *   bind-unknown-id          Bind reports an id that was never requested.
 *   bind-duplicate-id        Bind repeats one requested id.
 *   bind-wrong-type          Bind response payload is `as`.
 *   bind-denied              Bind Response(1, {}).
 *   bind-failed              Bind Response(2, {}).
 *   configure-error          ConfigureShortcuts replies with a D-Bus error.
 *   no-version               Properties.Get(version) fails as unknown property.
 *   version-wrong-type       Properties.Get(version) returns a string.
 *
 * A second BindShortcuts for the same session is refused with a D-Bus error and
 * recorded as a protocol violation: the one-Bind-per-session invariant can never
 * pass silently. Request and session Close calls are recorded, so a test can
 * assert the production teardown order through calls().
 */

#ifndef GOLDENDICT_TESTS_SUPPORT_FAKE_PORTAL_HH_INCLUDED
#define GOLDENDICT_TESTS_SUPPORT_FAKE_PORTAL_HH_INCLUDED

#include <QDBusArgument>
#include <QDBusConnection>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <memory>

namespace TestSupport {

struct FakePortalPrivate;

class FakePortal
{
public:
    /// One shortcut as it appears on the wire: `(sa{sv})`.
    struct Shortcut
    {
        QString id;
        QVariantMap properties;
    };

    /// One recorded incoming call.
    struct Call
    {
        QString member;           ///< CreateSession, ListShortcuts, BindShortcuts, ConfigureShortcuts,
                                  ///< Request.Close, Session.Close, Properties.Get
        QString session;          ///< Session path the call addressed (empty for CreateSession).
        QString requestPath;      ///< Request path the call addressed (Close) or was given (creation).
        QString parentWindow;     ///< Bind/Configure parent_window.
        QString activationToken;  ///< Configure activation_token when supplied.
        QStringList shortcutIds;  ///< Bind requested ids in wire order.
        QVector< Shortcut > shortcuts; ///< Bind requested entries with properties.
        QVariantMap options;      ///< Method options map.
        QByteArray signature;     ///< The exact wire signature of the call.
    };

    FakePortal();
    ~FakePortal();

    FakePortal( FakePortal const & ) = delete;
    FakePortal & operator=( FakePortal const & ) = delete;

    /// Exports the portal interfaces on `connection` (which must already be
    /// connected to the private bus) and takes org.freedesktop.portal.Desktop.
    /// Returns false and sets errorString() when the name is taken or the object
    /// cannot be exported; a test must fail on that, never skip.
    bool start( QDBusConnection const & connection, uint version );

    void setScenario( QString const & scenario );
    QString scenario() const;

    /// Every call received so far, in arrival order.
    QVector< Call > calls() const;

    void emitActivation( QString const & session, QString const & id, QString const & token );
    void loseService();

    static QString serviceName();
    static QString portalPath();
    static QString portalInterface();
    static QString requestInterface();
    static QString sessionInterface();

    /// Every scenario name this fixture implements, for a consumer that wants to
    /// assert the set it depends on exists.
    static QStringList scenarios();

    /// List/Bind payload configuration. Binding reports the requested entries by
    /// default (all of them, with the requested properties).
    void setListShortcuts( QVector< Shortcut > const & entries );
    void setBoundShortcuts( QVector< Shortcut > const & entries );
    void setBoundIds( QStringList const & ids );

    /// Additional portal events a consumer may need.
    void emitDeactivated( QString const & session, QString const & id, QString const & token );
    void emitShortcutsChanged( QString const & session, QVector< Shortcut > const & entries );
    void emitSessionClosed( QString const & session, QString const & reason );

    /// Flushes every reply/signal a "delayed"/"late"/"slow" scenario is holding.
    void releasePending();

    int count( QString const & member ) const;

    /// Calls received so far, any member. A test waits for this instead of
    /// sleeping when it needs to know the client's call has actually arrived.
    int receivedCallCount() const;
    int bindCountForSession( QString const & session ) const;
    QStringList requestPaths() const;
    QStringList protocolViolations() const;
    QString sessionPath() const;
    QString serviceOwner() const;
    uint version() const;
    bool isStarted() const;
    bool isRegistered() const;
    QString errorString() const;

private:
    std::unique_ptr< FakePortalPrivate > d;
};

/// Wire marshalling of one `(sa{sv})` shortcut. Declared at namespace scope so
/// argument-dependent lookup finds them from QtDBus' own templates.
QDBusArgument &operator<<( QDBusArgument & out, FakePortal::Shortcut const & value );
QDBusArgument const & operator>>( QDBusArgument const & in, FakePortal::Shortcut & value );

} // namespace TestSupport

Q_DECLARE_METATYPE( TestSupport::FakePortal::Shortcut )

#endif // GOLDENDICT_TESTS_SUPPORT_FAKE_PORTAL_HH_INCLUDED
