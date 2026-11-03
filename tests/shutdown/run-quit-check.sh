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
#                  process alive (xwininfo Map State: IsUnMapped, observed
#                  through the whole 1 s interval, seed config unchanged).  A
#                  second instance of the same binary must bring the window
#                  back; a focused Ctrl+Q must then exit 0.
#
# Dependencies, all required on PATH (checked before any of them is used):
#   bash python3 env mktemp mkdir dirname readlink chmod rm sleep
#   xvfb-run Xvfb xauth dbus-run-session dbus-daemon
#   openbox wmctrl xdotool xprop xwininfo   (xwininfo ships in xorg-xwininfo)
#
# Exit status:
#   0  the variant passed
#   1  a variant assertion failed after a valid startup (FAIL line)
#   2  usage, setup, environment, driver, interruption or cleanup error
#      (ERROR line).  A cleanup error always wins, even over a passing or
#      failing driver verdict.
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
# The supervisor owns argument validation, the dependency gate, the isolated
# root and the clean environment, and it trusts only the token-bound result
# file written by the driver.  The driver owns the window manager, both
# application instances and every driving tool, so everything shares one
# private X display and one private D-Bus session.  Each app and the WM run in
# their own process session; the driver records every such group in
# $ROOT/owned.pids so the supervisor can still clean up an owned group whose
# driver was killed before its own cleanup ran.
#
# On HUP/INT/TERM the supervisor signals only the process group it created for
# the xvfb-run/dbus-run-session/driver tree and then waits for that tree under a
# fixed monotonic bound.  If the bound expires - a stuck driver must never block
# cleanup - it escalates inside the same owned group, reaps any owned app/WM
# group that outlived its driver, and only then removes the root.  A cleanup
# failure, including a failed root removal after a variant failure, is always
# exit 2 with code=cleanup and reports the prior verdict as secondary detail.
# Escalation is always bounded to processes this invocation created: never
# `kill 0`, never a name based kill, never the caller's process group.
#
# Every supervisor deadline and every SIGTERM/SIGKILL grace period is measured
# on one genuine monotonic clock, selected once before the first bound is
# computed (/proc/uptime, or python3 time.monotonic_ns() when /proc/uptime is
# unreadable).  Wall-clock time is never read, so a clock step or an NTP
# correction can neither extend nor cut a bound; an unreadable clock expires
# pending bounds instead of silently moving the clock under them.
#
# A signal that arrives during result reporting or the final output can never
# produce a passing exit.  Finalization re-checks a pending interruption before
# the token-bound result is interpreted and again before a pass is returned,
# and the EXIT trap applies the same rule immediately before the root is
# removed.  Those diagnostics are printed while the captured logs still exist,
# and a cleanup error keeps precedence over an interrupt verdict.
#
# This is a standalone check.  It is deliberately NOT registered in
# `make check` and it adds no QtTest suite: the existing aggregate is a closed
# 19-suite inventory.  It is documented here, takes the binary as an argument
# and is intended to be run against pre-fix and fixed binaries separately.

set -u -o pipefail
# Run background jobs in their own process group so an interrupt can stop the
# private xvfb-run/dbus-run-session/driver tree without touching any other
# process.
set -m
umask 077

USAGE='usage: tests/shutdown/run-quit-check.sh <goldendict-binary> <barrier|no-barrier|close-to-tray>'

VARIANT=unset
ROOT=''
LOG=''
TOKEN=''
DRIVER_PID=''
DRIVER_GROUP=''
INTERRUPT_SIGNAL=''
GROUP_NOTE=''
TREE_ESCALATED=0
TREE_SURVIVORS=0
TREE_DETAIL=''
OWNED_DETAIL=''
VERDICT_NOTE=''
DRIVER_REAPED=0
DRIVER_UNREAPED=0
# Bounds for the driver wait.  Without a supervisor signal the driver bounds
# its own work, so its wait is a plain blocking wait.  Once HUP/INT/TERM has
# arrived the wait is bounded: a driver stuck in its own cleanup must never
# block the supervisor's cleanup and root removal.
DRIVER_WAIT_LIMIT_US=5000000
DRIVER_REAP_AFTER_KILL_US=2000000

supervisor_error() { # <code> <detail>
  VERDICT_NOTE="verdict=error code=$1"
  printf 'ERROR variant=%s code=%s %s\n' "$VARIANT" "$1" "$2" >&2
}

# --- monotonic clock -------------------------------------------------------
# Every supervisor deadline and grace period is measured on a genuine
# monotonic clock, never on wall-clock time.  $EPOCHREALTIME and $SECONDS are
# wall-clock based: a clock step would silently move every deadline under a
# running bound, so neither is used anywhere in this script.
#   proc    /proc/uptime (CLOCK_BOOTTIME); 10 ms resolution, no subprocess
#   python  time.monotonic_ns(), through the already required python3
# The source is chosen once, by select_monotonic_source(), before the first
# deadline exists, and every later read uses that same source, so two
# different clocks can never be mixed inside one bound.
MONOTONIC_SOURCE=''
CLOCK_FAILED=0
NOW_US=0

read_uptime_us() { # /proc/uptime -> monotonic microseconds; 1 when unreadable
  local uptime rest whole frac
  { IFS=' ' read -r uptime rest < /proc/uptime; } 2>/dev/null || return 1
  [ -n "$uptime" ] || return 1
  whole=${uptime%%.*}
  case $uptime in
    *.*) frac=${uptime#*.} ;;
    *) frac='' ;;
  esac
  case $whole in
    ''|*[!0-9]*) return 1 ;;
  esac
  case $frac in
    *[!0-9]*) return 1 ;;
  esac
  frac="${frac}000000"
  frac=${frac:0:6}
  printf '%s\n' "$(( 10#$whole * 1000000 + 10#$frac ))"
}

select_monotonic_source() { # choose one monotonic source, once, and cache it
  [ -n "$MONOTONIC_SOURCE" ] && return 0
  if read_uptime_us >/dev/null 2>&1; then
    MONOTONIC_SOURCE=proc
    return 0
  fi
  if python3 -c 'import time; print(time.monotonic_ns() // 1000)' >/dev/null 2>&1; then
    MONOTONIC_SOURCE=python
    return 0
  fi
  return 1
}

now_us() { # sets NOW_US; monotonic microseconds, never wall-clock time
  # The value is stored in NOW_US rather than printed: a command substitution
  # would run in a subshell, so a clock failure could not set CLOCK_FAILED for
  # the caller and the failure would stay invisible.
  local source_us=''
  case "$MONOTONIC_SOURCE" in
    proc)
      source_us=$(read_uptime_us 2>/dev/null) || source_us=''
      ;;
    python)
      source_us=$(python3 -c 'import time; print(time.monotonic_ns() // 1000)' 2>/dev/null) || source_us=''
      ;;
    *)
      source_us=''
      ;;
  esac
  case $source_us in
    ''|*[!0-9]*)
      # Fail closed and loudly: never fall back to wall-clock time inside a
      # live bound.  NOW_US is set beyond every representable deadline, so each
      # pending bound expires at once; main() reports CLOCK_FAILED as a
      # cleanup error, so a clock failure can neither pass nor hang.
      NOW_US=999999999999999999
      if [ "$CLOCK_FAILED" -eq 0 ]; then
        CLOCK_FAILED=1
        printf 'ERROR variant=%s code=cleanup the supervisor monotonic clock (source=%s) could not be read; pending bounds are treated as expired rather than measured on wall-clock time\n' \
          "$VARIANT" "${MONOTONIC_SOURCE:-unset}" >&2
      fi
      return 1
      ;;
  esac
  NOW_US=$source_us
  return 0
}

