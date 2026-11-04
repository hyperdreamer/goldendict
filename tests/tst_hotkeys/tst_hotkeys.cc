/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Hotkey syntax, backend capability contract and portable audio configuration
 * migration (specification §2.10, §4.11, §4.13, SD-15).
 *
 * The suite links the production conversion, the production XML load/save path
 * and the production audio factory. The two configuration fixtures are loaded
 * through the real Config::load()/Config::save() against an isolated HOME, so
 * every assertion is about the shipped code and the shipped save point rather
 * than a test-only parser.
 *
 * The pins that matter are the legacy behaviours:
 *   * the existing one/two-key HotKey layout (modifiers from the first
 *     combination only, second combination re-rendered with them);
 *   * untouched stored shortcut text, including text that is not a
 *     representable shortcut or not a bindable portal trigger, survives a
 *     save/load round trip byte-for-byte;
 *   * the removed "Qt Multimedia" backend and unknown backend names migrate to
 *     the public default backend in memory only, while a no_ffmpeg_player build
 *     keeps the stored text verbatim and reports the internal player as
 *     unavailable;
 *   * loading never saves, and the additive <hotkeys>/<portalBindings> section
 *     never replaces anything that existed before it.
 *
 * Every one of those has a mutation recorded in the task report: the fixture
 * fails when the legacy behaviour is restored or when the additive rule is
 * dropped.
 */

#include <QtTest>
#include <QPointer>

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QKeySequence>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QScopedPointer>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVector>

#include <memory>

#include "support/fixtures.hh"

#include "audioplayerfactory.hh"
#include "broken_xrecord.hh"
#include "config.hh"
#include "externalaudioplayer.hh"
#include "externalviewer.hh"
#include "ffmpegaudioplayer.hh"
#include "hotkeybackend.hh"
#include "hotkeysyntax.hh"
#include "hotkeywrapper.hh"
#include "keyboardstate.hh"
#include "platformwindow.hh"
#include "portalhotkeys.hh"
#include "waylandselection.hh"
#include "preferences.hh"
#include "hotkeyedit.hh"
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include "support/fake_portal.hh"

// Xlib last, because Qt's own headers reject a Status/Bool definition that
// precedes them, and its colliding macros dropped below the way fixx11h.h does it
// for the application: the recorder fixtures need the server's own keymap and
// input generator, the rest of this suite stays free of Xlib's identifiers.
#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#undef Bool
#undef None
#undef Status
#undef Success
#undef KeyPress
#undef KeyRelease
#undef Above
#undef Below
#undef Expose
#undef FocusIn
#undef FocusOut
#undef Always
#undef Unsorted
#undef GrayScale
#undef CursorShape
#undef FontChange
#undef min
#undef max
#endif

namespace {

/// The published backend contract, exactly as §2.10 states it. Taking the
/// address of each virtual with its complete type is a compile-time pin: a
/// backend whose accessor lost `const noexcept`, whose bind() lost the vector
/// parameter or whose state() changed its enum cannot be referenced here.
void requireBackendContract()
{
  using State = HotkeyBackend::State;
  using SupportedPointer = bool ( HotkeyBackend::* )() const noexcept;
  using BindPointer = void ( HotkeyBackend::* )( QVector< HotkeyBinding > const & );
  using UnbindPointer = void ( HotkeyBackend::* )();
  using StatePointer = State ( HotkeyBackend::* )() const noexcept;

  SupportedPointer const isSupported = &HotkeyBackend::isSupported;
  BindPointer const bind = &HotkeyBackend::bind;
  UnbindPointer const unbind = &HotkeyBackend::unbind;
  StatePointer const state = &HotkeyBackend::state;

  Q_ASSERT( isSupported );
  Q_ASSERT( bind );
  Q_ASSERT( unbind );
  Q_ASSERT( state );
}

/// A concrete backend over the real interface: it records the requested
/// bindings and emits every contract signal so the published signatures and the
/// state/display semantics are exercised for real. It is test-only; no
/// production backend is copied or stubbed here.
class RecordingHotkeyBackend : public HotkeyBackend
{
public:
  explicit RecordingHotkeyBackend( bool supported, QObject * parent = nullptr ) :
    HotkeyBackend( parent ), supported( supported )
  {
  }

  bool isSupported() const noexcept override { return supported; }

  void bind( QVector< HotkeyBinding > const & bindings ) override
  {
    ++bindCalls;
    requested = bindings;
    currentState = State::Binding;
    emit stateChanged( currentState, QStringLiteral( "requesting bindings" ) );

    QVector< PortalBindingDisplay > reported;
    for( HotkeyBinding const & binding : bindings )
      reported.push_back( PortalBindingDisplay{ binding.id, QStringLiteral( "Assigned by portal" ) } );

    currentState = State::Partial;
    emit stateChanged( currentState, QStringLiteral( "the portal accepted a subset" ) );
    emit bindingsChanged( reported );
    emit activated( bindings.value( 0 ).id, QStringLiteral( "activation-token" ) );
  }

  void unbind() override
  {
    ++unbindCalls;
    currentState = State::Closed;
    emit stateChanged( currentState, QStringLiteral( "session closed" ) );
  }

  State state() const noexcept override { return currentState; }

  QVector< HotkeyBinding > requested;
  int bindCalls = 0;
  int unbindCalls = 0;

private:
  bool supported;
  State currentState = State::Unsupported;
};

QByteArray readFileBytes( QString const & path )
{
  QFile file( path );
  if( !file.open( QIODevice::ReadOnly ) )
    return QByteArray();
  return file.readAll();
}

bool hasFfmpegPlayer()
{
  return TestSupport::compiledFeatureDefines().contains( QStringLiteral( "MAKE_FFMPEG_PLAYER" ) );
}

/// True when the running QPA is an actual X11 session. HAVE_X11 is only a build
/// capability: the cases that bind real X11 combinations assert this instead of
/// assuming that the gate happens to be xcb.
bool xcbSession()
{
  return HotkeyRuntime::isX11Session( QGuiApplication::platformName() );
}

/// A recorder session with no server behind it. It records what the production
/// recorder asks of its session (keycodes and grabs) and feeds key transitions
/// back in through the production sink, so every recording rule is driven without
/// a RECORD server. Its keymap is the fixture's own, so no assertion depends on
/// the host's keyboard layout.
class FixtureRecorderSession : public RecorderSession
{
public:
  /// Counts destructions so a test can prove an abandoned session stays alive.
  static inline int destroyedCount = 0;
  ~FixtureRecorderSession() override { ++destroyedCount; }
  enum Codes : quint32
  {
    LeftShift = 50,
    RightShift = 62,
    LeftControl = 37,
    RightControl = 105,
    LeftAlt = 64,
    RightAlt = 108,
    LeftMeta = 133,
    RightMeta = 134,
    KeyG = 55,
    KeyC = 54,
    KeyF = 41,
    KeyInsert = 118,
    KeyKeypadInsert = 119
  };

  FixtureRecorderSession()
  {
    modifiers.leftShift = LeftShift;
    modifiers.rightShift = RightShift;
    modifiers.leftControl = LeftControl;
    modifiers.rightControl = RightControl;
    modifiers.leftAlt = LeftAlt;
    modifiers.rightAlt = RightAlt;
    modifiers.leftMeta = LeftMeta;
    modifiers.rightMeta = RightMeta;

    cCode = KeyC;
    insertCode = KeyInsert;
    keypadInsertCode = KeyKeypadInsert;
  }

  bool start( QString * error ) override
  {
    Q_UNUSED( error )
    running = true;
    return true;
  }

  RecorderSession::Shutdown stop() override { running = false; return RecorderSession::Shutdown::Joined; }

  quint32 keyCodeForQtKey( int key ) override { return quint32( key ) & 0xff; }

  quint32 keyCodeForKeysymName( QString const & name ) override
  {
    if ( name == QStringLiteral( "g" ) )
      return KeyG;
    if ( name == QStringLiteral( "c" ) )
      return KeyC;
    if ( name == QStringLiteral( "F" ) )
      return KeyF;
    if ( name == QStringLiteral( "Insert" ) )
      return KeyInsert;
    if ( name == QStringLiteral( "KP_Insert" ) )
      return KeyKeypadInsert;
    return 0;
  }

  RecorderModifierKeycodes modifierKeyCodes() const override { return modifiers; }
  quint32 copyKeyCode() const override { return cCode; }
  quint32 insertKeyCode() const override { return insertCode; }
  quint32 keypadInsertKeyCode() const override { return keypadInsertCode; }

  bool grab( quint32 keyCode, quint32 modifiersValue ) override
  {
    grabCalls.append( qMakePair( keyCode, modifiersValue ) );

    if ( refuseGrabs )
      return false;

    grabs.insert( qMakePair( keyCode, modifiersValue ) );
    return true;
  }

  void ungrab( quint32 keyCode, quint32 modifiersValue ) override
  {
    ungrabCalls.append( qMakePair( keyCode, modifiersValue ) );
    grabs.remove( qMakePair( keyCode, modifiersValue ) );
  }

  void ungrabAll() override
  {
    for( QPair< quint32, quint32 > const & grab : grabs )
      ungrabCalls.append( grab );
    grabs.clear();
  }

  void setEventSink( RecorderEventSink * sinkValue ) override { sink = sinkValue; }
  bool isEventSink( RecorderEventSink const * sinkValue ) const override { return sink == sinkValue; }

  /// The server side of the fixture: one recorded key transition arriving from the
  /// recording thread.
  void pressRecorded( quint32 keyCode )
  {
    if ( sink )
      sink->postRecordedKey( true, keyCode );
  }

  void releaseRecorded( quint32 keyCode )
  {
    if ( sink )
      sink->postRecordedKey( false, keyCode );
  }

  bool holdsGrab( quint32 keyCode, quint32 modifiersValue ) const
  {
    return grabs.contains( qMakePair( keyCode, modifiersValue ) );
  }

  bool running = false;
  bool refuseGrabs = false;
  RecorderModifierKeycodes modifiers;
  quint32 cCode = 0;
  quint32 insertCode = 0;
  quint32 keypadInsertCode = 0;
  QSet< QPair< quint32, quint32 > > grabs;
  QVector< QPair< quint32, quint32 > > grabCalls;
  QVector< QPair< quint32, quint32 > > ungrabCalls;
  RecorderEventSink * sink = nullptr;
};

#ifdef HAVE_X11
/// Set by the X11 error handler below, so a refused grab is a return value instead
/// of a message on stderr and a process-wide handler left behind.
bool secondClientGrabError = false;

int secondClientGrabErrorHandler( Display *, XErrorEvent * event )
{
  if ( event->error_code == BadAccess )
    secondClientGrabError = true;
  return 0;
}

/// True when another X client can still take the combination, i.e. this process
/// does not hold it. The attempt is made on a connection of its own, because
/// Xlib only refuses a grab that another *client* holds.
bool anotherClientCanGrab( quint32 keyCode, quint32 modifiers )
{
  Display * const display = XOpenDisplay( nullptr );

  if ( !display )
    return false;

  bool const previousError = secondClientGrabError;
  secondClientGrabError = false;

  int ( *previousHandler )( Display *, XErrorEvent * ) = XSetErrorHandler( secondClientGrabErrorHandler );

  XGrabKey( display, keyCode, modifiers, DefaultRootWindow( display ), True, GrabModeAsync, GrabModeAsync );
  XSync( display, False );

  bool const took = !secondClientGrabError;

  if ( took )
    XUngrabKey( display, keyCode, modifiers, DefaultRootWindow( display ) );

  XSync( display, False );

  XSetErrorHandler( previousHandler );
  secondClientGrabError = previousError;
  XCloseDisplay( display );

  return took;
}

/// Presses and releases one key on the server's own input generator, i.e. exactly
/// the way a user would: the RECORD context sees a real device event.
bool fakeKey( Display * display, KeySym keySym, bool press )
{
  KeyCode const keyCode = XKeysymToKeycode( display, keySym );

  if ( keyCode == 0 )
    return false;

  return XTestFakeKeyEvent( display, keyCode, press ? True : False, 0 ) != 0;
}
#endif

QString selectionSourceText( char const * path )
{
  return QString::fromUtf8( readFileBytes( QDir( TestSupport::repositoryRoot() ).filePath( QLatin1String( path ) ) ) );
}
QString selectionCompact( QString text )
{
  text.remove( QRegularExpression( QStringLiteral( "\\s+" ) ) );
  return text;
}
// Extract a balanced body, ignoring braces in quoted strings and comments.
// Empty means anchor/opening/closing brace missing; every caller asserts nonempty.
// The extractor also reads the qmake text these assertions inspect: an anchor that
// ends with the opening brace opens the body itself, a leading '#' starts a line
// comment, a backslash escapes the next character, and "/*" is a block comment
// only when a closing "*/" exists (glob paths such as redist/icons/*.* are not).
QString selectionBlock( QString const & text, QString const & anchor )
{
  qsizetype const at = text.indexOf( anchor );
  if( at < 0 ) return {};
  qsizetype const first = anchor.endsWith( '{' ) ? at + anchor.size() - 1
                                                : text.indexOf( '{', at + anchor.size() );
  if( first < 0 ) return {};
  int depth = 0;
  QChar quote;
  bool line = false, block = false, escape = false, lineStart = true;
  for( qsizetype i = first; i < text.size(); ++i )
  {
    QChar const c = text[i];
    QChar const next = i + 1 < text.size() ? text[i+1] : QChar{};
    if( line ) { if( c == '\n' ) { line = false; lineStart = true; } continue; }
    if( block ) { if( c == '*' && next == '/' ) { block = false; ++i; } continue; }
    if( !quote.isNull() )
    {
      if( escape ) escape = false;
      else if( c == '\\' ) escape = true;
      else if( c == quote ) quote = QChar{};
      continue;
    }
    if( c == '\\' ) { ++i; continue; }
    if( c == '#' && lineStart ) { line = true; continue; }
    if( c == '/' && next == '/' ) { line = true; ++i; continue; }
    if( c == '/' && next == '*' && text.indexOf( QStringLiteral( "*/" ), i + 2 ) >= 0 )
    { block = true; ++i; continue; }
    if( c == '"' || c == '\'' ) { quote = c; continue; }
    if( c == '{' ) { ++depth; lineStart = false; }
    else if( c == '}' && --depth == 0 ) return text.mid( first + 1, i - first - 1 );
    else if( c == '\n' ) lineStart = true;
    else if( c != ' ' && c != '\t' && c != '\r' ) lineStart = false;
  }
  return {};
}
bool selectionOrdered( QString const & text, QStringList const & needles )
{
  qsizetype previous = -1;
  for( QString const & needle : needles )
  {
    previous = text.indexOf( needle, previous + 1 );
    if( previous < 0 ) return false;
  }
  return true;
}

/// Adds, replaces or removes one <preferences> child in the suite's installed
/// isolated fixture. Only the copy Config::load() reads is edited: the
/// repository fixtures stay exactly as committed, so a case can inspect the
/// effect of one stored value without inventing a second fixture. Returns false
/// when the document cannot be read, has no <preferences> element, or cannot be
/// written back completely.
bool selectionAmendPreference( QString const & name, QString const & value, bool present = true )
{
  QString const path = Config::getConfigFileName();
  QDomDocument document;

  if( !document.setContent( readFileBytes( path ) ) )
    return false;

  auto preferences = document.documentElement().firstChildElement( "preferences" );

  if( preferences.isNull() )
    return false;

  while( !preferences.firstChildElement( name ).isNull() )
    preferences.removeChild( preferences.firstChildElement( name ) );

  if( present )
  {
    auto element = document.createElement( name );
    element.appendChild( document.createTextNode( value ) );
    preferences.appendChild( element );
  }

  QFile output( path );

  if( !output.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    return false;

  auto const bytes = document.toByteArray();
  return output.write( bytes ) == bytes.size();
}

} // namespace

class TstHotkeys : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase();
  void cleanupTestCase();
  void externalViewerRetiresItsProcessObject();

  // Syntax: parsing, preferred trigger, serialization.
  void parsesSingleCombinationToPreferredTrigger();
  void parsesLegacyTwoKeyLayout();
  void parsesTwoKeyBeforeSingleKeyResetsSecondKey();
  void punctuationMapsToXKeysymNames();
  void namedKeysMapToSpecifiedNames();
  void functionKeysMapIdentically();
  void lettersAndDigitsMapCanonically();
  void modifierOnlyIsRejected();
  void longSequenceIsRejected();
  void emptyAndUnknownTextIsRejected();
  void keypadModifierNeverProducesNum();
  void unknownKeyNeverGetsAnInventedKeysym();
  void backtabBecomesShiftTab();
  void serializeWritesPortableText();

  // Backend capability contract.
  void backendContractSignatures();
  void backendPublishesBindingActivationAndStateContract();
  void hotkeyInstallDecisionTable();
  void bindingSettleAndInFlightClassification();
  void installGateRemembersOnlyIssuedBinds();
  void mainWindowUsesTheSharedInstallGate();
  void bindingExplanationNamesTheConfirmationDialog();
  void unassignedBindingLabelAndConfigureEnablement();
  void configureAvailabilityFollowsBindCompletion();
  void lateCreateSessionSuccessClosesItsSessionAfterCancellation();
  void unsupportedBackendStillReportsState();
  void unsupportedFactory();
  void factorySelection_data();
  void factorySelection();
  void factorySelectionProbe();

  // The X11 recorder (§2.10): platform gate, matcher, core and backend.
  void x11SessionNamesAreDecidedCaseInsensitively_data();
  void x11SessionNamesAreDecidedCaseInsensitively();
  void recorderModifierTrackingUsesTheXProtocolMasks();
  void recorderMatcherActivatesASingleKeyCombination();
  void recorderMatcherHandlesTwoKeyAndRepeatedPresses();
  void recorderCoreGrabsTheSecondKeyAndReleasesItAgain();
  void recorderCoreSecondKeyWindowExpiresAndRestarts();
  void recorderCoreRefusedGrabIsReportedNotFatal();
  void nonXcbCapability();
  void recorderBackendBindsStableIdsAndReportsEmptyTokens();
  void recorderBackendReportsUnbindableSequences();
  void recorderBackendRetiresItsBindings();
  void xcbRecorderDeliversTheCombinationThroughRecord();
  void brokenXRecordCheckToleratesASessionWithoutDisplay();
  void keyboardStateNeverClaimsAModifierWithoutADisplay();
  void shutdownPolicyAbandonsAWedgedRecorderInsteadOfHanging();

  // Legacy configuration loading and the audio backend migration.
  void legacyConfigLoadsWithoutRewriting();
  void legacyHotkeyTextSurvivesRoundTrip();
  void legacyAudioBackendMigratesInMemory();
  void unknownAudioBackendFallsBack();
  void noFfmpegKeepsStoredBackendText();
  void savedAudioBackendFollowsTheBuildVariant();
  void audioFactorySelectsTheAvailablePath();

  // Additive portal binding display records.
  void additivePortalBindingsLoadLastWinsAndDecodeEscapedText();
  void additivePortalBindingsRoundTrip();
  void additiveValuesSurviveRoundTrip();
  void unsupportedShortcutTextSurvivesRoundTrip();
  void recorderEditClearsTheRawTextMarker();
  void preferencesPreserveUnavailableValues();
  void preferencesReportHotkeyCapabilities();
  void preferencesExplainRendererParity();
  void preferencesKeepAudioVariant();

  // Shared portal binding contracts published for the selected-text action.
  void scanPopupBindingIsPortalOnly();
  void portalIdPredicateAcceptsThreeIds();
  void threeBindingsStayBound();
  void capabilityConstantMatchesBuild();
  void scanPopupSourceLoopContractIsPinned();

  // Opt-in selected-text shortcut preference (§5).
  void defaultScanPopupHotkeyIsBindable();
  void scanPopupPortalBindingSurvivesSaveLoad();
  void scanPopupHotkeyKeepsVerbatimText();
  void legacyConfigDefaultsScanPopupHotkeyOff();
  void scanPopupConfigBooleanValues();

private slots:
  void wireBarrier( uint marker ) { seenMarker = marker; }

private:
  /// Copies one fixture to the isolated configuration location Config::load()
  /// reads and returns its absolute path. Returns an empty string on failure.
  QString installConfigFixture( QString const & fixtureFile );

  Config::Class loadInstalledFixture( QString const & fixtureFile );

