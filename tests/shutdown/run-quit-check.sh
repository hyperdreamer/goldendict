#!/usr/bin/env bash
#
# run-quit-check.sh -- standalone quit/shutdown completion check for GoldenDict.
#
# Usage:
#   tests/shutdown/run-quit-check.sh <goldendict-binary> <barrier|no-barrier|close-to-tray>
#
# Variants (all seeds enableTrayIcon=1, startToTray=0, maxNetworkCacheSize=50):
#   barrier        closeToTray=0 clearNetworkCacheOnExit=1
#                  Graceful window close (wmctrl -i -c).  The deferred cache
#                  barrier reaches quit() from a later event-loop turn, so the
#                  main window must not veto Qt's close-all-windows step.
#   no-barrier     closeToTray=0 clearNetworkCacheOnExit=0
#                  No close event ever starts shutdown: focus is verified and
#                  Ctrl+Q is sent, then the process must still exit 0.
#   close-to-tray  closeToTray=1 clearNetworkCacheOnExit=1
#                  The first close must hide the main window and keep the
#                  process alive (xwininfo Map State: IsUnMapped, stable for
#                  1 s, seed config unchanged).  A second instance of the same
#                  binary must bring the window back; a focused Ctrl+Q must
#                  then exit 0.
#
# Dependencies, all required on PATH:
#   bash python3 env mktemp mkdir dirname readlink chmod rm
#   xvfb-run Xvfb xauth dbus-run-session dbus-daemon
#   openbox wmctrl xdotool xprop xwininfo   (xwininfo ships in xorg-xwininfo)
#
# Exit status:
#   0  the variant passed
#   1  a variant assertion failed after a valid startup (FAIL line)
#   2  usage, setup, environment, driver, interruption or cleanup error
#      (ERROR line)
#
# structure:
#   run-quit-check.sh (supervisor)
#     xvfb-run -a
#       dbus-run-session --
#         python3 -u - (inner driver)
#           openbox --sm-disable
#           original GoldenDict; optional second GoldenDict
#           wmctrl, xdotool, xprop, xwininfo
#
# The supervisor owns argument validation, the isolated root and the clean
# environment, and it trusts only the token-bound result file written by the
# driver.  The driver owns the window manager, both application instances and
# every driving tool, so everything shares one private X display and one
# private D-Bus session.
#
# This is a standalone check.  It is deliberately NOT registered in
# `make check` and it adds no QtTest suite: the existing aggregate is a closed
# 19-suite inventory.  It is documented here, takes the binary as an argument
# and is intended to be run against pre-fix and fixed binaries separately.

set -u -o pipefail
# Run background jobs in their own process group so an interrupt can stop the
# private xvfb-run/dbus-run-session/driver tree without touching any other
# process (never `kill 0`, never the caller's process group).
set -m
umask 077

USAGE='usage: tests/shutdown/run-quit-check.sh <goldendict-binary> <barrier|no-barrier|close-to-tray>'

VARIANT=unset
ROOT=''
LOG=''
TOKEN=''
WRAPPER_PID=''
INTERRUPT_SIGNAL=''

supervisor_error() { # <code> <detail>
  printf 'ERROR variant=%s code=%s %s\n' "$VARIANT" "$1" "$2" >&2
}

on_signal() { # <NAME>
  INTERRUPT_SIGNAL=$1
  if [ -n "$WRAPPER_PID" ]; then
    # Only the process group this supervisor created for the wrapper tree.
    kill -TERM -- "-$WRAPPER_PID" 2>/dev/null || true
  fi
}
trap 'on_signal INT' INT
trap 'on_signal HUP' HUP
trap 'on_signal TERM' TERM

cleanup_root_on_exit() {
  local status=$?
  if [ -n "$ROOT" ] && [ -d "$ROOT" ]; then
    if ! rm -rf -- "$ROOT"; then
      printf 'ERROR variant=%s code=cleanup could not remove root=%s\n' "$VARIANT" "$ROOT" >&2
      status=2
    fi
  fi
  exit "$status"
}
trap cleanup_root_on_exit EXIT

write_config() { # <variant>
  local close_to_tray clear_cache config_path
  case "$1" in
    barrier)        close_to_tray=0; clear_cache=1 ;;
    no-barrier)     close_to_tray=0; clear_cache=0 ;;
    close-to-tray)  close_to_tray=1; clear_cache=1 ;;
  esac
  config_path="$ROOT/home/.goldendict/config"
  printf '%s\n' \
    '<config>' \
    '  <!-- quit-check-seed -->' \
    '  <paths/>' \
    '  <programs/>' \
    '  <mediawikis/>' \
    '  <websites/>' \
    '  <dictservers/>' \
    '  <preferences>' \
    '    <enableTrayIcon>1</enableTrayIcon>' \
    '    <startToTray>0</startToTray>' \
    "    <closeToTray>$close_to_tray</closeToTray>" \
    "    <clearNetworkCacheOnExit>$clear_cache</clearNetworkCacheOnExit>" \
    '    <maxNetworkCacheSize>50</maxNetworkCacheSize>' \
    '    <checkForNewReleases>0</checkForNewReleases>' \
    '  </preferences>' \
    '</config>' >"$config_path" || return 1
  [ -s "$config_path" ]
}

print_file() { # <path> -- print one captured log without extra tools
  local line
  while IFS= read -r line || [ -n "$line" ]; do
    printf '%s\n' "$line"
  done <"$1"
  return 0
}

file_contains() { # <path> <needle>
  local line
  while IFS= read -r line || [ -n "$line" ]; do
    case $line in
      *"$2"*) return 0 ;;
    esac
  done <"$1"
  return 1
}

print_wrapper_stdout() {
  if [ -f "$LOG/wrapper.out" ]; then
    print_file "$LOG/wrapper.out"
  fi
  return 0
}

