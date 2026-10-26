/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * The global-hotkey backend interface (§2.10). One backend is selected at
 * startup from the actual QPA platform, never from a compile-time X11
 * capability: the X11 RECORD recorder on an active xcb session and the XDG
 * Desktop Portal GlobalShortcuts session on native Wayland. A platform without
 * either gets a concrete disabled backend that explains itself; the factory
 * never returns null and never throws for a missing portal.
 *
 * The backend is owned by MainWindow through a unique pointer and parented to it
 * for signal affinity; MainWindow resets it before its QObject children are
 * destroyed. PortalBindingDisplay (the display type of bindingsChanged) is
 * declared by config.hh so the configuration can persist it and this header can
 * publish it without a cyclic definition.
 */

#ifndef HOTKEYBACKEND_HH_INCLUDED
#define HOTKEYBACKEND_HH_INCLUDED

#include <QObject>
#include <QString>
#include <QVector>

#include "config.hh"

/// One shortcut requested from a backend. `id` is a stable application action ID
/// ("main-window", "clipboard-lookup"), `description` is localized display text
/// for the portal dialog, and `sequence` is the XDG preferred_trigger produced by
/// HotkeySyntax::preferredTrigger.
struct HotkeyBinding
{
  QString id;
  QString description;
  QString sequence;
};

class HotkeyBackend : public QObject
{
  Q_OBJECT
public:
  /// Capability/lifecycle state of the selected backend. Unsupported and
  /// Unavailable/Denied/Closed are normal, explained outcomes: global hotkeys are
  /// best-effort and never become a clipboard-polling fallback.
  enum class State
  {
    Unsupported,
    Creating,
    Ready,
    Binding,
    Bound,
    Partial,
    Denied,
    Unavailable,
    Closed
  };
  Q_ENUM( State )

  using QObject::QObject;

  /// False when the backend cannot serve global hotkeys on this session at all
  /// (no portal service, no X11 display, unsupported platform).
  virtual bool isSupported() const noexcept = 0;

  /// Requests the given bindings. A backend that cannot support one of them says
  /// so through stateChanged()/bindingsChanged() instead of silently dropping or
  /// rewriting the stored sequence.
  virtual void bind( QVector< HotkeyBinding > const & bindings ) = 0;

  /// Retires the current registration (portal session or recorder handles).
  virtual void unbind() = 0;

  virtual State state() const noexcept = 0;

signals:
  /// A registered binding fired. `activationToken` is the portal's activation
  /// token forwarded unchanged for PlatformWindow::applyActivation(); an X11
  /// backend emits an empty token. Tokens are never logged or persisted.
  void activated( QString const & id, QString const & activationToken );

  /// The bindings the backend currently reports, for display only. A portal
  /// description is localized output; it is never parsed and never compared with
  /// the requested preferred_trigger.
  void bindingsChanged( QVector< PortalBindingDisplay > const & reported );

  /// One state transition with a human-readable explanation. Portal session
  /// loss, denial and unsupported capability are reported here once per
  /// transition, without a retry loop.
  void stateChanged( HotkeyBackend::State state, QString const & explanation );
};

Q_DECLARE_METATYPE( HotkeyBackend::State )

#endif // HOTKEYBACKEND_HH_INCLUDED
