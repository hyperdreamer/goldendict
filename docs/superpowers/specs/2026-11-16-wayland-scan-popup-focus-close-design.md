# Wayland scan popup: close when it loses focus

Status: design revision 3, for review. Run `pm-run-20261116-123313-27afee37`,
topic `wayland-scan-popup-focus-close`.
Review history: R1 → 14 findings (0 blockers, 8 major, 6 minor); R2 → 7 findings
(0 blockers, 6 major, 1 minor), of which R1-1, R1-6 and R1-8 were re-opened as
partially resolved. This revision answers all seven R2 findings; §13 is the
finding-to-change ledger for both rounds.

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
| suppressed deactivation (D6) | a suppression **starts a bounded focus settlement**: the dismissal decision is taken after a short grace, cancelled by any activation inside the popup's own chain, and never by a bare "is a modal open" test | A modal dialog stays in Qt's modal stack while it is merely unfocused, so a stack test strands the popup. A zero-delay callback cannot win a race against a returning activation, so the grace is explicit. (R1-M1, R2-M1, R2-M2) |
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
4. No popup can be left visible, unfocused and undismissable, neither by its own
   suppression of a deactivation nor by a focus that silently returns. (R1-M1, R2-M1)
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
(`scanpopup.cc:688-696`). A **pinned, hidden** popup is shown without a new focus
request, exactly as today: the new rule excludes pinned windows, and the `giveFocus`
argument is false on Linux. The design does not add activation to that path.

### 5.2 Closing

The popup hides when all of these hold:

1. it is visible;
2. it is not pinned;
3. it received activation since it was last shown (`focusedSinceShown`);
4. no popup widget of its own is open (a `QMenu`, a combo dropdown, a context menu);
5. no modal dialog of its own is up.

The dismiss action is the existing `hideWindow()` (`scanpopup.cc:1176-1186`), so the
mouse interception stops, `closeMenu()` is emitted, the word field is deselected and the
popup hides (`hide()` at `scanpopup.cc:1185`). Nothing else is added to that path.

### 5.3 A suppressed deactivation settles instead of sticking

Conditions 4 and 5 suppress the immediate decision when a menu or dialog of the popup's
own took the keyboard. Suppression is never the end of the story: it starts a **focus
settlement** with three rules.

1. **Armed.** A suppression arms settlement and records the suppressing window
   (`suppressor`), which is watched from then on.
2. **Cancelled by the popup's own chain.** Any activation of the popup itself, or of a
   window the popup owns, cancels the pending settlement and keeps the popup. This is
   what protects the ordinary "close my dialog and keep typing in the popup" flow.
3. **Settled.** Settlement ends in a dismissal decision, taken at the first of:
   * an activation of *another* application window (a window that does not belong to the
     popup): decisively outside the chain, so hiding is immediate;
   * the grace deadline (`PopupDismiss::settleGraceMs`, 100 ms) after the suppressor's
     own `WindowDeactivate`, `Hide`, `Close` or destruction.

The settled decision is not "is a modal still open" — Qt keeps a merely unfocused
dialog in its modal stack, so that test would strand the popup. It is "did focus come
back to the popup's own chain": hide when the popup is visible, unpinned, still armed,
not itself the active window, and no window of its own is the active window.

**Focus that is temporarily unknown is never a decision point.** Between the suppressor
transition and the settled decision the popup is kept, even though nothing currently
holds the keyboard; if an activation arrives inside the grace it wins, and if none
arrives the grace expires and the popup is hidden. The grace is bounded work, not a
promise about the compositor: a later activation cannot resurrect a hidden popup, which
is the same outcome X11 produces when the popup closes on the dialog's own activation.
(R1-M1, R2-M1, R2-M2)

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
* A deactivation caused by a menu, a combo dropdown or a modal dialog that belongs to the
  popup is suppressed and then settled per §5.3.

## 6. Architecture

### 6.1 New module: `popupdismiss.hh` / `popupdismiss.cc`

