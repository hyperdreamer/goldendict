# Wayland scan popup: implementation specification

Authority: `2026-11-04-wayland-scan-popup-design.md`, revision 7, commit
`dd8c2895`. Source baseline: this worktree at `dd8c2895`. All `file:line`
anchors below refer to that baseline, before implementation; insertions are
identified by their enclosing function and neighbouring statement as well as
by line. This document specifies the implementation to be made subsequently;
writing this document does not implement or validate the feature.

## 1. Scope and decisions

The two fixed decisions are **shortcut-only triggering on Wayland** and an own
client for **`ext_data_control_manager_v1` only**. The client has a private
Wayland connection and no helper process or service dependency. Its only new
optional shared-library dependency is `libwayland-client`. The UI choice is the
third row in the existing Hotkeys tab, sharing its status and Configure controls.

The action ID is `scan-popup`, the portal description is `Translate the selected
text`, and the preference defaults to off. The default key is constructed as
`Config::HotKey( QKeySequence( Qt::CTRL | Qt::META | Qt::Key_D ) )`; its
PortableText is `Meta+Ctrl+D` and its preferred portal trigger is `CTRL+LOGO+d`.
Do not use the string `Ctrl+Win+D` or the two-argument sequence constructor.
The existing trigger converter rejects a keyless combination at
`hotkeysyntax.cc:201`, with the message at `hotkeysyntax.cc:205`; this feature
therefore uses a discrete key combination.

Registering a shortcut and reading the primary selection are separate
capabilities. Wayland plus the enabled preference registers the action even in
a build without selection capture; that build translates the clipboard.
Capture additionally requires the build capability. An enabled source observes
the current primary selection, including selections made within GoldenDict.
Selection and clipboard are compared by timestamps from one monotonic clock;
a copy newer than the selection wins. An activation promotes a pending capture
and settles within 300 ms. Timeout suppresses the old selection for that
activation. Successful translation uses the existing popup translation path,
including the send-to-main-window preference.

There is no new X11 runtime action, selection watcher, modifier rule or popup
creation reason. The new row remains visible but disabled on every non-Wayland
platform, and accepting that dialog preserves the two stored Wayland values.
The existing X11 modifier test is in `scanpopup.cc:571`; retain it unchanged.
The Wayland clipboard watch and mouse-hover guards are in `scanpopup.cc:503`
and `scanpopup.cc:530`; retain their behaviour unchanged.

Explicit non-goals: automatic popup on selection, held-modifier emulation,
`zwlr_data_control_v1`, GNOME/Mutter selection capture, owner/application
identification, multi-seat aggregation, clipboard-watch changes, clipboard
hotkey changes, and X11 focus-restoration changes. Compositor focus grants and
survival of a compositor restart are not promised.

**Evidence boundary.** Logic tests prove pure decisions. `tst_hotkeys` proves
configuration, portal binding and the Preferences renderer. MainWindow wiring
is pinned by source assertions because that suite does not link MainWindow,
ScanPopup or the source (`tests/tst_hotkeys/tst_hotkeys.pro:37`,
`tests/tst_hotkeys/tst_hotkeys.pro:51`). The live script links the real source
and tests transfers; it does not prove the popup opens. The actual popup and
visible notification ladder remain manual acceptance checks. A source assertion
is never described as a runtime observation.

## 2. File inventory and build contract

This inventory describes the subsequent feature patch. No other production or
test file is needed. Generated Makefiles and script scratch files are build
outputs, not repository additions.

| File | Change |
| --- | --- |
| `waylandselectionlogic.hh` | Create the complete pure API in §3.1. |
| `waylandselectionlogic.cc` | Implement the clock, MIME ordering, bounded payload, decoding, recency and token/offer decisions. Qt Core only. |
| `waylandselection.hh` | Create the complete source API, capability constant and status snapshot in §3.2. No Wayland headers in this public header. |
| `waylandselection.cc` | Implement the private-connection source and the unsupported stub in the same translation unit. |
| `protocols/ext-data-control-v1/ext-data-control-v1.xml` | Vendor the unmodified staging XML from wayland-protocols 1.50, retaining its licence. |
| `protocols/ext-data-control-v1/ext-data-control-v1-client-protocol.h` | Commit scanner-generated client header. |
| `protocols/ext-data-control-v1/ext-data-control-v1-protocol.c` | Commit scanner-generated private code. Compile as C, not as an included C++ implementation. |
| `protocols/ext-data-control-v1/regenerate.sh` | Create the executable, deterministic regeneration entry point specified below. |
| `goldendict.pro` | Always compile the two new C++ modules and MOC the source header; conditionally add protocol C, includes, library and feature define; correct aggregate-count comments to 20. |
| `config.hh` | Add the two preference members. |
| `config.cc` | Add defaults, load/save entries and use the shared action-ID predicate. Remove its local predicate definition. |
| `hotkeybackend.hh` | Publish one three-ID predicate and the two pure platform predicates; update the binding-ID comment. |
| `portalhotkeys.cc` | Delete the local `knownBindingId()` and use the shared predicate. |
| `mainwindow.hh` | Source ownership, request counter, clipboard stamp and reconciliation/reporting helpers. |
| `mainwindow.cc` | Reconcile monitoring, bind/dispatch the action, timestamp clipboard changes, apply recency, push snapshots, and stop at all four teardown sites. |
| `scanpopup.hh` | Declare `translateSelectedText()`. |
| `scanpopup.cc` | Implement that entry point without changing scanning or clipboard-hotkey paths. |
| `preferences.hh` | Include the snapshot type; extend renderer signature, edited flag and checkbox slot. |
| `preferences.cc` | Initialise/edit/save the third row, preserve non-Wayland values, render the status snapshot and capability note. |
| `preferences.ui` | Third checkbox/editor/status label in the existing Hotkeys layout. |
| `tests/tst_waylandselection/tst_waylandselection.pro` | Create the logic-only suite project. |
| `tests/tst_waylandselection/tst_waylandselection.cc` | Create the exact tests in §8.1. |
| `tests/tst_hotkeys/tst_hotkeys.pro` | Explain the header-only snapshot dependency; retain its production link closure. Do not link the real source or MOC its header here. |
| `tests/tst_hotkeys/tst_hotkeys.cc` | Add the named tests, UI matrix, child probe and source assertions in §8.2–8.3. Use existing isolated-config helpers, with generated per-test XML amendments rather than new fixture files. |
| `tests/tests.pro` | Register `tst_waylandselection` in the closed, sorted inventory and update all specified counts. |
| `tests/testsupport.pri` | Mirror the optional feature-definition probe, without imposing a Wayland link on logic-only suites. |
| `scripts/verify-wayland-selection-live.sh` | Create the executable live gate; embed its runner/helper/project text in the script and build it in a temporary directory. |
| `docs/wayland-limitations.md` | Amend §2 with the coverage, fallback and deferred limits in §10, retaining hover/focus limits and the distinction between evidence and unobserved acceptance. |
| `README.md` | Amend the platform-limits sentence with ext protocol coverage and clipboard fallback. |

### 2.1 Optional feature wiring

The application sets C++17 at `goldendict.pro:29`. Its Linux/Unix feature and
pkg-config block starts at `goldendict.pro:146` and `goldendict.pro:152`.
Inside that `unix:!mac` block, immediately after the existing
`CONFIG += link_pkgconfig`, add this block:

```qmake
packagesExist(wayland-client) {
    DEFINES += HAVE_WAYLAND_DATA_CONTROL
    PKGCONFIG += wayland-client
    INCLUDEPATH += $$PWD/protocols/ext-data-control-v1
    SOURCES += $$PWD/protocols/ext-data-control-v1/ext-data-control-v1-protocol.c
}
```

Add `waylandselectionlogic.hh` and `waylandselection.hh` to the unconditional
header list beginning at `goldendict.pro:282`, and the two C++ implementations
to the unconditional source list beginning at `goldendict.pro:435`. The stub
therefore builds on Windows, macOS and Unix without the package. Do not add a
scanner invocation to a normal build.

In the existing `unix:!mac` feature block at `tests/testsupport.pri:84`, use:

```qmake
CONFIG += link_pkgconfig
packagesExist(wayland-client) {
    DEFINES += HAVE_WAYLAND_DATA_CONTROL
}
```

This mirrors the application's condition and definition. It does not add
`PKGCONFIG += wayland-client` to every test: including a constant and an enum
needs no Wayland symbols. `tst_hotkeys` includes `waylandselection.hh` through
`preferences.hh`; it must not list that Q_OBJECT header in its `HEADERS`,
which would generate a source-class MOC without its implementation. The live
runner separately links the real implementation and protocol C.

### 2.2 Vendoring

The XML input has SHA-256
`293deba5dfdb974fbcf8dcf891a30a51a36a8bba0047dc6bdf6a03194089b7fb`.
The generation tool for the committed pair is wayland-scanner 1.26.0. The
executable `regenerate.sh` is exactly:

```sh
#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
wayland-scanner client-header ext-data-control-v1.xml ext-data-control-v1-client-protocol.h
wayland-scanner private-code ext-data-control-v1.xml ext-data-control-v1-protocol.c
```

Retain the generated file banners and XML copyright. Regenerating with the
pinned scanner must leave both generated files byte-identical. No production
code uses `create_data_source`, `set_selection` or `set_primary_selection`:
those requests are for the live script's helper only.

### 2.3 Closed inventory counts

The current inventory starts at `tests/tests.pro:123`; its count/error check is
at `tests/tests.pro:137`, its message at `tests/tests.pro:144`, and its header
comment at `tests/tests.pro:9`. Append `tst_waylandselection` after
`tst_utf8_wstring` and before `tst_website_frames`, producing this exact list:

```qmake
GD_REQUIRED_SUITE_NAMES = \
    tst_article_bridge tst_article_history tst_article_load \
    tst_article_schemes tst_article_stream tst_btree_index tst_encoding \
    tst_external_content tst_hotkeys tst_htmlescape tst_langcoder \
    tst_linkrewrite tst_platformwindow \
    tst_portal_hotkeys tst_print_pipeline tst_regex tst_save_article \
    tst_utf8_wstring tst_waylandselection tst_website_frames
```

Change `count(GD_TEST_NAMES, 19)` to `count(GD_TEST_NAMES, 20)`, the error to
`Expected exactly 20 required suites; found: $$GD_TEST_NAMES`, the message to
`tests/tests.pro: exact 20 suites: $$GD_TEST_NAMES`, and the header comment to
`exactly the 20 required suites`.

The application comment actually still says **18**, not 19, at
`goldendict.pro:758` and `goldendict.pro:759`. Replace that comment block with:

```qmake
# The suites are independent projects with private object directories, so the
# build half runs in parallel for all 20 suites. The run half stays serial for
# deterministic test execution within the gate's wall-clock budget.
```

Do not relabel the old measurement as a measurement of 20 suites. Regenerate
`tests/Makefile.tests` using `make tests-makefile`; that target's existing recipe
is at `goldendict.pro:753`. No checked-in Makefile is introduced.

## 3. New module contracts

### 3.1 Complete `waylandselectionlogic.hh`

Use this complete header. The API is independent of QObject, clipboard objects,
Wayland proxies, sockets and timers.

```cpp
#ifndef WAYLANDSELECTIONLOGIC_HH_INCLUDED
#define WAYLANDSELECTIONLOGIC_HH_INCLUDED

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class Monotonic final
{
public:
  static qint64 nowMs() noexcept;
};

namespace WaylandSelection {

inline constexpr int MaxPayloadBytes = 1024 * 1024;
inline constexpr int DebounceMs = 200;
inline constexpr int IdleTimeoutMs = 250;
inline constexpr int TransferTimeoutMs = 2000;
inline constexpr int ActivationTimeoutMs = 300;

QString chooseMimeType( QStringList const & offered );

class PayloadAccumulator
{
public:
  explicit PayloadAccumulator( int cap = MaxPayloadBytes );
  void append( QByteArray const & chunk );
  bool full() const noexcept;
  bool truncated() const noexcept;
  QByteArray payload() const;

private:
  int cap_;
  QByteArray data_;
  bool truncated_ = false;
};

QString decodePayload( QByteArray const & raw );
bool clipboardStampAdvances( bool modeIsClipboard ) noexcept;
QString chooseText( QString const & selection, qint64 capturedAt,
                    QString const & clipboard, qint64 clipboardChangedAt );

enum class ActivationPlan { UseCached, AwaitPending };
ActivationPlan planActivation( bool hasPendingCapture ) noexcept;

using ReadToken = quint64;

struct ChannelOffer
{
  quint64 id = 0;
  operator bool() const noexcept { return id != 0; }
};

class OfferState
{
public:
  void pendingOffer( quint64 offerId, QStringList const & mimeTypes );
  ChannelOffer takeForPrimary();
  ChannelOffer discardForClipboard();
  bool beginMimeAttempt();
  QString currentMimeType() const;
  void mimeAttemptFailed() noexcept;
  ReadToken beginRead() noexcept;
  bool accepts( ReadToken token ) const noexcept;
  void retire() noexcept;
  void completed( ReadToken token ) noexcept;

private:
  quint64 pendingId_ = 0;
  QStringList pendingMimeTypes_;
  quint64 primaryId_ = 0;
  QStringList mimeTypes_;
  int nextMime_ = 0;
  int currentMime_ = -1;
  ReadToken nextToken_ = 0;
  ReadToken activeToken_ = 0;
};

} // namespace WaylandSelection

#endif // WAYLANDSELECTIONLOGIC_HH_INCLUDED
```

`Monotonic::nowMs()` is defined once in the `.cc` using
`std::chrono::steady_clock`. A function-local static epoch is captured on its
first call; return `1 + duration_cast<milliseconds>(now - epoch).count()`.
Thus all callers have the same process epoch, real stamps are positive, and 0
unambiguously means never/no backing capture. This is a static function, not
an elapsed-timer instance method. Equal millisecond stamps are allowed.

