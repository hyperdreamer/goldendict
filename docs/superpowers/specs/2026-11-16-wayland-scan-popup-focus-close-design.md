# Wayland scan popup: close when it loses focus

Status: design revision 1, for review. Run `pm-run-20261116-123313-27afee37`,
topic `wayland-scan-popup-focus-close`.

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
| detection (D3) | **window deactivation events**, not pointer polling | The existing proximity polling cannot work on Wayland: `QCursor::pos()` is not the global pointer there, and a client has no pointer grab (`scanpopup.cc:1205`). Deactivation is the event the compositor actually delivers. |
| placement (D4) | new module `popupdismiss.{hh,cc}` | `ScanPopup` is unlinkable in a test; a pure rule plus a small watchable `QObject` keeps the decision and the event plumbing linkable, in the same style as `platformwindow.{hh,cc}`. |
| re-focus on later words (D5) | **no**: focus is requested once, when the popup opens | A clipboard change arrives while the user may be typing elsewhere; re-requesting focus on every later word would fight the user. Requesting on open is enough because a popup that is visible and unfocused has already either been declined (asking again cannot help) or is about to be hidden. |

## 2. Background

On X11 the unpinned popup is created with `Qt::Popup`
(`platformwindow.cc:38`, `defaultX11Flags` — the preserved default of the code this
replaced). A `Qt::Popup` window is given the keyboard by Qt and is closed by Qt
itself as soon as it loses the popup grab: click any other window and the popup is gone.

On Wayland the same call site returns something else on purpose:
`PlatformWindow::popupFlags()` returns `Qt::Tool | Qt::FramelessWindowHint |
Qt::WindowStaysOnTopHint` for every Wayland platform, unconditionally
(`platformwindow.cc:32-33, 118-119`). That is an ordinary window: clicking elsewhere
moves the keyboard focus and the popup stays where it is. The remaining dismissal paths
are:

* **Esc**, via `escapePressed()` (`scanpopup.cc:1170-1174`);
* **pointer proximity**: `interceptMouse()` starts `mouseGrabPollTimer`, which polls
  `QCursor::pos()` every 10 ms (`scanpopup.cc:299, 1188-1216`) and hides the popup once
  the pointer is far away (`scanpopup.cc:910-948`). On Wayland a client only learns the pointer
  position while the pointer is over its own surfaces, so this polling sees the last
  position it knew and the popup does not go away;
* **hover translation** and the scan flag, both X11-only
  (`scanpopup.cc:514-517, 543, 572-579`).

Focus today:

* `engagePopup( bool forcePopup, bool giveFocus = false )` (`scanpopup.hh:169`), so on
  Linux the popup is shown **without** asking for activation
  (`scanpopup.cc:496-504`: `engagePopup( false, false )` on Linux, `true` only under
  `Q_OS_WIN`).
* The only activation request on the show path is inside
  `#ifdef ENABLE_SPWF_CUSTOMIZATION` and only on `xcb`
  (`scanpopup.cc:665-671`), plus an xcb-only re-raise for `Qt::Tool` windows
  (`scanpopup.cc:697-705`).
* A popup opened by a **global hotkey** already asks for activation using the portal's
  activation token: `mainwindow.cc:3325-3336` calls
  `PlatformWindow::applyActivation( scanPopup.get(), token )` before delivering the
  word. That is the path that works today; the clipboard-change path is the one that
  produces a popup nobody can dismiss with the mouse.
* `docs/wayland-limitations.md` §3 states the platform truth this design has to accept:
  "Activation is best-effort: an activation request is a hint that a compositor may
  honour or decline … nothing in this migration promises that a window becomes focused".

Consequence, and the reason this design has two halves: for a clipboard-triggered popup
on Wayland the current code never has focus, so a deactivation-based close would be dead
code for exactly the case the reporter hit.

## 3. Goals

1. An unpinned scan popup on Wayland behaves like the X11 one: it takes the keyboard
   when it opens, and the moment that keyboard focus goes anywhere else it disappears
   immediately, using the same hide path as Esc.
2. A pinned popup never auto-hides, on any platform.
3. X11 does not regress: with the default stored window form the popup is still a
   `Qt::Popup` and Qt still owns its dismissal.
4. Nothing in the popup's other dismissals, geometry, content, or the main window
   changes.
5. The parts that can be proven on this host are proven by execution; the part that
   needs a live compositor is documented as a limit, not claimed as a result.

## 4. Non-goals

* No new preference, no new control, no changed default. (D2)
* No emulation of an X11 pointer/keyboard grab. Wayland has no such mechanism for a
  normal client, and the portal's global-shortcut and activation tokens are the only
  sanctioned inputs.
