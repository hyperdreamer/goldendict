# Wayland scan popup focus-loss dismissal: technical specification

Authority: `docs/superpowers/specs/2026-11-16-wayland-scan-popup-focus-close-design.md`, approved revision 8 at commit `a3493dcf`, SHA-256 `cbc3a771294a60bb3d50a2abee3f77c7b8551e22f647c08207bcc1b0c699e5e7`.

This document specifies implementation and acceptance; it does not record an implementation, a build, a test run, or a native Wayland result. Paths below are repository-relative. The source context is `/data/home/guest/Development/goldendict`; the specification belongs in the PM integration worktree. The approved revision 8 behaviour takes precedence over superseded mechanisms described in the design's review history.

## 1. Scope, goals, non-goals, and glossary

Implement full X11 parity for the unpinned scan popup on Wayland: request keyboard activation when opening a managed window, then hide immediately when keyboard focus leaves its own interaction after it has held focus. Activation remains a compositor decision. Use the existing `ScanPopup::hideWindow()` path, rather than `escapePressed()`: Esc first closes an open search panel, whereas a focus-loss dismissal hides directly.

The new closer applies on every platform. Its rule ignores hidden, pinned, and never-focused windows. It preserves an owned menu or dropdown interaction, and dismisses when a dialog takes focus, including a dialog opened by the scan popup. A pinned popup never auto-hides. Default X11 `Qt::Popup` flags and Qt's existing popup dismissal remain intact. The managed `xcb` `Tool` form intentionally gains this focus-loss rule as well.

Opening requests are added only when an unpinned window changes from hidden to visible with a non-`Qt::Popup` type, including the replacement window shown on unpin. Updating an already-visible unpinned window adds no request. Keep the existing pinned-visible re-raise, the `xcb` `giveFocus` block, the already-visible `xcb` `Tool` activation, and hotkey/token activation paths.

Non-goals:

- No preference, UI control, changed stored default, grab emulation, activation guarantee, timer, grace period, pointer polling, suppression state, or modal-dialog exception in the new module.
- No changes to hover translation, the scan flag, clipboard/selection watching, alt-mode, the tray, the main window, the article view, pinned flags, or the pinned stay-on-top option.
- No changes to existing proximity, leave-timer, outside-press, or other `hideWindow()` callers. Those paths retain their behaviour, including possible menu-time dismissal on `xcb` `Tool` windows. The menu-survival guarantee here belongs to the new focus rule.
- No README edit and no claim to have verified a compositor's grant/refusal or a genuine application-inactive transition using the single-process suite.

Implementation files are limited to new `popupdismiss.hh`, `popupdismiss.cc`, `tests/tst_popup_dismiss/tst_popup_dismiss.pro`, and `tests/tst_popup_dismiss/tst_popup_dismiss.cc`, plus edits to `scanpopup.hh`, `scanpopup.cc`, `goldendict.pro`, `tests/tests.pro`, and `docs/wayland-limitations.md`. Existing test helpers are copied into the new suite, not changed in `tst_hotkeys` or shared support.

Glossary:

| Term | Meaning |
| --- | --- |
| armed | This show cycle has held focus on the observed window's own `QWindow`. `Show` establishes the initial value from identity; a focus event for that window sets it; `Hide` clears it. A completed dismissal also clears it. |
| own window | The observed top-level widget's `windowHandle()`, compared by pointer identity. |
| own popup | The widget returned by `QApplication::activePopupWidget()`, strictly owned through QWidget parenting by the observed widget, whose own native handle is the focus destination and whose native window type is exactly `Qt::Popup`. Includes nested menus; excludes the observed widget itself and ordinary dialogs. |
| foreign destination | Any non-null focused window that is neither the own window nor an own popup under that definition. A dialog is foreign regardless of transient parent. |
| none | The focus signal carries a null window. Dismiss only if the common rule also finds the application not active. |
| decision | The rule's `Ignore` or `Hide` result for one focus/state event. It is synchronous; no later timer resolves it. |

## 2. New module contract

### 2.1 Complete `popupdismiss.hh`

The complete header is:

```cpp
/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file. */

#ifndef GOLDENDICT_POPUPDISMISS_HH_INCLUDED
#define GOLDENDICT_POPUPDISMISS_HH_INCLUDED

#include <QEvent>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <Qt>

#include <functional>

class QWidget;
class QWindow;

namespace PopupDismiss {

enum class Action { Ignore, Hide };

enum class FocusDestination {
  OwnWindow,
  OwnPopup,
  Foreign,
  None,
};

Action onFocusMoved( bool visible, bool pinned, bool armed,
                     FocusDestination destination, bool applicationActive );

bool needsExplicitActivation( Qt::WindowFlags flags );
bool armedAfterShow( QWindow const * ownWindow, QWindow const * focusWindow );
bool isOwnedPopup( QWidget const * owner, QWidget const * candidate );
bool isPopupWindowType( Qt::WindowType type );

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

} // namespace PopupDismiss

#endif // GOLDENDICT_POPUPDISMISS_HH_INCLUDED
```

Implement in `popupdismiss.cc`, in namespace `PopupDismiss`, including this header and `<QApplication>`, `<QDebug>`, `<QGuiApplication>`, `<QLoggingCategory>`, `<QWidget>`, `<QWindow>`, and `<utility>` for moving the handler. The header must compile without relying on any consumer's prior includes. It exposes no timer, extra state, activation-state API, or ScanPopup dependency. Both enum classes are scoped enums. No public testing seam or extra handler method is needed; the two connections use lambdas.

### 2.2 Pure functions and ownership

`onFocusMoved()` has exactly this rule, returning an `Action`, with no side effects:

```cpp
return visible && !pinned && armed &&
       ( destination == FocusDestination::Foreign ||
         ( destination == FocusDestination::None && !applicationActive ) )
         ? Action::Hide : Action::Ignore;
```

Thus `Foreign` does not require an inactive application; `None` does. `OwnWindow` and `OwnPopup` always ignore. The inputs are observations supplied by the caller, not queried by the pure function. Tests must distinguish missing/inverted guards and changed logical operators; exchanging equivalent `&&` operand order is not an observable mutation.

`needsExplicitActivation( flags )` returns exactly `( flags & Qt::WindowType_Mask ) != Qt::Popup`. Mask the type before comparing. Window hints do not affect the answer. In particular `Qt::Tool` requires activation even though its numeric bits contain the popup bits; `flags.testFlag( Qt::Popup )` is not a valid implementation.

`armedAfterShow( ownWindow, focusWindow )` returns exactly `ownWindow != nullptr && ownWindow == focusWindow`. It compares pointers without dereferencing them or creating a native handle. Equal non-null pointers return true; different pointers, either null pointer, and two null pointers return false.

`isOwnedPopup( owner, candidate )` is the strict QWidget ownership predicate used on Qt's reported active popup. Null owner, null candidate, and `owner == candidate` return false. Otherwise return true iff repeatedly following `candidate->parentWidget()` reaches `owner`, at any depth; direct and nested popup-widget parents count even across top-level popup widgets. A sibling, unrelated top-level, or menu parented to another top-level returns false. This helper checks ownership, not focus or active-popup status; the classifier supplies those checks and the separate type check. Do not use QWindow transient ancestry as ownership. A concrete expression of the required traversal is:

```cpp
if( !owner || !candidate || owner == candidate ) return false;
for( QWidget const * parent = candidate->parentWidget(); parent;
     parent = parent->parentWidget() )
  if( parent == owner ) return true;
return false;
```

`isPopupWindowType( type )` returns exactly `type == Qt::Popup`. `Qt::ToolTip`, `Qt::Dialog`, `Qt::Tool`, and `Qt::Window` return false. It takes an already identified `Qt::WindowType`, not a flags bit test. The ownership helper and this discriminator remain separate.

### 2.3 Exact `destinationOf()` precedence

Implement the following ordered classification. It must not create a native handle, read activation state, or follow any `QWindow::parent()` / `transientParent()` chain:

```cpp
if( !window ) return FocusDestination::Foreign;
if( !focusWindow ) return FocusDestination::None;
if( focusWindow == window->windowHandle() ) return FocusDestination::OwnWindow;

QWidget * const activePopup = QApplication::activePopupWidget();
if( isOwnedPopup( window, activePopup ) )
{
  QWindow * const popupHandle = activePopup->windowHandle();
  if( popupHandle && focusWindow == popupHandle &&
      isPopupWindowType( focusWindow->type() ) )
    return FocusDestination::OwnPopup;
}

return FocusDestination::Foreign;
```

A destroyed observed widget wins over null focus and classifies `Foreign`; signal handlers still return without acting on a null observed widget. A null focus wins over menu inspection. Own-window identity wins over popup inspection, which also prevents the scan popup's own `Qt::Popup` form being confused with one of its menus. A nested submenu is recognised through the innermost active popup's own handle, without an ancestry shortcut. The type discriminator's presence is mandatory even though other predicates already reject a normal dialog in the runtime dialog fixture.

### 2.4 Public state/action methods

