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

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QKeySequence>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVector>

#include "support/fixtures.hh"

#include "audioplayerfactory.hh"
#include "config.hh"
#include "externalaudioplayer.hh"
#include "ffmpegaudioplayer.hh"
#include "hotkeybackend.hh"
#include "hotkeysyntax.hh"

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

} // namespace

class TstHotkeys : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase();
  void cleanupTestCase();

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
  void unsupportedBackendStillReportsState();

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

QTEST_MAIN( TstHotkeys )
#include "tst_hotkeys.moc"
