# Global Hotkeys Portal Consent Wait Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the deterministic
> subagent-driven-development controller to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Wayland global-shortcut consent waits survive beyond the establishment deadline, report activity truthfully, permit explicit retry, and expose the existing Configure shortcuts repair route.

**Architecture:** Keep portal request establishment bounded while making an established BindShortcuts request wait for portal consent, then centralize active-ID derivation from non-empty `trigger_description` values. Put retry memory and the ordered retry decision in `HotkeyInstallGate`, and expose Configure availability as a live backend capability shared by the backend operation, MainWindow, and Preferences. Extend the private-bus fake portal and the two existing test suites without changing either `.pro` file, then apply exactly seven migration-spec amendments. **Ambiguities:** none; the approved specification resolves suite ownership, fixture ordering, retry intent, capability transitions, mutation evidence, and all seven amendment targets.

**Tech Stack:** C++17, Qt 6, QtDBus, QtTest, private D-Bus fixtures, qmake6, and the existing GoldenDict Wayland portal backend.

## Global Constraints

- The implementation may modify exactly these paths: `portalhotkeys.hh`, `portalhotkeys.cc`, `hotkeybackend.hh`, `hotkeybackend.cc`, `mainwindow.hh`, `mainwindow.cc`, `preferences.cc`, `tests/support/fake_portal.hh`, `tests/support/fake_portal.cc`, `tests/tst_portal_hotkeys/tst_portal_hotkeys.cc`, `tests/tst_hotkeys/tst_hotkeys.cc`, and `docs/superpowers/specs/2026-10-24-qt6-migration-spec.md`, limited to the seven amendments in the approved specification.
- No other implementation, test, build, packaging, generated, or documentation path may change; neither `tests/tests.pro` nor any other `.pro` file changes.
- Preserve the existing D-Bus names, object paths, interfaces, request payload shapes, persistent configuration, display policy, X11 `RECORD` path, `UnsupportedBackend`, `PortalBindingDisplay` persistence, and all existing request-path, owner-capture, generation, parsing, cancellation, and cleanup guards unless a task states an explicit change.
- `DeadlineMs = 4900` and `CleanupMs = 5000` are unchanged.
- There is exactly one `BindShortcuts` per portal session; a retry uses a fresh session and never issues a second bind on one session.
- The exact D5 ASCII explanation is `Binding portal shortcuts: waiting for the portal confirmation dialog`.
- The exact D4 inactive label is `Not assigned by the portal — use Configure shortcuts`; the U+2014 EM DASH is the only non-ASCII character introduced into the source strings by this change.
- The nine `HotkeyBackend::State` values are `Unsupported`, `Creating`, `Ready`, `Binding`, `Bound`, `Partial`, `Denied`, `Unavailable`, and `Closed`; `hotkeyBindingSettles()` is true only for `Bound`, `Partial`, and `Unsupported`, and `hotkeyBindingInFlight()` is true only for `Creating` and `Binding`.
- The explicit retry gesture is an accepted Preferences dialog. Only that call passes `HotkeyInstallIntent::UserAction`; startup restore, dictionary editing, rescan, always-on-top changes, and cancelled Preferences use `Refresh`.
- The closed 19-suite inventory remains unchanged, and no tests beyond the eighteen named tests in the specification may be invented. `tst_portal_hotkeys` owns tests 1-9 and 18; `tst_hotkeys` owns tests 10-17.
- The aggregate gate is exactly `qmake6 tests/tests.pro -o tests/Makefile && make -C tests -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`.
- Every mutation probe must run in a disposable copy, assert the named killer fails, restore the source immediately, and rerun the affected test green; the equivalent mutation documented in Task 1 is not claimable as killed.
- The final worktree must pass `git diff --check` and contain no generated binaries, Makefiles, or test artifacts.
- No host-transition command may be executed; planning and implementation must not reboot, stop the display manager, or terminate the PM host session.

## Task 1: Implement bounded establishment and consent-wait request lifecycle

**Implementer tier:** Advanced
**Lane:** portal-request