`setPinned( bool pinned )` assigns the stored pin state, without changing arming, flags, focus, visibility, or invoking a handler. `isPinned() const` returns that stored value. `setDismissHandler( std::function< void() > handler )` replaces the stored callable, moving the supplied value. An empty callable means observe only: decisions and state still run, but the closer never supplies a fallback `hide()` action. The observed widget is borrowed through a `QPointer`; it is neither reparented nor owned by the closer. The optional QObject parent owns the closer only under normal QObject ownership rules. The ScanPopup integration supplies unique ownership and the callback.

## 3. `FocusLossCloser` event model

Use this numbered procedure, in order. All processing occurs on the widget/application GUI thread. There are no queued settlement operations.

1. **Construct and subscribe.** Initialise `QObject( parent )`, store the widget in `window`, and retain the header's false initial values for `pinned`, `armed`, and `dismissing`. Do not arm from current focus in the constructor. If `window` exists, call `window->installEventFilter( this )`. Store exactly these two context-bound connections; `qApp` is the real application object and `this` is the receiver/context in both:

   ```cpp
   focusConnection = QObject::connect(
     qApp, &QGuiApplication::focusWindowChanged, this,
     [this]( QWindow * focusWindow ) { /* step 3 */ } );
   stateConnection = QObject::connect(
     qApp, &QGuiApplication::applicationStateChanged, this,
     [this]( Qt::ApplicationState state ) { /* step 4 */ } );
   ```

   Do not connect with the observed widget as the receiver or use a context-free functor. Do not show, focus, activate, or materialise the window from construction. A null observed widget is tolerated and remains inert.

2. **Observe Show/Hide only.** In `eventFilter`, process only `watched == window.data()` while `window` is non-null. For `QEvent::Show`, assign exactly:

   ```cpp
   armed = PopupDismiss::armedAfterShow(
     window->windowHandle(), QGuiApplication::focusWindow() );
   ```

   For `QEvent::Hide`, assign `armed = false`. Do not consume either event: return the base `QObject::eventFilter( watched, event )` result after observation. Other events have no effect. Do not use `isActiveWindow()`, `WindowActivate`, `WindowDeactivate`, `ActivationChange`, a modal stack, polling, or timers. Do not request activation in this filter. `Show` must set both true and false outcomes, not merely set true conditionally and preserve stale state.

3. **Handle `focusWindowChanged`.** First return if `window` is null. Compute `destinationOf( focusWindow )` once. If it is `OwnWindow`, assign `armed = true`, log the Ignore/own-focus decision, and return. If it is `OwnPopup`, leave `armed` unchanged, log Ignore/continuing-popup interaction, and return. Otherwise compute `applicationActive` as `qApp->applicationState() == Qt::ApplicationActive`, read widget visibility, and call the pure rule with that visibility, stored `pinned`, stored `armed`, the computed destination, and `applicationActive`. Log and return on `Ignore`. On `Hide`, call `dismiss( "focus moved to another window" )` for `Foreign`, or `dismiss( "focus left the application" )` for `None`; then return without touching any member. `None` while application state is Active does nothing. For this handler, any state other than Active yields `applicationActive == false`, as required by the rule and the design event table. Do not re-read widget activation state or infer a destination from transient parents. Own-popup focus does not independently arm a never-focused owner.

4. **Handle `applicationStateChanged`.** First ignore every payload except `Qt::ApplicationInactive`; then return if `window` is null. For the Inactive payload evaluate the same visible/unpinned/armed rule using `FocusDestination::None` and `applicationActive = false`. This represents application focus leaving independently of whether a separate focus-window event arrived. On `Ignore`, log and return. On `Hide`, call `dismiss( "application inactive" )` and return without accessing members. Use the signal's payload; do not require `QGuiApplication::focusWindow()` to have changed or re-read `applicationState()` in place of the payload. In particular a delivered Inactive signal must dismiss an armed visible unpinned widget even if the currently reported focus handle is still its own. Do not dismiss for Active, Hidden, or Suspended payloads through this state handler.

5. **Dismiss with reentrancy and lifetime guards.** Both handlers reach the same `dismiss( char const * reason )`. If `dismissing` is already true, return immediately, without invoking the callback, logging another Hide, or changing arming. Otherwise set `dismissing = true`, create a local `QPointer< FocusLossCloser >` to `this`, log this Hide decision, copy `dismissHandler` to a local callable, and invoke that local copy once if non-empty. The local copy must outlive a callback that deletes the closer. Immediately after the call, test the local self-guard. If it is null, return without reading or writing any member. If it survives, clear `dismissing`, then clear `armed`. Do not dereference `window` after the call: the handler may have deleted it. A synchronous Hide may already have cleared arming; the final clear remains required. Do not hide directly in the closer, call the callback twice, clear the reentrancy flag before the callback, or add a timer. If a callback synchronously generates another dismissal decision, the guard prevents a second invocation. Existing callers of `ScanPopup::hideWindow()` remain outside this new guard; the function emits `closeMenu()` on every call and is not globally idempotent.

6. **Destroy and detach.** Explicitly `QObject::disconnect( focusConnection )`, then `QObject::disconnect( stateConnection )`; if `window` still exists, remove this filter with `window->removeEventFilter( this )`. No callback, hide, or activation occurs during destruction. Qt also removes the context-bound connections on QObject destruction. If the observed widget was destroyed first, the QPointer is null and the closer stays inert until its own destruction. Never dereference a saved raw observed-widget pointer during teardown.

7. **Log decisions.** Declare `Q_LOGGING_CATEGORY( popupDismissLc, "goldendict.popup.dismiss" )` in `popupdismiss.cc` and use `qCDebug( popupDismissLc )`. Emit one line per evaluated decision: identify the rule (`onFocusMoved` or `applicationStateChanged`), destination (`OwnWindow`, `OwnPopup`, `Foreign`, or `None`), and action (`Ignore` or `Hide`). For Ignore, identify the decisive reason (own focus, own popup, hidden, pinned, unarmed, or None with active application); the rule's input booleans may be included. For Hide, the single line is emitted inside `dismiss()` and includes its exact reason string above; do not also log a second Hide in the caller. The three fixed reason strings identify Foreign, None, and the independent application-state rule respectively, so logging needs no additional member state or changed `dismiss()` signature. Show/Hide state maintenance and guard-only returns need no decision line. Never log window titles, looked-up words, or activation tokens, or report focus as granted merely because activation was requested. A refused activation produces no event to explain; the logger cannot diagnose it.

## 4. Exact `ScanPopup` edits

### 4.1 Header and constructor

In `scanpopup.hh`, add `#include "popupdismiss.hh"` with the project includes (immediately after `mainstatusbar.hh`) and an explicit `#include <memory>`. Immediately after `Ui::ScanPopup ui;`, add:

```cpp
std::unique_ptr< PopupDismiss::FocusLossCloser > focusLossCloser;
```

Keep the existing `eventFilter()` and other event declarations. Add no `event()` or `changeEvent()` override, in either header or implementation.

In `ScanPopup::ScanPopup`, replace the opening of the body with:

```cpp
{
  ui.setupUi( this );
  focusLossCloser = std::make_unique< PopupDismiss::FocusLossCloser >( this );
  focusLossCloser->setDismissHandler( [ this ] { hideWindow(); } );

  openSearchAction.setShortcut( QKeySequence( "Ctrl+F" ) );
```

Here the constructor argument `this` is the observed widget, not the QObject parent argument; unique ownership is sufficient. Keep the intervening existing constructor code. At the stored pin restoration, insert the synchronisation as follows:

```cpp
  ui.pinButton->setChecked( cfg.pinPopupWindow );
  focusLossCloser->setPinned( ui.pinButton->isChecked() );

  if ( cfg.pinPopupWindow )
```

The required order is one constructor-wide sequence: `ui.setupUi( this )` → closer construction on `this` → handler naming `hideWindow()` → restoration using `cfg.pinPopupWindow` → `setPinned( ui.pinButton->isChecked() )` → the first `setWindowFlags(`. Creation and handler installation immediately follow setup. The restored button value, never literal `false`, is the argument to synchronisation. Retain the pinned/unpinned flag branches exactly. `setWindowFlags()` can synchronously hide a visible widget, so the closer must already know the restored state before either flag branch runs.

### 4.2 Pin changes and unpin activation

The first complete statement in `void ScanPopup::pinButtonClicked( bool checked )` must be:

```cpp
focusLossCloser->setPinned( checked );
```

It must precede the whole `if ( checked )` statement, including both flag-changing branches, not merely precede `show()`. Keep the existing branch bodies. After the shared `show();` and before the existing `if( checked ) pinnedGeometry = saveGeometry();`, insert:

```cpp
  if ( !checked && PopupDismiss::needsExplicitActivation( windowFlags() ) )
  {
    activateWindow();
    raise();
  }
```

A flag replacement hides and resets arming; the unpin show is a new opening. There is no request on the checked/pinning path or for an unpinned `Qt::Popup` replacement. No existing flag, title, geometry, timer, mouse, or dictionary-bar action changes.

### 4.3 Hidden-to-visible opening in `engagePopup()`

Within `if ( !isVisible() )` only, keep `show();`, then keep the entire existing `#ifdef ENABLE_SPWF_CUSTOMIZATION` region, including:

```cpp
    if ( QGuiApplication::platformName() == "xcb" && !ui.pinButton->isChecked() )
      giveFocus = true;
```

Replace only the existing `if ( giveFocus )` condition and retain its ordered actions:

