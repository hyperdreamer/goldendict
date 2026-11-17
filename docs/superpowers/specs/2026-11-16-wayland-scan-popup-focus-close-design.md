# Wayland scan popup: close when it loses focus

Status: design revision 5, for review. Run `pm-run-20261116-123313-27afee37`,
topic `wayland-scan-popup-focus-close`.
Review history: R1 → 14 findings; R2 → 7; R3 → 13; R4 → 15 (1 blocker, 10 major,
4 minor). Revisions 2–4 kept patching an activation-state model; this revision replaces
it with an **event-driven focus-destination model**, which removes the blocker, the
timer, all suppression state and eleven of R4's findings by construction. §13 is the
finding-to-change ledger.

**Scope limit of this document.** It specifies a design, not a test implementation.
Most of the behaviour lives in `ScanPopup`, a translation unit no suite links (it needs
`ArticleView`, a `QWebEngineProfile`, the dictionary registry and the whole
application). Where a claim cannot be observed by a test, it is pinned by a *source
assertion* and stated as such. The decision logic and the event plumbing live in a new,
directly linkable module precisely so that the important half is proven by running it.

## 1. Decision record

| decision | choice | reason |
| --- | --- | --- |
| scope (D1) | **full X11 parity**: an unpinned popup asks for keyboard focus when it opens **and** hides as soon as it loses it | A window that never received focus cannot lose it. Closing on deactivation alone would not change anything for the clipboard-triggered popup the reporter was looking at. Recorded operator reply: `2`. |
| new preference (D2) | **none** | X11's `Qt::Popup` behaviour is unconditional; a toggle would create a third mode nobody asked for and a preference row to mock up. |
| detection (D3) | **`QGuiApplication::focusWindowChanged` / `applicationStateChanged`**, not activation state and not pointer polling | `QWidget::isActiveWindow()` is not focus evidence: for a visible, parentless, non-modal `Qt::Tool` Qt reports the window active whenever `SH_Widget_ShareActivation` is set, and `QCommonStyle` sets it — which is exactly the production popup's shape on Wayland. The focus *destination* is an event the platform really delivers, and identity comparison cannot be fooled by a style hint. (R4-B1) |
| placement (D4) | new module `popupdismiss.{hh,cc}` | `ScanPopup` is unlinkable in a test; one pure rule plus a small signal-driven `QObject` keep the decision and the wiring linkable, in the style of `platformwindow.{hh,cc}`. |
| re-focus on later words (D5) | **the new policy asks nothing on a later word**; only a transition from hidden to visible asks | A clipboard change arrives while the user may be typing elsewhere. Pre-existing requests (the `xcb` `giveFocus` block, the `xcb` `Tool` re-raise, the hotkey token) are preserved untouched and listed in §6.4. |
| modal dialogs are **not** an exception (D8) | opening the popup's own dialog hides the popup, exactly as X11's `Qt::Popup` does | Parity is the operator's criterion, and `Qt::Popup` closes when any other window takes the keyboard. It is also safe here: the only dialog the popup's own `closeMenu()` can reject is the resource-save dialog, which `ArticleView` gates off in the popup (`articleview.cc:1144`), so hiding cannot cancel a dialog the user just opened. |
| no settlement timer (D9) | the decision is taken **when focus moves**, with no deadline and no armed grace | A timer cannot order itself against compositor events; a coarse or late timer can fire before a returning activation. Focus-destination events are edge-triggered and ordered by the platform, so nothing needs to be guessed. (R2-M2, R3-M2, R4-M4) |
| application integration (D7) | `popupdismiss.{hh,cc}` are added to `goldendict.pro`'s enumerated `HEADERS`/`SOURCES`, and acceptance includes an application build | `goldendict.pro` enumerates its sources (`goldendict.pro:426, 573`); a green helper suite must not hide an unlinkable application. (R1-M4) |

## 2. Background