```cpp
namespace PopupDismiss {

/// How long a suppression waits for focus before the settled decision.
constexpr int settleGraceMs = 100;

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

/// Settled rule, evaluated once the grace expired or focus left the chain.
/// Hide iff visible && !pinned && focusedSinceShown
///          && !ownWindowActive && !focusInOwnChain.
Action onFocusSettled( bool visible, bool pinned, bool focusedSinceShown,
                       bool ownWindowActive, bool focusInOwnChain );

/// True when the window does not already receive the keyboard from the platform:
/// every window type except Qt::Popup. Window hints never change the answer.
bool needsExplicitActivation( Qt::WindowFlags flags );

/// True when `candidate` is `owner` or a descendant of it. Null-safe; decides
/// whether an active popup widget, modal dialog or active window belongs to this popup.
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
  void suppress( QWidget * culprit );   ///< remember and watch the owned transient
  void armSettlement();                ///< start the bounded grace
  void cancelSettlement( char const * why );
  void settle();                       ///< the settled decision, once per grace

  QWidget * const window;              ///< observed top-level window, never null
  bool pinned = false;
  bool focusedSinceShown = false;
  bool settlementArmed = false;
  bool settling = false;               ///< guards re-entrancy of settle()
  QPointer< QWidget > suppressor;      ///< owned popup/modal that caused the suppression
  QMetaObject::Connection suppressorGone;
  QTimer settlementTimer;              ///< single-shot, bound to this object's lifetime
  std::function< void() > dismissHandler;
};

} // namespace PopupDismiss
```

Decisions that keep the module honest:

* **No platform gate.** The flags change at runtime (pin toggle), so a construction-time
  platform test would be wrong. On a `Qt::Popup` window the closer is inert in practice —
  Qt closes the popup first — and both paths end in the same idempotent `hideWindow()`.
* **Nothing but the two rules decides.** `Context` construction is the only place that
  reads `QApplication::activePopupWidget()` / `activeModalWidget()`; the settled rule reads
  `window->isActiveWindow()` and `QApplication::activeWindow()`.
* **Every deferred callback is lifetime-bound.** The settlement timer is a `QTimer` member
  with `setSingleShot( true )` whose `timeout` is connected to this object, so destroying
  the closer destroys the timer and cancels the pending decision; `settle()` also returns
  early when `window` is hidden or gone. No `singleShot` lambda captures a raw `this`
  without a context object.
* **Logging** is one `qCDebug` line per decision on the category
  `goldendict.popup.dismiss`, naming the rule and the field that decided it. It logs
  *requests and decisions*, never window titles or looked-up words, and it cannot explain
  a compositor refusal because a refusal produces no event.

### 6.2 `FocusLossCloser` state machine

The closer installs itself as an event filter on `window` and on the suppressor while one
is recorded. It never consumes an event; it always returns `false`. It needs no
application-wide filter and no focus signal: the settlement deadline is what bounds every
suppressed case, and the only event that can change the outcome before then is an
activation of the popup's own chain, which arrives at the closer itself.

| Watched object | Event | Condition | Effect |
| --- | --- | --- | --- |
| window | `Show` | — | `focusedSinceShown = window->isActiveWindow()`; clear suppressor; cancel settlement |
| window | `Hide` | — | `focusedSinceShown = false`; clear suppressor; cancel settlement |
| window | `WindowActivate` | — | `focusedSinceShown = true`; clear suppressor; `cancelSettlement( "popup active" )` |
| window | `WindowDeactivate` | rule → `Ignore` because own popup/modal is active | `suppress( culprit )` |
| window | `WindowDeactivate` | rule → `Ignore` otherwise | nothing |
| window | `WindowDeactivate` | rule → `Hide` | `focusedSinceShown = false`; clear suppressor; call `dismissHandler` |
| suppressor | `WindowDeactivate`, `Hide`, `Close` | — | `armSettlement()` |
| suppressor | any other | — | nothing |
| suppressor | `destroyed` (via `suppressorGone`) | — | clear suppressor; `armSettlement()` |
| settlement timer | `timeout` | — | `settle( "grace expired" )` |

State transitions are explicit about their reset points:

* `armSettlement()` sets `settlementArmed = true` and starts the single-shot timer; a
  second call while armed restarts the grace rather than stacking decisions.
* `cancelSettlement( why )` clears `settlementArmed`, stops the timer and logs the reason.
  It runs on the three activation cases above and on `Show`/`Hide` of the window.
* `settle( why )` runs exactly once per armed settlement — on the timer, never earlier —
  first clears `settlementArmed` and stops the timer, then evaluates
  `onFocusSettled( window->isVisible(), pinned, focusedSinceShown,
  window->isActiveWindow(), isOwnedBy( window, QApplication::activeWindow() ) )`; on
  `Hide` it clears the suppressor and calls `dismissHandler`. `settling` guards
  re-entrancy, because `dismissHandler` hides the window synchronously.
* `suppress( culprit )` removes the previous suppressor's filter and `destroyed`
  connection when the culprit differs, then records and watches the new one.
* The destructor removes the window filter, the suppressor filter and connection, and
  stops the timer, so a pending settlement can never call a handler after teardown.

