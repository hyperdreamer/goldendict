/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef __HOTKEYEDIT_HH_INCLUDED__
#define __HOTKEYEDIT_HH_INCLUDED__

#include "config.hh"
#include <QLineEdit>

// This widget allows grabbing a hotkey
class HotKeyEdit: public QLineEdit
{
  Q_OBJECT

  Qt::KeyboardModifiers currentModifiers;
  int currentKey1, currentKey2;

  bool continuingCombo;

public:

  HotKeyEdit( QWidget * parent = 0 );

  void setHotKey( Config::HotKey const & );

  /// Shows a value this widget cannot record: a portal trigger description is
  /// localized display text, so it can neither be parsed into the internal
  /// combination nor offered back as a binding request. The combination is
  /// cleared, so getHotKey() never reports the mirror as a recorded shortcut.
  /// Callers make the widget read-only first.
  void setMirrorText( QString const & text );

  Config::HotKey getHotKey() const;

protected:

  void keyPressEvent( QKeyEvent * event );
  void keyReleaseEvent( QKeyEvent * event );

private:

  void renderCurrentValue();
  bool eventFilter( QObject *, QEvent * event );
#ifdef Q_OS_WIN
  int VkeyToQTkey( quint32 vkey );
#endif
};

#endif
