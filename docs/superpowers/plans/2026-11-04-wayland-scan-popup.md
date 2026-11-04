# Wayland Selected-Text Scan Popup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the deterministic
> subagent-driven-development controller to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax; leave the checked-in plan immutable.

**Goal:** Add the opt-in Wayland selected-text portal shortcut with ext-data-control capture, timestamp-based clipboard fallback and an honest Preferences status row.

**Architecture:** A Qt Core decision module owns MIME, payload, offer-token and recency rules. A GUI-thread source uses its own libwayland connection; MainWindow owns it, registers the portal action independently of capture capability, and sends the chosen text through ScanPopup's existing translation path. Preferences consumes value snapshots; pure tests, source assertions, a separate real-protocol harness and manual application checks have distinct evidence boundaries.

**Tech Stack:** C++17, Qt 6.12, qmake6, QtTest, QtDBus, optional libwayland-client, wayland-protocols 1.50 XML and wayland-scanner 1.26.0 for regeneration only.

**Authority:** `docs/superpowers/specs/2026-11-04-wayland-scan-popup-spec.md` at `5552312d7f11b41ee447b97ce9252b58ae77471a` (1849 lines). The revision-7 design supplies intent only; the specification wins where its prose corrects older design examples, especially timeout cache suppression and source reconciliation. Line ranges below refer to this baseline; locate edits by the named enclosing function after earlier tasks shift lines.

### File responsibilities

| Files | Responsibility / implementing task |
| --- | --- |
| `waylandselectionlogic.hh`, `waylandselectionlogic.cc`, `tests/tst_waylandselection/tst_waylandselection.pro`, `tests/tst_waylandselection/tst_waylandselection.cc` | Pure API and its complete logic-only suite; Task 1. |
| `tests/tests.pro`, aggregate comments in `goldendict.pro` | Closed inventory becomes 20 in the commit creating the suite; Task 1. |
| `hotkeybackend.hh`, local predicates in `config.cc` and `portalhotkeys.cc` | Shared three-ID and separate binding/capture predicates; Task 2. |
| `waylandselection.hh`, `waylandselection.cc`, `protocols/ext-data-control-v1/{ext-data-control-v1.xml,ext-data-control-v1-client-protocol.h,ext-data-control-v1-protocol.c,regenerate.sh}` | Public snapshot and complete private source, vendored protocol and deterministic regeneration; Task 3. |
| Feature scopes in `goldendict.pro`, `tests/testsupport.pri` | Optional library/protocol compilation and matching test macro without a global test link; Task 3. |
| `config.hh`, remaining `config.cc` edits | Off-by-default preferences and real XML load/save; Task 4. |
| `mainwindow.hh`, `mainwindow.cc`, `scanpopup.hh`, `scanpopup.cc`, signature extension in `preferences.hh/.cc` | Lifecycle, activation, timestamps, notification ladder, popup entry point and compatible snapshot transport; Task 5. |
| `preferences.hh`, `preferences.cc`, `preferences.ui` | Third row, edited-value preservation and exact renderer; Task 6. |
| `tests/tst_hotkeys/tst_hotkeys.cc`, `tests/tst_hotkeys/tst_hotkeys.pro` | Existing link closure plus exactly 20 new slots added with their implementing tasks, Tasks 2–6. |
| `scripts/verify-wayland-selection-live.sh` | One executable with embedded project/helper/runner and all 13 live cases; Task 7. |
| `docs/wayland-limitations.md`, `README.md` | Coverage, fallback, evidence boundaries and final acceptance; Task 8. |

### Requirement-to-task coverage

| Specification requirement | Implementing task / evidence |
| --- | --- |
| §1 shortcut-only, ext-only, opt-in, separate binding/capture, unchanged X11 | Tasks 2–6; Task 8 manual checks 9–16. |
| §2 inventory, normal qmake paths and all count strings | Task 1; Task 8 closure negative controls. |
| §2.1 optional compile/stub and header-only hotkeys dependency | Task 3; Task 8 genuine negative pkg-config build. |
| §2.2 XML hash, generated C, exact regenerate.sh | Task 3; Task 8 byte-identical regeneration. |
| §3.1 complete pure API and every OfferState postcondition | Task 1, all 14 behavioural slots. |
| §3.2 public API, affinity, sticky cache, stop vs failure, snapshot ownership | Tasks 3 and 5; source assertions and Task 7 real signals. |
| §4.1 three IDs, two platform predicates | Task 2; Task 4 display-record round trip. |
| §4.2 clipboard stamp and declaration/destruction order | Task 5 source assertions; Task 7 receipt-time case proves only source stamp. |
| §4.3 reconciliation, deferred start, all four teardown and recreation sites | Task 5 source assertions. |
| §4.4–4.6 binding, handle/token order, guarded popup creation, completion, empty ladder, snapshot and entry point | Task 5 source assertions; Task 8 manual popup/notification acceptance. |
| §4.7 and §7.2–7.3 all UI strings, base algorithm, plain text, edited flags and non-Wayland preservation | Task 6, seven exact UI slots including real Wayland child. |
| §5 all boolean/key defaults, missing/empty/invalid values, verbatim text and duplicate display records | Task 4, five real Config tests; Task 6 getter tests. |
| §6.1 registry sync, first manager/seat, replacement, exact failure reasons | Task 3 source and Task 7 availability evidence. |
| §6.2–6.5 channel identity, debounce, tokens, bounded reads, EOF/probe, MIME retry, shared deadlines | Task 3; Task 7 cases 2–10. |
| §6.6 request overlap, reentrancy, precise activation bound and cache suppression | Task 3; Task 7 cases 11–13. |
| §6.7 all prepare/read/cancel/dispatch/flush branches | Task 3 balanced-body source assertions, explicitly limited evidence. |
| §6.8 S1–S5 shared connection, two-phase retirement, synchronous outside-dispatch disposal | Task 3 source assertions; Task 8 isolated guard mutations. |
| §7.1 recency and phrase/duplicate handling | Tasks 1 and 5; Task 8 manual send-to-main-window and invalid-phrase checks. |
| §7.4 exact payload-free diagnostics | Task 3, Task 5 selection-empty and Task 7 log assertions. |
| §8.1 exact 16-function suite, §8.2 13 hotkeys functions, §8.3 seven hotkeys functions | Tasks 1–6; Task 8 exact declaration/definition inventory check. |
| §8.4 embedded helper JSON, isolation, prerequisites, bounds, cleanup, 13 complete cases | Task 7. |
| §9 acceptance criteria 1–17 | Task 8 names and executes each available gate, records manual checks without inventing outcomes. |
| §10 all deferred/platform/evidence limits, unchanged historical receipts | Task 8. |

## Global Constraints

- Use C++17 and Qt 6.12. Use qmake only (no CMake).
- Run `qmake6 goldendict.pro`, `make tests-makefile`, `make tests-build`, then `QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh make tests-run` from the repository root after each task; `make tests-makefile` regenerates `tests/Makefile.tests`. Generated Makefiles are build outputs and must not be committed.
- The acceptance inventory is closed and sorted: `tst_article_bridge tst_article_history tst_article_load tst_article_schemes tst_article_stream tst_btree_index tst_encoding tst_external_content tst_hotkeys tst_htmlescape tst_langcoder tst_linkrewrite tst_platformwindow tst_portal_hotkeys tst_print_pipeline tst_regex tst_save_article tst_utf8_wstring tst_waylandselection tst_website_frames`. Register the new suite and change all specified count comments/checks/messages to 20 in Task 1.
- Retain every existing test; do not rename a pre-existing suite or weaken its assertions.
- All named new test functions are top-level QtTest slots, declared and defined exactly once. The logic suite has 14 behavioural slots plus init/cleanup and no `_data`; hotkeys gains exactly 20 functions, including two `_data` suppliers and one child probe.
- British/American spelling is irrelevant in commentary; identifiers, English source strings, punctuation, XML names, diagnostics, object names and success markers are byte-exact. Use `tr()` for specified user-visible strings, U+2014 for the em dash and ` · ` (U+00B7) as row separator.
- Its only new optional shared-library dependency is `libwayland-client`. No helper process or service dependency is added to production. Commit generated protocol C and headers; normal builds never invoke wayland-scanner.
- There is no new X11 runtime action, selection watcher, modifier rule or popup creation reason. The existing X11 modifier test is in `scanpopup.cc:571`; retain it unchanged. The Wayland clipboard watch and mouse-hover guards are in `scanpopup.cc:503` and `scanpopup.cc:530`; retain their behaviour unchanged.
- Registering a shortcut and reading the primary selection are separate capabilities. Wayland plus the enabled preference registers the action even in a build without selection capture; that build translates the clipboard.
- The action ID is `scan-popup`, the portal description is `Translate the selected text`, and the preference defaults to off. The default key is constructed as `Config::HotKey( QKeySequence( Qt::CTRL | Qt::META | Qt::Key_D ) )`; its PortableText is `Meta+Ctrl+D` and its preferred portal trigger is `CTRL+LOGO+d`.
- Selection and clipboard use the single `Monotonic::nowMs()` epoch. Stamp the primary event at callback entry; read the clipboard and its current stamp at activation completion. A timed-out interested request answers empty/stamp 0, suppressing the sticky cache for that request.
- All source construction, accessors, calls, timer/notifier callbacks and signal emissions occur on its QObject/GUI thread. No worker thread, blocking roundtrip, blocking read, GUI event pumping or nested event loop is used in production.
- Never log selected text, clipboard text or activation tokens. Keep the source's private display independent of Qt's display.
- Preferences never receives a source pointer, constructs a source, or connects directly to it. MainWindow exclusively owns and pushes `SelectionStatus` snapshots.
- Evidence boundary: pure tests prove decisions; hotkeys links Config/Preferences/backend but not MainWindow, ScanPopup or the source; source assertions prove textual wiring only. The live runner proves its 13 protocol cases and does not prove popup opening, visible notifications or a compositor focus grant. Preserve historical receipts and leave unobserved manual checks explicitly unobserved.
- Each commit must build and pass the existing gate plus the new tests present at that commit. The compatible renderer parameter is added with its first consumer in Task 5; its UI behaviour follows in Task 6. No task may leave a failing test for a later task to repair.

## Task 1: Implement pure selection decisions and register the twentieth suite

**Implementer tier:** Advanced
**Lane:** implementation

**Files:**

- Create: `waylandselectionlogic.hh`
- Create: `waylandselectionlogic.cc`
- Create: `tests/tst_waylandselection/tst_waylandselection.pro`
- Create: `tests/tst_waylandselection/tst_waylandselection.cc`
- Modify: `goldendict.pro:282-421` (unconditional HEADERS)
- Modify: `goldendict.pro:435-572` (unconditional SOURCES)
- Modify: `goldendict.pro:758-761` (aggregate count comment)
- Modify: `tests/tests.pro:9-11`
- Modify: `tests/tests.pro:123-145`
- Test: `tests/tst_waylandselection/tst_waylandselection.cc`

**Interfaces:**

- Consumes existing `bool TestSupport::requiredEnvironment( QStringList * reasons )`, `void TestSupport::installWatchdog( int seconds )`, `void TestSupport::clearWatchdog()` and `constexpr int TestSupport::PureWatchdogSeconds = 60` from `tests/support/fixtures.hh`.
- Produces the complete header below: `static qint64 Monotonic::nowMs() noexcept`; all constants, free functions, `PayloadAccumulator`, `ReadToken`, `ChannelOffer` and `OfferState` signatures are part of the cross-task contract. Neither QObject nor a Wayland/clipboard implementation is consumed.

- [ ] **Step 1: Write the complete failing pure suite and register it atomically with its project**

Create the project exactly:

```qmake
TARGET = tst_waylandselection
include(../testsupport.pri)
QT += core
SOURCES += $$GD_REPO_ROOT/waylandselectionlogic.cc \
           tst_waylandselection.cc
HEADERS += $$GD_REPO_ROOT/waylandselectionlogic.hh
```

Create the complete test source:

```cpp
#include <QtTest>
#include "support/fixtures.hh"
#include "waylandselectionlogic.hh"
using namespace WaylandSelection;

class TstWaylandSelection : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanupTestCase();
  void mimePreferenceOrder();
  void payloadClampIsAppliedWhileReading();
  void decodeReplacesInvalidUtf8();
  void decodeStripsNul();
  void chooseTextUsesRecency();
  void clipboardStampIgnoresNonClipboardModes();
  void mimeAttemptsAdvanceAndExhaust();
  void payloadAtCapIsNotTruncated();
  void payloadOverCapIsTruncated();
  void offerSupersessionRetiresToken();
  void clipboardOfferIsDiscarded();
  void retiredOfferIsNeverReused();
  void nullOfferClearsWithoutRead();
  void activationPlanPromotesPendingCapture();
};
void TstWaylandSelection::initTestCase()
{
  QStringList reasons;
  QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
  TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
}
void TstWaylandSelection::cleanupTestCase() { TestSupport::clearWatchdog(); }
void TstWaylandSelection::mimePreferenceOrder()
{
  QStringList offered{ "text/html", "text/plain", "UTF8_STRING", "text/plain;charset=UTF-8",
                       "image/png", "text/plain;charset=utf-8", "text/plain;charset=utf-8" };
  for( QString const & expected : QStringList{ "text/plain;charset=utf-8", "text/plain;charset=UTF-8",
                                              "UTF8_STRING", "text/plain" } )
  {
    QCOMPARE( chooseMimeType( offered ), expected );
    offered.removeAll( expected );
  }
  QVERIFY( chooseMimeType( {} ).isEmpty() );
  QVERIFY( chooseMimeType( { "image/png", "text/html", "TEXT/PLAIN" } ).isEmpty() );
}
void TstWaylandSelection::payloadClampIsAppliedWhileReading()
{
  PayloadAccumulator p;
  p.append( QByteArray( 400000, 'a' ) );
  QCOMPARE( p.payload().size(), 400000 ); QVERIFY( !p.full() ); QVERIFY( !p.truncated() );
  p.append( QByteArray( 400000, 'b' ) );
  QCOMPARE( p.payload().size(), 800000 ); QVERIFY( !p.full() ); QVERIFY( !p.truncated() );
  p.append( QByteArray( 400000, 'c' ) );
  QByteArray const prefix = QByteArray( 400000, 'a' ) + QByteArray( 400000, 'b' ) + QByteArray( 248576, 'c' );
  QCOMPARE( p.payload(), prefix ); QVERIFY( p.full() ); QVERIFY( p.truncated() );
  p.append( QByteArray( 17, 'd' ) );
  QCOMPARE( p.payload(), prefix ); QVERIFY( p.truncated() );
  PayloadAccumulator one;
  one.append( QByteArray( MaxPayloadBytes + 17, 'x' ) );
  QCOMPARE( one.payload(), QByteArray( MaxPayloadBytes, 'x' ) ); QVERIFY( one.truncated() );
  PayloadAccumulator small( 3 );
  small.append( "ab" ); small.append( "c" );
  QCOMPARE( small.payload(), QByteArray( "abc" ) ); QVERIFY( small.full() ); QVERIFY( !small.truncated() );
  small.append( {} ); QVERIFY( !small.truncated() );
  PayloadAccumulator zero( 0 );
  QVERIFY( zero.full() ); QVERIFY( !zero.truncated() );
  zero.append( {} ); QVERIFY( !zero.truncated() );
  zero.append( "x" ); QVERIFY( zero.truncated() ); QVERIFY( zero.payload().isEmpty() );
}
void TstWaylandSelection::decodeReplacesInvalidUtf8()
{
  QCOMPARE( decodePayload( QByteArray::fromHex( "61ff62" ) ), QString( "a" ) + QChar( 0xfffd ) + "b" );
  QCOMPARE( decodePayload( QByteArray::fromHex( "c3a9" ) ), QString( QChar( 0x00e9 ) ) );
}
void TstWaylandSelection::decodeStripsNul()
{
  QString const text = decodePayload( QByteArray::fromHex( "0061006200" ) );
  QCOMPARE( text, QString( "ab" ) ); QVERIFY( !text.contains( QChar( 0 ) ) );
}
void TstWaylandSelection::chooseTextUsesRecency()
{
  QCOMPARE( chooseText( "selected", 20, "copied", 10 ), QString( "selected" ) );
  QCOMPARE( chooseText( "selected", 10, "copied", 20 ), QString( "copied" ) );
  QCOMPARE( chooseText( "selected", 20, "copied", 20 ), QString( "selected" ) );
  QCOMPARE( chooseText( " \t ", 30, "copied", 20 ), QString( "copied" ) );
  QCOMPARE( chooseText( "selected", 1, "copied", 0 ), QString( "selected" ) );
  QCOMPARE( chooseText( "selected", 10, " \t ", 20 ), QString( "selected" ) );
  QVERIFY( chooseText( " \n", 0, {}, 0 ).isEmpty() );
  QCOMPARE( chooseText( " selected ", 20, "copied", 10 ), QString( " selected " ) );
  QCOMPARE( chooseText( {}, 0, " copied ", 20 ), QString( " copied " ) );
}
void TstWaylandSelection::clipboardStampIgnoresNonClipboardModes()
{
  // The booleans represent Clipboard, Selection and FindBuffer. No clipboard object is linked.
  bool const isClipboard[] = { true, false, false };
  bool const expected[] = { true, false, false };
  for( int i = 0; i != 3; ++i ) QCOMPARE( clipboardStampAdvances( isClipboard[i] ), expected[i] );
}
void TstWaylandSelection::mimeAttemptsAdvanceAndExhaust()
{
  OfferState s;
  s.pendingOffer( 1, { "text/plain", "UTF8_STRING", "text/plain;charset=UTF-8",
                       "text/plain;charset=utf-8", "text/plain", "image/png" } );
  QCOMPARE( s.takeForPrimary().id, quint64( 1 ) );
  for( QString const & mime : QStringList{ "text/plain;charset=utf-8", "text/plain;charset=UTF-8",
                                          "UTF8_STRING", "text/plain" } )
  {
    QVERIFY( s.beginMimeAttempt() ); QCOMPARE( s.currentMimeType(), mime );
    QVERIFY( s.beginMimeAttempt() ); QCOMPARE( s.currentMimeType(), mime );
    s.mimeAttemptFailed();
  }
  QVERIFY( !s.beginMimeAttempt() ); QVERIFY( s.currentMimeType().isEmpty() );
  s.retire(); QVERIFY( !s.beginMimeAttempt() );
}
void TstWaylandSelection::payloadAtCapIsNotTruncated()
{
  PayloadAccumulator p;
  for( int i = 0; i != MaxPayloadBytes / 4096; ++i ) p.append( QByteArray( 4096, 'a' ) );
  QCOMPARE( p.payload(), QByteArray( MaxPayloadBytes, 'a' ) );
  QVERIFY( p.full() ); QVERIFY( !p.truncated() );
  p.append( {} ); QVERIFY( !p.truncated() );
}
void TstWaylandSelection::payloadOverCapIsTruncated()
{
  PayloadAccumulator p;
  p.append( QByteArray( MaxPayloadBytes, 'a' ) ); p.append( "b" );
  QCOMPARE( p.payload(), QByteArray( MaxPayloadBytes, 'a' ) ); QVERIFY( p.full() ); QVERIFY( p.truncated() );
  PayloadAccumulator one;
  one.append( QByteArray( MaxPayloadBytes + 1, 'a' ) );
  QCOMPARE( one.payload(), p.payload() ); QVERIFY( one.full() ); QVERIFY( one.truncated() );
}
void TstWaylandSelection::offerSupersessionRetiresToken()
{
  OfferState s; s.pendingOffer( 1, { "text/plain" } ); s.takeForPrimary();
  QVERIFY( s.beginMimeAttempt() ); auto const a = s.beginRead(); QVERIFY( a > 0 ); QVERIFY( s.accepts( a ) );
  s.pendingOffer( 2, { "text/plain" } ); QCOMPARE( s.takeForPrimary().id, quint64( 2 ) );
  QVERIFY( !s.accepts( a ) ); QVERIFY( s.beginMimeAttempt() ); auto const b = s.beginRead();
  QVERIFY( b > a ); QVERIFY( s.accepts( b ) );
  s.completed( a ); QVERIFY( s.accepts( b ) ); s.completed( b ); QVERIFY( !s.accepts( b ) );
}
void TstWaylandSelection::clipboardOfferIsDiscarded()
{
  OfferState s; s.pendingOffer( 1, { "text/plain" } ); s.takeForPrimary();
  QVERIFY( s.beginMimeAttempt() ); auto const a = s.beginRead();
  s.pendingOffer( 2, { "text/plain" } ); QCOMPARE( s.discardForClipboard().id, quint64( 2 ) );
  QVERIFY( s.accepts( a ) ); QCOMPARE( s.beginRead(), ReadToken( 0 ) );
  OfferState clipboardOnly; clipboardOnly.pendingOffer( 2, { "text/plain" } );
  QCOMPARE( clipboardOnly.discardForClipboard().id, quint64( 2 ) );
  QVERIFY( !clipboardOnly.beginMimeAttempt() ); QCOMPARE( clipboardOnly.beginRead(), ReadToken( 0 ) );
}
void TstWaylandSelection::retiredOfferIsNeverReused()
{
  OfferState s; s.pendingOffer( 1, { "text/plain" } ); s.takeForPrimary();
  QVERIFY( s.beginMimeAttempt() ); auto const token = s.beginRead(); s.retire(); s.retire();
  QVERIFY( !s.accepts( token ) ); QVERIFY( !s.beginMimeAttempt() );
  QCOMPARE( s.beginRead(), ReadToken( 0 ) ); QVERIFY( s.currentMimeType().isEmpty() );
  OfferState exhausted; exhausted.pendingOffer( 1, { "text/plain" } ); exhausted.takeForPrimary();
  QVERIFY( exhausted.beginMimeAttempt() ); exhausted.mimeAttemptFailed();
  QVERIFY( !exhausted.beginMimeAttempt() ); QCOMPARE( exhausted.beginRead(), ReadToken( 0 ) );
}
void TstWaylandSelection::nullOfferClearsWithoutRead()
{
  OfferState s; s.pendingOffer( 1, { "text/plain" } ); s.takeForPrimary();
  QVERIFY( s.beginMimeAttempt() ); auto const token = s.beginRead();
  s.pendingOffer( 0, {} ); ChannelOffer const offer = s.takeForPrimary();
  QCOMPARE( offer.id, quint64( 0 ) ); QVERIFY( !offer ); QVERIFY( !s.accepts( token ) );
  QVERIFY( !s.beginMimeAttempt() ); QCOMPARE( s.beginRead(), ReadToken( 0 ) );
  QVERIFY( !ChannelOffer{} ); QVERIFY( ChannelOffer{7} );
}
void TstWaylandSelection::activationPlanPromotesPendingCapture()
{
  QCOMPARE( planActivation( true ), ActivationPlan::AwaitPending );
  QCOMPARE( planActivation( false ), ActivationPlan::UseCached );
  qint64 ( *const clock )() noexcept = &Monotonic::nowMs;
  qint64 const first = clock(); qint64 const second = clock();
  QVERIFY( first > 0 ); QVERIFY( second >= first );
}
QTEST_GUILESS_MAIN( TstWaylandSelection )
#include "tst_waylandselection.moc"
```

Replace `GD_REQUIRED_SUITE_NAMES` with this exact list, change the count/error/message and header comment, and replace the old 18-suite comment in `goldendict.pro` with the block below in this same task:

```qmake
GD_REQUIRED_SUITE_NAMES = \
    tst_article_bridge tst_article_history tst_article_load \
    tst_article_schemes tst_article_stream tst_btree_index tst_encoding \
    tst_external_content tst_hotkeys tst_htmlescape tst_langcoder \
    tst_linkrewrite tst_platformwindow \
    tst_portal_hotkeys tst_print_pipeline tst_regex tst_save_article \
    tst_utf8_wstring tst_waylandselection tst_website_frames
```

```qmake
# Ordinary mode is a closed acceptance inventory: exactly the 20 required suites.
!count(GD_TEST_NAMES, 20): error("Expected exactly 20 required suites; found: $$GD_TEST_NAMES")
message("tests/tests.pro: exact 20 suites: $$GD_TEST_NAMES")
```

The three lines above replace their respective existing lines, retaining the inventory equality and project-existence checks between them. Replace only the application comment:

```qmake
# The suites are independent projects with private object directories, so the
# build half runs in parallel for all 20 suites. The run half stays serial for
# deterministic test execution within the gate's wall-clock budget.
```

- [ ] **Step 2: Record the expected RED build before creating the module**

```bash
qmake6 goldendict.pro
make tests-makefile
make tests-build
```

Expected: inventory prints `exact 20 suites`; the new project cannot compile because `waylandselectionlogic.hh/.cc` do not exist yet. Record that specific failure, not a missing-tool failure.

- [ ] **Step 3: Create the complete public decision API and implementation**

