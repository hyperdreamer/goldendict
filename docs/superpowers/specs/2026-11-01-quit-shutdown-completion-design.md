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

GoldenDict supplies exactly the visible window that refuses this close
(`mainwindow.cc:1239`), and it keeps that window visible on purpose while the cache
barrier runs (`mainwindow.cc:1257`):

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
  (`mainwindow.cc:667`), a session-manager request — reaches `quit()` with no close in
  progress, so the visible window vetoes it and `exit(0)` is never reached. Every later
  `quit()` is vetoed the same way.

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

1. A quit through any supported in-application entry point terminates the process;
   `app.exec()` returns normally with `main()` reaching its cleanup.
2. Default preferences are covered: the asynchronous cache barrier, the disabled window,
   and both `closeToTray` settings.
3. A close request that arrives after shutdown has started is accepted, never silently
   swallowed.
4. The regression is pinned by a check that fails on the current code and passes on the
   fixed code, for every initiation path that can be driven without a session manager.
5. A normal close keeps its current behaviour: hide to tray when `closeToTray=1`, quit
   when it is off.

## Non-goals

- No change to the cache barrier, its completion signal, or its 5 s timeout.
- No process-level watchdog or hard exit.
- No change to the shutdown UI (the window stays visible and disabled while the barrier
  runs), only to whether a close request is honoured.
- **No change to any other window's close handling**, including the application-modal
  `Initializing` progress dialog.
- **No change to any dictionary-reload path** (`loadDictionaries`, `makeDictionaries`,
  `on_rescanFiles_triggered`, `EditDictionaries`).
- No change to WebEngine teardown, portal hotkey teardown, or article-view destruction.
- No new QtTest suite in the closed 19-suite inventory.
- No synthetic session-manager logout in the automated check.

## Architecture

### D1 — While shutting down, the main window accepts the close

One branch of one function changes:

```cpp
void MainWindow::closeEvent( QCloseEvent * ev )
{
  // Qt's quit() closes every top-level window and cancels the quit while any
  // visible window refuses to close. Once shutdown has started a close request
  // must therefore succeed: the deferred cache barrier reaches quit() from a
  // later event-loop turn, where this window is no longer marked is_closing.
  // Accepting hides the window (no WA_DeleteOnClose); the object lives on and
  // finishShutdown() still destroys the article views.
  if( shuttingDown ) { ev->accept(); return; }
  ...
}
```

`ev->ignore()` becomes `ev->accept()` in the `shuttingDown` branch. Nothing else in the
function, and nothing else in the class, changes.

Rationale: once `shuttingDown` is set the window has no remaining function — it exists
only to show progress while the barrier runs, and `destroyArticleViews()` does not depend
on its visibility. Refusing the close was also wrong for a second close, for a
window-manager close, and for the session-manager quit path, all of which Qt implements
through the same close-all-windows step.

`shuttingDown` is already the correct predicate: it is set at the top of `quitApp()`,
before the barrier and before any `quit()` is requested, and it also guards the
fail-startup path (which calls `QCoreApplication::exit(1)` directly and never depends on
close handling).

### D2 — Nothing else is needed to make the app exit

No new flag, no new file, no build-system change. The `shuttingDown` latch is already
process-visible to the class that owns the only window that can veto the quit at shutdown
time. The scan popup, headwords dialog, full-text-search dialog and help window are
already retired earlier in `quitApp()` (`mainwindow.cc:1257-1278`), so the main window is
the only visible top-level window when `quit()` runs.

### D3 — The regression check

`tests/tests.pro` is a closed inventory of 19 suites, and `tests/article-test.pri`
guarantees that the suites never link `main.cc` or `MainWindow`. This defect lives in the
interaction between GoldenDict's `closeEvent` and Qt's `quit()`, so a unit test of an
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
instance of the same binary in the same isolated environment**; the single-instance IPC
delivers `bringToFront` (`main.cc:355`), the original window becomes visible and is
focused, and the check then sends `Ctrl+Q`. The second instance exits by itself
(`main.cc:357`); the check keeps tracking and waiting for the **original** child PID.

**Common procedure.**

