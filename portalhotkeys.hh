/* This file is part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#ifndef GOLDENDICT_PORTALHOTKEYS_HH_INCLUDED
#define GOLDENDICT_PORTALHOTKEYS_HH_INCLUDED

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

class PortalRequestState;

/// One asynchronous portal exchange, including responses sent before its reply.
class PortalRequest : public QObject
{
    Q_OBJECT
public:
    PortalRequest( QDBusConnection const & bus, quint64 generation, QObject * parent = nullptr );
    ~PortalRequest() override;
    void start( QDBusMessage message, QString const & handleToken );
    void cancel() noexcept;

signals:
    void response( quint64 generation, QString const & requestPath, uint code,
                   QVariantMap const & results );
    void failed( quint64 generation, QString const & message );

private:
    QPointer< PortalRequestState > d;
};

#endif