* No change to hover translation, the scan flag, the clipboard-watching rules, the
  alt-mode chord, or the tray behaviour.
* No change to the pinned popup's window flags or its stay-on-top option.
* No change to the main window or to the article view.
* Windows and macOS keep their current behaviour. On Windows the `giveFocus = true`
  path in `translateWord()` (`scanpopup.cc:490-496`) still asks for activation, and a
  stored non-popup window form there now also gets the explicit request — the same rule,
  applied uniformly.

## 5. Behaviour

### 5.1 Opening

When an **unpinned** popup transitions from hidden to visible and its window flags
describe a window that is **not** a platform popup (`Qt::Popup`), the popup requests
activation once: `activateWindow()` followed by `raise()`, exactly the pair the
`giveFocus` and `xcb + Tool` paths use today. A `Qt::Popup` window is skipped because
Qt already gives it the keyboard.

When the popup is already visible, a new word updates the content and requests nothing.
(D5)

A pinned popup keeps its current behaviour: show, activate, raise.

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

### 5.3 When nothing happens

* A deactivation for a popup that was never activated is ignored. This is what makes the
  compositor-refusal case honest: no focus was granted, so there is nothing to lose and
  the popup stays until Esc.
* A deactivation caused by a menu, combo dropdown or modal dialog that belongs to the
  popup is ignored, because hiding there would close the popup while its own menu is
  still on screen.
* A deactivation while already hidden is ignored.

## 6. Architecture

### 6.1 New module: `popupdismiss.hh` / `popupdismiss.cc`

```cpp
namespace PopupDismiss {

/// What one deactivation observation means for the popup.
enum class Action { Ignore, Hide };

/// Everything the rule may read. No widget, no application object, no globals.
struct Context {
  bool visible = false;
  bool pinned = false;
  bool focusedSinceShown = false;
  bool ownPopupOpen = false;   ///< one of the popup's own popup widgets is open
  bool ownModalOpen = false;   ///< one of the popup's own modal dialogs is up
};

/// The rule of §5.2/§5.3: Hide iff
/// visible && !pinned && focusedSinceShown && !ownPopupOpen && !ownModalOpen.
Action onWindowDeactivate( Context const & context );

/// True when the window does not already receive the keyboard from the platform:
/// every window type except Qt::Popup. Window hints never change the answer.
bool needsExplicitActivation( Qt::WindowFlags flags );

/// True when `candidate` is `owner` or is a descendant of it. Null-safe; used to
/// decide whether an active popup widget or modal dialog belongs to this popup.
bool isOwnedBy( QWidget const * owner, QWidget const * candidate );

/// Watches one top-level window and hides it when the rule says so.
///
/// State it owns: pinned, focusedSinceShown. It never decides anything else and
/// never touches ScanPopup: the caller supplies the dismiss action.
class FocusLossCloser : public QObject {
  Q_OBJECT
public:
  explicit FocusLossCloser( QWidget * window, QObject * parent = nullptr );

  void setPinned( bool pinned );
  bool isPinned() const;
  /// The action to run when the rule returns Hide. Empty means "observe only".
  void setDismissHandler( std::function< void() > handler );

protected:
  bool eventFilter( QObject * watched, QEvent * event ) override;

private:
  QWidget * const window;            ///< observed top-level window, never null
  bool pinned = false;
  bool focusedSinceShown = false;
  std::function< void() > dismissHandler;
};

} // namespace PopupDismiss
```

A categorized log line is emitted on each decision
(`goldendict.popup.dismiss`, `qCDebug`), carrying the reason the rule returned
`Ignore`, so a live session can be diagnosed without a debugger. It never logs window
titles or looked-up words.

### 6.2 `FocusLossCloser` transition table

Installed as an event filter on `window`; it never consumes an event (always returns
`false`), so the popup's own handling is unchanged.

| Event | Previous state | Effect |
| --- | --- | --- |
| `QEvent::Show` | any | `focusedSinceShown = window->isActiveWindow()` |
| `QEvent::Hide` | any | `focusedSinceShown = false` |
| `QEvent::WindowActivate` | any | `focusedSinceShown = true` |
| `QEvent::WindowDeactivate` | rule → `Ignore` | nothing |
| `QEvent::WindowDeactivate` | rule → `Hide` | `focusedSinceShown = false`, then call `dismissHandler` (which hides the window; the resulting `Hide` re-sets the flag) |
| other events | any | nothing |

`focusedSinceShown` is set from `isActiveWindow()` on `Show` because a re-shown window
can already be the active one when no activation event follows. Resetting it on `Hide`
is what stops a later deactivation of a *different* showing from closing the popup.

The guards are computed at decision time, from the live application state:

```cpp
QWidget * const popup = QApplication::activePopupWidget();
QWidget * const modal = QApplication::activeModalWidget();
Context const context{ window->isVisible(), pinned, focusedSinceShown,
                       isOwnedBy( window, popup ), isOwnedBy( window, modal ) };
```

A modal dialog or menu opened by the *main* window does not belong to the popup, so it
does not suppress the hide — which matches X11, where a `Qt::Popup` closes when any
other window takes the keyboard.

### 6.3 Wiring in `ScanPopup`

1. `scanpopup.hh`: `#include "popupdismiss.hh"`, member
   `std::unique_ptr< PopupDismiss::FocusLossCloser > focusLossCloser;`.
2. Constructor: create the closer on `this` immediately after `ui.setupUi( this )`,
   install the dismiss handler `[ this ] { hideWindow(); }`, and call
   `setPinned( ui.pinButton->isChecked() )` immediately after the existing
   pin/on-top restoration block (`scanpopup.cc:184-200`) so the initial pinned state is
   the stored one.
3. `pinButtonClicked( bool checked )` (`scanpopup.cc:1077-1105`): call
   `focusLossCloser->setPinned( checked )` in both branches, before `show()`.
4. `engagePopup()` (`scanpopup.cc:605-717`): in the `if ( !isVisible() )` branch, after
   `show();`, replace `if ( giveFocus )` with

   ```cpp
   if ( giveFocus || ( !ui.pinButton->isChecked() &&
                       PopupDismiss::needsExplicitActivation( windowFlags() ) ) )
   {
     activateWindow();
     raise();
   }
   ```

   Nothing else in that function changes; the `#ifdef ENABLE_SPWF_CUSTOMIZATION` block
   and the `else if` branches keep their current conditions.

No `ScanPopup::event()` override is added: the closer owns the event hook, which is what
makes it testable without linking the popup.

### 6.4 Behaviour by platform and stored window form

| platform | stored form | flags | explicit request | dismissal |
| --- | --- | --- | --- | --- |
| Wayland | any | `Tool \| Frameless \| StaysOnTop` | **yes**, on open | our rule |
| xcb | default | `Qt::Popup` | no | Qt (unchanged) |
| xcb | `Popup` | `Qt::Popup` | no | Qt (unchanged) |
| xcb | `Tool` | `Tool \| Frameless \| StaysOnTop` | **yes**, on open | our rule |
| Windows / macOS | default | `Qt::Popup` | Windows: `giveFocus` as today | Qt (unchanged) |

The xcb `Tool` row is a deliberate correction: those sessions had a managed window with
no dismissal by mouse, and the customized build only papered over it with a re-raise.

## 7. Error handling and edge cases

| situation | behaviour |
| --- | --- |
| `activePopupWidget()` / `activeModalWidget()` returns null | guard false; the rule decides on the remaining fields |
| closer constructed before the window has a native handle | no handle is needed: only widget events and `isActiveWindow()` are read |
| `Show` while already active | `focusedSinceShown = true`; a later deactivation hides |
| `Show` while not active (compositor declined, or shown behind) | `focusedSinceShown = false`; a deactivation is ignored until an activation arrives |
| pin toggled while visible | closer follows the button; a pinned popup ignores deactivation from then on |
| unpin while visible | deactivation closes it from then on, without asking for focus again |
| deactivation during `hideWindow()` | `isVisible()` is already false or the resulting `Hide` resets the flag; the handler is idempotent |
| window destroyed while the closer lives | the closer is parented to the window and the window owns it; no dangling call |
| pointer-poll dismissal racing the deactivation | both call `hideWindow()`; it is idempotent |
| a popup opened while another GoldenDict window holds focus | the request is still made; the compositor decides |

## 8. Test strategy

New suite `tests/tst_popup_dismiss/`, the 21st required suite, linking the production
`popupdismiss.cc` — never a copy of it.

### 8.1 Rule matrix (no display required beyond construction)

`onWindowDeactivate` is data-driven over all five fields: at least twelve rows covering
`Hide` (visible, unpinned, focused, no own popup, no own modal) and each single
`Ignore` cause — hidden, pinned, never focused, own popup open, own modal open — plus
two-combination rows (pinned + focused, hidden + focused) so a wrongly reordered
conjunction is red.

`needsExplicitActivation` is data-driven: `Qt::Popup` → false; `Qt::Popup` with
`FramelessWindowHint`/`StaysOnTop` hints → still false; `Qt::Tool`,
`Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint`, `Qt::Window`,
`Qt::Dialog`, `Qt::Tool | X11BypassWindowManagerHint` → true.

