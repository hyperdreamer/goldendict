# Global hotkeys: the portal's consent dialog is not a timeout

**Status:** design v6 for the continuation gate (v1 blocked by review 1, five
findings; v2 by review 2, two unresolved plus four new; v3 by review 3, one
unresolved plus four new; v4 by review 4, two; v5 by review 5, one — see
`reports/design-review-1.md` … `design-review-5.md`)
**Topic:** `hotkey-global-shortcut`
**Date:** 2026-10-30

## Background

On KDE Plasma 6.8 Wayland (this machine: `XDG_SESSION_TYPE=wayland`, Plasma
`6.8.1`, `xdg-desktop-portal-kde 6.8.1-1`, `kglobalaccel 6.31.0-1`, GoldenDict
`2:1.5.1.r2858.g278bb0c4-1`) the shortcut shown in **Preferences → Hotkeys → Show
or hide the main window** (`Ctrl+Alt+4`) never fires. The field shows a value, the
checkbox is enabled, and the key does nothing.

KGlobalAccel has the action with **no active assignment**:

```
$ grep -A2 '^\[org.goldendict.GoldenDict\]' ~/.config/kglobalshortcutsrc
_k_friendly_name=GoldenDict
main-window=none,Ctrl+Alt+4,Show or hide the main window          # active,default,friendly

$ gdbus call --session --dest org.kde.kglobalaccel \
    --object-path /component/org_goldendict_GoldenDict \
    --method org.kde.kglobalaccel.Component.allShortcutInfos
([('main-window', 'Show or hide the main window', 'org.goldendict.GoldenDict',
   'GoldenDict', 'default', 'Default Context', @ai [], [201326644])],)
```

active keys `[]`; default `[201326644]` = `0x0C000000` (`ControlModifier |
AltModifier`) `| 0x34` (`Qt::Key_4`). `shortcutKeys` is empty, no other component
claims `Ctrl+Alt+4` (not a conflict), and no portal request object is outstanding.

### Root cause 1 — a 4.9 s deadline on a consent dialog

`portalhotkeys.cc` gives every portal request one 4.9 s wall-clock budget
(`DeadlineMs = 4900`), started in `PortalRequestState::start()`, enforced by the
timer callback and by `expired()`. `startBind()` sends `BindShortcuts` through the
same class.