On X11 the unpinned popup is created with `Qt::Popup` when the stored form is `default`
or `Popup` (`platformwindow.cc:38`, `defaultX11Flags`), and with the managed tool window
when the stored form is `Tool` (`scanpopup.cc:433-440`). A `Qt::Popup` window is given
the keyboard by Qt and is closed by Qt itself as soon as it loses the popup grab: click
another window — or open a dialog from it — and the popup is gone. The same
`Qt::Popup` default is what a build without the X11-only customization uses on every
platform, so the Windows and macOS defaults have that dismissal too.

On Wayland the same call site returns the managed window on purpose:
`PlatformWindow::popupFlags()` returns `Qt::Tool | Qt::FramelessWindowHint |
Qt::WindowStaysOnTopHint` for every Wayland platform, unconditionally
(`platformwindow.cc:32-33, 118-119`). That is an ordinary window: clicking elsewhere
moves the keyboard focus and the popup stays where it is.

The dismissal paths that exist today:

* **Qt's own popup close**, wherever the window is a `Qt::Popup` — the `xcb` `default`
  and `Popup` forms, and a build without the X11-only customization;
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
* A popup opened by a **global hotkey** asks for activation using the portal's token
  (`mainwindow.cc:3325-3336`). The `scan-popup` binding first materialises a native
  handle (`scanPopup->winId()`, `mainwindow.cc:3335`), so a token can be installed; the
  `clipboard-lookup` binding does not, and `applyActivation()` reports `NoHandle`
  (`platformwindow.cc:160-172`) — and installs no token — **only when the window has no
  handle yet**, which is the normal case on a first lookup and not the case once an
  earlier show or `winId()` call created one. Both bindings' behaviour is preserved.

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
4. After it has been activated, no popup is left visible and unfocused: not when focus
   moves to another window, another application, or a dialog the popup opened, and not
   while and after one of its own menus is used. A popup that was never activated stays
   as it is (the documented refusal case).
5. The parts that can be proven on this host are proven by execution; the part that
   needs a live compositor is documented as a limit, not claimed as a result.

## 4. Non-goals

* No new preference, no new control, no changed default. (D2)
* No emulation of an X11 pointer/keyboard grab.
* No change to hover translation, the scan flag, the clipboard-watching rules, the
  alt-mode chord, or the tray behaviour.
* No change to the pinned popup's window flags or its stay-on-top option.
* No change to the main window or to the article view.
* No change to the pre-existing activation requests listed in §6.4, and no change to the
  pre-existing pointer-proximity dismissal paths, which keep their current behaviour —
  including the fact that on the `xcb` `Tool` form they can already hide a popup while
  one of its menus is open. The new rule adds no such behaviour and does not remove it.
  (R4-M3)

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
request, exactly as today. The design does not add activation to that path.

### 5.2 Closing: one rule, driven by where focus went

The popup keeps one piece of state besides "pinned": whether it is **armed**, meaning
"this window has held keyboard focus since it was last shown". It becomes armed on its
own activation and is cleared when it hides.

The popup hides when all of these hold:

1. it is visible;
2. it is not pinned;
3. it is armed;
4. keyboard focus moved **out of the popup's own interaction**, which means either

   * focus went to any other window — another GoldenDict window, a dialog, another
     application's window; or
   * focus went nowhere (`focusWindow() == nullptr`) **and** the application reports
     `Qt::ApplicationInactive`.

Focus that lands on one of the popup's own menus or combo dropdowns is **not** leaving:
it is the same interaction, so the popup stays and remains armed.

There is no timer, no grace, and no "wait and see" (D9). The decision is taken on the
platform event that reports the move.

### 5.3 Dialogs, menus and the application losing focus

* **A dialog the popup opened** takes focus, so focus left the popup's interaction and
  the popup hides — the X11 outcome (D8). Hiding cannot cancel that dialog: the popup's
  `closeMenu()` reaches `ArticleView::closePopupMenu`, whose only dialog connection is
  the resource-save dialog, and that one is gated off in the popup
  (`articleview.cc:1144`, `if( url.isEmpty() || popupView ) return;`).
* **A menu or combo dropdown of the popup** keeps the popup alive while it is open. When
  it closes, focus returns to the popup (kept and re-armed) or goes elsewhere (hidden) —
  whichever the platform reports, with no timer to guess.
