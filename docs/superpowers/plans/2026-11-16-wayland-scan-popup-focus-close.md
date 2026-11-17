# Wayland Scan Popup Focus-Loss Dismissal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the deterministic
> subagent-driven-development controller to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax; leave the pinned plan unchanged during execution.

**Goal:** Give the unpinned managed scan popup an opening activation request and synchronous dismissal after keyboard focus leaves its own interaction, using the approved cross-platform rule.

**Architecture:** A production-linked `PopupDismiss` module supplies five pure helpers and a GUI-thread `QObject` closer. `ScanPopup` uniquely owns the closer and supplies `hideWindow()` as its callback; Qt retains its existing `Qt::Popup` dismissal and all pre-existing activation and pointer paths. A dedicated QtTest suite checks the helpers, real xcb focus transitions, and scoped source wiring, followed by the application link and closed aggregate gate.

**Tech Stack:** C++17, Qt 6 Core/Gui/Widgets/QtTest, qmake6, GNU make, xcb, xvfb-run, existing `tests/testsupport.pri` and `TestSupport` fixtures.

**Contract:** `docs/superpowers/specs/2026-11-16-wayland-scan-popup-focus-close-spec.md`, approved commit `8985f7c3`, SHA-256 `8eca16004082a1420c31dc7af143ecc3617379fc89bdac9cebc96255bec51ce7`. The approved design is `docs/superpowers/specs/2026-11-16-wayland-scan-popup-focus-close-design.md`, commit `a3493dcf`. The specification supersedes obsolete activation-state, settlement, and transient-ancestry mechanisms in the design history.

**Execution:** One lane, `popup-dismiss`, strictly sequential. Each task consumes the committed result of its same-lane predecessor. Run task commands from the implementation checkout root. The planning session runs neither builds nor suites and commits nothing.

**Open questions:** None; the approved specification leaves no unresolved implementation choice, so no `## Open Questions` section is needed.

**File structure:** `popupdismiss.hh` publishes the exact API and guarded state; `popupdismiss.cc` implements the pure rules and event lifecycle. `tests/tst_popup_dismiss/tst_popup_dismiss.pro` links that production module; `tests/tst_popup_dismiss/tst_popup_dismiss.cc` owns all new fixtures and copied source helpers. `scanpopup.hh`/`scanpopup.cc` own integration, `goldendict.pro`/`tests/tests.pro` own build registration, and `docs/wayland-limitations.md` owns the three specified documentation insertions. No existing test/support file changes.

**Exclusive requirement ownership:** The following partition assigns each requirement one implementation owner; later tasks consume rather than reimplement earlier requirements. Global Constraints repeat contract invariants for isolated task briefs.

| Task | Owned contract requirements | Substantive test slots |
| --- | --- | --- |
| 1 | §2.1 header guard/includes/enums/free declarations; §2.2 five pure rules; §6.1 suite project, environment, entry point and free-function signature checks; §6.2–6.3 all literal matrices | 5 |
| 2 | §2.1 closer class; §2.3 classifier; §2.4 public state/action API; §3 entire synchronous lifecycle/logging/teardown; §6.1 runtime discipline and closer signature checks; §6.4 twelve runtime cases; §6.5 copied/adapted source helpers; §6.6(2–4,11–12); §6.7 and §9.2 evidence/mutation limits for these cases | 17 |
| 3 | §4 complete ScanPopup integration and preserved adjacent paths/platform table; §6.6(1,5–8) and their §9.2 mappings | 5 |
| 4 | §5 application registration and exact ordinary inventory; §6.6(9–10) and their §9.2 mappings | 2 |
| 5 | §7 exact documentation and preserved evidence totals | 0 |
| 6 | §8 verbatim acceptance and results; final reconciliation of §1 scope/non-goals, §6.7 evidence limits, and §9 traceability without new implementation | 0 |

## Global Constraints

Approved contract: `docs/superpowers/specs/2026-11-16-wayland-scan-popup-focus-close-spec.md` at commit `8985f7c3`, SHA-256 `8eca16004082a1420c31dc7af143ecc3617379fc89bdac9cebc96255bec51ce7`; numbered section references in every task refer to this specification.

The new closer applies on every platform. Its rule ignores hidden, pinned, and never-focused windows. It preserves an owned menu or dropdown interaction, and dismisses when a dialog takes focus, including a dialog opened by the scan popup. A pinned popup never auto-hides. Default X11 `Qt::Popup` flags and Qt's existing popup dismissal remain intact. The managed `xcb` `Tool` form intentionally gains this focus-loss rule as well.

- No preference, UI control, changed stored default, grab emulation, activation guarantee, timer, grace period, pointer polling, suppression state, or modal-dialog exception in the new module.
- No changes to hover translation, the scan flag, clipboard/selection watching, alt-mode, the tray, the main window, the article view, pinned flags, or the pinned stay-on-top option.
- No changes to existing proximity, leave-timer, outside-press, or other `hideWindow()` callers. Those paths retain their behaviour, including possible menu-time dismissal on `xcb` `Tool` windows. The menu-survival guarantee here belongs to the new focus rule.
- No README edit and no claim to have verified a compositor's grant/refusal or a genuine application-inactive transition using the single-process suite.

Opening requests are added only when an unpinned window changes from hidden to visible with a non-`Qt::Popup` type, including the replacement window shown on unpin. Updating an already-visible unpinned window adds no request. Keep the existing pinned-visible re-raise, the `xcb` `giveFocus` block, the already-visible `xcb` `Tool` activation, and hotkey/token activation paths.

No other pre-existing activation path changes. In particular retain Windows' existing `giveFocus` inputs, the default `giveFocus = false` parameter, all caller arguments, `MainWindow`'s `scanPopup->winId()` before scan-popup token application, the clipboard-lookup path without a forced handle, and `PlatformWindow::applyActivation()` behaviour. `NoHandle` means no native handle exists yet, not every clipboard lookup. No main-window or PlatformWindow edit is needed.

Implementation files are limited to new `popupdismiss.hh`, `popupdismiss.cc`, `tests/tst_popup_dismiss/tst_popup_dismiss.pro`, and `tests/tst_popup_dismiss/tst_popup_dismiss.cc`, plus edits to `scanpopup.hh`, `scanpopup.cc`, `goldendict.pro`, `tests/tests.pro`, and `docs/wayland-limitations.md`. Existing test helpers are copied into the new suite, not changed in `tst_hotkeys` or shared support.

```qmake
# Ordinary mode is a closed acceptance inventory: exactly the 21 required suites.
    GD_REQUIRED_SUITE_NAMES = \
        tst_article_bridge tst_article_history tst_article_load \
        tst_article_schemes tst_article_stream tst_btree_index tst_encoding \
        tst_external_content tst_hotkeys tst_htmlescape tst_langcoder \
        tst_linkrewrite tst_platformwindow tst_popup_dismiss \
        tst_portal_hotkeys tst_print_pipeline tst_regex tst_save_article \
        tst_utf8_wstring tst_waylandselection tst_website_frames
    !count(GD_TEST_NAMES, 21): error("Expected exactly 21 required suites; found: $$GD_TEST_NAMES")
    message("tests/tests.pro: exact 21 suites: $$GD_TEST_NAMES")
```

Run these commands verbatim from the implemented repository root during implementation acceptance, in order:

```bash
qmake6 goldendict.pro -o Makefile && make -j"$(nproc)"          # application links (D7)
qmake6 tests/tests.pro -o tests/Makefile
make -C tests -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check
```

All commands must succeed. The new suite is the 21st required ordinary-mode suite; every required new runtime case must execute without skips. `tests/tests.pro` does not select a QPA: the last command selects xcb explicitly. The application link is required in addition to the module suite. These are acceptance instructions, not commands executed by the specification drafter.

## Task 1: Establish the pure rules and production-linked matrix suite

**Implementer tier:** Advanced

**Lane/category:** popup-dismiss · backend

**Same-lane predecessor:** None; this is Task 1.

**Files:**

- Create: `popupdismiss.hh` — guard, includes, scoped enums, and five free declarations from §2.1; Task 2 adds the specified class before the namespace closes.
- Create: `popupdismiss.cc` — the five definitions in namespace `PopupDismiss`.
- Create: `tests/tst_popup_dismiss/tst_popup_dismiss.pro` — complete project below (§6.1).
- Create: `tests/tst_popup_dismiss/tst_popup_dismiss.cc` — includes, five signature checks, environment/watchdog hooks, the five inline matrix slots and their `_data` partners, and QtTest entry point. Later tasks insert their inline slots before the class-closing `};`.

**Interfaces:**

- Consumes: `bool TestSupport::requiredEnvironment( QStringList * reasons )`, `bool TestSupport::prerequisiteAvailable( QString const & token, QString * reason )`, `void TestSupport::installWatchdog( int seconds )`, `void TestSupport::clearWatchdog()`, and `constexpr int TestSupport::PureWatchdogSeconds = 60` from `tests/support/fixtures.hh` (§6.1); do not modify them.
- Produces (§2.1–2.2): `enum class PopupDismiss::Action { Ignore, Hide };` and `enum class PopupDismiss::FocusDestination { OwnWindow, OwnPopup, Foreign, None };`.
- Produces (§2.2): `PopupDismiss::Action PopupDismiss::onFocusMoved( bool visible, bool pinned, bool armed, PopupDismiss::FocusDestination destination, bool applicationActive )`.
- Produces (§2.2): `bool PopupDismiss::needsExplicitActivation( Qt::WindowFlags flags )`; `bool PopupDismiss::armedAfterShow( QWindow const * ownWindow, QWindow const * focusWindow )`; `bool PopupDismiss::isOwnedPopup( QWidget const * owner, QWidget const * candidate )`; `bool PopupDismiss::isPopupWindowType( Qt::WindowType type )`.
- Produces (§6.1): production-linked `tests/tst_popup_dismiss/tst_popup_dismiss`; no ScanPopup, hotkey/Xlib/KDE source closure and no portable variant.

**Implementation steps:**

- [ ] **Step 1: Write all five named matrix tests first.** Place the following class contents into the new translation unit before the production helpers are implemented. Each substantive name occurs here once; its `_data` partner belongs to the same case. Every expected value is a literal, never calculated with the production expression. The 64 rule rows have exactly three Hide results.

