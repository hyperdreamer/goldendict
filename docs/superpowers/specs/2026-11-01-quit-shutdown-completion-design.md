# Quitting GoldenDict never completes: `closeEvent` cancels Qt's own quit

## Background

Quitting GoldenDict with default preferences leaves the process running forever behind a
visible, greyed-out, unresponsive main window. The window cannot be closed, the process
does not exit, and only a kill removes it. The user-visible symptom is "the app becomes
unresponsive on quit".

### Reproduction

Environment: Plasma Wayland (`platform=wayland`, hotkey backend `portal`), GoldenDict
1.5.1.r2875 (shutdown sources byte-identical to `a33fdea8`), default preferences
(`enableTrayIcon=1`, `closeToTray=1`, `clearNetworkCacheOnExit=1`, 50 MB HTTP cache).

1. Start GoldenDict and let it finish initialising.
2. Tray → Quit, Ctrl+Q, or a window close when `closeToTray=0`.
3. Observe: the window is redrawn and disabled, config is written, and then nothing.
   The process stays alive indefinitely.

Measured state while hung:

- `main()` is still inside `app.exec()` (`main.cc:512`); the main thread is idle in
  `QEventLoop::exec()` inside the glib dispatcher.
- `QCoreApplication::quit()` was entered and returned; the event loop was not asked to
  exit. A second `quit()` from a later timer also does nothing, and a nested
  `QEventLoop` runs its full 300 ms timeout, which proves `quitNow` was never set.

### Root cause

Qt 6.12 implements `quit()` as "close all windows, then exit", and a window that refuses
that close cancels the quit:

```cpp
// qtbase 6.12, src/corelib/kernel/qcoreapplication.cpp
void QCoreApplication::quit()
{
    if (!self) return;
    if (!self->d_func()->in_exec) return;
    self->d_func()->quit();          // sends QEvent::Quit to the application object
}

// qtbase 6.12, src/widgets/kernel/qapplication.cpp
bool QApplication::event(QEvent *e)
{
    case QEvent::Quit:
        closeAllWindows();
        for (auto *w : topLevelWidgets()) {
            if (w->data->is_closing) continue;
            if (w->isVisible() && !(w->windowType() == Qt::Popup) && ... ) {
                e->ignore();                     // the quit is cancelled
                return true;
            }
        }
        return QCoreApplication::event(e);       // -> exit(0)
}
```

`closeAllWindows()` closes the active modal widget first, then every other top-level
window. GoldenDict supplies two windows that refuse to close — the main window
(`mainwindow.cc:1239`) and the application-modal indexing dialog
(`initializing.cc:78`, `initializing.ui:4`) — and it keeps the main window visible on
purpose while the cache barrier runs (`mainwindow.cc:1257`):

```cpp
void MainWindow::closeEvent( QCloseEvent * ev )
{
  if( shuttingDown ) { ev->ignore(); return; }   // cancels Qt's quit
  ...
  else { ev->ignore(); quitApp(); }
}

void MainWindow::quitApp()
{
  if( shuttingDown ) return;
  shuttingDown = true;
  ...
  show();                    // a visible top-level window ...
  setEnabled( false );       // ... that is disabled but still on screen
  ...
  if( articleProfile && cfg.preferences.clearNetworkCacheOnExit )
    articleProfile->clearForExit();
  else
    finishShutdown();        // -> destroyArticleViews(); qApp->quit();
}
```

### Why the defect is intermittent rather than obvious

Whether the quit is cancelled depends on **whether a close event is already being
handled**, not on the cache:

- Shutdown initiated inside a window-close event: Qt sets `is_closing` on that window
  while the close is dispatched, so `closeAllWindows()` and the verification loop skip
  it. That single quit is not cancelled and the app exits.
- Shutdown initiated any other way — tray → Quit (`mainwindow.cc:398`), Ctrl+Q
  (`mainwindow.cc:667`), a second close, a session-manager request — reaches `quit()`
  with no close in progress, so the visible window vetoes it and `exit(0)` is never
  reached. Every later `quit()` is vetoed the same way.

The asynchronous cache barrier (`clearNetworkCacheOnExit=1`, the default) is one reliable
way to lose the close-event context, because `clearForExit()` moves `finishShutdown()` and
its `quit()` to a later event-loop turn. It is not the only way: with
`clearNetworkCacheOnExit=0`, a tray/Ctrl+Q quit still hangs, because that path never had a
close event to begin with. Conversely, a window close with `clearNetworkCacheOnExit=0`
exits cleanly. Measured matrix (pre-fix binary):

| Initiation | Barrier on | Barrier off |
| --- | --- | --- |
| Window close (`closeToTray=0`) | hangs | exits |
| Tray Quit / Ctrl+Q | hangs | hangs |