Guards are computed at decision time, from live application state:

```cpp
QWidget * const popup = QApplication::activePopupWidget();
QWidget * const modal = QApplication::activeModalWidget();
Context const context{ window->isVisible(), pinned, focusedSinceShown,
                       isOwnedBy( window, popup ), isOwnedBy( window, modal ) };
```

A modal dialog or menu opened by the *main* window is not owned by the popup, so it does
not suppress the hide — which matches X11, where a `Qt::Popup` closes when any other
window takes the keyboard. The settled rule deliberately does **not** consult the modal
stack: a dialog that is still open but no longer focused must not keep the popup alive.

### 6.3 Wiring in `ScanPopup`

Ordering matters twice here, and both are pinned by assertions (§8.3).
`setWindowFlags()` on a visible widget hides it, and that hide can deliver events
synchronously, so no flag change may happen before the closer knows the new pin state.

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

   Nothing else in that function changes: the `#ifdef ENABLE_SPWF_CUSTOMIZATION` block
   and both `else if` branches keep their current conditions and actions, so the function
   still contains exactly three `activateWindow()` calls — the hidden-window branch
   (`:675`), the pinned-visible branch (`:694`) and the `xcb` `Tool` branch (`:702`) —
   each attributed to one branch. (R2-M5)
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
| Wayland | any | `Tool \| Frameless \| StaysOnTop` | **new rule** (unpinned); hotkey paths also apply their token (pre-existing) | **new rule** + §5.3 settlement |
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
| `activePopupWidget()`, `activeModalWidget()` or `activeWindow()` returns null | that guard is false; the rules decide on the remaining fields (a null active window means focus is nowhere in the application, so the settled rule hides) |
| closer constructed before the window has a native handle | no handle is needed: only widget events, `isActiveWindow()` and focus signals are read |
| `Show` while already active | `focusedSinceShown = true`; a later deactivation hides |
| `Show` while not active (compositor declined, or shown behind) | `focusedSinceShown = false`; a deactivation is ignored until an activation arrives |
| pin toggled while visible | `setPinned` runs before the flag change; a pinned popup ignores deactivation from then on |
| unpin while visible | §5.4: the replacement window is treated as a new opening and follows §5.1's rule |
| the popup deactivates because its own dialog opened | suppressed; settlement armed; the dialog is watched |
| the suppressor deactivates, hides, closes or is destroyed | settlement: an activation of the popup's chain inside the grace keeps it, otherwise it hides at the grace deadline (`settleGraceMs`, never later) |
| focus returns to the popup after its dialog closes, quickly or after one event-loop turn | the activation cancels settlement whenever it arrives inside the grace; a later activation finds the popup already hidden, exactly as X11 behaves |
| a second suppression arrives while a settlement is armed | the grace restarts; the previous suppressor's watch is removed when the culprit changes |
| the suppressor is destroyed while settlement is armed | the `destroyed` connection re-arms settlement; the timer belongs to the closer, so no dangling pointer is used |
| the closer or its window is destroyed with a settlement armed | the timer member is destroyed and the destructor removes the window and suppressor filters, so no handler runs after teardown |
| deactivation during `hideWindow()` | `isVisible()` is already false or the resulting `Hide` resets the flag; `settling` prevents re-entry; the handler is idempotent |
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

`onFocusSettled` over its five fields: `Hide` for the satisfying row; `Ignore` when the
popup is active again, pinned, unarmed, hidden, or when the active window is inside the
popup's own chain. Two rows exist specifically for the R2 finding: focus outside the
chain while a *modal is still registered* (the settled rule has no modal input and must
hide), and focus on an owned dialog (must keep).

`needsExplicitActivation`: `Qt::Popup` → false; `Qt::Popup` with `FramelessWindowHint`
or `StaysOnTop` hints → still false; `Qt::Tool`,
`Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint`, `Qt::Window`,
`Qt::Dialog`, `Qt::Tool | X11BypassWindowManagerHint` → true.

`isOwnedBy`: null/null, self/self, a child popup widget, a grandchild, a sibling, an
unrelated top-level, and a dialog parented to a different window.

### 8.2 Runtime behaviour of the production `FocusLossCloser`

On the `xcb` + `xvfb` acceptance display (§8.5), with real top-level widgets; every
fixture drives the production closer and none re-implements its logic.

1. **hides on a real deactivation** — closer on `first` with a counting handler that
   hides it; `first` shown and activated (`QTest::qWaitForWindowActive`), then `second`
   shown and activated; `QTRY_COMPARE` the handler ran once and `first` is hidden.