**Files:**
- Modify: `portalhotkeys.hh:27-54`
- Modify: `portalhotkeys.cc:36-54,172-499`
- Modify: `tests/support/fake_portal.hh:34-171`
- Modify: `tests/support/fake_portal.cc:100-175,210-329,430-645,1142-1170`
- Modify: `tests/tst_portal_hotkeys/tst_portal_hotkeys.cc:176-250,300-390,1400-1754`
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:392-450,2050-2378`

**Interfaces:**
- Consumes: the existing `PortalRequest(QDBusConnection const &, quint64, QObject *)`, `PortalRequestState::start()`, `cancel()`, `expired()`, `methodFinished()`, `deliver()`, and `receiveResponse()` lifecycle at `portalhotkeys.cc:172-499`.
- Produces: `enum class PortalResponsePolicy { MachineAnswered, UserConsent }` beside `PortalRequest`; `PortalRequest(QDBusConnection const &, quint64, QObject * = nullptr, PortalResponsePolicy = PortalResponsePolicy::MachineAnswered)` with policy last; and `startBind()` as the sole caller that passes `PortalResponsePolicy::UserConsent`.
- Produces: fake-portal methods `void holdResponse(QString const & member)`, `void releaseHeldResponse(QString const & member)`, `void holdMethodReply(QString const & member)`, and `void releaseHeldMethodReply(QString const & member)`, while preserving `void releasePending()` as the unchanged whole-scenario flush.

### Steps

- [ ] **Step 1: Preserve the existing request contract and add the policy type.** Add the two policy enumerators and append the defaulted policy parameter after `parent` in `portalhotkeys.hh:40-45`. Store the policy and `established = false` in `PortalRequestState`; leave all existing two- and three-argument constructions source-compatible, including `createSession()`, `startList()`, `startBind()`, and direct test construction.
- [ ] **Step 2: Implement the two-phase timer rule.** Keep timer start and `elapsed` start in `PortalRequestState::start()`. In `expired()`, retain the retired-first behavior, return without expiring when the policy is `UserConsent` and `established` is true, and otherwise retain the `Portal request deadline exceeded` failure at 4900 ms. In `methodFinished()`, keep `expired()` first, validate and record `returnedPath` exactly as before, then set `established = true` and stop the timer only while the request is live and its policy is `UserConsent`. Do not stop timers for `MachineAnswered` requests.
- [ ] **Step 3: Keep all late-delivery and cleanup guards intact.** Do not clear `candidates` in `cancel()`. Keep `Request.Close`, `CleanupMs`, `dispose()` clearing, owner-change clearing, predicted/returned path matching, `MaxCandidates`, captured-owner invocation, retired-generation suppression, and the retired `Session.Close` branch in `deliver()`. Ensure the established consent request remains `Binding` after its 4.9-second establishment budget and ends only on Response, Session.Closed, owner loss, generation retirement, `unbind()`, or destruction. The existing machine-answered requests still fail if their Response does not arrive within the whole-lifetime deadline.
- [ ] **Step 4: Pass the policy only from BindShortcuts.** Construct `PortalRequest` with `PortalResponsePolicy::UserConsent` in `startBind()` at `portalhotkeys.cc:963-1021`; leave `CreateSession` at `portalhotkeys.cc:883-917` and `ListShortcuts` at `portalhotkeys.cc:919-954` on the default `MachineAnswered` policy. Keep the one-bind-per-session guard and all response/error routing unchanged.
- [ ] **Step 5: Add per-method fake barriers without weakening the existing barrier.** Extend the fake portal state and public methods around `tests/support/fake_portal.hh:158-169` so response barriers hold only a named method's Response after its method reply, and method-reply barriers hold only that method reply. An early Response remains observable before a held method reply; releases preserve per-request arrival order; another method's reply, Response, and predicted-path plumbing are never held. Implement `releasePending()` exactly as the existing flush of all deferred items. Keep all existing scenarios and add no unrelated scenario behavior.
- [ ] **Step 6: Add the exact request-lifecycle tests.** Add the exact test names `bindConsentResponseWaitsBeyondTheEstablishmentDeadline`, `machineAnsweredResponseStillBoundedAfterMethodReply`, `consentEstablishmentStillBounded`, `createNoMethodReplyClosesOnlyARequestThatNeverLearnedASession`, `existingBindTimeoutRowNoLongerPassesThroughAListTimeout`, `bindDeniedThenRetryUsesNewSession`, and `consentWaitTerminals` to `tst_portal_hotkeys`. Test 1 holds only Bind's Response, waits 5.2 seconds, asserts `Binding`, no failure, no `Request.Close`, and no `Session.Close`, releases it, and asserts `Bound`. Test 2 holds only List's Response and asserts the machine request fails at the existing deadline. Test 3 holds Bind's method reply with a live session and asserts failure before five seconds, one `Request.Close`, and one `Session.Close`. Test 4 uses `no-method-reply` and asserts one `Request.Close` and zero `Session.Close`. Test 5 holds only Bind after List completes and asserts `Binding`, never `Unavailable`. Test 8 denies the first bind, calls `PortalBackend::bind()` explicitly with the same desired list after switching the fake to success, and asserts two sessions, exactly one BindShortcuts per session, no protocol violation, and final `Bound`. Test 9 covers unbind, destruction, owner loss, Session.Closed, and suppression of a late success Response after retirement. Add the exact cross-suite lifecycle test `lateCreateSessionSuccessClosesItsSessionAfterCancellation` to `tst_hotkeys`: stage the early CreateSession success envelope with its method reply held, drain it using the existing `flushWire()`/marker pattern at `tests/tst_portal_hotkeys/tst_portal_hotkeys.cc:310-324`, call `unbind()`, release the method reply, and assert exactly one late `Session.Close` and no state resurrection.
- [ ] **Step 7: Run the request-owned mutation probes.** In a disposable copy, delete only the `UserConsent && established` early return in `expired()` and run test 1; restore immediately and rerun test 1 green. Change `timer.stop()` to apply to every policy and run test 2; restore immediately and rerun test 2. Insert `candidates.clear()` into `cancel()` and run test 17; restore immediately and rerun test 17. Remove only the retired-branch `close(session, SessionInterface)` in `deliver()` and run test 17; restore immediately and rerun test 17. Record the deletion of `!retired` from the consent-only `timer.stop()` guard as the specified equivalent mutation, not as a killed mutation: the retired Bind request still reaches disposal and the change has no observable lifetime effect.
- [ ] **Step 8: Run the affected suite and commit.** Run `qmake6 tests/tests.pro -o tests/Makefile && make -C tests -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`, confirming the request lifecycle tests and the existing suite pass. Commit with `git add portalhotkeys.hh portalhotkeys.cc tests/support/fake_portal.hh tests/support/fake_portal.cc tests/tst_portal_hotkeys/tst_portal_hotkeys.cc tests/tst_hotkeys/tst_hotkeys.cc && git commit -m "fix(portal): wait for consent after request establishment"`.

## Task 2: Centralize portal activity and truthful binding display

**Implementer tier:** Advanced
**Lane:** portal-activity

**Files:**
- Modify: `portalhotkeys.cc:131-149,524-558,1030-1143,1145-1232`
- Modify: `tests/support/fake_portal.hh:35-90`
- Modify: `tests/support/fake_portal.cc:289-323`
- Modify: `tests/tst_portal_hotkeys/tst_portal_hotkeys.cc:176-250,1420-1608`

**Interfaces:**
- Consumes: `PortalWire::Shortcuts`, `PortalWire::Shortcut`, `parseShortcuts()`, `displayEntries()`, `PortalBackend::handleBindResponse()`, and the validated `ShortcutsChanged` branch at `portalhotkeys.cc:1182-1232`.
- Produces: `QSet<QString> activeIdsFromReply(PortalWire::Shortcuts const & entries, QSet<QString> const & requested)`; `PortalBackend::Private::addressedIds`; and one shared activity/classification rule used by BindShortcuts and ShortcutsChanged.
- Consumes: `FakePortal::emitShortcutsChanged(QString const &, QVector<Shortcut> const &)` at `tests/support/fake_portal.hh:165`; produces the `bind-returning-empty-trigger` scenario and default Bind payloads containing a non-empty `trigger_description` without rewriting `boundEntries`.

### Steps

- [ ] **Step 1: Add the shared activity helper and separate addressed from active IDs.** Implement `activeIdsFromReply()` near `displayEntries()` in `portalhotkeys.cc:131-149`. After existing wire validation, include only requested IDs whose reply entry has a string `trigger_description` with non-zero length; do not parse it or compare it with `preferred_trigger`. Add `addressedIds` to `PortalBackend::Private` and retain `activeIds` separately.
- [ ] **Step 2: Apply the same rule on both reply paths.** In `handleBindResponse()` at `portalhotkeys.cc:1081-1143` and the `ShortcutsChanged` branch at `portalhotkeys.cc:1182-1232`, preserve duplicate-ID, unknown-ID, and property-type validation, merge the validated records, set `addressedIds` to the IDs named by the reply, set `activeIds = activeIdsFromReply(entries, requestedIds)`, and classify `Bound` when all requested IDs are active, `Partial` when at least one but not all is active, and `Denied` when none is active. Keep `bindCompleted` and current-session gating intact. A historical ListShortcuts result may update display records but never replaces the active subset, activates an ID, or tears down a session because an old ID is absent.
- [ ] **Step 3: Remove the false display substitution.** In `displayEntries()` remove the `Assigned by portal` fallback. A missing or empty `trigger_description` must remain an empty display trigger for Preferences to render as unassigned; a non-empty reported trigger remains display-only and is not fed into BindShortcuts input.
- [ ] **Step 4: Extend the fake payloads for the activity contract.** Update `bindPayload()` at `tests/support/fake_portal.cc:289-323` so normal returned entries receive a non-empty `trigger_description` equal to the requested `preferred_trigger` unless the entry already supplies one or a scenario supplies another value. Keep `boundEntries` verbatim. Add `bind-returning-empty-trigger`, which returns the requested IDs with an empty `trigger_description`; keep `emitShortcutsChanged()` as the existing correctly marshaled emitter and do not add a second emitter.
- [ ] **Step 5: Add the exact activity tests and update the existing expectation.** Add `declinedSavedIdReportsUnassignedNotBound` using `bind-returning-empty-trigger`; assert `Denied`, empty `activeIds`, the unassigned label, and absence of `Assigned by portal`. Add `shortcutsChangedWithEmptyTriggerReportsUnassigned`; emit a requested ID with an empty trigger and assert the state leaves `Bound` and its label is unassigned. Update the existing `reportedTriggersAreDisplayOnly` at `tests/tst_portal_hotkeys/tst_portal_hotkeys.cc:1585-1598` to expect `Partial` and an empty/unassigned second label while keeping `boundEntries` verbatim and preserving the preferred-trigger assertions. Do not add another test name.
- [ ] **Step 6: Run the activity-owned mutation probes.** In disposable copies, replace Bind classification with ID-presence classification and run `declinedSavedIdReportsUnassignedNotBound`; restore immediately and rerun it. Replace ShortcutsChanged classification with ID-presence classification and run `shortcutsChangedWithEmptyTriggerReportsUnassigned`; restore immediately and rerun it. Restore the `Assigned by portal` substitution and run `declinedSavedIdReportsUnassignedNotBound`; restore immediately and rerun it.
- [ ] **Step 7: Run the affected suite and commit.** Run `qmake6 tests/tests.pro -o tests/Makefile && make -C tests -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`, confirming both activity reply paths and the updated existing test pass. Commit with `git add portalhotkeys.cc tests/support/fake_portal.hh tests/support/fake_portal.cc tests/tst_portal_hotkeys/tst_portal_hotkeys.cc && git commit -m "fix(portal): derive binding activity from reported triggers"`.

## Task 3: Add ordered retry decisions and install memory

**Implementer tier:** Capable
**Lane:** install-gate

**Files:**
- Modify: `hotkeybackend.hh:32-104`
- Modify: `hotkeybackend.cc:1-37`
- Modify: `mainwindow.hh:180-191,260-268`
- Modify: `mainwindow.cc:864,1031,1270,1314,2126,2187,2338,3195-3288,3713,3740,3771`
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:392-450,2050-2378`