* **The whole application losing focus** (`applicationStateChanged` to
  `Qt::ApplicationInactive`) hides an armed, unpinned, visible popup even when no
  `focusWindowChanged` event accompanies it.

### 5.4 Unpinning is a new opening

`pinButtonClicked( false )` replaces the window flags and shows the window again
(`scanpopup.cc:1092-1102`, `show()` at `:1102`). The flag change hides the widget, so
`armed` is cleared and the replacement window may be shown inactive. An unpin therefore
follows §5.1's rule: when the new flags are not a platform popup, the popup asks for
activation after `show()`, and can then lose focus and hide. On `xcb` with the default
form the replacement is a `Qt::Popup` and nothing is asked — the platform takes over.

### 5.5 When nothing happens

* A focus move while the popup was never activated (not armed) is ignored: no focus was
  granted, so there is nothing to lose. This is the documented compositor-refusal case.
* A focus move while the popup is hidden or pinned is ignored.
* A focus move to one of the popup's own menus or dropdowns is ignored.

## 6. Architecture

### 6.1 New module: `popupdismiss.hh` / `popupdismiss.cc`

```cpp
namespace PopupDismiss {

enum class Action { Ignore, Hide };

/// Where the application's keyboard focus is now.
enum class FocusDestination {
  OwnWindow,   ///< the popup window itself
  OwnPopup,    ///< a popup widget (menu, combo dropdown) that belongs to the popup
  Foreign,     ///< any other window, in this application or another
  None,        ///< the platform reports no focus window at all
};

/// The rule: Hide iff visible && !pinned && armed
/// && (destination == Foreign || (destination == None && !applicationActive)).
Action onFocusMoved( bool visible, bool pinned, bool armed,
                     FocusDestination destination, bool applicationActive );

/// True when the window does not already receive the keyboard from the platform:
/// every window type except Qt::Popup. Window hints never change the answer.
bool needsExplicitActivation( Qt::WindowFlags flags );

/// True when `candidate` is a popup widget *belonging to* `owner`: owned, and not
/// `owner` itself. Null-safe. The self-exclusion keeps a Qt::Popup-form popup from
/// being taken for one of its own menus. (R4-m1)
bool isOwnedPopup( QWidget const * owner, QWidget const * candidate );

/// Watches one top-level window's focus and hides it when the rule says so.
///
/// Owns: pinned, armed, the dismissal guard. It never decides anything outside the
/// rule above and never touches ScanPopup: the caller supplies the dismiss action.
class FocusLossCloser : public QObject {
  Q_OBJECT
public:
  explicit FocusLossCloser( QWidget * window, QObject * parent = nullptr );
  ~FocusLossCloser() override;

  void setPinned( bool pinned );
  bool isPinned() const;
  /// The action to run when the rule returns Hide. Empty means "observe only".
  /// The handler may destroy the closer; nothing is touched after it is called.
  void setDismissHandler( std::function< void() > handler );

protected:
  bool eventFilter( QObject * watched, QEvent * event ) override;   // Show / Hide only

private:
  FocusDestination destinationOf( QWindow const * focusWindow ) const;
  void dismiss( char const * reason );

  QWidget * const window;            ///< observed top-level window, never null
  bool pinned = false;
  bool armed = false;
  bool dismissing = false;           ///< re-entrancy guard for the handler
  QMetaObject::Connection focusConnection;
  QMetaObject::Connection stateConnection;
  std::function< void() > dismissHandler;
};

} // namespace PopupDismiss
```

Decisions that keep the module honest:

* **No activation-state reads.** The closer never calls `isActiveWindow()`; it compares
  focus-window identity and application state only. (R4-B1)
* **No timer, no suppression state.** Arming is the only state besides pinning, and it
  is set and cleared by two events. (D9)
* **Connections are context-bound.** Both `QObject::connect` calls pass `this` as the
  receiver, so Qt removes them when the closer is destroyed; the destructor also
  disconnects explicitly and removes the Show/Hide filter. A slot that destroys the
  closer is legal: `dismiss()` uses a `QPointer` guard and returns without touching any
  member afterwards. (R4-M10)