2. **pinned does not hide** — the same drive with `setPinned( true )`; the handler never
   runs and the window stays visible.
3. **an owned window-modal dialog suppresses, and the popup is dismissed once focus
   leaves its chain** — a `Qt::WindowModal` `QDialog` parented to `first` is shown; await
   `QTest::qWaitForWindowActive( &dialog )` and assert `!first->isActiveWindow()`; then
   activate the sibling `second` (allowed: window-modality blocks the dialog's parent
   chain, and the fixture asserts the activation rather than assuming it).
   `QTRY_COMPARE` the handler ran once, **and assert that the dialog is still visible and
   still `QApplication::activeModalWidget()`** — the settled rule must dismiss while a
   merely unfocused modal remains registered. (R2-M1)
4. **an owned popup suppresses the immediate decision** — a `QWidget` with
   `setWindowFlags( Qt::Popup )` parented to `first` is shown; assert
   `QApplication::activePopupWidget()` is that widget. Then deliver a **synthetic**
   `QEvent::WindowDeactivate` to `first` and assert the handler did **not** run: this is
   the fixture that proves the production `Context` construction reports the owned popup
   (a hard-wired `false` hides here and fails). It claims nothing about native popup
   focus, which Qt implements with a keyboard grab and which cannot be driven
   deterministically under a bare `Xvfb`. (R2-M4)
5. **native owned-popup teardown does not strand the popup** — the popup and an owned
   `Qt::Popup` widget are shown, the owned popup is then closed while `second` receives
   focus; `QTRY_COMPARE` the handler ran once. This fixture proves the settlement path
   after an owned popup disappears; it deliberately makes no claim about which window
   reported active while the popup was open.
6. **a deactivation before any activation is ignored** — `first` shown while `second` is
   active; `QVERIFY( !first->isActiveWindow() )` establishes the state, then a synthetic
   `QEvent::WindowDeactivate` is delivered to `first`; no hide.
7. **re-show resets the state** — after a hide, `first` is shown again while `second`
   remains active (asserted inactive first, as in 6); a synthetic deactivation is
   ignored.
8. **settlement ordering, both ways** (R2-M2):
   - *activation first*: suppress via an owned dialog, then activate `first` (the
     popup) inside the grace; `QTRY_VERIFY` after twice the grace that the handler never
     ran;
   - *activation later than the first callback opportunity*: suppress, let at least one
     event-loop turn pass with the popup still inactive, then deliver the activation
     before the grace expires; the handler never runs. If the implementation ignored the
     grace and hid on the first turn, this fixture fails.
9. **a late activation after the grace has expired does not resurrect the popup** — the
   popup is hidden by settlement; a later activation leaves it hidden (the stated,
   X11-consistent outcome).
10. **settlement lifecycle** (R2-M3): a second suppression after a completed settlement;
    destruction of the suppressor while armed; destruction of the window with a
    settlement armed. In the last case the fixture drains the event loop past the grace
    and asserts the dismiss callback ran zero times after teardown.
11. **unrelated popups are not owned** — `isOwnedBy` is exhaustive in 8.1; the runtime
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
its own copies of those small helpers. Each assertion names the mutant it kills.

1. `scanpopup.hh` includes `popupdismiss.hh`, declares the
   `PopupDismiss::FocusLossCloser` member, and adds **no** `event(` / `changeEvent(`
   override.
2. Constructor: `PopupDismiss::FocusLossCloser` is constructed on `this`,
   `setDismissHandler` names `hideWindow`, and `setPinned` appears **before** the first
   `setWindowFlags` (statement order within the constructor block).
3. `pinButtonClicked`: the **first complete statement** of the body —
   `selectionStatements( body ).first()`, which keeps control flow, not
   `selectionDirectActions()` which filters it — is `setPinned( checked );`. Moving the
   call after the whole `if ( checked )` statement but before `show()` still fails this
   assertion. (R2-M6)
4. `engagePopup`, inside the `if ( !isVisible() )` branch only:
   - the full condition text `if ( giveFocus || ( !ui.pinButton->isChecked() &&
     PopupDismiss::needsExplicitActivation( windowFlags() ) ) )` is present;
   - its consequent contains `activateWindow();` then `raise();` in that order;
   - `show();` **precedes** that guarded block in the branch's statement list, so moving
     the whole block above `show()` fails;
   - the function contains exactly **three** `activateWindow()` calls, attributed one
     each to the hidden-window branch, the pinned-visible branch
     (`scanpopup.cc:688-696`) and the pre-existing `#ifdef ENABLE_SPWF_CUSTOMIZATION`
     `xcb` `Tool` branch. A fourth, unconditional call fails the count; deleting a
     preserved call fails the attribution. (R2-M5)