The cache clear itself is not at fault: with the cache clear, the WebEngine clear, the
disk-cache clear, the portal shutdown, the scan-popup destruction, the FTS stop and the
article-view destruction all skipped, the deferred path still hangs.

## Goals

1. A quit through any supported entry point terminates the process; `app.exec()` returns
   normally with `main()` reaching its cleanup.
2. Default preferences are covered: the asynchronous cache barrier, the disabled window,
   and both `closeToTray` settings.
3. Every GoldenDict window honours a close request once shutdown has started — including
   the application-modal indexing dialog, so a session logout during a dictionary rescan
   cannot be vetoed.
4. No new application work starts after shutdown has begun: the dictionary-reload
   continuation is abandoned instead of restarting FTS indexing and recreating the scan
   popup.
5. A close request that arrives after shutdown has started is accepted, never silently
   swallowed.
6. The regression is pinned by a check that fails on the current code and passes on the
   fixed code, for each supported initiation path.
7. A normal close keeps its current behaviour: hide to tray when `closeToTray=1`, quit
   when it is off.

## Non-goals

- No change to the cache barrier, its completion signal, or its 5 s timeout.
- No process-level watchdog or hard exit; Option C was considered and rejected.
- No change to the shutdown UI (the window stays visible and disabled while the barrier
  runs), only to whether a close request is honoured.
- No cancellation of an in-flight dictionary load: the loader is drained, never
  terminated, because terminating it can corrupt index files.
- No change to WebEngine teardown, portal hotkey teardown, or article-view destruction.
- No new QtTest suite in the closed 19-suite inventory.

## Architecture

### D1 — One process-wide shutdown flag, set before any quit is requested

```cpp
// shutdownstate.hh
#ifndef GOLDENDICT_SHUTDOWNSTATE_HH_INCLUDED
#define GOLDENDICT_SHUTDOWNSTATE_HH_INCLUDED

/// Process-wide "shutdown has begun" flag.
///
/// Qt's quit() closes top-level windows and cancels the quit while any visible
/// window refuses to close. Once shutdown has begun every window must therefore
/// accept a close request. Set exactly once, from MainWindow::quitApp(), before
/// any code path can emit QEvent::Quit.
namespace ShutdownState {
void request() noexcept;
bool requested() noexcept;
}
#endif
```

`shutdownstate.cc` holds one `std::atomic<bool>`; `request()` stores `true`,
`requested()` loads it. No policy, no Qt dependency beyond `<atomic>`.

`MainWindow::quitApp()` calls `ShutdownState::request()` at the same point it sets its
member `shuttingDown`, and `shuttingDown` keeps its existing role as the main window's
local latch. `goldendict.pro` gains `shutdownstate.hh`/`shutdownstate.cc`.

**Ordering requirement.** Every supported entry point (tray Quit, Ctrl+Q, window close,
session logout) reaches `quitApp()` *before* any `QEvent::Quit` is generated, because
`quit()` is only ever called from `finishShutdown()` after the flag is set. Session logout
takes the `commitDataRequest` → `MainWindow::commitData(QSessionManager&)` → `quitApp()`
route, which also sets the flag first. A bare application-level `Quit` that bypasses
`quitApp()` is not a supported entry point; the application never emits one, and the
regression check exercises the four real entry points.

### D2 — While shutting down, a close request succeeds (main window)

```cpp
void MainWindow::closeEvent( QCloseEvent * ev )
{
  // Qt's quit() closes every top-level window and cancels the quit while any
  // visible window refuses to close. Once shutdown has started a close request
  // must therefore succeed: the deferred cache barrier reaches quit() from a
  // later event-loop turn, where this window is no longer marked is_closing.
  if( shuttingDown || ShutdownState::requested() ) { ev->accept(); return; }
  ...
}
```

`shuttingDown` stays in the condition so the fail-startup path
(`MainWindow::failStartup()`, which sets `shuttingDown` and calls
`QCoreApplication::exit(1)`) behaves exactly as today even though it never requests a
shutdown.

Because `MainWindow` is not `WA_DeleteOnClose`, accepting the close hides the window; the
object still lives until `main()` returns.

### D3 — The modal indexing dialog honours the same rule

```cpp
// initializing.cc
void Initializing::closeEvent( QCloseEvent * ev )
{
  if( ShutdownState::requested() ) { ev->accept(); return; }
  ev->ignore();
}
```

`Initializing` is created on the stack inside `loadDictionaries()` and lives across a
dictionary (re)index, so it can be the active modal widget when a session-manager logout
arrives. `closeAllWindows()` tries the active modal widget first, so without this change
that dialog alone can still veto the quit and reproduce the hang.

`Initializing::reject()` stays empty: Escape keeps working exactly as it does now, and
the close rule is the only behavioural change.

