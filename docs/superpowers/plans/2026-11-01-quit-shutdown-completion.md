# Quit shutdown completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the deterministic
> subagent-driven-development controller to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix GoldenDict's main-window close veto during ordinary shutdown and add an isolated end-to-end regression check that proves the affected quit paths complete.

**Architecture:** Keep the production change to the specified `MainWindow::closeEvent()` branch in `mainwindow.cc`. Add one standalone Bash check with a nested Python driver to isolate X11 and D-Bus, prove the three specified behaviors, and capture pre-fix failures before the production edit. Run the existing 19-suite aggregate separately; do not register the new check in it.

**Tech Stack:** C++/Qt 6, Bash, Python 3, Xvfb/X11 tools, Openbox, private D-Bus session, qmake, and the existing Qt test suites.

## Global Constraints

Only `MainWindow::closeEvent()` in `mainwindow.cc` changes in production. Do not change any other function, production file, header or build rule.

The only implementation addition outside production is the new check script specified below; do not register it in `make check` or add a QtTest suite.

The existing aggregate is a closed 19-suite inventory (`tests/tests.pro:119`, `tests/tests.pro:138`).

## Task 1: Implement the isolated quit-check harness and capture pre-fix failures

**Implementer tier:** Standard

**Lane/category:** Regression harness and pre-fix evidence.

**Files:**

- Create executable: `tests/shutdown/run-quit-check.sh`
- Temporary verification outputs (not committed): `$RESULT/`, including pre-fix logs and checksum; `./goldendict-pre-fix` as the preserved baseline executable.

**Interfaces:**

- Consumes: the authoritative harness contract in `docs/superpowers/specs/2026-11-01-quit-shutdown-completion-spec.md`, sections 2–5, and the unchanged baseline source at `a33fdea83ad23e5b16d376493a588d4624e93e8a`.
- Produces: `tests/shutdown/run-quit-check.sh <goldendict-binary> <barrier|no-barrier|close-to-tray>`, with exit 0 for pass, 1 for a variant assertion failure after valid startup, and 2 for usage/setup/environment/driver/interruption/cleanup errors. The supervisor generates a random per-invocation token and passes it to the driver; before returning, the driver writes a result file inside the root with `{"token":"<token>","verdict":"pass"|"fail"|"error",...}`. The supervisor trusts only a matching-token verdict, uses wrapper status solely to cross-check `pass`, and reports exit-2 `wrapper` for missing/unparsable result, token mismatch, or pass with nonzero wrapper status. The driver status is validated against its result file. Passing runs emit the specified `STAGE` records and `PASS` record; failures emit the specified `FAIL` or `ERROR` diagnostics and captured logs.
- Pre-fix evidence: one results directory whose exact path is printed as `Results: ...`, containing all three pre-fix logs and the pre-fix SHA-256. The evidence must show the named regression observations before Task 2 changes production code.

**Implementation steps:**

