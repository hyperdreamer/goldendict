# Wayland scan popup: close when it loses focus

Status: design revision 2, for review. Run `pm-run-20261116-123313-27afee37`,
topic `wayland-scan-popup-focus-close`.
Review history: R1 → 14 findings (0 blockers, 8 major, 6 minor). This revision answers
all fourteen; §13 is the finding-to-change ledger.

**Scope limit of this document.** It specifies a design, not a test implementation.
Most of the behaviour lives in `ScanPopup`, a translation unit no suite links (it needs
`ArticleView`, a `QWebEngineProfile`, the dictionary registry and the whole
application). Where a claim cannot be observed by a test, it is pinned by a *source
assertion* and stated as such. The decision logic and the event plumbing are moved into
a new, directly linkable module precisely so that the important half is proven by
running it, not by reading it.

## 1. Decision record

| decision | choice | reason |
| --- | --- | --- |
| scope (D1) | **full X11 parity**: an unpinned popup asks for keyboard focus when it opens **and** hides as soon as it loses it | A window that never received focus cannot lose it. Closing on deactivation alone would not change anything for the clipboard-triggered popup the reporter was looking at. Recorded operator reply: `2`. |
| new preference (D2) | **none** | X11's `Qt::Popup` behaviour is unconditional; a toggle would create a third mode nobody asked for and a preference row to mock up. |
| detection (D3) | **window deactivation events**, not pointer polling | The existing proximity polling cannot work on Wayland: `QCursor::pos()` is not the global pointer there, and a client has no pointer grab (`scanpopup.cc:299, 1205`). Deactivation is the event the compositor actually delivers. |
| placement (D4) | new module `popupdismiss.{hh,cc}` | `ScanPopup` is unlinkable in a test; a pure rule plus a small watchable `QObject` keeps the decision and the event plumbing linkable, in the same style as `platformwindow.{hh,cc}`. |
| re-focus on later words (D5) | **the new policy asks nothing on a later word**; only a transition from hidden to visible asks | A clipboard change arrives while the user may be typing elsewhere; re-requesting focus on every later word would fight the user. Pre-existing requests that already happen (the `xcb` `giveFocus` block, the `xcb` `Tool` re-raise, the hotkey token) are preserved untouched and listed in §6.4. |
| suppressed deactivation (D6) | a suppression **arms a deferred re-decision** that runs once the event loop settles, and is also re-armed by the suppressing window losing focus or going away | Otherwise a popup whose owned dialog took focus can never be dismissed again: it is already inactive, so it receives no second deactivation. (R1-M1) |
| application integration (D7) | `popupdismiss.{hh,cc}` are added to `goldendict.pro`'s enumerated `HEADERS`/`SOURCES`, and the acceptance run includes an application build | `goldendict.pro` enumerates its sources (`goldendict.pro:426, 573`); a new module is not discovered automatically, so a green helper suite could hide an unlinkable application. (R1-M4) |

## 2. Background

On X11 the unpinned popup is created with `Qt::Popup` (`platformwindow.cc:38`,
`defaultX11Flags` — the preserved default of the code this replaced). A `Qt::Popup`
window is given the keyboard by Qt and is closed by Qt itself as soon as it loses the
popup grab: click any other window and the popup is gone.

On Wayland the same call site returns something else on purpose:
`PlatformWindow::popupFlags()` returns `Qt::Tool | Qt::FramelessWindowHint |
Qt::WindowStaysOnTopHint` for every Wayland platform, unconditionally
(`platformwindow.cc:32-33, 118-119`). That is an ordinary window: clicking elsewhere
moves the keyboard focus and the popup stays where it is.

The dismissal paths that exist today:

* **Qt's own popup close**, on `xcb` only, because only there the window is a
  `Qt::Popup`;
* **Esc**, via `escapePressed()` (`scanpopup.cc:1170-1174`) — which first asks the
  article view to close an open search panel and only then hides the window, and which
  requires the popup to have keyboard focus to be delivered at all;