```cpp
    if ( giveFocus || ( !ui.pinButton->isChecked() &&
                       PopupDismiss::needsExplicitActivation( windowFlags() ) ) )
    {
      activateWindow();
      raise();
    }
```

The shared `show()` precedes this guarded block. Preserve the following `interceptMouse()` path and both existing visible-window branches:

```cpp
  else
  if ( ui.pinButton->isChecked() )
  {
    // Pinned-down window isn't always on top, so we need to raise it
    show();
    activateWindow();
    raise();
  }
#ifdef ENABLE_SPWF_CUSTOMIZATION
  else
  if ( QGuiApplication::platformName() == "xcb" && ( windowFlags() & Qt::Tool ) == Qt::Tool )
  {
    // Ensure that the window with Qt::Tool flag always has focus on X11.
    activateWindow();
    raise();
  }
#endif
```

`engagePopup()` must still contain exactly three executable `activateWindow()` calls, attributed one each to the hidden-window, pinned-visible, and `xcb` `Tool` branches. It must not acquire a fourth unconditional or later-word activation call. Preserve the unpinned `xcb` `giveFocus = true;` assignment even though the additional managed-window predicate overlaps it for `Tool`.

No other pre-existing activation path changes. In particular retain Windows' existing `giveFocus` inputs, the default `giveFocus = false` parameter, all caller arguments, `MainWindow`'s `scanPopup->winId()` before scan-popup token application, the clipboard-lookup path without a forced handle, and `PlatformWindow::applyActivation()` behaviour. `NoHandle` means no native handle exists yet, not every clipboard lookup. No main-window or PlatformWindow edit is needed.

The resulting platform behaviour is fixed:

| Platform/form | Type | New opening request | Dismissal |
| --- | --- | --- | --- |
| Wayland, any stored form | `Tool | FramelessWindowHint | WindowStaysOnTopHint` | Yes when unpinned; pre-existing hotkey requests remain | New rule after focus is held; own popups continue; dialogs dismiss |
| `xcb`, default or Popup | `Qt::Popup` | Helper says false; pre-existing unpinned `giveFocus` remains | Qt's popup close, with the closer's common guards |
| `xcb`, Tool | Managed Tool | Yes when unpinned, overlapping existing `giveFocus` | New rule plus unchanged proximity/leave paths |
| Windows/macOS without X11 customization | Default `Qt::Popup` | Helper says false; existing caller requests remain | Qt's popup close |
| Pinned, hidden | Existing pinned flags | No additional request | No auto-hide |
| Pinned, already visible | Existing pinned flags | Existing re-raise retained | No auto-hide |

## 5. Build and suite inventory integration

In `goldendict.pro`, the enumerated HEADERS list currently ends with `platformwindow.hh`. Replace that last entry with:

```qmake
    platformwindow.hh \
    popupdismiss.hh
```

The enumerated SOURCES list currently ends with `platformwindow.cc`. Replace that last entry with:

```qmake
    platformwindow.cc \
    popupdismiss.cc
```

These additions are unconditional; the closer is not an X11-only module. Keep every existing entry and all conditional build logic.

In the ordinary-mode branch of `tests/tests.pro`, insert `tst_popup_dismiss` after `tst_platformwindow` and before `tst_portal_hotkeys`. The complete resulting required list is:

```qmake
    GD_REQUIRED_SUITE_NAMES = \
        tst_article_bridge tst_article_history tst_article_load \
        tst_article_schemes tst_article_stream tst_btree_index tst_encoding \
        tst_external_content tst_hotkeys tst_htmlescape tst_langcoder \
        tst_linkrewrite tst_platformwindow tst_popup_dismiss \
        tst_portal_hotkeys tst_print_pipeline tst_regex tst_save_article \
        tst_utf8_wstring tst_waylandselection tst_website_frames
```

Change the literal count and its error string together to:

```qmake
    !count(GD_TEST_NAMES, 21): error("Expected exactly 21 required suites; found: $$GD_TEST_NAMES")
```

Change the informational message to:

```qmake
    message("tests/tests.pro: exact 21 suites: $$GD_TEST_NAMES")
```

Also change the introductory comment to `# Ordinary mode is a closed acceptance inventory: exactly the 21 required suites.` Preserve the independent mismatch message `Required suite inventory mismatch: $$GD_TEST_NAMES`, the missing/registration errors, discovery, sorting, SUBDIRS assignment, and compile-closure branch. The two strings that change are the count-error text and the informational message above; do not substitute weaker discovery-only acceptance.

## 6. Exhaustive test suite specification

### 6.1 Files, project shape, and common fixture rules

Create `tests/tst_popup_dismiss/`. The complete `tst_popup_dismiss.pro`, following the production-linking shape of `tests/tst_platformwindow/tst_platformwindow.pro`, is:

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

The common `.pri` supplies C++17, `testcase`, QtTest, support sources, build directories, feature defines, and repository lookup. Do not copy PlatformWindow's unrelated hotkey/Xlib/KDE sources or its optional portable variant into this suite. Do not link `scanpopup.cc`, `MainWindow`, or a substitute implementation of `popupdismiss.cc`.

Use a `TstPopupDismiss : public QObject` QtTest class, `QTEST_MAIN( TstPopupDismiss )`, and `#include "tst_popup_dismiss.moc"`. Include the production header, `support/fixtures.hh`, QtTest, QApplication/QGuiApplication, QWindow/QWidget/QDialog, QSignalSpy/QPointer, `<type_traits>`, and file/directory/regular-expression containers needed by the source checks. Static type checks in the translation unit must accept the five complete free-function signatures and the public closer method signatures from §2, including the const getter; `std::is_constructible_v< PopupDismiss::FocusLossCloser, QWidget * >` and the two-argument equivalent pin construction with its default parent. These checks need no extra test slot.

`initTestCase()` must assert `TestSupport::requiredEnvironment( &reasons )` with joined diagnostic reasons, assert the `display` prerequisite through `TestSupport::prerequisiteAvailable()`, and install `TestSupport::PureWatchdogSeconds`. `cleanupTestCase()` clears the watchdog. The required runtime fixtures run under the explicit `xcb` + xvfb command in §8. If the display, a real focus transition, or a stated platform assumption cannot be established, fail with a diagnostic; do not use `QSKIP`, silently return, replace a real event with a copied rule, or use `QWidget::isActiveWindow()` as proof.

Each runtime case owns its widgets and closer, and destroys them in a safe order before the next case. Use production-like managed Tool flags for the observed `first` where appropriate, ordinary independent top-level windows for `second`/`third`, and real `Qt::Popup` widgets for owned menu fixtures. Unless a case deliberately exercises an empty handler, install a counting handler as described below. Establish and assert `QGuiApplication::focusWindow()` pointer identity with `QTRY_COMPARE` for every relied-on focus transition; assert visibility and non-null handles too. A helper that performs these QTRY assertions must return a success/failure result that the test itself asserts, so a failed helper cannot let the enclosing case continue as if it succeeded.

Before every transition that is supposed to cause the first dismissal, assert count zero and the observed window visible. The own-window activation that arms a case must have reached the production connection before the loss transition. Retained-menu cases assert zero after each menu focus transition as well. `QTRY_COMPARE` waits are test observation only; production has no grace timer. A final count alone cannot substitute for the zero-before-transition assertions.

There are exactly **29 substantive test slots** below: five matrices, twelve runtime cases, twelve source assertions. Five corresponding `_data` slots and `initTestCase`/`cleanupTestCase` are not counted as additional cases. Implement these exact names once each:

```cpp
private slots:
  void initTestCase();
  void cleanupTestCase();

  void focusMoveRule_data();
  void focusMoveRule();
  void explicitActivationRule_data();
  void explicitActivationRule();
  void ownedPopupRule_data();
  void ownedPopupRule();
  void showArmingRule_data();
  void showArmingRule();
  void popupWindowTypeRule_data();
  void popupWindowTypeRule();

  void hidesWhenFocusMovesToAnotherWindow();
  void pinnedPopupDoesNotHide();
  void ownedMenuKeepsPopupUntilFocusMovesAway();
  void nestedOwnedMenuKeepsPopupUntilFocusMovesAway();
  void ownDialogHidesPopupAndRemainsVisible();
  void focusReturningFromMenuKeepsPopup();
  void applicationInactiveSignalUsesProductionConnection();
  void reshowWithoutFocusStaysUnarmed();
  void showWhileAlreadyFocusedArmsPopup();
  void dismissHandlerIsNotReentered();
  void dismissHandlerMayDestroyCloser();
  void destroyingArmedCloserDisconnectsSignals();

  void scanPopupDeclaresCloserWithoutEventOverrides();
  void closerApplicationStateConnectionIsContextBound();
  void closerShowAndHideMaintainArming();
  void destinationClassifierUsesOwnedPopupIdentityAndType();
  void constructorRestoresPinBeforeChangingFlags();
  void pinButtonSynchronizesBeforeFlags();
  void engagePopupPreservesScopedActivationPaths();
  void unpinRequestsActivationAfterShow();
  void applicationProjectRegistersPopupDismiss();
  void requiredSuiteInventoryIsExact();
  void ownPopupBranchPreservesArming();
  void dismissInvokesFreshHandlerBehindSelfGuard();
```