- [ ] Create `tests/shutdown/`, write the script, and set its executable bit with `chmod +x tests/shutdown/run-quit-check.sh`; its header documents invocation, all three variants, dependencies, exit statuses, and that this is standalone and not part of `make check`.
- [ ] Validate exactly two arguments and the exact variant spellings. Resolve the binary using `readlink -f --`; require a regular executable and reject a sibling `portable` directory (following symlinks) with exit 2 and diagnostic containing exactly `portable build not supported by this check`. Require and name every dependency listed in spec section 2, including `xwininfo`.
- [ ] Build the supervisor/driver nesting exactly as specified: one `xvfb-run -a`, one `dbus-run-session --`, and one Python 3 inner driver. The supervisor uses `env -i` with only the caller's PATH, private HOME/XDG/TMPDIR values, `LC_ALL=C`, `TZ=UTC`, and `QT_QPA_PLATFORM=xcb`; do not inherit display/session/D-Bus/Xauthority, other Qt or QtWebEngine, proxy, or loader-injection variables. `xvfb-run` supplies X display/authority and `dbus-run-session` the private bus. Do not disable Chromium's sandbox or copy host credentials. The Python driver starts and owns Openbox, the original application, any second application, and all driving tools. Start every owned child in its own process session; never signal an app on a passing run and never use `kill 0`, name-based killing, or a caller-owned process group.
- [ ] Use `umask 077`, `mktemp -d /tmp/gdq.XXXXXXXX`, and mode-0700 directories for `t`, `home/.goldendict`, `config`, `cache`, `data`, and `runtime`. Write the complete UTF-8 config fixture from spec section 2; for each variant change only its specified `closeToTray` or `clearNetworkCacheOnExit` value. Use the resolved binary path for both instances, with no app arguments and `cwd=$root`. Capture stdout/stderr separately for the supervisor/driver, Openbox, and each application instance.
- [ ] Start `openbox --sm-disable` as a direct child with captured output; within five seconds require it remains alive and `wmctrl -m` reports `Name: Openbox`. Check the WM remains alive before each action and while awaiting the final result. A missing or unready WM is an exit-2 `wm` setup failure, never a variant failure.
- [ ] Observe actual child PIDs and statuses using Python `subprocess.Popen`, `poll()`, and `wait()`. Require both `DISPLAY` and the private D-Bus address in the driver. Enforce monotonic deadlines, poll at intervals of at most 100 ms, and bound each external tool invocation by min(2 seconds, remaining stage time). Implement the per-command status table in spec section 2 exactly: ordinary empty `xdotool search` results are not tool errors; genuine stderr/status failures are classified as specified; startup exit is checked before interpreting a tool status.
- [ ] Implement startup readiness using a complete captured line that matches `re.fullmatch(r"startup-ready platform=xcb backend=\S+", line)` after removing only its line terminator. Enforce the 30-second startup deadline, reject all earlier exits including status 0, discover exactly one visible managed X window by intersecting PID-filtered `xdotool` IDs with `wmctrl -lp`, capture stdout/stderr separately per app instance, and verify the original is alive and config bytes still equal the selected seed before acting.
- [ ] Implement each variant exactly: `barrier` uses graceful `wmctrl -i -c`; `no-barrier` verifies activation and focus before sending `xdotool key --clearmodifiers ctrl+q` without `--window`; `close-to-tray` proves `xwininfo` reports the exact `Map State: IsUnMapped`, corroborates both visible/all-window searches, verifies `_NET_WM_PID` with `xprop`, and proves the same window remains unmapped and the original alive for one second before checking the unchanged seed. Then run a second instance, require its actual status 0 and the original window's return, and focus/quit the original.
- [ ] For every final quit, start the 10-second deadline immediately before the initiating command. Prove shutdown entry from the saved XML: the seed comment is gone, `mainWindowState` and `mainWindowGeometry` exist, and the tray/cache preferences plus cache size equal the chosen fixture. Reap the original and require actual status 0. Implement every status and stderr case in the per-command table in spec section 2, including the `xprop` and `xwininfo` BadWindow confirmations, focus retry semantics, ordinary `not found` output, tool timeouts, and the rule that premature original exit is classified before a tool failure. Preserve the exact failure-code distinctions and details in spec section 4, including `quit-not-observed` versus `quit-timeout`, `hide-window-lost`, premature-exit precedence, token mismatch, and wrapper-status cross-check.
- [ ] On all outcomes, terminate/reap Openbox; on failures or caught HUP/INT/TERM terminate only still-running app process groups owned by this driver, wait at most two seconds, then SIGKILL any still-running owned group and reap each direct child. Print all captured logs with labels and `<not created>` for absent logs before the supervisor removes only its own root. Supervisor cleanup failure overrides an otherwise passing result.
- [ ] Run `bash -n tests/shutdown/run-quit-check.sh`.
- [ ] Before any production edit, create the pre-fix results directory, verify the baseline source and build the unmodified binary using the commands below. Preserve it as `./goldendict-pre-fix` so the later fixed binary can be built beside it.

```bash
set -euo pipefail
RESULT=$(mktemp -d /tmp/gdquit-results.XXXXXXXX)
printf 'Results: %s\n' "$RESULT"
git diff --exit-code a33fdea83ad23e5b16d376493a588d4624e93e8a -- mainwindow.cc
test ! -e ./goldendict-pre-fix
qmake6 goldendict.pro 'CONFIG+=no_epwing_support'
make -j20
cp -p ./goldendict ./goldendict-pre-fix
sha256sum ./goldendict-pre-fix | tee "$RESULT/pre.sha256"
```

- [ ] Run all three pre-fix variants and record elapsed time and exit status. Require exit 1, the exact `shutdown-observed` stage, and `quit-timeout` for every variant; also require `hide-pass` for `close-to-tray`. Any setup/error result or missing named observation is not regression evidence and must be resolved before proceeding.

```bash
for variant in barrier no-barrier close-to-tray; do
  SECONDS=0
  rc=0
  tests/shutdown/run-quit-check.sh "$PWD/goldendict-pre-fix" "$variant" \
    >"$RESULT/pre-$variant.log" 2>&1 || rc=$?
  printf 'pre %s exit=%s elapsed=%ss\n' "$variant" "$rc" "$SECONDS" \
    | tee -a "$RESULT/summary.txt"
  test "$rc" -eq 1
  grep -F "STAGE variant=$variant name=shutdown-observed" "$RESULT/pre-$variant.log"
  grep -F "FAIL variant=$variant code=quit-timeout" "$RESULT/pre-$variant.log"
done
grep -F 'STAGE variant=close-to-tray name=hide-pass' "$RESULT/pre-close-to-tray.log"
```

