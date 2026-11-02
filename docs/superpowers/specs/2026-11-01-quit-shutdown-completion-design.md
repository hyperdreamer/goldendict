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
4. No new reload work starts after shutdown has begun on any of the dictionary-reload
   paths: no error dialog, no deferred initialisation, no FTS restart, no scan-popup
   recreation.
5. A close request that arrives after shutdown has started is accepted, never silently
   swallowed.
6. The regression is pinned by a check that fails on the current code and passes on the
   fixed code, for every initiation path that can be driven without a session manager.
7. A normal close keeps its current behaviour: hide to tray when `closeToTray=1`, quit
   when it is off.

## Non-goals

- No change to the cache barrier, its completion signal, or its 5 s timeout.
- No process-level watchdog or hard exit; Option C was considered and rejected.
- No change to the shutdown UI (the window stays visible and disabled while the barrier
  runs), only to whether a close request is honoured.
- No cancellation of an in-flight dictionary load: the loader is drained, never terminated,
  because terminating a thread that is writing an index can corrupt it.
- No change to WebEngine teardown, portal hotkey teardown, or article-view destruction.
- No new QtTest suite in the closed 19-suite inventory.
- **No repair of the two pre-existing shutdown hazards** described under D4; they are
  documented, deliberately out of scope, and unchanged by this work.
- No synthetic session-manager logout in the automated check (see D7).

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
/// accept a close request, and no reload work may restart. Set exactly once, from
/// MainWindow::quitApp(), before any code path can emit QEvent::Quit.
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
`quit()` is only ever called from `finishShutdown()`, after the flag is set. Session
logout takes the `commitDataRequest` → `MainWindow::commitData(QSessionManager&)` →
`quitApp()` route, which also sets the flag first.

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

`Initializing::reject()` stays empty: Escape keeps working exactly as it does now, and the
close rule is the only behavioural change.

### D4 — No reload work starts after shutdown: one checkpoint plus three caller guards

Four UI paths reach `loadDictionaries()`:

| Path | Call | Continuation after the load |
| --- | --- | --- |
| `MainWindow::makeDictionaries()` | `loadDictionaries(..., false)` (`mainwindow.cc:1387`) | FTS parameters, FTS indexing, group list |
| `MainWindow::on_rescanFiles_triggered()` | `loadDictionaries(..., true)` | group list, scan popup, hotkeys, suggestions |
| `EditDictionaries::acceptChangedSources()` | `loadDictionaries(..., true)` (`editdictionaries.cc:217`) | name updates, tab/model rebuild |
| `MainWindow::editDictionaries()` | via the dialog | after `dicts.exec()` (`mainwindow.cc:2154`): config save, groups, scan popup, hotkeys, FTS |

Inside `loadDictionaries()` the worker is joined and then more work happens
(`loaddictionaries.cc:271-282`, `:418`): an error `QMessageBox`, index-directory cleanup,
and `doDeferredInit()`. A caller-side guard alone would leave those running.

**Checkpoint (inside `loadDictionaries()`, immediately after the join):**

```cpp
  localLoop.exec();
  loadDicts.wait();

  // Shutdown drains the loader but starts no further work: no error dialog, no
  // index cleanup, no deferred initialisation, no dictionaries handed back.
  if( ShutdownState::requested() )
    return;

  if( loadDicts.getExceptionText().size() ) { ... }
```

**Caller guards (one line each):**

```cpp
// MainWindow::makeDictionaries()  -- keep the existing trailing `false`
loadDictionaries( this, isVisible(), cfg, dictionaries, dictNetMgr, false );
if( ShutdownState::requested() ) return;

// MainWindow::on_rescanFiles_triggered()
loadDictionaries( this, true, cfg, dictionaries, dictNetMgr );
if( ShutdownState::requested() ) return;

// MainWindow::editDictionaries(), immediately after the modal dialog returns
dicts.exec();
if( ShutdownState::requested() ) return;
```