```cpp
#include "popupdismiss.hh"
#include "support/fixtures.hh"
#include <QtTest>
#include <QApplication>
#include <QGuiApplication>
#include <QWindow>
#include <QWidget>
#include <QDialog>
#include <QSignalSpy>
#include <QPointer>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <type_traits>
#include <memory>

using Action = PopupDismiss::Action;
using Destination = PopupDismiss::FocusDestination;
static_assert( std::is_same_v< decltype( &PopupDismiss::onFocusMoved ),
  Action (*)( bool, bool, bool, Destination, bool ) > );
static_assert( std::is_same_v< decltype( &PopupDismiss::needsExplicitActivation ),
  bool (*)( Qt::WindowFlags ) > );
static_assert( std::is_same_v< decltype( &PopupDismiss::armedAfterShow ),
  bool (*)( QWindow const *, QWindow const * ) > );
static_assert( std::is_same_v< decltype( &PopupDismiss::isOwnedPopup ),
  bool (*)( QWidget const *, QWidget const * ) > );
static_assert( std::is_same_v< decltype( &PopupDismiss::isPopupWindowType ),
  bool (*)( Qt::WindowType ) > );

class TstPopupDismiss : public QObject {
  Q_OBJECT
private slots:
  void initTestCase()
  {
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ),
              qPrintable( reasons.join( '\n' ) ) );
    QString displayReason;
    QVERIFY2( TestSupport::prerequisiteAvailable( QStringLiteral( "display" ),
                                                &displayReason ),
              qPrintable( displayReason ) );
    TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
  }
  void cleanupTestCase() { TestSupport::clearWatchdog(); }

  void focusMoveRule_data()
  {
    QTest::addColumn< bool >( "visible" );
    QTest::addColumn< bool >( "pinned" );
    QTest::addColumn< bool >( "armed" );
    QTest::addColumn< int >( "destination" );
    QTest::addColumn< bool >( "applicationActive" );
    QTest::addColumn< int >( "expected" );
    struct Row { char const * name; bool v, p, a; Destination d; bool active; Action expected; };
    Row const rows[] = {
      { "0-0-0-OwnWindow-0", 0,0,0, Destination::OwnWindow,0,Action::Ignore },
      { "0-0-0-OwnWindow-1", 0,0,0, Destination::OwnWindow,1,Action::Ignore },
      { "0-0-0-OwnPopup-0", 0,0,0, Destination::OwnPopup,0,Action::Ignore },
      { "0-0-0-OwnPopup-1", 0,0,0, Destination::OwnPopup,1,Action::Ignore },
      { "0-0-0-Foreign-0", 0,0,0, Destination::Foreign,0,Action::Ignore },
      { "0-0-0-Foreign-1", 0,0,0, Destination::Foreign,1,Action::Ignore },
      { "0-0-0-None-0", 0,0,0, Destination::None,0,Action::Ignore },
      { "0-0-0-None-1", 0,0,0, Destination::None,1,Action::Ignore },
      { "0-0-1-OwnWindow-0", 0,0,1, Destination::OwnWindow,0,Action::Ignore },
      { "0-0-1-OwnWindow-1", 0,0,1, Destination::OwnWindow,1,Action::Ignore },
      { "0-0-1-OwnPopup-0", 0,0,1, Destination::OwnPopup,0,Action::Ignore },
      { "0-0-1-OwnPopup-1", 0,0,1, Destination::OwnPopup,1,Action::Ignore },
      { "0-0-1-Foreign-0", 0,0,1, Destination::Foreign,0,Action::Ignore },
      { "0-0-1-Foreign-1", 0,0,1, Destination::Foreign,1,Action::Ignore },
      { "0-0-1-None-0", 0,0,1, Destination::None,0,Action::Ignore },
      { "0-0-1-None-1", 0,0,1, Destination::None,1,Action::Ignore },
      { "0-1-0-OwnWindow-0", 0,1,0, Destination::OwnWindow,0,Action::Ignore },
      { "0-1-0-OwnWindow-1", 0,1,0, Destination::OwnWindow,1,Action::Ignore },
      { "0-1-0-OwnPopup-0", 0,1,0, Destination::OwnPopup,0,Action::Ignore },
      { "0-1-0-OwnPopup-1", 0,1,0, Destination::OwnPopup,1,Action::Ignore },
      { "0-1-0-Foreign-0", 0,1,0, Destination::Foreign,0,Action::Ignore },
      { "0-1-0-Foreign-1", 0,1,0, Destination::Foreign,1,Action::Ignore },
      { "0-1-0-None-0", 0,1,0, Destination::None,0,Action::Ignore },
      { "0-1-0-None-1", 0,1,0, Destination::None,1,Action::Ignore },
      { "0-1-1-OwnWindow-0", 0,1,1, Destination::OwnWindow,0,Action::Ignore },
      { "0-1-1-OwnWindow-1", 0,1,1, Destination::OwnWindow,1,Action::Ignore },
      { "0-1-1-OwnPopup-0", 0,1,1, Destination::OwnPopup,0,Action::Ignore },
      { "0-1-1-OwnPopup-1", 0,1,1, Destination::OwnPopup,1,Action::Ignore },
      { "0-1-1-Foreign-0", 0,1,1, Destination::Foreign,0,Action::Ignore },
      { "0-1-1-Foreign-1", 0,1,1, Destination::Foreign,1,Action::Ignore },
      { "0-1-1-None-0", 0,1,1, Destination::None,0,Action::Ignore },
      { "0-1-1-None-1", 0,1,1, Destination::None,1,Action::Ignore },
      { "1-0-0-OwnWindow-0", 1,0,0, Destination::OwnWindow,0,Action::Ignore },
      { "1-0-0-OwnWindow-1", 1,0,0, Destination::OwnWindow,1,Action::Ignore },
      { "1-0-0-OwnPopup-0", 1,0,0, Destination::OwnPopup,0,Action::Ignore },
      { "1-0-0-OwnPopup-1", 1,0,0, Destination::OwnPopup,1,Action::Ignore },
      { "1-0-0-Foreign-0", 1,0,0, Destination::Foreign,0,Action::Ignore },
      { "1-0-0-Foreign-1", 1,0,0, Destination::Foreign,1,Action::Ignore },
      { "1-0-0-None-0", 1,0,0, Destination::None,0,Action::Ignore },
      { "1-0-0-None-1", 1,0,0, Destination::None,1,Action::Ignore },
      { "1-0-1-OwnWindow-0", 1,0,1, Destination::OwnWindow,0,Action::Ignore },
      { "1-0-1-OwnWindow-1", 1,0,1, Destination::OwnWindow,1,Action::Ignore },
      { "1-0-1-OwnPopup-0", 1,0,1, Destination::OwnPopup,0,Action::Ignore },
      { "1-0-1-OwnPopup-1", 1,0,1, Destination::OwnPopup,1,Action::Ignore },
      { "1-0-1-Foreign-0", 1,0,1, Destination::Foreign,0,Action::Hide },
      { "1-0-1-Foreign-1", 1,0,1, Destination::Foreign,1,Action::Hide },
      { "1-0-1-None-0", 1,0,1, Destination::None,0,Action::Hide },
      { "1-0-1-None-1", 1,0,1, Destination::None,1,Action::Ignore },
      { "1-1-0-OwnWindow-0", 1,1,0, Destination::OwnWindow,0,Action::Ignore },
      { "1-1-0-OwnWindow-1", 1,1,0, Destination::OwnWindow,1,Action::Ignore },
      { "1-1-0-OwnPopup-0", 1,1,0, Destination::OwnPopup,0,Action::Ignore },
      { "1-1-0-OwnPopup-1", 1,1,0, Destination::OwnPopup,1,Action::Ignore },
      { "1-1-0-Foreign-0", 1,1,0, Destination::Foreign,0,Action::Ignore },
      { "1-1-0-Foreign-1", 1,1,0, Destination::Foreign,1,Action::Ignore },
      { "1-1-0-None-0", 1,1,0, Destination::None,0,Action::Ignore },
      { "1-1-0-None-1", 1,1,0, Destination::None,1,Action::Ignore },
      { "1-1-1-OwnWindow-0", 1,1,1, Destination::OwnWindow,0,Action::Ignore },
      { "1-1-1-OwnWindow-1", 1,1,1, Destination::OwnWindow,1,Action::Ignore },
      { "1-1-1-OwnPopup-0", 1,1,1, Destination::OwnPopup,0,Action::Ignore },
      { "1-1-1-OwnPopup-1", 1,1,1, Destination::OwnPopup,1,Action::Ignore },
      { "1-1-1-Foreign-0", 1,1,1, Destination::Foreign,0,Action::Ignore },
      { "1-1-1-Foreign-1", 1,1,1, Destination::Foreign,1,Action::Ignore },
      { "1-1-1-None-0", 1,1,1, Destination::None,0,Action::Ignore },
      { "1-1-1-None-1", 1,1,1, Destination::None,1,Action::Ignore },
    };
    static_assert( sizeof( rows ) / sizeof( rows[0] ) == 64 );
    for( auto const & r : rows )
      QTest::newRow( r.name ) << r.v << r.p << r.a << int( r.d ) << r.active << int( r.expected );
  }
  void focusMoveRule()
  {
    QFETCH( bool, visible ); QFETCH( bool, pinned ); QFETCH( bool, armed );
    QFETCH( int, destination ); QFETCH( bool, applicationActive ); QFETCH( int, expected );
    QCOMPARE( int( PopupDismiss::onFocusMoved( visible, pinned, armed,
                   Destination( destination ), applicationActive ) ), expected );
  }
  void explicitActivationRule_data()
  {
    QTest::addColumn< Qt::WindowFlags >( "flags" ); QTest::addColumn< bool >( "expected" );
    QTest::newRow( "popup" ) << Qt::WindowFlags( Qt::Popup ) << false;
    QTest::newRow( "popup-frameless" ) << ( Qt::Popup | Qt::FramelessWindowHint ) << false;
    QTest::newRow( "popup-stays-on-top" ) << ( Qt::Popup | Qt::WindowStaysOnTopHint ) << false;
    QTest::newRow( "popup-both-hints" ) << ( Qt::Popup | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint ) << false;
    QTest::newRow( "tool" ) << Qt::WindowFlags( Qt::Tool ) << true;
    QTest::newRow( "managed-tool" ) << ( Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint ) << true;
    QTest::newRow( "window" ) << Qt::WindowFlags( Qt::Window ) << true;
    QTest::newRow( "dialog" ) << Qt::WindowFlags( Qt::Dialog ) << true;
    QTest::newRow( "tool-bypass" ) << ( Qt::Tool | Qt::X11BypassWindowManagerHint ) << true;
  }
  void explicitActivationRule()
  {
    QFETCH( Qt::WindowFlags, flags ); QFETCH( bool, expected );
    QCOMPARE( PopupDismiss::needsExplicitActivation( flags ), expected );
  }
  void ownedPopupRule_data()
  {
    QTest::addColumn< QString >( "graph" ); QTest::addColumn< bool >( "expected" );
    struct Row { char const * graph; bool expected; };
    Row const rows[] = { { "both-null",false }, { "null-owner",false },
      { "null-candidate",false }, { "self",false }, { "popup-self",false },
      { "child-popup",true }, { "grandchild-popup",true }, { "nested-popup",true },
      { "sibling",false }, { "unrelated-top-level",false }, { "other-window-menu",false } };
    for( auto const & r : rows ) QTest::newRow( r.graph ) << QString::fromLatin1( r.graph ) << r.expected;
  }
  void ownedPopupRule()
  {
    QFETCH( QString, graph ); QFETCH( bool, expected );
    QWidget common, other;
    QWidget owner( &common ), sibling( &common ), child( &owner );
    QWidget popupOwner( nullptr, Qt::Popup );
    QWidget direct( &owner, Qt::Popup ), grandchild( &child, Qt::Popup );
    QWidget nested( &direct, Qt::Popup ), foreign( &other, Qt::Popup );
    QCOMPARE( owner.parentWidget(), &common ); QCOMPARE( sibling.parentWidget(), &common );
    QCOMPARE( direct.parentWidget(), &owner ); QCOMPARE( child.parentWidget(), &owner );
    QCOMPARE( grandchild.parentWidget(), &child ); QCOMPARE( nested.parentWidget(), &direct );
    QCOMPARE( foreign.parentWidget(), &other );
    QWidget const * a = &owner; QWidget const * b = nullptr;
    if( graph == "both-null" ) a = nullptr;
    else if( graph == "null-owner" ) { a = nullptr; b = &direct; }
    else if( graph == "null-candidate" ) b = nullptr;
    else if( graph == "self" ) b = &owner;
    else if( graph == "popup-self" ) { a = &popupOwner; b = &popupOwner; }
    else if( graph == "child-popup" ) b = &direct;
    else if( graph == "grandchild-popup" ) b = &grandchild;
    else if( graph == "nested-popup" ) b = &nested;
    else if( graph == "sibling" ) b = &sibling;
    else if( graph == "unrelated-top-level" ) b = &other;
    else if( graph == "other-window-menu" ) b = &foreign;
    else QFAIL( "Unknown ownership row" );
    if( graph != "self" && graph != "popup-self" ) QVERIFY( !a || a != b );
    QCOMPARE( PopupDismiss::isOwnedPopup( a, b ), expected );
  }
  void showArmingRule_data()
  {
    QTest::addColumn< int >( "identity" ); QTest::addColumn< bool >( "expected" );
    QTest::newRow( "same" ) << 0 << true;
    QTest::newRow( "different" ) << 1 << false;
    QTest::newRow( "null-own" ) << 2 << false;
    QTest::newRow( "null-focus" ) << 3 << false;
    QTest::newRow( "both-null" ) << 4 << false;
  }
  void showArmingRule()
  {
    QFETCH( int, identity ); QFETCH( bool, expected );
    QWindow A, B;
    QWindow const * const owners[] = { &A, &A, nullptr, &A, nullptr };
    QWindow const * const focused[] = { &A, &B, &A, nullptr, nullptr };
    QCOMPARE( PopupDismiss::armedAfterShow( owners[identity], focused[identity] ), expected );
  }
  void popupWindowTypeRule_data()
  {
    QTest::addColumn< int >( "type" ); QTest::addColumn< bool >( "expected" );
    QTest::newRow( "popup" ) << int( Qt::Popup ) << true;
    QTest::newRow( "tooltip" ) << int( Qt::ToolTip ) << false;
    QTest::newRow( "dialog" ) << int( Qt::Dialog ) << false;
    QTest::newRow( "tool" ) << int( Qt::Tool ) << false;
    QTest::newRow( "window" ) << int( Qt::Window ) << false;
  }
  void popupWindowTypeRule()
  {
    QFETCH( int, type ); QFETCH( bool, expected );
    QCOMPARE( PopupDismiss::isPopupWindowType( Qt::WindowType( type ) ), expected );
  }
};
QTEST_MAIN( TstPopupDismiss )
#include "tst_popup_dismiss.moc"
```

- [ ] **Step 2: Establish RED before the covered production edit.** Create the following exact project, run the commands, and record failure with the new test source already present. Before the module exists, an absent production header/symbol is an expected initial TDD failure; do not describe that as a behavioural mutation kill. After declarations exist, use the literal matrix assertions to identify wrong rule/helper results before completing the corresponding definition. Do not calculate expected rows from the implementation or weaken a failing row.

```qmake
# Scan-popup focus-destination dismissal.
# Link the production module; acceptance explicitly selects xcb under xvfb.
TARGET = tst_popup_dismiss
include(../testsupport.pri)

QT += core gui widgets

INCLUDEPATH += $$GD_REPO_ROOT

SOURCES += $$GD_REPO_ROOT/popupdismiss.cc \
           tst_popup_dismiss.cc

HEADERS += $$GD_REPO_ROOT/popupdismiss.hh
```

```bash
qmake6 tests/tst_popup_dismiss/tst_popup_dismiss.pro -o tests/tst_popup_dismiss/Makefile
make -C tests/tst_popup_dismiss -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_popup_dismiss/tst_popup_dismiss
```

- [ ] **Step 3: Implement the exact pure contract.** Start the header with the §2.1 license, `GOLDENDICT_POPUPDISMISS_HH_INCLUDED` guard, explicit `<QEvent>`, `<QMetaObject>`, `<QObject>`, `<QPointer>`, `<Qt>`, `<functional>`, forward declarations `class QWidget; class QWindow;`, the two scoped enums and the five exact declarations in Interfaces. Close the namespace and guard; Task 2 inserts the specified class. Include `popupdismiss.hh` first in the implementation, followed by `<QApplication>`, `<QDebug>`, `<QGuiApplication>`, `<QLoggingCategory>`, `<QWidget>`, `<QWindow>`, `<utility>`. Add these definitions, with no native-handle creation or focus/window I/O in the pure identity helpers:

```cpp
namespace PopupDismiss {
Action onFocusMoved( bool visible, bool pinned, bool armed,
                     FocusDestination destination, bool applicationActive )
{
  return visible && !pinned && armed &&
         ( destination == FocusDestination::Foreign ||
           ( destination == FocusDestination::None && !applicationActive ) )
           ? Action::Hide : Action::Ignore;
}
bool needsExplicitActivation( Qt::WindowFlags flags )
{
  return ( flags & Qt::WindowType_Mask ) != Qt::Popup;
}
bool armedAfterShow( QWindow const * ownWindow, QWindow const * focusWindow )
{
  return ownWindow != nullptr && ownWindow == focusWindow;
}
bool isOwnedPopup( QWidget const * owner, QWidget const * candidate )
{
  if( !owner || !candidate || owner == candidate ) return false;
  for( QWidget const * parent = candidate->parentWidget(); parent;
       parent = parent->parentWidget() )
    if( parent == owner ) return true;
  return false;
}
bool isPopupWindowType( Qt::WindowType type ) { return type == Qt::Popup; }
} // namespace PopupDismiss
```

