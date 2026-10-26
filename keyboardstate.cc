/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "keyboardstate.hh"
#include <QObject> // To get Qt Q_OS defines

#ifdef Q_OS_WIN32
#include <windows.h>
#elif defined(HAVE_X11)
// The X11 modifier state is read through the running session's display, never
// through a display of our own: HAVE_X11 says this build can talk to Xlib, the
// active QPA platform says whether there is anything to talk to.
#include "platformwindow.hh"
#include <X11/X.h>
#include <X11/XKBlib.h>
#elif defined Q_OS_MAC
#define __SECURITYHI__
#include <Carbon/Carbon.h>
#endif

bool KeyboardState::checkModifiersPressed( int mask )
{
  #if defined(Q_OS_WIN32)

  return !(
    ( mask & Alt && !( GetAsyncKeyState( VK_MENU ) & 0x8000 ) ) ||
    ( mask & Ctrl && !( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) ) ||
    ( mask & Shift && !( GetAsyncKeyState( VK_SHIFT ) & 0x8000 ) ) ||
    ( mask & LeftAlt && !( GetAsyncKeyState( VK_LMENU ) & 0x8000 ) ) ||
    ( mask & RightAlt && !( GetAsyncKeyState( VK_RMENU ) & 0x8000 ) ) ||
    ( mask & LeftCtrl && !( GetAsyncKeyState( VK_LCONTROL ) & 0x8000 ) ) ||
    ( mask & RightCtrl && !( GetAsyncKeyState( VK_RCONTROL ) & 0x8000 ) ) ||
    ( mask & LeftShift && !( GetAsyncKeyState( VK_LSHIFT ) & 0x8000 ) ) ||
    ( mask & RightShift && !( GetAsyncKeyState( VK_RSHIFT ) & 0x8000 ) ) );

  #elif defined Q_OS_MAC
  UInt32 keys = GetCurrentKeyModifiers();
  return !(
    ( mask & Alt && !( keys & ( 1 << optionKeyBit ) ) ) ||
    ( mask & Ctrl && !( keys & ( 1 << cmdKeyBit ) ) ) ||
    ( mask & Shift && !( keys & ( 1 << shiftKeyBit ) ) ) ||
    ( mask & Win && !( keys & ( 1 << controlKeyBit ) ) ) );
  #elif defined(HAVE_X11)
  Display * const display = PlatformWindow::x11Display();

  if ( !display )
  {
    // A Wayland or offscreen session has no X11 modifier state to read. Nothing
    // is claimed: only an empty request is trivially satisfied.
    return mask == 0;
  }

  XkbStateRec state;

  XkbGetState( display, XkbUseCoreKbd, &state );

  return !(
    ( mask & Alt && !( state.base_mods & Mod1Mask ) ) ||
    ( mask & Ctrl && !( state.base_mods & ControlMask ) ) ||
    ( mask & Shift && !( state.base_mods & ShiftMask ) ) ||
    ( mask & Win && !( state.base_mods & Mod4Mask ) ) );
  #else
  // This build has no keyboard-state source at all.
  return mask == 0;
  #endif
}