* **One dismissal path, guarded.** Both the focus slot and the application-state slot
  call `dismiss()`, which returns immediately while `dismissing`, and clears the flag
  only if the object still exists after the handler returned. `hideWindow()` is
  therefore called at most once per decision, even though it is not itself idempotent —
  it emits `closeMenu()` before hiding. (R4-M9)
* **Logging** is one `qCDebug` line per decision on the category
  `goldendict.popup.dismiss`, naming the rule and the destination that decided it. It
  logs *decisions*, never window titles or looked-up words, and it cannot explain a
  compositor refusal because a refusal produces no event.

### 6.2 `FocusLossCloser` event model

| Source | Event | Condition | Effect |
| --- | --- | --- | --- |
| window | `Show` | — | `armed = ( window->windowHandle() != nullptr && QGuiApplication::focusWindow() == window->windowHandle() )` |
| window | `Hide` | — | `armed = false` |
| `QGuiApplication` | `focusWindowChanged( QWindow * )` | destination `OwnWindow` | `armed = true`; nothing else |
| `QGuiApplication` | `focusWindowChanged( QWindow * )` | destination `OwnPopup` | nothing — the interaction continues, `armed` is unchanged |
| `QGuiApplication` | `focusWindowChanged( QWindow * )` | destination `Foreign` | `dismiss( "focus moved to another window" )` |
| `QGuiApplication` | `focusWindowChanged( QWindow * )` | destination `None` | `dismiss( "focus left the application" )` only when `qApp->applicationState() != Qt::ApplicationActive`; otherwise nothing |
| `QGuiApplication` | `applicationStateChanged( Qt::ApplicationState )` | `Qt::ApplicationInactive` | `dismiss( "application inactive" )` |

`destinationOf()` is the only place that maps a `QWindow` to a destination:

```cpp
if( !focusWindow ) return FocusDestination::None;
if( focusWindow == window->windowHandle() ) return FocusDestination::OwnWindow;
if( isOwnedPopup( window, QApplication::activePopupWidget() ) &&
    focusWindow == QApplication::activePopupWidget()->windowHandle() )
  return FocusDestination::OwnPopup;
return FocusDestination::Foreign;
```

`dismiss( reason )` returns immediately when `dismissing` is set, otherwise sets it,
logs, calls the handler under a `QPointer` guard, and clears the flag only if the closer
survived. `armed` is then cleared, because the window no longer holds focus.

Why this model removes the previous rounds' problems: there is no "did this window lose
activation" question (which a shared-activation style hint can answer wrongly), no
suppressor to lose track of when a second menu opens, no grace to race a returning
activation, and no timer whose coarseness can fire early. Every decision is the direct
consequence of one platform event.

### 6.3 Wiring in `ScanPopup`

Ordering matters twice, and both are pinned by assertions (§8.3). `setWindowFlags()` on a
visible widget hides it, and that hide can deliver events synchronously, so no flag
change may happen before the closer knows the new pin state.

1. `scanpopup.hh`: `#include "popupdismiss.hh"`, member
   `std::unique_ptr< PopupDismiss::FocusLossCloser > focusLossCloser;`. No `event()` or
   `changeEvent()` override is added.
2. Constructor, in this order — all steps are asserted:
   a. create the closer on `this` immediately after `ui.setupUi( this )`;
   b. install the dismiss handler `[ this ] { hideWindow(); }` on it;
   c. `ui.pinButton->setChecked( cfg.pinPopupWindow )` (`scanpopup.cc:184`) runs as today;
   d. **after** that line and **before** the `if ( cfg.pinPopupWindow )` block that first
      calls `setWindowFlags()` (`scanpopup.cc:186-200`), call
      `focusLossCloser->setPinned( ui.pinButton->isChecked() )` — the argument is the
      button's newly restored state, not a literal;
   e. only then may a `setWindowFlags()` call run.
   (R1-M2, R3-M6, R4-M8)