- [ ] **Step 4: Run GREEN and inspect the exact matrix inventory.** Re-run the three commands in Step 2. Require every literal row to pass and no skips. Check §9.2's matrix mappings: omitted/inverted visible/pin/armed guards, Foreign inactivity, OwnWindow/OwnPopup dismissal, None-active handling, unmasked flags/Popup bit containment, strict self/null/nested ownership, null identity and exact Popup type must change the designated expectations. Equivalent `&&` reordering has no claimed observable failure. The helper fixtures do not prove compositor activation.

- [ ] **Step 5: Commit** only these four files with the exact message:

```bash
git add popupdismiss.hh popupdismiss.cc tests/tst_popup_dismiss/tst_popup_dismiss.pro tests/tst_popup_dismiss/tst_popup_dismiss.cc
git commit -m "feat(popup): add focus dismissal rules and matrix coverage"
```

## Task 2: Implement and prove the closer event lifecycle

**Implementer tier:** Advanced

**Lane/category:** popup-dismiss · backend

**Same-lane predecessor:** Task 1 — Establish the pure rules and production-linked matrix suite.

**Files:**

- Modify: `popupdismiss.hh` — insert `class FocusLossCloser` immediately after `bool isPopupWindowType( Qt::WindowType type );`, before `} // namespace PopupDismiss`; the final header must equal §2.1 in full.
- Modify: `popupdismiss.cc` — after the includes add the logging category; append the constructor, destructor, setters/getter, `eventFilter`, `destinationOf`, and `dismiss` inside namespace `PopupDismiss`, after the five pure definitions.
- Modify: `tests/tst_popup_dismiss/tst_popup_dismiss.cc` — before `class TstPopupDismiss` add private fixture/source helpers and signature checks; insert the seventeen inline slots below before its closing `};`. Copy helpers from `tests/tst_hotkeys/tst_hotkeys.cc:395-587` without modifying that file.

**Interfaces:**

- Consumes (§2.2, Task 1): `Action onFocusMoved( bool visible, bool pinned, bool armed, FocusDestination destination, bool applicationActive )`, `bool needsExplicitActivation( Qt::WindowFlags flags )`, `bool armedAfterShow( QWindow const * ownWindow, QWindow const * focusWindow )`, `bool isOwnedPopup( QWidget const * owner, QWidget const * candidate )`, `bool isPopupWindowType( Qt::WindowType type )`, in namespace `PopupDismiss`; `Action = { Ignore, Hide }`, `FocusDestination = { OwnWindow, OwnPopup, Foreign, None }` are scoped enums.
- Consumes (§3): `void QGuiApplication::focusWindowChanged( QWindow * focusWindow )`, `void QGuiApplication::applicationStateChanged( Qt::ApplicationState state )`, `QWindow * QWidget::windowHandle() const`, `QWindow * QGuiApplication::focusWindow()`, `QWidget * QApplication::activePopupWidget()`, and the widget's Show/Hide events, on the GUI thread.
- Produces (§2.1, §2.4): `explicit PopupDismiss::FocusLossCloser( QWidget * window, QObject * parent = nullptr )`, `~FocusLossCloser() override`, `void setPinned( bool pinned )`, `bool isPinned() const`, `void setDismissHandler( std::function< void() > handler )`.
- Produces (§2.1, §3): protected `bool eventFilter( QObject * watched, QEvent * event ) override`; private `FocusDestination destinationOf( QWindow const * focusWindow ) const`, `void dismiss( char const * reason )`. State is exactly the specified `QPointer< QWidget > window`, false-initialised `pinned`, `armed`, `dismissing`, two `QMetaObject::Connection` members and one `std::function< void() > dismissHandler`. No additional member state or public seam.
- Produces (§6.5, suite-local only): `QString stripPreprocessorDirectives( QString const & text )`, `QString selectionBlock( QString const & text, QString const & anchor )`, `QString selectionBranch( QString const & text, QString const & condition )`, `QStringList selectionStatements( QString const & text )`, `QString selectionDirectActions( QString const & text )`, `bool selectionOrdered( QString const & text, QStringList const & needles )`. These do not become production APIs.
- Consumes (§6.1, Task 1): existing suite entry point/environment/watchdog; `QString TestSupport::repositoryRoot()` for real repository source reads.

**Implementation steps:**

- [ ] **Step 1: Write the test-only helpers, signature pins, and the seventeen named tests before the corresponding production event-model edits.** Preserve the five existing matrices. Use the code below for the common runtime fixture; every caller asserts the returned result, so a failed QTRY in a helper cannot let a test continue. A helper assertion runs in a void lambda and communicates success separately, avoiding QtTest's bare-return macros in a bool function.

```cpp
using Closer = PopupDismiss::FocusLossCloser;
static_assert( std::is_constructible_v< Closer, QWidget * > );
static_assert( std::is_constructible_v< Closer, QWidget *, QObject * > );
static_assert( std::is_same_v< decltype( &Closer::setPinned ), void (Closer::*)( bool ) > );
static_assert( std::is_same_v< decltype( &Closer::isPinned ), bool (Closer::*)() const > );
static_assert( std::is_same_v< decltype( &Closer::setDismissHandler ),
                             void (Closer::*)( std::function< void() > ) > );

namespace {
Qt::WindowFlags managedFlags()
{
  return Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
}
bool expectFocus( QWidget & widget )
{
  bool success = false;
  [&] {
    QVERIFY2( widget.isVisible(), "Focus destination must be visible" );
    QVERIFY2( widget.windowHandle(), "Focus destination must have a native handle" );
    QTRY_COMPARE( QGuiApplication::focusWindow(), widget.windowHandle() );
    success = true;
  }();
  return success;
}
bool activate( QWidget & widget )
{
  widget.show(); widget.activateWindow(); widget.raise();
  return expectFocus( widget );
}
struct Windows {
  QWidget first{ nullptr, managedFlags() };
  QWidget second, third;
  int count = 0;
  std::unique_ptr< Closer > closer;
  Windows() : closer( std::make_unique< Closer >( &first ) )
  {
    closer->setDismissHandler( [this] { ++count; first.hide(); } );
  }
};
struct ShowProbe : QObject {
  QWindow * ownAtShow = nullptr;
  QWindow * focusAtShow = nullptr;
  int spyCountAtShow = -1;
  QSignalSpy * spy = nullptr;
  bool eventFilter( QObject * watched, QEvent * event ) override
  {
    if( event->type() == QEvent::Show )
    {
      ownAtShow = static_cast< QWidget * >( watched )->windowHandle();
      focusAtShow = QGuiApplication::focusWindow();
      spyCountAtShow = spy->size();
    }
    return QObject::eventFilter( watched, event );
  }
};
QString source( char const * path )
{
  QString result;
  [&] {
    QString const root = TestSupport::repositoryRoot();
    QVERIFY2( !root.isEmpty(), "Cannot resolve real repository root" );
    QFile file( QDir( root ).filePath( QLatin1String( path ) ) );
    QVERIFY2( file.open( QIODevice::ReadOnly ), qPrintable( file.errorString() ) );
    result = QString::fromUtf8( file.readAll() );
    QVERIFY2( !result.isEmpty(), path );
  }();
  return result;
}
} // namespace
```

For source helpers, copy `selectionRawBlock`, `selectionTokens`, `selectionCode`, `selectionHas`, `selectionCount`, `selectionBlock`, `selectionStatementEnd`, `selectionStatements`, `selectionDirectActions`, `selectionBranch`, and `selectionOrdered` from the exact anchors in `tests/tst_hotkeys/tst_hotkeys.cc:399-587` into this suite's anonymous namespace. Preserve their token/literal and complete-statement semantics. Adapt the copies as follows, inside this task, rather than changing `tst_hotkeys` or shared support:

1. Add the following directive stripper. In the copied `selectionCode`, change only the initial token acquisition to `QStringList tokens = selectionTokens( stripPreprocessorDirectives( text ) );`. `selectionBlock`, statements, branches, counts and ordered needles then all normalise through this entry. `selectionTokens` itself remains a raw lexer for real include/directive inspection. No global whitespace deletion.
2. Keep a separate raw string for each source file; raw qmake parsing never passes through C++ normalisation. For preprocessor regions use a physical-line directive stack, matching the real opening `#ifdef ENABLE_SPWF_CUSTOMIZATION` to its closing `#endif`, retaining nested `#if` depth and both sides. Reject a missing/malformed region. Do not match a directive and assignment as one C++ statement.
3. Add `conditionalArms` below to partition a single top-level `if` chain, including its final `else` arm when present. Do not find the pinned-visible or xcb arm by searching arbitrary nested scopes. `selectionStatements` must retain the whole chain and lambda/initializer bodies as copied.
4. Replace the copied `selectionHas`, `selectionCount`, `selectionBlock`, and `selectionOrdered` bodies with the token-sequence implementations below. Their comparisons operate on whole lexer tokens, so executable-looking text inside one string/character literal cannot count as executable code. Keep the other copied helper bodies and their signatures.
5. Every source case first asserts non-empty real files, function scopes, relevant branch/lambda bodies and complete-statement lists. Missing anchors must fail, not return a successful empty check. Literal contents may match a literal argument but cannot count as executable actions.

```cpp
QString stripPreprocessorDirectives( QString const & text )
{
  QString result, rawClose;
  bool block = false, lineComment = false, continuedDirective = false;
  QChar quote;
  for( qsizetype begin = 0; begin < text.size(); )
  {
    qsizetype const newline = text.indexOf( '\n', begin );
    qsizetype const end = newline < 0 ? text.size() : newline;
    QString const line = text.mid( begin, end - begin );
    QString physical = line;
    if( physical.endsWith( '\r' ) ) physical.chop( 1 );
    qsizetype nonspace = 0;
    while( nonspace < physical.size() && physical[nonspace].isSpace() ) ++nonspace;
    bool const directive = continuedDirective ||
      ( !block && !lineComment && quote.isNull() && rawClose.isEmpty() &&
        nonspace < physical.size() && physical[nonspace] == '#' );
    bool const continuedLine = physical.endsWith( '\\' );
    for( qsizetype i = 0; i < physical.size(); ++i )
    {
      QChar const c = physical[i];
      QChar const next = i + 1 < physical.size() ? physical[i+1] : QChar{};
      if( lineComment ) break;
      if( block )
      {
        if( c == '*' && next == '/' ) { block = false; ++i; }
        continue;
      }
      if( !rawClose.isEmpty() )
      {
        if( physical.mid( i, rawClose.size() ) == rawClose )
        { i += rawClose.size() - 1; rawClose.clear(); }
        continue;
      }
      if( !quote.isNull() )
      {
        if( c == '\\' ) ++i;
        else if( c == quote ) quote = QChar{};
        continue;
      }
      if( c == '/' && next == '/' ) { lineComment = true; ++i; continue; }
      if( c == '/' && next == '*' ) { block = true; ++i; continue; }
      if( c == 'R' && next == '"' )
      {
        qsizetype const open = physical.indexOf( '(', i + 2 );
        if( open >= i + 2 && open - i - 2 <= 16 )
        { rawClose = ')' + physical.mid( i + 2, open - i - 2 ) + '"'; i = open; continue; }
      }
      if( c == '"' || c == '\'' ) quote = c;
    }
    if( directive ) result += '\n';
    else { result += line; if( newline >= 0 ) result += '\n'; }
    continuedDirective = directive && continuedLine;
    if( !continuedLine ) lineComment = false;
    begin = newline < 0 ? text.size() : newline + 1;
  }
  return result;
}
struct Arm { QString guard, body; };
QList< Arm > conditionalArms( QString const & chain )
{
  QStringList const tokens = selectionTokens( selectionCode( chain ) );
  QList< Arm > arms;
  qsizetype at = 0;
  while( at < tokens.size() )
  {
    bool const finalElse = tokens[at] == "else" &&
                           at + 1 < tokens.size() && tokens[at+1] != "if";
    if( tokens[at] == "else" ) ++at;
    if( finalElse )
    {
      qsizetype const end = selectionStatementEnd( tokens, at );
      if( end != tokens.size() || end <= at ) return {};
      QString body = tokens.mid( at, end-at ).join( ' ' );
      if( body.startsWith( "{ " ) ) body = selectionRawBlock( body, "{" );
      arms.append( { "else", body } );
      return arms;
    }
    if( at >= tokens.size() || tokens[at] != "if" ||
        at + 1 >= tokens.size() || tokens[at+1] != "(" ) return {};
    qsizetype conditionEnd = at + 1;
    int depth = 0;
    do {
      if( tokens[conditionEnd] == "(" ) ++depth;
      else if( tokens[conditionEnd] == ")" ) --depth;
      ++conditionEnd;
    } while( conditionEnd < tokens.size() && depth );
    if( depth || conditionEnd >= tokens.size() ) return {};
    qsizetype const end = selectionStatementEnd( tokens, conditionEnd );
    if( end <= conditionEnd ) return {};
    QString body = tokens.mid( conditionEnd, end - conditionEnd ).join( ' ' );
    if( body.startsWith( "{ " ) ) body = selectionRawBlock( body, "{" );
    arms.append( { tokens.mid( at, conditionEnd-at ).join( ' ' ), body } );
    at = end;
  }
  return arms;
}
```

The following replacements go after `selectionCode` and before their first callers. They keep the required signatures and use the same token/literal representation as the copied helpers:

```cpp
qsizetype selectionFindTokens( QStringList const & tokens, QStringList const & wanted,
                              qsizetype from = 0 )
{
  if( wanted.isEmpty() ) return -1;
  for( qsizetype i = from; i + wanted.size() <= tokens.size(); ++i )
    if( tokens.mid( i, wanted.size() ) == wanted ) return i;
  return -1;
}
bool selectionHas( QString const & text, QString const & needle )
{
  return selectionFindTokens( selectionTokens( selectionCode( text ) ),
                              selectionTokens( selectionCode( needle ) ) ) >= 0;
}
int selectionCount( QString const & text, QString const & needle )
{
  auto const tokens = selectionTokens( selectionCode( text ) );
  auto const wanted = selectionTokens( selectionCode( needle ) );
  int count = 0;
  for( qsizetype at = 0; ( at = selectionFindTokens( tokens, wanted, at ) ) >= 0;
       at += wanted.size() ) ++count;
  return count;
}
QString selectionBlock( QString const & text, QString const & anchor )
{
  auto const tokens = selectionTokens( selectionCode( text ) );
  auto const wanted = selectionTokens( selectionCode( anchor ) );
  qsizetype const at = selectionFindTokens( tokens, wanted );
  if( at < 0 ) return {};
  qsizetype open = wanted.last() == "{" ? at + wanted.size() - 1 : at + wanted.size();
  while( open < tokens.size() && tokens[open] != "{" ) ++open;
  if( open >= tokens.size() ) return {};
  int depth = 0;
  for( qsizetype end = open; end < tokens.size(); ++end )
  {
    if( tokens[end] == "{" ) ++depth;
    else if( tokens[end] == "}" && --depth == 0 )
      return tokens.mid( open + 1, end - open - 1 ).join( ' ' );
  }
  return {};
}
bool selectionOrdered( QString const & text, QStringList const & needles )
{
  auto const tokens = selectionTokens( selectionCode( text ) );
  qsizetype after = 0;
  for( auto const & needle : needles )
  {
    auto const wanted = selectionTokens( selectionCode( needle ) );
    qsizetype const at = selectionFindTokens( tokens, wanted, after );
    if( at < 0 ) return false;
    after = at + wanted.size();
  }
  return true;
}
```

Define `requiredBlock` and `requiredBranch` wrappers that call the corresponding selection helper, assert the result non-empty in a void lambda, then return it; the case must assert the returned value too. This is executable support code:

```cpp
QString requiredBlock( QString const & text, QString const & anchor )
{
  QString const body = selectionBlock( text, anchor );
  [&] { QVERIFY2( !body.isEmpty(), qPrintable( "Missing scope: " + anchor ) ); }();
  return body;
}
QString requiredBranch( QString const & text, QString const & condition )
{
  QString const body = selectionBranch( text, condition );
  [&] { QVERIFY2( !body.isEmpty(), qPrintable( "Missing branch: " + condition ) ); }();
  return body;
}
```

- [ ] **Step 2: Add the twelve runtime slots below, in the specification's ascending fixture order, before implementing their covered constructor/filter/classifier/handler/dismissal code.** Write each test first, run the suite to record RED before its covered production edit, then perform that edit and run GREEN. Initial missing API/linkage failure is TDD setup evidence only; the behavioural and source failures claimed for mutations must be the intended assertions under passing fixture preconditions (§6.7). No `QSKIP`, silent early success, injected own-focus signal, copied pure rule as a runtime handler, or `isActiveWindow()` focus proof is allowed.

```cpp
  void hidesWhenFocusMovesToAnotherWindow()
  {
    Windows f;
    QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    QTRY_COMPARE( f.count, 1 ); QVERIFY( !f.first.isVisible() );
    QCoreApplication::processEvents(); QCOMPARE( f.count, 1 );
  }
  void pinnedPopupDoesNotHide()
  {
    Windows f; f.closer->setPinned( true ); QVERIFY( f.closer->isPinned() );
    QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
  }
  void ownedMenuKeepsPopupUntilFocusMovesAway()
  {
    Windows f; QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QWidget menu( &f.first ); menu.setWindowFlags( Qt::Popup );
    QCOMPARE( menu.parentWidget(), &f.first );
    QVERIFY( activate( menu ) );
    QCOMPARE( QApplication::activePopupWidget(), &menu );
    QVERIFY( menu.windowHandle() ); QCOMPARE( menu.windowHandle()->type(), Qt::Popup );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
    QSignalSpy path( qApp, &QGuiApplication::focusWindowChanged ); QVERIFY( path.isValid() );
    menu.close();
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    QTRY_COMPARE( f.count, 1 ); QVERIFY( !f.first.isVisible() );
    for( auto const & payload : path )
      qInfo() << "menu-to-foreign focus payload" << qvariant_cast< QWindow * >( payload[0] );
    QVERIFY( !path.isEmpty() );
  }
  void nestedOwnedMenuKeepsPopupUntilFocusMovesAway()
  {
    Windows f; QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QWidget menu( &f.first ); menu.setWindowFlags( Qt::Popup );
    QVERIFY( activate( menu ) );
    QCOMPARE( menu.parentWidget(), &f.first );
    QCOMPARE( QApplication::activePopupWidget(), &menu );
    QVERIFY( menu.windowHandle() ); QCOMPARE( menu.windowHandle()->type(), Qt::Popup );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
    QWidget submenu( &menu ); submenu.setWindowFlags( Qt::Popup );
    QCOMPARE( submenu.parentWidget(), &menu );
    QVERIFY( activate( submenu ) );
    QCOMPARE( QApplication::activePopupWidget(), &submenu );
    QVERIFY( submenu.windowHandle() ); QCOMPARE( submenu.windowHandle()->type(), Qt::Popup );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
    submenu.close(); menu.close();
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    QTRY_COMPARE( f.count, 1 ); QVERIFY( !f.first.isVisible() );
  }
  void ownDialogHidesPopupAndRemainsVisible()
  {
    Windows f;
    QVERIFY( activate( f.first ) );
    QCOMPARE( f.first.windowHandle()->type(), Qt::Tool );
    QVERIFY( f.first.windowFlags().testFlag( Qt::FramelessWindowHint ) );
    QVERIFY( f.first.windowFlags().testFlag( Qt::WindowStaysOnTopHint ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QDialog dialog( &f.first ); dialog.setModal( true ); dialog.show();
    QVERIFY( dialog.isModal() ); QCOMPARE( dialog.parentWidget(), &f.first );
    QTRY_COMPARE( QApplication::activeModalWidget(), &dialog );
    QVERIFY( expectFocus( dialog ) );
    QCOMPARE( dialog.windowHandle()->type(), Qt::Dialog );
    QTRY_COMPARE( f.count, 1 ); QVERIFY( !f.first.isVisible() ); QVERIFY( dialog.isVisible() );
  }
  void focusReturningFromMenuKeepsPopup()
  {
    Windows f; QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QWidget menu( &f.first ); menu.setWindowFlags( Qt::Popup );
    QCOMPARE( menu.parentWidget(), &f.first );
    QVERIFY( activate( menu ) );
    QCOMPARE( QApplication::activePopupWidget(), &menu );
    QVERIFY( menu.windowHandle() ); QCOMPARE( menu.windowHandle()->type(), Qt::Popup );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
    menu.close(); QVERIFY( activate( f.first ) );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
  }
  void applicationInactiveSignalUsesProductionConnection()
  {
    for( int mode = 0; mode != 3; ++mode )
    {
      Windows f;
      f.closer->setPinned( mode == 1 ); QCOMPARE( f.closer->isPinned(), mode == 1 );
      if( mode == 2 )
      {
        QVERIFY( activate( f.second ) );
        f.first.setAttribute( Qt::WA_ShowWithoutActivating ); f.first.show();
        QVERIFY( f.first.windowHandle() ); QVERIFY( f.first.isVisible() );
        QVERIFY( expectFocus( f.second ) );
      }
      else QVERIFY( activate( f.first ) );
      QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
      QVERIFY( QMetaObject::invokeMethod(
        qApp, "applicationStateChanged",
        Q_ARG( Qt::ApplicationState, Qt::ApplicationInactive ) ) );
      QCOMPARE( f.count, mode == 0 ? 1 : 0 );
      QCOMPARE( f.first.isVisible(), mode != 0 );
    }
  }
  void reshowWithoutFocusStaysUnarmed()
  {
    Windows f; QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    f.first.hide(); QVERIFY( !f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    f.first.setAttribute( Qt::WA_ShowWithoutActivating ); f.first.show();
    QVERIFY( f.first.isVisible() ); QVERIFY( f.first.windowHandle() );
    QVERIFY( expectFocus( f.second ) ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.third ) );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
  }
  void showWhileAlreadyFocusedArmsPopup()
  {
    QWidget first( nullptr, managedFlags() ), second;
    QVERIFY( activate( first ) );
    Closer closer( &first ); int count = 0;
    closer.setDismissHandler( [&] { ++count; first.hide(); } );
    QSignalSpy focusSignals( qApp, &QGuiApplication::focusWindowChanged ); QVERIFY( focusSignals.isValid() );
    ShowProbe probe; probe.spy = &focusSignals; first.installEventFilter( &probe );
    first.hide(); first.show();
    QVERIFY2( probe.ownAtShow, "Already-focused Show assumption: native handle disappeared" );
    QVERIFY2( probe.focusAtShow == probe.ownAtShow,
              "Already-focused Show assumption: QPA cleared own focus on Hide/Show" );
    QVERIFY( probe.spyCountAtShow >= 0 );
    QVERIFY( first.isVisible() ); QCOMPARE( count, 0 );
    for( auto const & payload : focusSignals )
      QVERIFY2( qvariant_cast< QWindow * >( payload[0] ) != probe.ownAtShow,
                "Already-focused Show assumption: an own-focus signal independently armed the closer" );
    QVERIFY( activate( second ) );
    for( auto const & payload : focusSignals )
      QVERIFY2( qvariant_cast< QWindow * >( payload[0] ) != probe.ownAtShow,
                "An own-focus signal armed the closer before the tested loss" );
    QTRY_COMPARE( count, 1 ); QVERIFY( !first.isVisible() );
    first.removeEventFilter( &probe );
  }
  void dismissHandlerIsNotReentered()
  {
    Windows f; bool inHandler = false, nestedForeign = false, driven = false;
    QObject observer;
    QObject::connect( qApp, &QGuiApplication::focusWindowChanged, &observer,
      [&]( QWindow * destination ) {
        if( inHandler && destination == f.second.windowHandle() ) nestedForeign = true;
      } );
    f.closer->setDismissHandler( [&] {
      ++f.count;
      if( f.count != 1 ) return; // A broken guard fails finitely.
      inHandler = true;
      driven = activate( f.first ) && activate( f.second ) && activate( f.first );
      inHandler = false;
    } );
    QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    f.second.show(); f.second.activateWindow(); f.second.raise();
    QTRY_COMPARE( f.count, 1 );
    QVERIFY( driven ); QVERIFY2( nestedForeign, "No eligible Foreign event delivered inside callback" );
    QVERIFY( expectFocus( f.first ) ); QCOMPARE( f.count, 1 );
  }
  void dismissHandlerMayDestroyCloser()
  {
    Windows f; QPointer< Closer > observed = f.closer.get();
    f.closer->setDismissHandler( [&] { ++f.count; f.closer.reset(); } );
    QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    QTRY_COMPARE( f.count, 1 ); QVERIFY( observed.isNull() );
    QVERIFY( !f.closer ); QVERIFY( f.first.isVisible() );
    QVERIFY( activate( f.first ) ); QVERIFY( activate( f.second ) ); QCOMPARE( f.count, 1 );
  }
  void destroyingArmedCloserDisconnectsSignals()
  {
    Windows f; QPointer< Closer > observed = f.closer.get();
    QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    f.closer.reset(); QVERIFY( observed.isNull() ); QVERIFY( f.first.isVisible() );
    QVERIFY( activate( f.second ) ); QVERIFY( activate( f.first ) ); QVERIFY( activate( f.second ) );
    QVERIFY( QMetaObject::invokeMethod(
      qApp, "applicationStateChanged",
      Q_ARG( Qt::ApplicationState, Qt::ApplicationInactive ) ) );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
  }
```

In the dialog case, compare the type bits/hints actually specified rather than requiring Qt to preserve unrelated implicit decoration bits; `setWindowFlags( managedFlags() )` is the setup, and `first.windowHandle()->type() == Qt::Tool` after Show pins its managed form. Use `dialog.show()`, never `exec()`, and never activate a sibling while that modal dialog owns the application. The close handler uses the common hide path semantics; it must not reject the dialog.

The menu spy records every payload from the asserted menu focus through the asserted second-window focus, including nulls and returns to the owner. The nested test asserts zero after both openings. Close only the owned popups as needed to let xcb change focus, then explicitly assert the destination. These runtime fixtures do not isolate clearing arming in the OwnPopup branch; the source case below does. Likewise the returning-menu case makes no asynchronous/timer or missing-rearming kill claim. The re-show case hides directly before moving focus, and the independent source case pins isolated Hide and Show mutations. The last two runtime cases are destruction-safety checks, without a claimed receiver-lifetime or freed-write mutation discriminator. Do not count an environmental or focus-assumption failure as a behavioural kill.

- [ ] **Step 3: Add these five source slots and observe their RED assertions before adding the production class/event code they pin.** Place them before the test class closes. The snippets use the copied/adapted helpers above. Validate the stripper itself inside the first slot using ordinary literal/comment preservation and both continuation newline conventions; these are assertions in that slot, not extra substantive cases.