### 6.2 Rule matrices: all rows

`focusMoveRule_data()` supplies boolean `visible`, `pinned`, `armed`, `applicationActive`, a destination value, and an independently specified expected Action. Register enum metatypes or store their integer values and cast at the call boundary. Row names are the hyphen-joined literal values of `(visible,pinned,armed,destination,applicationActive)` in this table. `0`/`1` mean false/true. Every row is explicit: no production call computes its own expected result.

| visible | pinned | armed | destination | applicationActive | expected |
| --- | --- | --- | --- | --- | --- |
| 0 | 0 | 0 | OwnWindow | 0 | Ignore |
| 0 | 0 | 0 | OwnWindow | 1 | Ignore |
| 0 | 0 | 0 | OwnPopup | 0 | Ignore |
| 0 | 0 | 0 | OwnPopup | 1 | Ignore |
| 0 | 0 | 0 | Foreign | 0 | Ignore |
| 0 | 0 | 0 | Foreign | 1 | Ignore |
| 0 | 0 | 0 | None | 0 | Ignore |
| 0 | 0 | 0 | None | 1 | Ignore |
| 0 | 0 | 1 | OwnWindow | 0 | Ignore |
| 0 | 0 | 1 | OwnWindow | 1 | Ignore |
| 0 | 0 | 1 | OwnPopup | 0 | Ignore |
| 0 | 0 | 1 | OwnPopup | 1 | Ignore |
| 0 | 0 | 1 | Foreign | 0 | Ignore |
| 0 | 0 | 1 | Foreign | 1 | Ignore |
| 0 | 0 | 1 | None | 0 | Ignore |
| 0 | 0 | 1 | None | 1 | Ignore |
| 0 | 1 | 0 | OwnWindow | 0 | Ignore |
| 0 | 1 | 0 | OwnWindow | 1 | Ignore |
| 0 | 1 | 0 | OwnPopup | 0 | Ignore |
| 0 | 1 | 0 | OwnPopup | 1 | Ignore |
| 0 | 1 | 0 | Foreign | 0 | Ignore |
| 0 | 1 | 0 | Foreign | 1 | Ignore |
| 0 | 1 | 0 | None | 0 | Ignore |
| 0 | 1 | 0 | None | 1 | Ignore |
| 0 | 1 | 1 | OwnWindow | 0 | Ignore |
| 0 | 1 | 1 | OwnWindow | 1 | Ignore |
| 0 | 1 | 1 | OwnPopup | 0 | Ignore |
| 0 | 1 | 1 | OwnPopup | 1 | Ignore |
| 0 | 1 | 1 | Foreign | 0 | Ignore |
| 0 | 1 | 1 | Foreign | 1 | Ignore |
| 0 | 1 | 1 | None | 0 | Ignore |
| 0 | 1 | 1 | None | 1 | Ignore |
| 1 | 0 | 0 | OwnWindow | 0 | Ignore |
| 1 | 0 | 0 | OwnWindow | 1 | Ignore |
| 1 | 0 | 0 | OwnPopup | 0 | Ignore |
| 1 | 0 | 0 | OwnPopup | 1 | Ignore |
| 1 | 0 | 0 | Foreign | 0 | Ignore |
| 1 | 0 | 0 | Foreign | 1 | Ignore |
| 1 | 0 | 0 | None | 0 | Ignore |
| 1 | 0 | 0 | None | 1 | Ignore |
| 1 | 0 | 1 | OwnWindow | 0 | Ignore |
| 1 | 0 | 1 | OwnWindow | 1 | Ignore |
| 1 | 0 | 1 | OwnPopup | 0 | Ignore |
| 1 | 0 | 1 | OwnPopup | 1 | Ignore |
| 1 | 0 | 1 | Foreign | 0 | Hide |
| 1 | 0 | 1 | Foreign | 1 | Hide |
| 1 | 0 | 1 | None | 0 | Hide |
| 1 | 0 | 1 | None | 1 | Ignore |
| 1 | 1 | 0 | OwnWindow | 0 | Ignore |
| 1 | 1 | 0 | OwnWindow | 1 | Ignore |
| 1 | 1 | 0 | OwnPopup | 0 | Ignore |
| 1 | 1 | 0 | OwnPopup | 1 | Ignore |
| 1 | 1 | 0 | Foreign | 0 | Ignore |
| 1 | 1 | 0 | Foreign | 1 | Ignore |
| 1 | 1 | 0 | None | 0 | Ignore |
| 1 | 1 | 0 | None | 1 | Ignore |
| 1 | 1 | 1 | OwnWindow | 0 | Ignore |
| 1 | 1 | 1 | OwnWindow | 1 | Ignore |
| 1 | 1 | 1 | OwnPopup | 0 | Ignore |
| 1 | 1 | 1 | OwnPopup | 1 | Ignore |
| 1 | 1 | 1 | Foreign | 0 | Ignore |
| 1 | 1 | 1 | Foreign | 1 | Ignore |
| 1 | 1 | 1 | None | 0 | Ignore |
| 1 | 1 | 1 | None | 1 | Ignore |

`focusMoveRule()` fetches each row and compares the production result with its expected Action. Preconditions: none beyond live test application setup; this function itself performs no window I/O. It must fail when any visibility/pin/arming guard is deleted or inverted, Foreign wrongly requires inactivity, OwnWindow/OwnPopup hides, None ignores application state, or an `&&`/`||` change alters any row. There are three Hide rows, not a blanket hide-on-null-focus rule.

`explicitActivationRule_data()` supplies flags and expected bool with these exact rows; `explicitActivationRule()` compares the production helper for each:

| Row | flags | expected |
| --- | --- | --- |
| popup | `Qt::Popup` | false |
| popup-frameless | `Qt::Popup | Qt::FramelessWindowHint` | false |
| popup-stays-on-top | `Qt::Popup | Qt::WindowStaysOnTopHint` | false |
| popup-both-hints | `Qt::Popup | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint` | false |
| tool | `Qt::Tool` | true |
| managed-tool | `Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint` | true |
| window | `Qt::Window` | true |
| dialog | `Qt::Dialog` | true |
| tool-bypass | `Qt::Tool | Qt::X11BypassWindowManagerHint` | true |

Fixture/preconditions: flags values only, no native window needed. Mutants: compare all flags without the type mask; test whether Popup bits are contained; invert the result; treat all tool windows as already focused. This matrix pins the new activation policy without claiming the compositor grants a request.

### 6.3 Helper matrices

`ownedPopupRule_data()` uses row identifiers and expected booleans. `ownedPopupRule()` creates the actual QWidget ownership graph inside each row rather than passing dangling fixture pointers through data setup. All popups below have `Qt::Popup` flags, but are not shown. A shared parent can own `owner` and `sibling` for the sibling row; `other` is a distinct top-level. Preconditions: a QApplication, correct asserted `parentWidget()` relationships, and `owner != candidate` except in self rows. Both pointer-null cases must be independently covered.

| Row | owner | candidate / graph | expected |
| --- | --- | --- | --- |
| both-null | null | null | false |
| null-owner | null | live popup widget | false |
| null-candidate | live owner | null | false |
| self | live widget | that same widget | false |
| popup-self | `Qt::Popup` widget | that same popup widget | false |
| child-popup | owner | popup parented directly to owner | true |
| grandchild-popup | owner | popup parented to an intermediate child of owner | true |
| nested-popup | owner | submenu popup parented to a popup parented to owner | true |
| sibling | owner | another child of owner's parent | false |
| unrelated-top-level | owner | independent top-level | false |
| other-window-menu | owner | popup parented to other | false |

Assert the helper's result equals the literal expected bool. Mutants: null dereference, accept owner as its own menu, direct-parent-only ownership, ownership that stops at a popup boundary, accept any popup regardless of its owner, or accidentally accept a sibling.

`showArmingRule_data()` supplies identities `same`, `different`, `null-own`, `null-focus`, and `both-null`. `showArmingRule()` uses two live, unshown `QWindow` objects A/B solely for pointer identity. No handle creation or focus event is required.

| Row | ownWindow | focusWindow | expected |
| --- | --- | --- | --- |
| same | `&A` | `&A` | true |
| different | `&A` | `&B` | false |
| null-own | null | `&A` | false |
| null-focus | `&A` | null | false |
| both-null | null | null | false |

Assert exact bool equality. Mutants: always arm; always disarm; use bare pointer equality so two nulls arm; accept any non-null focus instead of the own handle. The source test in §6.6 separately pins use of the helper at Show.

`popupWindowTypeRule_data()` has the five rows below. `popupWindowTypeRule()` compares the production predicate directly. Preconditions: none; fixture is a WindowType value. These are independent expected literals, not a call to another implementation of the classifier.

| Row | type | expected |
| --- | --- | --- |
| popup | `Qt::Popup` | true |
| tooltip | `Qt::ToolTip` | false |
| dialog | `Qt::Dialog` | false |
| tool | `Qt::Tool` | false |
| window | `Qt::Window` | false |

Mutants: accept Dialog, ToolTip, or all types; reject Popup; use a bit-containment test that treats Tool as Popup. This tests the helper in isolation. The separate classifier source assertion pins that the helper is actually called.

### 6.4 Runtime cases, in ascending design fixture order

