#!/usr/bin/env bash
#
# build.sh - out-of-tree build helper for GoldenDict (qmake project).
#
# Deliberately leaves RESERVE logical CPUs idle so the machine stays
# responsive while it compiles. Override the job count with -j N / JOBS=N.
#
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PRO_FILE="$REPO_ROOT/goldendict.pro"

NCORES="$(nproc)"
RESERVE="${RESERVE:-4}"               # logical CPUs to keep free for other work
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
JOBS="${JOBS:-}"
QMAKE="${QMAKE:-qmake}"
LOAD_MAX="${LOAD_MAX:-}"              # optional `make -l` load ceiling
NICE="${NICE:-}"                      # optional `nice -n` level
RECONFIGURE=0
FRESH=0
QMAKE_ARGS=()

die() { printf 'build.sh: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: ./build.sh [options] [-- qmake-args...]

Options:
  -j N, -jN, --jobs=N   parallelism (default: nproc - RESERVE)
  --qt6                 build against Qt6 (uses qmake6)
  --qmake PATH          explicit qmake binary
  --build-dir DIR       build directory (default: ./build)
  --reconfigure         re-run qmake even if the Makefile looks current
  --fresh               delete the build directory first
  -h, --help            show this message

Environment:
  JOBS         same as -j
  RESERVE      cores to leave idle (default 4)
  BUILD_DIR    build directory
  QMAKE        qmake binary
  LOAD_MAX     pass `make --max-load=LOAD_MAX`
  NICE         pass `nice -n NICE` to make
  MAKE_ARGS    extra make arguments, e.g. MAKE_ARGS="clean"

Examples:
  ./build.sh                          # incremental, nproc-4 jobs
  ./build.sh -j12                     # exactly 12 jobs
  ./build.sh --qt6 --fresh            # clean Qt6 build
  ./build.sh -- CONFIG+=zim_support   # extra qmake flags
EOF
  printf '\nCurrent defaults: RESERVE=%s  BUILD_DIR=%s  QMAKE=%s\n' \
    "$RESERVE" "$BUILD_DIR" "$QMAKE"
}

while (( $# )); do
  case "$1" in
    -j)            [[ $# -ge 2 ]] || die "-j needs a value"; JOBS="$2"; shift 2 ;;
    -j*)           JOBS="${1#-j}"; shift ;;
    --jobs=*)      JOBS="${1#*=}"; shift ;;
    --qt6)         QMAKE=qmake6; shift ;;
    --qmake)       [[ $# -ge 2 ]] || die "--qmake needs a value"; QMAKE="$2"; shift 2 ;;
    --build-dir)   [[ $# -ge 2 ]] || die "--build-dir needs a value"; BUILD_DIR="$2"; shift 2 ;;
    --reconfigure) RECONFIGURE=1; shift ;;
    --fresh)       FRESH=1; shift ;;
    -h|--help)     usage; exit 0 ;;
    --)            shift; QMAKE_ARGS=("$@"); break ;;
    *)             die "unknown option: $1 (try --help)" ;;
  esac
done

# --- resolve parallelism -----------------------------------------------------
if [[ -z "$JOBS" ]]; then
  if (( NCORES > RESERVE )); then
    JOBS=$(( NCORES - RESERVE ))
  else
    JOBS=1
  fi
fi

[[ "$JOBS" =~ ^[0-9]+$ ]] || die "invalid job count: '$JOBS'"
(( JOBS > 0 )) || die "job count must be >= 1"
if (( JOBS > NCORES )); then
  printf 'build.sh: warning: -j%s exceeds the %s logical CPUs available\n' \
    "$JOBS" "$NCORES" >&2
fi

# --- locate qmake ------------------------------------------------------------
[[ -f "$PRO_FILE" ]] || die "cannot find $PRO_FILE"
command -v "$QMAKE" >/dev/null 2>&1 || die "'$QMAKE' not found in PATH (try --qmake or --qt6)"
QT_VERSION="$("$QMAKE" -query QT_VERSION 2>/dev/null || echo unknown)"

# --- prepare build dir -------------------------------------------------------
if (( FRESH )); then
  printf '==> removing %s\n' "$BUILD_DIR"
  rm -rf -- "$BUILD_DIR"
fi
mkdir -p -- "$BUILD_DIR"

# --- decide whether qmake needs to run ---------------------------------------
need_qmake=0
(( RECONFIGURE )) && need_qmake=1
(( ${#QMAKE_ARGS[@]} )) && need_qmake=1
[[ -f "$BUILD_DIR/Makefile" ]] || need_qmake=1
[[ -f "$BUILD_DIR/Makefile" && "$PRO_FILE" -nt "$BUILD_DIR/Makefile" ]] && need_qmake=1

if (( need_qmake )); then
  printf '==> qmake %s (Qt %s) -> %s\n' "$QMAKE" "$QT_VERSION" "$BUILD_DIR"
  ( cd -- "$BUILD_DIR" && "$QMAKE" "$PRO_FILE" "${QMAKE_ARGS[@]}" )
else
  printf '==> qmake: reusing %s/Makefile (use --reconfigure to regenerate)\n' "$BUILD_DIR"
fi

# --- build -------------------------------------------------------------------
make_cmd=(make -C "$BUILD_DIR" -j"$JOBS")
[[ -n "$LOAD_MAX" ]] && make_cmd+=("--max-load=$LOAD_MAX")
[[ -n "$NICE" ]] && make_cmd=(nice -n "$NICE" "${make_cmd[@]}")

# shellcheck disable=SC2206  # MAKE_ARGS is intentionally word-split
[[ -n "${MAKE_ARGS:-}" ]] && make_cmd+=(${MAKE_ARGS})

printf '==> make -j%s  (%s logical CPUs, %s kept free)\n' "$JOBS" "$NCORES" "$RESERVE"
"${make_cmd[@]}"

if [[ -x "$BUILD_DIR/goldendict" ]]; then
  printf '==> binary: %s\n' "$BUILD_DIR/goldendict"
fi
