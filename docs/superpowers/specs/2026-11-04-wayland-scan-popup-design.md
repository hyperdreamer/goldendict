# Wayland scan popup: selected-text shortcut

Status: design revision 4, for review. Run `pm-run-20261104-005240-d7e4068b`, topic `scan-popup`.
Review history: revision 1 → 27 findings (F1–F27, 3 blockers); revision 2 → 12 (G1–G12, 2 blockers);
revision 3 → 12 (H1–H12, 2 blockers, 6 partials). This revision resolves all three sets.

## 1. Decision record

| decision | choice | reason |
| --- | --- | --- |
| trigger on Wayland | **shortcut only** | The reporter wants X11's *checked* mode — no popup without keys — and accepts a discrete shortcut in place of a held chord. |
| selection source | **own client for `ext_data_control_manager_v1`** | The protocol KWin 6.8 actually advertises; no helper process and no new service dependency, only one optional shared-library link (`libwayland-client`, already present in any Wayland session). (F24) |
| UI placement | **(a)** third row in the existing Hotkeys group | One list of shortcuts, one status block, reuses the existing per-shortcut status line. |

`HotkeySyntax::preferredTrigger()` rejects a modifier-only binding (`hotkeysyntax.cc:205`, "The hotkey
has no key, only modifiers."), so a held `Ctrl+Win` chord can never become a portal shortcut. (F20)

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
  (`scanpopup.cc:496`). (F20)
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

1. A global shortcut on Wayland that opens the scan popup with the **current primary selection** —
   whichever client set it, GoldenDict included (H8).
2. Zero change to X11 runtime behaviour — same code path, no new binding, and no `ScanPopup` object
   created that X11 would not have created before. The Hotkeys tab gains one disabled row (§7.4).
3. Honest degradation: when the compositor cannot serve the primary selection, the status line says
   so, the shortcut falls back to the clipboard, and an empty result is reported through the first
   available channel. When the main window is hidden **and** no tray icon exists there is no visible
   channel; the event is then logged only, and this is the stated limit rather than a promise (H5).

## 4. Non-goals

* `zwlr_data_control_v1` (wlroots: sway, hyprland, labwc) and GNOME/Mutter. Recorded as deferred
  coverage in §11.
* Popping up automatically when a selection appears. Rejected at the design gate.
* Emulating a held-modifier chord. Impossible on Wayland; the portal cannot carry it.
* Any change to clipboard watching, the clipboard hotkey, or the X11 modifier gate.

## 5. Behaviour contract

1. The shortcut is registered **only** when
   `scanPopupBindingPlanned( kHaveWaylandDataControl, platformName, preferences.enableScanPopupHotkey )`
   is true — a real function (§6.6) over one consistently spelled capability constant, not an inline
   `#ifdef` and not an undefined macro (G1). It is false on `xcb`, so the RECORD backend never sees the id.
2. Activating it asks the source for the current selection (`requestCapture()`), which promotes a
   pending read instead of using a stale cache, and answers within 300 ms (§6.6).
3. **Which text wins (H1).** Selection and clipboard are independent on Wayland: copying does not
   change the primary selection. The activation therefore compares *timestamps*, not emptiness: the
   selection is used when it is non-empty **and** was captured no earlier than the clipboard's last
   change; otherwise the clipboard; otherwise the selection; otherwise nothing. Ties go to the
   selection.
4. **A stale cache is never used as if it were current.** `lastText()` is sticky — it survives the
   selection being unset, so select-click-away-then-shortcut still works — but an activation answered
   from the cache obeys rule 3. If a promoted read **times out**, the cached selection is suppressed
   for that activation: the answer is the clipboard, or nothing, logged as `selection-timeout`. An
   older capture than the one the user just made is never preferred.
5. The source does not attribute an offer to an owning application: the value is the current primary
   selection, whichever client set it, including GoldenDict's own article view (G10).
6. The phrase is raised through the existing `translateWord()` path, so sanitising, input-length
   limits, history and group selection are unchanged. Two behaviours are **not** inherited silently:
   * the same-phrase dedupe lives in `handleInputWord()` (`scanpopup.cc:540`), not in
     `translateWord()`, so the new entry point performs the same check itself before translating
     (F16);
   * `scanToMainWindow` is honoured: when it is set, `engagePopup()` sends the phrase to the main
     window instead of showing the popup (`scanpopup.cc:596`). The shortcut keeps that behaviour and
     the contract is "open the popup **unless** 'send to main window' is enabled" (F25).
7. The shortcut exists on Wayland only. On `xcb` the preferences row is disabled and the stored values
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

  bool hasPendingCapture() const;   ///< an offer is held, or a pipe read is in flight

public slots:
  void start();   ///< idempotent; Unsupported is a no-op that sets State::Unsupported
  void stop();    ///< idempotent; releases device, offers, seat, manager, connection

  /// Answer where the current selection is, bounded. If a capture is pending the
  /// read is promoted (the debounce is skipped) and answered on completion; the
  /// answer is guaranteed within 300 ms, falling back to lastText() (G5).
  void requestCapture( quint64 requestId );

signals:
  void stateChanged( State state, QString const & reason );
  void capabilityChanged();        ///< primarySelectionSeen changed while Available (G7)
  /// Always answered within the bound. `capturedAt` is the monotonic stamp of the
  /// capture that backs `text`, or 0 when no capture backs it (nothing ever
  /// captured, or a promoted read timed out and the cache is suppressed). The
  /// caller applies the recency rule of §5.3 with it.
  void captureFinished( quint64 requestId, QString const & text, qint64 capturedAt );
};
```

`Preferences` never holds a pointer to the source; the window pushes a **snapshot** on every change
(H4):

```cpp
struct SelectionStatus
{
  WaylandSelectionSource::State state = WaylandSelectionSource::State::Unsupported;
  QString reason;                  // non-empty iff Unavailable
  bool primarySelectionSeen = false;
};
```

State transitions, all of them terminal or absorbing:

```
Unsupported --start()--> Unsupported                (no connection is opened)
Starting    --manager+seat bound--> Available
Starting    --connect/bind failure--> Unavailable
Available   --display error | manager removed | seat removed (no other seat)--> Unavailable
Available   --device finished--> Unavailable
Available   --bound seat removed, another seat present--> rebind, stay Available
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

  /// Upper bound applied while reading, not after (F6/G8).
  static constexpr int MaxPayloadBytes = 1024 * 1024;

  /// Bounds memory during the read instead of copying afterwards: append() keeps
  /// at most `cap` bytes and latches truncated().
  class PayloadAccumulator
  {
  public:
    explicit PayloadAccumulator( int cap = MaxPayloadBytes );
    void append( QByteArray const & chunk );
    bool full() const;
    bool truncated() const;
    QByteArray payload() const;
  private:
    int cap_; QByteArray data_; bool truncated_ = false;
  };

  /// UTF-8 decode with replacement of invalid sequences; U+0000 is removed,
  /// because the phrase sanitiser does not remove it (F15).
  QString decodePayload( QByteArray const & raw );

  /// Recency rule of §5.3 (H1). `capturedAt` / `clipboardChangedAt` are monotonic
  /// millisecond stamps; a zero stamp means "never". Precedence: a non-empty
  /// selection no older than the clipboard change, then the clipboard, then a
  /// non-empty selection, then empty.
  QString chooseText( QString const & selection, qint64 capturedAt,
                      QString const & clipboard, qint64 clipboardChangedAt );

  /// What an activation does while a capture is still pending (G5/G9).
  enum class ActivationPlan { UseCached, AwaitPending };
  ActivationPlan planActivation( bool hasPendingCapture );

  /// Identity of one capture attempt. Every read is attributed to the token that
  /// started it; a completion for a retired token changes nothing (F4b).
  using ReadToken = quint64;

  /// A defined, testable handle for one channel event (G8). Id 0 is the protocol's
  /// null offer: it clears the channel without starting a read.
  struct ChannelOffer { quint64 id = 0; operator bool() const { return id != 0; } };

  /// Two channels share one `data_offer` prefix in the protocol (F4d). The state
  /// keeps the pending offer separately from the accepted primary offer.
  class OfferState
  {
  public:
    /// data_offer carries the offer id and the MIME types it advertised.
    void pendingOffer( quint64 offerId, QStringList const & mimeTypes );
    ChannelOffer takeForPrimary();                 ///< primary_selection (null clears)
    ChannelOffer discardForClipboard();            ///< selection: destroy, never read

    /// Walks the advertised list in preference order. False when the offer is
    /// retired or every acceptable MIME type has already been attempted (H6).
    bool beginMimeAttempt();
    QString currentMimeType() const;               ///< the type the next read must request
    void mimeAttemptFailed();                      ///< advance to the next acceptable type

    ReadToken beginRead();                         ///< reserve a token for the current attempt
    bool accepts( ReadToken token ) const;         ///< false for retired tokens
    void retire();                                 ///< supersede: no token is accepted any more
    void completed( ReadToken token );             ///< token consumed, state settles
  };
}
```

### 6.4 Protocol client rules

* One **private** `wl_display` connection; Qt's Wayland connection is never touched.
* **Discovery barrier (G4).** Globals arrive asynchronously; there is no "last global" event. After
  the registry listener is installed the source issues `wl_display.sync` and treats that callback's
  `done` as the end of the initial burst. `Available` is published then, if the manager and a seat are
  bound; otherwise the state becomes `Unavailable` with the missing global named. `sync` is a normal
  request, so the handshake stays inside the non-blocking loop.
* **Seat policy (F12).** Bind the first `wl_seat` reported; more than one seat is logged and the
  first is watched (primary selection is per-seat; the limitation is recorded in §11). If no seat
  arrived in the initial burst, stay `Starting` and bind the first seat that appears later. A
  `global_remove` of the bound seat destroys the device and its offers, then binds another seat if
  one remains, otherwise becomes `Unavailable("seat removed")`. A `global_remove` of the manager
  becomes `Unavailable("manager removed")`.
* `data_offer` creates a pending offer. The immediately following channel event decides it:
  `primary_selection` accepts it, `selection` (the ordinary clipboard) is destroyed immediately and
  never read — the clipboard stays Qt's (F4d). A `primary_selection` carrying the protocol's null
  offer clears the pending capture without starting a read and leaves `lastText()` unchanged.
* **Supersession (F4a, F4c).** A new `primary_selection` retires the previous offer: retire its read
  token, remove and close its read end, then `ext_data_control_offer_v1.destroy()`. A retired proxy is
  never marshalled again — libwayland aborts in `wl_proxy_marshal_array_flags` on a destroyed proxy.
  A late completion for a retired token is ignored by `OfferState::accepts()`.
* **Debounce (F13).** A `primary_selection` event does not read immediately. The accepted offer is
  held and read 200 ms after the last selection event, mirroring X11's `delayTimer`. A newer event
  replaces the held offer, so a drag-selection burst produces one transfer. `requestCapture()`
  promotes a held offer immediately (G5).
* **Pipe ownership and completion (G2, F6, H3).** The pipe is created with `pipe2( O_CLOEXEC )`; only
  the **read** end is switched to non-blocking with `fcntl( readEnd, F_SETFL, O_NONBLOCK )`. The write
  end stays blocking, because the compositor's writer must never see `EAGAIN`. The **write** end is passed to
  `ext_data_control_offer_v1.receive()` and then **closed by the client immediately after the
  request** — libwayland has already duplicated it into the outgoing message, and keeping our copy
  open would prevent the EOF that defines completion. The **read** end is non-blocking and owned by
  the source, driven by a `QSocketNotifier`.
  Completion is **EOF or `MaxPayloadBytes` + 1 byte**, never the first short `read()`. Reading one
  byte past the cap distinguishes a payload exactly at the limit from a truncated one, so
  `truncated()` is true only when that extra byte is observed (H10). Bytes are appended to
  `PayloadAccumulator`; `read() < 0 && errno == EAGAIN` means wait; `EINTR` retries; any other error
  drops the capture. On completion, timeout or retirement the read end is closed and the notifier
  removed. Two bounds apply: an idle timeout of 250 ms since the last successful read, and an
  **absolute transfer deadline of 2 s** from the `receive()` request.
* **Flush before waiting (H2).** Every outgoing request batch — the initial registry `sync`,
  `ext_data_control_manager_v1.get_data_device()`, `receive()` and `destroy()` — is followed
  immediately by `wl_display_flush( display )`. On `-1/EAGAIN` the writable notifier is armed and kept
  armed until a flush succeeds; the code then waits for the read notifier. Without this step a batch
  queued while the display is otherwise idle produces no incoming event, and the handshake stalls.
* **Event loop (F5, G3).** The display fd is driven by a read notifier with this exact branch table:
  1. `wl_display_prepare_read( display )`; if it returns `-1` and `errno == EAGAIN`,
     `wl_display_dispatch_pending( display )` and skip the read;
  2. only after a successful `prepare_read`: `wl_display_read_events( display )`; `-1` is a display
     error → stop;
  3. `wl_display_dispatch_pending( display )`;
  4. `wl_display_flush( display )`; if it returns `-1` and `errno == EAGAIN`, arm the writable
     notifier and **keep it armed** — each time it fires, flush again and disarm only when flush
     returns 0 (or a value ≥ 0), so pending requests are never left unflushed;
  5. `wl_display_cancel_read( display )` on any path where the readiness was consumed elsewhere;
  6. any `-1` with `wl_display_get_error( display ) != 0` ends the watcher.
  There is no `wl_display_roundtrip()` anywhere.
* **Capture is opt-in (H12).** The source is started only while
  `preferences.enableScanPopupHotkey` is true; disabling the shortcut stops the source and no
  selection is monitored. `MainWindow` reconciles the source with the preference at startup and after
  the Preferences dialog is accepted, in the same place it re-installs the hotkeys.
* **No ownership detection (G10).** The protocol carries no owner identity, and a focus test at
  dispatch time is not proof of ownership because the event may be delivered after focus changed.
  The contract is therefore the current **primary selection, whatever set it**, including a selection
  made inside GoldenDict itself. The previous claim "text selected in any other application" is
  withdrawn; §5.2 and §11 say so.

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

**Capability constant (G1).** One spelling, defined once, so the predicate can be tested without an
`#ifdef` at the call site:

```cpp
// waylandselection.hh
#ifdef HAVE_WAYLAND_DATA_CONTROL
inline constexpr bool kHaveWaylandDataControl = true;
#else
inline constexpr bool kHaveWaylandDataControl = false;
#endif
```

**Binding predicate (F18).** `hotkeybackend.hh` gains a pure inline predicate, used by
`installHotKeys()` and unit-tested directly:

```cpp
bool scanPopupBindingPlanned( bool buildSupportsSelection,
                              QString const & platformName,
                              bool preferenceEnabled ) noexcept;
```

`MainWindow::installHotKeys()` adds:

```cpp
if( scanPopupBindingPlanned( kHaveWaylandDataControl,
                             QGuiApplication::platformName(),
                             cfg.preferences.enableScanPopupHotkey ) )
  add( true, "scan-popup", tr( "Translate the selected text" ), cfg.preferences.scanPopupHotkey );
```

**One id predicate (F2, H11).** There are two hard-coded predicate copies today —
`knownBindingId()` (`portalhotkeys.cc:84`) and `isKnownPortalBindingId()` (`config.cc:181`) — and an id
missing from either breaks something: from the first, `beginGeneration()` sets the whole backend to
`Unavailable, "No valid portal bindings were requested"` and the existing shortcuts die on Wayland;
from the second, a stored `scan-popup` `PortalBindingDisplay` is dropped on load and the row shows
"not assigned" after a restart. Both are translation-unit-local today, so neither is testable.