Each entry inherits the required real-focus, zero-before-transition, and fail-rather-than-skip rules in §6.1. Parent ownership, `activePopupWidget()` identity, and focus-window identity are assertions, not assumptions. Cases 7 and 12 deliver the application-state signal explicitly as described; all stated focus transitions use the real QPA.

1. **Design 8.2(1): `hidesWhenFocusMovesToAnotherWindow()`.** Construct the closer on `first` before its Show. Install a handler that increments `count` and hides `first`. Show/activate `first`, wait for its handle to be the focus window, and assert visible and count zero. Show/activate independent `second`, assert its actual focus identity, then `QTRY_COMPARE( count, 1 )` and assert `first` hidden. Further processing of the resulting Hide must leave count one. Kills a disconnected focus connection, an absent OwnWindow arming assignment, a Foreign Ignore, or a callback omitted for a Hide decision. A hide before the second transition fails the zero assertion.

2. **Design 8.2(2): `pinnedPopupDoesNotHide()`.** Same fixture/drive, but call `setPinned( true )` before showing and assert `isPinned()`. After own focus and count zero, move real focus to `second`; count remains zero and `first` remains visible. Kills omission/inversion of the pin guard, including a handler that bypasses the common rule. Do not claim to runtime-test ScanPopup's pin wiring; its source assertions cover that.

3. **Design 8.2(3): `ownedMenuKeepsPopupUntilFocusMovesAway()`.** Arm visible `first` with the counting-and-hiding handler. Show an owned `QWidget menu( &first )` after setting its flags to `Qt::Popup`. Require `QApplication::activePopupWidget() == &menu`, `QGuiApplication::focusWindow() == menu.windowHandle()`, non-null menu handle, and `menu.windowHandle()->type() == Qt::Popup`. Count stays zero and `first` visible with focus genuinely on the menu. Move focus to `second` (closing the menu first if needed to let the QPA move focus); assert that destination, count one, and `first` hidden. Observe and record **every** `focusWindowChanged` payload between the asserted menu focus and the `second` loss, including any `OwnWindow` return caused by closing the menu, so the fixture states the transition sequence it actually drove instead of assuming one. Kills deletion of the OwnPopup recognition branch: without it the menu focus is `Foreign` and the zero-count assertion fails immediately. The "OwnPopup clears arming" mutant is **not** claimed here, because a menu close can legitimately re-arm through `OwnWindow`; it is pinned by `ownPopupBranchPreservesArming()` (§6.6(11)). A test that only observes focus still on `first` is invalid. (Spec review R1-M2)

4. **Design 8.2(3a): `nestedOwnedMenuKeepsPopupUntilFocusMovesAway()`.** Arm `first`; show/focus its popup menu as in case 3. Open a second `Qt::Popup` widget parented to that menu. Require `activePopupWidget() == &submenu`, focus exactly `submenu.windowHandle()`, its exact Popup type, and actual nested QWidget ownership. Count remains zero after each opening. Close popups as necessary and move directly to `second`, assert that focus, count one, and `first` hidden. Kills immediate-owner-only recognition, an outer-menu-only classifier, or deletion of the identity-based OwnPopup branch. It proves the current identity classifier, not the removed transient-chain algorithm.

5. **Design 8.2(4): `ownDialogHidesPopupAndRemainsVisible()`.** Give `first` production-like `Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint` flags. Arm it, assert zero, then show a **modal but non-blocking** `QDialog` parented to `first`:
construct it, call `dialog.setModal( true )`, then `dialog.show()` (never `exec()`).
Assert `dialog.isModal()` is true, `QApplication::activeModalWidget() == &dialog`,
`dialog.parentWidget() == &first`, focus exactly `dialog.windowHandle()`, and
`dialog.windowHandle()->type() == Qt::Dialog`. Only then require count one, `first` hidden,
and the dialog still visible. Do not activate a sibling while this modal dialog holds the
application. Kills a modal/dialog exemption in the handler — an implementation that returns
early whenever `QApplication::activeModalWidget()` is non-null leaves count at zero here and
fails — and a classifier that treats every owned/transient dialog as `OwnPopup`. (Spec review R1-M1) This fixture does **not** isolate deleting only the classifier's type check: other conditions reject this normal dialog already. Its type predicate and required call are pinned by the helper matrix and the source assertion respectively. Hiding the real scan popup cannot reject this dialog through `closeMenu()`: `ArticleView`'s resource-save action is gated by `if( url.isEmpty() || popupView ) return;` before creating its only closeMenu-rejected dialog.

6. **Design 8.2(5): `focusReturningFromMenuKeepsPopup()`.** Use a fresh instance of case 3's owned-menu fixture through its asserted menu-focus and zero-count step. Close the menu and activate `first` if needed to drive the real return; require focus identity back on `first`, count still zero, and `first` visible. No foreign focus intervenes. Kills a dismissal on `OwnWindow` return or a menu-close dismissal. The design forbids any asynchronous dismissal — there is no timer, grace or queued settlement anywhere in the module — so this fixture makes no claim about killing one, and adding one would be a design deviation rather than a surviving mutant. It does not independently claim to kill missing re-arming, since this interaction was already armed. (Spec review R1-m3)

7. **Design 8.2(6): `applicationInactiveSignalUsesProductionConnection()`.** Three independently reset subfixtures use the production connection, not an exposed test handler. In the positive subfixture, show/activate and arm `first`, assert own focus, visibility, unpinned state, and count zero. Assert the invocation succeeds:

   ```cpp
   QMetaObject::invokeMethod(
     qApp, "applicationStateChanged",
     Q_ARG( Qt::ApplicationState, Qt::ApplicationInactive ) )
   ```

   Require count one and `first` hidden. Repeat with an armed visible pinned window (`setPinned( true )`, getter asserted): count zero, still visible. Repeat with a visible unarmed window: set `Qt::WA_ShowWithoutActivating` before its Show, keep `second` genuinely focused throughout that Show, assert zero and that focus, then invoke the same signal; count zero, still visible. This delivers the real application's signal to the actual installed lambda without claiming it changed the OS application's state. Kills a deleted/miswired state connection, a slot depending on the current focus becoming null, a slot re-reading Active instead of using the payload, or bypassed pin/arming guards. No copied `dismiss()` or rule invocation satisfies this fixture.

8. **Design 8.2(7): `reshowWithoutFocusStaysUnarmed()`.** Arm `first` by focus, then hide it **directly** with `first.hide()`, without moving focus to `second` first and without entering `dismiss()`, so the only thing that can clear arming is the `Hide` event. Now give focus to `second` (focus it explicitly and assert the identity), set `Qt::WA_ShowWithoutActivating` on `first` for the re-show, show `first`, and assert it is visible while `focusWindow() == second.windowHandle()` and count zero. Show/activate `third` and assert its focus identity. Count remains zero and `first` visible. Preconditions include that the re-show really did not focus `first`. Kills stale arming retained across Hide/Show, unconditional arming on Show, or a missing unarmed guard; a failure to establish inactive re-show must fail, not be relabelled a refusal proof. The isolated `Hide`-reset and `Show`-assignment mutants are pinned by `closerShowAndHideMaintainArming()` (§6.6(3)), because a Show that re-derives arming from focus also masks a missing `Hide` reset at runtime. (Spec review R1-m1)

9. **Design 8.2(7a): `showWhileAlreadyFocusedArmsPopup()`.** First show/activate `first` and assert its own handle is the focus window. Construct the closer **while `first` is already visible and focused**; it starts unarmed. Install its counting-and-hiding handler, a focus-signal spy on the real application, and a test-side Show event filter installed after the closer so it observes the second Show before the production filter runs. That observer records the non-null own handle, actual focus handle, and spy count at the Show itself. Hide and immediately show `first` again without pumping away the focus relationship. At that second Show, require a non-null own handle and actual focus identity equal to it. Inspect the spy's payloads and assert that no own-window `focusWindowChanged` signal has armed the closer between its construction and this Show/next loss. Assert count zero before moving focus to `second`, assert `second`'s focus identity, then require one dismissal and `first` hidden. The only permitted arming cause in this fixture is Show. If the QPA clears focus on hide, or emits a new own-focus signal that would independently arm it, fail with that platform-assumption diagnostic; do not skip, inject an own-focus signal, or accept another arming cause. Kills deleting the Show arming assignment or forcing its outcome false, with the source assertion and pure matrix as independent coverage.

10. **Design 8.2(8): `dismissHandlerIsNotReentered()`.** Arm visible `first`, count zero, and drive a loss to `second`. Its handler increments the counter and synchronously drives focus moves involving `second` and a reactivation of `first`, observing each real handle. Ensure an additional Foreign focus event is actually delivered before the outer handler returns: after the reactivation of `first`, activate `second` again and then reactivate `first`, with real focus assertions while processing events inside the handler. This repeated pair makes the guard falsifiable rather than merely sending activation requests whose effects occur after return. On a recursive entry, increment but do not repeat the driving sequence, so an unguarded mutant fails finitely. Record that the nested foreign event occurred while the outer handler was on the stack, and require final count exactly one. The handler in this fixture need not hide; its synchronous focus effects are the stimulus. Kills deleting the `dismissing` guard or clearing it before the callback. Absence of a nested eligible decision is a fixture failure, not a guard proof.

