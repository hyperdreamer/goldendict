/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Shared test support: environment gates, the process wall watchdog and the
 * fixture lookup every GoldenDict test binary uses.
 *
 * Test-only code. It is built into the test binaries and into the test-only
 * compile closure (tests/tests.pro GD_COMPILE_CLOSURE=1); it is never part of
 * the shipped application and never available to production sources.
 *
 * The environment gate exists so an unavailable environment fails a *required*
 * test with a diagnostic reason through QVERIFY2 instead of silently skipping:
 *
 *     void initTestCase()
 *     {
 *         QStringList reasons;
 *         QVERIFY2( TestSupport::requiredEnvironment( &reasons ),
 *                   qPrintable( reasons.join( '\n' ) ) );
 *     }
 *
 * Requirements are selected per suite through the environment, which keeps the
 * binaries free of production-feature switches:
 *   GD_TEST_FORCE_ENV_UNAVAILABLE=1  force the gate to report an unavailable
 *                                    environment (harness falsification)
 *   GD_TEST_REQUIRED_FIXTURES=a:b    fixture files that must exist under the
 *                                    fixture root, ':'-separated
 *   GD_TEST_REQUIRED_PREREQUISITES=x:y
 *                                    suite-selected prerequisite tokens,
 *                                    ':'-separated (see prerequisiteAvailable())
 *   GD_TEST_FIXTURES=<dir>           fixture root override
 *   GD_TEST_REPO_ROOT=<dir>          repository root override
 */

#ifndef GOLDENDICT_TESTS_SUPPORT_FIXTURES_HH_INCLUDED
#define GOLDENDICT_TESTS_SUPPORT_FIXTURES_HH_INCLUDED

#include <QString>
#include <QStringList>

namespace TestSupport {

/// Wall-clock budget of a pure, GUI-less unit-test binary.
constexpr int PureWatchdogSeconds = 60;

/// Wall-clock budget of a binary that drives Chromium (browser/article).
constexpr int BrowserWatchdogSeconds = 120;

/// Wall-clock budget of the aggregate gate command.
constexpr int AggregateWatchdogSeconds = 900;

/// Exit code a watchdog expiry reports; the convention GNU timeout uses, so a
/// harness can tell a wedged binary from an ordinary assertion failure.
constexpr int WatchdogExitCode = 124;

/// Wall-clock budget the watchdog expiry probe process arms. The parent-side
/// verification requires the probe to die after (about) this long.
constexpr int WatchdogProbeSeconds = 1;

/// Returns false and appends one human-readable reason per unmet requirement.
/// `reasons` may be null when only the verdict is needed. The result is derived
/// from the environment on every call, so a suite can falsify its own gate.
bool requiredEnvironment( QStringList * reasons );

/// Arms a wall watchdog with the given budget. A helper thread terminates the
/// process with WatchdogExitCode when the budget expires, so a binary whose
/// event loop is wedged still dies. Arming twice replaces the previous
/// watchdog; a budget of zero or less only disarms.
void installWatchdog( int seconds );

/// Disarms a previously armed watchdog. Idempotent and safe to call from
/// cleanupTestCase() even when installWatchdog() was never called.
void clearWatchdog();

/// True when this process was started as the subprocess side of the watchdog
/// expiry test. Call it from main() before constructing the QCoreApplication.
bool isWatchdogProbeProcess();

/// Subprocess side of the watchdog expiry test: makes stderr hostile the way a
/// wedged suite does (a held stdio lock or a full, unread pipe), arms
/// WatchdogProbeSeconds and then stalls forever. Never returns: the watchdog is
/// expected to terminate the process.
[[noreturn]] void runWatchdogProbe();

/// Parent side of the watchdog expiry test: runs `binaryPath` in every probe
/// mode and verifies that it is terminated with WatchdogExitCode, after at
/// least WatchdogProbeSeconds and before the deadline. Appends one reason per
/// broken mode; `reasons` may be null.
bool verifyWatchdogExpiry( QString const & binaryPath, QStringList * reasons );

/// Repository root: GD_TEST_REPO_ROOT when set, otherwise the nearest ancestor
/// of the binary or the working directory that contains goldendict.pro, and
/// finally the source root recorded at build time. Empty when unresolvable.
QString repositoryRoot();

/// Fixture root: GD_TEST_FIXTURES when set, otherwise <repositoryRoot>/tests/fixtures.
QString fixtureRoot();

/// Absolute path of `relativePath` below the fixture root. An empty fixture root
/// yields the untouched relative path so callers can still report it.
QString fixturePath( QString const & relativePath );

/// True when `relativePath` resolves to a readable regular file.
bool fixtureExists( QString const & relativePath );

/// Feature defines the shared project mirrored from goldendict.pro that this
/// binary was actually compiled with, one token per entry. A suite can therefore
/// assert that a guarded production implementation was configured in instead of
/// guessing from the source tree.
QStringList compiledFeatureDefines();

/// Evaluates one suite-selected prerequisite token:
///   "display"    a display server (X11 or Wayland) is actually connectable
///   "x11"        an X11 DISPLAY is connectable and the QPA is not forced elsewhere
///   "wayland"    a Wayland compositor socket is connectable
///   "webengine"  the QtWebEngineProcess helper of this Qt installation exists
/// Unknown tokens are reported as unavailable: a typo must not pass a required
/// suite. `reason` may be null.
bool prerequisiteAvailable( QString const & token, QString * reason );

/// Bounded reachability probe of one X11 DISPLAY value
/// ("[protocol/][host]:display[.screen]"). A local display is probed through the
/// X11 unix socket, a remote one through TCP port 6000+display. The connect is
/// time-bounded, so an unreachable or half-open endpoint can never wedge the
/// environment gate. `reason` may be null.
bool x11DisplayReachable( QString const & display, QString * reason );

/// Bounded reachability probe of one WAYLAND_DISPLAY value, resolved against
/// XDG_RUNTIME_DIR unless it is already a path. `reason` may be null.
bool waylandDisplayReachable( QString const & waylandDisplay, QString * reason );

} // namespace TestSupport

#endif // GOLDENDICT_TESTS_SUPPORT_FIXTURES_HH_INCLUDED
