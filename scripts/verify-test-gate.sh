#!/bin/sh
# Four independent copies: no active source is mutated, and no baked-in wrapper.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
wrapper=$root/scripts/test-wrapper.sh
command -v rsync >/dev/null
command -v python3 >/dev/null
command -v qmake6 >/dev/null
[ -x "$wrapper" ] || { echo 'FAIL: missing executable wrapper' >&2; exit 1; }

# A failed gate or an unwrapped run fails observation, even with other markers.
observe() {
    python3 - "$1" "$2" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
lines = p.read_text().splitlines() if p.is_file() else []
base = '"-f" "Makefile.tests"'
build = any(base in line and '"check"' not in line for line in lines)
run = any(base + ' "check"' in line for line in lines)
for line in lines:
    print('wrapped argv: ' + line, flush=True)
print(f'markers: build={int(build)} run={int(run)}', flush=True)
sys.exit(0 if int(sys.argv[2]) == 0 and build and run else 1)
PY
}

probe() (
    name=$1
    copy=$(mktemp -d "/tmp/gd-gate.$name.XXXXXXXX") || exit 1
    trap 'rm -rf -- "$copy"' 0
    trap 'exit 130' INT
    trap 'exit 143' TERM HUP
    # Retain reusable objects, but NEVER generated Makefiles with source paths
    # or wrappers from another tree. Reconfigure every per-suite Makefile too.
    rsync -a --exclude='.git' --exclude='.superpowers' --exclude='.codegraph' \
        --exclude='Makefile*' --exclude='.qmake.stash' "$root/" "$copy/" || exit 1
    cd "$copy" || exit 1
    unset GD_TEST_WRAPPER GD_TEST_WRAPPER_LOG GD_TEST_FORCE_FAILURE GD_TEST_FORCE_ENV_UNAVAILABLE
    if [ "$name" = P1 ]; then
        python3 - <<'PY' || exit 1
from pathlib import Path
p = Path('tests/tst_utf8_wstring/tst_utf8_wstring.cc')
s = p.read_text()
needle = 'void TstUtf8Wstring::initTestCase()\n{'
assert s.count(needle) == 1
p.write_text(s.replace(needle, needle + '\n    QVERIFY2(qEnvironmentVariable("GD_TEST_FORCE_FAILURE") != "1", "T35 intentional binary assertion");'))
PY
    elif [ "$name" = P4 ]; then
        python3 - <<'PY' || exit 1
from pathlib import Path
p = Path('goldendict.pro')
s = p.read_text()
a = 'tests-run.commands = $(GD_TEST_WRAPPER) $(MAKE)'
b = 'tests-run.commands = $(MAKE)'
assert s.count(a) == 1
p.write_text(s.replace(a, b))
assert p.read_text().replace(b, a) == s, 'negative control changed more than the run prefix'
PY
    fi
    echo "$name command: (unset GD_TEST_WRAPPER; qmake6 goldendict.pro CONFIG+=verbose_build_output)"
    if ! qmake6 goldendict.pro CONFIG+=verbose_build_output > configure.log 2>&1; then
        tail -n 30 configure.log
        echo "$name FAIL: root configuration failed before probe"
        exit 1
    fi
    export QT_QPA_PLATFORM=xcb
    export GD_TEST_WRAPPER="$wrapper" GD_TEST_WRAPPER_LOG="$copy/wrapper.log"
    case $name in
        P1) export GD_TEST_FORCE_FAILURE=1 ;;
        P2) export GD_TEST_FORCE_ENV_UNAVAILABLE=1 ;;
    esac
    echo "$name command: QT_QPA_PLATFORM=$QT_QPA_PLATFORM GD_TEST_WRAPPER=$wrapper GD_TEST_WRAPPER_LOG=$copy/wrapper.log xvfb-run -a timeout 1800 make check (recursive argv: -f Makefile.tests check)"
    set +e
    # The outer display keeps P4's unwrapped run environment identical. Only
    # test-wrapper writes markers, never this environment provisioner.
    xvfb-run -a timeout 1800 make check > gate.log 2>&1
    status=$?
    observe "$copy/wrapper.log" "$status" > markers.log
    observation=$?
    set -e
    printf '%s exit=%s observation=%s ' "$name" "$status" "$observation"
    while IFS= read -r line; do printf '%s\n' "$line"; done < markers.log
    case $name in
        P1)
            [ "$status" -eq 2 ] &&
            grep -E 'FAIL!.*TstUtf8Wstring::initTestCase' gate.log >/dev/null &&
            grep -F 'T35 intentional binary assertion' gate.log >/dev/null &&
            grep -Fx 'markers: build=1 run=1' markers.log >/dev/null &&
            [ "$observation" -eq 1 ] || {
                tail -n 45 gate.log; echo 'P1 FAIL: not the executed intentional assertion'; exit 1;
            } ;;
        P2)
            [ "$status" -ne 0 ] && [ "$status" -ne 124 ] &&
            grep -E 'FAIL!.*::initTestCase' gate.log >/dev/null &&
            grep -F 'requiredEnvironment' gate.log >/dev/null &&
            grep -F 'GD_TEST_FORCE_ENV_UNAVAILABLE=1' gate.log >/dev/null &&
            ! grep -E 'SKIP.*requiredEnvironment' gate.log >/dev/null &&
            grep -Fx 'markers: build=1 run=1' markers.log >/dev/null &&
            [ "$observation" -eq 1 ] || {
                tail -n 45 gate.log; echo 'P2 FAIL: not a RequiredEnvironment assertion'; exit 1;
            } ;;
        P3)
            [ "$status" -eq 0 ] && [ "$observation" -eq 0 ] || {
                tail -n 45 gate.log; echo 'P3 FAIL: clean wrapped gate must pass with BOTH markers'; exit 1;
            } ;;
        P4)
            # shellcheck disable=SC2015
            [ "$status" -eq 0 ] && [ "$observation" -ne 0 ] &&
            grep -Fx 'markers: build=1 run=0' markers.log >/dev/null || {
                tail -n 45 gate.log; echo 'P4 FAIL: negative control did not isolate the missing run wrapper'; exit 1;
            } ;;
    esac
    echo "$name PASS"
)

failed=0
for name in P1 P2 P3 P4; do
    probe "$name" || failed=1
done
[ "$failed" -eq 0 ] || { echo 'test gate verifier: FAIL'; exit 1; }
echo 'test gate verifier: PASS (P3 is the unmodified clean wrapped root check)'