```cpp
  void closerApplicationStateConnectionIsContextBound()
  {
    QString const directives = "#if A\n#define X \\\n one\n#endif\n#if B\r\n#define Y \\\r\n two\r\n#endif\r\n";
    QCOMPARE( stripPreprocessorDirectives( directives ), QString( 8, '\n' ) );
    QString const ordinary = "const char *s = \"#if literal\";\n// #if comment\n/*\n#if comment\n*/\nchar c = '#';\n";
    QVERIFY( !selectionHas( "auto s = \" activateWindow ( ) ; \";", "activateWindow();" ) );
    QVERIFY( !selectionOrdered( "auto s = \" activateWindow ( ) ; \";", { "activateWindow();" } ) );
    QVERIFY( selectionBlock( "auto s = \" FocusLossCloser :: dismiss { armed = false ; } \";",
                             "FocusLossCloser::dismiss" ).isEmpty() );
    QCOMPARE( stripPreprocessorDirectives( ordinary ), ordinary );
    QString const file = source( "popupdismiss.cc" ); QVERIFY( !file.isEmpty() );
    QString const ctor = requiredBlock( file, "FocusLossCloser::FocusLossCloser" ); QVERIFY( !ctor.isEmpty() );
    QVERIFY( selectionHas( ctor, "focusConnection = QObject::connect( qApp, &QGuiApplication::focusWindowChanged, this," ) );
    QVERIFY( selectionHas( ctor, "stateConnection = QObject::connect( qApp, &QGuiApplication::applicationStateChanged, this," ) );
    QString const state = requiredBlock( ctor, "[this]( Qt::ApplicationState state )" ); QVERIFY( !state.isEmpty() );
    QStringList const statements = selectionStatements( state ); QVERIFY( statements.size() >= 4 );
    QCOMPARE( statements[0], selectionCode( "if( state != Qt::ApplicationInactive ) return;" ) );
    QCOMPARE( statements[1], selectionCode( "if( !window ) return;" ) );
    QString const ignore = requiredBranch( state,
      "if( onFocusMoved( window->isVisible(), pinned, armed, FocusDestination::None, false ) == Action::Ignore )" );
    QVERIFY( !ignore.isEmpty() ); QVERIFY( selectionHas( ignore, "return;" ) );
    QVERIFY( !selectionHas( state, "applicationState(" ) );
    QVERIFY( !selectionHas( state, "focusWindow(" ) );
    QCOMPARE( selectionCount( state, "dismiss(" ), 1 );
    QVERIFY( selectionOrdered( selectionDirectActions( state ),
                              { "dismiss( \"application inactive\" );", "return;" } ) );
    QString const focus = requiredBlock( ctor, "[this]( QWindow * focusWindow )" ); QVERIFY( !focus.isEmpty() );
    auto const focusStatements = selectionStatements( focus ); QVERIFY( !focusStatements.isEmpty() );
    QCOMPARE( focusStatements[0], selectionCode( "if( !window ) return;" ) );
    auto const teardown = selectionStatements( requiredBlock( file, "FocusLossCloser::~FocusLossCloser" ) );
    QCOMPARE( teardown.size(), 3 );
    QCOMPARE( teardown[0], selectionCode( "QObject::disconnect( focusConnection );" ) );
    QCOMPARE( teardown[1], selectionCode( "QObject::disconnect( stateConnection );" ) );
    QCOMPARE( teardown[2], selectionCode( "if( window ) window->removeEventFilter( this );" ) );
    QCOMPARE( selectionStatements( requiredBlock( file, "FocusLossCloser::setPinned" ) ),
              QStringList{ selectionCode( "this->pinned = pinned;" ) } );
    QCOMPARE( selectionStatements( requiredBlock( file, "FocusLossCloser::isPinned" ) ),
              QStringList{ selectionCode( "return pinned;" ) } );
    QCOMPARE( selectionStatements( requiredBlock( file, "FocusLossCloser::setDismissHandler" ) ),
              QStringList{ selectionCode( "dismissHandler = std::move( handler );" ) } );
  }
  void closerShowAndHideMaintainArming()
  {
    QString const file = source( "popupdismiss.cc" ); QVERIFY( !file.isEmpty() );
    QString const filter = requiredBlock( file, "FocusLossCloser::eventFilter" ); QVERIFY( !filter.isEmpty() );
    QString const observed = requiredBranch( filter, "if( window && watched == window.data() )" ); QVERIFY( !observed.isEmpty() );
    auto const chain = selectionStatements( observed ); QCOMPARE( chain.size(), 1 );
    auto const arms = conditionalArms( chain.first() ); QCOMPARE( arms.size(), 2 );
    QCOMPARE( arms[0].guard, selectionCode( "if( event->type() == QEvent::Show )" ) );
    QCOMPARE( arms[1].guard, selectionCode( "if( event->type() == QEvent::Hide )" ) );
    QCOMPARE( selectionStatements( arms[0].body ), QStringList{ selectionCode(
      "armed = PopupDismiss::armedAfterShow( window->windowHandle(), QGuiApplication::focusWindow() );" ) } );
    QCOMPARE( selectionStatements( arms[1].body ), QStringList{ selectionCode( "armed = false;" ) } );
    QCOMPARE( selectionDirectActions( filter ), selectionCode( "return QObject::eventFilter( watched, event );" ) );
    QCOMPARE( selectionCount( filter, "return" ), 1 );
  }
  void destinationClassifierUsesOwnedPopupIdentityAndType()
  {
    QString const file = source( "popupdismiss.cc" ); QVERIFY( !file.isEmpty() );
    QString const body = requiredBlock( file, "FocusLossCloser::destinationOf" ); QVERIFY( !body.isEmpty() );
    QVERIFY( selectionOrdered( body, {
      "if( !window ) return FocusDestination::Foreign;",
      "if( !focusWindow ) return FocusDestination::None;",
      "if( focusWindow == window->windowHandle() ) return FocusDestination::OwnWindow;",
      "QWidget * const activePopup = QApplication::activePopupWidget();",
      "if( isOwnedPopup( window, activePopup ) )",
      "QWindow * const popupHandle = activePopup->windowHandle();",
      "if( popupHandle && focusWindow == popupHandle && isPopupWindowType( focusWindow->type() ) )",
      "return FocusDestination::OwnPopup;", "return FocusDestination::Foreign;" } ) );
    QString const owned = requiredBranch( body, "if( isOwnedPopup( window, activePopup ) )" ); QVERIFY( !owned.isEmpty() );
    auto const direct = selectionStatements( owned ); QCOMPARE( direct.size(), 2 );
    QCOMPARE( direct[0], selectionCode( "QWindow * const popupHandle = activePopup->windowHandle();" ) );
    QString const typed = requiredBranch( owned,
      "if( popupHandle && focusWindow == popupHandle && isPopupWindowType( focusWindow->type() ) )" );
    QVERIFY( !typed.isEmpty() );
    QCOMPARE( selectionStatements( typed ), QStringList{ selectionCode( "return FocusDestination::OwnPopup;" ) } );
    auto const statements = selectionStatements( body ); QVERIFY( !statements.isEmpty() );
    QCOMPARE( statements.last(), selectionCode( "return FocusDestination::Foreign;" ) );
    for( auto const & forbidden : { "transientParent(", "->parent(", "applicationState(", "isActiveWindow(", "winId(" } )
      QVERIFY2( !selectionHas( body, forbidden ), forbidden );
  }
  void ownPopupBranchPreservesArming()
  {
    QString const file = source( "popupdismiss.cc" ); QVERIFY( !file.isEmpty() );
    QString const ctor = requiredBlock( file, "FocusLossCloser::FocusLossCloser" ); QVERIFY( !ctor.isEmpty() );
    QString const focus = requiredBlock( ctor, "[this]( QWindow * focusWindow )" ); QVERIFY( !focus.isEmpty() );
    QString const popup = requiredBranch( focus, "if( destination == FocusDestination::OwnPopup )" ); QVERIFY( !popup.isEmpty() );
    QVERIFY( !selectionStatements( popup ).isEmpty() );
    for( QString const & token : selectionTokens( popup ) )
    {
      QVERIFY( token != "armed" );
      QVERIFY( !token.contains( "cancel", Qt::CaseInsensitive ) );
      QVERIFY( !token.contains( "clear", Qt::CaseInsensitive ) );
    }
    for( auto const & forbidden : { "dismiss(", "hide(", "show(", "setVisible(", "setHidden(" } )
      QVERIFY2( !selectionHas( popup, forbidden ), forbidden );
    QVERIFY( selectionHas( popup, "return;" ) );
  }
  void dismissInvokesFreshHandlerBehindSelfGuard()
  {
    QString const file = source( "popupdismiss.cc" ); QVERIFY( !file.isEmpty() );
    QString const body = requiredBlock( file, "FocusLossCloser::dismiss" ); QVERIFY( !body.isEmpty() );
    QStringList const statements = selectionStatements( body ); QVERIFY( statements.size() >= 8 );
    QCOMPARE( statements[0], selectionCode( "if( dismissing ) return;" ) );
    QCOMPARE( statements[1], selectionCode( "dismissing = true;" ) );
    QCOMPARE( statements[2], selectionCode( "QPointer< FocusLossCloser > self( this );" ) );
    QVERIFY( selectionOrdered( body, {
      "dismissing = true;", "QPointer< FocusLossCloser > self( this );",
      "auto handler = dismissHandler;", "if( handler ) handler();",
      "if( !self ) return;", "dismissing = false;", "armed = false;" } ) );
    QCOMPARE( selectionCount( body, "handler();" ), 1 );
    QCOMPARE( selectionCount( body, "dismissHandler" ), 1 );
    auto const tokens = selectionTokens( selectionCode( body ) );
    qsizetype const callAt = selectionFindTokens( tokens, selectionTokens( selectionCode( "handler();" ) ) );
    QVERIFY( callAt >= 0 );
    QString const after = tokens.mid( callAt + selectionTokens( selectionCode( "handler();" ) ).size() ).join( ' ' );
    auto const tail = selectionStatements( after ); QCOMPARE( tail.size(), 3 );
    QCOMPARE( tail[0], selectionCode( "if( !self ) return;" ) );
    QCOMPARE( tail[1], selectionCode( "dismissing = false;" ) );
    QCOMPARE( tail[2], selectionCode( "armed = false;" ) );
    QVERIFY( !selectionHas( after, "window" ) );
    QVERIFY( !selectionHas( body, "hide(" ) );
  }
```

- [ ] **Step 4: Run RED before adding the closer class and lifecycle definitions.** Use exactly these focused commands; record the expected missing class/API failure separately from source-assertion and runtime failures. Never present a compile, import, timeout, skipped fixture, or failure to obtain a required handle as the §9.2 behavioural discriminator. Once a runnable API exists, a named assertion must fail before its covered event-model correction; tests that already pass need an isolated mechanism mutation with a passing control, not a fabricated RED claim.

```bash
qmake6 tests/tst_popup_dismiss/tst_popup_dismiss.pro -o tests/tst_popup_dismiss/Makefile
make -C tests/tst_popup_dismiss -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_popup_dismiss/tst_popup_dismiss
```

- [ ] **Step 5: Complete the §2.1 header exactly and implement §3.** Insert this class before the namespace closes; retain the guard, direct includes, forward declarations and pure declarations already present. The constructor's optional `parent` is independent from the borrowed widget.

```cpp
class FocusLossCloser : public QObject {
  Q_OBJECT
public:
  explicit FocusLossCloser( QWidget * window, QObject * parent = nullptr );
  ~FocusLossCloser() override;
  void setPinned( bool pinned );
  bool isPinned() const;
  void setDismissHandler( std::function< void() > handler );
protected:
  bool eventFilter( QObject * watched, QEvent * event ) override;
private:
  FocusDestination destinationOf( QWindow const * focusWindow ) const;
  void dismiss( char const * reason );
  QPointer< QWidget > window;
  bool pinned = false;
  bool armed = false;
  bool dismissing = false;
  QMetaObject::Connection focusConnection;
  QMetaObject::Connection stateConnection;
  std::function< void() > dismissHandler;
};
```

Place the category outside the namespace, after the includes:

```cpp
Q_LOGGING_CATEGORY( popupDismissLc, "goldendict.popup.dismiss" )
```

Append the following definitions inside `namespace PopupDismiss`. There are exactly two stored context-bound subscriptions, and no queued work. If a callback deletes either closer or observed widget, the local callback and self-guard preserve the mandated lifetimes. Empty callbacks never cause fallback hiding.

```cpp
FocusLossCloser::FocusLossCloser( QWidget * observedWindow, QObject * parent )
  : QObject( parent ), window( observedWindow )
{
  if( this->window ) this->window->installEventFilter( this );
  focusConnection = QObject::connect(
    qApp, &QGuiApplication::focusWindowChanged, this,
    [this]( QWindow * focusWindow ) {
      if( !window ) return;
      FocusDestination const destination = destinationOf( focusWindow );
      if( destination == FocusDestination::OwnWindow )
      {
        armed = true;
        qCDebug( popupDismissLc ) << "onFocusMoved OwnWindow Ignore" << "own focus";
        return;
      }
      if( destination == FocusDestination::OwnPopup )
      {
        qCDebug( popupDismissLc ) << "onFocusMoved OwnPopup Ignore" << "own popup";
        return;
      }
      bool const applicationActive = qApp->applicationState() == Qt::ApplicationActive;
      bool const visible = window->isVisible();
      if( onFocusMoved( visible, pinned, armed, destination, applicationActive ) == Action::Ignore )
      {
        char const * const why = !visible ? "hidden" : pinned ? "pinned" :
                                !armed ? "unarmed" : "None with active application";
        qCDebug( popupDismissLc ) << "onFocusMoved"
          << ( destination == FocusDestination::Foreign ? "Foreign" : "None" ) << "Ignore" << why;
        return;
      }
      dismiss( destination == FocusDestination::Foreign ? "focus moved to another window"
                                                       : "focus left the application" );
      return;
    } );
  stateConnection = QObject::connect(
    qApp, &QGuiApplication::applicationStateChanged, this,
    [this]( Qt::ApplicationState state ) {
      if( state != Qt::ApplicationInactive ) return;
      if( !window ) return;
      if( onFocusMoved( window->isVisible(), pinned, armed, FocusDestination::None, false ) == Action::Ignore )
      {
        char const * const why = !window->isVisible() ? "hidden" : pinned ? "pinned" : "unarmed";
        qCDebug( popupDismissLc ) << "applicationStateChanged None Ignore" << why;
        return;
      }
      dismiss( "application inactive" );
      return;
    } );
}
FocusLossCloser::~FocusLossCloser()
{
  QObject::disconnect( focusConnection );
  QObject::disconnect( stateConnection );
  if( window ) window->removeEventFilter( this );
}
void FocusLossCloser::setPinned( bool pinned ) { this->pinned = pinned; }
bool FocusLossCloser::isPinned() const { return pinned; }
void FocusLossCloser::setDismissHandler( std::function< void() > handler )
{
  dismissHandler = std::move( handler );
}
bool FocusLossCloser::eventFilter( QObject * watched, QEvent * event )
{
  if( window && watched == window.data() )
  {
    if( event->type() == QEvent::Show )
      armed = PopupDismiss::armedAfterShow( window->windowHandle(), QGuiApplication::focusWindow() );
    else if( event->type() == QEvent::Hide )
      armed = false;
  }
  return QObject::eventFilter( watched, event );
}
FocusDestination FocusLossCloser::destinationOf( QWindow const * focusWindow ) const
{
  if( !window ) return FocusDestination::Foreign;
  if( !focusWindow ) return FocusDestination::None;
  if( focusWindow == window->windowHandle() ) return FocusDestination::OwnWindow;
  QWidget * const activePopup = QApplication::activePopupWidget();
  if( isOwnedPopup( window, activePopup ) )
  {
    QWindow * const popupHandle = activePopup->windowHandle();
    if( popupHandle && focusWindow == popupHandle && isPopupWindowType( focusWindow->type() ) )
      return FocusDestination::OwnPopup;
  }
  return FocusDestination::Foreign;
}
void FocusLossCloser::dismiss( char const * reason )
{
  if( dismissing ) return;
  dismissing = true;
  QPointer< FocusLossCloser > self( this );
  bool const fromState = qstrcmp( reason, "application inactive" ) == 0;
  bool const fromForeign = qstrcmp( reason, "focus moved to another window" ) == 0;
  qCDebug( popupDismissLc ) << ( fromState ? "applicationStateChanged" : "onFocusMoved" )
    << ( fromForeign ? "Foreign" : "None" ) << "Hide" << reason;
  auto handler = dismissHandler;
  if( handler ) handler();
  if( !self ) return;
  dismissing = false;
  armed = false;
}
```

