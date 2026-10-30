/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "broken_xrecord.hh"

#include <QtGlobal>

#ifdef HAVE_X11
#include "platformwindow.hh"
#include <X11/Xlib.h>

#include <cstring>
#endif

bool isRECORDBroken()
{
#ifdef HAVE_X11

  // The check asks the running session's server. A Wayland or offscreen session
  // has no X11 display, and asking a null one would crash; there is no broken
  // RECORD extension where there is no RECORD extension at all.
  Display * const display = PlatformWindow::x11Display();

  if ( !display )
    return false;

  char const * const vendor = ServerVendor( display );

  if ( vendor && strstr( vendor, "X.Org" ) )
  {
    int const release = VendorRelease( display );

    return release >= 10600000 && release < 10701000;
  }

#endif

  return false;
}
