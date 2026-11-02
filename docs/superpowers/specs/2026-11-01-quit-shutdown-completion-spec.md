# Quit shutdown completion: technical specification

The authoritative design is
`docs/superpowers/specs/2026-11-01-quit-shutdown-completion-design.md`. The
production baseline is commit `a33fdea83ad23e5b16d376493a588d4624e93e8a`.
Implement the change and check below; this document does not authorize changing
the design's exclusions. Source citations refer to that baseline.

## 1. Production change

The complete current body is (`mainwindow.cc:1239`):

```cpp
void MainWindow::closeEvent( QCloseEvent * ev )
{
  if( shuttingDown ) { ev->ignore(); return; }
  if ( cfg.preferences.enableTrayIcon && cfg.preferences.closeToTray )
  {
    if( !cfg.preferences.searchInDock )
      translateBox->setPopupEnabled( false );

    ev->ignore();
    hide();
  }
  else
  {
    ev->ignore();
    quitApp();
  }
}
```

Replace it with exactly this body, including the comments:

```cpp
void MainWindow::closeEvent( QCloseEvent * ev )
{
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
  if ( cfg.preferences.enableTrayIcon && cfg.preferences.closeToTray )
  {
    if( !cfg.preferences.searchInDock )
      translateBox->setPopupEnabled( false );

    ev->ignore();
    hide();
  }
  else
  {
    ev->ignore();
    quitApp();
  }
}
```

The following table describes a close event actually delivered to the main
window, with the flags sampled on entry. `C` is `closeToTray`, `E` is
`enableTrayIcon`, `S` is `shuttingDown`, and `F` is `startupFailed`. A modal
window can prevent delivery; the table does not assert that every combination
is normally reachable. In particular, `failStartup()` sets `startupFailed`
before its dialog's finished handler sets `shuttingDown` (`mainwindow.cc:1302`).

| S | F | C | E | Window and process effect |
|---|---|---|---|---|
| 0 | 0 | 0 | 0 | Q |
| 0 | 0 | 0 | 1 | Q |
| 0 | 0 | 1 | 0 | Q |
| 0 | 0 | 1 | 1 | H |
| 0 | 1 | 0 | 0 | Q* |
| 0 | 1 | 0 | 1 | Q* |
| 0 | 1 | 1 | 0 | Q* |
| 0 | 1 | 1 | 1 | H* |
| 1 | 0 | 0 | 0 | A |
| 1 | 0 | 0 | 1 | A |
| 1 | 0 | 1 | 0 | A |
| 1 | 0 | 1 | 1 | A |
| 1 | 1 | 0 | 0 | V |
| 1 | 1 | 0 | 1 | V |
| 1 | 1 | 1 | 0 | V |
| 1 | 1 | 1 | 1 | V |

- **Q:** Ignore this initial event and call `quitApp()`. It shows and disables
  the window, saves state and starts the existing shutdown. For an ordinary,
  fully initialized shutdown without a competing veto or excluded reentrant
  path, the process exits 0; the window disappears at or before process exit.
  An immediate quit inside this close dispatch need not deliver a second close
  to that window (`mainwindow.cc:1239`, `mainwindow.cc:1257`,
  `mainwindow.cc:1293`).
- **H:** Ignore the event and hide the window. This event does not start
  shutdown; the process remains running. `main()` disables quit-on-last-window
  (`mainwindow.cc:1239`, `main.cc:496`).
- **Q\* / H\*:** The handler takes the same Q/H branch because it checks F
  only when S is true. A pending failure dialog or competing quit can change
  the eventual process result; do not promise exit 0 or continued lifetime
  beyond this event (`mainwindow.cc:1302`).