  /// Saves the given configuration and returns the parsed file Config::save()
  /// actually produced.
  QDomDocument saveAndRead( Config::Class const & configuration );

  QScopedPointer< QTemporaryDir > isolatedHome;
  QByteArray originalHome;
  QByteArray originalConfigHome;
  QByteArray originalDataHome;
  QByteArray originalCacheHome;
  uint nextMarker = 0;
  uint seenMarker = 0;
};

void TstHotkeys::initTestCase()
{
  QStringList reasons;
  QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );

  for( QString const & fixture :
       { QStringLiteral( "config/legacy.xml" ), QStringLiteral( "config/additive.xml" ) } )
  {
    QVERIFY2( TestSupport::fixtureExists( fixture ),
              qPrintable( QStringLiteral( "missing fixture %1" )
                            .arg( TestSupport::fixturePath( fixture ) ) ) );
  }

  qRegisterMetaType< PortalBindingDisplay >();
  qRegisterMetaType< QVector< PortalBindingDisplay > >();
  qRegisterMetaType< HotkeyBackend::State >( "HotkeyBackend::State" );

  // Config::load()/save() are the production entry points and they resolve the
  // user's configuration directory themselves. Isolating HOME and the XDG
  // directories keeps the real code under test without reading or writing the
  // configuration of whoever runs the suite.
  originalHome = qgetenv( "HOME" );
  originalConfigHome = qgetenv( "XDG_CONFIG_HOME" );
  originalDataHome = qgetenv( "XDG_DATA_HOME" );
  originalCacheHome = qgetenv( "XDG_CACHE_HOME" );

  isolatedHome.reset( new QTemporaryDir );
  QVERIFY( isolatedHome->isValid() );

  QByteArray const home = isolatedHome->path().toUtf8();
  qputenv( "HOME", home );
  qputenv( "XDG_CONFIG_HOME", home + "/.config" );
  qputenv( "XDG_DATA_HOME", home + "/.local/share" );
  qputenv( "XDG_CACHE_HOME", home + "/.cache" );

  QVERIFY2( Config::getConfigFileName().startsWith( isolatedHome->path() ),
            qPrintable( QStringLiteral( "config path escaped the isolated home: %1" )
                          .arg( Config::getConfigFileName() ) ) );

  TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
}

void TstHotkeys::externalViewerRetiresItsProcessObject()
{
  // Qt 6 renamed QProcess::error() to errorOccurred(). String-based connects to the
  // old name fail silently at run time, so the regression is invisible except as a
  // Qt warning. The external viewer is the caller that was broken by it.
  static QStringList warnings;
  warnings.clear();

  QtMessageHandler const previous = qInstallMessageHandler(
    []( QtMsgType type, QMessageLogContext const &, QString const & text ) {
      if ( type != QtDebugMsg && type != QtInfoMsg )
        warnings.append( text );
    } );

  QPointer< ExternalViewer > viewer;
  bool started = false;
  QString startFailure;
  try
  {
    viewer = new ExternalViewer( "payload", 7, QStringLiteral( "txt" ), QStringLiteral( "/bin/true" ) );
    viewer->start();
    started = true;
  }
  catch( ExternalViewer::exCantRunViewer const & error )
  {
    startFailure = QString::fromUtf8( error.what() );
  }

  bool const retired = started && QTest::qWaitFor( [ &viewer ] { return viewer.isNull(); }, 15000 );

  qInstallMessageHandler( previous );

  QVERIFY2( started, qPrintable( "the external viewer could not start: " + startFailure ) );
  QVERIFY2( retired, "the external viewer was not retired after its process finished" );
  QVERIFY2( !warnings.join( '\n' ).contains( QStringLiteral( "No such signal" ) ),
            qPrintable( "a Qt connection to a renamed signal failed: " + warnings.join( QStringLiteral( " | " ) ) ) );
}

void TstHotkeys::cleanupTestCase()
{
  TestSupport::clearWatchdog();

  if( originalHome.isNull() )
    qunsetenv( "HOME" );
  else
    qputenv( "HOME", originalHome );

  if( originalConfigHome.isNull() )
    qunsetenv( "XDG_CONFIG_HOME" );
  else
    qputenv( "XDG_CONFIG_HOME", originalConfigHome );

  if( originalDataHome.isNull() )
    qunsetenv( "XDG_DATA_HOME" );
  else
    qputenv( "XDG_DATA_HOME", originalDataHome );

  if( originalCacheHome.isNull() )
    qunsetenv( "XDG_CACHE_HOME" );
  else
    qputenv( "XDG_CACHE_HOME", originalCacheHome );

  isolatedHome.reset();
}

QString TstHotkeys::installConfigFixture( QString const & fixtureFile )
{
  Q_ASSERT( isolatedHome );

  QString const configDir = QDir( isolatedHome->path() ).filePath( QStringLiteral( ".config/goldendict" ) );
  if( !QDir().mkpath( configDir ) )
    return QString();

  QString const target = QDir( configDir ).filePath( QStringLiteral( "config" ) );
  QFile::remove( target );
  QFile::remove( target + QStringLiteral( ".tmp" ) );

  if( !QFile::copy( TestSupport::fixturePath( fixtureFile ), target ) )
    return QString();

  return target;
}

Config::Class TstHotkeys::loadInstalledFixture( QString const & fixtureFile )
{
  QString const installed = installConfigFixture( fixtureFile );
  if( installed.isEmpty() )
    return Config::Class();

  return Config::load();
}

QDomDocument TstHotkeys::saveAndRead( Config::Class const & configuration )
{
  QDomDocument document;
  Config::save( configuration );

  QByteArray const saved = readFileBytes( Config::getConfigFileName() );
  if( saved.isEmpty() )
    return document;

  document.setContent( saved );
  return document;
}

// ---------------------------------------------------------------------------
// Syntax
// ---------------------------------------------------------------------------

void TstHotkeys::parsesSingleCombinationToPreferredTrigger()
{
  Config::HotKey key;
  QString trigger;
  QString error;

  // The two oracles the task contract names.
  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+Alt+G" ), &key, &error ) );
  QVERIFY2( error.isEmpty(), qPrintable( error ) );
  QVERIFY( HotkeySyntax::preferredTrigger( key, &trigger, &error ) );
  QCOMPARE( trigger, QStringLiteral( "CTRL+ALT+g" ) );

  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Meta+Shift+F5" ), &key, &error ) );
  QVERIFY( HotkeySyntax::preferredTrigger( key, &trigger, &error ) );
  QCOMPARE( trigger, QStringLiteral( "SHIFT+LOGO+F5" ) );

  // Canonical modifier order, independent of the order the user typed.
  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Meta+Ctrl+Alt+Shift+G" ), &key, &error ) );
  QVERIFY( HotkeySyntax::preferredTrigger( key, &trigger, &error ) );
  QCOMPARE( trigger, QStringLiteral( "CTRL+ALT+SHIFT+LOGO+g" ) );

  // A single key needs no modifier.
  QVERIFY( HotkeySyntax::parse( QStringLiteral( "F11" ), &key, &error ) );
  QVERIFY( HotkeySyntax::preferredTrigger( key, &trigger, &error ) );
  QCOMPARE( trigger, QStringLiteral( "F11" ) );

  // Ctrl++ is a real, bindable combination; it is not a separator accident.
  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl++" ), &key, &error ) );
  QVERIFY( HotkeySyntax::preferredTrigger( key, &trigger, &error ) );
  QCOMPARE( trigger, QStringLiteral( "CTRL+plus" ) );

  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+," ), &key, &error ) );
  QVERIFY( HotkeySyntax::preferredTrigger( key, &trigger, &error ) );
  QCOMPARE( trigger, QStringLiteral( "CTRL+comma" ) );
}

void TstHotkeys::parsesLegacyTwoKeyLayout()
{
  // The legacy stored layout: modifiers live in the first combination only, the
  // second one contributes its key, and rendering puts the modifiers back on it.
  Config::HotKey key;
  QString error;

  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+F11,F11" ), &key, &error ) );
  QCOMPARE( key.modifiers, Qt::KeyboardModifiers( Qt::ControlModifier ) );
  QCOMPARE( key.key1, int( Qt::Key_F11 ) );
  QCOMPARE( key.key2, int( Qt::Key_F11 ) );

  QKeySequence const rendered = key.toKeySequence();
  QCOMPARE( rendered.count(), 2 );
  QCOMPARE( rendered.toString( QKeySequence::PortableText ),
            QStringLiteral( "Ctrl+F11, Ctrl+F11" ) );

  // A two-key sequence is preserved but is not a portal trigger.
  QString trigger = QStringLiteral( "untouched" );
  QString reason;
  QVERIFY( !HotkeySyntax::preferredTrigger( key, &trigger, &reason ) );
  QVERIFY( !reason.isEmpty() );
  QCOMPARE( trigger, QStringLiteral( "untouched" ) );
}

void TstHotkeys::parsesTwoKeyBeforeSingleKeyResetsSecondKey()
{
  Config::HotKey key;
  QString error;

  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+C,C" ), &key, &error ) );
  QCOMPARE( key.key1, int( Qt::Key_C ) );
  QCOMPARE( key.key2, int( Qt::Key_C ) );

  // The same output variable is reused exactly as a caller would: a successful
  // parse fully describes the new value, it never keeps the previous key2.
  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+Alt+G" ), &key, &error ) );
  QCOMPARE( key.key1, int( Qt::Key_G ) );
  QCOMPARE( key.key2, 0 );
  QCOMPARE( key.modifiers, Qt::KeyboardModifiers( Qt::ControlModifier | Qt::AltModifier ) );
}

void TstHotkeys::punctuationMapsToXKeysymNames()
{
  struct Row
  {
    Qt::Key key;
    char const * expectedName;
  };

  Row const rows[] = {
    { Qt::Key_Plus, "plus" },
    { Qt::Key_Comma, "comma" },
    { Qt::Key_Minus, "minus" },
    { Qt::Key_Period, "period" },
    { Qt::Key_Slash, "slash" },
    { Qt::Key_Semicolon, "semicolon" },
    { Qt::Key_Colon, "colon" },
    { Qt::Key_Equal, "equal" },
    { Qt::Key_BracketLeft, "bracketleft" },
    { Qt::Key_BracketRight, "bracketright" },
    { Qt::Key_Backslash, "backslash" },
    { Qt::Key_Apostrophe, "apostrophe" },
    { Qt::Key_QuoteDbl, "quotedbl" },
    { Qt::Key_QuoteLeft, "grave" },
    { Qt::Key_AsciiTilde, "asciitilde" },
    { Qt::Key_Exclam, "exclam" },
    { Qt::Key_At, "at" },
    { Qt::Key_NumberSign, "numbersign" },
    { Qt::Key_Dollar, "dollar" },
    { Qt::Key_Percent, "percent" },
    { Qt::Key_AsciiCircum, "asciicircum" },
    { Qt::Key_Ampersand, "ampersand" },
    { Qt::Key_Asterisk, "asterisk" },
    { Qt::Key_ParenLeft, "parenleft" },
    { Qt::Key_ParenRight, "parenright" },
    { Qt::Key_Underscore, "underscore" },
    { Qt::Key_BraceLeft, "braceleft" },
    { Qt::Key_BraceRight, "braceright" },
    { Qt::Key_Less, "less" },
    { Qt::Key_Greater, "greater" },
    { Qt::Key_Question, "question" },
    { Qt::Key_Bar, "bar" },
    { Qt::Key_Space, "space" },
  };

  for( Row const & row : rows )
  {
    // Parse through the production parser from the PortableText a stored value
    // would contain, then convert with the production trigger serializer.
    QString const stored = QStringLiteral( "Ctrl+" )
                           + QKeySequence( row.key ).toString( QKeySequence::PortableText );

    Config::HotKey key;
    QString trigger;
    QString error;
    QString const expected = QStringLiteral( "CTRL+" ) + QString::fromLatin1( row.expectedName );

    QVERIFY2( HotkeySyntax::parse( stored, &key, &error ), qPrintable( stored + ": " + error ) );
    QVERIFY2( HotkeySyntax::preferredTrigger( key, &trigger, &error ),
              qPrintable( stored + ": " + error ) );
    QCOMPARE( trigger, expected );
  }
}

void TstHotkeys::namedKeysMapToSpecifiedNames()
{
  struct Row
  {
    Qt::Key key;
    char const * expectedName;
  };

  Row const rows[] = {
    { Qt::Key_Escape, "Escape" },
    { Qt::Key_Tab, "Tab" },
    { Qt::Key_Backspace, "BackSpace" },
    { Qt::Key_Return, "Return" },
    { Qt::Key_Insert, "Insert" },
    { Qt::Key_Delete, "Delete" },
    { Qt::Key_Home, "Home" },
    { Qt::Key_End, "End" },
    { Qt::Key_Left, "Left" },
    { Qt::Key_Right, "Right" },
    { Qt::Key_Up, "Up" },
    { Qt::Key_Down, "Down" },
    { Qt::Key_PageUp, "Prior" },
    { Qt::Key_PageDown, "Next" },
    { Qt::Key_Pause, "Pause" },
    { Qt::Key_Print, "Print" },
    { Qt::Key_Menu, "Menu" },
  };

  for( Row const & row : rows )
  {
    QString const stored = QStringLiteral( "Ctrl+" )
                           + QKeySequence( row.key ).toString( QKeySequence::PortableText );

    Config::HotKey key;
    QString trigger;
    QString error;
    QString const expected = QStringLiteral( "CTRL+" ) + QString::fromLatin1( row.expectedName );

    QVERIFY2( HotkeySyntax::parse( stored, &key, &error ), qPrintable( stored + ": " + error ) );
    QVERIFY2( HotkeySyntax::preferredTrigger( key, &trigger, &error ),
              qPrintable( stored + ": " + error ) );
    QCOMPARE( trigger, expected );
  }

  // The keypad Enter key has no specified portal name.
  Config::HotKey keypadEnter;
  keypadEnter.key1 = int( Qt::Key_Enter );
  keypadEnter.modifiers = Qt::ControlModifier;
  QString trigger = QStringLiteral( "untouched" );
  QString reason;
  QVERIFY( !HotkeySyntax::preferredTrigger( keypadEnter, &trigger, &reason ) );
  QVERIFY( !reason.isEmpty() );
  QCOMPARE( trigger, QStringLiteral( "untouched" ) );
}

void TstHotkeys::functionKeysMapIdentically()
{
  for( int number = 1; number <= 35; ++number )
  {
    Qt::Key const qtKey = Qt::Key( int( Qt::Key_F1 ) + number - 1 );
    QString const stored = QKeySequence( qtKey ).toString( QKeySequence::PortableText );

    Config::HotKey key;
    QString trigger;
    QString error;

    QVERIFY2( HotkeySyntax::parse( stored, &key, &error ), qPrintable( stored + ": " + error ) );
    QVERIFY2( HotkeySyntax::preferredTrigger( key, &trigger, &error ),
              qPrintable( stored + ": " + error ) );
    QCOMPARE( trigger, QStringLiteral( "F%1" ).arg( number ) );
  }

  // One beyond the supported range: no invented key name.
  Config::HotKey beyond;
  beyond.key1 = int( Qt::Key_F1 ) + 35;
  QString trigger = QStringLiteral( "untouched" );
  QString reason;
  QVERIFY( !HotkeySyntax::preferredTrigger( beyond, &trigger, &reason ) );
  QVERIFY( !reason.isEmpty() );
}

void TstHotkeys::lettersAndDigitsMapCanonically()
{
  for( int letter = 0; letter < 26; ++letter )
  {
    Qt::Key const qtKey = Qt::Key( int( Qt::Key_A ) + letter );
    Config::HotKey key;
    QString trigger;
    QString error;

    QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+" )
                                    + QKeySequence( qtKey ).toString( QKeySequence::PortableText ),
                                  &key, &error ) );
    QVERIFY( HotkeySyntax::preferredTrigger( key, &trigger, &error ) );
    QCOMPARE( trigger, QStringLiteral( "CTRL+" ) + QChar( 'a' + letter ) );
  }

  // SHIFT is a modifier, the letter stays lowercase.
  Config::HotKey shifted;
  QString trigger;
  QString error;
  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+Shift+Z" ), &shifted, &error ) );
  QVERIFY( HotkeySyntax::preferredTrigger( shifted, &trigger, &error ) );
  QCOMPARE( trigger, QStringLiteral( "CTRL+SHIFT+z" ) );

  for( int digit = 0; digit <= 9; ++digit )
  {
    Qt::Key const qtKey = Qt::Key( int( Qt::Key_0 ) + digit );
    Config::HotKey key;
    QString digitTrigger;
    QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+" )
                                    + QKeySequence( qtKey ).toString( QKeySequence::PortableText ),
                                  &key, &error ) );
    QVERIFY( HotkeySyntax::preferredTrigger( key, &digitTrigger, &error ) );
    QCOMPARE( digitTrigger, QStringLiteral( "CTRL+%1" ).arg( digit ) );
  }
}

void TstHotkeys::modifierOnlyIsRejected()
{
  Config::HotKey key;
  QString error;
  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+Shift" ), &key, &error ) );

  QString trigger = QStringLiteral( "untouched" );
  QString reason;
  QVERIFY( !HotkeySyntax::preferredTrigger( key, &trigger, &reason ) );
  QVERIFY( !reason.isEmpty() );
  QCOMPARE( trigger, QStringLiteral( "untouched" ) );

  // An empty stored value is not parseable at all.
  Config::HotKey untouched;
  untouched.key1 = int( Qt::Key_G );
  QVERIFY( !HotkeySyntax::parse( QString(), &untouched, &error ) );
  QVERIFY( !error.isEmpty() );
  QCOMPARE( untouched.key1, int( Qt::Key_G ) );
}

void TstHotkeys::longSequenceIsRejected()
{
  Config::HotKey key;
  QString error;
  QVERIFY( !HotkeySyntax::parse( QStringLiteral( "Ctrl+F11,F11,F11" ), &key, &error ) );
  QVERIFY( !error.isEmpty() );
}

void TstHotkeys::emptyAndUnknownTextIsRejected()
{
  Config::HotKey key;
  key.key1 = int( Qt::Key_G );

  QString error;
  QVERIFY( !HotkeySyntax::parse( QStringLiteral( "garbageText" ), &key, &error ) );
  QVERIFY( !error.isEmpty() );
  QCOMPARE( key.key1, int( Qt::Key_G ) );

  // A dangling modifier word is not a shortcut either.
  QVERIFY( !HotkeySyntax::parse( QStringLiteral( "Ctrl" ), &key, &error ) );
  QVERIFY( !error.isEmpty() );
  QCOMPARE( key.key1, int( Qt::Key_G ) );

  QVERIFY( !HotkeySyntax::parse( QString(), &key, &error ) );
  QVERIFY( !error.isEmpty() );
  QCOMPARE( key.key1, int( Qt::Key_G ) );
}

void TstHotkeys::keypadModifierNeverProducesNum()
{
  Config::HotKey key;
  QString error;

  // QKeySequence maps the keypad modifier; the portal format has no NUM.
  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+Num+1" ), &key, &error ) );
  QVERIFY( key.modifiers & Qt::KeypadModifier );

  QString trigger = QStringLiteral( "untouched" );
  QString reason;
  QVERIFY( !HotkeySyntax::preferredTrigger( key, &trigger, &reason ) );
  QVERIFY( !reason.isEmpty() );
  QCOMPARE( trigger, QStringLiteral( "untouched" ) );

  Config::HotKey constructed;
  constructed.modifiers = Qt::KeypadModifier | Qt::ControlModifier;
  constructed.key1 = int( Qt::Key_1 );
  QVERIFY( !HotkeySyntax::preferredTrigger( constructed, &trigger, &reason ) );
  QVERIFY( !trigger.contains( QStringLiteral( "NUM" ) ) );
}

void TstHotkeys::unknownKeyNeverGetsAnInventedKeysym()
{
  Config::HotKey key;
  key.modifiers = Qt::ControlModifier;
  key.key1 = int( Qt::Key_MediaTogglePlayPause );

  QString trigger = QStringLiteral( "untouched" );
  QString reason;
  QVERIFY( !HotkeySyntax::preferredTrigger( key, &trigger, &reason ) );
  QVERIFY( !reason.isEmpty() );
  QCOMPARE( trigger, QStringLiteral( "untouched" ) );

  key.key1 = int( Qt::Key_ZoomIn );
  QVERIFY( !HotkeySyntax::preferredTrigger( key, &trigger, &reason ) );
  QCOMPARE( trigger, QStringLiteral( "untouched" ) );
}