The fix is structural: declare **one** inline predicate in `hotkeybackend.hh` and make both copies call
it —

```cpp
inline bool isKnownPortalBindingId( QString const & id )
{ return id == QLatin1String( "main-window" )
      || id == QLatin1String( "clipboard-lookup" )
      || id == QLatin1String( "scan-popup" ); }
```

`portalhotkeys.cc` deletes its copy and `config.cc` uses this one. The contract comment at
`hotkeybackend.hh:28-31` names all three ids. The test suite then tests the single function, and a
source assertion proves both call sites use it (no second copy can reappear).

**Activation (F9, G5).** A freshly constructed `ScanPopup` has no native handle, and
`applyActivation()` returns `NoHandle` without installing the token when `windowHandle()` is null
(`platformwindow.cc:85`), so the handle is materialised first. The capture is asked for, not assumed:
`requestCapture()` promotes a pending read, so pressing the shortcut right after selecting translates
the word just selected rather than the previous one.

```cpp
else if( id == "scan-popup" && scanPopup )
{
  scanPopup->winId();                        // native handle exists before the token
  PlatformWindow::applyActivation( scanPopup.get(), token );
  quint64 const requestId = ++scanPopupRequestId;   // member, monotonic
  selectionSource->requestCapture( requestId );
}

// connected once, in the constructor
connect( selectionSource.get(), &WaylandSelectionSource::captureFinished,
         this, [this]( quint64 requestId, QString const & selection, qint64 capturedAt )
{
  if( shuttingDown || requestId != scanPopupRequestId || !scanPopup ) return;
  // capturedAt == 0 means the promoted read timed out: the cached selection is
  // suppressed, because an older capture than the one the user just made must not
  // be preferred (H1).
  QString const text = WaylandSelection::chooseText(
      capturedAt > 0 ? selection : QString(), capturedAt,
      QApplication::clipboard()->text( QClipboard::Clipboard ), clipboardChangedAt );
  if( !scanPopup->translateSelectedText( text ) )
    reportNothingToTranslate();
} );
```