- **A:** Accept the close; Qt hides the window and the object remains alive.
  Acceptance alone is not a request to exit: for a targeted close, the already
  pending shutdown continues; when delivered by Qt's application-level quit,
  it no longer vetoes that quit. Subject to other windows and the exclusions,
  normal completion exits 0. `finishShutdown()` destroys article views and
  requests quit; `main()` returns the event-loop result after cleanup
  (`mainwindow.cc:1281`, `mainwindow.cc:1293`, `main.cc:512`).
- **V:** Ignore the event and leave the window's visibility unchanged. This
  event alone does not end the process and, when the visible main window is
  consulted during Qt's quit, it vetoes that quit. The failure dialog's
  finished handler calls `exit(1)`; the pre-existing hidden-window exception
  is in section 6 (`mainwindow.cc:1302`, `mainwindow.cc:869`).

Only `MainWindow::closeEvent()` in `mainwindow.cc` changes in production. Do
not change any other function, production file, header or build rule. In
particular, do not add a shutdown flag module or change `Initializing`,
`loadDictionaries`, `editDictionaries`, the cache barrier, WebEngine teardown
or other windows. The only implementation addition outside production is the
new check script specified below; do not register it in `make check` or add a
QtTest suite. The existing aggregate is a closed 19-suite inventory
(`tests/tests.pro:119`, `tests/tests.pro:138`).

## 2. Check harness contract

Implement executable `tests/shutdown/run-quit-check.sh` as a Bash script. Its
header must state the usage, three variants, dependencies, exit codes and that
it is a standalone check, not part of `make check`.

### Invocation and isolation

```text
tests/shutdown/run-quit-check.sh <goldendict-binary> <barrier|no-barrier|close-to-tray>
```

Require exactly two arguments and an exact variant spelling. Exit **0** only
for a passing variant, **1** for a variant assertion failure after a valid
startup, and **2** for usage, setup, environment, driver, interruption or
cleanup errors. A startup failure is invalid setup, not evidence of the quit
regression. Section 4 fixes the diagnostic codes and classification.

Resolve the binary argument with `readlink -f --`; require a regular executable
file. Test `$(dirname "$resolved")/portable` with `-d`, following symlinks. If
it is a directory, exit 2 and print exactly `portable build not supported by
this check` in the diagnostic. Launch the resolved path for both instances.
The application's portable test is `QFileInfo(.../portable).isDir()` and its
configuration path switches to that directory (`config.cc:49-52` for
`portableHomeDirPath()`, `config.cc:54-57` for the switch in `getHomeDir()`, and
`config.cc:2464-2470` for `isPortableVersion()`).

Require `bash`, `python3`, `env`, `mktemp`, `mkdir`, `dirname`, `readlink`,
`chmod`, `rm`, `xvfb-run`, `Xvfb`, `xauth`, `dbus-run-session`, `dbus-daemon`,
`openbox`, `wmctrl`, `xdotool`, `xprop` and `xwininfo` on `PATH`
(`xwininfo` ships in `xorg-xwininfo`). A missing dependency is
exit 2; name it. Use a Python 3 inner driver with `subprocess.Popen`, `poll()` and
`wait()` so the app's actual child PID and exit status are observed without
confusing them with either wrapper or a PID-existence check on a zombie.

Set `umask 077` and create the root with `mktemp -d /tmp/gdq.XXXXXXXX`, not
under the caller's `TMPDIR`. Create these directories, all mode 0700:

```text
$root/
  t/                    # TMPDIR, including Qt's single-instance socket
  home/.goldendict/     # contains config, below
  config/               # XDG_CONFIG_HOME
  cache/                # XDG_CACHE_HOME
  data/                 # XDG_DATA_HOME
  runtime/              # XDG_RUNTIME_DIR
```

The deliberately short private `TMPDIR` is required for the local socket and
lock name; they use `QDir::tempPath()` (`qtsingleapplication/src/qtlocalpeer.cpp:107-109`). Creating the legacy
`$HOME/.goldendict` directory makes this exact config path work both with and
without the X11/XDG compile-time branch (`config.cc:61`, `config.cc:71`,
`config.cc:2344`).

