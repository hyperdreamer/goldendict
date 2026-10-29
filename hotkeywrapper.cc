#ifdef __WIN32 // Q_OS_WIN32 isn't available at this point
#define _WIN32_WINNT 0x0430
#include <windows.h>
#endif

#include "hotkeywrapper.hh"
#include "platformwindow.hh"
#include "gddebug.hh"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <thread>

#ifdef Q_OS_WIN
#include "mainwindow.hh"
#include <QMainWindow>
#include <QWidget>
#endif

#ifdef HAVE_X11
// Xlib is included after the Qt headers, and the macros that collide with Qt are
// dropped below the way fixx11h.h does it for the application's translation
// units.
#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/keysym.h>
#include <X11/extensions/record.h>

#undef Bool
#undef None
#undef Status
#undef Success
#undef min
#undef max
#endif

//////////////////////////////////////////////////////////////////////////

QHotkeyApplication::QHotkeyApplication( int & argc, char ** argv ):
  QIntermediateApplication( argc, argv )
#ifdef Q_OS_WIN32
,  mainWindow( 0 )
#endif
{
  connect( this, SIGNAL( commitDataRequest( QSessionManager& ) ),
           this, SLOT( hotkeyAppCommitData( QSessionManager& ) ), Qt::DirectConnection );

  connect( this, SIGNAL( saveStateRequest( QSessionManager& ) ),
           this, SLOT( hotkeyAppSaveState( QSessionManager& ) ), Qt::DirectConnection );

#ifdef Q_OS_WIN
  installNativeEventFilter( this );
#endif
}

QHotkeyApplication::QHotkeyApplication( QString const & id,
                                        int & argc, char ** argv ):
  QIntermediateApplication( id, argc, argv )
#ifdef Q_OS_WIN32
,  mainWindow( 0 )
#endif
{
  connect( this, SIGNAL( commitDataRequest( QSessionManager& ) ),
           this, SLOT( hotkeyAppCommitData( QSessionManager& ) ), Qt::DirectConnection );

  connect( this, SIGNAL( saveStateRequest( QSessionManager& ) ),
           this, SLOT( hotkeyAppSaveState( QSessionManager& ) ), Qt::DirectConnection );

#ifdef Q_OS_WIN
  installNativeEventFilter( this );
#endif
}

void QHotkeyApplication::addDataCommiter( DataCommitter & d )
{
  dataCommitters.append( &d );
}

void QHotkeyApplication::removeDataCommiter( DataCommitter & d )
{
  dataCommitters.removeAll( &d );
}

void QHotkeyApplication::hotkeyAppCommitData( QSessionManager & mgr )
{
  for( int x = 0; x < dataCommitters.size(); ++x )
    dataCommitters[ x ]->commitData( mgr );
}

void QHotkeyApplication::hotkeyAppSaveState(QSessionManager & mgr )
{
  mgr.setRestartHint( QSessionManager::RestartNever );
}

void QHotkeyApplication::registerWrapper(HotkeyWrapper *wrapper)
{
  if (wrapper && !hotkeyWrappers.contains(wrapper))
    hotkeyWrappers.append(wrapper);
}

void QHotkeyApplication::unregisterWrapper(HotkeyWrapper *wrapper)
{
  if (wrapper && hotkeyWrappers.contains(wrapper))
    hotkeyWrappers.removeAll(wrapper);
}

//////////////////////////////////////////////////////////////////////////

HotkeyStruct::HotkeyStruct( quint32 key_, quint32 key2_, quint32 modifier_,
                            int handle_, int id_ ):
  key( key_ ),
  key2( key2_ ),
  modifier( modifier_ ),
  handle( handle_ ),
  id( id_ )
#ifdef Q_OS_MAC
  , hkRef( 0 )
  , hkRef2( 0 )
#endif
{
}

//////////////////////////////////////////////////////////////////////////

namespace HotkeyRuntime {

bool recordingLoopReturnedInTime( bool recordingFinished ) noexcept
{
  return recordingFinished;
}

bool isX11Session( QString const & platformName )
{
  QString const name = platformName.toLower();

  // Wayland first, for the same reason PlatformWindow::popupFlags() does it: a
  // compositor session must never be mistaken for X11, whatever else its name
  // says, and a capitalised name must not fall through the check.
  if ( name.contains( QLatin1String( "wayland" ) ) )
    return false;

  return name.startsWith( QLatin1String( "xcb" ) );
}

} // namespace HotkeyRuntime

//////////////////////////////////////////////////////////////////////////
// The recorder matcher. Pure: no Xlib, no QObject, no timer, so the X11 rules are
// provable on one build and without a RECORD server.

void RecorderMatcher::addBinding( quint32 key, quint32 key2, quint32 modifiers, int handle )
{
  RecorderBinding binding;
  binding.key = key;
  binding.key2 = key2;
  binding.modifiers = modifiers;
  binding.handle = handle;

  boundKeys.append( binding );
}

void RecorderMatcher::clearBindings()
{
  boundKeys.clear();
  cancelSecondKeyWait();
}

QVector< RecorderBinding > const & RecorderMatcher::bindings() const noexcept
{
  return boundKeys;
}

RecorderMatcher::Decision RecorderMatcher::press( quint32 keyCode, quint32 modifiers )
{
  Decision decision;

  if ( waiting )
  {
    // The wait never survives another press: the replaced code cancelled it
    // before comparing, and only the awaited combination itself may still fire.
    RecorderBinding const awaited = waiter;
    waiting = false;

    if ( awaited.key2 == keyCode && awaited.modifiers == modifiers )
    {
      decision.kind = Kind::Activated;
      decision.handle = awaited.handle;
      return decision;
    }
  }

  for( RecorderBinding const & binding : boundKeys )
  {
    if ( binding.key != keyCode || binding.modifiers != modifiers )
      continue;

    if ( binding.key2 == 0 )
    {
      decision.kind = Kind::Activated;
      decision.handle = binding.handle;
      return decision;
    }

    waiting = true;
    waiter = binding;

    decision.kind = Kind::SecondKeyWait;
    decision.handle = binding.handle;
    decision.secondKey = binding.key2;
    decision.secondKeyModifiers = binding.modifiers;
    // The second key is grabbed unless the combination begins with a non-copy key
    // and ends with the clipboard copy key: that copy must keep working.
    decision.grabSecondKey = isCopyToClipboardKey( binding.key, binding.modifiers )
                             || !isCopyToClipboardKey( binding.key2, binding.modifiers );
    return decision;
  }

  return decision;
}

void RecorderMatcher::cancelSecondKeyWait() noexcept
{
  waiting = false;
  waiter = RecorderBinding();
}

bool RecorderMatcher::awaitingSecondKey() const noexcept
{
  return waiting;
}

int RecorderMatcher::awaitedHandle() const noexcept
{
  return waiting ? waiter.handle : 0;
}

void RecorderMatcher::setModifierKeyCodes( RecorderModifierKeycodes const & keys ) noexcept
{
  modifierKeys = keys;
}

RecorderModifierKeycodes const & RecorderMatcher::modifierKeyCodes() const noexcept
{
  return modifierKeys;
}

void RecorderMatcher::setCopyKeyCodes( quint32 cKeyCode, quint32 insertKeyCode,
                                       quint32 keypadInsertKeyCode ) noexcept
{
  copyKeys.c = cKeyCode;
  copyKeys.insert = insertKeyCode;
  copyKeys.keypadInsert = keypadInsertKeyCode;
}