Plasma 6.8's `BindShortcuts` withholds its `Response` until the user answers a
modal consent dialog (`xdg-desktop-portal-kde/src/globalshortcuts.cpp`,
`QuickDialog` + `delayReply`, *"GoldenDict wants to register the following
shortcut"*). A human needs far longer than 4.9 s:

1. `CreateSession`, `ListShortcuts` complete (machine-answered).
2. `BindShortcuts` opens the dialog and withholds `Response`.
3. At 4.9 s the request fails → `failGeneration()` → `Session.Close`.
4. Closing the session resolves KDE's deferred reply as `Rejected`; KDE calls
   `KGlobalAccel::setDefaultShortcut(preferred)` and `setShortcut({})`, writing
   exactly `main-window=none,Ctrl+Alt+4,...`.
5. GoldenDict retired that generation, suppresses the reply, and never re-asks.

### Root cause 2 — a saved shortcut ID never prompts again

`CreateSession` calls `session->loadActions()`, which loads every saved ID,
including one with an empty active assignment. `BindShortcuts` then splits the
request by **ID**, so a known ID takes the `newShortcutInfos.empty()` branch and
returns **success without a dialog**; `setActions()` restores the saved keys and
`KGlobalAccel::setShortcut(..., Autoloading)` ignores the replacement for an
existing action. KDE says so in its own rejection branch: *"We also register the
denied shortcuts (with no key combinations) so we don't prompt on next
application start. The user can assign bindings later in systemsettings."*

A first-time registration and a previously declined ID are therefore different
cases; only the first is repaired by re-binding, and the second's repair route is
KDE's own shortcut editor — what the portal's `ConfigureShortcuts` opens.

### Root cause 3 — activity is inferred from ID presence, on two paths

`handleBindResponse()` classifies purely by which IDs the reply names, and
`displayEntries()` invents a label when the portal reports no trigger:

```cpp
// portalhotkeys.cc:139-148
if( trigger.isEmpty() )
    trigger = QStringLiteral( "Assigned by portal" );

// portalhotkeys.cc:1135-1141
d->activeIds = returned;
if (returned.isEmpty())                              setState( Denied, ... );
else if (returned.size() == d->requestedIds.size())   setState( Bound, "Portal shortcuts are bound" );
else                                                  setState( Partial, ... );
```

The same rule appears again in the `ShortcutsChanged` handler
(`portalhotkeys.cc:1218-1229`), which clears `activeIds` and inserts every ID the
portal reports, then decides `Bound` from the count alone:

```cpp
d->activeIds.clear();
for ( PortalShortcut const & entry : entries )
    d->activeIds.insert( entry.id );
... setState( d->activeIds.size() == d->requestedIds.size() ? Bound : Partial );
```

So a portal reply that names an ID with an empty `trigger_description` — the shape
KDE uses for a declined shortcut, and the shape a `ShortcutsChanged` update
carries when the user unassigns a key in System Settings — makes GoldenDict claim
`Bound` and display **"Assigned by portal"** for a key that cannot fire. That is
why the reported UI looks healthy.

### Root cause 4 — the specification mandates two of these defects

- `docs/superpowers/specs/2026-10-24-qt6-migration-spec.md:1705-1707`: *"An empty
  trigger_description displays 'Assigned by portal'."*
- `…spec.md:942` (§3.4 state table): Configure is reachable only from
  `Bound/Partial`; and the table classifies `Binding → Bound` from *all valid IDs
  reported*, without an activity rule. Its timeout row (*"Any live state | Timeout
  … no retry"*) has no consent exemption.

An implementer following the specification as written reproduces the false label.

### Secondary defect — no retry after a failure

`MainWindow::installHotKeys()` returns early whenever its `requestedHotkeys`
member equals the new set (`mainwindow.cc:3215-3221`), without consulting the
backend's outcome, so after a denial or a loss the running application cannot ask
again.

## Goals

1. A portal response gated on user consent is never failed by a wall clock.
2. The short bound still protects request establishment (owner lookup, method
   reply, request-path resolution) and the whole lifetime of machine-answered
   requests (`CreateSession`, `ListShortcuts`).
3. **One activity rule for every portal reply path** — `BindShortcuts` and
   `ShortcutsChanged` alike: an ID is active only when the portal reports a
   non-empty `trigger_description` for it. An empty trigger is reported as *not
   assigned*, never as *assigned* and never as `Bound`.
4. A user with a failed or denied binding can retry on an explicit gesture, and
   every retry uses a fresh portal session (one `BindShortcuts` per session).
5. A user whose ID KDE already saved as declined has a supported repair route in
   the existing UI, consistent with KDE's saved user choice.
6. While a consent dialog is outstanding the Hotkeys page says so in words that
   point at the dialog.
7. On the reporter's machine `Ctrl+Alt+4` becomes the **active** KGlobalAccel
   assignment and toggles the main window.

## Non-goals

- **No automatic repair of a saved KDE assignment.** Rewriting
  `kglobalshortcutsrc` or driving `KGlobalAccel::setShortcut` directly would
  overwrite a user choice behind the compositor's back and depend on kglobalaccel
  internals. The in-app Configure route is the supported repair.
- The X11 `RECORD` backend, `HotkeySyntax::preferredTrigger`,
  `PortalBindingDisplay` persistence, display policy, and portal wire shape.
- The existing portal-session reinstall that `on_rescanFiles_triggered()`
  performs (`mainwindow.cc:3713` resets the backend before `3740` re-installs).
  It is pre-existing behaviour, unchanged here, and is named in D2 so nobody
  mistakes it for the retry gate.
- Any new Preferences widget. UI changes are one explanation string, the
  truthfulness of one existing label, and the enablement rule of the existing
  **Configure shortcuts** button.
- Background auto-retry; retries happen only on an explicit user gesture (D2).

## Architecture

Six changes across `portalhotkeys.{hh,cc}`, `hotkeybackend.{hh,cc}`,
`mainwindow.{hh,cc}`, `preferences.cc`, the two test suites, the fake portal, and
the migration specification. The header edits are load-bearing: `mainwindow.hh`
loses the `requestedHotkeys` and `configureHotkeysAvailable` members
(`mainwindow.hh:185,189`), gains the `installGate` member, and `installHotKeys()`
changes from `mainwindow.hh:266`'s zero-argument declaration to the defaulted-intent
form.

### D1 — `PortalResponsePolicy`: one request, two bounded phases

`portalhotkeys.hh` declares the policy beside `PortalRequest`:

```cpp
enum class PortalResponsePolicy
{
  /// The portal answers without human interaction; the 4.9 s budget covers the
  /// whole request, exactly as before.
  MachineAnswered,
  /// The portal withholds its Response until the user answers a consent dialog.
  /// The budget covers establishment only.
  UserConsent
};
```

`PortalRequest` gains the policy as its **last** parameter, after the existing
`parent`:

```cpp
PortalRequest( QDBusConnection const & bus, quint64 generation,
               QObject * parent = nullptr,
               PortalResponsePolicy policy = PortalResponsePolicy::MachineAnswered );
```

Placing it last preserves every existing two- and three-argument construction
unchanged — including `createSession()`, `startList()` and `startBind()`, which all
pass `this` in third position, and the direct constructions in
`tests/tst_portal_hotkeys/tst_portal_hotkeys.cc`. Inserting the policy immediately
after `generation` would have made that `this` bind to the policy parameter. Only
`startBind()` passes the fourth argument, `PortalResponsePolicy::UserConsent`.

**Phases.** *Establishment*: from `start()` until `methodFinished()` has validated
and recorded `returnedPath` — the `GetNameOwner` lookup, the method call,
request-path resolution, and the `candidates` buffer (bounded by `MaxCandidates`).
*Delivery*: from that point until the request is retired.

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

and in `methodFinished()`, immediately after `returnedPath` is validated:

```cpp
established = true;
if ( !retired && policy == PortalResponsePolicy::UserConsent )
    timer.stop();
```

Stopping only while live preserves `cancel()`'s retirement path (`retired = true`,
`closeRequests()`, `timer.start(CleanupMs)`) and the `CleanupMs` linger that closes
a late session. Every other guard is untouched: `expired()` still runs first in
`methodFinished()`/`receiveResponse()`, `validPath()` still rejects an invalid
reply, predicted/returned-path matching, the `MaxCandidates` buffer, captured-owner
invocation, retired-generation suppression in `deliver()`, and `CleanupMs`.

**Consent terminals.** The wait ends on `Response(0|1|2)` → `deliver()`;
`Session.Closed` or portal owner change → `failGeneration`; a new bind or
`unbind()` → `cancelRequests()` → `PortalRequest::cancel`; destruction →
`~PortalBackend`.

### D2 — Retry requires an explicit gesture; the gate owns the memory

`hotkeybackend.hh` publishes the whole decision as one value-level function, so a
suite that never links `MainWindow` can exercise it:

```cpp
/// What the caller is doing. Only UserAction may re-bind an unchanged set that is
/// not settled.
enum class HotkeyInstallIntent { Refresh, UserAction };

/// True when installHotKeys must issue bind() for this call.
bool hotkeyInstallShouldBind( bool backendPresent, bool unchangedSet,
                              HotkeyBackend::State state,
                              HotkeyInstallIntent intent ) noexcept;

bool hotkeyBindingSettles( HotkeyBackend::State ) noexcept;   // Bound, Partial, Unsupported
bool hotkeyBindingInFlight( HotkeyBackend::State ) noexcept;  // Creating, Binding
```

```text
 1. !backendPresent                        -> true   (first bind for this backend)
 2. state == Unsupported                   -> false  (absolute: the QPA platform
                                            cannot serve hotkeys, so no set binds;
                                            the value is still stored and shown)
 3. inFlight(state) &&  unchangedSet       -> false  (never preempt for the same set)
 4. inFlight(state) && !unchangedSet       -> true   (a real edit replaces the
                                            pending attempt; see the consequence)
 5. !unchangedSet                          -> true   (a real edit always binds)
 6. settles(state)                         -> false  (nothing to fix)
 7. intent == UserAction                   -> true   (explicit retry)
 8. otherwise                              -> false  (Refresh never retries)
```

The rules are **ordered**; the first match wins, so every intersection is decided
and there is no row where two rules disagree. In particular a changed set with
`Creating`/`Binding` matches rule 4 (bind) and *not* rule 3, and a changed set with
`Unsupported` matches rule 2 (no bind) and *not* rule 5.

**Documented consequence of rule 4.** Preempting an in-flight consent attempt calls
`beginGeneration()`, which closes the portal session; KDE resolves the still-open
dialog as rejected and records *that* shortcut ID as declined. This is the existing
`bind()` behaviour, it only happens when the user genuinely edits a shortcut while
the dialog is open, and it is self-announcing and repairable: the page reports the ID
as *not assigned* (D3) and **Configure shortcuts** repairs it (D4). The alternative —
silently deferring an accepted edit until the pending attempt concludes — was
rejected as more machinery than the case justifies.

`Unsupported` is final because the QPA platform cannot change while the process
runs. Totality across the nine enumerators is guaranteed by classification test 11,
not by a compiler diagnostic (a `-Wswitch` omission is not an error under this
tree's flags). Test 10 pins every intersection explicitly, including
changed-plus-`Creating`, changed-plus-`Binding`, changed-plus-`Unsupported`, and
unchanged-plus-`Refresh`/`UserAction` for all nine states.

**The gate owns the memory.** `requestedHotkeys` is removed from `MainWindow`;
a small `HotkeyInstallGate` (declared in `hotkeybackend.hh`) holds the last desired
list and exposes one method:

```cpp
class HotkeyInstallGate
{
public:
  /// Compares against the last accepted list, applies hotkeyInstallShouldBind(),
  /// and records the list when a bind is issued.
  bool shouldBind( bool backendPresent, QVector< HotkeyBinding > const & desired,
                   HotkeyBackend::State state, HotkeyInstallIntent intent );
  void reset();                        // called where the backend is reset
private:
  QVector< HotkeyBinding > lastDesired;
  bool primed = false;
};
```

`MainWindow::installHotKeys()` then contains no decision: it builds `desired`,
calls `installGate.shouldBind( hotkeyBackend != nullptr, desired, hotkeyState, intent )`,
returns when false, and otherwise keeps its existing creation, connection, bind and
status code unchanged. `installGate.reset()` is called wherever
`hotkeyBackend.reset()` is called (`mainwindow.cc:1031, 1270, 1314, 2126, 3713`).

**Intent at every call site.** `installHotKeys()` gains a defaulted parameter
`HotkeyInstallIntent intent = HotkeyInstallIntent::Refresh`. The real call sites,
verified in this tree, are:

| Line | Enclosing function | Role in this tree | Intent |
| --- | --- | --- | --- |
| `864` | `MainWindow::MainWindow` (starts 124) | startup restore; backend not yet created | `Refresh` (binds because `!backendPresent`) |
| `2187` | `MainWindow::editDictionaries` (starts 2123) | tail after dictionaries or groups changed | `Refresh` |
| `2338` | `MainWindow::editPreferences` (starts 2204) | tail after `int const result = preferences.exec()` at 2227; reached on **OK and Cancel** | `UserAction` **iff** `result == QDialog::Accepted`, else `Refresh` |
| `3740` | `MainWindow::on_rescanFiles_triggered` (starts 3710) | after `hotkeyBackend.reset()` at 3713 | `Refresh` (backend absent, so it reinstalls — the pre-existing rescan behaviour named in Non-goals) |
| `3771` | `MainWindow::on_alwaysOnTop_triggered` | unrelated window-flag change | `Refresh` |

There is no `applyPreferences()` call site and no separate "hotkey edit" site: a
hotkey edit arrives as an accepted Preferences dialog whose desired list differs,
so `!unchangedSet` already binds it. **`2338` is the only site that can pass
`UserAction`**, and `result` is in scope there, so the intent is carryable without
restructuring. Accepting Preferences with an unchanged list is therefore the whole
retry gesture, exactly as the operator chose.

### D3 — One activity rule, shared by both reply paths

`portalhotkeys.cc` gains one helper used by `handleBindResponse()` and by the
`ShortcutsChanged` branch of `receivePortalSignal()`:

```cpp
/// The subset of `requested` the portal reports as actually assigned: the entry is
/// present and its trigger_description is a non-empty string. The string is tested
/// only for emptiness — never parsed, never compared with preferred_trigger.
QSet< QString > activeIdsFromReply( PortalWire::Shortcuts const & entries,
                                    QSet< QString > const & requested );
```

Both paths then do the same three things: replace their `records` subset, set
`d->activeIds = activeIdsFromReply(...)`, and classify from `activeIds` — `Bound`
when every requested ID is active, `Partial` when at least one is, `Denied` when
none is. `d->addressedIds` (IDs the reply names at all) is remembered separately
from `d->activeIds`, because the two now differ and the difference is the point.
`displayEntries()` drops the `"Assigned by portal"` substitution; an empty trigger
is what the UI renders as *not assigned* (D4).

### D4 — The existing Configure button becomes the repair route, gated on a live session

The enable rule needs a real predicate, because `Denied` has two origins:
`handleCreateResponse()` sets `Denied` for a `CreateSession` denial *before* any
`sessionPath` exists, while a `BindShortcuts` denial keeps a live session. A UI
rule keyed on the state name alone would enable a button that cannot work.

`HotkeyBackend` therefore publishes the capability directly:

```cpp
/// True when a Configure action can be served right now. For the portal backend:
/// supported bus, portal version >= 2, a live session path, a pinned owner, and a
/// bind attempt that has concluded — a pending consent wait is not configurable.
virtual bool isConfigureAvailable() const noexcept { return false; }
```

`PortalBackend::isConfigureAvailable()` is `isSupported() && !d->owner.isEmpty() &&
!d->sessionPath.isEmpty() && d->version >= 2 && d->bindCompleted`. It deliberately
calls **`isSupported()`**, never the cached `d->supported` flag: `isSupported()` is
`d && d->versionKnown && d->supported && d->bus.isConnected()`, and the live-bus
qualification is exactly what a concluded binding can outlive. Substituting the
cached flag would advertise a Configure action on a disconnected connection and
send `ConfigureShortcuts` to a transport error instead of refusing — the regression
review 5 B7 found, and the guard the current `configure()` already applies. The
remaining terms are the session identity and completion state: `bindCompleted` is
the existing flag that `handleBindResponse()` sets and that `beginGeneration()`,
`failGeneration()` and `unbind()` clear, and it is already required before a
`ShortcutsChanged` update is accepted (`portalhotkeys.cc:1158,1185`). `bindCompleted`
is what keeps Configure disabled while a consent dialog is still pending — review 3
B1. The null/destruction entry protection in `configure()` (`!d || d->destroying`)
is kept and runs before the delegated predicate.

**The operation enforces the same predicate, not a copy of it.**
`PortalBackend::configure()` replaces its hand-written guard with
`if ( !isConfigureAvailable() ) { setState( d->state, "ConfigureShortcuts requires an
available session" ); return; }`, so the button, the public method and the state walk
cannot drift apart and a direct call during `Binding` is refused exactly like a
disabled button (review 4 B1).

`MainWindow` deletes the asynchronous `Properties.Get("version")` watcher and the
`configureHotkeysAvailable` flag it fed; `refreshHotkeyStatus()` passes
`hotkeyBackend && hotkeyBackend->isConfigureAvailable()`, and the
`configureShortcutsRequested` lambda at `mainwindow.cc:2220` uses the same predicate
instead of the deleted flag. `HotkeyBackend` gains one signal,
`configureAvailabilityChanged()`. `PortalBackend` calls one private helper
`publishConfigureAvailability()` — which compares against a cached bool and emits
only on change — at every point that can flip a term: after `CreateSession` stores
the session, after the version probe answers, after `handleBindResponse()` sets
`bindCompleted`, and at each session invalidation (`closeSession()` before it clears
the path, `failGeneration()`, owner loss, `Session.Closed`, `unbind()`,
`beginGeneration()`), so the page refreshes without polling.

`Preferences::setHotkeyStatus()` enables the button for
`wayland && configureAvailable` — no state enumeration. Consequences: `Bound` and
`Partial` enabled; a **bind** denial enabled (the repair route, `bindCompleted` true
with a live session); a **session-creation** denial disabled (no session);
`Creating`, `Ready` and `Binding` disabled (no concluded bind); `Unavailable`,
`Closed`, `Unsupported` disabled.

The per-ID label rule:

| Situation | Per-ID label |
| --- | --- |
| the ID is active | the portal's reported trigger |
| the ID is not active, and Configure is available | `"Not assigned by the portal — use Configure shortcuts"` |
| the ID is not active, Configure unavailable | the existing state/explanation text |

After the user assigns the key in System Settings, KDE updates the session, emits
portal `ShortcutsChanged`, and GoldenDict's page refreshes through D3's rule.

### D5 — One actionable explanation

`startBind()` publishes
`QStringLiteral( "Binding portal shortcuts: waiting for the portal confirmation dialog" )`
instead of `"Binding portal shortcuts"`. ASCII, `QStringLiteral`, matching every
other portal explanation in the file (none are `tr()`-wrapped today).

**UI decision record.** Question 3 of the dialogue chose this actionable wording
over a bare `"Binding global shortcuts"` or a new `Confirming` state; question 5
chose OK/hotkey-edit as the retry gesture over a new button; question 4 chose the
Configure repair route over automatic `kglobalshortcutsrc` cleanup. No visual
mockup was produced or is applicable: two strings inside existing `QLabel`s plus
the enablement of an existing button.

### D6 — Specification amendments (seven sites, all behaviourally load-bearing)

1. **§4.11 lines 1705-1707** (the offending sentence is on line 1707) — replace
   *"An empty trigger_description displays 'Assigned by portal'"* with: *"An empty trigger_description means the portal reports no
   active trigger for that ID: it is displayed as not assigned and never as active.
   The string is tested only for emptiness, never parsed and never compared with
   preferred_trigger."*
2. **§3.4 state table, `Binding` rows** — replace *"Response 0, all valid IDs
   reported → Bound"* and *"Response 0, proper nonempty subset → Partial"* with
   activity-based rows: *"Response 0 where every requested ID is reported with a
   non-empty trigger_description → Bound; at least one but not all → Partial, the
   rest visibly unassigned; none → Denied with explanation. Every activity
   decision uses the same rule for ShortcutsChanged responses."*
3. **§3.4 state table, Configure row** — replace *"Bound/Partial | Configure
   action, version >= 2"* with *"Bound, Partial, and a concluded BindShortcuts denial
   | Configure action, version >= 2, live session | not available while a bind is
   pending, and not after a session-creation denial, which has no session."*
4. **§4.12, early-envelope sentence** — replace with the two-purpose text: active
   establishment buffering bounded by the phase budget and `MaxCandidates` and
   cleared on delivery; a *retired* request retains that same bounded buffer only
   for `CleanupMs` so a late `CreateSession` success can still have its
   `session_handle` closed; dropped at disposal or on owner change. No
   unconditional clearing at cancellation.
5. **§4.12, `BindShortcuts`** — add the consent deadline rule (the 4.9 s budget
   covers establishment only; the request ends on `Response`, `Session.Closed`,
   owner change, retirement, or destruction; `CreateSession`/`ListShortcuts` keep
   the whole-lifetime bound), the activity rule of D3, and a retry rule scoped to a
   **live portal backend**: *"a retry of an unchanged set happens only for an
   explicit user action — an accepted preferences dialog — and only when the previous
   outcome was neither Bound nor Partial; it uses a fresh session and exactly one
   BindShortcuts. A backend the platform declared Unsupported is never retried."*
6. **§3.4 lines 960-961** — replace *"Only a successful BindShortcuts response can
   replace the active subset, and only bound current-session IDs can activate."*
   with: *"The active subset may be replaced by a successful BindShortcuts response
   and by a validated current-session ShortcutsChanged update received after the
   bind-completion gate; each applies the same activity rule — an ID is active only
   when the portal reports a non-empty trigger_description for it. Pre-bind
   historical ListShortcuts entries still never replace the subset and never
   activate."* Without this the Configure repair route, which works purely through
   ShortcutsChanged, contradicts §3.4.
7. **§3.4 lines 965-967** — replace *"Its 5 s deadline begins before invocation and
   covers both method reply and Response."* with: *"Its deadline begins before
   invocation and covers establishment — owner lookup, method reply and request-path
   resolution — for every request. A MachineAnswered request keeps that same 5 s
   budget for its whole lifetime. A UserConsent request (BindShortcuts) is exempt
   from any wall-clock bound once established and ends only on Response,
   Session.Closed, portal owner change, generation retirement, or destruction."*
   §4.12 amendment 5 alone cannot reconcile this unconditional §3.4 sentence.

## Error handling matrix

| Situation | Method | Outcome |
| --- | --- | --- |
| method replies with a D-Bus error | any | `fail("Portal method failed: …")` → `failGeneration` |
| no method reply within 4.9 s | any | `"Portal request deadline exceeded"`; `Request.Close`, plus `Session.Close` only when a session handle was learned |
| method reply carries an invalid request path | any | `failGeneration("Portal method returned an invalid request path")` |
| method reply names a valid alternate path | any | accepted via `matches()`; other paths' envelopes discarded |
| `Response` before the method reply | any | buffered in `candidates`, delivered once the path is known |
| reply ok, `Response` never arrives | `CreateSession`, `ListShortcuts` | fails at 4.9 s |
| reply ok, `Response` never arrives | `BindShortcuts` | stays `Binding`; ends only on a D1 terminal |
| `Response(1)`/`Response(2)` | `BindShortcuts` | `Denied`, `"Portal shortcut binding was denied"`; session kept, Configure available |
| `Response(1)` | `CreateSession` | `Denied`, `"Portal session creation was denied"`; no session, Configure unavailable |
| `Response(0)`, every requested ID active | `BindShortcuts` | `Bound` |
| `Response(0)`, some IDs active | `BindShortcuts` | `Partial`; the rest labelled not assigned |
| `Response(0)`, no ID active or IDs with empty trigger | `BindShortcuts` | `Denied`; each ID labelled not assigned |
| `ShortcutsChanged` with mixed/empty triggers | signal | the same rule: `Bound`/`Partial`/`Denied` from `activeIdsFromReply` |
| `Response(0)` carrying an unrequested ID | `BindShortcuts` | `failGeneration("…unknown shortcut id")` |
| malformed `a(sa{sv})` payload | `ListShortcuts`, `BindShortcuts`, `ShortcutsChanged` | `failGeneration("…invalid data: …")` |
| owner change while waiting | any | `failed("Portal service owner changed")` → `Unavailable`, `supported=false` |
| `Session.Closed` while waiting | any | `Unavailable` |
| rebind/`unbind()` while waiting | any | `cancelRequests()` → `Request.Close`; retired generation suppresses the reply |
| late `CreateSession` reply after cancellation | `MachineAnswered` | buffered `session_handle` still closed during `CleanupMs` |
| app shutdown while waiting | any | `~PortalBackend` → `cancelRequests()`; `CleanupMs` covers a late session |
| Configure requested with no live session | UI + backend | button disabled by `isConfigureAvailable()`; `configure()` guard still refuses |
| Configure requested while a consent dialog is pending | UI + backend | disabled: `bindCompleted` is false, so `isConfigureAvailable()` is false; `configure()` itself refuses through the same predicate and sends no `ConfigureShortcuts` |

## Compatibility

No wire, D-Bus, config, or persisted-format change: `PortalBindingDisplay`,
`<hotkeys>/<portalBindings>`, display policy, and `<mainWindowHotkey>` are
untouched. `DeadlineMs`/`CleanupMs` keep their values. The X11 `RECORD` path and
`UnsupportedBackend` are unmodified; `UnsupportedBackend::isConfigureAvailable()`
keeps the header's default `false`. The closed 19-suite inventory is unchanged.

**Existing installations.** A previously declined ID stays declined until the user
uses **Configure shortcuts**; a first-time ID shows the consent dialog once the
deadline no longer kills it. The reporter's `main-window` entry is the declined
case, so its acceptance path is the Configure route.

## Test strategy

The fake portal needs three additive extensions; nothing existing changes.

**Fake portal** (`tests/support/fake_portal.{hh,cc}`):

1. **Active-trigger payloads.** The production Bind request carries only
   `description` and `preferred_trigger` (`portalhotkeys.cc:976-983`); the portal is
   what adds `trigger_description`. Today `bindPayload()` returns the requested
   entries unchanged, so under D3 every default success would classify as `Denied`.
   `bindPayload()` therefore decorates each returned entry with a non-empty
   `trigger_description` (the request's `preferred_trigger` text) unless the entry
   already carries one or the scenario says otherwise. A new scenario
   `bind-returning-empty-trigger` returns the requested IDs with
   `trigger_description` emptied — KDE's declined-ID shape. `boundEntries` remains
   verbatim, so a test can also build the empty case explicitly.
2. **Per-method response barriers.** `holdResponse( member )` /
   `releaseHeldResponse( member )` withhold **only that method's `Response`** after
   its method reply has been sent; they never withhold the reply and never touch
   another method's Response or the predicted-path plumbing. `holdMethodReply(
   member )` / `releaseHeldMethodReply( member )` withhold **only that method's
   reply**; an early Response for the held method is still emitted if the selected
   scenario emits one. Releases preserve arrival order per request. The existing
   whole-scenario `releasePending()` is unchanged and still flushes everything.
3. **A `ShortcutsChanged` emitter** for the D3 signal test.

`tst_portal_hotkeys`:

1. `bindConsentResponseWaitsBeyondTheEstablishmentDeadline` — Create and List
   complete; hold only Bind's response; wait 5.2 s; assert `Binding`, no failure,
   no `Request.Close`/`Session.Close`; release; assert `Bound`.
2. `machineAnsweredResponseStillBoundedAfterMethodReply` — hold only List's
   response; assert the 4.9 s failure. Scopes the exemption to `UserConsent`.
3. `consentEstablishmentStillBounded` — hold Bind's **method reply** with a live
   session; assert failure before 5 s and one `Request.Close` **and** one
   `Session.Close`.
4. `createNoMethodReplyClosesOnlyARequestThatNeverLearnedASession` — existing
   `no-method-reply` shape; assert failure, one `Request.Close`, **zero**
   `Session.Close`.
5. `existingBindTimeoutRowNoLongerPassesThroughAListTimeout` — restage the
   `backendRetirementDuringPendingWork:bind-timeout` row so List completes and only
   Bind is held; it must assert `Binding`, never `Unavailable`.
6. `declinedSavedIdReportsUnassignedNotBound` — `bind-returning-empty-trigger`;
   assert `Denied`, `activeIds` empty, label is the unassigned text and **not**
   `"Assigned by portal"`.
7. `shortcutsChangedWithEmptyTriggerReportsUnassigned` — emit `ShortcutsChanged`
   naming the requested ID with an empty trigger; assert the state leaves `Bound`
   and the label is unassigned (kills an ID-presence rule on the signal path).
8. `bindDeniedThenUserActionRetryUsesNewSession` — `bind-denied` → `Denied`; retry;
   two `CreateSession`, one `BindShortcuts` per session (no protocol violation),
   ending `Bound`.
9. Consent-wait terminals, one case each: `unbind()` while waiting; destruction
   while waiting; portal owner loss while waiting; `Session.Closed` while waiting;
   a late success `Response` after retirement being suppressed.

`tst_hotkeys` (links `hotkeybackend.cc`, `portalhotkeys.cc`, `preferences.cc`,
`preferences.ui`, and `tests/support/fixtures.cc`):

10. `hotkeyInstallDecisionTable` — the full cross product of `backendPresent` ×
    `unchangedSet` × nine states × two intents, with the ordered rules of D2 asserted
    explicitly at the intersections: changed-plus-`Creating` → bind, changed-plus-
    `Binding` → bind, changed-plus-`Unsupported` → no bind, changed-plus-`Bound` →
    bind, unchanged-plus-`Refresh` → no bind in every state, unchanged-plus-
    `UserAction` → bind only in `Ready`/`Denied`/`Unavailable`/`Closed`, no bind in
    `Bound`/`Partial`/`Unsupported`/`Creating`/`Binding`, and `!backendPresent` → bind
    in every state and both intents.
11. `bindingSettleAndInFlightClassification` — the three-way partition of the nine
    enumerators.
12. `installGateRemembersOnlyIssuedBinds` — `HotkeyInstallGate` records the desired
    list when it returns true, does not when it returns false, and `reset()`
    primes a fresh bind.
13. `mainWindowUsesTheSharedInstallGate` — a source-shape assertion, the only way
    to pin a call inside a translation unit no suite links: read
    `TestSupport::repositoryRoot() + "/mainwindow.cc"` (already available via
    `fixtures.hh:100`) and assert it contains exactly one
    `installGate.shouldBind(` and **no** `requestedHotkeys` identifier. This kills
    review 2's B3 mutation (restoring the equality-only early return) deterministically
    and without linking `MainWindow`. The repo already ships static-shape checks
    (`scripts/check-qt6-only.sh`, `scripts/verify-test-gate.sh`), so this is an
    existing idiom rather than a new one.
14. `bindingExplanationNamesTheConfirmationDialog` — D5's exact string, rendered by
    `Preferences::setHotkeyStatus` for a non-active state.
15. `unassignedBindingLabelAndConfigureEnablement` — D4's table with **both**
    `Denied` origins distinguished: a bind-denied status with `configureAvailable`
    true (button enabled) and a session-creation-denied status with
    `configureAvailable` false (button disabled), plus `Bound`/`Partial` enabled and
    `Creating`/`Ready`/`Binding`/`Unavailable`/`Closed`/`Unsupported` disabled, and
    the unassigned label for an inactive ID.
16. `configureAvailabilityFollowsBindCompletion` — backend-driven, so the UI
    contract cannot be faked by hand-supplied booleans: over the private bus,
    `isConfigureAvailable()` is false before a session, **false while the Bind
    response is held (state `Binding`)**, true once a bind reply lands with `Bound`, true
    after a bind denial (the repair route), false after a session-creation denial,
    and false again after `Session.Closed`, owner loss and `unbind()`; and
    `configureAvailabilityChanged()` fires once per transition, not per poll. The
    same case calls **`backend.configure()` directly while `Binding`** and asserts no
    `ConfigureShortcuts` call reaches the portal, then calls it after a bind denial
    and asserts exactly one — the operation-path half of review 4 B1. It finally
    **disconnects the client connection** after a concluded `Bound` bind and asserts
    `isConfigureAvailable()` is false (the `isSupported()` term) and that
    `configure()` takes the unavailable-session refusal path with no
    `ConfigureShortcuts` — the live-bus half of review 5 B7.
17. `lateCreateSessionSuccessClosesItsSessionAfterCancellation` — stage an early
    success envelope for `CreateSession` with its **method reply** held, then
    `unbind()`; release the method reply and assert exactly one `Session.Close` for
    the buffered handle and no state resurrection. This is the retained-cleanup
    probe review 3 B5 asked for.
18. Existing expectations that D3 intentionally changes: `reportedTriggersAreDisplayOnly`
    (`tests/tst_portal_hotkeys/tst_portal_hotkeys.cc:1585-1598`) supplies one entry
    with a non-empty trigger and one with an empty trigger and currently expects
    `Bound` plus `"Assigned by portal"`; under D3 it must expect `Partial` and the
    unassigned label for the second ID. Decoration never rewrites verbatim
    `boundEntries`, so this expectation change is in the suite, not the fake.

**Mutation probes**, each with a named killing test, restored immediately:

| Mutation | Must fail |
| --- | --- |
| delete the `UserConsent && established` early return in `expired()` | 1 |
| `timer.stop()` applied to every policy | 2 |
| clear `candidates` in `cancel()` | 17 (a late `CreateSession` session is never closed) |
| skip the retired-branch `close(session, SessionInterface)` in `deliver()` | 17 |
| classify Bind by ID presence | 6 |
| classify `ShortcutsChanged` by ID presence | 7 |
| restore the `"Assigned by portal"` substitution | 6 |
| `hotkeyInstallShouldBind()` always true | 10 |
| `hotkeyInstallShouldBind()` always false | 10 |
| treat `Refresh` as `UserAction` | 10 |
| keep `requestedHotkeys` in `mainwindow.cc` | 13 |
| `isConfigureAvailable()` returns true unconditionally | 16 (and 15) |
| `isConfigureAvailable()` uses the cached `d->supported` instead of `isSupported()` | 16 (disconnected connection) |
| `configure()` keeps only its four hand-written guards | 16 (direct call during `Binding`) |
| enable Configure by state name instead of availability | 15 |
| reword or remove the D5 explanation | 14 |

One listed guard is **deliberately not claimed as killable**. Deleting the
`!retired` condition from the `UserConsent` `timer.stop()` call is an *equivalent
mutation* on every production construction path: `UserConsent` is assigned only to
`BindShortcuts`, which is not a `CreateSession` request, so a retired
`methodFinished()` reaches `if (retired && !createSession) dispose()` regardless and
stopping the cleanup timer one line earlier changes no observable lifetime. Review 1
B3 explicitly permits documenting an equivalent mutation instead of promising an
unrelated killing test. The guard stays as defensive code; the retained-cleanup
behaviour it protects is pinned by test 17 through the two mutations above, which
are observable.

## Verification plan

1. Build and run the aggregate gate unchanged:
   `qmake6 tests/tests.pro -o tests/Makefile && make -C tests -j$(nproc) &&
   QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`.
2. Run each mutation probe, confirm its named test fails, restore, re-run green.
3. Build the application from the integration worktree.
4. **Desktop acceptance, first-time registration** (`clipboard-lookup` has no saved
   entry): enable the clipboard hotkey, bind `Ctrl+Alt+5`, confirm KDE's dialog
   appears and **stays answerable for more than 5 seconds**, confirm it, then verify
   `shortcutKeys` is non-empty and the key fires.
5. **Desktop acceptance, the reported case** (declined ID): the page reports
   `main-window` as *not assigned* rather than `Bound`; **Configure shortcuts** is
   enabled; it opens KDE's shortcut editor on GoldenDict's component; assigning
   `Ctrl+Alt+4` there flips the page to the active label and
   `main-window=Ctrl+Alt+4,Ctrl+Alt+4,…`; the key then toggles the main window.
6. **Retry gesture**: deny a bind (a new *value* does not change GoldenDict's stable
   shortcut ID, so KDE will **not** prompt again for it — that is Root cause 2, not a
   failure). Verify instead that **OK** in Preferences starts a **fresh portal
   session** with exactly one `BindShortcuts` on it (assert session count and the
   per-session bind count in the portal's request tree), that the page then reports
   the ID as *not assigned* rather than `Bound`, that **Configure shortcuts** is now
   enabled and repairs it, and that **Cancel** produces no portal activity at all (no
   `CreateSession`). Step 4 remains the only place a consent dialog is expected.
7. `git diff --check`; no generated binaries, Makefiles, or test artifacts; changes
   limited to `portalhotkeys.{hh,cc}`, `hotkeybackend.{hh,cc}`, `mainwindow.{hh,cc}`,
   `preferences.cc`, the two test suites, the fake portal, and the specification.

## Rollout

One implementation commit on the PM integration branch. `packaging/arch/PKGBUILD`
pins `_commit` and `packaging/arch/src` is untracked makepkg scratch space, so the
packaging pin is refreshed by the usual follow-up commit after delivery.