Pure functions accept immutable values and are reentrant. Distinct accumulator
and offer-state instances can be used on different threads; an individual
mutable instance has one owner and is not concurrently mutated. The production
owner is the GUI thread. Return values own their data through Qt value types.
The `noexcept` methods perform only scalar state changes or read-only checks.
In particular `retire()` marks the MIME list inactive with indices/IDs rather
than detaching or clearing a shared Qt container; the next non-noexcept
`takeForPrimary()` replaces that storage. Allocation/copy-producing interfaces
do not promise `noexcept`.

MIME acceptance is case-sensitive and in exactly this order, regardless of
advertisement order: `text/plain;charset=utf-8`,
`text/plain;charset=UTF-8`, `UTF8_STRING`, `text/plain`. Ignore duplicate
advertisements and all other types. `chooseMimeType()` returns the first
accepted type or an empty QString. Do not normalise case or add codecs.

`PayloadAccumulator` requires `cap >= 0`; assert that precondition. It starts
empty and untruncated. `append()` retains at most the remaining capacity and
sets the truncation latch only when the chunk contains bytes beyond it. The
latch never resets. `full()` means `data_.size() == cap_`, including at cap 0;
fullness alone is not truncation. Appending an empty chunk changes nothing.
`payload()` returns the retained prefix. No append first constructs an
unbounded copy of the input. The source itself limits every read allocation
as described in §6.4.

`decodePayload()` uses `QString::fromUtf8(raw.constData(), raw.size())`, so
invalid sequences use U+FFFD, and then removes every U+0000. Length is explicit:
an embedded NUL never terminates decoding. Phrase sanitisation happens later.

`clipboardStampAdvances(modeIsClipboard)` returns that boolean unchanged.
MainWindow supplies the comparison to `QClipboard::Clipboard`; this is the
only rule that decides which clipboard modes advance the stamp.

`chooseText()` treats `text.trimmed().isEmpty()` as empty for either candidate,
but returns the chosen original QString, not the trimmed version. Preconditions:
both stamps are non-negative; a real selection capture has a positive stamp.
Apply the ordered rules in §7.1 exactly. A caller suppressing an invalid or
timed-out capture supplies an empty selection and stamp 0.

`planActivation(false)` is `UseCached`; `planActivation(true)` is
`AwaitPending`. Neither function reads external state.

OfferState is a decision record, not a proxy owner. Source-assigned offer IDs
are increasing nonzero integers, distinct for the source lifetime; they are
not reusable Wayland object numbers or pointer addresses. Read tokens also
increase for the lifetime of this OfferState. Never reset `nextToken_` when
retiring or accepting a new primary offer. Zero is never a valid read token.
The source owns and destroys the proxies associated with these IDs.

| Operation | Preconditions and exact postconditions |
| --- | --- |
| Default construction | No pending/primary offer, no MIME selected and no accepted token. |
| `pendingOffer(id, types)` | Replace only the pending candidate and its MIME list. Leave the accepted primary and its token alone: a clipboard offer must not cancel a primary read. `id == 0` represents a null channel event and ignores `types`. No read is started. |
| `takeForPrimary()` | Save the pending ID/types, retire the previous primary/token, consume the pending candidate, and accept the saved nonzero ID with the distinct acceptable MIME list in preference order. Return that ID. For null, return `{0}`, clear the primary state and accept no token. The previous token is rejected in both cases. |
| `discardForClipboard()` | Consume/return the pending candidate and clear its MIME list; leave primary state and its token unchanged. Caller destroys the returned nonzero proxy without receiving data. |
| `beginMimeAttempt()` | With no primary or no remaining acceptable MIME return false. If a current attempt has already been selected and not failed, return true without advancing it. Otherwise select the next untried MIME and advance `nextMime_`; return true. |
| `currentMimeType()` | Return the currently selected type, or empty when no attempt is selected. |
| `mimeAttemptFailed()` | Invalidate the current token and deselect the current MIME. The next `beginMimeAttempt()` selects the next untried type. Calling it without a current attempt is a no-op. |
| `beginRead()` | Require a live primary and selected MIME with no active token; return a newly reserved nonzero token. If these conditions are absent return 0, leaving state unchanged. It does not touch a file descriptor. |
| `accepts(token)` | True exactly for a nonzero token equal to the active token of the live primary. |
| `retire()` | Clear primary ID, MIME/attempt state and active token; retain the pending candidate and the increasing token counter. Idempotent. |
| `completed(token)` | If `accepts(token)` is false, change nothing. Otherwise consume the token and retire the accepted primary; it cannot be read a second time. Cache publication and proxy destruction belong to the source. |

### 3.2 Complete `waylandselection.hh`

```cpp
#ifndef WAYLANDSELECTION_HH_INCLUDED
#define WAYLANDSELECTION_HH_INCLUDED

#include <QObject>
#include <QString>
#include <QtGlobal>
#include <memory>

#ifdef HAVE_WAYLAND_DATA_CONTROL
inline constexpr bool kHaveWaylandDataControl = true;
#else
inline constexpr bool kHaveWaylandDataControl = false;
#endif

class WaylandSelectionSource : public QObject
{
  Q_OBJECT

public:
  enum class State { Unsupported, Starting, Available, Unavailable };

  explicit WaylandSelectionSource( QObject * parent = nullptr );
  ~WaylandSelectionSource() override;

  State state() const noexcept;
  QString reason() const;
  bool primarySelectionSeen() const noexcept;
  QString lastText() const;
  bool hasPendingCapture() const noexcept;

public slots:
  void start();
  void stop();
  void requestCapture( quint64 requestId );

signals:
  void stateChanged( State state, QString const & reason );
  void capabilityChanged();
  void captureFinished( quint64 requestId, QString const & text, qint64 capturedAt );

private:
  class Impl;
  std::unique_ptr< Impl > d;
};

struct SelectionStatus
{
  WaylandSelectionSource::State state = WaylandSelectionSource::State::Unsupported;
  QString reason;
  bool primarySelectionSeen = false;
};

Q_DECLARE_METATYPE( WaylandSelectionSource::State )

#endif // WAYLANDSELECTION_HH_INCLUDED
```

The constructor opens no connection. It records build/QPA support and starts
with empty cache, stamp 0, no pending capture, false capability and empty
reason. Its reported state before `start()` is `Starting` for a supported
Wayland instance and `Unsupported` otherwise. The implementation includes the
logic header and calls the shared capture predicate; all Wayland/POSIX
includes and operations are inside `HAVE_WAYLAND_DATA_CONTROL`.

The source's private implementation must use these exact entry points in
`waylandselection.cc`; they are internal contracts for the source assertions,
not additional public API:

```cpp
// Members of WaylandSelectionSource::Impl, with data == Impl* in listeners.
bool flushRequests();
void dispatchDisplayReadable();
void dispatchDisplayWritable();
bool startMimeAttempt();
void drainPipe( WaylandSelection::ReadToken token );
void checkTransferDeadline( WaylandSelection::ReadToken token );
void retirePrimary();
void dispose();

static void registryGlobal( void * data, wl_registry * registry,
                            uint32_t name, char const * interface, uint32_t version );
static void registryGlobalRemoved( void * data, wl_registry * registry, uint32_t name );
static void discoveryDone( void * data, wl_callback * callback, uint32_t callbackData );
static void dataOffer( void * data, ext_data_control_device_v1 * device,
                       ext_data_control_offer_v1 * offer );
static void offeredMime( void * data, ext_data_control_offer_v1 * offer,
                         char const * mimeType );
static void selection( void * data, ext_data_control_device_v1 * device,
                       ext_data_control_offer_v1 * offer );
static void primarySelection( void * data, ext_data_control_device_v1 * device,
                              ext_data_control_offer_v1 * offer );
static void deviceFinished( void * data, ext_data_control_device_v1 * device );
static void seatCapabilities( void * data, wl_seat * seat, uint32_t capabilities );
static void seatName( void * data, wl_seat * seat, char const * name );
```

All these declarations/definitions are in the guarded implementation, not the
public header. `flushRequests()` returns true while the connection remains live
(including an EAGAIN with an armed writer), false after fatal disposal.
`startMimeAttempt()` returns true only if it has installed a new live pipe;
false means no attempt was started. Seat callbacks consume but otherwise
ignore their payload: no keyboard/pointer client is created. Use the generated
listener layouts and these exact static signatures. Timer/notifier lambdas
capture their token and call the two token-parameter methods. `retirePrimary`
invalidates the accepted token before notifier/pipe/proxy disposal. `dispose()`
is idempotent and permanently retires this private connection and its protocol
callbacks. Keep explicit owner disposal separate from connection failure:
`stop()` sets an `ownerStopped` flag before disposal; a terminal Unavailable
failure disposes the connection but leaves `ownerStopped` false, so later
no-pending requests can still receive the retained cache synchronously. Those
requests never reopen a connection. Only explicit stop/destruction suppresses
all subsequent API answers.

All source construction, accessors, calls, timer/notifier callbacks and signal
emissions occur on its QObject/GUI thread. Assert thread affinity in mutating
entry points. No worker thread, blocking roundtrip, blocking read, GUI event
pumping or nested event loop is used. Signals carry value snapshots; a receiver
that retains a const-reference parameter must copy it.

`reason()` is non-empty if and only if `state() == Unavailable`.
`primarySelectionSeen()` latches true on the first primary event, including
null; it is never inferred from non-empty text. `lastText()` is the latest
successfully decoded payload and survives null events, bad offers, failed
transfers and source failure. A successful empty payload can make it empty;
its returned capture stamp is then 0. `hasPendingCapture()` is true for an
accepted held offer or an active read, including MIME retry intervals; a
clipboard offer and a settled cache do not count.

`start()` is idempotent. Unsupported instances open nothing and remain
Unsupported. A supported instance starts its one private connection once.
An Unavailable or explicitly stopped instance is never restarted. `stop()`
is terminal disposal: cancel all timers, detach any interested request without
emitting `captureFinished`, invalidate tokens, close reads, disable/remove
notifiers and release every proxy/connection. It is idempotent and the
destructor calls it. Explicit disposal freezes the last published state/reason;
it introduces no fifth public state or synthetic failure message. The owner
immediately destroys the instance on preference disable, or retains only that
stopped instance during teardown. There are no signals or successful capture
publications after stop, even from already queued callbacks. Calls to
`requestCapture()` after stop are ignored; the request guarantee applies to a
live, not disposed instance.

`requestCapture(id)` requires a nonzero ID newer than any ID previously supplied
by that caller. There is at most one interested ID, one activation timer and
one physical transfer. Its complete answer/overlap rules are in §6.6. A live
unsupported/unavailable/starting instance with nothing pending answers
synchronously from its cache. Use positive `capturedAt` only for the exact
capture backing the returned non-empty text; zero means there is no usable
backing capture. Do not substitute the transfer-finish time.

Snapshot ownership is exclusively MainWindow's. Preferences never receives a
source pointer, constructs a source, or connects directly to it. Source state
changes and capability changes both cause MainWindow to push a new snapshot.

## 4. Existing-code integration points

### 4.1 Configuration and shared hotkey contracts

Insert these adjacent fields immediately after `clipboardHotkey` at
`config.hh:314`, before `enableScanPopup`:

```cpp
bool enableScanPopupHotkey;
HotKey scanPopupHotkey;
```

Immediately after the clipboard-hotkey initialiser at `config.cc:314`, add:

```cpp
enableScanPopupHotkey( false ),
scanPopupHotkey( QKeySequence( Qt::CTRL | Qt::META | Qt::Key_D ) ),
```

The exact load/save insertions are §5. They belong after the clipboard entries
and before `enableScanPopup`, at `config.cc:972` and `config.cc:1868`.

In `hotkeybackend.hh`, between the config include (`hotkeybackend.hh:26`)
and the binding comment (`hotkeybackend.hh:28`), define:

```cpp
inline bool isKnownPortalBindingId( QString const & id )
{
  return id == QLatin1String( "main-window" )
      || id == QLatin1String( "clipboard-lookup" )
      || id == QLatin1String( "scan-popup" );
}

inline bool scanPopupBindingPlanned( QString const & platformName,
                                     bool preferenceEnabled ) noexcept
{
  return preferenceEnabled && platformName.startsWith( QLatin1String( "wayland" ) );
}

inline bool scanPopupCaptureSupported( bool buildSupportsSelection,
                                       QString const & platformName ) noexcept
{
  return buildSupportsSelection && platformName.startsWith( QLatin1String( "wayland" ) );
}
```

Change the comment at `hotkeybackend.hh:28` to name all three stable IDs.
These predicates use the case-sensitive QPA prefix `wayland`, matching the
design and the production caller; they do not interpret environment variables.

Add `#include "hotkeybackend.hh"` to `config.cc` after its hotkey syntax include
at `config.cc:6`. Delete the local predicate definition at `config.cc:179`.
The load and save filters at `config.cc:198` and `config.cc:2324` then call the
single global inline function unchanged. Delete `knownBindingId()` at
`portalhotkeys.cc:82` and replace its call in `PortalBackend::beginGeneration()`
at `portalhotkeys.cc:852` with `isKnownPortalBindingId(binding.id)`.
This is necessary because that function currently invalidates the whole request
set for an unknown ID, leading to the message at `portalhotkeys.cc:872`.

### 4.2 MainWindow declarations and clipboard stamp

Add `#include "waylandselection.hh"` and `#include "waylandselectionlogic.hh"`
beside the hotkey includes at `mainwindow.hh:34`. Immediately **before** the
popup member at `mainwindow.hh:181`, declare:

```cpp
std::unique_ptr< WaylandSelectionSource > selectionSource;
quint64 scanPopupRequestId = 0;
qint64 clipboardChangedAt = 0;
```

Keep source declaration before both `scanPopup` and `articleProfile`
(`mainwindow.hh:183`), so reverse member destruction outlives those consumers.
Near `refreshHotkeyStatus()` at `mainwindow.hh:113`, add:

```cpp
void reconcileSelectionSource();
void reportNothingToTranslate();
```

Immediately before the constructor's `makeScanPopup()` at
`mainwindow.cc:837`, install this connection **once**:

```cpp
connect( QApplication::clipboard(), &QClipboard::changed, this,
         [this]( QClipboard::Mode mode ) {
  if( WaylandSelection::clipboardStampAdvances( mode == QClipboard::Clipboard ) )
    clipboardChangedAt = Monotonic::nowMs();
} );
```

Do not reset the stamp when recreating the popup/source or accepting
Preferences. It remains 0 until the first Clipboard-mode change after this
connection is installed. Selection and FindBuffer signals do not advance it.
It is independent of `trackClipboardChanges` and of whether scanning is on.

### 4.3 Source reconciliation and lifetime

At the beginning of `MainWindow::installHotKeys()`, immediately after its
`shuttingDown` guard at `mainwindow.cc:3213`, call
`reconcileSelectionSource()`, **before** the existing install-gate early return
at `mainwindow.cc:3231`. Otherwise an unchanged binding set could prevent a
capture capability/preference update from being applied.

`reconcileSelectionSource()` uses this exact controlling expression:

```cpp
bool const wanted = cfg.preferences.enableScanPopupHotkey &&
  scanPopupCaptureSupported( kHaveWaylandDataControl, QGuiApplication::platformName() );
```

If `!wanted`, and a source exists, increment `scanPopupRequestId`, call its
`stop()`, then reset the unique pointer. Refresh the status snapshot and return.
If wanted and the pointer already exists, keep it, including in terminal
Unavailable state; do not silently retry on every Preferences OK. If wanted
and absent, construct `std::make_unique<WaylandSelectionSource>(this)`, connect
its three signals once for that instance, refresh status, and schedule:

```cpp
QPointer< WaylandSelectionSource > const source( selectionSource.get() );
QTimer::singleShot( 0, this, [this, source] {
  if( shuttingDown || !source || source.data() != selectionSource.get() ) return;
  source->start();
} );
```

Use one connection of `stateChanged` to a lambda ignoring the two signal
arguments and calling `refreshHotkeyStatus()`, one typed connection of
`capabilityChanged` directly to `MainWindow::refreshHotkeyStatus`, and the one
capture connection in §4.5. Re-enabling constructs a fresh instance; the queued
start for an older instance cannot start its replacement. Connections live in
the creation block, not on every install and not on every popup creation.
This gives the design's once-connected handler semantics with opt-in creation.

The existing startup call is `mainwindow.cc:861`; the accepted/cancelled dialog
path reinstalls at `mainwindow.cc:2354` after getting values at
`mainwindow.cc:2248`. Retain those entry points. Both use the reconciliation
above, so monitoring changes as soon as the dialog result is applied.

Insert `if( selectionSource ) selectionSource->stop();` immediately before
each of these existing resets, with no intervening popup use:

| Teardown function | Baseline reset anchor | Additional requirement |
| --- | --- | --- |
| `MainWindow::~MainWindow()` | `mainwindow.cc:1031` | Set `shuttingDown = true` on entry, before backend/source operations. |
| `MainWindow::quitApp()` | `mainwindow.cc:1285` | Existing guard and shutdown flag remain; stop before the popup reset. |
| `MainWindow::finishShutdown()` | `mainwindow.cc:1312` | Stop even if quit already stopped it; disposal is idempotent. |
| `MainWindow::failStartup()` dialog-finished lambda | `mainwindow.cc:1330` | Stop in the lambda immediately before reset, not merely when opening the error dialog. |

Do not stop/reset the source at the dictionary/preferences recreation resets:
`mainwindow.cc:2143` (`editDictionaries()`), `mainwindow.cc:2224`
(`editPreferences()`), `mainwindow.cc:3713` (`on_rescanFiles_triggered()`), or
`mainwindow.cc:1521` (`makeScanPopup()`). Increment `scanPopupRequestId` on entry
to `makeScanPopup()` before resetting the popup, invalidating an activation
addressed to a replaced widget without disturbing background observation.
During periods with no popup the capture handler's `!scanPopup` guard simply
discards a response. No dictionary reset initiates a new display connection.

### 4.4 Registration and popup creation guard

After the two existing `add()` calls at `mainwindow.cc:3229` and
`mainwindow.cc:3230`, insert exactly:

```cpp
if( scanPopupBindingPlanned( QGuiApplication::platformName(),
                             cfg.preferences.enableScanPopupHotkey ) )
  add( true, "scan-popup", tr( "Translate the selected text" ), cfg.preferences.scanPopupHotkey );
```

Retain the shared install gate and portal display policy. Do not gate this
binding with `kHaveWaylandDataControl` or source availability. No X11 recorder
receives the new ID through this installation path.

Replace only the guard in `makeScanPopup()` at `mainwindow.cc:1523` with:

```cpp
if( !cfg.preferences.enableScanPopup &&
    !cfg.preferences.enableClipboardHotkey &&
    !scanPopupBindingPlanned( QGuiApplication::platformName(),
                              cfg.preferences.enableScanPopupHotkey ) )
  return;
```

The popup destructor calls `saveConfigData()` at `scanpopup.cc:336`, and that
method writes popup geometry at `scanpopup.cc:344`. The platform predicate is
therefore required: a synced Wayland preference alone must not construct and
later save an otherwise absent popup on xcb.

### 4.5 Activation, completion and empty reporting

Extend the existing activation lambda beginning at `mainwindow.cc:3240`, after
its clipboard branch at `mainwindow.cc:3248`. Keep the two old branches
unchanged. Add:

```cpp
else if( id == "scan-popup" && scanPopup )
{
  scanPopup->winId();
  PlatformWindow::applyActivation( scanPopup.get(), token );
  if( selectionSource )
  {
    quint64 const requestId = ++scanPopupRequestId;
    selectionSource->requestCapture( requestId );
  }
  else if( !scanPopup->translateSelectedText(
             QApplication::clipboard()->text( QClipboard::Clipboard ) ) )
    reportNothingToTranslate();
}
```

`PlatformWindow::detail::activateIn()` checks `windowHandle()` at
`platformwindow.cc:85`, before installing a token. The explicit `winId()` is
therefore before `applyActivation()`, never after it. Do not log/persist tokens
or claim the compositor granted activation.

Connect the new source's completion once in its creation block:

```cpp
connect( selectionSource.get(), &WaylandSelectionSource::captureFinished,
         this, [this]( quint64 requestId, QString const & selection, qint64 capturedAt ) {
  if( shuttingDown || requestId != scanPopupRequestId || !scanPopup ) return;
  QString const text = WaylandSelection::chooseText(
      capturedAt > 0 ? selection : QString(), capturedAt,
      QApplication::clipboard()->text( QClipboard::Clipboard ), clipboardChangedAt );
  if( !scanPopup->translateSelectedText( text ) )
    reportNothingToTranslate();
} );
```

Read clipboard text and its latest stamp at completion, not at keypress, so a
copy during the wait can win. The exact stale-ID comparison appears once.
There must be exactly two `"scan-popup"` literals in `mainwindow.cc`: the
binding and activation branch above. Do not introduce a third in a comment,
lookup or diagnostic.

`reportNothingToTranslate()` logs `selection-empty` once and uses this ladder:

```cpp
gdDebug( "selection-empty" );
QString const message = tr( "No selected text or clipboard text to translate" );
if( isVisible() )
  mainStatusBar->showMessage( message, 3000 );
else if( trayIcon )
  trayIcon->showMessage( tr( "GoldenDict" ), message, QSystemTrayIcon::Information, 3000 );
```

No modal dialog and no popup-owned status bar. A tray notification is an
attempt through the existing tray object, subject to desktop notification
policy. If hidden without a tray object, the log is the sole channel. The
current tray pointer and main status bar members are declared at
`mainwindow.hh:115` and `mainwindow.hh:146`.

### 4.6 Snapshot and ScanPopup entry point

Extend the call in `MainWindow::refreshHotkeyStatus()` at
`mainwindow.cc:3281` with a local default `SelectionStatus selection;`. If a
source exists, copy its `state()`, `reason()` and `primarySelectionSeen()` into
that value. Pass it as the final renderer argument. Otherwise pass the default
snapshot, even in an unsupported build. The disabled-row rendering in §7.3
avoids claiming a disabled watcher is an unsupported running watcher.

Declare `bool translateSelectedText( QString const & text );` immediately after
`translateWord()` at `scanpopup.hh:52`. Implement it immediately before the
existing `ScanPopup::translateWord()` definition at `scanpopup.cc:469`:

```cpp
bool ScanPopup::translateSelectedText( QString const & text )
{
  Config::InputPhrase const phrase = cfg.preferences.sanitizeInputPhrase( text );
  if( !phrase.isValid() )
    return false;
  if( isVisible() && phrase == inputPhrase )
    return true;
  translateWord( text );
  return true;
}
```

The sanitizer is `config.cc:268`. Same-visible-phrase dedupe is currently in
`ScanPopup::handleInputWord()` at `scanpopup.cc:540`, whereas `translateWord()`
starts at `scanpopup.cc:469` and does not perform that check. Returning true for
a duplicate means handled, not an empty-result notification. `engagePopup()`
honours `scanToMainWindow` at `scanpopup.cc:596`; do not bypass it or force the
popup. `ScanPopup::showStatusBarMessage()` at `scanpopup.cc:1154` addresses the
popup's own bar and is not a fallback notification for this action.

### 4.7 Preferences integration

In `preferences.hh`, include `waylandselection.hh` beside the existing backend
include at `preferences.hh:7`. Replace the declaration at
`preferences.hh:28` with:

```cpp
void setHotkeyStatus( QString const & platform, HotkeyBackend::State state,
                      QString const & explanation,
                      QVector<PortalBindingDisplay> const & bindings,
                      bool configureAvailable,
                      SelectionStatus const & selection = SelectionStatus{} );
```

Add `bool scanPopupHotkeyEdited = false;` beside the flags at
`preferences.hh:39`, and `void on_enableScanPopupHotkey_toggled( bool checked );`
beside the existing slots at `preferences.hh:57`.

In `preferences.ui`, insert the checkbox and indented editor after the clipboard
editor's enclosing layout (the editor starts at `preferences.ui:796`) and
before `verticalSpacer_13` at `preferences.ui:805`. Object names are
`enableScanPopupHotkey`, `scanPopupHotkey`, `horizontalLayout_scanPopupHotkey`
and `horizontalSpacer_scanPopupHotkey`. The spacer is horizontal, width 40,
height 20, matching the row geometry. The checkbox text is exactly
`Use the following hotkey to translate the selected text:`. The HotKeyEdit has
`enabled=false` in the XML. Insert a word-wrapped `QLabel` named
`scanPopupHotkeyStatus` immediately after the existing clipboard label at
`preferences.ui:843`, before Configure; its initial text is
`Selected text shortcut unavailable`. Do not create another status block/tab.

In the constructor, immediately after the existing hotkey value/filter setup
ending at `preferences.cc:208`, compute the actual QPA Wayland prefix, set the
new checkbox enabled only there, set it checked only when Wayland and the
stored boolean is true, set the stored HotKey on the editor, set the editor
enabled only when the checkbox is enabled and checked, and install this dialog
as its event filter. Loading a value must not set the edited flag.

At `Preferences::eventFilter()` (`preferences.cc:600`), inside the existing
ordinary-key branch after the other two flag assignments, add
`if( watched == ui.scanPopupHotkey ) scanPopupHotkeyEdited = true;`.
The existing modifier-only exclusion remains. The new auto-connected slot is:

```cpp
void Preferences::on_enableScanPopupHotkey_toggled( bool checked )
{
  ui.scanPopupHotkey->setEnabled( ui.enableScanPopupHotkey->isEnabled() && checked );
}
```

Immediately after the clipboard assignments at `preferences.cc:433` in
`getPreferences()`, add exactly:

```cpp
bool const wayland = QGuiApplication::platformName().startsWith( QLatin1String( "wayland" ) );
p.enableScanPopupHotkey = wayland ? ui.enableScanPopupHotkey->isChecked()
                                  : cfg.preferences.enableScanPopupHotkey;
p.scanPopupHotkey = wayland
    ? ( scanPopupHotkeyEdited ? ui.scanPopupHotkey->getHotKey() : cfg.preferences.scanPopupHotkey )
    : cfg.preferences.scanPopupHotkey;
```

The untouched-value guard is essential: `HotKeyEdit::getHotKey()` constructs a
fresh value at `hotkeyedit.cc:29` and does not retain `rawText`, while its display
uses NativeText at `hotkeyedit.cc:40`. Do not treat rendering a sequence as an
edit.

Extend the definition at `preferences.cc:615` with the default-free final
parameter. Replace the two-editor ternary in the local display lambda
(`preferences.cc:642`) with this row table:

```cpp
struct Row {
  QString id;
  HotKeyEdit * editor;
  QCheckBox * enabled;
  QLabel * label;
  Config::HotKey const & stored;
  bool const & edited;
};
Row const rows[] = {
  { "main-window", ui.mainWindowHotkey, ui.enableMainWindowHotkey,
    ui.mainWindowHotkeyStatus, cfg.preferences.mainWindowHotkey, mainHotkeyEdited },
  { "clipboard-lookup", ui.clipboardHotkey, ui.enableClipboardHotkey,
    ui.clipboardHotkeyStatus, cfg.preferences.clipboardHotkey, clipboardHotkeyEdited },
  { "scan-popup", ui.scanPopupHotkey, ui.enableScanPopupHotkey,
    ui.scanPopupHotkeyStatus, cfg.preferences.scanPopupHotkey, scanPopupHotkeyEdited },
};
```