```cpp
#ifndef WAYLANDSELECTIONLOGIC_HH_INCLUDED
#define WAYLANDSELECTIONLOGIC_HH_INCLUDED
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>
class Monotonic final
{
public:
  static qint64 nowMs() noexcept;
};
namespace WaylandSelection {
inline constexpr int MaxPayloadBytes = 1024 * 1024;
inline constexpr int DebounceMs = 200;
inline constexpr int IdleTimeoutMs = 250;
inline constexpr int TransferTimeoutMs = 2000;
inline constexpr int ActivationTimeoutMs = 300;
QString chooseMimeType( QStringList const & offered );
class PayloadAccumulator
{
public:
  explicit PayloadAccumulator( int cap = MaxPayloadBytes );
  void append( QByteArray const & chunk );
  bool full() const noexcept;
  bool truncated() const noexcept;
  QByteArray payload() const;
private:
  int cap_;
  QByteArray data_;
  bool truncated_ = false;
};
QString decodePayload( QByteArray const & raw );
bool clipboardStampAdvances( bool modeIsClipboard ) noexcept;
QString chooseText( QString const & selection, qint64 capturedAt,
                    QString const & clipboard, qint64 clipboardChangedAt );
enum class ActivationPlan { UseCached, AwaitPending };
ActivationPlan planActivation( bool hasPendingCapture ) noexcept;
using ReadToken = quint64;
struct ChannelOffer
{
  quint64 id = 0;
  operator bool() const noexcept { return id != 0; }
};
class OfferState
{
public:
  void pendingOffer( quint64 offerId, QStringList const & mimeTypes );
  ChannelOffer takeForPrimary();
  ChannelOffer discardForClipboard();
  bool beginMimeAttempt();
  QString currentMimeType() const;
  void mimeAttemptFailed() noexcept;
  ReadToken beginRead() noexcept;
  bool accepts( ReadToken token ) const noexcept;
  void retire() noexcept;
  void completed( ReadToken token ) noexcept;
private:
  quint64 pendingId_ = 0;
  QStringList pendingMimeTypes_;
  quint64 primaryId_ = 0;
  QStringList mimeTypes_;
  int nextMime_ = 0;
  int currentMime_ = -1;
  ReadToken nextToken_ = 0;
  ReadToken activeToken_ = 0;
};
} // namespace WaylandSelection
#endif // WAYLANDSELECTIONLOGIC_HH_INCLUDED
```

```cpp
#include "waylandselectionlogic.hh"
#include <algorithm>
#include <chrono>
#include <limits>
qint64 Monotonic::nowMs() noexcept
{
  using Clock = std::chrono::steady_clock;
  static auto const epoch = Clock::now();
  return 1 + std::chrono::duration_cast<std::chrono::milliseconds>( Clock::now() - epoch ).count();
}
namespace WaylandSelection {
namespace {
QStringList acceptedMimes( QStringList const & offered )
{
  QStringList result;
  for( auto const * literal : { "text/plain;charset=utf-8", "text/plain;charset=UTF-8", "UTF8_STRING", "text/plain" } )
    if( offered.contains( QLatin1String( literal ) ) ) result.append( QLatin1String( literal ) );
  return result;
}
}
QString chooseMimeType( QStringList const & offered ) { return acceptedMimes( offered ).value( 0 ); }
PayloadAccumulator::PayloadAccumulator( int cap ) : cap_( cap ) { Q_ASSERT( cap >= 0 ); }
void PayloadAccumulator::append( QByteArray const & chunk )
{
  qsizetype const remaining = cap_ - data_.size();
  qsizetype const kept = std::min( remaining, chunk.size() );
  if( chunk.size() > remaining ) truncated_ = true;
  if( kept ) data_.append( chunk.constData(), kept );
}
bool PayloadAccumulator::full() const noexcept { return data_.size() == cap_; }
bool PayloadAccumulator::truncated() const noexcept { return truncated_; }
QByteArray PayloadAccumulator::payload() const { return data_; }
QString decodePayload( QByteArray const & raw )
{
  QString text = QString::fromUtf8( raw.constData(), raw.size() );
  text.remove( QChar( 0 ) );
  return text;
}
bool clipboardStampAdvances( bool modeIsClipboard ) noexcept { return modeIsClipboard; }
QString chooseText( QString const & selection, qint64 capturedAt,
                    QString const & clipboard, qint64 clipboardChangedAt )
{
  Q_ASSERT( capturedAt >= 0 && clipboardChangedAt >= 0 );
  bool const hasSelection = !selection.trimmed().isEmpty();
  bool const hasClipboard = !clipboard.trimmed().isEmpty();
  if( hasSelection && capturedAt >= clipboardChangedAt ) return selection;
  if( hasClipboard ) return clipboard;
  if( hasSelection ) return selection;
  return {};
}
ActivationPlan planActivation( bool pending ) noexcept
{
  return pending ? ActivationPlan::AwaitPending : ActivationPlan::UseCached;
}
void OfferState::pendingOffer( quint64 id, QStringList const & types )
{
  pendingId_ = id;
  pendingMimeTypes_ = id ? types : QStringList{};
}
ChannelOffer OfferState::takeForPrimary()
{
  quint64 const id = pendingId_;
  QStringList const types = pendingMimeTypes_;
  retire();
  pendingId_ = 0;
  pendingMimeTypes_.clear();
  primaryId_ = id;
  mimeTypes_ = id ? acceptedMimes( types ) : QStringList{};
  return { id };
}
ChannelOffer OfferState::discardForClipboard()
{
  ChannelOffer const result{ pendingId_ };
  pendingId_ = 0;
  pendingMimeTypes_.clear();
  return result;
}
bool OfferState::beginMimeAttempt()
{
  if( !primaryId_ ) return false;
  if( currentMime_ >= 0 ) return true;
  if( nextMime_ >= mimeTypes_.size() ) return false;
  currentMime_ = nextMime_++;
  return true;
}
QString OfferState::currentMimeType() const { return currentMime_ < 0 ? QString{} : mimeTypes_.value( currentMime_ ); }
void OfferState::mimeAttemptFailed() noexcept
{
  if( currentMime_ < 0 ) return;
  activeToken_ = 0;
  currentMime_ = -1;
}
ReadToken OfferState::beginRead() noexcept
{
  if( !primaryId_ || currentMime_ < 0 || activeToken_ ) return 0;
  Q_ASSERT( nextToken_ != std::numeric_limits<ReadToken>::max() );
  activeToken_ = ++nextToken_;
  return activeToken_;
}
bool OfferState::accepts( ReadToken token ) const noexcept { return primaryId_ && token && token == activeToken_; }
void OfferState::retire() noexcept
{
  primaryId_ = 0; nextMime_ = 0; currentMime_ = -1; activeToken_ = 0;
}
void OfferState::completed( ReadToken token ) noexcept { if( accepts( token ) ) retire(); }
} // namespace WaylandSelection
```

Add `waylandselectionlogic.hh` immediately before `platformwindow.hh` in the existing unconditional HEADERS list, and `waylandselectionlogic.cc` immediately before `platformwindow.cc` in SOURCES. The companion source module is not referenced until Task 3 creates it.

- [ ] **Step 4: Verify the GUI-less deliverable and full gate**

```bash
qmake6 goldendict.pro
make -j$(nproc)
make tests-makefile
make tests-build
QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_waylandselection/tst_waylandselection
QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh make tests-run
```

Expected: app links, exact 20 inventory, 16 pure init/test/cleanup functions pass; no compositor connection is attempted by the pure production subject. Do not call the shared support's GUI link libraries runtime GUI coverage.

- [ ] **Step 5: Commit the independently usable pure module and suite**

```bash
git add waylandselectionlogic.hh waylandselectionlogic.cc tests/tst_waylandselection/tst_waylandselection.pro tests/tst_waylandselection/tst_waylandselection.cc tests/tests.pro goldendict.pro
git commit -m "feat(selection): add pure Wayland selection decisions and tests"
```

## Task 2: Publish shared portal IDs and independent platform predicates

**Implementer tier:** Advanced
**Lane:** implementation

**Files:**

- Modify: `hotkeybackend.hh:26-32`
- Modify: `config.cc:6-7`
- Modify: `config.cc:176-183`
- Modify: `portalhotkeys.cc:82-85`
- Modify: `portalhotkeys.cc:852-852`
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:31-78` (includes and anonymous-namespace helpers)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:397-472` (three new slot declarations)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:2844-2845` (insert definitions before QTEST_MAIN)
- Test: `tests/tst_hotkeys/tst_hotkeys.cc`

**Interfaces:**

- Consumes `struct HotkeyBinding { QString id; QString description; QString sequence; }`; `void HotkeyBackend::bind( QVector<HotkeyBinding> const & )`; `HotkeyBackend::State HotkeyBackend::state() const noexcept`; signals `void stateChanged( State, QString const & )` and `void bindingsChanged( QVector<PortalBindingDisplay> const & )`.
- Consumes `bool TestSupport::PrivateSessionBus::start( QString * )`, `QDBusConnection connection() const`, `QDBusConnection fixtureConnection() const`; `bool FakePortal::start( QDBusConnection const &, uint )`, `void setListShortcuts( QVector<Shortcut> const & )`, `void setBoundIds( QStringList const & )`, `QVector<Call> calls() const`, `QStringList protocolViolations() const`, `QString errorString() const`; `Call::member`, `Call::shortcutIds`, `Call::shortcuts` and `Shortcut::{id,properties}`.
- Produces `inline bool isKnownPortalBindingId( QString const & id )`, `inline bool scanPopupBindingPlanned( QString const & platformName, bool preferenceEnabled ) noexcept`, and `inline bool scanPopupCaptureSupported( bool buildSupportsSelection, QString const & platformName ) noexcept` with the definitions below.
- Produces test-local helpers `QString selectionSourceText( char const * path )`, `QString selectionBlock( QString const & text, QString const & anchor )`, `bool selectionOrdered( QString const & text, QStringList const & needles )`, and `QString selectionCompact( QString text )`. Later source assertions consume these exact signatures; they never instantiate the inspected production subject.

- [ ] **Step 1: Add the three complete failing test slots and balanced-body assertion helpers**

Add `#include <QRegularExpression>` with Qt includes, declare these slots once in the existing TstHotkeys class, and insert the following helper definitions in the existing anonymous namespace before its closing brace:

```cpp
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
QString selectionBlock( QString const & text, QString const & anchor )
{
  qsizetype const at = text.indexOf( anchor );
  if( at < 0 ) return {};
  qsizetype const first = text.indexOf( '{', at + anchor.size() );
  if( first < 0 ) return {};
  int depth = 0;
  QChar quote;
  bool line = false, block = false, escape = false;
  for( qsizetype i = first; i < text.size(); ++i )
  {
    QChar const c = text[i];
    QChar const next = i + 1 < text.size() ? text[i+1] : QChar{};
    if( line ) { if( c == '\n' ) line = false; continue; }
    if( block ) { if( c == '*' && next == '/' ) { block = false; ++i; } continue; }
    if( !quote.isNull() )
    {
      if( escape ) escape = false;
      else if( c == '\\' ) escape = true;
      else if( c == quote ) quote = QChar{};
      continue;
    }
    if( c == '/' && next == '/' ) { line = true; ++i; continue; }
    if( c == '/' && next == '*' ) { block = true; ++i; continue; }
    if( c == '"' || c == '\'' ) { quote = c; continue; }
    if( c == '{' ) ++depth;
    else if( c == '}' && --depth == 0 ) return text.mid( first + 1, i - first - 1 );
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
```

The declarations and full tests are:

```cpp
// Insert in private slots:
void scanPopupBindingIsPortalOnly();
void portalIdPredicateAcceptsThreeIds();
void threeBindingsStayBound();
```

```cpp
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
```

- [ ] **Step 2: Run the new tests and record the missing shared-contract failure**

```bash
make tests-makefile
make tests-build
```

Expected RED: the two platform predicates and public known-ID predicate are undeclared. The portal regression also fails against the former two-ID implementation once compilation is enabled.

- [ ] **Step 3: Insert the exact shared definitions and remove both local copies**

Immediately after `#include "config.hh"` in `hotkeybackend.hh` insert:

```cpp
inline bool isKnownPortalBindingId( QString const & id )
{
  return id == QLatin1String( "main-window" )
      || id == QLatin1String( "clipboard-lookup" )
      || id == QLatin1String( "scan-popup" );
}
inline bool scanPopupBindingPlanned( QString const & platformName,
                                     bool preferenceEnabled ) noexcept
{
  return preferenceEnabled && platformName.startsWith( QLatin1String( "wayland" ) );
}
inline bool scanPopupCaptureSupported( bool buildSupportsSelection,
                                       QString const & platformName ) noexcept
{
  return buildSupportsSelection && platformName.startsWith( QLatin1String( "wayland" ) );
}
```

Replace the binding comment's ID line with:

```cpp
/// ("main-window", "clipboard-lookup", "scan-popup"), `description` is localized display text
```

Add the include after `#include "hotkeysyntax.hh"` in `config.cc`:

```cpp
#include "hotkeybackend.hh"
```

Delete the complete local `isKnownPortalBindingId` definition and its immediately preceding local-predicate comment in `config.cc`; retain the existing two call sites unchanged. Delete the complete `knownBindingId` definition in `portalhotkeys.cc` and replace its only call in `PortalBackend::beginGeneration` with:

```cpp
if ( binding.id.isEmpty() || !isKnownPortalBindingId( binding.id ) || ids.contains( binding.id ) )
```

- [ ] **Step 4: Verify the exact ID set and no capture dependency in registration**

```bash
qmake6 goldendict.pro
make -j$(nproc)
make tests-makefile
make tests-build
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys scanPopupBindingIsPortalOnly portalIdPredicateAcceptsThreeIds threeBindingsStayBound
QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh make tests-run
```

Expected: full and subset portal responses are Bound/Partial respectively, never Unavailable; existing Config and recorder cases remain green. This task publishes support for the ID but creates no new runtime binding yet.

- [ ] **Step 5: Commit the shared contracts**

```bash
git add hotkeybackend.hh config.cc portalhotkeys.cc tests/tst_hotkeys/tst_hotkeys.cc
git commit -m "feat(hotkeys): publish selected-text portal binding contracts"
```

## Task 3: Implement the private ext-data-control source and optional build

**Implementer tier:** Capable
**Lane:** implementation

**Files:**

- Create: `waylandselection.hh`
- Create: `waylandselection.cc`
- Create: `protocols/ext-data-control-v1/ext-data-control-v1.xml`
- Create: `protocols/ext-data-control-v1/ext-data-control-v1-client-protocol.h`
- Create: `protocols/ext-data-control-v1/ext-data-control-v1-protocol.c`
- Create: `protocols/ext-data-control-v1/regenerate.sh`
- Modify: `goldendict.pro:152-153` (optional feature scope)
- Modify: `goldendict.pro:282-421` (unconditional header)
- Modify: `goldendict.pro:435-572` (unconditional C++ source)
- Modify: `tests/testsupport.pri:84-86`
- Modify: `tests/tst_hotkeys/tst_hotkeys.pro:35-49` (header-only dependency comment)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:397-472` (two new declarations)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:2844-2845` (definitions before QTEST_MAIN)
- Test: `tests/tst_hotkeys/tst_hotkeys.cc`

**Interfaces:**

- Consumes `bool scanPopupCaptureSupported( bool buildSupportsSelection, QString const & platformName ) noexcept` from Task 2; `qint64 Monotonic::nowMs() noexcept`; constants `MaxPayloadBytes=1048576`, `DebounceMs=200`, `IdleTimeoutMs=250`, `TransferTimeoutMs=2000`, `ActivationTimeoutMs=300`; `QString chooseMimeType( QStringList const & )`; `QString decodePayload( QByteArray const & )`; `ActivationPlan planActivation( bool ) noexcept` with `UseCached/AwaitPending`; `PayloadAccumulator( int cap=MaxPayloadBytes )`, `void append( QByteArray const & )`, `bool full() const noexcept`, `bool truncated() const noexcept`, `QByteArray payload() const`; `ReadToken=quint64`; `OfferState::pendingOffer( quint64, QStringList const & )`, `ChannelOffer takeForPrimary()`, `ChannelOffer discardForClipboard()`, `bool beginMimeAttempt()`, `QString currentMimeType() const`, `void mimeAttemptFailed() noexcept`, `ReadToken beginRead() noexcept`, `bool accepts( ReadToken ) const noexcept`, `void retire() noexcept`, `void completed( ReadToken ) noexcept`.
- Consumes test helpers `QString selectionSourceText( char const * )`, `QString selectionBlock( QString const &, QString const & )`, `QString selectionCompact( QString )`, `bool selectionOrdered( QString const &, QStringList const & )` from Task 2.
- Produces the complete public header below, including exactly `State { Unsupported, Starting, Available, Unavailable }`, `kHaveWaylandDataControl`, `SelectionStatus`, the const/noexcept accessors, slots and three signals. No public header includes Wayland/POSIX headers.
- Produces the exact private static callback signatures and named Impl methods shown in the complete `.cc` below. `flushRequests()` means connection remains live, including EAGAIN; `startMimeAttempt()` means a new live pipe was installed. Only the helper in Task 7 may call create_data_source/set_selection/set_primary_selection.

- [ ] **Step 1: Add complete source-evidence and feature-definition tests**

Declare these two slots once and insert their complete definitions before QTEST_MAIN. Add `#include "waylandselection.hh"` alongside the production includes. The tests deliberately inspect balanced function/branch bodies rather than linking the source.

```cpp
void capabilityConstantMatchesBuild();
void scanPopupSourceLoopContractIsPinned();
```

```cpp
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
```

- [ ] **Step 2: Record RED, then vendor the exact pinned protocol in this task**

Run `make tests-makefile` and `make tests-build`; expected RED is missing `waylandselection.hh`. Obtain the unmodified staging XML from installed wayland-protocols 1.50 (verified baseline path below), verify the hash before copying, then create the exact script and generated pair:

```bash
pkg-config --modversion wayland-protocols
wayland-scanner --version
sha256sum /usr/share/wayland-protocols/staging/ext-data-control/ext-data-control-v1.xml
mkdir -p protocols/ext-data-control-v1
cp /usr/share/wayland-protocols/staging/ext-data-control/ext-data-control-v1.xml protocols/ext-data-control-v1/ext-data-control-v1.xml
```

Required outputs: `1.50`, `wayland-scanner 1.26.0` and XML SHA-256 `293deba5dfdb974fbcf8dcf891a30a51a36a8bba0047dc6bdf6a03194089b7fb`. If the installed XML differs, obtain that release's file and require this same hash; do not rewrite it. `regenerate.sh` is exactly:

```sh
#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
wayland-scanner client-header ext-data-control-v1.xml ext-data-control-v1-client-protocol.h
wayland-scanner private-code ext-data-control-v1.xml ext-data-control-v1-protocol.c
```

```bash
chmod +x protocols/ext-data-control-v1/regenerate.sh
protocols/ext-data-control-v1/regenerate.sh
```

Retain the XML licence and generated banners. Compile generated `.c` as C; never include it into C++.

- [ ] **Step 3: Create the complete public source header**

```cpp
#ifndef WAYLANDSELECTION_HH_INCLUDED
#define WAYLANDSELECTION_HH_INCLUDED
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <memory>
#ifdef HAVE_WAYLAND_DATA_CONTROL
inline constexpr bool kHaveWaylandDataControl = true;
#else
inline constexpr bool kHaveWaylandDataControl = false;
#endif
class WaylandSelectionSource : public QObject
{
  Q_OBJECT
public:
  enum class State { Unsupported, Starting, Available, Unavailable };
  explicit WaylandSelectionSource( QObject * parent = nullptr );
  ~WaylandSelectionSource() override;
  State state() const noexcept;
  QString reason() const;
  bool primarySelectionSeen() const noexcept;
  QString lastText() const;
  bool hasPendingCapture() const noexcept;
public slots:
  void start();
  void stop();
  void requestCapture( quint64 requestId );
signals:
  void stateChanged( State state, QString const & reason );
  void capabilityChanged();
  void captureFinished( quint64 requestId, QString const & text, qint64 capturedAt );
private:
  class Impl;
  std::unique_ptr< Impl > d;
};
struct SelectionStatus
{
  WaylandSelectionSource::State state = WaylandSelectionSource::State::Unsupported;
  QString reason;
  bool primarySelectionSeen = false;
};
Q_DECLARE_METATYPE( WaylandSelectionSource::State )
#endif // WAYLANDSELECTION_HH_INCLUDED
```

- [ ] **Step 4: Create the complete source with guarded POSIX/protocol implementation**

Use this implementation. The `Connection` is the shared lifetime for physical teardown; its deferred functor never refers to Impl or the source. Listeners receive `Impl*`; phase 1 removes every callback-bearing proxy before Impl can be freed. No owner callback may reset the source during libwayland dispatch; MainWindow's handlers only copy snapshots or process capture results, and its source disposal is outside dispatch.