`clipboardChangedAt` is a `MainWindow` member updated by a `QClipboard::changed` connection installed in
the constructor (0 until the first change in this run, which makes a stored selection win over a
clipboard nobody touched).

`requestCapture()` always answers within 300 ms: immediately with `lastText()` when nothing is
pending, or on completion of the promoted read, falling back to `lastText()` at the deadline. Stale
ids are ignored, so two quick presses cannot open the popup twice with mixed text.

**Empty result (F10, G6).** `reportNothingToTranslate()` uses the first visible channel, because the
main window is often hidden (`startToTray`, close-to-tray):

1. main window visible → `mainStatusBar->showMessage( tr("No selected text or clipboard text to
   translate"), 3000 )`;
2. otherwise the tray icon, when one exists → `trayIcon->showMessage( tr("GoldenDict"), ... )`;
3. otherwise nothing is shown; the event is logged as `selection-empty`.

**Popup creation guard (F11).** `enableScanPopupHotkey` defaults to true and is preserved on xcb, so a
naive third term would construct a `ScanPopup` on X11 that never existed before — and its destructor
would overwrite the stored popup geometry from a never-shown widget (`ScanPopup::saveConfigData()`,
`scanpopup.cc:337`). The guard is therefore Wayland-only:

```cpp
if( !cfg.preferences.enableScanPopup &&
    !cfg.preferences.enableClipboardHotkey &&
    !scanPopupBindingPlanned( kHaveWaylandDataControl, QGuiApplication::platformName(),
                              cfg.preferences.enableScanPopupHotkey ) )
  return;
```