11. **Design 8.2(9): `dismissHandlerMayDestroyCloser()`.** Use a separately owned closer, a QPointer observer, and visible armed `first`. Its handler increments once and destroys/resets the closer synchronously. Assert count zero before driving focus to `second`, then assert that focus, count one, and null self observer. Drive later focus back to `first` and away again and assert those handles; count remains one and the process does not crash. Do not leave a unique_ptr owning already-deleted storage. This is a crash-safety and
no-extra-callback check, not a mutant discriminator: without memory-error instrumentation a
write into freed storage can complete silently, so the missing-self-guard mutant is pinned by
`dismissInvokesFreshHandlerBehindSelfGuard()` (§6.6(12)) instead. There is no claim that the
destruction handler hides the widget. (Spec review R1-M3)

12. **Design 8.2(10): `destroyingArmedCloserDisconnectsSignals()`.** Arm visible `first` with a live counting handler and assert count zero. Destroy the closer before the loss; assert a QPointer to it is null while `first` still exists. Move focus to `second`, back to `first`, then away again, asserting the relied-on handles; deliver the Inactive signal on the real application as well. Count remains zero and no crash occurs. This is an overall destruction-safety check. It does
not isolate a receiver/lifetime mutant: both connections are explicitly disconnected by §3(6)
and `QObject` destruction removes installed event filters anyway, so context-stripping and
filter-teardown deletions are pinned by `closerApplicationStateConnectionIsContextBound()`
(§6.6(2)) and by the mandatory teardown procedure in §3(6), not by this fixture.
(Spec review R1-m2)

### 6.5 Directive-stripping and scoped-source helper contract

The new suite carries its own copies/adaptations of `tst_hotkeys`' source-assertion helpers. They read from `TestSupport::repositoryRoot()` and fail when a required file, anchor, scope, or complete statement is missing. They must not be satisfied by a comment, string containing code, a different function, or a call buried in a lambda unrelated to the required action.

Before C++ tokenisation or statement splitting, apply `stripPreprocessorDirectives( QString const & text )`. It removes preprocessor directive lines: a directive begins when the first non-whitespace character on a physical line is `#` outside a comment/string/character literal. Replace each removed physical line with a newline, including every backslash-continued line (recognising both LF and CRLF); preserve all other characters on ordinary code lines and both sides of conditional compilation. Do not evaluate the directives. This prevents `#ifdef`/`#endif` tokens from being joined to a following complete statement. Keep a separate raw-text view for assertions about an actual directive and its enclosed region. qmake scope/list parsing uses its own raw syntax, not the C++ normaliser.

Required helper signatures and semantics, matching the cited style:

```cpp
QString stripPreprocessorDirectives( QString const & text );
QString selectionBlock( QString const & text, QString const & anchor );
QString selectionBranch( QString const & text, QString const & condition );
QStringList selectionStatements( QString const & text );
QString selectionDirectActions( QString const & text );
bool selectionOrdered( QString const & text, QStringList const & needles );
```

- Normalise C++ by lexing identifiers/operators/literals, dropping comments and syntax whitespace while preserving token boundaries and complete string/character literal contents. Directive stripping precedes this step. Do not use global whitespace deletion that would merge identifiers or alter literal contents. A literal token can be matched as a literal, but its contents cannot count as executable statements.
- `selectionBlock` finds the token-normalised anchor and returns the matching balanced-brace body; braces in comments/literals do not change depth. Missing/malformed anchors yield empty output that the caller must reject. Apply it first to the named function/constructor, then to the relevant branch/lambda inside that function.
- `selectionStatements` splits a block only at complete statement boundaries, retaining an entire `if`/`else if`/`else` chain, loop, initializer, and lambda body within its own complete statement. It does not flatten a controlled action into a top-level action. In particular the first statement of `pinButtonClicked()` is not "the first direct non-control action".
- `selectionBranch` extracts the immediate consequent of the matching control at the inspected scope, with a balanced block or complete single statement. It never searches arbitrary nested functions/lambdas for an action. To inspect an `else if`, explicitly inspect that arm of the same top-level conditional chain (retained by `selectionStatements`), not a later independent matching condition. The engagePopup assertion must partition that one hidden/visible conditional chain and inspect its three arms individually.
- `selectionDirectActions` returns only direct non-control statements at the inspected scope, excluding controlled branches/loops/standalone blocks. Use branch extraction before testing a branch's direct activation actions. Do not use it to determine the first complete statement.
- `selectionOrdered` searches token-normalised needles in sequence, advancing past each match within the supplied block. Supply the full constructor expectation as one call, and independently ensure the final flag needle refers to the **first** flag call. Optional braces around exactly one simple controlled statement may be normalised as in `tst_hotkeys`; nested controls and multi-statement scopes must stay intact.

For the preserved `giveFocus` assertion, perform two independent checks: find the real `#ifdef ENABLE_SPWF_CUSTOMIZATION` directive in the raw hidden-window region, then check the full xcb/unpinned condition and `giveFocus = true;` assignment inside that directive's matching region. Do not try to match a directive plus an assignment as a single C++ statement; do not let a same-text assignment elsewhere satisfy the check.

### 6.6 Source assertion cases

The common precondition for every source assertion is a resolvable repository root and readable real repository files, with non-empty extracted required scopes. These assertions prove wiring that the new suite cannot link, or a mandatory classifier discriminator that is not isolated by the normal-dialog runtime case. They are explicitly source evidence, not GUI execution evidence.

1. **`scanPopupDeclaresCloserWithoutEventOverrides()`.** Read `scanpopup.hh` and `scanpopup.cc`. Assert the real include `"popupdismiss.hh"`, explicit `<memory>`, and exact `std::unique_ptr< PopupDismiss::FocusLossCloser > focusLossCloser;` member in ScanPopup. Inspect the class declaration and qualified method definitions for absence of `event(` / `changeEvent(` overrides; the existing `eventFilter(` must not trigger that absence check. Kills removing/changing the member/include or adding activation-event overrides instead of the specified module wiring.

2. **`closerApplicationStateConnectionIsContextBound()`.** Read the production closer constructor in `popupdismiss.cc`. Assert a stored `stateConnection` made by `QObject::connect( qApp, &QGuiApplication::applicationStateChanged, this, ... )`. Inspect that lambda's own body: non-Inactive payloads return, the null-window guard precedes access, the common rule uses the Inactive payload independently of current focus, and Hide calls `dismiss( "application inactive" )`. Assert no unconditional dismissal for Active/Hidden/Suspended and no re-read state substituting for the payload. Also pin the stored focus connection's `this` receiver in the same constructor. Kills deleting/context-stripping the state connection, moving the inactive action outside its guard, or wiring a copied/test-only handler. Case 7 is its behavioural twin.

3. **`closerShowAndHideMaintainArming()`.** Extract `FocusLossCloser::eventFilter` and its Show and Hide branches. Assert Show's full assignment is `armed = PopupDismiss::armedAfterShow( window->windowHandle(), QGuiApplication::focusWindow() );`, Hide assigns `armed = false;`, and both occur only for the observed widget. Assert the filter does not consume Show/Hide. Kills deleting Show maintenance, replacing an argument with a literal/other handle, assigning constant true/false, or omitting the Hide reset. The helper matrix pins function semantics; runtime case 9 pins its already-focused Show path.

4. **`destinationClassifierUsesOwnedPopupIdentityAndType()`.** Extract `FocusLossCloser::destinationOf`. Assert, in order, null observed widget → Foreign, null focus → None, own-handle equality → OwnWindow, acquisition of `QApplication::activePopupWidget()`, guarded `isOwnedPopup( window, activePopup )`, acquisition of that active popup's `windowHandle()`, and the single conjunction `popupHandle && focusWindow == popupHandle && isPopupWindowType( focusWindow->type() )` → OwnPopup, then the final Foreign return. Assert the required identities and type call belong to that same branch. Assert no transient/parent-window walk and no activation-state read in the classifier. Kills deleting just the classifier's type check, substituting a dialog/transient shortcut, admitting an unrelated active popup, or changing precedence. This is the source-only pin for removing just the type check; do not attribute that kill to runtime case 5.

5. **`constructorRestoresPinBeforeChangingFlags()`.** Extract the whole `ScanPopup::ScanPopup` body. Use one ordered expectation containing all six needles: `ui.setupUi( this )`; construction of `PopupDismiss::FocusLossCloser` on `this`; `setDismissHandler` with the lambda calling `hideWindow()`; `ui.pinButton->setChecked( cfg.pinPopupWindow )`; `focusLossCloser->setPinned( ui.pinButton->isChecked() )`; first `setWindowFlags(`. The first three complete statements must be the setup, unique_ptr assignment, and handler installation from §4.1. Independently assert that synchronisation precedes the first flag call in either branch and follows restoration, with the full button-state argument. Kills omitting creation/handler, constructing late, using literal false, moving synchronisation before restoration, or moving it after a flag change. Searching for unordered substrings alone is insufficient.