```cpp
#include "waylandselection.hh"
#include "waylandselectionlogic.hh"
#include "hotkeybackend.hh"
#include "gddebug.hh"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <utility>
#ifdef HAVE_WAYLAND_DATA_CONTROL
#include <QHash>
#include <QSocketNotifier>
#include <QVector>
#include <wayland-client.h>
#include "ext-data-control-v1-client-protocol.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif
using namespace WaylandSelection;
class WaylandSelectionSource::Impl
{
public:
  WaylandSelectionSource * q;
  State current;
  QString explanation, cache;
  qint64 cacheStamp = 0;
  bool primarySeen = false, started = false, ownerStopped = false;
  quint64 lastRequestId = 0, interestedId = 0;
  qint64 activationCalledAt = 0, activationDeadline = 0;
  QTimer debounce, transferWake, activation;
  explicit Impl( WaylandSelectionSource * owner ) : q( owner ),
    current( scanPopupCaptureSupported( kHaveWaylandDataControl, QGuiApplication::platformName() )
               ? State::Starting : State::Unsupported )
  {
    for( QTimer * timer : { &debounce, &transferWake, &activation } )
    { timer->setSingleShot( true ); timer->setTimerType( Qt::PreciseTimer ); }
    QObject::connect( &activation, &QTimer::timeout, q, [this] { expireInterest(); } );
#ifdef HAVE_WAYLAND_DATA_CONTROL
    QObject::connect( &debounce, &QTimer::timeout, q, [this] { if( live() ) promote(); } );
#endif
  }
  void assertThread() const { Q_ASSERT( QThread::currentThread() == q->thread() ); }
  static char const * stateName( State state )
  {
    switch( state )
    {
      case State::Unsupported: return "Unsupported";
      case State::Starting: return "Starting";
      case State::Available: return "Available";
      case State::Unavailable: return "Unavailable";
    }
    Q_UNREACHABLE();
  }
  void publishState()
  {
    gdDebug( "selection-source: state=%s reason=%s", stateName( current ), explanation.toUtf8().constData() );
    emit q->stateChanged( current, explanation );
  }
  bool pending() const noexcept
  {
#ifdef HAVE_WAYLAND_DATA_CONTROL
    return !ownerStopped && primaryId != 0;
#else
    return false;
#endif
  }
  void armActivation()
  {
    if( interestedId ) activation.start( int( std::max<qint64>( 1, activationDeadline - Monotonic::nowMs() ) ) );
  }
  void answerInterest( QString text, qint64 stamp )
  {
    quint64 const id = std::exchange( interestedId, 0 );
    activation.stop();
    if( id && !ownerStopped ) emit q->captureFinished( id, text, text.isEmpty() ? 0 : stamp );
  }
  void expireInterest()
  {
    assertThread();
    if( ownerStopped || !interestedId ) return;
    if( Monotonic::nowMs() < activationDeadline ) { armActivation(); return; }
    gdDebug( "selection-timeout" );
    answerInterest( {}, 0 );
  }
  void requestCapture( quint64 id )
  {
    assertThread();
    if( ownerStopped ) return;
    Q_ASSERT( id && id > lastRequestId );
    lastRequestId = id;
    quint64 const superseded = interestedId;
    bool const await = planActivation( pending() ) == ActivationPlan::AwaitPending;
    interestedId = id;
    activationCalledAt = Monotonic::nowMs();
    activationDeadline = activationCalledAt + ActivationTimeoutMs;
    armActivation();
    if( superseded )
    {
      emit q->captureFinished( superseded, QString(), 0 );
    }
    if( ownerStopped || interestedId != id ) return;
    if( !await ) { answerInterest( cache, cache.isEmpty() ? 0 : cacheStamp ); return; }
#ifdef HAVE_WAYLAND_DATA_CONTROL
    if( primaryId && !readToken ) promote();
#endif
  }
  void start()
  {
    assertThread();
    if( started || ownerStopped ) return;
    started = true;
    publishState();
    if( ownerStopped || current == State::Unsupported ) return;
#ifdef HAVE_WAYLAND_DATA_CONTROL
    connection = std::make_shared<Connection>();
    connection->listenerContext = this;
    connection->display = wl_display_connect( nullptr );
    if( !connection->display ) { fail( "cannot connect to Wayland display" ); return; }
    connection->fd = wl_display_get_fd( connection->display );
    if( connection->fd < 0 ) { fail( "Wayland display error" ); return; }
    displayRead = std::make_unique<QSocketNotifier>( connection->fd, QSocketNotifier::Read );
    displayWrite = std::make_unique<QSocketNotifier>( connection->fd, QSocketNotifier::Write );
    displayWrite->setEnabled( false );
    QObject::connect( displayRead.get(), &QSocketNotifier::activated, q,
                      [this] { dispatchDisplayReadable(); } );
    QObject::connect( displayWrite.get(), &QSocketNotifier::activated, q,
                      [this] { dispatchDisplayWritable(); } );
    registry = wl_display_get_registry( connection->display );
    if( !registry ) { fail( "cannot get Wayland registry" ); return; }
    static wl_registry_listener const registryListener{ registryGlobal, registryGlobalRemoved };
    wl_registry_add_listener( registry, &registryListener, connection->listenerContext );
    sync = wl_display_sync( connection->display );
    if( !sync ) { fail( "cannot create Wayland sync callback" ); return; }
    static wl_callback_listener const syncListener{ discoveryDone };
    wl_callback_add_listener( sync, &syncListener, connection->listenerContext );
    flushRequests();
#endif
  }
  void stop()
  {
    assertThread();
    ownerStopped = true;
    interestedId = 0;
    debounce.stop(); transferWake.stop(); activation.stop();
#ifdef HAVE_WAYLAND_DATA_CONTROL
    dispose();
#endif
  }
#ifdef HAVE_WAYLAND_DATA_CONTROL
  struct Connection
  {
    wl_display * display = nullptr;
    Impl * listenerContext = nullptr;
    int fd = -1, dispatchDepth = 0;
    quint64 generation = 1;
    bool prepared = false, retired = false, destroyed_ = false;
    void destroyConnection()
    {
      if( destroyed_ ) return;
      Q_ASSERT( dispatchDepth == 0 );
      destroyed_ = true;
      if( !display ) return;
      if( prepared ) { wl_display_cancel_read( display ); prepared = false; }
      if( wl_display_get_error( display ) == 0 ) (void)wl_display_flush( display );
      wl_display_disconnect( display );
      display = nullptr; listenerContext = nullptr; fd = -1;
    }
    ~Connection() { destroyConnection(); }
  };
  struct Seat { uint32_t name, version; };
  struct Offer { quint64 id; QStringList mimes; };
  std::shared_ptr<Connection> connection;
  quint64 failedRequestId = 0;
  bool terminalNotificationPending = false;
  std::unique_ptr<QSocketNotifier> displayRead, displayWrite, pipeRead;
  wl_registry * registry = nullptr;
  wl_callback * sync = nullptr;
  ext_data_control_manager_v1 * manager = nullptr;
  wl_seat * seat = nullptr;
  ext_data_control_device_v1 * device = nullptr;
  uint32_t managerName = 0, seatNameId = 0;
  QVector<Seat> seats;
  bool discoveryComplete = false, multipleSeatsLogged = false;
  QHash<ext_data_control_offer_v1 *, Offer> offers;
  OfferState offersState;
  quint64 nextOfferId = 0, primaryId = 0;
  ReadToken readToken = 0;
  int readEnd = -1, bytesObserved = 0;
  qint64 offerStamp = 0, transferStartedAt = 0, transferDeadline = 0, lastProgressAt = 0;
  PayloadAccumulator payload;
  bool live() const { return !ownerStopped && connection && !connection->retired && connection->display; }
  bool healthy() const { return connection && connection->display && wl_display_get_error( connection->display ) == 0; }
  ext_data_control_offer_v1 * proxyFor( quint64 id ) const
  {
    for( auto it = offers.cbegin(); it != offers.cend(); ++it ) if( it.value().id == id ) return it.key();
    return nullptr;
  }
  void destroyOffer( quint64 id )
  {
    auto * proxy = proxyFor( id );
    if( !proxy ) return;
    offers.remove( proxy );
    if( healthy() ) ext_data_control_offer_v1_destroy( proxy );
    else wl_proxy_destroy( reinterpret_cast<wl_proxy *>( proxy ) );
  }
  void closeRead()
  {
    transferWake.stop();
    QObject::disconnect( &transferWake, nullptr, q, nullptr );
    if( pipeRead ) { pipeRead->setEnabled( false ); pipeRead.reset(); }
    if( readEnd >= 0 ) { close( readEnd ); readEnd = -1; }
    readToken = 0;
  }
  void retirePrimary()
  {
    offersState.retire();
    debounce.stop();
    closeRead();
    quint64 const id = std::exchange( primaryId, 0 );
    destroyOffer( id );
    transferStartedAt = 0; transferDeadline = 0; bytesObserved = 0;
  }
  void destroyDeviceAndSeat()
  {
    if( device )
    {
      if( healthy() ) ext_data_control_device_v1_destroy( device );
      else wl_proxy_destroy( reinterpret_cast<wl_proxy *>( device ) );
      device = nullptr;
    }
    if( seat ) { wl_seat_destroy( seat ); seat = nullptr; }
    seatNameId = 0;
  }
  void destroyProtocol()
  {
    for( auto const & offer : offers.values() ) destroyOffer( offer.id );
    primaryId = 0;
    destroyDeviceAndSeat();
    if( manager )
    {
      if( healthy() ) ext_data_control_manager_v1_destroy( manager );
      else wl_proxy_destroy( reinterpret_cast<wl_proxy *>( manager ) );
      manager = nullptr;
    }
    if( sync ) { wl_callback_destroy( sync ); sync = nullptr; }
    if( registry ) { wl_registry_destroy( registry ); registry = nullptr; }
    managerName = 0;
  }
  void dispose()
  {
    assertThread();
    auto const c = connection;
    if( !c ) return;
    if( !c->retired )
    {
      c->retired = true;
      ++c->generation;
      offersState.retire();
      interestedId = 0;
      debounce.stop(); transferWake.stop(); activation.stop();
      closeRead();
      if( displayRead ) displayRead->setEnabled( false );
      if( displayWrite ) displayWrite->setEnabled( false );
      displayRead.reset(); displayWrite.reset();
      destroyProtocol();
      c->listenerContext = nullptr;
      if( terminalNotificationPending )
      {
        terminalNotificationPending = false;
        if( !ownerStopped ) publishState();
        quint64 const id = std::exchange( failedRequestId, 0 );
        if( id && !ownerStopped ) emit q->captureFinished( id, QString(), 0 );
      }
    }
    if( c->dispatchDepth == 0 ) c->destroyConnection();
    else QTimer::singleShot( 0, QCoreApplication::instance(), [c] { c->destroyConnection(); } );
  }
  void fail( char const * reason )
  {
    if( ownerStopped || current == State::Unavailable ) return;
    current = State::Unavailable; explanation = QLatin1String( reason );
    failedRequestId = std::exchange( interestedId, 0 );
    terminalNotificationPending = true;
    dispose();
  }
  bool flushRequests()
  {
    assertThread();
    if( !live() ) return false;
    for( ;; )
    {
      if( wl_display_get_error( connection->display ) ) { fail( "Wayland display error" ); return false; }
      int const result = wl_display_flush( connection->display );
      if( wl_display_get_error( connection->display ) ) { fail( "Wayland display error" ); return false; }
      if( result >= 0 ) { displayWrite->setEnabled( false ); return true; }
      if( errno == EINTR ) continue;
      if( errno == EAGAIN ) { displayWrite->setEnabled( true ); return true; }
      fail( "Wayland display error" ); return false;
    }
  }
  bool dispatchPending( std::shared_ptr<Connection> const & c, quint64 generation )
  {
    ++c->dispatchDepth;
    int const result = wl_display_dispatch_pending( c->display );
    --c->dispatchDepth;
    if( c->retired || c->generation != generation ) { c->destroyConnection(); return false; }
    if( wl_display_get_error( c->display ) ) { fail( "Wayland display error" ); return false; }
    if( result < 0 ) return flushRequests();
    return true;
  }
  void dispatchDisplayReadable()
  {
    assertThread();
    if( !live() ) return;
    auto const c = connection;
    quint64 const generation = c->generation;
    displayRead->setEnabled( false );
    if( wl_display_prepare_read( c->display ) != 0 )
    {
      if( wl_display_get_error( c->display ) ) { fail( "Wayland display error" ); return; }
      // EAGAIN and the no-display-error errno branch both dispatch once, never read.
      if( !dispatchPending( c, generation ) ) return;
      if( flushRequests() && live() ) displayRead->setEnabled( true );
      return;
    }
    c->prepared = true;
    pollfd ready{ c->fd, POLLIN, 0 };
    if( ::poll( &ready, 1, 0 ) <= 0 || !( ready.revents & ( POLLIN | POLLHUP | POLLERR ) ) )
    {
      wl_display_cancel_read( c->display ); c->prepared = false;
      if( flushRequests() && live() ) displayRead->setEnabled( true );
      return;
    }
    ++c->dispatchDepth;
    int const result = wl_display_read_events( c->display );
    --c->dispatchDepth;
    c->prepared = false;
    if( c->retired || c->generation != generation ) { c->destroyConnection(); return; }
    if( result < 0 || wl_display_get_error( c->display ) ) { fail( "Wayland display error" ); return; }
    if( !dispatchPending( c, generation ) ) return;
    if( flushRequests() && live() ) displayRead->setEnabled( true );
  }
  void dispatchDisplayWritable()
  {
    assertThread();
    if( ownerStopped || !connection || connection->retired ) return;
    flushRequests();
  }
  bool bindSeat( Seat const & candidate )
  {
    seat = static_cast<wl_seat *>( wl_registry_bind( registry, candidate.name, &wl_seat_interface,
                                                    std::min( candidate.version, 2u ) ) );
    if( !seat ) { fail( "cannot bind wl_seat" ); return false; }
    seatNameId = candidate.name;
    static wl_seat_listener const listener{ seatCapabilities, seatName };
    wl_seat_add_listener( seat, &listener, this );
    return flushRequests();
  }
  void maybeAvailable()
  {
    if( !live() ) return;
    if( manager && seat && !device )
    {
      device = ext_data_control_manager_v1_get_data_device( manager, seat );
      if( !device ) { fail( "cannot create ext_data_control_device_v1" ); return; }
      static ext_data_control_device_v1_listener const listener{ dataOffer, selection, deviceFinished, primarySelection };
      ext_data_control_device_v1_add_listener( device, &listener, this );
      if( !flushRequests() ) return;
    }
    if( discoveryComplete && device && current == State::Starting )
    { current = State::Available; explanation.clear(); publishState(); }
  }
  static void registryGlobal( void * data, wl_registry * registry,
                              uint32_t name, char const * interface, uint32_t version )
  {
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    if( std::strcmp( interface, "ext_data_control_manager_v1" ) == 0 && !self->manager )
    {
      self->manager = static_cast<ext_data_control_manager_v1 *>( wl_registry_bind(
        registry, name, &ext_data_control_manager_v1_interface, 1 ) );
      if( !self->manager ) { self->fail( "cannot bind ext_data_control_manager_v1" ); return; }
      self->managerName = name;
      if( !self->flushRequests() ) return;
    }
    else if( std::strcmp( interface, "wl_seat" ) == 0 )
    {
      self->seats.append( {name, version} );
      if( self->seats.size() > 1 && !self->multipleSeatsLogged )
      { self->multipleSeatsLogged = true; gdDebug( "selection-source: multiple seats; watching first seat" ); }
      if( !self->seat && !self->bindSeat( self->seats.first() ) ) return;
    }
    self->maybeAvailable();
  }
  static void registryGlobalRemoved( void * data, wl_registry * registry, uint32_t name )
  {
    Q_UNUSED( registry )
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    if( name == self->managerName ) { self->fail( "manager removed" ); return; }
    self->seats.erase( std::remove_if( self->seats.begin(), self->seats.end(),
                       [name]( Seat const & value ) { return value.name == name; } ), self->seats.end() );
    if( name != self->seatNameId ) return;
    self->retirePrimary();
    self->destroyDeviceAndSeat();
    if( self->seats.isEmpty() ) { self->fail( "seat removed" ); return; }
    if( !self->bindSeat( self->seats.first() ) ) return;
    self->answerInterest( {}, 0 );
    self->maybeAvailable();
  }
  static void discoveryDone( void * data, wl_callback * callback, uint32_t callbackData )
  {
    Q_UNUSED( callbackData )
    auto * self = static_cast<Impl *>( data ); self->assertThread();
    wl_callback_destroy( callback ); self->sync = nullptr;
    if( !self->live() ) return;
    self->discoveryComplete = true;
    if( !self->manager ) { self->fail( "no ext_data_control_manager_v1" ); return; }
    self->maybeAvailable();
  }
  static void dataOffer( void * data, ext_data_control_device_v1 * device,
                         ext_data_control_offer_v1 * offer )
  {
    Q_UNUSED( device )
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    self->offers.insert( offer, { ++self->nextOfferId, {} } );
    static ext_data_control_offer_v1_listener const listener{ offeredMime };
    ext_data_control_offer_v1_add_listener( offer, &listener, self );
  }
  static void offeredMime( void * data, ext_data_control_offer_v1 * offer, char const * mimeType )
  {
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    auto it = self->offers.find( offer );
    if( it != self->offers.end() ) it.value().mimes.append( QString::fromUtf8( mimeType ) );
  }
  static void selection( void * data, ext_data_control_device_v1 * device,
                         ext_data_control_offer_v1 * offer )
  {
    Q_UNUSED( device )
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    Offer const candidate = self->offers.value( offer, {0,{}} );
    self->offersState.pendingOffer( candidate.id, candidate.mimes );
    auto const discarded = self->offersState.discardForClipboard();
    self->destroyOffer( discarded.id );
    self->flushRequests();
  }
  static void primarySelection( void * data, ext_data_control_device_v1 * device,
                                ext_data_control_offer_v1 * offer )
  {
    qint64 const receivedAt = Monotonic::nowMs();
    Q_UNUSED( device )
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    if( !self->primarySeen )
    {
      self->primarySeen = true;
      gdDebug( "selection-source: primary-selection-seen=1" );
      if( self->current == State::Available ) emit self->q->capabilityChanged();
    }
    Offer const candidate = self->offers.value( offer, {0,{}} );
    self->retirePrimary();
    self->offersState.pendingOffer( candidate.id, candidate.mimes );
    self->primaryId = self->offersState.takeForPrimary().id;
    self->offerStamp = receivedAt;
    if( !self->flushRequests() ) return;
    if( !offer ) { self->expireInterest(); self->answerInterest( self->cache, self->cacheStamp ); return; }
    if( !self->primaryId || chooseMimeType( candidate.mimes ).isEmpty() )
    {
      gdDebug( "selection-ignored reason=no-text-mime" );
      self->dropCapture( false ); return;
    }
    if( self->interestedId ) { self->expireInterest(); self->promote(); }
    else self->debounce.start( DebounceMs );
  }
  static void deviceFinished( void * data, ext_data_control_device_v1 * device )
  {
    Q_UNUSED( device )
    auto * self = static_cast<Impl *>( data ); self->assertThread();
    if( self->live() ) self->fail( "device finished" );
  }
  static void seatCapabilities( void * data, wl_seat * seat, uint32_t capabilities )
  {
    Q_UNUSED( seat ) Q_UNUSED( capabilities )
    static_cast<Impl *>( data )->assertThread();
  }
  static void seatName( void * data, wl_seat * seat, char const * name )
  {
    Q_UNUSED( seat ) Q_UNUSED( name )
    static_cast<Impl *>( data )->assertThread();
  }
  void dropCapture( bool timeout )
  {
    if( timeout ) gdDebug( "selection-ignored reason=timeout" );
    retirePrimary();
    flushRequests();
    expireInterest();
    answerInterest( {}, 0 );
  }
  void promote()
  {
    if( !live() || !primaryId || readToken ) return;
    debounce.stop();
    if( !transferStartedAt )
    {
      transferStartedAt = Monotonic::nowMs();
      transferDeadline = transferStartedAt + TransferTimeoutMs;
    }
    startMimeAttempt();
  }
  bool startMimeAttempt()
  {
    assertThread();
    if( !live() || !primaryId ) return false;
    if( Monotonic::nowMs() >= transferDeadline ) { dropCapture( true ); return false; }
    if( !offersState.beginMimeAttempt() ) { dropCapture( false ); return false; }
    int fds[2] = { -1, -1 };
    if( pipe2( fds, O_CLOEXEC ) != 0 )
    { gdDebug( "selection-transfer-error operation=pipe errno=%d", errno ); dropCapture( false ); return false; }
    int const oldFlags = fcntl( fds[0], F_GETFL );
    if( oldFlags < 0 || fcntl( fds[0], F_SETFL, oldFlags | O_NONBLOCK ) < 0 )
    {
      int const error = errno; close( fds[0] ); close( fds[1] );
      gdDebug( "selection-transfer-error operation=fcntl errno=%d", error ); dropCapture( false ); return false;
    }
    readEnd = fds[0]; bytesObserved = 0; payload = PayloadAccumulator{};
    readToken = offersState.beginRead();
    ReadToken const token = readToken;
    lastProgressAt = Monotonic::nowMs();
    QByteArray const mime = offersState.currentMimeType().toUtf8();
    ext_data_control_offer_v1_receive( proxyFor( primaryId ), mime.constData(), fds[1] );
    close( fds[1] );
    if( !flushRequests() ) return false;
    pipeRead = std::make_unique<QSocketNotifier>( readEnd, QSocketNotifier::Read );
    QObject::connect( pipeRead.get(), &QSocketNotifier::activated, q, [this, token] { drainPipe( token ); } );
    QObject::connect( &transferWake, &QTimer::timeout, q, [this, token] { checkTransferDeadline( token ); } );
    checkTransferDeadline( token );
    return offersState.accepts( token );
  }
  void checkTransferDeadline( ReadToken token )
  {
    assertThread();
    if( ownerStopped || !offersState.accepts( token ) ) return;
    qint64 const now = Monotonic::nowMs();
    if( now >= transferDeadline ) { dropCapture( true ); return; }
    if( now >= lastProgressAt + IdleTimeoutMs )
    {
      if( bytesObserved == 0 )
      {
        offersState.mimeAttemptFailed(); closeRead();
        if( !offersState.beginMimeAttempt() ) { dropCapture( true ); return; }
        startMimeAttempt();
      }
      else dropCapture( true );
      return;
    }
    transferWake.start( int( std::max<qint64>( 1, std::min( transferDeadline, lastProgressAt + IdleTimeoutMs ) - now ) ) );
  }
  void complete( ReadToken token )
  {
    if( !offersState.accepts( token ) ) { gdDebug( "selection-ignored reason=stale-token" ); return; }
    checkTransferDeadline( token );
    if( !offersState.accepts( token ) ) return;
    cache = decodePayload( payload.payload() );
    cacheStamp = cache.isEmpty() ? 0 : offerStamp;
    gdDebug( "selection-captured mime=%s bytes=%lld truncated=%d", offersState.currentMimeType().toUtf8().constData(),
             static_cast<long long>( payload.payload().size() ), payload.truncated() ? 1 : 0 );
    offersState.completed( token );
    closeRead();
    quint64 const id = std::exchange( primaryId, 0 );
    destroyOffer( id );
    transferStartedAt = 0; transferDeadline = 0;
    flushRequests();
    expireInterest();
    answerInterest( cache, cacheStamp );
  }
  void drainPipe( ReadToken token )
  {
    assertThread();
    if( ownerStopped ) return;
    if( !offersState.accepts( token ) ) { gdDebug( "selection-ignored reason=stale-token" ); return; }
    pipeRead->setEnabled( false );
    for( ;; )
    {
      checkTransferDeadline( token );
      if( !offersState.accepts( token ) ) return;
      char buffer[16384];
      int const remaining = MaxPayloadBytes + 1 - bytesObserved;
      ssize_t const count = ::read( readEnd, buffer, std::min<int>( sizeof buffer, remaining ) );
      if( count > 0 )
      {
        payload.append( QByteArray( buffer, count ) );
        bytesObserved += int( count ); lastProgressAt = Monotonic::nowMs();
        if( bytesObserved == MaxPayloadBytes + 1 ) { complete( token ); return; }
        continue;
      }
      if( count == 0 ) { complete( token ); return; }
      if( errno == EINTR ) continue;
      if( errno == EAGAIN || errno == EWOULDBLOCK )
      { checkTransferDeadline( token ); if( offersState.accepts( token ) ) pipeRead->setEnabled( true ); return; }
      int const error = errno;
      gdDebug( "selection-transfer-error operation=read errno=%d", error );
      if( bytesObserved == 0 )
      { offersState.mimeAttemptFailed(); closeRead(); startMimeAttempt(); }
      else dropCapture( false );
      return;
    }
  }
#endif // HAVE_WAYLAND_DATA_CONTROL
};
WaylandSelectionSource::WaylandSelectionSource( QObject * parent ) : QObject( parent ), d( std::make_unique<Impl>( this ) ) {}
WaylandSelectionSource::~WaylandSelectionSource() { stop(); }
WaylandSelectionSource::State WaylandSelectionSource::state() const noexcept { return d->current; }
QString WaylandSelectionSource::reason() const { return d->explanation; }
bool WaylandSelectionSource::primarySelectionSeen() const noexcept { return d->primarySeen; }
QString WaylandSelectionSource::lastText() const { return d->cache; }
bool WaylandSelectionSource::hasPendingCapture() const noexcept { return d->pending(); }
void WaylandSelectionSource::start() { d->start(); }
void WaylandSelectionSource::stop() { d->stop(); }
void WaylandSelectionSource::requestCapture( quint64 requestId ) { d->requestCapture( requestId ); }
```

The no-feature compilation retains the public API and cache-answer/stop rules without any Wayland/POSIX type or syscall. Fix any compiler diagnostic before continuing; no alternate public API is permitted.

- [ ] **Step 5: Wire the optional feature and header-only test dependency**

Immediately after the existing `CONFIG += link_pkgconfig` in the application's `unix:!mac` block insert exactly:

```qmake
packagesExist(wayland-client) {
    DEFINES += HAVE_WAYLAND_DATA_CONTROL
    PKGCONFIG += wayland-client
    INCLUDEPATH += $$PWD/protocols/ext-data-control-v1
    SOURCES += $$PWD/protocols/ext-data-control-v1/ext-data-control-v1-protocol.c
}
```

Add `waylandselection.hh` and `waylandselection.cc` to the unconditional lists beside `waylandselectionlogic.hh/.cc`, so the source header is MOCed by the application in all builds. Extend the existing `unix:!mac` scope in testsupport.pri to:

```qmake
unix:!mac {
    DEFINES += HAVE_X11
    CONFIG += link_pkgconfig
    packagesExist(wayland-client) {
        DEFINES += HAVE_WAYLAND_DATA_CONTROL
    }
}
```

In `tst_hotkeys.pro` before FORMS insert this comment, and leave its production link closure unchanged:

```qmake
# preferences.hh uses only waylandselection.hh's enum, snapshot and capability
# constant. Do not list that Q_OBJECT header in HEADERS: the source implementation
# and its MOC belong to the application and the separate live runner, not this suite.
```

- [ ] **Step 6: Verify generated bytes, full build, pure/source gates and resource contracts**

```bash
qmake6 goldendict.pro
make -j$(nproc)
make tests-makefile
make tests-build
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys capabilityConstantMatchesBuild scanPopupSourceLoopContractIsPinned
QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh make tests-run
```

Record source assertions as source evidence. The live transfer tests land with the self-contained harness in Task 7; no popup runtime claim belongs to this task. Before committing inspect each callback failure path for S1, the sole nonzero transfer-start assignment for S2, the overlap branch for S3, synchronous outside-dispatch disposal for S4/S5 and the no-`this` deferred capture. Stage protocol files, regenerate again, then require `git diff --exit-code -- protocols/ext-data-control-v1/` to prove the second generation does not change the staged bytes.

- [ ] **Step 7: Commit the complete source and optional build**

```bash
git add waylandselection.hh waylandselection.cc protocols/ext-data-control-v1/ext-data-control-v1.xml protocols/ext-data-control-v1/ext-data-control-v1-client-protocol.h protocols/ext-data-control-v1/ext-data-control-v1-protocol.c protocols/ext-data-control-v1/regenerate.sh goldendict.pro tests/testsupport.pri tests/tst_hotkeys/tst_hotkeys.pro tests/tst_hotkeys/tst_hotkeys.cc
protocols/ext-data-control-v1/regenerate.sh
git diff --exit-code -- protocols/ext-data-control-v1/
git commit -m "feat(selection): add nonblocking private ext-data-control source"
```

## Task 4: Persist opt-in selected-text preferences without rewriting stored keys

**Implementer tier:** Advanced
**Lane:** implementation

**Files:**