**Tests:** `bash -n` plus the three pre-fix end-to-end runs above. Their expected statuses are assertion failures (exit 1), specifically `quit-timeout` after shutdown was observed; close-to-tray must also prove `hide-pass`.

**Commit step:** Only after all pre-fix evidence checks pass, commit the executable script: `git add tests/shutdown/run-quit-check.sh && git commit -m "test: add quit shutdown completion check"`. Do not add the generated baseline binary or `/tmp` results.

## Task 2: Accept main-window closes during shutdown and verify the fixed variants

**Implementer tier:** Fast

**Lane/category:** Production fix and automated regression verification.

**Files:**

- Modify only: `mainwindow.cc`, within `MainWindow::closeEvent()`.
- Temporary verification outputs (not committed): the Task 1 `$RESULT` directory, `./goldendict`, and the three fixed-run logs/checksum.

**Interfaces:**

- Consumes: the Task 1 script interface and its pre-fix evidence. Read the printed `Results: ...` path from Task 1 and reuse that exact directory so pre-fix and fixed evidence remain together.
- Produces: the specified `closeEvent()` implementation and fixed executable results for all three variants; each variant must exit 0 with its shutdown-observed stage, and close-to-tray must also include hide-pass.

**Implementation and verification steps:**

- [ ] Confirm the Task 1 pre-fix logs show the required observations before editing production.
- [ ] Replace only the current `if( shuttingDown ) { ev->ignore(); return; }` branch in `MainWindow::closeEvent()` with the following exact branch, leaving the rest of the function byte-for-byte unchanged:

```cpp
  if( shuttingDown )
  {
    // The fail-startup path calls exit(1) from its dialog handler. Qt's quit path
    // would close the window and then call exit(0), overwriting that status, so
    // keep vetoing while a startup failure is what is ending the process.
    if( startupFailed ) { ev->ignore(); return; }

    // Qt's quit() closes every top-level window and cancels the quit while any
    // visible window refuses to close. Once shutdown has started a close request
    // must therefore succeed: the deferred cache barrier reaches quit() from a
    // later event-loop turn, where this window is no longer marked is_closing.
    // Accepting hides the window (no WA_DeleteOnClose); the object lives on and
    // finishShutdown() still destroys the article views.
    ev->accept();
    return;
  }
```

- [ ] Build the fixed executable and save its checksum in the same results directory:

```bash
qmake6 goldendict.pro 'CONFIG+=no_epwing_support'
make -j20
sha256sum ./goldendict | tee "$RESULT/fixed.sha256"
```

- [ ] Run the three variants against `"$PWD/goldendict"` in the same shell and require each command to return 0, emit `shutdown-observed`, and emit `PASS`; require `hide-pass` from close-to-tray. Save each log and elapsed time as specified:

```bash
for variant in barrier no-barrier close-to-tray; do
  SECONDS=0
  rc=0
  tests/shutdown/run-quit-check.sh "$PWD/goldendict" "$variant" \
    >"$RESULT/fixed-$variant.log" 2>&1 || rc=$?
  printf 'fixed %s exit=%s elapsed=%ss\n' "$variant" "$rc" "$SECONDS" \
    | tee -a "$RESULT/summary.txt"
  test "$rc" -eq 0
  grep -F "STAGE variant=$variant name=shutdown-observed" "$RESULT/fixed-$variant.log"
  grep -F "PASS variant=$variant " "$RESULT/fixed-$variant.log"
done
grep -F 'STAGE variant=close-to-tray name=hide-pass' "$RESULT/fixed-close-to-tray.log"
```

- [ ] Review the final diff to confirm only the specified closeEvent branch changed in `mainwindow.cc`; do not adjust `startupFailed`, other close handlers, shutdown sequencing, the cache barrier, WebEngine teardown, or build/test registration.

**Tests:** The three post-fix harness variants above; each must pass with status 0. Confirm the pre-fix logs remain available and unchanged in `$RESULT`.

**Commit step:** Commit only `mainwindow.cc`: `git add mainwindow.cc && git commit -m "fix: allow main window close during shutdown"`.

## Task 3: Verify tray Quit manually on Wayland

**Implementer tier:** Fast

**Lane/category:** Manual platform acceptance check.

**Files:**

- No tracked files. Temporary manual root and captured evidence: `$MANUAL_ROOT` and `$RESULT/manual-wayland-*`.

**Interfaces:**

- Consumes: the fixed `./goldendict` binary from Task 2 and its saved `$RESULT` directory.
- Produces: a recorded observation containing PID, before/after process state, elapsed time, exact exit status or timeout, `startup-ready platform=wayland backend=...` line, and app log. This check is only for tray Quit; it is not evidence for native session logout or the documented exclusions.

**Verification steps:**