bool RecorderMatcher::isCopyToClipboardKey( quint32 keyCode, quint32 modifiers ) const noexcept
{
  return isCopyToClipboardKey( keyCode, modifiers, copyKeys.c, copyKeys.insert, copyKeys.keypadInsert );
}

bool RecorderMatcher::isModifierKeyCode( quint32 keyCode ) const noexcept
{
  return modifierFlagForKeyCode( keyCode, modifierKeys ) != 0;
}

quint32 RecorderMatcher::modifierPressed( quint32 keyCode ) noexcept
{
  currentModifiers |= modifierFlagForKeyCode( keyCode, modifierKeys );
  return currentModifiers;
}

quint32 RecorderMatcher::modifierReleased( quint32 keyCode ) noexcept
{
  currentModifiers &= ~modifierFlagForKeyCode( keyCode, modifierKeys );
  return currentModifiers;
}

quint32 RecorderMatcher::modifiers() const noexcept
{
  return currentModifiers;
}

quint32 RecorderMatcher::modifierFlagForKeyCode( quint32 keyCode,
                                                 RecorderModifierKeycodes const & keys ) noexcept
{
  if ( keyCode == 0 )
    return 0;

  if ( keyCode == keys.leftShift || keyCode == keys.rightShift )
    return RecorderShiftMask;

  if ( keyCode == keys.leftControl || keyCode == keys.rightControl )
    return RecorderControlMask;

  if ( keyCode == keys.leftAlt || keyCode == keys.rightAlt )
    return RecorderAltMask;

  if ( keyCode == keys.leftMeta || keyCode == keys.rightMeta )
    return RecorderMetaMask;

  return 0;
}

bool RecorderMatcher::isCopyToClipboardKey( quint32 keyCode,
                                            quint32 modifiers,
                                            quint32 cKeyCode,
                                            quint32 insertKeyCode,
                                            quint32 keypadInsertKeyCode ) noexcept
{
  if ( modifiers != RecorderControlMask )
    return false;

  return ( cKeyCode != 0 && keyCode == cKeyCode )
      || ( insertKeyCode != 0 && keyCode == insertKeyCode )
      || ( keypadInsertKeyCode != 0 && keyCode == keypadInsertKeyCode );
}

//////////////////////////////////////////////////////////////////////////

RecorderCore::RecorderCore( RecorderSession * session_, QObject * parent ):
  QObject( parent ),
  session( session_ ),
  secondKeyTimer( new QTimer( this ) )
{
  secondKeyTimer->setSingleShot( true );
  secondKeyTimer->setInterval( RecorderMatcher::secondKeyTimeoutMs );
  connect( secondKeyTimer, &QTimer::timeout, this, &RecorderCore::secondKeyWaitTimedOut );

  // The session must already be running: the keymap is what these codes come from.
  if ( session )
  {
    // The core is the session's event sink by construction: recorded transitions
    // are posted to it, and nothing else has to remember to wire the two together.
    session->setEventSink( this );
    recorderMatcher.setModifierKeyCodes( session->modifierKeyCodes() );
    recorderMatcher.setCopyKeyCodes( session->copyKeyCode(),
                                     session->insertKeyCode(),
                                     session->keypadInsertKeyCode() );
  }
}

RecorderCore::~RecorderCore()
{
  if ( session && session->isEventSink( this ) )
    session->setEventSink( nullptr );
}

void RecorderCore::addBinding( quint32 key, quint32 key2, quint32 modifiers, int handle )
{
  recorderMatcher.addBinding( key, key2, modifiers, handle );

  // Make sure the key does not get caught by other applications, except for the
  // clipboard copy key, whose press must still reach the application below.
  if ( !recorderMatcher.isCopyToClipboardKey( key, modifiers ) )
    grab( key, modifiers );
}

void RecorderCore::clear()
{
  endSecondKeyWait();

  QSet< QPair< quint32, quint32 > > const grabs = grabbedKeys;
  grabbedKeys.clear();

  for( QSet< QPair< quint32, quint32 > >::const_iterator i = grabs.constBegin();
       i != grabs.constEnd(); ++i )
    ungrab( i->first, i->second );

  recorderMatcher.clearBindings();
}

int RecorderCore::bindingCount() const noexcept
{
  return recorderMatcher.bindings().size();
}

bool RecorderCore::isGrabbed( quint32 keyCode, quint32 modifiers ) const noexcept
{
  return grabbedKeys.contains( qMakePair( keyCode, modifiers ) );
}

RecorderMatcher const & RecorderCore::matcher() const noexcept
{
  return recorderMatcher;
}

void RecorderCore::cancelSecondKeyWait()
{
  endSecondKeyWait();
}

void RecorderCore::recordedKey( bool pressed, quint32 keyCode )
{
  if ( !pressed )
  {
    if ( recorderMatcher.isModifierKeyCode( keyCode ) )
      recorderMatcher.modifierReleased( keyCode );
    return;
  }

  if ( recorderMatcher.isModifierKeyCode( keyCode ) )
  {
    recorderMatcher.modifierPressed( keyCode );
    return;
  }

  // Both Insert keys drive the same combination.
  if ( session && keyCode == session->keypadInsertKeyCode() )
    keyCode = session->insertKeyCode();

  // A press ends the *effects* of an outstanding second-key wait (its grab and its
  // timer) before the combination is matched: the replaced code cancelled its
  // 2nd-key stage at exactly that point. Whether the awaited combination itself
  // completed is decided by the matcher, which drops the wait either way.
  releaseSecondKeyGrab();

  RecorderMatcher::Decision const decision = recorderMatcher.press( keyCode, recorderMatcher.modifiers() );

  switch ( decision.kind )
  {
    case RecorderMatcher::Kind::Ignored:
      break;

    case RecorderMatcher::Kind::Activated:
      emit activated( decision.handle );
      break;

    case RecorderMatcher::Kind::SecondKeyWait:
      if ( decision.grabSecondKey && !isGrabbed( decision.secondKey, decision.secondKeyModifiers ) )
      {
        waiter.keyCode = decision.secondKey;
        waiter.modifiers = decision.secondKeyModifiers;
        waiter.grabbed = true;
        grab( waiter.keyCode, waiter.modifiers );
      }
      // One 500 ms window per first-key press: a wait that starts later is not
      // cut short by an earlier one.
      secondKeyTimer->start();
      break;
  }
}

void RecorderCore::postRecordedKey( bool pressed, quint32 keyCode )
{
  if ( QThread::currentThread() == thread() )
  {
    recordedKey( pressed, keyCode );
    return;
  }

  // The recording thread never runs the matcher: the second-key timer lives on
  // this object's thread, exactly where the replaced slot-based code ran. The
  // queued invocation is tied to this object, so an event that arrives during
  // teardown is dropped instead of touching a half-destroyed recorder.
  QMetaObject::invokeMethod( this,
                             [ this, pressed, keyCode ]() { recordedKey( pressed, keyCode ); },
                             Qt::QueuedConnection );
}

void RecorderCore::secondKeyWaitTimedOut()
{
  endSecondKeyWait();
}

void RecorderCore::endSecondKeyWait()
{
  recorderMatcher.cancelSecondKeyWait();
  releaseSecondKeyGrab();
}

