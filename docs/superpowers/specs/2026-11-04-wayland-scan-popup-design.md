# Wayland scan popup: selected-text shortcut

Status: design for review. Run `pm-run-20261104-005240-d7e4068b`, topic `scan-popup`.

## 1. Decision record

| decision | choice | reason |
| --- | --- | --- |
| trigger on Wayland | **shortcut only** (option 2 of the gate) | The reporter wants X11's *checked* mode — no popup without keys — and accepts a discrete shortcut in place of a held chord. |
| selection source | **own client for `ext_data_control_manager_v1`** (option 1) | The protocol KWin 6.8 actually advertises. No new build or runtime dependency. |
| UI placement | **(a)** third row in the existing Hotkeys group | One list of shortcuts, one status block, reuses the existing per-shortcut status line. |

`preferredTrigger()` rejects a modifier-only binding (`hotkeysyntax.cc:203` "The hotkey has no key,
only modifiers."), so a held `Ctrl+Win` chord can never become a portal shortcut. The shortcut needs
a real key: default `Ctrl+Win+D`, which serialises to the portal trigger `CTRL+LOGO+d`.

## 2. Background

The scan popup is the product's signature feature and it is X11-only today. On `xcb` a primary
selection change arms `delayTimer` (200 ms) and `delayShow()` hands the text to `handleInputWord()`,
which applies the modifier gate and raises the popup.

On Wayland every one of those steps is absent:

* `ScanPopup::clipboardChanged()` returns early unless clipboard watching is on and the mode is
  `Clipboard` (`scanpopup.cc:506`); the `Selection` branch is `xcb`-only (`scanpopup.cc:522`);
  `delayShow()` is inside `#ifdef HAVE_X11` (`scanpopup.cc:496`).
* the only `emit hovered()` is inside `Q_OS_WIN32` (`mouseover.cc:362`), and `mouseHovered()`
  returns early on Wayland (`scanpopup.cc:531`).
* `docs/superpowers/specs/2026-10-24-qt6-migration-spec.md` §9 puts "Wayland InputCapture hover
  translation" out of scope, and `docs/wayland-limitations.md` §2 states the limit.

Measured on this machine (`receipts/investigation-scanpopup.json` in the run root): with the
reporter's configuration the popup raises correctly on `xcb` (modifier held, alt-mode within 3 s, and
a clean negative control without modifiers), and cannot raise at all on Wayland.

## 3. Goals

1. A global shortcut on Wayland that opens the scan popup with the text most recently selected in any
   other application.
2. Zero change to X11 runtime behaviour — same code path, no new binding. The Hotkeys tab gains one
   disabled row, described in 7.3.
3. Honest degradation: when the compositor cannot serve the primary selection, the status line says
   so and the shortcut falls back to the clipboard.

## 4. Non-goals

* `zwlr_data_control_v1` (wlroots: sway, hyprland, labwc) and GNOME/Mutter. Recorded as deferred
  coverage in §11, not implemented here.
* Popping up automatically when a selection appears. Rejected at the design gate.
* Emulating a held-modifier chord. Impossible on Wayland; the portal cannot carry it.
* Any change to clipboard watching, the clipboard hotkey, or the X11 modifier gate.

## 5. Behaviour contract

1. The shortcut is registered **only** when the QPA platform name starts with `wayland` **and**
   `preferences.enableScanPopupHotkey` is true. On `xcb` no such binding is created.
2. Activating it opens the scan popup with `lastText()` of the selection source. If that is empty,
   the clipboard text is used. If both are empty, no window opens; the status bar reports
   `No selected text or clipboard text to translate` for 3 s.
3. `lastText()` is sticky: it survives the selection being unset, so the user may select, click
   elsewhere, then press the shortcut. It is replaced only by a newer successful read.
4. The popup is raised through the existing `translateWord()` path, so sanitising, the
   same-phrase dedupe, input-length limits, history and group selection are unchanged.
5. The shortcut exists on Wayland only. On `xcb` the preferences row is disabled and the stored
   values are preserved unchanged (§7.3).

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


### 6.1 Units

