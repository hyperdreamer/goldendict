/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * A hotkey field can only record a combination the desktop lets through. On a
 * Wayland session the compositor matches global shortcuts before the key ever
 * reaches the focused client, so a combination that is currently an active
 * shortcut - this application's own portal binding, or any other desktop
 * shortcut - silently never arrives at the editor. The desktop has to be told
 * to stop matching while the field is armed, exactly as the Plasma shortcut KCM
 * does while it records.
 *
 * Everything here is best effort: a session without KGlobalAccel keeps matching
 * its shortcuts and recording behaves as before.
 */

#ifndef __GLOBALSHORTCUTBLOCK_HH_INCLUDED__
#define __GLOBALSHORTCUTBLOCK_HH_INCLUDED__

#include <QObject>

#include <functional>

/// Suspends the desktop's global shortcuts while armed, so that a key whose
/// combination is an active shortcut still reaches the recording widget.
class GlobalShortcutBlock: public QObject
{
  Q_OBJECT

public:
  /// How a suspension request leaves this process. The test suite installs a
  /// recorder so a test run never suspends the shortcuts of the machine it runs
  /// on.
  using Sender = std::function< void ( bool block ) >;

  explicit GlobalShortcutBlock( QObject * parent = nullptr );
  ~GlobalShortcutBlock() override;

  /// Replaces the default sender; an empty function restores it.
  void setSender( Sender replacement );

  /// Balanced and idempotent: begin() while armed or end() while released sends
  /// nothing, so a stray second focus-in cannot leave the desktop suspended.
  void begin();
  void end();

  bool isActive() const;

private:
  Sender sender;
  bool active = false;
};

/// Asks KGlobalAccel to stop (or resume) matching global shortcuts. The call is
/// synchronous so the suspension is in place before the user's next keystroke is
/// matched, and bounded so an unresponsive daemon cannot hang the dialog.
void suspendDesktopShortcuts( bool block );

#endif