void TstHotkeys::backtabBecomesShiftTab()
{
  Config::HotKey key;
  QString trigger;
  QString error;

  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+Backtab" ), &key, &error ) );
  QVERIFY( HotkeySyntax::preferredTrigger( key, &trigger, &error ) );
  QCOMPARE( trigger, QStringLiteral( "CTRL+SHIFT+Tab" ) );

  // The implicit SHIFT is merged once, not duplicated.
  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Shift+Backtab" ), &key, &error ) );
  QVERIFY( HotkeySyntax::preferredTrigger( key, &trigger, &error ) );
  QCOMPARE( trigger, QStringLiteral( "SHIFT+Tab" ) );

  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl+Shift+Tab" ), &key, &error ) );
  QVERIFY( HotkeySyntax::preferredTrigger( key, &trigger, &error ) );
  QCOMPARE( trigger, QStringLiteral( "CTRL+SHIFT+Tab" ) );
}

void TstHotkeys::serializeWritesPortableText()
{
  Config::HotKey key;
  QString error;

  QVERIFY( HotkeySyntax::parse( QStringLiteral( "Ctrl++" ), &key, &error ) );
  QCOMPARE( HotkeySyntax::serialize( key ), QStringLiteral( "Ctrl++" ) );

  QVERIFY( HotkeySyntax::parse( QStringLiteral( "ctrl+alt+g" ), &key, &error ) );
  QCOMPARE( HotkeySyntax::serialize( key ), QStringLiteral( "Ctrl+Alt+G" ) );

  Config::HotKey const twoKey( QKeySequence( QStringLiteral( "Ctrl+C,C" ) ) );
  QCOMPARE( HotkeySyntax::serialize( twoKey ), QStringLiteral( "Ctrl+C, Ctrl+C" ) );

  QCOMPARE( HotkeySyntax::serialize( Config::HotKey() ), QString() );
}

// ---------------------------------------------------------------------------
// Backend contract
// ---------------------------------------------------------------------------

void TstHotkeys::hotkeyInstallDecisionTable()
{
  using State = HotkeyBackend::State;
  using Intent = HotkeyInstallIntent;
  struct Row
  {
    State state;
    char const * name;
    bool expected[2][2]; // [unchangedSet][UserAction], with a backend present.
  };
  // Literal expectations, independent of the classifiers and decision helper.
  Row const rows[] = {
    { State::Unsupported, "Unsupported", { { false, false }, { false, false } } },
    { State::Creating, "Creating", { { true, true }, { false, false } } },
    { State::Ready, "Ready", { { true, true }, { false, true } } },
    { State::Binding, "Binding", { { true, true }, { false, false } } },
    { State::Bound, "Bound", { { true, true }, { false, false } } },
    { State::Partial, "Partial", { { true, true }, { false, false } } },
    { State::Denied, "Denied", { { true, true }, { false, true } } },
    { State::Unavailable, "Unavailable", { { true, true }, { false, true } } },
    { State::Closed, "Closed", { { true, true }, { false, true } } }
  };
  int combinations = 0;
  for( Row const & row : rows )
    for( bool backendPresent : { false, true } )
      for( bool unchangedSet : { false, true } )
        for( Intent intent : { Intent::Refresh, Intent::UserAction } )
        {
          bool const expected = !backendPresent || row.expected[unchangedSet][intent == Intent::UserAction];
          bool const actual = hotkeyInstallShouldBind( backendPresent, unchangedSet, row.state, intent );
          QString const context = QStringLiteral( "%1 backendPresent=%2 unchangedSet=%3 intent=%4 expected=%5 actual=%6" )
            .arg( QLatin1String( row.name ) ).arg( backendPresent ).arg( unchangedSet )
            .arg( intent == Intent::UserAction ? "UserAction" : "Refresh" ).arg( expected ).arg( actual );
          QVERIFY2( actual == expected, qPrintable( context ) );
          ++combinations;
        }
  QCOMPARE( combinations, 72 );

  QVERIFY( hotkeyInstallShouldBind( true, false, State::Creating, Intent::Refresh ) );
  QVERIFY( hotkeyInstallShouldBind( true, false, State::Binding, Intent::Refresh ) );
  QVERIFY( !hotkeyInstallShouldBind( true, false, State::Unsupported, Intent::UserAction ) );
  QVERIFY( hotkeyInstallShouldBind( true, false, State::Bound, Intent::Refresh ) );
}

void TstHotkeys::bindingSettleAndInFlightClassification()
{
  using State = HotkeyBackend::State;
  struct Row { State state; bool settled; bool inFlight; };
  Row const rows[] = {
    { State::Unsupported, true, false },
    { State::Creating, false, true },
    { State::Ready, false, false },
    { State::Binding, false, true },
    { State::Bound, true, false },
    { State::Partial, true, false },
    { State::Denied, false, false },
    { State::Unavailable, false, false },
    { State::Closed, false, false }
  };
  for( Row const & row : rows )
  {
    QCOMPARE( hotkeyBindingSettles( row.state ), row.settled );
    QCOMPARE( hotkeyBindingInFlight( row.state ), row.inFlight );
    QVERIFY( !( hotkeyBindingSettles( row.state ) && hotkeyBindingInFlight( row.state ) ) );
  }
}

void TstHotkeys::installGateRemembersOnlyIssuedBinds()
{
  using State = HotkeyBackend::State;
  using Intent = HotkeyInstallIntent;
  QVector< HotkeyBinding > const first{ { "main-window", "Main window", "CTRL+ALT+g" } };
  QVector< HotkeyBinding > const edited{ { "main-window", "Main window", "CTRL+ALT+h" } };
  HotkeyInstallGate gate;

  // An unprimed empty list still issues a bind, and that true result is remembered.
  QVERIFY( gate.shouldBind( true, {}, State::Bound, Intent::Refresh ) );
  QVERIFY( !gate.shouldBind( true, {}, State::Bound, Intent::Refresh ) );
  gate.reset();
  QVERIFY( gate.shouldBind( true, {}, State::Bound, Intent::Refresh ) );

  QVERIFY( gate.shouldBind( true, first, State::Bound, Intent::Refresh ) );
  QVERIFY( !gate.shouldBind( true, first, State::Bound, Intent::Refresh ) );
  QVERIFY( !gate.shouldBind( true, edited, State::Unsupported, Intent::UserAction ) );
  // A refused edit must not replace the list of the last issued bind.
  QVERIFY( gate.shouldBind( true, edited, State::Bound, Intent::Refresh ) );
  QVERIFY( !gate.shouldBind( true, edited, State::Binding, Intent::UserAction ) );
  QVERIFY( gate.shouldBind( true, first, State::Binding, Intent::Refresh ) );
  QVERIFY( !gate.shouldBind( true, first, State::Denied, Intent::Refresh ) );
  QVERIFY( gate.shouldBind( true, first, State::Denied, Intent::UserAction ) );

  // The existing comparison uses stable IDs and sequences, not localized labels.
  auto relabeled = first;
  relabeled[0].description = "Localized main window";
  QVERIFY( !gate.shouldBind( true, relabeled, State::Bound, Intent::Refresh ) );
  auto otherId = first;
  otherId[0].id = "clipboard-lookup";
  QVERIFY( gate.shouldBind( true, otherId, State::Bound, Intent::Refresh ) );
  auto both = first;
  both.append( otherId[0] );
  QVERIFY( gate.shouldBind( true, both, State::Bound, Intent::Refresh ) );

  gate.reset();
  QVERIFY( gate.shouldBind( true, both, State::Bound, Intent::Refresh ) );
  gate.reset();
  QVERIFY( !gate.shouldBind( true, first, State::Unsupported, Intent::Refresh ) );
  // A false result on an unprimed gate must not prime it either.
  QVERIFY( gate.shouldBind( true, first, State::Bound, Intent::Refresh ) );
  QVERIFY( gate.shouldBind( false, first, State::Unsupported, Intent::Refresh ) );
}

void TstHotkeys::mainWindowUsesTheSharedInstallGate()
{
  QFile source( TestSupport::repositoryRoot() + "/mainwindow.cc" );
  QVERIFY2( source.open( QIODevice::ReadOnly ), qPrintable( source.errorString() ) );
  QByteArray const contents = source.readAll();
  QCOMPARE( contents.count( "installGate.shouldBind(" ), 1 );
  QVERIFY( !contents.contains( "requestedHotkeys" ) );
}

void TstHotkeys::backendContractSignatures()
{
  requireBackendContract();

  RecordingHotkeyBackend backend( true );
  QVERIFY( backend.isSupported() );
  QCOMPARE( backend.state(), HotkeyBackend::State::Unsupported );

  // The signal signatures are part of the contract: a QSignalSpy over a
  // different arity or argument type does not compile.
  QSignalSpy activated( &backend, &HotkeyBackend::activated );
  QSignalSpy bindingsChanged( &backend, &HotkeyBackend::bindingsChanged );
  QSignalSpy stateChanged( &backend, &HotkeyBackend::stateChanged );

  QVERIFY( activated.isValid() );
  QVERIFY( bindingsChanged.isValid() );
  QVERIFY( stateChanged.isValid() );
}

void TstHotkeys::backendPublishesBindingActivationAndStateContract()
{
  RecordingHotkeyBackend backend( true );

  QSignalSpy activated( &backend, &HotkeyBackend::activated );
  QSignalSpy bindingsChanged( &backend, &HotkeyBackend::bindingsChanged );
  QSignalSpy stateChanged( &backend, &HotkeyBackend::stateChanged );

  QVector< HotkeyBinding > const requested{
    HotkeyBinding{ QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                   QStringLiteral( "CTRL+ALT+g" ) },
    HotkeyBinding{ QStringLiteral( "clipboard-lookup" ), QStringLiteral( "Clipboard lookup" ),
                   QStringLiteral( "CTRL+ALT+c" ) },
  };

  backend.bind( requested );

  QCOMPARE( backend.bindCalls, 1 );
  QCOMPARE( backend.requested.size(), requested.size() );
  QCOMPARE( backend.requested.at( 0 ).id, QStringLiteral( "main-window" ) );
  QCOMPARE( backend.requested.at( 0 ).sequence, QStringLiteral( "CTRL+ALT+g" ) );

  QCOMPARE( activated.count(), 1 );
  QCOMPARE( activated.at( 0 ).at( 0 ).toString(), QStringLiteral( "main-window" ) );
  QCOMPARE( activated.at( 0 ).at( 1 ).toString(), QStringLiteral( "activation-token" ) );

  QCOMPARE( bindingsChanged.count(), 1 );
  QVector< PortalBindingDisplay > const reported =
    bindingsChanged.at( 0 ).at( 0 ).value< QVector< PortalBindingDisplay > >();
  QCOMPARE( reported.size(), 2 );
  QCOMPARE( reported.at( 0 ).id, QStringLiteral( "main-window" ) );
  QCOMPARE( reported.at( 0 ).triggerDescription, QStringLiteral( "Assigned by portal" ) );

  QCOMPARE( stateChanged.count(), 2 );
  QCOMPARE( stateChanged.at( 0 ).at( 0 ).value< HotkeyBackend::State >(),
            HotkeyBackend::State::Binding );
  QCOMPARE( stateChanged.at( 1 ).at( 0 ).value< HotkeyBackend::State >(),
            HotkeyBackend::State::Partial );
  QVERIFY( !stateChanged.at( 1 ).at( 1 ).toString().isEmpty() );

  backend.unbind();
  QCOMPARE( backend.unbindCalls, 1 );
  QCOMPARE( backend.state(), HotkeyBackend::State::Closed );
  QCOMPARE( stateChanged.count(), 3 );
}

void TstHotkeys::bindingExplanationNamesTheConfirmationDialog()
{
  TestSupport::PrivateSessionBus privateBus;
  QString reason;
  QVERIFY2( privateBus.start( &reason ), qPrintable( reason ) );
  TestSupport::FakePortal portal;
  QVERIFY2( portal.start( privateBus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );
  portal.holdResponse( QStringLiteral( "BindShortcuts" ) );
  QString bindingExplanation;
  PortalBackend backend( privateBus.connection() );
  connect( &backend, &HotkeyBackend::stateChanged, this,
           [ & ]( HotkeyBackend::State state, QString const & explanation ) {
    if( state == HotkeyBackend::State::Binding )
      bindingExplanation = explanation;
  } );
  backend.bind( { { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                    QStringLiteral( "CTRL+ALT+g" ) } } );
  QVERIFY( QTest::qWaitFor( [ & ] {
    return !bindingExplanation.isEmpty() && portal.count( QStringLiteral( "BindShortcuts" ) ) == 1;
  }, 1500 ) );
  QCOMPARE( backend.state(), HotkeyBackend::State::Binding );
  QCOMPARE( bindingExplanation,
            QStringLiteral( "Binding portal shortcuts: waiting for the portal confirmation dialog" ) );

  Config::Class cfg;
  cfg.preferences.enableMainWindowHotkey = true;
  cfg.preferences.mainWindowHotkey = Config::HotKey( QKeySequence( "Ctrl+Alt+G" ) );
  Preferences dialog( nullptr, cfg );
  auto capability = dialog.findChild< QLabel * >( "hotkeyCapability" );
  auto main = dialog.findChild< QLabel * >( "mainWindowHotkeyStatus" );
  QVERIFY( capability && main );
  dialog.setHotkeyStatus( "wayland", backend.state(), bindingExplanation, {},
                         backend.isConfigureAvailable() );
  QVERIFY( capability->text().contains( bindingExplanation ) );
  QCOMPARE( main->text(), bindingExplanation );
}

void TstHotkeys::unassignedBindingLabelAndConfigureEnablement()
{
  using State = HotkeyBackend::State;
  Config::Class cfg;
  cfg.preferences.enableMainWindowHotkey = true;
  cfg.preferences.enableClipboardHotkey = true;
  cfg.preferences.mainWindowHotkey = Config::HotKey( QKeySequence( "Ctrl+Alt+G" ) );
  cfg.preferences.clipboardHotkey = Config::HotKey( QKeySequence( "Ctrl+Alt+H" ) );
  Preferences dialog( nullptr, cfg );
  auto main = dialog.findChild< QLabel * >( "mainWindowHotkeyStatus" );
  auto clipboard = dialog.findChild< QLabel * >( "clipboardHotkeyStatus" );
  auto configure = dialog.findChild< QPushButton * >( "configureShortcuts" );
  QVERIFY( main && clipboard && configure );
  QString const unassigned = QStringLiteral( "Not assigned by the portal — use Configure shortcuts" );

  TestSupport::PrivateSessionBus privateBus;
  QString reason;
  QVERIFY2( privateBus.start( &reason ), qPrintable( reason ) );
  TestSupport::FakePortal portal;
  QVERIFY2( portal.start( privateBus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );
  portal.setBoundShortcuts( {
    { QStringLiteral( "main-window" ), { { QStringLiteral( "trigger_description" ), QStringLiteral( "Portal main trigger" ) } } },
    { QStringLiteral( "clipboard-lookup" ), { { QStringLiteral( "trigger_description" ), QStringLiteral( "Portal clipboard trigger" ) } } }
  } );
  QString explanation;
  QVector< PortalBindingDisplay > reported;
  PortalBackend backend( privateBus.connection() );
  connect( &backend, &HotkeyBackend::stateChanged, this,
           [ & ]( State, QString const & value ) { explanation = value; } );
  connect( &backend, &HotkeyBackend::bindingsChanged, this,
           [ & ]( QVector< PortalBindingDisplay > const & values ) { reported = values; } );
  auto render = [ & ] {
    dialog.setHotkeyStatus( "wayland", backend.state(), explanation, reported,
                           backend.isConfigureAvailable() );
  };
  QVector< HotkeyBinding > const desired{
    { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ), QStringLiteral( "CTRL+ALT+g" ) },
    { QStringLiteral( "clipboard-lookup" ), QStringLiteral( "Clipboard" ), QStringLiteral( "CTRL+ALT+h" ) }
  };

  // These false values come from production, so a predicate that always returns
  // true cannot hide behind a table of hand-supplied UI capability booleans.
  QCOMPARE( backend.state(), State::Creating );
  QVERIFY( !backend.isConfigureAvailable() );
  render();
  QVERIFY( !configure->isEnabled() );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Ready; }, 1500 ) );
  render();
  QVERIFY( !configure->isEnabled() );
  portal.holdResponse( QStringLiteral( "BindShortcuts" ) );
  backend.bind( desired );
  QVERIFY( QTest::qWaitFor( [ & ] {
    return backend.state() == State::Binding && portal.count( QStringLiteral( "BindShortcuts" ) ) == 1;
  }, 1500 ) );
  render();
  QVERIFY( !configure->isEnabled() );
  portal.releaseHeldResponse( QStringLiteral( "BindShortcuts" ) );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Bound; }, 1500 ) );
  render();
  QVERIFY( configure->isEnabled() );
  QCOMPARE( main->text(), QStringLiteral( "Portal main trigger" ) );
  QCOMPARE( clipboard->text(), QStringLiteral( "Portal clipboard trigger" ) );

  portal.setBoundShortcuts( {
    { QStringLiteral( "main-window" ), { { QStringLiteral( "trigger_description" ), QStringLiteral( "Portal main trigger" ) } } },
    { QStringLiteral( "clipboard-lookup" ), { { QStringLiteral( "trigger_description" ), QString() } } }
  } );
  backend.bind( desired );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Partial; }, 1500 ) );
  render();
  QVERIFY( configure->isEnabled() );
  QCOMPARE( main->text(), QStringLiteral( "Portal main trigger" ) );
  QCOMPARE( clipboard->text(), unassigned );

  portal.setScenario( QStringLiteral( "bind-denied" ) );
  backend.bind( desired );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Denied; }, 1500 ) );
  render();
  QVERIFY( configure->isEnabled() );
  QCOMPARE( main->text(), unassigned );
  QCOMPARE( clipboard->text(), unassigned );

  portal.setScenario( QStringLiteral( "denied" ) );
  backend.bind( desired );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Denied; }, 1500 ) );
  QVERIFY( !backend.isConfigureAvailable() );
  render();
  QVERIFY( !configure->isEnabled() );
  QCOMPARE( main->text(), explanation );

  portal.setScenario( QStringLiteral( "fast" ) );
  backend.bind( desired );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Partial; }, 1500 ) );
  portal.emitSessionClosed( portal.sessionPath(), QStringLiteral( "fixture closed" ) );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Unavailable; }, 1500 ) );
  render();
  QVERIFY( !configure->isEnabled() );
  QCOMPARE( main->text(), explanation );
  backend.unbind();
  QCOMPARE( backend.state(), State::Closed );
  render();
  QVERIFY( !configure->isEnabled() );
  QCOMPARE( main->text(), explanation );

  RecordingHotkeyBackend unsupported( false );
  dialog.setHotkeyStatus( "wayland", State::Unsupported, QStringLiteral( "platform unsupported" ), {},
                         unsupported.isConfigureAvailable() );
  QVERIFY( !configure->isEnabled() );
  // Even a stale Bound/Partial display cannot override a false live capability.
  for( State state : { State::Bound, State::Partial } )
  {
    dialog.setHotkeyStatus( "wayland", state, {}, {}, backend.isConfigureAvailable() );
    QVERIFY( !configure->isEnabled() );
  }
  dialog.setHotkeyStatus( "xcb", State::Bound, {}, {}, true );
  QVERIFY( !configure->isEnabled() );
}