Write the following complete UTF-8 file to `$root/home/.goldendict/config`.
For `barrier` use it exactly; for `no-barrier` change only
`clearNetworkCacheOnExit` to `0`; for `close-to-tray` change only
`closeToTray` to `1`.

```xml
<config>
  <!-- quit-check-seed -->
  <paths/>
  <programs/>
  <mediawikis/>
  <websites/>
  <dictservers/>
  <preferences>
    <enableTrayIcon>1</enableTrayIcon>
    <startToTray>0</startToTray>
    <closeToTray>0</closeToTray>
    <clearNetworkCacheOnExit>1</clearNetworkCacheOnExit>
    <maxNetworkCacheSize>50</maxNetworkCacheSize>
    <checkForNewReleases>0</checkForNewReleases>
  </preferences>
</config>
```

The empty source lists avoid scanning installed dictionaries and activating
default external sources; disable the release timer for this check. The
loader reads `<config>`, source lists and these preferences
(`config.cc:669`, `config.cc:673`, `config.cc:790`, `config.cc:817`,
`config.cc:845`, `config.cc:873`, `config.cc:930`, `config.cc:942`,
`config.cc:1028`, `config.cc:1040`); the release timer is controlled by that
preference (`mainwindow.cc:3297`). Explicit tray values matter because missing
tray elements in an existing preferences block load as false (`config.cc:942`).

Start the wrapper using a clean environment: `env -i` with only the caller's
`PATH`, `HOME=$root/home`, `XDG_CONFIG_HOME=$root/config`,
`XDG_CACHE_HOME=$root/cache`, `XDG_DATA_HOME=$root/data`,
`XDG_RUNTIME_DIR=$root/runtime`, `TMPDIR=$root/t`, `LC_ALL=C`,
`TZ=UTC`, and `QT_QPA_PLATFORM=xcb`. In particular, do not inherit
`SESSION_MANAGER`, `DISPLAY`, `XAUTHORITY`, `WAYLAND_DISPLAY`, any
`DBUS_SESSION_BUS_*`, `QT_*` or `QTWEBENGINE_*` variable other than the
specified platform, proxy variables, `LD_PRELOAD`, or `LD_LIBRARY_PATH`.
`xvfb-run` supplies its own display and X authority; `dbus-run-session`
supplies the private bus address. Fail setup if either is absent inside the
driver. Do not disable Chromium's sandbox or silently copy host credentials.

There is exactly one enclosing invocation and one inner driver:

```text
run-quit-check.sh supervisor
  xvfb-run -a
    dbus-run-session --
      python3 - <resolved-binary> <variant> <root>   # inner driver
        openbox --sm-disable
        original GoldenDict; optional second GoldenDict
        wmctrl, xdotool, xprop
```

The Python program may be supplied on standard input by the script. The same
driver starts and owns the window manager, both application children and all
driving commands; no app or tool runs outside both wrappers. This ensures one
private display and one private D-Bus session, and `SESSION_MANAGER` cannot
attach the test to the caller's XSMP session. Run the app with `cwd=$root`, no
application arguments, and merged stdout/stderr captured separately for each
instance. The tray host is intentionally absent; the application still tests
the configured hide branch (`mainwindow.cc:1239`).

### Readiness, windows and actions

Use monotonic-clock deadlines and poll at intervals of at most 100 ms. Bound
each external tool invocation by the smaller of two seconds and the remaining
stage deadline.

Exit statuses are interpreted **per command**; no command's status is
blanket-treated as a driver error:

| Command | Status 0 | Status 1 | Any other status |
|---|---|---|---|
| `xdotool search …` | candidate IDs on stdout | **ordinary empty result only when stderr is empty**: no window matched yet while polling discovery, or the window is gone while verifying a hide. A status 1 **with non-empty stderr** is an X protocol error → exit 2 `tool` | driver error |
| `wmctrl -lp`, `wmctrl -m` | success | driver error (the WM is missing or not ready) | driver error |
| `wmctrl -i -c` | success: the request was sent | exit 2 `tool` (the command could not send the request; this is **never** an application refusal) | driver error |
| `xdotool windowactivate`, `xdotool windowfocus` | success | retry until the 5 s focus deadline, then exit 2 `focus` | driver error |
| `xdotool getactivewindow`, `xdotool getwindowfocus` | ID on stdout | no active/focused window: retry until the 5 s focus deadline, then exit 2 `focus` | driver error |
| `xdotool key --clearmodifiers ctrl+q` | success | exit 2 `tool` (the key could not be delivered; this is never a variant failure) | driver error |
| `xprop -id <id> _NET_WM_PID` | success with a value, **or** success with ordinary `not found` output (a lost window during hide verification) | during hide verification exit 1 `hide-window-lost` **only when stderr names `BadWindow`**, otherwise exit 2 `tool` | driver error |
| `xwininfo -id <id>` | success; parse the `Map State:` value (`IsUnMapped`, `IsUnviewable`, `IsViewable`) | during hide verification exit 1 `hide-window-lost` **only when stderr names `BadWindow`, `Bad Drawable`, or `No such window with id`, and a following `xprop -id <id> _NET_WM_PID` confirms the window is gone**; otherwise exit 2 `tool` | driver error |
| `readlink`, `mktemp`, `mkdir`, `dirname` | success | driver error | driver error |

The **inner driver itself** is not governed by this table: its `python3`
exit status is its documented protocol (`0` pass, `1` variant failure, `2`
driver error), validated against the result file it writes. An unparsable or
missing result makes the wrapper report `wrapper`, never `tool`.

A premature exit of the original application is reported before any tool
status is interpreted. An X connection failure, malformed output, or a timeout
is always a driver error.

1. Start `openbox --sm-disable` as a direct child with captured output. Within
   **5 s**, require it still running and `wmctrl -m` to exit 0 and report
   `Name: Openbox`. Missing `openbox` or a failed readiness check is exit 2
   with the diagnostic in section 4. Check that the WM remains alive before
   each action and while waiting for the final result.
2. Start the resolved binary directly, record its `Popen.pid`, and start a
   **30 s** startup deadline. Require a complete stderr/stdout line that
   satisfies `re.fullmatch(r"startup-ready platform=xcb backend=\S+", line)`
   after removing only the trailing line terminator — a Python regular
   expression, since the driver is Python; POSIX character classes such as
   `[^[:space:]]` are **not** valid there. The backend group must be nonempty
   and contain no whitespace. The source
   emits that record after the startup view is ready (`mainwindow.cc:1936-1946`).
   Reject every exit before the record, including exit 0; also reject any exit
   after the record but before the initiating action. Never substitute a sleep
   or a merely visible window for the record.
3. Before that same 30 s deadline, discover the original's main window. Run
   `xdotool search --all --onlyvisible --pid <original-pid>` and intersect its
   numeric window IDs with `wmctrl -lp` rows whose PID column equals the
   original PID. Require exactly one visible managed client; never select by
   title, current focus, or a wrapper PID. Record its numeric ID. Reject zero
   or multiple candidates at the deadline as an invalid environment. Before
   the first action, require the app alive and the config byte-for-byte equal
   to the seed. Log the PID, ID, and readiness record.
4. For a graceful close use exactly `wmctrl -i -c <window-id>`; do not use
   `xdotool windowclose`, `windowkill`, a signal or a synthetic session
   request. For Ctrl+Q, allow **5 s** to run `xdotool windowactivate --sync
   <window-id>` and `xdotool windowfocus --sync <window-id>`, then require
   both `xdotool getactivewindow` and `xdotool getwindowfocus` to identify
   that same top-level ID. Retry the verification within the deadline. Only
   then send `xdotool key --clearmodifiers ctrl+q` without `--window`, so the
   key goes to the verified focus. The existing Quit action has that shortcut
   and is connected to `quitApp()` (`mainwindow.ui:428`, `mainwindow.cc:666`).