| unit | responsibility | depends on |
| --- | --- | --- |
| `waylandselectionlogic.hh/.cc` (new) | pure decisions: MIME preference, payload clamp, UTF-8 decode, text choice, offer state machine | Qt core only |
| `waylandselection.hh/.cc` (new) | `wl_display` connection, registry bind, seat, device, offers, reads | `libwayland-client`, generated protocol code, the logic unit |
| `protocols/ext-data-control-v1/` (new) | vendored protocol XML, generated client header and private code, `regenerate.sh` | `wayland-scanner` (regeneration only) |
| `config.hh/.cc` | `enableScanPopupHotkey`, `scanPopupHotkey` | — |
| `mainwindow.cc` | third binding id `scan-popup`, activation dispatch, watcher ownership | existing backend, `ScanPopup` |
| `scanpopup.cc/.hh` | `translateSelectedText( QString const & )` | existing `translateWord()` |
| `preferences.cc/.ui` | third hotkey row and its status | existing `setHotkeyStatus()` |

The split between `waylandselectionlogic` and `waylandselection` mirrors `RecorderMatcher` /
`RecorderCore` / `X11RecordSession`: the rules are proved without a server, the session work needs
the real API.

### 6.2 Selection source API

```cpp
class WaylandSelectionSource : public QObject
{
  Q_OBJECT
public:
  explicit WaylandSelectionSource( QObject * parent = nullptr );
  ~WaylandSelectionSource() override;

  /// False when the build or the session cannot serve this at all.
  bool isSupported() const;

  /// True once the manager and a seat are bound.
  bool isAvailable() const;

  /// Most recent non-empty primary selection payload; sticky (see 5.3).
  QString lastText() const;

  /// Human-readable reason for isAvailable() == false, empty otherwise.
  QString unavailableReason() const;

public slots:
  void start();   /// idempotent; safe to call twice
  void stop();    /// releases the device, the seat, the manager and the connection

signals:
  /// Emitted at most once per start() when the session cannot serve selections.
  void unavailable( QString const & reason );
};
```

The source owns a **private** `wl_display` connection. It never touches Qt's Wayland connection, so
Qt's own clipboard handling cannot be disturbed.

### 6.3 Pure helpers (`waylandselectionlogic`)

```cpp
namespace WaylandSelection {
  /// Preference order, first match wins:
  ///   text/plain;charset=utf-8, text/plain;charset=UTF-8, UTF8_STRING, text/plain
  /// Empty when the offer carries no usable text MIME type.
  QString chooseMimeType( QStringList const & offered );

  /// Upper bound applied before decoding: 1 MiB.
  QByteArray clampPayload( QByteArray const & raw );

  /// UTF-8 decode with replacement of invalid sequences; embedded NULs are
  /// preserved (the phrase sanitiser decides what is usable).
  QString decodePayload( QByteArray const & raw );

  /// The selection wins when it is non-empty after trimming; otherwise the clipboard.
  QString chooseText( QString const & selection, QString const & clipboard );

  /// Offer state: a new offer replaces the previous one; null clears it; a read
  /// that times out leaves the cached text untouched.
  class OfferState { /* applyOffer(id), applyCleared(), applyRead(QByteArray) */ };
}
```

### 6.4 Protocol client rules

* Bind `ext_data_control_manager_v1` at version 1, then `get_data_device()` for the first `wl_seat`
  the registry reports. No seat means `unavailable("no seat")`.
* Watch `primary_selection` only; `selection` (the ordinary clipboard) is ignored — the clipboard
  stays Qt's.
* Reading an offer: `receive()` into a pipe, read on the display fd, 250 ms guard, 1 MiB cap.
  A timeout or a short read that never delivers leaves `lastText()` unchanged.
* All I/O is fd-notifier driven in the GUI thread with the standard
  `wl_display_prepare_read` / `wl_display_read_events` / `wl_display_dispatch_pending` /
  `wl_display_flush` sequence. No blocking call, no worker thread.
* A display error (`EPIPE`/`ECONNRESET`) stops the watcher, emits `unavailable` once and never
  retries in a loop.

### 6.5 Configuration

Two additive preferences, read and written beside the existing hotkey entries:

```xml
<enableScanPopupHotkey>1</enableScanPopupHotkey>
<scanPopupHotkey>Ctrl+Win+D</scanPopupHotkey>
```

Defaults: `true` and `QKeySequence("Ctrl+Win+D")`, matching the other two hotkeys' default-on
behaviour. Storage goes through the existing `hotKeyStorageText()` / `loadHotKey()` pair, so the file
holds that helper's PortableText form of the default rather than a literal `Ctrl+Win+D`, and an
untouched stored sequence keeps its verbatim text (`HotKey::rawText`).