1. Throwaway root with a **short** `TMPDIR` (the single-instance socket is
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
   window instead of asking it to close. A missing WM is a hard, explained failure;
   `openbox` is a dependency of this check only.
4. Launch under `xvfb-run -a` with `QT_QPA_PLATFORM=xcb`, capture stderr, wait up to 30 s
   for the `startup-ready platform=… backend=…` record (`mainwindow.cc:1946`); any exit
   before it fails the run. Record the child PID directly (never the `xvfb-run` wrapper)
   and match windows with `xdotool search --pid <child-pid>`.
5. Apply the initiation above, then assert: exit within 10 s with status **exactly 0**
   (a crash or any non-zero status fails), except for the documented two-stage
   `close-to-tray` variant, whose first stage asserts the window is unmapped while the
   process stays alive.
6. Never kill the process as part of a passing run; remove the temporary root; print the
   captured log on failure.

The script is documented in its own header, is not part of `make check`, and takes the
binary path as an argument.

### D4 — Coverage and known limitations

| Entry point | Coverage |
| --- | --- |
| Window close (tray off) | automated: `barrier` |
| Ctrl+Q / File → Quit | automated: `no-barrier`, and the final stage of `close-to-tray` |
| Hide-to-tray on close | automated: first stage of `close-to-tray` |
| Tray → Quit | manual: requires a StatusNotifier host; none runs under Xvfb |

**Known limitations, deliberately not addressed.**

1. *Session logout while a dictionary (re)index is running.* `Initializing` is an
   application-modal dialog that ignores closes (`initializing.cc:78`), and Qt closes the
   active modal widget first. On XSMP sessions (xcb) a logout reaches `quitApp()` through
   `commitDataRequest`, but the flag is set while the dialog already owns input, so the
   dialog's veto can pre-empt this fix. On native Wayland the logout arrives as window
   close events instead — Qt 6.12's Wayland session manager implements only session
   restore (`qwaylandsessionmanager.cpp`) and never emits `commitDataRequest` — so the
   same veto applies. This is pre-existing behaviour, unchanged by D1, and it is recorded
   here rather than fixed because it cannot be verified without a real session host.
2. *Dictionary-reload continuations.* `loadDictionaries()` and its callers keep running
   after a shutdown begins. Unchanged by D1.
3. *Event pumping inside FTS cleanup.* `closeFullTextSearchDialog()` →
   `stopSearch()` runs `QApplication::processEvents()` in a loop
   (`fulltextsearch.cc:371-382`), so `quitApp()` can be entered from inside cleanup.
   Pre-existing, unchanged by D1.

## Error handling matrix

| Situation | Behaviour |
| --- | --- |
| Tray Quit / Ctrl+Q, barrier on or off | Window closes when Qt's quit runs; process exits |
| Window close, `closeToTray=0`, barrier on | Config saved, window disabled, then hidden by Qt's quit; process exits |
| Window close, `closeToTray=1` (normal operation) | Unchanged: `ev->ignore()`, window hides, app keeps running |
| Second close while shutting down | Accepted: window hides; shutdown continues |
| Another `quitApp()` call during shutdown | Returns at the `shuttingDown` latch; teardown continues |
| Application-level `QEvent::Quit` during the cache wait | Window closes, `exec()` returns, teardown runs from `~MainWindow`; the barrier is best-effort |
| Barrier times out after 5 s | Unchanged: `finishExit(true)` logs and completes shutdown |
| Fail-startup dialog | Unchanged: `QCoreApplication::exit(1)`; `shuttingDown` is set, so closes are accepted |
| Quit during a dictionary rescan | **Unchanged / documented limitation:** the modal dialog can still veto the quit |

## Compatibility

- Qt 6.12 `quit()` semantics are as quoted; earlier 6.x releases also refuse to quit while
  a visible window vetoes the close, so the change is a compatibility improvement rather
  than a version-specific workaround.
- No platform-specific code: the change is one widget event handler and applies to xcb,
  Wayland, offscreen and Windows/macOS builds alike.
- No configuration, file-format, interface, or build-system change.
- Existing suites are unaffected: `MainWindow` is not part of any suite's closure.

## Test strategy

1. **Regression matrix**: `run-quit-check.sh <binary> <variant>` for the three variants.
   Pre-fix: `barrier` hangs, `no-barrier` hangs, `close-to-tray` hides then hangs.
   Post-fix: every variant completes with exit status 0. The pre-fix failure of each
   variant is recorded once as evidence.
2. **Existing gate**: `make -C tests check` (19 suites) stays green.
3. **Manual**: tray Quit on Wayland once, recording the process state before and after.

## Verification plan

1. Build the tree without EPWing support when `libeb` is unavailable
   (`CONFIG+=no_epwing_support`).
2. Run the three variants of `tests/shutdown/run-quit-check.sh` against the pre-fix and
   fixed binaries; record exit codes and elapsed times.
3. Run the closed inventory: `qmake6 tests/tests.pro -o tests/Makefile`,
   `make -C tests -j$(nproc)`, `QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`.
4. Run the manual tray quit and record the observation.
5. Archive outputs, exit codes, and the pre-fix failures in the run report.

## Rollout

Changed files: `mainwindow.cc` (one branch of `closeEvent`) and the new
`tests/shutdown/run-quit-check.sh`. No migration, no feature flag, no new module. Shipping
it restores the documented behaviour of the Quit action, the tray menu and the window
close button.