### Variants and assertions

| Variant | `closeToTray` | `clearNetworkCacheOnExit` | Initiation and required assertions |
|---|---:|---:|---|
| `barrier` | 0 | 1 | Graceful close; observe shutdown entry and the original exits 0 within 10 s. |
| `no-barrier` | 0 | 0 | Establish and verify focus, send Ctrl+Q; observe shutdown entry and the original exits 0 within 10 s. |
| `close-to-tray` | 1 | 1 | Graceful close; prove hide and continued lifetime, restore through a second instance, then focus and Ctrl+Q; observe shutdown entry and the original exits 0 within 10 s. |

All rows use `enableTrayIcon=1`, `startToTray=0`, and
`maxNetworkCacheSize=50`. For each final quit, start the **10 s** deadline
immediately before launching the graceful-close command or sending Ctrl+Q.
Observe the original child until it exits or the deadline expires. Require
its actual `wait()` status to be exactly 0, not a signal or another status.
The existing code selects `clearForExit()` based on the seeded flag and the
already constructed profile (`mainwindow.cc:782`, `mainwindow.cc:1277`);
the barrier's existing timeout is 5 s (`articleprofile.cc:223`).

Also prove the initiating action reached shutdown, rather than merely timing
out after an undelivered key or close. After the action and within the same
10 s, parse the saved config as XML and require: the `quit-check-seed` comment
is gone, `mainWindowState` and `mainWindowGeometry` elements exist, and the
four tray/cache preferences and cache size still equal the selected seed.
The initial bytes were checked immediately before the action. Record
`STAGE variant=<variant> name=shutdown-observed` when this first becomes true.
`quitApp()` calls `commitData()`, which saves the config, and the serializer
creates a fresh document with those window-state elements
(`mainwindow.cc:1257`, `mainwindow.cc:1099`, `mainwindow.cc:1116`,
`config.cc:1391`, `config.cc:1398`, `config.cc:2159`, `config.cc:2341`).
If the child exits 0 before this observation, inspect the file once more
after reaping it; absence is a failure, not a pass. If the deadline expires
without this observation, report `quit-not-observed`, not `quit-timeout`.

For `close-to-tray`, make the first stage independently observable:

1. After the close command, allow **5 s** for the recorded window ID to reach
   map state `IsUnMapped` — note the exact spelling produced by `xwininfo`
   (capital `M`), which differs from the Xlib macro `IsUnmapped`. The
   authoritative oracle is
   `xwininfo -id <window-id>`, whose output must contain a `Map State:` line
   reading exactly `IsUnMapped`; `xdotool search --onlyvisible` tests
   `IsViewable` and cannot distinguish an unmapped client from one merely
   hidden under an unmapped ancestor, so it is used only as a corroborating
   assertion (the ID must be absent from
   `xdotool search --all --onlyvisible --pid <original-pid>` while still
   present in `xdotool search --all --pid <original-pid>`). Also require
   `xprop -id <window-id> _NET_WM_PID` to resolve to the original PID, treating
   xprop's ordinary `not found` output (which exits 0) as a lost window: a
   destroyed window is not a successful hide. Then observe the same
   `IsUnMapped` state and the original still alive continuously for **1 s**;
   require the config still byte-for-byte equal to the seed. Emit
   `STAGE variant=close-to-tray name=hide-pass` only after all these checks.
2. Launch the same resolved executable, without arguments, as another direct
   child in the same driver environment. Within **5 s** require that child to
   exit exactly 0, the original to remain alive, and a unique visible managed
   window belonging to the original PID to appear using the discovery rule
   above. Use that newly discovered ID for focus and Ctrl+Q. Never wait for the
   second child instead of the original. The second instance sends
   `bringToFront` and returns 0 (`main.cc:319`, `main.cc:326`, `main.cc:355`);
   the original handles that message by showing its window
   (`mainwindow.cc:3916`, `mainwindow.cc:3058`). The second child's status
   alone does not prove message delivery; the original's reappearance does.

