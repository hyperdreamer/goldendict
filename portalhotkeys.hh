/* This file is part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#ifndef GOLDENDICT_PORTALHOTKEYS_HH_INCLUDED
#define GOLDENDICT_PORTALHOTKEYS_HH_INCLUDED

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

#include <memory>

#include "hotkeybackend.hh"

namespace PortalWire {

/// One GlobalShortcuts wire entry, encoded as `(sa{sv})`.
struct Shortcut
{
    QString id;
    QVariantMap properties;
};

using Shortcuts = QList< Shortcut >;

QDBusArgument &operator<<( QDBusArgument & out, Shortcut const & value );
QDBusArgument const &operator>>( QDBusArgument const & in, Shortcut & value );

} // namespace PortalWire

Q_DECLARE_METATYPE( PortalWire::Shortcut )
Q_DECLARE_METATYPE( PortalWire::Shortcuts )

class PortalRequestState;
class QDBusServiceWatcher;

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

class PortalBackend final : public HotkeyBackend
{
  Q_OBJECT
public:
  explicit PortalBackend( QDBusConnection const & bus, QObject * parent = nullptr );
  ~PortalBackend() override;

  bool isSupported() const noexcept override;
  HotkeyBackend::State state() const noexcept override;
  void bind( QVector< HotkeyBinding > const & bindings ) override;
  void unbind() override;
  void configure( QString const & activationToken ) override;

  /// Decides which shortcut IDs may be shown. The config/UI layer owns this
  /// policy; it is deliberately independent of the IDs the current bind attempt
  /// carries, because a portal session may be unable to bind a shortcut whose
  /// stored value should still be displayed. Until it is called, the IDs of the
  /// first request are used, so a caller that has no separate policy keeps the
  /// previous behaviour.
  void setDisplayPolicy( QSet< QString > const & ids );

private slots:
  void receivePortalSignal( QDBusMessage const & message );

private:
  struct Private;

  void setState( HotkeyBackend::State state, QString const & explanation );
  void beginGeneration( QVector< HotkeyBinding > const & bindings );
  void readVersion( quint64 generation );
  void createSession( quint64 generation );
  void startList( quint64 generation );
  void startBind( quint64 generation );
  void handleCreateResponse( quint64 generation, uint code, QVariantMap const & results );
  void handleListResponse( quint64 generation, quint64 revision, uint code,
                           QVariantMap const & results );
  void handleBindResponse( quint64 generation, uint code, QVariantMap const & results );
  void failGeneration( QString const & explanation, bool supported = true );
  void cancelRequests() noexcept;
  void forgetRequest( PortalRequest * request ) noexcept;
  void closeSession() noexcept;
  void ownerChanged( QString const & oldOwner, QString const & newOwner );

  std::unique_ptr< Private > d;
  QDBusServiceWatcher * serviceWatcher = nullptr;
};

#endif // GOLDENDICT_PORTALHOTKEYS_HH_INCLUDED