### 6.6 Wiring

`MainWindow::installHotKeys()`:

```cpp
if( portal && cfg.preferences.enableScanPopupHotkey )
  add( true, "scan-popup", tr( "Translate the selected text" ), cfg.preferences.scanPopupHotkey );
```

`portal` is the existing `platform.startsWith("wayland")` local, so the X11 RECORD backend never sees
this id. The activation lambda gains:

```cpp
else if( id == "scan-popup" && scanPopup )
{
  PlatformWindow::applyActivation( scanPopup.get(), token );
  scanPopup->translateSelectedText( selectionSource ? selectionSource->lastText() : QString() );
}
```

`makeScanPopup()`'s early return becomes:

```cpp
if( !cfg.preferences.enableScanPopup &&
    !cfg.preferences.enableClipboardHotkey &&
    !cfg.preferences.enableScanPopupHotkey )
  return;
```

`MainWindow` owns the source (a `std::unique_ptr` member, created on Wayland when
`HAVE_WAYLAND_DATA_CONTROL` is compiled in), starts it on the first event-loop turn, and stops it in
the existing shutdown path before the profile and the popup are torn down.

### 6.7 Popup entry point

```cpp
void ScanPopup::translateSelectedText( QString const & selectionText )
{
  QString const text = WaylandSelection::chooseText(
      selectionText, QApplication::clipboard()->text( QClipboard::Clipboard ) );
  if( cfg.preferences.sanitizeInputPhrase( text ).isValid() )
  {
    translateWord( text );
    return;
  }
  emit showStatusBarMessage( tr( "No selected text or clipboard text to translate" ), 3000, {} );
}
```

## 7. User interface

### 7.1 Placement (a) — recorded choice

```text
Hotkeys
  [ ] Use the following hotkey to show or hide the main window:
                                          [ Alt+4       ]
  [ ] Use the following hotkey to translate a word from clipboard:
                                          [ Ctrl+C,C    ]
  [ ] Use the following hotkey to translate the selected text:
                                          [ Ctrl+Win+D  ]

  The global shortcuts use the active desktop backend. See the capability
  and per-shortcut status below.

  Global shortcut status: Portal bound
    Main window shortcut   ...
    Clipboard shortcut     ...
    Selected text shortcut: Active — selection readable (ext-data-control-v1)
  [ Configure shortcuts… ]
```

### 7.2 Wayland states for the third row

| condition | row | status line |
| --- | --- | --- |
| portal bound, source available | enabled, checked | the portal-reported trigger |
| portal bound, source unavailable (no `ext_data_control_manager_v1`) | enabled, checked | `Selection unavailable: <reason> — the clipboard is used instead` |
| portal denied / partial | enabled | the existing portal states, unchanged |
| shortcut disabled | unchecked editor greyed | `Disabled` |

### 7.3 X11 state for the third row

The row stays visible so a synced configuration explains itself, but it is disabled with the status
`Not used on X11 — the scan popup modifiers above apply`. `Preferences::getPreferences()` writes the
checkbox and the hotkey back only on Wayland:

```cpp
p.enableScanPopupHotkey = wayland ? ui.enableScanPopupHotkey->isChecked()
                                  : cfg.preferences.enableScanPopupHotkey;
p.scanPopupHotkey = wayland ? ui.scanPopupHotkey->getHotKey()
                            : cfg.preferences.scanPopupHotkey;
```

This keeps one configuration file usable on both a Wayland and an X11 machine.

## 8. Data flow

1. Startup on `wayland…`: `MainWindow` creates the source; the first event-loop turn calls
   `start()`. The registry roundtrip binds `ext_data_control_manager_v1` and `wl_seat`; a device is
   created; `supported`/`unavailable` is decided once and pushed to the preferences status.
2. The compositor sends `primary_selection` with an offer. The source records the offer, picks the
   MIME type, receives into a pipe, reads it under the 250 ms guard, decodes it and stores it as
   `lastText()`.
3. The user presses the portal shortcut. The portal emits `Activated(id="scan-popup", token)`.
4. `PlatformWindow::applyActivation()` hands the token to the popup window; the popup is raised with
   focus because the activation token is present.
5. `translateSelectedText( lastText() )` → clipboard fallback → `translateWord()` → article view, as
   on X11.

## 9. Error handling