**Lifecycle (F27, G12, H12).** `MainWindow` owns the source as a `std::unique_ptr` member declared
**before** `scanPopup` and `articleProfile`, so it outlives both. It is created only when
`kHaveWaylandDataControl` and the platform is Wayland. `start()` is scheduled on the first event-loop
turn with `QTimer::singleShot( 0, this, ... )` guarded by `shuttingDown`, and only when
`cfg.preferences.enableScanPopupHotkey` is true; `stop()` runs when that preference is cleared. The
reconciliation happens in the same method that re-installs the hotkeys, so accepting the Preferences
dialog starts or stops monitoring immediately. `stop()` is called **before
the popup reset** at every teardown site:

* `~MainWindow` (`mainwindow.cc:1026-1031`),
* `quitApp()` (`mainwindow.cc:1272-1293`),
* `finishShutdown()` (`mainwindow.cc:1308-1315`),
* `failStartup()` (`mainwindow.cc:1317-1335`) — it already calls `scanPopup.reset()` at
  `mainwindow.cc:1330`, so `stop()` goes immediately before that reset.

The dictionary-reset sites that recreate the popup (`mainwindow.cc:2143`, `2224`, `3713` and
`makeScanPopup()` at 1521) must **not** stop the source: the watcher survives popup recreation.