The display lambda takes `Row const &`. Use `row.edited` and `row.stored` in
place of both ternaries; retain the old two rows' base-label algorithm exactly
as recorded in §7.3. Set all three status labels to `Qt::PlainText` so a portal
label/reason is displayed literally. On non-Wayland set the third checkbox
unchecked and disabled, disable its editor, assign the platform label in §7.3,
and **skip** the display lambda for that row. On Wayland enable the checkbox
and set the editor from its current checked state; do not reload the checkbox
or key on subsequent snapshot renders, which would erase unsaved edits.

Update the note at `preferences.cc:670` as specified in §7.3. The existing
five-argument constructor call at `preferences.cc:398` uses the default
snapshot. MainWindow supplies the real snapshot thereafter.

## 5. Configuration schema and round trip

The new elements are direct children of `<config>/<preferences>`, alongside
existing shortcut entries, not children of `<hotkeys>`. Their exact meanings:

| Element | Default when absent | Stored values |
| --- | --- | --- |
| `enableScanPopupHotkey` | `false` | `1` enables; `0` disables. |
| `scanPopupHotkey` | one key, Ctrl+Meta+D | PortableText `Meta+Ctrl+D` for a new/default value; original text for an untouched loaded value. |

For an enabled default binding the XML is:

```xml
<enableScanPopupHotkey>1</enableScanPopupHotkey>
<scanPopupHotkey>Meta+Ctrl+D</scanPopupHotkey>
```

A fresh saved configuration writes `0`, not `1`, for the first element.

Insert after the clipboard load at `config.cc:972`:

```cpp
applyBoolOption( c.preferences.enableScanPopupHotkey,
                 preferences.namedItem( "enableScanPopupHotkey" ) );
if( !preferences.namedItem( "scanPopupHotkey" ).isNull() )
  c.preferences.scanPopupHotkey = loadHotKey(
      preferences.namedItem( "scanPopupHotkey" ).toElement().text() );
```

`applyBoolOption()` at `config.cc:459` changes an option only for exact `1` or
`0`; missing, empty and any other text preserve its constructor default. Do
not copy the neighbouring `enableScanPopup` expression at `config.cc:974`,
which compares a possibly missing node's text directly with `1`. Missing key
text preserves the constructed default; an explicitly present empty key goes
through the existing loader and remains an empty, unbindable user value.

Insert before the scan-popup save block at `config.cc:1868`:

```cpp
opt = dd.createElement( "enableScanPopupHotkey" );
opt.appendChild( dd.createTextNode( c.preferences.enableScanPopupHotkey ? "1" : "0" ) );
preferences.appendChild( opt );

opt = dd.createElement( "scanPopupHotkey" );
opt.appendChild( dd.createTextNode( hotKeyStorageText( c.preferences.scanPopupHotkey ) ) );
preferences.appendChild( opt );
```

Use the existing private `loadHotKey()` at `config.cc:154` and
`hotKeyStorageText()` at `config.cc:171`. The former stores the original text
in `rawText` even if unparseable; the latter returns nonempty `rawText`
verbatim, otherwise `HotkeySyntax::serialize()`. The serializer is
`hotkeysyntax.cc:260` and uses PortableText. Neither private helper becomes a
new public API merely for a test; tests observe them through real load/save.

The additive display record remains:

```xml
<hotkeys>
  <portalBindings>
    <binding id="scan-popup" triggerDescription="Assigned by portal"/>
  </portalBindings>
</hotkeys>
```

The existing loader (`config.cc:188`) keeps known IDs, decodes attributes and
uses the last duplicate for display; the save loop is at `config.cc:2322`.
Extend acceptance to the third ID solely through the shared predicate. A
reported trigger description remains display-only and never replaces the
requested `scanPopupHotkey` or grants activation by itself.

## 6. Protocol state machine and resource rules

### 6.1 Source lifecycle and discovery

Maintain a separate internal started/stopped flag; public states have only the
four values in the header. Unsupported and Unavailable are absorbing for the
instance. Explicit stop is disposal under §3.2, not a public recovery state.
Emit/log each public state transition once; reason is empty outside Unavailable.

| Current condition/event | Action and resulting state |
| --- | --- |
| Unsupported, `start()` | No display or registry operation; remain Unsupported. |
| Supported, first `start()` | `wl_display_connect(nullptr)` on a private connection, install display-fd notifiers, obtain registry/listener, enqueue `wl_display_sync`, install its callback, immediately flush. Remain Starting. |
| Initial registry burst | Record globals in arrival order. Bind the first manager at version 1 and the first seat using `min(advertisedVersion, 2u)`. Collect other seats for replacement; log the multi-seat limitation once. Create a device when manager and seat exist and install its listener. Every outgoing batch is flushed. |
| Sync `done`, manager absent | Destroy the callback; fail once with `no ext_data_control_manager_v1`. |
| Sync `done`, manager present, no seat | Destroy callback; stay Starting indefinitely without polling/reconnect. Bind a seat if a later global arrives. |
| Sync done and manager/seat/device bound | Available. Initial primary events received before this transition are retained in the snapshot. |
| Later first seat while Starting | Bind/create device, flush, then Available; discovery is already complete. |
| Another seat arrives while Available | Keep the first bound seat. Store the other seat's global name/version for possible replacement. |
| Bound seat `global_remove`, another seat remains | Retire offers/read, destroy the old device and seat proxy, bind the earliest remaining seat, create its device and flush. Stay Available; cached text and already-latched capability remain. |
| Bound seat removed, no replacement | Retire resources and fail with `seat removed`. A later seat does not revive this terminal instance. |
| Bound manager removed | Fail with `manager removed`, even if a seat remains. |
| Unbound seat/manager removed | Remove its registry record; no change to the bound device. |
| Device `finished` | Destroy device/offers, fail with `device finished`; retain cache. |
| Display failure | Dispose the private connection and fail with `Wayland display error`; do not touch Qt's display. |

Bind only the first manager instance. No arbitrary discovery timer replaces
the sync barrier. No `wl_display_roundtrip()` is present anywhere in the new
source. Missing seat alone is not the missing-manager error and never
prematurely becomes Unavailable.

Exact terminal reason strings for local setup failures are:

| Failed operation | `reason()` |
| --- | --- |
| `wl_display_connect` | `cannot connect to Wayland display` |
| `wl_display_get_registry` | `cannot get Wayland registry` |
| `wl_display_sync` | `cannot create Wayland sync callback` |
| manager `wl_registry_bind` | `cannot bind ext_data_control_manager_v1` |
| seat `wl_registry_bind` | `cannot bind wl_seat` |
| `get_data_device` | `cannot create ext_data_control_device_v1` |
| fatal display dispatch/read/flush or nonzero display error | `Wayland display error` |

A failed replacement bind uses the same relevant setup reason. On source
failure settle an interested pending activation once with empty text/stamp 0,
then tear down. Requests made afterwards find nothing pending and answer from
the retained cache under the recency rule. Reconnect requires preference off
then on. The startup handshake itself has no invented timeout or retry loop;
the activation timer still bounds requests while discovery is Starting.

### 6.2 Offer/channel association

The device listener receives `data_offer`, `selection`, `finished` and
`primary_selection`. On `data_offer`, allocate an internal increasing ID, store
the proxy and install its offer listener. Gather its MIME advertisements until
the immediately following channel event; then call
`pendingOffer(id, mimeTypes)`. Channel callbacks must use the proxy passed by
the event, not assume every data_offer is primary.

For `selection` (ordinary clipboard), consume the matching candidate with
`discardForClipboard()`, destroy the returned proxy, and flush. Do not call
`receive()`, update clipboard timestamps/cache, or disturb an accepted primary
read. Qt continues to own clipboard access. A null ordinary clipboard event
uses `pendingOffer(0,{})` then discard and creates no proxy/read.

For every `primary_selection`, take `Monotonic::nowMs()` **at callback entry**
as that event's offer stamp, including null. If capability was false, latch it
true; when already Available emit `capabilityChanged()` once. State entry
refreshes an initial pre-Available change. Log the capability transition once.
Retire the previous primary token, stop its debounce/read timers, remove its
read notifier, close its read end, then destroy its offer proxy. Only then
accept the incoming candidate using `takeForPrimary()` and flush destroys.
Never marshal a `receive` or `destroy` on an already destroyed proxy.

A null event supplies `pendingOffer(0,{})` before take: it clears pending capture,
starts no read, and does not erase sticky text or its capture stamp. If an
activation was waiting, settle it immediately from that sticky cache; recency
still applies. A non-null offer with no usable MIME is destroyed without a
read, leaves cache untouched and logs `selection-ignored reason=no-text-mime`.
An interested activation whose new offer cannot be read receives empty/stamp 0
rather than an older purported capture.

A usable offer enters **Held**. Restart a single-shot 200 ms debounce from the
last primary event. A new event replaces Held; a drag burst yields one receive.
If an activation is already waiting, immediately promote the replacement too,
without resetting that activation's deadline. Null events and ordinary
clipboard events never issue receives.

The source's private offer phases are Empty, Held, Reading, BetweenMimeAttempts
and Settled/Retired. Settled/Retired returns to Empty after proxy disposal.
Each pipe/notifier callback captures the read token and checks
`OfferState::accepts()` before touching payload/cache. A false check is a no-op
apart from `selection-ignored reason=stale-token`. There is no completion for a
retired token, even if a notifier activation was queued before deletion.

### 6.3 Transfer start and MIME attempts

Promoting Held cancels debounce and starts the first untried acceptable MIME.
Call `beginMimeAttempt()`, use `currentMimeType()` literally for `receive()`,
then `beginRead()`. Exhaustion retires/destroys the offer. Every attempt has a
new pipe, empty accumulator and new token. Retrying one MIME never reuses the
old pipe or accumulated bytes.

Record `transferStartedAt` once, immediately before the first receive, and an
absolute deadline `transferStartedAt + 2000`. Preserve both across MIME
attempts. At each attempt start set `lastProgressAt` to that attempt's start;
advance it only for a positive read. The idle deadline is
`lastProgressAt + 250`. Timers use the same monotonic clock as the comparisons.
Timers are wakeups, not authority to accept a result after a deadline. On each
read/timeout callback check absolute expiry first, then idle expiry, then read
readiness. At equality the deadline is expired. An early wakeup rearms for the
remaining duration.

### 6.4 Exact pipe and completion contract

Create each pipe with `pipe2(fds, O_CLOEXEC)`. Obtain the read flags using
`fcntl(readEnd, F_GETFL)` and set `oldFlags | O_NONBLOCK` using `F_SETFL`.
Do **not** pass `O_NONBLOCK` to pipe2 and do not change the write end flags.
Both ends are close-on-exec, and only the read end is non-blocking.

Call `ext_data_control_offer_v1_receive(proxy, mime.constData(), writeEnd)`
and immediately `close(writeEnd)`, before flush/wait. The request has already
duplicated the fd into libwayland's outgoing message; retaining a client writer
would prevent EOF. Receive returns void; never branch on its return value.
The source owns the read end and one read QSocketNotifier for it.

Drain with a temporary buffer of at most 16,384 bytes per syscall and at most
`MaxPayloadBytes + 1 - bytesObserved` bytes remaining for the entire attempt.
Append each positive chunk to `PayloadAccumulator` and count observed bytes
separately. Thus retained data never exceeds 1,048,576 bytes, and even the
probe cannot trigger a large temporary allocation. Disable the notifier while
draining; re-enable only when waiting for more data and the token is live.

| `read()` result | Exact action |
| --- | --- |
| Positive, total observed <= cap | Append, advance progress time, continue draining. A short positive read is not completion. At exactly cap, wait for EOF or attempt a one-byte probe. |
| Positive, total observed == cap+1 | Append through the accumulator; it keeps the cap-byte prefix and latches truncation. Complete successfully immediately; close read end and stop waiting. |
| 0 (EOF), any byte count including 0 | Complete successfully with the accumulated payload; exactly cap bytes is untruncated. Empty payload is a successful empty capture, not a MIME retry. |
| -1/EINTR | Retry the syscall after checking the deadlines. |
| -1/EAGAIN or EWOULDBLOCK | Keep the partial payload and wait for readiness/deadline. |
| Other negative result, zero bytes observed | Close/remove this read; fail this MIME attempt and try the next acceptable type within the same absolute deadline. |
| Other negative result, at least one byte observed | Drop this capture and retire/destroy the whole offer. No MIME retry and no partial publication. |

Successful completion checks token and deadlines again before publishing,
updates `lastText` and its event-receipt stamp, logs the capture, consumes the
token using `completed()`, closes/removes the pipe/notifier, destroys the offer
and flushes. Deliver an interested request only after clearing it and stopping
its timer, preventing signal-handler reentrancy from completing it twice.
Retain a nonempty successful payload even if it will later be invalid under
the phrase sanitizer; the popup entry point decides phrase validity.

`pipe2` or either `fcntl` failure closes any created ends, drops/retires this
capture without attempting to marshal receive on a bad fd, and answers an
interested activation empty/stamp 0. This is a local capture failure, not a
claim that the compositor lacks the protocol.

### 6.5 Retry and timeout table

| Condition before completion | Next MIME? | Cache and interested activation |
| --- | --- | --- |
| 250 ms idle, zero bytes | Yes, if another acceptable type remains and the absolute deadline has not expired. | Old cache unchanged; activation continues waiting under its own deadline. |
| Read error other than EINTR/EAGAIN, zero bytes | Same retry rule. | Same. |
| 250 ms idle with partial payload | No; retire the offer. | Drop partial bytes, keep old cache, answer interested activation empty/stamp 0. |
| Mid-stream read error | No; retire. | Same. |
| Absolute 2 s expiry, any byte count | No; retire. | Same. |
| No acceptable type remains | No; retire. | Same. |
| EOF or observed cap+1 before deadlines | No retry; successful completion. | Publish decoded prefix with original event stamp. |

