/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "mutex.hh"

Mutex::Lock::Lock( Mutex & m_ ): locker( &m_ )
{
}

Mutex::Lock::~Lock()
{
}