### D4 — A dictionary reload is abandoned once shutdown has begun

`loadDictionaries()` runs a nested loop and then an unconditional `loadDicts.wait()`
(`loaddictionaries.cc:271-273`). `QCoreApplication::exit()` unwinds that nested loop, so
after a quit during indexing the continuation would otherwise run *during* shutdown:

```cpp
// mainwindow.cc, on_rescanFiles_triggered() / makeDictionaries()
loadDictionaries( this, ... );
// ... continue only when shutdown has NOT begun:
for( auto &d : dictionaries ) { d->setFTSParameters(...); ... }
ftsIndexing.setDictionaries( dictionaries );
ftsIndexing.doIndexing();
updateGroupList();
makeScanPopup();          // <-- would recreate the destroyed scan popup
installHotKeys();
```

Both continuation sites return immediately after `loadDictionaries()` when
`ShutdownState::requested()` is true:

```cpp
loadDictionaries( this, isVisible(), cfg, dictionaries, dictNetMgr );
if( ShutdownState::requested() ) return;   // never restart FTS or recreate the popup
```

`on_rescanFiles_triggered()` already refuses to *start* a rescan while `shuttingDown`, so
this closes the window between start and finish.

**Policy for the in-flight load.** Shutdown drains the loader: `loadDicts.wait()` is left
in place and the worker is never terminated, because terminating a thread that is writing
an index can corrupt the file. A quit during indexing therefore exits after the in-flight
load finishes rather than immediately, and the reload continuation is discarded. The
5 s cache barrier is unaffected; this is the only path where shutdown waits on work.

### D5 — Second quit request, and which teardown path runs

**Policy.** If a Quit arrives while the cache wait is in flight (for example the
session-manager `Die` callback), the window now closes, `app.exec()` returns, and
`main()` tears the application down without waiting for `clearHttpCacheCompleted`. This is
deliberate: the barrier is best-effort, and refusing the close is exactly the behaviour
that hangs the application.

**Which teardown runs.** On the normal path the barrier completes and `finishShutdown()`
calls `destroyArticleViews()` before `quit()`. On the early-quit path `finishShutdown()`
never runs and the same work happens in `MainWindow::~MainWindow()`, which calls
`destroyArticleViews()` and then resets the profile (`mainwindow.cc:1052`). Both paths
destroy the article views before the profile; neither leaves a page attached to a dead
profile.

### D6 — The regression is pinned outside the QtTest inventory

`tests/tests.pro` is a closed inventory of 19 suites, and `tests/article-test.pri`
guarantees that the suites never link `main.cc` or `MainWindow`. This defect lives in the
interaction between GoldenDict's close handlers and Qt's `quit()`, so a unit test of an
extracted helper would neither have caught it nor catch a regression.

The guard is a headless end-to-end check driving the real binary:

```
tests/shutdown/run-quit-check.sh <goldendict-binary> <variant>
```

`<variant>` is one of `barrier`, `no-barrier`, `close-to-tray`, and each variant names its
own preferences, its initiation path and its expected pre-fix/post-fix outcome:

| Variant | Preferences | Initiation | Pre-fix | Post-fix |
| --- | --- | --- | --- | --- |
| `barrier` | `closeToTray=0`, `clearNetworkCacheOnExit=1` | window close | hangs | exit 0 |
| `no-barrier` | `closeToTray=0`, `clearNetworkCacheOnExit=0` | focused Ctrl+Q | hangs | exit 0 |
| `close-to-tray` | `closeToTray=1`, `clearNetworkCacheOnExit=1` | window close, then quit | hides, then hangs | hides, then exit 0 |

`no-barrier` deliberately uses Ctrl+Q: a *window close* with the barrier off already
succeeds pre-fix and would not pin anything. `close-to-tray` asserts its two stages
separately, because the hide stage passes on both versions and only the quit stage is the
regression.

The script:

1. Creates a throwaway root with a **short** `TMPDIR` (the single-instance socket name is
   `qtsingleapp-…-<uid>` inside `QDir::tempPath()`, `qtlocalpeer.cpp:85-108`) plus private
   `HOME`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME`.
2. **Rejects a portable binary**: `Config::isPortableVersion()` is exactly "a `portable`
   directory next to the executable" (`config.cc:2464-2470`), and portable mode would
   ignore `HOME`/XDG and write inside the build tree. The script fails with an explicit
   "portable build not supported by this check" diagnostic when
   `$(dirname "$binary")/portable` exists.
3. Starts a window manager inside the Xvfb display (`openbox --sm-disable`, verified with
   `wmctrl -m`). A WM is required for a genuine close request: `xdotool windowquit` sends
   EWMH `_NET_CLOSE_WINDOW` (verified in `libxdo`, which contains no
   `WM_DELETE_WINDOW` path), and `xdotool windowclose` destroys the window instead of
   asking it to close. The check fails with a clear diagnostic if no WM is running;
   `openbox` is a documented dependency of this check only.
4. Launches the binary under `xvfb-run -a` with `QT_QPA_PLATFORM=xcb`, capturing stderr,
   and waits a bounded time (30 s) for the `startup-ready platform=… backend=…` record
   (`mainwindow.cc:1946`). Any exit before that record fails the run. The child's PID is
   recorded directly (not the `xvfb-run` wrapper's), and the window is matched with
   `xdotool search --pid <child-pid>` rather than by title.
5. Delivers the initiation for the variant (`wmctrl -c` / focused `xdotool key ctrl+q`).
6. Asserts: process exits within 10 s with status **exactly 0** for `barrier` and
   `no-barrier`; for `close-to-tray`, the window becomes unmapped within 5 s while the
   process stays alive, then a quit is requested and the child exits with status exactly
   0.
7. Never kills the process as part of a passing run, removes its temporary root, and
   prints the captured log on failure.

The script is documented in its own header, is not part of `make check`, and takes the
binary path as an argument so it works for a build tree or a package build.

## Error handling matrix

| Situation | Behaviour |
| --- | --- |
| Tray Quit / Ctrl+Q, barrier on or off | Window closes when Qt's quit runs; process exits |
| Window close, `closeToTray=0`, barrier on | Config saved, window disabled, then hidden by Qt's quit; process exits |
| Window close, `closeToTray=1` (normal operation) | Unchanged: `ev->ignore()`, window hides, app keeps running |
| Second close while shutting down | Accepted: window hides; shutdown continues |
| Quit during dictionary (re)index | Dialog accepts, main window accepts; the in-flight load is drained; the reload continuation is skipped; process exits |
| Quit request during the cache wait (e.g. session `Die`) | Window closes, `exec()` returns, teardown runs from `~MainWindow` |
| Barrier times out after 5 s | Unchanged: `finishExit(true)` logs and completes shutdown |
| Cache completion never arrives, timer alive | Unchanged: the timeout path drives the same `quit()` |
| Fail-startup dialog | Unchanged: `QCoreApplication::exit(1)`; `shuttingDown` alone accepts closes |
| A future top-level window that ignores the flag | Explicitly out of contract: any new window must consult `ShutdownState::requested()` |

## Compatibility

- Qt 6.12 `quit()` semantics are as quoted; earlier 6.x releases also refuse to quit while
  a visible window vetoes the close, so the change is a compatibility improvement rather
  than a version-specific workaround.
- No platform-specific code: the change is in widget event handlers and applies to xcb,
  Wayland, offscreen and Windows/macOS builds alike.
- No configuration, file-format, or interface change; no persisted state changes.
- `goldendict.pro` gains `shutdownstate.hh`/`shutdownstate.cc`. The test closure build
  keeps its existing inputs because neither `MainWindow` nor `Initializing` is part of any
  suite.
- Existing suites are unaffected: no `MainWindow`, `Initializing` or `ShutdownState`
  symbol is linked by any of the 19 suites.

## Test strategy

1. **Regression matrix**: `run-quit-check.sh <binary> <variant>` for all three variants.
   Pre-fix expectations: `barrier` hangs, `no-barrier` hangs, `close-to-tray` hides then
   hangs. Post-fix: every variant exits with status 0. The pre-fix failure of each variant
   is recorded once as evidence.
2. **Modal indexing path**: force a dictionary rescan, trigger quit while the
   `Initializing` dialog is up, and assert that the process exits within a bounded time
   after the loader drains. Driven headlessly by launching with a rescan request where the
   CLI allows it; otherwise performed manually with the observation recorded (dialog
   present, quit requested, process gone, elapsed time).
3. **Existing gate**: `make -C tests check` (19 suites) stays green.
4. **Manual confirmation**: quit from the tray in a real Wayland session; the process
   disappears and no disabled window is left behind.

## Verification plan

1. Build the tree without EPWing support when `libeb` is unavailable
   (`CONFIG+=no_epwing_support`).
2. Run all three variants of `tests/shutdown/run-quit-check.sh` against the pre-fix and
   fixed binaries; record exit codes and elapsed times.
3. Run the modal-indexing check from the test strategy and record the observation.
4. Run the closed inventory: `qmake6 tests/tests.pro -o tests/Makefile`,
   `make -C tests -j$(nproc)`, `QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`.
5. Run the manual Wayland quit once and record the process state before and after.
6. Archive the outputs, exit codes, and the pre-fix failures in the run report.

## Rollout

Two close handlers, one continuation guard, one tiny new module, and one new check under
`tests/shutdown/`; no migration and no feature flag. Shipping it restores the documented
behaviour of the Quit action, the tray menu, the window close button and session logout.