Absolute expiry wins over retry even if a zero-byte idle deadline coincides.
A new MIME's idle clock does not replenish the absolute budget. Every exit
from an attempt closes its read end and removes its notifier. Retiring the
offer invalidates the token before destroying its proxy. Timeout retirement
logs `selection-ignored reason=timeout`; local syscall failures use the separate
log form in §7.4 rather than mislabelling an error as a timeout.

### 6.6 Activation requests, overlap and the 300 ms bound

Use a single interested-ID record containing ID, call time and absolute
activation deadline. Use a `Qt::PreciseTimer` wakeup and monotonic comparisons;
check expiry before accepting capture completion. Timer/event dispatch cannot
run while the GUI thread is blocked elsewhere: 300 ms is the source's deadline
under a running event loop, not a guarantee against OS suspension or an
unrelated blocked GUI thread.

| Request/event | Required result |
| --- | --- |
| Request with no pending capture (`UseCached`) | Synchronous `captureFinished(id,lastText,lastStamp)`; use stamp 0 when no nonempty cached payload backs the answer. No debounce or transfer starts. |
| Request with Held offer | Store deadline `callTime+300`, cancel debounce, start receive immediately and await completion. |
| Request while Reading/BetweenMimeAttempts | Attach to that transfer; never cancel/restart its pipe merely for a keypress. |
| New request while an ID is interested | Replace the interested record with the new ID/deadline, rearm the timer, and immediately emit exactly one empty/stamp-0 answer for the superseded ID. The pipe, token, MIME and transfer absolute deadline do not change. Install the new record before emitting the old answer. |
| Capture completes before current activation deadline | Answer the current ID once with that payload and event stamp; no answer for previously settled IDs. |
| Activation deadline expires | Clear interested record, emit once with empty text and stamp 0, and log `selection-timeout`. MainWindow therefore chooses only the clipboard. Never return the old cache with a positive stamp here. |
| Transfer fails/retires while an activation waits | Answer current ID once empty/stamp 0 and preserve cache for subsequent no-pending requests. |
| New primary supersedes the pending offer | Retire old transfer, hold/promote the new offer under the same request deadline; never extend it merely because selections keep arriving. |
| Explicit stop/destruction | Cancel the interested record without a completion; all later callbacks are ignored. |

An activation timeout detaches that request; it does not extend/restart the
physical transfer. The transfer may complete into the background cache under
its own 250 ms/2 s bounds, but cannot send a second answer for that ID. A newer
request has its own 300 ms budget measured from its own call. MainWindow's
stale-ID guard suppresses the old ID's immediate zero-stamp answer, so two
quick presses cannot mix their translations.

### 6.7 Complete display event-loop branch table

Only this source reads its private display. The display has read and write
QSocketNotifiers; the write notifier starts disabled. Every request batch,
including initial get-registry/sync, bind/device creation, receive and proxy
destroy, calls one private flush helper immediately. Do not wait for an
incoming event to flush an otherwise idle connection.

| Entry/result | Next operation and notifier state |
| --- | --- |
| Display read notifier fires | Disable it during the callback; call `wl_display_prepare_read(display)` on a live display. Track a `prepared` boolean, initially false. |
| prepare returns 0 | Set prepared=true; proceed to `wl_display_read_events(display)` only while fd readiness is still owned by this callback. |
| prepare returns -1/EAGAIN | No read was prepared. Dispatch pending events, skip read_events for this activation, then flush through the common helper. Re-enable read notifier if still live. Do not spin in a prepare loop. |
| prepare returns -1, nonzero `wl_display_get_error` | Fail with `Wayland display error`; no read_events. |
| prepare returns -1, other errno and no display error | Dispatch pending once, flush and return without reading. A -1 with pending events never authorises read_events. |
| Prepared path loses readiness or must return early | Call `wl_display_cancel_read()` exactly once while prepared=true, set it false, then flush/return or tear down. |
| read_events returns >=0 | Its prepared read is consumed: set prepared=false, then dispatch_pending, then common flush. |
| read_events returns -1 | Its prepared read is consumed: set prepared=false; fail with `Wayland display error`. Do not cancel an already consumed read. |
| dispatch_pending returns -1 with nonzero display error | Fail/stop the watcher. |
| dispatch_pending returns -1 with no display error | Do not read speculatively; common flush then wait for the next activation. |
| Any call after which display get_error is nonzero | End watcher once, cancel a still-prepared read before destroying connection, and emit one Unavailable transition. |
| Common flush returns >=0 | Disable the write notifier; re-enable/retain the read notifier if live. |
| Common flush returns -1/EAGAIN, display error 0 | Enable/retain the write notifier. Leave it enabled until a later successful flush. Continue to monitor reads. |
| Common flush returns -1/EINTR, display error 0 | Retry flush, without disabling an already armed writer. |
| Common flush returns -1 for any other reason, or nonzero display error | Fail the watcher. |
| Write notifier fires | Flush again through the same helper. EAGAIN leaves it armed; only >=0 disables it. A queued write activation after stop does nothing. |

At callback exit every successful prepare has been paired with read_events or
cancel_read, never both. A listener may only run **phase 1** of §6.8: it
invalidates the generation and schedules phase 2. The connection is never
disconnected, and the source object is never destroyed, while any libwayland
dispatch is on the stack. The notifier callback checks the retirement flag and
the display generation immediately after
`wl_display_read_events()`/`wl_display_dispatch_pending()` return — never from
inside a listener. Removing a notifier does
not justify dereferencing a destroyed proxy from a queued lambda. No display
fd operation uses Qt's native connection. The live KWin gate does not
systematically force flush EAGAIN, dispatch errors, registry removals or every
cancel-read branch; §8.2 pins those branches with explicitly limited source
assertions instead of inventing coverage.

### 6.8 Resource teardown order (two phases)

Teardown is **two-phase**, because every terminal failure is discovered inside a
libwayland listener — the sync barrier's `done`, a manager or seat
`global_remove`, or a device `finished`. `wl_display_disconnect()` frees the
display, its mutex and the event queue that libwayland is still walking when the
listener returns, so disposing the connection there is a use-after-free
(S1). The rule is mechanical: **phase 1 may run anywhere, phase 2 may not run
inside dispatch.**

**Phase 1 — retirement, safe inside any callback.** Mark the instance retired,
bump the display generation, invalidate every read token and every outstanding
request id, stop the debounce/idle/absolute/activation timers, remove every
notifier (display read, display write, per-read pipe) and close the pipe fds,
destroy the offer, device, seat, manager, sync callback and registry proxies,
and emit exactly one terminal `stateChanged`. Phase 1 never calls
`wl_display_disconnect()`, never `delete`s the instance, never resets the owning
`unique_ptr`, and never frees the callback context.

**Phase 2 — physical teardown, never inside dispatch.** `destroyConnection()` is
idempotent (a `destroyed_` guard releases the display exactly once) and runs at
exactly one of three points (S4):

1. **Synchronously from the common `dispose()` while `dispatchDepth == 0`.**
   Every retirement goes through `dispose()`: an owner stop
   (`stop()`/`~WaylandSelectionSource()`), a **terminal failure discovered
   outside dispatch** (registry or sync request failure in `start()`, a fatal
   flush error from the write notifier, a `stop()` the owner never makes), and
   `~MainWindow` after `app.exec()` returns — where no event loop remains to run
   a timer. `dispose()` runs phase 1 and then phase 2 in the same call whenever
   no libwayland dispatch is on the stack, so a failed `start()` never leaves an
   open connection behind (S5). A terminal failure and an owner stop remain
   distinguishable: the failure path leaves `ownerStopped == false`, so cached
   text and outstanding request answers behave exactly as §3.2 specifies, while
   the owner stop clears them.
2. **From the display notifier's tail**, after `wl_display_read_events()` /
   `wl_display_dispatch_pending()` return and `dispatchDepth` drops to 0, when
   phase 1 ran during that dispatch.
3. **From a zero-shot**, only when phase 1 ran inside dispatch and the notifier
   tail did not already release the connection.

`dispatchDepth` is incremented before `wl_display_read_events()` and
`wl_display_dispatch_pending()`, and decremented after they return; every
libwayland listener therefore runs with `dispatchDepth > 0`.

**Deferred work never captures `this`.** The `wl_display`, its listener context
and the `destroyed_`/`dispatchDepth` state live in a `Connection` object held by
`std::shared_ptr`: the source holds one reference and a deferred zero-shot
captures another. If the source is destroyed first, the timer functor keeps the
`Connection` alive until it runs; if the event loop is already gone, destroying
that functor runs `~Connection`, which calls the same idempotent
`destroyConnection()` synchronously — still outside dispatch. Notifier-tail
teardown and a queued teardown therefore cannot release the display twice, and a
queued teardown cannot outlive the objects it touches.

Phase 2 cancels a still-prepared read, performs one best-effort **non-blocking**
flush of the queued destructors when the display is healthy, calls
`wl_display_disconnect()`, clears every pointer and fd, and deletes the instance
if deletion was requested. On a display error it performs no flush and touches
no proxy. Seat version 2 uses local `wl_seat_destroy()` rather than a
higher-version release. Cache and its stamp survive failure and null events;
explicit preference disable destroys the entire instance and its cache.

## 7. Behavioural contracts and exact strings

### 7.1 Recency, suppression and translation

Order these rules, with whitespace-only candidates treated as empty:

1. Nonempty selection with `capturedAt >= clipboardChangedAt` wins; ties favour
   selection.
2. Otherwise nonempty clipboard wins.
3. Otherwise nonempty selection wins.
4. Otherwise return empty.

A never-changed clipboard has stamp 0. Selection uses its primary-event receipt
stamp, never completion/debounce time. MainWindow supplies an empty selection
when `capturedAt == 0`, including an activation timeout. No separate cache age,
wall-clock date, focus test, owner detection or clipboard-emptiness shortcut
replaces these rules. A valid phrase is translated through `translateWord()`;
sanitisation/length limits and send-to-main-window are inherited. Invalid
phrases return false and use the visible-channel ladder; a visible duplicate
returns true without retranslating.

### 7.2 New action, messages and state suffixes

The exact English source strings are:

| Use | Text |
| --- | --- |
| Portal action description | `Translate the selected text` |
| Checkbox | `Use the following hotkey to translate the selected text:` |
| Initial form label | `Selected text shortcut unavailable` |
| Empty-result message | `No selected text or clipboard text to translate` |
| Tray message title | `GoldenDict` |
| Starting suffix | `selection source starting` |
| Available, primary seen | `primary selection supported` |
| Available, no primary event | `selection not observed yet` |
| Unavailable suffix template | `selection unavailable: %1 — clipboard is used instead` |
| Unsupported suffix | `selection unsupported in this build — clipboard is used instead` |
| Non-Wayland xcb label | `Not used on X11 — the Held keys setting on the Scan Popup tab applies` |
| Other non-Wayland label, including Windows/macOS | `Not used outside Wayland sessions` |
| Disabled selection note | `Selected-text shortcut is disabled.` |

Use `tr()` for these strings, `%1` for reason substitution, U+2014 for em dash,
and U+00B7 with one space on either side as the row separator. No log message
contains selected text, clipboard text or an activation token.

### 7.3 Exact row algorithm and capability note

The existing base-label algorithm is at `preferences.cc:639` and is used for
all enabled Wayland rows through the id-driven Row value:

1. Unchecked → `Disabled`.
2. Untouched nonempty rawText that cannot be parsed →
   `Unavailable on Wayland: %1` with the parser's exact reason.
3. A parsed/editor key with no preferred trigger → the same template with the
   converter's exact reason.
4. Backend Bound/Partial and a nonempty description for this ID → that exact
   portal-reported description, display only.
5. Wayland and Configure available but no assigned description →
   `Not assigned by the portal — use Configure shortcuts`.
6. Otherwise backend Bound on non-Wayland → `Active`; in all remaining cases
   use explanation if nonempty, otherwise the backend status text.

For the enabled third row on Wayland append ` · ` and the source suffix in
§7.2 to **whichever base** resulted, including unsupported key syntax or an
unassigned portal binding. For its unchecked state show exactly `Disabled`
without a fictional source capability. The first two rows do not gain a
selection suffix. Non-Wayland third rows bypass this algorithm unconditionally
and use the platform labels, ignoring stale bindings/reasons/snapshots.

Keep these backend status source strings from the switch at
`preferences.cc:623`: `Portal bound`, `X11 active`, `Portal partial`,
`X11 partial`, `Portal denied`, `Global shortcuts unavailable`,
`Global shortcuts unsupported`, `Creating portal session`,
`Binding global shortcuts`, `Portal session ready`, `Global shortcuts closed`.
The global capability label retains `status` or `status + ": " + explanation`.
The Configure button is visible only on Wayland and enabled only when its live
Configure capability is true, as at `preferences.cc:668`.

For `clipboardCapabilityNote`, preserve the existing non-Wayland text exactly:
`Clipboard-change scanning and explicit clipboard lookup remain available independently of global shortcuts.`
For Wayland use this exact base:
`Hover lookup and focus restoration are unavailable on Wayland. Clipboard-change scanning and explicit clipboard lookup remain available; global shortcuts use the portal.`
Append one space, then `Selected-text shortcut is disabled.` when unchecked;
otherwise append the translated template `Selected-text shortcut: %1.` with
the source suffix substituted. This preserves both existing limits and adds
the actual selection-source state without treating portal binding as capture.

Refreshing state/snapshot must not modify stored keys, an editor's current
value, edited flags or a Wayland checkbox's unsaved checked state. A
capability-only transition from Available/unseen to Available/seen must rerender
the row; it cannot wait for a new State enum value.

### 7.4 Exact diagnostic contract

Use `gdDebug` with these message bodies, without payload/token contents:

```text
selection-source: state=<state> reason=<reason>
selection-source: primary-selection-seen=1
selection-source: multiple seats; watching first seat
selection-captured mime=<type> bytes=<n> truncated=<0|1>
selection-ignored reason=no-text-mime
selection-ignored reason=stale-token
selection-ignored reason=timeout
selection-transfer-error operation=<pipe|fcntl|read> errno=<decimal>
selection-timeout
selection-empty
```