* **pointer proximity**: `interceptMouse()` starts `mouseGrabPollTimer`, which polls
  `QCursor::pos()` every 10 ms (`scanpopup.cc:299, 1188-1216`) and hides the popup once
  the pointer is far away (`scanpopup.cc:910-948`); `leaveEvent()` additionally starts
  the 400 ms `hideTimer` for an unpinned window (`scanpopup.cc:995-1010, 1124-1129`);
* **an outside-mouse press**, which relies on a pointer grab (`scanpopup.cc:950-970`)
  and therefore only works where such a grab exists.

On Wayland none of the pointer-based paths can see the global pointer, so the popup
stays.

Opening mechanisms that are sometimes confused with dismissal are different things:
hover translation is a platform-selected `MouseOver` implementation whose
`ScanPopup::mouseHovered()` handler returns early on Wayland (`scanpopup.cc:541-545`),
and the scan flag is an `xcb`-only trigger (`scanpopup.cc:572-579`). Neither dismisses
anything.

Focus today:

* `engagePopup( bool forcePopup, bool giveFocus = false )` (`scanpopup.hh:169`), so on
  Linux the popup is shown **without** asking for activation
  (`scanpopup.cc:496-504`: `engagePopup( false, false )` on Linux, `true` only under
  `Q_OS_WIN`).
* In an X11 build the pre-existing `#ifdef ENABLE_SPWF_CUSTOMIZATION` block forces
  `giveFocus = true` on `xcb` for every unpinned form (`scanpopup.cc:665-671`), and
  `ENABLE_SPWF_CUSTOMIZATION` is defined whenever `HAVE_X11` is (`config.hh:242-245`).
  A second pre-existing block re-activates an already-visible unpinned `Qt::Tool`
  popup on every later word (`scanpopup.cc:697-705`).
* A popup opened by a **global hotkey** already asks for activation using the portal's
  activation token: `mainwindow.cc:3325-3336` calls
  `PlatformWindow::applyActivation( scanPopup.get(), token )` before delivering the
  word, including when the popup is already visible.

So on Wayland the clipboard-triggered popup never has focus, which is why a
deactivation-based close would be dead code for exactly the case the reporter hit.

`docs/wayland-limitations.md` §3 states the platform truth this design must accept:
"Activation is best-effort: an activation request is a hint that a compositor may honour
or decline … nothing in this migration promises that a window becomes focused".

## 3. Goals

1. An unpinned scan popup on Wayland behaves like the X11 one: it takes the keyboard
   when it opens, and the moment that keyboard focus goes anywhere else it disappears
   immediately, using the same hide path as Esc.
2. A pinned popup never auto-hides, on any platform.
3. X11 does not regress: the default stored window form is still a `Qt::Popup` and Qt
   still owns its dismissal.
4. No popup can be left visible, unfocused and undismissable because it suppressed its
   own deactivation. (R1-M1)
5. The parts that can be proven on this host are proven by execution; the part that
   needs a live compositor is documented as a limit, not claimed as a result.

## 4. Non-goals

* No new preference, no new control, no changed default. (D2)
* No emulation of an X11 pointer/keyboard grab.
* No change to hover translation, the scan flag, the clipboard-watching rules, the
  alt-mode chord, or the tray behaviour.
* No change to the pinned popup's window flags or its stay-on-top option.
* No change to the main window or to the article view.
* No change to the pre-existing activation requests listed in §6.4; they are preserved
  exactly as they are, including the `xcb` `Tool` re-raise on later words.

## 5. Behaviour

### 5.1 Opening

When an **unpinned** popup transitions from hidden to visible and its window flags
describe a window that is **not** a platform popup (`Qt::Popup`), the popup requests
activation once: `activateWindow()` followed by `raise()`, the same pair the
`giveFocus` path uses. A `Qt::Popup` window is skipped because Qt already gives it the
keyboard.

When the popup is already visible, a new word updates the content and the new policy
requests nothing. (D5)

A **pinned, already visible** popup keeps its current re-raise
(`scanpopup.cc:689-695`). A **pinned, hidden** popup is shown without a new focus
request, exactly as today: the new rule excludes pinned windows, and the `giveFocus`
argument is false on Linux. The design does not add activation to that path.