### 6.7 Popup entry point (F10, F16)

```cpp
bool ScanPopup::translateSelectedText( QString const & text )
{
  Config::InputPhrase const phrase = cfg.preferences.sanitizeInputPhrase( text );
  if( !phrase.isValid() )
    return false;                       // caller reports it through the ladder above
  if( isVisible() && phrase == inputPhrase )
    return true;                        // same-phrase dedupe, as handleInputWord() does
  translateWord( text );
  return true;
}
```

The recency decision lives in `MainWindow`, which owns both the source and the clipboard-change
timestamp; `ScanPopup` only sanitises, de-duplicates and translates.

It returns a bool instead of emitting a status message, because the popup's
`showStatusBarMessage()` writes to the popup's own status bar (`scanpopup.cc:1154`), which does not
exist when the popup never opens.

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
| `Starting` | `selection source starting` |
| `Available`, `primarySelectionSeen` | `selection readable` |
| `Available`, not yet seen | `selection not observed yet` |
| `Unavailable` | `selection unavailable: <reason> — clipboard is used instead` |
| `Unsupported` | `selection unsupported in this build — clipboard is used instead` |

`<reason>` is `WaylandSelectionSource::reason()`. The portal-reported trigger stays the first part of
the string, so the existing "reported trigger is display-only" rule is untouched.

`primarySelectionSeen` flips from false to true while the state stays `Available`, so the row is
refreshed from `capabilityChanged()` as well as from `stateChanged()` (G7). The preferences dialog
connects both to the same re-render.

### 7.3 Implementation of the row (F8)

`Preferences::setHotkeyStatus()` gains one `SelectionStatus const &` parameter (§6.2), defaulted so
existing callers compile; it carries state, reason **and** `primarySelectionSeen`, which the renderer
needs but cannot query (H4). Its local `display` lambda becomes id-driven instead of the current
two-way ternary:

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

`MainWindow::refreshHotkeyStatus()` — already called on `bindingsChanged` and `stateChanged` — is also
called from `capabilityChanged()` and passes the snapshot, so an open Preferences dialog repaints when
the selection source becomes readable, fails, or starts/stops with the preference (G7, H4).

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

1. **Startup.** `MainWindow` creates the source; the first event-loop turn calls `start()`. Globals are
   collected until the `wl_display.sync` done callback closes the initial burst (§6.4). With the
   manager and a seat bound the state becomes `Available` and the preferences row is refreshed;
   otherwise `Unavailable` with the missing global named.
2. **Capture.** `data_offer` + `primary_selection` → the offer is held, debounced 200 ms, then read to
   EOF or the 1 MiB cap and decoded. Supersession retires the older token; its late completion is
   ignored.
3. **Activation.** Portal emits `Activated("scan-popup", token)`. The popup's native handle is
   materialised, the token applied, then `requestCapture( requestId )` is issued.
4. **Answer.** `captureFinished( requestId, text, capturedAt )` arrives within 300 ms — promoted read,
   cached value, or timeout with `capturedAt == 0` — and `MainWindow` applies the recency rule before
   `translateSelectedText( text )`.
5. **Raise.** `translateWord()` → `engagePopup()` → `show()`; focus is requested through the token and
   may be declined. If `scanToMainWindow` is set the phrase goes to the main window instead.

## 9. Error handling