`<state>` is exactly `Unsupported`, `Starting`, `Available` or `Unavailable`;
`<reason>` is empty outside Unavailable, otherwise one of §6.1's exact reasons.
Emit the starting/current-state record once on first start and each subsequent
actual transition once. Log the capability latch once, even if it precedes
Available. `<type>` is the chosen literal MIME, `<n>` the retained byte count,
and truncated is decimal 0/1; a cap+1 transfer therefore logs
`bytes=1048576 truncated=1`. A transfer at exactly cap logs
`bytes=1048576 truncated=0`. A zero-byte EOF logs bytes=0, truncated=0.
`selection-timeout` refers to the 300 ms activation deadline;
`selection-ignored reason=timeout` refers to a retired physical transfer.
Error after partial bytes is an error/drop, not successful truncation.

## 8. Test plan

All named new test functions below are top-level QtTest slots, declared and
defined exactly once. `_data` functions have only their stated data rows.
Retain every existing test; do not rename a pre-existing suite or weaken its
assertions. Assertions listed as source assertions inspect repository text
and do not instantiate the inspected production class.

### 8.1 New suite: `tst_waylandselection`

Project: `tests/tst_waylandselection/tst_waylandselection.pro`. Exact project:

```qmake
TARGET = tst_waylandselection
include(../testsupport.pri)
QT += core
SOURCES += $$GD_REPO_ROOT/waylandselectionlogic.cc \
           tst_waylandselection.cc
HEADERS += $$GD_REPO_ROOT/waylandselectionlogic.hh
```

It links only the logic unit as the production subject, plus shared test
support supplied by the include. It does **not** link `waylandselection.cc`,
protocol C, `libwayland-client`, MainWindow or ScanPopup. Shared support itself
adds Qt GUI/browser libraries at `tests/testsupport.pri:35`; that link-time
fact does not make this a GUI test. Use `QTEST_GUILESS_MAIN(TstWaylandSelection)`
and never instantiate a GUI/browser object. Runtime needs no display or
compositor. `initTestCase` calls `TestSupport::requiredEnvironment()` and
installs `PureWatchdogSeconds`; `cleanupTestCase` clears it, using the APIs at
`tests/support/fixtures.hh:69`, `tests/support/fixtures.hh:75` and
`tests/support/fixtures.hh:79`; the 60 s constant is at
`tests/support/fixtures.hh:46`.

| Test function | Exact assertions |
| --- | --- |
| `initTestCase` | requiredEnvironment returns true; failure reports joined reasons. Install the 60 s pure watchdog. |
| `cleanupTestCase` | Clear watchdog; no additional behavioural claim. |
| `mimePreferenceOrder` | For all four types shuffled with unknowns, choose lower-case utf-8; remove it, choose upper-case UTF-8; remove that, choose UTF8_STRING; remove that, choose text/plain. Empty and `{image/png,text/html,TEXT/PLAIN}` choose empty. Duplicate acceptable types do not change result. |
| `payloadClampIsAppliedWhileReading` | Append 400000 `a`, 400000 `b`, then 400000 `c`: sizes after each append are 400000, 800000, 1048576; retained value is exactly `a*400000+b*400000+c*248576`; full/truncated are false/false, false/false, true/true. Appending more never grows it or clears truncation. A single cap+17 chunk retains cap and truncates. A separate cap=3 accumulator fed `ab`, then `c` has payload `abc`, full true, truncated false; appending empty leaves it false. |
| `decodeReplacesInvalidUtf8` | Decoding hex `61ff62` equals `a` + U+FFFD + `b`; decoding valid UTF-8 `c3a9` equals U+00E9. |
| `decodeStripsNul` | Decoding explicit-length bytes hex `0061006200` equals `ab` with no U+0000. NUL is removed at beginning, middle and end. |
| `chooseTextUsesRecency` | `chooseText("selected",20,"copied",10)=="selected"`; `(10,20)=="copied"`; equal 20/20 gives selected; whitespace-only selection with stamp 30 and nonempty clipboard at 20 gives copied; selection stamp 1 versus untouched clipboard stamp 0 gives selected; older selection stamp 10 versus whitespace-only clipboard at 20 gives selected; both whitespace/empty yield empty. Chosen nonempty text retains its original surrounding spaces. |
| `clipboardStampIgnoresNonClipboardModes` | Iterate `QClipboard::Clipboard`, Selection and FindBuffer represented by test booleans; passing `mode==Clipboard` returns true, false, false respectively. The production call-site source assertion supplies the actual enum comparison. Do not link a clipboard implementation or claim receipt-time coverage here. |
| `mimeAttemptsAdvanceAndExhaust` | pending offer ID 1 advertises the four types in reverse order plus duplicate/unknown; take returns 1. Each `beginMimeAttempt()` is true, `currentMimeType()` equals the next literal priority type, and a repeated begin before failure retains that type. Fail each; fifth begin is false and currentMimeType is empty. Retire then begin is false. |
| `payloadAtCapIsNotTruncated` | Append exactly cap bytes in 4096-byte chunks: retained bytes equal input, full true, truncated false; appending empty changes nothing. |
| `payloadOverCapIsTruncated` | Append exactly cap, then one extra byte: retained bytes equal first cap, full true, truncated true. A separate single cap+1 chunk gives the identical state. |
| `offerSupersessionRetiresToken` | Accept A=1, select MIME and reserve token a>0; accepts(a) true. Introduce/accept B=2; accepts(a) false, select MIME and reserve b>a, accepts(b) true. completed(a) leaves accepts(b) true; completed(b) makes it false. |
| `clipboardOfferIsDiscarded` | Accept primary A with token a; pending clipboard B=2 then discard returns 2; accepts(a) remains true and no token for B can be reserved while A's read is active. A separate clipboard-only state discards B, beginMimeAttempt false, beginRead 0. |
| `retiredOfferIsNeverReused` | Retire active offer; accepts(oldToken) false, beginMimeAttempt false, beginRead 0, currentMimeType empty. Separately exhaust a one-type offer by failure; a further begin is false and beginRead is 0. |
| `nullOfferClearsWithoutRead` | Accept A/token a, then pendingOffer(0,{}) and take: returned ID 0 and bool false; accepts(a) false, beginMimeAttempt false, beginRead 0. Default ChannelOffer is false; ChannelOffer{7} is true. No pure cache is invented. |
| `activationPlanPromotesPendingCapture` | planActivation(true)==AwaitPending and planActivation(false)==UseCached. Two consecutive Monotonic::nowMs calls are positive/nondecreasing; compile-pin its type as `qint64 (*)() noexcept`. |

There are 14 behavioural slots plus init/cleanup: 16 functions, no `_data`
functions. Do not claim they exercise a real debounce, pipe, source deadline,
MIME transfer retry or overlapping requestCapture call.

### 8.2 Extended suite: `tst_hotkeys`

Project: `tests/tst_hotkeys/tst_hotkeys.pro`. Keep its production config, syntax,
backend, portal, recorder and Preferences link closure; the Preferences sources
and form are at `tests/tst_hotkeys/tst_hotkeys.pro:36` and
`tests/tst_hotkeys/tst_hotkeys.pro:37`; the other production
sources are at `tests/tst_hotkeys/tst_hotkeys.pro:51`. It includes the new source
header's enum/snapshot/constant only. It does not instantiate or link the source.
The existing read-source precedent is
`tests/tst_hotkeys/tst_hotkeys.cc:1145`; isolated config save/read helpers are
at `tests/tst_hotkeys/tst_hotkeys.cc:603` and
`tests/tst_hotkeys/tst_hotkeys.cc:630`. Use them rather than calling anonymous
namespace helpers from config.cc.

| New function | Exact assertion and evidence class |
| --- | --- |
| `scanPopupBindingIsPortalOnly` | For every platform in `{xcb,wayland,wayland-egl,offscreen,windows,cocoa}` and preference false/true, compare binding predicate to `preference && (platform==wayland \|\| platform==wayland-egl)`. Separately for build false/true compare capture predicate to `build && sameWaylandSet`. No capture capability participates in the binding matrix. Runtime pure predicate assertions. |
| `portalIdPredicateAcceptsThreeIds` | Shared function returns true for main-window, clipboard-lookup, scan-popup; false for empty and foreign-action. Source assertions: hotkeybackend.hh has one inline definition listing all three; config.cc and portalhotkeys.cc contain no definitions of either known-ID predicate; config load/save call shared name; beginGeneration calls it and never knownBindingId. |
| `capabilityConstantMatchesBuild` | Under `#ifdef HAVE_WAYLAND_DATA_CONTROL`, QCOMPARE(kHaveWaylandDataControl,true), else false. Source assertions extract the enclosing qmake scopes and require `DEFINES += HAVE_WAYLAND_DATA_CONTROL` inside `packagesExist(wayland-client)` inside unix:!mac in both projects, and PKGCONFIG plus protocol C in that application scope. Do not accept the mere presence of those tokens elsewhere. |
| `sourceFollowsThePreference` | Source assertions extract reconcileSelectionSource and assert the exact wanted expression in §4.3; !wanted path orders stop before reset; wanted+absent path constructs once, connects before the zero-shot, and guards shutdown/QPointer/current-instance before start; existing pointer path never recreates it. installHotKeys calls reconciliation after shutdown guard and before installGate.shouldBind; constructor and Preferences result path call installHotKeys. Four teardown bodies stop immediately before their popup reset; four recreation bodies contain no source stop/reset. This is source evidence, not a behavioural source-lifecycle test. |
| `defaultScanPopupHotkeyIsBindable` | Default preferences boolean false; key1 Key_D, key2 0, modifiers Ctrl plus Meta; toKeySequence count 1 and PortableText Meta+Ctrl+D. Real preferredTrigger returns true, empty error, trigger CTRL+LOGO+d. Real Config::save writes nonempty scanPopupHotkey text exactly Meta+Ctrl+D. This observes private hotKeyStorageText through save. |
| `threeBindingsStayBound` | On a fresh TestSupport::PrivateSessionBus/FakePortal version 2, call setListShortcuts({}) after portal.start and before constructing the backend, then setBoundIds({main-window,clipboard-lookup,scan-popup}). Bind those IDs with triggers CTRL+ALT+g, CTRL+ALT+h, CTRL+LOGO+d. Within 1500 ms backend reaches Bound, all three IDs appear once in the recorded BindShortcuts request, protocolViolations is empty, and Unavailable is never emitted. Repeat on a fresh backend/session with setBoundIds({main-window,scan-popup}): state Partial, not Unavailable; both returned IDs remain reported. |
| `scanPopupPortalBindingSurvivesSaveLoad` | Save three known display records plus foreign-action. Saved binding elements contain exactly the three known IDs and exact scan-popup triggerDescription `Portal <selected> & key`; Config::load returns that description decoded and that ID once. The requested default key remains Meta+Ctrl+D. An amended XML with duplicate scan-popup descriptions keeps the last; unknown remains ignored. |
| `scanPopupHotkeyKeepsVerbatimText` | For stored text `Ctrl+Meta+D`, `Ctrl+G,Ctrl+H,Ctrl+I`, and `not a shortcut`, load a per-test amended legacy fixture, open/accept an unedited Preferences result, then save/reload: DOM text and rawText equal the original for each. Scan-popup raw preservation is exercised even when the current platform disables the row. The existing analogous main/clipboard assertions are at tests/tst_hotkeys/tst_hotkeys.cc:1768. |
| `legacyConfigDefaultsScanPopupHotkeyOff` | Original legacy fixture without either key: load yields false, one key D with Ctrl plus Meta, PortableText Meta+Ctrl+D; file bytes before/after load are identical. Save writes enable=0 and default PortableText; reload preserves both. |
| `scanPopupConfigBooleanValues` | Amend real fixture with boolean text 1,0,empty,true,2 and with node missing. Load results exactly true,false,false,false,false,false. Existing scanPopupHotkey node empty stays empty/unbindable; absent node keeps the default. Runtime load/save assertions. |
| `scanPopupActivationWiringIsPinned` | Perform all ordered/count source assertions immediately below. No claim of activation runtime behaviour. |
| `xcbGuardDoesNotCreateThePopup` | Predicate false for xcb with preference true; model guard with both old preferences false therefore returns without creation. Source extraction of makeScanPopup requires the exact three-term platform-predicate guard from §4.4 and rejects a bare third `!cfg.preferences.enableScanPopupHotkey`. Require reset to remain after request-ID invalidation. This tests the predicate and source expression, not widget construction. |
| `scanPopupSourceLoopContractIsPinned` | Source assertions require private connect, sync callback/barrier, explicit bound-global removal branches, read-only O_NONBLOCK setup, immediate write close, cap+1 probe, both deadline comparisons, zero-byte-only MIME retry, token check before publication, and the display branch/rearm invariants in §6.7. Additional assertions: `transferStartedAt` is assigned exactly once, before the first `receive()`, and never inside the per-MIME attempt path (S2); every listener path runs only phase 1 of §6.8 — no `wl_display_disconnect`, `delete`, or owning-pointer reset appears in any registry/offer/seat/manager callback body (S1); every retirement — owner stop, destructor, and terminal failure — goes through one `dispose()` that calls `destroyConnection()` synchronously when `dispatchDepth == 0` and defers it only inside dispatch (S4, S5); `destroyConnection()` carries an idempotence guard; every deferred teardown captures a `std::shared_ptr<Connection>` rather than `this`; and a failed `start()` (registry/sync request failure) still releases the connection before returning (S5); the overlap branch of `requestCapture()` neither calls `beginRead()` nor touches the active token (S3). Reject roundtrip and Qt-native-display borrowing. Extract relevant function/branch bodies with balanced braces; verify ordering and negated paths, not a bag of file-wide tokens. It is source evidence for branches the named live harness cannot deterministically force. |