**Interfaces:**
- Consumes: `HotkeyBackend::State`, `HotkeyBinding`, the current equality-only `requestedHotkeys` early return at `mainwindow.cc:3215-3222`, and the five reset sites at `mainwindow.cc:1031,1270,1314,2126,3713`.
- Produces: `enum class HotkeyInstallIntent { Refresh, UserAction }`; `bool hotkeyInstallShouldBind(bool backendPresent, bool unchangedSet, HotkeyBackend::State state, HotkeyInstallIntent intent) noexcept`; `bool hotkeyBindingSettles(HotkeyBackend::State) noexcept`; `bool hotkeyBindingInFlight(HotkeyBackend::State) noexcept`; and `HotkeyInstallGate::shouldBind(bool, QVector<HotkeyBinding> const &, HotkeyBackend::State, HotkeyInstallIntent)` plus `reset()`.
- Produces: `MainWindow::installHotKeys(HotkeyInstallIntent intent = HotkeyInstallIntent::Refresh)` with no `requestedHotkeys` member and no duplicate equality-only decision; only the accepted Preferences path at `mainwindow.cc:2338` can pass `UserAction`.

### Steps

- [ ] **Step 1: Implement the total state classifiers and ordered decision table.** In `hotkeybackend.hh/.cc`, define the nine-state partition and implement the first-match rules exactly: no backend -> true; `Unsupported` -> false absolutely; in-flight unchanged -> false; in-flight changed -> true; changed set -> true; settled state -> false; `UserAction` -> true; remaining Refresh -> false. The changed-plus-`Creating`, changed-plus-`Binding`, changed-plus-`Unsupported`, changed-plus-`Bound`, unchanged Refresh, unchanged UserAction, and no-backend intersections must be explicit in code or table-driven logic.
- [ ] **Step 2: Implement `HotkeyInstallGate` as the sole comparison memory.** Store `lastDesired` and `primed`; compare desired bindings with the last list; apply the value-level decision; record the desired list only when `shouldBind()` returns true; and make `reset()` clear both fields. Do not leave `requestedHotkeys` or another equality-only early return in MainWindow.
- [ ] **Step 3: Wire every install call and reset with the required intent.** Change the startup restore at `mainwindow.cc:864`, dictionary-edit tail at `2187`, rescan tail at `3740`, and always-on-top path at `3771` to use the default/explicit `Refresh`. At the Preferences tail around `2338`, pass `UserAction` only when `result == QDialog::Accepted`, otherwise pass `Refresh`. Call `installGate.reset()` immediately at every existing `hotkeyBackend.reset()` site, preserving the pre-existing rescan reinstall behavior. Keep backend creation, signal connections, display-policy setup, bind, and status flow after the gate.
- [ ] **Step 4: Add the exact install-gate tests.** Add `hotkeyInstallDecisionTable` covering `backendPresent` x `unchangedSet` x all nine states x both intents and explicitly asserting every intersection named in the specification. Add `bindingSettleAndInFlightClassification` for the three-way partition. Add `installGateRemembersOnlyIssuedBinds` for true recording, false non-recording, and reset priming. Add `mainWindowUsesTheSharedInstallGate`, reading `TestSupport::repositoryRoot() + "/mainwindow.cc"` and asserting exactly one `installGate.shouldBind(` and no `requestedHotkeys` identifier.
- [ ] **Step 5: Run the install-gate mutation probes.** In disposable copies, make `hotkeyInstallShouldBind()` always true and run `hotkeyInstallDecisionTable`; restore immediately and rerun it. Make it always false and repeat. Treat `Refresh` as `UserAction` and repeat. Reintroduce `requestedHotkeys` in `mainwindow.cc` and run `mainWindowUsesTheSharedInstallGate`; restore immediately and rerun it.
- [ ] **Step 6: Run the affected suite and commit.** Run `qmake6 tests/tests.pro -o tests/Makefile && make -C tests -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`, confirming the gate tests and existing X11/unsupported backend tests pass. Commit with `git add hotkeybackend.hh hotkeybackend.cc mainwindow.hh mainwindow.cc tests/tst_hotkeys/tst_hotkeys.cc && git commit -m "fix(hotkeys): gate retries by install intent and state"`.

