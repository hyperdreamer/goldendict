# Wayland scan popup: selected-text shortcut

Status: design revision 2, for review. Run `pm-run-20261104-005240-d7e4068b`, topic `scan-popup`.
Revision 1 was returned with 27 findings (3 blockers); this revision addresses every one and names
them inline as `(F1)` … `(F27)` so the review can be checked item by item.

## 1. Decision record

| decision | choice | reason |
| --- | --- | --- |
| trigger on Wayland | **shortcut only** | The reporter wants X11's *checked* mode — no popup without keys — and accepts a discrete shortcut in place of a held chord. |
| selection source | **own client for `ext_data_control_manager_v1`** | The protocol KWin 6.8 actually advertises; no helper process and no new service dependency, only one optional shared-library link (`libwayland-client`, already present in any Wayland session). |
| UI placement | **(a)** third row in the existing Hotkeys group | One list of shortcuts, one status block, reuses the existing per-shortcut status line. |

`HotkeySyntax::preferredTrigger()` rejects a modifier-only binding (`hotkeysyntax.cc:205`, "The hotkey
has no key, only modifiers."), so a held `Ctrl+Win` chord can never become a portal shortcut.

**Default shortcut (F1).** `QKeySequence( "Ctrl+Win+D" )` is not a valid sequence: Qt 6.12 parses
`Win` to `Qt::Key_unknown` (measured: `count=1, key=33554431, modifiers=0x0`, both string forms
empty), `Config::HotKey` stores `key1 = Key_unknown`, and `preferredTrigger()` then reports
"only modifiers". The default is therefore built from the key-combination form:

```cpp
Config::HotKey( QKeySequence( Qt::CTRL | Qt::META | Qt::Key_D ) )   // meta+ctrl+D
```

which measures `key=68, modifiers=0x14000000`, `PortableText = "Meta+Ctrl+D"`, and derives the portal
trigger `CTRL+LOGO+d` (`modifierNames` order is CTRL, ALT, SHIFT, LOGO). The `QKeySequence( key,
modifiers )` constructor must **not** be used: it produces a two-key sequence (`count=2`).

## 2. Background

The scan popup is the product's signature feature and it is X11-only today. On `xcb` a primary
selection change arms `delayTimer` (200 ms), `delayShow()` hands the text to `handleInputWord()`, the
modifier gate runs, and the popup rises.

On Wayland every one of those steps is absent:

* `ScanPopup::clipboardChanged()` opens with a Wayland-only guard (`scanpopup.cc:505`): unless
  clipboard watching is on and the mode is `Clipboard`, it returns. The `Selection` branch is
  additionally `xcb`-only (`scanpopup.cc:517`), and `delayShow()` is inside `#ifdef HAVE_X11`
  (`scanpopup.cc:496`).
* the only `emit hovered()` is inside `Q_OS_WIN32` (`mouseover.cc:362`), and `mouseHovered()` returns
  early on Wayland (`scanpopup.cc:532`).
* `docs/superpowers/specs/2026-10-24-qt6-migration-spec.md` §9 puts "Wayland InputCapture hover
  translation" out of scope, and `docs/wayland-limitations.md` §2 states the limit.

**Evidence.** Measured on this machine (`receipts/investigation-scanpopup.json` in the run root): with
the reporter's configuration the popup raises correctly on `xcb` — modifier held, alt-mode within 3 s,
and a clean negative control without modifiers. The Wayland half of that receipt is a **code-path
conclusion** derived from the branches cited above, not a live Wayland observation; no Wayland
session was available on this host. (F19)

## 3. Goals

1. A global shortcut on Wayland that opens the scan popup with the text most recently selected in any
   other application.
2. Zero change to X11 runtime behaviour — same code path, no new binding, and no `ScanPopup` object
   created that X11 would not have created before. The Hotkeys tab gains one disabled row (§7.4).
3. Honest degradation: when the compositor cannot serve the primary selection, the status line says
   so, the shortcut falls back to the clipboard, and an empty result is reported where the user can
   see it.

## 4. Non-goals

* `zwlr_data_control_v1` (wlroots: sway, hyprland, labwc) and GNOME/Mutter. Recorded as deferred
  coverage in §11.
* Popping up automatically when a selection appears. Rejected at the design gate.
* Emulating a held-modifier chord. Impossible on Wayland; the portal cannot carry it.
* Any change to clipboard watching, the clipboard hotkey, or the X11 modifier gate.

## 5. Behaviour contract

1. The shortcut is registered **only** when
   `scanPopupBindingPlanned( buildSupportsSelection, platformName, preferences.enableScanPopupHotkey )`
   is true — a real function (§6.6), not an inline condition, so it can be tested. It is false on `xcb`, so the RECORD backend never sees the id.
2. Activating it opens the scan popup with `lastText()` of the selection source. If that is empty, the
   clipboard text is used. If both are empty, no window opens and `MainWindow`'s status bar reports
   `No selected text or clipboard text to translate` for 3 s (§6.7).
3. `lastText()` is sticky: it survives the selection being unset, so the user may select, click
   elsewhere, then press the shortcut. It is replaced only by a newer successful read for a newer
   offer.
4. The phrase is raised through the existing `translateWord()` path, so sanitising, input-length
   limits, history and group selection are unchanged. Two behaviours are **not** inherited silently:
   * the same-phrase dedupe lives in `handleInputWord()` (`scanpopup.cc:540`), not in
     `translateWord()`, so the new entry point performs the same check itself before translating
     (F16);
   * `scanToMainWindow` is honoured: when it is set, `engagePopup()` sends the phrase to the main
     window instead of showing the popup (`scanpopup.cc:596`). The shortcut keeps that behaviour and
     the contract is "open the popup **unless** 'send to main window' is enabled" (F25).
5. The shortcut exists on Wayland only. On `xcb` the preferences row is disabled and the stored values
   are preserved unchanged (§7.4).

## 6. Architecture

### 6.0 Vendored protocol code

`protocols/ext-data-control-v1/` holds the staging XML copied from wayland-protocols 1.50, the two
generated files and a `regenerate.sh` that reproduces them:

```sh
wayland-scanner client-header ext-data-control-v1.xml ext-data-control-v1-client-protocol.h
wayland-scanner private-code  ext-data-control-v1.xml ext-data-control-v1-protocol.c
```

The generated files are committed, so a build needs no `wayland-scanner`; only regeneration does.
`goldendict.pro` adds the generated `.c` to `SOURCES` and the directory to `INCLUDEPATH` when
`PKGCONFIG` can resolve `wayland-client`, and defines `HAVE_WAYLAND_DATA_CONTROL` for that case only.
The resulting binary links `libwayland-client`; nothing else is added.

### 6.1 Units

| unit | responsibility | depends on |
| --- | --- | --- |
| `waylandselectionlogic.hh/.cc` (new) | pure decisions: MIME preference, payload clamp, UTF-8 decode, NUL stripping, text choice, offer/read state machine | Qt core only |
| `waylandselection.hh/.cc` (new) | `wl_display` connection, registry bind, seat, device, offers, pipe reads, availability | `libwayland-client`, generated protocol code, the logic unit |
| `protocols/ext-data-control-v1/` (new) | vendored XML, generated header and code, `regenerate.sh` | `wayland-scanner` (regeneration only) |
| `config.hh/.cc` | `enableScanPopupHotkey`, `scanPopupHotkey`, third portal id in `isKnownPortalBindingId()` | — |
| `hotkeybackend.hh` | `scanPopupBindingPlanned()` predicate and the extended id contract comment | — |
| `portalhotkeys.cc` | third id in `knownBindingId()` | — |
| `mainwindow.cc` | binding, activation dispatch, watcher ownership and teardown | existing backend, `ScanPopup` |
| `scanpopup.cc/.hh` | `translateSelectedText( QString const & ) -> bool` | existing `translateWord()` |
| `preferences.cc/.ui` | third hotkey row, its status and the selection-source state | existing `setHotkeyStatus()` |

The split between `waylandselectionlogic` and `waylandselection` mirrors `RecorderMatcher` /
`RecorderCore` / `X11RecordSession`: the rules are proved without a server, the session work needs the
real API.

### 6.2 Selection source API (F14)

```cpp
class WaylandSelectionSource : public QObject
{
  Q_OBJECT
public:
  enum class State
  {
    Unsupported,   ///< no HAVE_WAYLAND_DATA_CONTROL, or a non-Wayland platform: terminal
    Starting,      ///< start() called, registry handshake not complete
    Available,     ///< manager bound and a seat bound
    Unavailable    ///< terminal failure; reason() explains it
  };

  explicit WaylandSelectionSource( QObject * parent = nullptr );
  ~WaylandSelectionSource() override;

  State state() const;              ///< valid at any time, including before start()
  QString reason() const;           ///< non-empty iff state() == Unavailable
  bool primarySelectionSeen() const;///< true once any primary_selection event arrived
  QString lastText() const;         ///< most recent payload; sticky (5.3)

public slots:
  void start();   ///< idempotent; Unsupported is a no-op that sets State::Unsupported
  void stop();    ///< idempotent; releases device, offers, seat, manager, connection

signals:
  void stateChanged( State state, QString const & reason );
  void textCaptured( QString const & text );   ///< for logging and tests only
};
```

State transitions, all of them terminal or absorbing:

```
Unsupported --start()--> Unsupported                (no connection is opened)
Starting    --manager+seat bound--> Available
Starting    --connect/bind failure--> Unavailable
Available   --display error | manager removed | device finished--> Unavailable
```

`primarySelectionSeen` is the capability flag for the *primary selection itself*: the protocol sends
the first `primary_selection` event (possibly null) upon binding when the compositor supports it
(ext-data-control-v1.xml:154-156). A compositor that binds the manager but never supports primary
selection therefore reports `Available` with `primarySelectionSeen == false`, and the status row says
so instead of claiming readability (F14c). The transition is logged once.

### 6.3 Pure helpers and state (`waylandselectionlogic`)

```cpp
namespace WaylandSelection {
  /// Preference order, first match wins:
  ///   text/plain;charset=utf-8, text/plain;charset=UTF-8, UTF8_STRING, text/plain
  /// Empty when the offer carries no usable text MIME type.
  QString chooseMimeType( QStringList const & offered );

  /// Upper bound applied while reading, not after: 1 MiB.
  static constexpr int MaxPayloadBytes = 1024 * 1024;

  /// UTF-8 decode with replacement of invalid sequences; U+0000 is removed,
  /// because the phrase sanitiser does not remove it (F15).
  QString decodePayload( QByteArray const & raw );

  /// The selection wins when it is non-empty after trimming; otherwise the clipboard.
  QString chooseText( QString const & selection, QString const & clipboard );

  /// Identity of one capture attempt. Every read is attributed to the token that
  /// started it; a completion for a retired token changes nothing (F4b).
  using ReadToken = quint64;

  /// Two channels share one `data_offer` prefix in the protocol (F4d). The state
  /// keeps the pending offer separately from the accepted primary offer.
  class OfferState
  {
  public:
    void pendingOffer( quint64 offerId );          ///< data_offer
    ChannelOffer takeForPrimary();                 ///< primary_selection: accept
    ChannelOffer discardForClipboard();            ///< selection: destroy, never read
    ReadToken beginRead();                         ///< reserve a token for the accepted offer
    bool accepts( ReadToken token ) const;         ///< false for retired tokens
    void retire();                                 ///< supersede: no token is accepted any more
    void completed( ReadToken token );             ///< token consumed, state settles
  };
}
```

### 6.4 Protocol client rules

* One **private** `wl_display` connection; Qt's Wayland connection is never touched.
* Bind `ext_data_control_manager_v1` version 1 and the `wl_seat` globals through the registry. Seat
  policy: bind the first seat reported; if more than one exists, log and use the first; if no seat is
  bound when the handshake ends, stay `Starting` and bind the first seat that appears later
  (`global_remove`/`global` are both handled) (F12). The multi-seat limitation is documented: primary
  selection is per-seat and only the first seat is watched.
* `data_offer` creates a pending offer. The immediately following channel event decides it:
  `primary_selection` accepts it, `selection` (the ordinary clipboard) is destroyed immediately and
  never read — the clipboard stays Qt's (F4d).
* A new `primary_selection` retires the previous offer: cancel its read (close the read end and drop
  the notifier), then `ext_data_control_offer_v1.destroy()`. A retired proxy is never marshalled
  again — libwayland aborts on `wl_proxy_marshal_array_flags` for a destroyed proxy (F4a, F4c).
* **Debounce (F13).** A `primary_selection` event does not read immediately. The accepted offer is
  held and read 200 ms after the last selection event, mirroring X11's `delayTimer`. A newer event
  replaces the held offer, so a drag-selection burst produces one transfer, not one per pixel.
* **Reading (F6).** `receive( mimeType, writeFd )` into a pipe; the read end is added to a
  `QSocketNotifier`; bytes are accumulated **until EOF or `MaxPayloadBytes`**, whichever comes first,
  then the read end is closed and the payload decoded. A single short `read()` is normal and is not a
  completion. `read() < 0 && errno == EAGAIN` means wait. Timeout: 250 ms from the last successful
  read; on timeout the payload is dropped, the cached text is unchanged, and the offer is destroyed.
* **Event loop (F5).** The display fd is driven by a read notifier with the complete branch table:
  1. `wl_display_prepare_read( display )`; if it returns `-1` and `errno == EAGAIN`,
     `wl_display_dispatch_pending( display )` and do not read;
  2. otherwise `wl_display_read_events( display )`; a `-1` here is a display error → stop;
  3. `wl_display_dispatch_pending( display )`;
  4. `wl_display_flush( display )`; if it returns `-1` and `errno == EAGAIN`, enable a writable
     notifier and flush again when it fires, then disable it;
  5. if the fd notifier fires while a different source is pending, `wl_display_cancel_read( display )`;
  6. any `-1` with `wl_display_get_error( display ) != 0` ends the watcher.
  There is no `wl_display_roundtrip()` anywhere: the startup handshake is the same non-blocking loop,
  and `Available` is published from the registry handler when the last global arrives (§8.1).
* **Self-selection (F26).** An offer is ignored while any GoldenDict widget holds the keyboard:
  `QApplication::activeWindow() != nullptr`. Selecting text inside GoldenDict's own article view is
  therefore never stored, and the contract "text selected in any other application" holds. The
  residual case — select inside GoldenDict, switch away, press the shortcut — falls back to the
  clipboard and is documented in §11.

### 6.5 Configuration

Two additive preferences, read and written beside the existing hotkey entries:

```xml
<enableScanPopupHotkey>1</enableScanPopupHotkey>
<scanPopupHotkey>Meta+Ctrl+D</scanPopupHotkey>
```

* the boolean is loaded with `applyBoolOption()` (`config.cc:459`), which leaves the default intact
  when the node is missing — unlike the neighbouring `enableScanPopup` line, which parses a missing
  node as false (F22);
* the hotkey is loaded with the existing `loadHotKey()` and stored with `hotKeyStorageText()`, so an
  untouched stored sequence keeps its verbatim text (`HotKey::rawText`, SD-15);
* defaults: `true` and the combination from §1. The stored PortableText of that default is
  `Meta+Ctrl+D`; the file never contains `Ctrl+Win+D` (F1).

### 6.6 Wiring

**Binding predicate (F18).** `hotkeybackend.hh` gains a pure inline predicate, used by
`installHotKeys()` and unit-tested directly:

```cpp
// buildSupportsSelection is HAVE_WAYLAND_DATA_CONTROL at the call site, so the
// compile-time capability is part of the tested decision, not an #ifdef secret.
bool scanPopupBindingPlanned( bool buildSupportsSelection,
                              QString const & platformName,
                              bool preferenceEnabled ) noexcept;
```

`MainWindow::installHotKeys()` adds:

```cpp
if( scanPopupBindingPlanned( GD_HAS_WAYLAND_DATA_CONTROL,
                             QGuiApplication::platformName(),
                             cfg.preferences.enableScanPopupHotkey ) )
  add( true, "scan-popup", tr( "Translate the selected text" ), cfg.preferences.scanPopupHotkey );
```

**Id allowlists (F2).** The id must be added in *both* hard-coded places, or the feature breaks the
existing shortcuts:

* `knownBindingId()` (`portalhotkeys.cc:84`) — otherwise `beginGeneration()` sets the whole backend to
  `Unavailable, "No valid portal bindings were requested"` and the main-window and clipboard
  shortcuts stop working on Wayland;
* `isKnownPortalBindingId()` (`config.cc:181`) — otherwise the stored `scan-popup` `PortalBindingDisplay`
  is silently dropped on load and the row shows "not assigned" after a restart;
* the contract comment in `hotkeybackend.hh:28-31` naming the stable ids.

**Activation (F9).** A freshly constructed `ScanPopup` has no native handle, and
`applyActivation()` returns `NoHandle` without installing the token when `windowHandle()` is null
(`platformwindow.cc:85`). The branch therefore materialises the handle first:

```cpp
else if( id == "scan-popup" && scanPopup )
{
  scanPopup->winId();                       // native handle exists before the token
  PlatformWindow::applyActivation( scanPopup.get(), token );
  if( !scanPopup->translateSelectedText( selectionSource ? selectionSource->lastText()
                                                         : QString() ) )
    mainStatusBar->showMessage( tr( "No selected text or clipboard text to translate" ), 3000 );
}
```

Focus remains **best-effort**: the token and `requestActivate` are hints a compositor may decline,
exactly as `docs/wayland-limitations.md` §3 states. The design does not promise focus.

**Popup creation guard (F11).** `enableScanPopupHotkey` defaults to true and is preserved on xcb, so a
naive third term would construct a `ScanPopup` on X11 that never existed before — and its destructor
would overwrite the stored popup geometry from a never-shown widget (`ScanPopup::saveConfigData()`,
`scanpopup.cc:337`). The guard is therefore Wayland-only:

```cpp
if( !cfg.preferences.enableScanPopup &&
    !cfg.preferences.enableClipboardHotkey &&
    !( scanPopupBindingPlanned( GD_HAS_WAYLAND_DATA_CONTROL,
                                QGuiApplication::platformName(),
                                cfg.preferences.enableScanPopupHotkey ) ) )
  return;
```

**Lifecycle (F27).** `MainWindow` owns the source as a `std::unique_ptr` member declared **before**
`scanPopup` and `articleProfile`, so it outlives both. `start()` is scheduled on the first event-loop
turn with `QTimer::singleShot( 0, this, ... )` guarded by `shuttingDown`. `stop()` is called at every
teardown site that already resets the popup:

* `~MainWindow` (`mainwindow.cc:1026-1031`),
* `quitApp()` (`mainwindow.cc:1272-1293`),
* `finishShutdown()` (`mainwindow.cc:1308-1315`),
* `failStartup()` (`mainwindow.cc:1317-1335`) — this path exits without a popup teardown today.

The dictionary-reset sites that recreate the popup (`mainwindow.cc:2143`, `2224`, `3713` and
`makeScanPopup()` at 1521) must **not** stop the source: the watcher survives popup recreation.

### 6.7 Popup entry point (F10, F16)

```cpp
bool ScanPopup::translateSelectedText( QString const & selectionText )
{
  QString const text = WaylandSelection::chooseText(
      selectionText, QApplication::clipboard()->text( QClipboard::Clipboard ) );
  Config::InputPhrase const phrase = cfg.preferences.sanitizeInputPhrase( text );
  if( !phrase.isValid() )
    return false;                       // caller reports it where the user can see it
  if( isVisible() && phrase == inputPhrase )
    return true;                        // same-phrase dedupe, as handleInputWord() does
  translateWord( text );
  return true;
}
```

The failure message is emitted by `MainWindow` into its own status bar, because the popup's
`showStatusBarMessage()` writes to the popup's status bar (`scanpopup.cc:1154`), which is invisible
when the popup never opens (F10).

## 7. User interface

### 7.1 Placement (a) — recorded choice

```text
Hotkeys
  [ ] Use the following hotkey to show or hide the main window:
                                          [ Alt+4        ]
  [ ] Use the following hotkey to translate a word from clipboard:
                                          [ Ctrl+C,C     ]
  [ ] Use the following hotkey to translate the selected text:
                                          [ Meta+Ctrl+D  ]

  The global shortcuts use the active desktop backend. See the capability
  and per-shortcut status below.

  Global shortcut status: Portal bound
    Main window shortcut   ...
    Clipboard shortcut     ...
    Selected text shortcut: <portal trigger> · <selection-source state>
  [ Configure shortcuts… ]
```

`Meta+Ctrl+D` is what `HotKeyEdit::renderCurrentValue()` actually renders for the default
(`QKeySequence(...).toString(NativeText)`, `hotkeyedit.cc:40-51`); `Win` never appears (F17).

### 7.2 Selection-source states (rendered after the shortcut status)

| source state | text appended to the row |
| --- | --- |
| `Available`, `primarySelectionSeen` | `selection readable` |
| `Available`, not yet seen | `selection not observed yet` |
| `Unavailable` | `selection unavailable: <reason> — clipboard is used instead` |
| `Unsupported` | `selection unsupported in this build — clipboard is used instead` |

`<reason>` is `WaylandSelectionSource::reason()`. The portal-reported trigger stays the first part of
the string, so the existing "reported trigger is display-only" rule is untouched.

### 7.3 Implementation of the row (F8)

`Preferences::setHotkeyStatus()` gains two parameters: `WaylandSelectionSource::State` and
`QString const & selectionReason` (defaulted, so existing callers compile). Its local `display` lambda
becomes id-driven instead of the current two-way ternary:

```cpp
struct Row { QString id; HotKeyEdit * editor; QCheckBox * enabled; QLabel * label;
             Config::HotKey const & stored; bool const & edited; };
Row const rows[] = {
  { "main-window",    ui.mainWindowHotkey,    ui.enableMainWindowHotkey,    ui.mainWindowHotkeyStatus,    cfg.preferences.mainWindowHotkey,    mainHotkeyEdited },
  { "clipboard-lookup", ui.clipboardHotkey,   ui.enableClipboardHotkey,   ui.clipboardHotkeyStatus,   cfg.preferences.clipboardHotkey,   clipboardHotkeyEdited },
  { "scan-popup",     ui.scanPopupHotkey,     ui.enableScanPopupHotkey,   ui.scanPopupHotkeyStatus,   cfg.preferences.scanPopupHotkey,   scanPopupHotkeyEdited },
};
```

A third `QLabel scanPopupHotkeyStatus` is added to `preferences.ui` beside the existing two, plus an
auto-connected `on_enableScanPopupHotkey_toggled( bool )` slot (the existing pair at
`preferences.cc:743-751` follow that naming, so the editor's enable state follows the checkbox).

### 7.4 X11 state for the third row (F17)

The row stays visible so a configuration synced between machines explains itself, but on xcb:

* the checkbox is shown **unchecked and disabled**, the editor disabled, and `display()` never runs
  for the row — its label is set unconditionally to
  `Not used on X11 — the Held keys setting on the Scan Popup tab applies` (the modifier checkboxes
  live on that tab, `preferences.ui:426/568`, not "above" the Hotkeys group).

`Preferences::getPreferences()` writes the two values back only on Wayland, so an X11 session never
disables the shortcut for the Wayland machine sharing the file:

```cpp
bool const wayland = QGuiApplication::platformName().startsWith( QLatin1String( "wayland" ) );
p.enableScanPopupHotkey = wayland ? ui.enableScanPopupHotkey->isChecked()
                                  : cfg.preferences.enableScanPopupHotkey;
p.scanPopupHotkey = wayland
    ? ( scanPopupHotkeyEdited ? ui.scanPopupHotkey->getHotKey() : cfg.preferences.scanPopupHotkey )
    : cfg.preferences.scanPopupHotkey;
```

The `*Edited` guard is mandatory (F7): `HotKeyEdit::getHotKey()` returns a fresh `Config::HotKey`
with no `rawText` (`hotkeyedit.cc:29-38`), so an unedited value would be rewritten from PortableText
— or, with the F1 default, to empty — merely by pressing OK. `scanPopupHotkeyEdited` is set in the
existing `eventFilter` (`preferences.cc:600-613`) exactly as the other two are.

## 8. Data flow

1. **Startup.** `MainWindow` creates the source; the first event-loop turn calls `start()`. The
   registry handshake is the non-blocking loop of §6.4 — no `wl_display_roundtrip()`. When the manager
   and a seat are bound, `State::Available` is published and pushed into the preferences status.
2. **Capture.** `data_offer` + `primary_selection` → the offer is held, 200 ms debounced, then read to
   EOF under the 1 MiB cap and decoded. Ignored while a GoldenDict window is focused.
3. **Activation.** Portal emits `Activated("scan-popup", token)`. The popup's native handle is
   materialised, the token applied, then `translateSelectedText()` runs with `lastText()`.
4. **Raise.** `translateWord()` → `engagePopup()` → `show()`; focus is requested through the token and
   may be declined. If `scanToMainWindow` is set the phrase goes to the main window instead.

## 9. Error handling

| condition | behaviour |
| --- | --- |
| `wayland-client` missing at build | `HAVE_WAYLAND_DATA_CONTROL` undefined → `State::Unsupported`; the binding is **not** added (the predicate also checks the macro); the row shows the unsupported text |
| no `ext_data_control_manager_v1` (GNOME, wlroots without the ext protocol) | `Unavailable("no ext_data_control_manager_v1")`, one signal, no retry; shortcut falls back to the clipboard |
| no seat yet | stays `Starting`; a later seat global is bound |
| manager bound, no `primary_selection` ever | `Available` with `primarySelectionSeen == false`; row says "not observed yet" |
| device `finished` | destroy device and offers, `Unavailable("device finished")`, cached text kept |
| offer without a text MIME | offer destroyed, cached text untouched, logged |
| read timeout (250 ms, no EOF) | payload dropped, offer destroyed, cached text untouched |
| payload over 1 MiB | read stops at the cap, payload marked truncated, decoded and sanitised |
| invalid UTF-8 / embedded NUL | replaced / stripped, then sanitised; an invalid phrase opens nothing |
| protocol error confined to the private connection | stop, one `Unavailable`, app keeps running |
| compositor restart | the private connection and Qt's own Wayland connection die together; Qt terminates the process. The old "app keeps running" claim is withdrawn (F21) |
| empty selection and empty clipboard | nothing opens; the **main window** status bar reports it |

## 10. Testing

### 10.1 New suite `tests/tst_waylandselection`

Links `waylandselectionlogic.cc` only, so it needs no compositor and no libwayland. The suite must
also be registered in the closed inventory (F3): `tests/tests.pro:123-138` enumerates
`GD_REQUIRED_SUITE_NAMES` and enforces `count( GD_TEST_NAMES, 19 )`. The change is: add
`tst_waylandselection` to that list and raise both the count and the message string to 20, then
regenerate `Makefile.tests` with `make tests-makefile`.

Cases:

* `mimePreferenceOrder` — the four accepted types in priority order; unknown types rejected.
* `payloadClampIsAppliedWhileReading` — a 1 MiB + 1 payload yields exactly `MaxPayloadBytes` and the
  truncated flag; the boundary byte counts.
* `decodeReplacesInvalidUtf8`, `decodeStripsNul` — NUL is removed, not preserved (F15).
* `chooseTextPrefersSelection` — selection wins; whitespace-only selection falls back; both empty.
* `offerSupersessionRetiresToken` — a read token for offer A is rejected after offer B arrives (F4b).
* `clipboardOfferIsDiscarded` — the `selection` channel never yields a read (F4d).
* `retiredOfferIsNeverReused` — a second MIME attempt after retirement is refused by the state.

### 10.2 Extended `tests/tst_hotkeys`

* `scanPopupBindingIsPortalOnly` — the real `scanPopupBindingPlanned()` over `xcb`, `wayland`,
  `wayland-egl`, `offscreen`, the preference on/off, and `buildSupportsSelection` false (F18).
* `portalIdAllowlistsAgree` — a table test proving `knownBindingId()` and `isKnownPortalBindingId()`
  accept exactly the same three ids, so F2 cannot regress one without the other.
* `defaultScanPopupHotkeyIsBindable` — `preferredTrigger( default ) == "CTRL+LOGO+d"` and
  `hotKeyStorageText( default )` is non-empty. This is the regression test for F1.
* `threeBindingsStayBound` — `beginGeneration()` with all three ids stays `Bound`/`Partial`, not
  `Unavailable` (F2).
* `scanPopupPortalBindingSurvivesSaveLoad` — a `scan-popup` `<portalBindings>` record round-trips.
* `scanPopupHotkeyKeepsVerbatimText` — an untouched stored sequence survives
  `Preferences`-style save/load, mirroring the rawText assertions at `tst_hotkeys.cc:1768-1786` (F7).
* `legacyConfigDefaultsScanPopupHotkeyOn` — a fixture without the two keys loads with the default
  `true` and `Meta+Ctrl+D` (F22).
* Source assertions, same convention as `mainWindowUsesTheSharedInstallGate`: exactly two
  `"scan-popup"` literals in `mainwindow.cc`, the activation branch calls `winId()` then
  `applyActivation()` then `translateSelectedText()`, and the `makeScanPopup()` guard mentions
  `scanPopupBindingPlanned`.
* `xcbGuardDoesNotCreateThePopup` — a source/behaviour assertion that the third guard term is the
  platform predicate, not the bare preference (F11).

### 10.3 Third-row statuses

`tst_hotkeys` drives `Preferences::setHotkeyStatus()` directly with each `WaylandSelectionSource::State`
and asserts the exact rendered label for the third row, including the disabled xcb text and the
"not observed yet" case (F8, F17).

### 10.4 Live gate

The repository already has the harness this needs: `tests/tst_hotkeys` runs nested `kwin_wayland` on
a private bus with `TestSupport::FakePortal`, and `FakePortal::emitActivation()` exists
(`tests/support/fake_portal.hh:145`). The gate therefore:

* starts a nested `kwin_wayland`, sets a primary selection from a helper client, and asserts the
  source reached `Available` with `primarySelectionSeen`, logged
  `selection-captured mime=<type> bytes=<n>`, and captured the expected text;
* emits `scan-popup` through `FakePortal::emitActivation()` and asserts the popup opened with that
  text;
* asserts a burst of selections produces exactly one read after the debounce (F13);
* asserts a selection made while the app is focused is ignored (`selection-ignored
  reason=own-window-focused`).

**Log contract** the gate asserts (defined here, not invented by the test): `gdDebug` lines
`selection-source: state=<state> reason=<reason>`, `selection-captured mime=<type> bytes=<n>
truncated=<0|1>`, `selection-ignored reason=<own-window-focused|no-text-mime|stale-token|timeout>`.

### 10.5 Acceptance on the reporter's session

Select a word in any application, press the shortcut, confirm the popup shows that word; press it
again after copying something else with no new selection, confirm the clipboard fallback; press it
with neither, confirm the main window status message.

## 11. Deferred coverage

Recorded in `docs/wayland-limitations.md` §2 when the feature lands. The repository keeps platform
limits in that one evidence-bearing document, so a second TODO list would only duplicate it:

* **wlroots compositors** (sway, hyprland, labwc) expose `zwlr_data_control_v1` instead. Adding it is
  one more generated protocol pair and one more bind attempt behind the same
  `WaylandSelectionSource` interface; `waylandselectionlogic` needs no change.
* **GNOME/Mutter** exposes neither data-control protocol, so no selection can be read by a
  background application at all. Only a future GNOME-side protocol, or the RemoteDesktop `Clipboard`
  portal with its remote-control consent prompt, could serve it. The shortcut stays registered and
  falls back to the clipboard.
* **Multi-seat sessions**: only the first seat is watched.
* **Self-selection residual**: a selection made inside GoldenDict while it was focused is never
  stored, so pressing the shortcut afterwards falls back to the clipboard.

## 12. Documentation updates (F23)

* `docs/wayland-limitations.md` §2: keep both existing limits — hover translation **and** X11 focus
  restoration stay X11-only — and add that selected-text translation is available through the global
  shortcut on compositors exposing `ext_data_control_manager_v1`, with the clipboard fallback.
* `README.md` limits sentence: name the compositor coverage and the fallback.
* `clipboardCapabilityNote` in `preferences.cc:669-673` gains the selection-source state.

## 13. Alternatives considered

| alternative | why rejected |
| --- | --- |
| `zwlr_data_control_v1` in the same change | doubles the generated protocol surface for compositors the reporter does not use; kept as the first follow-up |
| `wl-paste --primary --watch` subprocess | adds a helper process and a runtime dependency, no MIME control |
| auto-popup on every selection | rejected at the design gate: the reporter wants the popup only on a key |
| a held-chord emulation via the portal | the portal cannot carry a modifier-only trigger |
| reading the clipboard instead of the primary selection | the clipboard is stale when the app holds no focus; it stays a fallback only |
