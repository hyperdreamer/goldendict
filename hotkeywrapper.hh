#ifndef HOTKEYWRAPPER_H
#define HOTKEYWRAPPER_H

/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Global hotkeys (specification §2.10).
 *
 * On X11 the combinations are read with the RECORD extension instead of XGrabKey,
 * because a grab would keep other clients from receiving their input. HAVE_X11
 * records that this build can talk to Xlib; it is never evidence that the running
 * session is X11. Every X11 path below therefore asks PlatformWindow::x11Display()
 * (the active QPA platform) and does nothing when that answers null, so a Wayland
 * session never opens a display, never creates a RECORD context and never grabs a
 * key.
 *
 * The recorder is split so its rules can be proved without a RECORD server:
 *
 *   RecorderMatcher   the combination state machine, pure: no Xlib, no QObject;
 *   RecorderCore      the matcher plus the second-key timer and the grab
 *                     bookkeeping, driven by a RecorderSession;
 *   RecorderSession   the session work: keycode resolution, grabbing and the
 *                     blocking RECORD loop (X11RecordSession in the .cc);
 *   X11RecordBackend  the §2.10 HotkeyBackend over the recorder, mapping stable
 *                     application IDs ("main-window") to recorder handles.
 *
 * HotkeyWrapper keeps its historical integer-handle API for the current
 * MainWindow consumer and drives the same core.
 */

#include <QList>
#include <QObject>
#include <QPair>
#include <QSessionManager>
#include <QSet>
#include <QString>
#include <QThread>
#include <QVector>
#include <Qt>

#include <memory>

#include "ex.hh"
#include "hotkeybackend.hh"
#include "qtsingleapplication/src/qtsingleapplication.h"

#ifdef Q_OS_WIN32
#include "hotkeys.h"
#endif

#ifdef Q_OS_MAC
#define __SECURITYHI__
#include <Carbon/Carbon.h>
#endif

class QTimer;

//////////////////////////////////////////////////////////////////////////

struct HotkeyStruct
{
  HotkeyStruct() {}
  HotkeyStruct( quint32 key, quint32 key2, quint32 modifier, int handle, int id );

  quint32 key, key2;
  quint32 modifier;
  int handle;
  int id;
#ifdef Q_OS_MAC
  EventHotKeyRef hkRef, hkRef2;
#endif
};

//////////////////////////////////////////////////////////////////////////

/// Runtime platform decisions of the recorder.
namespace HotkeyRuntime {

/// True only when the named QPA platform is an actual X11 session. HAVE_X11 is a
/// build capability and never decides this: the platform name does, and the
/// comparison is case-insensitive because QPA names are not guaranteed to be
/// lower case ("Wayland", "XCB"). A Wayland session is rejected by name first,
/// so no spelling of it can reach the X11 recorder.
bool isX11Session( QString const & platformName );

/// True when the recording loop is known to have returned, i.e. its thread can be
/// joined. A blocking XRecordEnableContext cannot be cancelled from another
/// connection, so a loop that has not returned by the shutdown bound is abandoned
/// (detached, session parked) rather than joined. Kept pure so the policy is
/// testable without a wedged X server.
bool recordingLoopReturnedInTime( bool recordingFinished ) noexcept;

} // namespace HotkeyRuntime

//////////////////////////////////////////////////////////////////////////

/// The modifier bits the recorder tracks. The values are the X protocol's own
/// modifier masks (ShiftMask, ControlMask, Mod1Mask, Mod4Mask), spelled here
/// because the matcher is pure and must not need an Xlib header — and so a
/// decision can be handed to XGrabKey/XUngrabKey unchanged.
enum RecorderModifier : quint32
{
  RecorderShiftMask = 1u << 0,
  RecorderControlMask = 1u << 2,
  RecorderAltMask = 1u << 3,
  RecorderMetaMask = 1u << 6
};

/// The keycodes the session uses for the eight modifier keys, resolved once from
/// the keymap of the recorder's display. Plain data: resolving them is X11 work,
/// using them is not. A zero entry is a key this keymap does not have.
struct RecorderModifierKeycodes
{
  quint32 leftShift = 0;
  quint32 rightShift = 0;
  quint32 leftControl = 0;
  quint32 rightControl = 0;
  quint32 leftAlt = 0;
  quint32 rightAlt = 0;
  quint32 leftMeta = 0;
  quint32 rightMeta = 0;
};