## Task 4: Expose live Configure capability and consent UI state

**Implementer tier:** Capable
**Lane:** configure-ui

**Files:**
- Modify: `hotkeybackend.hh:55-104`
- Modify: `portalhotkeys.hh:56-107`
- Modify: `portalhotkeys.cc:524-709,734-879,963-1143,1235-1350`
- Modify: `mainwindow.hh:184-191,260-268`
- Modify: `mainwindow.cc:2219-2220,3195-3288`
- Modify: `preferences.cc:615-667`
- Modify: `tests/tst_hotkeys/tst_hotkeys.cc:392-450,2250-2378`

**Interfaces:**
- Consumes: `HotkeyBackend::isSupported()`, `HotkeyBackend::State`, `PortalBackend::configure()`, `bindCompleted`, `sessionPath`, `owner`, `version`, and the existing MainWindow status callback/watcher at `mainwindow.cc:3248-3269`.
- Produces: `virtual bool HotkeyBackend::isConfigureAvailable() const noexcept { return false; }`, signal `void configureAvailabilityChanged()`, and `PortalBackend::isConfigureAvailable()` equal to `isSupported() && !d->owner.isEmpty() && !d->sessionPath.isEmpty() && d->version >= 2 && d->bindCompleted`.
- Produces: private `PortalBackend::publishConfigureAvailability()` with transition-only emission; `configure()` delegates to that same predicate after retaining the null/destruction guard; and `Preferences::setHotkeyStatus()` capability-based Configure enablement and inactive-label rendering.