### 5.2 Closing

The popup hides when all of these hold:

1. it is visible;
2. it is not pinned;
3. it received activation since it was last shown (`focusedSinceShown`);
4. no popup widget of its own is open (a `QMenu`, a combo dropdown, a context menu);
5. no modal dialog of its own is up.

The dismiss action is the existing `hideWindow()` (`scanpopup.cc:1176-1184`), so the
mouse interception stops, `closeMenu()` is emitted, the word field is deselected and the
popup hides. Nothing else is added to that path.

### 5.3 A suppressed deactivation is re-decided later

Conditions 4 and 5 suppress a deactivation that a menu or dialog caused. Suppression is
never final: the suppression arms a **deferred re-decision** that runs on the next
event-loop turn, and the suppressing window is watched as well, so its own deactivation,
closing or destruction re-arms it. The re-decision hides the popup only when, by then,
the popup is visible, unpinned, still armed, not the active window, and no popup or
modal of its own is active. This closes the trap where the popup is left visible and
unfocused with nothing left to observe. (D6, R1-M1)

The deferral exists for one reason: when a modal dialog closes, focus normally returns to
the popup, and hiding on the dialog's own `Hide` event would kill the popup the user is
returning to. The re-decision runs after that return has been delivered, so a returning
focus keeps the popup alive.

### 5.4 Unpinning is a new opening

`pinButtonClicked( false )` replaces the window flags and shows the window again
(`scanpopup.cc:1092-1102`). The flag change hides the widget, so the previous
`focusedSinceShown` is cleared and the replacement window may be shown inactive. An
unpin therefore follows §5.1's rule: when the new flags are not a platform popup, the
popup asks for activation after `show()`, and can then lose focus and hide. On `xcb`
with the default form the replacement is a `Qt::Popup` and nothing is asked — the
platform takes over, as it does today. (R1-M3)

### 5.5 When nothing happens

* A deactivation for a popup that was never activated is ignored. This is what makes the
  compositor-refusal case honest: no focus was granted, so there is nothing to lose.
* A deactivation while already hidden or pinned is ignored.
* A deactivation caused by a menu, a combo dropdown or a modal dialog that belongs to the
  popup is suppressed, and then re-decided per §5.3.

## 6. Architecture

### 6.1 New module: `popupdismiss.hh` / `popupdismiss.cc`

```cpp
namespace PopupDismiss {

/// What one deactivation observation means for the popup.
enum class Action { Ignore, Hide };

/// Everything the immediate rule may read. No widget, no application object, no globals.
struct Context {
  bool visible = false;
  bool pinned = false;
  bool focusedSinceShown = false;
  bool ownPopupOpen = false;   ///< one of the popup's own popup widgets is active
  bool ownModalOpen = false;   ///< one of the popup's own modal dialogs is active
};

/// Immediate rule: Hide iff
/// visible && !pinned && focusedSinceShown && !ownPopupOpen && !ownModalOpen.
Action onWindowDeactivate( Context const & context );

/// Deferred rule, evaluated once the event loop settled after a suppression.
/// Hide iff visible && !pinned && focusedSinceShown
///          && !ownWindowActive && !ownPopupOpen && !ownModalOpen.
Action onDeferredRecheck( bool visible, bool pinned, bool focusedSinceShown,
                          bool ownWindowActive, bool ownPopupOpen, bool ownModalOpen );

/// True when the window does not already receive the keyboard from the platform:
/// every window type except Qt::Popup. Window hints never change the answer.
bool needsExplicitActivation( Qt::WindowFlags flags );

/// True when `candidate` is `owner` or a descendant of it. Null-safe; decides
/// whether an active popup widget or modal dialog belongs to this popup.
bool isOwnedBy( QWidget const * owner, QWidget const * candidate );

/// Watches one top-level window and hides it when the rules say so.
///
/// Owns: pinned, focusedSinceShown, the suppression state and the deferred timer.
/// It never decides anything outside the two rules above and never touches
/// ScanPopup: the caller supplies the dismiss action.
class FocusLossCloser : public QObject {
  Q_OBJECT
public:
  explicit FocusLossCloser( QWidget * window, QObject * parent = nullptr );

  void setPinned( bool pinned );
  bool isPinned() const;
  /// The action to run when a rule returns Hide. Empty means "observe only".
  void setDismissHandler( std::function< void() > handler );

protected:
  bool eventFilter( QObject * watched, QEvent * event ) override;

private:
  void suppress( QWidget * culprit );   ///< remember and watch the owned transient
  void scheduleRecheck();               ///< one deferred re-decision per turn
  void recheck();

  QWidget * const window;               ///< observed top-level window, never null
  bool pinned = false;
  bool focusedSinceShown = false;
  bool recheckScheduled = false;
  QPointer< QWidget > suppressor;       ///< owned popup/modal that caused the suppression
  std::function< void() > dismissHandler;
};

} // namespace PopupDismiss
```