/// The keycodes the clipboard copy shortcuts use in this session. A combination
/// built from them must not be grabbed before its first key is pressed: the copy
/// has to reach the application below.
struct RecorderCopyKeycodes
{
  quint32 c = 0;
  quint32 insert = 0;
  quint32 keypadInsert = 0;
};

/// One combination the recorder watches: the key codes and modifier mask of this
/// session, plus the integer handle an activation reports.
struct RecorderBinding
{
  quint32 key = 0;
  quint32 key2 = 0;
  quint32 modifiers = 0;
  int handle = 0;
};

/// The recorder's combination matcher: the rule that turns a stream of recorded
/// key transitions into an activation. It keeps the one-key, two-key and
/// double-press (Ctrl+C then Ctrl+C) forms, the 500 ms window of the second key
/// and the clipboard-copy keys out of the first-key grab, exactly as the replaced
/// slot-based code did.
///
/// Pure: no Xlib, no QObject, no timer; the owner runs the timer and the session
/// performs the grabs. Every branch is therefore provable on one build without a
/// RECORD server.
class RecorderMatcher
{
public:
  /// How long the second key of a two-key combination is awaited, in ms.
  static int constexpr secondKeyTimeoutMs = 500;

  enum class Kind
  {
    Ignored,       ///< Nothing is bound to this press.
    Activated,     ///< `handle` fired.
    SecondKeyWait  ///< The first key of a two-key combination matched.
  };

  struct Decision
  {
    Kind kind = Kind::Ignored;
    int handle = 0;

    /// Valid for SecondKeyWait: the second key and the modifier mask it needs.
    quint32 secondKey = 0;
    quint32 secondKeyModifiers = 0;

    /// True when the recorder should grab the second key while it waits, i.e.
    /// unless the first key already was the clipboard copy key and the second one
    /// is not.
    bool grabSecondKey = false;
  };

  void addBinding( quint32 key, quint32 key2, quint32 modifiers, int handle );
  void clearBindings();

  QVector< RecorderBinding > const & bindings() const noexcept;

  /// Offers one recorded key press. A press that arrives while a second key is
  /// awaited always ends that wait first — the wait never survives into another
  /// combination, as in the replaced code — and then either reports the awaited
  /// handle or matches the bindings afresh.
  Decision press( quint32 keyCode, quint32 modifiers );

  /// Ends a second-key wait without an activation (the timeout path).
  void cancelSecondKeyWait() noexcept;

  bool awaitingSecondKey() const noexcept;
  int awaitedHandle() const noexcept;

  /// The modifier key codes of this session.
  void setModifierKeyCodes( RecorderModifierKeycodes const & keys ) noexcept;
  RecorderModifierKeycodes const & modifierKeyCodes() const noexcept;

  /// True when `keyCode` is one of the tracked modifier keys.
  bool isModifierKeyCode( quint32 keyCode ) const noexcept;

  /// Records a modifier key transition and returns the mask now in effect.
  quint32 modifierPressed( quint32 keyCode ) noexcept;
  quint32 modifierReleased( quint32 keyCode ) noexcept;
  quint32 modifiers() const noexcept;

  /// The modifier flag one modifier key code carries, or 0 for a non-modifier.
  static quint32 modifierFlagForKeyCode( quint32 keyCode,
                                         RecorderModifierKeycodes const & keys ) noexcept;

  /// The clipboard copy key codes of this session, used by both rules below.
  void setCopyKeyCodes( quint32 cKeyCode, quint32 insertKeyCode,
                        quint32 keypadInsertKeyCode ) noexcept;

  /// True for the combinations that ask the application below for a copy: plain
  /// Ctrl with C or with either Insert key, according to setCopyKeyCodes().
  bool isCopyToClipboardKey( quint32 keyCode, quint32 modifiers ) const noexcept;

  /// The same rule as a plain function, for a caller that has no matcher yet.
  static bool isCopyToClipboardKey( quint32 keyCode,
                                    quint32 modifiers,
                                    quint32 cKeyCode,
                                    quint32 insertKeyCode,
                                    quint32 keypadInsertKeyCode ) noexcept;

private:
  QVector< RecorderBinding > boundKeys;
  RecorderModifierKeycodes modifierKeys;
  RecorderCopyKeycodes copyKeys;
  quint32 currentModifiers = 0;