3. `pinButtonClicked( bool checked )` (`scanpopup.cc:1077-1105`): its **first complete
   statement** is `focusLossCloser->setPinned( checked );`, before the `if ( checked )`
   statement and therefore before either branch's `setWindowFlags()`.
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
   (`scanpopup.cc:665-671`) — and both `else if` branches keep their conditions and
   actions, so the function still contains exactly three `activateWindow()` calls: the
   hidden-window branch (`:675`), the pinned-visible branch (`:694`) and the `xcb` `Tool`
   branch (`:702`). (R2-M5, R3-M7, R4-M11)
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
| Wayland | any | `Tool \| Frameless \| StaysOnTop` | **new rule** (unpinned); hotkey paths also request activation, with a token only when a native handle exists (pre-existing) | **new rule** + §5.3 |
| xcb (X11 build) | `default` or `Popup` | `Qt::Popup` | pre-existing `giveFocus` block, unpinned only; the new rule answers false | Qt's own popup close |
| xcb (X11 build) | `Tool` | `Tool \| Frameless \| StaysOnTop` | pre-existing `giveFocus` block **and** the new rule (both unpinned) | **new rule** + the pre-existing proximity/leave timer |
| Windows / macOS | n/a (`SPWF_default` when the X11-only customization is absent) | `Qt::Popup` | Windows: `giveFocus = true` for the two clipboard/hotkey word paths; the new rule answers false | Qt's own popup close |

The `xcb` `Tool` row is an intentional **addition** beyond the Wayland goal, recorded so
that the X11 behaviour change is a stated decision rather than an accident. (R1-m4)

## 7. Error handling and edge cases

| situation | behaviour |
| --- | --- |
| `focusWindow()` is null because the application is inactive | hide (rule, §5.2) |
| `focusWindow()` is null while the application is active (a popup/menu closing) | ignore; the next focus event decides |
| `activePopupWidget()` returns null | destination is `OwnWindow`/`Foreign`/`None` accordingly; no menu logic runs |
| closer constructed before the window has a native handle | `Show` arms only once a handle exists; the first `focusWindowChanged( own handle )` arms it |
| `Show` while already focused | `armed = true` from identity, no event needed |
| `Show` while not focused (compositor declined, or shown behind) | `armed = false`; focus moves are ignored until an activation arrives |
| pin toggled while visible | `setPinned` runs before the flag change; a pinned popup ignores everything |
| unpin while visible | §5.4: treated as a new opening |
| the popup opens its own dialog | focus moved to a foreign window, so it hides; the dialog is unaffected (§5.3) |
| the popup's menu is open | destination `OwnPopup`: nothing happens |
| the menu closes and focus returns to the popup | `focusWindowChanged( own handle )`: kept and re-armed |
| a second menu opens after the first closed | each move is its own event; no suppressor bookkeeping exists to get out of date |
| the handler hides the window synchronously | the resulting `Hide` clears `armed`; `dismissing` prevents a nested decision |
| the handler destroys the closer or the window | tolerated: `dismiss()` returns under a `QPointer` guard, and Qt removes the connections |
| other code paths call `hideWindow()` | unchanged; the closer adds no requirement on them (§4) |
| activation refused by the compositor | no activation event, so nothing is armed: the popup stays. Clicking it gives it focus, after which focus leaving hides it, and Esc reaches it (closing an open search panel first, as `escapePressed()` already does) |

## 8. Test strategy

New suite `tests/tst_popup_dismiss/`, the 21st required suite, linking the production
`popupdismiss.cc` — never a copy of it.

### 8.1 Rule matrix (data-driven)

`onFocusMoved` over its five inputs: `Hide` for the satisfying rows and `Ignore` for each
single cause — hidden, pinned, unarmed, own window, own popup — plus the `None` rows with
the application active and inactive, and combination rows. The matrix distinguishes an
omitted guard, an inverted predicate and a swapped logical operator; it does not claim
that reordering `&&` operands is observable, because it is not.

`needsExplicitActivation`: `Qt::Popup` → false; `Qt::Popup` with `FramelessWindowHint` or
`StaysOnTop` hints → still false; `Qt::Tool`,
`Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint`, `Qt::Window`,
`Qt::Dialog`, `Qt::Tool | X11BypassWindowManagerHint` → true.

`isOwnedPopup`: null/null, self/self (**false** — the popup is not its own menu), a child
popup widget, a grandchild, a sibling, an unrelated top-level, and a menu parented to a
different window. (R4-m1)