Decisions that keep the module honest:

* **No platform gate.** The flags change at runtime (pin toggle), so a construction-time
  platform test would be wrong. On a `Qt::Popup` window the closer is inert in practice —
  Qt closes the popup first — and both paths end in the same idempotent `hideWindow()`.
* **Nothing but the two rules decides.** `Context` construction is the only place that
  reads `QApplication::activePopupWidget()` / `activeModalWidget()`, and the runtime
  fixtures below observe that construction through its effect.
* **Logging** is one `qCDebug` line per decision on the category
  `goldendict.popup.dismiss`, naming the rule and the field that decided it. It logs
  *requests and decisions*, never window titles or looked-up words, and it cannot
  explain a compositor refusal because a refusal produces no event.

### 6.2 `FocusLossCloser` state machine

Installed as an event filter on `window`; it never consumes an event that belongs to the
window (it returns `false` for it), so the popup's own handling is unchanged. It also
observes the suppressor it installed itself on, and there it may consume nothing either.

| Watched object | Event | Condition | Effect |
| --- | --- | --- | --- |
| window | `Show` | — | `focusedSinceShown = window->isActiveWindow()`; clear suppressor |
| window | `Hide` | — | `focusedSinceShown = false`; clear suppressor |
| window | `WindowActivate` | — | `focusedSinceShown = true`; clear suppressor |
| window | `WindowDeactivate` | rule → `Ignore` because own popup/modal is active | `suppress( culprit )`, nothing else |
| window | `WindowDeactivate` | rule → `Ignore` otherwise | nothing |
| window | `WindowDeactivate` | rule → `Hide` | `focusedSinceShown = false`; clear suppressor; call `dismissHandler` |
| suppressor | `WindowDeactivate`, `Hide`, `Close` | — | `scheduleRecheck()` |
| suppressor | destroyed | — | `scheduleRecheck()` via the `QPointer` going null in the deferred pass |
| any | other | — | nothing |

`focusedSinceShown` is set from `isActiveWindow()` on `Show` because a re-shown window
can already be the active one when no activation event follows. Resetting it on `Hide`
is what stops a later showing's deactivation from closing a popup that has been hidden
and shown again without focus.

`scheduleRecheck()` sets a flag and posts a single `QTimer::singleShot( 0, … )`; a second
call in the same turn is a no-op, so a burst of suppressor events produces one decision.
`recheck()` evaluates `onDeferredRecheck( window->isVisible(), pinned, focusedSinceShown,
window->isActiveWindow(), isOwnedBy( window, QApplication::activePopupWidget() ),
isOwnedBy( window, QApplication::activeModalWidget() ) )`, hides on `Hide`, and otherwise
keeps the suppressor watched only while an owned popup or modal is still active.

Guards are computed at decision time, from live application state:

```cpp
QWidget * const popup = QApplication::activePopupWidget();
QWidget * const modal = QApplication::activeModalWidget();
Context const context{ window->isVisible(), pinned, focusedSinceShown,
                       isOwnedBy( window, popup ), isOwnedBy( window, modal ) };
```

A modal dialog or menu opened by the *main* window is not owned by the popup, so it does
not suppress the hide — which matches X11, where a `Qt::Popup` closes when any other
window takes the keyboard.