### Steps

- [ ] **Step 1: Add the backend capability contract.** Add the virtual capability method with default false and `configureAvailabilityChanged()` to `HotkeyBackend` in `hotkeybackend.hh:55-104`; leave `UnsupportedBackend` and X11 behavior at the default. Declare the PortalBackend override and private publisher in `portalhotkeys.hh`.
- [ ] **Step 2: Implement the exact live predicate and its transition publisher.** In `PortalBackend`, call `isSupported()` rather than cached `d->supported`, require a non-empty pinned owner and session path, version at least 2, and `bindCompleted`. Cache the last published value and emit only on changes. Publish after storing a session path, after the version probe, after Bind completion including denial, and during `beginGeneration()`, `failGeneration()`, `closeSession()` before clearing the path, owner loss, Session.Closed, and `unbind()`. Clear `bindCompleted` on generation start and every invalidation, and keep it false during Creating, Ready, and Binding.
- [ ] **Step 3: Make configure() use the predicate once.** Keep `if (!d || d->destroying) return;`, then use `if (!isConfigureAvailable())` with the exact explanation `ConfigureShortcuts requires an available session`; send no ConfigureShortcuts call when false. Once true, retain the existing asynchronous v2 ConfigureShortcuts operation, with no Request.Response lifecycle and no second Bind.
- [ ] **Step 4: Wire MainWindow and Preferences to the live capability.** Remove `configureHotkeysAvailable`, `hotkeyStatusRevision`, and the asynchronous version watcher from `mainwindow.hh/.cc`. Pass `hotkeyBackend && hotkeyBackend->isConfigureAvailable()` through `refreshHotkeyStatus()`, use the same predicate in the `configureShortcutsRequested` lambda at `mainwindow.cc:2219-2220`, and connect `configureAvailabilityChanged()` to the existing refresh path. In `Preferences::setHotkeyStatus()` enable Configure only for `wayland && configureAvailable`, without enumerating states. Render an active ID's portal trigger; render `Not assigned by the portal — use Configure shortcuts` for an inactive ID when capability is true; otherwise retain the existing state/explanation text.
- [ ] **Step 5: Replace the Binding explanation.** At `startBind()` around `portalhotkeys.cc:972`, publish exactly `QStringLiteral( "Binding portal shortcuts: waiting for the portal confirmation dialog" )`; retain the `Binding` enum and do not add a Confirming state.
- [ ] **Step 6: Add the exact UI and capability tests.** Add `bindingExplanationNamesTheConfirmationDialog`: drive a real PortalBackend over the private bus until its real `stateChanged` Binding explanation is captured, assert the exact D5 string, then pass the captured string to Preferences and assert it renders. Add `unassignedBindingLabelAndConfigureEnablement`: cover Bound, Partial, Bind denial with live Configure enabled, CreateSession denial disabled, Creating, Ready, Binding, Unavailable, Closed, and Unsupported disabled, plus the inactive label; at least one capability value must come from a real backend, including a false-capability case. Add `configureAvailabilityFollowsBindCompletion`: assert false before a session and during held Bind Response, true after Bound and Bind denial, false after CreateSession denial, Session.Closed, owner loss, and unbind; assert one signal per transition and no polling; call configure directly during Binding and after denial; then stop the private bus daemon after a concluded Bound bind, not merely `QDBusConnection::disconnectFromBus`, and assert both `isSupported()` and capability become false and ConfigureShortcuts is not sent.
- [ ] **Step 7: Run the capability-owned mutation probes.** In disposable copies, make `isConfigureAvailable()` return true unconditionally and run tests 16 and 15; restore immediately and rerun both. Replace `isSupported()` with cached `d->supported` and run the disconnected-connection case in test 16; restore immediately and rerun it. Keep only the old hand-written Configure guards and run the direct-Binding call in test 16; restore immediately and rerun it. Enable Configure by state name instead of capability and run test 15; restore immediately and rerun it. Reword or remove the D5 emission and run test 14; restore immediately and rerun it.
- [ ] **Step 8: Run the affected suite and commit.** Run `qmake6 tests/tests.pro -o tests/Makefile && make -C tests -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`, confirming capability transitions, UI labels, the real explanation capture, and existing Configure lifecycle tests pass. Commit with `git add hotkeybackend.hh portalhotkeys.hh portalhotkeys.cc mainwindow.hh mainwindow.cc preferences.cc tests/tst_hotkeys/tst_hotkeys.cc && git commit -m "fix(hotkeys): expose live portal configure capability"`.