### 8.2 Runtime behaviour of the production `FocusLossCloser`

On the `xcb` + `xvfb` acceptance display (§8.5). Every fixture drives the production
closer through real platform events, states its preconditions, and asserts the focus
transitions it relies on instead of assuming them: each step waits for
`QGuiApplication::focusWindow()` to be the expected handle (`QTRY_COMPARE`), so a
`QWidget` that merely reports itself active cannot stand in for a real focus move.

1. **hides when focus moves to another window** — closer on `first` with a counting
   handler that hides it; `first` shown and activated with `focusWindow() == first`'s
   handle asserted; `second` shown and activated with the same assertion; then
   `QTRY_COMPARE` the handler ran once and `first` is hidden.
2. **pinned does not hide** — the same drive with `setPinned( true )`; the handler never
   runs and the window stays visible.
3. **a menu of the popup's own keeps it** — `first` activated; an owned `QWidget` with
   `setWindowFlags( Qt::Popup )` shown, with `QApplication::activePopupWidget() ==
   &menu` asserted; the platform focus transition to the menu asserted
   (`focusWindow() == menu.windowHandle()`) or, if this QPA does not report the popup as
   the focus window, the fixture `QSKIP`s with that reason rather than pretending. While
   that holds, the handler never runs.
4. **opening the popup's own dialog hides it** — a dialog parented to `first` is shown;
   assert the focus transition to the dialog and `QTRY_COMPARE` the handler ran once,
   with `first` hidden and the dialog still visible.
5. **focus returning to the popup keeps it** — after 3, the menu is closed; the focus
   transition back to `first` is asserted; the handler never ran and `first` is visible.
6. **the application losing focus hides it** — the `ApplicationInactive` branch cannot be
   driven without a second process on this host; the pure matrix covers it, and the
   runtime layer states that it makes no claim here (§8.4).
7. **re-show arms only on a real activation** — after a hide, `first` is shown while
   `second` keeps focus (asserted); activating a third window then does not dismiss
   `first`.
8. **a handler that causes another focus move is not re-entered** — a handler that
   synchronously activates `second` and re-activates `first` is installed; the drive of 1
   still calls it exactly once.
9. **a handler that destroys the closer is tolerated** — the handler deletes the closer;
   a later focus move must not crash and must call nothing.
10. **teardown with the closer armed** — the closer is destroyed while `first` is armed
    and visible; a later focus move calls nothing and does not crash (Qt removes the
    connections).

Every fixture that claims an early decision also asserts **zero dismissals before the
transition it is about**, so a production mutant that hides too early fails even when
the final count is right.

### 8.3 Source assertions (wiring the suite cannot link)

`ScanPopup` cannot be linked into a suite, so its wiring is pinned by reading the sources
with the branch-scoped style of `tst_hotkeys` (`selectionBlock`, `selectionBranch`,
`selectionStatements`, `selectionDirectActions`, `selectionOrdered`). The suite carries
its own copies, which **strip preprocessor directive lines** before statement parsing, so
a statement-order assertion never spans an `#ifdef` boundary; the preserved `giveFocus =
true;` assignment is instead asserted with two independent searches (the directive line
and the assignment line inside that region). (R4-M11)

1. `scanpopup.hh` includes `popupdismiss.hh`, declares the
   `PopupDismiss::FocusLossCloser` member, and adds **no** `event(` / `changeEvent(`
   override.
2. Constructor, as one ordered expectation over the constructor block:
   `ui.setupUi( this )` → construction of `PopupDismiss::FocusLossCloser` on `this` →
   `setDismissHandler` naming `hideWindow` → `ui.pinButton->setChecked( cfg.pinPopupWindow )`
   → `focusLossCloser->setPinned( ui.pinButton->isChecked() )` → the first
   `setWindowFlags(`. The setPinned argument text is asserted (a literal `false` fails)
   and its position is asserted relative to `setChecked` and the first flag change.
   (R3-M6, R4-M8)
3. `pinButtonClicked`: the **first complete statement** of the body — the first element of
   `selectionStatements( body )`, which keeps control flow — is the full statement
   `focusLossCloser->setPinned( checked );` after whitespace normalisation. Moving it
   below the whole `if ( checked )` statement but above `show()` still fails.