  bool waiting = false;
  RecorderBinding waiter;
};

//////////////////////////////////////////////////////////////////////////

/// Where a RecorderSession posts its recorded key transitions. The session calls
/// this from its own (blocking) thread; implementations must not run the matcher
/// there but hand the event to the owner's thread, which is where the second-key
/// timer lives and where the replaced slot-based code ran.
class RecorderEventSink
{
public:
  virtual ~RecorderEventSink() = default;

  virtual void postRecordedKey( bool pressed, quint32 keyCode ) = 0;
};

//////////////////////////////////////////////////////////////////////////

/// The session work a recorder needs: the keymap of the running session, the
/// grabbing of combinations and the blocking RECORD loop. Implemented over Xlib
/// by X11RecordSession (hotkeywrapper.cc), which returns null for a session
/// without an X11 display, and by fixtures in the test suite so the recording
/// rules can be driven without a server.
class RecorderSession
{
public:
  virtual ~RecorderSession() = default;

  /// Resolves the keymap, creates the RECORD context and starts recording.
  /// Returns false with a reason instead of throwing, so a caller can decide
  /// whether a missing recorder is fatal for it.
  virtual bool start( QString * error ) = 0;

  /// What stopping a recorder actually achieved.
  enum class Shutdown
  {
    Joined,    ///< The recording loop returned and its thread was joined.
    Abandoned  ///< The loop did not return in time; its thread was detached and
               ///< the session object must outlive it (never freed).
  };

  /// Disables the context and joins the recording thread. A blocking
  /// XRecordEnableContext cannot be cancelled from another connection, so a
  /// recorder that does not return within the shutdown bound is detached and
  /// reported as Abandoned instead of hanging the process: the caller must keep
  /// the object alive, which is why the outcome is not a plain void.
  virtual Shutdown stop() = 0;

  /// The keycode `key` has in this session's keymap, 0 when there is none.
  virtual quint32 keyCodeForQtKey( int key ) = 0;

  /// The keycode the X keysym `name` has in this session's keymap, 0 when the
  /// session knows no such keysym. The portal trigger format names its key with
  /// exactly that keysym name.
  virtual quint32 keyCodeForKeysymName( QString const & name ) = 0;

  virtual RecorderModifierKeycodes modifierKeyCodes() const = 0;
  virtual quint32 copyKeyCode() const = 0;
  virtual quint32 insertKeyCode() const = 0;
  virtual quint32 keypadInsertKeyCode() const = 0;

  /// Grabs the combination on this session's root window. False when the grab was
  /// refused (another client already holds it).
  virtual bool grab( quint32 keyCode, quint32 modifiers ) = 0;
  virtual void ungrab( quint32 keyCode, quint32 modifiers ) = 0;
  virtual void ungrabAll() = 0;

  virtual void setEventSink( RecorderEventSink * sink ) = 0;

  /// True when `sink` is the sink this session posts to. A recorder that is torn
  /// down must not detach a sink somebody else installed in the meantime.
  virtual bool isEventSink( RecorderEventSink const * sink ) const = 0;
};

/// Creates the RECORD session of the running session, or null when the active QPA
/// platform is not an X11 one (or Qt exposes no display for it). An X11 session
/// always gets a session object; whether RECORD itself works is only known from
/// start().
std::unique_ptr< RecorderSession > createRecorderSession();

/// Takes ownership of a session whose recording thread could not be joined. Such
/// a thread is detached and keeps reading the session, so the object is parked
/// for the life of the process instead of being freed under it.
class RecorderCore;

void parkAbandonedRecorder( std::unique_ptr< RecorderSession > session,
                            std::unique_ptr< RecorderCore > core );

//////////////////////////////////////////////////////////////////////////

/// The recorder: the matcher above plus the second-key timer and the grab
/// bookkeeping, driven by a session. It lives on the owner's thread — the same
/// thread the replaced slot-based code ran on — so the timer needs no extra
/// thread of its own.
class RecorderCore : public QObject, public RecorderEventSink
{
  Q_OBJECT

public:
  explicit RecorderCore( RecorderSession * session, QObject * parent = nullptr );
  ~RecorderCore() override;