`isOwnedBy`: null/null, self/self, a child popup widget, a grandchild, a sibling, an
unrelated top-level, and a dialog parented to a different window.

### 8.2 Runtime behaviour of the production `FocusLossCloser`

On the gate's `xcb` + `xvfb` display, with two real top-level widgets:

1. **hides on a real deactivation** — closer on `first` with a counting handler that
   hides it; `first` shown and activated (`QTest::qWaitForWindowActive`), then `second`
   shown and activated; `QTRY_COMPARE` the handler ran once and `first` is hidden.
   This is the fixture that proves the production event hook and the real platform
   event, not a pure function.
2. **pinned does not hide** — same drive with `setPinned( true )`; the handler never
   runs and the window stays visible.
3. **own modal suppresses the hide** — a modal `QDialog` parented to `first` is shown,
   `second` is activated; the handler does not run. Then the dialog is closed and a
   fresh activation cycle hides the popup, proving the guard is not sticky.
4. **a deactivation before any activation is ignored** — `first` shown while `second`
   is active, a synthetic `QEvent::WindowDeactivate` delivered to `first`; no hide.
5. **re-show resets the state** — after a hide, `first` is shown again without
   activation; a synthetic deactivation is ignored.

The synthetic deliveries use `QCoreApplication::sendEvent`, so the same production
filter receives them; they are labelled as synthetic in the fixture names.

The runtime cases assert a display prerequisite with a reason instead of skipping
silently, following `tst_platformwindow`. They are `xcb`-only: on another QPA
(`offscreen`) activation is not the platform this ships on, and the fixtures `QSKIP`
with that reason. The gate in `tests/tests.pro` always runs `xcb`.

### 8.3 Source assertions (wiring that the suite cannot link)

`ScanPopup` cannot be linked into a suite, so the wiring is pinned by reading the
sources, in the style already used in `tst_hotkeys` (`selectionSourceText`,
`selectionBlock`, `selectionOrdered`):

1. `scanpopup.hh` declares the `PopupDismiss::FocusLossCloser` member and includes the
   header.
2. The constructor creates the closer on `this`, installs the `hideWindow()` handler,
   and calls `setPinned` with the pin button state.
3. `pinButtonClicked` calls `setPinned` in both branches.
4. In the `engagePopup` block, `needsExplicitActivation( windowFlags() )` appears, and
   `show();` precedes it; `activateWindow()` and `raise()` follow it.
5. `ScanPopup` gains no `event()`/`changeEvent()` override, so the closer stays the
   single hook.

### 8.4 Suite inventory

`tests/tests.pro` is a closed inventory: the discovery list must equal the sorted
`GD_REQUIRED_SUITE_NAMES`, and the `!count(...)` literal must equal its length
(`tests/tests.pro:123-145`). The change adds `tst_popup_dismiss` to the list, moves the
count from 20 to 21, and updates both message strings. A new source assertion in the
suite parses `tests/tests.pro` and checks that the list, the discovered
`tests/tst_*/tst_*.pro` basenames and the count literal all agree, so a later suite
addition cannot silently pass the gate.

### 8.5 What is not proven here

The compositor's decision. No fixture can force KWin to grant or refuse activation, and
none is written that pretends to. `docs/wayland-limitations.md` records the limit
instead.

## 9. Documentation changes

`docs/wayland-limitations.md`:

* §2's closing list gains the popup rule: the scan popup asks for activation when it
  opens and closes when it loses focus; a refused activation leaves it open until Esc.
* §3 keeps its "activation is best-effort" paragraph and adds that a refused request
  means the popup is dismissed manually, which is the documented consequence rather than
  a regression.

No README change: its Wayland summary does not claim the popup dismisses itself.

## 10. Risks

| risk | mitigation |
| --- | --- |
| a clipboard-triggered popup takes the keyboard from what the user is typing in | accepted by the operator as the price of X11 parity (D1); it is what `Qt::Popup` does on xcb today |
| a compositor refuses focus, so nothing changes for the user | documented (§9); the popup still closes on Esc, and the reason is logged |
| hiding while a menu is open | `ownPopupOpen` guard, tested both as a rule and at runtime |
| a deactivation delivered during the open transition closes the popup immediately | the rule requires `focusedSinceShown`, which only an activation sets |
| the closer swallowing an event | it always returns `false` |
| a regression on X11 | the xcb default form still yields `Qt::Popup`, already pinned by `tst_platformwindow`; the new suite adds no xcb path that changes it |

## 11. Out of scope

Hover translation, the scan flag, the clipboard-watch rules, alt-mode, the tray, the
pinned popup's flags and stay-on-top option, main-window focus restoration, and any
attempt to emulate an X11 grab on Wayland.