**Deliberately no guard inside `EditDictionaries::acceptChangedSources()`.** It clears
`groups` and `orderAndProps` before loading (`editdictionaries.cc:211-215`), and
`buttonBoxClicked()` calls `acceptChangedSources(true)` and then `save()`
(`editdictionaries.cc:165-173`), where `save()` dereferences both pointers with no null
check (`editdictionaries.cc:85-89`). An early return from that function would therefore
turn a shutdown during an Apply into a null dereference. The function is allowed to
finish; `MainWindow::editDictionaries()`'s guard stops everything that would follow, and
the internal checkpoint keeps the reload from starting new work or showing the error
dialog.

**Policy for the in-flight load.** Shutdown drains the loader: `loadDicts.wait()` stays,
and the worker is never terminated, because terminating a thread that is writing an index
can corrupt the file. A quit during indexing therefore exits after the in-flight load
finishes rather than immediately, and every continuation listed above is discarded. The
cache barrier is unaffected. This is not the only place shutdown can wait on work: FTS
search draining (`fulltextsearch.cc:380-382`) and `ftsIndexing.stopIndexing()` also wait.

**Two pre-existing hazards, unchanged and out of scope.** (a) `save()` after an Apply
whose reload was interrupted, as described above, is only safe because
`acceptChangedSources()` is allowed to finish; the design does not restructure that
lifecycle. (b) `closeFullTextSearchDialog()` → `stopSearch()` calls
`QApplication::processEvents()` in a loop (`fulltextsearch.cc:371-382`), so `quitApp()` can
be entered from inside cleanup. Both predate this change, are reachable on `master` today
via session logout, and are recorded here so the reviewer and the fixer do not mistake them
for regressions introduced by this work.

### D5 — Three distinct "second quit" events

The phrase "second quit request" was ambiguous; the three cases are different:

| Event | Behaviour |
| --- | --- |
| Another `quitApp()` call (tray, Ctrl+Q) | Returns at the existing `shuttingDown` latch. The barrier keeps running, teardown continues normally. |
| Another window close | `closeEvent` accepts it, the window hides, shutdown continues. `quitOnLastWindowClosed` is false, so nothing else happens. |
| A delivered application `QEvent::Quit` (session-manager `Die`) | `QApplication::event()` closes the windows, `exec()` returns, and teardown happens in `~MainWindow` without waiting for `clearHttpCacheCompleted`. The barrier is best-effort. |

**Which teardown runs.** On the normal path the barrier completes and `finishShutdown()`
calls `destroyArticleViews()` before `quit()`. On the `QEvent::Quit` path `finishShutdown()`
never runs and the same work happens in `MainWindow::~MainWindow()`, which calls
`destroyArticleViews()` and then resets the profile (`mainwindow.cc:1052`). Both paths
destroy the article views before the profile; neither leaves a page attached to a dead
profile.

### D6 — The regression check

`tests/tests.pro` is a closed inventory of 19 suites, and `tests/article-test.pri`
guarantees that the suites never link `main.cc` or `MainWindow`. This defect lives in the
interaction between GoldenDict's close handlers and Qt's `quit()`, so a unit test of an
extracted helper would neither have caught it nor catch a regression.

```
tests/shutdown/run-quit-check.sh <goldendict-binary> <variant>
```

`<variant>` is one of `barrier`, `no-barrier`, `close-to-tray`:

| Variant | Seeded preferences | Initiation | Pre-fix | Post-fix |
| --- | --- | --- | --- | --- |
| `barrier` | `enableTrayIcon=1`, `startToTray=0`, `closeToTray=0`, `clearNetworkCacheOnExit=1` | graceful window close (`wmctrl -c`) | hangs | exit 0 |
| `no-barrier` | `enableTrayIcon=1`, `startToTray=0`, `closeToTray=0`, `clearNetworkCacheOnExit=0` | focused `Ctrl+Q` | hangs | exit 0 |
| `close-to-tray` | `enableTrayIcon=1`, `startToTray=0`, `closeToTray=1`, `clearNetworkCacheOnExit=1` | graceful window close, then quit | hides, then hangs | hides, then exit 0 |