before_deadline() { # <deadline_us>; 0 while that monotonic bound has time left
  now_us && [ "$NOW_US" -lt "$1" ]
}

group_members() { # <pgid>; live (non-zombie) pids in that process group
  local pgid=$1 entry pid stat state pgrp
  for entry in /proc/[0-9]*; do
    pid=${entry#/proc/}
    stat=''
    { IFS= read -r stat <"$entry/stat"; } 2>/dev/null || continue
    [ -n "$stat" ] || continue
    # Field 2 (comm) may contain spaces and parentheses; everything after the
    # last ") " is: state ppid pgrp session ...
    stat=${stat##*') '}
    # shellcheck disable=SC2086
    set -- $stat
    [ "$#" -ge 3 ] || continue
    state=$1
    pgrp=$3
    [ "$state" = 'Z' ] && continue
    [ "$pgrp" = "$pgid" ] && printf '%s\n' "$pid"
  done
  return 0
}

group_list() { # <pgid> -> space separated live pids
  local members
  members=$(group_members "$1")
  members=${members//$'\n'/ }
  printf '%s\n' "${members% }"
}

stop_owned_group() { # <pgid>; TERM then KILL one owned group, bounded
  local pgid=$1 deadline members
  GROUP_NOTE=''
  members=$(group_members "$pgid")
  [ -n "$members" ] || return 0
  kill -TERM -- "-$pgid" 2>/dev/null || true
  now_us || true
  deadline=$(( NOW_US + 2000000 ))
  while [ -n "$(group_members "$pgid")" ] && before_deadline "$deadline"; do
    sleep 0.05
  done
  members=$(group_members "$pgid")
  if [ -n "$members" ]; then
    kill -KILL -- "-$pgid" 2>/dev/null || true
    now_us || true
    deadline=$(( NOW_US + 2000000 ))
    while [ -n "$(group_members "$pgid")" ] && before_deadline "$deadline"; do
      sleep 0.05
    done
    members=$(group_members "$pgid")
  fi
  if [ -n "$members" ]; then
    GROUP_NOTE="owned process group $pgid still has live pids=$(group_list "$pgid") after SIGTERM and SIGKILL"
    return 1
  fi
  return 0
}

proc_state() { # <pid>; /proc state letter, empty when the pid is gone
  local stat=''
  { IFS= read -r stat <"/proc/$1/stat"; } 2>/dev/null || return 0
  [ -n "$stat" ] || return 0
  # Field 2 (comm) may contain spaces and parentheses; everything after the
  # last ") " is: state ppid pgrp session ...
  stat=${stat##*') '}
  # shellcheck disable=SC2086
  set -- $stat
  [ "$#" -ge 1 ] || return 0
  case $1 in
    [A-Z]) printf '%s\n' "$1" ;;
  esac
  return 0
}

wait_driver_bounded() { # <pid> <bound_us>; 0 = reaped (WAIT_STATUS), 1 = bound
  local pid=$1 bound_us=$2 deadline state attempt
  WAIT_STATUS=0
  now_us || true
  deadline=$(( NOW_US + bound_us ))
  while :; do
    state=$(proc_state "$pid")
    if [ -z "$state" ] || [ "$state" = 'Z' ]; then
      # The child has already exited, so wait(1) cannot block; a trapped signal
      # can still interrupt the collection, so it is retried while the pid is
      # still visible.
      attempt=0
      while [ "$attempt" -lt 100 ]; do
        attempt=$(( attempt + 1 ))
        WAIT_STATUS=0
        wait "$pid" || WAIT_STATUS=$?
        if [ -z "$(proc_state "$pid")" ]; then
          return 0
        fi
      done
      return 1
    fi
    if ! before_deadline "$deadline"; then
      return 1
    fi
    sleep 0.05
  done
}

escalate_wrapper_tree() { # <why>; TERM then KILL this invocation's group only
  TREE_ESCALATED=1
  if stop_owned_group "$DRIVER_GROUP"; then
    TREE_DETAIL="escalated to SIGTERM/SIGKILL for owned process group $DRIVER_GROUP $1"
  else
    TREE_SURVIVORS=1
    TREE_DETAIL=$GROUP_NOTE
  fi
}

on_signal() { # <NAME>
  if [ -n "$INTERRUPT_SIGNAL" ]; then
    return 0
  fi
  INTERRUPT_SIGNAL=$1
  if [ -n "$DRIVER_PID" ]; then
    # Only the process group this supervisor created for the wrapper tree.
    kill -TERM -- "-$DRIVER_PID" 2>/dev/null || true
  fi
}
trap 'on_signal INT' INT
trap 'on_signal HUP' HUP
trap 'on_signal TERM' TERM

cleanup_owned_pids() { # reap any owned app/WM group that outlived its driver
  local pgid rc=0
  OWNED_DETAIL=''
  [ -f "$ROOT/owned.pids" ] || return 0
  while IFS= read -r pgid; do
    case $pgid in
      ''|*[!0-9]*) continue ;;
    esac
    [ -n "$(group_members "$pgid")" ] || continue
    if ! stop_owned_group "$pgid"; then
      OWNED_DETAIL+="${OWNED_DETAIL:+; }$GROUP_NOTE"
      rc=1
    fi
  done <"$ROOT/owned.pids"
  return "$rc"
}

cleanup_root_on_exit() {
  local status=$? interrupt_overrode=0
  trap - EXIT
  if [ -n "$ROOT" ] && [ -d "$ROOT" ]; then
    # A signal that is still pending when finalization ends must never leave a
    # passing exit status behind.  It is honoured here, before the root - and
    # with it every captured log - is removed, so its diagnostics can still be
    # printed.  A failed removal below stays a cleanup error and keeps
    # precedence through VERDICT_NOTE.
    if [ -n "$INTERRUPT_SIGNAL" ] && [ "$status" -eq 0 ]; then
      interrupt_overrode=1
      supervisor_error interrupted \
        "received SIG$INTERRUPT_SIGNAL during finalization (result reporting or the final output); the token-bound result is not accepted and exit status 0 is overridden"
      print_wrapper_stdout
      print_failure_logs
      status=2
    fi
    if ! rm -rf -- "$ROOT"; then
      # A failed removal is a cleanup error even when the prior verdict was a
      # variant failure: exit 2, with the prior verdict as secondary detail.
      printf 'ERROR variant=%s code=cleanup could not remove root=%s exit_status_before_cleanup=%s prior_verdict=%s\n' \
        "$VARIANT" "$ROOT" "$status" "${VERDICT_NOTE:-none}" >&2
      status=2
    fi
    # A signal can also arrive while the root is being removed.  The same rule
    # applies, but the logs are gone by now, so only the verdict is reported.
    if [ "$interrupt_overrode" -eq 0 ] && [ -n "$INTERRUPT_SIGNAL" ] && [ "$status" -eq 0 ]; then
      supervisor_error interrupted \
        "received SIG$INTERRUPT_SIGNAL while the root was being removed; the token-bound result is not accepted and exit status 0 is overridden"
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

interrupt_during_finalization() { # <where>; 1 when a pending signal wins
  [ -n "$INTERRUPT_SIGNAL" ] || return 0
  # The driver may already have written a pass into its token-bound result
  # file.  A signal that arrived while that result was being read or reported
  # makes this an interruption error, never a pass: finalization did not run to
  # completion, so its outcome is not trustworthy.
  supervisor_error interrupted \
    "received SIG$INTERRUPT_SIGNAL $1; the token-bound result is not accepted because a signal received during finalization must never produce a passing exit"
  print_wrapper_stdout
  print_failure_logs
  return 1
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
MAP_STATES = ("IsUnMapped", "IsUnviewable", "IsViewable")
INTERRUPT_SIGNALS = {signal.SIGINT, signal.SIGHUP, signal.SIGTERM}

POLL = 0.1
TOOL_LIMIT = 2.0
# The stage windows of the specification (5 s / 10 s / 30 s) are the only
# deadlines there are.  There is no per-stage slack: every tool call is bounded
# by the time that actually remains before the real monotonic deadline, and a
# budget that runs out is reported as that stage's deadline, never as a tool
# error and never as extra seconds.
STARTUP_TIMEOUT = 30.0
HIDE_TIMEOUT = 5.0
HIDE_STABLE = 1.0
# The 1 s hide-stability interval is observed end to end by the polling that
# runs until its own monotonic deadline.  The boundary observation that may
# authorise hide-pass starts only after that deadline has passed and carries
# its own budget, so the polling inside the interval can never spend it and a
# fast observation taken before the interval ends can never authorise a pass.
HIDE_BOUNDARY_LIMIT = TOOL_LIMIT
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


class StageDeadline(Exception):
    """A stage's monotonic deadline was reached before a tool could finish.

    It is not a tool error: the caller converts it into the deadline
    diagnostic of its own stage (hide-timeout, focus, discovery, ...).
    """

    def __init__(self, detail):
        Exception.__init__(self, detail)
        self.detail = detail


class DeferredSignals(object):
    """Block the interruption signals across a spawn and its registration.

    A signal delivered in that window stays pending; the Python handler runs
    as soon as the mask is restored, by which point cleanup already owns the
    new child.  This is what keeps an app or the WM that was spawned but not
    yet assigned to its state slot from being lost.
    """

    def __enter__(self):
        self.previous = signal.pthread_sigmask(signal.SIG_BLOCK, INTERRUPT_SIGNALS)
        return self

    def __exit__(self, exc_type, exc, tb):
        signal.pthread_sigmask(signal.SIG_SETMASK, self.previous)
        return False


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
st.budget_deadline = None
st.owned_groups = set()
st.owned_apps = []
st.owned_file = None


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
    """The smaller of two seconds and the time actually left in the stage.

    The budget always comes from the real monotonic deadline; there is no
    per-stage allowance.  An exhausted budget is a stage deadline, so the
    caller reports the stage's own diagnostic instead of a tool error.
    """
    st.budget_deadline = deadline
    if deadline is None:
        return TOOL_LIMIT
    left = deadline - now()
    if left <= 0.0:
        raise StageDeadline(
            "stage %s reached its deadline before another tool call" % (st.stage,)
        )
    return min(TOOL_LIMIT, left)


def poll_sleep(window_end):
    """Sleep until the next <=POLL-spaced poll, but never past the window end."""
    target = min(st.iteration + POLL, window_end)
    delay = target - now()
    if delay > 0.0:
        time.sleep(delay)


def run_tool(argv, timeout, what, original_guard="always"):
    """Run one external tool, bounded by the remaining stage budget.

    Statuses are interpreted by the caller according to the per-command table
    in spec section 2: this helper only reports, it never classifies.
    `original_guard` decides when the original process is re-checked before the
    caller classifies the result:

      "always"    after every command, so an application that died during a
                  query is reported as premature-exit rather than as a
                  window/tool symptom;
      "on-error"  for the final key and close commands: a successful dispatch
                  may be followed by the original's own normal exit, but a
                  nonzero command result must not be blamed on the tool when
                  the process died first.

    A call that times out exactly when its stage deadline expires is the stage
    deadline, not a tool malfunction, and is reported as such.
    """
    if timeout <= 0.0:
        raise StageDeadline(
            "%s: no time left in stage %s before its deadline" % (what, st.stage)
        )
    try:
        proc = subprocess.run(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
    except FileNotFoundError:
        raise DriverError(
            "tool", "%s: command not found: %s" % (what, argv[0]), st.stage
        )
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or b"").decode("utf-8", "replace")
        err = (exc.stderr or b"").decode("utf-8", "replace")
        if st.budget_deadline is not None and now() >= st.budget_deadline:
            raise StageDeadline(
                "%s: timed out after %.2fs at the stage %s deadline "
                "(stdout=%r stderr=%r)"
                % (what, timeout, st.stage, out, err)
            )
        raise DriverError(
            "tool",
            "%s: timed out after %.2fs (stage %s) stdout=%r stderr=%r"
            % (what, timeout, st.stage, out, err),
            st.stage,
        )
    result = (
        proc.returncode,
        proc.stdout.decode("utf-8", "replace"),
        proc.stderr.decode("utf-8", "replace"),
    )
    if original_guard == "always" or (
        original_guard == "on-error" and result[0] != 0
    ):
        guard_original_status(what)
    return result


def register_group(pgid):
    """Record an owned process group before interruption can be handled.

    The record is appended while the spawn's signals are still deferred, so
    the supervisor can reap a group whose driver was killed before its own
    cleanup ran.
    """
    st.owned_groups.add(pgid)
    if st.owned_file is None:
        return
    try:
        handle = os.open(
            st.owned_file, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600
        )
    except OSError:
        return
    try:
        os.write(handle, ("%d\n" % pgid).encode("ascii"))
    finally:
        os.close(handle)


def group_members(pgid):
    """Live (non-zombie) pids still in process group `pgid`."""
    members = []
    try:
        entries = os.listdir("/proc")
    except OSError:
        return members
    for name in entries:
        if not name.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % name, "rb") as handle:
                data = handle.read().decode("utf-8", "replace")
        except OSError:
            continue
        close_paren = data.rfind(")")
        if close_paren < 0:
            continue
        fields = data[close_paren + 2:].split()
        if len(fields) < 3:
            continue
        state, pgrp = fields[0], fields[2]
        if state == "Z":
            continue
        try:
            if int(pgrp) == pgid:
                members.append(int(name))
        except ValueError:
            continue
    return sorted(members)


def reap_group(pgid, grace):
    """TERM then KILL one owned process group; returns a problem string.

    The group is addressed independently of its leader's exit status: a
    descendant can outlive the leader the group was created with.
    """
    if not group_members(pgid):
        return ""
    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        return ""
    except PermissionError as exc:
        return "killpg(%d, SIGTERM): %s" % (pgid, exc)
    end = now() + grace
    while now() < end and group_members(pgid):
        time.sleep(0.05)
    members = group_members(pgid)
    if members:
        try:
            os.killpg(pgid, signal.SIGKILL)
        except ProcessLookupError:
            return ""
        except PermissionError as exc:
            return "killpg(%d, SIGKILL): %s" % (pgid, exc)
        end = now() + grace
        while now() < end and group_members(pgid):
            time.sleep(0.05)
        members = group_members(pgid)
    if members:
        return (
            "owned process group %d still has live pids=%s after SIGTERM and "
            "SIGKILL" % (pgid, members)
        )
    return ""


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

    The spawn and the registration of the new process group happen with the
    interruption signals blocked; a failure after the fork reaps the child
    instead of leaving it behind.
    """

    def __init__(self, tag):
        self.tag = tag
        self.path = os.path.join(st.logdir, tag + ".log")
        self.handle = None
        self.master = None
        self.proc = None
        self.thread = None
        self.stopped = False
        with DeferredSignals():
            slave = None
            try:
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
            except BaseException:
                if slave is not None:
                    try:
                        os.close(slave)
                    except OSError:
                        pass
                self.reap_failed_construct()
                raise
            else:
                try:
                    os.close(slave)
                except OSError:
                    pass
            register_group(self.proc.pid)
            st.owned_apps.append(self)
        try:
            self.thread = threading.Thread(target=self._drain, daemon=True)
            self.thread.start()
        except BaseException:
            self.reap_failed_construct()
            raise

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
        if self.thread is not None:
            self.thread.join(timeout=2.0)
        if self.master is not None:
            try:
                os.close(self.master)
            except OSError:
                pass
            self.master = None
        if self.handle is not None:
            try:
                self.handle.close()
            except OSError:
                pass
            self.handle = None

    def reap_failed_construct(self):
        proc = self.proc
        if proc is not None:
            reap_group(proc.pid, 1.0)
            try:
                proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                pass
        self.finish()


def complete_lines(text):
    parts = text.split("\n")
    parts.pop()  # trailing newline (empty tail) or incomplete tail
    return [line.rstrip("\r") for line in parts]


def guard_original_status(what):
    """Re-check the original process right after a command, before judgement."""
    app = st.original
    if app is None or app.proc is None:
        return
    status = app.proc.poll()
    if status is None:
        return
    status = app.proc.wait()
    app.finish()
    raise VariantFailure(
        "premature-exit",
        "original pid %d exited status %d%s during stage %s immediately after "
        "%s, before that command's result was classified"
        % (app.proc.pid, status, signal_note(status), st.stage, what),
        st.stage,
    )


def assert_alive(app, stage_name):
    status = app.proc.poll()
    if status is not None:
        status = app.proc.wait()
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


def wm_probe(window_end):
    """Wait (<=5 s) until Openbox owns the WM selection and names itself.

    This is the 5 s readiness window of spec section 2 step 1.  It is probed
    with the root selection property, so `wmctrl -m` itself is invoked exactly
    once afterwards: its status 1 is the table's driver error ("the WM is
    missing or not ready") and must not be produced by a transient startup
    state.
    """
    last = "<no poll>"
    while True:
        st.iteration = now()
        if st.iteration >= window_end:
            # The 5 s readiness window is over: the probes below would have no
            # budget left, so the deadline owns the outcome.
            break
        wm_alive("wm")
        try:
            rc, out, err = run_tool(
                ["xprop", "-root", "_NET_SUPPORTING_WM_CHECK"],
                tool_budget(window_end),
                "xprop -root _NET_SUPPORTING_WM_CHECK",
            )
            last = "status=%d stdout=%r stderr=%r" % (rc, out, err)
            if rc not in (0, 1):
                raise DriverError(
                    "wm",
                    "xprop -root _NET_SUPPORTING_WM_CHECK: unexpected status %d "
                    "stderr=%r; openbox %s" % (rc, err, wm_output()),
                    "wm",
                )
            if rc == 1:
                raise DriverError(
                    "wm",
                    "xprop -root _NET_SUPPORTING_WM_CHECK exited 1 (%r); openbox %s"
                    % (err.strip(), wm_output()),
                    "wm",
                )
            match = re.search(r"window id # (0x[0-9a-fA-F]+)", out)
            if match is not None:
                owner = match.group(1)
                rc2, out2, err2 = run_tool(
                    ["xprop", "-id", owner, "_NET_WM_NAME"],
                    tool_budget(window_end),
                    "xprop -id <wm-supporting-window> _NET_WM_NAME",
                )
                last += (
                    " supporting_window=%s name_status=%d name_stdout=%r "
                    "name_stderr=%r" % (owner, rc2, out2, err2)
                )
                if rc2 == 1 and "BadWindow" in err2:
                    raise DriverError(
                        "wm",
                        "the window manager's supporting window %s vanished while "
                        "its name was being read; openbox %s" % (owner, wm_output()),
                        "wm",
                    )
                if rc2 not in (0, 1):
                    raise DriverError(
                        "wm",
                        "xprop -id %s _NET_WM_NAME: unexpected status %d stderr=%r; "
                        "openbox %s" % (owner, rc2, err2, wm_output()),
                        "wm",
                    )
                if rc2 == 0 and "Openbox" in out2:
                    return
        except StageDeadline as exc:
            last += " (deadline: %s)" % (exc.detail,)
            break
        poll_sleep(window_end)
    raise DriverError(
        "wm",
        "openbox --sm-disable did not publish the WM selection and its "
        "'Openbox' name within 5 s; last observation %s; openbox %s"
        % (last, wm_output()),
        "wm",
    )


def wm_ready(window_end):
    stage("wm")
    wm_probe(window_end)
    # Spec section 2 status table, applied literally: status 1 from wmctrl -m is
    # a driver error, status 0 with 'Name: Openbox' is readiness, and every
    # other status is a driver error too.
    try:
        budget = tool_budget(window_end)
    except StageDeadline as exc:
        raise DriverError(
            "wm",
            "wmctrl -m could not run inside the 5 s readiness window: %s; "
            "openbox %s" % (exc.detail, wm_output()),
            "wm",
        )
    rc, out, err = run_tool(["wmctrl", "-m"], budget, "wmctrl -m")
    if rc == 1:
        raise DriverError(
            "wm",
            "wmctrl -m exited 1: the window manager is missing or not ready "
            "(stderr=%r); openbox %s" % (err.strip(), wm_output()),
            "wm",
        )
    if rc != 0:
        raise DriverError(
            "wm",
            "wmctrl -m failed with status %d (%r); openbox %s"
            % (rc, err.strip(), wm_output()),
            "wm",
        )
    if "Name: Openbox" not in out:
        raise DriverError(
            "wm",
            "wmctrl -m exited 0 but did not report 'Name: Openbox' "
            "(stdout=%r); openbox %s" % (out, wm_output()),
            "wm",
        )


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
        # Section 2 table: a non-zero wmctrl -lp is a driver error because the
        # window manager is missing or no longer ready.
        raise DriverError(
            "wm",
            "%s: wmctrl -lp status %d stderr=%r; %s"
            % (what, rc, err.strip(), wm_output()),
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


def discover(app, window_end, stage_name):
    last = "<no poll>"
    while True:
        st.iteration = now()
        if st.iteration >= window_end:
            break
        assert_alive(app, stage_name)
        wm_alive(stage_name)
        try:
            window, last = discover_once(app, window_end, stage_name)
        except StageDeadline as exc:
            last += " (deadline: %s)" % (exc.detail,)
            break
        if window is not None:
            return window
        poll_sleep(window_end)
    if last == "<no poll>":
        kind = "no complete observation was made inside the deadline"
    elif "candidates=[]" in last:
        kind = "zero candidate windows"
    else:
        kind = "ambiguous candidate windows"
    raise DriverError(
        "discovery",
        "30 s discovery deadline expired; pid %d; %s; last observation %s"
        % (app.proc.pid, kind, last),
        stage_name,
    )


def wait_ready(app, window_end):
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
            status = app.proc.wait()
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
        if st.iteration >= window_end:
            raise DriverError(
                "startup-timeout",
                "30 s deadline expired; original pid %d still running; the "
                "startup-ready record was not observed; log tail=%r"
                % (app.proc.pid, app.text()[-2000:]),
                "startup",
            )
        poll_sleep(window_end)


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
        if state not in MAP_STATES:
            raise DriverError(
                "tool",
                "%s: invalid Map State value %r (expected one of %s)"
                % (what, state, ", ".join(MAP_STATES)),
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


def send_close(window, deadline, stage_name, original_guard="always"):
    what = "wmctrl -i -c %s" % hex_id(window)
    rc, out, err = run_tool(
        ["wmctrl", "-i", "-c", hex_id(window)],
        tool_budget(deadline),
        what,
        original_guard=original_guard,
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


def focus_window(window, window_end):
    stage("focus")
    last = "<no poll>"
    while True:
        st.iteration = now()
        if st.iteration >= window_end:
            break
        assert_alive(st.original, "focus")
        wm_alive("focus")
        try:
            rc, _out, err = run_tool(
                ["xdotool", "windowactivate", "--sync", str(window)],
                tool_budget(window_end),
                "xdotool windowactivate",
            )
            if rc not in (0, 1):
                raise DriverError(
                    "tool",
                    "xdotool windowactivate %d: unexpected status %d stderr=%r"
                    % (window, rc, err.strip()),
                    "focus",
                )
            activate_ok = rc == 0
            rc2, _out2, err2 = run_tool(
                ["xdotool", "windowfocus", "--sync", str(window)],
                tool_budget(window_end),
                "xdotool windowfocus",
            )
            if rc2 not in (0, 1):
                raise DriverError(
                    "tool",
                    "xdotool windowfocus %d: unexpected status %d stderr=%r"
                    % (window, rc2, err2.strip()),
                    "focus",
                )
            focus_ok = rc2 == 0
            rc3, out3, err3 = run_tool(
                ["xdotool", "getactivewindow"],
                tool_budget(window_end),
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
                tool_budget(window_end),
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
            if rc3 == 0 and not active.isdigit():
                raise DriverError(
                    "tool",
                    "xdotool getactivewindow: malformed output %r" % (out3,),
                    "focus",
                )
            if rc4 == 0 and not focused.isdigit():
                raise DriverError(
                    "tool",
                    "xdotool getwindowfocus: malformed output %r" % (out4,),
                    "focus",
                )
            last = (
                "activate=%d/%r focus=%d/%r getactivewindow=%r getwindowfocus=%r"
                % (rc, err.strip(), rc2, err2.strip(), active, focused)
            )
            if (
                activate_ok
                and focus_ok
                and rc3 == 0
                and rc4 == 0
                and active == str(window)
                and focused == str(window)
            ):
                return
        except StageDeadline as exc:
            last += " (deadline: %s)" % (exc.detail,)
            break
        poll_sleep(window_end)
    raise DriverError(
        "focus",
        "5 s focus deadline expired; requested window %d (%s); last "
        "observation %s" % (window, hex_id(window), last),
        "focus",
    )


def await_quit(app, window, deadline):
    """Observe the original until it exits, never past the 10 s deadline.

    Only a status sample taken inside the deadline can produce a pass: an exit
    observed after the deadline is a deadline expiry, not a success.  When the
    deadline is reached the outcome is still classified so the diagnostic says
    whether the process stayed alive or exited too late.
    """
    stage("await-shutdown")
    observed = False
    while True:
        st.iteration = now()
        if st.iteration > deadline:
            break
        status = app.proc.poll()
        if status is None:
            wm_alive("await-shutdown")
            if not observed:
                ok, _why = shutdown_evidence()
                if ok:
                    observed = True
                    emit("shutdown-observed", " elapsed=%.3fs" % elapsed())
        else:
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
        if now() >= deadline:
            break
        time.sleep(max(0.0, min(POLL, deadline - now())))

    # The deadline was reached without an accepted exit sample.  The status
    # below is reported only; it is never accepted as a success.
    status = app.proc.poll()
    if status is None:
        outcome = "original pid %d is still alive" % (app.proc.pid,)
    else:
        status = app.proc.wait()
        app.finish()
        outcome = (
            "original pid %d exited status %d%s only after the 10 s deadline, "
            "which is not accepted as a success"
            % (app.proc.pid, status, signal_note(status))
        )
    ok, why = shutdown_evidence()
    if ok and not observed:
        observed = True
        emit("shutdown-observed", " elapsed=%.3fs" % elapsed())
    if observed:
        raise VariantFailure(
            "quit-timeout",
            "10 s deadline expired; %s; window id %d; shutdown was observed in "
            "the saved config" % (outcome, window),
            "await-shutdown",
        )
    raise VariantFailure(
        "quit-not-observed",
        "10 s deadline expired; %s; window id %d; saved-config shutdown "
        "evidence missing (%s)" % (outcome, window, why),
        "await-shutdown",
    )


def hidden_observation(app, deadline, stage_name, label):
    """One complete hide observation of the recorded window.

    Every place that claims the window is hidden uses this function, so the
    first hide poll, the stability polls and the mandatory final observation
    cannot drift apart.  It performs every check the specification requires:
    xwininfo's exact `Map State:` value, absence from the visible search,
    presence in the all-windows search and `_NET_WM_PID` still equal to the
    original pid.  A window that is destroyed or that no longer belongs to the
    original process raises hide-window-lost directly; any other map state is
    returned, because only the caller knows whether it is a hide still in
    progress or a remap.  A StageDeadline means the observation did not
    complete inside the deadline.
    """
    state = xwininfo_map_state(app.window, deadline, stage_name)
    visible = xdotool_ids(
        app.proc.pid,
        True,
        deadline,
        "xdotool search --all --onlyvisible --pid (%s)" % label,
    )
    every = xdotool_ids(
        app.proc.pid,
        False,
        deadline,
        "xdotool search --all --pid (%s)" % label,
    )
    detail = "map_state=%r visible=%s all=%s" % (state, visible, every)
    if state != "IsUnMapped":
        return state, visible, every, None, detail
    if app.window in visible:
        return state, visible, every, None, detail + " (still listed as visible)"
    if app.window not in every:
        raise VariantFailure(
            "hide-window-lost",
            "window %d is no longer present in xdotool search --all --pid %d; "
            "a destroyed window is not a hide; %s"
            % (app.window, app.proc.pid, detail),
            stage_name,
        )
    kind, value = xprop_pid(app.window, deadline, stage_name)
    if kind == "notfound":
        raise VariantFailure(
            "hide-window-lost",
            "window %d: xprop _NET_WM_PID reports %r, so the window is "
            "destroyed rather than hidden" % (app.window, value),
            stage_name,
        )
    if kind == "badwindow":
        raise VariantFailure(
            "hide-window-lost",
            "window %d: xprop _NET_WM_PID reports BadWindow (%r)"
            % (app.window, value),
            stage_name,
        )
    if value != app.proc.pid:
        raise VariantFailure(
            "hide-window-lost",
            "window %d reports _NET_WM_PID=%d but the original pid is %d"
            % (app.window, value, app.proc.pid),
            stage_name,
        )
    return state, visible, every, value, detail + " pid=%d" % (value,)


def hide_then_restore(app):
    stage("hide")
    wm_alive("hide")
    assert_alive(app, "hide")
    assert_seed("hide")
    send_close(app.window, now() + TOOL_LIMIT, "hide")
    hide_window_end = now() + HIDE_TIMEOUT
    last = "<no poll>"
    hidden = False
    while True:
        st.iteration = now()
        if st.iteration >= hide_window_end:
            break
        assert_alive(app, "hide")
        wm_alive("hide")
        try:
            state, visible, _every, _value, detail = hidden_observation(
                app, hide_window_end, "hide", "hide verification"
            )
            last = detail
            if state == "IsUnMapped" and app.window not in visible:
                hidden = True
        except StageDeadline as exc:
            last += " (deadline: %s)" % (exc.detail,)
        if hidden:
            break
        if st.iteration >= hide_window_end:
            break
        poll_sleep(hide_window_end)
    if not hidden:
        raise VariantFailure(
            "hide-timeout",
            "5 s; window %d did not reach the exact xwininfo 'Map State: "
            "IsUnMapped'; last observation %s" % (app.window, last),
            "hide",
        )

    # Process and seed checks that need no external tool.  They run in every
    # stability iteration and once more immediately before the pass record, so
    # an exit during the interval is still reported as premature-exit, which
    # takes precedence over hide-not-stable.
    def stability_guard():
        status = app.proc.poll()
        if status is not None:
            status = app.proc.wait()
            app.finish()
            raise VariantFailure(
                "premature-exit",
                "original pid %d exited status %d%s during the 1 s hide "
                "interval; this takes precedence over hide-not-stable"
                % (app.proc.pid, status, signal_note(status)),
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

    # Observe window, process and seed through the whole interval, and only
    # then perform the boundary observation.  hide-pass is never emitted from a
    # sampling iteration and never from an observation that starts before the
    # interval has fully elapsed: a fast observation taken 0.1-0.5 s early must
    # not authorise a pass for less than the specified second.
    stage("hide-stability")
    stable_end = now() + HIDE_STABLE
    last = "<no poll>"
    while True:
        st.iteration = now()
        stability_guard()
        if st.iteration >= stable_end:
            break
        wm_alive("hide-stability")
        try:
            state, visible, _every, _value, detail = hidden_observation(
                app, stable_end, "hide-stability", "hide stability"
            )
        except StageDeadline as exc:
            # The interval ended while this observation was running: it is not
            # extended, and the boundary observation below decides the outcome.
            last += " (deadline: %s)" % (exc.detail,)
            break
        last = detail
        if state != "IsUnMapped":
            raise VariantFailure(
                "hide-not-stable",
                "window %d remapped to %r during the 1 s hide interval"
                % (app.window, state),
                "hide-stability",
            )
        if app.window in visible:
            raise VariantFailure(
                "hide-not-stable",
                "window %d reappeared in the visible search %s during the "
                "1 s hide interval" % (app.window, visible),
                "hide-stability",
            )
        target = min(st.iteration + POLL, stable_end)
        delay = target - now()
        if delay > 0.0:
            time.sleep(delay)

    # The boundary observation.  It starts only after the whole interval has
    # elapsed - the polling loop above can only stop at or after stable_end,
    # because its exit condition is a fresh monotonic sample and a
    # StageDeadline can only be raised once that same deadline has passed - and
    # it has its own budget after the interval, so the polling inside the
    # interval can never spend it.  Its completion time is checked as well: an
    # observation that completes before the interval ended is never a pass.
    st.iteration = now()
    stability_guard()
    wm_alive("hide-stability")
    boundary_started = now()
    if boundary_started < stable_end:
        raise VariantFailure(
            "hide-not-stable",
            "the boundary observation of window %d would start %.3fs before "
            "the 1 s hide interval ended; an observation that does not reach "
            "the boundary is not evidence of a stable hide"
            % (app.window, stable_end - boundary_started),
            "hide-stability",
        )
    boundary_deadline = boundary_started + HIDE_BOUNDARY_LIMIT
    try:
        state, visible, every, value, final_detail = hidden_observation(
            app, boundary_deadline, "hide-stability", "hide stability boundary"
        )
    except StageDeadline as exc:
        raise VariantFailure(
            "hide-timeout",
            "the boundary observation of window %d at the end of the 1 s hide "
            "interval did not complete within %.1fs of the interval's end "
            "(%s); last completed observation %s"
            % (app.window, HIDE_BOUNDARY_LIMIT, exc.detail, last),
            "hide-stability",
        )
    boundary_completed = now()
    if boundary_completed < stable_end:
        raise VariantFailure(
            "hide-not-stable",
            "the boundary observation of window %d completed %.3fs before the "
            "1 s hide interval ended; a fast observation that does not reach "
            "the boundary is not evidence of a stable hide"
            % (app.window, stable_end - boundary_completed),
            "hide-stability",
        )
    if state != "IsUnMapped":
        raise VariantFailure(
            "hide-not-stable",
            "the boundary observation at the end of the 1 s hide interval "
            "reports window %d in map state %r" % (app.window, state),
            "hide-stability",
        )
    if app.window in visible:
        raise VariantFailure(
            "hide-not-stable",
            "the boundary observation at the end of the 1 s hide interval "
            "lists window %d in the visible search %s"
            % (app.window, visible),
            "hide-stability",
        )
    if app.window not in every or value != app.proc.pid:
        raise VariantFailure(
            "hide-window-lost",
            "the boundary observation at the end of the 1 s hide interval did "
            "not keep window %d in the all-windows search with "
            "_NET_WM_PID=%d (all=%s pid=%r)"
            % (app.window, app.proc.pid, every, value),
            "hide-stability",
        )
    stability_guard()
    emit(
        "hide-pass",
        " elapsed=%.3fs boundary_started_after_interval_end=%.3fs "
        "boundary_completed_after_interval_end=%.3fs final_observation=%s"
        % (
            elapsed(),
            boundary_started - stable_end,
            boundary_completed - stable_end,
            final_detail,
        ),
    )
    return


def relaunch(app):
    stage("relaunch")
    wm_alive("relaunch")
    assert_alive(app, "relaunch")
    assert_seed("relaunch")
    st.second = App("second")
    window_end = now() + RELAUNCH_TIMEOUT
    last = "<no poll>"
    window = None
    status = None
    while True:
        st.iteration = now()
        status = st.second.proc.poll()
        assert_alive(app, "relaunch")
        wm_alive("relaunch")
        if status is not None:
            # Reap the second instance here and judge its real wait() status: a
            # nonzero or signal status must fail even when the child had
            # already exited before the window was observed.
            status = st.second.proc.wait()
            if status != 0:
                st.second.finish()
                raise VariantFailure(
                    "relaunch-status",
                    "second pid %d exited status %d%s; the second instance must "
                    "exit exactly 0"
                    % (st.second.proc.pid, status, signal_note(status)),
                    "relaunch",
                )
        if status == 0 and st.iteration < window_end:
            try:
                window, last = discover_once(app, window_end, "relaunch")
            except StageDeadline as exc:
                last += " (deadline: %s)" % (exc.detail,)
                break
            if window is not None:
                break
        if st.iteration >= window_end:
            break
        poll_sleep(window_end)
    if window is None:
        raise VariantFailure(
            "relaunch-timeout",
            "5 s; second pid %d status %s; original pid %d alive; the "
            "original's window did not uniquely reappear; last observation "
            "%s"
            % (
                st.second.proc.pid,
                "exited 0"
                if status == 0
                else ("running" if status is None else status),
                app.proc.pid,
                last,
            ),
            "relaunch",
        )

    # The window reappeared, but the exit-0 requirement still holds in every
    # case, including the case where the second instance had already exited
    # when the window was discovered.  Reap it (bounded by the same 5 s window)
    # and require exactly 0 instead of discarding the status.
    while True:
        st.iteration = now()
        status = st.second.proc.poll()
        if status is not None:
            status = st.second.proc.wait()
            break
        if now() >= window_end:
            raise VariantFailure(
                "relaunch-timeout",
                "5 s; second pid %d still running after the window "
                "reappeared; original pid %d alive"
                % (st.second.proc.pid, app.proc.pid),
                "relaunch",
            )
        time.sleep(max(0.0, min(POLL, window_end - now())))
    if status != 0:
        st.second.finish()
        raise VariantFailure(
            "relaunch-status",
            "second pid %d exited status %d%s after the window reappeared"
            % (st.second.proc.pid, status, signal_note(status)),
            "relaunch",
        )
    st.second.finish()
    app.window = window
    st.window = window
    return window


def send_ctrl_q(quit_deadline):
    rc, _out, err = run_tool(
        ["xdotool", "key", "--clearmodifiers", "ctrl+q"],
        tool_budget(quit_deadline),
        "xdotool key --clearmodifiers ctrl+q",
        # The key may legitimately end the process: only a nonzero command
        # status is re-checked against the original's liveness.
        original_guard="on-error",
    )
    if rc == 1:
        raise DriverError(
            "tool",
            "xdotool key --clearmodifiers ctrl+q: the key could not be "
            "delivered (status 1, stderr=%r); this is never a variant failure"
            % (err.strip(),),
            st.stage,
        )
    if rc != 0:
        raise DriverError(
            "tool",
            "xdotool key --clearmodifiers ctrl+q: unexpected status %d "
            "stderr=%r" % (rc, err.strip()),
            st.stage,
        )


def run_barrier(app):
    stage("close")
    wm_alive("close")
    assert_alive(app, "close")
    assert_seed("close")
    quit_deadline = now() + QUIT_TIMEOUT
    send_close(app.window, quit_deadline, "close", original_guard="on-error")
    await_quit(app, app.window, quit_deadline)


def run_no_barrier(app):
    focus_window(app.window, now() + FOCUS_TIMEOUT)
    stage("pre-key")
    wm_alive("pre-key")
    assert_alive(app, "pre-key")
    assert_seed("pre-key")
    quit_deadline = now() + QUIT_TIMEOUT
    send_ctrl_q(quit_deadline)
    await_quit(app, app.window, quit_deadline)


def run_close_to_tray(app):
    hide_then_restore(app)
    window = relaunch(app)
    focus_window(window, now() + FOCUS_TIMEOUT)
    stage("pre-key")
    wm_alive("pre-key")
    assert_alive(app, "pre-key")
    assert_seed("pre-key")
    quit_deadline = now() + QUIT_TIMEOUT
    send_ctrl_q(quit_deadline)
    await_quit(app, window, quit_deadline)


def start_wm():
    stage("wm")
    st.wm_out = os.path.join(st.logdir, "openbox.out")
    st.wm_err = os.path.join(st.logdir, "openbox.err")
    with DeferredSignals():
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
        register_group(st.wm.pid)
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
    """Reap every owned group and child; any problem is a cleanup error.

    On failure every owned group is signalled (TERM, then KILL) whether or not
    its leader still exists: a descendant can outlive the leader that created
    the group.  On success the apps must have been reaped naturally, so their
    groups are only observed; a leak is reported instead of signalled.
    """
    problems = []
    wm_pgid = st.wm.pid if st.wm is not None else None
    if aborted:
        for pgid in sorted(st.owned_groups):
            problem = reap_group(pgid, 2.0)
            if problem:
                problems.append(problem)
    for app in list(st.owned_apps):
        try:
            app.proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            problems.append("owned pid %d did not reap within 5 s" % (app.proc.pid,))
            problem = reap_group(app.proc.pid, 2.0)
            if problem:
                problems.append(problem)
            try:
                app.proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                problems.append(
                    "owned pid %d still did not reap after SIGKILL" % (app.proc.pid,)
                )
        app.finish()
    if st.wm is not None:
        if st.wm.poll() is None or group_members(wm_pgid):
            problem = reap_group(wm_pgid, 2.0)
            if problem:
                problems.append(problem)
        try:
            st.wm.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            problems.append("openbox pid %d did not reap" % (wm_pgid,))
    if not aborted:
        for pgid in sorted(st.owned_groups):
            if pgid == wm_pgid:
                continue
            end = now() + 2.0
            while now() < end and group_members(pgid):
                time.sleep(0.05)
            members = group_members(pgid)
            if members:
                problems.append(
                    "owned process group %d still has live pids=%s after a "
                    "passing run (not signalled on success)"
                    % (pgid, members)
                )
    if problems:
        raise DriverError("cleanup", "; ".join(problems), "cleanup")


def print_logs():
    apps = list(st.owned_apps)
    original_path = (
        st.original.path
        if st.original is not None
        else (apps[0].path if apps else None)
    )
    second_path = (
        st.second.path
        if st.second is not None
        else (apps[1].path if len(apps) > 1 else None)
    )
    entries = (
        ("original application log", original_path),
        ("second application log", second_path),
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


def write_result(verdict, code, detail, stage_name, secondary):
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
        "secondary": secondary,
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
    st.owned_file = os.path.join(st.root, "owned.pids")
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGHUP, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    verdict = "pass"
    code = None
    detail = ""
    secondary = None
    try:
        run()
    except VariantFailure as exc:
        verdict, code, detail = "fail", exc.code, exc.detail
        fail_report(exc.code, exc.detail, exc.stage_name)
    except DriverError as exc:
        verdict, code, detail = "error", exc.code, exc.detail
        error_report(exc.code, exc.detail, exc.stage_name)
    except StageDeadline as exc:
        # Defensive: every stage converts its own deadline, but an exhausted
        # budget must never escape as an unclassified failure.
        verdict, code, detail = "error", "tool", exc.detail
        error_report(code, detail, st.stage)
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
        # Spec section 4: a cleanup error makes the verdict error/exit 2 and
        # the original outcome is retained only as secondary detail.
        secondary = {
            "verdict": verdict,
            "code": code,
            "detail": detail,
            "stage": st.stage,
        }
        verdict, code = "error", "cleanup"
        detail = "%s (secondary: verdict=%s code=%s detail=%s)" % (
            exc.detail,
            secondary["verdict"],
            secondary["code"] or "-",
            secondary["detail"] or "-",
        )
        error_report(code, detail, "cleanup")

    try:
        write_result(verdict, code, detail, st.stage, secondary)
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

  # Check every required command before the first one is used, so a missing
  # readlink reports the missing-dependency diagnostic and not a binary error.
  local -a deps=(
    bash python3 env mktemp mkdir dirname readlink chmod rm sleep
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

  # A genuine monotonic clock is as required as the commands above: every
  # supervisor bound and grace period is measured on it, never on wall-clock
  # time.  It is selected once here, before the first deadline exists.
  if ! select_monotonic_source; then
    supervisor_error dependency \
      "no genuine monotonic clock: /proc/uptime is unreadable and python3 time.monotonic_ns() failed"
    return 2
  fi

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
  DRIVER_PID=$!
  DRIVER_GROUP=$DRIVER_PID
  if [ -n "$INTERRUPT_SIGNAL" ]; then
    # The signal arrived between the fork and the pid assignment above.
    kill -TERM -- "-$DRIVER_PID" 2>/dev/null || true
  fi

  local wrapper_rc=0
  # Wait for the driver tree.  Without a supervisor signal the driver bounds
  # its own work, so this is a plain blocking wait that is restarted only when
  # a trapped signal interrupts it.  As soon as HUP/INT/TERM has arrived the
  # wait is bounded by a real monotonic deadline; expiry escalates inside the
  # process group this supervisor created, and a bounded reaping attempt
  # follows so that cleanup and root removal always run.
  while :; do
    if [ -n "$INTERRUPT_SIGNAL" ]; then
      if wait_driver_bounded "$DRIVER_PID" "$DRIVER_WAIT_LIMIT_US"; then
        wrapper_rc=$WAIT_STATUS
        DRIVER_REAPED=1
      fi
      break
    fi
    WAIT_STATUS=0
    wait "$DRIVER_PID" || WAIT_STATUS=$?
    if [ -z "$(proc_state "$DRIVER_PID")" ]; then
      wrapper_rc=$WAIT_STATUS
      DRIVER_REAPED=1
      break
    fi
    # The wait was interrupted by a trapped signal, or the child is a zombie
    # that still has to be collected: re-check whether a bound now applies.
  done
  if [ "$DRIVER_REAPED" -eq 1 ]; then
    DRIVER_PID=''
  else
    # The bound expired with the driver still alive: escalate within the owned
    # wrapper group only, then make one bounded attempt to collect its status.
    escalate_wrapper_tree \
      "after SIG$INTERRUPT_SIGNAL and a bounded ${DRIVER_WAIT_LIMIT_US}us driver wait expired"
    if wait_driver_bounded "$DRIVER_PID" "$DRIVER_REAP_AFTER_KILL_US"; then
      wrapper_rc=$WAIT_STATUS
      DRIVER_REAPED=1
    else
      DRIVER_UNREAPED=1
      wrapper_rc='unreaped'
    fi
  fi

  # The direct child is reaped (or escalated).  The wrappers stop their own
  # private Xvfb and D-Bus first; only the process group this invocation
  # created is ever addressed.  Give them a bounded moment, then escalate
  # inside that group when nobody escalated it already.
  if [ "$TREE_ESCALATED" -eq 0 ]; then
    local deadline
    now_us || true
    deadline=$(( NOW_US + 1000000 ))
    while [ -n "$(group_members "$DRIVER_GROUP")" ] && before_deadline "$deadline"; do
      sleep 0.02
    done
    if [ -n "$(group_members "$DRIVER_GROUP")" ]; then
      escalate_wrapper_tree "after the wrappers exited"
    else
      TREE_DETAIL='no owned wrapper process survived'
    fi
  fi

  # Belt and braces: an app or WM group can outlive a driver that was killed
  # before its own cleanup ran.  They are separate sessions, so they are only
  # reachable by the group ids the driver recorded.
  local owned_rc=0
  if ! cleanup_owned_pids; then
    owned_rc=1
  fi
  if [ -n "$OWNED_DETAIL" ]; then
    TREE_DETAIL+="; $OWNED_DETAIL"
  fi

  if [ "$TREE_SURVIVORS" -eq 1 ] || [ "$owned_rc" -eq 1 ] || [ "$DRIVER_UNREAPED" -eq 1 ] || [ "$CLOCK_FAILED" -eq 1 ]; then
    local cleanup_problem=''
    if [ "$DRIVER_UNREAPED" -eq 1 ]; then
      cleanup_problem="the driver's direct child did not exit within ${DRIVER_REAP_AFTER_KILL_US}us after the owned wrapper group was escalated to SIGTERM/SIGKILL"
    fi
    if [ "$CLOCK_FAILED" -eq 1 ]; then
      cleanup_problem+="${cleanup_problem:+; }the supervisor's monotonic clock (source=${MONOTONIC_SOURCE:-unset}) could not be read, so a bound was treated as expired instead of measured"
    fi
    if [ "$TREE_SURVIVORS" -eq 1 ]; then
      cleanup_problem+="${cleanup_problem:+; }$TREE_DETAIL"
    fi
    if [ "$owned_rc" -eq 1 ]; then
      cleanup_problem+="${cleanup_problem:+; }an owned app/WM process group outlived its driver"
    fi
    # Any cleanup failure is exit 2 with verdict error; an interrupt that led
    # to it stays visible as detail.
    if [ -n "$INTERRUPT_SIGNAL" ]; then
      supervisor_error cleanup \
        "received SIG$INTERRUPT_SIGNAL wrapper_status=$wrapper_rc $cleanup_problem"
    else
      supervisor_error cleanup "wrapper_status=$wrapper_rc $cleanup_problem"
    fi
    print_wrapper_stdout
    print_failure_logs
    return 2
  fi

  if [ -n "$INTERRUPT_SIGNAL" ]; then
    supervisor_error interrupted \
      "received SIG$INTERRUPT_SIGNAL while the xvfb-run/Xvfb and dbus-run-session tree was running; wrapper_status=$wrapper_rc; $TREE_DETAIL"
    print_wrapper_stdout
    print_failure_logs
    return 2
  fi

  local parsed word1 word2 word3
  parsed=$(read_driver_result) || parsed='READER_ERROR '
  # A signal may have arrived while the result file was being read.  It is
  # honoured before the result is interpreted, with the logs still present.
  if ! interrupt_during_finalization "while the driver result was being read"; then
    return 2
  fi
  read -r word1 word2 word3 <<<"$parsed"
  if [ "${word1:-}" != 'OK' ]; then
    supervisor_error wrapper \
      "wrapper_status=$wrapper_rc driver_result=${parsed:-<empty>} (xvfb-run/Xvfb display and private dbus-run-session tree; no trustworthy token-bound verdict)"
    print_wrapper_stdout
    print_failure_logs
    return 2
  fi

  local verdict=$word2 driver_code=$word3
  VERDICT_NOTE="verdict=$verdict code=$driver_code"
  if [ "$TREE_ESCALATED" -eq 1 ]; then
    printf 'NOTE wrapper tree escalated after exit: %s\n' "$TREE_DETAIL"
  fi
  case "$verdict" in
    pass)
      # The wrapper status is only a cross-check for a passing verdict.
      if [ "$wrapper_rc" -ne 0 ]; then
        supervisor_error wrapper \
          "driver verdict=pass but wrapper_status=$wrapper_rc (xvfb-run/Xvfb and the private dbus-run-session tree did not exit cleanly)"
        print_wrapper_stdout
        print_failure_logs
        return 2
      fi
      print_wrapper_stdout
      # A signal that arrived while the passing result was being reported must
      # not leave exit status 0 behind, and its diagnostics still have the
      # captured logs available here.
      if ! interrupt_during_finalization "while the passing result was being reported"; then
        return 2
      fi
      return 0
      ;;
    fail)
      # The token-bound driver result is authoritative: a fail stays exit 1
      # even if a wrapper's own cleanup changed the wrapper status.
      print_wrapper_stdout
      printf 'NOTE token-bound verdict=fail code=%s wrapper_status=%s\n' \
        "$driver_code" "$wrapper_rc"
      print_failure_logs
      return 1
      ;;
    error)
      print_wrapper_stdout
      printf 'NOTE token-bound verdict=error code=%s wrapper_status=%s\n' \
        "$driver_code" "$wrapper_rc"
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
