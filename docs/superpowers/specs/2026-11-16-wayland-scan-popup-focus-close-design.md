# Wayland scan popup: close when it loses focus

Status: design revision 4, for review. Run `pm-run-20261116-123313-27afee37`,
topic `wayland-scan-popup-focus-close`.
Review history: R1 → 14 findings (0 blockers, 8 major, 6 minor); R2 → 7 (0 blockers,
6 major, 1 minor); R3 → 13 (0 blockers, 8 major, 5 minor). This revision answers all of
R3, and it removes two whole problem classes by narrowing the guard to what parity
actually needs (§1 D8). §13 is the finding-to-change ledger for all three rounds.

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
| detection (D3) | **window activation events**, not pointer polling | The existing proximity polling cannot work on Wayland: `QCursor::pos()` is not the global pointer there, and a client has no pointer grab (`scanpopup.cc:299, 1205`). Deactivation is the event the compositor actually delivers. |
| placement (D4) | new module `popupdismiss.{hh,cc}` | `ScanPopup` is unlinkable in a test; two pure rules plus a small watchable `QObject` keep the decision and the event plumbing linkable, in the same style as `platformwindow.{hh,cc}`. |
| re-focus on later words (D5) | **the new policy asks nothing on a later word**; only a transition from hidden to visible asks | A clipboard change arrives while the user may be typing elsewhere; re-requesting focus on every later word would fight the user. Pre-existing requests that already happen (the `xcb` `giveFocus` block, the `xcb` `Tool` re-raise, the hotkey token) are preserved untouched and listed in §6.4. |
| modal dialogs are **not** a suppression cause (D8) | opening the popup's own dialog hides the popup, exactly as X11's `Qt::Popup` does | X11 parity is the operator's criterion, and `Qt::Popup` closes when any other window takes the keyboard — including a dialog the popup itself opened. Suppressing for modals created two rounds of problems (`activeModalWidget()` stays registered while merely unfocused, and an owned dialog chain is not observable), so the guard is limited to the one case parity genuinely cannot tolerate: the popup's own **menus and dropdowns**, which are the same window's UI, not another window's. (R2-M1, R3-M1) |
| suppressed menu deactivation (D6) | a suppression **starts a bounded settlement**: the dismissal decision is taken at a fixed deadline, cancelled by an activation of the popup | A zero-delay callback cannot win a race against a returning activation, and Wayland offers no "focus has settled" signal. A fixed, documented deadline is testable at both sides, and an activation later than it finds the popup already hidden — the same outcome X11 produces. (R1-M1, R2-M1, R2-M2, R3-M2) |
| application integration (D7) | `popupdismiss.{hh,cc}` are added to `goldendict.pro`'s enumerated `HEADERS`/`SOURCES`, and the acceptance run includes an application build | `goldendict.pro` enumerates its sources (`goldendict.pro:426, 573`); a new module is not discovered automatically, so a green helper suite could hide an unlinkable application. (R1-M4) |

## 2. Background

On X11 the unpinned popup is created with `Qt::Popup` (`platformwindow.cc:38`,
`defaultX11Flags` — the preserved default of the code this replaced). A `Qt::Popup`
window is given the keyboard by Qt and is closed by Qt itself as soon as it loses the
popup grab: click any other window — or open a dialog from it — and the popup is gone.
The same `Qt::Popup` form is what a build without the X11-only customization uses on
every platform (`scanpopup.cc:433-440`, `platformwindow.cc:118-119`), so the Windows and
macOS defaults have that dismissal too.

On Wayland the same call site returns something else on purpose:
`PlatformWindow::popupFlags()` returns `Qt::Tool | Qt::FramelessWindowHint |
Qt::WindowStaysOnTopHint` for every Wayland platform, unconditionally
(`platformwindow.cc:32-33, 118-119`). That is an ordinary window: clicking elsewhere
moves the keyboard focus and the popup stays where it is.

The dismissal paths that exist today:

* **Qt's own popup close**, wherever the window is a `Qt::Popup` — always on `xcb`, and
  on a build without the X11-only customization;
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
  word. For the `scan-popup` binding the call site first materialises a native handle
  (`scanPopup->winId()`, `mainwindow.cc:3335`), so a token can be installed; the
  `clipboard-lookup` binding calls the same helper without materialising one, and
  `applyActivation()` then reports `NoHandle` (`platformwindow.cc:160-172`) and installs
  no token. Both bindings' behaviour is preserved as it is.

So on Wayland the clipboard-triggered popup never has focus, which is why a
deactivation-based close would be dead code for exactly the case the reporter hit.