For every passing variant emit `PASS variant=<variant> original_pid=<pid>
window=<id> status=0`; include stage records and elapsed seconds in the normal
output. Never send a signal to the GoldenDict process or its process group on
a pass. See section 4 for capture and cleanup on all outcomes.

## 3. Test-to-requirement mapping

| Requirement | Observable assertion | Pre-fix expectation | Fixed expectation |
|---|---|---|---|
| Deferred close must not veto Qt's quit | `barrier`: ready, graceful close, saved config, original status 0 by 10 s | `quit-timeout`, exit 1 after shutdown observed | pass, exit 0 |
| Non-close-initiated quit must work without a barrier | `no-barrier`: verified focus, Ctrl+Q, saved config, original status 0 by 10 s | `quit-timeout`, exit 1 after shutdown observed | pass, exit 0 |
| Normal close-to-tray still hides without quitting | `close-to-tray`: same owned X window unmapped, PID alive for 1 s, seed unchanged | `hide-pass` | `hide-pass` |
| A later quit works with close-to-tray enabled | `close-to-tray`: second child exits 0, original reappears, focused Ctrl+Q, saved config, original exits 0 | `quit-timeout`, exit 1 after `hide-pass` and shutdown observed | pass, exit 0 |
| No crash, forced kill, wrong instance or startup false positive | direct child status 0, ready record, unique PID-owned window and no app signal on pass | an environment error or different failure is **not** the expected pre-fix result | all checks pass |
| Visible startup-failure veto and exclusions | inspect the exact F/S branch and failure handler; see section 6 | not covered by these variants | not covered by these variants |

These pre-fix outcomes are the design's expected observations, not a claim
that this specification has run them. A close with the barrier disabled is
not the `no-barrier` initiation: it already succeeds before the fix according
to the approved design. Tray-menu Quit is a manual check, not a substitute for
any automated row.

## 4. Failure diagnostics and cleanup

Every failure begins with one line `FAIL variant=<variant> code=<code> ...`
for exit 1 or `ERROR variant=<variant-or-unset> code=<code> ...` for exit 2.
Include the stage, expected and observed values, original PID and window ID
when known, elapsed time, and the failing command and status when applicable.
Use these codes and print the indicated details:

| Exit | Code | Required detail |
|---:|---|---|
| 2 | `usage` | received argument count/variant and exact usage line |
| 2 | `binary` | original argument and resolution, regular-file or execute-permission failure |
| 2 | `portable` | resolved path and `portable build not supported by this check` |
| 2 | `dependency` | each missing command |
| 2 | `temp` | failed root/directory/fixture operation and OS error |
| 2 | `wrapper` | wrapper command/status, or missing/mismatched driver result; mention Xvfb/private D-Bus |
| 2 | `wm` | `openbox --sm-disable`, whether missing/exited or `wmctrl -m` was not ready within 5 s, plus WM output |
| 2 | `startup-exit` | original actual status and whether the ready line was observed; no regression claim |
| 2 | `startup-timeout` | 30 s, original PID/status, last captured output and missing ready line |
| 2 | `discovery` | 30 s deadline, PID and both candidate lists; report zero or ambiguous windows |
| 2 | `fixture` | expected seed or preferences and observed config/parse error |
| 2 | `focus` | 5 s, requested ID and observed active/focus IDs |
| 2 | `tool` | command, exit status or timeout, stderr, and stage. Only for a genuine tool error under the per-command status table in section 2 — never for an ordinary empty `xdotool search` result |
| 2 | `interrupted` | received signal and stage |
| 2 | `cleanup` | owned PID/group or root and failed cleanup operation; override an otherwise passing result |
| 1 | `premature-exit` | original status and stage after valid startup, before the specified quit. **Takes precedence** over `hide-not-stable` when the original exits during the 1 s hide interval |
| 1 | `hide-timeout` | ID, 5 s and last visible/mapping state |
| 1 | `hide-window-lost` | ID and missing/mismatched X PID, or a destroyed window evidenced by `BadWindow`, `Bad Drawable` or `No such window with id` in xwininfo stderr and confirmed by xprop |
| 1 | `hide-not-stable` | observed remap or config change during the 1 s hide interval (an original exit there is reported as `premature-exit`) |
| 1 | `relaunch-status` | second PID and actual nonzero/signal status |
| 1 | `relaunch-timeout` | 5 s, second status, original PID and visibility/candidate lists |
| 1 | `quit-not-observed` | 10 s, original status and missing saved-config evidence |
| 1 | `quit-status` | original actual exit status or signal and shutdown-observation state |
| 1 | `quit-timeout` | 10 s, original still alive, PID/window and confirmation that shutdown was observed |