void RecorderCore::releaseSecondKeyGrab()
{
  if ( waiter.grabbed )
  {
    grabbedKeys.remove( qMakePair( waiter.keyCode, waiter.modifiers ) );
    ungrab( waiter.keyCode, waiter.modifiers );
  }

  waiter = Waiter();
  secondKeyTimer->stop();
}

void RecorderCore::grab( quint32 keyCode, quint32 modifiers )
{
  if ( !session || keyCode == 0 )
    return;

  if ( session->grab( keyCode, modifiers ) )
    grabbedKeys.insert( qMakePair( keyCode, modifiers ) );
}

void RecorderCore::ungrab( quint32 keyCode, quint32 modifiers )
{
  if ( !session )
    return;

  session->ungrab( keyCode, modifiers );
  grabbedKeys.remove( qMakePair( keyCode, modifiers ) );
}

//////////////////////////////////////////////////////////////////////////

#ifdef HAVE_X11

namespace {

/// Releases a grab/ungrab error as a return value instead of letting it reach the
/// default Xlib handler, as the replaced code did. Xlib reports the failure
/// asynchronously, so the flush in isError() is what makes it observable.
class X11GrabUngrabErrorHandler
{
public:
  explicit X11GrabUngrabErrorHandler( Display * display_ ): display( display_ )
  {
    error = false;
    previousErrorHandler = XSetErrorHandler( grabUngrabErrorHandler );
  }

  ~X11GrabUngrabErrorHandler()
  {
    XFlush( display );
    XSetErrorHandler( previousErrorHandler );
  }

  X11GrabUngrabErrorHandler( X11GrabUngrabErrorHandler const & ) = delete;
  X11GrabUngrabErrorHandler & operator=( X11GrabUngrabErrorHandler const & ) = delete;

  bool isError() const
  {
    XFlush( display );
    return error;
  }

private:
  static int grabUngrabErrorHandler( Display *, XErrorEvent * event )
  {
    switch ( event->error_code )
    {
      case BadAccess:
      case BadValue:
      case BadWindow:
        if ( event->request_code == 33 /* X_GrabKey */ ||
             event->request_code == 34 /* X_UngrabKey */ )
          error = true;
        break;
      default:
        break;
    }
    return 0;
  }

  static bool error;

  Display * display;
  int ( *previousErrorHandler )( Display *, XErrorEvent * );
};

bool X11GrabUngrabErrorHandler::error = false;

void setError( QString * error, QString const & text )
{
  if ( error )
    *error = text;
}

/// One modifier token of the portal's preferred_trigger format and the recorder
/// mask it sets.
struct TriggerModifier
{
  char const * token;
  quint32 mask;
};

TriggerModifier const triggerModifiers[] = {
  { "CTRL", RecorderControlMask },
  { "ALT", RecorderAltMask },
  { "SHIFT", RecorderShiftMask },
  { "LOGO", RecorderMetaMask },
};

/// Splits a preferred_trigger ("CTRL+ALT+g") into the modifier mask and the X
/// keysym name of its key. A token this recorder does not know, an empty token
/// and a trigger without a key are reported instead of guessed: the stored value
/// of an unbindable shortcut is never rewritten.
bool splitTrigger( QString const & trigger, quint32 * modifiers, QString * keyName,
                   QString * reason )
{
  QStringList const parts = trigger.split( QLatin1Char( '+' ), Qt::KeepEmptyParts );

  if ( parts.isEmpty() )
  {
    setError( reason, QObject::tr( "The sequence is empty." ) );
    return false;
  }

  quint32 mask = 0;

  for( int i = 0; i + 1 < parts.size(); ++i )
  {
    bool known = false;

    for( TriggerModifier const & entry : triggerModifiers )
    {
      if ( parts.at( i ).compare( QLatin1String( entry.token ), Qt::CaseInsensitive ) == 0 )
      {
        mask |= entry.mask;
        known = true;
        break;
      }
    }

    if ( !known )
    {
      setError( reason, QObject::tr( "\"%1\" is not a modifier this recorder can bind." )
                          .arg( parts.at( i ) ) );
      return false;
    }
  }

  QString const key = parts.last();

  if ( key.isEmpty() )
  {
    setError( reason, QObject::tr( "The sequence names no key." ) );
    return false;
  }

  *modifiers = mask;
  *keyName = key;
  return true;
}


/// The X11 RECORD session of the running session.
///
/// It borrows the Xlib display Qt already opened for the keymap and the grabs,
/// and owns one further display for the blocking recording loop: the loop is a
/// thread of its own because XRecordEnableContext does not return until the
/// context is disabled, and it must never block the GUI thread or share its
/// connection. Nothing here opens a display when the session is not an X11 one —
/// createRecorderSession() asks PlatformWindow first.
class X11RecordSession : public RecorderSession
{
public:
  X11RecordSession() = default;
  ~X11RecordSession() override;

  bool start( QString * error ) override;
  RecorderSession::Shutdown stop() override;

  quint32 keyCodeForQtKey( int key ) override;
  quint32 keyCodeForKeysymName( QString const & name ) override;

  RecorderModifierKeycodes modifierKeyCodes() const override;
  quint32 copyKeyCode() const override;
  quint32 insertKeyCode() const override;
  quint32 keypadInsertKeyCode() const override;

  bool grab( quint32 keyCode, quint32 modifiers ) override;
  void ungrab( quint32 keyCode, quint32 modifiers ) override;
  void ungrabAll() override;

  void setEventSink( RecorderEventSink * sink ) override;
  bool isEventSink( RecorderEventSink const * sink ) const override;

private:
  static void recordEventCallback( XPointer ptr, XRecordInterceptData * data );
  void handleRecordData( XRecordInterceptData * data );
  void run(); // The blocking recording loop, on its own thread.

  Display * display = nullptr;     // Borrowed from Qt: keycodes and grabbing.
  Display * dataDisplay = nullptr; // Owned: only the recording loop touches it.
  XRecordRange * recordRange = nullptr;
  XRecordContext recordContext = 0;
  XRecordClientSpec recordClientSpec = XRecordAllClients;
  // The recording thread reads this while the GUI thread detaches it, so the
  // hand-off is atomic: a plain pointer would let the worker load it before the
  // detach and call through a sink that has already been destroyed.
  std::atomic< RecorderEventSink * > eventSink{ nullptr };
  std::thread recordingThread;
  std::atomic< bool > recordingFinished{ true };

  RecorderModifierKeycodes modifierKeys;
  quint32 cCode = 0;
  quint32 insertCode = 0;
  quint32 keypadInsertCode = 0;