## Task 5: Amend the migration specification and perform final acceptance

**Implementer tier:** Fast
**Lane:** spec-amendments

**Files:**
- Modify: `docs/superpowers/specs/2026-10-24-qt6-migration-spec.md:938-942,960-967,1705-1707,1733-1734,1752-1756`

**Interfaces:**
- Consumes: the seven exact current target texts in migration-specification Section 7: the empty-trigger sentence, two Binding rows, the Configure row, the early-envelope sentence, the BindShortcuts deadline/retry paragraph, the active-subset sentence, and the request-deadline paragraph.
- Produces: exactly seven replacements in `docs/superpowers/specs/2026-10-24-qt6-migration-spec.md`; no other migration-specification wording, line, table, or formatting changes.

### Steps

- [ ] **Step 1: Replace the empty-trigger rule.** Replace `An empty trigger_description displays “Assigned by portal”.` at migration line 1707 with exactly `An empty trigger_description means the portal reports no active trigger for that ID: it is displayed as not assigned and never as active. The string is tested only for emptiness, never parsed and never compared with preferred_trigger.`
- [ ] **Step 2: Replace the Binding state rows.** Replace the two current rows at migration lines 938-940 with exactly `Response 0 where every requested ID is reported with a non-empty trigger_description -> Bound; at least one but not all -> Partial, the rest visibly unassigned; none -> Denied with explanation. Every activity decision uses the same rule for ShortcutsChanged responses.` Preserve table columns if the surrounding table requires them, and do not leave an ID-presence classification in any row.
- [ ] **Step 3: Replace the Configure capability row.** Replace the current `Bound/Partial` row at migration line 942 with exactly `Bound, Partial, and a concluded BindShortcuts denial | Configure action, version >= 2, live session | not available while a bind is pending, and not after a session-creation denial, which has no session.`
- [ ] **Step 4: Replace the early-envelope lifecycle sentence.** Replace the sentence around migration lines 1733-1734 with exactly `The active establishment buffer is bounded by the phase budget and MaxCandidates and is cleared after delivery. A retired request retains that same bounded buffer only for CleanupMs so a late CreateSession success can still expose its session_handle for Session.Close (org.freedesktop.portal.Session.Close; Request.Close remains the interface for the predicted and returned request paths and is unchanged by this amendment). The buffer is dropped at disposal or on service owner change; cancellation does not unconditionally clear it.`
- [ ] **Step 5: Replace the BindShortcuts deadline and retry text.** Replace the current BindShortcuts paragraph around migration lines 1752-1756 with exactly the approved text: `BindShortcuts(session, [(id,{description,preferred_trigger}),...], "", {handle_token}) -> request; only handle_token goes in **Bind options**. The 4.9 s budget covers establishment only. After establishment the request ends on Response, Session.Closed, portal owner change, generation retirement, or destruction. CreateSession and ListShortcuts keep the whole-lifetime bound. Activity is determined only by a non-empty trigger_description for a requested ID. A retry of an unchanged set happens only for an explicit user action - an accepted preferences dialog - and only when the previous outcome was neither Bound nor Partial; it uses a fresh session and exactly one BindShortcuts. A backend the platform declared Unsupported is never retried. Response `shortcuts` must be a unique subset of the requested valid IDs; unknown/duplicate IDs or wrong DBus types are protocol error -> Unavailable. Partial/empty is legal, not protocol error.` Preserve the existing code-fence and list formatting around the replacement.
- [ ] **Step 6: Replace active-subset and request-deadline statements.** Replace the sentence at migration lines 960-961 with exactly `The active subset may be replaced by a successful BindShortcuts response and by a validated current-session ShortcutsChanged update received after the bind-completion gate; each applies the same activity rule - an ID is active only when the portal reports a non-empty trigger_description for it. Pre-bind historical ListShortcuts entries still never replace the subset and never activate.` Replace the paragraph at lines 965-967 with exactly `Its deadline begins before invocation and covers establishment - owner lookup, method reply and request-path resolution - for every request. A MachineAnswered request keeps that same 5 s budget for its whole lifetime. A UserConsent request (BindShortcuts) is exempt from any wall-clock bound once established and ends only on Response, Session.Closed, portal owner change, generation retirement, or destruction.`
- [ ] **Step 7: Run the consolidated mutation sweep. Headless only.**
  After every named test exists, execute each mutation mapping from Tasks 1-4 in a disposable copy,
  restoring each source immediately and rerunning its killer green; confirm the documented
  equivalent mutation is not claimable. Lane children are headless and isolated: never start
  GoldenDict, never open a desktop dialog, and never touch the user's session or configuration.
  The seven-step desktop acceptance procedure (real Wayland registration, the real consent
  dialog, the poisoned-ID repair through System Settings, the retry gesture, and the final diff and
  artifact audit) is **PM-owned** and runs after integration, not inside this plan.

- [ ] **Step 8: Validate the final worktree and commit the specification amendment.** Run `qmake6 tests/tests.pro -o tests/Makefile && make -C tests -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`, then run `git diff --check && git status --short --untracked-files=all`; remove only generated test/build artifacts so the final status contains the expected committed implementation history and no build output. Commit with `git add docs/superpowers/specs/2026-10-24-qt6-migration-spec.md && git commit -m "docs(spec): amend portal consent and activity rules"`.
