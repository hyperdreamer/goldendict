/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef __MUTEX_HH_INCLUDED__
#define __MUTEX_HH_INCLUDED__

#include <QMutexLocker>
#include <QRecursiveMutex>

/// This provides a mutex class. As you can see, it's just a Qt one, but it
/// does provide the Lock class which doesn't seem to exist in Qt, and it does
/// provide some abstraction for dictionaries in case they are to be ported
/// away from Qt.
///
/// Qt 6 has no recursive mode for QMutex, so recursion comes from
/// QRecursiveMutex. The lock is still taken through QMutexLocker, which is a
/// template since Qt 6 and therefore accepts this class (or deduces it from a
/// pointer) without an implicit conversion to QMutex.
class Mutex: public QRecursiveMutex
{
public:
  Mutex() = default;
  ~Mutex() = default;

  /// Locks the given mutex on construction and unlocks on destruction
  class Lock
  {
    QMutexLocker< Mutex > locker;

  public:

    Lock( Mutex & );
    ~Lock();

  private:
    Lock( Lock const & );
  };
};

#endif