print_failure_logs() {
  printf '===== driver/supervisor stderr =====\n'
  if [ -f "$LOG/wrapper.err" ]; then
    print_file "$LOG/wrapper.err"
  else
    printf '<not created>\n'
  fi
  # The driver prints the application and openbox logs itself when it reached
  # its own conclusion.  When it did not (early wrapper or startup failure),
  # print whatever the driver left in the root, and say so when a log is absent.
  if [ -f "$LOG/wrapper.out" ] &&
     file_contains "$LOG/wrapper.out" '===== original application log'; then
    return 0
  fi
  local entry label path
  for entry in \
    'original application log:original.log' \
    'second application log:second.log' \
    'openbox stdout:openbox.out' \
    'openbox stderr:openbox.err'; do
    label=${entry%%:*}
    path=$LOG/${entry#*:}
    printf '===== %s =====\n' "$label"
    if [ -f "$path" ]; then
      print_file "$path"
    else
      printf '<not created>\n'
    fi
  done
}

read_driver_result() { # reads $ROOT/result.json, prints "<STATE> [verdict] [code]"
  python3 - "$ROOT/result.json" "$TOKEN" <<'PY_RESULT'
import json
import sys

path, token = sys.argv[1], sys.argv[2]
try:
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
except OSError as exc:
    print("MISSING", exc)
    raise SystemExit(0)
except ValueError as exc:
    print("UNPARSABLE", exc)
    raise SystemExit(0)
if not isinstance(data, dict):
    print("UNPARSABLE", "result is not a JSON object")
    raise SystemExit(0)
if data.get("token") != token:
    print("MISMATCH", repr(data.get("token")))
    raise SystemExit(0)
verdict = data.get("verdict")
if verdict not in ("pass", "fail", "error"):
    print("UNPARSABLE", "verdict=%r" % (verdict,))
    raise SystemExit(0)
print("OK", verdict, data.get("code") or "-")
PY_RESULT
}

run_driver() { # <resolved-binary> <variant>
  local resolved=$1 variant=$2
  env -i \
    PATH="$PATH" \
    HOME="$ROOT/home" \
    XDG_CONFIG_HOME="$ROOT/config" \
    XDG_CACHE_HOME="$ROOT/cache" \
    XDG_DATA_HOME="$ROOT/data" \
    XDG_RUNTIME_DIR="$ROOT/runtime" \
    TMPDIR="$ROOT/t" \
    LC_ALL=C \
    TZ=UTC \
    QT_QPA_PLATFORM=xcb \
    GDQ_CHECK_TOKEN="$TOKEN" \
    xvfb-run -a dbus-run-session -- python3 -u - "$resolved" "$variant" "$ROOT" \
    >"$LOG/wrapper.out" 2>"$LOG/wrapper.err" <<'PY_DRIVER'
import json
import os
import pty
import re
import signal
import subprocess
import sys
import threading
import time
import traceback
import tty
import xml.etree.ElementTree as ET

READY_RE = re.compile(r"startup-ready platform=xcb backend=\S+")
XWININFO_LOST_TOKENS = ("BadWindow", "Bad Drawable", "No such window with id")

POLL = 0.1
TOOL_LIMIT = 2.0
STARTUP_TIMEOUT = 30.0
HIDE_TIMEOUT = 5.0
HIDE_STABLE = 1.0
RELAUNCH_TIMEOUT = 5.0
FOCUS_TIMEOUT = 5.0
QUIT_TIMEOUT = 10.0

# Expected preferences of the seeded config, per variant (spec section 2).
SEEDS = {
    "barrier": {
        "enableTrayIcon": "1",
        "startToTray": "0",
        "closeToTray": "0",
        "clearNetworkCacheOnExit": "1",
        "maxNetworkCacheSize": "50",
    },
    "no-barrier": {
        "enableTrayIcon": "1",
        "startToTray": "0",
        "closeToTray": "0",
        "clearNetworkCacheOnExit": "0",
        "maxNetworkCacheSize": "50",
    },
    "close-to-tray": {
        "enableTrayIcon": "1",
        "startToTray": "0",
        "closeToTray": "1",
        "clearNetworkCacheOnExit": "1",
        "maxNetworkCacheSize": "50",
    },
}
CONFIG_PREFS = (
    "enableTrayIcon",
    "startToTray",
    "closeToTray",
    "clearNetworkCacheOnExit",
    "maxNetworkCacheSize",
)


class VariantFailure(Exception):
    """A variant assertion failed after a valid startup (exit 1)."""

    def __init__(self, code, detail, stage_name):
        Exception.__init__(self, detail)
        self.code = code
        self.detail = detail
        self.stage_name = stage_name


class DriverError(Exception):
    """A setup/environment/driver problem (exit 2)."""

    def __init__(self, code, detail, stage_name):
        Exception.__init__(self, detail)
        self.code = code
        self.detail = detail
        self.stage_name = stage_name


class Interrupted(Exception):
    def __init__(self, signum):
        Exception.__init__(self, "signal %d" % signum)
        self.signum = signum


class State(object):
    pass


st = State()
st.binary = None
st.variant = None
st.root = None
st.logdir = None
st.cfg = None
st.seed = None
st.token = None
st.stage = "setup"
st.pid = None
st.window = None
st.started = time.monotonic()
st.in_cleanup = False
st.wm = None
st.original = None
st.second = None
st.wm_out = None
st.wm_err = None
st.iteration = time.monotonic()


def now():
    return time.monotonic()


def elapsed():
    return now() - st.started


def signal_note(status):
    if status < 0:
        return " (signal %d)" % (-status,)
    return ""


def stage(name):
    st.stage = name


def emit(name, extra=""):
    print("STAGE variant=%s name=%s%s" % (st.variant, name, extra), flush=True)


def fail_report(code, detail, stage_name):
    print(
        "FAIL variant=%s code=%s stage=%s original_pid=%s window=%s elapsed=%.3fs %s"
        % (
            st.variant,
            code,
            stage_name,
            st.pid if st.pid is not None else "-",
            st.window if st.window is not None else "-",
            elapsed(),
            detail,
        ),
        flush=True,
    )


def error_report(code, detail, stage_name):
    print(
        "ERROR variant=%s code=%s stage=%s original_pid=%s window=%s elapsed=%.3fs %s"
        % (
            st.variant,
            code,
            stage_name,
            st.pid if st.pid is not None else "-",
            st.window if st.window is not None else "-",
            elapsed(),
            detail,
        ),
        flush=True,
    )


def hex_id(window):
    return "0x%08x" % window


def tool_budget(deadline):
    # The smaller of two seconds and the remaining stage time.  A floor of one
    # poll interval keeps the last poll of a windowed stage from turning a
    # healthy X tool into a spurious timeout; the loop guards below stop
    # polling a stage once less than that interval remains.
    if deadline is None:
        return TOOL_LIMIT
    return min(TOOL_LIMIT, max(POLL, deadline - now()))


def require_time(deadline, code, detail, stage_name):
    if deadline is not None and deadline - now() <= POLL:
        raise DriverError(code, detail, stage_name)


def sleep_poll(deadline=None):
    # Keep the observation cadence at or below one poll interval: sleep only the
    # part of the interval the tools did not already consume.
    delay = POLL - (now() - st.iteration)
    if deadline is not None:
        delay = min(delay, deadline - now())
    time.sleep(max(0.01, min(POLL, delay)))


def run_tool(argv, timeout, what):
    """Run one external tool, bounded by the stage budget.

    Statuses are interpreted by the caller according to the per-command table
    in spec section 2: this helper only reports, it never classifies.
    """
    if timeout <= 0.0:
        raise DriverError(
            "tool",
            "%s: no time left in stage %s" % (what, st.stage),
            st.stage,
        )
    try:
        proc = subprocess.run(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            start_new_session=True,
        )
    except FileNotFoundError:
        raise DriverError(
            "tool", "%s: command not found: %s" % (what, argv[0]), st.stage
        )
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or b"").decode("utf-8", "replace")
        err = (exc.stderr or b"").decode("utf-8", "replace")
        raise DriverError(
            "tool",
            "%s: timed out after %.2fs (stage %s) stdout=%r stderr=%r"
            % (what, timeout, st.stage, out, err),
            st.stage,
        )
    return (
        proc.returncode,
        proc.stdout.decode("utf-8", "replace"),
        proc.stderr.decode("utf-8", "replace"),
    )


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def parse_preferences(data, stage_name):
    try:
        root = ET.fromstring(data)
    except ET.ParseError as exc:
        raise DriverError(
            "fixture", "config %s is not parseable XML: %s" % (st.cfg, exc), stage_name
        )
    prefs = root.find("preferences")
    if prefs is None:
        raise DriverError(
            "fixture", "config %s has no preferences element" % st.cfg, stage_name
        )
    values = {}
    for name in CONFIG_PREFS:
        element = prefs.find(name)
        values[name] = None if element is None else (element.text or "").strip()
    return values


def assert_seed(stage_name):
    try:
        data = read_bytes(st.cfg)
    except OSError as exc:
        raise DriverError(
            "fixture",
            "config %s is unreadable at stage %s: %s" % (st.cfg, stage_name, exc),
            stage_name,
        )
    if data != st.seed:
        raise DriverError(
            "fixture",
            "config bytes differ from the selected seed at stage %s (%d vs %d bytes)"
            % (stage_name, len(data), len(st.seed)),
            stage_name,
        )


def shutdown_evidence():
    """Return (ok, detail) for the saved-config shutdown evidence."""
    try:
        data = read_bytes(st.cfg)
    except OSError as exc:
        return False, "config %s is unreadable: %s" % (st.cfg, exc)
    if b"quit-check-seed" in data:
        return False, "the quit-check-seed comment is still present"
    try:
        root = ET.fromstring(data)
    except ET.ParseError as exc:
        return False, "config XML parse error: %s" % (exc,)
    if root.find(".//mainWindowState") is None:
        return False, "mainWindowState element is missing"
    if root.find(".//mainWindowGeometry") is None:
        return False, "mainWindowGeometry element is missing"
    prefs = root.find("preferences")
    if prefs is None:
        return False, "preferences element is missing"
    for name, want in SEEDS[st.variant].items():
        element = prefs.find(name)
        got = None if element is None else (element.text or "").strip()
        if got != want:
            return False, "preference %s=%r differs from the seed value %r" % (
                name,
                got,
                want,
            )
    return (
        True,
        "seed comment gone, mainWindowState and mainWindowGeometry present, "
        "tray/cache preferences and cache size unchanged",
    )


class App(object):
    """One GoldenDict instance started as a direct child in its own session.

    stderr is a private pseudo-terminal: Qt only emits its qInfo records (the
    startup-ready oracle) when stderr is a console, and the check must not add
    environment variables beyond the ones the specification allows.  The
    terminal is set raw and drained continuously, so the captured log holds the
    exact merged stdout/stderr bytes.
    """

    def __init__(self, tag):
        self.tag = tag
        self.path = os.path.join(st.logdir, tag + ".log")
        self.handle = open(self.path, "wb")
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        self.proc = subprocess.Popen(
            [st.binary],
            cwd=st.root,
            stdin=subprocess.DEVNULL,
            stdout=slave,
            stderr=slave,
            start_new_session=True,
            close_fds=True,
        )
        os.close(slave)
        self.stopped = False
        self.thread = threading.Thread(target=self._drain, daemon=True)
        self.thread.start()

    def _drain(self):
        while True:
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                return
            if not chunk:
                return
            try:
                self.handle.write(chunk)
                self.handle.flush()
            except (ValueError, OSError):
                return

    def text(self):
        try:
            with open(self.path, "rb") as handle:
                return handle.read().decode("utf-8", "replace")
        except OSError:
            return ""

    def finish(self):
        if self.stopped:
            return
        self.stopped = True
        self.thread.join(timeout=2.0)
        try:
            os.close(self.master)
        except OSError:
            pass
        try:
            self.handle.close()
        except OSError:
            pass


def complete_lines(text):
    parts = text.split("\n")
    parts.pop()  # trailing newline (empty tail) or incomplete tail
    return [line.rstrip("\r") for line in parts]


def assert_alive(app, stage_name):
    status = app.proc.poll()
    if status is not None:
        app.proc.wait()
        app.finish()
        raise VariantFailure(
            "premature-exit",
            "original pid %d exited status %d%s during stage %s, after a valid "
            "startup and before the specified quit"
            % (app.proc.pid, status, signal_note(status), stage_name),
            stage_name,
        )


def wm_alive(stage_name):
    if st.wm is None:
        raise DriverError("wm", "openbox was never started", stage_name)
    status = st.wm.poll()
    if status is not None:
        st.wm.wait()
        raise DriverError(
            "wm",
            "openbox --sm-disable exited status %d%s during stage %s; %s"
            % (status, signal_note(status), stage_name, wm_output()),
            stage_name,
        )


def read_text(path):
    if path is None:
        return ""
    try:
        with open(path, "rb") as handle:
            return handle.read().decode("utf-8", "replace")
    except OSError:
        return ""


def wm_output():
    return "stdout=%r stderr=%r" % (read_text(st.wm_out), read_text(st.wm_err))


def wm_ready(deadline):
    stage("wm")
    last = "<no poll>"
    while True:
        st.iteration = now()
        wm_alive("wm")
        require_time(
            deadline,
            "wm",
            "wmctrl -m did not exit 0 and report 'Name: Openbox' within 5 s; "
            "last observation %s; openbox %s"
            % (last, wm_output()),
            "wm",
        )
        rc, out, err = run_tool(
            ["wmctrl", "-m"], tool_budget(deadline), "wmctrl -m"
        )
        last = "status=%d stdout=%r stderr=%r" % (rc, out, err)
        if rc == 0:
            if "Name: Openbox" in out:
                return
            left = deadline - now()
            if left <= 0.02:
                raise DriverError(
                    "wm",
                    "wmctrl -m did not report 'Name: Openbox' within 5 s; "
                    "last observation %s" % last,
                    "wm",
                )
        elif rc == 1:
            left = deadline - now()
            if left <= 0.02:
                raise DriverError(
                    "wm",
                    "wmctrl -m was not ready within 5 s (the window manager is "
                    "missing or not ready); last observation %s; openbox %s"
                    % (last, wm_output()),
                    "wm",
                )
        else:
            raise DriverError(
                "wm",
                "wmctrl -m failed with status %d (%r); openbox %s"
                % (rc, err, wm_output()),
                "wm",
            )
        sleep_poll(deadline)


def xdotool_ids(original_pid, visible_only, deadline, what):
    args = ["xdotool", "search", "--all"]
    if visible_only:
        args.append("--onlyvisible")
    args += ["--pid", str(original_pid)]
    rc, out, err = run_tool(args, tool_budget(deadline), what)
    if rc == 0:
        ids = []
        for line in out.splitlines():
            line = line.strip()
            if not line:
                continue
            if not line.isdigit():
                raise DriverError(
                    "tool",
                    "%s: malformed xdotool output %r" % (what, out),
                    st.stage,
                )
            ids.append(int(line))
        return ids
    if rc == 1 and err.strip() == "":
        # Ordinary empty result: no window matched yet, or the window is gone
        # while verifying a hide.  Never a tool error.
        return []
    if rc == 1:
        raise DriverError(
            "tool",
            "%s: X protocol error (status 1, stderr=%r)" % (what, err.strip()),
            st.stage,
        )
    raise DriverError(
        "tool",
        "%s: unexpected status %d stderr=%r" % (what, rc, err.strip()),
        st.stage,
    )


def wmctrl_managed(deadline, what):
    rc, out, err = run_tool(["wmctrl", "-lp"], tool_budget(deadline), what)
    if rc != 0:
        raise DriverError(
            "tool",
            "%s: wmctrl -lp status %d stderr=%r" % (what, rc, err.strip()),
            st.stage,
        )
    mapping = {}
    for line in out.splitlines():
        if not line.strip():
            continue
        fields = line.split(None, 4)
        if len(fields) < 3:
            raise DriverError(
                "tool",
                "%s: malformed wmctrl -lp line %r" % (what, line),
                st.stage,
            )
        try:
            window = int(fields[0], 0)
            pid = int(fields[2])
        except ValueError:
            raise DriverError(
                "tool",
                "%s: malformed wmctrl -lp line %r" % (what, line),
                st.stage,
            )
        mapping[window] = pid
    return mapping


def managed_for_pid(original_pid, deadline, what):
    mapping = wmctrl_managed(deadline, what)
    return [window for window, pid in mapping.items() if pid == original_pid]


def discover_once(app, deadline, stage_name):
    """One discovery poll; returns (unique_window_or_None, observation)."""
    visible = xdotool_ids(
        app.proc.pid,
        True,
        deadline,
        "xdotool search --all --onlyvisible --pid",
    )
    managed = managed_for_pid(
        app.proc.pid, deadline, "wmctrl -lp (window discovery)"
    )
    candidates = [window for window in visible if window in managed]
    observation = "xdotool_visible=%s wmctrl_managed_for_pid=%s candidates=%s" % (
        visible,
        sorted(managed),
        candidates,
    )
    if len(candidates) == 1:
        return candidates[0], observation
    return None, observation


def discover(app, deadline, stage_name):
    last = "<no poll>"
    while True:
        st.iteration = now()
        assert_alive(app, stage_name)
        wm_alive(stage_name)
        require_time(
            deadline,
            "discovery",
            "30 s discovery deadline expired; pid %d; last observation %s"
            % (app.proc.pid, last),
            stage_name,
        )
        window, last = discover_once(app, deadline, stage_name)
        if window is not None:
            return window
        if deadline - now() <= 0.02:
            raise DriverError(
                "discovery",
                "30 s discovery deadline expired; pid %d; %s; %s"
                % (
                    app.proc.pid,
                    last,
                    "multiple candidate windows"
                    if "candidates=[]" not in last
                    else "zero candidate windows",
                ),
                stage_name,
            )
        sleep_poll(deadline)


def wait_ready(app, deadline):
    stage("startup")
    seen = None
    while True:
        st.iteration = now()
        for line in complete_lines(app.text()):
            if READY_RE.fullmatch(line):
                seen = line
                break
        status = app.proc.poll()
        if status is not None:
            app.proc.wait()
            app.finish()
            # A line written just before the exit is still a valid record, but
            # an exit before the action is rejected either way.
            for line in complete_lines(app.text()):
                if READY_RE.fullmatch(line):
                    seen = line
                    break
            raise DriverError(
                "startup-exit",
                "original pid %d exited status %d%s before the initiating "
                "action (startup-ready record observed: %s); log tail=%r"
                % (
                    app.proc.pid,
                    status,
                    signal_note(status),
                    "yes" if seen else "no",
                    app.text()[-2000:],
                ),
                "startup",
            )
        if seen is not None:
            return seen
        if deadline - now() <= POLL:
            raise DriverError(
                "startup-timeout",
                "30 s deadline expired; original pid %d still running; the "
                "startup-ready record was not observed; log tail=%r"
                % (app.proc.pid, app.text()[-2000:]),
                "startup",
            )
        sleep_poll(deadline)


def xwininfo_map_state(window, deadline, stage_name):
    what = "xwininfo -id %s" % hex_id(window)
    rc, out, err = run_tool(
        ["xwininfo", "-id", hex_id(window)], tool_budget(deadline), what
    )
    if rc == 0:
        state = None
        for line in out.splitlines():
            if line.strip().startswith("Map State:"):
                state = line.split(":", 1)[1].strip()
        if state is None:
            raise DriverError(
                "tool",
                "%s: no 'Map State:' line in output %r" % (what, out),
                stage_name,
            )
        return state
    if rc == 1:
        if any(token in err for token in XWININFO_LOST_TOKENS):
            kind, value = xprop_pid(window, deadline, stage_name)
            if kind == "badwindow" or kind == "notfound":
                raise VariantFailure(
                    "hide-window-lost",
                    "window %d is destroyed: %s status 1 stderr=%r; the "
                    "following xprop confirmation reported %s"
                    % (window, what, err.strip(), kind),
                    stage_name,
                )
            raise DriverError(
                "tool",
                "%s: status 1 stderr=%r, but the following xprop -id %s "
                "_NET_WM_PID still resolves the window (%s)"
                % (what, err.strip(), hex_id(window), value),
                stage_name,
            )
        raise DriverError(
            "tool", "%s: status 1 stderr=%r" % (what, err.strip()), stage_name
        )
    raise DriverError(
        "tool",
        "%s: unexpected status %d stderr=%r" % (what, rc, err.strip()),
        stage_name,
    )


def xprop_pid(window, deadline, stage_name):
    what = "xprop -id %s _NET_WM_PID" % hex_id(window)
    rc, out, err = run_tool(
        ["xprop", "-id", hex_id(window), "_NET_WM_PID"],
        tool_budget(deadline),
        what,
    )
    if rc == 0:
        text = out.strip()
        if "not found" in text:
            # xprop without the property exits 0; a destroyed/replaced window
            # is not a successful hide.
            return ("notfound", text)
        match = re.search(r"_NET_WM_PID\(CARDINAL\)\s*=\s*(\d+)", text)
        if match is None:
            raise DriverError(
                "tool",
                "%s: malformed output %r" % (what, out),
                stage_name,
            )
        return ("pid", int(match.group(1)))
    if rc == 1:
        if "BadWindow" in err:
            return ("badwindow", err.strip())
        raise DriverError(
            "tool", "%s: status 1 stderr=%r" % (what, err.strip()), stage_name
        )
    raise DriverError(
        "tool",
        "%s: unexpected status %d stderr=%r" % (what, rc, err.strip()),
        stage_name,
    )


def send_close(window, deadline, stage_name):
    what = "wmctrl -i -c %s" % hex_id(window)
    rc, out, err = run_tool(
        ["wmctrl", "-i", "-c", hex_id(window)], tool_budget(deadline), what
    )
    if rc == 0:
        return
    if rc == 1:
        raise DriverError(
            "tool",
            "%s: could not send the close request (status 1, stderr=%r); this "
            "is never an application refusal" % (what, err.strip()),
            stage_name,
        )
    raise DriverError(
        "tool",
        "%s: unexpected status %d stderr=%r" % (what, rc, err.strip()),
        stage_name,
    )


def focus_window(window, deadline):
    stage("focus")
    last = "<no poll>"
    while True:
        st.iteration = now()
        assert_alive(st.original, "focus")
        wm_alive("focus")
        require_time(
            deadline,
            "focus",
            "5 s focus deadline expired; requested window %d (%s); last "
            "observation %s" % (window, hex_id(window), last),
            "focus",
        )
        rc, out, err = run_tool(
            ["xdotool", "windowactivate", "--sync", str(window)],
            tool_budget(deadline),
            "xdotool windowactivate",
        )
        if rc not in (0, 1):
            raise DriverError(
                "tool",
                "xdotool windowactivate %d: unexpected status %d stderr=%r"
                % (window, rc, err.strip()),
                "focus",
            )
        activate = "activate=%d/%r" % (rc, err.strip())
        rc2, out2, err2 = run_tool(
            ["xdotool", "windowfocus", "--sync", str(window)],
            tool_budget(deadline),
            "xdotool windowfocus",
        )
        if rc2 not in (0, 1):
            raise DriverError(
                "tool",
                "xdotool windowfocus %d: unexpected status %d stderr=%r"
                % (window, rc2, err2.strip()),
                "focus",
            )
        focus = "focus=%d/%r" % (rc2, err2.strip())
        rc3, out3, err3 = run_tool(
            ["xdotool", "getactivewindow"],
            tool_budget(deadline),
            "xdotool getactivewindow",
        )
        if rc3 not in (0, 1):
            raise DriverError(
                "tool",
                "xdotool getactivewindow: unexpected status %d stderr=%r"
                % (rc3, err3.strip()),
                "focus",
            )
        rc4, out4, err4 = run_tool(
            ["xdotool", "getwindowfocus"],
            tool_budget(deadline),
            "xdotool getwindowfocus",
        )
        if rc4 not in (0, 1):
            raise DriverError(
                "tool",
                "xdotool getwindowfocus: unexpected status %d stderr=%r"
                % (rc4, err4.strip()),
                "focus",
            )
        active = out3.strip()
        focused = out4.strip()
        last = "%s %s %s getactivewindow=%r getwindowfocus=%r" % (
            activate,
            focus,
            "verify",
            active,
            focused,
        )
        if rc3 == 0 and rc4 == 0 and active == str(window) and focused == str(window):
            return
        sleep_poll(deadline)


def await_quit(app, window, deadline):
    stage("await-shutdown")
    observed = False
    while True:
        st.iteration = now()
        status = app.proc.poll()
        if status is None:
            wm_alive("await-shutdown")
            if not observed:
                ok, why = shutdown_evidence()
                if ok:
                    observed = True
                    emit("shutdown-observed", " elapsed=%.3fs" % elapsed())
        if status is not None:
            status = app.proc.wait()
            app.finish()
            if not observed:
                ok, why = shutdown_evidence()
                if ok:
                    observed = True
                    emit("shutdown-observed", " elapsed=%.3fs" % elapsed())
                else:
                    raise VariantFailure(
                        "quit-not-observed",
                        "the original exited status %d%s before the saved "
                        "config showed shutdown evidence (%s)"
                        % (status, signal_note(status), why),
                        "await-shutdown",
                    )
            if status != 0:
                raise VariantFailure(
                    "quit-status",
                    "original actual exit status %d%s after shutdown was "
                    "observed (shutdown_observed=%s)"
                    % (status, signal_note(status), observed),
                    "await-shutdown",
                )
            return
        if deadline - now() <= POLL:
            ok, why = shutdown_evidence()
            if ok and not observed:
                observed = True
                emit("shutdown-observed", " elapsed=%.3fs" % elapsed())
            if observed:
                raise VariantFailure(
                    "quit-timeout",
                    "10 s deadline expired; original pid %d still alive; window "
                    "id %d; shutdown was observed in the saved config"
                    % (app.proc.pid, window),
                    "await-shutdown",
                )
            raise VariantFailure(
                "quit-not-observed",
                "10 s deadline expired; original pid %d still alive; window id "
                "%d; saved-config shutdown evidence missing (%s)"
                % (app.proc.pid, window, why),
                "await-shutdown",
            )
        sleep_poll(deadline)


def hide_then_restore(app):
    stage("hide")
    wm_alive("hide")
    assert_alive(app, "hide")
    assert_seed("hide")
    send_close(app.window, None, "hide")
    hide_deadline = now() + HIDE_TIMEOUT
    last = "<no poll>"
    while True:
        st.iteration = now()
        assert_alive(app, "hide")
        wm_alive("hide")
        if hide_deadline - now() <= POLL:
            raise VariantFailure(
                "hide-timeout",
                "5 s; window %d did not reach the exact xwininfo 'Map State: "
                "IsUnMapped'; last observation %s" % (app.window, last),
                "hide",
            )
        state = xwininfo_map_state(app.window, hide_deadline, "hide")
        visible = xdotool_ids(
            app.proc.pid,
            True,
            hide_deadline,
            "xdotool search --all --onlyvisible --pid (hide verification)",
        )
        every = xdotool_ids(
            app.proc.pid,
            False,
            hide_deadline,
            "xdotool search --all --pid (hide verification)",
        )
        last = "map_state=%r visible=%s all=%s" % (state, visible, every)
        if state == "IsUnMapped":
            if app.window in visible:
                last += " (still listed as visible)"
            elif app.window not in every:
                raise VariantFailure(
                    "hide-window-lost",
                    "window %d is no longer present in xdotool search --all "
                    "--pid %d; %s" % (app.window, app.proc.pid, last),
                    "hide",
                )
            else:
                kind, value = xprop_pid(app.window, hide_deadline, "hide")
                if kind == "notfound":
                    raise VariantFailure(
                        "hide-window-lost",
                        "window %d: xprop _NET_WM_PID reports %r, so the window "
                        "is destroyed rather than hidden" % (app.window, value),
                        "hide",
                    )
                if kind == "badwindow":
                    raise VariantFailure(
                        "hide-window-lost",
                        "window %d: xprop _NET_WM_PID reports BadWindow (%r)"
                        % (app.window, value),
                        "hide",
                    )
                if value != app.proc.pid:
                    raise VariantFailure(
                        "hide-window-lost",
                        "window %d reports _NET_WM_PID=%d but the original pid "
                        "is %d" % (app.window, value, app.proc.pid),
                        "hide",
                    )
                break
        sleep_poll(hide_deadline)

    stage("hide-stability")
    stable_deadline = now() + HIDE_STABLE
    while stable_deadline - now() > POLL:
        st.iteration = now()
        status = app.proc.poll()
        if status is not None:
            app.proc.wait()
            app.finish()
            raise VariantFailure(
                "premature-exit",
                "original pid %d exited status %d%s during the 1 s hide "
                "interval; this takes precedence over hide-not-stable"
                % (app.proc.pid, status, signal_note(status)),
                "hide-stability",
            )
        wm_alive("hide-stability")
        state = xwininfo_map_state(app.window, stable_deadline, "hide-stability")
        if state != "IsUnMapped":
            raise VariantFailure(
                "hide-not-stable",
                "window %d remapped to %r during the 1 s hide interval"
                % (app.window, state),
                "hide-stability",
            )
        visible = xdotool_ids(
            app.proc.pid,
            True,
            stable_deadline,
            "xdotool search --all --onlyvisible --pid (hide stability)",
        )
        every = xdotool_ids(
            app.proc.pid,
            False,
            stable_deadline,
            "xdotool search --all --pid (hide stability)",
        )
        if app.window in visible:
            raise VariantFailure(
                "hide-not-stable",
                "window %d reappeared in the visible search %s during the 1 s "
                "hide interval" % (app.window, visible),
                "hide-stability",
            )
        if app.window not in every:
            raise VariantFailure(
                "hide-window-lost",
                "window %d disappeared from xdotool search --all --pid %d "
                "during the 1 s hide interval"
                % (app.window, app.proc.pid),
                "hide-stability",
            )
        kind, value = xprop_pid(app.window, stable_deadline, "hide-stability")
        if kind == "notfound" or kind == "badwindow":
            raise VariantFailure(
                "hide-window-lost",
                "window %d lost its _NET_WM_PID during the 1 s hide interval "
                "(%s=%r)" % (app.window, kind, value),
                "hide-stability",
            )
        if value != app.proc.pid:
            raise VariantFailure(
                "hide-window-lost",
                "window %d reports _NET_WM_PID=%d during the 1 s hide interval "
                "but the original pid is %d"
                % (app.window, value, app.proc.pid),
                "hide-stability",
            )
        try:
            current = read_bytes(st.cfg)
        except OSError as exc:
            raise DriverError(
                "fixture",
                "config %s became unreadable during the hide interval: %s"
                % (st.cfg, exc),
                "hide-stability",
            )
        if current != st.seed:
            raise VariantFailure(
                "hide-not-stable",
                "config bytes changed during the 1 s hide interval",
                "hide-stability",
            )
        sleep_poll(stable_deadline)

    emit("hide-pass", " elapsed=%.3fs" % elapsed())
    return


def relaunch(app):
    stage("relaunch")
    wm_alive("relaunch")
    assert_alive(app, "relaunch")
    assert_seed("relaunch")
    st.second = App("second")
    deadline = now() + RELAUNCH_TIMEOUT
    last = "<no poll>"
    while True:
        st.iteration = now()
        status = st.second.proc.poll()
        if status is not None and status != 0:
            st.second.proc.wait()
            st.second.finish()
            raise VariantFailure(
                "relaunch-status",
                "second pid %d exited status %d%s; the second instance must "
                "exit exactly 0" % (
                    st.second.proc.pid,
                    status,
                    signal_note(status),
                ),
                "relaunch",
            )
        assert_alive(app, "relaunch")
        wm_alive("relaunch")
        if deadline - now() > POLL:
            window = None
            if status == 0:
                window, last = discover_once(app, deadline, "relaunch")
                if window is not None:
                    break
        if deadline - now() <= POLL:
            raise VariantFailure(
                "relaunch-timeout",
                "5 s; second pid %d status %s; original pid %d alive; the "
                "original's window did not uniquely reappear; last observation "
                "%s"
                % (
                    st.second.proc.pid,
                    "exited 0" if status == 0 else ("running" if status is None else status),
                    app.proc.pid,
                    last,
                ),
                "relaunch",
            )
        sleep_poll(deadline)

    if st.second.proc.poll() is None:
        # A window reappeared but the second instance is still running; it must
        # still reach status 0 within the 5 s deadline.
        while True:
            st.iteration = now()
            status = st.second.proc.poll()
            if status is not None:
                break
            if deadline - now() <= POLL:
                raise VariantFailure(
                    "relaunch-timeout",
                    "5 s; second pid %d still running after the window "
                    "reappeared; original pid %d alive"
                    % (st.second.proc.pid, app.proc.pid),
                    "relaunch",
                )
            sleep_poll(deadline)
        if status != 0:
            st.second.proc.wait()
            st.second.finish()
            raise VariantFailure(
                "relaunch-status",
                "second pid %d exited status %d%s after the window reappeared"
                % (st.second.proc.pid, status, signal_note(status)),
                "relaunch",
            )
    st.second.proc.wait()
    st.second.finish()
    app.window = window
    st.window = window
    return window


def run_barrier(app):
    stage("close")
    wm_alive("close")
    assert_alive(app, "close")
    assert_seed("close")
    quit_deadline = now() + QUIT_TIMEOUT
    send_close(app.window, quit_deadline, "close")
    await_quit(app, app.window, quit_deadline)


def run_no_barrier(app):
    stage("focus")
    focus_window(app.window, now() + FOCUS_TIMEOUT)
    wm_alive("pre-key")
    assert_alive(app, "pre-key")
    assert_seed("pre-key")
    quit_deadline = now() + QUIT_TIMEOUT
    rc, out, err = run_tool(
        ["xdotool", "key", "--clearmodifiers", "ctrl+q"],
        tool_budget(quit_deadline),
        "xdotool key --clearmodifiers ctrl+q",
    )
    if rc == 1:
        raise DriverError(
            "tool",
            "xdotool key --clearmodifiers ctrl+q: the key could not be "
            "delivered (status 1, stderr=%r); this is never a variant failure"
            % (err.strip(),),
            "pre-key",
        )
    if rc != 0:
        raise DriverError(
            "tool",
            "xdotool key --clearmodifiers ctrl+q: unexpected status %d "
            "stderr=%r" % (rc, err.strip()),
            "pre-key",
        )
    await_quit(app, app.window, quit_deadline)


def run_close_to_tray(app):
    hide_then_restore(app)
    window = relaunch(app)
    focus_window(window, now() + FOCUS_TIMEOUT)
    wm_alive("pre-key")
    assert_alive(app, "pre-key")
    assert_seed("pre-key")
    quit_deadline = now() + QUIT_TIMEOUT
    rc, out, err = run_tool(
        ["xdotool", "key", "--clearmodifiers", "ctrl+q"],
        tool_budget(quit_deadline),
        "xdotool key --clearmodifiers ctrl+q",
    )
    if rc == 1:
        raise DriverError(
            "tool",
            "xdotool key --clearmodifiers ctrl+q: the key could not be "
            "delivered (status 1, stderr=%r); this is never a variant failure"
            % (err.strip(),),
            "pre-key",
        )
    if rc != 0:
        raise DriverError(
            "tool",
            "xdotool key --clearmodifiers ctrl+q: unexpected status %d "
            "stderr=%r" % (rc, err.strip()),
            "pre-key",
        )
    await_quit(app, window, quit_deadline)


def start_wm():
    stage("wm")
    st.wm_out = os.path.join(st.logdir, "openbox.out")
    st.wm_err = os.path.join(st.logdir, "openbox.err")
    out_handle = open(st.wm_out, "wb")
    err_handle = open(st.wm_err, "wb")
    try:
        st.wm = subprocess.Popen(
            ["openbox", "--sm-disable"],
            cwd=st.root,
            stdin=subprocess.DEVNULL,
            stdout=out_handle,
            stderr=err_handle,
            start_new_session=True,
            close_fds=True,
        )
    except FileNotFoundError:
        raise DriverError("wm", "openbox --sm-disable: command not found", "wm")
    finally:
        out_handle.close()
        err_handle.close()
    wm_ready(now() + 5.0)


def run():
    stage("setup")
    st.token = os.environ.pop("GDQ_CHECK_TOKEN", "")
    if not st.token:
        raise DriverError(
            "wrapper", "GDQ_CHECK_TOKEN is missing inside the driver", "setup"
        )
    if not os.environ.get("DISPLAY"):
        raise DriverError(
            "wrapper",
            "DISPLAY is unset inside the driver: xvfb-run did not supply a "
            "private X display",
            "setup",
        )
    if not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        raise DriverError(
            "wrapper",
            "DBUS_SESSION_BUS_ADDRESS is unset inside the driver: "
            "dbus-run-session did not supply a private session bus",
            "setup",
        )
    if not os.path.isdir(st.root):
        raise DriverError("temp", "root %s is missing" % st.root, "setup")
    try:
        st.seed = read_bytes(st.cfg)
    except OSError as exc:
        raise DriverError(
            "temp", "seed config %s is unreadable: %s" % (st.cfg, exc), "setup"
        )
    observed = parse_preferences(st.seed, "fixture")
    if observed != SEEDS[st.variant]:
        raise DriverError(
            "fixture",
            "seeded preferences %r do not match the expected %r"
            % (observed, SEEDS[st.variant]),
            "fixture",
        )

    start_wm()

    stage("startup")
    st.original = App("original")
    st.pid = st.original.proc.pid
    app = st.original
    startup_deadline = now() + STARTUP_TIMEOUT
    ready_line = wait_ready(app, startup_deadline)
    app.window = discover(app, startup_deadline, "discovery")
    st.window = app.window
    assert_alive(app, "discovery")
    assert_seed("discovery")
    print(
        "READY variant=%s original_pid=%d window=%d record=%r config_seed=matched"
        % (st.variant, st.pid, app.window, ready_line),
        flush=True,
    )

    if st.variant == "barrier":
        run_barrier(app)
    elif st.variant == "no-barrier":
        run_no_barrier(app)
    elif st.variant == "close-to-tray":
        run_close_to_tray(app)
    else:
        raise DriverError("usage", "unknown variant %r" % st.variant, "setup")


def cleanup(aborted):
    problems = []
    apps = [app for app in (st.original, st.second) if app is not None]
    if aborted:
        for app in apps:
            if app.proc.poll() is None:
                try:
                    os.killpg(app.proc.pid, signal.SIGTERM)
                except (ProcessLookupError, PermissionError):
                    pass
        end = now() + 2.0
        while now() < end and any(app.proc.poll() is None for app in apps):
            time.sleep(0.05)
        for app in apps:
            if app.proc.poll() is None:
                try:
                    os.killpg(app.proc.pid, signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass
    for app in apps:
        try:
            app.proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            problems.append("pid %d did not reap" % (app.proc.pid,))
        app.finish()
    if st.wm is not None:
        if st.wm.poll() is None:
            try:
                st.wm.terminate()
            except ProcessLookupError:
                pass
            try:
                st.wm.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                st.wm.kill()
                try:
                    st.wm.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    problems.append("openbox pid %d did not reap" % (st.wm.pid,))
        else:
            st.wm.wait()
    if problems:
        raise DriverError("cleanup", "; ".join(problems), "cleanup")


def print_logs():
    entries = (
        (
            "original application log",
            st.original.path if st.original is not None else None,
        ),
        ("second application log", st.second.path if st.second is not None else None),
        ("openbox stdout", st.wm_out),
        ("openbox stderr", st.wm_err),
    )
    for label, path in entries:
        print("===== %s =====" % label, flush=True)
        if path is None:
            print("<not created>", flush=True)
            continue
        try:
            with open(path, "rb") as handle:
                data = handle.read()
        except OSError:
            print("<not created>", flush=True)
            continue
        text = data.decode("utf-8", "replace")
        print(text, end="" if text.endswith("\n") else "\n", flush=True)


def write_result(verdict, code, detail, stage_name):
    path = os.path.join(st.root, "result.json")
    payload = {
        "token": st.token,
        "verdict": verdict,
        "code": code,
        "detail": detail,
        "variant": st.variant,
        "stage": stage_name,
        "original_pid": st.pid,
        "window": st.window,
        "elapsed_seconds": round(elapsed(), 3),
    }
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, sort_keys=True)
        handle.write("\n")
    os.replace(temporary, path)


def on_signal(signum, frame):
    if st.in_cleanup:
        return
    raise Interrupted(signum)


def main():
    if len(sys.argv) != 4:
        print(
            "ERROR variant=unset code=usage driver received %d positional "
            "arguments" % (len(sys.argv) - 1,),
            flush=True,
        )
        return 2
    st.binary, st.variant, st.root = sys.argv[1], sys.argv[2], sys.argv[3]
    st.logdir = os.path.join(st.root, "log")
    st.cfg = os.path.join(st.root, "home/.goldendict/config")
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGHUP, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    verdict = "pass"
    code = None
    detail = ""
    try:
        run()
    except VariantFailure as exc:
        verdict, code, detail = "fail", exc.code, exc.detail
        fail_report(exc.code, exc.detail, exc.stage_name)
    except DriverError as exc:
        verdict, code, detail = "error", exc.code, exc.detail
        error_report(exc.code, exc.detail, exc.stage_name)
    except Interrupted as exc:
        verdict = "error"
        code = "interrupted"
        detail = "received signal %d during stage %s" % (exc.signum, st.stage)
        error_report(code, detail, st.stage)
    except BaseException as exc:  # noqa: BLE001 - report, never crash silently
        verdict = "error"
        code = "wrapper"
        detail = "unexpected driver failure: %r" % (exc,)
        traceback.print_exc()
        error_report(code, detail, st.stage)

    st.in_cleanup = True
    try:
        cleanup(aborted=(verdict != "pass"))
    except DriverError as exc:
        if verdict == "pass":
            verdict, code, detail = "error", "cleanup", exc.detail
        else:
            code = "cleanup"
            detail = detail + "; cleanup: " + exc.detail
        error_report(code, exc.detail, "cleanup")

    try:
        write_result(verdict, code, detail, st.stage)
    except OSError as exc:
        print(
            "ERROR variant=%s code=cleanup stage=result detail=could not write "
            "the token-bound result file: %s" % (st.variant, exc),
            flush=True,
        )
        return 2

    if verdict != "pass":
        print_logs()
        return 1 if verdict == "fail" else 2

    print(
        "PASS variant=%s original_pid=%d window=%d status=0 elapsed=%.3fs"
        % (st.variant, st.pid, st.window, elapsed()),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
PY_DRIVER
}

main() {
  if [ "$#" -ne 2 ]; then
    VARIANT=${2:-unset}
    [ -n "$VARIANT" ] || VARIANT=unset
    supervisor_error usage "received_arguments=$# ${USAGE}"
    return 2
  fi
  VARIANT=$2
  local binary_arg=$1
  case "$VARIANT" in
    barrier|no-barrier|close-to-tray) ;;
    *)
      supervisor_error usage "received_arguments=$# unrecognized_variant=$VARIANT ${USAGE}"
      return 2
      ;;
  esac

  local resolved=''
  if ! resolved=$(readlink -f -- "$binary_arg" 2>/dev/null); then
    supervisor_error binary "argument=$binary_arg readlink -f failed"
    return 2
  fi
  if [ -z "$resolved" ]; then
    supervisor_error binary "argument=$binary_arg readlink -f produced an empty path"
    return 2
  fi
  if [ ! -e "$resolved" ]; then
    supervisor_error binary "argument=$binary_arg resolved=$resolved does not exist"
    return 2
  fi
  if [ ! -f "$resolved" ]; then
    supervisor_error binary "argument=$binary_arg resolved=$resolved is not a regular file"
    return 2
  fi
  if [ ! -x "$resolved" ]; then
    supervisor_error binary "argument=$binary_arg resolved=$resolved is not executable"
    return 2
  fi

  local bin_dir
  bin_dir=$(dirname -- "$resolved")
  if [ -d "$bin_dir/portable" ]; then
    supervisor_error portable \
      "resolved_binary=$resolved portable_dir=$bin_dir/portable reason=portable build not supported by this check"
    return 2
  fi

  local -a deps=(
    bash python3 env mktemp mkdir dirname readlink chmod rm
    xvfb-run Xvfb xauth dbus-run-session dbus-daemon
    openbox wmctrl xdotool xprop xwininfo
  )
  local -a missing=()
  local dep
  for dep in "${deps[@]}"; do
    if ! command -v "$dep" >/dev/null 2>&1; then
      missing+=("$dep")
    fi
  done
  if [ "${#missing[@]}" -gt 0 ]; then
    local missing_list=''
    for dep in "${missing[@]}"; do
      missing_list+="$dep "
    done
    supervisor_error dependency "missing_commands=${missing_list% }"
    return 2
  fi

  if ! ROOT=$(mktemp -d /tmp/gdq.XXXXXXXX); then
    supervisor_error temp "mktemp -d /tmp/gdq.XXXXXXXX failed"
    return 2
  fi
  LOG=$ROOT/log
  if ! mkdir -p -- \
      "$ROOT/t" "$ROOT/home/.goldendict" "$ROOT/config" "$ROOT/cache" \
      "$ROOT/data" "$ROOT/runtime" "$LOG"; then
    supervisor_error temp "mkdir failed under root=$ROOT"
    return 2
  fi
  if ! chmod 700 -- \
      "$ROOT/t" "$ROOT/home" "$ROOT/home/.goldendict" "$ROOT/config" \
      "$ROOT/cache" "$ROOT/data" "$ROOT/runtime" "$LOG"; then
    supervisor_error temp "chmod 700 failed under root=$ROOT"
    return 2
  fi
  if ! write_config "$VARIANT"; then
    supervisor_error temp "could not write config fixture $ROOT/home/.goldendict/config"
    return 2
  fi
  if ! TOKEN=$(python3 -c 'import secrets; print(secrets.token_hex(16))'); then
    supervisor_error temp "could not generate the per-invocation token"
    return 2
  fi
  if [ -n "$INTERRUPT_SIGNAL" ]; then
    supervisor_error interrupted "received SIG$INTERRUPT_SIGNAL during setup"
    return 2
  fi

  run_driver "$resolved" "$VARIANT" &
  WRAPPER_PID=$!

  local wrapper_rc=0
  wait "$WRAPPER_PID" || wrapper_rc=$?
  WRAPPER_PID=''

  if [ -n "$INTERRUPT_SIGNAL" ]; then
    supervisor_error interrupted \
      "received SIG$INTERRUPT_SIGNAL while the xvfb-run/Xvfb and dbus-run-session driver tree was running (wrapper_status=$wrapper_rc)"
    print_wrapper_stdout
    print_failure_logs
    return 2
  fi

  local parsed word1 word2 word3
  parsed=$(read_driver_result) || parsed='READER_ERROR '
  read -r word1 word2 word3 <<<"$parsed"
  if [ "${word1:-}" != 'OK' ]; then
    supervisor_error wrapper \
      "wrapper_status=$wrapper_rc driver_result=${parsed:-<empty>} (xvfb-run/Xvfb display and private dbus-run-session tree; no trustworthy token-bound verdict)"
    print_wrapper_stdout
    print_failure_logs
    return 2
  fi

  local verdict=$word2 driver_code=$word3
  case "$verdict" in
    pass)
      if [ "$wrapper_rc" -ne 0 ]; then
        supervisor_error wrapper \
          "driver verdict=pass but wrapper_status=$wrapper_rc (xvfb-run/Xvfb and the private dbus-run-session tree did not exit cleanly)"
        print_wrapper_stdout
        print_failure_logs
        return 2
      fi
      print_wrapper_stdout
      return 0
      ;;
    fail)
      if [ "$wrapper_rc" -ne 1 ]; then
        supervisor_error wrapper \
          "driver verdict=fail code=$driver_code but wrapper_status=$wrapper_rc; expected 1 from the driver inside xvfb-run/dbus-run-session"
        print_wrapper_stdout
        print_failure_logs
        return 2
      fi
      print_wrapper_stdout
      print_failure_logs
      return 1
      ;;
    error)
      if [ "$wrapper_rc" -ne 2 ]; then
        supervisor_error wrapper \
          "driver verdict=error code=$driver_code but wrapper_status=$wrapper_rc; expected 2 from the driver inside xvfb-run/dbus-run-session"
        print_wrapper_stdout
        print_failure_logs
        return 2
      fi
      print_wrapper_stdout
      print_failure_logs
      return 2
      ;;
    *)
      supervisor_error wrapper "driver verdict=${verdict:-<empty>} is not recognized"
      print_wrapper_stdout
      print_failure_logs
      return 2
      ;;
  esac
}

main "$@"
status=$?

exit "$status"