`docs/wayland-limitations.md` §3 states the platform truth this design must accept:
"Activation is best-effort: an activation request is a hint that a compositor may honour
or decline … nothing in this migration promises that a window becomes focused".

## 3. Goals

1. An unpinned scan popup on Wayland behaves like the X11 one: it takes the keyboard
   when it opens, and the moment that keyboard focus goes anywhere else it disappears,
   using the same hide path as Esc.
2. A pinned popup never auto-hides, on any platform.
3. X11 does not regress: the default stored window form is still a `Qt::Popup` and Qt
   still owns its dismissal.
4. No popup can be left visible, unfocused and undismissable — not while one of its own
   menus is open, and not after that menu closes. (R1-M1, R2-M1, R3-M1)
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
(`scanpopup.cc:689-696`). A **pinned, hidden** popup is shown without a new focus
request, exactly as today: the new rule excludes pinned windows, and the `giveFocus`
argument is false on Linux. The design does not add activation to that path.

### 5.2 Closing: the immediate rule

The popup hides, immediately and synchronously, when all of these hold:

1. it is visible;
2. it is not pinned;
3. it received activation since it was last shown (`focusedSinceShown`);
4. **no popup widget of its own is open** — a `QMenu`, a combo dropdown, a context menu.

There is deliberately **no modal-dialog exception** (D8): a dialog belongs to another
window, and X11's `Qt::Popup` closes when one takes the keyboard. This rule is therefore
the parity rule, not a new policy.

The dismiss action is the existing `hideWindow()` (`scanpopup.cc:1176-1186`), so the
mouse interception stops, `closeMenu()` is emitted, the word field is deselected and the
popup hides (`hide()` at `scanpopup.cc:1185`). Nothing else is added to that path.

### 5.3 A suppressed menu deactivation settles instead of sticking

Condition 4 is the only suppression: while one of the popup's own menus or dropdowns is
open, the popup is still on screen and still in use, and the owner's deactivation is not
the user leaving. That suppression starts a **focus settlement**:

1. **Armed.** Suppression arms settlement and records the menu widget (`suppressor`),
   which is watched from then on.
2. **Cancelled by an activation of the popup.** Any activation of the popup — or of
   another popup widget it owns, which continues the same menu interaction — cancels the
   pending settlement and keeps the popup.
3. **Decided at the deadline.** Settlement ends at `PopupDismiss::settleGraceMs` (100 ms)
   after the suppressor's own `WindowDeactivate`, `Hide`, `Close` or destruction,
   whichever was last. The decision is: hide when the popup is visible, unpinned, still
   armed, not itself active, and no menu of its own is active.

**The deadline is a decision point, not a wait for focus** (D6, R3-M2). The platform
gives no "focus has settled" event, so the alternative would be an unbounded wait. An
activation that arrives after the deadline finds the popup already hidden; that is the
same outcome X11 produces for a popup whose focus left, and it is accepted explicitly
rather than pretended away. The tests exercise both sides of the deadline: an activation
inside the grace keeps the popup, and an activation after it does not resurrect it.

### 5.4 Unpinning is a new opening

`pinButtonClicked( false )` replaces the window flags and shows the window again
(`scanpopup.cc:1092-1102`, `show()` at `:1102`). The flag change hides the widget, so the
previous `focusedSinceShown` is cleared and the replacement window may be shown inactive.
An unpin therefore follows §5.1's rule: when the new flags are not a platform popup, the
popup asks for activation after `show()`, and can then lose focus and hide. On `xcb`
with the default form the replacement is a `Qt::Popup` and nothing is asked — the
platform takes over, as it does today. (R1-M3)

### 5.5 When nothing happens

* A deactivation for a popup that was never activated is ignored. This is what makes the
  compositor-refusal case honest: no focus was granted, so there is nothing to lose.
* A deactivation while already hidden or pinned is ignored.
* A deactivation caused by the popup's own menu is suppressed and then settled per §5.3.

## 6. Architecture

### 6.1 New module: `popupdismiss.hh` / `popupdismiss.cc`

