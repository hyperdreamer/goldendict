# Global hotkeys: portal consent wait and repair route

**Status:** implementation specification derived from approved design v6
**Topic:** `hotkey-global-shortcut`
**Date:** 2026-10-30
**Implementation baseline:** source checkout `/data/home/guest/Development/goldendict`, commit `499c6cc3779ca7b129cace5b96fd1c2361b01ca2`
**Approved design:** `docs/superpowers/specs/2026-10-30-hotkey-portal-consent-wait-design.md`, SHA-256 `91ddf01c4e420baa2e0bdb829d04939df7753b2f25098b70dba93b5ad14c96ce`
**Migration baseline:** `docs/superpowers/specs/2026-10-24-qt6-migration-spec.md`, SHA-256 `74882c637a0db132cc4973b3a7a85868f44dde59127bd302079a18e80edc1606`

## 1. Scope, authority, and exact file allowlist

This document is the executable contract for the implementation described by design v6. The approved design is authoritative for the decisions recorded here; the migration specification is the document amended by Section 7. The source baseline is clean at the commit recorded above. Source anchors used while deriving this contract are `portalhotkeys.hh:40-45`, `portalhotkeys.cc:46-48`, `portalhotkeys.cc:498-510`, `portalhotkeys.cc:972-1005`, `portalhotkeys.cc:1081-1185`, `mainwindow.cc:3195-3288`, `mainwindow.hh:184-191`, `mainwindow.cc:864,2187,2338,3740,3771`, and the five `hotkeyBackend.reset()` sites at `mainwindow.cc:1031,1270,1314,2126,3713`.

The implementation may modify exactly these paths:

- `portalhotkeys.hh`
- `portalhotkeys.cc`
- `hotkeybackend.hh`
- `hotkeybackend.cc`
- `mainwindow.hh`
- `mainwindow.cc`
- `preferences.cc`
- `tests/support/fake_portal.hh`
- `tests/support/fake_portal.cc`
- `tests/tst_portal_hotkeys/tst_portal_hotkeys.cc`
- `tests/tst_hotkeys/tst_hotkeys.cc`
- `docs/superpowers/specs/2026-10-24-qt6-migration-spec.md`, limited to the seven amendments in Section 7

No other implementation, test, build, packaging, generated, or documentation path may change. This specification-authoring task writes only this new specification file.

The existing D-Bus names, object paths, interfaces, request payload shapes, persistent configuration, display policy, X11 path, and `DeadlineMs`/`CleanupMs` values remain the baseline unless a later section states an explicit behavior change.

## 2. Portal request policy and two-phase lifecycle

This section implements design D1, approved design lines 169-240. `PortalRequest` currently uses one whole-lifetime deadline. The implementation must distinguish establishment from delivery without changing the existing request-path, owner-capture, generation, cleanup, or cancellation guards.

Declare this enum beside `PortalRequest` in `portalhotkeys.hh`:

```cpp
enum class PortalResponsePolicy
{
  // The portal answers without human interaction; the 4.9 s budget covers the
  // whole request, exactly as before.
  MachineAnswered,
  // The portal withholds its Response until the user answers a consent dialog.
  // The budget covers establishment only.
  UserConsent
};
```

Change the constructor by appending the policy after the existing `parent` parameter:

```cpp
PortalRequest( QDBusConnection const & bus, quint64 generation,
               QObject * parent = nullptr,
               PortalResponsePolicy policy = PortalResponsePolicy::MachineAnswered );
```

The policy must be the last parameter. Existing two- and three-argument construction remains source-compatible, including `createSession()`, `startList()`, `startBind()`, and direct test construction. Only `startBind()` passes `PortalResponsePolicy::UserConsent`; `CreateSession` and `ListShortcuts` remain `MachineAnswered`.

`PortalRequestState` stores the policy and an `established` boolean initialized to `false`. Establishment runs from `start()` until `methodFinished()` has validated and recorded `returnedPath`; it covers `GetNameOwner`, the method call, request-path resolution, and the bounded `candidates` buffer. Delivery begins after that point and continues until retirement.

`expired()` must retain its existing first guard and deadline behavior, with this policy branch before the clock comparison:

```cpp
bool expired()
{
    if ( retired )
        return false;
    if ( policy == PortalResponsePolicy::UserConsent && established )
        return false;      // consent timing belongs to the portal, never to a clock
    if ( elapsed.elapsed() >= DeadlineMs )
    {
        fail( QStringLiteral( "Portal request deadline exceeded" ) );
        return true;
    }
    return false;
}
```

Immediately after `methodFinished()` validates `returnedPath`, set the establishment gate and stop the timer only for a live consent request:

```cpp
established = true;
if ( !retired && policy == PortalResponsePolicy::UserConsent )
    timer.stop();
```

The `!retired` guard is retained. `cancel()` must continue to set `retired`, issue `closeRequests()`, and start `CleanupMs`; the implementation must not add unconditional `candidates.clear()` to cancellation. `dispose()` still clears retained request data. Owner change still invalidates stored handles and candidate buffers.

`expired()` continues to run first in `methodFinished()` and `receiveResponse()`. `validPath()`, predicted/returned path matching, `MaxCandidates`, captured-owner invocation, retired-generation suppression in `deliver()`, and `CleanupMs` remain unchanged. Consent terminals are:

- `Response(0)`, `Response(1)`, or `Response(2)`: deliver the response.
- `Session.Closed` or portal owner loss: fail the generation.
- A new bind or `unbind()`: cancel requests and retire the generation.
- Object destruction: `~PortalBackend()` cancels requests and preserves the cleanup path for a late session handle.

The consent request can remain in `Binding` beyond 4.9 seconds after establishment. A machine-answered request still fails at the existing whole-lifetime deadline if its response does not arrive.

## 3. Explicit retry gating and install memory

This section implements design D2, approved design lines 242-338. An unchanged shortcut set is retried only after an explicit accepted Preferences action. A refresh, rescan, window-flag change, dictionary edit, or startup restore does not retry a settled or failed unchanged set, except for the first bind when no backend exists.

Declare the following value-level API in `hotkeybackend.hh` so `tests/tst_hotkeys` can test the decision without linking `MainWindow`:

```cpp
enum class HotkeyInstallIntent { Refresh, UserAction };

bool hotkeyInstallShouldBind( bool backendPresent, bool unchangedSet,
                              HotkeyBackend::State state,
                              HotkeyInstallIntent intent ) noexcept;

bool hotkeyBindingSettles( HotkeyBackend::State ) noexcept;
bool hotkeyBindingInFlight( HotkeyBackend::State ) noexcept;
```

`hotkeyBindingSettles()` returns true only for `Bound`, `Partial`, and `Unsupported`. `hotkeyBindingInFlight()` returns true only for `Creating` and `Binding`. The nine-state partition is complete: `Unsupported`, `Creating`, `Ready`, `Binding`, `Bound`, `Partial`, `Denied`, `Unavailable`, and `Closed`.

`hotkeyInstallShouldBind()` is ordered and first-match-wins:

1. `!backendPresent` returns `true`.
2. `state == Unsupported` returns `false`. This is absolute for every set and intent; the value remains stored and displayed.
3. `hotkeyBindingInFlight(state) && unchangedSet` returns `false`.
4. `hotkeyBindingInFlight(state) && !unchangedSet` returns `true`.
5. `!unchangedSet` returns `true`.
6. `hotkeyBindingSettles(state)` returns `false`.
7. `intent == UserAction` returns `true`.
8. Every remaining `Refresh` case returns `false`.

The ordered form resolves all intersections: changed-plus-`Creating` and changed-plus-`Binding` bind; changed-plus-`Unsupported` does not; changed-plus-`Bound` binds; unchanged-plus-`Refresh` never retries; unchanged-plus-`UserAction` retries only in `Ready`, `Denied`, `Unavailable`, and `Closed`; and no backend always binds regardless of state or intent.

The implementation must replace `MainWindow::requestedHotkeys` with this gate in `hotkeybackend.hh`:

```cpp
class HotkeyInstallGate
{
public:
  bool shouldBind( bool backendPresent, QVector< HotkeyBinding > const & desired,
                   HotkeyBackend::State state, HotkeyInstallIntent intent );
  void reset();
private:
  QVector< HotkeyBinding > lastDesired;
  bool primed = false;
};
```

`shouldBind()` compares the current desired list with the last list, applies the ordered value-level decision, and records the list only when it returns `true`. `reset()` clears the remembered list and `primed` state. The gate owns all comparison memory; no duplicate equality-only early return or `requestedHotkeys` member may remain in `MainWindow`.

`MainWindow::installHotKeys()` gains `HotkeyInstallIntent intent = HotkeyInstallIntent::Refresh`. It builds `desired`, calls exactly one `installGate.shouldBind(hotkeyBackend != nullptr, desired, hotkeyState, intent)`, returns when the result is false, and keeps the existing backend creation, signal connections, bind call, and status flow after the gate.

`installGate.reset()` must be called at every existing `hotkeyBackend.reset()` site: destructor line 1031, shutdown line 1270, startup-failure handling line 1314, dictionary editing line 2126, and rescan line 3713.

The five call sites have these exact intents:

| Source line | Call site | Intent |
| --- | --- | --- |
| `mainwindow.cc:864` | `MainWindow::MainWindow` startup restore | `Refresh`; it binds because no backend exists |
| `mainwindow.cc:2187` | `MainWindow::editDictionaries` tail | `Refresh` |
| `mainwindow.cc:2338` | `MainWindow::editPreferences` after `preferences.exec()` | `UserAction` only when `result == QDialog::Accepted`; otherwise `Refresh` |
| `mainwindow.cc:3740` | `MainWindow::on_rescanFiles_triggered` tail | `Refresh`; reset made the backend absent |
| `mainwindow.cc:3771` | `MainWindow::on_alwaysOnTop_triggered` | `Refresh` |

Only the Preferences call at `2338` may pass `UserAction`. The `result` variable is already in scope there. Accepting Preferences with a changed list binds through the changed-set rule; accepting it with an unchanged list is the explicit retry gesture. Cancel passes `Refresh` and produces no retry. No other input path may pass `UserAction`.

The documented consequence of rule 4 is preserved: a real edit during a pending consent attempt preempts the old generation through `beginGeneration()`, closes the old session, and can cause KDE to record the open dialog's shortcut ID as declined. The page then reports that ID as not assigned, and the Configure route in Section 5 repairs it.

## 4. Activity semantics and displayed binding state

This section implements design D3, approved design lines 340-359, and the activity amendments in Section 7. ID presence means only that the portal named an ID. Activity requires a portal-reported non-empty `trigger_description`.

Add this shared helper in `portalhotkeys.cc` and use it from both `handleBindResponse()` and the `ShortcutsChanged` branch of `receivePortalSignal()`:

```cpp
QSet< QString > activeIdsFromReply( PortalWire::Shortcuts const & entries,
                                    QSet< QString > const & requested );
```

The helper returns exactly the requested IDs whose reply entry exists and whose `trigger_description` is a string with non-zero length. It tests that string only for emptiness. It never parses the localized display value and never compares it with `preferred_trigger`. A missing, wrong-type, or empty trigger is inactive; malformed payloads remain protocol errors under Section 8.

Both reply paths must perform the same operations:

1. Validate the wire shape, duplicate IDs, ID membership, and property types through the existing parsing contract.
2. Replace the applicable `records` subset with the validated reply data.
3. Record `d->addressedIds` as the IDs named by the reply.
4. Set `d->activeIds = activeIdsFromReply(entries, d->requestedIds)`.
5. Classify from `activeIds`: `Bound` when every requested ID is active, `Partial` when at least one but not all is active, and `Denied` when none is active.

`addressedIds` and `activeIds` are separate fields. Historical or declined IDs can be addressed while inactive. A pre-bind `ListShortcuts` result may update labels and trigger display records only; it never replaces the active subset, activates an ID, or tears down a session because an old ID is absent from the current bind input. Only a successful `BindShortcuts` response or a validated current-session `ShortcutsChanged` update received after the bind-completion gate may replace the active subset.

`displayEntries()` must remove the old `Assigned by portal` substitution. An empty `trigger_description` is rendered as not assigned. A reported trigger is display-only and is not fed back into wire input or compared with the preferred trigger.

The state table is therefore:

| Portal result | State | Per-ID activity |
| --- | --- | --- |
| Response 0 and every requested ID has a non-empty trigger | `Bound` | every requested ID active |
| Response 0 and at least one but not every requested ID has a non-empty trigger | `Partial` | active IDs show their reported trigger; the rest are not assigned |
| Response 0 and no requested ID has a non-empty trigger | `Denied` | every requested ID is not assigned |
| Response 1 or 2 from BindShortcuts | `Denied` | every requested ID is not assigned |
| Validated ShortcutsChanged with mixed or empty triggers | `Bound`, `Partial`, or `Denied` | same activity rule as BindShortcuts |

## 5. Live Configure capability and repair UI

This section implements design D4, approved design lines 361-429, and the Configure amendments in Section 7. The existing Configure button is the repair route for a concluded bind denial. It is enabled by a backend capability predicate, not by the state name `Denied`.

Add this virtual API to `HotkeyBackend`:

```cpp
/// True when a Configure action can be served right now.
virtual bool isConfigureAvailable() const noexcept { return false; }
signals:
  void configureAvailabilityChanged();
```

`PortalBackend::isConfigureAvailable()` must be exactly the live capability check below, with the existing null/destruction protection retained in `configure()` before the delegated predicate:

```cpp
return isSupported() && !d->owner.isEmpty() && !d->sessionPath.isEmpty()
       && d->version >= 2 && d->bindCompleted;
```