  /// Binds one combination under `handle`. The first key is grabbed unless it is
  /// the clipboard copy key, whose press must still reach the application.
  void addBinding( quint32 key, quint32 key2, quint32 modifiers, int handle );

  /// Ends any outstanding wait and releases every grab this core made.
  void clear();

  /// Ends an outstanding second-key wait without an activation (and releases the
  /// second-key grab, if any).
  void cancelSecondKeyWait();

  int bindingCount() const noexcept;
  bool isGrabbed( quint32 keyCode, quint32 modifiers ) const noexcept;
  RecorderMatcher const & matcher() const noexcept;

  /// Feeds one recorded key transition in, on the owner's thread.
  void recordedKey( bool pressed, quint32 keyCode );

  /// RecorderEventSink: called on the session's recording thread. The event is
  /// posted to this object's thread, so the matcher and its timer are only ever
  /// touched from there.
  void postRecordedKey( bool pressed, quint32 keyCode ) override;

signals:
  /// One bound combination fired. The handle is the one addBinding() was given.
  void activated( int handle );

private slots:
  void secondKeyWaitTimedOut();

private:
  void grab( quint32 keyCode, quint32 modifiers );
  void ungrab( quint32 keyCode, quint32 modifiers );
  void endSecondKeyWait();
  void releaseSecondKeyGrab();

  RecorderSession * session;
  RecorderMatcher recorderMatcher;
  QTimer * secondKeyTimer;
  QSet< QPair< quint32, quint32 > > grabbedKeys;

  struct Waiter
  {
    quint32 keyCode = 0;
    quint32 modifiers = 0;
    bool grabbed = false;
  };
  Waiter waiter;
};

//////////////////////////////////////////////////////////////////////////

#ifdef HAVE_X11
/// The §2.10 X11 backend: the RECORD recorder behind the HotkeyBackend contract.
/// Stable application IDs map to recorder integer handles, an activation reports
/// the ID of the combination with an empty token (X11 has no portal token), and a
/// session without an X11 display reports itself unsupported instead of falling
/// back to anything else.
class X11RecordBackend : public HotkeyBackend
{
  Q_OBJECT

public:
  explicit X11RecordBackend( QObject * parent = nullptr );
  ~X11RecordBackend() override;

  /// True when this session has an X11 display the recorder may borrow. Never
  /// derived from HAVE_X11: HotkeyRuntime::isX11Session() of the active QPA and
  /// the borrowed display decide.
  bool isSupported() const noexcept override;

  void bind( QVector< HotkeyBinding > const & bindings ) override;
  void unbind() override;
  State state() const noexcept override;

  /// The explanation of the current state: set together with every stateChanged()
  /// and empty while the bindings are in place. A session without an X11 display
  /// explains why nothing is bound.
  QString explanation() const;

  /// The keycode `key` has in this session, 0 when the keymap has none or no
  /// recorder is running. Public because recordedKey() is driven with real
  /// keycodes.
  quint32 keyCodeForKey( int key );

  /// Feeds one recorded key transition through the recorder, exactly as the RECORD
  /// loop does. The rules under test are the production ones; this entry point
  /// exists so they are provable without a RECORD server.
  void recordedKey( bool pressed, quint32 keyCode );

  /// The recorder handle currently bound to `id`, or 0 when the ID is not bound.
  /// A rebind retires the old handle, so an activation can never report a stale
  /// ID.
  int handleForId( QString const & id ) const;

private slots:
  void recorderActivated( int handle );

private:
  bool bindOne( HotkeyBinding const & binding, QString * reason );
  void retire();
  void setState( State state, QString const & explanation );

  std::unique_ptr< RecorderSession > session;
  std::unique_ptr< RecorderCore > core;

  QVector< QPair< QString, int > > bound;  // id -> handle, in binding order

  // Never reused: a retired registration's handle must not address a later one.
  int nextHandle = 1;

  State currentState = State::Unsupported;
  QString currentExplanation;
};
#endif // HAVE_X11

//////////////////////////////////////////////////////////////////////////

class HotkeyWrapper : public QThread // Thread is actually only used on X11
{
  Q_OBJECT