- Modify: `config.hh:310-317`
- Modify: `config.cc:310-317`
- Modify: `config.cc:969-974`
- Modify: `config.cc:1864-1872`
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:176-187` (test-local XML amendment helper)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:397-472` (five declarations)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:2844-2845` (definitions)
- Test: `tests/tst_hotkeys/tst_hotkeys.cc`

**Interfaces:**

- Consumes existing `Config::Preferences::Preferences()`, `Config::Class Config::load()`, `void Config::save( Config::Class const & )`, `QString Config::getConfigFileName()`, `Config::HotKey( QKeySequence const & )`, `QKeySequence Config::HotKey::toKeySequence() const`, and `HotKey::{modifiers,key1,key2,rawText}`. `loadHotKey( QString const & )` and `hotKeyStorageText( HotKey const & )` remain private implementation helpers, observed only through load/save.
- Consumes `bool HotkeySyntax::preferredTrigger( Config::HotKey const &, QString * trigger, QString * reason )`; shared `bool isKnownPortalBindingId( QString const & )` from Task 2; `PortalBindingDisplay { QString id; QString triggerDescription; }`.
- Consumes test members `QString TstHotkeys::installConfigFixture( QString const & )` and `QDomDocument TstHotkeys::saveAndRead( Config::Class const & )`; test-local `QByteArray readFileBytes( QString const & )`.
- Consumes existing `Preferences( QWidget * parent, Config::Class & cfg )`, `Config::Preferences Preferences::getPreferences()` and inherited `void QDialog::accept()`; no new row or getter API is required for the unedited-key round trip.
- Produces `bool Config::Preferences::enableScanPopupHotkey` (default false) and `Config::HotKey Config::Preferences::scanPopupHotkey` (one Ctrl+Meta+D combination); direct preference XML children `enableScanPopupHotkey` and `scanPopupHotkey`. Produces test helper `bool selectionAmendPreference( QString const & name, QString const & value, bool present = true )`.

- [ ] **Step 1: Add the exact five config tests and real-fixture amendment helper**

Insert the helper into the existing anonymous namespace. It edits only the suite's installed isolated fixture, never repository fixtures:

```cpp
bool selectionAmendPreference( QString const & name, QString const & value, bool present = true )
{
  QString const path = Config::getConfigFileName();
  QDomDocument document;
  if( !document.setContent( readFileBytes( path ) ) ) return false;
  auto preferences = document.documentElement().firstChildElement( "preferences" );
  if( preferences.isNull() ) return false;
  while( !preferences.firstChildElement( name ).isNull() )
    preferences.removeChild( preferences.firstChildElement( name ) );
  if( present )
  {
    auto element = document.createElement( name );
    element.appendChild( document.createTextNode( value ) ); preferences.appendChild( element );
  }
  QFile output( path );
  if( !output.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) return false;
  auto const bytes = document.toByteArray();
  return output.write( bytes ) == bytes.size();
}
```

```cpp
void defaultScanPopupHotkeyIsBindable();
void scanPopupPortalBindingSurvivesSaveLoad();
void scanPopupHotkeyKeepsVerbatimText();
void legacyConfigDefaultsScanPopupHotkeyOff();
void scanPopupConfigBooleanValues();
```

```cpp
void TstHotkeys::defaultScanPopupHotkeyIsBindable()
{
  Config::Class cfg;
  auto const & p = cfg.preferences;
  QVERIFY( !p.enableScanPopupHotkey );
  QCOMPARE( p.scanPopupHotkey.key1, int( Qt::Key_D ) ); QCOMPARE( p.scanPopupHotkey.key2, 0 );
  QCOMPARE( p.scanPopupHotkey.modifiers, Qt::KeyboardModifiers( Qt::ControlModifier | Qt::MetaModifier ) );
  QCOMPARE( p.scanPopupHotkey.toKeySequence().count(), 1 );
  QCOMPARE( p.scanPopupHotkey.toKeySequence().toString( QKeySequence::PortableText ), QString( "Meta+Ctrl+D" ) );
  QString trigger, reason;
  QVERIFY2( HotkeySyntax::preferredTrigger( p.scanPopupHotkey, &trigger, &reason ), qPrintable( reason ) );
  QVERIFY( reason.isEmpty() ); QCOMPARE( trigger, QString( "CTRL+LOGO+d" ) );
  auto const saved = saveAndRead( cfg );
  QCOMPARE( saved.documentElement().firstChildElement( "preferences" ).firstChildElement( "scanPopupHotkey" ).text(), QString( "Meta+Ctrl+D" ) );
}
void TstHotkeys::scanPopupPortalBindingSurvivesSaveLoad()
{
  Config::Class cfg;
  cfg.hotkeys.portalBindings = { { "main-window", "Main" }, { "clipboard-lookup", "Clipboard" },
    { "scan-popup", "Portal <selected> & key" }, { "foreign-action", "Foreign" } };
  QDomDocument saved = saveAndRead( cfg );
  auto bindings = saved.documentElement().firstChildElement( "hotkeys" ).firstChildElement( "portalBindings" );
  auto const nodes = bindings.elementsByTagName( "binding" ); QCOMPARE( nodes.size(), 3 );
  QSet<QString> ids;
  for( int i = 0; i != nodes.size(); ++i )
  {
    auto const e = nodes.at( i ).toElement(); ids.insert( e.attribute( "id" ) );
    if( e.attribute( "id" ) == "scan-popup" ) QCOMPARE( e.attribute( "triggerDescription" ), QString( "Portal <selected> & key" ) );
  }
  QCOMPARE( ids, (QSet<QString>{ "main-window", "clipboard-lookup", "scan-popup" }) );
  auto loaded = Config::load(); int count = 0;
  for( auto const & entry : loaded.hotkeys.portalBindings ) if( entry.id == "scan-popup" )
  { ++count; QCOMPARE( entry.triggerDescription, QString( "Portal <selected> & key" ) ); }
  QCOMPARE( count, 1 );
  QCOMPARE( loaded.preferences.scanPopupHotkey.toKeySequence().toString( QKeySequence::PortableText ), QString( "Meta+Ctrl+D" ) );
  for( auto const & pair : QList<QPair<QString,QString>>{ { "scan-popup", "last <description> & value" }, { "foreign-action", "ignored" } } )
  {
    auto e = saved.createElement( "binding" ); e.setAttribute( "id", pair.first );
    e.setAttribute( "triggerDescription", pair.second ); bindings.appendChild( e );
  }
  QFile file( Config::getConfigFileName() ); QVERIFY( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
  auto bytes = saved.toByteArray(); QCOMPARE( file.write( bytes ), bytes.size() ); file.close();
  loaded = Config::load(); count = 0;
  for( auto const & entry : loaded.hotkeys.portalBindings )
  {
    QVERIFY( entry.id != "foreign-action" );
    if( entry.id == "scan-popup" ) { ++count; QCOMPARE( entry.triggerDescription, QString( "last <description> & value" ) ); }
  }
  QCOMPARE( count, 1 );
}
void TstHotkeys::scanPopupHotkeyKeepsVerbatimText()
{
  for( QString const & raw : QStringList{ "Ctrl+Meta+D", "Ctrl+G,Ctrl+H,Ctrl+I", "not a shortcut" } )
  {
    QVERIFY( !installConfigFixture( "config/legacy.xml" ).isEmpty() );
    QVERIFY( selectionAmendPreference( "enableScanPopupHotkey", "1" ) );
    QVERIFY( selectionAmendPreference( "scanPopupHotkey", raw ) );
    auto cfg = Config::load(); QCOMPARE( cfg.preferences.scanPopupHotkey.rawText, raw );
    Preferences dialog( nullptr, cfg ); dialog.accept();
    cfg.preferences = dialog.getPreferences();
    auto const saved = saveAndRead( cfg );
    QCOMPARE( saved.documentElement().firstChildElement( "preferences" ).firstChildElement( "scanPopupHotkey" ).text(), raw );
    QCOMPARE( Config::load().preferences.scanPopupHotkey.rawText, raw );
  }
}
void TstHotkeys::legacyConfigDefaultsScanPopupHotkeyOff()
{
  QString const installed = installConfigFixture( "config/legacy.xml" ); QVERIFY( !installed.isEmpty() );
  QByteArray const before = readFileBytes( installed );
  auto cfg = Config::load(); QCOMPARE( readFileBytes( installed ), before );
  QVERIFY( !cfg.preferences.enableScanPopupHotkey );
  auto const & key = cfg.preferences.scanPopupHotkey;
  QCOMPARE( key.key1, int( Qt::Key_D ) ); QCOMPARE( key.key2, 0 );
  QCOMPARE( key.modifiers, Qt::KeyboardModifiers( Qt::ControlModifier | Qt::MetaModifier ) );
  QCOMPARE( key.toKeySequence().count(), 1 );
  QCOMPARE( key.toKeySequence().toString( QKeySequence::PortableText ), QString( "Meta+Ctrl+D" ) );
  auto const saved = saveAndRead( cfg ); auto const p = saved.documentElement().firstChildElement( "preferences" );
  QCOMPARE( p.firstChildElement( "enableScanPopupHotkey" ).text(), QString( "0" ) );
  QCOMPARE( p.firstChildElement( "scanPopupHotkey" ).text(), QString( "Meta+Ctrl+D" ) );
  auto const reloaded = Config::load(); QVERIFY( !reloaded.preferences.enableScanPopupHotkey );
  QCOMPARE( reloaded.preferences.scanPopupHotkey.toKeySequence().toString( QKeySequence::PortableText ), QString( "Meta+Ctrl+D" ) );
}
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
  QString trigger, reason; QVERIFY( !HotkeySyntax::preferredTrigger( empty, &trigger, &reason ) );
  QVERIFY( selectionAmendPreference( "scanPopupHotkey", "", false ) );
  QCOMPARE( Config::load().preferences.scanPopupHotkey.toKeySequence().toString( QKeySequence::PortableText ), QString( "Meta+Ctrl+D" ) );
}
```

- [ ] **Step 2: Compile and record RED for the absent preference members**

Run `make tests-makefile` and `make tests-build`; require compiler diagnostics naming `enableScanPopupHotkey`/`scanPopupHotkey`, rather than an unrelated environment failure.

- [ ] **Step 3: Insert the exact members, defaults and load/save blocks**

Immediately after `HotKey clipboardHotkey;` in Config::Preferences:

```cpp
bool enableScanPopupHotkey;
HotKey scanPopupHotkey;
```

Immediately after `clipboardHotkey( QKeySequence( "Ctrl+C,C" ) ),` in its constructor:

```cpp
enableScanPopupHotkey( false ),
scanPopupHotkey( QKeySequence( Qt::CTRL | Qt::META | Qt::Key_D ) ),
```

Immediately after the clipboard key load, before enableScanPopup:

```cpp
applyBoolOption( c.preferences.enableScanPopupHotkey,
                 preferences.namedItem( "enableScanPopupHotkey" ) );
if( !preferences.namedItem( "scanPopupHotkey" ).isNull() )
  c.preferences.scanPopupHotkey = loadHotKey(
      preferences.namedItem( "scanPopupHotkey" ).toElement().text() );
```

Immediately before the existing scan-popup save block:

```cpp
opt = dd.createElement( "enableScanPopupHotkey" );
opt.appendChild( dd.createTextNode( c.preferences.enableScanPopupHotkey ? "1" : "0" ) );
preferences.appendChild( opt );

opt = dd.createElement( "scanPopupHotkey" );
opt.appendChild( dd.createTextNode( hotKeyStorageText( c.preferences.scanPopupHotkey ) ) );
preferences.appendChild( opt );
```

No private Config helper becomes public. An explicitly empty node remains empty, an absent node retains the constructed default, and malformed booleans leave false intact.

- [ ] **Step 4: Verify real XML round trips and the full gate**

```bash
qmake6 goldendict.pro
make -j$(nproc)
make tests-makefile
make tests-build
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys defaultScanPopupHotkeyIsBindable scanPopupPortalBindingSurvivesSaveLoad scanPopupHotkeyKeepsVerbatimText legacyConfigDefaultsScanPopupHotkeyOff scanPopupConfigBooleanValues
QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh make tests-run
```

- [ ] **Step 5: Commit the configuration deliverable**

```bash
git add config.hh config.cc tests/tst_hotkeys/tst_hotkeys.cc
git commit -m "feat(config): persist opt-in selected-text shortcut preferences"
```

## Task 5: Wire selection ownership and portal activation through the popup

**Implementer tier:** Capable
**Lane:** implementation

**Files:**

- Modify: `mainwindow.hh:34-37`
- Modify: `mainwindow.hh:109-114`
- Modify: `mainwindow.hh:179-185`
- Modify: `mainwindow.cc:833-837`
- Modify: `mainwindow.cc:1026-1032`
- Modify: `mainwindow.cc:1282-1286`
- Modify: `mainwindow.cc:1308-1331`
- Modify: `mainwindow.cc:1519-1526`
- Modify: `mainwindow.cc:3211-3286`
- Modify: `scanpopup.hh:50-54`
- Modify: `scanpopup.cc:469-469` (insert immediately before translateWord)
- Modify: `preferences.hh:7-8`
- Modify: `preferences.hh:28-30`
- Modify: `preferences.cc:615-620` (compatible final parameter only)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:397-472` (three declarations)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:2844-2845` (definitions)
- Test: `tests/tst_hotkeys/tst_hotkeys.cc`

**Interfaces:**

- Consumes `bool scanPopupBindingPlanned( QString const &, bool ) noexcept`, `bool scanPopupCaptureSupported( bool, QString const & ) noexcept`, `inline constexpr bool kHaveWaylandDataControl`, `qint64 Monotonic::nowMs() noexcept`, `bool WaylandSelection::clipboardStampAdvances( bool ) noexcept`, and `QString WaylandSelection::chooseText( QString const & selection, qint64 capturedAt, QString const & clipboard, qint64 clipboardChangedAt )`.
- Consumes `WaylandSelectionSource( QObject * parent = nullptr )`, `~WaylandSelectionSource() override`, `State state() const noexcept`, `QString reason() const`, `bool primarySelectionSeen() const noexcept`, `QString lastText() const`, `bool hasPendingCapture() const noexcept`, `void start()`, `void stop()`, `void requestCapture( quint64 requestId )`; signals `void stateChanged( State, QString const & )`, `void capabilityChanged()`, `void captureFinished( quint64, QString const &, qint64 )`; `State { Unsupported, Starting, Available, Unavailable }`; `SelectionStatus { State state=Unsupported; QString reason; bool primarySelectionSeen=false; }`.
- Consumes `void PlatformWindow::applyActivation( QWidget *, QString const & token )`; `void ScanPopup::translateWord( QString const & )`; `Config::InputPhrase Config::Preferences::sanitizeInputPhrase( QString const & ) const`; `bool Config::InputPhrase::isValid() const` and phrase equality; `void MainStatusBar::showMessage( QString const &, int, QPixmap const & = QPixmap() )`; existing QSystemTrayIcon notification API.
- Consumes `void MainWindow::installHotKeys( HotkeyInstallIntent intent = HotkeyInstallIntent::Refresh )` and `void MainWindow::refreshHotkeyStatus()`; `bool HotkeyInstallGate::shouldBind( bool, QVector<HotkeyBinding> const &, HotkeyBackend::State, HotkeyInstallIntent )`; Config's two new members from Task 4.
- Produces `void MainWindow::reconcileSelectionSource()`, `void MainWindow::reportNothingToTranslate()`, `std::unique_ptr<WaylandSelectionSource> selectionSource`, `quint64 scanPopupRequestId=0`, `qint64 clipboardChangedAt=0`; `bool ScanPopup::translateSelectedText( QString const & text )`; and `void Preferences::setHotkeyStatus( QString const & platform, HotkeyBackend::State state, QString const & explanation, QVector<PortalBindingDisplay> const & bindings, bool configureAvailable, SelectionStatus const & selection = SelectionStatus{} )` (default only in the declaration).
- Consumes test helpers `selectionSourceText( char const * ) -> QString`, `selectionBlock( QString const &, QString const & ) -> QString`, `selectionCompact( QString ) -> QString`, `selectionOrdered( QString const &, QStringList const & ) -> bool`. The three tests below are source evidence, not widget/source runtime tests.

- [ ] **Step 1: Add the three complete failing wiring tests**

Declare each slot once and insert these definitions before QTEST_MAIN:

```cpp
void sourceFollowsThePreference();
void scanPopupActivationWiringIsPinned();
void xcbGuardDoesNotCreateThePopup();
```

```cpp
void TstHotkeys::sourceFollowsThePreference()
{
  qInfo( "source assertions: source preference/lifetime wiring; no runtime MainWindow observation" );
  QString const source = selectionSourceText( "mainwindow.cc" );
  QString const header = selectionSourceText( "mainwindow.hh" );
  QVERIFY( !source.isEmpty() && !header.isEmpty() );
  QVERIFY( selectionOrdered( header, { "std::unique_ptr< WaylandSelectionSource > selectionSource", "sptr< ScanPopup > scanPopup", "std::unique_ptr<ArticleProfileController> articleProfile" } ) );
  QString const reconcile = selectionBlock( source, "void MainWindow::reconcileSelectionSource()" );
  QVERIFY( !reconcile.isEmpty() );
  QVERIFY( selectionCompact( reconcile ).contains( "boolconstwanted=cfg.preferences.enableScanPopupHotkey&&scanPopupCaptureSupported(kHaveWaylandDataControl,QGuiApplication::platformName());" ) );
  QString const disabled = selectionBlock( reconcile, "if( !wanted )" ); QVERIFY( !disabled.isEmpty() );
  QVERIFY( selectionOrdered( disabled, { "++scanPopupRequestId", "selectionSource->stop()", "selectionSource.reset()", "refreshHotkeyStatus()", "return" } ) );
  QVERIFY( selectionCompact( reconcile ).contains( "if(selectionSource){refreshHotkeyStatus();return;}" ) );
  QCOMPARE( reconcile.count( "std::make_unique<WaylandSelectionSource>(this)" ), 1 );
  QVERIFY( selectionOrdered( reconcile, { "std::make_unique<WaylandSelectionSource>(this)", "&WaylandSelectionSource::stateChanged", "&WaylandSelectionSource::capabilityChanged", "&WaylandSelectionSource::captureFinished", "QPointer< WaylandSelectionSource >", "QTimer::singleShot" } ) );
  QString const deferred = selectionBlock( reconcile, "QTimer::singleShot" );
  QVERIFY( selectionCompact( deferred ).contains( "if(shuttingDown||!source||source.data()!=selectionSource.get())return;" ) );
  QVERIFY( selectionOrdered( deferred, { "if( shuttingDown", "source->start()" } ) );
  QString const install = selectionBlock( source, "void MainWindow::installHotKeys(" );
  QVERIFY( selectionOrdered( install, { "if( shuttingDown ) return", "reconcileSelectionSource()", "installGate.shouldBind" } ) );
  QVERIFY( selectionBlock( source, "MainWindow::MainWindow(" ).contains( "installHotKeys()" ) );
  QVERIFY( selectionBlock( source, "void MainWindow::editPreferences()" ).contains( "installHotKeys( result == QDialog::Accepted" ) );
  for( QString const & name : QStringList{ "MainWindow::~MainWindow()", "void MainWindow::quitApp()", "void MainWindow::finishShutdown()", "void MainWindow::failStartup(" } )
  {
    QString const b = selectionBlock( source, name ); QVERIFY2( !b.isEmpty(), qPrintable( name ) );
    QVERIFY2( selectionCompact( b ).contains( "if(selectionSource)selectionSource->stop();scanPopup.reset();" ), qPrintable( "source assertion: missing immediate stop/reset in " + name ) );
  }
  QVERIFY( selectionOrdered( selectionBlock( source, "MainWindow::~MainWindow()" ), { "shuttingDown = true", "hotkeyBackend.reset()", "selectionSource->stop()" } ) );
  QString const failure = selectionBlock( selectionBlock( source, "void MainWindow::failStartup(" ), "connect( dialog" );
  QVERIFY( failure.contains( "selectionSource->stop()" ) );
  for( QString const & name : QStringList{ "void MainWindow::makeScanPopup()", "void MainWindow::editDictionaries(", "void MainWindow::editPreferences()", "void MainWindow::on_rescanFiles_triggered()" } )
  {
    QString const b = selectionBlock( source, name ); QVERIFY2( !b.isEmpty(), qPrintable( name ) );
    QVERIFY2( !b.contains( "selectionSource->stop()" ) && !b.contains( "selectionSource.reset()" ), qPrintable( "source assertion: recreation reconnects source in " + name ) );
  }
}
void TstHotkeys::scanPopupActivationWiringIsPinned()
{
  qInfo( "source assertions: selected-text activation, snapshot and notification wiring" );
  QString const source = selectionSourceText( "mainwindow.cc" ); QVERIFY( !source.isEmpty() );
  QCOMPARE( source.count( "\"scan-popup\"" ), 2 );
  QString const install = selectionBlock( source, "void MainWindow::installHotKeys(" );
  QVERIFY2( selectionCompact( install ).contains( "if(scanPopupBindingPlanned(QGuiApplication::platformName(),cfg.preferences.enableScanPopupHotkey))add(true,\"scan-popup\",tr(\"Translatetheselectedtext\"),cfg.preferences.scanPopupHotkey);" ), "source assertion: binding predicate/description/key" );
  QString const branch = selectionBlock( install, "else if( id == \"scan-popup\" && scanPopup )" );
  QVERIFY2( !branch.isEmpty(), "source assertion: missing activation branch" );
  QVERIFY2( selectionOrdered( branch, { "scanPopup->winId()", "PlatformWindow::applyActivation", "if( selectionSource )" } ), "source assertion: native handle/token/source order" );
  QString const withSource = selectionBlock( branch, "if( selectionSource )" );
  QVERIFY2( selectionOrdered( withSource, { "quint64 const requestId = ++scanPopupRequestId", "selectionSource->requestCapture( requestId )" } ), "source assertion: guarded incremented request" );
  QVERIFY2( selectionCompact( branch ).contains( "elseif(!scanPopup->translateSelectedText(QApplication::clipboard()->text(QClipboard::Clipboard)))reportNothingToTranslate();" ), "source assertion: clipboard-only branch" );
  QCOMPARE( source.count( "requestId != scanPopupRequestId" ), 1 );
  QString const reconcile = selectionBlock( source, "void MainWindow::reconcileSelectionSource()" );
  QString const completion = selectionBlock( reconcile, "&WaylandSelectionSource::captureFinished" );
  QVERIFY2( selectionCompact( completion ).contains( "if(shuttingDown||requestId!=scanPopupRequestId||!scanPopup)return;" ), "source assertion: completion guard" );
  QVERIFY2( selectionCompact( completion ).contains( "WaylandSelection::chooseText(capturedAt>0?selection:QString(),capturedAt,QApplication::clipboard()->text(QClipboard::Clipboard),clipboardChangedAt)" ), "source assertion: suppressed selection and current clipboard at completion" );
  QVERIFY2( selectionCompact( completion ).contains( "if(!scanPopup->translateSelectedText(text))reportNothingToTranslate();" ), "source assertion: completion translation/empty report" );
  QCOMPARE( source.count( "&WaylandSelectionSource::capabilityChanged" ), 1 );
  QVERIFY2( selectionCompact( reconcile ).contains( "&WaylandSelectionSource::capabilityChanged,this,&MainWindow::refreshHotkeyStatus" ), "source assertion: typed capability snapshot refresh" );
  QVERIFY2( reconcile.contains( "&WaylandSelectionSource::stateChanged" ) && reconcile.contains( "&WaylandSelectionSource::captureFinished" ), "source assertion: creation-block signal connections" );
  QCOMPARE( source.count( "clipboardStampAdvances(" ), 1 );
  QString const constructor = selectionBlock( source, "MainWindow::MainWindow(" );
  QString const changed = selectionBlock( constructor, "&QClipboard::changed" );
  QVERIFY2( selectionCompact( changed ).contains( "if(WaylandSelection::clipboardStampAdvances(mode==QClipboard::Clipboard))clipboardChangedAt=Monotonic::nowMs();" ), "source assertion: Clipboard-only monotonic stamp" );
  QVERIFY2( !changed.contains( "QClipboard::Selection" ) && !changed.contains( "QClipboard::FindBuffer" ), "source assertion: no alternate stamp assignment" );
  QString const status = selectionBlock( source, "void MainWindow::refreshHotkeyStatus()" );
  for( QString const & field : QStringList{ "selection.state = selectionSource->state()", "selection.reason = selectionSource->reason()", "selection.primarySelectionSeen = selectionSource->primarySelectionSeen()" } )
    QVERIFY2( status.contains( field ), qPrintable( "source assertion: snapshot " + field ) );
  QVERIFY2( selectionCompact( status ).contains( "hotkeyBackend&&hotkeyBackend->isConfigureAvailable(),selection)" ), "source assertion: final snapshot argument" );
  QString const preferences = selectionSourceText( "preferences.hh" ) + selectionSourceText( "preferences.cc" );
  QVERIFY2( !QRegularExpression( "WaylandSelectionSource\\s*[*&]" ).match( preferences ).hasMatch(), "source assertion: Preferences must hold no source pointer/reference" );
  QString const popup = selectionBlock( selectionSourceText( "scanpopup.cc" ), "bool ScanPopup::translateSelectedText(" );
  QVERIFY2( selectionOrdered( popup, { "sanitizeInputPhrase( text )", "if( !phrase.isValid() )", "return false", "if( isVisible() && phrase == inputPhrase )", "return true", "translateWord( text )", "return true" } ), "source assertion: sanitize/invalid/dedupe/translate order" );
  QVERIFY2( !popup.contains( "showStatusBarMessage" ) && !popup.contains( "engagePopup" ) && !popup.contains( "forcePopup" ), "source assertion: entry point preserves existing translation path" );
  QString const report = selectionBlock( source, "void MainWindow::reportNothingToTranslate()" );
  QCOMPARE( report.count( "gdDebug( \"selection-empty\" )" ), 1 );
  QVERIFY2( selectionOrdered( report, { "gdDebug", "No selected text or clipboard text to translate", "if( isVisible() )", "mainStatusBar->showMessage( message, 3000 )", "else if( trayIcon )", "trayIcon->showMessage( tr( \"GoldenDict\" ), message, QSystemTrayIcon::Information, 3000 )" } ), "source assertion: exact visible-channel ladder" );
  QVERIFY2( !report.contains( "else\n" ) && !report.contains( "QMessageBox" ) && !report.contains( "scanPopup" ) && !report.contains( "showStatusBarMessage" ), "source assertion: exhausted ladder has no additional visible channel" );
}
void TstHotkeys::xcbGuardDoesNotCreateThePopup()
{
  QVERIFY( !scanPopupBindingPlanned( "xcb", true ) );
  bool const oldScan = false, oldClipboard = false;
  QVERIFY( !oldScan && !oldClipboard && !scanPopupBindingPlanned( "xcb", true ) );
  qInfo( "source assertions: popup creation expression; no widget construction observed" );
  QString const body = selectionBlock( selectionSourceText( "mainwindow.cc" ), "void MainWindow::makeScanPopup()" );
  QVERIFY( !body.isEmpty() );
  QVERIFY( selectionOrdered( body, { "++scanPopupRequestId", "scanPopup.reset()", "if( !cfg.preferences.enableScanPopup", "new ScanPopup" } ) );
  QVERIFY( selectionCompact( body ).contains( "if(!cfg.preferences.enableScanPopup&&!cfg.preferences.enableClipboardHotkey&&!scanPopupBindingPlanned(QGuiApplication::platformName(),cfg.preferences.enableScanPopupHotkey))return;" ) );
  QVERIFY( !body.contains( "!cfg.preferences.enableScanPopupHotkey" ) );
}
```

- [ ] **Step 2: Run RED for the missing MainWindow/ScanPopup wiring**

```bash
make tests-makefile
make tests-build
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys sourceFollowsThePreference scanPopupActivationWiringIsPinned xcbGuardDoesNotCreateThePopup
```

Expected: the source assertions fail on the missing reconciliation/activation/guard, with distinct diagnostics. This RED is independent of MainWindow runtime linkage.

- [ ] **Step 3: Extend the Preferences call signature with its first consumer**

Add `#include "waylandselection.hh"` beside the backend include in `preferences.hh`. Replace its declaration with:

```cpp
void setHotkeyStatus( QString const & platform, HotkeyBackend::State state,
                      QString const & explanation,
                      QVector<PortalBindingDisplay> const & bindings,
                      bool configureAvailable,
                      SelectionStatus const & selection = SelectionStatus{} );
```

Change only the definition's signature and insert `Q_UNUSED( selection )` at the beginning of its existing body:

```cpp
void Preferences::setHotkeyStatus( QString const & platform, HotkeyBackend::State state,
                                   QString const & explanation,
                                   QVector<PortalBindingDisplay> const & bindings,
                                   bool configureAvailable,
                                   SelectionStatus const & selection )
```

This keeps the existing five-argument constructor call valid and provides a buildable transport for MainWindow now. Task 6 replaces the whole renderer with the complete snapshot-aware UI; this task introduces no partial UI or failing future test.

- [ ] **Step 4: Add the popup entry point and MainWindow member/lifecycle edits**

Immediately after `translateWord` in `scanpopup.hh` declare:

```cpp
bool translateSelectedText( QString const & text );
```

Immediately before its existing implementation in scanpopup.cc insert:

```cpp
bool ScanPopup::translateSelectedText( QString const & text )
{
  Config::InputPhrase const phrase = cfg.preferences.sanitizeInputPhrase( text );
  if( !phrase.isValid() )
    return false;
  if( isVisible() && phrase == inputPhrase )
    return true;
  translateWord( text );
  return true;
}
```

Add these includes beside the backend includes in `mainwindow.hh`:

```cpp
#include "waylandselection.hh"
#include "waylandselectionlogic.hh"
```

Add the helpers beside refreshHotkeyStatus and the members immediately before `sptr< ScanPopup > scanPopup;`, in exactly this order:

```cpp
void reconcileSelectionSource();
void reportNothingToTranslate();
```

```cpp
std::unique_ptr< WaylandSelectionSource > selectionSource;
quint64 scanPopupRequestId = 0;
qint64 clipboardChangedAt = 0;
```

Immediately before the constructor's `makeScanPopup()` insert the single stamp connection:

```cpp
connect( QApplication::clipboard(), &QClipboard::changed, this,
         [this]( QClipboard::Mode mode ) {
  if( WaylandSelection::clipboardStampAdvances( mode == QClipboard::Clipboard ) )
    clipboardChangedAt = Monotonic::nowMs();
} );
```

On entry to `~MainWindow` insert `shuttingDown = true;` before backend operations. At each of its reset, quitApp's reset, finishShutdown's reset and failStartup's dialog-finished lambda reset, insert this exact line immediately before `scanPopup.reset();`:

```cpp
if( selectionSource ) selectionSource->stop();
```

At the beginning of makeScanPopup, immediately before the existing popup reset, insert:

```cpp
++scanPopupRequestId;
```

Replace only its two-term creation guard with:

```cpp
if( !cfg.preferences.enableScanPopup &&
    !cfg.preferences.enableClipboardHotkey &&
    !scanPopupBindingPlanned( QGuiApplication::platformName(),
                              cfg.preferences.enableScanPopupHotkey ) )
  return;
```

Do not touch the source in editDictionaries, editPreferences, on_rescanFiles_triggered or makeScanPopup. Their existing popup-reset/recreation paths remain; `!scanPopup` discards capture responses while a dialog has removed it.

- [ ] **Step 5: Insert complete reconciliation, reporting, binding and completion code**

Immediately before installHotKeys insert these complete definitions:

```cpp
void MainWindow::reconcileSelectionSource()
{
  bool const wanted = cfg.preferences.enableScanPopupHotkey &&
    scanPopupCaptureSupported( kHaveWaylandDataControl, QGuiApplication::platformName() );
  if( !wanted )
  {
    if( selectionSource )
    {
      ++scanPopupRequestId;
      selectionSource->stop();
      selectionSource.reset();
    }
    refreshHotkeyStatus();
    return;
  }
  if( selectionSource ) { refreshHotkeyStatus(); return; }
  selectionSource = std::make_unique<WaylandSelectionSource>(this);
  connect( selectionSource.get(), &WaylandSelectionSource::stateChanged,
           this, [this]( WaylandSelectionSource::State, QString const & ) { refreshHotkeyStatus(); } );
  connect( selectionSource.get(), &WaylandSelectionSource::capabilityChanged,
           this, &MainWindow::refreshHotkeyStatus );
  connect( selectionSource.get(), &WaylandSelectionSource::captureFinished,
           this, [this]( quint64 requestId, QString const & selection, qint64 capturedAt ) {
    if( shuttingDown || requestId != scanPopupRequestId || !scanPopup ) return;
    QString const text = WaylandSelection::chooseText(
        capturedAt > 0 ? selection : QString(), capturedAt,
        QApplication::clipboard()->text( QClipboard::Clipboard ), clipboardChangedAt );
    if( !scanPopup->translateSelectedText( text ) )
      reportNothingToTranslate();
  } );
  refreshHotkeyStatus();
  QPointer< WaylandSelectionSource > const source( selectionSource.get() );
  QTimer::singleShot( 0, this, [this, source] {
    if( shuttingDown || !source || source.data() != selectionSource.get() ) return;
    source->start();
  } );
}
void MainWindow::reportNothingToTranslate()
{
  gdDebug( "selection-empty" );
  QString const message = tr( "No selected text or clipboard text to translate" );
  if( isVisible() )
    mainStatusBar->showMessage( message, 3000 );
  else if( trayIcon )
    trayIcon->showMessage( tr( "GoldenDict" ), message, QSystemTrayIcon::Information, 3000 );
}
```

Immediately after installHotKeys' shutdown guard call `reconcileSelectionSource();`. After its two old add calls and before installGate.shouldBind insert exactly:

```cpp
if( scanPopupBindingPlanned( QGuiApplication::platformName(),
                             cfg.preferences.enableScanPopupHotkey ) )
  add( true, "scan-popup", tr( "Translate the selected text" ), cfg.preferences.scanPopupHotkey );
```

After the existing clipboard activation branch, preserving both old branches byte-for-byte, insert:

```cpp
else if( id == "scan-popup" && scanPopup )
{
  scanPopup->winId();
  PlatformWindow::applyActivation( scanPopup.get(), token );
  if( selectionSource )
  {
    quint64 const requestId = ++scanPopupRequestId;
    selectionSource->requestCapture( requestId );
  }
  else if( !scanPopup->translateSelectedText(
             QApplication::clipboard()->text( QClipboard::Clipboard ) ) )
    reportNothingToTranslate();
}
```

There must be exactly two `"scan-popup"` literals in mainwindow.cc after this insertion. Do not add that literal to comments or diagnostics there. Replace refreshHotkeyStatus with:

```cpp
void MainWindow::refreshHotkeyStatus()
{
  SelectionStatus selection;
  if( selectionSource )
  {
    selection.state = selectionSource->state();
    selection.reason = selectionSource->reason();
    selection.primarySelectionSeen = selectionSource->primarySelectionSeen();
  }
  if( preferencesDialog )
    preferencesDialog->setHotkeyStatus( QGuiApplication::platformName(), hotkeyState,
      hotkeyExplanation, reportedHotkeys, hotkeyBackend && hotkeyBackend->isConfigureAvailable(), selection );
}
```

- [ ] **Step 6: Verify application closure, all wiring assertions and existing guards**

```bash
qmake6 goldendict.pro
make -j$(nproc)
make tests-makefile
make tests-build
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys sourceFollowsThePreference scanPopupActivationWiringIsPinned xcbGuardDoesNotCreateThePopup
QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh make tests-run
```

Compare the three protected ScanPopup guard bodies and old MainWindow activation branches to the baseline; require no change except the new entry point outside them. Verify no clipboard stamp reset occurs in Preferences/source/popup recreation. End-to-end translation and visible notification channels remain manual acceptance in Task 8.

- [ ] **Step 7: Commit integrated activation and ownership**

```bash
git add mainwindow.hh mainwindow.cc scanpopup.hh scanpopup.cc preferences.hh preferences.cc tests/tst_hotkeys/tst_hotkeys.cc
git commit -m "feat(scanpopup): dispatch Wayland selected text through existing translation"
```

## Task 6: Add the third Hotkeys row and render selection snapshots

**Implementer tier:** Advanced
**Lane:** implementation

**Files:**

- Modify: `preferences.ui:803-805` (checkbox and editor layout)
- Modify: `preferences.ui:843-847` (status label)
- Modify: `preferences.hh:39-40` (edited flag)
- Modify: `preferences.hh:56-58` (checkbox slot)
- Modify: `preferences.cc:203-209` (constructor)
- Modify: `preferences.cc:431-435` (getter)
- Modify: `preferences.cc:600-613` (event filter)
- Modify: `preferences.cc:615-673` (complete renderer replacement)
- Modify: `preferences.cc:748-752` (auto-connected slot)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:31-78` (QScopeGuard include)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:397-472` (seven declarations)
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:2844-2845` (definitions)
- Test: `tests/tst_hotkeys/tst_hotkeys.cc`

**Interfaces:**

- Consumes `Preferences( QWidget *, Config::Class & )`; `Config::Preferences getPreferences()`; the exact six-argument `void setHotkeyStatus( QString const &, HotkeyBackend::State, QString const &, QVector<PortalBindingDisplay> const &, bool, SelectionStatus const & = SelectionStatus{} )` from Task 5; `SelectionStatus { WaylandSelectionSource::State state=Unsupported; QString reason; bool primarySelectionSeen=false; }` and enum values Unsupported/Starting/Available/Unavailable.
- Consumes `Config::Preferences::{enableScanPopupHotkey,scanPopupHotkey}` from Task 4; `void HotKeyEdit::setHotKey( Config::HotKey const & )`, `Config::HotKey HotKeyEdit::getHotKey() const`; `bool HotkeySyntax::parse( QString const &, Config::HotKey *, QString * )`, `bool HotkeySyntax::preferredTrigger( Config::HotKey const &, QString *, QString * )`. Raw text is stored in Config, not retained by HotKeyEdit's getter.
- Consumes `HotkeyBackend::State { Unsupported, Creating, Ready, Binding, Bound, Partial, Denied, Unavailable, Closed }`; `PortalBindingDisplay { QString id; QString triggerDescription; }`; existing test `bool xcbSession()` and `PrivateSessionBus::start( QString * )`, `QString address() const`.
- Produces `bool scanPopupHotkeyEdited = false`, private slot `void Preferences::on_enableScanPopupHotkey_toggled( bool checked )`, objects `enableScanPopupHotkey`, `scanPopupHotkey`, `horizontalLayout_scanPopupHotkey`, `horizontalSpacer_scanPopupHotkey`, `scanPopupHotkeyStatus`. Renderer signature remains unchanged from Task 5, so MainWindow keeps compiling.
- Consumes source helpers `QString selectionSourceText( char const * )`, `QString selectionBlock( QString const &, QString const & )`, `QString selectionCompact( QString )` for the actual-QPA getter pin; logical platform tests do not fake the actual application platform.

- [ ] **Step 1: Add the complete seven-function UI inventory**

Add `#include <QScopeGuard>`, declare these seven slots, and insert the full definitions below before QTEST_MAIN. Do not add any other `_data` suppliers or child slot:

```cpp
void selectionStatusRowRendersSnapshots_data();
void selectionStatusRowRendersSnapshots();
void scanPopupRowPlatformMatrix_data();
void scanPopupRowPlatformMatrix();
void scanPopupRowWaylandValuesAndEdits();
void scanPopupRowWaylandProbe();
void scanPopupRowBaseStatuses();
```

```cpp
void TstHotkeys::selectionStatusRowRendersSnapshots_data()
{
  QTest::addColumn<int>( "state" ); QTest::addColumn<QString>( "reason" );
  QTest::addColumn<bool>( "seen" ); QTest::addColumn<QString>( "suffix" );
  using S = WaylandSelectionSource::State;
  QTest::newRow( "starting" ) << int(S::Starting) << QString() << false << QString( "selection source starting" );
  QTest::newRow( "available-seen" ) << int(S::Available) << QString() << true << QString( "primary selection supported" );
  QTest::newRow( "available-unseen" ) << int(S::Available) << QString() << false << QString( "selection not observed yet" );
  QTest::newRow( "unavailable" ) << int(S::Unavailable) << QString( "manager removed" ) << true << QString( "selection unavailable: manager removed — clipboard is used instead" );
  QTest::newRow( "unsupported" ) << int(S::Unsupported) << QString() << false << QString( "selection unsupported in this build — clipboard is used instead" );
}
void TstHotkeys::selectionStatusRowRendersSnapshots()
{
  QFETCH( int, state ); QFETCH( QString, reason ); QFETCH( bool, seen ); QFETCH( QString, suffix );
  Config::Class cfg; cfg.preferences.enableScanPopupHotkey = true;
  cfg.preferences.scanPopupHotkey.rawText = "Ctrl+Meta+D";
  Preferences dialog( nullptr, cfg );
  auto enabled = dialog.findChild<QCheckBox *>( "enableScanPopupHotkey" );
  auto editor = dialog.findChild<HotKeyEdit *>( "scanPopupHotkey" );
  auto label = dialog.findChild<QLabel *>( "scanPopupHotkeyStatus" );
  QVERIFY( enabled && editor && label );
  dialog.setHotkeyStatus( "wayland", HotkeyBackend::State::Bound, {}, {}, true );
  enabled->setChecked( true );
  SelectionStatus snapshot{ WaylandSelectionSource::State(state), reason, seen };
  auto render = [&] { dialog.setHotkeyStatus( "wayland", HotkeyBackend::State::Bound, {}, {{"scan-popup", "Portal selected trigger"}}, true, snapshot ); };
  render(); QCOMPARE( label->text(), QString( "Portal selected trigger · " ) + suffix );
  QCOMPARE( label->textFormat(), Qt::PlainText );
  for( char const * name : { "mainWindowHotkeyStatus", "clipboardHotkeyStatus" } )
  {
    auto other = dialog.findChild<QLabel *>( name ); QVERIFY( other );
    QVERIFY( !other->text().contains( " · " ) ); QCOMPARE( other->textFormat(), Qt::PlainText );
  }
  auto const before = editor->getHotKey().toKeySequence(); QString const display = editor->text();
  snapshot = { WaylandSelectionSource::State::Available, {}, false }; render();
  QCOMPARE( label->text(), QString( "Portal selected trigger · selection not observed yet" ) );
  snapshot.primarySelectionSeen = true; render();
  QCOMPARE( label->text(), QString( "Portal selected trigger · primary selection supported" ) );
  QVERIFY( enabled->isChecked() ); QCOMPARE( editor->text(), display );
  QCOMPARE( editor->getHotKey().toKeySequence(), before );
  QCOMPARE( dialog.getPreferences().scanPopupHotkey.rawText, QString( "Ctrl+Meta+D" ) );
}
void TstHotkeys::scanPopupRowPlatformMatrix_data()
{
  QTest::addColumn<QString>( "platform" ); QTest::addColumn<bool>( "wayland" ); QTest::addColumn<QString>( "label" );
  QTest::newRow( "xcb" ) << QString( "xcb" ) << false << QString( "Not used on X11 — the Held keys setting on the Scan Popup tab applies" );
  QTest::newRow( "windows" ) << QString( "windows" ) << false << QString( "Not used outside Wayland sessions" );
  QTest::newRow( "cocoa" ) << QString( "cocoa" ) << false << QString( "Not used outside Wayland sessions" );
  QTest::newRow( "offscreen" ) << QString( "offscreen" ) << false << QString( "Not used outside Wayland sessions" );
  QTest::newRow( "wayland" ) << QString( "wayland" ) << true << QString( "Portal selected trigger · selection unavailable: manager removed — clipboard is used instead" );
  QTest::newRow( "wayland-egl" ) << QString( "wayland-egl" ) << true << QString( "Portal selected trigger · selection unavailable: manager removed — clipboard is used instead" );
}
void TstHotkeys::scanPopupRowPlatformMatrix()
{
  QFETCH( QString, platform ); QFETCH( bool, wayland ); QFETCH( QString, label );
  Config::Class cfg; cfg.preferences.enableScanPopupHotkey = true;
  cfg.preferences.scanPopupHotkey.rawText = "Ctrl+Meta+D";
  Config::HotKey const original = cfg.preferences.scanPopupHotkey;
  Preferences dialog( nullptr, cfg );
  auto enabled = dialog.findChild<QCheckBox *>( "enableScanPopupHotkey" );
  auto editor = dialog.findChild<HotKeyEdit *>( "scanPopupHotkey" );
  auto status = dialog.findChild<QLabel *>( "scanPopupHotkeyStatus" );
  QVERIFY( enabled && editor && status );
  SelectionStatus const snapshot{ WaylandSelectionSource::State::Unavailable, "manager removed", true };
  auto render = [&] { dialog.setHotkeyStatus( platform, HotkeyBackend::State::Bound, {}, {{"scan-popup", "Portal selected trigger"}}, true, snapshot ); };
  render(); if( wayland ) enabled->setChecked( true ); render();
  QCOMPARE( enabled->isEnabled(), wayland ); QCOMPARE( enabled->isChecked(), wayland );
  QCOMPARE( editor->isEnabled(), wayland ); QCOMPARE( status->text(), label );
  QVERIFY( cfg.preferences.enableScanPopupHotkey );
  QCOMPARE( cfg.preferences.scanPopupHotkey.rawText, original.rawText );
  QCOMPARE( cfg.preferences.scanPopupHotkey.toKeySequence(), original.toKeySequence() );
  QCOMPARE( scanPopupBindingPlanned( platform, true ), wayland );
  if( xcbSession() && !wayland )
  {
    auto const result = dialog.getPreferences(); QVERIFY( result.enableScanPopupHotkey );
    QCOMPARE( result.scanPopupHotkey.rawText, original.rawText );
    QCOMPARE( result.scanPopupHotkey.toKeySequence(), original.toKeySequence() );
    QCOMPARE( result.scanPopupHotkey.key1, original.key1 ); QCOMPARE( result.scanPopupHotkey.key2, original.key2 );
    QCOMPARE( result.scanPopupHotkey.modifiers, original.modifiers );
  }
  qInfo( "source assertions: real-QPA persistence ternaries; logical matrix is renderer evidence" );
  QString const getter = selectionCompact( selectionBlock( selectionSourceText( "preferences.cc" ), "Config::Preferences Preferences::getPreferences()" ) );
  QVERIFY( getter.contains( "boolconstwayland=QGuiApplication::platformName().startsWith(QLatin1String(\"wayland\"));" ) );
  QVERIFY( getter.contains( "p.enableScanPopupHotkey=wayland?ui.enableScanPopupHotkey->isChecked():cfg.preferences.enableScanPopupHotkey;" ) );
  QVERIFY( getter.contains( "p.scanPopupHotkey=wayland?(scanPopupHotkeyEdited?ui.scanPopupHotkey->getHotKey():cfg.preferences.scanPopupHotkey):cfg.preferences.scanPopupHotkey;" ) );
}
void TstHotkeys::scanPopupRowWaylandValuesAndEdits()
{
  QString const kwin = QStandardPaths::findExecutable( "kwin_wayland" );
  QVERIFY2( !kwin.isEmpty(), "required selected-text UI probe needs kwin_wayland" );
  TestSupport::PrivateSessionBus bus; QString reason; QVERIFY2( bus.start( &reason ), qPrintable( reason ) );
  QTemporaryDir runtime; QVERIFY( runtime.isValid() );
  QVERIFY( QFile::setPermissions( runtime.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );
  QProcess compositor, child;
  auto cleanup = qScopeGuard( [&] {
    for( QProcess * process : { &child, &compositor } ) if( process->state() != QProcess::NotRunning )
    {
      process->terminate();
      if( !process->waitForFinished( 500 ) ) { process->kill(); process->waitForFinished( 1000 ); }
    }
  } );
  auto env = QProcessEnvironment::systemEnvironment(); env.insert( "DBUS_SESSION_BUS_ADDRESS", bus.address() );
  env.insert( "XDG_RUNTIME_DIR", runtime.path() ); env.insert( "QT_QPA_PLATFORM", "offscreen" );
  env.insert( "KWIN_COMPOSE", "Q" ); env.insert( "LIBGL_ALWAYS_SOFTWARE", "1" );
  compositor.setProcessEnvironment( env );
  compositor.start( kwin, { "--virtual", "--no-kactivities", "--no-global-shortcuts", "--socket", "gd-selection-ui" } );
  QVERIFY2( QTest::qWaitFor( [&] { return QFile::exists( runtime.filePath( "gd-selection-ui" ) ) || compositor.state() == QProcess::NotRunning; }, 5000 ), "selection UI compositor startup deadline" );
  QVERIFY2( QFile::exists( runtime.filePath( "gd-selection-ui" ) ), compositor.readAllStandardError().constData() );
  env.insert( "WAYLAND_DISPLAY", "gd-selection-ui" ); env.insert( "QT_QPA_PLATFORM", "wayland" );
  env.insert( "GD_SCAN_POPUP_UI_PROBE", "1" ); child.setProcessEnvironment( env );
  child.start( QCoreApplication::applicationFilePath(), { "scanPopupRowWaylandProbe" } );
  QVERIFY2( QTest::qWaitFor( [&] { return child.state() == QProcess::NotRunning; }, 15000 ), "selected-text Wayland UI child deadline" );
  QByteArray const output = child.readAllStandardOutput() + child.readAllStandardError();
  QCOMPARE( child.exitStatus(), QProcess::NormalExit ); QVERIFY2( child.exitCode() == 0, output.constData() );
  QVERIFY2( output.contains( "scan-popup-wayland-preferences: PASS" ), output.constData() );
}
void TstHotkeys::scanPopupRowWaylandProbe()
{
  if( qEnvironmentVariable( "GD_SCAN_POPUP_UI_PROBE" ) != "1" ) return;
  QVERIFY( QGuiApplication::platformName().startsWith( QLatin1String( "wayland" ) ) );
  Config::Class cfg; cfg.preferences.enableScanPopupHotkey = true;
  cfg.preferences.scanPopupHotkey.rawText = "Ctrl+Meta+D";
  Preferences dialog( nullptr, cfg );
  auto enabled = dialog.findChild<QCheckBox *>( "enableScanPopupHotkey" );
  auto editor = dialog.findChild<HotKeyEdit *>( "scanPopupHotkey" ); QVERIFY( enabled && editor );
  QVERIFY( enabled->isEnabled() && enabled->isChecked() && editor->isEnabled() );
  QCOMPARE( dialog.getPreferences().scanPopupHotkey.rawText, QString( "Ctrl+Meta+D" ) );
  enabled->setChecked( false ); QVERIFY( !dialog.getPreferences().enableScanPopupHotkey );
  QCOMPARE( dialog.getPreferences().scanPopupHotkey.rawText, QString( "Ctrl+Meta+D" ) );
  enabled->setChecked( true ); QVERIFY( dialog.getPreferences().enableScanPopupHotkey );
  QTest::keyPress( editor, Qt::Key_Control ); QTest::keyRelease( editor, Qt::Key_Control );
  QCOMPARE( dialog.getPreferences().scanPopupHotkey.rawText, QString( "Ctrl+Meta+D" ) );
  QTest::keyClick( editor, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier );
  auto edited = dialog.getPreferences(); QVERIFY( edited.scanPopupHotkey.rawText.isEmpty() );
  QCOMPARE( edited.scanPopupHotkey.toKeySequence().toString( QKeySequence::PortableText ), QString( "Ctrl+Alt+J" ) );
  auto const sequence = editor->getHotKey().toKeySequence(); auto const display = editor->text();
  for( SelectionStatus const & snapshot : QList<SelectionStatus>{
         { WaylandSelectionSource::State::Available, {}, false },
         { WaylandSelectionSource::State::Available, {}, true },
         { WaylandSelectionSource::State::Unavailable, "manager removed", true } } )
  {
    dialog.setHotkeyStatus( "wayland", HotkeyBackend::State::Bound, {}, {{"scan-popup", "Portal selected trigger"}}, true, snapshot );
    QCOMPARE( editor->getHotKey().toKeySequence(), sequence ); QCOMPARE( editor->text(), display ); QVERIFY( enabled->isChecked() );
    QVERIFY( dialog.getPreferences().scanPopupHotkey.rawText.isEmpty() );
  }
  enabled->setChecked( false );
  dialog.setHotkeyStatus( "wayland", HotkeyBackend::State::Bound, {}, {}, true, {} );
  QVERIFY( !enabled->isChecked() ); QVERIFY( !editor->isEnabled() );
  dialog.reject(); QVERIFY( cfg.preferences.enableScanPopupHotkey );
  QCOMPARE( cfg.preferences.scanPopupHotkey.rawText, QString( "Ctrl+Meta+D" ) );
  qInfo( "scan-popup-wayland-preferences: PASS" );
}
void TstHotkeys::scanPopupRowBaseStatuses()
{
  using B = HotkeyBackend::State;
  SelectionStatus const snapshot{ WaylandSelectionSource::State::Available, {}, true };
  QString const base = "Hover lookup and focus restoration are unavailable on Wayland. Clipboard-change scanning and explicit clipboard lookup remain available; global shortcuts use the portal.";
  Config::Class cfg; cfg.preferences.enableScanPopupHotkey = true;
  Preferences dialog( nullptr, cfg );
  auto enabled = dialog.findChild<QCheckBox *>( "enableScanPopupHotkey" );
  auto editor = dialog.findChild<HotKeyEdit *>( "scanPopupHotkey" );
  auto label = dialog.findChild<QLabel *>( "scanPopupHotkeyStatus" );
  auto note = dialog.findChild<QLabel *>( "clipboardCapabilityNote" ); QVERIFY( enabled && editor && label && note );
  dialog.setHotkeyStatus( "wayland", B::Bound, {}, {}, true, snapshot ); enabled->setChecked( true );
  dialog.setHotkeyStatus( "wayland", B::Bound, {}, {}, true, snapshot );
  QCOMPARE( label->text(), QString( "Not assigned by the portal — use Configure shortcuts · primary selection supported" ) );
  QCOMPARE( note->text(), base + " Selected-text shortcut: primary selection supported." );
  auto const before = editor->getHotKey().toKeySequence();
  dialog.setHotkeyStatus( "wayland", B::Denied, "permission refused", {{"scan-popup", "stale"}}, false, snapshot );
  QCOMPARE( label->text(), QString( "permission refused · primary selection supported" ) );
  QCOMPARE( editor->getHotKey().toKeySequence(), before ); QVERIFY( enabled->isChecked() );
  enabled->setChecked( false ); dialog.setHotkeyStatus( "wayland", B::Bound, {}, {}, true, snapshot );
  QCOMPARE( label->text(), QString( "Disabled" ) );
  QCOMPARE( note->text(), base + " Selected-text shortcut is disabled." );
  cfg.preferences.scanPopupHotkey.rawText = "Ctrl+G,Ctrl+H,Ctrl+I";
  Preferences invalid( nullptr, cfg );
  auto invalidEnabled = invalid.findChild<QCheckBox *>( "enableScanPopupHotkey" );
  auto invalidLabel = invalid.findChild<QLabel *>( "scanPopupHotkeyStatus" ); QVERIFY( invalidEnabled && invalidLabel );
  invalid.setHotkeyStatus( "wayland", B::Bound, {}, {}, true, snapshot ); invalidEnabled->setChecked( true );
  for( int i = 0; i != 2; ++i )
  {
    invalid.setHotkeyStatus( "wayland", B::Bound, {}, {{"scan-popup", "stale"}}, true, snapshot );
    QCOMPARE( invalidLabel->text(), QString( "Unavailable on Wayland: Only one or two key combinations are supported. · primary selection supported" ) );
    QVERIFY( !invalidLabel->text().contains( "stale" ) ); QVERIFY( invalidEnabled->isChecked() );
  }
}
```