4. `engagePopup`, inside the `if ( !isVisible() )` branch only:
   - the full condition text `if ( giveFocus || ( !ui.pinButton->isChecked() &&
     PopupDismiss::needsExplicitActivation( windowFlags() ) ) )` is present;
   - its consequent contains `activateWindow();` then `raise();` in that order;
   - `show();` **precedes** that guarded block;
   - the function contains exactly **three** `activateWindow()` calls, attributed one
     each to the hidden-window branch, the pinned-visible branch and the `xcb` `Tool`
     branch; a fourth unconditional call fails the count and deleting a preserved call
     fails the attribution;
   - the pre-existing `giveFocus = true;` assignment and its `xcb`/unpinned guard are
     still present in the `#ifdef ENABLE_SPWF_CUSTOMIZATION` region.
5. Unpin branch: after the shared `show()`, the activation request is guarded by
   `!checked && PopupDismiss::needsExplicitActivation( windowFlags() )`.
6. `goldendict.pro`: `popupdismiss.hh` is in `HEADERS` and `popupdismiss.cc` in
   `SOURCES`.
7. `tests/tests.pro`: the required-suite list, the discovered `tests/tst_*/tst_*.pro`
   basenames and the `!count(...)` literal agree, and include `tst_popup_dismiss`.

### 8.4 What is not proven here

The compositor's decision, and the genuine application-inactive transition (which needs a
second process on this host). No fixture claims either: the application-inactive branch is
covered by the pure rule matrix and by the identical code path the foreign-window fixture
drives, and `docs/wayland-limitations.md` records the platform limit.

### 8.5 Acceptance commands

`tests/tests.pro` does not choose a QPA platform: its inventory rules are what it
enforces, and its documented `xcb` line is a comment. The acceptance run states the
platform explicitly, and a skipped runtime fixture does not satisfy it:

```bash
qmake6 goldendict.pro -o Makefile && make -j"$(nproc)"          # application links (D7)
qmake6 tests/tests.pro -o tests/Makefile
make -C tests -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check
```

## 9. Documentation changes

`docs/wayland-limitations.md`:

* §2's limit list gains the popup rule: the scan popup asks for activation when it opens
  and closes when focus leaves it — including when one of its own dialogs takes the
  keyboard, as on X11; a refused activation leaves it open until it is clicked, after
  which Esc and click-away work.
* §3 keeps its "activation is best-effort" paragraph and adds that a refused request
  means the popup is dismissed manually, which is the documented consequence rather than
  a regression.

No README change: its Wayland summary does not claim the popup dismisses itself.

## 10. Risks

| risk | mitigation |
| --- | --- |
| a clipboard-triggered popup takes the keyboard from what the user is typing in | accepted by the operator as the price of X11 parity (D1) |
| a compositor refuses focus, so nothing changes for the user | documented (§9); clicking the popup makes it dismissable |
| the popup closes when a menu of its own opens | that is what `OwnPopup` prevents, asserted at rule and runtime level (§8.2(3)) |
| the popup survives after its menu closes while focus went elsewhere | decided by the next focus event, with no timer (§5.3) |
| the popup closes when a dialog it opened takes focus | intended parity (D8), and the only dialog `closeMenu()` could reject is gated off in the popup (§5.3) |
| a handler that destroys the closer | `QPointer` guard plus context-bound connections, fixtured (§8.2(9,10)) |
| the handler is re-entered by its own synchronous effects | `dismissing`, fixtured (§8.2(8)) |
| a popup that was never activated stays | intended (§5.5), and the reason the rule needs `armed` |
| a regression on X11 | the default form is still `Qt::Popup` (`tst_platformwindow` pins the flags) and the preserved `giveFocus` assignment is asserted |
| a new module that the application never links | `goldendict.pro` entries plus the application build in acceptance (D7) |

## 11. Out of scope

Hover translation, the scan flag, the clipboard-watch rules, alt-mode, the tray, the
pinned popup's flags and stay-on-top option, main-window focus restoration, the
pre-existing pointer-proximity dismissal paths, and any attempt to emulate an X11 grab on
Wayland.