  QSet< QPair< quint32, quint32 > > grabs;
};

X11RecordSession::~X11RecordSession()
{
  stop();
}

bool X11RecordSession::start( QString * error )
{
  display = PlatformWindow::x11Display();

  if ( !display )
  {
    setError( error, QObject::tr( "This session has no X11 display to record from." ) );
    return false;
  }

  modifierKeys.leftShift = XKeysymToKeycode( display, XK_Shift_L );
  modifierKeys.rightShift = XKeysymToKeycode( display, XK_Shift_R );
  modifierKeys.leftControl = XKeysymToKeycode( display, XK_Control_L );
  modifierKeys.rightControl = XKeysymToKeycode( display, XK_Control_R );
  modifierKeys.leftAlt = XKeysymToKeycode( display, XK_Alt_L );
  modifierKeys.rightAlt = XKeysymToKeycode( display, XK_Alt_R );
  modifierKeys.leftMeta = XKeysymToKeycode( display, XK_Super_L );
  modifierKeys.rightMeta = XKeysymToKeycode( display, XK_Super_R );

  cCode = XKeysymToKeycode( display, XK_c );
  insertCode = XKeysymToKeycode( display, XK_Insert );
  keypadInsertCode = XKeysymToKeycode( display, XK_KP_Insert );

  // We use RECORD instead of XGrabKey, because XGrabKey prevents other clients
  // from getting their input if it is grabbed.
  dataDisplay = XOpenDisplay( nullptr );

  if ( !dataDisplay )
  {
    setError( error, QObject::tr( "The X11 recording display could not be opened." ) );
    return false;
  }

  recordRange = XRecordAllocRange();

  if ( !recordRange )
  {
    XCloseDisplay( dataDisplay );
    dataDisplay = nullptr;
    setError( error, QObject::tr( "The X11 RECORD range could not be allocated." ) );
    return false;
  }

  recordRange->device_events.first = KeyPress;
  recordRange->device_events.last = KeyRelease;

  recordContext = XRecordCreateContext( display, 0, &recordClientSpec, 1, &recordRange, 1 );

  if ( !recordContext )
  {
    XFree( recordRange );
    recordRange = nullptr;
    XCloseDisplay( dataDisplay );
    dataDisplay = nullptr;
    setError( error, QObject::tr( "The X server has no usable RECORD extension." ) );
    return false;
  }

  // This is required to ensure the context was indeed created.
  XSync( display, False );

  recordingFinished = false;
  recordingThread = std::thread( [ this ]() { run(); } );

  return true;
}

RecorderSession::Shutdown X11RecordSession::stop()
{
  if ( recordContext && display )
  {
    // XRecordEnableContext returns only once the context is disabled, but it runs
    // on the recording thread while the disable is issued here, on another
    // connection. A disable that reaches the server before it has processed the
    // enable is dropped, so it is repeated while the recording thread is still
    // inside the blocking call. The bound keeps a wedged server from blocking
    // teardown forever; in practice the enable has been processed long before the
    // recorder is stopped.
    QElapsedTimer shutdownDeadline;
    shutdownDeadline.start();

    while ( !recordingFinished.load() && shutdownDeadline.elapsed() < 5000 )
    {
      XRecordDisableContext( display, recordContext );
      XSync( display, False );
      std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
    }
  }

  if ( recordingThread.joinable() )
  {
    if ( !HotkeyRuntime::recordingLoopReturnedInTime( recordingFinished.load() ) )
    {
      // The blocking XRecordEnableContext has not returned even after the
      // repeated disables, and XSync above is itself a blocking round trip, so
      // joining here would hang the process at shutdown. The thread is detached
      // and reported as abandoned; the caller keeps this session and the core it
      // posts to alive, because the worker may still be inside a callback.
      recordingThread.detach();
      return Shutdown::Abandoned;
    }

    recordingThread.join();
  }

  if ( recordContext && display )
  {
    XRecordFreeContext( display, recordContext );
    recordContext = 0;
  }

  if ( recordRange )
  {
    XFree( recordRange );
    recordRange = nullptr;
  }

  if ( dataDisplay )
  {
    XCloseDisplay( dataDisplay );
    dataDisplay = nullptr;
  }

  ungrabAll();

  display = nullptr;
  eventSink.store( nullptr );

  return Shutdown::Joined;
}

quint32 X11RecordSession::keyCodeForQtKey( int key )
{
  if ( !display )
    return 0;

  QString name;

  switch ( key )
  {
    case Qt::Key_Insert:
      // Qt's portable name of this key is not the X keysym name.
      name = QStringLiteral( "Insert" );
      break;
    default:
      name = QKeySequence( int( key ) ).toString();
      break;
  }

  return keyCodeForKeysymName( name );
}

quint32 X11RecordSession::keyCodeForKeysymName( QString const & name )
{
  if ( !display || name.isEmpty() )
    return 0;

  KeySym const keySym = XStringToKeysym( name.toLatin1().data() );

  if ( keySym == NoSymbol )
    return 0;

  return XKeysymToKeycode( display, keySym );
}

RecorderModifierKeycodes X11RecordSession::modifierKeyCodes() const
{
  return modifierKeys;
}

quint32 X11RecordSession::copyKeyCode() const
{
  return cCode;
}

quint32 X11RecordSession::insertKeyCode() const
{
  return insertCode;
}

quint32 X11RecordSession::keypadInsertKeyCode() const
{
  return keypadInsertCode;
}

bool X11RecordSession::grab( quint32 keyCode, quint32 modifiers )
{
  if ( !display || keyCode == 0 )
    return false;

  QPair< quint32, quint32 > const combination = qMakePair( keyCode, modifiers );

  if ( grabs.contains( combination ) )
    return true;

  {
    X11GrabUngrabErrorHandler errorHandler( display );

    XGrabKey( display, keyCode, modifiers, DefaultRootWindow( display ),
              True, GrabModeAsync, GrabModeAsync );

    if ( errorHandler.isError() )
    {
      gdWarning( "Possible hotkeys conflict. Check your hotkeys options." );
      // The server may have left a partial grab behind: release it.
      X11GrabUngrabErrorHandler ungrabHandler( display );
      XUngrabKey( display, keyCode, modifiers, DefaultRootWindow( display ) );
      return false;
    }
  }

  grabs.insert( combination );
  return true;
}

void X11RecordSession::ungrab( quint32 keyCode, quint32 modifiers )
{
  if ( !display || keyCode == 0 )
    return;

  grabs.remove( qMakePair( keyCode, modifiers ) );

  X11GrabUngrabErrorHandler errorHandler( display );
  XUngrabKey( display, keyCode, modifiers, DefaultRootWindow( display ) );

  if ( errorHandler.isError() )
    gdWarning( "Cannot ungrab the hotkey" );
}

void X11RecordSession::ungrabAll()
{
  if ( !display )
  {
    grabs.clear();
    return;
  }

  QSet< QPair< quint32, quint32 > > const remaining = grabs;
  grabs.clear();

  for( QSet< QPair< quint32, quint32 > >::const_iterator i = remaining.constBegin();
       i != remaining.constEnd(); ++i )
  {
    X11GrabUngrabErrorHandler errorHandler( display );
    XUngrabKey( display, i->first, i->second, DefaultRootWindow( display ) );

    if ( errorHandler.isError() )
      gdWarning( "Cannot ungrab the hotkey" );
  }
}

void X11RecordSession::setEventSink( RecorderEventSink * sink )
{
  eventSink.store( sink );
}

bool X11RecordSession::isEventSink( RecorderEventSink const * sink ) const
{
  return eventSink.load() == sink;
}

void X11RecordSession::run()
{
  bool const enabled = XRecordEnableContext( dataDisplay, recordContext, recordEventCallback,
                                             reinterpret_cast< XPointer >( this ) );

  // Recorded before the thread ends: stop() waits for this flag instead of relying
  // on the disable having been seen by the server on the first attempt.
  recordingFinished = true;

  if ( !enabled )
    GD_DPRINTF( "Failed to enable record context\n" );
}

void X11RecordSession::recordEventCallback( XPointer ptr, XRecordInterceptData * data )
{
  reinterpret_cast< X11RecordSession * >( ptr )->handleRecordData( data );
}

void X11RecordSession::handleRecordData( XRecordInterceptData * data )
{
  if ( data->category == XRecordFromServer && data->data )
  {
    xEvent const * event = reinterpret_cast< xEvent const * >( data->data );

    // The recorded detail is the session's keycode: the matcher translates nothing
    // here, it only reports the transition.
    if ( event->u.u.type == KeyPress )
    {
      if ( RecorderEventSink * const sink = eventSink.load() )
        sink->postRecordedKey( true, event->u.u.detail );
    }
    else if ( event->u.u.type == KeyRelease )
    {
      if ( RecorderEventSink * const sink = eventSink.load() )
        sink->postRecordedKey( false, event->u.u.detail );
    }
  }

  XRecordFreeData( data );
}

} // namespace