### 6.3 Wiring in `ScanPopup`

Ordering matters twice here and both are pinned by assertions (§8.4). `setWindowFlags()`
on a visible widget hides it, and that hide can deliver events synchronously, so no flag
change may happen before the closer knows the new pin state.

1. `scanpopup.hh`: `#include "popupdismiss.hh"`, member
   `std::unique_ptr< PopupDismiss::FocusLossCloser > focusLossCloser;`. No `event()` or
   `changeEvent()` override is added.
2. Constructor, in this order:
   a. create the closer on `this` immediately after `ui.setupUi( this )`, and install the
      dismiss handler `[ this ] { hideWindow(); }`;
   b. immediately after `ui.pinButton->setChecked( cfg.pinPopupWindow )`
      (`scanpopup.cc:184`) and **before** the `if ( cfg.pinPopupWindow )` block that
      first calls `setWindowFlags()` (`scanpopup.cc:186-200`), call
      `focusLossCloser->setPinned( ui.pinButton->isChecked() )`.
3. `pinButtonClicked( bool checked )` (`scanpopup.cc:1077-1105`): its **first statement**
   is `focusLossCloser->setPinned( checked );`, before either branch's
   `setWindowFlags()`. (R1-M2)
4. Unpin branch: after the common `show()` at the end of the function
   (`scanpopup.cc:1100`), when `!checked && PopupDismiss::needsExplicitActivation(
   windowFlags() )`, call `activateWindow(); raise();`. (§5.4)
5. `engagePopup()` (`scanpopup.cc:605-717`): in the `if ( !isVisible() )` branch, after
   `show();`, replace `if ( giveFocus )` with

   ```cpp
   if ( giveFocus || ( !ui.pinButton->isChecked() &&
                       PopupDismiss::needsExplicitActivation( windowFlags() ) ) )
   {
     activateWindow();
     raise();
   }
   ```

   Nothing else in that function changes: the `#ifdef ENABLE_SPWF_CUSTOMIZATION` block
   and both `else if` branches keep their current conditions and actions.

6. `goldendict.pro`: add `popupdismiss.hh` to the enumerated `HEADERS` list and
   `popupdismiss.cc` to `SOURCES`. (D7)

### 6.4 Behaviour by platform and stored window form

Facts behind the table: `ENABLE_SPWF_CUSTOMIZATION` is defined whenever `HAVE_X11` is
(`config.hh:242-245`); with it, `unpinnedWindowFlags()` passes the stored form
(`scanpopup.cc:433-440`), without it the stored form is always `SPWF_default`; and
`popupFlags()` maps `SPWF_default` to `Qt::Popup` on every non-Wayland platform while
Wayland always gets the managed tool window.

| platform | stored form | resulting flags | focus request **on open** | dismissal |
| --- | --- | --- | --- | --- |
| Wayland | any | `Tool \| Frameless \| StaysOnTop` | **new rule** (unpinned); hotkey paths also apply their token (pre-existing) | **new rule** + §5.3 re-decision |
| xcb (X11 build) | `default` or `Popup` | `Qt::Popup` | pre-existing `giveFocus` block, unpinned only; the new rule answers false | Qt's own popup close |
| xcb (X11 build) | `Tool` | `Tool \| Frameless \| StaysOnTop` | pre-existing `giveFocus` block **and** the new rule (both unpinned) | **new rule** + proximity/leave timer (pre-existing) |
| Windows / macOS | n/a (`SPWF_default` when the X11-only customization is absent) | `Qt::Popup` | Windows: `giveFocus = true` for the two clipboard/hotkey word paths; the new rule answers false | Qt's own popup close |

The `xcb` `Tool` row is an intentional **addition** beyond the Wayland goal: those
sessions keep their existing proximity and leave-timer dismissal
(`scanpopup.cc:910-948, 995-1010`) and gain focus-loss dismissal. It is recorded here so
that the X11 behaviour change is a stated decision rather than an accident. (R1-m4)

## 7. Error handling and edge cases