6. **`pinButtonSynchronizesBeforeFlags()`.** Extract `ScanPopup::pinButtonClicked`; require a non-empty `selectionStatements( body )` and compare its **first element**, after token/whitespace normalisation, with the complete `focusLossCloser->setPinned( checked );`. Assert the flag-changing checked/unchecked chain follows it. Kills moving synchronisation below the whole `if ( checked )` but above `show()`, deleting it, or passing a different value. A check of only the first `selectionDirectActions()` entry would miss the forbidden move and is not accepted.

7. **`engagePopupPreservesScopedActivationPaths()`.** Extract `ScanPopup::engagePopup`, partition its hidden/visible conditional chain, and assert all of the following together:

   - Only the hidden-window arm contains the full new condition `if ( giveFocus || ( !ui.pinButton->isChecked() && PopupDismiss::needsExplicitActivation( windowFlags() ) ) )`.
   - That guard's consequent has direct `activateWindow();` then `raise();`, and the hidden arm's direct `show();` precedes the guarded request.
   - Across the function, exactly three executable `activateWindow()` calls exist, with one in that consequent, one in the pinned-visible arm guarded by `ui.pinButton->isChecked()`, and one in the `xcb` Tool arm guarded by `QGuiApplication::platformName() == "xcb" && ( windowFlags() & Qt::Tool ) == Qt::Tool`. Each preserved arm retains its ordered actions; pinned-visible retains `show()` first.
   - The raw hidden arm retains `#ifdef ENABLE_SPWF_CUSTOMIZATION`; independently, that region retains the exact xcb/unpinned `if` guard and `giveFocus = true;` controlled assignment, before the widened activation condition.

   Kills removal of the new predicate; removal/reordering of `show`, activate, or raise; an unconditional fourth request; deletion/movement of either preserved request; weakened pin/xcb guards; or removal of the preserved assignment. A global count without branch attribution is insufficient.

8. **`unpinRequestsActivationAfterShow()`.** Extract `pinButtonClicked`. Locate the shared direct `show();` after the whole pin/unpin chain; after it, require the exact `if ( !checked && PopupDismiss::needsExplicitActivation( windowFlags() ) )` with direct `activateWindow();` then `raise();`. Confirm the request precedes the final checked-geometry save and is not inside the pinning branch or before the shared Show. Kills omission, unconditional request, missing `!checked`, inverted helper, or pre-show request.

9. **`applicationProjectRegistersPopupDismiss()`.** Parse the enumerated HEADERS/SOURCES lists in `goldendict.pro`, respecting continuations and qmake comments. Assert `popupdismiss.hh` appears once in HEADERS immediately after `platformwindow.hh`, and `popupdismiss.cc` once in SOURCES immediately after `platformwindow.cc`; neither is only in a comment or platform-specific scope. Kills an unlinked module, wrong-list registration, or an X11-only registration. The application link command is the independent build acceptance for this wiring.

10. **`requiredSuiteInventoryIsExact()`.** Parse `GD_REQUIRED_SUITE_NAMES` from the ordinary-mode branch of `tests/tests.pro`; discover real `tests/tst_*/tst_*.pro` files at the two directory levels used by qmake; collect and sort their extensionless basenames. Assert both lists equal the full 21-name list in §5, contain `tst_popup_dismiss` once between platformwindow and portal_hotkeys, and have no duplicates, missing, or extra entries. Parse and compare the actual `!count(GD_TEST_NAMES, 21)` literal against both list sizes. Assert both exact changed strings from §5, the introductory 21-suite comment, and the retained independent inventory-equality guard/error. Kills adding a suite without registration, stale count/messages, a missing/extra project, registration in closure mode only, or removal of inventory equality validation. A bare textual presence of `tst_popup_dismiss` does not suffice.

11. **`ownPopupBranchPreservesArming()`.** Extract the `OwnPopup` arm of `destinationOf()`/the
    `focusWindowChanged` handler, whichever carries it, and assert that this arm contains
    **no assignment to `armed`**: no `armed =`, no `armed(`/`armed,` mutation call, and no
    `cancel`/`clear` helper. Assert as well that the same arm performs no `dismiss(` call and
    no visibility write. This is the deterministic pin for the "OwnPopup clears arming" mutant,
    which case 3's runtime drive cannot isolate: closing the owned menu may legitimately return
    focus to the popup, re-arming through `OwnWindow` before any foreign loss is observed.
    A runtime fixture may not claim that kill. (Spec review R1-M2)

12. **`dismissInvokesFreshHandlerBehindSelfGuard()`.** Extract `FocusLossCloser::dismiss`.
    Assert, in order: the `dismissing` early return; `dismissing = true`; a local self-guard
    of the form `QPointer< FocusLossCloser > <name>( this )`; a local copy of `dismissHandler`
    assigned *before* the call; the single invocation of that local copy; a null test of the
    self-guard immediately after the invocation; and — only after that test — the writes that
    clear `dismissing` and `armed`. Assert no member is read after the invocation before the
    guard test, and that `window` is not dereferenced after the invocation. This is the
    deterministic pin for the "missing self-guard" mutant, which a runtime crash-safety case
    cannot distinguish because writes into freed storage may complete silently and the
    acceptance commands run no memory-error instrumentation. (Spec review R1-M3)

### 6.7 Evidence limits and mutation accounting

The required mutation mappings in §9 describe expected failures of these cases; this document does not assert that any mutation was run. A build/import/fixture-precondition failure is not evidence of the intended behavioural kill. Each runtime kill requires the documented passing preconditions and the named assertion failing after the targeted mutation. A runtime fixture may not skip its OwnPopup focus identity and still count as coverage.

The injected application-state case proves delivery to the real production connection and its payload handling. It does not prove the compositor generates that signal during a real switch to another process. Likewise no xcb fixture proves native Wayland activation is granted. The already-focused Show fixture explicitly fails if its platform assumption is not met; the independent pure/source assertions do not turn that failure into a passing runtime acceptance. Equivalent conjunction reordering and deleting only redundant explicit disconnects have no claimed runtime kill.

## 7. Exact documentation change

Edit only `docs/wayland-limitations.md` for user-facing documentation. In §2, immediately after the paragraph ending `Hover translation and X11 focus restoration remain X11-only.` and before `Coverage is limited to the ext protocol.`, add this paragraph verbatim:

> An unpinned scan popup requests keyboard activation when it opens and closes as soon as focus leaves its own interaction, including when a dialog it opened takes the keyboard, as on X11. Its own menus and combo dropdowns keep the popup open, and pinned popups never auto-hide. If the compositor refuses activation, the popup remains open; click it to give it focus, after which clicking away hides it and Esc closes an open search panel first, or otherwise hides the popup.

After that paragraph, add this evidence-limit paragraph verbatim:

> Automated tests cover the shared popup rule and its signal handlers on xcb. The compositor's focus decision and a genuine application-inactive transition require a live session and are not claimed here.

In §3, keep the existing paragraph starting `Activation is best-effort:` unchanged. Immediately after its final sentence `restoration stay X11-only (see §2).`, and before §4, add this paragraph verbatim:

> For the scan popup, a refused activation request means it must be dismissed manually, as described in §2. This is the documented consequence of best-effort activation rather than a regression.

Do not rewrite old evidence totals or claim new observed results when adding these specified limits. No README change is required.

## 8. Acceptance commands

Run these commands verbatim from the implemented repository root during implementation acceptance, in order:

```bash
qmake6 goldendict.pro -o Makefile && make -j"$(nproc)"          # application links (D7)
qmake6 tests/tests.pro -o tests/Makefile
make -C tests -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check
```

All commands must succeed. The new suite is the 21st required ordinary-mode suite; every required new runtime case must execute without skips. `tests/tests.pro` does not select a QPA: the last command selects xcb explicitly. The application link is required in addition to the module suite. These are acceptance instructions, not commands executed by the specification drafter.

## 9. Traceability

### 9.1 Design decisions and sections to this specification

The approved design assigns D1–D5 and D7–D9; it contains no D6 label. The D6 row below records that numbering fact without inventing a decision or an implementation choice.