- [ ] **Step 2: Run RED for missing objects and snapshot rendering**

```bash
make tests-makefile
make tests-build
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys selectionStatusRowRendersSnapshots scanPopupRowPlatformMatrix scanPopupRowBaseStatuses
```

Expected: object lookup or exact-label assertions fail because the third row has not been added. The child probe is run after the UI is complete; missing KWin is a failure, not a skipped success.

- [ ] **Step 3: Insert the exact UI and edited-value wiring**

Immediately before `verticalSpacer_13`, after the clipboard editor layout, insert:

```xml
<item>
 <widget class="QCheckBox" name="enableScanPopupHotkey">
  <property name="text"><string>Use the following hotkey to translate the selected text:</string></property>
 </widget>
</item>
<item>
 <layout class="QHBoxLayout" name="horizontalLayout_scanPopupHotkey">
  <item>
   <spacer name="horizontalSpacer_scanPopupHotkey">
    <property name="orientation"><enum>Qt::Horizontal</enum></property>
    <property name="sizeHint" stdset="0"><size><width>40</width><height>20</height></size></property>
   </spacer>
  </item>
  <item>
   <widget class="HotKeyEdit" name="scanPopupHotkey">
    <property name="enabled"><bool>false</bool></property>
   </widget>
  </item>
 </layout>
</item>
```

Immediately after the existing clipboard status label, before Configure, insert:

```xml
<item>
 <widget class="QLabel" name="scanPopupHotkeyStatus">
  <property name="wordWrap"><bool>true</bool></property>
  <property name="text"><string>Selected text shortcut unavailable</string></property>
 </widget>
</item>
```

Add the edited flag and slot in the header:

```cpp
bool scanPopupHotkeyEdited = false;
```

```cpp
void on_enableScanPopupHotkey_toggled( bool checked );
```

After the existing two hotkey event-filter installations in the constructor insert:

```cpp
bool const wayland = QGuiApplication::platformName().startsWith( QLatin1String( "wayland" ) );
ui.enableScanPopupHotkey->setEnabled( wayland );
ui.enableScanPopupHotkey->setChecked( wayland && p.enableScanPopupHotkey );
ui.scanPopupHotkey->setHotKey( p.scanPopupHotkey );
ui.scanPopupHotkey->setEnabled( ui.enableScanPopupHotkey->isEnabled() && ui.enableScanPopupHotkey->isChecked() );
ui.scanPopupHotkey->installEventFilter( this );
```

Inside eventFilter's existing ordinary-key branch, after the two old edited-flag assignments, insert:

```cpp
if( watched == ui.scanPopupHotkey ) scanPopupHotkeyEdited = true;
```

After getPreferences' clipboard assignments insert exactly:

```cpp
bool const wayland = QGuiApplication::platformName().startsWith( QLatin1String( "wayland" ) );
p.enableScanPopupHotkey = wayland ? ui.enableScanPopupHotkey->isChecked()
                                  : cfg.preferences.enableScanPopupHotkey;
p.scanPopupHotkey = wayland
    ? ( scanPopupHotkeyEdited ? ui.scanPopupHotkey->getHotKey() : cfg.preferences.scanPopupHotkey )
    : cfg.preferences.scanPopupHotkey;
```

Beside the existing checkbox slots insert:

```cpp
void Preferences::on_enableScanPopupHotkey_toggled( bool checked )
{
  ui.scanPopupHotkey->setEnabled( ui.enableScanPopupHotkey->isEnabled() && checked );
}
```

- [ ] **Step 4: Replace the complete status renderer with the id-driven algorithm**

Replace the existing setHotkeyStatus definition, including Task 5's temporary unused-parameter statement, with:

```cpp
void Preferences::setHotkeyStatus( QString const & platform, HotkeyBackend::State state,
                                   QString const & explanation,
                                   QVector<PortalBindingDisplay> const & bindings,
                                   bool configureAvailable,
                                   SelectionStatus const & selection )
{
  bool const wayland = platform.startsWith( QLatin1String( "wayland" ) );
  bool const active = state == HotkeyBackend::State::Bound || state == HotkeyBackend::State::Partial;
  QString status;
  switch( state )
  {
    case HotkeyBackend::State::Bound: status = wayland ? tr( "Portal bound" ) : tr( "X11 active" ); break;
    case HotkeyBackend::State::Partial: status = wayland ? tr( "Portal partial" ) : tr( "X11 partial" ); break;
    case HotkeyBackend::State::Denied: status = tr( "Portal denied" ); break;
    case HotkeyBackend::State::Unavailable: status = tr( "Global shortcuts unavailable" ); break;
    case HotkeyBackend::State::Unsupported: status = tr( "Global shortcuts unsupported" ); break;
    case HotkeyBackend::State::Creating: status = tr( "Creating portal session" ); break;
    case HotkeyBackend::State::Binding: status = tr( "Binding global shortcuts" ); break;
    case HotkeyBackend::State::Ready: status = tr( "Portal session ready" ); break;
    case HotkeyBackend::State::Closed: status = tr( "Global shortcuts closed" ); break;
  }
  ui.hotkeyCapability->setText( explanation.isEmpty() ? status : status + ": " + explanation );
  QString suffix;
  switch( selection.state )
  {
    case WaylandSelectionSource::State::Starting: suffix = tr( "selection source starting" ); break;
    case WaylandSelectionSource::State::Available:
      suffix = selection.primarySelectionSeen ? tr( "primary selection supported" ) : tr( "selection not observed yet" ); break;
    case WaylandSelectionSource::State::Unavailable:
      suffix = tr( "selection unavailable: %1 — clipboard is used instead" ).arg( selection.reason ); break;
    case WaylandSelectionSource::State::Unsupported:
      suffix = tr( "selection unsupported in this build — clipboard is used instead" ); break;
  }
  struct Row {
    QString id;
    HotKeyEdit * editor;
    QCheckBox * enabled;
    QLabel * label;
    Config::HotKey const & stored;
    bool const & edited;
  };
  Row const rows[] = {
    { "main-window", ui.mainWindowHotkey, ui.enableMainWindowHotkey,
      ui.mainWindowHotkeyStatus, cfg.preferences.mainWindowHotkey, mainHotkeyEdited },
    { "clipboard-lookup", ui.clipboardHotkey, ui.enableClipboardHotkey,
      ui.clipboardHotkeyStatus, cfg.preferences.clipboardHotkey, clipboardHotkeyEdited },
    { "scan-popup", ui.scanPopupHotkey, ui.enableScanPopupHotkey,
      ui.scanPopupHotkeyStatus, cfg.preferences.scanPopupHotkey, scanPopupHotkeyEdited },
  };
  auto display = [&]( Row const & row ) {
    if( !row.enabled->isChecked() ) { row.label->setText( tr( "Disabled" ) ); return; }
    QString reason, trigger;
    Config::HotKey key = row.editor->getHotKey();
    if( wayland && !row.edited && !row.stored.rawText.isEmpty() && !HotkeySyntax::parse( row.stored.rawText, &key, &reason ) )
    { row.label->setText( tr( "Unavailable on Wayland: %1" ).arg( reason ) ); return; }
    if( wayland && !HotkeySyntax::preferredTrigger( key, &trigger, &reason ) )
    { row.label->setText( tr( "Unavailable on Wayland: %1" ).arg( reason ) ); return; }
    if( active )
      for( auto const & binding : bindings )
        if( binding.id == row.id && !binding.triggerDescription.isEmpty() )
        { row.label->setText( binding.triggerDescription ); return; }
    if( wayland && configureAvailable )
    { row.label->setText( tr( "Not assigned by the portal — use Configure shortcuts" ) ); return; }
    row.label->setText( state == HotkeyBackend::State::Bound && !wayland ? tr( "Active" ) :
                       ( explanation.isEmpty() ? status : explanation ) );
  };
  for( Row const & row : rows )
  {
    row.label->setTextFormat( Qt::PlainText );
    if( row.id == QLatin1String( "scan-popup" ) )
    {
      row.enabled->setEnabled( wayland );
      if( !wayland )
      {
        row.enabled->setChecked( false ); row.editor->setEnabled( false );
        row.label->setText( platform == QLatin1String( "xcb" )
          ? tr( "Not used on X11 — the Held keys setting on the Scan Popup tab applies" )
          : tr( "Not used outside Wayland sessions" ) );
        continue;
      }
      row.editor->setEnabled( row.enabled->isChecked() );
    }
    display( row );
    if( wayland && row.id == QLatin1String( "scan-popup" ) && row.enabled->isChecked() )
      row.label->setText( row.label->text() + QStringLiteral( " · " ) + suffix );
  }
  ui.configureShortcuts->setVisible( wayland );
  ui.configureShortcuts->setEnabled( wayland && configureAvailable );
  QString note = wayland
    ? tr( "Hover lookup and focus restoration are unavailable on Wayland. Clipboard-change scanning and explicit clipboard lookup remain available; global shortcuts use the portal." )
    : tr( "Clipboard-change scanning and explicit clipboard lookup remain available independently of global shortcuts." );
  if( wayland ) note += QLatin1Char( ' ' ) + ( ui.enableScanPopupHotkey->isChecked()
    ? tr( "Selected-text shortcut: %1." ).arg( suffix ) : tr( "Selected-text shortcut is disabled." ) );
  ui.clipboardCapabilityNote->setText( note );
}
```

This retains the old rows' base algorithm including configure-before-explanation behaviour. Only the enabled Wayland third row gains a suffix, after every base outcome. Never reload a Wayland key or checkbox during snapshot refresh.

- [ ] **Step 5: Verify the complete matrix, real Wayland getter and full gate**

```bash
qmake6 goldendict.pro
make -j$(nproc)
make tests-makefile
make tests-build
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys selectionStatusRowRendersSnapshots scanPopupRowPlatformMatrix scanPopupRowBaseStatuses scanPopupRowWaylandValuesAndEdits
QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh make tests-run
QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_hotkeys/tst_hotkeys nonXcbCapability
```

Expected: five snapshot rows, six logical platform rows, edited/raw preservation in the real Wayland child, and the exact child marker. Logical Windows/macOS labels do not establish a native platform release check.

- [ ] **Step 6: Commit the complete row and renderer**

```bash
git add preferences.hh preferences.cc preferences.ui tests/tst_hotkeys/tst_hotkeys.cc
git commit -m "feat(preferences): show selected-text shortcut and capture status"
```

## Task 7: Add the isolated live protocol gate with all thirteen cases

**Implementer tier:** Capable
**Lane:** implementation

**Files:**

- Create: `scripts/verify-wayland-selection-live.sh`
- Test: `scripts/verify-wayland-selection-live.sh` (embedded real-source runner; no extra checked-in helper/project)

**Interfaces:**

- Consumes `WaylandSelectionSource( QObject * = nullptr )`, `~WaylandSelectionSource()`, `void start()`, `void stop()`, `void requestCapture( quint64 )`, `State state() const noexcept`, `QString reason() const`, `bool primarySelectionSeen() const noexcept`, `QString lastText() const`, `bool hasPendingCapture() const noexcept`; signals `void stateChanged( State, QString const & )`, `void capabilityChanged()`, `void captureFinished( quint64, QString const &, qint64 )`; enum `State { Unsupported, Starting, Available, Unavailable }` and its Q_DECLARE_METATYPE.
- Consumes real `qint64 Monotonic::nowMs() noexcept`, `WaylandSelection::MaxPayloadBytes=1048576`, `WaylandSelection::DebounceMs=200`, and `QString WaylandSelection::chooseText( QString const &, qint64, QString const &, qint64 )`. The script links real source and logic, never a substitute.
- Consumes committed `ext-data-control-v1-client-protocol.h` and protocol C from Task 3 and `void gdDebug( char const *, ... )`; exact source log bodies come from the spec §7.4. There is no dependency on MainWindow, ScanPopup, Preferences or the hotkey backend implementation.
- Produces executable no-argument `scripts/verify-wayland-selection-live.sh`; final success is exactly `wayland-selection-live: PASS (13 cases)`, ordinary failure is `wayland-selection-live: FAIL <case>` with exit 1, watchdog expiry exits 124.
- Produces test-only newline-delimited compact JSON: commands have integer `id` and `command` equal to clear/set/release/quit; set has `channel`, `serial`, `mimes`, and per-MIME `schedule` with relative `chunks[{at,data}]` (or `{at,repeat,count}`) and `close` milliseconds (-1 means held). Clear/set ack is `{event:"ack",id}` only after a helper display-sync done callback. Receive events contain serial, literal MIME, fd flags/nonblock, byte count and helper receive time; runner adds its own `_at` receipt stamp and never compares the helper clock to source stamps. Writes/close/errors are metadata events; diagnostics go to stderr.

- [ ] **Step 1: Create the complete script with embedded project/helper/runner**

Write the following entire script. Its Python supervisor owns only its nested compositor/runner process groups; the helper belongs to the runner's group. The runner uses fresh source objects and clears both channels per case. Timing tolerances belong to observation only; successful stamps after an activation or transfer deadline never become acceptable.