namespace {
/// Sessions whose blocking recorder never returned. They are deliberately never
/// freed: the detached thread still reads them, and the alternative is a hang at
/// shutdown or a use-after-free. The list is bounded by the number of recorders a
/// process starts, which is one per wrapper.
std::vector< std::pair< std::unique_ptr< RecorderSession >,
                        std::unique_ptr< RecorderCore > > > & abandonedSessions()
{
  // Deliberately leaked: a parked recorder can still be reached by its detached
  // worker, so the holder must never be destroyed, not even at process exit.
  static auto * const parked =
    new std::vector< std::pair< std::unique_ptr< RecorderSession >,
                                std::unique_ptr< RecorderCore > > >();
  return *parked;
}
} // namespace

void parkAbandonedRecorder( std::unique_ptr< RecorderSession > session,
                            std::unique_ptr< RecorderCore > core )
{
  if( core )
  {
    // The core was constructed as a QObject child of its backend/wrapper, and a
    // parent destroys its children regardless of who else owns them. Detach it
    // first, or destroying the owner would free the object the detached worker
    // may still be inside.
    core->setParent( nullptr );
  }

  abandonedSessions().push_back( { std::move( session ), std::move( core ) } );
}

std::unique_ptr< RecorderSession > createRecorderSession()
{
  // HAVE_X11 only says this build can talk to Xlib. The active QPA platform
  // decides whether there is a display to borrow, and a Wayland or offscreen
  // session never reaches an Xlib call through here.
  if ( !HotkeyRuntime::isX11Session( QGuiApplication::platformName() )
       || !PlatformWindow::x11Display() )
    return std::unique_ptr< RecorderSession >();

  return std::unique_ptr< RecorderSession >( new X11RecordSession );
}

#else // !HAVE_X11

std::unique_ptr< RecorderSession > createRecorderSession()
{
  return std::unique_ptr< RecorderSession >();
}

#endif // HAVE_X11

//////////////////////////////////////////////////////////////////////////

#if defined( HAVE_X11 ) && !defined( Q_OS_WIN ) && !defined( Q_OS_MAC )

X11RecordBackend::X11RecordBackend( QObject * parent ):
  HotkeyBackend( parent )
{
  if ( isSupported() )
  {
    currentState = State::Ready;
  }
  else
  {
    currentState = State::Unsupported;
    currentExplanation = tr( "Global hotkeys need an X11 session; this session runs on \"%1\"." )
                           .arg( QGuiApplication::platformName() );
  }
}

X11RecordBackend::~X11RecordBackend()
{
  retire();
}

bool X11RecordBackend::isSupported() const noexcept
{
  // HAVE_X11 is a build capability, never evidence of an active X11 session: the
  // active QPA platform and the display it lends decide.
  return HotkeyRuntime::isX11Session( QGuiApplication::platformName() )
         && PlatformWindow::x11Display() != nullptr;
}

void X11RecordBackend::bind( QVector< HotkeyBinding > const & bindings )
{
  retire();

  if ( !isSupported() )
  {
    setState( State::Unsupported,
              tr( "Global hotkeys need an X11 session; this session runs on \"%1\"." )
                .arg( QGuiApplication::platformName() ) );
    return;
  }

  QString error;
  session = createRecorderSession();

  if ( !session || !session->start( &error ) )
  {
    session.reset();
    setState( State::Unavailable,
              error.isEmpty() ? tr( "The X11 recorder could not be started." ) : error );
    return;
  }

  core.reset( new RecorderCore( session.get(), this ) );
  connect( core.get(), &RecorderCore::activated, this, &X11RecordBackend::recorderActivated );

  QStringList failures;
  int boundCount = 0;

  for( HotkeyBinding const & binding : bindings )
  {
    QString reason;

    if ( bindOne( binding, &reason ) )
    {
      ++boundCount;
      continue;
    }

    // An unrepresentable sequence is reported, never silently dropped and never
    // rewritten into something else.
    failures << tr( "\"%1\" (%2): %3" )
                    .arg( binding.id, binding.sequence, reason );
  }

  if ( boundCount == 0 )
  {
    setState( State::Unavailable,
              failures.isEmpty() ? tr( "No combination was requested." )
                                 : tr( "No combination could be bound. %1" ).arg( failures.join( QLatin1Char( ' ' ) ) ) );
    return;
  }

  if ( !failures.isEmpty() )
  {
    setState( State::Partial, tr( "%1 of %2 combinations are bound. %3" )
                                .arg( QString::number( boundCount ),
                                      QString::number( bindings.size() ),
                                      failures.join( QLatin1Char( ' ' ) ) ) );
    return;
  }

  setState( State::Bound, QString() );
}

void X11RecordBackend::unbind()
{
  bool const wasRegistered = session != nullptr;

  retire();

  if ( wasRegistered )
    setState( State::Closed, tr( "The X11 recorder was retired and its combinations released." ) );
}

HotkeyBackend::State X11RecordBackend::state() const noexcept
{
  return currentState;
}

QString X11RecordBackend::explanation() const
{
  return currentExplanation;
}

quint32 X11RecordBackend::keyCodeForKey( int key )
{
  return session ? session->keyCodeForQtKey( key ) : 0;
}

void X11RecordBackend::recordedKey( bool pressed, quint32 keyCode )
{
  if ( core )
    core->recordedKey( pressed, keyCode );
}

int X11RecordBackend::handleForId( QString const & id ) const
{
  for( QPair< QString, int > const & entry : bound )
  {
    if ( entry.first == id )
      return entry.second;
  }

  return 0;
}

void X11RecordBackend::recorderActivated( int handle )
{
  for( QPair< QString, int > const & entry : bound )
  {
    if ( entry.second != handle )
      continue;

    // X11 has no XDG activation token: the token is empty, never invented.
    emit activated( entry.first, QString() );
    return;
  }
}