5. Unpin branch: after the shared `show()`, the activation request is guarded by
   `!checked && PopupDismiss::needsExplicitActivation( windowFlags() )`.
6. `goldendict.pro`: `popupdismiss.hh` is in the `HEADERS` assignment and
   `popupdismiss.cc` in `SOURCES`.
7. `tests/tests.pro`: the required-suite list, the discovered `tests/tst_*/tst_*.pro`
   basenames and the `!count(...)` literal agree, and include `tst_popup_dismiss` (§8.5).

### 8.4 What is not proven here

The compositor's decision, and native popup-mode focus. No fixture can force KWin to grant
or refuse activation, and none is written that pretends to; likewise a native `Qt::Popup`
keyboard grab is proved only through the synthetic-`Context` fixture in §8.2(4) and the
settlement fixtures, never claimed as a native owner-deactivation observation.
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
| hiding while a menu or dialog is open | suppressed, then settled per §5.3, tested at rule and runtime level |
| the popup is stranded by its own suppression | settlement with a bounded grace, plus the fixture that dismisses while the dialog is still registered as modal (R2-M1) |
| a returning focus loses the race and the popup disappears under the user | the grace plus activation-cancels rule, with both orderings fixtured (R2-M2) |
| a settlement callback runs after teardown | the timer and both filters are members of the closer; the destructor clears them, fixtured (§8.2(10)) |
| a deactivation delivered during the open transition closes the popup immediately | the rule requires `focusedSinceShown`, which only an activation sets |
| the closer swallowing an event | it returns `false` for every event |
| a regression on X11 | the default form is still `Qt::Popup` (`tst_platformwindow` pins the flags); the only X11 behaviour change is the stated `Tool`-form addition in §6.4 |
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
| suppressed deactivation strands the popup | major | §5.3 settlement, §6.2, §8.2(3) — **further corrected in R2** |
| `setPinned` after `setWindowFlags` | major | §6.3(3) first statement; §8.3(3) |
| unpin while visible cannot hide | major | §5.4 + §6.3(4); §8.3(5) |
| `goldendict.pro` registration missing | major | D7, §6.3(6), §8.3(6), §8.5 |
| modal fixture cannot activate a sibling | major | §8.2(3) |
| source assertions too weak | major | §8.3(4) — **extended in R2** |
| activation policy contradicts preserved `xcb` branches | major | §2 facts, §6.4 table, D5/§4 |
| no runtime proof of `ownPopupOpen` | major | §8.2(4) — **restructured in R2** |
| pinned-open and Windows/macOS descriptions | minor | §5.1, §6.4 |
| `tests.pro` does not select `xcb` | minor | §8.5 |
| declined-activation fallback overstated | minor | §7 last row, §5.5, §9 |
| `xcb` `Tool` rationale wrong | minor | §6.4 closing paragraph, §10 |
| hover/scan-flag called dismissal paths | minor | §2 rewritten |
| "reordered conjunction" claim | minor | §8.1 |

### Round 2 → revision 3

| finding | severity | change |
| --- | --- | --- |
| modal-stack test still strands the popup | major | §5.3 settled rule is "did focus come back to the chain", not "is a modal open"; §6.1 `onFocusSettled`; §8.2(3) asserts the dialog is still the active modal widget when the popup is dismissed |
| zero-delay settlement cannot beat a returning activation | major | §5.3 bounded grace + activation-cancels; §6.1 `settleGraceMs` + `QTimer` member; §8.2(8,9) both orderings fixture plus the late-activation case |
| deferred-work lifecycle incomplete | major | §6.1 timer member with `this` lifetime, §6.2 reset points, destructor, `suppressorGone`; §8.2(10) teardown and re-suppression fixtures |
| owned-popup fixture assumes native popup activation | major | §8.2(4) synthetic `Context` proof, §8.2(5) separate native teardown proof, §8.4 states the non-claim |
| "exactly twice" `activateWindow()` rejects valid wiring | major | §6.3(5) and §8.3(4): exactly three calls, one per permitted branch, with attribution |
| two ordering mutants survive the assertions | major | §8.3(3) first *statement* (control flow kept); §8.3(4) `show()` precedes the guarded block |
| three wrong references | minor | §6.3 refers to §8.3; `show()` at `scanpopup.cc:1102`; `hideWindow()` range 1176-1186 including `hide()` at `:1185` |