- [ ] **Step 6: Run GREEN and verify the bounded claims.** Re-run the three focused commands from Step 4, requiring all 22 owned substantive slots so far and their data rows to pass, every runtime precondition to execute, and no skips. Review null observed-widget construction, widget-destroyed-before-closer and empty/replaced-handler paths directly against the full source; no additional public seam or additional substantive slot is authorised. Verify the header's self-contained includes, no constructor arming/materialisation, no activation/event overrides/timer/parent-window traversal, setters with no extra side effects, and exactly one Hide log from inside guarded `dismiss()`, with no title/lookup/token data. Logs cannot prove refusal or granted activation.

For an already-green assertion, preserve a passing control and restore after each isolated targeted source mutation; the evidence record must name the mutation, actual failing assertion, intended precondition, and restored GREEN. Honour §6.7/§9.2: type-check-only, OwnPopup arming, post-callback self-guard and receiver-context mutations belong to their source assertions; redundant disconnect deletion and equivalent conjunction ordering have no claimed runtime kill. Do not add a 30th slot for the evidence ledger or edit the pinned plan.

- [ ] **Step 7: Commit** only the three modified files with this exact message:

```bash
git add popupdismiss.hh popupdismiss.cc tests/tst_popup_dismiss/tst_popup_dismiss.cc
git commit -m "feat(popup): add guarded synchronous focus loss closer"
```

## Task 3: Wire the closer and opening requests into ScanPopup

**Implementer tier:** Advanced

**Lane/category:** popup-dismiss · backend

**Same-lane predecessor:** Task 2 — Implement and prove the closer event lifecycle.

**Files:**

- Modify: `scanpopup.hh:13-20` — after `#include "mainstatusbar.hh"`, add the project include and explicit `<memory>`; `scanpopup.hh:139-144`, class `ScanPopup`, immediately after `Ui::ScanPopup ui;`, add the unique owner.
- Modify: `scanpopup.cc:53-84` — `ScanPopup::ScanPopup`, immediately after `ui.setupUi( this );`; `scanpopup.cc:188-204`, same constructor, immediately after `ui.pinButton->setChecked( cfg.pinPopupWindow );` and before the first flags.
- Modify: `scanpopup.cc:605-720` — `void ScanPopup::engagePopup( bool forcePopup, bool giveFocus )`, only the hidden arm's existing `if ( giveFocus )` condition after `show();` and the preserved customization block.
- Modify: `scanpopup.cc:1077-1110` — `void ScanPopup::pinButtonClicked( bool checked )`, first complete statement and the gap between shared `show();` and final geometry-save conditional. Baseline line numbers move after earlier insertions; these exact functions/anchors identify the edits.
- Modify: `tests/tst_popup_dismiss/tst_popup_dismiss.cc` — anonymous namespace before `class TstPopupDismiss`, add raw-source helpers below; append the five inline source slots before the test class's closing `};`.

**Interfaces:**

- Consumes (§2.4, Task 2): `explicit PopupDismiss::FocusLossCloser( QWidget * window, QObject * parent = nullptr )`, `~FocusLossCloser() override`, `void PopupDismiss::FocusLossCloser::setPinned( bool pinned )`, `void PopupDismiss::FocusLossCloser::setDismissHandler( std::function< void() > handler )`, uniquely owned as `std::unique_ptr< PopupDismiss::FocusLossCloser > focusLossCloser`; one constructor argument is the observed widget, not QObject parent.
- Consumes (§2.2, Task 1): `bool PopupDismiss::needsExplicitActivation( Qt::WindowFlags flags )`.
- Consumes (§4, pre-existing): `void ScanPopup::hideWindow()`, `Qt::WindowFlags ScanPopup::unpinnedWindowFlags() const`, `void ScanPopup::pinButtonClicked( bool checked )`, `void ScanPopup::engagePopup( bool forcePopup, bool giveFocus = false )` (default only in the declaration), `Qt::WindowFlags QWidget::windowFlags() const`, `void QWidget::show()`, `void QWidget::activateWindow()`, `void QWidget::raise()`.
- Consumes (§6.5, Task 2, suite-local): `QString source( char const * path )`, `QString requiredBlock( QString const & text, QString const & anchor )`, `QString requiredBranch( QString const & text, QString const & condition )`, `QString selectionCode( QString const & text )`, `QStringList selectionTokens( QString const & text )`, `QStringList selectionStatements( QString const & text )`, `QString selectionDirectActions( QString const & text )`, `bool selectionHas( QString const & text, QString const & needle )`, `int selectionCount( QString const & text, QString const & needle )`, `bool selectionOrdered( QString const & text, QStringList const & needles )`, `QString selectionRawBlock( QString const & text, QString const & anchor )`, `QList< Arm > conditionalArms( QString const & chain )` with `struct Arm { QString guard, body; };`, and `QString stripPreprocessorDirectives( QString const & text )`, and `qsizetype selectionFindTokens( QStringList const & tokens, QStringList const & wanted, qsizetype from = 0 )`.
- Produces (§4): owned closer immediately after UI setup, callback exactly to `hideWindow()`, pin synchronization before flags, widened hidden-only activation guard and guarded post-Show unpin request. All existing caller signatures, arguments, flags, pointer timers/callers, visible-window branches, token paths and `escapePressed()` semantics remain as specified by Global Constraints.

**Implementation steps:**

- [ ] **Step 1: Add robust raw-source support within the new suite.** Implement these helpers before the five new slots. They select real code-token anchors, preserving the original physical lines for directive tests. Use a token stream with source offsets so a comment or string containing a function name or include cannot supply an executable anchor. Retain all required non-empty-scope assertions.

```cpp
QString rawCodeBlock( QString const & text, QString const & anchor )
{
  QRegularExpression const lexer( R"cpp("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|//[^\n]*|/\*[\s\S]*?\*/|[A-Za-z_][A-Za-z_0-9]*|[0-9]+|::|->|\+\+|--|&&|\|\||==|!=|<=|>=|\+=|-=|<<|>>|\S)cpp" );
  QStringList tokens; QList< qsizetype > starts, ends;
  auto matches = lexer.globalMatch( text );
  while( matches.hasNext() )
  {
    auto const match = matches.next(); QString const token = match.captured();
    if( token.startsWith( "//" ) || token.startsWith( "/*" ) ) continue;
    tokens.append( token ); starts.append( match.capturedStart() ); ends.append( match.capturedEnd() );
  }
  QStringList const wanted = selectionTokens( selectionCode( anchor ) );
  if( wanted.isEmpty() ) return {};
  for( qsizetype i = 0; i + wanted.size() <= tokens.size(); ++i )
    if( tokens.mid( i, wanted.size() ) == wanted )
    {
      qsizetype open = i + wanted.size();
      while( open < tokens.size() && tokens[open] != "{" ) ++open;
      if( wanted.last() == "{" ) open = i + wanted.size() - 1;
      if( open >= tokens.size() ) return {};
      int depth = 0;
      for( qsizetype close = open; close < tokens.size(); ++close )
      {
        if( tokens[close] == "{" ) ++depth;
        else if( tokens[close] == "}" && --depth == 0 )
          return text.mid( ends[open], starts[close]-ends[open] );
      }
      return {};
    }
  return {};
}
QString directiveRegion( QString const & raw, QString const & opening )
{
  auto const physical = raw.split( '\n' );
  auto const stripped = stripPreprocessorDirectives( raw ).split( '\n' );
  if( physical.size() != stripped.size() ) return {};
  int depth = 0; bool found = false; QString result;
  for( qsizetype i = 0; i < physical.size(); ++i )
  {
    QString const line = physical[i].trimmed();
    bool const directive = line.startsWith( '#' ) && stripped[i].isEmpty();
    if( !found )
    {
      if( directive && line == opening ) { found = true; depth = 1; }
      continue;
    }
    if( directive )
    {
      if( QRegularExpression( "^#\\s*(if|ifdef|ifndef)\\b" ).match( line ).hasMatch() ) ++depth;
      else if( QRegularExpression( "^#\\s*endif\\b" ).match( line ).hasMatch() && --depth == 0 ) return result;
    }
    result += physical[i] + '\n';
  }
  return {}; // Missing or unmatched actual directive is a test failure at the caller.
}
bool hasInclude( QString const & raw, QString const & token )
{
  auto const tokens = selectionTokens( raw );
  auto const wanted = selectionTokens( "#include " + token );
  for( qsizetype i = 0; i + wanted.size() <= tokens.size(); ++i )
    if( tokens.mid( i, wanted.size() ) == wanted ) return true;
  return false;
}
```

- [ ] **Step 2: Write the five named source tests first, then run their RED suite before editing ScanPopup.** All checks below read actual repository files through the existing helper. Constructor order is a single six-needle expectation plus independent first-statement/first-flag checks; the pin test compares the first complete statement, not the first non-control action. The engage test partitions one conditional chain and separately attributes all three activation calls.