- [ ] Run this only in a real Wayland session with a functioning tray host. Require `XDG_SESSION_TYPE=wayland`, non-empty `WAYLAND_DISPLAY`, and non-empty `DBUS_SESSION_BUS_ADDRESS`. Do not request logout, reboot, shutdown, service restart, or any host/session action.
- [ ] In the same shell that will observe the child, create a private home/socket root and seed the tray-enabled close-to-tray configuration shown below; then start the fixed binary with the environment overrides specified in spec section 5:

```bash
test "${XDG_SESSION_TYPE-}" = wayland
test -n "${WAYLAND_DISPLAY-}" && test -n "${DBUS_SESSION_BUS_ADDRESS-}"
MANUAL_ROOT=$(mktemp -d /tmp/gdq-manual.XXXXXXXX)
mkdir -p "$MANUAL_ROOT"/{t,home/.goldendict,config,cache,data}
chmod 700 "$MANUAL_ROOT"/{t,home,home/.goldendict,config,cache,data}
printf '%s\n' \
  '<config><paths/><programs/><mediawikis/><websites/><dictservers/><preferences><enableTrayIcon>1</enableTrayIcon><startToTray>0</startToTray><closeToTray>1</closeToTray><clearNetworkCacheOnExit>1</clearNetworkCacheOnExit><maxNetworkCacheSize>50</maxNetworkCacheSize><checkForNewReleases>0</checkForNewReleases></preferences></config>' \
  >"$MANUAL_ROOT/home/.goldendict/config"
env -u SESSION_MANAGER -u QT_LOGGING_RULES -u QT_MESSAGE_PATTERN \
  HOME="$MANUAL_ROOT/home" XDG_CONFIG_HOME="$MANUAL_ROOT/config" \
  XDG_CACHE_HOME="$MANUAL_ROOT/cache" XDG_DATA_HOME="$MANUAL_ROOT/data" \
  TMPDIR="$MANUAL_ROOT/t" QT_QPA_PLATFORM=wayland \
  "$PWD/goldendict" >"$MANUAL_ROOT/app.log" 2>&1 &
MANUAL_PID=$!
printf 'manual pid=%s root=%s\n' "$MANUAL_PID" "$MANUAL_ROOT"
```

- [ ] Wait for the Wayland startup-ready record and confirm `ps -p "$MANUAL_PID" -o pid=,stat=,args=` identifies the original process. Select **Quit** from that instance's tray menu. Within 10 seconds verify the original process disappeared, then use `wait "$MANUAL_PID"` in the launching shell and require status 0. If still alive at 10 seconds, record the timeout and do not wait indefinitely. Save all required observations and the app log under `$RESULT`; retain `$MANUAL_ROOT` until the process is gone.

**Tests:** The single manual tray-menu Quit observation on a real Wayland session. If no suitable session/tray host is available, record `not run` and the reason; do not report a pass.

**Commit step:** No commit; this is a verification-only task with no tracked-file changes. Keep its evidence in `$RESULT` and clean the private manual root only after the application process is gone.

## Task 4: Run the closed 19-suite test gate

**Implementer tier:** Fast

**Lane/category:** Dedicated aggregate verification.

**Files:**

- Generated verification files only: `tests/Makefile` and the existing test build outputs; do not commit generated build artifacts.
- Gate logs: `$RESULT/gate-qmake.log`, `$RESULT/gate-build.log`, and `$RESULT/gate-check.log`.

**Interfaces:**

- Consumes: the committed Task 2 production fix and the unchanged closed suite inventory in `tests/tests.pro`.
- Produces: qmake/build/check logs showing the exact existing 19-suite inventory and a successful `make -C tests check` result. The standalone quit-check script remains outside this aggregate.

**Verification steps:**

- [ ] From the integration checkout, with `$RESULT` set to the existing results directory and `set -o pipefail` active, run the dedicated gate commands exactly:

```bash
qmake6 tests/tests.pro -o tests/Makefile 2>&1 | tee "$RESULT/gate-qmake.log"
make -C tests -j20 2>&1 | tee "$RESULT/gate-build.log"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check \
  2>&1 | tee "$RESULT/gate-check.log"
```

- [ ] Confirm the check log reports the exact 19-suite inventory (`tests/tests.pro:5`, `tests/tests.pro:124`, `tests/tests.pro:138`) and exits successfully. A setup error, unexpected assertion, crash, missing suite, or suite-count drift fails this gate.
- [ ] Archive `$RESULT`, including all six variant logs, checksums, gate logs, elapsed times, and the manual observation (or its explicit not-run reason), as required by spec section 5. Do not add generated outputs or evidence files to the source commit.

**Tests:** The exact qmake, test build, and 900-second `make -C tests check` commands above; success requires all 19 existing suites.

**Commit step:** No commit; this dedicated task changes no tracked files. Preserve the verification logs in the results archive and leave generated test artifacts uncommitted.