| situation | behaviour |
| --- | --- |
| `activePopupWidget()` / `activeModalWidget()` returns null | guard false; the rule decides on the remaining fields |
| closer constructed before the window has a native handle | no handle is needed: only widget events and `isActiveWindow()` are read |
| `Show` while already active | `focusedSinceShown = true`; a later deactivation hides |
| `Show` while not active (compositor declined, or shown behind) | `focusedSinceShown = false`; a deactivation is ignored until an activation arrives |
| pin toggled while visible | `setPinned` is called before the flag change; a pinned popup ignores deactivation from then on |
| unpin while visible | §5.4: the replacement window is treated as a new opening and follows §5.1's rule |
| the popup deactivates because its own dialog opened | suppressed, suppressor watched, re-decided per §5.3 |
| the suppressor loses focus to another application, closes, or is destroyed | deferred re-decision: hide when nothing of the popup's is active any more |
| focus returns to the popup when its dialog closes | the activation is delivered before the deferred pass, which then keeps the popup |
| deactivation during `hideWindow()` | `isVisible()` is already false or the resulting `Hide` resets the flag; the handler is idempotent |
| window destroyed while the closer lives | the closer is parented to the window; the suppressor is a `QPointer` |
| pointer-poll dismissal racing the deactivation | both call `hideWindow()`; it is idempotent |
| a popup opened while another GoldenDict window holds focus | the request is still made; the compositor decides |
| activation refused by the compositor | no activation, so no decision: the popup stays. Clicking it gives it focus, after which a click elsewhere hides it, and Esc reaches it (closing an open search panel first, as `escapePressed()` already does) |

## 8. Test strategy

New suite `tests/tst_popup_dismiss/`, the 21st required suite, linking the production
`popupdismiss.cc` — never a copy of it.

### 8.1 Rule matrices (data-driven)

`onWindowDeactivate` over all five fields: `Hide` for the one satisfying row, and one row
per single `Ignore` cause — hidden, pinned, never focused, own popup active, own modal
active — plus two combination rows and one row per *omitted* guard. The matrix
distinguishes an omitted guard, an inverted predicate and a swapped logical operator; it
does **not** claim that reordering the `&&` operands is observable, because it is not.

`onDeferredRecheck` over its six fields: `Hide` for the satisfying row; `Ignore` when the
popup is active again, pinned, unarmed, hidden, or when an owned popup or modal is still
active.

`needsExplicitActivation`: `Qt::Popup` → false; `Qt::Popup` with `FramelessWindowHint`
or `StaysOnTop` hints → still false; `Qt::Tool`,
`Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint`, `Qt::Window`,
`Qt::Dialog`, `Qt::Tool | X11BypassWindowManagerHint` → true.

`isOwnedBy`: null/null, self/self, a child popup widget, a grandchild, a sibling, an
unrelated top-level, and a dialog parented to a different window.

### 8.2 Runtime behaviour of the production `FocusLossCloser`

On the `xcb` + `xvfb` acceptance display, with real top-level widgets; the gate command
is stated in §8.5. Every fixture drives the production closer; none of them re-implements
its logic.

1. **hides on a real deactivation** — closer on `first` with a counting handler that
   hides it; `first` shown and activated (`QTest::qWaitForWindowActive`), then `second`
   shown and activated; `QTRY_COMPARE` the handler ran once and `first` is hidden.
2. **pinned does not hide** — the same drive with `setPinned( true )`; the handler never
   runs and the window stays visible.
3. **an owned modal suppresses, and its own deactivation re-decides** — a
   `Qt::WindowModal` `QDialog` parented to `first` is shown; await the transition with
   `QTest::qWaitForWindowActive( &dialog )` and assert `!first->isActiveWindow()`; then
   activate the sibling `second` (allowed: window-modality blocks only the dialog's
   parent chain, and this fixture asserts the activation rather than assuming it) and
   `QTRY_COMPARE` the handler ran once. This is also the fixture that proves the
   production `Context` construction really reports the owned modal: were the guard
   hard-wired to false, the popup would hide at the *dialog* transition and the
   "did not hide yet" assertion would fail.