```cpp
  void scanPopupDeclaresCloserWithoutEventOverrides()
  {
    QString const header = source( "scanpopup.hh" ), file = source( "scanpopup.cc" );
    QVERIFY( !header.isEmpty() ); QVERIFY( !file.isEmpty() );
    QVERIFY( hasInclude( header, "\"popupdismiss.hh\"" ) );
    QVERIFY( hasInclude( header, "<memory>" ) );
    QString const declaration = requiredBlock( header, "class ScanPopup" ); QVERIFY( !declaration.isEmpty() );
    QVERIFY( selectionHas( declaration, "std::unique_ptr< PopupDismiss::FocusLossCloser > focusLossCloser;" ) );
    QVERIFY( !selectionHas( declaration, "event(" ) ); QVERIFY( !selectionHas( declaration, "changeEvent(" ) );
    QVERIFY( !selectionHas( file, "ScanPopup::event(" ) ); QVERIFY( !selectionHas( file, "ScanPopup::changeEvent(" ) );
    QVERIFY( selectionHas( declaration, "eventFilter(" ) );
    QVERIFY( selectionHas( file, "ScanPopup::eventFilter(" ) );
  }
  void constructorRestoresPinBeforeChangingFlags()
  {
    QString const file = source( "scanpopup.cc" ); QVERIFY( !file.isEmpty() );
    QString const ctor = requiredBlock( file, "ScanPopup::ScanPopup" ); QVERIFY( !ctor.isEmpty() );
    QString const setup = "ui.setupUi( this );";
    QString const create = "focusLossCloser = std::make_unique< PopupDismiss::FocusLossCloser >( this );";
    QString const handler = "focusLossCloser->setDismissHandler( [ this ] { hideWindow(); } );";
    QString const restore = "ui.pinButton->setChecked( cfg.pinPopupWindow );";
    QString const sync = "focusLossCloser->setPinned( ui.pinButton->isChecked() );";
    QVERIFY( selectionOrdered( ctor, { setup, create, handler, restore, sync, "setWindowFlags(" } ) );
    QStringList const statements = selectionStatements( ctor ); QVERIFY( statements.size() >= 3 );
    QCOMPARE( statements[0], selectionCode( setup ) );
    QCOMPARE( statements[1], selectionCode( create ) );
    QCOMPARE( statements[2], selectionCode( handler ) );
    auto const tokens = selectionTokens( selectionCode( ctor ) );
    qsizetype const restored = selectionFindTokens( tokens, selectionTokens( selectionCode( restore ) ) );
    qsizetype const synchronised = selectionFindTokens( tokens, selectionTokens( selectionCode( sync ) ) );
    qsizetype const firstFlags = selectionFindTokens( tokens, selectionTokens( selectionCode( "setWindowFlags(" ) ) );
    QVERIFY( restored >= 0 ); QVERIFY( synchronised > restored ); QVERIFY( firstFlags > synchronised );
    QCOMPARE( selectionCount( ctor, "focusLossCloser->setPinned(" ), 1 );
    QVERIFY( !selectionHas( ctor, "escapePressed();" ) );
  }
  void pinButtonSynchronizesBeforeFlags()
  {
    QString const file = source( "scanpopup.cc" ); QVERIFY( !file.isEmpty() );
    QString const body = requiredBlock( file, "ScanPopup::pinButtonClicked" ); QVERIFY( !body.isEmpty() );
    auto const statements = selectionStatements( body ); QVERIFY( statements.size() >= 5 );
    QCOMPARE( statements.first(), selectionCode( "focusLossCloser->setPinned( checked );" ) );
    auto const arms = conditionalArms( statements[1] ); QCOMPARE( arms.size(), 2 );
    QCOMPARE( arms[0].guard, selectionCode( "if( checked )" ) ); QCOMPARE( arms[1].guard, QString( "else" ) );
    QVERIFY( !arms[0].body.isEmpty() ); QVERIFY( !arms[1].body.isEmpty() );
    QVERIFY( selectionHas( arms[0].body, "setWindowFlags( flags );" ) );
    QVERIFY( selectionHas( arms[1].body, "setWindowFlags( unpinnedWindowFlags() );" ) );
    QCOMPARE( statements[2], selectionCode( "show();" ) );
  }
  void engagePopupPreservesScopedActivationPaths()
  {
    QString const file = source( "scanpopup.cc" ); QVERIFY( !file.isEmpty() );
    QString const body = requiredBlock( file, "ScanPopup::engagePopup" ); QVERIFY( !body.isEmpty() );
    QString chain;
    for( QString const & statement : selectionStatements( body ) )
      if( statement.startsWith( selectionCode( "if( !isVisible() )" ) + ' ' ) )
      { QVERIFY( chain.isEmpty() ); chain = statement; }
    QVERIFY( !chain.isEmpty() );
    auto const arms = conditionalArms( chain ); QCOMPARE( arms.size(), 3 );
    QCOMPARE( arms[0].guard, selectionCode( "if( !isVisible() )" ) );
    QCOMPARE( arms[1].guard, selectionCode( "if( ui.pinButton->isChecked() )" ) );
    QCOMPARE( arms[2].guard, selectionCode(
      "if( QGuiApplication::platformName() == \"xcb\" && ( windowFlags() & Qt::Tool ) == Qt::Tool )" ) );
    for( auto const & arm : arms ) QVERIFY( !arm.body.isEmpty() );
    QString const guard = "if( giveFocus || ( !ui.pinButton->isChecked() && PopupDismiss::needsExplicitActivation( windowFlags() ) ) )";
    QString const request = requiredBranch( arms[0].body, guard ); QVERIFY( !request.isEmpty() );
    QCOMPARE( selectionStatements( request ), QStringList( { selectionCode( "activateWindow();" ), selectionCode( "raise();" ) } ) );
    QCOMPARE( selectionDirectActions( request ), selectionCode( "activateWindow(); raise();" ) );
    auto const hiddenStatements = selectionStatements( arms[0].body ); QVERIFY( !hiddenStatements.isEmpty() );
    qsizetype show = -1, requestAt = -1;
    for( qsizetype i = 0; i < hiddenStatements.size(); ++i )
    {
      if( hiddenStatements[i] == selectionCode( "show();" ) ) { QVERIFY( show < 0 ); show = i; }
      if( hiddenStatements[i].startsWith( selectionCode( guard ) + ' ' ) ) { QVERIFY( requestAt < 0 ); requestAt = i; }
    }
    QVERIFY( show >= 0 ); QVERIFY( requestAt > show );
    QCOMPARE( selectionCount( body, "activateWindow(" ), 3 );
    for( auto const & arm : arms ) QCOMPARE( selectionCount( arm.body, "activateWindow(" ), 1 );
    QCOMPARE( selectionCount( body, "PopupDismiss::needsExplicitActivation(" ), 1 );
    QCOMPARE( selectionDirectActions( arms[1].body ), selectionCode( "show(); activateWindow(); raise();" ) );
    QCOMPARE( selectionDirectActions( arms[2].body ), selectionCode( "activateWindow(); raise();" ) );
    QString const rawFunction = rawCodeBlock( file, "ScanPopup::engagePopup" ); QVERIFY( !rawFunction.isEmpty() );
    QString const rawHidden = rawCodeBlock( rawFunction, "if( !isVisible() )" ); QVERIFY( !rawHidden.isEmpty() );
    QString const region = directiveRegion( rawHidden, "#ifdef ENABLE_SPWF_CUSTOMIZATION" ); QVERIFY( !region.isEmpty() );
    QString const giveFocus = requiredBranch( region,
      "if( QGuiApplication::platformName() == \"xcb\" && !ui.pinButton->isChecked() )" );
    QVERIFY( !giveFocus.isEmpty() );
    QCOMPARE( selectionStatements( giveFocus ), QStringList{ selectionCode( "giveFocus = true;" ) } );
    QVERIFY( selectionOrdered( arms[0].body, {
      "show();", "if( QGuiApplication::platformName() == \"xcb\" && !ui.pinButton->isChecked() )",
      "giveFocus = true;", guard } ) );
  }
  void unpinRequestsActivationAfterShow()
  {
    QString const file = source( "scanpopup.cc" ); QVERIFY( !file.isEmpty() );
    QString const body = requiredBlock( file, "ScanPopup::pinButtonClicked" ); QVERIFY( !body.isEmpty() );
    auto const statements = selectionStatements( body ); QCOMPARE( statements.size(), 5 );
    QCOMPARE( statements[0], selectionCode( "focusLossCloser->setPinned( checked );" ) );
    QVERIFY( statements[1].startsWith( selectionCode( "if( checked )" ) + ' ' ) );
    QCOMPARE( statements[2], selectionCode( "show();" ) );
    QString const guard = "if( !checked && PopupDismiss::needsExplicitActivation( windowFlags() ) )";
    QVERIFY( statements[3].startsWith( selectionCode( guard ) + ' ' ) );
    QString const request = requiredBranch( statements[3], guard ); QVERIFY( !request.isEmpty() );
    QCOMPARE( selectionStatements( request ), QStringList( { selectionCode( "activateWindow();" ), selectionCode( "raise();" ) } ) );
    QCOMPARE( selectionDirectActions( request ), selectionCode( "activateWindow(); raise();" ) );
    QCOMPARE( statements[4], selectionCode( "if( checked ) pinnedGeometry = saveGeometry();" ) );
    QCOMPARE( selectionCount( body, "activateWindow(" ), 1 );
    QVERIFY( selectionOrdered( body, { "show();", guard, "activateWindow();", "raise();",
                                      "if( checked ) pinnedGeometry = saveGeometry();" } ) );
  }
```

- [ ] **Step 3: Execute RED with the new five assertions present and no ScanPopup edits.** Require the failures to identify the missing include/member, constructor construction/sync, first pin statement, widened hidden guard and guarded unpin request, rather than an empty/misparsed scope. Use these exact focused commands:

```bash
qmake6 tests/tst_popup_dismiss/tst_popup_dismiss.pro -o tests/tst_popup_dismiss/Makefile
make -C tests/tst_popup_dismiss -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_popup_dismiss/tst_popup_dismiss
```

- [ ] **Step 4: Apply the exact header/constructor changes.** Insert the include lines after `mainstatusbar.hh`, the member immediately after `Ui::ScanPopup ui;`, and the constructor statements at the two specified anchors. Keep every intervening constructor statement and both existing flag branches.

```cpp
#include "mainstatusbar.hh"
#include "popupdismiss.hh"
#include <memory>
```

```cpp
  Ui::ScanPopup ui;
  std::unique_ptr< PopupDismiss::FocusLossCloser > focusLossCloser;
```

```cpp
  ui.setupUi( this );
  focusLossCloser = std::make_unique< PopupDismiss::FocusLossCloser >( this );
  focusLossCloser->setDismissHandler( [ this ] { hideWindow(); } );

  openSearchAction.setShortcut( QKeySequence( "Ctrl+F" ) );
```

```cpp
  ui.pinButton->setChecked( cfg.pinPopupWindow );
  focusLossCloser->setPinned( ui.pinButton->isChecked() );

  if ( cfg.pinPopupWindow )
```

- [ ] **Step 5: Add pin synchronization and the post-Show unpin request.** The following are exact replacement regions in `pinButtonClicked`; preserve the complete existing checked/unchecked bodies between them.

```cpp
void ScanPopup::pinButtonClicked( bool checked )
{
  focusLossCloser->setPinned( checked );
  if ( checked )
```

```cpp
  show();

  if ( !checked && PopupDismiss::needsExplicitActivation( windowFlags() ) )
  {
    activateWindow();
    raise();
  }

  if( checked )
    pinnedGeometry = saveGeometry();
```

- [ ] **Step 6: Widen only the existing hidden-window activation condition.** Replace the hidden arm's `if ( giveFocus )` block with this code, leaving direct `show();` before the complete raw customization block and `interceptMouse()` afterwards. Retain both later arms and all their comments/guards/actions verbatim; they remain the other two executable activation calls.

```cpp
    if ( giveFocus || ( !ui.pinButton->isChecked() &&
                       PopupDismiss::needsExplicitActivation( windowFlags() ) ) )
    {
      activateWindow();
      raise();
    }
```

- [ ] **Step 7: Run GREEN and review the protected paths.** Re-run the three Step 3 commands; all 27 substantive cases so far must pass without skips. Compare the working diff against the approved baseline to confirm no edits to `unpinnedWindowFlags()`, `hideWindow()`, `escapePressed()`, proximity/outside-press/leave paths, pinned flag/title/geometry branches, giveFocus defaults/callers, or MainWindow/PlatformWindow token and handle behavior. The module suite's scoped-source tests prove this wiring; they do not claim to execute ScanPopup. Task 6 supplies the independent application-link gate.

- [ ] **Step 8: Commit** the exact three changed files with this message:

```bash
git add scanpopup.hh scanpopup.cc tests/tst_popup_dismiss/tst_popup_dismiss.cc
git commit -m "feat(scanpopup): request opening focus and wire dismissal"
```

## Task 4: Register the module and enforce the closed suite inventory

**Implementer tier:** Advanced

**Lane/category:** popup-dismiss · build

**Same-lane predecessor:** Task 3 — Wire the closer and opening requests into ScanPopup.

**Files:**

- Modify: `goldendict.pro:288-426` — unconditional enumerated `HEADERS += folding.hh` list, last `platformwindow.hh` entry; `goldendict.pro:444-573` — unconditional enumerated `SOURCES += folding.cc` list, last `platformwindow.cc` entry.
- Modify: `tests/tests.pro:9` — introductory ordinary-mode inventory comment; `tests/tests.pro:132-160` — ordinary `} else {` branch's `GD_REQUIRED_SUITE_NAMES`, `!count(GD_TEST_NAMES, 20)` literal/error text and exact-20 message. Preserve all discovery/closure/equality logic.
- Modify: `tests/tst_popup_dismiss/tst_popup_dismiss.cc` — add the qmake-only helpers in its anonymous namespace and the final two inline source slots before `TstPopupDismiss` closes. No new project beyond the one already created in Task 1.

**Interfaces:**

- Consumes (§5, Task 1): unconditional application inputs `popupdismiss.hh` and `popupdismiss.cc`; production-linked project `tests/tst_popup_dismiss/tst_popup_dismiss.pro`, with `TARGET = tst_popup_dismiss`, `include(../testsupport.pri)`, `QT += core gui widgets`, `INCLUDEPATH += $$GD_REPO_ROOT`, `SOURCES += $$GD_REPO_ROOT/popupdismiss.cc tst_popup_dismiss.cc`, `HEADERS += $$GD_REPO_ROOT/popupdismiss.hh`.
- Consumes (§6.5–6.6, Task 2, suite-local): `QString source( char const * path )`, `QString selectionRawBlock( QString const & text, QString const & anchor )`, `QString TestSupport::repositoryRoot()`. qmake uses a raw comment/continuation/scope parser, not the C++ normaliser.
- Produces (§5): exactly one unconditional application registration per new file, immediately after the matching platformwindow entry, and exactly 21 ordinary-mode suites, sorted with `tst_popup_dismiss` between platformwindow and portal_hotkeys. The required list, discovered project basenames, count literal and both changed messages agree; closure mode and independent inventory-equality guard stay intact.

**Implementation steps:**

- [ ] **Step 1: Write the qmake-only parsing helpers and two named assertions first.** Add `<QFileInfo>` and `<QFileInfoList>` as needed. The parser below strips only actual qmake comments, joins physical backslash continuations, retains scope depth outside quoted strings, and distinguishes all enumerated assignments from unconditional ones. It does not evaluate qmake or use C++ token normalisation.

```cpp
QString qmakeUncomment( QString const & text )
{
  QString result; QChar quote; bool escaped = false, comment = false;
  for( QChar c : text )
  {
    if( c == '\n' ) { result += c; comment = false; escaped = false; continue; }
    if( comment ) continue;
    if( escaped ) { result += c; escaped = false; continue; }
    if( c == '\\' ) { result += c; escaped = true; continue; }
    if( !quote.isNull() ) { result += c; if( c == quote ) quote = QChar{}; continue; }
    if( c == '"' || c == '\'' ) { quote = c; result += c; continue; }
    if( c == '#' ) { comment = true; continue; }
    result += c;
  }
  return result;
}
QStringList qmakeLines( QString const & text )
{
  QStringList result; QString pending;
  for( QString line : qmakeUncomment( text ).split( '\n' ) )
  {
    line = line.trimmed();
    bool const continued = line.endsWith( '\\' );
    if( continued ) line.chop( 1 );
    pending += line + ' ';
    if( !continued ) { if( !pending.trimmed().isEmpty() ) result.append( pending.trimmed() ); pending.clear(); }
  }
  if( !pending.trimmed().isEmpty() ) result.append( pending.trimmed() );
  return result;
}
struct QmakeAssignment { int depth; QStringList values; };
QList< QmakeAssignment > qmakeAssignments( QString const & text, QString const & variable )
{
  QRegularExpression const assign( "^" + QRegularExpression::escape( variable ) + "\\s*\\+?=\\s*(.*)$" );
  QList< QmakeAssignment > result; int depth = 0;
  for( QString const & line : qmakeLines( text ) )
  {
    auto const match = assign.match( line );
    if( match.hasMatch() ) result.append( { depth, match.captured( 1 ).split( QRegularExpression( "\\s+" ), Qt::SkipEmptyParts ) } );
    QChar quote; bool escaped = false;
    for( QChar c : line )
    {
      if( escaped ) { escaped = false; continue; }
      if( c == '\\' ) { escaped = true; continue; }
      if( !quote.isNull() ) { if( c == quote ) quote = QChar{}; continue; }
      if( c == '"' || c == '\'' ) { quote = c; continue; }
      if( c == '{' ) ++depth;
      else if( c == '}' ) --depth;
    }
    if( depth < 0 ) return {};
  }
  return depth == 0 ? result : QList< QmakeAssignment >{};
}
```

