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

1. Start GoldenDict, let it finish initialising.
2. Tray → Quit (or Ctrl+Q, or a window close when `closeToTray=0`).
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

GoldenDict supplies exactly the window that refuses the close (`mainwindow.cc:1239`):

```cpp
void MainWindow::closeEvent( QCloseEvent * ev )
{
  if( shuttingDown ) { ev->ignore(); return; }   // cancels Qt's quit
  ...
  else { ev->ignore(); quitApp(); }
}
```

and keeps that window visible on purpose while the cache barrier runs
(`mainwindow.cc:1257`):

```cpp
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

### Why only the deferred path hangs

`ArticleProfileController::clearForExit()` (`articleprofile.cc:327`) defers
`finishShutdown()` — and therefore `qApp->quit()` — to a later event-loop turn
(`clearHttpCacheCompleted`, or the 5 s `exitTimer`). That deferral is the difference:

- `quit()` called from inside the window-close event: Qt sets `is_closing` on the window
  while the close is being handled, so `closeAllWindows()` and the verification loop both
  skip it. The quit is not cancelled and the app exits.
- `quit()` called from a later event-loop turn: `is_closing` is false, the visible window
  refuses the close, and `QApplication::event()` cancels the quit. `exit(0)` is never
  reached, so nothing ends the loop, and every later `quit()` is cancelled identically.

This is why `clearNetworkCacheOnExit=0` (synchronous `finishShutdown()`) exits cleanly
while the default `clearNetworkCacheOnExit=1` always hangs. The cache clear itself is not
at fault: with the cache clear, the WebEngine clear, the disk-cache clear, the portal
shutdown, the scan-popup destruction, the FTS stop and the article-view destruction all
skipped, the deferred path still hangs.

## Goals

1. A quit through any existing entry point terminates the process; `app.exec()` returns
   normally with `main()` reaching its cleanup.
2. Default preferences are covered: the asynchronous cache barrier, the disabled window,
   and both `closeToTray` settings.
3. A close request that arrives after shutdown has started is accepted, never silently
   swallowed.
4. The regression is pinned by a check that fails on the current code and passes on the
   fixed code.
5. The existing behaviour of a normal close (hide to tray, or quit when close-to-tray is
   off) is unchanged.

## Non-goals

- No change to the cache barrier, its completion signal, or its 5 s timeout.
- No process-level watchdog or hard exit; Option C was considered and rejected.
- No change to the shutdown UI (the window stays visible and disabled while the barrier
  runs), only to whether a close request is honoured.
- No change to WebEngine teardown, portal hotkey teardown, FTS shutdown, or article-view
  destruction.
- No new QtTest suite in the closed 19-suite inventory.

## Architecture

### D1 — While shutting down, a close request succeeds

`closeEvent` treats "already shutting down" as "close allowed":

```cpp
void MainWindow::closeEvent( QCloseEvent * ev )
{
  // Qt's quit() closes every top-level window and cancels the quit while any
  // visible window refuses to close. Once shutdown has started a close request
  // must therefore succeed: the deferred cache barrier reaches quit() from a
  // later event-loop turn, where this window is no longer marked is_closing.
  if( shuttingDown ) { ev->accept(); return; }
  ...
}
```

Rationale: the window has no remaining function once `shuttingDown` is set. It exists only
to show progress while the barrier runs, and `destroyArticleViews()` does not depend on
its visibility. Refusing the close was also the wrong answer for a second close, for a
window-manager close, and for the session-manager quit path, all of which Qt implements
through the same close-all-windows step.

Because `MainWindow` is not `WA_DeleteOnClose`, accepting the close hides the window; the
object still lives until `main()` returns, and `destroyArticleViews()` still runs in
`finishShutdown()`.

### D2 — Everything else stays exactly as it is

The cache barrier keeps its completion signal and its 5 s timeout, `quitApp()` keeps
`show()`/`setEnabled(false)`, `finishShutdown()` keeps its order (scan popup, article
views, then `quit()`), and the tray/close-to-tray branch is untouched. D1 changes only the
`shuttingDown` branch of `closeEvent`.

The two shutdown entry points remain unchanged in their relationship to Qt:

- synchronous (`clearNetworkCacheOnExit=0`): `quit()` runs inside the close event, where
  the window is already `is_closing`; D1 has no effect on this path.
- deferred (`clearNetworkCacheOnExit=1`): `quit()` runs from a later turn; D1 makes the
  window closable so Qt's quit completes.

### D3 — The regression is pinned outside the QtTest inventory

`tests/tests.pro` is a closed inventory of 19 suites, and `tests/article-test.pri`
guarantees that the suites never link `main.cc` or `MainWindow`. This defect lives in the
interaction between GoldenDict's `closeEvent` and Qt's `quit()`, so a unit test of an
extracted helper would not have caught it and would not catch a regression.

The guard is a headless end-to-end check that drives the real binary:

```
tests/shutdown/run-quit-check.sh
```

It must:

1. Create a throwaway `HOME`/`XDG_CONFIG_HOME` with a config derived from the repository
   defaults, `startToTray=0`, `closeToTray=0`, `clearNetworkCacheOnExit=1` — i.e. the
   configuration that reaches the deferred path through a window close.
2. Run the built `goldendict` under `xvfb-run -a` with `QT_QPA_PLATFORM=xcb`.
3. Wait for the main window, request a close (`xdotool windowclose` or `wmctrl -c`),
   then wait up to a fixed deadline (10 s) for the process to exit.
4. Exit 0 only when the process exited on its own; exit non-zero with the captured log
   otherwise. Never kill the process as part of a passing run.

The script is documented in its own header, is not part of `make check`, and takes the
binary path as an argument so it works for a build tree, a package build, or the installed
binary.

## Error handling matrix

| Situation | Behaviour |
| --- | --- |
| Quit from tray/Ctrl+Q, close-to-tray on, barrier enabled | Window closes when Qt's quit runs; process exits |
| Window close, close-to-tray off, barrier enabled | Config saved, window disabled, then hidden by Qt's quit; process exits |
| Window close, close-to-tray on (normal operation) | Unchanged: `ev->ignore()`, window hides, app keeps running |
| Second close while shutting down | Accepted: window hides; shutdown continues |
| Barrier times out after 5 s | Unchanged: `finishExit(true)` logs and completes shutdown |
| Cache completion never arrives and timer alive | Unchanged: the timeout path drives the same `quit()` |
| A different visible top-level window refuses to close | Out of scope: Qt would still cancel the quit; no such window exists at quit time (popup/dialogs are closed in `quitApp()`) |
| Session-manager logout during shutdown | Now completes instead of being cancelled |

## Compatibility

- Qt 6.12 `quit()` semantics are as quoted; the fix is compatible with the documented
  behaviour of earlier 6.x releases, where a refused close also prevents the app from
  quitting.
- No platform-specific code: the change is in a widget event handler and applies to xcb,
  Wayland, offscreen and Windows/macOS builds alike.
- No configuration, file-format, or interface change; no persisted state changes.
- Existing suites are unaffected: `MainWindow` is not part of any test closure.

## Test strategy

1. **End-to-end regression check** (`tests/shutdown/run-quit-check.sh`): must fail against
   the pre-fix binary (a process that outlives the deadline) and pass against the fixed
   binary. The pre-fix run is recorded as evidence, not repeated in CI.
2. **Synchronous path**: the same script with `clearNetworkCacheOnExit=0` must pass both
   before and after the change, proving the change does not disturb the path that already
   worked.
3. **Close-to-tray path**: with `closeToTray=1`, closing the window must leave the process
   running (hide), proving the tray branch is intact.
4. **Existing gate**: the 19-suite `make -C tests check` run stays green.
5. **Manual confirmation**: quit from the tray in a real Wayland session and confirm the
   process disappears; confirm the window is not left disabled and visible.

## Verification plan

1. Build the fixed tree without EPWing support if `libeb` is unavailable
   (`CONFIG+=no_epwing_support`), otherwise with the default configuration.
2. Run `tests/shutdown/run-quit-check.sh <binary>`; expect exit 0 for
   `clearNetworkCacheOnExit=1` and for `=0`, and expect the close-to-tray variant to leave
   the process alive.
3. Run the closed test inventory: `qmake6 tests/tests.pro -o tests/Makefile`,
   `make -C tests -j$(nproc)`, `QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`.
4. Record the executable evidence: script output, exit codes, and the pre-fix failure.

## Rollout

Single small change in `mainwindow.cc`, one new check under `tests/shutdown/`, no
migration and no feature flag. Shipping it restores the documented behaviour of the Quit
action, the tray menu, the window close button and session logout.