`enableTrayIcon=1` is explicit because `closeEvent` requires **both** `enableTrayIcon` and
`closeToTray` to hide, and a missing `<enableTrayIcon>` element inside an existing
`<preferences>` block is read as false (`config.cc:942`).

`no-barrier` deliberately uses Ctrl+Q: a window close with the barrier off already succeeds
pre-fix and would pin nothing. `close-to-tray` asserts its two stages separately, because
the hide stage passes on both versions and only the quit stage is the regression.

**`close-to-tray` final quit.** After the window hides, the check launches a **second
instance of the same binary in the same isolated environment**; the existing single-
instance IPC delivers `bringToFront` (`main.cc:355`), the original window becomes visible
and is focused, and the check then sends `Ctrl+Q`. The second instance exits by itself
(`main.cc:357`); the check keeps tracking and waiting for the **original** child PID.

**Common procedure.**

1. Throwaway root with a **short** `TMPDIR` (single-instance socket is
   `qtsingleapp-…-<uid>` inside `QDir::tempPath()`, `qtlocalpeer.cpp:85-108`) plus private
   `HOME`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME`, `XDG_RUNTIME_DIR`, and
   **`SESSION_MANAGER` unset** so Qt's xcb platform cannot attach to the live XSMP
   session. The application, the second instance, the window manager and the driving
   tools all run inside this same environment.
2. **Portable rejection:** resolve the binary with `readlink -f`, fail with
   "portable build not supported by this check" when `$(dirname <resolved>)/portable`
   exists (`Config::isPortableVersion()` is exactly that directory test,
   `config.cc:2464-2470`), and launch the resolved path so the check and Qt agree on
   `applicationDirPath()`.
3. Start a window manager inside the display (`openbox --sm-disable`, verified with
   `wmctrl -m`), because `xdotool windowquit` sends EWMH `_NET_CLOSE_WINDOW` (verified:
   `libxdo` contains no `WM_DELETE_WINDOW` path) and `xdotool windowclose` destroys the
   window instead of asking it to close. Missing WM is a hard, explained failure;
   `openbox` is a dependency of this check only.
4. Launch under `xvfb-run -a` with `QT_QPA_PLATFORM=xcb`, capture stderr, wait up to 30 s
   for the `startup-ready platform=… backend=…` record (`mainwindow.cc:1946`); any exit
   before it fails the run. Record the child PID directly (never the `xvfb-run` wrapper)
   and match windows with `xdotool search --pid <child-pid>`.
5. Apply the initiation above, then assert: exit within 10 s with status **exactly 0**
   (a crash or any non-zero status fails), except for the documented two-stage
   `close-to-tray` variant.
6. Never kill the process as part of a passing run; remove the temporary root; print the
   captured log on failure.

The script is documented in its own header, is not part of `make check`, and takes the
binary path as an argument.

### D7 — Coverage: what is automated and what is manual

| Entry point | Coverage | Note |
| --- | --- | --- |
| Window close (tray off) | automated: `barrier` | needs the WM |
| Ctrl+Q / File → Quit | automated: `no-barrier`, and the final stage of `close-to-tray` | |
| Hide-to-tray on close | automated: first stage of `close-to-tray` | proves the tray branch is untouched |
| Tray → Quit | **manual** | requires a StatusNotifier host; none runs under Xvfb |
| Session logout during a dictionary rescan | **manual** | requires a real session host, and it is the only way to call `quitApp()` while the modal `Initializing` dialog owns input; the route (`commitDataRequest` → `quitApp()`) is the same one the automated variants exercise |

The modal-rescan path cannot be automated: closing `Initializing` does **not** call
`quitApp()`, so `ShutdownState` stays false and the dialog ignores the close on both the
fixed and unfixed builds. A variant built on that sequence would prove nothing, so it is
not part of the check. The manual procedure — Force a rescan (Ctrl+F5), wait for the
`Initializing` dialog, then log out of the session — is recorded with the observed process
state and elapsed time.

## Error handling matrix

| Situation | Behaviour |
| --- | --- |
| Tray Quit / Ctrl+Q, barrier on or off | Window closes when Qt's quit runs; process exits |
| Window close, `closeToTray=0`, barrier on | Config saved, window disabled, then hidden by Qt's quit; process exits |
| Window close, `closeToTray=1` (normal operation) | Unchanged: `ev->ignore()`, window hides, app keeps running |
| Second window close while shutting down | Accepted: window hides; shutdown continues |
| Quit during dictionary (re)index | Dialog accepts, main window accepts; loader drained; checkpoint and caller guards discard the continuation; process exits |
| Load finishes with an error during shutdown | No error dialog is shown; the reload is abandoned |
| Apply in the Dictionaries dialog interrupted by shutdown | `acceptChangedSources()` completes so `save()` never sees cleared models; `MainWindow::editDictionaries()` discards everything after `dicts.exec()` |
| Application-level `QEvent::Quit` during the cache wait | Window closes, `exec()` returns, teardown runs from `~MainWindow`; the barrier is best-effort |
| Another `quitApp()` call during shutdown | Returns at the `shuttingDown` latch; teardown continues |
| Barrier times out after 5 s | Unchanged: `finishExit(true)` logs and completes shutdown |
| Fail-startup dialog | Unchanged: `QCoreApplication::exit(1)`; `shuttingDown` alone accepts closes |
| A future top-level window that ignores the flag | Out of contract: any new window must consult `ShutdownState::requested()` |

## Compatibility

- Qt 6.12 `quit()` semantics are as quoted; earlier 6.x releases also refuse to quit while
  a visible window vetoes the close, so the change is a compatibility improvement rather
  than a version-specific workaround.
- No platform-specific code: the change is in widget event handlers, plus one pure flag
  module, and applies to xcb, Wayland, offscreen and Windows/macOS builds alike.
- No configuration, file-format, or interface change; no persisted state changes.
- `goldendict.pro` gains `shutdownstate.hh`/`shutdownstate.cc`. The test closure build
  keeps its existing inputs because none of `MainWindow`, `Initializing`,
  `EditDictionaries` or `ShutdownState` is linked by any suite.
- Existing suites are unaffected.

## Test strategy

1. **Regression matrix**: `run-quit-check.sh <binary> <variant>` for the three variants.
   Pre-fix: `barrier` hangs, `no-barrier` hangs, `close-to-tray` hides then hangs.
   Post-fix: every variant completes with exit status 0. The pre-fix failure of each
   variant is recorded once as evidence.
2. **Existing gate**: `make -C tests check` (19 suites) stays green.
3. **Manual**: tray Quit on Wayland, and a session logout during a rescan (the only way to
   reach `quitApp()` while `Initializing` owns input); both recorded with process state
   before, during and after, plus elapsed time.

## Verification plan

1. Build the tree without EPWing support when `libeb` is unavailable
   (`CONFIG+=no_epwing_support`).
2. Run the three variants of `tests/shutdown/run-quit-check.sh` against the pre-fix and
   fixed binaries; record exit codes and elapsed times.
3. Run the closed inventory: `qmake6 tests/tests.pro -o tests/Makefile`,
   `make -C tests -j$(nproc)`, `QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`.
4. Run the two manual steps and record the observations.
5. Archive outputs, exit codes, and the pre-fix failures in the run report.

## Rollout

Changed files: `mainwindow.cc` (close rule, two caller guards), `initializing.cc` (close
rule), `loaddictionaries.cc` (checkpoint), new `shutdownstate.hh`/`shutdownstate.cc`,
`goldendict.pro`, and the new `tests/shutdown/run-quit-check.sh`. No migration and no
feature flag. Shipping it restores the documented behaviour of the Quit action, the tray
menu, the window close button and session logout.