  friend class QHotkeyApplication;

public:

  DEF_EX( exInit, "Hotkey wrapper failed to init", std::exception )

  HotkeyWrapper(QObject *parent);
  virtual ~HotkeyWrapper();

  /// The handle is passed back in hotkeyActivated() to inform which hotkey
  /// was activated.
  bool setGlobalKey( int key, int key2, Qt::KeyboardModifiers modifier,
                     int handle );

  /// Unregisters everything
  void unregister();
#ifdef Q_OS_WIN32
  bool handleViaDLL()
  { return dllHandler.hDLLHandle != 0; }
#endif

#ifdef HAVE_X11
  /// Feeds one recorded key transition through this wrapper's recorder, exactly as
  /// the RECORD loop does. The X11 rules are the production ones; this entry point
  /// exists so they are provable without a RECORD server.
  void recordedKey( bool pressed, quint32 keyCode );

  /// The keycode `key` has in this session, 0 when the keymap has none.
  quint32 keyCodeForKey( int key );
#endif

signals:

  void hotkeyActivated( int );

protected slots:

  void waitKey2();

private:

  void init();
  quint32 nativeKey(int key);

  QList<HotkeyStruct> hotkeys;

  bool state2;
  HotkeyStruct state2waiter;

#ifdef Q_OS_WIN32
  virtual bool winEvent ( MSG * message, long * result );
  HWND hwnd;

  struct DLL_HANDLER
  {
    HMODULE hDLLHandle;
    setHookProc setHook;
    removeHookProc removeHook;
    setHotkeysProc setHotkeys;
    clearHotkeysProc clearHotkeys;
  };

  DLL_HANDLER dllHandler;

  bool checkState( quint32 vk, quint32 mod );

#elif defined(Q_OS_MAC)

public:
  void activated( int hkId );
private:
  void sendCmdC();

  static EventHandlerUPP hotKeyFunction;
  quint32 keyC;
  EventHandlerRef handlerRef;

#else

  void run(); // QThread

  /// The recorder this wrapper drives. Both are null when this session has no X11
  /// display: then every combination is refused and no Xlib call is made at all.
  std::unique_ptr< RecorderSession > recorderSession;
  std::unique_ptr< RecorderCore > recorderCore;

#endif
};

//////////////////////////////////////////////////////////////////////////

class DataCommitter
{
public:

  virtual void commitData( QSessionManager & )=0;
  virtual ~DataCommitter()
  {}
};

// Intermediate class to avoid misunderstanding of #ifdef's
// by Qt meta-object compiler

class QIntermediateApplication : public QtSingleApplication
#if defined( Q_OS_WIN )
        , public QAbstractNativeEventFilter
#endif
{
public:
  QIntermediateApplication( int & argc, char ** argv ) :
    QtSingleApplication( argc, argv )
  {}

  QIntermediateApplication( QString const & id, int & argc, char ** argv ) :
    QtSingleApplication( id, argc, argv )
  {}
};

class QHotkeyApplication : public QIntermediateApplication
{
  Q_OBJECT

  friend class HotkeyWrapper;

  QList< DataCommitter * > dataCommitters;

public:
  QHotkeyApplication( int & argc, char ** argv );
  QHotkeyApplication( QString const & id, int & argc, char ** argv );

  void addDataCommiter( DataCommitter & );
  void removeDataCommiter( DataCommitter & );

private slots:
  /// This calls all data committers.
  void hotkeyAppCommitData( QSessionManager & );

  void hotkeyAppSaveState( QSessionManager & );

protected:
  void registerWrapper(HotkeyWrapper *wrapper);
  void unregisterWrapper(HotkeyWrapper *wrapper);

#ifdef Q_OS_WIN32
  /// Qt 6 made the result pointer's integer type explicit; the MSG handlers below
  /// still speak its historical `long *` form.
  virtual bool nativeEventFilter( const QByteArray & eventType, void * message, qintptr * result );

  QWidget * mainWindow;
public:
  void setMainWindow( QWidget * widget )
  { mainWindow = widget; }
protected:
#endif // Q_OS_WIN32
  QList<HotkeyWrapper*> hotkeyWrappers;
};

//////////////////////////////////////////////////////////////////////////

#endif // HOTKEYWRAPPER_H