`scanPopupActivationWiringIsPinned` must assert these independently, with a
separate diagnostic for each condition:

- Exactly two `"scan-popup"` literals in mainwindow.cc; one is the add call with
  the binding predicate and one is the activation branch.
- In that branch: winId before applyActivation before the source null test;
  requestCapture is inside the non-null branch and uses the incremented member
  request ID; the else branch reads Clipboard mode and calls
  translateSelectedText plus reportNothingToTranslate on false.
- Exactly one `requestId != scanPopupRequestId` comparison, in the completion
  lambda's shutdown/ID/popup early return; the zero-stamp conditional precedes
  chooseText, which reads clipboard and clipboardChangedAt at completion.
- Exactly one typed capabilityChanged connection to refreshHotkeyStatus, in the
  source-creation block; stateChanged and completion are also connected there.
- Exactly one call to clipboardStampAdvances in mainwindow.cc, within the
  constructor's QClipboard::changed lambda, with the actual `mode ==
  QClipboard::Clipboard` argument; its true branch assigns Monotonic::nowMs to
  clipboardChangedAt and has no Selection/FindBuffer assignment alternative.
- refreshHotkeyStatus copies all three snapshot fields and passes the final
  parameter to Preferences, which never stores a source pointer.
- ScanPopup's new entry point orders sanitize, invalid false, visible dedupe
  true, translateWord, then true. It contains no status-bar notification and
  no direct engagePopup/forcePopup alternative.
- Empty reporting logs selection-empty exactly once and branches on main-window
  visibility before testing the tray pointer; the exhausted branch shows
  nothing. Assert the exact message/title and 3000 ms duration, and absence of
  any call to ScanPopup::showStatusBarMessage.

The source-loop check names the corresponding Impl methods as
`flushRequests`, `dispatchDisplayReadable`, `dispatchDisplayWritable`,
`registryGlobal`, `registryGlobalRemoved`, `discoveryDone`,
`primarySelection`, `startMimeAttempt`, `drainPipe`, `checkTransferDeadline`,
`retirePrimary` and `dispose`. Use those names in the implementation so source
extraction has a stable target. The tests must explicitly report themselves as
source assertions; removing a branch/order condition must fail that assertion.
They do not prove syscall scheduling or a compositor's registry semantics.

### 8.3 Preferences matrix and edited-value tests in `tst_hotkeys`

Add these exact slots to the same project; no new suite is created:

| Function | Exact assertions |
| --- | --- |
| `selectionStatusRowRendersSnapshots_data` | Five rows named starting, available-seen, available-unseen, unavailable, unsupported. Snapshot fields and expected suffixes are the five rows below. |
| `selectionStatusRowRendersSnapshots` | Enabled/default-bindable scan key; initialise the logical Wayland renderer and programmatically check the enabled checkbox, then call setHotkeyStatus("wayland",Bound,empty,{{scan-popup,"Portal selected trigger"}},true,snapshot). Third label equals `Portal selected trigger · ` plus exact expected suffix. First two rows have no selection suffix. Call again with Available/unseen then Available/seen and assert the whole label changes from `selection not observed yet` to `primary selection supported` without changing editor/checkbox values or the returned stored raw key. The real-Wayland child separately checks edited-value preservation. This test proves rendering two snapshots, not the MainWindow signal connection. |
| `scanPopupRowPlatformMatrix_data` | Rows xcb, windows, cocoa, offscreen, wayland, wayland-egl. Expected checkbox/editability and exact label are specified below. |
| `scanPopupRowPlatformMatrix` | New dialog for each logical platform. cfg stores enabled=true and rawText Ctrl+Meta+D. Supply Unavailable/manager removed and a stale Bound scan-popup description. For each non-Wayland platform, checkbox unchecked+disabled, editor disabled, exact platform label independent of supplied source state/portal description. For Wayland, enable/check the UI for this renderer exercise and assert checkbox enabled+checked, editor enabled, exact `Portal selected trigger · selection unavailable: manager removed — clipboard is used instead`. Assert stored cfg fields unchanged. On the xcb runtime call getPreferences for non-Wayland rows and assert the stored true/rawText/key fields are retained. Source-pin the actual-QPA ternary in getPreferences and test the shared platform predicate for windows/cocoa as well; this is a logical-platform UI matrix, not execution of native Windows/macOS plugins. |
| `scanPopupRowWaylandValuesAndEdits` | Parent case starts nested KWin and the same suite's child probe on real QPA wayland; require normal exit 0. Use the bounded compositor/child process pattern of tests/tst_hotkeys/tst_hotkeys.cc:1625, with a distinct socket gd-selection-ui. Require child marker `scan-popup-wayland-preferences: PASS`. |
| `scanPopupRowWaylandProbe` | Return immediately unless GD_SCAN_POPUP_UI_PROBE=1. In child require actual QPA prefix wayland. Unedited enabled raw Ctrl+Meta+D survives getPreferences; checkbox off returns false without changing key; on restores true. Pure modifier keypress leaves raw untouched; ordinary Ctrl+Alt+J edit makes rawText empty and key PortableText Ctrl+Alt+J; subsequent state/snapshot rerender retains that edit. Reject dialog leaves cfg unchanged. Emit the parent marker only after all assertions. |
| `scanPopupRowBaseStatuses` | Use Available/seen=true/empty reason for every suffix and an enabled valid default key; initialise logical Wayland and programmatically check its checkbox before rendering assertions. Bound/configurable with no record equals `Not assigned by the portal — use Configure shortcuts · primary selection supported`; Denied with explanation `permission refused` and stale record equals `permission refused · primary selection supported`; unchecked equals exactly `Disabled`. In a fresh dialog store untouched raw `Ctrl+G,Ctrl+H,Ctrl+I`; the label equals `Unavailable on Wayland: Only one or two key combinations are supported. · primary selection supported` and contains no stale portal description. Repeated renders retain checkbox/editor values. Assert exact clipboardCapabilityNote base plus ` Selected-text shortcut: primary selection supported.` when checked, and base plus ` Selected-text shortcut is disabled.` when unchecked. |

The five snapshot rows are:

| State/fields | Exact expected suffix |
| --- | --- |
| Starting, empty reason, seen=false | `selection source starting` |
| Available, empty reason, seen=true | `primary selection supported` |
| Available, empty reason, seen=false | `selection not observed yet` |
| Unavailable, reason=`manager removed`, seen=true | `selection unavailable: manager removed — clipboard is used instead` |
| Unsupported, empty reason, seen=false | `selection unsupported in this build — clipboard is used instead` |

The platform matrix expects xcb's X11 label from §7.2; windows, cocoa and
offscreen expect `Not used outside Wayland sessions`; the two Wayland rows
expect the explicit joined portal/source string. Programmatic checkbox setup
for simulated Wayland occurs after enabling the logical row and before the
renderer assertion; production getPreferences always consults the real QPA.
Do not fake QGuiApplication's platform or claim a simulated renderer call proves
native platform persistence. The real xcb and real Wayland getter tests plus
the explicit source/predicate pins cover that persistence rule without adding
a new public test-only preference API.

The invalid three-combination label reuses the exact parser message at
`hotkeysyntax.cc:168`; do not add a second parser or an invented translation.
The new hotkeys inventory is 20 functions: 13 in §8.2 and seven in §8.3,
including the two `_data` suppliers and child-probe slot. Existing tests remain
unchanged. The fake portal start, request-record and bound-payload APIs are at
`tests/support/fake_portal.hh:137`, `tests/support/fake_portal.hh:143`,
`tests/support/fake_portal.hh:162` and `tests/support/fake_portal.hh:163`;
its method-count accessor is at `tests/support/fake_portal.hh:180`.

### 8.4 Live protocol gate

Create executable `scripts/verify-wayland-selection-live.sh`, a required live
gate separate from the aggregate's pure suite. The existing isolated-script
pattern starts at `scripts/verify-test-gate.sh:27`; emulate its temporary-copy
cleanup discipline, not its four unrelated mutation scenarios.

Invocation is `scripts/verify-wayland-selection-live.sh`, with no arguments.
It requires `qmake6`, a C/C++ compiler, `make`, `pkg-config` with wayland-client,
`kwin_wayland`, `dbus-run-session`, `timeout` and `python3`. Missing tools,
missing ext protocol, build failure and failed assertions are nonzero failures,
never skip-success. It does not require wayland-scanner because generated
protocol code is committed. Run all cases under a 60 s runner watchdog and
outer `timeout 120`; compositor startup has a 5 s deadline. Print prerequisites
or failing case names to stderr and a final PASS only if every case passed.

Use mktemp under /tmp, a private mode-0700 XDG_RUNTIME_DIR, a private D-Bus
session, isolated config/cache/data directories and socket `gd-selection-live`.
Launch nested KWin with `QT_QPA_PLATFORM=offscreen`, `KWIN_COMPOSE=Q`,
`LIBGL_ALWAYS_SOFTWARE=1` and:

```sh
kwin_wayland --virtual --no-kactivities --no-global-shortcuts --socket gd-selection-live
```

Run the capture runner with QPA wayland and that socket. The generated project
is `$tmp/selection-live.pro`, the embedded runner/helper implementation is
`$tmp/selection-live.cc`, and the project is exactly this (the script supplies
an absolute GD_REPO_ROOT argument to qmake):

```qmake
TEMPLATE = app
TARGET = selection-live
CONFIG += c++17 console link_pkgconfig
QT = core gui testlib
PKGCONFIG += wayland-client
DEFINES += HAVE_WAYLAND_DATA_CONTROL
INCLUDEPATH += $$GD_REPO_ROOT $$GD_REPO_ROOT/protocols/ext-data-control-v1
SOURCES += $$GD_REPO_ROOT/waylandselection.cc \
           $$GD_REPO_ROOT/waylandselectionlogic.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           $$GD_REPO_ROOT/protocols/ext-data-control-v1/ext-data-control-v1-protocol.c \
           selection-live.cc
HEADERS += $$GD_REPO_ROOT/waylandselection.hh \
           $$GD_REPO_ROOT/waylandselectionlogic.hh