| condition | behaviour |
| --- | --- |
| `wayland-client` missing at build time | `HAVE_WAYLAND_DATA_CONTROL` undefined; no source, no binding, row shows the unavailable reason |
| no `ext_data_control_manager_v1` (GNOME, wlroots without the ext protocol) | one `unavailable` signal with the reason; no retry; shortcut falls back to the clipboard |
| no seat | same as above |
| offer has no text MIME type | offer ignored, `lastText()` untouched |
| read timeout (250 ms) | that offer dropped, `lastText()` untouched |
| payload above 1 MiB | clamped before decoding |
| invalid UTF-8 | replaced, then sanitised; an invalid phrase opens nothing |
| display disconnect | stop, one `unavailable`, app keeps running |
| empty selection and empty clipboard | nothing opens, 3 s status message |

## 10. Testing

### 10.1 New suite `tests/tst_waylandselection`

Links `waylandselectionlogic.cc` only, so it needs no compositor and no libwayland:

* `mimePreferenceOrder` — the four accepted types and their priority; unknown types rejected.
* `payloadClampKeepsFirstMebibyte` — exact boundary.
* `decodeReplacesInvalidUtf8` / `decodeKeepsEmbeddedNul`.
* `chooseTextPrefersSelection` — selection wins, whitespace-only selection falls back, both empty.
* `offerStateReplacementAndClear` — new offer replaces, null clears, timeout keeps the cached text.

### 10.2 Extended `tests/tst_hotkeys`

* `scanPopupBindingIsPortalOnly` — the predicate used by `installHotKeys()` is true only for a
  `wayland…` platform with the preference enabled.
* A source assertion that `mainwindow.cc` contains exactly two `"scan-popup"` literals (the binding
  in `installHotKeys()` and the activation branch), that the branch calls `translateSelectedText`,
  and that the `makeScanPopup()` guard tests `enableScanPopupHotkey` (same convention as
  `mainWindowUsesTheSharedInstallGate`).
* Config round-trip: a config without the two new keys loads with defaults; a config with them saves
  and reloads unchanged, verbatim sequence included.

### 10.3 Live gate

Nested `kwin_wayland` (the previous migration lane used socket `gd-t32`) with a helper client that
sets the primary selection. Assertions: the app logs
`selection-captured mime=<type> bytes=<n>` and never logs `selection-unavailable`; injecting the
`scan-popup` activation opens the popup with that text. The nested session has no portal, so the
activation is injected by the harness rather than by the desktop.

### 10.4 Acceptance on the reporter's session

Select a word in any application, press the shortcut, confirm the popup shows that word; press it
again after copying something else with no selection, confirm the clipboard fallback.

## 11. Deferred coverage

Recorded in `docs/wayland-limitations.md` §2 when the feature lands. The repository keeps platform
limits in that one evidence-bearing document, so a second TODO list would only duplicate it:

* **wlroots compositors** (sway, hyprland, labwc) expose `zwlr_data_control_v1` instead. Adding it is
  one more generated protocol pair and one more bind attempt behind the same
  `WaylandSelectionSource` interface; `waylandselectionlogic` needs no change.
* **GNOME/Mutter** exposes neither data-control protocol, so no selection can be read by a
  background application at all. Only a future GNOME-side protocol, or the RemoteDesktop
  `Clipboard` portal with its remote-control consent prompt, could serve it. The shortcut stays
  registered and falls back to the clipboard.

## 12. Documentation updates

* `docs/wayland-limitations.md` §2: replace "Hover translation and X11 focus restoration remain
  X11-only" with the new statement — hover translation stays X11-only; selected-text translation is
  available through the global shortcut on compositors that expose `ext_data_control_manager_v1`.
* `README.md` limits sentence: name the compositor coverage and the fallback.
* The hotkeys paragraph in the preferences capability note (`clipboardCapabilityNote`) gains the
  selection-source state.

## 13. Alternatives considered

| alternative | why rejected |
| --- | --- |
| `zwlr_data_control_v1` in the same change | doubles the generated protocol surface for compositors the reporter does not use; kept as the first follow-up |
| `wl-paste --primary --watch` subprocess | adds a runtime dependency, spawns a helper, no MIME control |
| auto-popup on every selection | rejected at the design gate: the reporter wants the popup only on a key |
| a held-chord emulation via the portal | the portal cannot carry a modifier-only trigger |