4. **an owned popup suppresses, and its close re-decides** — a `QWidget` with
   `setWindowFlags( Qt::Popup )` parented to `first` is shown; assert
   `QApplication::activePopupWidget()` is that widget and `first` is not active; activate
   `second` and assert the handler did **not** run while the owned popup is open; close
   the owned popup with focus already on `second`, then `QTRY_COMPARE` the handler ran
   once. If the production context forced `ownPopupOpen` false, the middle assertion
   fails.
5. **a deactivation before any activation is ignored** — `first` shown while `second` is
   active; `QVERIFY( !first->isActiveWindow() )` establishes the state, then a synthetic
   `QEvent::WindowDeactivate` is delivered to `first`; no hide.
6. **re-show resets the state** — after a hide, `first` is shown again while `second`
   remains active (asserted inactive first, as in 5); a synthetic deactivation is
   ignored.
7. **unrelated popups are not owned** — `isOwnedBy` is exhaustive in 8.1; the runtime
   layer does not claim to prove it, because a second, unrelated `Qt::Popup` grab cannot
   be driven deterministically under a bare `Xvfb`.

Synthetic deliveries use `QCoreApplication::sendEvent`, so the same production filter
receives them; their fixtures are named `…Synthetic…` so the difference stays visible.

The runtime fixtures require a display and fail with a reason when there is none, like
`tst_platformwindow`. They are `xcb`-only: on another QPA (`offscreen`) they `QSKIP` with
that reason, and §8.5 states that a skipped fixture does not satisfy acceptance.

### 8.3 Source assertions (wiring the suite cannot link)

`ScanPopup` cannot be linked into a suite, so its wiring is pinned by reading the sources
with the branch-scoped style of `tst_hotkeys` (`selectionBlock`, `selectionBranch`,
`selectionDirectActions`, `selectionOrdered`). The suite carries its own copies of those
small helpers. Assertions, each one red when its mechanism is removed:

1. `scanpopup.hh` includes `popupdismiss.hh`, declares the
   `PopupDismiss::FocusLossCloser` member, and adds **no** `event(` / `changeEvent(`
   override.
2. Constructor: `PopupDismiss::FocusLossCloser` is constructed on `this`,
   `setDismissHandler` names `hideWindow`, and `setPinned` appears **before** the first
   `setWindowFlags` in the constructor block (a statement-order assertion, not a
   whole-function `contains`).
3. `pinButtonClicked`: `setPinned( checked )` is the **first statement** of the function
   body (`selectionDirectActions` of the body starts with it), hence before either
   branch's `setWindowFlags`.
4. `engagePopup`: inside the `if ( !isVisible() )` branch only,
   - the full condition text `if ( giveFocus || ( !ui.pinButton->isChecked() &&
     PopupDismiss::needsExplicitActivation( windowFlags() ) ) )` is present;
   - its consequent contains `activateWindow();` then `raise();` in that order;
   - `activateWindow()` occurs exactly twice in the function: once in that consequent and
     once in the pre-existing `#ifdef ENABLE_SPWF_CUSTOMIZATION` `xcb` `Tool` branch.
     A third, unconditional call — the "activate everything" mutant — fails this count.
5. Unpin branch: after the shared `show()`, the activation request is guarded by
   `!checked && PopupDismiss::needsExplicitActivation( windowFlags() )`.
6. `goldendict.pro`: `popupdismiss.hh` is in the `HEADERS` assignment and
   `popupdismiss.cc` in `SOURCES`.
7. `tests/tests.pro`: the required-suite list, the discovered `tests/tst_*/tst_*.pro`
   basenames and the `!count(...)` literal agree, and include `tst_popup_dismiss`
   (§8.5).

### 8.4 What is not proven here

The compositor's decision. No fixture can force KWin to grant or refuse activation, and
none is written that pretends to. `docs/wayland-limitations.md` records the limit
instead.

### 8.5 Acceptance commands

`tests/tests.pro` does not choose a QPA platform: its inventory rules are what it
enforces, and its documented `xcb` line is a comment. The acceptance run therefore states
the platform explicitly, and a skipped runtime fixture does not satisfy it:

```bash
qmake6 goldendict.pro -o Makefile && make -j"$(nproc)"          # application links (D7)
qmake6 tests/tests.pro -o tests/Makefile
make -C tests -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check
```

The application build is part of acceptance because `goldendict.pro` enumerates its
sources: a new module that is not listed compiles nowhere and the suite alone would stay
green. (R1-M4)

## 9. Documentation changes

`docs/wayland-limitations.md`:

* §2's limit list gains the popup rule: the scan popup asks for activation when it opens
  and closes when it loses focus; a refused activation leaves it open, and giving it
  focus (a click) restores both Esc and click-away dismissal.
* §3 keeps its "activation is best-effort" paragraph and adds that a refused request
  means the popup is dismissed manually, which is the documented consequence rather than
  a regression.

No README change: its Wayland summary does not claim the popup dismisses itself.

## 10. Risks

| risk | mitigation |
| --- | --- |
| a clipboard-triggered popup takes the keyboard from what the user is typing in | accepted by the operator as the price of X11 parity (D1); it is what `Qt::Popup` does on xcb today |
| a compositor refuses focus, so nothing changes for the user | documented (§9); clicking the popup makes it dismissable, and the log records the request, not a refusal |
| hiding while a menu or dialog is open | suppressed, then re-decided per §5.3, tested at rule and runtime level |
| the popup is stranded by its own suppression | the deferred re-decision (D6) with a fixture that does not re-activate the popup artificially |
| a deactivation delivered during the open transition closes the popup immediately | the rule requires `focusedSinceShown`, which only an activation sets |
| the closer swallowing an event | it returns `false` for every event of the window |
| a regression on X11 | the default form is still `Qt::Popup` (`tst_platformwindow` pins the flags); the only X11 behaviour change is the stated `Tool`-form addition in §6.4 |
| a new module that the application never links | `goldendict.pro` entries plus the application build in the acceptance command (D7) |

## 11. Out of scope

Hover translation, the scan flag, the clipboard-watch rules, alt-mode, the tray, the
pinned popup's flags and stay-on-top option, main-window focus restoration, and any
attempt to emulate an X11 grab on Wayland.

## 12. Implementation-order note

Not a plan, but the sequence the later plan must respect: the module and its rules first
(provable alone), then the closer, then the `ScanPopup` wiring, then the project-file and
inventory integration, then the documentation. The application build is the last thing
that can go red, so it is where a missing registration surfaces.

## 13. Review ledger (R1 → this revision)

| finding | severity | change |
| --- | --- | --- |
| suppressed deactivation strands the popup | major | §5.3, §6.2 `onDeferredRecheck` + suppressor watch, §8.2(3,4) |
| `setPinned` after `setWindowFlags` | major | §6.3(3) first statement; §8.3(3) order assertion |
| unpin while visible cannot hide | major | §5.4 + §6.3(4); §8.3(5) |
| `goldendict.pro` registration missing | major | D7, §6.3(6), §8.3(6), §8.5 application build |
| modal fixture cannot activate a sibling | major | §8.2(3) window-modality, awaited transition, asserted activation |
| source assertions too weak | major | §8.3(4) full condition, branch scope, exact `activateWindow()` count |
| activation policy contradicts preserved `xcb` branches | major | §2 facts, §6.4 table, D5 and §4 wording |
| no runtime proof of `ownPopupOpen` | major | §8.2(4) owned `Qt::Popup` fixture with a close-driven re-decision |
| pinned-open and Windows/macOS platform descriptions | minor | §5.1, §6.4 |
| `tests.pro` does not select `xcb` | minor | §8.5 explicit acceptance commands |
| declined-activation fallback overstated | minor | §7 last row, §5.5, §9 |
| `xcb` `Tool` rationale wrong | minor | §6.4 closing paragraph, §10 |
| hover/scan-flag called dismissal paths | minor | §2 rewritten |
| "reordered conjunction" claim | minor | §8.1 replaced with the defects the matrix can distinguish |