bool X11RecordBackend::bindOne( HotkeyBinding const & binding, QString * reason )
{
  if ( !core || !session )
  {
    setError( reason, tr( "The recorder is not running." ) );
    return false;
  }

  // A stored X11 combination may be a single trigger or the legacy two-combination
  // form ("Ctrl+F11,F11", "Ctrl+C,C"): the first combination carries the modifiers and
  // the second names the key that completes the chord.
  QStringList const combinations =
    binding.sequence.split( QLatin1Char( ',' ), Qt::SkipEmptyParts );
  QStringList triggers;
  for( QString const & combination : combinations )
  {
    QString const trimmed = combination.trimmed();
    if( !trimmed.isEmpty() )
      triggers.append( trimmed );
  }

  if( triggers.isEmpty() || triggers.size() > 2 )
  {
    setError( reason, tr( "Only a one- or two-combination sequence can be bound." ) );
    return false;
  }

  quint32 modifiers = 0;
  QString keyName;

  if ( !splitTrigger( triggers.first(), &modifiers, &keyName, reason ) )
    return false;

  quint32 const keyCode = session->keyCodeForKeysymName( keyName );

  if ( keyCode == 0 )
  {
    setError( reason, tr( "This keymap has no key named \"%1\"." ).arg( keyName ) );
    return false;
  }

  quint32 secondKeyCode = 0;

  if( triggers.size() == 2 )
  {
    quint32 secondModifiers = 0;
    QString secondName;

    if ( !splitTrigger( triggers.at( 1 ), &secondModifiers, &secondName, reason ) )
      return false;

    // The legacy form keeps the second combination's modifiers implicit: the recorder
    // only tracks the key that completes the chord.
    if ( secondModifiers != 0 && secondModifiers != modifiers )
    {
      setError( reason, tr( "The second combination must reuse the first one's modifiers." ) );
      return false;
    }

    secondKeyCode = session->keyCodeForKeysymName( secondName );

    if ( secondKeyCode == 0 )
    {
      setError( reason, tr( "This keymap has no key named \"%1\"." ).arg( secondName ) );
      return false;
    }
  }

  // Stable IDs map to recorder handles: the handle is only handed to the matcher,
  // the ID is what an activation reports.
  int const handle = nextHandle++;
  core->addBinding( keyCode, secondKeyCode, modifiers, handle );
  bound.append( qMakePair( binding.id, handle ) );

  return true;
}

void X11RecordBackend::retire()
{
  // Order matters. The grabs belong to the session, so the core releases them
  // first, while the session is still alive; then the sink is detached so the
  // recording thread can no longer reach the core; only then is the recorder
  // stopped (joined, or abandoned and parked) and the core destroyed.
  if ( core )
    core->clear();

  if ( session )
  {
    session->setEventSink( nullptr );

    std::unique_ptr< RecorderSession > stopping = std::move( session );
    if( stopping->stop() == RecorderSession::Shutdown::Abandoned )
    {
      // The worker may still be inside a callback that captured the sink, so the
      // core must stay alive with the session it posts to; both are parked.
      parkAbandonedRecorder( std::move( stopping ), std::move( core ) );
    }
    else
    {
      // The worker is joined: no callback can be in flight, so the core can go.
      core.reset();
    }
  }
  else
    core.reset();

  // The handles are never reused: an activation that carries the handle of a
  // retired registration would otherwise report whatever was bound to it next.
  bound.clear();
}

void X11RecordBackend::setState( State state, QString const & explanation )
{
  currentState = state;
  currentExplanation = explanation;
  emit stateChanged( state, explanation );
}

#endif // HAVE_X11 && !Q_OS_WIN && !Q_OS_MAC

//////////////////////////////////////////////////////////////////////////

#if !defined( Q_OS_MAC )

HotkeyWrapper::HotkeyWrapper(QObject *parent) : QThread( parent ),
    state2(false)
{
#ifdef Q_OS_WIN
  hwnd=(HWND)((static_cast<QMainWindow*>(parent))->winId());

  dllHandler.hDLLHandle = LoadLibraryA( "GdHotkeys.dll" );
  if( dllHandler.hDLLHandle )
  {
    dllHandler.setHook = ( setHookProc )GetProcAddress( dllHandler.hDLLHandle, "setHook" );
    dllHandler.removeHook = ( removeHookProc )GetProcAddress( dllHandler.hDLLHandle, "removeHook" );
    dllHandler.setHotkeys = ( setHotkeysProc )GetProcAddress( dllHandler.hDLLHandle, "setHotkeys" );
    dllHandler.clearHotkeys = ( clearHotkeysProc )GetProcAddress( dllHandler.hDLLHandle, "clearHotkeys" );

    if( !dllHandler.setHook || !dllHandler.removeHook || !dllHandler.setHotkeys || !dllHandler.clearHotkeys )
    {
      FreeLibrary( dllHandler.hDLLHandle );
      dllHandler.hDLLHandle = 0;
    }
  }

  if( dllHandler.hDLLHandle )
    gdWarning( "Handle global hotkeys via GdHotkeys.dll" );
  else
    gdWarning( "Handle global hotkeys via RegisterHotkey()" );

#else
  init();
#endif

  // The wrapper used to assume the application object; a plain QApplication (a
  // test binary, for one) simply does not collect wrappers.
  if( QHotkeyApplication * app = qobject_cast< QHotkeyApplication * >( qApp ) )
    app->registerWrapper(this);
}

HotkeyWrapper::~HotkeyWrapper()
{
  unregister();
#ifdef Q_OS_WIN32
  if( dllHandler.hDLLHandle )
    FreeLibrary( dllHandler.hDLLHandle );
#endif
}

#endif // !Q_OS_MAC

//////////////////////////////////////////////////////////////////////////

#ifdef Q_OS_WIN

void HotkeyWrapper::waitKey2()
{
  state2 = false;
}

bool HotkeyWrapper::checkState(quint32 vk, quint32 mod)
{
  if ( state2 )
  { // wait for 2nd key

    waitKey2(); // Cancel the 2nd-key wait stage

    if (state2waiter.key2 == vk && state2waiter.modifier == mod)
    {
      emit hotkeyActivated( state2waiter.handle );
      return true;
    }
  }

  for (int i = 0; i < hotkeys.count(); i++) {

    const HotkeyStruct &hs = hotkeys.at(i);

    if (hs.key == vk && hs.modifier == mod) {

      if( hs.key2 != 0 || ( mod == MOD_CONTROL && ( vk == VK_INSERT || vk == 'c' || vk == 'C' ) ) )
      {
        // Pass-through first part of compound hotkey or clipdoard copy command

        INPUT i[ 10 ];
        memset( i, 0, sizeof( i ) );
        int nextKeyNom = 0;
        short emulateModKeys = 0;

        // If modifier keys aren't pressed it looks like emulation
        // We then also emulate full sequence

        if( ( mod & MOD_ALT ) != 0 && ( GetAsyncKeyState( VK_MENU ) & 0x8000 ) == 0 )
        {
          emulateModKeys |= MOD_ALT;
          i[ nextKeyNom ].type = INPUT_KEYBOARD;
          i[ nextKeyNom ].ki.wVk = VK_MENU;
          nextKeyNom += 1;
        }
        if( ( mod & MOD_CONTROL ) != 0 && ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) == 0 )
        {
          emulateModKeys |= MOD_CONTROL;
          i[ nextKeyNom ].type = INPUT_KEYBOARD;
          i[ nextKeyNom ].ki.wVk = VK_CONTROL;
          nextKeyNom += 1;
        }
        if( ( mod & MOD_SHIFT ) != 0 && ( GetAsyncKeyState( VK_SHIFT ) & 0x8000 ) == 0 )
        {
          emulateModKeys |= MOD_SHIFT;
          i[ nextKeyNom ].type = INPUT_KEYBOARD;
          i[ nextKeyNom ].ki.wVk = VK_SHIFT;
          nextKeyNom += 1;
        }
        if( ( mod & MOD_WIN ) != 0 && ( GetAsyncKeyState( VK_LWIN ) & 0x8000 ) == 0
            && ( GetAsyncKeyState( VK_RWIN ) & 0x8000 ) == 0 )
        {
          emulateModKeys |= MOD_WIN;
          i[ nextKeyNom ].type = INPUT_KEYBOARD;
          i[ nextKeyNom ].ki.wVk = VK_LWIN;
          nextKeyNom += 1;
        }

        i[ nextKeyNom ].type = INPUT_KEYBOARD;
        i[ nextKeyNom ].ki.wVk = vk;
        nextKeyNom += 1;
        i[ nextKeyNom ].type = INPUT_KEYBOARD;
        i[ nextKeyNom ].ki.wVk = vk;
        i[ nextKeyNom ].ki.dwFlags = KEYEVENTF_KEYUP;
        nextKeyNom += 1;

        if( emulateModKeys & MOD_WIN )
        {
          i[ nextKeyNom ].type = INPUT_KEYBOARD;
          i[ nextKeyNom ].ki.wVk = VK_LWIN;
          i[ nextKeyNom ].ki.dwFlags = KEYEVENTF_KEYUP;
          nextKeyNom += 1;
        }
        if( emulateModKeys & MOD_SHIFT )
        {
          i[ nextKeyNom ].type = INPUT_KEYBOARD;
          i[ nextKeyNom ].ki.wVk = VK_SHIFT;
          i[ nextKeyNom ].ki.dwFlags = KEYEVENTF_KEYUP;
          nextKeyNom += 1;
        }
        if( emulateModKeys & MOD_CONTROL )
        {
          i[ nextKeyNom ].type = INPUT_KEYBOARD;
          i[ nextKeyNom ].ki.wVk = VK_CONTROL;
          i[ nextKeyNom ].ki.dwFlags = KEYEVENTF_KEYUP;
          nextKeyNom += 1;
        }
        if( emulateModKeys & MOD_ALT )
        {
          i[ nextKeyNom ].type = INPUT_KEYBOARD;
          i[ nextKeyNom ].ki.wVk = VK_MENU;
          i[ nextKeyNom ].ki.dwFlags = KEYEVENTF_KEYUP;
          nextKeyNom += 1;
        }

        UnregisterHotKey( hwnd, hs.id );
        SendInput( nextKeyNom, i, sizeof( *i ) );
        RegisterHotKey(hwnd, hs.id, hs.modifier, hs.key);
      }

      if (hs.key2 == 0) {
         emit hotkeyActivated( hs.handle );
         return true;
      }

      state2 = true;
      state2waiter = hs;
      QTimer::singleShot( RecorderMatcher::secondKeyTimeoutMs, this, SLOT(waitKey2()) );

      return true;
    }
  }

  state2 = false;
  return false;
}