```cpp
namespace PopupDismiss {

/// Deadline of a settled decision after a suppressed menu deactivation.
constexpr int settleGraceMs = 100;

/// What one deactivation observation means for the popup.
enum class Action { Ignore, Hide };

/// Everything the immediate rule may read. No widget, no application object, no globals.
struct Context {
  bool visible = false;
  bool pinned = false;
  bool focusedSinceShown = false;
  bool ownMenuOpen = false;   ///< one of the popup's own popup widgets is active
};

/// Immediate rule: Hide iff visible && !pinned && focusedSinceShown && !ownMenuOpen.
Action onWindowDeactivate( Context const & context );

/// Settled rule, evaluated once at the deadline.
/// Hide iff visible && !pinned && focusedSinceShown && !ownWindowActive && !ownMenuActive.
Action onFocusSettled( bool visible, bool pinned, bool focusedSinceShown,
                       bool ownWindowActive, bool ownMenuActive );

/// True when the window does not already receive the keyboard from the platform:
/// every window type except Qt::Popup. Window hints never change the answer.
bool needsExplicitActivation( Qt::WindowFlags flags );

/// True when `candidate` is `owner` or a descendant of it. Null-safe; decides whether
/// an active popup widget belongs to this popup.
bool isOwnedBy( QWidget const * owner, QWidget const * candidate );

/// Watches one top-level window and hides it when the rules say so.
///
/// Owns: pinned, focusedSinceShown, the suppression/settlement state and its timer.
/// It never decides anything outside the two rules above and never touches
/// ScanPopup: the caller supplies the dismiss action.
class FocusLossCloser : public QObject {
  Q_OBJECT
public:
  explicit FocusLossCloser( QWidget * window, QObject * parent = nullptr );
  ~FocusLossCloser() override;

  void setPinned( bool pinned );
  bool isPinned() const;
  /// The action to run when a rule returns Hide. Empty means "observe only".
  void setDismissHandler( std::function< void() > handler );

protected:
  bool eventFilter( QObject * watched, QEvent * event ) override;

private:
  void suppress( QWidget * menu );          ///< remember and watch the owned menu
  void armSettlement();                    ///< (re)start the bounded grace
  void cancelSettlement( char const * why );
  void settle( char const * why );         ///< the settled decision, once per grace
  void dismiss( char const * why );        ///< guarded handler invocation

  QWidget * const window;                  ///< observed top-level window, never null
  bool pinned = false;
  bool focusedSinceShown = false;
  bool settlementArmed = false;
  bool dismissing = false;                 ///< re-entrancy guard for the handler
  QPointer< QWidget > suppressor;          ///< owned menu that caused the suppression
  QMetaObject::Connection suppressorGone;
  QTimer settlementTimer;                  ///< single-shot, member, bound to this object
  std::function< void() > dismissHandler;
};

} // namespace PopupDismiss
```

Decisions that keep the module honest:

* **No platform gate.** The flags change at runtime (pin toggle), so a construction-time
  platform test would be wrong. On a `Qt::Popup` window the closer is inert in practice —
  Qt closes the popup first — and both paths end in the same idempotent `hideWindow()`.
* **One place reads application state.** `Context` construction is the only place that
  reads `QApplication::activePopupWidget()`; the settled rule reads
  `window->isActiveWindow()` and `QApplication::activePopupWidget()` again, never the
  modal stack (D8).
* **One dismissal path with one guard.** Both rules call `dismiss()`, which sets
  `dismissing` before invoking the handler and clears it afterwards, so a handler that
  synchronously causes another activation or deactivation cannot re-enter it and
  `hideWindow()` is never called twice for one decision. (R3-M8)
* **Every deferred callback is lifetime-bound.** `settlementTimer` is a member with
  `setSingleShot( true )` whose `timeout` is connected to this object, so destroying the
  closer destroys the timer and cancels the pending decision. No context-free lambda
  captures `this`.
* **Logging** is one `qCDebug` line per decision on the category
  `goldendict.popup.dismiss`, naming the rule and the field that decided it. It logs
  *requests and decisions*, never window titles or looked-up words, and it cannot explain
  a compositor refusal because a refusal produces no event.

### 6.2 `FocusLossCloser` state machine

The closer installs itself as an event filter on `window` and on the suppressor while one
is recorded. It never consumes an event; it always returns `false`.

| Watched object | Event | Condition | Effect |
| --- | --- | --- | --- |
| window | `Show` | — | `focusedSinceShown = window->isActiveWindow()`; clear suppressor; `cancelSettlement( "shown" )` |
| window | `Hide` | — | `focusedSinceShown = false`; clear suppressor; `cancelSettlement( "hidden" )` |
| window | `WindowActivate` | — | `focusedSinceShown = true`; clear suppressor; `cancelSettlement( "popup active" )` |
| window | `WindowDeactivate` | rule → `Ignore` because an owned menu is active | `suppress( menu )` |
| window | `WindowDeactivate` | rule → `Ignore` otherwise | nothing |
| window | `WindowDeactivate` | rule → `Hide` | `focusedSinceShown = false`; clear suppressor; `dismiss( "deactivated" )` |
| suppressor | `WindowDeactivate`, `Hide`, `Close` | — | `armSettlement()` |
| suppressor | any other | — | nothing |
| suppressor | `destroyed` (via `suppressorGone`) | — | clear suppressor; `armSettlement()` |
| settlement timer | `timeout` | — | `settle( "grace expired" )` |