```cpp
  void applicationProjectRegistersPopupDismiss()
  {
    QString const project = source( "goldendict.pro" ); QVERIFY( !project.isEmpty() );
    auto const headers = qmakeAssignments( project, "HEADERS" );
    auto const sources = qmakeAssignments( project, "SOURCES" );
    QVERIFY( !headers.isEmpty() ); QVERIFY( !sources.isEmpty() );
    auto assertRegistration = []( QList< QmakeAssignment > const & lists,
                                  QString const & before, QString const & added ) {
      int count = 0;
      for( auto const & list : lists )
        for( qsizetype i = 0; i < list.values.size(); ++i )
          if( list.values[i] == added )
          { ++count; QCOMPARE( list.depth, 0 ); QVERIFY( i > 0 ); QCOMPARE( list.values[i-1], before ); }
      QCOMPARE( count, 1 );
    };
    assertRegistration( headers, "platformwindow.hh", "popupdismiss.hh" );
    assertRegistration( sources, "platformwindow.cc", "popupdismiss.cc" );
    for( auto const & list : headers ) QVERIFY( !list.values.contains( "popupdismiss.cc" ) );
    for( auto const & list : sources ) QVERIFY( !list.values.contains( "popupdismiss.hh" ) );
  }
  void requiredSuiteInventoryIsExact()
  {
    QString const project = source( "tests/tests.pro" ); QVERIFY( !project.isEmpty() );
    QString const clean = qmakeUncomment( project );
    QString const ordinary = selectionRawBlock( clean, "} else {" ); QVERIFY( !ordinary.isEmpty() );
    auto const lists = qmakeAssignments( ordinary, "GD_REQUIRED_SUITE_NAMES" ); QCOMPARE( lists.size(), 1 );
    QCOMPARE( lists[0].depth, 0 );
    QStringList const expected = {
      "tst_article_bridge", "tst_article_history", "tst_article_load", "tst_article_schemes",
      "tst_article_stream", "tst_btree_index", "tst_encoding", "tst_external_content",
      "tst_hotkeys", "tst_htmlescape", "tst_langcoder", "tst_linkrewrite", "tst_platformwindow",
      "tst_popup_dismiss", "tst_portal_hotkeys", "tst_print_pipeline", "tst_regex",
      "tst_save_article", "tst_utf8_wstring", "tst_waylandselection", "tst_website_frames" };
    QCOMPARE( expected.size(), 21 ); QCOMPARE( lists[0].values, expected );
    QStringList discovered;
    QDir const tests( QDir( TestSupport::repositoryRoot() ).filePath( "tests" ) );
    QVERIFY( tests.exists() );
    for( auto const & directory : tests.entryInfoList( { "tst_*" }, QDir::Dirs | QDir::NoDotAndDotDot ) )
      for( auto const & file : QDir( directory.absoluteFilePath() ).entryInfoList( { "tst_*.pro" }, QDir::Files ) )
        discovered.append( file.completeBaseName() );
    discovered.sort(); QCOMPARE( discovered, expected );
    QCOMPARE( QSet< QString >( discovered.begin(), discovered.end() ).size(), discovered.size() );
    QCOMPARE( QSet< QString >( lists[0].values.begin(), lists[0].values.end() ).size(), lists[0].values.size() );
    QCOMPARE( discovered.count( "tst_popup_dismiss" ), 1 );
    QCOMPARE( expected[12], QString( "tst_platformwindow" ) );
    QCOMPARE( expected[13], QString( "tst_popup_dismiss" ) );
    QCOMPARE( expected[14], QString( "tst_portal_hotkeys" ) );
    QRegularExpression const countPattern( "!count\\s*\\(\\s*GD_TEST_NAMES\\s*,\\s*([0-9]+)\\s*\\)" );
    auto counts = countPattern.globalMatch( ordinary ); QVERIFY( counts.hasNext() );
    auto const count = counts.next(); QVERIFY( !counts.hasNext() );
    QCOMPARE( count.captured( 1 ).toInt(), int( lists[0].values.size() ) );
    QCOMPARE( count.captured( 1 ).toInt(), int( discovered.size() ) );
    auto const lines = qmakeLines( ordinary );
    QVERIFY( lines.contains( "!count(GD_TEST_NAMES, 21): error(\"Expected exactly 21 required suites; found: $$GD_TEST_NAMES\")" ) );
    QVERIFY( lines.contains( "message(\"tests/tests.pro: exact 21 suites: $$GD_TEST_NAMES\")" ) );
    QVERIFY( lines.contains( "!equals(GD_TEST_NAMES, $$GD_REQUIRED_SUITE_NAMES): error(\"Required suite inventory mismatch: $$GD_TEST_NAMES\")" ) );
    QVERIFY( project.split( '\n' ).contains( "# Ordinary mode is a closed acceptance inventory: exactly the 21 required suites." ) );
  }
```

- [ ] **Step 2: Record RED before the project/list edits.** Run the commands below with the two new slots present. The intended new failures are missing unconditional registrations and the old 20-suite list/count/messages against 21 discovered projects. Scope-parser/environment errors are fixture failures to repair before counting RED. Earlier 27 cases remain GREEN.

```bash
qmake6 tests/tst_popup_dismiss/tst_popup_dismiss.pro -o tests/tst_popup_dismiss/Makefile
make -C tests/tst_popup_dismiss -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_popup_dismiss/tst_popup_dismiss
```

- [ ] **Step 3: Register the application inputs unconditionally.** Change only the last entry of the enumerated HEADERS and SOURCES lists to these respective two-line tails; keep all prior entries and conditional build logic:

```qmake
    platformwindow.hh \
    popupdismiss.hh
```

```qmake
    platformwindow.cc \
    popupdismiss.cc
```

- [ ] **Step 4: Update exactly the ordinary-mode inventory locations.** Replace the introductory comment and required list/count/message with the exact content in Global Constraints (§5). The list order is fixed, including popup_dismiss after platformwindow; preserve the independent equality guard/error, real two-level discovery, required-project existence/registration checks, SUBDIRS registration, closure-mode archive path and explicit caller-selected QPA.

```qmake
# Ordinary mode is a closed acceptance inventory: exactly the 21 required suites.
```

```qmake
    GD_REQUIRED_SUITE_NAMES = \
        tst_article_bridge tst_article_history tst_article_load \
        tst_article_schemes tst_article_stream tst_btree_index tst_encoding \
        tst_external_content tst_hotkeys tst_htmlescape tst_langcoder \
        tst_linkrewrite tst_platformwindow tst_popup_dismiss \
        tst_portal_hotkeys tst_print_pipeline tst_regex tst_save_article \
        tst_utf8_wstring tst_waylandselection tst_website_frames
```

```qmake
    !count(GD_TEST_NAMES, 21): error("Expected exactly 21 required suites; found: $$GD_TEST_NAMES")
    !equals(GD_TEST_NAMES, $$GD_REQUIRED_SUITE_NAMES): error("Required suite inventory mismatch: $$GD_TEST_NAMES")
```

```qmake
    message("tests/tests.pro: exact 21 suites: $$GD_TEST_NAMES")
```

- [ ] **Step 5: Run GREEN and configure the aggregate.** Re-run the three Step 2 commands and require all 29 substantive slots with their data rows and no skips. Then run `qmake6 tests/tests.pro -o tests/Makefile`; require its exact 21-suite message and successful inventory-equality gate. Task 6 owns the application link and full aggregate build/check; source registration alone is not link evidence. Review §9.2's build mappings: wrong/conditional/duplicate application entries, suite discovery omissions/extras, stale literal/messages, closure-only registration and removed independent equality validation must fail the corresponding source assertion.

- [ ] **Step 6: Commit** only these three changed files with this exact message:

```bash
git add goldendict.pro tests/tests.pro tests/tst_popup_dismiss/tst_popup_dismiss.cc
git commit -m "build(popup): register module and twenty first required suite"
```

## Task 5: Document the focus behavior and evidence limits

**Implementer tier:** Fast

**Lane/category:** popup-dismiss · docs

**Same-lane predecessor:** Task 4 — Register the module and enforce the closed suite inventory.

**Files:**

- Modify: `docs/wayland-limitations.md` — §2, after the paragraph ending `Hover translation and X11 focus restoration remain X11-only.` and before `Coverage is limited to the ext protocol.`; §3, after the existing paragraph starting `Activation is best-effort:` and ending `restoration stay X11-only (see §2).`, immediately before §4. No README change or evidence-total rewrite.

**Interfaces:**

- Consumes (§7): documented opening request, granted-focus dismissal, owned menu/dropdown continuity, dialog dismissal, pin immunity and activation refusal limit from completed Tasks 1–4. These are documentation semantics; this task adds no C++ interface.
- Produces (§7): the three verbatim paragraphs below at their exact anchors, leaving the existing best-effort paragraph and old observed evidence totals unchanged.

**Implementation steps:**

- [ ] **Step 1: Insert the two specified §2 paragraphs at the exact anchor.** Use ordinary Markdown paragraphs with this exact text, in this order:

```text
An unpinned scan popup requests keyboard activation when it opens and closes as soon as focus leaves its own interaction, including when a dialog it opened takes the keyboard, as on X11. Its own menus and combo dropdowns keep the popup open, and pinned popups never auto-hide. If the compositor refuses activation, the popup remains open; click it to give it focus, after which clicking away hides it and Esc closes an open search panel first, or otherwise hides the popup.

Automated tests cover the shared popup rule and its signal handlers on xcb. The compositor's focus decision and a genuine application-inactive transition require a live session and are not claimed here.
```

- [ ] **Step 2: Insert the §3 consequence paragraph immediately after the untouched best-effort paragraph, before §4.** Use this exact text:

```text
For the scan popup, a refused activation request means it must be dismissed manually, as described in §2. This is the documented consequence of best-effort activation rather than a regression.
```

- [ ] **Step 3: Review the documentation-only diff.** Run the following commands, compare every inserted paragraph against §7, and require exactly these insertions, correct anchors, unchanged old totals/best-effort text, and no extra file edits. This literal documentation change adds no test slot and needs no fabricated failing test.

```bash
git diff --check
git diff -- docs/wayland-limitations.md
```

- [ ] **Step 4: Commit** the single documentation file with the exact message:

```bash
git add docs/wayland-limitations.md
git commit -m "docs(wayland): describe scan popup focus dismissal limits"
```

## Task 6: Run the application and complete acceptance gate

**Implementer tier:** Standard

**Lane/category:** popup-dismiss · acceptance

**Same-lane predecessor:** Task 5 — Document the focus behavior and evidence limits.

**Files:**

- Create/Modify: None. This task performs no new implementation, test, project, documentation or pinned-plan edits. Build products are generated by the specified commands; record evidence in the controller's task result rather than an extra tracked report.
- Read-only review: `popupdismiss.hh`, `popupdismiss.cc`, `scanpopup.hh`, `scanpopup.cc`, `goldendict.pro`, `tests/tests.pro`, `tests/tst_popup_dismiss/tst_popup_dismiss.pro`, `tests/tst_popup_dismiss/tst_popup_dismiss.cc`, `docs/wayland-limitations.md`, and §8–§9 of the approved specification.

**Interfaces:**

- Consumes (§5–§8, Tasks 1–5): application target from `goldendict.pro`; exact ordinary 21-suite target from `tests/tests.pro`; production-linked `tst_popup_dismiss`; exactly 29 substantive new slots plus their `_data` partners and QtTest lifecycle hooks. No new runtime/public interface.
- Consumes (§8): shell commands below, repository-root cwd, working qmake6/make/xvfb-run/timeout, explicit `QT_QPA_PLATFORM=xcb` for aggregate execution. QPA selection belongs to the acceptance caller.
- Produces (§8, §6.7, §9): recorded command/stdout/stderr/exit evidence, exact 21-suite registration, execution of every required new runtime case without skips, successful independent application linkage and complete aggregate check. Reports distinguish runtime, scoped-source, build, and unobserved native-session claims.

**Implementation steps:**

- [ ] **Step 1: Reconcile source and inventory without changing them.** Run `git diff --check` and inspect the committed implementation diff. Confirm exactly the nine allowed implementation/doc paths, protected activation/pointer paths, exact header/API/lifecycle, three documentation insertions, 29 named cases and §9.2 coverage ownership. `_data`, `initTestCase`, and `cleanupTestCase` are not additional substantive cases. Confirm no skips were added to required runtime cases and no README/shared-helper/other-suite edits. An acceptance failure is evidence to return to its owning implementation task; it is not permission to implement a repair in this task.

- [ ] **Step 2: Run the specification's acceptance commands verbatim, in order, from the implemented repository root.** Execute each line exactly as shown, capture its complete stdout/stderr and numeric exit status through the controller/tool result, and require success before proceeding. Do not replace the application link with a module-only build, force an alternative QPA, reduce the suite inventory, add skips, or change timeouts.

```bash
qmake6 goldendict.pro -o Makefile && make -j"$(nproc)"          # application links (D7)
qmake6 tests/tests.pro -o tests/Makefile
make -C tests -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check
```

- [ ] **Step 3: Record the actual results.** In the controller's task evidence, include all four exact invocations and their exit statuses/output references; the application link result; the exact 21-suite qmake message; the new suite's executed substantive/data/runtime case accounting; every skip/failure/timeout if present; and the overall aggregate result. Record failure honestly and withhold the acceptance commit until the owned repair and a rerun succeed. A precondition/build/import failure is never a claimed behavioural mutation kill. Preserve §6.7's limits: xcb tests and the injected real-application signal prove the shared rule/installed handlers, not native Wayland grant/refusal or a genuine compositor-generated inactive transition. No live-session result is invented.

- [ ] **Step 4: Commit** only after every acceptance command succeeds and all required runtime cases execute without skips. There is no source change in this task; use this exact empty acceptance-commit message, and leave the evidence in the controller's result:

```bash
git commit --allow-empty -m "test(popup): accept application link and twenty one suite gate"
```