void HotkeyWrapper::init()
{
  QWidget *root = qApp->topLevelWidgets().value(0);
  hwnd = (HWND)root->winId();
}

bool HotkeyWrapper::setGlobalKey( int key, int key2,
                                  Qt::KeyboardModifiers modifier, int handle )
{
  if ( !key )
    return false; // We don't monitor empty combinations

  static int id = 0;
  if( id > 0xBFFF - 1 )
    id = 0;

  quint32 mod = 0;
  if (modifier & Qt::CTRL)
    mod |= MOD_CONTROL;
  if (modifier & Qt::ALT)
    mod |= MOD_ALT;
  if (modifier & Qt::SHIFT)
    mod |= MOD_SHIFT;
  if (modifier & Qt::META)
    mod |= MOD_WIN;

  quint32 vk = nativeKey( key );
  quint32 vk2 = key2 ? nativeKey( key2 ) : 0;

  hotkeys.append( HotkeyStruct( vk, vk2, mod, handle, id ) );

  if( dllHandler.hDLLHandle )
  {
    dllHandler.removeHook();
    dllHandler.setHotkeys( vk, vk2, mod, hotkeys.size() - 1 );
    dllHandler.setHook( hwnd );
    return true;
  }

  if (!RegisterHotKey(hwnd, id++, mod, vk))
    return false;

  if ( key2 && key2 != key )
    return RegisterHotKey(hwnd, id++, mod, vk2);

  return true;
}

bool HotkeyWrapper::winEvent ( MSG * message, long * result )
{
  (void) result;
  if (message->message == WM_HOTKEY)
    return checkState( (message->lParam >> 16), (message->lParam & 0xffff) );

  if( message->message == GD_HOTKEY_MESSAGE )
  {
    int n = (int)message->wParam;
    if( n < hotkeys.size() && n >= 0 )
      emit hotkeyActivated( hotkeys.at( n ).handle );
    return true;
  }

  return false;
}

quint32 HotkeyWrapper::nativeKey(int key)
{
  if (key >= Qt::Key_0 && key <= Qt::Key_9)
    return VK_NUMPAD0 + ( key - Qt::Key_0 );

  if (key >= Qt::Key_A && key <= Qt::Key_Z)
    return key;

  switch (key) {
    case Qt::Key_Space:     return VK_SPACE;
    case Qt::Key_Asterisk:  return VK_MULTIPLY;
    case Qt::Key_Plus:      return VK_ADD;
    case Qt::Key_Comma:     return VK_SEPARATOR;
    case Qt::Key_Minus:     return VK_SUBTRACT;
    case Qt::Key_Slash:     return VK_DIVIDE;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:   return VK_TAB;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Return:
    case Qt::Key_Escape:    return VK_ESCAPE;
    case Qt::Key_Enter:     return VK_RETURN;
    case Qt::Key_Insert:    return VK_INSERT;
    case Qt::Key_Delete:    return VK_DELETE;
    case Qt::Key_Pause:     return VK_PAUSE;
    case Qt::Key_Print:     return VK_PRINT;
    case Qt::Key_Clear:     return VK_CLEAR;
    case Qt::Key_Home:      return VK_HOME;
    case Qt::Key_End:       return VK_END;
    case Qt::Key_Up:        return VK_UP;
    case Qt::Key_Down:      return VK_DOWN;
    case Qt::Key_Left:      return VK_LEFT;
    case Qt::Key_Right:     return VK_RIGHT;
    case Qt::Key_PageUp:    return VK_PRIOR;
    case Qt::Key_PageDown:  return VK_NEXT;
    case Qt::Key_F1:        return VK_F1;
    case Qt::Key_F2:        return VK_F2;
    case Qt::Key_F3:        return VK_F3;
    case Qt::Key_F4:        return VK_F4;
    case Qt::Key_F5:        return VK_F5;
    case Qt::Key_F6:        return VK_F6;
    case Qt::Key_F7:        return VK_F7;
    case Qt::Key_F8:        return VK_F8;
    case Qt::Key_F9:        return VK_F9;
    case Qt::Key_F10:       return VK_F10;
    case Qt::Key_F11:       return VK_F11;
    case Qt::Key_F12:       return VK_F12;
    case Qt::Key_F13:       return VK_F13;
    case Qt::Key_F14:       return VK_F14;
    case Qt::Key_F15:       return VK_F15;
    case Qt::Key_F16:       return VK_F16;
    case Qt::Key_F17:       return VK_F17;
    case Qt::Key_F18:       return VK_F18;
    case Qt::Key_F19:       return VK_F19;
    case Qt::Key_F20:       return VK_F20;
    case Qt::Key_F21:       return VK_F21;
    case Qt::Key_F22:       return VK_F22;
    case Qt::Key_F23:       return VK_F23;
    case Qt::Key_F24:       return VK_F24;
    case Qt::Key_Colon:
    case Qt::Key_Semicolon:    return VK_OEM_1;
    case Qt::Key_Question:     return VK_OEM_2;
    case Qt::Key_AsciiTilde:
    case Qt::Key_QuoteLeft:    return VK_OEM_3;
    case Qt::Key_BraceLeft:
    case Qt::Key_BracketLeft:  return VK_OEM_4;
    case Qt::Key_Bar:
    case Qt::Key_Backslash:    return VK_OEM_5;
    case Qt::Key_BraceRight:
    case Qt::Key_BracketRight: return VK_OEM_6;
    case Qt::Key_QuoteDbl:
    case Qt::Key_Apostrophe:   return VK_OEM_7;
    case Qt::Key_Less:         return VK_OEM_COMMA;
    case Qt::Key_Greater:      return VK_OEM_PERIOD;
    case Qt::Key_Equal:        return VK_OEM_PLUS;
    case Qt::Key_ParenRight:   return 0x30;
    case Qt::Key_Exclam:       return 0x31;
    case Qt::Key_At:           return 0x32;
    case Qt::Key_NumberSign:   return 0x33;
    case Qt::Key_Dollar:       return 0x34;
    case Qt::Key_Percent:      return 0x35;
    case Qt::Key_AsciiCircum:  return 0x36;
    case Qt::Key_Ampersand:    return 0x37;
    case Qt::Key_copyright:    return 0x38;
    case Qt::Key_ParenLeft:    return 0x39;
    case Qt::Key_Underscore:   return VK_OEM_MINUS;
    default:;
  }

  return key;
}