void TstHotkeys::configureAvailabilityFollowsBindCompletion()
{
  using State = HotkeyBackend::State;
  TestSupport::PrivateSessionBus privateBus;
  QString reason;
  QVERIFY2( privateBus.start( &reason ), qPrintable( reason ) );
  auto portal = std::make_unique< TestSupport::FakePortal >();
  QVERIFY2( portal->start( privateBus.fixtureConnection(), 2 ), qPrintable( portal->errorString() ) );
  QDBusConnection client = privateBus.connection();
  QVERIFY( client.connect( QString(), QStringLiteral( "/gd_test" ),
                           QStringLiteral( "org.goldendict.Test" ), QStringLiteral( "Barrier" ),
                           this, SLOT( wireBarrier( uint ) ) ) );
  auto flushWire = [ this ]( QDBusConnection const & sender ) {
    const uint marker = ++nextMarker;
    QDBusMessage message = QDBusMessage::createSignal(
        QStringLiteral( "/gd_test" ), QStringLiteral( "org.goldendict.Test" ),
        QStringLiteral( "Barrier" ) );
    message << marker;
    return sender.send( message )
           && QTest::qWaitFor( [ this, marker ] { return seenMarker == marker; }, 1000 );
  };
  QVector< HotkeyBinding > const desired{
    { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ), QStringLiteral( "CTRL+ALT+g" ) }
  };
  QVector< bool > published;
  PortalBackend backend( client );
  QSignalSpy availability( &backend, &HotkeyBackend::configureAvailabilityChanged );
  QSignalSpy states( &backend, &HotkeyBackend::stateChanged );
  connect( &backend, &HotkeyBackend::configureAvailabilityChanged, this,
           [ & ] { published.append( backend.isConfigureAvailable() ); } );

  QVERIFY( !backend.isConfigureAvailable() );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Ready; }, 1500 ) );
  QVERIFY( backend.isSupported() );
  QVERIFY( !backend.isConfigureAvailable() );
  QCOMPARE( availability.count(), 0 );

  portal->holdResponse( QStringLiteral( "BindShortcuts" ) );
  backend.bind( desired );
  QVERIFY( !backend.isConfigureAvailable() );
  QVERIFY( QTest::qWaitFor( [ & ] {
    return backend.state() == State::Binding && portal->count( QStringLiteral( "BindShortcuts" ) ) == 1;
  }, 1500 ) );
  QVERIFY( !backend.isConfigureAvailable() );
  QCOMPARE( availability.count(), 0 );
  backend.configure( {} );
  QVERIFY( flushWire( client ) );
  QCOMPARE( portal->count( QStringLiteral( "ConfigureShortcuts" ) ), 0 );
  QCOMPARE( states.last().at( 1 ).toString(),
            QStringLiteral( "ConfigureShortcuts requires an available session" ) );

  portal->releaseHeldResponse( QStringLiteral( "BindShortcuts" ) );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Bound; }, 1500 ) );
  QVERIFY( backend.isConfigureAvailable() );
  QCOMPARE( availability.count(), 1 );
  QCOMPARE( published, QVector< bool >{ true } );
  // Queries and unrelated display/state notifications do not publish a transition.
  for( int i = 0; i != 3; ++i )
    QVERIFY( backend.isConfigureAvailable() );
  backend.setDisplayPolicy( { QStringLiteral( "main-window" ) } );
  QVERIFY( flushWire( privateBus.fixtureConnection() ) );
  QCOMPARE( availability.count(), 1 );

  backend.unbind();
  QVERIFY( !backend.isConfigureAvailable() );
  QCOMPARE( availability.count(), 2 );
  backend.unbind();
  QCOMPARE( availability.count(), 2 );

  portal->setScenario( QStringLiteral( "bind-denied" ) );
  backend.bind( desired );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Denied; }, 1500 ) );
  QVERIFY( backend.isConfigureAvailable() );
  QCOMPARE( availability.count(), 3 );
  backend.configure( {} );
  QVERIFY( QTest::qWaitFor( [ & ] {
    return portal->count( QStringLiteral( "ConfigureShortcuts" ) ) == 1;
  }, 1500 ) );
  QCOMPARE( portal->bindCountForSession( portal->sessionPath() ), 1 );
  QCOMPARE( availability.count(), 3 );

  portal->setScenario( QStringLiteral( "denied" ) );
  backend.bind( desired );
  QVERIFY( !backend.isConfigureAvailable() );
  QCOMPARE( availability.count(), 4 );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Denied; }, 1500 ) );
  QVERIFY( !backend.isConfigureAvailable() );
  QCOMPARE( availability.count(), 4 );

  portal->setScenario( QStringLiteral( "fast" ) );
  backend.bind( desired );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Bound; }, 1500 ) );
  QCOMPARE( availability.count(), 5 );
  portal->emitSessionClosed( portal->sessionPath(), QStringLiteral( "fixture closed" ) );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Unavailable; }, 1500 ) );
  QVERIFY( !backend.isConfigureAvailable() );
  QCOMPARE( availability.count(), 6 );

  backend.bind( desired );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Bound; }, 1500 ) );
  QCOMPARE( availability.count(), 7 );
  portal->loseService();
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Unavailable; }, 1500 ) );
  QVERIFY( !backend.isSupported() );
  QVERIFY( !backend.isConfigureAvailable() );
  QCOMPARE( availability.count(), 8 );

  portal.reset( new TestSupport::FakePortal );
  QVERIFY2( portal->start( privateBus.fixtureConnection(), 2 ), qPrintable( portal->errorString() ) );
  backend.bind( desired );
  QVERIFY( QTest::qWaitFor( [ & ] { return backend.state() == State::Bound; }, 1500 ) );
  QVERIFY( backend.isSupported() );
  QVERIFY( backend.isConfigureAvailable() );
  QCOMPARE( availability.count(), 9 );
  QVERIFY( flushWire( privateBus.fixtureConnection() ) );
  const int configureCalls = portal->count( QStringLiteral( "ConfigureShortcuts" ) );

  // stop() terminates the owned daemon, breaking the backend's retained transport.
  // Merely disconnecting a named Qt connection leaves retained handles connected.
  privateBus.stop();
  QVERIFY( !privateBus.isRunning() );
  QVERIFY( QTest::qWaitFor( [ & ] { return !client.isConnected(); }, 1500 ) );
  QVERIFY( !backend.isSupported() );
  QCOMPARE( backend.state(), State::Bound );
  QVERIFY( !backend.isConfigureAvailable() );
  QVERIFY( QTest::qWaitFor( [ & ] { return availability.count() == 10; }, 1500 ) );
  QCOMPARE( published, ( QVector< bool >{ true, false, true, false, true, false, true, false, true, false } ) );
  backend.configure( {} );
  QCOMPARE( states.last().at( 1 ).toString(),
            QStringLiteral( "ConfigureShortcuts requires an available session" ) );
  QCOMPARE( portal->count( QStringLiteral( "ConfigureShortcuts" ) ), configureCalls );
  QCOMPARE( portal->protocolViolations(), QStringList() );
}