State transitions and their reset points:

* `armSettlement()` sets `settlementArmed = true` and starts the single-shot timer; a call
  while armed **restarts** the grace rather than stacking a second decision, so the last
  suppressor transition owns the deadline.
* `cancelSettlement( why )` clears `settlementArmed`, stops the timer and logs the reason.
* `settle( why )` runs once per armed settlement, at the deadline only: it clears
  `settlementArmed`, stops the timer, evaluates
  `onFocusSettled( window->isVisible(), pinned, focusedSinceShown,
  window->isActiveWindow(), isOwnedBy( window, QApplication::activePopupWidget() ) )`,
  and on `Hide` clears the suppressor and calls `dismiss()`.
* `dismiss( why )` returns immediately when `dismissing` is already true, otherwise sets
  it, calls `dismissHandler`, and clears it — including when the handler hides the window
  and the resulting `Hide` arrives synchronously.
* `suppress( menu )` removes the previous suppressor's filter and `destroyed` connection
  when the culprit differs, then records and watches the new one. A null culprit (no
  active popup widget — not reachable through today's guard computation) still arms
  settlement and watches nothing, so the deadline stays bounded.
* The destructor removes the window filter, the suppressor filter and connection, and
  stops the timer, so a pending settlement can never call a handler after teardown. The
  closer must be a **member** of its window's owner, so it is destroyed before the
  QWidget base of the window.

### 6.3 Wiring in `ScanPopup`

Ordering matters twice here, and both are pinned by assertions (§8.3).
`setWindowFlags()` on a visible widget hides it, and that hide can deliver events
synchronously, so no flag change may happen before the closer knows the new pin state,
and the closer must know the *stored* pin state before the constructor's own flag change.

1. `scanpopup.hh`: `#include "popupdismiss.hh"`, member
   `std::unique_ptr< PopupDismiss::FocusLossCloser > focusLossCloser;`. No `event()` or
   `changeEvent()` override is added.
2. Constructor, in this order — all four steps are asserted:
   a. create the closer on `this` immediately after `ui.setupUi( this )`, and install the
      dismiss handler `[ this ] { hideWindow(); }`;
   b. `ui.pinButton->setChecked( cfg.pinPopupWindow )` (`scanpopup.cc:184`) runs as today;
   c. **after** that line and **before** the `if ( cfg.pinPopupWindow )` block that first
      calls `setWindowFlags()` (`scanpopup.cc:186-200`), call
      `focusLossCloser->setPinned( ui.pinButton->isChecked() )` — the argument is the
      button's newly restored state, not a literal;
   d. only then may a `setWindowFlags()` call run.
   (R1-M2, R3-M6)
3. `pinButtonClicked( bool checked )` (`scanpopup.cc:1077-1105`): its **first complete
   statement** is `focusLossCloser->setPinned( checked );`, before the `if ( checked )`
   statement and therefore before either branch's `setWindowFlags()`. (R1-M2, R2-M6)
4. Unpin branch: after the common `show()` (`scanpopup.cc:1102`), when `!checked &&
   PopupDismiss::needsExplicitActivation( windowFlags() )`, call
   `activateWindow(); raise();`. (§5.4)
5. `engagePopup()` (`scanpopup.cc:605-717`): in the `if ( !isVisible() )` branch, after its
   `show();` (`scanpopup.cc:663`), replace `if ( giveFocus )` with

   ```cpp
   if ( giveFocus || ( !ui.pinButton->isChecked() &&
                       PopupDismiss::needsExplicitActivation( windowFlags() ) ) )
   {
     activateWindow();
     raise();
   }
   ```

   Nothing else in that function changes: the `#ifdef ENABLE_SPWF_CUSTOMIZATION` block —
   including its `giveFocus = true;` assignment for an unpinned `xcb` popup
   (`scanpopup.cc:665-671`) — and both `else if` branches keep their current conditions
   and actions, so the function still contains exactly three `activateWindow()` calls: the
   hidden-window branch (`:675`), the pinned-visible branch (`:694`) and the `xcb` `Tool`
   branch (`:702`), each attributed to one branch. (R2-M5, R3-M7)
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
| Wayland | any | `Tool \| Frameless \| StaysOnTop` | **new rule** (unpinned); hotkey paths also request activation, with a token only where a native handle exists (pre-existing) | **new rule** + §5.3 settlement |
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
| `activePopupWidget()` returns null | the guard is false, which is correct: no menu of the popup's is open |
| closer constructed before the window has a native handle | no handle is needed: only widget events and `isActiveWindow()` are read |
| `Show` while already active | `focusedSinceShown = true`; a later deactivation hides |
| `Show` while not active (compositor declined, or shown behind) | `focusedSinceShown = false`; a deactivation is ignored until an activation arrives |
| pin toggled while visible | `setPinned` runs before the flag change; a pinned popup ignores deactivation from then on |
| unpin while visible | §5.4: the replacement window is treated as a new opening and follows §5.1's rule |
| the popup opens its own dialog | no exception: it deactivates and hides, exactly as X11 (D8) |
| the popup opens its own menu or dropdown | suppressed; settlement armed; the menu is watched |
| the menu closes and focus returns to the popup | the activation cancels settlement whenever it arrives inside the deadline; after the deadline the popup is already hidden (the accepted bounded race, D6) |
| a second menu is opened while a settlement is armed | the previous suppressor's watch is removed when the culprit changes; the last menu owns the restartable deadline |
| the suppressor is destroyed while settlement is armed | the `destroyed` connection re-arms settlement; the timer belongs to the closer, so no dangling pointer is used |
| the closer or its window is destroyed with a settlement armed | the timer member is destroyed and the destructor removes the window and suppressor filters, so no handler runs after teardown |
| a handler that hides the window synchronously causes another activation/deactivation | `dismissing` makes the second decision a no-op; `hideWindow()` is called once per decision |
| deactivation during `hideWindow()` | `isVisible()` is already false or the resulting `Hide` resets the flag; `dismissing` prevents re-entry; the handler is idempotent |
| pointer-poll dismissal racing the deactivation | both call `hideWindow()`; it is idempotent |
| a popup opened while another GoldenDict window holds focus | the request is still made; the compositor decides |
| activation refused by the compositor | no activation, so no decision: the popup stays. Clicking it gives it focus, after which a click elsewhere hides it, and Esc reaches it (closing an open search panel first, as `escapePressed()` already does) |

## 8. Test strategy

New suite `tests/tst_popup_dismiss/`, the 21st required suite, linking the production
`popupdismiss.cc` — never a copy of it.

### 8.1 Rule matrices (data-driven)

`onWindowDeactivate` over its four fields: `Hide` for the satisfying row, one row per
single `Ignore` cause — hidden, pinned, never focused, own menu active — plus two
combination rows and one row per *omitted* guard. The matrix distinguishes an omitted
guard, an inverted predicate and a swapped logical operator; it does not claim that
reordering the `&&` operands is observable, because it is not.

`onFocusSettled` over its five fields: `Hide` for the satisfying row; `Ignore` when the
popup is active again, pinned, hidden, or when a menu of its own is still active. Two
rows exist for the settled exception: a menu that is still active keeps the popup, and a
popup that is not active with no menu open hides. The rule intentionally has **no**
"unarmed" input — the arming gate belongs to the closer, and the controller-level
fixtures in §8.2(9,10) prove it, so no matrix row pretends to. (R3-M5)

`needsExplicitActivation`: `Qt::Popup` → false; `Qt::Popup` with `FramelessWindowHint`
or `StaysOnTop` hints → still false; `Qt::Tool`,
`Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint`, `Qt::Window`,
`Qt::Dialog`, `Qt::Tool | X11BypassWindowManagerHint` → true.

`isOwnedBy`: null/null, self/self, a child popup widget, a grandchild, a sibling, an
unrelated top-level, and a menu parented to a different window.

### 8.2 Runtime behaviour of the production `FocusLossCloser`

On the `xcb` + `xvfb` acceptance display (§8.5), with real top-level widgets; every
fixture drives the production closer and none re-implements its logic. Every fixture that
claims a suppression states and asserts its preconditions — the window visible, unpinned,
activated — and asserts **zero dismissals before the transition it is about**, so a
production mutant that hides too early fails even when the final count is right.
(R3-M3)

1. **hides on a real deactivation** — closer on `first` with a counting handler that
   hides it; `first` shown and activated (`QTest::qWaitForWindowActive`), preconditions
   asserted, then `second` shown and activated; `QTRY_COMPARE` the handler ran once and
   `first` is hidden.
2. **pinned does not hide** — the same drive with `setPinned( true )`; the handler never
   runs and the window stays visible.
3. **an owned menu suppresses, and the guard is really consulted** — `first` activated,
   an owned `QWidget` with `setWindowFlags( Qt::Popup )` shown and asserted as
   `QApplication::activePopupWidget()`; a **synthetic** `QEvent::WindowDeactivate`
   delivered to `first` while `first` is visible, unpinned and armed; assert the handler
   ran **zero** times. A production `Context` that hard-wired `ownMenuOpen` to false hides
   here and fails. The fixture claims nothing about native popup focus, which Qt
   implements with a keyboard grab and which cannot be driven deterministically under a
   bare `Xvfb`. (R2-M4, R3-M3, R3-M4)
4. **opening the popup's own dialog hides it** — a modal `QDialog` parented to `first` is
   shown; `QTRY_COMPARE` the handler ran once, proving D8's parity rule and that no modal
   guard survives in production.
5. **settlement after a menu closes: activation inside the grace keeps the popup** —
   suppress as in 3, close the menu, then activate `first` and
   `QVERIFY( QTest::qWaitForWindowActive( first ) )` so the precondition is established
   rather than assumed; `QTRY_VERIFY` after twice the grace that the handler never ran. A
   zero-delay implementation has already hidden `first`, so the activation assertion
   fails first.
6. **settlement after a menu closes: activation later than the first callback
   opportunity** — the same drive, but one event-loop turn and a `QTest::qWait( 0 )` pass
   with the popup inactive before `first` is activated inside the grace, asserting the
   activation succeeded; the handler never runs. An implementation that decides on the
   first turn instead of the deadline fails.
7. **settlement after a menu closes: no activation at all** — the same drive with no
   activation; assert the popup is still visible after `settleGraceMs / 2` and hidden
   after `settleGraceMs * 2`. That brackets the deadline from both sides instead of
   trusting a single wait. (R3-M2)
8. **a late activation does not resurrect a settled popup** — after 7, activating `first`
   leaves it hidden; the fixture asserts the activation request is issued and the window
   stays hidden, without claiming the compositor granted it.
9. **the deadline restarts on a second suppression** — suppress with `menuA`, close
   `menuA`, open and close `menuB` inside the grace, then assert the popup is still
   visible at `settleGraceMs / 2` after the second close and hidden after
   `settleGraceMs * 2`, with the handler called exactly once. (R3-M5)
10. **suppressor destruction and teardown are bounded** — (a) delete the suppressor while
    armed and assert the popup is hidden by the deadline; (b) destroy the window with a
    settlement armed, drain the event loop past the deadline, and assert the dismiss
    callback ran zero times after teardown. (R3-M5)
11. **a deactivation before any activation is ignored** — `first` shown while `second` is
    active; `QVERIFY( !first->isActiveWindow() )` establishes the state, then a synthetic
    `QEvent::WindowDeactivate` is delivered; no hide.
12. **re-show resets the state** — after a hide, `first` is shown again while `second`
    remains active (asserted inactive first, as in 11); a synthetic deactivation is
    ignored.
13. **the handler is not re-entered** — a handler that synchronously sends a
    `WindowActivate` and a `WindowDeactivate` back to `first` is installed; the drive of
    1 must still call it exactly once. (R3-M8)
14. **unrelated popups are not owned** — `isOwnedBy` is exhaustive in 8.1; the runtime
    layer does not claim to prove it, because a second, unrelated `Qt::Popup` grab cannot
    be driven deterministically under a bare `Xvfb`.

Synthetic deliveries use `QCoreApplication::sendEvent`, so the same production filter
receives them; their fixture names contain `Synthetic` so the difference stays visible.

The runtime fixtures require a display and fail with a reason when there is none, like
`tst_platformwindow`. They are `xcb`-only: on another QPA (`offscreen`) they `QSKIP` with
that reason, and §8.5 states that a skipped fixture does not satisfy acceptance.

### 8.3 Source assertions (wiring the suite cannot link)

`ScanPopup` cannot be linked into a suite, so its wiring is pinned by reading the sources
with the branch-scoped style of `tst_hotkeys` (`selectionBlock`, `selectionBranch`,
`selectionStatements`, `selectionDirectActions`, `selectionOrdered`). The suite carries
its own copies of those small helpers, and every assertion names the mutant it kills.

1. `scanpopup.hh` includes `popupdismiss.hh`, declares the
   `PopupDismiss::FocusLossCloser` member, and adds **no** `event(` / `changeEvent(`
   override.
2. Constructor, as one ordered expectation over the constructor block:
   `ui.setupUi( this )` → `ui.pinButton->setChecked( cfg.pinPopupWindow )` →
   `focusLossCloser->setPinned( ui.pinButton->isChecked() )` → the first
   `setWindowFlags(`. The setPinned argument text is asserted, so a literal `false` fails,
   and the position assertion is relative to `setChecked`, so moving the call above it
   fails as well. (R3-M6)
3. `pinButtonClicked`: the **first complete statement** of the body — the first element
   of `selectionStatements( body )`, which keeps control flow, not
   `selectionDirectActions()` which filters it — is the full statement
   `focusLossCloser->setPinned( checked );` after whitespace normalisation. Moving the
   call below the whole `if ( checked )` statement but above `show()` still fails.
   (R2-M6, R3-m3)
4. `engagePopup`, inside the `if ( !isVisible() )` branch only:
   - the full condition text `if ( giveFocus || ( !ui.pinButton->isChecked() &&
     PopupDismiss::needsExplicitActivation( windowFlags() ) ) )` is present;
   - its consequent contains `activateWindow();` then `raise();` in that order;
   - `show();` **precedes** that guarded block in the branch's statement list, so moving
     the block above `show()` fails;
   - the function contains exactly **three** `activateWindow()` calls, attributed one
     each to the hidden-window branch, the pinned-visible branch
     (`scanpopup.cc:689-696`) and the pre-existing `#ifdef ENABLE_SPWF_CUSTOMIZATION`
     `xcb` `Tool` branch. A fourth, unconditional call fails the count; deleting a
     preserved call fails the attribution;
   - the preserved assignment `giveFocus = true;` still exists inside the
     `#ifdef ENABLE_SPWF_CUSTOMIZATION` block, guarded by
     `QGuiApplication::platformName() == "xcb" && !ui.pinButton->isChecked()`, so removing
     the pre-existing activation for the default `Qt::Popup` path fails.
     (R2-M5, R3-M7)
5. Unpin branch: after the shared `show()`, the activation request is guarded by
   `!checked && PopupDismiss::needsExplicitActivation( windowFlags() )`.
6. `goldendict.pro`: `popupdismiss.hh` is in the `HEADERS` assignment and
   `popupdismiss.cc` in `SOURCES`.
7. `tests/tests.pro`: the required-suite list, the discovered `tests/tst_*/tst_*.pro`
   basenames and the `!count(...)` literal agree, and include `tst_popup_dismiss` (§8.5).

### 8.4 What is not proven here

The compositor's decision, and native popup-mode focus. No fixture can force KWin to grant
or refuse activation, and none is written that pretends to; a native `Qt::Popup` keyboard
grab is proved only through the synthetic-context fixture (§8.2(3)) and the settlement
fixtures, never claimed as a native owner-deactivation observation.
`docs/wayland-limitations.md` records the platform limit.

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
  and closes when it loses focus — including when one of its own dialogs takes the
  keyboard, as on X11; a refused activation leaves it open, and giving it focus (a click)
  restores both Esc and click-away dismissal.
* §3 keeps its "activation is best-effort" paragraph and adds that a refused request
  means the popup is dismissed manually, which is the documented consequence rather than
  a regression.

No README change: its Wayland summary does not claim the popup dismisses itself.

## 10. Risks

| risk | mitigation |
| --- | --- |
| a clipboard-triggered popup takes the keyboard from what the user is typing in | accepted by the operator as the price of X11 parity (D1); it is what `Qt::Popup` does on xcb today |
| a compositor refuses focus, so nothing changes for the user | documented (§9); clicking the popup makes it dismissable, and the log records the request, not a refusal |
| the popup closes while a menu of its own is open | suppressed by the one guard parity needs, then settled at the deadline (§5.3) |
| the popup is stranded after its menu closes | settlement with a deadline, fixtured from both sides and with a restarted deadline (§8.2(5–10)) |
| a returning focus arrives later than the deadline and the popup is already gone | the accepted bounded race (D6), stated here and fixtured beyond the deadline (§8.2(7,8)); this is also the X11 outcome |
| a dialog opened from the popup closes it | intended parity (D8), fixtured (§8.2(4)) |
| a settlement callback runs after teardown | the timer and both filters are members of the closer; the destructor clears them, fixtured (§8.2(10)) |
| the handler is re-entered by its own synchronous effects | `dismissing` guards both decision paths, fixtured (§8.2(13)) |
| a deactivation delivered during the open transition closes the popup immediately | the rule requires `focusedSinceShown`, which only an activation sets |
| the closer swallowing an event | it returns `false` for every event |
| a regression on X11 | the default form is still `Qt::Popup` (`tst_platformwindow` pins the flags) and the preserved `giveFocus` assignment is asserted; the only X11 behaviour change is the stated `Tool`-form addition in §6.4 |
| a new module that the application never links | `goldendict.pro` entries plus the application build in the acceptance command (D7) |

## 11. Out of scope

Hover translation, the scan flag, the clipboard-watch rules, alt-mode, the tray, the
pinned popup's flags and stay-on-top option, main-window focus restoration, and any
attempt to emulate an X11 grab on Wayland.

## 12. Implementation-order note

Not a plan, but the sequence the later plan must respect: the module and its two rules
first (provable alone), then the closer with its settlement lifecycle, then the
`ScanPopup` wiring, then the project-file and inventory integration, then the
documentation. The application build is the last thing that can go red, so it is where a
missing registration surfaces.

## 13. Review ledger

### Round 1 → revision 2

| finding | severity | change |
| --- | --- | --- |
| suppressed deactivation strands the popup | major | settlement introduced (§5.3), further reshaped in R2/R3 |
| `setPinned` after `setWindowFlags` | major | §6.3(3) first statement; §8.3(3) |
| unpin while visible cannot hide | major | §5.4 + §6.3(4); §8.3(5) |
| `goldendict.pro` registration missing | major | D7, §6.3(6), §8.3(6), §8.5 |
| modal fixture cannot activate a sibling | major | the modal case is now the simple parity fixture §8.2(4) (D8) |
| source assertions too weak | major | §8.3, extended in R2/R3 |
| activation policy contradicts preserved `xcb` branches | major | §2 facts, §6.4 table, D5/§4 |
| no runtime proof of `ownPopupOpen` | major | §8.2(3), restructured in R2/R3 |
| pinned-open and Windows/macOS descriptions | minor | §5.1, §6.4 |
| `tests.pro` does not select `xcb` | minor | §8.5 |
| declined-activation fallback overstated | minor | §7 last row, §5.5, §9 |
| `xcb` `Tool` rationale wrong | minor | §6.4 closing paragraph, §10 |
| hover/scan-flag called dismissal paths | minor | §2 rewritten |
| "reordered conjunction" claim | minor | §8.1 |

### Round 2 → revision 3

| finding | severity | change |
| --- | --- | --- |
| modal-stack test strands the popup | major | superseded by D8: modals are no longer a suppression cause |
| zero-delay settlement cannot beat a returning activation | major | deadline + activation-cancels, restated as an accepted bounded race (D6) |
| deferred-work lifecycle incomplete | major | §6.1 timer member, §6.2 reset points and destructor, §8.2(9,10) |
| owned-popup fixture assumes native popup activation | major | §8.2(3) synthetic context proof; native claims removed |
| "exactly twice" `activateWindow()` | major | exactly three, attributed (§6.3(5), §8.3(4)) |
| two ordering mutants survive | major | §8.3(3) first complete statement; §8.3(4) `show()` precedes the block |
| three wrong references | minor | corrected |

### Round 3 → revision 4

| finding | severity | change |
| --- | --- | --- |
| only one suppressor is watched, so an owned-dialog chain can strand the popup | major | D8 removes modal suppression entirely; the only suppressor is a menu, and a menu chain keeps `activePopupWidget()` owned (§6.2 `ownMenuActive`) |
| a returning activation after 100 ms still races | major | made an explicit accepted bounded race (D6), fixtured on both sides and beyond the deadline (§8.2(5–8)) |
| fixtures do not assert preconditions or zero early dismissals | major | §8.2 preamble; §8.2(3) states visible/unpinned/armed and zero dismissals; (4) is the parity proof |
| synthetic deactivation does not make a window inactive | major | §8.2(3) is a context-construction proof, not a native-transition proof; the ordering fixtures (5–7) use a real menu close and assert the activation instead |
| lifecycle fixtures do not distinguish restart from the running timer | major | §8.2(9) restarted deadline with assertions at half and twice the grace; (10) destruction and teardown |
| constructor assertion ignores `setChecked` and the argument | major | §8.3(2) full ordered expectation including the argument text |
| the preserved `xcb` `giveFocus` assignment is not pinned | major | §8.3(4) last bullet |
| no dismissal-in-progress guard on the immediate path | major | `dismiss()` with `dismissing`, §6.1/§6.2, fixtured in §8.2(13) |
| §5.2 wording contradicts the settled rule | minor | §5.2 labelled the immediate rule; §5.3 states the exception |
| `settle()` signature inconsistent | minor | §6.1/§6.2 both use `settle( char const * why )` |
| first-statement assertion text differs from the implementation | minor | §8.3(3) states the full receiver-bearing statement and normalisation |
| background claims `Qt::Popup` only on `xcb` | minor | §2 corrected to match §6.4 |
| hotkey token claim too broad | minor | §2 distinguishes the `scan-popup` binding (`winId()` then token) from `clipboard-lookup` (`NoHandle`, no token) |
