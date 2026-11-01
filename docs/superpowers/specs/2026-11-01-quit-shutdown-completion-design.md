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

1. A quit through any existing entry point terminates the process; `app.exec()` returns
   normally with `main()` reaching its cleanup.
2. Default preferences are covered: the asynchronous cache barrier, the disabled window,
   and both `closeToTray` settings.
3. Every GoldenDict window honours a close request once shutdown has started — including
   the application-modal indexing dialog, so a session logout during a dictionary rescan
   cannot be vetoed.
4. A close request that arrives after shutdown has started is accepted, never silently
   swallowed.
5. The regression is pinned by a check that fails on the current code and passes on the
   fixed code.
6. A normal close keeps its current behaviour: hide to tray when `closeToTray=1`, quit
   when it is off.

## Non-goals

- No change to the cache barrier, its completion signal, or its 5 s timeout.
- No process-level watchdog or hard exit; Option C was considered and rejected.
- No change to the shutdown UI (the window stays visible and disabled while the barrier
  runs), only to whether a close request is honoured.
- No change to WebEngine teardown, portal hotkey teardown, FTS shutdown, or article-view
  destruction.
- No new QtTest suite in the closed 19-suite inventory.

## Architecture

### D1 — One process-wide shutdown flag

A new, deliberately tiny module owns the rule:

```cpp
// shutdownstate.hh
#ifndef GOLDENDICT_SHUTDOWNSTATE_HH_INCLUDED
#define GOLDENDICT_SHUTDOWNSTATE_HH_INCLUDED

/// Process-wide "shutdown has begun" flag.
///
/// Qt's quit() closes top-level windows and cancels the quit while any visible
/// window refuses to close. Once shutdown has begun every window must therefore
/// accept a close request. Set exactly once, from MainWindow::quitApp().
namespace ShutdownState {
void request() noexcept;
bool requested() noexcept;
}
#endif
```

`shutdownstate.cc` holds one `std::atomic<bool>`; `request()` stores `true`,
`requested()` loads it. No behaviour, no policy, no Qt dependency beyond `<atomic>`.

`MainWindow::quitApp()` calls `ShutdownState::request()` at the same point it sets its
member `shuttingDown`, and `shuttingDown` keeps its existing role as the main window's
local latch.

The interface is a function pair rather than an exported `bool` so that the rule can only
be set in one direction, and so that a reader can find every participant with one grep.

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

`shuttingDown` is retained in the condition so the fail-startup path
(`MainWindow::failStartup()`, which sets `shuttingDown` and calls
`QCoreApplication::exit(1)`) behaves exactly as it does today even though it does not
request a shutdown.

Because `MainWindow` is not `WA_DeleteOnClose`, accepting the close hides the window; the
object still lives until `main()` returns, and `destroyArticleViews()` still runs in
`finishShutdown()`.

### D3 — The modal indexing dialog honours the same rule

`Initializing` is an application-modal `QDialog` that refuses every close today:

```cpp
// initializing.cc
void Initializing::closeEvent( QCloseEvent * ev )
{
  if( ShutdownState::requested() ) { ev->accept(); return; }
  ev->ignore();
}
```

The dialog is created on the stack inside `loadDictionaries()` and lives across a
dictionary (re)index, so it can be the active modal widget when a session-manager logout
arrives. `closeAllWindows()` tries the active modal widget before any other window, so
without this change that dialog alone can still veto the quit and reproduce the hang.
`Initializing::reject()` stays empty: Escape must keep working as it does now, and the
close rule is the only thing that changes.

### D4 — Everything else stays exactly as it is

The cache barrier keeps its completion signal and its 5 s timeout, `quitApp()` keeps
`show()`/`setEnabled(false)`, `finishShutdown()` keeps its order (scan popup, article
views, then `quit()`), and the tray/close-to-tray branch is untouched.

**Policy for a second quit request during the barrier.** If a Quit arrives while the
cache wait is in flight (for example the session-manager `Die` callback), the window now
closes, `app.exec()` returns, and `main()` tears the application down without waiting for
`clearHttpCacheCompleted`. This is deliberate: the barrier is best-effort, and refusing
the close is precisely the behaviour that hangs the application. The timeout and
completion paths remain the normal route when no second request arrives.

### D5 — The regression is pinned outside the QtTest inventory

`tests/tests.pro` is a closed inventory of 19 suites, and `tests/article-test.pri`
guarantees that the suites never link `main.cc` or `MainWindow`. This defect lives in the
interaction between GoldenDict's close handlers and Qt's `quit()`, so a unit test of an
extracted helper would neither have caught it nor catch a regression.

The guard is a headless end-to-end check driving the real binary:

```
tests/shutdown/run-quit-check.sh <goldendict-binary> <variant>
```

with `<variant>` one of `barrier`, `no-barrier`, `close-to-tray`. The script:

1. Creates a throwaway root with a **short** `TMPDIR` (the single-instance socket name is
   `qtsingleapp-…-<uid>` inside `QDir::tempPath()`, `qtlocalpeer.cpp:85-108`) plus private
   `HOME`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME`. It writes a config
   derived from the repository defaults with `startToTray=0` and, per variant,
   `closeToTray` / `clearNetworkCacheOnExit`.
2. Launches the binary under `xvfb-run -a` with `QT_QPA_PLATFORM=xcb`, capturing stderr,
   and waits a bounded time (30 s) for the `startup-ready platform=… backend=…` record
   (`mainwindow.cc:1946`). Any exit before that record fails the run.
3. Identifies the main window by its own PID (`xdotool search --pid`, not by title) and
   delivers a graceful close request with `xdotool windowquit`, which sends
   `WM_DELETE_WINDOW` and needs no window manager. (`xdotool windowclose` destroys the
   window and is forbidden here; `wmctrl` is not used because no WM runs under
   `xvfb-run`.)
4. Asserts per variant:
   - `barrier`, `no-barrier`: the child exits within 10 s with status **exactly 0**.
   - `close-to-tray`: the window becomes unmapped within 5 s while the process stays
     alive; then a graceful quit is requested (re-show through the single-instance
     `bringToFront` path, then `xdotool key ctrl+q`) and the child must exit with status
     exactly 0.
5. Never kills the process as part of a passing run, removes its temporary root, and
   prints the captured log on failure.

The script is documented in its own header, is not part of `make check`, and takes the
binary path as an argument so it works for a build tree, a package build, or an installed
binary.

## Error handling matrix

| Situation | Behaviour |
| --- | --- |
| Tray Quit / Ctrl+Q, barrier on or off | Window closes when Qt's quit runs; process exits |
| Window close, `closeToTray=0`, barrier on | Config saved, window disabled, then hidden by Qt's quit; process exits |
| Window close, `closeToTray=1` (normal operation) | Unchanged: `ev->ignore()`, window hides, app keeps running |
| Second close while shutting down | Accepted: window hides; shutdown continues |
| Quit during dictionary (re)index (modal `Initializing`) | Dialog accepts the close, then the main window accepts, then the app exits |
| Quit request during the cache wait (e.g. session `Die`) | Window closes, `exec()` returns, teardown proceeds without waiting for the clear |
| Barrier times out after 5 s | Unchanged: `finishExit(true)` logs and completes shutdown |
| Cache completion never arrives, timer alive | Unchanged: the timeout path drives the same `quit()` |
| Fail-startup dialog | Unchanged: `QCoreApplication::exit(1)`; the close rule is not involved |
| A window that refuses to close and never consults the flag | Not possible for GoldenDict's own window classes; any future top-level window must consult `ShutdownState::requested()` |

## Compatibility

- Qt 6.12 `quit()` semantics are as quoted; earlier 6.x releases also refuse to quit while
  a visible window vetoes the close, so the change is a compatibility improvement rather
  than a version-specific workaround.
- No platform-specific code: the change is in widget event handlers and applies to xcb,
  Wayland, offscreen and Windows/macOS builds alike.
- No configuration, file-format, or interface change; no persisted state changes.
- `goldendict.pro` gains `shutdownstate.hh`/`shutdownstate.cc` in `HEADERS`/`SOURCES`;
  the test closure build keeps its existing inputs because `MainWindow` and
  `Initializing` are not part of any suite.
- Existing suites are unaffected: no `MainWindow` or `Initializing` symbol is linked by
  any of the 19 suites.

## Test strategy

1. **End-to-end regression check**: `run-quit-check.sh <binary> barrier` must fail against
   the pre-fix binary (process outlives the 10 s deadline) and pass against the fixed
   binary. The pre-fix failure is recorded as evidence once, not repeated in CI.
2. **Direct-quit path**: `run-quit-check.sh <binary> no-barrier` covers the tray/Ctrl+Q
   class of initiation that the original report did not name; it must fail pre-fix and
   pass post-fix.
3. **Tray path intact**: `run-quit-check.sh <binary> close-to-tray` must pass both before
   and after the change, proving the `closeToTray` branch is untouched.
4. **Modal indexing dialog**: quit during a forced dictionary reindex under Xvfb (Edit →
   Dictionaries → rescan via the CLI equivalent if available, otherwise a manual step
   recorded in the verification log) must exit. If the modal path cannot be driven
   headlessly, it is verified manually and recorded as such.
5. **Existing gate**: `make -C tests check` (19 suites) stays green.
6. **Manual confirmation**: quit from the tray in a real Wayland session; the process
   disappears and no disabled window is left behind.

## Verification plan

1. Build the tree without EPWing support when `libeb` is unavailable
   (`CONFIG+=no_epwing_support`).
2. Run all three variants of `tests/shutdown/run-quit-check.sh` against the fixed binary;
   record exit codes and the pre-fix results from the same script.
3. Run the closed inventory: `qmake6 tests/tests.pro -o tests/Makefile`,
   `make -C tests -j$(nproc)`, `QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`.
4. Run the manual Wayland quit once and record the process state before and after.
5. Archive the outputs, exit codes, and the pre-fix failure in the run report.

## Rollout

Two close handlers, one tiny new module, one new check under `tests/shutdown/`; no
migration and no feature flag. Shipping it restores the documented behaviour of the Quit
action, the tray menu, the window close button and session logout.