## 12. Implementation-order note

Not a plan, but the sequence the later plan must respect: the module and the rule first
(provable alone), then the closer's event model, then the `ScanPopup` wiring, then the
project-file and inventory integration, then the documentation.

## 13. Review ledger

### Round 1 → 2

| finding | severity | change |
| --- | --- | --- |
| suppressed deactivation strands the popup | major | superseded: there is no suppression state any more (§5.2, D9) |
| `setPinned` after `setWindowFlags` | major | §6.3(3) first statement; §8.3(3) |
| unpin while visible cannot hide | major | §5.4 + §6.3(4); §8.3(5) |
| `goldendict.pro` registration missing | major | D7, §6.3(6), §8.3(6), §8.5 |
| modal fixture cannot activate a sibling | major | replaced by the dialog focus fixture §8.2(4) |
| source assertions too weak | major | §8.3, extended in R2–R4 |
| activation policy contradicts preserved `xcb` branches | major | §2 facts, §6.4 table, D5/§4 |
| no runtime proof of the menu guard | major | §8.2(3) |
| pinned-open and Windows/macOS descriptions | minor | §5.1, §6.4 |
| `tests.pro` does not select `xcb` | minor | §8.5 |
| declined-activation fallback overstated | minor | §7 last row, §5.5, §9 |
| `xcb` `Tool` rationale wrong | minor | §6.4 closing paragraph, §4, §10 |
| hover/scan-flag called dismissal paths | minor | §2 rewritten |
| "reordered conjunction" claim | minor | §8.1 |

### Round 2 → 3, round 3 → 4 (summary)

Both rounds refined the settlement model that revision 5 removes. Their durable results
survive in this revision: the constructor and pin-ordering assertions with the restored
argument (§8.3(2,3)), the preserved `xcb` `giveFocus` assignment assertion (§8.3(4)), the
three attributed activation calls, the full first-statement comparison, the guarded
dismissal path, the "no modal-stack test" rule, the preconditions-and-zero-early-dismissal
requirement, and the acceptance commands.

### Round 4 → 5

| finding | severity | change |
| --- | --- | --- |
| `isActiveWindow()` reports a shared-activation `Qt::Tool` as active | **blocker** | D3: activation state is gone; the model compares `QGuiApplication::focusWindow()` identity (and `activePopupWidget()` for menus), §6.2 |
| only the recorded suppressor is watched, so a successor menu can strand the popup | major | no suppressor exists: every focus event is evaluated, and any owned popup is `OwnPopup` (§6.2) |
| existing pointer paths can hide the popup while a menu is open | major | stated and scoped: the new rule adds no such behaviour and removes none (§4, §11) |
| a coarse timer can fire early or late | major | no timer (D9) |
| fixtures cannot establish the inactive state | major | they assert `focusWindow()` identity at every step; the application-inactive branch is declared unproven (§8.2, §8.4) |
| restart fixture cannot show `menuB` was adopted | major | no suppressor to adopt; fixture 3/5 drive the real transitions |
| constructor assertion omits creation and handler | major | §8.3(2) full ordered expectation |
| statement helpers do not parse preprocessor directives | major | §8.3 preamble: directive stripping, plus two independent searches for the preserved assignment |
| dialog claim ignores Qt's real close mechanics | major | §5.3 states the mechanism and cites `articleview.cc:1144` for why `closeMenu()` cannot cancel that dialog |
| `hideWindow()` is not idempotent and other callers bypass the guard | major | `dismiss()` calls it at most once per decision and documents the pre-existing paths as unchanged (§6.1, §7) |
| a callback that destroys the closer | major | context-bound connections, `QPointer` guard, documented at the API boundary, fixtured (§8.2(9,10)) |
| `isOwnedBy` accepts `owner == candidate` | minor | `isOwnedPopup()` excludes the owner (§6.1, §8.1) |
| `Qt::Popup` claimed for every `xcb` form | minor | §2 qualified by stored form |
| `NoHandle` stated unconditionally | minor | §2 now says "only when no handle exists yet" |
| goal 4 contradicts the refusal case | minor | §3(4) qualified to "after it has been activated" |