Have the driver write a **token-bound** result file inside the root before
returning. The supervisor generates a random token per invocation, passes it to
the driver in the environment, and the driver writes
`{"token": "<token>", "verdict": "pass"|"fail"|"error", ...}`. The supervisor
reads that file and treats its `verdict` as authoritative: a matching token
proves the driver reached its own conclusion, and the wrapper's process status
is used only as a cross-check for `pass`. A missing file, an unparsable file, a
token mismatch, or `verdict: pass` with a non-zero wrapper status is `wrapper`,
exit 2. This is required because a wrapper's own cleanup can fail with status 1
after it re-enables `set -e` (`/usr/bin/xvfb-run:182`, with `kill` and
`xauth remove` at 185-188) and would otherwise be indistinguishable from a
driver-reported variant failure. On failure print the complete captured supervisor/
driver, original-app, second-app if present, and openbox logs with labeled
delimiters; explicitly print `<not created>` for a missing log. This includes
early wrapper and startup errors, not just timeouts. Do not rely on files left
in the temporary directory for the diagnostic.

The inner driver owns each app and the WM as direct children, each started in
its own new process session. On an assertion/environment failure or caught
HUP/INT/TERM, terminate only still-running app process groups created by that
driver, wait at most 2 s, then SIGKILL any still-running owned group and reap
the direct child. Never use `kill 0`, a name-based kill, or a caller-owned
process group. On success, reap the original and any second child naturally;
do not signal either app group. In every case terminate/reap the test openbox,
allow the wrappers to stop their own private D-Bus and Xvfb, print failure logs
before deletion, and remove only the root created by this invocation. The
supervisor handles EXIT/HUP/INT/TERM and removes the root after the inner
driver/wrappers have completed; it reports cleanup failure instead of success.

## 5. Verification procedure

These commands are for implementation and review, not commands already run by
this specification. Run them from the integration checkout with Bash. The check
also needs `xwininfo`, which ships in `xorg-xwininfo`; install it first if
`command -v xwininfo` is empty, otherwise every variant fails the dependency
gate with exit 2. First
implement the script, but do **not** change `mainwindow.cc` until the three
pre-fix checks have been captured. The documented build switch omits EPWING
when libeb is unavailable (`README.md:138`, `goldendict.pro:614`). Preserve
the pre-fix executable alongside the fixed executable so both retain the same
application-data directory (`config.cc:2416`).