void TstHotkeys::lateCreateSessionSuccessClosesItsSessionAfterCancellation()
{
  TestSupport::PrivateSessionBus privateBus;
  QString reason;
  QVERIFY2( privateBus.start( &reason ), qPrintable( reason ) );
  TestSupport::FakePortal portal;
  QVERIFY2( portal.start( privateBus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );
  portal.setScenario( QStringLiteral( "early-response" ) );
  portal.holdMethodReply( QStringLiteral( "CreateSession" ) );

  QVERIFY( privateBus.connection().connect( QString(), QStringLiteral( "/gd_test" ),
                                             QStringLiteral( "org.goldendict.Test" ),
                                             QStringLiteral( "Barrier" ), this,
                                             SLOT( wireBarrier( uint ) ) ) );

  PortalBackend backend( privateBus.connection() );
  backend.bind( { HotkeyBinding{ QStringLiteral( "main-window" ), QString(), QStringLiteral( "F11" ) } } );
  QVERIFY( QTest::qWaitFor( [ &portal ]() {
    return portal.count( QStringLiteral( "CreateSession" ) ) == 1;
  }, 1000 ) );

  const uint marker = ++nextMarker;
  QDBusMessage barrier = QDBusMessage::createSignal(
      QStringLiteral( "/gd_test" ), QStringLiteral( "org.goldendict.Test" ),
      QStringLiteral( "Barrier" ) );
  barrier << marker;
  QVERIFY( privateBus.fixtureConnection().send( barrier ) );
  QVERIFY( QTest::qWaitFor( [ this, marker ]() { return seenMarker == marker; }, 1000 ) );

  backend.unbind();
  QCOMPARE( backend.state(), HotkeyBackend::State::Closed );
  portal.releaseHeldMethodReply( QStringLiteral( "CreateSession" ) );
  QVERIFY( QTest::qWaitFor( [ &portal ]() {
    return portal.count( QStringLiteral( "Session.Close" ) ) == 1;
  }, 1000 ) );
  QCOMPARE( backend.state(), HotkeyBackend::State::Closed );
  QCOMPARE( portal.count( QStringLiteral( "Session.Close" ) ), 1 );
  QVERIFY( portal.protocolViolations().isEmpty() );
  QVERIFY( privateBus.connection().disconnect( QString(), QStringLiteral( "/gd_test" ),
                                                QStringLiteral( "org.goldendict.Test" ),
                                                QStringLiteral( "Barrier" ), this,
                                                SLOT( wireBarrier( uint ) ) ) );
}
void TstHotkeys::unsupportedBackendStillReportsState()
{
  // An unsupported platform returns a concrete disabled backend, never null and
  // never a silent no-op that claims success.
  RecordingHotkeyBackend backend( false );
  QVERIFY( !backend.isSupported() );

  QSignalSpy stateChanged( &backend, &HotkeyBackend::stateChanged );
  backend.bind( { HotkeyBinding{ QStringLiteral( "main-window" ), QString(), QStringLiteral( "F11" ) } } );
  QCOMPARE( backend.bindCalls, 1 );
  QVERIFY( stateChanged.count() >= 1 );
  backend.unbind();
}

// ---------------------------------------------------------------------------
// Legacy configuration
// ---------------------------------------------------------------------------

void TstHotkeys::unsupportedFactory()
{
  // The named offscreen invocation is required independently of the table.
  if ( qEnvironmentVariable( "QT_QPA_PLATFORM" ) != QLatin1String( "offscreen" ) )
    return;
  QVERIFY( !QGuiApplication::platformName().startsWith( QStringLiteral( "xcb" ) ) );
  QVERIFY( !QGuiApplication::platformName().startsWith( QStringLiteral( "wayland" ) ) );
  QObject owner;
  auto backend = createHotkeyBackend( &owner );
  QVERIFY( backend );
  QCOMPARE( backend->parent(), &owner );
  QVERIFY( !backend->isSupported() );
  QCOMPARE( backend->state(), HotkeyBackend::State::Unsupported );
  QSignalSpy states( backend.get(), &HotkeyBackend::stateChanged );
  backend->bind( { HotkeyBinding{ QStringLiteral( "main-window" ), {}, QStringLiteral( "CTRL+F11" ) } } );
  QCOMPARE( states.count(), 1 );
  QVERIFY( !states.first().at( 1 ).toString().isEmpty() );
  QCOMPARE( backend->state(), HotkeyBackend::State::Unsupported );
  backend.reset();
  QCOMPARE( owner.children().size(), 0 );
}

void TstHotkeys::factorySelection_data()
{
  QTest::addColumn< QString >( "platform" );
  QTest::addColumn< QString >( "availability" );
  QTest::newRow( "xcb" ) << QStringLiteral( "xcb" ) << QStringLiteral( "present" );
  QTest::newRow( "other" ) << QStringLiteral( "offscreen" ) << QStringLiteral( "present" );
  QTest::newRow( "wayland" ) << QStringLiteral( "wayland" ) << QStringLiteral( "present" );
  // Capability-when-the-portal-cannot-answer is pinned in tst_portal_hotkeys instead:
  // there the private bus belongs to the test alone, whereas these rows share it with a
  // nested compositor whose own portal integration can auto-activate a withdrawn or
  // misbehaving service and make the outcome depend on scheduling.

  QTest::newRow( "wayland-no-bus" ) << QStringLiteral( "wayland" ) << QStringLiteral( "no-bus" );
}

void TstHotkeys::factorySelection()
{
  QFETCH( QString, platform );
  QFETCH( QString, availability );
  TestSupport::PrivateSessionBus privateBus;
  QString reason;
  QVERIFY2( privateBus.start( &reason ), qPrintable( reason ) );
  TestSupport::FakePortal portal;
  QVERIFY2( portal.start( privateBus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );
  QTemporaryDir runtime;
  QVERIFY( runtime.isValid() );
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert( QStringLiteral( "DBUS_SESSION_BUS_ADDRESS" ), privateBus.address() );
  environment.insert( QStringLiteral( "XDG_RUNTIME_DIR" ), runtime.path() );
  QProcess compositor;
  // QProcess destruction reaps the isolated compositor even when an assertion fails.
  if ( platform == QLatin1String( "wayland" ) )
  {
    const QString kwin = QStandardPaths::findExecutable( QStringLiteral( "kwin_wayland" ) );
    QVERIFY2( !kwin.isEmpty(), "required native Wayland factory probe needs kwin_wayland" );
    environment.insert( QStringLiteral( "QT_QPA_PLATFORM" ), QStringLiteral( "offscreen" ) );
    environment.insert( QStringLiteral( "KWIN_COMPOSE" ), QStringLiteral( "Q" ) );
    environment.insert( QStringLiteral( "LIBGL_ALWAYS_SOFTWARE" ), QStringLiteral( "1" ) );
    compositor.setProcessEnvironment( environment );
    compositor.start( kwin, { QStringLiteral( "--virtual" ), QStringLiteral( "--no-kactivities" ),
      QStringLiteral( "--no-global-shortcuts" ), QStringLiteral( "--socket" ), QStringLiteral( "gd-t32" ) } );
    QVERIFY2( QTest::qWaitFor( [ & ]() {
      return QFile::exists( runtime.filePath( QStringLiteral( "gd-t32" ) ) )
             || compositor.state() == QProcess::NotRunning;
    }, 5000 ), "native Wayland compositor startup deadline" );
    QVERIFY2( QFile::exists( runtime.filePath( QStringLiteral( "gd-t32" ) ) ),
              compositor.readAllStandardError().constData() );
    environment.insert( QStringLiteral( "WAYLAND_DISPLAY" ), QStringLiteral( "gd-t32" ) );
  }
  if ( availability == QLatin1String( "absent" ) )
  {
    portal.loseService();
    QVERIFY2( QTest::qWaitFor( [ & ]() {
      return privateBus.fixtureConnection().interface()
                 ->serviceOwner( TestSupport::FakePortal::serviceName() )
                 .value()
          .isEmpty();
    }, 1000 ), "fake portal service did not withdraw" );
  }
  if ( availability == QLatin1String( "no-bus" ) )
    environment.insert( QStringLiteral( "DBUS_SESSION_BUS_ADDRESS" ),
                        QStringLiteral( "unix:path=" ) + runtime.filePath( QStringLiteral( "missing-bus" ) ) );
  environment.insert( QStringLiteral( "QT_QPA_PLATFORM" ), platform );
  environment.insert( QStringLiteral( "GD_T32_FACTORY_PROBE" ), availability );
  QProcess child;
  child.setProcessEnvironment( environment );
  child.start( QCoreApplication::applicationFilePath(), { QStringLiteral( "factorySelectionProbe" ) } );
  QVERIFY2( QTest::qWaitFor( [ & ]() { return child.state() == QProcess::NotRunning; }, 8000 ),
            "factory subprocess exceeded deadline" );
  const QByteArray output = child.readAllStandardOutput() + child.readAllStandardError();
  QCOMPARE( child.exitStatus(), QProcess::NormalExit );
  QVERIFY2( child.exitCode() == 0, output.constData() );
  QCOMPARE( portal.count( QStringLiteral( "BindShortcuts" ) ),
            platform == QLatin1String( "wayland" ) && availability == QLatin1String( "present" ) ? 1 : 0 );
  QVERIFY( portal.protocolViolations().isEmpty() );
  if ( compositor.state() != QProcess::NotRunning )
  {
    compositor.terminate();
    QVERIFY( QTest::qWaitFor( [ & ]() { return compositor.state() == QProcess::NotRunning; }, 2000 ) );
  }
}

void TstHotkeys::factorySelectionProbe()
{
  const QString availability = qEnvironmentVariable( "GD_T32_FACTORY_PROBE" );
  if ( availability.isEmpty() )
    return; // This slot is the subprocess side of factorySelection.
  QObject owner;
  auto backend = createHotkeyBackend( &owner );
  QVERIFY( backend );
  QCOMPARE( backend->parent(), &owner );
  const QString platform = QGuiApplication::platformName();
  if ( platform.startsWith( QLatin1String( "wayland" ) ) )
  {
    QVERIFY( dynamic_cast< PortalBackend * >( backend.get() ) );
    QCOMPARE( backend->state(), HotkeyBackend::State::Creating );
    QVERIFY( !backend->isSupported() );
    bool capabilityTransitioned = false;
    connect( backend.get(), &HotkeyBackend::stateChanged, backend.get(),
             [ &capabilityTransitioned ]( HotkeyBackend::State state, QString const & ) {
               if ( state == HotkeyBackend::State::Ready
                    || state == HotkeyBackend::State::Unavailable )
                   capabilityTransitioned = true;
             } );
    QVERIFY2( QTest::qWaitFor( [ &capabilityTransitioned ]() {
      return capabilityTransitioned;
    }, 5500 ), "portal capability probe did not reach a terminal state" );
    QCOMPARE( backend->isSupported(), availability == QLatin1String( "present" ) );
    QCOMPARE( backend->state(), availability == QLatin1String( "present" )
      ? HotkeyBackend::State::Ready : HotkeyBackend::State::Unavailable );
    backend->bind( { HotkeyBinding{ QStringLiteral( "main-window" ), {}, QStringLiteral( "CTRL+F11" ) } } );
    QVERIFY( QTest::qWaitFor( [ & ]() {
      return backend->state() == HotkeyBackend::State::Bound
             || backend->state() == HotkeyBackend::State::Unavailable;
    }, 5500 ) );
    QCOMPARE( backend->state(), availability == QLatin1String( "present" )
      ? HotkeyBackend::State::Bound : HotkeyBackend::State::Unavailable );
  }
  else if ( platform.startsWith( QLatin1String( "xcb" ) ) )
  {
#ifdef HAVE_X11
    QVERIFY( dynamic_cast< X11RecordBackend * >( backend.get() ) );
    QVERIFY( backend->isSupported() );
    QCOMPARE( backend->state(), HotkeyBackend::State::Ready );
#else
    QVERIFY( !backend->isSupported() );
#endif
  }
  else
  {
    QVERIFY( !dynamic_cast< PortalBackend * >( backend.get() ) );
    QVERIFY( !backend->isSupported() );
    QCOMPARE( backend->state(), HotkeyBackend::State::Unsupported );
  }
  backend.reset();
  QCOMPARE( owner.children().size(), 0 );
}

void TstHotkeys::legacyConfigLoadsWithoutRewriting()
{
  QString const installed = installConfigFixture( QStringLiteral( "config/legacy.xml" ) );
  QVERIFY( !installed.isEmpty() );

  QByteArray const before = readFileBytes( installed );
  QVERIFY( !before.isEmpty() );

  Config::Class const configuration = Config::load();

  // Values that existed before the migration are still read exactly.
  QCOMPARE( configuration.preferences.interfaceLanguage, QStringLiteral( "en_US" ) );
  QCOMPARE( configuration.preferences.zoomFactor, 1.0 );
  QVERIFY( configuration.preferences.proxyServer.enabled );
  QVERIFY( !configuration.preferences.proxyServer.useSystemProxy );
  QCOMPARE( configuration.preferences.proxyServer.type, Config::ProxyServer::Socks5 );
  QCOMPARE( configuration.preferences.proxyServer.host, QStringLiteral( "127.0.0.1" ) );
  QCOMPARE( configuration.preferences.proxyServer.port, 9050u );
  QCOMPARE( configuration.preferences.proxyServer.user, QStringLiteral( "proxyuser" ) );
  QCOMPARE( configuration.preferences.proxyServer.password, QStringLiteral( "proxypass" ) );
  QCOMPARE( configuration.preferences.audioPlaybackProgram, QStringLiteral( "mplayer -ao alsa" ) );
  QCOMPARE( configuration.paths.size(), 1 );
  QVERIFY( configuration.paths.first().recursive );

  // A configuration without <hotkeys> loads an empty display record set.
  QVERIFY( configuration.hotkeys.portalBindings.isEmpty() );

  // Loading must not save: the existing save point is the only writer.
  QCOMPARE( readFileBytes( installed ), before );
}

void TstHotkeys::legacyHotkeyTextSurvivesRoundTrip()
{
  QString const installed = installConfigFixture( QStringLiteral( "config/legacy.xml" ) );
  QVERIFY( !installed.isEmpty() );

  Config::Class configuration = Config::load();

  // The untouched text is kept next to the parsed combination.
  QCOMPARE( configuration.preferences.mainWindowHotkey.rawText, QStringLiteral( "Ctrl+F11,F11" ) );
  QCOMPARE( configuration.preferences.clipboardHotkey.rawText, QStringLiteral( "Ctrl+C,C" ) );
  QCOMPARE( configuration.preferences.mainWindowHotkey.key1, int( Qt::Key_F11 ) );
  QCOMPARE( configuration.preferences.mainWindowHotkey.key2, int( Qt::Key_F11 ) );

  QDomDocument const saved = saveAndRead( configuration );
  QDomNode const preferences = saved.namedItem( QStringLiteral( "config" ) )
                                 .namedItem( QStringLiteral( "preferences" ) );
  QVERIFY( !preferences.isNull() );

  // Byte-for-byte: the untouched stored text is not re-rendered.
  QCOMPARE( preferences.namedItem( QStringLiteral( "mainWindowHotkey" ) ).toElement().text(),
            QStringLiteral( "Ctrl+F11,F11" ) );
  QCOMPARE( preferences.namedItem( QStringLiteral( "clipboardHotkey" ) ).toElement().text(),
            QStringLiteral( "Ctrl+C,C" ) );

  // And the reloaded value is identical again.
  Config::Class const reloaded = Config::load();
  QCOMPARE( reloaded.preferences.mainWindowHotkey.rawText, QStringLiteral( "Ctrl+F11,F11" ) );
  QCOMPARE( reloaded.preferences.mainWindowHotkey.key1, int( Qt::Key_F11 ) );
  QCOMPARE( reloaded.preferences.mainWindowHotkey.key2, int( Qt::Key_F11 ) );
}

void TstHotkeys::legacyAudioBackendMigratesInMemory()
{
  QVERIFY( installConfigFixture( QStringLiteral( "config/legacy.xml" ) ).size() > 0 );

  Config::Class const configuration = Config::load();

  // The stored value really was the removed Qt Multimedia name.
  QCOMPARE( readFileBytes( Config::getConfigFileName() )
              .contains( "Qt Multimedia" ),
            true );

  bool const hasFfmpegToken = hasFfmpegPlayer();
#ifdef GD_TEST_CONFIG_FFMPEG_PLAYER
  QVERIFY( hasFfmpegToken );
  QCOMPARE( configuration.preferences.internalPlayerBackend,
            Config::InternalPlayerBackend::defaultBackend() );
  QVERIFY( configuration.preferences.internalPlayerBackend.isFfmpeg() );
  QCOMPARE( configuration.preferences.internalPlayerBackend.uiName(),
            QStringLiteral( "FFmpeg+libao" ) );
  QVERIFY( configuration.preferences.useInternalPlayer );
#else
  QVERIFY( !hasFfmpegToken );
  QCOMPARE( configuration.preferences.internalPlayerBackend.uiName(),
            QStringLiteral( "Qt Multimedia" ) );
  QVERIFY( !configuration.preferences.useInternalPlayer );
  QVERIFY( !Config::InternalPlayerBackend::anyAvailable() );
#endif
}

void TstHotkeys::unknownAudioBackendFallsBack()
{
  QVERIFY( installConfigFixture( QStringLiteral( "config/additive.xml" ) ).size() > 0 );

  Config::Class const configuration = Config::load();

  QCOMPARE( readFileBytes( Config::getConfigFileName() ).contains( "Some Future Backend" ), true );

#ifdef GD_TEST_CONFIG_FFMPEG_PLAYER
  QCOMPARE( configuration.preferences.internalPlayerBackend,
            Config::InternalPlayerBackend::defaultBackend() );
  QVERIFY( configuration.preferences.useInternalPlayer );
#else
  QCOMPARE( configuration.preferences.internalPlayerBackend.uiName(),
            QStringLiteral( "Some Future Backend" ) );
  QVERIFY( !configuration.preferences.useInternalPlayer );
#endif
}

void TstHotkeys::noFfmpegKeepsStoredBackendText()
{
  QVERIFY( installConfigFixture( QStringLiteral( "config/legacy.xml" ) ).size() > 0 );

  Config::Class configuration = Config::load();
  QDomDocument const saved = saveAndRead( configuration );

  QString const savedBackend = saved.namedItem( QStringLiteral( "config" ) )
                                 .namedItem( QStringLiteral( "preferences" ) )
                                 .namedItem( QStringLiteral( "internalPlayerBackend" ) )
                                 .toElement().text();

#ifdef GD_TEST_CONFIG_FFMPEG_PLAYER
  // With the internal player available the migration is persisted by the next
  // ordinary save; the save point itself did not move.
  QCOMPARE( savedBackend, QStringLiteral( "FFmpeg+libao" ) );
  QVERIFY( configuration.preferences.useInternalPlayer );
#else
  // Without it the stored text is preserved verbatim for a later build, while
  // this build reports the internal player unavailable.
  QCOMPARE( savedBackend, QStringLiteral( "Qt Multimedia" ) );
  QVERIFY( !configuration.preferences.useInternalPlayer );
  QVERIFY( !Config::InternalPlayerBackend::anyAvailable() );
#endif
}

void TstHotkeys::savedAudioBackendFollowsTheBuildVariant()
{
  // The variant probe the variant step relies on: the binary's feature record and
  // the compiled branch must agree, so a mis-wired CONFIG=no_ffmpeg_player shows
  // up as a failure instead of silently testing the other half.
#ifdef GD_TEST_CONFIG_FFMPEG_PLAYER
  QVERIFY( hasFfmpegPlayer() );
  QVERIFY( Config::InternalPlayerBackend::anyAvailable() );
  QCOMPARE( Config::InternalPlayerBackend::defaultBackend().uiName(),
            QStringLiteral( "FFmpeg+libao" ) );
#else
  QVERIFY( !hasFfmpegPlayer() );
  QVERIFY( !Config::InternalPlayerBackend::anyAvailable() );
  QVERIFY( Config::InternalPlayerBackend::defaultBackend().uiName().isEmpty() );
#endif
}

void TstHotkeys::audioFactorySelectsTheAvailablePath()
{
  // Preferences defaults are derived from the real default backend, exactly as
  // the application derives them.
  Config::Preferences const preferences;
  AudioPlayerFactory factory( preferences );

  QVERIFY( factory.player() );

#ifdef GD_TEST_CONFIG_FFMPEG_PLAYER
  QVERIFY( qobject_cast< Ffmpeg::AudioPlayer * >( factory.player().data() ) != nullptr );
  QVERIFY( qobject_cast< ExternalAudioPlayer * >( factory.player().data() ) == nullptr );

  // Switching to the external player still works and is selected for real.
  Config::Preferences externalOnly = preferences;
  externalOnly.useInternalPlayer = false;
  externalOnly.audioPlaybackProgram = QStringLiteral( "aplay" );
  factory.setPreferences( externalOnly );
  QVERIFY( qobject_cast< ExternalAudioPlayer * >( factory.player().data() ) != nullptr );
#else
  // No internal player in this build: the factory falls back to the external
  // path and never constructs a missing internal player.
  QVERIFY( qobject_cast< ExternalAudioPlayer * >( factory.player().data() ) != nullptr );

  ExternalAudioPlayer * const external =
    qobject_cast< ExternalAudioPlayer * >( factory.player().data() );
  QVERIFY( external );
  // The external path really runs (and reports a clean failure for a missing
  // program name instead of silently doing nothing).
  QVERIFY( !external->play( "not audio", 9 ).isEmpty() );
#endif
}

// ---------------------------------------------------------------------------
// Additive portal binding display records
// ---------------------------------------------------------------------------

void TstHotkeys::additivePortalBindingsLoadLastWinsAndDecodeEscapedText()
{
  QVERIFY( installConfigFixture( QStringLiteral( "config/additive.xml" ) ).size() > 0 );

  QByteArray const before = readFileBytes( Config::getConfigFileName() );
  Config::Class const configuration = Config::load();

  QVector< PortalBindingDisplay > const & bindings = configuration.hotkeys.portalBindings;

  // The unknown "future-action" ID is ignored, the duplicated known ID is
  // last-wins for display, and the escaped attribute is plain text.
  QCOMPARE( bindings.size(), 2 );
  QCOMPARE( bindings.at( 0 ).id, QStringLiteral( "main-window" ) );
  QCOMPARE( bindings.at( 0 ).triggerDescription, QStringLiteral( "Ctrl+Shift+G" ) );
  QCOMPARE( bindings.at( 1 ).id, QStringLiteral( "clipboard-lookup" ) );
  QCOMPARE( bindings.at( 1 ).triggerDescription,
            QStringLiteral( "Ctrl+<Alt>+& \"C\"" ) );

  for( PortalBindingDisplay const & binding : bindings )
    QVERIFY( binding.id != QStringLiteral( "future-action" ) );

  // Loading is still a read-only operation.
  QCOMPARE( readFileBytes( Config::getConfigFileName() ), before );
}

void TstHotkeys::additivePortalBindingsRoundTrip()
{
  QVERIFY( installConfigFixture( QStringLiteral( "config/additive.xml" ) ).size() > 0 );

  Config::Class configuration = Config::load();

  // New display records are only written through the existing save point, and
  // they are additive to the section that was already there.
  PortalBindingDisplay extra;
  extra.id = QStringLiteral( "clipboard-lookup" );
  extra.triggerDescription = QStringLiteral( "Ctrl+Alt+C" );

  configuration.hotkeys.portalBindings.clear();
  configuration.hotkeys.portalBindings.push_back(
    PortalBindingDisplay{ QStringLiteral( "main-window" ), QStringLiteral( "Ctrl+Alt+G" ) } );
  configuration.hotkeys.portalBindings.push_back( extra );
  // A foreign ID on an in-memory record is never persisted either.
  configuration.hotkeys.portalBindings.push_back(
    PortalBindingDisplay{ QStringLiteral( "foreign-action" ), QStringLiteral( "Ctrl+Alt+F" ) } );

  QByteArray savedBytes;
  {
    QDomDocument const saved = saveAndRead( configuration );
    QVERIFY( !saved.isNull() );

    QDomNode const portalBindings = saved.namedItem( QStringLiteral( "config" ) )
                                      .namedItem( QStringLiteral( "hotkeys" ) )
                                      .namedItem( QStringLiteral( "portalBindings" ) );
    QVERIFY( !portalBindings.isNull() );

    QDomNodeList const written = portalBindings.toElement().elementsByTagName( QStringLiteral( "binding" ) );
    QCOMPARE( written.length(), 2 );
    QCOMPARE( written.item( 0 ).toElement().attribute( QStringLiteral( "id" ) ),
              QStringLiteral( "main-window" ) );
    QCOMPARE( written.item( 0 ).toElement().attribute( QStringLiteral( "triggerDescription" ) ),
              QStringLiteral( "Ctrl+Alt+G" ) );
    QCOMPARE( written.item( 1 ).toElement().attribute( QStringLiteral( "id" ) ),
              QStringLiteral( "clipboard-lookup" ) );
    QCOMPARE( written.item( 1 ).toElement().attribute( QStringLiteral( "triggerDescription" ) ),
              QStringLiteral( "Ctrl+Alt+C" ) );

    savedBytes = readFileBytes( Config::getConfigFileName() );
    QVERIFY( !savedBytes.contains( "foreign-action" ) );
  }

  Config::Class const reloaded = Config::load();
  QCOMPARE( reloaded.hotkeys.portalBindings.size(), 2 );
  QCOMPARE( reloaded.hotkeys.portalBindings.at( 0 ).id, QStringLiteral( "main-window" ) );
  QCOMPARE( reloaded.hotkeys.portalBindings.at( 1 ).triggerDescription,
            QStringLiteral( "Ctrl+Alt+C" ) );
}

void TstHotkeys::additiveValuesSurviveRoundTrip()
{
  QVERIFY( installConfigFixture( QStringLiteral( "config/additive.xml" ) ).size() > 0 );

  Config::Class configuration = Config::load();
  QDomDocument const saved = saveAndRead( configuration );
  QVERIFY( !saved.isNull() );

  QDomNode const config = saved.namedItem( QStringLiteral( "config" ) );
  QDomNode const preferences = config.namedItem( QStringLiteral( "preferences" ) );

  QCOMPARE( preferences.namedItem( QStringLiteral( "interfaceLanguage" ) ).toElement().text(),
            QStringLiteral( "de_DE" ) );
  QCOMPARE( preferences.namedItem( QStringLiteral( "zoomFactor" ) ).toElement().text(),
            QStringLiteral( "1.25" ) );
  QCOMPARE( preferences.namedItem( QStringLiteral( "maxNetworkCacheSize" ) ).toElement().text(),
            QStringLiteral( "100" ) );
  QCOMPARE( preferences.namedItem( QStringLiteral( "audioPlaybackProgram" ) ).toElement().text(),
            QStringLiteral( "aplay" ) );
  QCOMPARE( preferences.namedItem( QStringLiteral( "proxyserver" ) )
              .toElement().attribute( QStringLiteral( "useSystemProxy" ) ),
            QStringLiteral( "1" ) );
  QCOMPARE( config.namedItem( QStringLiteral( "maxPictureWidth" ) ).toElement().text(),
            QStringLiteral( "1200" ) );

  // The untouched lowercase text is not canonicalized by the round trip.
  QCOMPARE( preferences.namedItem( QStringLiteral( "mainWindowHotkey" ) ).toElement().text(),
            QStringLiteral( "ctrl+shift+g" ) );

  Config::Class const reloaded = Config::load();
  QCOMPARE( reloaded.preferences.interfaceLanguage, QStringLiteral( "de_DE" ) );
  QCOMPARE( reloaded.preferences.zoomFactor, 1.25 );
  QCOMPARE( reloaded.preferences.mainWindowHotkey.key1, int( Qt::Key_G ) );
  QCOMPARE( reloaded.preferences.mainWindowHotkey.modifiers,
            Qt::KeyboardModifiers( Qt::ControlModifier | Qt::ShiftModifier ) );

  // Extension elements the writer does not know are not carried over: save()
  // builds a fresh document from the known settings, and it did so before this
  // migration too (the Qt5 writer rebuilt the document the same way). This is
  // pinned here so nobody mistakes the fixture's <futureOption>/<futureSection>
  // nodes for a preservation promise, and so a future decision to preserve
  // them has to change this assertion deliberately.
  QDomNode const hotkeys = config.namedItem( QStringLiteral( "hotkeys" ) );
  QVERIFY( hotkeys.isNull() || hotkeys.namedItem( QStringLiteral( "futureSection" ) ).isNull() );
  QVERIFY( hotkeys.isNull() ||
           hotkeys.namedItem( QStringLiteral( "portalBindings" ) )
             .namedItem( QStringLiteral( "futureOption" ) ).isNull() );
}

void TstHotkeys::unsupportedShortcutTextSurvivesRoundTrip()
{
  QVERIFY( installConfigFixture( QStringLiteral( "config/additive.xml" ) ).size() > 0 );

  Config::Class configuration = Config::load();

  // "Ctrl+F11,F11,F11" is not representable as a Config::HotKey and is not a
  // bindable portal trigger; it must stay visible and untouched.
  QCOMPARE( configuration.preferences.clipboardHotkey.rawText,
            QStringLiteral( "Ctrl+F11,F11,F11" ) );

  QString trigger = QStringLiteral( "untouched" );
  QString reason;
  QVERIFY( !HotkeySyntax::preferredTrigger( configuration.preferences.clipboardHotkey,
                                            &trigger, &reason ) );
  QCOMPARE( trigger, QStringLiteral( "untouched" ) );

  QDomDocument const saved = saveAndRead( configuration );
  QCOMPARE( saved.namedItem( QStringLiteral( "config" ) )
              .namedItem( QStringLiteral( "preferences" ) )
              .namedItem( QStringLiteral( "clipboardHotkey" ) )
              .toElement().text(),
            QStringLiteral( "Ctrl+F11,F11,F11" ) );
}

void TstHotkeys::recorderEditClearsTheRawTextMarker()
{
  QVERIFY( installConfigFixture( QStringLiteral( "config/additive.xml" ) ).size() > 0 );

  Config::Class configuration = Config::load();
  QCOMPARE( configuration.preferences.mainWindowHotkey.rawText,
            QStringLiteral( "ctrl+shift+g" ) );

  // Exactly what HotKeyEdit::getHotKey() hands back after a recorder edit: a
  // fresh value built from the recorded combination, with no raw marker.
  configuration.preferences.mainWindowHotkey =
    Config::HotKey( QKeySequence( QStringLiteral( "Ctrl+Alt+H" ) ) );

  QDomDocument const saved = saveAndRead( configuration );
  QCOMPARE( saved.namedItem( QStringLiteral( "config" ) )
              .namedItem( QStringLiteral( "preferences" ) )
              .namedItem( QStringLiteral( "mainWindowHotkey" ) )
              .toElement().text(),
            QStringLiteral( "Ctrl+Alt+H" ) );

  // The untouched second value keeps its own text.
  QCOMPARE( saved.namedItem( QStringLiteral( "config" ) )
              .namedItem( QStringLiteral( "preferences" ) )
              .namedItem( QStringLiteral( "clipboardHotkey" ) )
              .toElement().text(),
            QStringLiteral( "Ctrl+F11,F11,F11" ) );
}

// ---------------------------------------------------------------------------
// The X11 recorder
// ---------------------------------------------------------------------------

void TstHotkeys::x11SessionNamesAreDecidedCaseInsensitively_data()
{
  QTest::addColumn< QString >( "platformName" );
  QTest::addColumn< bool >( "x11Session" );

  // The names a QPA actually reports, including the capitalised spelling that a
  // case-sensitive comparison gets wrong, and names that merely contain "xcb".
  QTest::newRow( "xcb" ) << QStringLiteral( "xcb" ) << true;
  QTest::newRow( "xcb capitalised" ) << QStringLiteral( "XCB" ) << true;
  QTest::newRow( "xcb variant" ) << QStringLiteral( "xcb-gl" ) << true;
  QTest::newRow( "wayland" ) << QStringLiteral( "wayland" ) << false;
  QTest::newRow( "Wayland capitalised" ) << QStringLiteral( "Wayland" ) << false;
  QTest::newRow( "WAYLAND" ) << QStringLiteral( "WAYLAND" ) << false;
  QTest::newRow( "wayland-egl" ) << QStringLiteral( "wayland-egl" ) << false;
  QTest::newRow( "wayland plus xcb" ) << QStringLiteral( "wayland/xcb" ) << false;
  QTest::newRow( "offscreen" ) << QStringLiteral( "offscreen" ) << false;
  QTest::newRow( "minimal" ) << QStringLiteral( "minimal" ) << false;
  QTest::newRow( "empty" ) << QString() << false;
}

void TstHotkeys::x11SessionNamesAreDecidedCaseInsensitively()
{
  QFETCH( QString, platformName );
  QFETCH( bool, x11Session );

  QCOMPARE( HotkeyRuntime::isX11Session( platformName ), x11Session );
}

void TstHotkeys::recorderModifierTrackingUsesTheXProtocolMasks()
{
  // The masks a decision carries are handed to XGrabKey/XUngrabKey unchanged, so
  // they must be the X protocol's own values and nothing invented.
#ifdef HAVE_X11
  QCOMPARE( int( RecorderShiftMask ), int( ShiftMask ) );
  QCOMPARE( int( RecorderControlMask ), int( ControlMask ) );
  QCOMPARE( int( RecorderAltMask ), int( Mod1Mask ) );
  QCOMPARE( int( RecorderMetaMask ), int( Mod4Mask ) );
#else
  QCOMPARE( int( RecorderShiftMask ), 1 );
  QCOMPARE( int( RecorderControlMask ), 4 );
  QCOMPARE( int( RecorderAltMask ), 8 );
  QCOMPARE( int( RecorderMetaMask ), 64 );
#endif

  FixtureRecorderSession session;
  RecorderMatcher matcher;
  matcher.setModifierKeyCodes( session.modifierKeyCodes() );

  QVERIFY( matcher.isModifierKeyCode( FixtureRecorderSession::LeftControl ) );
  QVERIFY( matcher.isModifierKeyCode( FixtureRecorderSession::RightControl ) );
  QVERIFY( !matcher.isModifierKeyCode( FixtureRecorderSession::KeyG ) );

  QCOMPARE( matcher.modifiers(), 0u );
  QCOMPARE( matcher.modifierPressed( FixtureRecorderSession::LeftControl ),
            quint32( RecorderControlMask ) );
  QCOMPARE( matcher.modifierPressed( FixtureRecorderSession::RightShift ),
            quint32( RecorderControlMask | RecorderShiftMask ) );
  // Releasing a key that is not held changes nothing.
  QCOMPARE( matcher.modifierReleased( FixtureRecorderSession::RightShift ),
            quint32( RecorderControlMask ) );
  QCOMPARE( matcher.modifierReleased( FixtureRecorderSession::LeftControl ), 0u );

  // Both sides of one modifier carry the same mask.
  QCOMPARE( RecorderMatcher::modifierFlagForKeyCode( FixtureRecorderSession::RightAlt,
                                                     session.modifierKeyCodes() ),
            quint32( RecorderAltMask ) );
  QCOMPARE( RecorderMatcher::modifierFlagForKeyCode( FixtureRecorderSession::KeyG,
                                                     session.modifierKeyCodes() ),
            quint32( 0 ) );
}

void TstHotkeys::recorderMatcherActivatesASingleKeyCombination()
{
  FixtureRecorderSession session;
  RecorderMatcher matcher;
  matcher.setModifierKeyCodes( session.modifierKeyCodes() );
  matcher.setCopyKeyCodes( session.copyKeyCode(), session.insertKeyCode(),
                           session.keypadInsertKeyCode() );

  matcher.addBinding( FixtureRecorderSession::KeyG, 0,
                      RecorderControlMask | RecorderAltMask, 7 );
  QCOMPARE( matcher.bindings().size(), 1 );

  matcher.modifierPressed( FixtureRecorderSession::LeftControl );
  matcher.modifierPressed( FixtureRecorderSession::LeftAlt );

  RecorderMatcher::Decision decision = matcher.press( FixtureRecorderSession::KeyG,
                                                      matcher.modifiers() );
  QCOMPARE( int( decision.kind ), int( RecorderMatcher::Kind::Activated ) );
  QCOMPARE( decision.handle, 7 );

  // The modifiers are part of the combination: the same key alone is nothing.
  matcher.modifierReleased( FixtureRecorderSession::LeftAlt );
  decision = matcher.press( FixtureRecorderSession::KeyG, matcher.modifiers() );
  QCOMPARE( int( decision.kind ), int( RecorderMatcher::Kind::Ignored ) );

  matcher.modifierReleased( FixtureRecorderSession::LeftControl );
  QCOMPARE( matcher.modifiers(), 0u );
  decision = matcher.press( FixtureRecorderSession::KeyG, matcher.modifiers() );
  QCOMPARE( int( decision.kind ), int( RecorderMatcher::Kind::Ignored ) );

  // Neither does another key with the right modifiers.
  matcher.modifierPressed( FixtureRecorderSession::LeftControl );
  matcher.modifierPressed( FixtureRecorderSession::LeftAlt );
  decision = matcher.press( FixtureRecorderSession::KeyF, matcher.modifiers() );
  QCOMPARE( int( decision.kind ), int( RecorderMatcher::Kind::Ignored ) );
}

void TstHotkeys::recorderMatcherHandlesTwoKeyAndRepeatedPresses()
{
  FixtureRecorderSession session;
  RecorderMatcher matcher;
  matcher.setModifierKeyCodes( session.modifierKeyCodes() );
  matcher.setCopyKeyCodes( session.copyKeyCode(), session.insertKeyCode(),
                           session.keypadInsertKeyCode() );
  matcher.modifierPressed( FixtureRecorderSession::LeftControl );

  // The stored Ctrl+C,C layout: the first key starts a 500 ms window, the same key
  // pressed again inside it is the second key.
  matcher.addBinding( FixtureRecorderSession::KeyC, FixtureRecorderSession::KeyC,
                      RecorderControlMask, 9 );

  RecorderMatcher::Decision decision = matcher.press( FixtureRecorderSession::KeyC,
                                                      matcher.modifiers() );
  QCOMPARE( int( decision.kind ), int( RecorderMatcher::Kind::SecondKeyWait ) );
  QCOMPARE( decision.handle, 9 );
  QCOMPARE( decision.secondKey, quint32( FixtureRecorderSession::KeyC ) );
  QCOMPARE( decision.secondKeyModifiers, quint32( RecorderControlMask ) );
  // The combination begins with the copy key, so its second key is grabbed: the
  // double press has to reach the recorder rather than the application.
  QVERIFY( decision.grabSecondKey );
  QVERIFY( matcher.awaitingSecondKey() );
  QCOMPARE( matcher.awaitedHandle(), 9 );

  decision = matcher.press( FixtureRecorderSession::KeyC, matcher.modifiers() );
  QCOMPARE( int( decision.kind ), int( RecorderMatcher::Kind::Activated ) );
  QCOMPARE( decision.handle, 9 );
  QVERIFY( !matcher.awaitingSecondKey() );

  // A second key with a different modifier mask is not the awaited press, and the
  // wait does not survive it: a later Ctrl+C starts a new window instead of firing
  // the one that was abandoned.
  decision = matcher.press( FixtureRecorderSession::KeyC, matcher.modifiers() );
  QCOMPARE( int( decision.kind ), int( RecorderMatcher::Kind::SecondKeyWait ) );
  decision = matcher.press( FixtureRecorderSession::KeyC, 0 );
  QCOMPARE( int( decision.kind ), int( RecorderMatcher::Kind::Ignored ) );
  QVERIFY( !matcher.awaitingSecondKey() );
  decision = matcher.press( FixtureRecorderSession::KeyC, matcher.modifiers() );
  QCOMPARE( int( decision.kind ), int( RecorderMatcher::Kind::SecondKeyWait ) );

  // The timeout cancels the wait without an activation.
  matcher.cancelSecondKeyWait();
  QVERIFY( !matcher.awaitingSecondKey() );

  // A combination that begins with a non-copy key and ends with the copy key must
  // not grab that copy key: pressing Ctrl+F and then Ctrl+C has to let the copy
  // through to the application below.
  matcher.clearBindings();
  matcher.addBinding( FixtureRecorderSession::KeyF, FixtureRecorderSession::KeyC,
                      RecorderControlMask, 3 );
  decision = matcher.press( FixtureRecorderSession::KeyF, matcher.modifiers() );
  QCOMPARE( int( decision.kind ), int( RecorderMatcher::Kind::SecondKeyWait ) );
  QVERIFY2( !decision.grabSecondKey,
            "grabbing the second copy key would swallow the application's copy" );
}

void TstHotkeys::recorderCoreGrabsTheSecondKeyAndReleasesItAgain()
{
  FixtureRecorderSession session;
  RecorderCore core( &session );
  QSignalSpy activations( &core, &RecorderCore::activated );

  // A plain combination is grabbed as soon as it is bound: other clients must not
  // receive it.
  core.addBinding( FixtureRecorderSession::KeyG, 0,
                   RecorderControlMask | RecorderAltMask, 7 );
  QVERIFY( core.isGrabbed( FixtureRecorderSession::KeyG,
                           RecorderControlMask | RecorderAltMask ) );
  QCOMPARE( session.grabCalls.size(), 1 );

  // The copy combination is not grabbed before its first key: the copy still has to
  // reach the application.
  core.addBinding( FixtureRecorderSession::KeyC, FixtureRecorderSession::KeyC,
                   RecorderControlMask, 9 );
  QVERIFY( !session.holdsGrab( FixtureRecorderSession::KeyC, RecorderControlMask ) );
  QCOMPARE( session.grabCalls.size(), 1 );

  session.pressRecorded( FixtureRecorderSession::LeftControl );
  session.pressRecorded( FixtureRecorderSession::KeyC );

  QCOMPARE( activations.count(), 0 );
  QVERIFY( core.matcher().awaitingSecondKey() );
  // While the second key is awaited it is grabbed, so the second copy cannot be
  // swallowed by the application below either.
  QVERIFY( session.holdsGrab( FixtureRecorderSession::KeyC, RecorderControlMask ) );

  session.pressRecorded( FixtureRecorderSession::KeyC );
  QCOMPARE( activations.count(), 1 );
  QCOMPARE( activations.at( 0 ).at( 0 ).toInt(), 9 );
  // ... and released again once it has been used.
  QVERIFY( !session.holdsGrab( FixtureRecorderSession::KeyC, RecorderControlMask ) );

  // An unrelated press ends a wait and releases its grab with it.
  session.pressRecorded( FixtureRecorderSession::KeyC );
  QVERIFY( session.holdsGrab( FixtureRecorderSession::KeyC, RecorderControlMask ) );
  session.pressRecorded( FixtureRecorderSession::KeyC );
  QCOMPARE( activations.count(), 2 );
  QVERIFY( !session.holdsGrab( FixtureRecorderSession::KeyC, RecorderControlMask ) );

  core.clear();
  QVERIFY( session.grabs.isEmpty() );
}

void TstHotkeys::recorderCoreSecondKeyWindowExpiresAndRestarts()
{
  QCOMPARE( RecorderMatcher::secondKeyTimeoutMs, 500 );

  FixtureRecorderSession session;
  RecorderCore core( &session );
  QSignalSpy activations( &core, &RecorderCore::activated );

  core.addBinding( FixtureRecorderSession::KeyC, FixtureRecorderSession::KeyC,
                   RecorderControlMask, 9 );
  session.pressRecorded( FixtureRecorderSession::LeftControl );

  QElapsedTimer elapsed;
  elapsed.start();
  session.pressRecorded( FixtureRecorderSession::KeyC );
  QVERIFY( core.matcher().awaitingSecondKey() );
  QVERIFY( session.holdsGrab( FixtureRecorderSession::KeyC, RecorderControlMask ) );

  // The window really expires: no activation, and the grab is released so the copy
  // key works again.
  QTRY_VERIFY_WITH_TIMEOUT( !core.matcher().awaitingSecondKey(), 3000 );
  QVERIFY2( elapsed.elapsed() >= RecorderMatcher::secondKeyTimeoutMs,
            "the second-key window expired early" );
  QCOMPARE( activations.count(), 0 );
  QVERIFY( !session.holdsGrab( FixtureRecorderSession::KeyC, RecorderControlMask ) );

  // A press after the window starts a fresh one instead of completing the expired
  // one, and the fresh window drives the same activation.
  session.pressRecorded( FixtureRecorderSession::KeyC );
  QVERIFY( core.matcher().awaitingSecondKey() );
  QCOMPARE( activations.count(), 0 );
  session.pressRecorded( FixtureRecorderSession::KeyC );
  QCOMPARE( activations.count(), 1 );
  QCOMPARE( activations.at( 0 ).at( 0 ).toInt(), 9 );
}

void TstHotkeys::recorderCoreRefusedGrabIsReportedNotFatal()
{
  FixtureRecorderSession session;
  session.refuseGrabs = true;

  RecorderCore core( &session );
  QSignalSpy activations( &core, &RecorderCore::activated );

  core.addBinding( FixtureRecorderSession::KeyG, 0,
                   RecorderControlMask | RecorderAltMask, 7 );

  // The attempt was made and refused; the core never pretends to hold the grab.
  QCOMPARE( session.grabCalls.size(), 1 );
  QVERIFY( !core.isGrabbed( FixtureRecorderSession::KeyG,
                            RecorderControlMask | RecorderAltMask ) );

  // Recording does not depend on the grab, so the combination still fires.
  session.pressRecorded( FixtureRecorderSession::LeftControl );
  session.pressRecorded( FixtureRecorderSession::LeftAlt );
  session.pressRecorded( FixtureRecorderSession::KeyG );
  QCOMPARE( activations.count(), 1 );
  QCOMPARE( activations.at( 0 ).at( 0 ).toInt(), 7 );
}

void TstHotkeys::shutdownPolicyAbandonsAWedgedRecorderInsteadOfHanging()
{
  // A blocking XRecordEnableContext cannot be cancelled from another connection,
  // so the rule is: a loop that returned by the shutdown bound is joined, one that
  // did not is abandoned. A wedged X server cannot be produced inside the gate, so
  // the policy is pinned directly here, and the parking helper is pinned by
  // showing that it does not destroy the session handed to it.
  QVERIFY( HotkeyRuntime::recordingLoopReturnedInTime( true ) );
  QVERIFY( !HotkeyRuntime::recordingLoopReturnedInTime( false ) );

  int const destroyedBefore = FixtureRecorderSession::destroyedCount;
  {
    std::unique_ptr< RecorderSession > owned( new FixtureRecorderSession() );
    parkAbandonedRecorder( std::move( owned ), nullptr );

    // A parked core must survive its former QObject owner: if the parent is not
    // detached before parking, destroying the owner frees the core the detached
    // worker may still be inside.
    FixtureRecorderSession session;
    QObject * const owner = new QObject();
    auto * const core = new RecorderCore( &session, owner );
    QPointer< RecorderCore > watch( core );

    std::unique_ptr< RecorderSession > parkedSession( new FixtureRecorderSession() );
    parkAbandonedRecorder( std::move( parkedSession ),
                           std::unique_ptr< RecorderCore >( core ) );
    delete owner;

    QVERIFY2( !watch.isNull(), "parking must detach the core from its QObject parent" );
  }
  // Exactly one fixture session was destroyed here: the stack-local one used to
  // own a core. The parked sessions must still be alive, because a detached
  // recording thread would otherwise be reading freed storage.
  QCOMPARE( FixtureRecorderSession::destroyedCount, destroyedBefore + 1 );
}

void TstHotkeys::nonXcbCapability()
{
  // This case is the required offscreen run of this suite:
  //   QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_hotkeys/tst_hotkeys nonXcbCapability
  // On an xcb session there is a display and nothing to disable, so the case is
  // not applicable there instead of being green by accident.
  if( HotkeyRuntime::isX11Session( QGuiApplication::platformName() ) )
    QSKIP( "start the suite with a non-X11 QPA (QT_QPA_PLATFORM=offscreen) for this case" );

  QVERIFY( !QGuiApplication::platformName().startsWith( QStringLiteral( "xcb" ) ) );

#ifdef HAVE_X11
  X11RecordBackend backend;
  QSignalSpy activations( &backend, &HotkeyBackend::activated );

  QVERIFY( !backend.isSupported() );

  backend.bind( { { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                    QStringLiteral( "Ctrl+Alt+G" ) } } );
  backend.unbind();

  QCOMPARE( activations.count(), 0 );

  // A session without an X11 display gets a concrete disabled backend that
  // explains itself, never a silent no-op and never a fallback.
  QCOMPARE( backend.state(), HotkeyBackend::State::Unsupported );
  QVERIFY( !backend.explanation().isEmpty() );
  QVERIFY( backend.explanation().contains( QGuiApplication::platformName() ) );

  // And every Xlib call of the recorder lives behind the session factory, which
  // answers only for an xcb session: nothing here can open a display.
  QVERIFY( !createRecorderSession() );
  QVERIFY( !PlatformWindow::x11Display() );
  QVERIFY( !HotkeyRuntime::isX11Session( QGuiApplication::platformName() ) );
#endif

  // The other X11 consumers of the hotkey machinery are gated the same way: on this
  // session they answer instead of asking a null display.
  QCOMPARE( isRECORDBroken(), false );

  KeyboardState state;
  QVERIFY( state.checkModifiersPressed( 0 ) );
  QVERIFY( !state.checkModifiersPressed( KeyboardState::Alt ) );
  QVERIFY( !state.checkModifiersPressed( KeyboardState::Ctrl | KeyboardState::Shift ) );

  // The historical integer-handle wrapper is gated the same way.
  HotkeyWrapper wrapper( nullptr );
  QVERIFY( !wrapper.setGlobalKey( int( Qt::Key_G ), 0,
                                  Qt::ControlModifier | Qt::AltModifier, 0 ) );
}

void TstHotkeys::recorderBackendBindsStableIdsAndReportsEmptyTokens()
{
#ifdef HAVE_X11
  QVERIFY2( xcbSession(), "this case binds real X11 combinations and needs the xcb gate" );

  Display * const display = PlatformWindow::x11Display();
  QVERIFY( display != nullptr );

  X11RecordBackend backend;
  QVERIFY( backend.isSupported() );
  QCOMPARE( backend.state(), HotkeyBackend::State::Ready );

  QSignalSpy activations( &backend, &HotkeyBackend::activated );
  QSignalSpy states( &backend, &HotkeyBackend::stateChanged );

  backend.bind( { { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                    QStringLiteral( "CTRL+ALT+g" ) },
                  { QStringLiteral( "clipboard-lookup" ), QStringLiteral( "Clipboard lookup" ),
                    QStringLiteral( "CTRL+C" ) } } );

  QCOMPARE( backend.state(), HotkeyBackend::State::Bound );
  QCOMPARE( states.count(), 1 );
  QCOMPARE( states.at( 0 ).at( 0 ).value< HotkeyBackend::State >(),
            HotkeyBackend::State::Bound );
  QVERIFY( backend.explanation().isEmpty() );

  int const mainHandle = backend.handleForId( QStringLiteral( "main-window" ) );
  int const clipboardHandle = backend.handleForId( QStringLiteral( "clipboard-lookup" ) );
  QVERIFY( mainHandle > 0 );
  QVERIFY( clipboardHandle > 0 );
  QVERIFY( mainHandle != clipboardHandle );

  quint32 const control = XKeysymToKeycode( display, XK_Control_L );
  quint32 const alt = XKeysymToKeycode( display, XK_Alt_L );
  quint32 const g = backend.keyCodeForKey( int( Qt::Key_G ) );
  quint32 const c = backend.keyCodeForKey( int( Qt::Key_C ) );
  QVERIFY( control != 0 );
  QVERIFY( alt != 0 );
  QVERIFY( g != 0 );
  QVERIFY( c != 0 );

  // The live session really holds the bound combination: a second client is refused
  // it. That is what "the recorder hooked it" means to the rest of the system.
  QVERIFY2( !anotherClientCanGrab( g, RecorderControlMask | RecorderAltMask ),
            "the bound combination was not grabbed on the running server" );
  // The copy combination must not be grabbed: its press has to reach the
  // application, which is the difference between recording and swallowing a copy.
  QVERIFY2( anotherClientCanGrab( c, RecorderControlMask ),
            "the plain copy combination must stay available to other clients" );

  // An activation reports the ID of the combination that fired, with an empty
  // token: X11 has no portal activation token and none is invented.
  backend.recordedKey( true, control );
  backend.recordedKey( true, alt );
  backend.recordedKey( true, g );
  QCOMPARE( activations.count(), 1 );
  QCOMPARE( activations.at( 0 ).at( 0 ).toString(), QStringLiteral( "main-window" ) );
  QCOMPARE( activations.at( 0 ).at( 1 ).toString(), QString() );

  backend.recordedKey( false, g );
  backend.recordedKey( false, alt );
  backend.recordedKey( false, control );

  // The second combination reports its own ID, never the first one.
  backend.recordedKey( true, control );
  backend.recordedKey( true, c );
  backend.recordedKey( false, c );
  backend.recordedKey( false, control );
  QCOMPARE( activations.count(), 2 );
  QCOMPARE( activations.at( 1 ).at( 0 ).toString(),
            QStringLiteral( "clipboard-lookup" ) );
  QCOMPARE( activations.at( 1 ).at( 1 ).toString(), QString() );

  // A key that is bound to nothing reports nothing: no clipboard fallback, no
  // invented activation.
  backend.recordedKey( true, XKeysymToKeycode( display, XK_z ) );
  QCOMPARE( activations.count(), 2 );

  backend.unbind();
#else
  QSKIP( "this build has no X11 capability, so there is no X11 backend to bind" );
#endif
}

void TstHotkeys::recorderBackendReportsUnbindableSequences()
{
#ifdef HAVE_X11
  QVERIFY2( xcbSession(), "this case binds real X11 combinations and needs the xcb gate" );

  X11RecordBackend backend;
  QSignalSpy activations( &backend, &HotkeyBackend::activated );

  backend.bind( { { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                    QStringLiteral( "CTRL+ALT+g" ) },
                  { QStringLiteral( "clipboard-lookup" ), QStringLiteral( "Clipboard lookup" ),
                    QStringLiteral( "CTRL+ALT+NoSuchKeysym" ) } } );

  // A sequence the recorder cannot represent is reported, never dropped silently
  // and never rewritten into something else.
  QCOMPARE( backend.state(), HotkeyBackend::State::Partial );
  QVERIFY( backend.handleForId( QStringLiteral( "main-window" ) ) > 0 );
  QCOMPARE( backend.handleForId( QStringLiteral( "clipboard-lookup" ) ), 0 );
  QVERIFY( backend.explanation().contains( QStringLiteral( "clipboard-lookup" ) ) );

  // The legacy two-combination form is bindable: spec 4.11 keeps the stored defaults
  // (Ctrl+F11,F11 and Ctrl+C,C) working on X11, with the second combination completing
  // the chord through the recorder's second-key slot.
  X11RecordBackend chord;
  chord.bind( { { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                  QStringLiteral( "CTRL+F11,F11" ) } } );
  QCOMPARE( chord.state(), HotkeyBackend::State::Bound );
  QVERIFY( chord.handleForId( QStringLiteral( "main-window" ) ) > 0 );

  // The bindable half still works.
  Display * const display = PlatformWindow::x11Display();
  QVERIFY( display != nullptr );
  backend.recordedKey( true, XKeysymToKeycode( display, XK_Control_L ) );
  backend.recordedKey( true, XKeysymToKeycode( display, XK_Alt_L ) );
  backend.recordedKey( true, backend.keyCodeForKey( int( Qt::Key_G ) ) );
  QCOMPARE( activations.count(), 1 );
  QCOMPARE( activations.at( 0 ).at( 0 ).toString(), QStringLiteral( "main-window" ) );

  // A trigger with a modifier the recorder does not know is refused as well.
  backend.bind( { { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                    QStringLiteral( "HYPER+a" ) } } );
  QCOMPARE( backend.state(), HotkeyBackend::State::Unavailable );
  QCOMPARE( backend.handleForId( QStringLiteral( "main-window" ) ), 0 );
  QVERIFY( !backend.explanation().isEmpty() );

  backend.unbind();
#else
  QSKIP( "this build has no X11 capability, so there is no X11 backend to bind" );
#endif
}

void TstHotkeys::recorderBackendRetiresItsBindings()
{
#ifdef HAVE_X11
  QVERIFY2( xcbSession(), "this case binds real X11 combinations and needs the xcb gate" );

  Display * const display = PlatformWindow::x11Display();
  QVERIFY( display != nullptr );

  X11RecordBackend backend;
  QSignalSpy activations( &backend, &HotkeyBackend::activated );

  backend.bind( { { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                    QStringLiteral( "CTRL+ALT+g" ) } } );
  int const firstHandle = backend.handleForId( QStringLiteral( "main-window" ) );
  QVERIFY( firstHandle > 0 );

  quint32 const control = XKeysymToKeycode( display, XK_Control_L );
  quint32 const alt = XKeysymToKeycode( display, XK_Alt_L );
  quint32 const g = backend.keyCodeForKey( int( Qt::Key_G ) );
  QVERIFY( g != 0 );
  QVERIFY( !anotherClientCanGrab( g, RecorderControlMask | RecorderAltMask ) );

  // Binding again retires the previous registration: the ID gets a fresh handle and
  // no stale mapping survives.
  backend.bind( { { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                    QStringLiteral( "CTRL+ALT+g" ) } } );
  int const secondHandle = backend.handleForId( QStringLiteral( "main-window" ) );
  QVERIFY( secondHandle > 0 );
  QVERIFY( secondHandle != firstHandle );

  backend.recordedKey( true, control );
  backend.recordedKey( true, alt );
  backend.recordedKey( true, g );
  QCOMPARE( activations.count(), 1 );
  QCOMPARE( activations.at( 0 ).at( 0 ).toString(), QStringLiteral( "main-window" ) );
  backend.recordedKey( false, g );
  backend.recordedKey( false, alt );
  backend.recordedKey( false, control );

  backend.unbind();

  QCOMPARE( backend.state(), HotkeyBackend::State::Closed );
  QCOMPARE( backend.handleForId( QStringLiteral( "main-window" ) ), 0 );

  // The grab is released with the registration, so other clients may take it again.
  QVERIFY2( anotherClientCanGrab( g, RecorderControlMask | RecorderAltMask ),
            "the retired registration kept the combination grabbed" );

  // Nothing can fire after the registration was retired.
  backend.recordedKey( true, control );
  backend.recordedKey( true, alt );
  backend.recordedKey( true, g );
  QCOMPARE( activations.count(), 1 );
#else
  QSKIP( "this build has no X11 capability, so there is no X11 backend to retire" );
#endif
}

void TstHotkeys::xcbRecorderDeliversTheCombinationThroughRecord()
{
#ifdef HAVE_X11
  QVERIFY2( xcbSession(), "the live RECORD proof needs the xcb gate" );

  Display * const display = PlatformWindow::x11Display();
  QVERIFY( display != nullptr );

  X11RecordBackend backend;
  QSignalSpy activations( &backend, &HotkeyBackend::activated );

  backend.bind( { { QStringLiteral( "main-window" ), QStringLiteral( "Main window" ),
                    QStringLiteral( "CTRL+ALT+g" ) } } );
  QCOMPARE( backend.state(), HotkeyBackend::State::Bound );

  // The keys are pressed by the server's own input generator, so the activation can
  // only come through the real RECORD context, the recording thread and the
  // matcher: nothing here calls into the backend.
  //
  // The combination is repeated a few times because the recording thread has to be
  // inside its blocking enable call before the server delivers anything, and that
  // moment is not observable from here. Every attempt is a complete press and
  // release, so a delivered attempt reports exactly one activation.
  bool synthesized = true;

  for( int attempt = 0; attempt < 20 && activations.isEmpty(); ++attempt )
  {
    synthesized = fakeKey( display, XK_Control_L, true )
                  && fakeKey( display, XK_Alt_L, true )
                  && fakeKey( display, XK_g, true )
                  && fakeKey( display, XK_g, false )
                  && fakeKey( display, XK_Alt_L, false )
                  && fakeKey( display, XK_Control_L, false );
    XSync( display, False );
    QTest::qWait( 50 );
  }

  QVERIFY2( synthesized, "the gate's X server must provide XTEST" );
  QCOMPARE( activations.count(), 1 );
  QCOMPARE( activations.at( 0 ).at( 0 ).toString(), QStringLiteral( "main-window" ) );
  QCOMPARE( activations.at( 0 ).at( 1 ).toString(), QString() );

  backend.unbind();
#else
  QSKIP( "this build has no X11 capability, so there is no RECORD context to prove" );
#endif
}

void TstHotkeys::brokenXRecordCheckToleratesASessionWithoutDisplay()
{
  // No session may ask a null display: a Wayland or offscreen session has no X
  // RECORD extension to be broken, and the gate's own X server reports an X.Org
  // release outside the broken RECORD range.
  QCOMPARE( isRECORDBroken(), false );
}

void TstHotkeys::keyboardStateNeverClaimsAModifierWithoutADisplay()
{
  KeyboardState state;

  // An empty request is trivially satisfied, as it always was.
  QVERIFY( state.checkModifiersPressed( 0 ) );

  // Nothing is claimed about a modifier state this session cannot read: no key is
  // held on the gate's bare X server, and a session without an X11 display must not
  // claim either instead of dereferencing a null one.
  QVERIFY( !state.checkModifiersPressed( KeyboardState::Alt ) );
  QVERIFY( !state.checkModifiersPressed( KeyboardState::Ctrl | KeyboardState::Shift ) );
}

// Losing the disabled plugin value or normalizing raw shortcuts on OK would
// destroy settings the current backend cannot represent.
void TstHotkeys::preferencesPreserveUnavailableValues()
{
  Config::Class cfg;
  QString const raw = QStringLiteral( "Ctrl+F11,F11,Ctrl+F12" );
  cfg.preferences.mainWindowHotkey.rawText = raw;
  cfg.preferences.clipboardHotkey = Config::HotKey( QKeySequence( "Ctrl+C,C" ) );
  cfg.preferences.clipboardHotkey.rawText = "Ctrl+C,C";
  cfg.preferences.enableWebPlugins = true;
  {
    Preferences dialog( nullptr, cfg );
    auto plugins = dialog.findChild<QCheckBox *>( "enableWebPlugins" );
    QVERIFY( plugins );
    QVERIFY( !plugins->isEnabled() );
    QVERIFY( plugins->isChecked() );
    auto const accepted = dialog.getPreferences();
    QCOMPARE( accepted.mainWindowHotkey.rawText, raw );
    QCOMPARE( accepted.clipboardHotkey.rawText, QStringLiteral( "Ctrl+C,C" ) );
    QVERIFY( accepted.enableWebPlugins );
    dialog.reject();
  }
  QCOMPARE( cfg.preferences.mainWindowHotkey.rawText, raw );
  Preferences edited( nullptr, cfg );
  auto recorder = edited.findChild<HotKeyEdit *>( "mainWindowHotkey" );
  QVERIFY( recorder );
  QTest::keyClick( recorder, Qt::Key_G, Qt::ControlModifier );
  auto const changed = edited.getPreferences().mainWindowHotkey;
  QVERIFY( changed.rawText.isEmpty() );
  QCOMPARE( changed.toKeySequence().toString( QKeySequence::PortableText ), QStringLiteral( "Ctrl+G" ) );
}

// An unavailable/denied backend must never look active because an old portal
// description survived. Partial states retain the per-ID display and reason.
void TstHotkeys::preferencesReportHotkeyCapabilities()
{
  Config::Class cfg;
  Preferences dialog( nullptr, cfg );
  auto state = dialog.findChild<QLabel *>( "hotkeyCapability" );
  auto main = dialog.findChild<QLabel *>( "mainWindowHotkeyStatus" );
  auto clipboard = dialog.findChild<QLabel *>( "clipboardHotkeyStatus" );
  auto configure = dialog.findChild<QPushButton *>( "configureShortcuts" );
  QVERIFY( state && main && clipboard && configure );
  dialog.setHotkeyStatus( "xcb", HotkeyBackend::State::Bound, {}, {}, false );
  QVERIFY( state->text().contains( "X11 active" ) );
  dialog.setHotkeyStatus( "xcb", HotkeyBackend::State::Partial, "clipboard-lookup was refused", {}, false );
  QVERIFY( !main->text().contains( "Active", Qt::CaseInsensitive ) );
  QVERIFY( clipboard->text().contains( "refused" ) );
  cfg.preferences.mainWindowHotkey = Config::HotKey( QKeySequence( "Ctrl+Alt+G" ) );
  Preferences portal( nullptr, cfg );
  state = portal.findChild<QLabel *>( "hotkeyCapability" );
  main = portal.findChild<QLabel *>( "mainWindowHotkeyStatus" );
  clipboard = portal.findChild<QLabel *>( "clipboardHotkeyStatus" );
  configure = portal.findChild<QPushButton *>( "configureShortcuts" );
  QVector<PortalBindingDisplay> const reported{ { "main-window", "Assigned in System Settings" } };
  portal.setHotkeyStatus( "wayland", HotkeyBackend::State::Partial, "one binding refused", reported, true );
  QVERIFY( state->text().contains( "partial", Qt::CaseInsensitive ) );
  QVERIFY( main->text().contains( "Assigned in System Settings" ) );
  QVERIFY( clipboard->text().contains( "unavailable", Qt::CaseInsensitive ) );
  QVERIFY( clipboard->text().contains( "Wayland" ) );
  QVERIFY( configure->isEnabled() );
  QSignalSpy requested( &portal, &Preferences::configureShortcutsRequested );
  configure->click();
  QCOMPARE( requested.size(), 1 );
  portal.setHotkeyStatus( "wayland", HotkeyBackend::State::Denied, "permission refused", reported, false );
  QVERIFY( state->text().contains( "permission refused" ) );
  QVERIFY( !configure->isEnabled() );
  QVERIFY( main->text().contains( "permission refused" ) );
  portal.setHotkeyStatus( "wayland", HotkeyBackend::State::Unavailable, "portal absent", {}, false );
  QVERIFY( state->text().contains( "portal absent" ) );
  auto note = portal.findChild<QLabel *>( "clipboardCapabilityNote" );
  QVERIFY( note );
  QVERIFY( note->text().contains( "clipboard-change", Qt::CaseInsensitive ) );
  QVERIFY( note->text().contains( "explicit clipboard", Qt::CaseInsensitive ) );
  QVERIFY( note->text().contains( "Hover" ) );

  // Unparseable raw text must not acquire the parsed field's valid trigger.
  cfg.preferences.mainWindowHotkey.rawText = "Ctrl+G,Ctrl+H,Ctrl+I";
  Preferences unsupported( nullptr, cfg );
  unsupported.setHotkeyStatus( "wayland", HotkeyBackend::State::Bound, {}, reported, true );
  main = unsupported.findChild<QLabel *>( "mainWindowHotkeyStatus" );
  QVERIFY( main->text().contains( "Unavailable", Qt::CaseInsensitive ) );
  QVERIFY( !main->text().contains( "Assigned" ) );
}

void TstHotkeys::preferencesExplainRendererParity()
{
  Config::Class cfg;
  Preferences dialog( nullptr, cfg );
  auto note = dialog.findChild<QLabel *>( "rendererParityNote" );
  QVERIFY( note );
  QVERIFY( note->text().contains( "inline", Qt::CaseInsensitive ) );
  QVERIFY( note->text().contains( "snapshot", Qt::CaseInsensitive ) );
  auto proxy = dialog.findChild<QComboBox *>( "proxyType" );
  QVERIFY( proxy );
  QVERIFY( proxy->toolTip().contains( "Chromium" ) );
  QVERIFY( proxy->toolTip().contains( "HTTP proxy" ) );
}

void TstHotkeys::preferencesKeepAudioVariant()
{
  Config::Class cfg;
  cfg.preferences.internalPlayerBackend.setUiName( "Legacy custom backend" );
  Preferences dialog( nullptr, cfg );
  auto internal = dialog.findChild<QRadioButton *>( "useInternalPlayer" );
  auto backend = dialog.findChild<QComboBox *>( "internalPlayerBackend" );
  auto external = dialog.findChild<QRadioButton *>( "useExternalPlayer" );
  QVERIFY( internal && backend && external );
#ifdef MAKE_FFMPEG_PLAYER
  QCOMPARE( backend->count(), 1 );
  QCOMPARE( dialog.getPreferences().internalPlayerBackend.uiName(), QStringLiteral( "FFmpeg+libao" ) );
#else
  QVERIFY( internal->isHidden() );
  QVERIFY( backend->isHidden() );
  QVERIFY( external->isChecked() );
  QCOMPARE( dialog.getPreferences().internalPlayerBackend.uiName(), QStringLiteral( "Legacy custom backend" ) );
#endif
}

void TstHotkeys::scanPopupBindingIsPortalOnly()
{
  for( QString const & platform : QStringList{ "xcb", "wayland", "wayland-egl", "offscreen", "windows", "cocoa" } )
  {
    bool const wayland = platform == "wayland" || platform == "wayland-egl";
    for( bool enabled : { false, true } )
      QCOMPARE( scanPopupBindingPlanned( platform, enabled ), enabled && wayland );
    for( bool build : { false, true } )
      QCOMPARE( scanPopupCaptureSupported( build, platform ), build && wayland );
  }
}
void TstHotkeys::portalIdPredicateAcceptsThreeIds()
{
  for( QString const & id : QStringList{ "main-window", "clipboard-lookup", "scan-popup" } )
    QVERIFY( isKnownPortalBindingId( id ) );
  QVERIFY( !isKnownPortalBindingId( {} ) ); QVERIFY( !isKnownPortalBindingId( "foreign-action" ) );
  qInfo( "source assertions: shared portal ID predicate and call sites" );
  QString const header = selectionSourceText( "hotkeybackend.hh" );
  QString const config = selectionSourceText( "config.cc" );
  QString const portal = selectionSourceText( "portalhotkeys.cc" );
  QVERIFY( !header.isEmpty() && !config.isEmpty() && !portal.isEmpty() );
  QCOMPARE( header.count( "inline bool isKnownPortalBindingId(" ), 1 );
  QString const predicate = selectionBlock( header, "inline bool isKnownPortalBindingId(" );
  for( QString const & id : QStringList{ "main-window", "clipboard-lookup", "scan-popup" } )
    QVERIFY2( predicate.contains( '"' + id + '"' ), qPrintable( "source assertion: missing ID " + id ) );
  QRegularExpression const definition( "\\bbool\\s+(?:isKnownPortalBindingId|knownBindingId)\\s*\\(" );
  QVERIFY2( !definition.match( config ).hasMatch(), "source assertion: config retains a predicate definition" );
  QVERIFY2( !definition.match( portal ).hasMatch(), "source assertion: portal retains a predicate definition" );
  QVERIFY( selectionBlock( config, "loadPortalBindings(" ).contains( "isKnownPortalBindingId( id )" ) );
  QVERIFY( selectionBlock( config, "void save( Class const & c )" ).contains( "isKnownPortalBindingId( display.id )" ) );
  QString const generation = selectionBlock( portal, "void PortalBackend::beginGeneration(" );
  QVERIFY( !generation.isEmpty() );
  QVERIFY( selectionCompact( generation ).contains( "isKnownPortalBindingId(binding.id)" ) );
  QVERIFY( !generation.contains( "knownBindingId(" ) );
}
void TstHotkeys::threeBindingsStayBound()
{
  using State = HotkeyBackend::State;
  QStringList const all{ "main-window", "clipboard-lookup", "scan-popup" };
  QVector<HotkeyBinding> const desired{
    { "main-window", "Main", "CTRL+ALT+g" },
    { "clipboard-lookup", "Clipboard", "CTRL+ALT+h" },
    { "scan-popup", "Translate the selected text", "CTRL+LOGO+d" }
  };
  for( QStringList const & returned : QList<QStringList>{ all, { "main-window", "scan-popup" } } )
  {
    TestSupport::PrivateSessionBus bus; QString reason;
    QVERIFY2( bus.start( &reason ), qPrintable( reason ) );
    TestSupport::FakePortal portal;
    QVERIFY2( portal.start( bus.fixtureConnection(), 2 ), qPrintable( portal.errorString() ) );
    portal.setListShortcuts( {} );
    portal.setBoundIds( returned );
    QVector<PortalBindingDisplay> reported;
    PortalBackend backend( bus.connection() );
    QSignalSpy states( &backend, &HotkeyBackend::stateChanged );
    connect( &backend, &HotkeyBackend::bindingsChanged, this,
             [&]( QVector<PortalBindingDisplay> const & value ) { reported = value; } );
    backend.bind( desired );
    State const expected = returned.size() == 3 ? State::Bound : State::Partial;
    QVERIFY2( QTest::qWaitFor( [&] { return backend.state() == expected; }, 1500 ), "three-ID portal bind did not settle" );
    for( auto const & event : states ) QVERIFY( qvariant_cast<State>( event[0] ) != State::Unavailable );
    int requests = 0;
    for( auto const & call : portal.calls() ) if( call.member == "BindShortcuts" )
    {
      ++requests; QCOMPARE( call.shortcutIds, all );
      QSet<QString> unique; for( auto const & id : call.shortcutIds ) unique.insert( id );
      QCOMPARE( unique.size(), 3 );
      QCOMPARE( call.shortcuts.size(), 3 );
      for( int i = 0; i != 3; ++i )
        QCOMPARE( call.shortcuts[i].properties.value( "preferred_trigger" ).toString(), desired[i].sequence );
    }
    QCOMPARE( requests, 1 ); QVERIFY( portal.protocolViolations().isEmpty() );
    QStringList reportedIds; for( auto const & entry : reported ) reportedIds.append( entry.id );
    QStringList sortedReturned = returned; reportedIds.sort(); sortedReturned.sort();
    QCOMPARE( reportedIds, sortedReturned );
  }
}

void TstHotkeys::capabilityConstantMatchesBuild()
{
#ifdef HAVE_WAYLAND_DATA_CONTROL
  QCOMPARE( kHaveWaylandDataControl, true );
#else
  QCOMPARE( kHaveWaylandDataControl, false );
#endif
  qInfo( "source assertions: nested qmake optional-feature scopes" );
  for( char const * file : { "goldendict.pro", "tests/testsupport.pri" } )
  {
    QString const project = selectionSourceText( file ); QVERIFY( !project.isEmpty() );
    QString const unixScope = selectionBlock( project, "unix:!mac {" ); QVERIFY( !unixScope.isEmpty() );
    QString const probe = selectionBlock( unixScope, "packagesExist(wayland-client)" ); QVERIFY( !probe.isEmpty() );
    QVERIFY2( probe.contains( "DEFINES += HAVE_WAYLAND_DATA_CONTROL" ), file );
    if( QString::fromLatin1( file ) == "goldendict.pro" )
    {
      QVERIFY( probe.contains( "PKGCONFIG += wayland-client" ) );
      QVERIFY( probe.contains( "INCLUDEPATH += $$PWD/protocols/ext-data-control-v1" ) );
      QVERIFY( probe.contains( "SOURCES += $$PWD/protocols/ext-data-control-v1/ext-data-control-v1-protocol.c" ) );
    }
    else QVERIFY( !project.contains( "PKGCONFIG += wayland-client" ) );
  }
  QString const hotkeys = selectionSourceText( "tests/tst_hotkeys/tst_hotkeys.pro" );
  QVERIFY( !hotkeys.contains( "$$GD_REPO_ROOT/waylandselection.cc" ) );
  QVERIFY( !hotkeys.contains( "HEADERS += $$GD_REPO_ROOT/waylandselection.hh" ) );
}
void TstHotkeys::scanPopupSourceLoopContractIsPinned()
{
  qInfo( "source assertions: protocol loop/teardown contracts; no syscall or compositor observation" );
  QString const source = selectionSourceText( "waylandselection.cc" ); QVERIFY( !source.isEmpty() );
  auto method = [&]( QString const & name ) { return selectionBlock( source, name ); };
  QString const start = method( "void start()" );
  QVERIFY( !start.isEmpty() );
  QVERIFY( selectionOrdered( start, { "if( started || ownerStopped ) return", "started = true", "wl_display_connect( nullptr )",
                                      "wl_display_get_registry", "wl_registry_add_listener", "wl_display_sync", "wl_callback_add_listener", "flushRequests()" } ) );
  QVERIFY( start.contains( "fail( \"cannot get Wayland registry\" ); return" ) );
  QVERIFY( start.contains( "fail( \"cannot create Wayland sync callback\" ); return" ) );
  QString const done = method( "static void discoveryDone(" );
  QVERIFY( selectionOrdered( done, { "wl_callback_destroy", "discoveryComplete = true", "if( !self->manager )", "no ext_data_control_manager_v1", "maybeAvailable()" } ) );
  QString const removed = method( "static void registryGlobalRemoved(" );
  QVERIFY( selectionOrdered( removed, { "name == self->managerName", "manager removed", "name != self->seatNameId", "retirePrimary()",
                                        "destroyDeviceAndSeat()", "seats.isEmpty()", "seat removed", "bindSeat" } ) );
  QString const primary = method( "static void primarySelection(" );
  QVERIFY( selectionOrdered( primary, { "receivedAt = Monotonic::nowMs()", "primary-selection-seen=1", "retirePrimary()", "pendingOffer", "takeForPrimary()", "offerStamp = receivedAt", "flushRequests()" } ) );
  QString const clipboard = method( "static void selection(" );
  QVERIFY( selectionOrdered( clipboard, { "pendingOffer", "discardForClipboard()", "destroyOffer", "flushRequests()" } ) );
  QVERIFY( !clipboard.contains( "receive(" ) && !clipboard.contains( "cache =" ) );
  QString const promote = method( "void promote()" );
  QCOMPARE( source.count( "transferStartedAt = Monotonic::nowMs()" ), 1 );
  QVERIFY( selectionOrdered( promote, { "if( !transferStartedAt )", "transferStartedAt = Monotonic::nowMs()", "TransferTimeoutMs", "startMimeAttempt()" } ) );
  QString const attempt = method( "bool startMimeAttempt()" );
  QVERIFY( selectionOrdered( attempt, { "beginMimeAttempt()", "pipe2( fds, O_CLOEXEC )", "fcntl( fds[0], F_GETFL )",
                                        "fcntl( fds[0], F_SETFL, oldFlags | O_NONBLOCK )", "beginRead()", "ext_data_control_offer_v1_receive", "close( fds[1] )", "flushRequests()" } ) );
  QVERIFY( !attempt.contains( "O_CLOEXEC | O_NONBLOCK" ) );
  QVERIFY( !attempt.contains( "fcntl( fds[1]" ) );
  QVERIFY( !attempt.contains( "transferStartedAt =" ) );
  QString const drain = method( "void drainPipe(" );
  QVERIFY( selectionOrdered( drain, { "offersState.accepts( token )", "checkTransferDeadline( token )", "char buffer[16384]",
                                      "MaxPayloadBytes + 1 - bytesObserved", "::read(", "if( count > 0 )", "bytesObserved == MaxPayloadBytes + 1", "if( count == 0 )" } ) );
  QVERIFY( drain.contains( "errno == EINTR" ) && drain.contains( "errno == EAGAIN || errno == EWOULDBLOCK" ) );
  QString const readFailure = selectionBlock( drain, "if( bytesObserved == 0 )" );
  QVERIFY( selectionOrdered( readFailure, { "offersState.mimeAttemptFailed()", "closeRead()", "startMimeAttempt()" } ) );
  QVERIFY( selectionCompact( drain ).contains( "elsedropCapture(false);" ) );
  QString const timeout = method( "void checkTransferDeadline(" );
  QVERIFY( selectionOrdered( timeout, { "now >= transferDeadline", "now >= lastProgressAt + IdleTimeoutMs", "bytesObserved == 0", "mimeAttemptFailed()", "startMimeAttempt()" } ) );
  QString const success = method( "void complete(" );
  QVERIFY( selectionOrdered( success, { "offersState.accepts( token )", "checkTransferDeadline( token )", "offersState.accepts( token )", "cache = decodePayload", "cacheStamp =", "offersState.completed( token )", "closeRead()", "destroyOffer", "flushRequests()", "expireInterest()", "answerInterest" } ) );
  QString const retire = method( "void retirePrimary()" );
  QVERIFY( selectionOrdered( retire, { "offersState.retire()", "debounce.stop()", "closeRead()", "destroyOffer" } ) );
  QString const readable = method( "void dispatchDisplayReadable()" );
  QString const prepareFailure = selectionBlock( readable, "if( wl_display_prepare_read( c->display ) != 0 )" );
  QVERIFY( !prepareFailure.isEmpty() );
  QVERIFY( prepareFailure.contains( "dispatchPending( c, generation )" ) );
  QVERIFY( !prepareFailure.contains( "wl_display_read_events" ) );
  QVERIFY( selectionCompact( prepareFailure ).endsWith( "return;" ) );
  QVERIFY( prepareFailure.contains( "if( flushRequests() && live() ) displayRead->setEnabled( true );" ) );
  QString const pollNotReady = selectionBlock( readable, "if( ::poll(" );
  QVERIFY( !pollNotReady.isEmpty() && !pollNotReady.contains( "wl_display_read_events" ) );
  QVERIFY( selectionOrdered( pollNotReady, { "wl_display_cancel_read", "c->prepared = false", "flushRequests()", "displayRead->setEnabled( true )", "return" } ) );
  QVERIFY( readable.trimmed().endsWith( "if( flushRequests() && live() ) displayRead->setEnabled( true );" ) );
  QVERIFY( selectionOrdered( readable, { "displayRead->setEnabled( false )", "wl_display_prepare_read", "c->prepared = true",
                                         "poll(", "wl_display_cancel_read", "++c->dispatchDepth", "wl_display_read_events", "--c->dispatchDepth", "c->prepared = false", "c->retired || c->generation != generation", "dispatchPending" } ) );
  QString const dispatch = method( "bool dispatchPending(" );
  QVERIFY( selectionOrdered( dispatch, { "++c->dispatchDepth", "wl_display_dispatch_pending", "--c->dispatchDepth", "c->retired || c->generation != generation", "destroyConnection()" } ) );
  QString const flush = method( "bool flushRequests()" );
  QVERIFY( selectionOrdered( flush, { "wl_display_flush", "if( result >= 0 )", "displayWrite->setEnabled( false )", "errno == EINTR", "errno == EAGAIN", "displayWrite->setEnabled( true )", "fail( \"Wayland display error\" )" } ) );
  QVERIFY( selectionBlock( flush, "if( errno == EAGAIN )" ).contains( "return true" ) );
  QString const writable = method( "void dispatchDisplayWritable()" );
  QVERIFY( writable.contains( "if( ownerStopped || !connection || connection->retired ) return" ) );
  QVERIFY( writable.contains( "flushRequests()" ) );
  QString const dispose = method( "void dispose()" );
  QVERIFY( selectionOrdered( dispose, { "c->retired = true", "++c->generation", "offersState.retire()", "debounce.stop()", "activation.stop()",
                                       "closeRead()", "displayRead.reset()", "displayWrite.reset()", "destroyProtocol()", "if( c->dispatchDepth == 0 )", "c->destroyConnection()", "[c]" } ) );
  QVERIFY( !dispose.contains( "[this" ) );
  QVERIFY( selectionOrdered( dispose, { "destroyProtocol()", "terminalNotificationPending = false", "publishState()", "captureFinished( id, QString(), 0 )", "if( c->dispatchDepth == 0 )", "c->destroyConnection()" } ) );
  QString const destroy = method( "void destroyConnection()" );
  QVERIFY( selectionOrdered( destroy, { "if( destroyed_ ) return", "Q_ASSERT( dispatchDepth == 0 )", "destroyed_ = true", "if( prepared )", "wl_display_cancel_read", "wl_display_flush", "wl_display_disconnect", "display = nullptr" } ) );
  QVERIFY( source.contains( "std::shared_ptr<Connection> connection" ) );
  QVERIFY( method( "~Connection()" ).contains( "destroyConnection()" ) );
  QVERIFY( method( "void stop()" ).contains( "ownerStopped = true" ) );
  QVERIFY( selectionOrdered( method( "void stop()" ), { "ownerStopped = true", "dispose()" } ) );
  QVERIFY( method( "void fail(" ).contains( "dispose()" ) );
  QVERIFY( method( "WaylandSelectionSource::~WaylandSelectionSource()" ).contains( "stop()" ) );
  for( QString const & callback : QStringList{ "registryGlobal", "registryGlobalRemoved", "discoveryDone", "dataOffer", "offeredMime", "selection", "primarySelection", "deviceFinished", "seatCapabilities", "seatName" } )
  {
    QString const b = method( "static void " + callback + "(" ); QVERIFY2( !b.isEmpty(), qPrintable( callback ) );
    QVERIFY2( !b.contains( "wl_display_disconnect" ), qPrintable( "source assertion: listener disconnects: " + callback ) );
    QVERIFY2( !QRegularExpression( "\\bdelete\\b|\\.reset\\s*\\(|destroyConnection\\s*\\(|deleteLater\\s*\\(" ).match( b ).hasMatch(), qPrintable( "source assertion: listener performs physical/owner teardown: " + callback ) );
  }
  QString const request = method( "void requestCapture( quint64 id )" );
  QString const overlap = selectionBlock( request, "if( superseded )" ); QVERIFY( !overlap.isEmpty() );
  QVERIFY( !overlap.contains( "beginRead" ) && !overlap.contains( "activeToken" ) && !overlap.contains( "retirePrimary" ) );
  QVERIFY( selectionOrdered( request, { "interestedId = id", "armActivation()", "if( superseded )", "captureFinished( superseded", "if( ownerStopped || interestedId != id ) return" } ) );
  QVERIFY( !source.contains( "wl_display_roundtrip" ) );
  QVERIFY( !source.contains( "nativeInterface" ) && !source.contains( "PlatformWindow::x11Display" ) );
  QVERIFY( !source.contains( "create_data_source(" ) && !source.contains( "set_primary_selection(" ) && !source.contains( "set_selection(" ) );
}

// ---------------------------------------------------------------------------
// Opt-in selected-text shortcut preference (§5)
// ---------------------------------------------------------------------------

// The default the specification names: one Ctrl+Meta+D combination, off by
// default, bindable as a portal shortcut and stored as its PortableText.
void TstHotkeys::defaultScanPopupHotkeyIsBindable()
{
  Config::Class cfg;
  auto const & p = cfg.preferences;

  QVERIFY( !p.enableScanPopupHotkey );
  QCOMPARE( p.scanPopupHotkey.key1, int( Qt::Key_D ) );
  QCOMPARE( p.scanPopupHotkey.key2, 0 );
  QCOMPARE( p.scanPopupHotkey.modifiers,
            Qt::KeyboardModifiers( Qt::ControlModifier | Qt::MetaModifier ) );
  QCOMPARE( p.scanPopupHotkey.toKeySequence().count(), 1 );
  QCOMPARE( p.scanPopupHotkey.toKeySequence().toString( QKeySequence::PortableText ),
            QString( "Meta+Ctrl+D" ) );

  QString trigger, reason;
  QVERIFY2( HotkeySyntax::preferredTrigger( p.scanPopupHotkey, &trigger, &reason ),
            qPrintable( reason ) );
  QVERIFY( reason.isEmpty() );
  QCOMPARE( trigger, QString( "CTRL+LOGO+d" ) );

  auto const saved = saveAndRead( cfg );
  QCOMPARE( saved.documentElement().firstChildElement( "preferences" )
              .firstChildElement( "scanPopupHotkey" ).text(),
            QString( "Meta+Ctrl+D" ) );
}

// The additive <hotkeys>/<portalBindings> section is the only place a portal
// description is stored, it keeps escaped text, and later duplicates are
// last-wins. A stored description never turns a foreign ID into a known one.
void TstHotkeys::scanPopupPortalBindingSurvivesSaveLoad()
{
  Config::Class cfg;
  cfg.hotkeys.portalBindings = { { "main-window", "Main" },
                                 { "clipboard-lookup", "Clipboard" },
                                 { "scan-popup", "Portal <selected> & key" },
                                 { "foreign-action", "Foreign" } };

  QDomDocument saved = saveAndRead( cfg );
  auto bindings = saved.documentElement().firstChildElement( "hotkeys" )
                    .firstChildElement( "portalBindings" );
  auto const nodes = bindings.elementsByTagName( "binding" );

  QCOMPARE( nodes.size(), 3 );

  QSet<QString> ids;

  for( int i = 0; i != nodes.size(); ++i )
  {
    auto const e = nodes.at( i ).toElement();
    ids.insert( e.attribute( "id" ) );

    if( e.attribute( "id" ) == "scan-popup" )
      QCOMPARE( e.attribute( "triggerDescription" ), QString( "Portal <selected> & key" ) );
  }

  QCOMPARE( ids, ( QSet<QString>{ "main-window", "clipboard-lookup", "scan-popup" } ) );

  auto loaded = Config::load();
  int count = 0;

  for( auto const & entry : loaded.hotkeys.portalBindings )
    if( entry.id == "scan-popup" )
    {
      ++count;
      QCOMPARE( entry.triggerDescription, QString( "Portal <selected> & key" ) );
    }

  QCOMPARE( count, 1 );
  QCOMPARE( loaded.preferences.scanPopupHotkey.toKeySequence().toString( QKeySequence::PortableText ),
            QString( "Meta+Ctrl+D" ) );

  for( auto const & pair : QList<QPair<QString, QString>>{
         { "scan-popup", "last <description> & value" }, { "foreign-action", "ignored" } } )
  {
    auto e = saved.createElement( "binding" );
    e.setAttribute( "id", pair.first );
    e.setAttribute( "triggerDescription", pair.second );
    bindings.appendChild( e );
  }

  QFile file( Config::getConfigFileName() );
  QVERIFY( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
  auto bytes = saved.toByteArray();
  QCOMPARE( file.write( bytes ), bytes.size() );
  file.close();

  loaded = Config::load();
  count = 0;

  for( auto const & entry : loaded.hotkeys.portalBindings )
  {
    QVERIFY( entry.id != "foreign-action" );

    if( entry.id == "scan-popup" )
    {
      ++count;
      QCOMPARE( entry.triggerDescription, QString( "last <description> & value" ) );
    }
  }

  QCOMPARE( count, 1 );
}

// An untouched stored value is written back exactly as it was read, including
// text that is not a representable shortcut. Opening the preferences dialog and
// accepting it unchanged is part of the round trip (SD-15).
void TstHotkeys::scanPopupHotkeyKeepsVerbatimText()
{
  for( QString const & raw : QStringList{ "Ctrl+Meta+D", "Ctrl+G,Ctrl+H,Ctrl+I", "not a shortcut" } )
  {
    QVERIFY( !installConfigFixture( "config/legacy.xml" ).isEmpty() );
    QVERIFY( selectionAmendPreference( "enableScanPopupHotkey", "1" ) );
    QVERIFY( selectionAmendPreference( "scanPopupHotkey", raw ) );

    auto cfg = Config::load();
    QCOMPARE( cfg.preferences.scanPopupHotkey.rawText, raw );

    Preferences dialog( nullptr, cfg );
    dialog.accept();
    cfg.preferences = dialog.getPreferences();

    auto const saved = saveAndRead( cfg );
    QCOMPARE( saved.documentElement().firstChildElement( "preferences" )
                .firstChildElement( "scanPopupHotkey" ).text(),
              raw );
    QCOMPARE( Config::load().preferences.scanPopupHotkey.rawText, raw );
  }
}

// A configuration written before the preference existed keeps the constructed
// default: the boolean stays off and the shortcut stays the shipped combination.
void TstHotkeys::legacyConfigDefaultsScanPopupHotkeyOff()
{
  QString const installed = installConfigFixture( "config/legacy.xml" );
  QVERIFY( !installed.isEmpty() );

  QByteArray const before = readFileBytes( installed );
  auto cfg = Config::load();

  QCOMPARE( readFileBytes( installed ), before );
  QVERIFY( !cfg.preferences.enableScanPopupHotkey );

  auto const & key = cfg.preferences.scanPopupHotkey;
  QCOMPARE( key.key1, int( Qt::Key_D ) );
  QCOMPARE( key.key2, 0 );
  QCOMPARE( key.modifiers, Qt::KeyboardModifiers( Qt::ControlModifier | Qt::MetaModifier ) );
  QCOMPARE( key.toKeySequence().count(), 1 );
  QCOMPARE( key.toKeySequence().toString( QKeySequence::PortableText ), QString( "Meta+Ctrl+D" ) );

  auto const saved = saveAndRead( cfg );
  auto const p = saved.documentElement().firstChildElement( "preferences" );

  QCOMPARE( p.firstChildElement( "enableScanPopupHotkey" ).text(), QString( "0" ) );
  QCOMPARE( p.firstChildElement( "scanPopupHotkey" ).text(), QString( "Meta+Ctrl+D" ) );

  auto const reloaded = Config::load();
  QVERIFY( !reloaded.preferences.enableScanPopupHotkey );
  QCOMPARE( reloaded.preferences.scanPopupHotkey.toKeySequence().toString( QKeySequence::PortableText ),
            QString( "Meta+Ctrl+D" ) );
}

// Only "1" and "0" set the boolean; every other stored value (including an
// empty one) leaves the default intact. An explicitly empty shortcut node is an
// empty shortcut, while a missing node keeps the constructed default.
void TstHotkeys::scanPopupConfigBooleanValues()
{
  QStringList const values{ "1", "0", "", "true", "2", "missing" };

  for( int i = 0; i != values.size(); ++i )
  {
    QVERIFY( !installConfigFixture( "config/legacy.xml" ).isEmpty() );
    QVERIFY( selectionAmendPreference( "enableScanPopupHotkey", values[i], i != 5 ) );
    QCOMPARE( Config::load().preferences.enableScanPopupHotkey, i == 0 );
  }

  QVERIFY( selectionAmendPreference( "scanPopupHotkey", "" ) );

  auto const empty = Config::load().preferences.scanPopupHotkey;
  QVERIFY( empty.toKeySequence().isEmpty() );

  QString trigger, reason;
  QVERIFY( !HotkeySyntax::preferredTrigger( empty, &trigger, &reason ) );

  QVERIFY( selectionAmendPreference( "scanPopupHotkey", "", false ) );
  QCOMPARE( Config::load().preferences.scanPopupHotkey.toKeySequence().toString( QKeySequence::PortableText ),
            QString( "Meta+Ctrl+D" ) );
}

QTEST_MAIN( TstHotkeys )
#include "tst_hotkeys.moc"