```

MOC the real source header. It does not link MainWindow, ScanPopup, Preferences
or the hotkey backend implementation. Register the source State metatype and
spy on real source signals. Install a Qt message handler for the exact source
log bodies. Run the runner through `timeout 60`; this watchdog is independent
of GUI event-loop progress. No replacement source/accumulator implementation
is permitted.

Run the same binary as `selection-live --helper` under QProcess for the helper:
main constructs QCoreApplication in that mode, QGuiApplication in runner mode.
The embedded helper client uses its own libwayland connection to create an
ext data-control source/device, advertise selected MIME types and set primary
or ordinary clipboard values. For each receive it records offer serial, MIME,
fd flags, bytes and receive time. It splits writes, keeps a writer open,
refuses a MIME by holding its fd without writing, and delays a final write.
The control channel is newline-delimited compact JSON on QProcess stdin/stdout;
source/helper diagnostics go to stderr. Commands are `clear` (channel), `set`
(channel, serial, mimes, per-MIME chunk schedule and close time), `release`
(serial) and `quit`. A `set`/`clear` acknowledgement is emitted only after the
helper's display-sync done callback. A `receive` event includes serial, literal
MIME and whether F_GETFL has O_NONBLOCK. A write-error event includes serial
and decimal errno. Runner request IDs match acknowledgements, and the runner
stamps receipt of helper events with its own Monotonic clock; helper-local
absolute clock values are never compared with source stamps. Chunk delays are
relative to that helper receive, not the preceding set command. The scheduled
chunk/selection delays are controlled stimuli; assertion code waits on events
and deadlines. Ignore SIGPIPE in the helper and observe EPIPE/close when the
source retires a transfer. Blocking helper writes cannot block the runner's
source event loop because the helper is a separate child process.

Every case uses a fresh source and clears primary/clipboard state so sticky
cache cannot leak between cases. Except for the receipt-time case below, wait
for the initial null primary event before setting the test offer. The
receipt-time case deliberately installs its primary before source start so
that its first primary event supplies an observable capability-log barrier.
In all timing assertions allow 50 ms **only as
measurement/dispatch tolerance**, never as an extension to the implementation's
deadline or acceptance of a late successful result. A late result must still
have stamp 0/no publication as specified. Assert the following named runner
functions, each returning failure on any listed mismatch:

| Case/function | Exact stimulus and observable assertions |
| --- | --- |
| `sourceBecomesAvailable` | Start real source twice; it reaches Available within 5 s, reason empty, primarySelectionSeen true (initial null counts), one source Starting/Available log sequence and one capability latch. Assert source state/capability log bodies. Idempotent connection creation itself is additionally source-pinned; these logs alone do not count physical connections. |
| `multiChunkCompletesAtEof` | Primary helper sends ASCII `first-` at t=0, `second-` at +40 ms, `third` at +80 ms, closes at +120 ms. After debounce and EOF, requestCapture returns exactly `first-second-third`, positive event stamp, and only one captured log with bytes=18/truncated=0. No publication after the first short chunk. Helper confirms received write fd lacks O_NONBLOCK. |
| `tenSelectionsDebounceToOneRead` | Set ten distinct primary values `burst-0` through `burst-9` 10 ms apart, without activation. Only final offer receives a send request, exactly once, no receive before the last event's 200 ms debounce, final cached text burst-9. Subsequent no-pending request returns it synchronously. |
| `overCapIsTruncated` | Advertise UTF-8 text and send cap+8192 ASCII `x` bytes. Source caches exactly cap `x`, emits captured bytes=1048576/truncated=1, and does not drop the capture for exceeding cap. Helper tolerates reader closure after the probe. |
| `exactCapIsNotTruncated` | Send exactly cap ASCII `y`, close; cached value cap `y` and captured truncated=0. This distinguishes a full accumulator from the one-byte probe. |
| `supersededCaptureCannotPublish` | Promote offer A; helper sends prefix `old` then holds its writer. Set B=`new-value` before A completes; allow A's delayed remaining bytes/close. B is the only final cache value, old is never logged as a completed capture, old request/pipe is retired and source remains Available. Later no-pending request returns new-value with B's stamp. Do not require a stale-token log if removing the notifier prevents any stale callback. |
| `receiptStampLetsNewCopyWin` | Before starting this fresh source, helper sets primary `selected-before-copy` and confirms its set request is processed by a helper-display sync barrier. Start the source. Its first primary event is therefore the non-null test selection. On the `selection-source: primary-selection-seen=1` log, queue a runner timer for 10 ms; when it fires record clipboardChangedAt=Monotonic::nowMs and ask the helper to set ordinary clipboard `copied-after-selection`. This is after primary receipt and inside its 200 ms debounce. After primary completion, capturedAt is positive and strictly less than the clipboard stamp; chooseText returns copied-after-selection. Ordinary clipboard offer triggers zero source receive calls. This proves real primary receipt stamping, not MainWindow's clipboard connection. |
| `zeroByteIdleRetriesNextMime` | Advertise exactly `text/plain;charset=utf-8` then `text/plain`; on first receive keep writer open with zero bytes; after idle expiry the helper observes a second receive for text/plain, writes `retry-success` and closes. Source returns retry-success and logs that MIME, helper MIME order exactly those two literal strings. An early EOF on the first attempt is not used as a refusal. |
| `partialIdleRetiresWithoutRetry` | Same two types, first sends `incomplete` then holds writer indefinitely. After its 250 ms idle deadline, pending capture becomes false, old cache remains unchanged, helper receive count remains 1 beyond the point a second attempt would have started, and timeout-ignore log occurs. An activation waiting on this read settles empty/stamp 0. |
| `absoluteDeadlineSurvivesProgress` | Advertise two acceptable types. The **first** attempt keeps its writer open with zero bytes, so it expires on the 250 ms idle timeout and the helper observes a second `receive` for the second type. That second attempt then writes one ASCII byte every 100 ms and never closes, so it never reaches the idle timeout. Pending capture must be false by **first receive + 2050 ms** — not second receive + 2050 ms — with no partial cache publication, helper read-end closure and a timeout-ignore log; at 1800 ms it was still pending. Deleting the absolute-deadline check leaves it pending past 2050 ms, and resetting `transferStartedAt` at the second attempt also leaves it pending, so both mutants fail this one case (S2). |
| `overlappingRequestsReuseTransfer` | On one promoted read, call requestCapture(101); helper writes `over-` immediately, `lap-` at 150 ms, `result` at 300 ms and closes at 350 ms. Call requestCapture(102) 100 ms after 101. Observe exactly one answer for 101, immediately empty/stamp 0, and no restart/second helper receive. ID 102 receives exactly `over-lap-result` with positive stamp and elapsed time at least 240 ms but strictly less than 300 ms from its own call; ID 101 receives no second answer. This case does **not** claim read-token
identity: the public API exposes only request ids, text and stamps. That the
overlap branch neither reserves nor replaces the active read token is pinned by
the source assertion in §8.2 (S3). |
| `activationDeadlineSuppressesCache` | First cache `old-cache`. Set a new offer that continuously progresses but cannot finish by 300 ms and promote ID 201. By call+350 ms answer is empty/stamp 0 with selection-timeout; chooseText with that suppressed selection and clipboard `fallback` returns fallback. If the transfer later succeeds, ID 201 is not answered again. |
| `stopSuppressesLateCompletion` | Promote a helper-held read with interested ID 301, stop twice before EOF, then release helper's writer and process queued events for 400 ms. No captureFinished for 301 after stop, no capture log after stop, no pending capture; late source start/request calls open no connection and emit no answer. |

The chunk schedule for each case is measured from the helper's receipt of
its first receive request (t=0), unless the case explicitly names a request or
selection time. Before promoting a newly set offer, wait until the fresh or
previously settled source reports `hasPendingCapture()==true`; a helper set
acknowledgement alone is not proof that the source received its event.
Offer MIME arrays not otherwise stated are exactly
`{text/plain;charset=utf-8}`. The overlap case synchronises both the helper
receive and the first request call in the runner before scheduling its second
call, and asserts that their observed start difference is within 10 ms; a
heavily delayed setup fails explicitly instead of silently changing the timing
oracle. The one-byte absolute-deadline case uses `x` as every byte. The
activation-timeout case sends `new-` at 0 ms, `pend` at 100 ms, `ing` at
200 ms, holds the writer, then closes at 400 ms; its valid eventual cache is
`new-pending`, delivered to no already-timed-out request. Empty-result and
fallback assertions supply the exact clipboard string stated in each case.

Use source signals/cache and helper receive records as primary assertions; logs
add the exact protocol diagnostics. The receipt-time case uses only its
already-required first-event capability log as a receipt barrier, so no public
test accessor or private-display handle is needed. The log hook queues the
10 ms action; it does not synchronously reenter the source. Assert that the
copy action actually occurred before the first receive, that no activation
promoted this case's read before the copy, and that capturedAt is less than
the separately recorded copy stamp. A timestamp wrongly assigned at transfer
completion fails those comparisons. A source assertion also pins the actual
assignment to the primary callback entry.

The script's final success marker is exactly
`wayland-selection-live: PASS (13 cases)`; each case prints its name and PASS
only after all its assertions. On any failure print
`wayland-selection-live: FAIL <case>` and exit 1; a watchdog exits 124. Trap
EXIT/INT/TERM/HUP, terminate only child processes created by the script, wait
with bounded escalation, remove temporary files, and leave the user's running
compositor/session/configuration untouched. Do not claim portal activation or
popup UI coverage from this runner.

## 9. Acceptance criteria

These criteria apply to the implemented patch, not to this document-only
change. Commands run from the repository root unless stated otherwise.

1. Run `protocols/ext-data-control-v1/regenerate.sh` using scanner 1.26.0,
   then `git diff --exit-code -- protocols/ext-data-control-v1/`: exit 0, XML
   and generated pair unchanged; the XML SHA-256 matches §2.2.
2. Run `qmake6 goldendict.pro CONFIG+=verbose_build_output` then
   `make -j$(nproc)`: application links. On a host where pkg-config resolves
   wayland-client, the compile command defines HAVE_WAYLAND_DATA_CONTROL and
   the link includes that library and generated protocol C object.
3. In a disposable source copy, run that build with a pkg-config wrapper that
   returns nonzero only when an argument is exactly `wayland-client` and
   forwards every other invocation to the real pkg-config; point
   `QMAKE_PKG_CONFIG` at that wrapper. Application and tst_hotkeys build with
   the define/protocol C/library absent, the capability test takes its false
   branch, and the enabled Wayland shortcut still registers and translates
   clipboard text. Do not emulate this by manually undefining a true probe.
4. Run `make tests-makefile`: qmake prints `exact 20 suites` with the exact
   inventory in §2.3. Removing the new project in a disposable copy makes
   configuration fail on inventory closure; an extra tst project also fails.
5. Run `make tests-build`, then
   `QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER="$PWD/scripts/test-wrapper.sh" make tests-run`:
   the aggregate executes 20 suites, including all 14 new logic cases and all
   new hotkeys cases, with no failing assertion. Existing environment-specific
   nonXcbCapability requires its separate offscreen invocation.
6. Run `QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_waylandselection/tst_waylandselection`:
   all 16 init/test/cleanup functions pass with no compositor; no Wayland
   connection is attempted.
7. Run `QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_hotkeys/tst_hotkeys nonXcbCapability`:
   the pre-existing non-X11 capability check passes. Run the new selected
   hotkeys cases under xcb/Xvfb, and the matrix's real Wayland child under its
   nested compositor: every exact label/value assertion passes.
8. Run `scripts/verify-wayland-selection-live.sh`: all 13 named cases pass,
   final marker is exactly §8.4's PASS marker and exit is 0. Removing the
   absolute deadline, treating short reads as completion, setting both pipe
   ends non-blocking, restarting overlap reads, or stamping at completion must
   fail its corresponding case in a disposable mutation copy.
9. Start with a legacy config lacking both new keys, load then save: no
   monitoring begins; saved enable is 0 and key is Meta+Ctrl+D. Merely opening
   and accepting Preferences preserves an unedited noncanonical/unsupported
   stored key text exactly.
10. On a KWin Wayland session exposing the ext manager, enable the third row,
    accept the portal assignment, select a word in another application and
    press the assigned shortcut: that word is translated in the popup. Making
    the selection alone produces no new shortcut-driven popup. Repeat with a
    selection in GoldenDict's article: the same primary-selection contract
    applies.
11. Keep that old primary selection, copy a different word, then press the
    shortcut: the copied word wins. Select a fresh word and press immediately:
    promoted capture translates the new word, not the previous cached one;
    an unfinished capture at 300 ms yields clipboard-only or empty handling.
12. Set send-to-main-window, select/activate: the phrase goes to the main
    window. With a visible popup already showing the same sanitised phrase,
    repeat activation: it is handled without a duplicate translation/empty
    warning. With the input-length limit rejecting a phrase, nothing opens
    and the empty-result channel is attempted.
13. With a fresh source and no retained selection/clipboard phrase, activate
    while main window visible: exact empty message appears there for 3000 ms.
    Hide with tray present: a GoldenDict tray message is requested. Hide with
    no tray: only selection-empty is logged. Null primary alone does not
    erase a previously nonempty sticky cache, so prepare this test with a
    fresh source or successful empty payload.
14. Turn the preference off while a capture is pending: monitoring stops,
    pipe/connection close, and no late result translates. Turn it on again:
    a fresh connection starts. Open/cancel Preferences or rescan dictionaries
    with it enabled: popup recreation does not reconnect the selection source.
15. On a compositor without the ext manager, the enabled action remains
    registered, row suffix is exactly `selection unavailable: no ext_data_control_manager_v1 — clipboard is used instead`,
    and clipboard lookup works. In a no-capture build the suffix is instead
    `selection unsupported in this build — clipboard is used instead`.
16. On xcb with both old popup-enabling preferences off and a synced new
    preference true, start/exit: no new popup is constructed and its stored
    geometry is not overwritten by that preference. The third row is unchecked
    and disabled with its X11 label; accepting it retains both stored Wayland
    fields. On logical Windows/macOS matrix rows the generic non-Wayland label
    and preservation assertions pass; no native release validation is inferred.
17. Read source-assertion output: MainWindow wiring and unforced protocol-loop
    branches are labelled as source evidence. Documentation retains hover and
    X11-focus limits, names ext-only coverage and clipboard fallback, and does
    not claim the live runner proves a popup/focus grant or a surviving
    compositor restart.

## 10. Out of scope and stated limits

Carry these statements into the implementation documentation with the same
meaning as revision 7:

- wlroots compositors (sway, hyprland, labwc) expose `zwlr_data_control_v1`
  instead. Support is deferred. A later implementation can add its generated
  pair and bind attempt behind the same source interface; the pure selection
  logic need not change.
- GNOME/Mutter exposes neither data-control protocol, so a background app
  cannot read its primary selection through this feature. A future GNOME-side
  protocol or the RemoteDesktop Clipboard portal with remote-control consent
  would be needed. The shortcut remains registered and falls back to clipboard.
- Only the first seat is watched; there is no simultaneous multi-seat coverage.
- Data-control carries no owner identity. GoldenDict cannot distinguish a
  selection made elsewhere from one made inside itself; both are eligible.
- A compositor can bind the manager without ever sending a primary event.
  Available plus primarySelectionSeen=false says `selection not observed yet`,
  not that primary selection was proven readable.
- Clipboard-change scanning, explicit clipboard lookup and the new
  selected-text action remain separate. Automatic selection popup and held
  modifiers on Wayland are outside this change. Hover lookup and X11 focus
  restoration remain X11-only.
- A private-connection protocol error ends only this watcher. A compositor
  restart also kills Qt's Wayland connection and Qt terminates the process;
  application survival/reconnection is not promised.
- Activation is best-effort. Materialising a native handle and applying a
  token requests activation; the compositor may decline focus.
- A hidden main window without a tray object has no visible notification
  channel. Empty activation is logged only. A desktop can also suppress a
  requested tray notification; the application cannot guarantee its display.
- `lastText` is intentionally sticky across null selection. A timed-out
  promoted activation suppresses that cache for that request; it does not
  turn old text into a newly captured selection.
- The 300 ms deadline is a source policy under event-loop progress; there is
  no claim of progress through OS suspension or unrelated GUI-thread blocking.
- Logic-only tests do not exercise the Wayland session. The hotkeys suite
  does not link MainWindow, ScanPopup or the source. Its source assertions pin
  wiring without proving its runtime effect. The separate live script links
  the real source and proves its stated protocol cases, including receipt
  stamps, MIME policy and overlapping requests; end-to-end popup opening and
  the visible-channel ladder still require the reporter's manual session.
  Flush-pressure/removal/error paths that the live harness cannot force are
  explicitly source assertions, not claimed compositor observations.

The existing limits paragraph is at `README.md:229`, and the canonical
clipboard/hover/focus discussion is at `docs/wayland-limitations.md:30`.
In the README shortcut bullet, replace its clipboard/hover sentences with:

> Selected-text translation is available through a global shortcut on
> compositors exposing `ext_data_control_manager_v1`, with clipboard fallback
> when primary-selection capture is unavailable. Clipboard-change scanning and
> explicit clipboard lookup remain separate behaviours. Hover translation and
> X11 focus restoration remain X11-only.

Add the same coverage/fallback statement and the four deferred-coverage points
(wlroots, GNOME, first-seat-only and no owner identity) to §2 of the canonical
limits document. Preserve its historical evidence and absence of unobserved
acceptance receipts; do not manufacture successful runs. The approved design
is not modified by this specification or by its integration instructions.