```bash
set -euo pipefail
cd /data/home/henry-arch/.local/state/pi/project-manager/worktrees/45381ca55ac94484daf6a2a2a3d772de88e84356333f8725e76d3cd32e199af5/pm-run-20261101-194036-dac1a62c/integration
RESULT=$(mktemp -d /tmp/gdquit-results.XXXXXXXX)
printf 'Results: %s\n' "$RESULT"
git diff --exit-code a33fdea83ad23e5b16d376493a588d4624e93e8a -- mainwindow.cc
test ! -e ./goldendict-pre-fix
qmake6 goldendict.pro 'CONFIG+=no_epwing_support'
make -j20
cp -p ./goldendict ./goldendict-pre-fix
sha256sum ./goldendict-pre-fix | tee "$RESULT/pre.sha256"

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

Apply the section 1 replacement, then, in the **same shell** so `$RESULT` is
retained, build and run the fixed executable:

```bash
qmake6 goldendict.pro 'CONFIG+=no_epwing_support'
make -j20
sha256sum ./goldendict | tee "$RESULT/fixed.sha256"
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

qmake6 tests/tests.pro -o tests/Makefile 2>&1 | tee "$RESULT/gate-qmake.log"
make -C tests -j20 2>&1 | tee "$RESULT/gate-build.log"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check \
  2>&1 | tee "$RESULT/gate-check.log"
```

The gate must report the exact 19-suite inventory and finish successfully
(`tests/tests.pro:5`, `tests/tests.pro:124`, `tests/tests.pro:138`). A setup
error, unexpected assertion, crash, or a pre-fix failure without the named
shutdown observation and timeout is not a regression demonstration.

**Manual tray Quit on Wayland.** On a real Wayland desktop with a functioning
tray host, run the following in the same shell. This uses a private home and
single-instance socket but intentionally retains the real Wayland display,
runtime directory and session bus so the tray is actually reachable. It does
not request a logout or any session/power action.

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

Wait for `startup-ready platform=wayland backend=...` in
`$MANUAL_ROOT/app.log` and confirm `ps -p "$MANUAL_PID" -o pid=,stat=,args=`
shows the original process. Select **Quit** from that instance's tray menu.
Within 10 s verify that the original process has disappeared and use
`wait "$MANUAL_PID"` in the launching shell to record its exact status; 0 is
required. If it remains, record that fact and do not wait indefinitely. Save
the PID, before/after process state, elapsed time, exit status or timeout,
platform/backend record, and app log with `$RESULT`; retain the manual root
until the process is gone. Never target the desktop session or another
GoldenDict instance. Archive `$RESULT`, including all six variant logs,
checksums, gate logs, elapsed times and this manual observation.

## 6. Open questions and limits

1. **Startup oracle ordering:** `pageLoaded()` logs `startup-ready`, then queues a
   zero-delay callback that emits `startupReady()` (`mainwindow.cc:1946-1955`). The required log oracle does
   not itself prove that queued callback has run. The approved design specifies
   this log and provides no external signal for that later point. Is the log
   boundary sufficient for the intended meaning of "fully initialized"? The
   harness must follow section 2; adding instrumentation or another production
   change requires a separate design decision.
2. **Failure-path scope:** the startup-failure veto is inspection-only here;
   this driver cannot inject an application-level `QEvent::Quit` while the
   failure dialog is open. When `enableTrayIcon=1` and `startToTray=1`, the
   main window is not initially shown, so Qt can skip its handler; preservation
   of exit 1 is not claimed in that pre-existing case (`mainwindow.cc:869`,
   `mainwindow.cc:1302`). No automated result may be reported as proving it.
3. **Session and reentrancy:** the approved design leaves native Wayland logout
   behavior unverified. The `Initializing` dialog vetoes close
   (`initializing.cc:78`); dictionary-reload continuations and event pumping
   in FTS cleanup remain excluded (`mainwindow.cc:1375`,
   `mainwindow.cc:2155`, `fulltextsearch.cc:371`). A separate design and real
   session-host verification would be needed to broaden those guarantees.
4. **Barrier completion:** an independent application-level quit during the
   cache wait may end the event loop before the barrier completes; this fix
   does not add a watchdog or make that barrier mandatory. Its timer and
   completion remain unchanged (`articleprofile.cc:223`,
   `articleprofile.cc:327`, `articleprofile.cc:339`).