`isSupported()` must be used, never cached `d->supported`. `isSupported()` includes a connected bus, known version, and supported capability. `bindCompleted` remains false during `Creating`, `Ready`, and `Binding`; it becomes true only after Bind has concluded, including a Bind denial, and is cleared by `beginGeneration()`, `failGeneration()`, `unbind()`, session close, and owner loss. A session-creation denial has no session and therefore cannot enable Configure.

`PortalBackend::configure()` keeps `if (!d || d->destroying) return;`, then uses the same capability predicate:

```cpp
if ( !isConfigureAvailable() )
{
    setState( d->state,
              QStringLiteral( "ConfigureShortcuts requires an available session" ) );
    return;
}
```

It must not retain a separate hand-written copy of the version, owner, session, support, and bind-completion conditions. Once the predicate is true, ConfigureShortcuts remains the existing asynchronous v2 operation with no `Request.Response` lifecycle and no second Bind in the session.

Add a private `publishConfigureAvailability()` helper in `PortalBackend`. It computes the predicate, compares it to a cached last-published value, and emits `configureAvailabilityChanged()` only on a transition. Call it at every point that can change a term: after CreateSession stores `sessionPath`, after the version probe answers, after `handleBindResponse()` sets `bindCompleted`, and during every session invalidation (`closeSession()` before clearing the path, `failGeneration()`, owner loss, `Session.Closed`, `unbind()`, and `beginGeneration()`). The page must update from this signal; it must not poll.

`MainWindow` removes `configureHotkeysAvailable` and the asynchronous version watcher that fed it. `refreshHotkeyStatus()` passes `hotkeyBackend && hotkeyBackend->isConfigureAvailable()`. The `configureShortcutsRequested` lambda at `mainwindow.cc:2220` uses that same predicate before calling `configure({})`. Connect `configureAvailabilityChanged()` to the existing status refresh path.

`Preferences::setHotkeyStatus()` enables the existing Configure button for `wayland && configureAvailable`, with no state enumeration. Consequently:

- `Bound` and `Partial` are enabled when the live predicate is true.
- A Bind denial is enabled because the session is live and the bind has concluded.
- A session-creation denial is disabled because no session exists.
- `Creating`, `Ready`, `Binding`, `Unavailable`, `Closed`, and `Unsupported` are disabled.
- A direct `configure()` call during `Binding` refuses through the same predicate and sends no ConfigureShortcuts call.

Per-ID display labels are:

| Condition | Label |
| --- | --- |
| ID active | the portal-reported trigger |
| ID inactive and Configure available | `Not assigned by the portal - use Configure shortcuts` |
| ID inactive and Configure unavailable | the existing state/explanation text |

After the user assigns the key in System Settings, KDE emits `ShortcutsChanged`; the shared activity rule refreshes the label and state. This route repairs a declined ID without changing its stable GoldenDict shortcut ID.

## 6. Actionable consent explanation

This section implements design D5, approved design lines 431-443. At the existing `startBind()` state transition (`portalhotkeys.cc:972`), replace the current explanation string with this exact ASCII `QStringLiteral`:

```cpp
QStringLiteral( "Binding portal shortcuts: waiting for the portal confirmation dialog" )
```

The previous `QStringLiteral( "Binding portal shortcuts" )` must not remain. The state remains `Binding`; no new `Confirming` state is introduced. The explanation is visible while the consent request is waiting and tells the user where the interaction is occurring.

## 7. Required amendments to the Qt 6 migration specification

This section implements design D6, approved design lines 445-492. The implementation must change only the seven sites below in `docs/superpowers/specs/2026-10-24-qt6-migration-spec.md`. Each amendment quotes the current target text and then gives the exact replacement text. The present task does not modify that migration specification.

### 7.1 `trigger_description` interpretation, migration line 1707

Current target text, exactly as present in the migration specification:

```text
An empty trigger_description displays “Assigned by portal”.
```

Replace it with exactly:

```text
An empty trigger_description means the portal reports no active trigger for that ID: it is displayed as not assigned and never as active. The string is tested only for emptiness, never parsed and never compared with preferred_trigger.
```

### 7.2 Binding state rows, migration lines 938-940

Current target rows:

```text
| Binding | Response 0, all valid IDs reported | Replace display subset; Bound |
| Binding | Response 0, proper nonempty subset | Replace subset; Partial; missing IDs visibly unavailable |
```

Replace them with this exact activity rule text:

```text
Response 0 where every requested ID is reported with a non-empty trigger_description -> Bound; at least one but not all -> Partial, the rest visibly unassigned; none -> Denied with explanation. Every activity decision uses the same rule for ShortcutsChanged responses.
```

If the table remains tabular, the replacement must preserve the same columns while expressing those three outcomes and the ShortcutsChanged rule without reverting to ID-presence semantics.

### 7.3 Configure state row, migration line 942

Current target row:

```text
| Bound/Partial | Configure action, version >= 2 | ConfigureShortcuts existing session; no BindShortcuts call; keep current state until ShortcutsChanged |
```

Replace the row's state/capability contract with exactly:

```text
Bound, Partial, and a concluded BindShortcuts denial | Configure action, version >= 2, live session | not available while a bind is pending, and not after a session-creation denial, which has no session.
```

### 7.4 Early-envelope lifecycle, migration Section 4.12 around lines 1733-1734

Current target sentence:

```text
The early-envelope buffer is bounded to this request's 5 s lifetime and cleared on completion; no broad handler can deliver an unrelated request's result.
```

Replace it with exactly:

```text
The active establishment buffer is bounded by the phase budget and MaxCandidates and is cleared after delivery. A retired request retains that same bounded buffer only for CleanupMs so a late CreateSession success can still expose its session_handle for Request.Close. The buffer is dropped at disposal or on service owner change; cancellation does not unconditionally clear it.
```

### 7.5 BindShortcuts deadline and retry rule, migration Section 4.12 around lines 1752-1756

Current target text:

```text
- BindShortcuts(session, [(id,{description,preferred_trigger}),...], "",
  {handle_token}) -> request; only handle_token goes in **Bind options**. Response
  `shortcuts` must be a unique subset of the requested valid IDs; unknown/duplicate
  IDs or wrong DBus types are protocol error -> Unavailable. Partial/empty is legal,
  not protocol error.
```

Replace it with exactly:

```text
- BindShortcuts(session, [(id,{description,preferred_trigger}),...], "",
  {handle_token}) -> request; only handle_token goes in **Bind options**. The 4.9 s
  budget covers establishment only. After establishment the request ends on
  Response, Session.Closed, portal owner change, generation retirement, or
  destruction. CreateSession and ListShortcuts keep the whole-lifetime bound.
  Activity is determined only by a non-empty trigger_description for a requested ID.
  A retry of an unchanged set happens only for an explicit user action - an accepted
  preferences dialog - and only when the previous outcome was neither Bound nor
  Partial; it uses a fresh session and exactly one BindShortcuts. A backend the
  platform declared Unsupported is never retried. Response `shortcuts` must be a
  unique subset of the requested valid IDs; unknown/duplicate IDs or wrong DBus
  types are protocol error -> Unavailable. Partial/empty is legal, not protocol
  error.
```

### 7.6 Active-subset replacement, migration lines 960-961

Current target text:

```text
Only a successful BindShortcuts response can replace the active subset, and only bound current-session IDs can activate.
```

Replace it with exactly:

```text
The active subset may be replaced by a successful BindShortcuts response and by a validated current-session ShortcutsChanged update received after the bind-completion gate; each applies the same activity rule - an ID is active only when the portal reports a non-empty trigger_description for it. Pre-bind historical ListShortcuts entries still never replace the subset and never activate.
```

### 7.7 Request deadline paragraph, migration lines 965-967

Current target text:

```text
Its 5 s deadline begins before invocation and covers both method reply and Response.
```

Replace it with exactly:

```text
Its deadline begins before invocation and covers establishment - owner lookup, method reply and request-path resolution - for every request. A MachineAnswered request keeps that same 5 s budget for its whole lifetime. A UserConsent request (BindShortcuts) is exempt from any wall-clock bound once established and ends only on Response, Session.Closed, portal owner change, generation retirement, or destruction.
```

## 8. Error handling matrix and retained wire validation

Every existing request, generation, owner, and cleanup guard remains active under these outcomes:

| Situation | Method or path | Required outcome |
| --- | --- | --- |
| Method replies with a D-Bus error | any | `fail("Portal method failed: ...")` and `failGeneration` |
| No method reply within 4.9 s | any | `"Portal request deadline exceeded"`; issue `Request.Close`, plus `Session.Close` only when a session handle was learned |
| Method reply carries an invalid request path | any | `failGeneration("Portal method returned an invalid request path")` |
| Method reply names a valid alternate path | any | accept through `matches()`; discard envelopes for other paths |
| `Response` arrives before the method reply | any | buffer in bounded `candidates`; deliver once the path is known |
| Method reply succeeds and `Response` never arrives | `CreateSession`, `ListShortcuts` | fail at 4.9 s |
| Method reply succeeds and `Response` never arrives | `BindShortcuts` | remain `Binding`; end only on a Section 2 terminal |
| `Response(1)` or `Response(2)` | `BindShortcuts` | `Denied`, explanation `Portal shortcut binding was denied`; retain the live session and expose Configure |
| `Response(1)` | `CreateSession` | `Denied`, explanation `Portal session creation was denied`; no session and Configure unavailable |
| `Response(0)` and every requested ID is active | `BindShortcuts` | `Bound` |
| `Response(0)` and some but not all IDs are active | `BindShortcuts` | `Partial`; inactive IDs are labeled not assigned |
| `Response(0)` and no ID is active, including empty triggers | `BindShortcuts` | `Denied`; every ID is labeled not assigned |
| `ShortcutsChanged` with mixed or empty triggers | signal | classify as `Bound`, `Partial`, or `Denied` through `activeIdsFromReply` |
| `Response(0)` carries an unrequested ID | `BindShortcuts` | protocol failure through `failGeneration("...unknown shortcut id")` |
| Malformed `a(sa{sv})` payload | `ListShortcuts`, `BindShortcuts`, or `ShortcutsChanged` | protocol failure through `failGeneration("...invalid data: ...")` |
| Service owner changes while waiting | any | fail with `Portal service owner changed`; transition to `Unavailable` and set `supported=false` |
| `Session.Closed` arrives while waiting | any | transition to `Unavailable` |
| Rebind or `unbind()` arrives while waiting | any | `cancelRequests()` issues `Request.Close`; retired-generation guards suppress late delivery |
| Late `CreateSession` success after cancellation | machine-answered request | retained handle is closed during `CleanupMs`; it never becomes current |
| Application shutdown while waiting | any | `~PortalBackend()` cancels requests; `CleanupMs` covers a late session handle |
| Configure requested with no live session | UI and backend | button disabled by `isConfigureAvailable()`; the public method refuses |
| Configure requested while consent is pending | UI and backend | `bindCompleted` is false, so the button is disabled; the public method refuses and sends no ConfigureShortcuts |

The retained parser contract is mandatory. `a(sa{sv})` is marshaled through an explicitly registered QtDBus structure/list with entries `{QString id, QVariantMap properties}`. Duplicate IDs, malformed tuples/maps, wrong DBus types, and unknown IDs in a Bind response remain protocol failures. A well-formed historical ListShortcuts ID outside the current display policy is ignored for the current display; it is not compared with the current bindable set and does not invalidate the session. `trigger_description` is validated as a string property, then its non-empty value alone determines activity. The `BindShortcuts` response carries only the requested valid IDs as a unique subset; empty or partial subsets are legal and are not protocol errors.

## 9. Test contract and fake-portal behavior

The fake portal receives three additive extensions. Existing scenario behavior and the whole-scenario `releasePending()` contract remain unchanged except for the explicitly listed D3 expectation update in test 18.

1. **Active-trigger payloads.** Production Bind input still carries only `description` and `preferred_trigger` (`portalhotkeys.cc:976-983`). The fake portal's `bindPayload()` must add a non-empty `trigger_description`, using the requested preferred-trigger text, unless an entry already has one or a scenario explicitly requests another value. Add scenario `bind-returning-empty-trigger`, which returns requested IDs with an empty `trigger_description`. `boundEntries` remains verbatim so tests can construct empty-trigger payloads directly.
2. **Per-method barriers.** Add `holdResponse(member)` and `releaseHeldResponse(member)`. They hold only that method's `Response` after that method's method reply has been sent; they never hold the method reply, another method's Response, or predicted-path plumbing. Add `holdMethodReply(member)` and `releaseHeldMethodReply(member)`. They hold only that method's method reply; an early Response for the held method is still emitted when the scenario emits one. Releases preserve arrival order per request.
3. **ShortcutsChanged emitter.** Add a fake signal emitter for a validated `ShortcutsChanged` payload so the shared activity rule is exercised independently of the Bind response.

The two explicit fixture ordering constraints are:

- `holdResponse(member)` is response-only: the method reply is observable before the held Response, and releasing it cannot retroactively withhold or reorder another method's response.
- `holdMethodReply(member)` is reply-only: an early Response remains observable before the held method reply, and releasing it cannot alter another request's order.

The existing whole-scenario `releasePending()` must remain unchanged and still flush every deferred item. These barriers must not be implemented by replacing or weakening that existing helper.

Add these exact tests to `tests/tst_portal_hotkeys/tst_portal_hotkeys.cc`:

1. `bindConsentResponseWaitsBeyondTheEstablishmentDeadline`: complete Create and List, hold only Bind's Response, wait 5.2 seconds, assert `Binding`, no failure, no `Request.Close`, and no `Session.Close`; release the Response and assert `Bound`.
2. `machineAnsweredResponseStillBoundedAfterMethodReply`: hold only List's Response and assert the 4.9-second failure. This scopes the exemption to `UserConsent`.
3. `consentEstablishmentStillBounded`: hold Bind's method reply while a session is live; assert failure before 5 seconds, one `Request.Close`, and one `Session.Close`.
4. `createNoMethodReplyClosesOnlyARequestThatNeverLearnedASession`: use the existing no-method-reply shape; assert failure, one `Request.Close`, and zero `Session.Close`.
5. `existingBindTimeoutRowNoLongerPassesThroughAListTimeout`: restage `backendRetirementDuringPendingWork:bind-timeout` so List completes and only Bind is held; assert `Binding`, never `Unavailable`.
6. `declinedSavedIdReportsUnassignedNotBound`: use `bind-returning-empty-trigger`; assert `Denied`, empty `activeIds`, the unassigned label, and not `Assigned by portal`.
7. `shortcutsChangedWithEmptyTriggerReportsUnassigned`: emit `ShortcutsChanged` naming a requested ID with an empty trigger; assert the state leaves `Bound` and the label is unassigned, killing an ID-presence rule on the signal path.
8. `bindDeniedThenUserActionRetryUsesNewSession`: produce `bind-denied` and `Denied`, retry through accepted Preferences, then assert two CreateSession calls, one BindShortcuts per session, no protocol violation, and final `Bound`.
9. `consentWaitTerminals`: cover one case each for `unbind()` while waiting, destruction while waiting, portal owner loss while waiting, `Session.Closed` while waiting, and a late success Response after retirement being suppressed.

Add these exact tests to `tests/tst_hotkeys/tst_hotkeys.cc`:

10. `hotkeyInstallDecisionTable`: assert the complete cross product of `backendPresent`, `unchangedSet`, all nine states, and both intents. Explicitly assert changed-plus-`Creating` and changed-plus-`Binding` bind, changed-plus-`Unsupported` does not, changed-plus-`Bound` binds, unchanged-plus-`Refresh` never retries, unchanged-plus-`UserAction` binds only in `Ready`/`Denied`/`Unavailable`/`Closed`, and no backend binds in every state and intent.
11. `bindingSettleAndInFlightClassification`: assert the three-way partition of the nine states.
12. `installGateRemembersOnlyIssuedBinds`: assert that `HotkeyInstallGate` records a desired list when it returns true, does not record it when false, and `reset()` primes a fresh bind.
13. `mainWindowUsesTheSharedInstallGate`: read `TestSupport::repositoryRoot() + "/mainwindow.cc"` and assert exactly one `installGate.shouldBind(` and no `requestedHotkeys` identifier. This is the source-shape test for the gate because the suite does not link `MainWindow`.
14. `bindingExplanationNamesTheConfirmationDialog`: assert the exact D5 string through `Preferences::setHotkeyStatus` for a non-active state.
15. `unassignedBindingLabelAndConfigureEnablement`: assert D4 for both origins of `Denied`: Bind denial has Configure enabled with a live session; CreateSession denial has it disabled. Assert `Bound` and `Partial` enabled, and `Creating`, `Ready`, `Binding`, `Unavailable`, `Closed`, and `Unsupported` disabled. Assert the unassigned label for an inactive ID.
16. `configureAvailabilityFollowsBindCompletion`: over the private bus, assert `isConfigureAvailable()` is false before a session, false while Bind Response is held in `Binding`, true after a `Bound` Bind response, true after Bind denial, false after CreateSession denial, and false after `Session.Closed`, owner loss, and `unbind()`. Assert `configureAvailabilityChanged()` once per transition and no poll-driven emissions. Call `backend.configure()` directly during `Binding` and assert no ConfigureShortcuts call; call it after Bind denial and assert exactly one. Disconnect the client after a concluded `Bound` bind and assert the live-bus predicate becomes false and direct `configure()` refuses without sending ConfigureShortcuts.
17. `lateCreateSessionSuccessClosesItsSessionAfterCancellation`: stage an early CreateSession success envelope with its method reply held, call `unbind()`, release the method reply, and assert exactly one Session.Close for the buffered handle and no state resurrection.
18. `reportedTriggersAreDisplayOnly`: update the existing expectation (`tests/tst_portal_hotkeys/tst_portal_hotkeys.cc:1585-1598`): with one non-empty and one empty trigger, expect `Partial` and the unassigned label for the second ID. `boundEntries` remains verbatim; only the suite expectation changes.

`tst_hotkeys` links `hotkeybackend.cc`, `portalhotkeys.cc`, `preferences.cc`, `preferences.ui`, and `tests/support/fixtures.cc`. `tst_portal_hotkeys` remains the owner of real portal request ordering, private-bus, and session lifecycle assertions.

## 10. Mutation contract and equivalent mutation record

Run each mutation in a disposable copy, assert the named test fails, restore the mutation immediately, and rerun the affected green test. The required mutation-to-killer mapping is:

| Mutation | Killing test |
| --- | --- |
| Delete the `UserConsent && established` early return in `expired()` | 1 |
| Apply `timer.stop()` to every policy | 2 |
| Clear `candidates` in `cancel()` | 17 |
| Skip the retired-branch `close(session, SessionInterface)` in `deliver()` | 17 |
| Classify Bind by ID presence | 6 |
| Classify ShortcutsChanged by ID presence | 7 |
| Restore the `Assigned by portal` substitution | 6 |
| Make `hotkeyInstallShouldBind()` always true | 10 |
| Make `hotkeyInstallShouldBind()` always false | 10 |
| Treat `Refresh` as `UserAction` | 10 |
| Keep `requestedHotkeys` in `mainwindow.cc` | 13 |
| Make `isConfigureAvailable()` return true unconditionally | 16 and 15 |
| Use cached `d->supported` instead of `isSupported()` | 16, disconnected-connection case |
| Keep only the old hand-written Configure guards | 16, direct call during `Binding` |
| Enable Configure by state name instead of capability | 15 |
| Reword or remove the D5 explanation | 14 |

Deleting `!retired` from the `UserConsent` `timer.stop()` condition is deliberately recorded as an equivalent mutation rather than assigned a false killing test. `UserConsent` is used only for BindShortcuts; a retired request that reaches `methodFinished()` still reaches the existing retired disposal path, so stopping the cleanup timer earlier produces no observable production lifetime change. The defensive guard remains required. Test 17 proves the observable retained-cleanup behavior through the two distinct mutations that clear the candidate buffer or skip the late session close.

## 11. Seven-step acceptance procedure

1. Build and run the aggregate gate unchanged:

   ```text
   qmake6 tests/tests.pro -o tests/Makefile && make -C tests -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check
   ```

2. Run every mutation probe, confirm its named test fails, restore the production code, and rerun the affected test green.
3. Build the application from the integration worktree.
4. Desktop first-time registration: with no saved `clipboard-lookup` entry, enable the clipboard hotkey, bind `Ctrl+Alt+5`, confirm that KDE's dialog appears and remains answerable beyond five seconds, confirm it, then verify `shortcutKeys` is non-empty and the key fires.
5. Desktop reported declined-ID repair: the page reports `main-window` as not assigned rather than `Bound`; Configure shortcuts is enabled; it opens KDE's shortcut editor for GoldenDict; assigning `Ctrl+Alt+4` flips the page to the active label and `main-window=Ctrl+Alt+4,Ctrl+Alt+4,...`; the key toggles the main window.
6. Retry and cancellation: deny a bind, then use accepted Preferences OK to start a fresh portal session with exactly one BindShortcuts on that session. Assert the ID remains not assigned rather than `Bound`, Configure shortcuts is enabled and repairs it, and Cancel produces no portal activity, including no CreateSession. A new value for the stable ID is not expected to prompt again; the first-time consent test is step 4.
7. Run `git diff --check`; verify there are no generated binaries, Makefiles, or test artifacts; verify the implementation diff is limited to the twelve paths in Section 1.

## 12. Preserved behavior, non-goals, and resolved choices

No wire, D-Bus, configuration, or persisted-format change is allowed. Preserve `PortalBindingDisplay`, `<hotkeys>/<portalBindings>`, display policy, `<mainWindowHotkey>`, the X11 `RECORD` path, `UnsupportedBackend`, the closed 19-suite inventory, and the existing `DeadlineMs` and `CleanupMs` values. Wayland continues to disable hover translation and focus restoration. Clipboard-change scanning and explicit clipboard lookup remain usable and are not a global-hotkey fallback.

Do not add a new portal wire call, a second Bind in a session, a new confirmation state, automatic retries, `kglobalshortcutsrc` cleanup, a private Qt surface export, or a new Configure button. Configure remains the existing v2 operation and receives no `Request.Response` lifecycle. `ShortcutsChanged` is the repair update path.

The explicit retry gesture is an accepted Preferences dialog, including an unchanged list. Cancel is `Refresh` and cannot retry. A real shortcut edit may replace an in-flight generation, with the documented KDE decline consequence. Unsupported QPA is final for the process lifetime. A Bind denial and a CreateSession denial share the `Denied` state but differ in capability because only the Bind denial retains a live session.

The active/inactive distinction is fully determined: a non-empty portal `trigger_description` is active; an empty or absent trigger is not assigned and is never displayed as `Assigned by portal`. Historical ListShortcuts entries remain display-only. Consent timing is fully determined: establishment is bounded; an established UserConsent request waits for the portal terminal; cleanup still closes late sessions. The source allowlist, test names, fixture ordering, mutation evidence, and acceptance procedure are fully specified; no implementation choice remains underdetermined by this document or the approved design.