```sh
#!/bin/sh
set -eu
[ "$#" -eq 0 ] || { echo 'wayland-selection-live: no arguments accepted' >&2; exit 1; }
for tool in qmake6 cc c++ make pkg-config kwin_wayland dbus-run-session timeout python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "wayland-selection-live: missing prerequisite $tool" >&2; exit 1; }
done
pkg-config --exists wayland-client || { echo 'wayland-selection-live: missing prerequisite wayland-client' >&2; exit 1; }
if [ "${GD_SELECTION_LIVE_SESSION:-0}" != 1 ]; then
    export GD_SELECTION_LIVE_SESSION=1
    exec timeout 120 dbus-run-session -- "$0"
fi
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d /tmp/gd-selection-live.XXXXXXXX)
cleanup() { rm -rf -- "$tmp"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
mkdir -m 700 "$tmp/runtime" "$tmp/config" "$tmp/cache" "$tmp/data"
export XDG_RUNTIME_DIR="$tmp/runtime" XDG_CONFIG_HOME="$tmp/config"
export XDG_CACHE_HOME="$tmp/cache" XDG_DATA_HOME="$tmp/data"
cat > "$tmp/selection-live.pro" <<'GD_PRO'
TEMPLATE = app
TARGET = selection-live
CONFIG += c++17 console link_pkgconfig
QT = core gui testlib
PKGCONFIG += wayland-client
DEFINES += HAVE_WAYLAND_DATA_CONTROL
INCLUDEPATH += $$GD_REPO_ROOT $$GD_REPO_ROOT/protocols/ext-data-control-v1
SOURCES += $$GD_REPO_ROOT/waylandselection.cc \
           $$GD_REPO_ROOT/waylandselectionlogic.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           $$GD_REPO_ROOT/protocols/ext-data-control-v1/ext-data-control-v1-protocol.c \
           selection-live.cc
HEADERS += $$GD_REPO_ROOT/waylandselection.hh \
           $$GD_REPO_ROOT/waylandselectionlogic.hh
GD_PRO
cat > "$tmp/selection-live.cc" <<'GD_CPP'
#include "waylandselection.hh"
#include "waylandselectionlogic.hh"
#include "ext-data-control-v1-client-protocol.h"
#include <wayland-client.h>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QTimer>
#include <QtTest>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <unistd.h>
#include <vector>
using WaylandSelection::MaxPayloadBytes;
static QString const Utf8 = "text/plain;charset=utf-8";
static QString const Plain = "text/plain";
static void jsonOut( QJsonObject const & object )
{
  QByteArray const bytes = QJsonDocument( object ).toJson( QJsonDocument::Compact ) + '\n';
  std::fwrite( bytes.constData(), 1, size_t( bytes.size() ), stdout ); std::fflush( stdout );
}
class Helper : public QObject
{
  struct Source;
  struct Writer { int fd=-1, serial=0; QString mime; qint64 received=0, bytes=0; ~Writer(){ if(fd>=0) ::close(fd); } };
  struct Source { Helper * helper; int serial; ext_data_control_source_v1 * proxy; QJsonObject schedule; };
  struct Ack { int id; };
  wl_display * display=nullptr;
  wl_registry * registry=nullptr;
  ext_data_control_manager_v1 * manager=nullptr;
  wl_seat * seat=nullptr;
  ext_data_control_device_v1 * device=nullptr;
  std::unique_ptr<QSocketNotifier> displayNotifier, stdinNotifier;
  QByteArray commands;
  std::vector<std::unique_ptr<Source>> sources;
  std::vector<std::shared_ptr<Writer>> writers;
  static void global( void * data, wl_registry * r, uint32_t name, char const * interface, uint32_t version )
  {
    auto * h=static_cast<Helper *>(data);
    if(!h->manager && std::strcmp(interface,"ext_data_control_manager_v1")==0)
      h->manager=static_cast<ext_data_control_manager_v1 *>(wl_registry_bind(r,name,&ext_data_control_manager_v1_interface,1));
    else if(!h->seat && std::strcmp(interface,"wl_seat")==0)
    {
      h->seat=static_cast<wl_seat *>(wl_registry_bind(r,name,&wl_seat_interface,std::min(version,2u)));
      static wl_seat_listener const listener{
        [](void*,wl_seat*,uint32_t){}, [](void*,wl_seat*,char const*){} };
      wl_seat_add_listener(h->seat,&listener,h);
    }
  }
  static void removed(void*,wl_registry*,uint32_t) {}
  static void ackDone( void * data, wl_callback * callback, uint32_t )
  {
    std::unique_ptr<Ack> ack(static_cast<Ack *>(data)); wl_callback_destroy(callback);
    jsonOut({{"event","ack"},{"id",ack->id}});
  }
  void closeWriter( std::shared_ptr<Writer> const & w )
  {
    if(w->fd<0) return;
    ::close(w->fd); w->fd=-1; jsonOut({{"event","closed"},{"serial",w->serial},{"mime",w->mime},{"bytes",double(w->bytes)}});
  }
  void writeChunk( std::shared_ptr<Writer> const & w, QByteArray const & bytes )
  {
    if(w->fd<0) return;
    qsizetype sent=0;
    while(sent<bytes.size())
    {
      ssize_t const n=::write(w->fd,bytes.constData()+sent,size_t(bytes.size()-sent));
      if(n>0) { sent+=n; w->bytes+=n; continue; }
      if(n<0 && errno==EINTR) continue;
      int const error=n<0?errno:EIO;
      jsonOut({{"event","write-error"},{"serial",w->serial},{"mime",w->mime},{"errno",error},{"bytes",int(sent)},{"totalBytes",double(w->bytes)}});
      closeWriter(w); return;
    }
    jsonOut({{"event","write"},{"serial",w->serial},{"mime",w->mime},{"bytes",int(sent)},{"totalBytes",double(w->bytes)}});
  }
  static void send( void * data, ext_data_control_source_v1 *, char const * mime, int fd )
  {
    auto * source=static_cast<Source *>(data); auto * h=source->helper;
    auto w=std::make_shared<Writer>(); w->fd=fd; w->serial=source->serial; w->mime=QString::fromUtf8(mime); w->received=Monotonic::nowMs();
    h->writers.push_back(w);
    int const flags=fcntl(fd,F_GETFL);
    jsonOut({{"event","receive"},{"serial",source->serial},{"mime",QString::fromUtf8(mime)},
             {"fdFlags",flags},{"nonblock",bool(flags&O_NONBLOCK)},{"bytes",0},{"receiveTimeMs",double(w->received)}});
    QJsonObject const schedule=source->schedule.value(QString::fromUtf8(mime)).toObject();
    for(auto const & item:schedule.value("chunks").toArray())
    {
      auto const chunk=item.toObject(); QByteArray bytes;
      if(chunk.contains("repeat")) bytes=QByteArray(chunk.value("count").toInt(),chunk.value("repeat").toString().toLatin1().at(0));
      else bytes=chunk.value("data").toString().toUtf8();
      QTimer::singleShot(chunk.value("at").toInt(),h,[h,w,bytes]{h->writeChunk(w,bytes);});
    }
    int const end=schedule.value("close").toInt(-1);
    if(end>=0) QTimer::singleShot(end,h,[h,w]{h->closeWriter(w);});
  }
  static void cancelled( void * data, ext_data_control_source_v1 * proxy )
  {
    auto * source=static_cast<Source *>(data);
    ext_data_control_source_v1_destroy(proxy); source->proxy=nullptr;
    // Existing writer schedules intentionally survive supersession to observe EPIPE.
  }
  bool command( QJsonObject const & c )
  {
    QString const action=c.value("command").toString(); int const id=c.value("id").toInt();
    if(action=="quit") { QCoreApplication::quit(); return true; }
    if(action=="release")
    {
      for(auto const & w:writers) if(w->serial==c.value("serial").toInt()) closeWriter(w);
      jsonOut({{"event","ack"},{"id",id}}); return true;
    }
    if(action!="set" && action!="clear") return false;
    ext_data_control_source_v1 * proxy=nullptr;
    if(action=="set")
    {
      auto value=std::make_unique<Source>(); value->helper=this; value->serial=c.value("serial").toInt();
      value->schedule=c.value("schedule").toObject();
      value->proxy=ext_data_control_manager_v1_create_data_source(manager); proxy=value->proxy;
      if(!proxy) return false;
      static ext_data_control_source_v1_listener const listener{send,cancelled};
      ext_data_control_source_v1_add_listener(proxy,&listener,value.get());
      for(auto const & mime:c.value("mimes").toArray())
        ext_data_control_source_v1_offer(proxy,mime.toString().toUtf8().constData());
      sources.push_back(std::move(value));
    }
    if(c.value("channel").toString()=="primary") ext_data_control_device_v1_set_primary_selection(device,proxy);
    else ext_data_control_device_v1_set_selection(device,proxy);
    wl_callback * sync=wl_display_sync(display); if(!sync) return false;
    static wl_callback_listener const listener{ackDone};
    wl_callback_add_listener(sync,&listener,new Ack{id});
    return wl_display_flush(display)>=0 || errno==EAGAIN;
  }
public:
  ~Helper() override
  {
    for(auto const & w:writers) closeWriter(w);
    if(displayNotifier) displayNotifier->setEnabled(false);
    for(auto const & s:sources) if(s->proxy) ext_data_control_source_v1_destroy(s->proxy);
    if(device) ext_data_control_device_v1_destroy(device);
    if(seat) wl_seat_destroy(seat);
    if(manager) ext_data_control_manager_v1_destroy(manager);
    if(registry) wl_registry_destroy(registry);
    if(display) wl_display_disconnect(display);
  }
  bool init()
  {
    std::signal(SIGPIPE,SIG_IGN);
    display=wl_display_connect(nullptr); if(!display) return false;
    registry=wl_display_get_registry(display); if(!registry) return false;
    static wl_registry_listener const listener{global,removed}; wl_registry_add_listener(registry,&listener,this);
    // Helper-only discovery may block; it runs in its own child and has the outer wall watchdog.
    if(wl_display_roundtrip(display)<0 || !manager || !seat) return false;
    device=ext_data_control_manager_v1_get_data_device(manager,seat); if(!device) return false;
    static ext_data_control_device_v1_listener const deviceListener{
      [](void*,ext_data_control_device_v1*,ext_data_control_offer_v1 * offer){
        static ext_data_control_offer_v1_listener const l{[](void*,ext_data_control_offer_v1*,char const*){}};
        ext_data_control_offer_v1_add_listener(offer,&l,nullptr);
      },
      [](void*,ext_data_control_device_v1*,ext_data_control_offer_v1 * offer){if(offer) ext_data_control_offer_v1_destroy(offer);},
      [](void*,ext_data_control_device_v1*){QCoreApplication::exit(1);},
      [](void*,ext_data_control_device_v1*,ext_data_control_offer_v1 * offer){if(offer) ext_data_control_offer_v1_destroy(offer);}
    };
    ext_data_control_device_v1_add_listener(device,&deviceListener,this);
    displayNotifier=std::make_unique<QSocketNotifier>(wl_display_get_fd(display),QSocketNotifier::Read);
    connect(displayNotifier.get(),&QSocketNotifier::activated,this,[this]{
      if(wl_display_dispatch(display)<0) QCoreApplication::exit(1);
      else if(wl_display_flush(display)<0 && errno!=EAGAIN) QCoreApplication::exit(1);
    });
    int const flags=fcntl(STDIN_FILENO,F_GETFL); if(flags<0 || fcntl(STDIN_FILENO,F_SETFL,flags|O_NONBLOCK)<0) return false;
    stdinNotifier=std::make_unique<QSocketNotifier>(STDIN_FILENO,QSocketNotifier::Read);
    connect(stdinNotifier.get(),&QSocketNotifier::activated,this,[this]{
      char buffer[4096]; ssize_t n=::read(STDIN_FILENO,buffer,sizeof buffer);
      if(n==0) {QCoreApplication::quit();return;}
      if(n<0) {if(errno!=EINTR && errno!=EAGAIN) QCoreApplication::exit(1);return;}
      commands.append(buffer,n);
      for(qsizetype end;(end=commands.indexOf('\n'))>=0;)
      {
        QJsonParseError error; auto const document=QJsonDocument::fromJson(commands.left(end),&error); commands.remove(0,end+1);
        if(error.error!=QJsonParseError::NoError || !document.isObject() || !command(document.object()))
        {std::fprintf(stderr,"helper command/protocol failure\n");QCoreApplication::exit(1);return;}
      }
    });
    if(wl_display_flush(display)<0 && errno!=EAGAIN) return false;
    jsonOut({{"event","ready"}}); return true;
  }
};
#define REQUIRE(x) do { if(!(x)) { std::fprintf(stderr,"assertion line %d: %s\n",__LINE__,#x); return false; } } while(false)
class Live : public QObject
{
public:
  struct Answer { quint64 id; QString text; qint64 stamp, at; };
  QProcess helper;
  QByteArray incoming;
  QList<QJsonObject> events;
  QStringList logs;
  QList<Answer> answers;
  std::unique_ptr<WaylandSelectionSource> source;
  std::unique_ptr<QSignalSpy> stateSpy, capabilitySpy;
  std::function<void(QString const &)> logHook;
  int commandId=0;
  bool wait( std::function<bool()> const & predicate, int ms=5000 )
  {
    qint64 const deadline=Monotonic::nowMs()+ms;
    while(!predicate() && Monotonic::nowMs()<deadline)
    {QCoreApplication::processEvents(QEventLoop::AllEvents,5);QTest::qWait(1);}
    return predicate();
  }
  bool until(qint64 time) {return wait([=]{return Monotonic::nowMs()>=time;},int(std::max<qint64>(1,time-Monotonic::nowMs()+100)));}
  int send(QJsonObject command)
  {
    int const id=++commandId; command.insert("id",id);
    helper.write(QJsonDocument(command).toJson(QJsonDocument::Compact)+'\n');return id;
  }
  bool ack(int id)
  {
    return wait([&]{for(auto const & e:events) if(e.value("event")=="ack" && e.value("id").toInt()==id) return true;return false;});
  }
  bool clear(QString const & channel) {return ack(send({{"command","clear"},{"channel",channel}}));}
  bool set(int serial, QJsonObject const & schedule, QStringList const & mimes={Utf8}, QString const & channel="primary")
  {
    QJsonArray advertised;for(auto const & mime:mimes)advertised.append(mime);
    return ack(send({{"command","set"},{"serial",serial},{"channel",channel},{"mimes",advertised},{"schedule",schedule}}));
  }
  static QJsonObject transfer(QJsonArray const & chunks,int close) {return {{"chunks",chunks},{"close",close}};}
  static QJsonObject text(QString const & value,int close=0) {return {{Utf8,transfer({QJsonObject{{"at",0},{"data",value}}},close)}};}
  QList<QJsonObject> receives(int serial) const
  {QList<QJsonObject> r;for(auto const & e:events)if(e.value("event")=="receive"&&e.value("serial").toInt()==serial)r.append(e);return r;}
  bool event(int serial,QString const & name) const
  {for(auto const & e:events)if(e.value("event")==name&&e.value("serial").toInt()==serial)return true;return false;}
  int captures() const {int n=0;for(auto const & l:logs)if(l.startsWith("selection-captured "))++n;return n;}
  QList<Answer> forId(quint64 id) const {QList<Answer> r;for(auto const & a:answers)if(a.id==id)r.append(a);return r;}
  bool init()
  {
    connect(&helper,&QProcess::readyReadStandardOutput,this,[this]{
      incoming+=helper.readAllStandardOutput();
      for(qsizetype end;(end=incoming.indexOf('\n'))>=0;)
      {
        QJsonParseError error;auto doc=QJsonDocument::fromJson(incoming.left(end),&error);incoming.remove(0,end+1);
        if(error.error!=QJsonParseError::NoError||!doc.isObject()) {QCoreApplication::exit(1);return;}
        auto e=doc.object();e.insert("_at",double(Monotonic::nowMs()));events.append(e);
      }
    });
    helper.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    helper.start(QCoreApplication::applicationFilePath(),{"--helper"});
    return wait([&]{for(auto const & e:events)if(e.value("event")=="ready")return true;return false;});
  }
  ~Live() override
  {
    source.reset();
    if(helper.state()!=QProcess::NotRunning)
    {
      send({{"command","quit"}});
      if(!helper.waitForFinished(500)){helper.kill();helper.waitForFinished(1000);}
    }
  }
  bool reset(bool start=true)
  {
    source.reset();stateSpy.reset();capabilitySpy.reset();logHook={};
    REQUIRE(clear("primary")); REQUIRE(clear("clipboard"));
    events.clear();logs.clear();answers.clear();
    source=std::make_unique<WaylandSelectionSource>();
    stateSpy=std::make_unique<QSignalSpy>(source.get(),&WaylandSelectionSource::stateChanged);
    capabilitySpy=std::make_unique<QSignalSpy>(source.get(),&WaylandSelectionSource::capabilityChanged);
    connect(source.get(),&WaylandSelectionSource::captureFinished,this,[this](quint64 id,QString const & text,qint64 stamp){answers.append({id,text,stamp,Monotonic::nowMs()});});
    if(start)
    {
      source->start();source->start();
      REQUIRE(wait([&]{return source->state()==WaylandSelectionSource::State::Available && source->primarySelectionSeen();}));
    }
    return true;
  }
  bool sourceBecomesAvailable()
  {
    REQUIRE(reset()); REQUIRE(source->reason().isEmpty());REQUIRE(source->primarySelectionSeen());
    REQUIRE(logs.count("selection-source: state=Starting reason=")==1);
    REQUIRE(logs.count("selection-source: state=Available reason=")==1);
    REQUIRE(logs.count("selection-source: primary-selection-seen=1")==1);
    REQUIRE(logs.indexOf("selection-source: state=Starting reason=")<logs.indexOf("selection-source: state=Available reason="));
    REQUIRE(stateSpy->isValid()&&capabilitySpy->isValid());
    int available=0;for(auto const & s:*stateSpy)if(qvariant_cast<WaylandSelectionSource::State>(s[0])==WaylandSelectionSource::State::Available)++available;
    REQUIRE(available==1);return true;
  }
  bool multiChunkCompletesAtEof()
  {
    REQUIRE(reset());
    REQUIRE(set(2,{{Utf8,transfer({QJsonObject{{"at",0},{"data","first-"}},QJsonObject{{"at",40},{"data","second-"}},QJsonObject{{"at",80},{"data","third"}}},120)}}));
    REQUIRE(wait([&]{return receives(2).size()==1;}));qint64 const t=qint64(receives(2)[0].value("_at").toDouble());
    REQUIRE(!receives(2)[0].value("nonblock").toBool());
    REQUIRE(until(t+60));REQUIRE(source->lastText().isEmpty()&&captures()==0);
    REQUIRE(until(t+100));REQUIRE(source->lastText().isEmpty()&&captures()==0);
    REQUIRE(wait([&]{return source->lastText()=="first-second-third";}));
    source->requestCapture(1);REQUIRE(forId(1).size()==1);
    REQUIRE(forId(1)[0].text=="first-second-third"&&forId(1)[0].stamp>0);
    REQUIRE(captures()==1);REQUIRE(logs.contains("selection-captured mime=text/plain;charset=utf-8 bytes=18 truncated=0"));return true;
  }
  bool tenSelectionsDebounceToOneRead()
  {
    REQUIRE(reset());
    for(int i=0;i<10;++i)
    {REQUIRE(set(100+i,text("burst-"+QString::number(i))));REQUIRE(until(Monotonic::nowMs()+10));}
    REQUIRE(wait([&]{return source->lastText()=="burst-9"&&!source->hasPendingCapture();}));
    for(int i=0;i<9;++i)REQUIRE(receives(100+i).isEmpty());
    REQUIRE(receives(109).size()==1);source->requestCapture(1);REQUIRE(forId(1).size()==1);
    auto const a=forId(1)[0];REQUIRE(a.text=="burst-9"&&a.stamp>0);
    REQUIRE(qint64(receives(109)[0].value("_at").toDouble())-a.stamp>=200);return true;
  }
  bool overCapIsTruncated()
  {
    REQUIRE(reset());REQUIRE(set(4,{{Utf8,transfer({QJsonObject{{"at",0},{"repeat","x"},{"count",MaxPayloadBytes+8192}}},0)}}));
    REQUIRE(wait([&]{return source->lastText().size()==MaxPayloadBytes;}));
    REQUIRE(source->lastText()==QString(MaxPayloadBytes,'x'));
    REQUIRE(logs.contains("selection-captured mime=text/plain;charset=utf-8 bytes=1048576 truncated=1"));return true;
  }
  bool exactCapIsNotTruncated()
  {
    REQUIRE(reset());REQUIRE(set(5,{{Utf8,transfer({QJsonObject{{"at",0},{"repeat","y"},{"count",MaxPayloadBytes}}},0)}}));
    REQUIRE(wait([&]{return source->lastText().size()==MaxPayloadBytes;}));
    REQUIRE(source->lastText()==QString(MaxPayloadBytes,'y'));
    REQUIRE(logs.contains("selection-captured mime=text/plain;charset=utf-8 bytes=1048576 truncated=0"));return true;
  }
  bool supersededCaptureCannotPublish()
  {
    REQUIRE(reset());REQUIRE(set(61,{{Utf8,transfer({QJsonObject{{"at",0},{"data","old"}},QJsonObject{{"at",350},{"data","-late"}}},400)}}));
    REQUIRE(wait([&]{return source->hasPendingCapture();}));source->requestCapture(1);
    REQUIRE(wait([&]{return receives(61).size()==1&&event(61,"write");}));
    REQUIRE(set(62,text("new-value")));REQUIRE(wait([&]{return source->lastText()=="new-value";}));
    REQUIRE(until(Monotonic::nowMs()+450));
    REQUIRE(source->lastText()=="new-value");REQUIRE(source->state()==WaylandSelectionSource::State::Available);
    REQUIRE(captures()==1);REQUIRE(event(61,"write-error")||event(61,"closed"));
    source->requestCapture(2);REQUIRE(forId(2).size()==1&&forId(2)[0].text=="new-value"&&forId(2)[0].stamp>0);
    REQUIRE(forId(1).size()==1&&forId(1)[0].text=="new-value");return true;
  }
  bool receiptStampLetsNewCopyWin()
  {
    REQUIRE(reset(false));REQUIRE(set(71,text("selected-before-copy")));
    qint64 copyStamp=0;int copyAck=0;bool beforeReceive=false;
    auto clearHook=qScopeGuard([&]{logHook={};});
    logHook=[&](QString const & line){if(line=="selection-source: primary-selection-seen=1")QTimer::singleShot(10,this,[&]{
      copyStamp=Monotonic::nowMs();beforeReceive=receives(71).isEmpty();
      QJsonArray mimes{Utf8};copyAck=send({{"command","set"},{"channel","clipboard"},{"serial",72},{"mimes",mimes},{"schedule",text("copied-after-selection")}});
    });};
    source->start();
    REQUIRE(wait([&]{return copyStamp>0&&source->lastText()=="selected-before-copy"&&!source->hasPendingCapture();}));
    REQUIRE(copyAck>0&&ack(copyAck));REQUIRE(beforeReceive);REQUIRE(receives(71).size()==1&&receives(72).isEmpty());
    source->requestCapture(1);REQUIRE(forId(1).size()==1);auto const a=forId(1)[0];
    REQUIRE(a.stamp>0&&a.stamp<copyStamp);
    REQUIRE(copyStamp<qint64(receives(71)[0].value("_at").toDouble()));
    REQUIRE(WaylandSelection::chooseText(a.text,a.stamp,"copied-after-selection",copyStamp)=="copied-after-selection");return true;
  }
  bool zeroByteIdleRetriesNextMime()
  {
    REQUIRE(reset());REQUIRE(set(8,{{Utf8,transfer({},-1)},{Plain,transfer({QJsonObject{{"at",0},{"data","retry-success"}}},0)}},{Utf8,Plain}));
    REQUIRE(wait([&]{return source->lastText()=="retry-success"&&!source->hasPendingCapture();}));
    auto const r=receives(8);REQUIRE(r.size()==2&&r[0].value("mime")==Utf8&&r[1].value("mime")==Plain);
    REQUIRE(logs.contains("selection-captured mime=text/plain bytes=13 truncated=0"));
    source->requestCapture(1);REQUIRE(forId(1).size()==1&&forId(1)[0].text=="retry-success");return true;
  }
  bool partialIdleRetiresWithoutRetry()
  {
    REQUIRE(reset());REQUIRE(set(901,text("old-cache")));REQUIRE(wait([&]{return source->lastText()=="old-cache"&&!source->hasPendingCapture();}));
    REQUIRE(set(9,{{Utf8,transfer({QJsonObject{{"at",0},{"data","incomplete"}}},-1)},{Plain,transfer({QJsonObject{{"at",0},{"data","must-not-read"}}},0)}},{Utf8,Plain}));
    REQUIRE(wait([&]{return source->hasPendingCapture();}));source->requestCapture(1);
    REQUIRE(wait([&]{return receives(9).size()==1;}));qint64 const t=qint64(receives(9)[0].value("_at").toDouble());
    REQUIRE(wait([&]{return !source->hasPendingCapture();},300));
    REQUIRE(source->lastText()=="old-cache"&&forId(1).size()==1&&forId(1)[0].stamp==0&&forId(1)[0].text.isEmpty());
    REQUIRE(until(t+600));REQUIRE(receives(9).size()==1);REQUIRE(logs.contains("selection-ignored reason=timeout"));return true;
  }
  bool absoluteDeadlineSurvivesProgress()
  {
    REQUIRE(reset());QJsonArray chunks;for(int i=0;i<35;++i)chunks.append(QJsonObject{{"at",i*100},{"data","x"}});
    REQUIRE(set(10,{{Utf8,transfer({},-1)},{Plain,transfer(chunks,-1)}},{Utf8,Plain}));
    REQUIRE(wait([&]{return receives(10).size()>=1;}));qint64 const first=qint64(receives(10)[0].value("_at").toDouble());
    REQUIRE(wait([&]{return receives(10).size()==2;}));REQUIRE(receives(10)[1].value("mime")==Plain);
    REQUIRE(until(first+1800));REQUIRE(source->hasPendingCapture());
    REQUIRE(wait([&]{return !source->hasPendingCapture();},int(std::max<qint64>(1,first+2050-Monotonic::nowMs()))));
    REQUIRE(Monotonic::nowMs()<=first+2050);REQUIRE(source->lastText().isEmpty()&&captures()==0);
    REQUIRE(logs.contains("selection-ignored reason=timeout"));
    REQUIRE(wait([&]{return event(10,"write-error");},200));
    bool ep = false;
    for( auto const & e : events ) if( e.value("event")=="write-error" && e.value("serial").toInt()==10 && e.value("errno").toInt()==EPIPE ) ep = true;
    REQUIRE(ep);return true;
  }
  bool overlappingRequestsReuseTransfer()
  {
    REQUIRE(reset());REQUIRE(set(11,{{Utf8,transfer({QJsonObject{{"at",0},{"data","over-"}},QJsonObject{{"at",150},{"data","lap-"}},QJsonObject{{"at",300},{"data","result"}}},350)}}));
    REQUIRE(wait([&]{return source->hasPendingCapture();}));qint64 const first=Monotonic::nowMs();source->requestCapture(101);
    REQUIRE(wait([&]{return receives(11).size()==1;}));qint64 const receive=qint64(receives(11)[0].value("_at").toDouble());
    REQUIRE(std::abs(receive-first)<=10);
    qint64 second=0;
    QTimer::singleShot(int(std::max<qint64>(0,first+100-Monotonic::nowMs())),this,[&]{second=Monotonic::nowMs();source->requestCapture(102);});
    REQUIRE(wait([&]{return second>0&&forId(101).size()==1;}));
    REQUIRE(forId(101)[0].text.isEmpty()&&forId(101)[0].stamp==0);
    REQUIRE(forId(101)[0].at-second<=50);
    REQUIRE(wait([&]{return forId(102).size()==1;}));auto const a=forId(102)[0];
    REQUIRE(a.text=="over-lap-result"&&a.stamp>0);REQUIRE(a.at-second>=240&&a.at-second<300);
    REQUIRE(receives(11).size()==1);REQUIRE(until(first+500));REQUIRE(forId(101).size()==1&&forId(102).size()==1);return true;
  }
  bool activationDeadlineSuppressesCache()
  {
    REQUIRE(reset());REQUIRE(set(121,text("old-cache")));REQUIRE(wait([&]{return source->lastText()=="old-cache"&&!source->hasPendingCapture();}));
    REQUIRE(set(122,{{Utf8,transfer({QJsonObject{{"at",0},{"data","new-"}},QJsonObject{{"at",100},{"data","pend"}},QJsonObject{{"at",200},{"data","ing"}}},400)}}));
    REQUIRE(wait([&]{return source->hasPendingCapture();}));qint64 const called=Monotonic::nowMs();source->requestCapture(201);
    REQUIRE(wait([&]{return forId(201).size()==1;},350));auto const a=forId(201)[0];
    REQUIRE(a.at-called<=350&&a.text.isEmpty()&&a.stamp==0);REQUIRE(logs.contains("selection-timeout"));
    REQUIRE(WaylandSelection::chooseText(a.text,a.stamp,"fallback",1)=="fallback");
    REQUIRE(wait([&]{return source->lastText()=="new-pending"&&!source->hasPendingCapture();}));
    REQUIRE(forId(201).size()==1);return true;
  }
  bool stopSuppressesLateCompletion()
  {
    REQUIRE(reset());REQUIRE(set(13,text("held",-1)));REQUIRE(wait([&]{return source->hasPendingCapture();}));source->requestCapture(301);
    REQUIRE(wait([&]{return receives(13).size()==1&&event(13,"write");}));
    int const captured=captures();auto const frozen=source->state();QString const reason=source->reason();
    source->stop();source->stop();REQUIRE(!source->hasPendingCapture());
    REQUIRE(ack(send({{"command","release"},{"serial",13}})));REQUIRE(until(Monotonic::nowMs()+400));
    REQUIRE(forId(301).isEmpty()&&captures()==captured);source->start();source->requestCapture(302);
    REQUIRE(until(Monotonic::nowMs()+50));REQUIRE(forId(302).isEmpty());REQUIRE(receives(13).size()==1);
    REQUIRE(source->state()==frozen&&source->reason()==reason);return true;
  }
};
static Live * observed=nullptr;
static void message(QtMsgType,QMessageLogContext const &,QString const & text)
{
  if(observed&&text.startsWith("selection-"))
  {observed->logs.append(text);if(observed->logHook)observed->logHook(text);}
  QByteArray const bytes=text.toUtf8();std::fprintf(stderr,"%s\n",bytes.constData());
}
int main(int argc,char **argv)
{
  if(argc>1&&std::strcmp(argv[1],"--helper")==0)
  {
    QCoreApplication app(argc,argv);Helper helper;
    if(!helper.init()){std::fprintf(stderr,"helper: missing ext protocol or connection failure\n");return 1;}
    return app.exec();
  }
  QGuiApplication app(argc,argv);qRegisterMetaType<WaylandSelectionSource::State>();
  Live runner;observed=&runner;auto const previous=qInstallMessageHandler(message);
  auto restore=qScopeGuard([&]{observed=nullptr;qInstallMessageHandler(previous);});
  struct Case {char const * name;bool(Live::*run)();};
  Case const cases[]={
    {"sourceBecomesAvailable",&Live::sourceBecomesAvailable},
    {"multiChunkCompletesAtEof",&Live::multiChunkCompletesAtEof},
    {"tenSelectionsDebounceToOneRead",&Live::tenSelectionsDebounceToOneRead},
    {"overCapIsTruncated",&Live::overCapIsTruncated},
    {"exactCapIsNotTruncated",&Live::exactCapIsNotTruncated},
    {"supersededCaptureCannotPublish",&Live::supersededCaptureCannotPublish},
    {"receiptStampLetsNewCopyWin",&Live::receiptStampLetsNewCopyWin},
    {"zeroByteIdleRetriesNextMime",&Live::zeroByteIdleRetriesNextMime},
    {"partialIdleRetiresWithoutRetry",&Live::partialIdleRetiresWithoutRetry},
    {"absoluteDeadlineSurvivesProgress",&Live::absoluteDeadlineSurvivesProgress},
    {"overlappingRequestsReuseTransfer",&Live::overlappingRequestsReuseTransfer},
    {"activationDeadlineSuppressesCache",&Live::activationDeadlineSuppressesCache},
    {"stopSuppressesLateCompletion",&Live::stopSuppressesLateCompletion}
  };
  if(!runner.init()){std::fprintf(stderr,"wayland-selection-live: FAIL sourceBecomesAvailable\n");return 1;}
  for(auto const & test:cases)
  {
    if(!(runner.*test.run)()){std::fprintf(stderr,"wayland-selection-live: FAIL %s\n",test.name);return 1;}
    std::printf("%s: PASS\n",test.name);std::fflush(stdout);
  }
  std::puts("wayland-selection-live: PASS (13 cases)");return 0;
}
GD_CPP
if ! (cd "$tmp" && qmake6 selection-live.pro "GD_REPO_ROOT=$root" && make -j2) > "$tmp/build.log" 2>&1; then
    tail -n 60 "$tmp/build.log" >&2
    echo 'wayland-selection-live: FAIL build' >&2
    exit 1
fi
python3 - "$tmp" <<'GD_PY'
import os, pathlib, signal, subprocess, sys, time
root = pathlib.Path(sys.argv[1])
children = []
def interrupted(signum, frame):
    raise SystemExit(128 + signum)
for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(sig, interrupted)
def stop(process):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            print('wayland-selection-live: child did not reap within cleanup bound', file=sys.stderr)
    except ProcessLookupError:
        pass
try:
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', KWIN_COMPOSE='Q', LIBGL_ALWAYS_SOFTWARE='1')
    with (root / 'kwin.log').open('wb') as log:
        compositor = subprocess.Popen(['kwin_wayland', '--virtual', '--no-kactivities', '--no-global-shortcuts', '--socket', 'gd-selection-live'], env=env, stdout=log, stderr=log, start_new_session=True)
        children.append(compositor)
        deadline = time.monotonic() + 5
        socket = root / 'runtime/gd-selection-live'
        while not socket.exists() and compositor.poll() is None and time.monotonic() < deadline:
            time.sleep(0.01)
        if not socket.exists():
            print((root / 'kwin.log').read_text(errors='replace'), file=sys.stderr)
            print('wayland-selection-live: FAIL compositor-startup', file=sys.stderr)
            raise SystemExit(1)
        env.update(QT_QPA_PLATFORM='wayland', WAYLAND_DISPLAY='gd-selection-live')
        runner = subprocess.Popen(['timeout', '60', str(root / 'selection-live')], env=env, start_new_session=True)
        children.append(runner)
        try:
            status = runner.wait(timeout=62)
        except subprocess.TimeoutExpired:
            status = 124
        raise SystemExit(status if status >= 0 else 128 - status)
finally:
    for process in reversed(children):
        stop(process)
GD_PY
```