void HotkeyWrapper::unregister()
{
  if( dllHandler.hDLLHandle )
  {
    dllHandler.removeHook();
    dllHandler.clearHotkeys();
  }
  else
  {
    for (int i = 0; i < hotkeys.count(); i++)
    {
      HotkeyStruct const & hk = hotkeys.at( i );

      UnregisterHotKey( hwnd, hk.id );

      if ( hk.key2 && hk.key2 != hk.key )
        UnregisterHotKey( hwnd, hk.id+1 );
    }
  }

  if( QHotkeyApplication * app = qobject_cast< QHotkeyApplication * >( qApp ) )
    app->unregisterWrapper(this);
}

bool QHotkeyApplication::nativeEventFilter( const QByteArray & /*eventType*/, void * message,
                                            qintptr * result )
{
  MSG * msg = reinterpret_cast< MSG * >( message );
  // The MSG handlers keep their historical 32-bit result form.
  long nativeResult = result ? static_cast< long >( *result ) : 0;
  bool handled = false;

  if ( msg->message == WM_HOTKEY || msg->message == GD_HOTKEY_MESSAGE )
  {
    for (int i = 0; i < hotkeyWrappers.size(); i++)
    {
      if ( hotkeyWrappers.at(i)->winEvent( msg, &nativeResult ) )
      {
        handled = true;
        break;
      }
    }
  }

  if( !handled && mainWindow )
  {
    if( ( static_cast< MainWindow * >( mainWindow ) )->handleGDMessage( msg, &nativeResult ) )
      handled = true;
  }

  if( handled && result )
    *result = static_cast< qintptr >( nativeResult );

  return handled;
}

//////////////////////////////////////////////////////////////////////////

#elif defined( Q_OS_MAC )

//////////////////////////////////////////////////////////////////////////

#elif defined( HAVE_X11 )

//////////////////////////////////////////////////////////////////////////

void HotkeyWrapper::init()
{
  recorderSession = createRecorderSession();

  if ( !recorderSession )
  {
    // No X11 display on this session (Wayland, offscreen): nothing is recorded and
    // no Xlib call is made. Every combination is refused below, so the caller
    // keeps a working application instead of a broken hotkey.
    gdWarning( "Global hotkeys are not available on platform \"%s\"",
               qPrintable( QGuiApplication::platformName() ) );
    return;
  }

  QString error;

  if ( !recorderSession->start( &error ) )
  {
    // An X11 session that cannot record is the historical failure: the caller
    // explains the missing RECORD extension, exactly as before.
    gdWarning( "Global hotkeys are not available: %s", qPrintable( error ) );
    recorderSession.reset();
    throw exInit();
  }

  recorderCore.reset( new RecorderCore( recorderSession.get(), this ) );
  connect( recorderCore.get(), &RecorderCore::activated, this, &HotkeyWrapper::hotkeyActivated );
}

void HotkeyWrapper::waitKey2()
{
  if( recorderCore )
    recorderCore->cancelSecondKeyWait();
}

void HotkeyWrapper::run() // The blocking recording loop lives in the session.
{
}

bool HotkeyWrapper::setGlobalKey( int key, int key2,
                                  Qt::KeyboardModifiers modifier, int handle )
{
  if ( !key )
    return false; // We don't monitor empty combinations

  if( !recorderCore )
    return false;

  quint32 const vk = nativeKey( key );
  quint32 const vk2 = key2 ? nativeKey( key2 ) : 0;

  if( vk == 0 )
    return false;

  quint32 mod = 0;
  if (modifier & Qt::ShiftModifier)
      mod |= RecorderShiftMask;
  if (modifier & Qt::ControlModifier)
      mod |= RecorderControlMask;
  if (modifier & Qt::AltModifier)
      mod |= RecorderAltMask;
  if (modifier & Qt::MetaModifier)
      mod |= RecorderMetaMask;

  recorderCore->addBinding( vk, vk2, mod, handle );

  return true;
}

quint32 HotkeyWrapper::nativeKey( int key )
{
  if( !recorderSession )
    return 0;

  return recorderSession->keyCodeForQtKey( key );
}

void HotkeyWrapper::recordedKey( bool pressed, quint32 keyCode )
{
  if( recorderCore )
    recorderCore->recordedKey( pressed, keyCode );
}

quint32 HotkeyWrapper::keyCodeForKey( int key )
{
  return nativeKey( key );
}

void HotkeyWrapper::unregister()
{
  // Same ordering as X11RecordBackend::retire(): release the grabs while the
  // session is alive, detach the sink so the worker cannot reach the core, then
  // stop the recorder (parking it if it could not be joined) and destroy the core.
  if( recorderCore )
    recorderCore->clear();

  if( recorderSession )
  {
    recorderSession->setEventSink( nullptr );

    std::unique_ptr< RecorderSession > stopping = std::move( recorderSession );
    if( stopping->stop() == RecorderSession::Shutdown::Abandoned )
      parkAbandonedRecorder( std::move( stopping ), std::move( recorderCore ) );
    else
      recorderCore.reset();
  }
  else
    recorderCore.reset();

  if( QHotkeyApplication * app = qobject_cast< QHotkeyApplication * >( qApp ) )
    app->unregisterWrapper(this);
}

//////////////////////////////////////////////////////////////////////////

#else // !Q_OS_WIN && !Q_OS_MAC && !HAVE_X11

//////////////////////////////////////////////////////////////////////////

// This build has no X11 capability at all: the wrapper refuses every combination
// instead of pretending to hook one.

void HotkeyWrapper::init()
{
}

void HotkeyWrapper::waitKey2()
{
  state2 = false;
}

void HotkeyWrapper::run()
{
}

bool HotkeyWrapper::setGlobalKey( int, int, Qt::KeyboardModifiers, int )
{
  return false;
}

quint32 HotkeyWrapper::nativeKey( int )
{
  return 0;
}

void HotkeyWrapper::unregister()
{
  if( QHotkeyApplication * app = qobject_cast< QHotkeyApplication * >( qApp ) )
    app->unregisterWrapper(this);
}

#endif
