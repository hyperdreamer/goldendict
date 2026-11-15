/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file. */

#include "globalshortcutblock.hh"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QString>

#include <utility>

namespace {

QString const serviceName = QStringLiteral( "org.kde.kglobalaccel" );
QString const objectPath = QStringLiteral( "/kglobalaccel" );
QString const interfaceName = QStringLiteral( "org.kde.KGlobalAccel" );
QString const methodName = QStringLiteral( "blockGlobalShortcuts" );
int const callTimeoutMs = 500;

} // unnamed namespace

void suspendDesktopShortcuts( bool block )
{
#ifdef QT_TESTCASE_BUILDDIR
  // A test run must never suspend the shortcuts of the desktop it runs on: the
  // suite injects its own sender and this one stays inert.
  Q_UNUSED( block )
  return;
#else
  QDBusConnection const bus = QDBusConnection::sessionBus();
  if( !bus.isConnected() )
    return;

  QDBusConnectionInterface * const busInterface = bus.interface();
  if( !busInterface || !busInterface->isServiceRegistered( serviceName ) )
    return;

  QDBusMessage message =
    QDBusMessage::createMethodCall( serviceName, objectPath, interfaceName, methodName );
  message << block;
  bus.call( message, QDBus::Block, callTimeoutMs );
#endif
}

GlobalShortcutBlock::GlobalShortcutBlock( QObject * parent ):
  QObject( parent ),
  sender( &suspendDesktopShortcuts )
{
}

GlobalShortcutBlock::~GlobalShortcutBlock()
{
  end();
}

void GlobalShortcutBlock::setSender( Sender replacement )
{
  sender = replacement ? std::move( replacement ) : Sender( &suspendDesktopShortcuts );
}

void GlobalShortcutBlock::begin()
{
  if( active )
    return;

  active = true;
  if( sender )
    sender( true );
}

void GlobalShortcutBlock::end()
{
  if( !active )
    return;

  active = false;
  if( sender )
    sender( false );
}

bool GlobalShortcutBlock::isActive() const
{
  return active;
}