- [ ] **Step 2: Make the script executable, syntax-check and run the live gate**

```bash
chmod +x scripts/verify-wayland-selection-live.sh
sh -n scripts/verify-wayland-selection-live.sh
scripts/verify-wayland-selection-live.sh
```

Expected: exactly the 13 named case PASS lines, exact final marker and exit 0 on a KWin host exposing the ext manager. A missing manager/tool, helper start error, failed assertion or build error fails nonzero; the runner watchdog is independent of GUI progress. The script creates no source helper/project outside its temporary directory and does not require the scanner.

- [ ] **Step 3: Falsify the live oracles in isolated copies**

For each row below create a separate `/tmp` copy of the current tracked source plus this new script using `git ls-files` and copy the untracked script explicitly if it has not yet been staged. Exclude generated Makefiles and build directories. Make exactly the replacement shown using a Python `str.replace` guarded by `assert text.count(old) == 1`; run the same script there, require exit 1 and the named failed case, then discard the copy. Before mutations run the copied unmodified script once and require PASS; a build or prerequisite failure is never a mutation kill.

| Mutant | Exact replacement in `waylandselection.cc` | Required failing case |
| --- | --- | --- |
| Delete absolute expiry | `if( now >= transferDeadline ) { dropCapture( true ); return; }` → `if( false ) { dropCapture( true ); return; }` | `absoluteDeadlineSurvivesProgress` |
| Replenish at each MIME | Insert `transferDeadline = Monotonic::nowMs() + TransferTimeoutMs;` immediately after the unique `bool startMimeAttempt()` opening brace | `absoluteDeadlineSurvivesProgress` |
| Complete short reads | `if( count > 0 )` → `if( count > 0 )` plus insert `if( count < ssize_t(sizeof buffer) ) { complete( token ); return; }` after the unique `bytesObserved += int( count ); lastProgressAt = Monotonic::nowMs();` | `multiChunkCompletesAtEof` |
| Make writer nonblocking | `pipe2( fds, O_CLOEXEC )` → `pipe2( fds, O_CLOEXEC | O_NONBLOCK )` | `multiChunkCompletesAtEof` (fd flag assertion) |
| Restart on overlap | At the start of the unique `if( superseded )` body insert `retirePrimary();` | `overlappingRequestsReuseTransfer` |
| Stamp at completion | `cacheStamp = cache.isEmpty() ? 0 : offerStamp;` → `cacheStamp = cache.isEmpty() ? 0 : Monotonic::nowMs();` | `receiptStampLetsNewCopyWin` |

For the overlap mutant, run the named case alone by the copy-only array narrowing in the command below; its identical unmutated selection must pass first. This prevents an earlier supersession case from masking the named overlap oracle. Preserve the active worktree throughout. Run this complete command, whose guarded replacements implement the table exactly:

```bash
python3 - <<'PY'
import pathlib, shutil, subprocess, tempfile
root = pathlib.Path.cwd()
files = [p for p in subprocess.check_output(['git', 'ls-files', '-z']).decode().split('\0') if p]
script_name = 'scripts/verify-wayland-selection-live.sh'
if script_name not in files:
    files.append(script_name)
original_source = (root / 'waylandselection.cc').read_text()
original_script = (root / script_name).read_text()
mutants = [
    ('absolute', 'if( now >= transferDeadline ) { dropCapture( true ); return; }', 'if( false ) { dropCapture( true ); return; }', 'absoluteDeadlineSurvivesProgress'),
    ('reset-clock', 'bool startMimeAttempt()\n  {', 'bool startMimeAttempt()\n  {\n    transferDeadline = Monotonic::nowMs() + TransferTimeoutMs;', 'absoluteDeadlineSurvivesProgress'),
    ('short-read', 'bytesObserved += int( count ); lastProgressAt = Monotonic::nowMs();', 'bytesObserved += int( count ); lastProgressAt = Monotonic::nowMs();\n        if( count < ssize_t(sizeof buffer) ) { complete( token ); return; }', 'multiChunkCompletesAtEof'),
    ('writer-flags', 'pipe2( fds, O_CLOEXEC )', 'pipe2( fds, O_CLOEXEC | O_NONBLOCK )', 'multiChunkCompletesAtEof'),
    ('overlap', 'if( superseded )\n    {', 'if( superseded )\n    {\n      retirePrimary();', 'overlappingRequestsReuseTransfer'),
    ('completion-stamp', 'cacheStamp = cache.isEmpty() ? 0 : offerStamp;', 'cacheStamp = cache.isEmpty() ? 0 : Monotonic::nowMs();', 'receiptStampLetsNewCopyWin'),
]
for name, old, new, killer in mutants:
    assert original_source.count(old) == 1, (name, 'source anchor')
    with tempfile.TemporaryDirectory(prefix=f'gd-selection-mutant-{name}.', dir='/tmp') as temporary:
        copy = pathlib.Path(temporary)
        for relative in files:
            src = root / relative
            if not src.is_file() and not src.is_symlink():
                continue
            dst = copy / relative
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst, follow_symlinks=False)
        script = original_script
        if name == 'overlap':
            start = script.index('  Case const cases[]={\n')
            end = script.index('\n  };\n  if(!runner.init())', start)
            script = script[:start] + '  Case const cases[]={\n    {"overlappingRequestsReuseTransfer",&Live::overlappingRequestsReuseTransfer}' + script[end:]
            (copy / script_name).write_text(script)
        baseline = subprocess.run([str(copy / script_name)], cwd=copy, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert baseline.returncode == 0, (name, 'baseline', baseline.stdout[-5000:])
        (copy / 'waylandselection.cc').write_text(original_source.replace(old, new))
        result = subprocess.run([str(copy / script_name)], cwd=copy, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        expected = 'wayland-selection-live: FAIL ' + killer
        assert result.returncode == 1 and expected in result.stdout, (name, result.returncode, result.stdout[-5000:])
        print(f'{name}: baseline PASS; killed by {killer}')
print('wayland-selection live mutations: PASS')
PY
```

- [ ] **Step 4: Run the normal gate and commit the real-source live harness**

```bash
qmake6 goldendict.pro
make tests-makefile
make tests-build
QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh make tests-run
git add scripts/verify-wayland-selection-live.sh
git commit -m "test(selection): verify thirteen real Wayland transfer cases"
```

## Task 8: Document coverage and run the complete acceptance contract

**Implementer tier:** Advanced
**Lane:** implementation

**Files:**

- Modify: `README.md:227-230`
- Modify: `docs/wayland-limitations.md:30-42`
- Test: `tests/tst_waylandselection/tst_waylandselection.cc`
- Test: `tests/tst_hotkeys/tst_hotkeys.cc`
- Test: `scripts/verify-wayland-selection-live.sh`
- Test: `protocols/ext-data-control-v1/regenerate.sh`
- Test: `goldendict.pro`
- Test: `tests/tests.pro`

**Interfaces:**

- Consumes no new production API. Consumes CLI `qmake6 goldendict.pro [CONFIG+=verbose_build_output]`, root targets `make tests-makefile`, `make tests-build`, `make tests-run`, `GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh`, and `QMAKE_PKG_CONFIG=<absolute wrapper path>` as a qmake assignment for the no-capture copy.
- Consumes `scripts/verify-wayland-selection-live.sh` with no arguments, exit 0 plus exactly `wayland-selection-live: PASS (13 cases)`; scanner entry point `protocols/ext-data-control-v1/regenerate.sh` pinned to 1.26.0 and XML hash `293deba5dfdb974fbcf8dcf891a30a51a36a8bba0047dc6bdf6a03194089b7fb`.
- Consumes the exact 14 pure behavioural slot names and exact 20 hotkeys function names listed in the verification code below; no additional `_data` function or fixture file is allowed.
- Consumes observable manual contract: preference `enableScanPopupHotkey`/`scanPopupHotkey`, portal action `scan-popup`, default `Meta+Ctrl+D`, source suffixes and notification message `No selected text or clipboard text to translate` (3000 ms). Source assertions and the real protocol script do not replace these application-level observations.
- Produces updated ext-only coverage/fallback prose and an acceptance report through the controller's report path, with actual command/status/evidence and explicitly unobserved manual cases. No extra checked-in report, fixture, helper or project file is introduced.

- [ ] **Step 1: Replace the README limit text and extend only canonical §2**

In the existing Portal shortcuts bullet retain the first two sentences about one combination and untouched stored multi-key sequences. Replace its remaining clipboard/hover sentences with this exact text:

```text
Selected-text translation is available through a global shortcut on
compositors exposing `ext_data_control_manager_v1`, with clipboard fallback
when primary-selection capture is unavailable. Clipboard-change scanning and
explicit clipboard lookup remain separate behaviours. Hover translation and
X11 focus restoration remain X11-only.
```

In §2 of docs/wayland-limitations.md retain the existing portal description paragraphs and replace its final clipboard/hover paragraph with the following complete addition. Leave every historical evidence table, date, count and unobserved interaction entry unchanged:

```markdown
Selected-text translation is available through a global shortcut on compositors
exposing `ext_data_control_manager_v1`, with clipboard fallback when primary-selection
capture is unavailable. The shortcut is opt-in and defaults off. Binding the shortcut
and reading the selection are separate capabilities: a build without libwayland-client
still registers the enabled Wayland action and uses the clipboard.

Clipboard-change scanning, explicit clipboard lookup and the selected-text action
remain separate behaviours. A selection alone does not open a new shortcut-driven
popup. Automatic selection popups and held-modifier emulation on Wayland are outside
this change. Hover translation and X11 focus restoration remain X11-only.

Coverage is limited to the ext protocol. wlroots compositors (sway, hyprland and labwc)
expose `zwlr_data_control_v1`; support is deferred. Its generated pair and bind attempt
can later fit behind the same source interface without changing the pure selection
logic. GNOME/Mutter exposes neither data-control protocol, so this feature cannot read
its primary selection in the background. A future GNOME-side protocol or the
RemoteDesktop Clipboard portal with remote-control consent would be needed; the
shortcut remains registered and falls back to the clipboard. Only the first seat is
watched, with no simultaneous multi-seat aggregation. Data-control has no owner
identity: selections in GoldenDict's own article and in other applications are both
eligible.

Binding the manager does not prove primary-selection support. Available with no
primary event means `selection not observed yet`; even a null primary event latches
the capability. Cached text is intentionally sticky across null selections. A promoted
activation that reaches its 300 ms deadline suppresses that cache for that request;
it does not turn old text into a fresh capture. The deadline is a source policy under
event-loop progress, without a promise through OS suspension or unrelated GUI-thread
blocking.

The source uses a private connection. A protocol failure ends that watcher while the
rest of the application may continue. A compositor restart also kills Qt's own
Wayland connection and Qt terminates the process; application survival or reconnection
is not promised. An unavailable source stays terminal until the preference is turned
off and on. Materialising a native handle and applying an activation token requests
activation; the compositor can decline focus.

An empty activation attempts the main-window status bar when visible, otherwise a
GoldenDict tray notification if the tray object exists. A hidden main window without
a tray object has no visible notification channel and only logs `selection-empty`.
Desktop notification policy can also suppress a requested tray message.

The logic suite proves pure decisions without exercising a Wayland session. The
hotkeys suite links Config, Preferences and the backend, but not MainWindow, ScanPopup
or the selection source; its source assertions pin wiring without proving runtime
effects. The separate live script links the real source and tests its stated protocol
cases, including receipt stamps, MIME policy and overlapping requests. Flush pressure,
registry removal and error branches that the live harness cannot force remain
explicitly labelled source assertions. Popup opening, the visible notification ladder
and focus outcomes require a person's application session; no successful interaction
receipt is inferred from a source assertion or protocol-runner PASS.
```

- [ ] **Step 2: Verify exact test inventories and protocol regeneration**

Run this source-based inventory check; it checks both declarations and definitions without adding a repository file:

```bash
python3 - <<'PY'
import pathlib, re
pure = '''mimePreferenceOrder payloadClampIsAppliedWhileReading decodeReplacesInvalidUtf8 decodeStripsNul chooseTextUsesRecency clipboardStampIgnoresNonClipboardModes mimeAttemptsAdvanceAndExhaust payloadAtCapIsNotTruncated payloadOverCapIsTruncated offerSupersessionRetiresToken clipboardOfferIsDiscarded retiredOfferIsNeverReused nullOfferClearsWithoutRead activationPlanPromotesPendingCapture'''.split()
hotkeys = '''scanPopupBindingIsPortalOnly portalIdPredicateAcceptsThreeIds capabilityConstantMatchesBuild sourceFollowsThePreference defaultScanPopupHotkeyIsBindable threeBindingsStayBound scanPopupPortalBindingSurvivesSaveLoad scanPopupHotkeyKeepsVerbatimText legacyConfigDefaultsScanPopupHotkeyOff scanPopupConfigBooleanValues scanPopupActivationWiringIsPinned xcbGuardDoesNotCreateThePopup scanPopupSourceLoopContractIsPinned selectionStatusRowRendersSnapshots_data selectionStatusRowRendersSnapshots scanPopupRowPlatformMatrix_data scanPopupRowPlatformMatrix scanPopupRowWaylandValuesAndEdits scanPopupRowWaylandProbe scanPopupRowBaseStatuses'''.split()
for path, cls, names in [
    ('tests/tst_waylandselection/tst_waylandselection.cc', 'TstWaylandSelection', ['initTestCase', 'cleanupTestCase'] + pure),
    ('tests/tst_hotkeys/tst_hotkeys.cc', 'TstHotkeys', hotkeys),
]:
    text = pathlib.Path(path).read_text()
    for name in names:
        assert len(re.findall(r'\bvoid\s+' + re.escape(name) + r'\s*\(\s*\)\s*;', text)) == 1, (path, name, 'declaration')
        assert len(re.findall(r'\bvoid\s+' + cls + r'::' + re.escape(name) + r'\s*\(\s*\)\s*\{', text)) == 1, (path, name, 'definition')
    if cls == 'TstWaylandSelection':
        actual = re.findall(r'\bvoid\s+' + cls + r'::(\w+)\s*\(\s*\)\s*\{', text)
        assert set(actual) == set(names) and len(actual) == 16, actual
        assert not re.search(r'\b\w+_data\s*\(', text)
    print(f'{path}: exact required declarations/definitions: PASS ({len(names)})')
assert len(pure) == 14 and len(hotkeys) == 20
print('wayland selection test inventory: PASS')
PY
wayland-scanner --version
sha256sum protocols/ext-data-control-v1/ext-data-control-v1.xml
protocols/ext-data-control-v1/regenerate.sh
git diff --exit-code -- protocols/ext-data-control-v1/
```

Require pinned scanner/hash and unchanged generated files. Do not use a mere successful qmake configure as proof of inventory closure.

- [ ] **Step 3: Run acceptance criteria 2, 4–8 against the actual tree**

```bash
qmake6 goldendict.pro
qmake6 goldendict.pro CONFIG+=verbose_build_output
make -j$(nproc)
make tests-makefile
make tests-build
QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh make tests-run
QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_waylandselection/tst_waylandselection
QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_hotkeys/tst_hotkeys nonXcbCapability
scripts/verify-wayland-selection-live.sh
```

Record actual compiler/link commands: on a positive package probe, they contain HAVE_WAYLAND_DATA_CONTROL, generated protocol C object and the new library link. Record aggregate `exact 20 suites`, the new tests and live marker. The separate offscreen invocation is required for the pre-existing nonXcbCapability; its xcb skip does not substitute for that invocation.

- [ ] **Step 4: Prove genuine no-package compilation and closed-inventory rejection in disposable copies**

Run this complete verification script from the active repository. All fixture/wrapper/build writes stay in its temporary copy. `QMAKE_PKG_CONFIG` is supplied to both app and hotkeys qmake invocations so the test's feature macro really follows the negative probe:

```bash
python3 - <<'PY'
import os, pathlib, shutil, subprocess, tempfile
source = pathlib.Path.cwd()
tracked = subprocess.check_output(['git', 'ls-files', '-z']).decode().split('\0')
real_pkg_config = shutil.which('pkg-config')
assert real_pkg_config
with tempfile.TemporaryDirectory(prefix='gd-selection-no-package.', dir='/tmp') as temporary:
    root = pathlib.Path(temporary)
    for name in filter(None, tracked):
        src = source / name
        if not src.is_file() and not src.is_symlink():
            continue
        dst = root / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst, follow_symlinks=False)
    wrapper = root / 'without-wayland-pkg-config'
    wrapper.write_text('''#!/bin/sh
for arg do
    if [ "$arg" = wayland-client ]; then exit 1; fi
done
exec "$GD_REAL_PKG_CONFIG" "$@"
''')
    wrapper.chmod(0o755)
    env = dict(os.environ, GD_REAL_PKG_CONFIG=real_pkg_config)
    def run(argv, expected=0):
        result = subprocess.run(argv, cwd=root, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout)
        assert (result.returncode == 0) if expected == 0 else (result.returncode != 0), (argv, result.returncode)
        return result.stdout
    run(['qmake6', 'goldendict.pro', 'CONFIG+=verbose_build_output', f'QMAKE_PKG_CONFIG={wrapper}'])
    app_makefile = (root / 'Makefile').read_text()
    for forbidden in ('-DHAVE_WAYLAND_DATA_CONTROL', '-lwayland-client', 'ext-data-control-v1-protocol.o'):
        assert forbidden not in app_makefile, forbidden
    run(['make', '-j2'])
    run(['qmake6', 'tests/tst_hotkeys/tst_hotkeys.pro', '-o', 'tests/tst_hotkeys/Makefile', f'QMAKE_PKG_CONFIG={wrapper}'])
    hotkeys_makefile = (root / 'tests/tst_hotkeys/Makefile').read_text()
    assert '-DHAVE_WAYLAND_DATA_CONTROL' not in hotkeys_makefile
    run(['make', '-C', 'tests/tst_hotkeys', '-j2'])
    env['QT_QPA_PLATFORM'] = 'xcb'
    run(['xvfb-run', '-a', 'timeout', '60', 'tests/tst_hotkeys/tst_hotkeys', 'capabilityConstantMatchesBuild', 'scanPopupBindingIsPortalOnly', 'threeBindingsStayBound', 'scanPopupRowWaylandValuesAndEdits'])
    project = root / 'tests/tst_waylandselection/tst_waylandselection.pro'
    original = project.read_bytes()
    project.unlink()
    out = run(['qmake6', 'tests/tests.pro', '-o', 'tests/Makefile.missing'], expected=1)
    assert 'Expected exactly 20 required suites' in out
    project.write_bytes(original)
    extra = root / 'tests/tst_selection_extra/tst_selection_extra.pro'
    extra.parent.mkdir()
    extra.write_text('TARGET = tst_selection_extra\ninclude(../testsupport.pri)\n')
    out = run(['qmake6', 'tests/tests.pro', '-o', 'tests/Makefile.extra'], expected=1)
    assert 'Expected exactly 20 required suites' in out
print('negative package probe and closed inventory: PASS')
PY
```

Before the manual session, also falsify S1, S4/S5 and the unforced read/cancel/rearm assertions against the same passing hotkeys binary. Each run below has one production-text change in a fresh source copy; GD_TEST_REPO_ROOT redirects only source inspection, while the executable and GUI/environment gate remain the verified original. These are source-assertion kills and do not claim compositor/syscall behaviour:

```bash
python3 - <<'PY'
import os, pathlib, shutil, subprocess, tempfile
root = pathlib.Path.cwd()
binary = root / 'tests/tst_hotkeys/tst_hotkeys'
paths = [p for p in subprocess.check_output(['git', 'ls-files', '-z']).decode().split('\0') if p]
source = (root / 'waylandselection.cc').read_text()
checks = [
    ('S1-listener', 'if( self->live() ) self->fail( "device finished" );', 'if( self->live() ) { self->fail( "device finished" ); self->connection->destroyConnection(); }'),
    ('S4-always-defer', 'if( c->dispatchDepth == 0 ) c->destroyConnection();\n    else QTimer::singleShot', 'if( false ) c->destroyConnection();\n    else QTimer::singleShot'),
    ('S5-early-return', 'if( !registry ) { fail( "cannot get Wayland registry" ); return; }', 'if( !registry ) { return; }'),
    ('read-cancel', 'wl_display_cancel_read( c->display ); c->prepared = false;', 'c->prepared = false;'),
    ('flush-pressure', 'if( errno == EAGAIN ) { displayWrite->setEnabled( true ); return true; }', 'if( errno == EAGAIN ) { return true; }'),
    ('read-rearm', 'if( flushRequests() && live() ) displayRead->setEnabled( true );\n  }\n  void dispatchDisplayWritable()', 'flushRequests();\n  }\n  void dispatchDisplayWritable()'),
]
for name, old, new in checks:
    assert source.count(old) == 1, (name, 'source anchor')
    with tempfile.TemporaryDirectory(prefix=f'gd-selection-source-{name}.', dir='/tmp') as temporary:
        copy = pathlib.Path(temporary)
        for relative in paths:
            src = root / relative
            if not src.is_file() and not src.is_symlink():
                continue
            dst = copy / relative
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst, follow_symlinks=False)
        env = dict(os.environ, QT_QPA_PLATFORM='xcb', GD_TEST_REPO_ROOT=str(copy))
        command = ['xvfb-run', '-a', 'timeout', '60', str(binary), 'scanPopupSourceLoopContractIsPinned']
        baseline = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert baseline.returncode == 0, (name, 'baseline', baseline.stdout[-5000:])
        (copy / 'waylandselection.cc').write_text(source.replace(old, new))
        mutant = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert mutant.returncode == 1 and 'FAIL!  : TstHotkeys::scanPopupSourceLoopContractIsPinned()' in mutant.stdout, (name, mutant.returncode, mutant.stdout[-5000:])
        print(f'{name}: passing source baseline; named source assertion killed mutant')
print('selection source-contract mutations: PASS')
PY
```

The fake-portal three-binding test and real Wayland Preferences child prove their own behaviours in the negative build. Actual clipboard translation by that app is still manual criterion 15; do not call those tests end-to-end popup coverage.

- [ ] **Step 5: Execute or explicitly record manual acceptance criteria 9–16**

Use an isolated copy of the reporter's config when running the actual application; do not edit the user's active config. Record platform, build capability, command/session, observed behaviour and evidence for each check in the controller report. If the interactive session is unavailable, mark that specific observation unobserved and report the acceptance limitation; never fabricate a successful receipt.

1. Legacy config without the new keys loads with no monitoring. Save writes enable `0` and key `Meta+Ctrl+D`. Open/accept Preferences with each noncanonical/unsupported stored key; untouched text survives exactly (also automated by Task 4).
2. In KWin Wayland with ext manager, enable the third row and accept the portal assignment. Select a word in another app and activate: it translates through the popup; selection alone creates no new shortcut-driven popup. Repeat using GoldenDict's article selection.
3. Keep the old selection, copy a different word and activate: copied word wins. Select and activate immediately: promoted capture uses the new word. An unfinished activation at 300 ms uses only clipboard or empty handling.
4. Enable send-to-main-window: selected phrase goes there. A visible popup showing the same sanitised phrase handles another activation without duplicate translation or an empty warning. Reject a phrase through the configured input-length limit: nothing opens and empty handling is attempted.
5. With a fresh source or successful empty capture and an empty clipboard, activate while the main window is visible: exact message for 3000 ms. Hide with a tray: GoldenDict tray message requested. Hide without a tray: only selection-empty logged. Desktop suppression of the requested tray message is not reported as a guarantee of display.
6. Disable while capture is pending: connection/pipe close with no late translation. Re-enable: fresh source starts. Open/cancel Preferences and rescan dictionaries: popup recreation keeps the selection connection.
7. Without the ext manager, the enabled shortcut remains registered and the suffix is exactly `selection unavailable: no ext_data_control_manager_v1 — clipboard is used instead`; clipboard translation works. Run the no-capture application build too: registration/clipboard still work and suffix is exactly `selection unsupported in this build — clipboard is used instead`.
8. On xcb with both old popup-enabling preferences false and synced new preference true, start/exit: no popup is created and stored geometry remains unchanged. Third row is unchecked/disabled with its exact X11 label; accepting it retains both Wayland values. The logical Windows/macOS tests cover renderer/preservation expressions without claiming native release validation.

- [ ] **Step 6: Complete source/evidence review and commit the documentation**

Review acceptance criterion 17 and the §10 limit list against the exact prose above. Check that every source assertion reports its evidence class and that historical 18-suite measurements were not relabelled as 20-suite measurements. Run `git diff --check` and inspect the documentation diff. The final deliverable is the tested implementation plus accurate documentation and an evidence report whose manual gaps are explicit.

```bash
git diff --check
git diff -- README.md docs/wayland-limitations.md
git add README.md docs/wayland-limitations.md
git commit -m "docs(wayland): describe selected-text coverage and acceptance limits"
```