| Approved design item | Contract carried here | Specification sections |
| --- | --- | --- |
| D1: full X11 parity | Opening activation, immediate focus-loss dismissal, refusal limit | 1, 2.2, 3, 4, 6.2–6.4, 7 |
| D2: no preference | No UI/configuration/default change | 1, 4 |
| D3: focus/state signals | Identity-based classification and independent Inactive signal | 2.3, 3, 6.4(1–7), 6.6(2–4) |
| D4: new linkable module | Complete public/private API, production-linked suite | 2, 3, 5, 6.1 |
| D5: no new later-word request | Hidden-only widening, preservation of existing visible branches | 1, 4.3, 6.6(7) |
| D6: not assigned in design | No decision inferred from the missing label | 9.1 |
| D7: application integration | Both project lists, link acceptance | 5, 6.6(9), 8 |
| D8: own dialogs dismiss | Foreign-dialog treatment and safe real hide path | 1, 2.3, 4.1, 6.4(5), 7 |
| D9: no settlement timer | Synchronous event decisions, arming only, no suppression state | 1, 2, 3, 6.4 |
| §1 Decision record | All assigned choices above | 1–9.1 |
| §2 Background | Real activation/hide paths and QPA flags preserved | 1, 4, 5, 6.4(5), 7 |
| §3 Goals | Parity after granted focus; pins and evidence limits | 1, 3, 4, 6.7, 7, 8 |
| §4 Non-goals | Existing callers, pointer behaviour and UI preserved | 1, 3(5), 4, 7 |
| §5 Behaviour (whole) | Opening, loss, own menus/dialogs, unpin, refusal | 1–4, 6.2–6.4 |
| §5.1 Opening | Masked helper and hidden/open request | 2.2, 4.2–4.3, 6.2 |
| §5.2 Closing rule | Exact five-input expression and event execution | 2.2–2.3, 3, 6.2 |
| §5.3 Dialogs, menus, application | Identity/type classifier and Inactive connection | 2.3, 3(3–4), 6.4(3–7), 6.6(2,4) |
| §5.4 Unpinning | Pin state before flags; request after shared Show | 4.2, 6.6(6,8) |
| §5.5 Nothing happens | Hidden/pinned/unarmed/owned-popup ignores | 2.2, 3, 6.2–6.4, 7 |
| §6 Architecture (whole) | Module, event lifecycle, integration | 2–5 |
| §6.1 Module | Exact header, helpers, guards and logs | 2, 3, 6.1–6.3 |
| §6.2 Event model | Numbered connection/filter/handler/destructor procedure | 2.3, 3, 6.4, 6.6(2–4) |
| §6.3 ScanPopup wiring | Constructor, pin/unpin, hidden activation, project lists | 4, 5, 6.6 |
| §6.4 Platform/form table | Managed Wayland/Tool addition, Qt Popup and caller preservation | 1, 4.3, 6.2, 6.6(7) |
| §7 Error handling and edge cases | Nulls, already-focused Show, re-show, lifetime, refusal | 2, 3, 4.2, 6.3–6.4, 6.7, 7 |
| §8 Test strategy (whole) | Required production-linked 21st suite | 5, 6, 8, 9.2 |
| §8.1 Matrices | Full rule rows and helper expectations | 6.2–6.3 |
| §8.2 Runtime | Fixtures 1,2,3,3a,4,5,6,7,7a,8,9,10 in order | 6.4, 9.2 |
| §8.3 Source assertions | Scoped parsing, restoration/order, three requests, inventory | 6.5–6.6, 9.2 |
| §8.4 Not proven | Native grant/refusal and genuine application-inactive limit | 1, 6.4(7), 6.7, 7 |
| §8.5 Commands | Verbatim application and full-suite acceptance | 8 |
| §9 Documentation | Exact additions in existing limit sections | 7 |
| §10 Risks | Focus theft accepted, refusal documented, menu/dialog distinction, lifetime/guard/build coverage | 1–3, 5–8 |
| §11 Out of scope | No unrelated window, watcher, pointer, grab, or UI changes | 1, 4, 7 |
| §12 Implementation-order note | Module/rule → event model → wiring → build/inventory → docs | 2 → 3 → 4 → 5 → 7 |
| §13 Review ledger (whole) | Durable findings integrated; obsolete settlement/transient models excluded | 1–6, 9.1–9.2 |
| §13 Round 1 → 2 | Pin/unpin order, build, preserved activation, strict source checks | 3–6 |
| §13 Rounds 2 → 3 and 3 → 4 summary | Full first statement, restored argument, attributed requests, guard, preconditions | 3, 4, 6.4–6.6 |
| §13 Round 4 → 5 | No activation-state reads/timer, callback lifetime, no self-popup | 2, 3, 6.3–6.6 |
| §13 Round 5 → 6 | Show helper/fixture and owned/nested menu evidence retained; transient mechanism superseded | 2.2–2.3, 6.3, 6.4(3,4,9), 6.6(3,4) |
| §13 Round 6 → 7 | Dialog dismissal and genuine popup-focus identity retained; chain mechanism superseded | 2.3, 6.4(3–5), 6.6(4) |
| §13 Round 7 → 8 | Identity plus type only; real signal connection; honest discriminator coverage | 2.3, 3(4), 6.3–6.4, 6.6(2,4), 6.7 |

### 9.2 Exact test names and the mutants they must reject

The `_data` partner of each matrix is part of the same case. This is a required coverage mapping, not a report of executed mutation outcomes.

| Test name | Mutant(s) / required failing observation |
| --- | --- |
| `focusMoveRule` | Delete/invert any visible/pinned/armed guard; change destination or active-state logic; altered row result, including combinations |
| `explicitActivationRule` | Compare unmasked flags; Popup bit-containment misclassifies Tool; inverted/all-false policy; wrong literal expectation |
| `ownedPopupRule` | Null dereference; self accepted; only immediate parent accepted; popup-boundary ownership cut; sibling/unrelated owner accepted |
| `showArmingRule` | Always true/false; two nulls treated as focused; any focus accepted instead of own identity |
| `popupWindowTypeRule` | Dialog/ToolTip/Tool accepted as Popup or Popup rejected; helper result differs from the literal matrix |
| `hidesWhenFocusMovesToAnotherWindow` | Focus connection removed, own focus does not arm, Foreign ignored, or callback not invoked; final count is not one / first remains visible |
| `pinnedPopupDoesNotHide` | Pin guard omitted/inverted or bypassed by signal plumbing; count becomes nonzero / window hides |
| `ownedMenuKeepsPopupUntilFocusMovesAway` | OwnPopup branch removed, so the focused menu is Foreign and the zero-count assertion fails at once; the "clears arming" mutant is pinned by `ownPopupBranchPreservesArming` instead |
| `nestedOwnedMenuKeepsPopupUntilFocusMovesAway` | Immediate-owner/outer-menu-only mapping or removed OwnPopup branch; nested real-focus step hides early or later loss is ignored |
| `ownDialogHidesPopupAndRemainsVisible` | Any modal/dialog/transient-owned exemption, with a genuinely modal non-blocking dialog asserted as the active modal widget; owner remains visible / count stays zero. **Not** deletion of only the classifier type check |
| `focusReturningFromMenuKeepsPopup` | Return-to-own focus or menu close dismisses; count ceases to be zero. No timer kill claimed: the module has no asynchronous dismissal |
| `applicationInactiveSignalUsesProductionConnection` | State connection deleted/miswired, Inactive depends on null focus/current state, pin or unarmed negative bypassed; real signal assertions fail |
| `reshowWithoutFocusStaysUnarmed` | Arming retained across a direct `hide()` and an unfocused re-show, or the unarmed guard removed; the later foreign transition dismisses. Isolated `Hide`-reset and `Show`-assignment mutants belong to `closerShowAndHideMaintainArming` |
| `showWhileAlreadyFocusedArmsPopup` | Show arming removed/false; after asserted no-other-arming precondition, loss fails to invoke once |
| `dismissHandlerIsNotReentered` | `dismissing` check removed or flag cleared before callback; observed nested eligible decision raises count above one |
| `dismissHandlerMayDestroyCloser` | Crash-safety and no-extra-callback check; no mutation kill claimed (a freed-storage write can be silent without instrumentation). The missing self-guard is pinned by `dismissInvokesFreshHandlerBehindSelfGuard` |
| `destroyingArmedCloserDisconnectsSignals` | Overall destruction-safety check; no receiver-lifetime mutant claimed, because the explicit disconnects and `QObject` teardown both mask it — that mutant is pinned by `closerApplicationStateConnectionIsContextBound` |
| `scanPopupDeclaresCloserWithoutEventOverrides` | Missing/wrong include/member or forbidden event/changeEvent override; real declaration/definition check fails |
| `closerApplicationStateConnectionIsContextBound` | Missing/state connection without `this`, non-Inactive dismissal, wrong payload handling, or context-free focus connection; scoped source check fails |
| `closerShowAndHideMaintainArming` | Show helper assignment/real arguments deleted/replaced, Hide reset removed, wrong watched object, or consuming filter; exact branch assertion fails |
| `destinationClassifierUsesOwnedPopupIdentityAndType` | Delete only type call, add transient shortcut, weaken own-popup/handle identity, or change null/own precedence; classifier source check fails |
| `constructorRestoresPinBeforeChangingFlags` | Create/handler omitted or late; synchronisation before setChecked / after first flags; literal-false argument; one ordered expectation fails |
| `pinButtonSynchronizesBeforeFlags` | setPinned moved below complete checked chain even if still before Show; absent/wrong argument; first complete statement comparison fails |
| `engagePopupPreservesScopedActivationPaths` | Widened guard missing/weakened/moved, Show/request order changed, fourth activation, preserved arm/request removed, or xcb guarded giveFocus assignment removed |
| `unpinRequestsActivationAfterShow` | Missing/unconditional/pinning-only/pre-Show request or changed helper guard; guarded direct action/order assertion fails |
| `applicationProjectRegistersPopupDismiss` | Header/source omitted, wrong enumerated list, duplicate, wrong insertion or conditional-only registration; project-list assertion fails |
| `requiredSuiteInventoryIsExact` | Suite absent/extra/unregistered/duplicate, stale 20 count or either message, missing equality guard, or closure-only registration; list/count/discovery equality fails |
| `ownPopupBranchPreservesArming` | Any assignment to `armed`, dismissal or visibility write inside the `OwnPopup` arm |
| `dismissInvokesFreshHandlerBehindSelfGuard` | Handler invoked from the member after the self-guard is skipped, member writes before the guard test, or `window` dereferenced after the call |

## 10. Open questions