| condition | behaviour |
| --- | --- |
| `wayland-client` missing at build | `HAVE_WAYLAND_DATA_CONTROL` undefined → `kHaveWaylandDataControl == false` → `State::Unsupported`; the binding is **not** added (the predicate takes the constant); the row shows the unsupported text (G1) |
| no `ext_data_control_manager_v1` (GNOME, wlroots without the ext protocol) | `Unavailable("no ext_data_control_manager_v1")`, one signal, no retry; shortcut falls back to the clipboard |
| no seat in the initial burst | stays `Starting` after the `sync` barrier; a later seat global is bound |
| bound seat removed (`global_remove`) | device and offers destroyed, another seat bound if present, else `Unavailable("seat removed")` (G4) |
| bound manager removed (`global_remove`) | `Unavailable("manager removed")` |
| manager bound, no `primary_selection` ever | `Available` with `primarySelectionSeen == false`; row says "not observed yet" |
| device `finished` | destroy device and offers, `Unavailable("device finished")`, cached text kept |
| offer without a text MIME | offer destroyed, cached text untouched, logged |
| read idle timeout (250 ms since the last byte) | read end closed, payload dropped, offer destroyed, cached text untouched |
| absolute transfer deadline (2 s from `receive()`) | same as the idle timeout (G2) |
| payload over 1 MiB | read stops at the cap, payload marked truncated, decoded and sanitised |
| invalid UTF-8 / embedded NUL | replaced / stripped, then sanitised; an invalid phrase opens nothing |
| protocol error confined to the private connection | stop, one `Unavailable`, app keeps running |
| compositor restart | the private connection and Qt's own Wayland connection die together; Qt terminates the process. The old "app keeps running" claim is withdrawn (F21) |
| activation with no pending capture | answered immediately from `lastText()` with `capturedAt` = that capture's stamp (0 if the cache is empty); the recency rule then decides selection vs clipboard (H1) |
| activation while a capture is pending | the read is promoted and answered on completion, bounded at 300 ms; on timeout the answer is clipboard-only with `capturedAt = 0`, logged `selection-timeout` (G5, H1) |
| empty selection and empty clipboard | nothing opens; the message uses the first available channel — main window status bar, else tray message, else log only (G6, H5) |

## 10. Testing

### 10.1 New suite `tests/tst_waylandselection`

Links `waylandselectionlogic.cc` only, so it needs no compositor and no libwayland. The suite must
also be registered in the closed inventory (F3): `tests/tests.pro:123-138` enumerates
`GD_REQUIRED_SUITE_NAMES` and enforces `count( GD_TEST_NAMES, 19 )`. The change is: add
`tst_waylandselection` to that list and raise both the count and the message string to 20, then
regenerate `Makefile.tests` with `make tests-makefile`.

Cases:

* `mimePreferenceOrder` — the four accepted types in priority order; unknown types rejected.
* `payloadClampIsAppliedWhileReading` — `PayloadAccumulator` fed in chunks keeps exactly
  `MaxPayloadBytes` and latches `truncated()`; the boundary byte counts, a single oversized chunk, and
  a chunk that exactly fills the cap (F6/G8).
* `decodeReplacesInvalidUtf8`, `decodeStripsNul` — NUL is removed, not preserved (F15).
* `chooseTextUsesRecency` — selection newer than the clipboard wins; a clipboard change after the
  capture wins; equal stamps give the selection; a whitespace-only selection falls back; a never-changed
  clipboard (`0`) always yields the selection; both empty yields empty (H1).
* `mimeAttemptsAdvanceAndExhaust` — `pendingOffer()` with four advertised types, `beginMimeAttempt()`
  + `currentMimeType()` + `mimeAttemptFailed()` walk the preference order and return false after the
  last acceptable type; a retired offer refuses immediately (H6).
* `payloadAtCapIsNotTruncated` / `payloadOverCapIsTruncated` — exactly `MaxPayloadBytes` versus
  `MaxPayloadBytes + 1`, proving the one-byte probe (H10).
* `offerSupersessionRetiresToken` — a read token for offer A is rejected after offer B arrives (F4b).
* `clipboardOfferIsDiscarded` — the `selection` channel never yields a read (F4d).
* `retiredOfferIsNeverReused` — `beginMimeAttempt()` returns false after retirement, and after the
  last offered MIME type has been tried (G8).
* `nullOfferClearsWithoutRead` — a `primary_selection` with `ChannelOffer{0}` clears the pending
  capture and starts no read (F4).
* `activationPlanPromotesPendingCapture` — `planActivation()` returns `AwaitPending` while a capture
  is pending and `UseCached` otherwise (G5/G9).

### 10.2 Extended `tests/tst_hotkeys`

* `scanPopupBindingIsPortalOnly` — the real `scanPopupBindingPlanned()` over `xcb`, `wayland`,
  `wayland-egl`, `offscreen`, the preference on/off, and `buildSupportsSelection` false (F18).
* `portalIdPredicateAcceptsThreeIds` — the single shared `isKnownPortalBindingId()` over the three ids
  and a rejection case, plus a source assertion that neither `portalhotkeys.cc` nor `config.cc` keeps a
  second copy (F2, H11).
* `capabilityConstantMatchesBuild` — the suite compiles `HAVE_WAYLAND_DATA_CONTROL` under the same
  pkg-config condition as `goldendict.pro`, and asserts `kHaveWaylandDataControl` is exactly that
  value; a source assertion checks that `goldendict.pro` guards the define with the `wayland-client`
  probe (G1, H7).
* `sourceFollowsThePreference` — a source/behaviour assertion that the startup path starts the source
  only when `enableScanPopupHotkey` is true and that clearing the preference stops it (H12).
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
and asserts the exact rendered label for the third row — `Starting`, `Available` with and without
`primarySelectionSeen`, `Unavailable` with a reason, `Unsupported`, and the disabled xcb text — plus a
`capabilityChanged()` re-render (F8, F17, G7).

### 10.4 Live protocol gate (G9)

`tests/tst_hotkeys` does **not** link `MainWindow`, `ScanPopup` or the new source — its nested-KWin
probe only checks backend creation and portal binding (`tst_hotkeys.cc:1606`). The live gate is
therefore a separate script, in the pattern of `scripts/verify-test-gate.sh`, not a claim about the
existing suite:

`scripts/verify-wayland-selection-live.sh` builds a small runner that links the **real**
`waylandselection.cc` + `waylandselectionlogic.cc`, starts a nested `kwin_wayland`, sets the primary
selection from a helper client, and asserts:

* the source reaches `Available` with `primarySelectionSeen == true`;
* the captured text equals what the helper set, for a multi-chunk payload (proving EOF handling, not
  a single short read);
* a burst of ten rapid selections produces exactly one read (the debounce);
* a payload larger than `MaxPayloadBytes` is captured truncated, not dropped;
* a superseded selection never overwrites the newer capture (token retirement).

The activation path is proven separately and does not need the compositor: `planActivation()` is unit
tested (10.1), the wiring is pinned by the source assertions and `threeBindingsStayBound` (10.2/10.3),
and the end-to-end popup remains the manual acceptance of 10.5. This section makes no claim that the
nested-KWin harness proves the popup opens.

**Log contract** the script asserts (defined here, not invented by the test): `gdDebug` lines
`selection-source: state=<state> reason=<reason>`, `selection-captured mime=<type> bytes=<n>
truncated=<0|1>`, `selection-ignored reason=<no-text-mime|stale-token|timeout>`.

### 10.5 Acceptance on the reporter's session

1. Select a word in another application, press the shortcut → the popup shows that word.
2. Copy a different word (leaving the old selection untouched), press the shortcut → the popup shows
   the **copied** word, because the clipboard change is newer (H1).
3. Press the shortcut immediately after selecting a new word → the new word, never the previous one
   (promoted capture).
4. With neither a selection nor clipboard text, press the shortcut → the message lands in the first
   available channel: main window status bar, else tray message, else the log only.

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
* **Provenance**: the data-control protocols carry no owner identity, so the source cannot tell a
  selection made in another application from one made inside GoldenDict. Both are stored, and the
  shortcut translates the current primary selection whichever client set it (G10).

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
