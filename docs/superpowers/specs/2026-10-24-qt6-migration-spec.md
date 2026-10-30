# GoldenDict Qt6 Migration — Technical Specification

Date: 2026-10-24. Status: implementation contract for the PM planner.
Authority: [approved design, revision 6](2026-10-24-qt6-migration-design.md), decisions B, B1, W2, P1, T1, K4–K7.
Baseline: application commit `b4f3fcdc`; approved-design commit `0dbcccf4`.
Target: Linux, Qt 6.12, GCC 16, KDE Plasma 6, native Wayland; explicit X11 remains supported.

Repository citations below refer to the unchanged integration checkout at the drafting baseline.
Installed-header citations are relative to `/usr/include/qt6`, unless an absolute path is given.
An interface declared below is a proposed **application** interface; Qt interfaces are identified separately.
“Spec decision SD-nn” resolves a detail absent or ambiguous in the design without changing its decisions.

## 1. Scope, invariants, and evidence

### 1.1 Required resulting tree and behaviour

The resulting application is Qt6-only, uses QtWebEngine for article and picture documents,
uses iconv for Encoding, and has no Qt5Compat or QtMultimedia dependency. The ffmpeg and
external players remain. The root project remains a qmake application. `tests/tests.pro`
and the scripts in §6 are part of the implementation, not optional follow-up work.

Preserve dictionary formats, indexing algorithms and on-disk index signatures; make only
API adaptations in those engines. The existing FTS format version is declared at
`ftshelpers.hh:25`; changing regex implementation does not authorize an index format change.
The article renderer must cover the parity list in §6.6, including popup views.

Startup must respect an explicit platform chosen by the user. Delete the Wayland→xcb
assignment at `main.cc:264–268`. Register schemes before application construction at
`main.cc:318`, before creating profiles, pages or the popup.

The completed implementation must satisfy all of these invariants:

1. `qmake6 && make -j$(nproc)` succeeds in the default Linux configuration.
2. Root `make check` generates, builds and runs all 18 required test targets in §6.
3. The three mutations and the wrapper observation in §6.4 prove the gate actually fails
   for a failed testcase, a missing required environment, and an unwrapped test run.
4. The Qt6-only audit passes for compiled inputs and the generated application link command.
5. One shared named profile serves tabs, popup and DevTools; every page dies before it.
6. Every asynchronous article operation has a document generation and a guarded owner.
7. No synchronous JS-result shim or nested event loop is introduced.
8. Job cancellation cancels its producer and releases its device; finished data is drained.
9. Portal denial, missing service and unsupported sequences have visible capability states.
10. Wayland activation is best-effort; no X11 call runs merely because `HAVE_X11` is compiled.
11. Save and PDF export publish complete output; cancellation leaves no new partial output.
12. Required environment failures fail tests. A passing smoke timeout cannot replace tests.

The scope of this drafting deliverable is this specification alone. Build commands and
mutation probes below are instructions for implementation and acceptance; they were not
run while drafting.

### 1.2 Baseline anchors that determine the migration

| Existing behaviour or migration surface | Source evidence |
| --- | --- |
| Root module/version branches, optional multimedia, X11Extras | `goldendict.pro:31`, `goldendict.pro:37`, `goldendict.pro:45`, `goldendict.pro:159` |
| QWebView-based view and synchronous HTML/printing surface | `articleview.hh:7`, `articleview.hh:167`, `articleview.hh:176` |
| Article HTML's parser-time callback | `article_maker.cc:637–638` |
| WebKit object injection | `articleview.cc:1175` |
| Main/website-frame hit testing and selection | `articleview.cc:2323`, `articleview.cc:2927` |
| Hidden website iframe; no explicit name attribute | `website.cc:499–515` |
| Header-stripping and origin-whitelist code | `article_netmgr.hh:26`, `article_netmgr.hh:56`, `article_netmgr.cc:118`, `article_netmgr.cc:289` |
| Lookup parameters and explicit-dictionary precedence | `article_netmgr.cc:392–452` |
| Resource dispatch, generated icons and picture wrapper | `article_netmgr.cc:455`, `article_netmgr.cc:471`, `article_netmgr.cc:520–525` |
| Old resource reply's slice/cancel contract | `article_netmgr.cc:567`, `article_netmgr.cc:596–630`; `dictionary.hh:187–196` |
| Outgoing-history state capture and reload-to-top | `articleview.cc:866–879`, `articleview.cc:886`, `articleview.cc:1691`, `articleview.cc:1698`, `articleview.cc:1710` |
| Print preview uses current view synchronously | `mainwindow.cc:3568–3598` |
| Save collector and direct final-file write | `mainwindow.cc:3599`, `mainwindow.cc:3687–3696` |
| Proxy mode mapping, cache byte conversion and credentials | `mainwindow.cc:1286–1336`, `mainwindow.cc:1347–1374`, `mainwindow.cc:4703–4752` |
| Cache clear at exit | `mainwindow.cc:1108` |
| Hotkeys allow two keys; defaults are sequences | `config.cc:109–120`, `config.cc:229–231`; `hotkeywrapper.hh:68` |
| Existing hotkey storage is QKeySequence text | `config.cc:887–891`, `config.cc:1764–1773` |
| Backend names and audio preference load | `config.hh:215–221`, `config.cc:924–925` |
| Regex, codec and version-conditional baseline counts | approved design §1 (`docs/superpowers/specs/2026-10-24-qt6-migration-design.md:20–27`); concrete file inventories in §7 |

The design's counts (248 regex references, eight codec files, 406 version references,
27 `IS_QT_5` uses and 13 Qt4-only branches) are baseline inventory counts, not targets
for a global replacement script. Dead branches, comments and non-Linux helpers account
for some references. Audit scope and vendored-source handling are explicit in §7.

### 1.3 Spec decisions register

| ID | Resolution beyond the design |
| --- | --- |
| SD-01 | Semantic errors use result/error signals except Encoding's specified encoder exception. Allocation failure is outside recoverable contracts. Methods without explicit `noexcept` have no domain exception unless stated. |
| SD-02 | Encoding uses explicit UTF-32LE scalar interchange, canonical aliases, deterministic locale fallback, and the byte-by-byte decoder recovery interpretation in §4.1. |
| SD-03 | HTML detection scans the first 1024 body bytes for declarations, ignores unknown candidates, and strips only a matching initial BOM. |
| SD-04 | Shared LinkRewrite functions handle common rules; format-specific normalization stays with the engine. Already absolute and protocol-relative targets remain unchanged. `applyGdLookup` normalizes only the lookup path component and preserves structural query and fragment components, so `dict` and `gdanchor` survive a common pass and a second pass is idempotent. |
| SD-05 | Context maps use a pinned QDataStream version; FTS pattern text and wildcard mode travel separately in `FtsHighlightSpec`. |
| SD-06 | Audio/video responses stream; other responses buffer to completion. Unknown resource size waits before selecting a successful reply. A live sequential reply advertises `size() == 0`, reports only unread backlog through `bytesAvailable()`, ignores Range at the device level and serves a full 200 body; any Range field instead selects completion buffering. Before `reply()`, the handler resolves a valid single range against the final resource size and positions the full-size seekable buffer at N for `bytes=N-`/`bytes=N-M`, or `max(0, size-K)` for `bytes=-K`; Qt is not responsible for initial positioning. Unsatisfiable ranges fail with `RequestFailed`; no Range or an unparseable Range uses position 0, full size and 200. Terminal producer errors are distinct from successful completion and cancellation, including after a reply. GUI producer ownership and cancellation are separate from the mutex-protected device state used by WebEngine's IO thread. |
| SD-07 | The JS facade remains a stamped wrapper after channel readiness; an explicit inbound ready handshake releases queued work. All scripts for one generation have deterministic installation order and back/forward document caching is disabled to ensure reinjection. |
| SD-08 | Frame identity uses a child-tree path, frame value equality and document nonce; message measurement/resize extensions and coordinate units are specified in §4.7. |
| SD-09 | Same-site matching preserves the existing host-base heuristic with DNS-boundary checks. Page policy and file fix-ups are immutable IO-thread inputs. |
| SD-10 | Profile uses no persistent cookies; cache size saturates at the Qt API's signed-int limit; origin credentials are never persisted. |
| SD-11 | Navigation waits asynchronously for outgoing state capture with a 500 ms fallback to the last debounced state. Fragment scrolling is handled without engine navigation. |
| SD-12 | Complete save preserves an `.html` destination: rename a staged immutable resource directory, then atomically publish HTML as the commit point; roll back the directory on pre-commit failure. |
| SD-13 | PDF generation has a 30 s operational timeout; preview and print re-render the frozen snapshot with aspect-fit metrics. |
| SD-14 | Portal request subscription covers early responses; application binding edits replace the session, while v2's Configure action edits existing bindings through portal UI. No automatic recovery retry. The returned-ID subset rule applies only to each BindShortcuts response; ListShortcuts validates wire shape and duplicate structure, treats pre-bind results as historical display data, filters them by current display policy, and never activates or invalidates a session solely because a historical ID is absent from the current bind input. |
| SD-15 | Hotkey IDs, supported portal keys, additive XML layout and capability messages are fixed in §4.11–4.13. Existing unsupported sequence text is retained verbatim. |
| SD-16 | Tests use private D-Bus and ephemeral HTTP ports, three loopback IP aliases for distinct origins/hosts, exact fixture paths, and explicit forced-failure hooks only in test support. The real handler/device invoke the production-visible `DataRequestReader` seam inside their catch boundaries; production readers forward full/slice reads to the real nonvirtual DataRequest getters and copy live bytes without a producer reference. Test readers substitute one-shot full-data, slice and stream-copy exceptions with operation/thread evidence; no forced-failure policy or switch exists in production code. |
| SD-17 | `Use::DocumentLoad` enforces required group semantics for ordinary lookup documents, while `Use::UserAction` accepts group-less path and `bword` lookups. The retained ArticleView action handler supplies reference group, FTS dictionary subset and contexts, then applies `dict` and `gdanchor` before constructing a validated document URL; URL fields are never discarded before that dispatch. |

Attempt-3 review corrections change **SD-06** for R2-M1 (handler-resolved range
positioning, full resource size and unsatisfiable-range failure) and **SD-16** for
R2-M2 (the reader seam and test-only exception wiring). Every other SD entry is
preserved unchanged; neither correction changes an approved-design decision.

### 1.4 Installed Qt API evidence

These declarations were checked by reading or `rg -n` against installed Qt 6.12 headers.
The implementation must use these public APIs and overload shapes.

| Use | Verified declaration location and API |
| --- | --- |
| Register schemes | `QtWebEngineCore/qwebengineurlscheme.h:27–45`, `:65–81`: `Syntax::Host`, `setSyntax`, `setFlags`, `registerScheme`, `schemeByName` |
| Reply/error | `QtWebEngineCore/qwebengineurlrequestjob.h:34–52`: `requestUrl`, `requestMethod`, `requestHeaders`, `reply(QByteArray const &, QIODevice *)`, `fail(Error)` |
| Intercept | `QtWebEngineCore/qwebengineurlrequestinfo.h:67–79`: `resourceType`, `requestUrl`, `firstPartyUrl`, `initiator`, `block`, `redirect`; `qwebengineurlrequestinterceptor.h:21`: `interceptRequest` |
| Page construction and JS | `QtWebEngineCore/qwebenginepage.h`: constructor accepting `QWebEngineProfile *`; `:273` `toHtml`, `:289–295` `runJavaScript`, `scripts`, `setWebChannel` |
| Page events | `QtWebEngineCore/qwebenginepage.h:336–338`, `:365–366`, `:376`, `:396`, `:404–414`: load/auth/URL/find signals, `createWindow`, `acceptNavigationRequest` |
| Frame API | `QtWebEngineCore/qwebengineframe.h:36–62`: copyable frame values, `isValid`, `name`, `htmlName`, `children`, `url`, `isMainFrame`, `runJavaScript`; equality operator follows `:71` |
| Injected scripts | `QtWebEngineCore/qwebenginescript.h`: `DocumentCreation`, `DocumentReady`, `MainWorld`, `setName`, `setInjectionPoint`, `setWorldId`, `setRunsOnSubFrames`, `setSourceCode`; collection `insert`/`remove` in `qwebenginescriptcollection.h` |
| Web channel | `QtWebChannel/qwebchannel.h:30–36`: constructor, `registerObject`, `deregisterObject` |
| History/find/context | `QtWebEngineCore/qwebenginehistory.h`: `count`, `currentItemIndex`, `clear`, `back`, `forward`; `qwebenginefindtextresult.h`: `numberOfMatches`, `activeMatch`; `qwebenginecontextmenurequest.h:101–105`: `position`, `selectedText`, `linkText`, `linkUrl`, `mediaUrl` |
| Profile | `QtWebEngineCore/qwebengineprofile.h:38–89`, `:108`, `:140`: named constructor, cache/storage paths, user agent, cache type/maximum, persistent cookie policy, `clearHttpCache`, `clearHttpCacheCompleted`; scheme installation is declared in the same header |
| PDF snapshot | `QtWebEngineWidgets/qwebengineview.h:89–96`, `QtWebEngineCore/qwebenginepage.h:307–312`: callback `printToPdf(std::function<void(QByteArray const &)> const &, QPageLayout const &, QPageRanges const &)` |
| PDF decoding | `QtPdf/qpdfdocument.h:30–35`, `:77–105`: `Status::Ready/Error`, `load(QIODevice *)`, `status`, `error`, `pageCount`, `pagePointSize`, `render`, `statusChanged` |
| Printing | `QtPrintSupport/qprintpreviewdialog.h`: `paintRequested(QPrinter *)`; `qprinter.h:119–155`: `resolution`, `setFullPage`, `pageRect`, `newPage`; `QtGui/qpagedpaintdevice.h:36–40`: `setPageLayout`, `pageLayout`; `qpagelayout.h:103–108`: pixel rectangles |
| Streaming | `QtCore/qiodevice.h:57–77`, `:119–136`: virtual `isSequential`, `open`, `close`, `size`, `atEnd`, `bytesAvailable`, `readyRead`, `readChannelFinished`, `readData`, `writeData`; `qiodevicebase.h:25`: `Unbuffered` |
| Buffered range handoff | `QtCore/qbuffer.h:24–25`, `:36–48`: job-parented QBuffer, `setData`, `open`, full backing `size`, `pos`, `seek(qint64)`; `QtWebEngineCore/qwebengineurlrequestjob.h:38`, `:45–49`: `RequestFailed`, `requestHeaders`, `reply`, `fail` |
| Regex | `QtCore/qregularexpression.h:36–42`, `:111–157`, `:219–307`: options, `globalMatch`, wildcard conversion, `anchoredPattern`, captures, iterator |
| Qt6 string/time/locking | `QtCore/qchar.h:126`, `:154`: `QChar(char16_t)`, `fromUcs4`; `qstringview.h`: `QStringView`; `qdatetime.h:427`, `:522–528`: seconds APIs; `qmutex.h:188–202`, `:241–242`: `QRecursiveMutex`, templated locker; `qtextstream.h:89`: `setEncoding` |
| Compiled vendor checksum | `QtCore/qbytearrayalgorithms.h:262`, `qbytearrayview.h:171`: `qChecksum(QByteArrayView, Qt::ChecksumType)`, QByteArray view constructor; retain the default `Qt::ChecksumIso3309` |
| Document cache setting | `QtWebEngineCore/qwebenginesettings.h:69`, `:104`: `BackForwardCacheEnabled`, `setAttribute` |
| URL/data stream | `QtCore/qurlquery.h:63–71`: query operations; `qurl.h:199–212`: decoded components; `qdatastream.h:71`, `:149`: `Qt_5_0`, `setVersion` |
| Hotkeys | `QtGui/qkeysequence.h:116–153`: `PortableText`, `NativeText`, `count`, `fromString`, `toString`, indexed `QKeyCombination`; `QtCore/qnamespace.h:1926–1960`: modifier/key access and `toCombined` |
| Proxy/auth | `QtNetwork/qnetworkproxy.h:85–88`, `:137–173`: proxy types, application proxy, system query; `qauthenticator.h:32–37`: credential setters and realm |
| Window/platform | `QtGui/qguiapplication.h:74–83`: platform and screens; `qguiapplication_platform.h:63–70`: `QNativeInterface::QX11Application::display()`; `qwindow.h:263`: `requestActivate`; `qscreen.h`: geometries |
| D-Bus | `QtDBus/qdbusconnection.h:122–173`: asynchronous calls, signal connections, service/object registration, bus construction; `qdbusmessage.h:46–83`: method message/path/arguments; `qdbuspendingcall.h:69–83`: watcher and `finished`; `qdbusservicewatcher.h:32–58`: service loss |
| Test/publication helpers | `QtTest/qsignalspy.h:30–50`, `qtestcase.h:65–77`, `:211–227`, `:291–297`; `QtNetwork/qtcpserver.h:29–40`; `QtCore/qsavefile.h:46–48`, `qdir.h:204`, `qfile.h:204–208`; `qprocess.h:133`, `:223`, `:240–247` |

KDE activation is independently verified at
`/usr/include/KF6/KWindowSystem/kwindowsystem.h:200`.
Portal wire signatures are from
`/usr/share/dbus-1/interfaces/org.freedesktop.portal.GlobalShortcuts.xml`.
`ConfigureShortcuts` returns no request handle. Qt's frame API has no public parent or
frame-ID accessor; the spec does not depend on either. Construct pages with their
profile; do not depend on a page profile setter. Set printer orientation through
`QPageLayout`.

Range ordering is pinned to QtWebEngine commit
`d4735ce2f841ffbe7cbac9ee7fd954f00514825f`:
[`url_request_custom_job_proxy.cpp:60–65`](https://github.com/qt/qtwebengine/blob/d4735ce2f841ffbe7cbac9ee7fd954f00514825f/src/core/net/url_request_custom_job_proxy.cpp#L60)
seeks only a positive unresolved first position, then samples `size()`;
[`custom_url_loader_factory.cpp:223–255`](https://github.com/qt/qtwebengine/blob/d4735ce2f841ffbe7cbac9ee7fd954f00514825f/src/core/net/custom_url_loader_factory.cpp#L223)
resolves bounds and generates range metadata afterwards. Its pinned Chromium
`71a2fa55e2e2f5a5d6567aac93bda8775fc7b084`
[`http_byte_range.cc:83–112`](https://github.com/qt/qtwebengine-chromium/blob/71a2fa55e2e2f5a5d6567aac93bda8775fc7b084/chromium/net/http/http_byte_range.cc#L83)
resolves suffix starts only when the final size is known. This ordering requires the
handler's pre-reply positioning in §3.1/§4.4; a seekable buffer alone is insufficient.

## 2. Module and file inventory

### 2.1 Interface, ownership and exception conventions

All paths are repository-relative. A listed `.hh`/`.cc` pair means both exact files.
All QObject controllers, pages, views, handlers and reply devices are constructed on the GUI thread unless the table explicitly says otherwise. All JS completions return on that thread. Dictionary producers retain their existing worker/thread arrangement; queued connections marshal `updated` and `finished` into GUI-owned consumers.

The handler, job-facing operations and teardown coordination, job records, producer signal connections, request ownership and producer cancellation are GUI-owned. `ArticleStreamDevice` is constructed with GUI affinity and remains parented to the request job, but QtWebEngine may invoke its QIODevice virtuals from its Chromium IO thread after `reply()`. QObject affinity controls queued signal delivery, parent/child operations and GUI-owned slots; it does not constrain the thread that invokes a reply device, and moving the device is neither required nor a substitute for synchronization. The device's cross-thread contract is the explicit mutex-protected shared state in §3.1; IO-thread methods never dereference the `Dictionary::DataRequest`, handler map, job pointer or GUI connection objects.

Value objects are copied, contain no QObject ownership, and are usable from different
threads when immutable. All destructors are non-throwing. A declaration marked
`noexcept` must not propagate exceptions; other new interfaces report ordinary failures
through their specified result or signal and do not throw domain exceptions, except
`Encoding::encode` and the internal `DataRequestReader` operations explicitly caught
at the handler/device boundaries in §3.1. Resource construction catches existing
dictionary exceptions before crossing a Qt callback boundary. Allocation failure is
not an application recovery path (SD-01). Signal signatures below are part of the
public contract.

### 2.2 New production modules

| Exact files | Responsibility and public interface below | Owner, lifetime, thread |
| --- | --- | --- |
| `encoding.hh`, `encoding.cc` | Immutable named conversion; §2.4 | Values; per-call iconv descriptors; any thread |
| `linkrewrite.hh`, `linkrewrite.cc` | Common HTML link rules; §2.4 | Pure functions; any thread |
| `articleurls.hh`, `articleurls.cc` | URL kinds, query parsing and producers; §2.5 | Values/pure functions; resource resolution remains GUI |
| `articleschemes.hh`, `articleschemes.cc` | Registration, handler, streaming device, interceptor; §2.6 | Handler per profile, device per job; interceptor IO callback only |
| `articlebridge.hh`, `articlebridge.cc` | Generation, script installation, JS queue and inbound calls; §2.7 | Parented to ArticleView; page explicitly destroyed before bridge |
| `websiteframes.hh`, `websiteframes.cc` | Child-tree discovery, messaging, measurements, frame-local lookup; §2.7 | One per ArticleView; timers stop on generation change/destruction |
| `articlehistory.hh`, `articlehistory.cc` | Indexed application state; §2.8 | Value member of ArticleView; GUI access |
| `printpipeline.hh`, `printpipeline.cc` | PDF snapshot, Ready barrier, synchronous painting; §2.8 | One per ArticleView; owns bytes/buffer/document/preview |
| `articleprofile.hh`, `articleprofile.cc` | Shared profile, settings, two auth adapters, shutdown cache barrier; §2.9 | MainWindow owns controller; profile outlives every page |
| `savearticle.hh`, `savearticle.cc` | Extract existing save collector into one cancellable transaction; §2.9 | One controller per view; owns downloads and staging paths |
| `hotkeybackend.hh`, `hotkeybackend.cc` | Backend interface, capability state, startup factory; §2.10 | Unique owner in MainWindow; QObject parent is not a second deletion owner |
| `hotkeysyntax.hh`, `hotkeysyntax.cc` | Config text parsing and XDG trigger serialization; §2.10 | Pure functions; any thread |
| `portalhotkeys.hh`, `portalhotkeys.cc` | PortalBackend and PortalRequest; §2.10 | Backend GUI-owned; request children own watchers/timers/subscriptions |
| `platformwindow.hh`, `platformwindow.cc` | Screen, flags, X display and activation helpers; §2.10 | No owned windows/screens/display; GUI access only |

The two extraction modules `articleprofile` and `savearticle` are spec-level file
boundaries for the adapters/pipelines already required by design §6.5. They prevent
MainWindow from owning low-level request state; they add no product feature.

### 2.3 Rewritten/deleted production files and integration consumers

| Exact files | Required resulting responsibility/interface | Ownership and threading |
| --- | --- | --- |
| `articlewebview.hh`, `articlewebview.cc` | ArticleWebView/ArticleWebPage in §2.11; replace all QWebKit inheritance and event routing | View owns page; page borrows shared profile |
| `articleview.hh`, `articleview.cc` | Orchestrate async navigation, cached UI state, pipeline actions; §2.11 | Own bridge, frames, history, save, print; GUI |
| `articleinspector.hh`, `articleinspector.cc` | `ArticleInspector : QWidget`, constructor `(QWebEnginePage * inspected, Config::Class * cfg, QWidget * parent = nullptr)`; destructor; `void beforeClosed()` slot. Own a QWebEngineView/page using inspected page's profile; attach via `setDevToolsPage`; preserve geometry | Inspected ArticleView owns it; close before inspected page; cfg borrowed |
| `article_netmgr.hh`, `article_netmgr.cc` | Retain `ArticleNetworkAccessManager` constructor and `getResource(QUrl const &, QString & contentType) -> sptr<Dictionary::DataRequest>`; explicit-download adapter only; remove `SecurityWhiteList`, `Origins`, `AllowFrameReply`, renderer Referer policy; §4.8 | MainWindow owns manager; borrowed dictionaries/maker outlive requests |
| `hotkeywrapper.hh`, `hotkeywrapper.cc` | Retain X11 recorder mechanics as wrapper; add `X11RecordBackend : HotkeyBackend` with `(QObject * parent = nullptr)`, `isSupported`, `bind`, `unbind`. Preserve `QHotkeyApplication` class needed by main; remove Qt-version wrappers | Backend owns recorder; blocking RECORD loop remains worker-side; signals queued to GUI |
| `config.hh`, `config.cc` | Add `PortalBindingDisplay`, XML display keys, hotkey raw-text preservation; retain `HotKey`, `InternalPlayerBackend` except removed multimedia functions; §4.13 | Config owned by application; no IO-thread access |
| `gddebug.hh`, `gddebug.cc` | Remove `gdCodecForLocale` and locale codec mutation object; existing logging entry signatures unchanged, UTF-8 output | Existing logging synchronization remains; no global codec writes |
| `epwing_book.hh`, `epwing_book.cc` | Replace codec pointer members/accessors with immutable Encoding values and const references; §7.2 | EPWING book owns values; worker use is safe |
| `website.cc`, `slob.cc`, `indexedzip.cc`, `loaddictionaries.cc` | Codec migration; website real-origin frames/name and inline resolver; SLOB owns Encoding value | Existing engine lifetimes; no new QObject owner |
| `mainwindow.hh`, `mainwindow.cc` | Inject shared profile into tabs/popup; own hotkey backend/profile controller; use async cache/print/save; replace FTS regex signatures; §2.11 | Shut pipelines/pages down before profile and dictionaries |
| `scanpopup.hh`, `scanpopup.cc` | Add borrowed profile parameter before ArticleNetworkAccessManager in constructor; pass to ArticleView; platform flags/activation | MainWindow owns popup; popup owns view; GUI |
| `keyboardstate.cc`, `broken_xrecord.cc`, `scanflag.cc`, `translatebox.cc` | Use PlatformWindow helpers; X11-only paths guard both platform and non-null native display | Borrow native objects; GUI-side X calls only |
| `hotkeyedit.cc`, `preferences.hh`, `preferences.cc`, `preferences.ui`, `sources.cc`, `sources.ui` | Qt6 key handling and capability text, removed player choice, proxy/iframe/print notes; existing preferences getters/setters otherwise retained | Dialog owns controls; live capability signals go to GUI |
| `ftshelpers.hh`, `ftshelpers.cc`, `fulltextsearch.hh`, `fulltextsearch.cc` | QRegularExpression for matching and `FtsHighlightSpec` for display query metadata; §7.1 | Existing FTS workers; immutable compiled regex copies per operation |
| `stardict.cc`, `zim.cc`, `dictdfiles.cc`, `dictserver.cc`, `gls.cc` | Delegate common link transforms; preserve format-specific parsers and lookups | Existing engine ownership and threads |
| `main.cc` | Register schemes, remove forced xcb, choose backend/log platform, runtime engine readiness handling | Startup before app for registration; all later UI work GUI |
| `audioplayerfactory.cc` | Remove multimedia include/selection; retain ffmpeg and external paths | Existing audio ownership |
| `goldendict.pro` | Exact module/flag/source wiring and root gate in §7.4 | Build configuration |
| `resources.qrc`, `articleview.ui` | Embed JS files; retain promoted ArticleWebView name/header, update base metadata to QWebEngineView | Compiled resources/forms |
| `multimediaaudioplayer.hh`, `multimediaaudioplayer.cc` | **Delete**; no replacement | K4 |
| `qt4x5.hh` | **Delete** after all consumers are converted | B |

The mechanical files listed exhaustively in §7.3 are also implementation inputs;
they retain their existing public responsibilities/signatures except type corrections
listed there. Do not treat omission from the architectural table as permission to
leave Qt4/Qt5 shims in a compiled source.

### 2.4 Encoding and link interfaces

```cpp
class Encoding {
public:
  DEF_EX(Ex, "Encoding error", std::exception)
  DEF_EX(exUnrepresentable, "Character not representable in target encoding", Ex)
  Encoding() noexcept;                         // invalid value; decode falls back to UTF-8
  static std::unique_ptr<Encoding> byName(QString const & name);
  static Encoding utf8();
  static Encoding forLocale() noexcept;        // decode-only contract
  enum class DetectedBy { Header, Bom, Meta, XmlDeclaration, Fallback };
  static Encoding forHtml(QByteArray const & body, QByteArray const & contentTypeHeader,
                          QString const & fallback = QStringLiteral("UTF-8"),
                          DetectedBy * detectedBy = nullptr);
  bool isValid() const noexcept;
  QString name() const;
  QString decode(QByteArray const & data) const noexcept;
  QByteArray encode(QString const & text) const; // only domain exception: exUnrepresentable
};
namespace LinkRewrite {
QString applyGdLookup(QString const & html);
QString applyBres(QString const & html, QString const & dictId);
QString applyGico(QString const & html, QString const & dictId);
QString applyGdau(QString const & html, QString const & dictId);
}
```

Encoding copy/move retain only the canonical name and decode-only marker; no shared
iconv descriptor or mutable converter. `byName` returns null for unknown/empty names;
`utf8` is valid. LinkRewrite returns a new owning string and never retains the input.

### 2.5 URL and FTS data interfaces

```cpp
struct FtsHighlightSpec {
  QString sourcePattern;
  QRegularExpression expression;
  bool wildcards = false;
  bool matchCase = false;
};
namespace ArticleUrls {
enum class Use { DocumentLoad, UserAction };
enum class Kind { LookupDocument, PictureDocument, Resource, LookupAction,
                  AudioAction, VideoAction, ExternalAction, Invalid };
struct Lookup {
  bool blank = false;
  Config::InputPhrase phrase;
  unsigned group = 0;
  bool groupSpecified = false;
  QStringList dictionaries, muted;
  QMap<QString, QString> contexts;
  bool ignoreDiacritics = false;
  QString scrollTo, anchor;
  QString dictionaryName; // decoded `dict` target, retained for UserAction dispatch
  FtsHighlightSpec highlight;
};
struct Parsed {
  Kind kind = Kind::Invalid;
  QUrl url;
  Lookup lookup;
  QString dictionaryId, resourceName, error;
};
Parsed parse(QUrl const & url, Use use);
QUrl lookupDocument(Lookup const & lookup);
QUrl pictureDocument(QString const & dictionaryId, QString const & resourceName);
QByteArray encodeContexts(QMap<QString, QString> const & contexts);
bool decodeContexts(QByteArray const & base64, QMap<QString, QString> * out,
                    QString * error);
QString resourcePath(QUrl const & url); // decoded path + '?' + decoded query, no leading '/'
}
```

All functions are pure. `decodeContexts` clears `out` on failure, returns false and
fills `error`; caller never partially applies a map. The `FtsHighlightSpec` owns both
source syntax and compiled PCRE; Qt's regex object cannot remember wildcard syntax.

### 2.6 Scheme, device and interceptor interfaces

```cpp
void registerArticleSchemes(); // idempotent pre-application call, main thread only
class DataRequestReader {
public:
  virtual ~DataRequestReader() noexcept = default;
  virtual QByteArray fullData(Dictionary::DataRequest & request) const = 0; // GUI
  virtual void dataSlice(Dictionary::DataRequest & request, size_t offset,
                         size_t size, void * destination) const = 0; // GUI
  virtual void copyStreamBytes(char * destination, char const * source,
                               qint64 count) const = 0; // device read thread; no producer
};
std::shared_ptr<DataRequestReader const> makeDataRequestReader(); // production forwarding reader
using DataRequestReaderFactory = std::function<std::shared_ptr<DataRequestReader const>(
    sptr<Dictionary::DataRequest> const &)>; // called only on GUI, once per job
class ArticleSchemeHandler : public QWebEngineUrlSchemeHandler {
  Q_OBJECT
public:
  ArticleSchemeHandler(ArticleNetworkAccessManager & resources, QObject * parent = nullptr);
  ArticleSchemeHandler(ArticleNetworkAccessManager & resources,
                       DataRequestReaderFactory readerFactory, QObject * parent = nullptr);
  ~ArticleSchemeHandler() override;
  void requestStarted(QWebEngineUrlRequestJob * job) override;
signals:
  void producerFailed(QUrl const & url, QString const & message);
};
class ArticleStreamDevice : public QIODevice {
  Q_OBJECT
public:
  ArticleStreamDevice(sptr<Dictionary::DataRequest> const & request,
                      std::shared_ptr<DataRequestReader const> reader, QObject * jobParent);
  ~ArticleStreamDevice() override;
  bool isSequential() const override;
  qint64 size() const override;
  qint64 bytesAvailable() const override;
  bool atEnd() const override;
  void close() override;
  void cancel() noexcept; // GUI thread; producer cancellation follows lock release
signals:
  void completed(); // successful producer completion observed once
  void failed(QString const & message); // terminal read/producer failure, once
protected:
  qint64 readData(char * data, qint64 maxSize) override;
  qint64 writeData(char const * data, qint64 maxSize) override;
};
struct ArticleRequestPolicy {
  bool blockExternal = false;
  QSet<QString> dictionaryHosts;
  QMap<QString, QUrl> localFileRedirects; // original fully encoded URL -> fixed URL
};
class ArticleRequestInterceptor : public QWebEngineUrlRequestInterceptor {
public:
  explicit ArticleRequestInterceptor(QObject * parent = nullptr);
  void setPolicy(std::shared_ptr<ArticleRequestPolicy const> policy);
  void interceptRequest(QWebEngineUrlRequestInfo & info) override;
};
```

One handler can be installed under all seven names on the shared profile. Its borrowed
resource manager outlives it. Job map records contain a guarded job pointer, shared
DataRequest, reader, GUI-owned connections, device, reply/failure latches, cancellation
latch, range policy and failure message. Reply buffers/devices are parented to the job, not the
profile. The handler never deletes a reply device independently of the job. Before
`reply()` the handler opens a device on the GUI thread. After `reply()`, the streaming
device's `read`, `size`, `atEnd`, `bytesAvailable`, `isSequential` and loader-driven
`close` operations may run on Chromium's IO thread; they use only the synchronized
state and immutable reader dependency defined in §3.1. GUI code does not call those
QIODevice state operations after handoff. A device remains valid for the documented
`QWebEngineUrlRequestJob` lifetime. A producer/read failure selects the shared failure
latch once: when a stream device exists it emits `ArticleStreamDevice::failed`, which
the handler forwards once as `producerFailed`; a buffered failure without a stream
device emits `producerFailed` directly. Before a reply the failure also selects
`QWebEngineUrlRequestJob::RequestFailed`; after a reply the notification and failed-device
read contract wake the loader without a second `fail()` or `reply()`.

`DataRequestReader`, its factory and `makeDataRequestReader()` are production interfaces
in `articleschemes.hh/.cc`, not replacements for dictionary engine APIs. The two-argument
handler constructor delegates to the factory overload using `makeDataRequestReader()`
for each job. An injected factory is retained by the GUI-owned handler and invoked
immediately after resource creation, before initial inspection or signal-driven copies;
its returned non-null reader is held by shared ownership in the job record and, for a
live response, the device state. A null reader fails once with `RequestFailed` before
reply. The factory and the request's existing `sptr` remain GUI-only; no IO operation
copies, releases or dereferences that producer handle.

Readers are plain C++ objects with no QObject affinity. `fullData` and `dataSlice`
are invoked only on GUI, receive the producer by reference for that call, and must not
retain or capture it. `copyStreamBytes` may run on Chromium's IO thread (or the consumer
thread in direct-device tests); it receives only a destination, a stable byte span in
the device's locked buffer and a nonnegative count. It must not access a producer,
handler, job, GUI object or GUI connection. The reader remains alive through every
operation and until the job-owned device releases its state, even after its handler
record is erased. Reader destruction may occur on either thread and must not require
GUI work or release a producer; any mutable reader state must be separately synchronized.

The production forwarding implementation returned by `makeDataRequestReader()` is
stateless: `fullData` copies the real `request.getFullData()` vector into an owning
QByteArray after successful completion; `dataSlice` forwards directly to the real
`request.getDataSlice(offset, size, destination)`; `copyStreamBytes` copies the supplied
span with `std::memcpy` for a positive count and has no producer dependency. The real
getters are nonvirtual (`dictionary.hh:191–196`, `dictionary.cc:119–140`); defining
same-named fixture methods cannot intercept them. These reader operations may propagate
an existing request exception or a substituted reader's exception only into the enclosing
production try/catch specified in §3.1. Copy operations publish no progress on an
exception; offsets advance only after a successful return.

Test support supplies a reader factory and throwing reader as specified in §6.1.
Failure flags, setters, one-shot throwing policy and probe counters exist only in
`tests/support/dictionary_stub.hh/.cc`; production readers and handler/device branches
contain no forced-failure switch, environment hook or test-type check. Normal application
construction always installs the stateless forwarding reader.

The interceptor atomically loads a `shared_ptr<const ArticleRequestPolicy>` per callback;
no live config, dictionary method, QFile lookup or widget read is permitted there.

### 2.7 Bridge and frame interfaces

```cpp
class ArticleBridge : public QObject {
  Q_OBJECT
public:
  using Result = std::function<void(QVariant const &)>;
  ArticleBridge(QWebEnginePage * page, QObject * viewParent);
  ~ArticleBridge() override;
  int generation() const noexcept;
  void reset();                       // increments, abandons operations, reinstalls scripts
  void run(QString const & script, Result callback = {});
  void runInFrame(QWebEngineFrame const & frame, QString const & script,
                  Result callback = {});
public slots:
  void onJsActiveArticleChanged(QString id, int generation);
  void onJsReadingState(QVariantMap state, int generation);
  void onJsFrameEvent(QVariantMap event, int generation);
signals:
  void activeArticleChanged(QString const & id);
  void readingStateChanged(QVariantMap const & state);
  void frameEvent(QVariantMap const & event);
  void generationChanged(int generation);
  void bridgeReady(int generation);
};
struct WebsiteFrameKey {
  QVector<int> childPath;  // indices in the native children() tree at verified discovery
  QStringList namePath;   // validated parent-DOM names; may contain empty/duplicate names
  QString docId;
  int generation = 0;
};
class WebsiteFrames : public QObject {
  Q_OBJECT
public:
  WebsiteFrames(QWebEnginePage * page, ArticleBridge * bridge, QObject * parent = nullptr);
  ~WebsiteFrames() override;
  void reset(int generation);
  void discover();
  void wordAtPoint(QPointF const & viewPoint, std::function<void(QString const &)> callback);
public slots:
  void receive(QVariantMap const & message);
signals:
  void frameText(QString const & dictionaryId, QString const & text, bool contextMenu);
  void frameUnavailable(QString const & dictionaryId, QString const & explanation);
};
```

Bridge owns its QWebChannel, registers itself as `articleview`, and borrows the page.
Every callback captures QPointer guards plus generation/operation number. Frame values
are short-lived handles revalidated before use; no invented frame signals/accessors.
`WebsiteFrames` sends all JS through the bridge queue, including frame-local scripts.

New resource files are `scripts/article-bootstrap.js`, `scripts/article-dom.js`,
`scripts/website-frame.js`; expose them as `:/scripts/article-bootstrap.js`,
`:/scripts/article-dom.js`, `:/scripts/website-frame.js` through `resources.qrc`.
Their public JS APIs are specified in §4.6–4.7.

### 2.8 History and printing interfaces

```cpp
enum class NavigationKind { NewLookup, LinkClick, Back, Forward, Reload, Redirect };
struct ArticleState {
  QString currentArticle;
  double sx = 0, sy = 0;
  bool restoreScroll = true;
};
class ArticleHistory {
public:
  void reset();
  void commit(QUrl const & url, int itemIndex, NavigationKind kind);
  bool stateFor(int itemIndex, ArticleState * out) const;
  void setCurrent(int itemIndex, ArticleState const & state);
  void truncateAfter(int itemIndex);
};
class PrintPipeline : public QObject {
  Q_OBJECT
public:
  enum class State { Idle, Generating, LoadingPdf, Ready, Previewing, Printing,
                     Failed, Cancelled };
  PrintPipeline(QWebEngineView * view, ArticleBridge * bridge, QObject * parent = nullptr);
  ~PrintPipeline() override;
  State state() const noexcept;
  void snapshot(QPageLayout const & layout);
  void preview(QPrinter * printer);      // only Ready; borrows printer for dialog lifetime
  bool paint(QPrinter * printer);        // synchronous; false on paint/printer failure
  void exportPdf(QString const & destination, QPageLayout const & layout);
  void cancel() noexcept;
signals:
  void ready(int pageCount);
  void failed(QString const & message);
  void cancelled(QString const & message);
};
```

History owns entries keyed by integer index, never QWebEngineHistoryItem instances.
Print owns the PDF QByteArray, an opened QBuffer, and QPdfDocument; destroy/close the
document before the buffer/bytes. No QPrinter pointer is retained after dialog/action
completion. Preview geometry still uses existing configuration.

### 2.9 Profile/auth and save interfaces

```cpp
class ProxyAuthAdapter : public QObject {
  Q_OBJECT
public:
  ProxyAuthAdapter(Config::Class & config, QWidget * dialogParent, QObject * parent = nullptr);
  void authenticate(QUrl const & requestUrl, QAuthenticator * auth, QString const & proxyHost);
};
class OriginAuthAdapter : public QObject {
  Q_OBJECT
public:
  OriginAuthAdapter(QWidget * dialogParent, QObject * parent = nullptr);
  void authenticate(QUrl const & requestUrl, QAuthenticator * auth);
};
class ArticleProfileController : public QObject {
  Q_OBJECT
public:
  ArticleProfileController(Config::Class & config, ArticleNetworkAccessManager & resources,
                           QWidget * windowParent);
  ~ArticleProfileController() override;
  QWebEngineProfile * profile() const noexcept; // borrowed
  void attachPage(QWebEnginePage * page);
  void applyPreferences();
  void clearForExit();
signals:
  void exitCachesReady(bool timedOut);
};
class SaveArticle : public QObject {
  Q_OBJECT
public:
  enum class State { Idle, Capturing, Collecting, Downloading, Publishing,
                     Complete, Cancelled, Failed };
  SaveArticle(ArticleView * view, ArticleNetworkAccessManager & resources,
              QObject * parent = nullptr);
  ~SaveArticle() override;
  State state() const noexcept;
  void start(QString const & htmlDestination, bool complete);
  void cancel() noexcept;
signals:
  void saved(QString const & htmlDestination);
  void failed(QString const & message);
  void cancelled(QString const & message);
};
```

Profile controller owns profile, handler and auth adapters. Page-local interceptors
are owned by their pages and attached during `attachPage`. Save controller borrows
view/resources, owns all current downloads/staging paths and retains its own operation
and document generation. `QAuthenticator *` is used only during the auth signal call;
neither adapter stores it for an asynchronous continuation.

### 2.10 Hotkey and platform interfaces

```cpp
struct HotkeyBinding { QString id, description, sequence; };
struct PortalBindingDisplay { QString id, triggerDescription; };
class HotkeyBackend : public QObject {
  Q_OBJECT
public:
  enum class State { Unsupported, Creating, Ready, Binding, Bound, Partial, Denied,
                     Unavailable, Closed };
  using QObject::QObject;
  virtual bool isSupported() const noexcept = 0;
  virtual void bind(QVector<HotkeyBinding> const & bindings) = 0;
  virtual void unbind() = 0;
  virtual State state() const noexcept = 0;
signals:
  void activated(QString const & id, QString const & activationToken);
  void bindingsChanged(QVector<PortalBindingDisplay> const & reported);
  void stateChanged(HotkeyBackend::State state, QString const & explanation);
};
std::unique_ptr<HotkeyBackend> createHotkeyBackend(QObject * parent);
namespace HotkeySyntax {
bool parse(QString const & stored, Config::HotKey * out, QString * error);
bool preferredTrigger(Config::HotKey const & key, QString * trigger, QString * error);
QString serialize(Config::HotKey const & key); // PortableText for a newly edited value
}
class PortalRequest : public QObject {
  Q_OBJECT
public:
  PortalRequest(QDBusConnection const & bus, quint64 generation, QObject * parent = nullptr);
  ~PortalRequest() override;
  void start(QDBusMessage message, QString const & handleToken);
  void cancel() noexcept;
signals:
  void response(quint64 generation, QString const & requestPath, uint code,
                QVariantMap const & results);
  void failed(quint64 generation, QString const & explanation);
};
class PortalBackend : public HotkeyBackend {
  Q_OBJECT
public:
  PortalBackend(QDBusConnection const & bus, QObject * parent = nullptr);
  ~PortalBackend() override;
  bool isSupported() const noexcept override;
  State state() const noexcept override;
  void bind(QVector<HotkeyBinding> const & bindings) override;
  void unbind() override;
  void configure(QString const & activationToken); // v2, existing session only
};
namespace PlatformWindow {
QScreen * screenForPoint(QPoint const & point); // borrowed; primary fallback, null if no screens
QRect availableGeometryForPoint(QPoint const & point); // empty if no screen
Qt::WindowFlags popupFlags(QString const & platformName,
                           Config::ScanPopupWindowFlags configured, bool bypass);
void applyActivation(QWidget * window, QString const & token);
Display * x11Display(); // null unless active QPA is xcb and native interface exists
}
```

`Display` is the Xlib declaration under `HAVE_X11`; a non-X11 build omits that helper
and compiles the existing platform-specific branches. Runtime selection on Linux is
independent of that compile-time macro. Factory returns a unique pointer, parented to
the supplied parent for signal affinity; MainWindow explicitly resets it before its
QObject children are destroyed. Unsupported platforms return a concrete disabled
backend emitting an explanation, not null. No factory exception denotes a missing portal.

X11 wrapper public methods `setGlobalKey(int,int,Qt::KeyboardModifiers,int) -> bool`,
`unregister()` and existing application/session hooks remain for the recorder and
non-Linux platform branches (`hotkeywrapper.hh:63–80`, `:236–255`). X11RecordBackend
maps stable string IDs to recorder integer handles and emits empty activation tokens.

### 2.11 Rewritten view/page public contract

```cpp
class ArticleWebPage : public QWebEnginePage {
  Q_OBJECT
public:
  ArticleWebPage(QWebEngineProfile * profile, QObject * parent = nullptr);
  ~ArticleWebPage() override;
signals:
  void userLink(QUrl const & url, bool newTab);
  void navigationRequested(QUrl const & url, NavigationKind kind);
protected:
  bool acceptNavigationRequest(QUrl const & url, NavigationType type,
                               bool isMainFrame) override;
  QWebEnginePage * createWindow(WebWindowType type) override;
};
class ArticleWebView : public QWebEngineView {
  Q_OBJECT
public:
  explicit ArticleWebView(QWidget * parent = nullptr);
  ~ArticleWebView() override;
  void configure(QWebEngineProfile * profile, Config::Class * config);
  void setSelectionBySingleClick(bool enabled);
  bool isMidButtonPressed() const noexcept;
  void triggerPageAction(QWebEnginePage::WebAction action, bool checked = false);
signals:
  void doubleClicked(QPoint position);
};
```

`configure` is called once after setupUi and before loading; it replaces any default
page with a view-owned ArticleWebPage constructed on the shared profile. Inspect action
opens ArticleInspector. Keep mouse/focus/wheel handling only where it preserves existing
behaviour (`articlewebview.cc:86–178`); install event filters on Chromium's focus child
as necessary, with actual QTest pointer events proving delivery.

ArticleView retains its existing constructor parameters at `articleview.hh:75–85` with
`QWebEngineProfile & profile` inserted **before** `ArticleNetworkAccessManager &`.
All existing public operations/signals not listed as changed retain their signatures.
Changes to its public surface are:

```cpp
// ArticleView members
void showDefinition(QString const & word, QStringList const & dictionaryIds,
                    FtsHighlightSpec const & highlight, unsigned group,
                    bool ignoreDiacritics);
void toHtml(std::function<void(QString const &)> callback); // async replacement
void captureState(std::function<void(ArticleState const &)> callback);
void saveArticle(QString const & destination, bool complete);
void printPreview(QPrinter * printer);
void print(QPrinter * printer);
void exportPdf(QString const & destination, QPageLayout const & layout);
QStringList getArticlesList(); // cached, current generation only
QString getCurrentArticle();  // cached "gdfrom-..."
QString getActiveArticleId(); // cached dictionary ID
bool hasSound();             // cached
// Additional signals
void articlesChanged(QStringList const & dictionaryIds);
void soundAvailabilityChanged(bool available);
```

Remove ArticleViewJsProxy, `attachToJavaScript`, synchronous `toHtml`, WebKit-only
private helpers and FindFlags. `onJsActiveArticleChanged` is now exposed solely on
ArticleBridge with the two-argument signature. `MainWindow::showTranslationFor` and
FTS signal connections using QRegExp (`mainwindow.cc:4769`) change to
`FtsHighlightSpec const &`; use typed connections rather than stale string signatures.
Signal consumers at `mainwindow.cc:2041`, `:2088` update from cache-change notifications.
The UI never blocks waiting for a cache getter.

### 2.12 Test, build and documentation files

New build/support files: `tests/tests.pro`, `tests/testsupport.pri`,
`tests/support/fixtures.hh`, `tests/support/fixtures.cc`,
`tests/support/dictionary_stub.hh`, `tests/support/dictionary_stub.cc`,
`tests/support/http_fixture.hh`, `tests/support/http_fixture.cc`,
`tests/support/fake_portal.hh`, `tests/support/fake_portal.cc`.
Their test-only public interfaces and every testcase/fixture path are in §6.

New executable scripts: `scripts/test-wrapper.sh`, `scripts/verify-test-gate.sh`,
`scripts/check-qt6-only.sh`. Their exact command contracts are in §6.4–6.5.
New document: `docs/wayland-limitations.md`. Update `README.md` for dependencies,
build/test commands, removed switches and documented parity differences. The approved
design remains an authority document, not an implementation file to rewrite.

## 3. Data structures and state machines

### 3.1 Scheme jobs and streaming

Each handler job record is `{job, request, reader, device, connections, replied,
terminal, cancelled, rangeRequested, rangeBounds, failureMessage, failureNotified}`.
`request` is shared using the existing `sptr`; `reader` is the per-job
`shared_ptr<DataRequestReader const>` from §2.6; `job` is guarded with QPointer.
The handler owns the map and GUI-thread connections, not the job itself.
`rangeRequested` is derived from a case-insensitive `Range` field in
`job->requestHeaders()` before reply selection; `rangeBounds`, when satisfiable,
contains the resolved start/end and full final size from §4.4.

| State | Event | Action and next state |
| --- | --- | --- |
| Received | Non-GET, invalid parse or forbidden document host | Fail once; Terminal |
| Received | Valid URL, request construction succeeds | Create the per-job reader, connect GUI producer signals, inspect immediate finished/data/error state; Waiting |
| Waiting | Buffered producer is unfinished, live stream has no initial data and is unfinished, or any Range stream is unfinished; no terminal error | Keep Waiting; no false not-found, successful reply or Range response |
| Waiting | Terminal producer error or caught reader/copy exception is latched, even with partial bytes | Record the message; `RequestFailed`; Terminal |
| Waiting | Buffered kind with no Range completes successfully with size >= 0 | Call `reader->fullData(*request)` inside the handler catch boundary, populate a job-owned full-size QBuffer, open ReadOnly and seek 0, reply once; Replied |
| Waiting | Live stream, no Range, has data or completes successfully empty | Stage initial data through `reader->dataSlice` inside the device's GUI catch boundary, open ArticleStreamDevice ReadOnly\|Unbuffered, reply once; Replied |
| Waiting | Any Range request's producer completes successfully with size >= 0 | Read full bytes through the handler's reader boundary; resolve Range, open and position the full-size QBuffer as below; unsatisfiable → `RequestFailed` without reply; otherwise reply once; Replied |
| Waiting | Completes with size -1 and no terminal error | `UrlNotFound`; Terminal |
| Replied | Producer appends successfully | Device wakes and drains; never reply/fail a second time |
| Replied | Producer error or device read/copy exception | Latch failure, drain existing bytes, wake loader; failed device read returns -1 with `atEnd() == false`; never call `fail()` a second time |
| Any live state | Job destroyed, profile shutdown or loader close | Disconnect, mark/cancel once, release request/map entry; device remains job-owned until job deletion; Terminal |
| Any state | Duplicate completion or queued callback after teardown | Ignore by record identity/terminal latch |

`dataSize() == -1` denotes existence not yet known, not an empty successful resource
(`dictionary.hh:183–206`). `0` after successful completion is empty content. A
nonempty terminal error from `getErrorString()` takes precedence over `dataSize()`:
partial bytes plus an error are never a successful reply, and `UrlNotFound` is selected
only when completion is terminal, `dataSize() == -1`, and no error is latched.
The production read sequence is normative:

1. Create the real DataRequest and its reader on GUI, connect producer signals, then
   inspect immediate state so an instant-finish request still takes the same boundary.
   A nonempty producer error wins over size/not-found and prevents a successful reply.
2. For a buffered kind or any Range field, wait for successful `isFinished()` and
   nonnegative `dataSize()`. Inside the handler's try/catch call
   `reader->fullData(*request)`, retain its owning bytes in a job-parented QBuffer,
   open it ReadOnly and resolve/seek before `reply()`. This is the only full-data
   read at this boundary; direct calls to the nonvirtual getter bypassing the reader
   are forbidden. Exceptions from the reader or associated copy latch failure, emit
   `producerFailed` once and fail `RequestFailed` without a successful reply.
3. For live no-Range media, construct the device with the same reader and a GUI-only
   producer handle. Its initial pump and every GUI `updated`/`finished` pump call
   `reader->dataSlice(*request, offset, count, destination)` for each newly available
   positive slice inside the device's production try/catch. Only a successful copy
   publishes append length/state. A thrown slice is latched before reply selection
   or, after a live reply, through the failed-device path; it cannot escape a Qt slot.
4. Within `ArticleStreamDevice::readData`, hold the device-state mutex and call
   `reader->copyStreamBytes(data, bufferStart + readOffset, count)` for a positive
   backlog read inside that method's own production try/catch. Advance `readOffset`
   only after return. An exception latches failure with no published copy progress,
   returns a failed read (`-1`, `atEnd() == false`), and publishes the single failure
   wakeup/notification after releasing the mutex. This operation receives no request
   and cannot invoke a GUI producer. A test throwing at any other call site does not
   satisfy this boundary's acceptance requirement.

For a Range field the handler, not Qt, resolves bounds against the final byte count S
and positions the QBuffer **before** `reply()`: ordinary `bytes=N-`/`bytes=N-M` seek to
N; suffix `bytes=-K` seeks to `max(0, S-K)`. Its backing bytes and `size()` remain the
entire resource of size S, never a sliced body or remaining-length size. Unsatisfiable
bounds fail with `QWebEngineUrlRequestJob::RequestFailed` before any reply (§5).
No Range or an unparseable Range opens/seeks to 0 and serves the full body with 200;
the no-Range media path retains its live device instead of using this buffer. Failed
open/seek is also `RequestFailed`. Qt may repeat an ordinary positive seek and produce
206/read limits/Content-Range, but the handler must not rely on Qt to position the
buffer, especially for suffixes. The exact resolution cases are in §4.4.

`ArticleStreamDevice` owns a GUI-only producer handle and a shared device state
containing a mutex-protected append-only byte buffer, `readOffset`, and terminal flags
`completed`, `failed`, `cancelled` plus the failure message. The state also retains
its immutable reader dependency. These synchronized values and the reader's isolated
copy operation are the only application state accessed by IO-thread QIODevice methods.
GUI producer slots observe `updated`/`finished`, copy only the newly appended slice
through `reader->dataSlice` into that buffer while holding the device mutex, and
publish terminal state under the same mutex. They never hold the device mutex while
invoking producer cancellation. The IO thread never calls `dataSize()`,
`getDataSlice()`, `getFullData()`, `getErrorString()` or any other producer method,
either directly or through the reader. A read/copy exception is never converted to
not-found or successful EOF.

The device is opened ReadOnly|Unbuffered on the GUI thread before `reply()`. Its QObject
affinity remains GUI and its parent remains the request job; that affinity governs
parenting and queued slots, not the thread used for `read`, `size`, `atEnd`,
`bytesAvailable`, `isSequential` or `close` after handoff. After handoff GUI code does
not call those QIODevice methods. `size() const` always returns 0 for a live stream;
this means the total is unknown, not that the body is empty, including after completion.
`bytesAvailable()` is the unread backlog only. A request with a Range field never
uses this device: the handler waits for completion, resolves the range itself and
serves the full-size, positioned QBuffer instead. The live device ignores Range at
the device level, always reports `size() == 0` and delivers a full 200 body; it never
reads from or shares that fixed buffer. The device remains valid for the job's
documented lifetime and is not independently deleted by the handler.

`cancel()` is GUI-only: it marks `cancelled` under the device mutex, releases that
mutex, and then invokes the producer's GUI-thread cancellation once. An IO-thread
`close()` marks the same cancellation state, releases the mutex, and queues that
GUI-side cancellation continuation; it never dereferences the producer or calls it
while holding the device lock. Job/profile teardown uses the same idempotent path.

| Condition for `readData(data,maxSize)` | Result |
| --- | --- |
| Cancelled | -1; `atEnd() == true` |
| maxSize == 0 and not cancelled | 0, even at EOF or failure |
| maxSize < 0 | -1, invalid caller request |
| Backlog > 0 and copy succeeds | Copy min(maxSize, backlog), advance offset, return copied count even if terminal state is set |
| Backlog > 0 and reader/copy throws | Latch failure once, leave offset unchanged, return -1 with `atEnd() == false`; notify after releasing the mutex |
| Backlog == 0 and failed | -1; `atEnd() == false` |
| Backlog == 0 and producer unfinished | 0; `atEnd() == false` |
| Backlog == 0 and completed successfully | -1; `atEnd() == true` |

`isSequential() == true`; `writeData` always returns -1. `atEnd()` is cancellation or
successful completion after the backlog is drained, never merely zero availability.
A failed stream stays not-at-end after its backlog drains so Chromium takes its read
failure path rather than treating the negative read as successful EOF. This preserves
zero-as-waiting and finished-and-drained EOF while adding a distinct failed terminal
state (SD-06).

GUI appends emit `readyRead` once per observed append. Successful completion emits
`readyRead` once more, then `completed` and `readChannelFinished` once after marking
completion, even when no bytes were appended. A failure emits `readyRead` to wake the
loader and `failed` once, with no `readChannelFinished`; the handler forwards the
message through `producerFailed` on its GUI thread. A GUI pump's `reader->dataSlice`
or buffer copy and the IO read's `reader->copyStreamBytes` have their own enclosing
production catch boundaries described above; either latches the same failure under
the mutex. Emit/queue its wakeup and `failed` only after lock release, then forward
`producerFailed` exactly once on GUI. Already staged bytes remain drainable when a
later copy succeeds; a throwing copy itself returns the failed read without advancing
the offset. Once failure wins, later completion/update callbacks cannot emit
`completed`/`readChannelFinished` or clear failure into successful EOF. The IO thread
never invokes producer methods. Cancellation is idempotent and later producer signals
cannot wake a released device.

### 3.2 Bridge generation and cached state

`generation` starts at 1; outbound operation numbers are monotonic within the bridge.
States are Installing → ChannelPending → Ready; reset from any state abandons that
state and enters Installing for the next generation. Closing enters Closed.

An outbound queue entry contains generation, operation number, optional frame value,
script, callback and completion latch. Exactly one entry is dispatched at a time;
queue dispatch for main-document DOM work waits for bridge-ready and DOM-ready.
Frame helper installation does not depend on the remote frame having a QWebChannel.
A callback validates owner, page, generation and current operation before invoking
user code or dispatching the next item. A stale callback cannot clear a newer in-flight
item. A reset clears pending callbacks and abandons the outstanding operation without
waiting for Chromium to cancel execution.

Before each controlled top-level load: capture outgoing state, reset bridge, clear
cached articles/current article/audio state, publish the new page policy, then navigate.
Do not reset on `loadStarted` after scripts may already have run. Redirects belonging
to that load share its generation. Back/forward restores a newly injected bridge even
when the URL repeats. Set `QWebEngineSettings::BackForwardCacheEnabled` false on every
application page before its first navigation (SD-07); restoring a cached live document
would otherwise bypass DocumentCreation and retain its old facade generation.
On generation exhaustion at signed-int maximum, recreate the page and bridge under
the view before another load; never wrap and accept an ancient integer generation.

Inbound ID/reading/frame events are accepted only for the current generation and
matching current document/frame identity. Emit UI signals only on actual cache changes.
Cached getters return empty/false while a new generation has no data. Page teardown
is explicit before bridge deletion because WebEngine may invoke final callbacks during
page destruction.

### 3.3 History entry lifecycle

Entry data is `{index, finalUrl, navigationKind, hasState, ArticleState}`. URL is
metadata, not the key. Absent state and zero offsets are distinct.

1. Initial blank/new tab clears the engine history and calls reset together.
2. Before leaving a committed document, capture live state and setCurrent on the
   outgoing `currentItemIndex`; §4.10 defines the asynchronous barrier.
3. On successful final `loadFinished(true)`, commit once at the engine's actual
   `currentItemIndex()`. A failed load leaves an absent-state slot; never copy the
   previous entry into an error document. Mirror index positions and engine count,
   allowing absent state slots for unsuccessful loads.
4. A new navigation after going back truncates application entries after the outgoing
   index before associating the new successful item. Back/Forward preserve entries.
5. Reload preserves identity but sets restoreScroll=false; return to top. A later
   user reading-state update can record fresh offsets for a subsequent Back restore.
6. Fragment links scroll in the existing document without asking Chromium to navigate;
   no new engine/application item is created.
7. At a successful Back/Forward destination, restore current article and scroll after
   the layout-settle policy in §4.10. Apply once per generation.
8. Only an actual engine history clear resets the map. Ordinary loads and bridge
   resets never do. Duplicate URLs at different indices remain independent.

### 3.4 Backend selection and portal lifecycle

Select once from `QGuiApplication::platformName()`: `xcb...` → X11RecordBackend;
`wayland...` → PortalBackend; anything else → Unsupported. Log one selection line.
Missing native X11 interface, absent service or version < 1 disables hotkeys visibly.
Never fall back from Wayland to an X11 backend or to clipboard polling.

Portal state stores generation, service owner, session path, `bindAttempted`, desired
bindings, reported subset, active requests, and last displayed terminal reason.
Generation is incremented before cancelling an old session or applying an edit.

| State | Event | Action/next state |
| --- | --- | --- |
| Unsupported/initial | Portal owner/version available and first bind | Create generation; CreateSession; Creating |
| Creating | Response 0 with valid session_handle string | Associate session/generation; ListShortcuts for display; Ready |
| Ready | Valid desired combinations | Set bindAttempted **before** invocation; BindShortcuts once; Binding |
| Binding | Response 0, all valid IDs reported | Replace display subset; Bound |
| Binding | Response 0, proper nonempty subset | Replace subset; Partial; missing IDs visibly unavailable |
| Binding | Empty subset or response code 1/2 | Clear active bindings; Denied with explanation |
| Bound/Partial/Denied | Application changes IDs, enable flags or sequence text | Close requests/session, clear active subset, increment; new Creating |
| Bound/Partial | Configure action, version >= 2 | ConfigureShortcuts existing session; no BindShortcuts call; keep current state until ShortcutsChanged |
| Any live state | Timeout, Session.Closed, service disappearance, malformed reply, method error | Close requests/session if reachable; Unavailable, clear active subset; no retry |
| Any live state | Explicit unbind/shutdown | Cancel requests, Session.Close, Closed |
| Any state | Stale response containing a newly created old session | Close that returned session immediately; do not bind/activate it |

ListShortcuts completion cannot overwrite a more recent BindShortcuts or
ShortcutsChanged result: capture a display-revision counter at invocation. Bind does
not wait for listing display data, so listing cannot delay binding indefinitely.
ListShortcuts validates the response payload as `a(sa{sv})`: every entry has a string
ID, a map of properties, `description` and `trigger_description` are strings when
present, and no duplicate ID. A malformed tuple/map/type or duplicate ID is a protocol
failure. A well-formed ID that is outside the current display policy (the config/UI set
of IDs currently eligible for display, with stable ID mapping and loaded display
records; this policy is independent of the current bindable subset) is historical data
and is ignored for the current display; it is not compared with the current generation's
bind input and does not make that response a protocol error. Pre-bind historical entries may populate
labels/triggers for display only. They never activate an ID, never become part of the
active subset, and never tear down a session merely because an old ID is absent from
this generation's bind input. Only a successful BindShortcuts response can replace the
active subset, and only bound current-session IDs can activate. Ignore timestamp for
focus timing; forward `activation_token` unchanged. Deactivated never triggers another
lookup.

PortalRequest states: Subscribed → Calling → AwaitingResponse → Completed or
Cancelled/TimedOut/Failed. Its 5 s deadline begins before invocation and covers both
method reply and Response. Cancel issues Request.Close once when the path is known;
Close generates no completion expectation. Details of early replies are in §4.12.

### 3.5 Print, save, encoding and config lifecycles

Print: Idle → Generating → LoadingPdf → Ready → Previewing/Printing → Ready.
Empty PDF, loading Error, timeout or printer failure → Failed. Cancel/tab close
from generation/loading → Cancelled and invalidates callbacks. Closing preview
retains Ready until operation replacement or view close. New generation during PDF
creation cancels; a Ready snapshot remains a frozen article snapshot during later
navigation, until replaced or tab closed. Export uses an independent operation ID
and atomic destination publication after successful bytes.

Save: Idle → Capturing → Collecting → Downloading → Publishing → Complete.
Cancel/navigation/tab close before commit → Cancelled; any IO/download failure →
Failed; both clean staging and cancel all retained requests. Commit is the HTML
publication in §4.9; no later cancellation retracts a Complete save.

Encoding: name lookup → valid immutable descriptor name or null; decode-call opens
its private descriptor → convert/recover/flush → close → return. Encoder convert
error closes descriptor before throwing. No partially encoded result escapes.
Config: defaults → parse existing XML → apply additive in-memory migration → normal
existing save timing. Loading never immediately rewrites the configuration file.

## 4. Component contracts

### 4.1 Encoding details (K5, SD-02, SD-03)

Normalize names by trimming ASCII whitespace, ASCII case-folding, and accepting the
following aliases without relying on the host iconv's alias spelling:
UTF8→UTF-8; UTF16/UTF32→UTF-16/UTF-32; Latin1/ISO8859-1/ISO 8859-1→ISO-8859-1;
Big-5→BIG5; Big5-HKSCS→BIG5-HKSCS; Shift-JIS/Shift_JIS/SJIS→SHIFT_JIS;
Windows-1251/CP1251→WINDOWS-1251; EUC-JP/EUCJP→EUC-JP; GB2312 and GB18030
retain those names. Normalize `ISO[ -]?8859[ -]?N` to `ISO-8859-N`.
Probe iconv_open for the requested direction(s); unsupported names return null.
`unicode` in HTML declarations means UTF-8, preserving the explicit old special case
at `website.cc:173`. Never add `//IGNORE` or `//TRANSLIT`.

Use private iconv descriptors converting to/from UTF-32LE scalar bytes. This avoids
assuming Linux wchar_t size/endian matches QString. Translate scalars to UTF-16
without losing embedded NULs or supplementary characters. No automatic BOM is emitted
by `encode` for explicit UTF-8, UTF-16LE/BE or UTF-32LE/BE. Generic UTF-16/32 output
uses the iconv-defined endian/BOM behaviour, pinned separately in fixtures; callers
requiring byte order must request the explicit form.

| Decode condition | Exact recovery/result |
| --- | --- |
| Empty input | Empty QString |
| E2BIG | Grow output buffer, retain descriptor/input offset and retry |
| EILSEQ at input byte n | Append one U+FFFD; consume **one byte**; reset shift state; resume at n+1 |
| Several consecutive bad bytes | One replacement per EILSEQ recovery, not one for the entire run |
| EINVAL at tail | Append one U+FFFD for the remaining incomplete suffix; consume remainder and stop |
| Successful conversion of a multibyte character | Append the complete scalar(s); never split a successful character around replacement |
| End of input | Flush shift state; E2BIG grows/retries; any other flush error appends one U+FFFD and stops |
| Unknown/invalid Encoding value or descriptor-open failure | Decode through private UTF-8 path; warning once for that operation |

This is the executable interpretation of the design's “one invalid sequence” wording:
there is one replacement **per recovery step**. Do not infer charset-specific skip
lengths; e.g. bad UTF-8 bytes `ff ff` produce two replacements, and a truncated valid
prefix `e2 82` produces one. A reset must not emit a second replacement of its own.
Log at most one recovery warning per decode call even if it replaces several bytes.

`decode` handles a matching leading BOM as follows: explicit UTF-8/16LE/16BE/32LE/32BE
strip one matching signature; generic UTF-16/32 lets its converter consume its BOM.
Do not strip U+FEFF in the middle or strip an incompatible BOM when an HTTP charset
has won. BOM-less explicit endian encodings work. UTF-16 surrogate pairs and UTF-32
scalars are validated; malformed values go through the same recovery rules.

Encoder validates the full QString for unpaired surrogates before converting, converts
valid scalars to UTF-32LE, then iconv to target. E2BIG grows; EILSEQ/EINVAL or a
non-reversible conversion reports `exUnrepresentable`; flush errors also throw that
exception. It returns no partial result. Include charset and offending scalar index
in diagnostic context; never substitute `?` or UTF-8 bytes silently.

Per-caller rules are mandatory:

- Website legacy URL substitutions leave the specific `%25GD...%25` placeholder
  untouched on unavailable codec or encoder exception and log once per failed
  substitution. Other successfully substituted placeholders remain substituted.
- EPWING search skips the failing codepage and tries the next (EUC-JP then GB2312);
  exhausting supported codepages is “no match”. No UTF-8 escape fallback enters libeb.
- EPWING display, SLOB text and locale filenames decode with replacement and continue.
- Remove global Qt locale-codec mutation; logging writes explicit UTF-8.

HTML detection returns the **first recognized** candidate in this precedence:
HTTP Content-Type charset → BOM (test UTF-32 before UTF-16 prefixes) → meta charset →
meta http-equiv Content-Type → XML encoding declaration → named fallback → UTF-8.
Attribute/header names are ASCII case-insensitive; allow single/double quotes,
unquoted charset labels, whitespace around `=`, and either meta attribute order.
Search only the first 1024 raw bytes for declarations, using ASCII-compatible tokens;
UTF-16/32 declarations are reached through their BOM first. Ignore unknown/empty charset
candidates with one warning, then continue down precedence. For multiple candidates
at one precedence level use source order. `DetectedBy::Meta` covers both meta forms;
Fallback covers invalid fallback→UTF-8 too. Header explicitly wins conflicting BOM.

`forLocale()` reads `nl_langinfo(CODESET)` after application locale initialization;
if recognized, return it as decode-only. If missing/unrecognized, inspect
`QLocale::system().name()`: C/POSIX → ASCII; any other valid locale name → UTF-8;
if that cannot be recognized → UTF-8. This deterministic fallback is SD-02, not a
claim that a locale name itself is an encoding. `forLocale()` never changes locale
or global codec state and never throws. Calling encode on its decode-only value
throws `exUnrepresentable` with a decode-only reason; existing locale consumers only decode.

### 4.2 LinkRewrite and inline website URL rules

Use QRegularExpression, preserve unmatched bytes/whitespace and original attribute
quotes where a rule does not require existing canonical output. Rewriting is a
single scan over immutable source spans; emitted replacements are not rescanned in
that pass. Common rules are idempotent. A target is absolute if QUrl identifies a
scheme, it begins `//`, or it is a fragment; these targets are left intact unless it
is the explicitly handled bword→lookup format rule.

Shared pattern sources move from `stardict.cc:468–484` (src/link),
`stardict.cc:489–560` (article links), `stardict.cc:572–632` (audio),
`dictdfiles.cc:348–399`/`dictserver.cc:813–887` (canonical lookup href cleanup).
Keep original capture order and replacement backreferences when moving a pattern,
then apply the option/matching rules in §4.3. Avoid a global “rewrite all hrefs” rule.

| Function | Exact common rule, exclusions and example |
| --- | --- |
| applyGdLookup | For existing `<a href="gdlookup://localhost/TARGET">` links, parse the target as a URI rather than encoding the whole suffix. Apply the old markup/entity/whitespace cleanup and one UTF-8 percent-encoding pass to the decoded path headword only; preserve query and fragment boundaries and their decoded key/value semantics. A structured target is never treated as a DICT-style whole-target capture. `<a href="gdlookup://localhost/red&nbsp;fox">x</a>` → `<a href="gdlookup://localhost/red%20fox">x</a>`, while `<a href="gdlookup://localhost/cat?gdanchor=part">x</a>` remains path `/cat` with query `gdanchor=part`. `dict=Known%20Name`, `gdanchor=part` and fragments remain available to the action handler: `gdlookup://localhost/cat?dict=Known%20Name&gdanchor=part#frag` retains path `/cat`, query items `dict=Known Name`/`gdanchor=part`, and fragment `frag` after the common pass. Already percent-encoded path bytes are decoded once before canonical re-encoding, so `%20` does not become `%2520`; applying the helper twice is byte- and component-idempotent. |
| applyBres | Prefix relative img/script `src`, stylesheet/link `href`, and CSS `url(...)` with `bres://DICT/`, retaining relative path spelling. Pattern prefixes from `stardict.cc:468–471` retain tag/attribute captures. `<img src='a.png'>` → `<img src='bres://d/a.png'>`; CSS `url(../a.png)` → `url(bres://d/../a.png)`. Absolute, protocol-relative, data, qrcx and already-qualified application URLs are unchanged. Root-relative `/a.png` is dictionary-root `bres://d/a.png`, never `bres://d//a.png`. |
| applyGico | Qualify only explicit unqualified icon scheme targets `gico:/PATH` or `gico:///PATH` as `gico://DICT/PATH`. Already qualified icons remain unchanged. Generated `gico://DICT/dicticon.png` in `article_maker.cc:697` stays unchanged. It does not turn arbitrary images into icons. |
| applyGdau | For relative `<audio src=...>BODY</audio>` use the existing addAudioLink/wrapper/playsound-icon output from `stardict.cc:609–619`, with `gdau://DICT/NAME`. Preserve BODY, add the playsound image only if BODY lacks an img tag. Absolute/protocol-relative audio is untouched. Existing qualified gdau links are idempotent. |

The format-specific normalization runs before common URI rewriting. A retained engine
rule that creates a lookup URL composes the cleaned headword into the path component
before attaching `dict`, `gdanchor`, query and fragment fields; `applyGdLookup` then
normalizes only that path component. It must never percent-encode `?`, `=`, `#` or the
query/fragment text as part of the headword. Thus StarDict `cat#part` becomes
`gdlookup://localhost/cat?gdanchor=part`, and the common pass preserves path `/cat`
and query item `gdanchor=part`; a second pass produces the same URL. DICT brace
cleanup remains in the engine that recognizes braces, so literal headword punctuation
is cleaned before URI composition rather than relying on common HTML rewriting.

Spec decision SD-04 limits common functions to these rules because the four design
signatures contain no dialect or dictionary lookup callback. Keep these transformations
in their original engines, with tests, before invoking the common helpers:

| Engine | Retained format-specific normalization and expected result |
| --- | --- |
| StarDict | Existing bare/bword article link rule: `href="bword://cat"` → `href="bword:cat"`; `href="cat#part"` → `href="gdlookup://localhost/cat?gdanchor=part"`; `#part` alone unchanged (`stardict.cc:519–547`). |
| DICT/dictd | `{red fox}` → `<a href="gdlookup://localhost/red%20fox">red fox</a>`; phonetic slash text retains its phonetic span, before common lookup sanitization (`dictdfiles.cc:370–373`, `dictserver.cc:839`). The engine cleans brace/entity headword text before composing the path; common `applyGdLookup` never interprets braces in arbitrary HTML and never consumes structural query or fragment fields. |
| Zim | Wiki URL localization, title preference, .html suffix and underscore rules and SLASH/NO_SLASH dictionary lookup stay in Zim (`zim.cc:876–980`). A titled `Pre_Cambrian.html` link becomes the chosen headword lookup with `%20` for spaces. |
| Website inline | Resolve remote references against final reply URL and `<base href>`; do not use dictionary-relative applyBres on remote HTML. |

For inline website mode, replace the four-tag URL matcher (`website.cc:227–300`) with
a tokenizer that preserves text and edits only URL-valued attribute spans. It must
skip comments and script text, recognize case-insensitive names, quotes/unquoted values,
and the first valid `<base href>`. Use `QUrl::resolved` for `src`, `href`, form `action`,
`poster` and each URL in `srcset`; resolve CSS `url()`/`@import` against the stylesheet's
own final URL. Preserve absolute data/javascript/mailto/tel and fragment-only targets.
An absent/empty action resolves to the document URL. Retain inline stylesheet fetching
through website resource requests (existing mapping at `website.cc:288–300`), encoding
the full absolute URL into its existing bres website-resource form, including port.

Examples for base `http://127.0.0.1:43127/dir/page?q=1`:
`../img.png` → `http://127.0.0.1:43127/img.png`;
`?q=2` → `http://127.0.0.1:43127/dir/page?q=2`;
`//127.0.0.2:43128/s.css` retains that host/port with `http:`;
`<base href="/other/">` makes `x.png` resolve under `/other/` on port 43127.
Iframe-mode HTML never enters this rewriter.

### 4.3 Regex migration by pattern family

All migrated patterns are validated at construction; an invalid built-in pattern is a
programming failure caught by tst_regex, logged with pattern/error at runtime, and
fails the current transform rather than silently matching everything. Invalid user
FTS regex is a displayed validation error and does not start a search. Empty user
patterns retain the existing early-return behaviour.

| Family / baseline examples | Conversion contract and pinned examples |
| --- | --- |
| HTML tags/resources, media, CSS and save collector: `stardict.cc:468`, `zim.cc:861`, `website.cc:311`, `mainwindow.cc:3599` | DotMatchesEverythingOption when `.` can scan HTML or multiline content; preserve case flag. Two audio tags must remain two matches; multiline body matches. Avoid new greedy matching across tags. |
| Unicode words/digits, folding and FTS: `ftshelpers.cc:83–89`, `ftshelpers.cc:188`, `folding.cc:672` | UseUnicodePropertiesOption wherever word/digit classes classify text; existing normalization/case folding stays. Cyrillic `привет`, Arabic-Indic `٠`, decomposed accents and CJK/surrogate pairs are fixtures. ASCII-only protocol/ID patterns stay ASCII. |
| Find index/captures/reverse searches | indexIn → match(subject,offset); use capturedStart/End/Length, not a mutable regex's last-match state. A no-match index is -1. Reverse searches enumerate matches then choose the last match whose start is at/before the original limit. Negative offsets are translated to an explicit subject-relative index, never passed through accidentally. |
| exactMatch / anchored filters | Use anchoredPattern and hasMatch, preserving whole-string intent. Pattern `abc` accepts `abc`, rejects `xabc` and `abc\n`. Do not add whole-string anchoring to former indexIn patterns. |
| Empty and zero-width | Explicit empty guard before loops where old caller required nonempty input; globalMatch handles zero-width progression. No loop using only capturedLength as an increment can hang on an empty match. |
| Minimal matching | Use explicit lazy quantifiers for reviewed patterns; InvertedGreedinessOption only for a fixture-pinned direct translation. Preserve existing deliberately greedy quantifiers; do not combine inversion and ad hoc laziness without expected outputs. |
| Wildcards: `btreeidx.cc:198–230`, `ftshelpers.cc:176–180` | Full wildcard matching uses wildcardToRegularExpression with NonPathWildcardConversion so dictionary headwords may contain `/`. For unanchored search/highlight also use UnanchoredWildcardConversion. Keep escape and malformed-bracket expectations from `wildcard.cc:9–107`; retain that helper for headword patterns whose existing escape grammar differs. Pin `*.txt`, `a/b`, `a\*b`, `[!a]`, unmatched `[` and literal backslash. |
| RegExp2/escape grammar: `website.cc:311`, `zim.cc:923`, `ftshelpers.cc:88` | Keep capture numbering when converting non-capturing groups. Convert legacy literal `\xHHHH` tokens to `\x{HHHH}` outside character-escape quoting, rather than interpreting two digits and suffix text. Pin octal `\0DDD`, braces, lookahead, unmatched optional captures and greedy/minimal behaviour individually. The inline website matcher is replaced by the resolver contract, while its pre-change cases remain fixtures. |
| String split/count/list filter | Use the Qt6 QRegularExpression overloads; preserve KeepEmptyParts/SkipEmptyParts, case and Unicode flags. `"a1b22".count("\\d+")` expectation is number of matches (=2 with greedy +), while the approved design's measured count of 3 is pinned with minimal/inverted matching, not asserted for a different pattern configuration. |
| Replacement captures | `\1` in QString replacement remains a backreference. Use captured owning strings or capturedView only within source lifetime; optional absent captures become empty. Never keep a view after the source QString is replaced. |
| FTS protocol/signal parameters | Replace QRegExp with QRegularExpression for matching; carry original pattern, wildcard flag and case separately in FtsHighlightSpec. Recompile when options change; do not try to read a non-existent wildcard property from QRegularExpression. |

Do not make the design's illustrative regex count override the original pattern's
options. Each fixture identifies source file/line, original pattern/options, input,
expected captures/replacement and migrated options. The migration ledger in
`tests/fixtures/regex/cases.json` covers every remaining live pattern family; it
contains no Qt5 dependency and tst_regex consumes fixed expected values.

### 4.4 Scheme matrix and URL parameter contract

All seven schemes register `Syntax::Host`, no port/default port, flags exactly
`SecureScheme | LocalScheme | LocalAccessAllowed | CorsEnabled` before QApplication.
Do not use HostAndPort with -1; do not add FetchApiAllowed or a universal access grant.
CORS-enabled is not an origin-policy bypass. Custom resources are element/media
subresources; this implementation does not add fetch/XHR callers for them.

| Scheme | Job MIME/producer | Navigation/action |
| --- | --- | --- |
| gdlookup | text/html, makeEmptyPage/makeDefinitionFor | Controlled document load allowed; clicked path/headword lookup is handled by ArticleView and refused as engine navigation |
| gdpicture | text/html, makePicturePage on corresponding bres URL | Picture document allowed; never return raw image bytes as its document |
| bres | QMimeDatabase by decoded resource name, fallback application/octet-stream; dictionary getResource | Subresource or explicit context download |
| gdau | Audio bytes; name-derived MIME with octet-stream fallback | User click plays through AudioPlayerFactory, not navigation/fetch |
| gdvideo | Video bytes; name-derived MIME with octet-stream fallback | Existing internal/external media action preserved |
| gico | image/png; generated dictionary 16px icon, existing icon producer | Subresource |
| qrcx | QFile bytes at corresponding `:/PATH`; MIME by resource path | Subresource; localhost host is discarded when mapping to qrc resource |

Reject credentials and explicit ports on application schemes. Hostnames are ASCII
dictionary IDs or `localhost`; compare QUrl canonical hosts, never raw string splits.
Percent-encoded UTF-8 paths survive exactly one decode/encode cycle. Query values use
QUrlQuery FullyDecoded; `+` remains a literal plus, not form-urlencoded space.
Resource names preserve decoded query suffix because existing `fullPath` includes it
(`qt4x5.hh:135–148`). Strip only one structural leading slash.

| gdlookup parameter | Parse/default and dispatch |
| --- | --- |
| host | Empty legacy host or localhost accepted; producer always uses localhost; other hosts invalid |
| blank | Exactly `1` → makeEmptyPage; bypass word/group requirements |
| word | DocumentLoad: trim and require a valid InputPhrase except `blank`; UserAction: use the decoded path headword for a `gdlookup` action or decoded bword form, with `word` only as fallback when the action has no path headword |
| punctuation_suffix | Default empty; construct Config::InputPhrase with word |
| group | DocumentLoad: checked unsigned decimal, required for an ordinary lookup and zero is valid; explicit-dictionary requests may omit it and dispatch with group 0. UserAction: optional checked override; when absent, the action handler inherits the reference view group |
| dictionaries | Comma-separated IDs; for DocumentLoad a nonempty list selects the explicit-dictionary branch, with contexts/muted ignored by the maker exactly as baseline; unknown IDs simply contribute no definition. For UserAction, the reference action context supplies the FTS subset unless the handler explicitly accepts a target subset |
| muted | Comma-separated IDs, remove empty entries, convert to QSet; empty by default |
| contexts | Base64 map, format below; absent → empty; malformed → UrlInvalid, never partial map |
| ignore_diacritics | Exactly `1` → true; false otherwise; preserve explicit-dictionary baseline maker behaviour (the existing branch omits this argument) |
| scrollto | UI-only `gdfrom-ID` restoration/selection target; does not alter maker arguments |
| dict | Decode one target dictionary name and retain it in `Lookup::dictionaryName`; UserAction consumes it through the retained action handler to select `scrollToFromDictionaryId`, while DocumentLoad never passes it to maker arguments |
| gdanchor | UI-only decoded fragment anchor; retain it through UserAction dispatch, where it overrides the inherited `contexts["gdanchor"]`; when producing a document URL, extract it from contexts as in `articleview.cc:433–437` |
| regexp | Original FTS source pattern, UI-only; absent/empty disables highlight |
| matchcase | Exactly `1`; UI-only compile/highlight option |
| wildcards | Exactly `1`; UI-only wildcard conversion, retained independently of compiled regex |

The parser and action handler use this mode/form table; `parse()` performs syntax and
wire validation, while the retained ArticleView action handler resolves reference
context before the final document URL is built:

| Input/form | `Use::DocumentLoad` | `Use::UserAction` |
| --- | --- | --- |
| `gdlookup://localhost/?word=cat&group=G` | `LookupDocument`; ordinary lookup requires the valid `group` (or a nonempty explicit `dictionaries` list) | `LookupAction`; explicit URL fields are retained, but the action handler applies reference context and then validates the resulting document URL |
| `gdlookup://localhost/cat`, `gdlookup://localhost/cat?gdanchor=part`, or `gdlookup://localhost/cat#frag` | Not a complete ordinary document request; reject as `UrlInvalid` rather than inventing a group | `LookupAction` for the first two forms; decoded path is the headword, group may be absent, and query `gdanchor` remains an action field. If a URL fragment is present, existing fragment navigation takes precedence over lookup dispatch and the fragment is never folded into the headword or `gdanchor` |
| `bword:cat`, `bword:/cat`, or `bword://cat` | Invalid for a document load; `bword` is an action form | `LookupAction`; normalize the opaque/path/authority form to headword `cat`, preserving query fields |
| `gdlookup` with `blank=1` | `LookupDocument` empty page; bypass word/group | Invalid as a lookup action unless the retained handler explicitly routes it as an empty-page action |
| `gdpicture`, `bres`, `gdau`, `gdvideo`, `gico`, `qrcx`, external URL | Existing document/resource/action validation for the scheme; no lookup group is invented | Existing action dispatch; no lookup group is required and no lookup context is discarded before that dispatch |

For `Use::UserAction`, a group-less path lookup and every bword form above produces a
`LookupAction`, never a direct maker call. The retained `ArticleView::openLink`-style
handler supplies, in order, the reference `getGroup(ref)`, the reference
`dictionaries` list when the reference is an FTS result, and a copy of the reference
contexts/scroll state. An explicit valid target `group` overrides the inherited group;
when it is absent the reference group is mandatory at dispatch time even though the
URL parser did not require it. The target path/bword headword takes precedence over a
query `word`; a query word is used only when no action headword exists.

Action/query precedence is then: a URL fragment takes the existing page-fragment
navigation path before lookup dispatch and is never interpreted as a headword or
`gdanchor`; for a fragment-free action, a reference FTS dictionary subset selects the
existing explicit-dictionary `showDefinition` branch and takes precedence over ordinary
target group/maker-context routing. Otherwise the target's `dict` maps a known
dictionary name to `scrollToFromDictionaryId` and takes precedence over an inherited
scroll target, while an unknown name leaves the inherited target unchanged. A target
`gdanchor` overrides the inherited `contexts["gdanchor"]`; all other context keys are
copied from the reference. The handler passes the resulting phrase, group, dictionary subset,
scroll target and contexts through the existing `showDefinition`/`lookupDocument`
path, which emits a validated `Use::DocumentLoad` URL with `group` before the scheme
handler sees it. Thus `dict` and `gdanchor` are consumed at the same boundary as
`articleview.cc:1248–1267`, not silently treated as headword text.

Duplicate scalar query keys are invalid (SD-05), avoiding parser/producer ambiguity.
Unknown query keys are ignored only after the mode-specific fields above have been
retained. The explicit-dictionary branch requires word but does not require a valid
group; this preserves `article_netmgr.cc:413–418`. Its UI group query may still be
retained.

Contexts are a QMap<QString,QString> serialized into QBuffer with QDataStream version
`Qt_5_0`, then Base64, then QUrlQuery encoding. Decode with the same version and require
stream status Ok and no trailing bytes; reject invalid Base64. SD-05 pins the version
rather than allowing future default changes. This is an ephemeral URL payload, not a
config-format change. Producer and parser must migrate together.

For gdpicture: host must identify a dictionary, path must be nonempty; preserve query
and fragment through the bres conversion, then call makePicturePage with its fully
encoded URL. No lookup query fields apply. `gdpicture://d/folder/a%20b.png` produces
HTML containing `bres://d/folder/a%20b.png`.

Resource dispatch errors: unrecognized dictionary, final producer dataSize=-1 with no
error or missing qrc resource → UrlNotFound; invalid URL → UrlInvalid; unsupported
method → RequestDenied; caught dictionary exception/producer IO or copy failure,
unsatisfiable byte range, or failed buffer open/seek → RequestFailed. A terminal
producer error wins over not-found and over any partial `dataSize()` before reply.
GET with successful zero-byte payload and no satisfiable-range requirement is success.
Scheme jobs use one reply or one fail, never both.

Streaming policy is gdau/gdvideo. A no-Range live stream replies as one full-body 200
response with `ArticleStreamDevice::size() == 0`; `bytesAvailable()` is only current
backlog, and the handler/response never advertises a partial length, Content-Range or
fabricated 206. This device ignores Range at the device level and never uses the
completed QBuffer described below. The handler recognizes a case-insensitive `Range`
field through `requestHeaders()`; any such field, including an unparseable value,
selects completion buffering for that request. All other kinds already buffer.

For every buffered response, obtain the full successful resource bytes through
`DataRequestReader::fullData` (§3.1). The handler parses a single byte range with the
same acceptance as the pinned Qt/Chromium loader, using checked nonnegative decimal
qint64 positions/lengths; multiple ranges, malformed/invalid forms and numeric overflow
are unparseable/unsupported and are ignored as a Range selection. No private Chromium
API is called. Resolve against the final resource size S, then open the job-parented
QBuffer ReadOnly and perform the following seek before `reply()`:

| Range selection | Resolution and buffer position before reply | Delivered response |
| --- | --- | --- |
| No Range, or unparseable/unsupported Range | Seek 0; retain all S bytes and `size() == S` | 200, full body; no Content-Range |
| `bytes=N-`, with `0 <= N < S` | Start N, end S-1; seek N; retain `size() == S` | Qt supplies 206, `Content-Range: bytes N-(S-1)/S`, length S-N |
| `bytes=N-M`, with `0 <= N <= M` and N < S | Start N, end `min(M, S-1)`; seek N; retain `size() == S` | Qt supplies 206 and matching resolved Content-Range/length |
| `bytes=-K`, with K > 0 and S > 0 | Start `max(0, S-K)`, end S-1; seek the resolved start; retain `size() == S` | Qt supplies 206 and matching suffix Content-Range/length, including all S bytes when K >= S |
| Parsed range with N >= S, or any parsed valid range against S == 0 | Unsatisfiable; do not reply or expose any buffer | Fail once with `QWebEngineUrlRequestJob::RequestFailed` per §5; never a truncated 200 |

The **handler is responsible for positioning**, including ordinary and suffix
ranges; it does not delegate this step to Qt. Keep the full backing bytes and full
resource `size()` after seeking; do not copy only the selected subrange or report
remaining bytes as the resource size. Qt uses that size to resolve bounds and supply
the response metadata/read limit, and may repeat a positive ordinary seek. Its pinned
loader does not seek a suffix after resolving bounds, which is why position 0 would
incorrectly deliver prefix bytes under suffix headers (§1.4). A failed open/seek is
`RequestFailed` without reply. A Range request never uses a growing sequential device;
no-Range media retains its zero-size live device and 200 full-body behaviour. Article
HTML and icons continue to finish into QBuffer (SD-06).

### 4.5 Navigation and actions

ArticleWebPage allows only explicitly issued document loads plus their redirects and
back/forward traversal. Main-frame user links dispatch exactly once to the retained
ArticleView action handler, which calls `ArticleUrls::parse(url, Use::UserAction)` for
`gdlookup` and every bword form. It supplies the reference group/FTS subset/contexts
described in §4.4, applies `dict` and `gdanchor`, then routes the result through the
validated document URL path; it never asks a group-less action URL to satisfy the strict
DocumentLoad group check on its own. `gdlookup` path/headword links become lookups unless they carry a URL fragment, which
uses the existing page-fragment navigation path; gdpicture becomes a picture load; gdau/audio URLs play; resource/save actions retain
existing handlers; external http/https/mailto/tel links open through desktop services
unless the existing website context rule directs an in-frame navigation. Website
subframe forms and self-navigation stay in Chromium.

Middle-click/modifier-open routes to the application's existing new-tab signal; a
`createWindow` request uses a temporary page on the shared profile solely to capture
its first target, emits userLink(newTab=true), and destroys the temporary page. It
must not create a hidden independent browser or bypass dictionary context. Context-menu
state copies `lastContextMenuRequest()` fields during the menu event, then asynchronously
obtains enclosing text; never retain the ephemeral request pointer. User action is
resumed only if its page generation and context position still match.

Find bar uses page findText with FindBackward/FindCaseSensitively as needed. Increment a
query ID for **every** text/options change or close, stamp callbacks with query ID and
document generation, ignore old completions, display activeMatch/numberOfMatches and
not-found from QWebEngineFindTextResult. Empty text clears matches. FTS highlighting
uses the separate JS match index and never relies on a removed HighlightAllOccurrences
flag. Zoom uses WebEngine zoomFactor and frame coordinate conversion in §4.7.

### 4.6 JS bootstrap, inbound slots and DOM helpers

Embed the installed `qwebchannel.js` source (available to Qt as
`:/qtwebchannel/qwebchannel.js`, readable source at `/usr/share/qt6/webchannel/qwebchannel.js`)
into one top-document bootstrap source with the facade and setup code, so its order
is deterministic. Do not use asynchronous network script loading.

| Script | Injection/world/subframes | Responsibility |
| --- | --- | --- |
| gd-bootstrap-G | DocumentCreation, MainWorld, false | Set generation; install stamped facade before parser scripts; include qwebchannel.js; initialize channel and drain queued callbacks |
| gd-dom-G | DocumentCreation, MainWorld, false | Define pure DOM helper functions immediately; defer document listeners needing DOM until DOMContentLoaded |
| gd-frames-G | DocumentCreation, MainWorld, true | Install frame-local helpers/listeners and parent message relay in every document, including after iframe navigation |

Remove the previous generation's three named QWebEngineScript objects and insert
replacements before top-level navigation. Do not clear unrelated application scripts. No bridge
transport is exposed in remote subframes; those use postMessage only.

Bootstrap semantics, including after readiness (SD-07):

```js
(() => {
  const generation = /* C++ substituted signed integer */;
  const pending = [];
  let real = null;
  const forward = (method, args) => {
    if (real) real[method](...args, generation);
    else pending.push({method, args});
  };
  window.__gdDocumentGeneration = generation;
  window.__gdChannelReady = false;
  window.articleview = {
    onJsActiveArticleChanged(id) {
      forward("onJsActiveArticleChanged", [String(id)]);
    },
    onJsReadingState(state) {
      forward("onJsReadingState", [state]);
    },
    onJsFrameEvent(event) {
      forward("onJsFrameEvent", [event]);
    }
  };
  // qwebchannel.js source executes here before the following statement.
  new QWebChannel(qt.webChannelTransport, channel => {
    real = channel.objects.articleview;
    for (const call of pending.splice(0))
      real[call.method](...call.args, generation);
    // The facade stays installed: every later inline/helper call is stamped too.
    window.__gdChannelReady = true;
    if (window.gdOnBridgeReady) window.gdOnBridgeReady();
  });
})();
```

Generated article_maker one-argument calls remain unchanged. The wire slot is exactly
`ArticleBridge::onJsActiveArticleChanged(QString id, int generation)`; no default
argument or legacy overload accepts an un-stamped call. Validate ID starts `gdfrom-`
and is in the current generation's article list before updating active dictionary.
During parser-time list construction, queue accepted-generation IDs until the list
snapshot arrives, then reconcile them in order. Similar wrapper forwarding stamps
reading and frame messages; the raw C++ channel object is a private bootstrap variable.

`gdOnBridgeReady()` belongs to gd-dom-G and is idempotent per document. Call it after
installing the DOM helpers, on DOMContentLoaded, and from the channel callback. It
returns until both `window.__gdChannelReady` and DOM readiness are true; this handles
the channel callback arriving before the helper definition. Then it sends exactly
one fresh `{kind:"ready",currentArticle,sx,sy,articles,hasAudio}` snapshot through
`window.articleview.onJsReadingState`. The facade supplies the generation; no C++
outbound queue is needed to deliver this inbound readiness event. ArticleBridge
validates the generation and snapshot, updates caches, enters Ready, emits bridgeReady
and starts its queue. Later reading snapshots use `kind:"state"` with the same fields;
unknown kinds are rejected and a second ready cannot reset the queue. Reading-state
updates are debounced 100 ms on scroll/active-article change. Callers supply fresh
plain-value objects and do not mutate them after forwarding. Each top-document
message includes its generation. List/audio snapshot derives from existing
`gdArticleContents`, `gdCurrentArticle` and `gdAudioLinks` producers
(`article_maker.cc:182`, `:637`, `:679`).

| Public JS helper | Input/result and algorithm |
| --- | --- |
| gdWordAtPoint(x,y) | CSS viewport coordinates; return string. Use caretPositionFromPoint where available, otherwise caretRangeFromPoint, then walk adjacent text nodes within the enclosing text block; expand using Unicode letters/marks/numbers with internal apostrophes/hyphens; whitespace or nontext returns empty. Preserve previous selection unless explicit select action requested. |
| gdSaveSelection() | Return opaque JS-local token saving cloned Range(s) plus current generation; empty selection is a valid token. No DOM nodes cross QWebChannel. |
| gdRestoreSelection(token) | Restore only ranges still attached to this document and matching generation; otherwise no-op. |
| gdHighlightAll(words,matchCase) | Search text nodes excluding script/style and existing controls/marks; preserve DOM structure; return match count and store ordered ranges. Accept compiled/precomputed FTS match spans rather than interpreting PCRE as JS RegExp when syntax differs. |
| gdClearHighlights() | Remove only GoldenDict marks/ranges, normalize affected text nodes, clear match index. |
| gdScrollToHit(i) | Bounds-check, activate corresponding dictionary, scroll range into view; return false if absent. |
| gdFrameRect(name) | Parent-document lookup by iframe name/id; return CSS viewport `{x,y,width,height,borderLeft,borderTop}` or null. No child DOM read. |
| gdMeasureFrame() | Frame-local shape `{docId,height,width}` from document/body scroll/offset extents; finite positive CSS values only. |
| gdCaptureState() | `{currentArticle,sx:scrollX,sy:scrollY}`; generation is attached by bootstrap wrapper. |
| gdRestoreArticleState(state) | Activate recorded article; apply settle loop in §4.10; return completion state, never block GUI. |

For FTS PCRE results, extract the normalized visible text with a text-node offset map,
compute spans in C++ using QRegularExpression, then apply those spans against the same
text snapshot/version in JS. If the DOM changed, recapture and retry once within the
current generation; if it changed again, clear highlights and report “Article changed;
run the search again.” This preserves the migrated Unicode/PCRE semantics without
assuming JS RegExp implements Qt's pattern language. Selection save/restore brackets
this operation. tst_article_bridge and tst_regex pin offsets including surrogate pairs.

### 4.7 Website frames, protocol, measurements and coordinates (K6)

Website iframe output must set **both** `id` and `name` to `gdexpandframe-DICTID`;
current output only supplies id (`website.cc:499`). Keep the actual http(s) src URL,
port, form base, cookies and normal remote origin. Child traversal starts at
`page()->mainFrame().children()` and recursively enumerates children; identify named
website frames and their descendants. No QWebEngineFrame load-finished signal exists.

Each injected frame helper creates a new random string docId, installs once per
frame document, and waits for DOMContentLoaded before its first ready. It listens for
click/contextmenu, retains enclosing link text in `gdLastUrlText`, defines word/measure
helpers and a parent-message relay, and observes height changes. A subsequent iframe
navigation runs the same DocumentCreation helper afresh with a new docId. If the page
sets window.name, correlate using the parent's iframe name/id rather than trusting
the changed window.name alone.

Messages are structured objects, never executable strings:

```text
{ gdFrameEvent: "ready" | "click" | "context" | "measure" | "resize",
  generation: G, name: "gdexpandframe-DICTID",
  path: [{name: childName, index: parentWindowFramesIndex}, ...],
  docId: string, text: string, height: number, width: number }
```

Only fields relevant to the event are required. The frame posts to `window.parent`;
target origin `*` is necessary because the application parent is a custom origin.
A parent listener validates event.source by comparing it with the contentWindow of
an actual child iframe element, adds its own verified path segment (parent-DOM
name/id and matching index in window.frames) and relays upward. Empty/duplicate
nested names are allowed; child-supplied ancestor segments are never trusted. The
top listener derives the root dictionary name from its own iframe element, accepts
only these actual child routes, validates types/finite measurements, generation and
dictionary membership, then forwards through the stamped bridge. Cap forwarded text
at 64 Ki UTF-16 units, path depth at 32 and positive measured height at 2^24 CSS pixels.
Ignore unknown events/malformed input. No direct cross-origin DOM access is attempted.

On ready, rediscover the current native frame tree and read frame-local docId through
the bridge in current candidate frames. Require a unique matching nonce, a valid
frame value and unchanged native child-tree path/value equality when its callback
returns. Record that native index path separately from the validated DOM name path;
window.frames ordering is not assumed to match QWebEngineFrame::children ordering.
Bind `{generation,childPath,namePath,docId}` only after those checks. A replacement or
sibling reorder invalidates the discovery and causes rediscovery. Messages from an
old docId cannot reset a newer measurement run. Names alone never identify a frame.

A measure request is sent by a queued frame-local runJavaScript call that invokes
`gdMeasureFrame()` and posts a `measure` object. Reporting back uses postMessage;
C++ never reads a cross-origin DOM from the parent. On every ready: request an immediate
sample, then every 250 ms, applying each valid height to the parent iframe's height
and setting display=block. Stop after three equal consecutive heights or 10 s from
that run's start. Resize reports re-arm a bounded run only for the current docId and
only when height differs; assigning the same height cannot create a feedback loop.
New ready always re-arms. A zero/invalid sample waits, never hides already visible content.

If there is no valid ready/measurement by 10 s, show an inline placeholder explaining
that the site could not be framed, log once, and offer the existing inline-mode
preferences hint. Do not claim to know X-Frame-Options specifically from silence;
Chromium may expose the framing error only in console. tst_website_frames includes
actual X-Frame-Options and CSP frame-ancestors fixtures to prove the visible fallback.
Header removal applies only to retained self-fetched content, never third-party frames.

Coordinates use logical view pixels and CSS viewport pixels, never device pixels.
For zoom z, top parent CSS point is viewPoint/z. For each containing iframe, subtract
its CSS boundingRect x/y **and content-border offsets**, then continue in that child's
CSS viewport. Equivalently `(viewPoint - z*rectOrigin)/z` at the first boundary. Rect
values come from the parent document; children supply their own nested rects through
frame-local script. Hit-test inside the deepest valid containing frame with
`gdWordAtPoint`. Rects are viewport-relative, so do not add scroll offsets twice.
At 200% zoom a 100-logical-pixel offset inside the frame becomes 50 CSS pixels.
A frame's real remote origin is never changed to make these calculations possible.

### 4.8 Network policy, profile, proxies and authentication

Evaluate policy for each request on its page interceptor; install a new immutable
policy snapshot before navigation, containing only dictionary hosts contributing to
that page. Never union every enabled dictionary globally. On live preference changes,
replace each page's snapshot. Host input is QUrl canonical lowercase DNS/IP, with a
single trailing DNS dot removed. An absent/opaque initiator has no application privilege.

SD-09 defines same-site in terms of the current `getHostBase` algorithm
(`article_netmgr.cc:213–239`): retain its short-final-label heuristic; compare equal
bases, equal hosts, or a DNS-boundary subdomain suffix in either direction. IP literals
match by exact address only; empty hosts never match; `evil-example.org` is not a
subdomain of `example.org`. Ports do not affect this host-based preference. This is
compatibility host blocking, not a replacement for Chromium's origin/CORS enforcement.
Pin `.co.uk`, unrelated hosts, IPv4, IPv6 and malicious suffix examples.

Let R=request host, F=firstPartyUrl host; I=initiator host when initiator scheme is
http/https, otherwise F. Rules are applied in this order:

| Request/context | Action |
| --- | --- |
| Invalid/unsupported application URL | Deny; handler also validates |
| bres/gico/gdau/gdvideo/qrcx or controlled gdlookup/gdpicture | Allow dispatch; external-content flag never blocks internal resources |
| file URL with precomputed adjusted path | redirect to snapshot's fixed URL; otherwise allow engine normal file handling |
| data/about/blob subresources | Allow normal Chromium policy; no custom privilege is added |
| http(s), preference off | Allow |
| http(s), preference on, absent/opaque initiator | Allow only sameSite(R,F) |
| http(s), preference on, concrete initiator | Allow if sameSite(R,I) or sameSite(R,F) |
| http(s), preference on, application initiator gdlookup/bres/gico | Also allow R belonging to this page's dictionaryHosts (equal or DNS-boundary subdomain) |
| All other external subresource cases under blocking | block(true), once-per-URL/generation diagnostic |

Remote frame A's own CSS/image/fetch thus succeeds via I=A; A→dictionary B is blocked
even if B contributes another article on the same page. Application document→its own
dictionary website is permitted through the per-page set. A missing initiator must
not acquire that set merely because F is gdlookup.

File fix-ups preserve `ArticleMaker::adjustFilePath` intent (`article_netmgr.cc:338–353`).
Precompute mappings on GUI from known emitted file URLs before installing the page
policy; `article_maker.cc` resource generation and snapshot extraction report these
URLs. For a dynamically encountered missing mapping, queue a GUI resolution and retry
through a later controlled URL update if available; interceptor itself never blocks
on a GUI call or filesystem scan. It cannot redirect an in-flight request after returning.

Profile storage name is `goldendict-articles`; cache path is
`Config::getNetworkCacheDir()/webengine/cache`, storage path the corresponding
`webengine/storage`. Set NoPersistentCookies (SD-10); session cookies still allow
real-origin frame behaviour, while existing config stores no new web credentials.
Explicitly enable local-content access to remote/file URLs on article pages using
the verified QWebEngineSettings attributes, subject to this interceptor and Chromium.
Remove plugin preference effect with visible text that WebKit browser plugins are
unavailable; retain the stored value without pretending Chromium supports them.

Configure the application proxy before constructing the profile and on preferences
changes. No Chromium proxy CLI flag, profile-specific proxy method or restart is used.
NoProxy means explicit QNetworkProxy::NoProxy, not a default/system proxy. System mode
queries systemProxyForQuery; an empty result becomes NoProxy and logs a reason.
HttpConnect→HttpProxy; Socks5→Socks5Proxy; HttpGet→HttpCachingProxy, with the UI stating
Chromium treats it as an HTTP proxy. Retained QNAM requests share the same application
proxy preference but independently resolve/authenticate.

Positive cache MiB enables DiskHttpCache and sets `min(INT_MAX, qint64(mib)<<20)`;
non-positive selects NoCache. Do not call setHttpCacheMaximumSize(0) to disable it.
Apply equivalent limits to retained QNAM's QNetworkDiskCache. User agent derives from
the profile's default HTTP UA and existing hideGoldenDictHeader setting
(`mainwindow.cc:1339–1343`, `article_netmgr.cc:361–367`), using setHttpUserAgent for
WebEngine; no new user-agent config key is invented.

ProxyAuthAdapter is wired only to proxyAuthenticationRequired; it reuses saved proxy
credentials or the existing proxy dialog and writes only proxy preference fields.
OriginAuthAdapter is wired only to authenticationRequired, labels scheme/host/port and
realm, accepts credentials for that call, and never reads/writes proxy fields.
Cancellation leaves authenticator unset. No origin password is persisted or logged.
Both adapters operate during signal delivery; a modal credential dialog may run its
normal UI loop, but is not used to simulate synchronous WebEngine results.

Exit cache barrier: disable new lookups/download actions, cancel active page pipelines,
clear QNAM cache synchronously, connect clearHttpCacheCompleted **before** invoking
clearHttpCache, then wait asynchronously until signal or 5 s deadline. Emit one
exitCachesReady and proceed to page/profile destruction even on timeout, logging the
incomplete clear. Repeated quit requests join the same barrier. Do not block Qt's
signal delivery or ignore a session-manager exit request while secretly still running.

### 4.9 Save transaction and resource collection

Keep “Complete” and “HTML Only” choices and `.html` save destination from
`mainwindow.cc:3659–3678`. Capture with page.toHtml callback, stamped with generation.
Filter the same resource families from the collector at `mainwindow.cc:3599–3695`,
using immutable source spans, QRegularExpression and QUrl decoding. Resolve resource
URLs before collecting; deduplicate by canonical encoded URL, not only visible text.
Retain query distinction; map query suffixes to stable `_qN` filenames as before.
Reject destination resource paths that escape the staging directory after cleaning.

SD-12 publication algorithm:

1. Choose a unique operation ID; create a staging directory in the destination's
   parent filesystem. A complete save downloads into its `assets` subtree and
   rewrites HTML to a unique final sibling `<stem>_files-<operationId>`.
2. Hold all DataRequests/QNetworkReply cancellation handles until success/error.
   A single failed required resource aborts the save with its URL/reason; no silent
   success for an incomplete “Complete” article.
3. After every download and write succeeds, rename the staged asset directory to
   that unique final sibling. Same-filesystem directory rename publishes assets
   atomically without overwriting an existing resource directory.
4. Publish the final UTF-8 HTML through QSaveFile::commit, with direct-write fallback
   disabled. This is the transaction commit point: readers see the old complete
   HTML or new complete HTML referring to already present resources.
5. If HTML publication fails/cancels before commit, remove this operation's new
   resource directory and staging directory. Preserve any preexisting HTML/assets.
   Once committed, emit saved exactly once and retain referenced resources.
6. HTML-only skips the asset directory and commits only HTML. PDF export similarly
   commits successful snapshot bytes with QSaveFile.

This preserves the existing file choice while resolving why two sibling outputs
cannot be committed by one ordinary rename. Cancellation/navigation/tab close before
commit aborts every download, invalidates callbacks, removes only owned paths and
shows “Save cancelled because the article changed/closed.” Starting another save
cancels the previous operation before creating new staging state. Crash-orphan cleanup
of immutable resource versions is outside the transactional cancellation guarantee;
no background garbage collector is added in this migration.

### 4.10 Outgoing capture, history restore and print snapshot

Navigation coordinator is the single entry for new lookup, user link, back, forward
and reload. It records intended NavigationKind before invoking an engine action.
Before leaving a successful current document, call captureState through the bridge,
then setCurrent on the **outgoing** index, then reset/install scripts and navigate.
A 500 ms deadline uses the most recent 100 ms-debounced reading snapshot if the
renderer never replies, logs capture timeout and continues (SD-11). Closing the view
abandons the pending continuation. Rapid navigation requests replace the pending
intent; do not perform two engine actions when a previous capture finishes.

`acceptNavigationRequest` refuses an uncontrolled main-frame user navigation and
queues the coordinator action after returning. An explicitly reissued navigation has
one consumed permit, avoiding recursive interception. Redirects in that navigation
are allowed under the same generation; do not capture the outgoing state twice.
Fragment-only links use JS anchor scrolling, no engine item. Scroll snapshots include
finite double offsets; NaN/Infinity is rejected.

At destination success, commit once using currentItemIndex/count. Restore article
selection and, when restoreScroll is true, requestAnimationFrame until scrollHeight
is unchanged across two consecutive samples, at most 10 frames or 500 ms. Apply the
recorded scroll on that first stable sample, otherwise at the bound; generation-check
every iteration. Reload sets top/restoreScroll=false. Test image/frame-induced growth
inside the bound; later content can still shift layout as documented by the bounded rule.

Print begins with the selected view captured at action invocation, not a later
`getCurrentArticleView()` during paint. Call view.printToPdf(callback, layout), stamp
operation/generation, disable duplicate print actions during Generating/LoadingPdf.
Empty bytes or no callback within 30 s fail visibly. Copy bytes into controller-owned
QBuffer, open ReadOnly, connect statusChanged, call QPdfDocument::load(QIODevice *),
and inspect current status as well as signals to handle immediate Ready/Error.
Only Ready with pageCount>0 enables/opens preview or direct printing.

`paintRequested(QPrinter *)` calls synchronous PrintPipeline::paint. Set printer
full-page mode consistently, obtain printable pixel rect at printer resolution,
for each PDF page calculate aspect-fit image size from pagePointSize, render it,
paint centered within the printable rect, call newPage between pages, and end the
painter. Null image, begin/newPage failure returns false with visible error. Rendering
at new printer page size/orientation reuses snapshot bytes and page count; it does
not call WebEngine or reflow HTML. “Page-size regeneration” in tests means these new
render metrics, not another asynchronous snapshot on the preview paint path (K7).

Closing the tab during Generating/LoadingPdf invalidates callbacks, discards bytes
and reports cancellation through the owning window if still present. Cancel creates
no file. Closing preview destroys its dialog before PDF document/buffer. Direct print
and export use the same readiness/error rules. A completed snapshot retains its
QBuffer for the document's entire life; a local stack buffer is forbidden.

### 4.11 Hotkey syntax, X11 matching and capabilities

Stable action IDs are `main-window` and `clipboard-lookup`. Application preferences
still store existing QKeySequence PortableText strings. Parsing uses QKeySequence,
count and QKeyCombination key/modifier accessors; never a comma split or the old
bitmask on the Qt6 indexed return type. Existing untouched multi-key text survives
byte-for-byte round-trip through the new raw-text fields (SD-15).

X11 accepts the existing one/two-key Config::HotKey combinations and retains recorder
matching, modifier handling, double-Ctrl+C behaviour and timing. Verify this with
matcher unit fixtures plus a separate explicit live xcb gate. Obtain the active Qt
Xlib Display through `nativeInterface<QNativeInterface::QX11Application>()->display()`;
root is DefaultRootWindow(display). Never cast the XCB connection pointer to Display.
Preserve the recorder's separate worker connection/lifetime; do not run blocking RECORD
on the GUI/native connection. Null display/backend errors disable with explanation.

Portal requires exactly one nonempty combination with an ordinary key and only
Ctrl/Alt/Shift/Meta modifiers. Modifier-only, key2/sequences, unknown keys and unsupported
modifiers are preserved in config but marked unavailable. Defaults `Ctrl+F11,F11` and
`Ctrl+C,C` therefore are initially unavailable on Wayland until the user chooses
single combinations; do not silently collapse them.

Preferred trigger format is XDG, not QKeySequence text: canonical modifier order
`CTRL`, `ALT`, `SHIFT`, `LOGO`, joined with `+`, followed by case-sensitive keysym.
ASCII A–Z Qt keys serialize to lowercase a–z, with SHIFT represented by modifier;
digits remain digits. F1–F35 map identically. Named mapping:
Space→space; Escape→Escape; Tab→Tab; Backtab→SHIFT+Tab;
Backspace→BackSpace; Return→Return; keypad Enter unsupported;
Insert/Delete/Home/End/Left/Right/Up/Down map identically;
PageUp→Prior; PageDown→Next; Pause→Pause; Print→Print; Menu→Menu.
ASCII punctuation uses the standard X keysym name (`+`→plus, `,`→comma,
`-`→minus, `.`→period, `/`→slash, `;`→semicolon, `:`→colon,
`=`→equal, `[`→bracketleft, `]`→bracketright, `\\`→backslash,
`'`→apostrophe, `"`→quotedbl, `` ` ``→grave, `~`→asciitilde,
`!`→exclam, `@`→at, `#`→numbersign, `$`→dollar, `%`→percent,
`^`→asciicircum, `&`→ampersand, `*`→asterisk, `(`→parenleft,
`)`→parenright, `_`→underscore, `{`→braceleft, `}`→braceright,
`<`→less, `>`→greater, `?`→question, `|`→bar).
Other keys return unsupported with a reason, never an invented keysym (SD-15).

Examples: `Ctrl+Alt+G`→`CTRL+ALT+g`; `Meta+Shift+F5`→`SHIFT+LOGO+F5`;
`Ctrl++`→`CTRL+plus`; `Ctrl+C,C`→unavailable sequence. Do not send NUM for
Qt KeypadModifier. `trigger_description` is localized display-only output; never parse
it, compare it to preferred_trigger, or claim a requested binding is active without
portal reporting it. An empty trigger_description displays “Assigned by portal”.

UI states: X11 active; portal bound/partial with per-ID descriptions; portal
unsupported/absent/denied/unavailable with reason; preserved multi-key sequence
unavailable on Wayland. All states separately state that clipboard-change scanning
and explicit clipboard lookup remain usable; those actions are not a global-hotkey
fallback. Hover translation/focus restoration stay disabled on Wayland.

### 4.12 Portal wire protocol, races and deadlines

Use service `org.freedesktop.portal.Desktop`, path `/org/freedesktop/portal/desktop`,
interface `org.freedesktop.portal.GlobalShortcuts`. Read version as uint via D-Bus
Properties.Get. Subscribe to Activated, ShortcutsChanged, Session.Closed and service
owner loss before creating/binding sessions. Marshal `a(sa{sv})` through an explicitly
registered QtDBus structure/list; shortcut entry is `{QString id,QVariantMap properties}`,
not a stringified QVariant. `session_handle` in CreateSession results is a **string**
validated as an object path; Bind/List inputs carry QDBusObjectPath.

Every request gets unique valid object-path-element handle_token and every session a
session_handle_token. Derive predicted request path from the connection's unique bus
name (strip leading `:`, replace `.` with `_`) and handle_token. Subscribe before
asyncCall. To handle portal implementations returning another request path and fast
Response before method return, the request listens to Response with an empty-path
match, records the message path, and buffers candidate early envelopes until the
method result identifies the handle. Only envelopes matching this request's predicted
or returned path and current service owner can complete it. Ignore unrelated requests.
The early-envelope buffer is bounded to this request's 5 s lifetime and cleared on
completion; no broad handler can deliver an unrelated request's result.

On method response set the actual path, process its buffered Response at most once,
and discard other paths. On cancelled request whose actual path arrives late, issue
Request.Close immediately. Preserve enough retired-generation request metadata to
close a late CreateSession session handle; it must never become current. Close that
session even if the old request's public completion is already suppressed. Service
owner change invalidates all stored handles and envelope buffers.

Wire calls:

- CreateSession(options: handle_token, session_handle_token) → request path;
  Response(0, {session_handle: s}) establishes the session.
- ListShortcuts(session, {handle_token}) → request; validate the `a(sa{sv})` wire
  shape and duplicate IDs. Project well-formed entries through the current display
  policy for labels/triggers only; historical IDs outside that policy are ignored and
  never activate or invalidate the session. Apply only if its display revision is
  current.
- BindShortcuts(session, [(id,{description,preferred_trigger}),...], "",
  {handle_token}) → request; only handle_token goes in **Bind options**. Response
  `shortcuts` must be a unique subset of the requested valid IDs; unknown/duplicate
  IDs or wrong DBus types are protocol error → Unavailable. Partial/empty is legal,
  not protocol error.
- ConfigureShortcuts(session, "", {activation_token: token when supplied}) is v2-only,
  an asynchronous method with **no** Request.Response lifecycle. Observe method error
  and later ShortcutsChanged; never attempt a second Bind in this session.
- Request.Close has no promised Response; Session.Close retires a session.

Wayland parent_window is always empty; no private Qt surface export/helper is added.
An X11 token is not required because that platform selects RECORD. Response codes:
0 success; 1 cancellation/denial; 2 other portal failure. The returned-ID subset rule
above applies only to each BindShortcuts response; ListShortcuts has the independent
wire-shape/duplicate/historical-display rules above. Never activate an ID merely
because it appeared in a listing.

If all desired sequences are unsupported, display per-ID reasons and do not pretend
they are bound; do not send an empty binding attempt merely to manufacture success.
An explicit later user edit can create a fresh session. No restore token is stored.
Timeout/error/session-loss handling is idempotent; session/request Close calls are
asynchronous and never block shutdown waiting for a missing service.

### 4.13 Platform helpers and additive config migration

screenForPoint uses QGuiApplication::screenAt, falling back to primaryScreen; an empty
screen list returns null. availableGeometryForPoint returns its availableGeometry or
empty QRect. Existing screenGeometry consumers that require full geometry use the
returned screen's geometry. Clamp popups to the chosen screen, not a virtual-desktop
rectangle (`scanpopup.cc:644`, `scanflag.cc:65`, `translatebox.cc:293`).

popupFlags on wayland returns Tool|FramelessWindowHint|WindowStaysOnTopHint regardless
of stored X11 bypass/popup preference. On xcb preserve the current preference mapping:
Popup or Tool|FramelessWindowHint|WindowStaysOnTopHint, adding X11BypassWindowManagerHint
only when enabled (`scanpopup.cc:448–468`). Wayland never gets that X11 flag.
applyActivation checks window existence, shows it through the caller's normal path,
then with HAVE_KWINDOWSYSTEM and nonempty token calls
KWindowSystem::setCurrentXdgActivationToken(token), followed by
windowHandle()->requestActivate(). Without KDE helper requestActivate is still called.
Return void; log request, never claim the compositor granted focus. Never log token.

Config load migrates exact `Qt Multimedia` and unknown nonempty backend names to
`InternalPlayerBackend::defaultBackend()` when MAKE_FFMPEG_PLAYER exists; after K4
this is ffmpeg. Keep the existing private ffmpeg factory private (`config.hh:214–217`)
and use the public defaultBackend interface (`config.hh:192`). Unknown names log
once. Already ffmpeg and external settings stay. In no_ffmpeg_player builds:
internal player unavailable/UI hidden, useInternalPlayer false for runtime, stored
backend text preserved verbatim for later builds, external playback works. Remove
qtmultimedia/isQtmultimedia/name-list plumbing; no_ffmpeg_player remains a real build.

Add under the existing XML root:

```xml
<hotkeys>
  <portalBindings>
    <binding id="main-window" triggerDescription="Ctrl+Alt+G"/>
    <binding id="clipboard-lookup" triggerDescription="Ctrl+Alt+C"/>
  </portalBindings>
</hotkeys>
```

In-memory member is `Config::Class::hotkeys.portalBindings`, a vector of
PortalBindingDisplay. Missing section → empty. Only successful Bind/List/Changed output
is serialized at the existing save point. Unknown IDs ignored on config load; duplicates
last-wins for display, never for binding wire validation. Stored descriptions are
escaped XML text/attributes, never executable or an activation source.

Preserve untouched original mainWindowHotkey/clipboardHotkey text alongside parsed
Config::HotKey in Preferences; setting a new value through the recorder clears that
raw-text marker and writes PortableText. Invalid/multi-key stored strings remain
visible and are not overwritten merely because a portal cannot bind them. New keys
are additive; proxy schema and existing save timing remain unchanged.

## 5. Error handling matrix

Diagnostics use existing gdWarning/gdDebug infrastructure after its explicit UTF-8
migration. Prefixes below are stable enough for gates; variable URL/user data must be
escaped, credentials and activation tokens omitted. “Once” means once per affected
operation/generation or backend state transition, not once per polling tick.

| Failure mode | User-visible behaviour | Required diagnostic template | Post-failure state |
| --- | --- | --- | --- |
| Unknown charset | Continue with lower detection precedence/UTF-8 | `Encoding: unknown charset <name>; using <fallback>` | Valid fallback value |
| Invalid/truncated decode bytes | Replacement glyphs, lookup continues | `Encoding: replacement during decode charset=<name> reason=<errno>` | Descriptor closed, result returned |
| Unrepresentable URL substitution | Placeholder remains; lookup can continue | `Encoding: URL placeholder <placeholder> not representable in <name>` | Other substitutions retained |
| EPWING unrepresentable search | Try next codepage; then no match | `Encoding: EPWING search skipped charset=<name>` | No bad bytes sent to libeb |
| Invalid user regex | Inline search validation error | `Regex: invalid user pattern: <error>` | No search launched |
| Invalid built-in regex | Current dictionary/save operation fails visibly | `Regex: invalid built-in pattern source=<file:line>: <error>` | No unbounded/rewrite-all fallback |
| Bad URL/method/missing resource | Engine failed resource/document indicator; status for explicit action | `ArticleScheme: <error> url=<url>` | Failed once; request released |
| Producer error before reply, including partial bytes | No successful document is exposed; dictionary error visible in article or action status | `ArticleScheme: producer failed before reply url=<url>: <reason>` | RequestFailed; no reply; request released |
| Producer error after a live reply | Bytes already buffered are delivered, then the loader is woken and receives a failed read; failure is not reported as successful EOF | `ArticleScheme: producer failed after reply url=<url>: <reason>` | Failed terminal latch; no second `fail()`/`reply()`, `atEnd() == false` after drain |
| Producer read/copy exception | Same failure path as a producer error; never converted to not-found or EOF | `ArticleScheme: device read/copy failed url=<url>: <reason>` | Failed terminal latch; one `producerFailed` notification |
| Streaming Range request | Request waits for complete successful bytes; handler resolves bounds and seeks the full-size buffer before reply | `ArticleScheme: buffered Range url=<url>` (debug) | Positioned buffered reply or ordinary terminal failure |
| Unsatisfiable byte range | Engine failed-resource indicator; no truncated body is exposed | `ArticleScheme: unsatisfiable Range url=<url> size=<size>` | `QWebEngineUrlRequestJob::RequestFailed` once before reply; no successful EOF or 200 reply |
| Job destroyed mid-production | No stale content or crash | `ArticleScheme: cancelled url=<url>` (debug) | Cancelled, connections/map/device released |
| Malformed ListShortcuts wire payload or duplicate ID | Hotkeys listing is rejected and unavailable reason is shown | `Hotkeys: malformed ListShortcuts response generation=<g>: <reason>` | Unavailable; no ID activated |
| Well-formed historical ListShortcuts ID outside current display policy | Historical label/trigger is ignored for current display; session remains usable | `Hotkeys: ignored historical shortcut id=<id>` (debug) | Existing session state unchanged; no activation |
| Group-less ordinary DocumentLoad | Document load is rejected; UserAction path remains valid through retained handler context | `ArticleUrls: group required for DocumentLoad url=<url>` | UrlInvalid; no maker request |
| Stale script/event | No UI mutation | `ArticleBridge: dropped stale generation=<g>` (debug) | New queue/state unaffected |
| State-capture timeout | Navigation proceeds using last cached reading state | `ArticleHistory: capture timed out; using cached state` | Destination generation starts once |
| Frame refuses/never completes instrumentation | Visible unavailable-frame hint to use inline mode | `WebsiteFrame: unavailable dict=<id> url=<url>` | Timers stop after bound; no origin rewrite |
| Blocked external content | Optional content absent under enabled preference | `ArticlePolicy: blocked url=<url> initiator=<origin>` | Request blocked only |
| Empty system-proxy result | Proxy settings show direct mode/reason | `ArticleProxy: system query empty; using NoProxy` | Explicit NoProxy |
| Origin/proxy auth cancel | Request remains unauthenticated/fails normally | `ArticleAuth: <origin|proxy> credentials declined for <host>` | No stored origin secret or proxy cross-write |
| Missing/unusable WebEngine runtime | Startup error dialog (stderr if GUI unavailable), nonzero exit | `WebEngine: initialization failed: <reason>` | No main application session claimed usable |
| Portal absent/version unsupported | Hotkeys preferences explain unavailable; clipboard actions remain | `Hotkeys: portal unavailable: <reason>` | Unsupported/Unavailable; no X11 fallback |
| Portal denied/partial/empty | Per-ID inactive state and descriptions | `Hotkeys: portal result state=<state> active=<n> requested=<n>` | Denied/Partial/Bound, no retry |
| Portal malformed reply/timeout/loss/session close | Active subset cleared; explanation updated once | `Hotkeys: portal unavailable generation=<g>: <reason>` | Unavailable; pending requests closed |
| Unsupported portal sequence | Original text shown with single-combination explanation | `Hotkeys: unsupported sequence id=<id>: <reason>` | Preserved config, not reported active |
| Missing X11 display/RECORD failure | Global shortcuts disabled with reason | `Hotkeys: X11 RECORD unavailable: <reason>` | Unavailable; GUI still works |
| Save cancel/navigation/tab close | Cancellation status; existing destination preserved | `SaveArticle: cancelled: <reason>` | Owned temp/final-uncommitted assets removed |
| Save download/write/rename/commit failure | Save error with resource/destination reason | `SaveArticle: failed: <reason>` | Failed; no partial new HTML |
| Empty/invalid PDF, timeout, render/printer failure | Print/export error | `PrintPipeline: failed state=<state>: <reason>` | Failed; buffer/document cleaned or Ready snapshot retained for printer-only failure |
| Print cancel/tab close | Cancellation status, no partial output | `PrintPipeline: cancelled: <reason>` | Cancelled; callbacks inert |
| Cache clear exceeds 5 s | Exit proceeds; diagnostic records incomplete clear | `ArticleCache: exit clear timed out after 5000 ms` | Exit barrier completed once, then teardown |
| Unknown audio backend with ffmpeg enabled | Preferences normalized to ffmpeg | `Audio: unknown backend <name>; using FFmpeg+libao` | Valid in-memory backend |

Startup readiness uses the first actual controlled blank-document load and bridge
handshake, not an invented Qt initialization-success function. A missing renderer or
initialization error before readiness fails startup visibly. Render-process failure
after readiness is handled by the view's ordinary failed-load/status path; acceptance
must not classify a live but blank window as successful startup.

## 6. Test fixtures, targets, and acceptance

### 6.1 Common harness and deterministic support

`tests/tests.pro` is TEMPLATE=subdirs with all 18 names below. Each target directory
contains exactly `<name>.pro` and `<name>.cc`; TEMPLATE=app, CONFIG+=testcase c++17,
QT+=testlib plus the listed modules. tests/testsupport.pri supplies root include path,
out-of-source object/MOC directories per test, needed platform/feature definitions
and the same ffmpeg/EPWING optional feature configuration as the application.

Test support interfaces, in the files listed in §2.12:

- `TestSupport::requiredEnvironment(QStringList * reasons) -> bool` in fixtures.hh/.cc:
  validates required per-suite prerequisites; `GD_TEST_FORCE_ENV_UNAVAILABLE=1` returns
  false only here. `GD_TEST_FORCE_FAILURE=1` forces an assertion failure in tst_utf8_wstring.
  Production code has no such hooks. Every suite's initTestCase calls QVERIFY2 on
  its own required checks, before tests can pass without doing work.
- `FixtureDictionary : Dictionary::Class` in dictionary_stub.hh/.cc: fixed dictionary
  ID, articles/resources, captured lookup arguments; no real user dictionaries.
  `ControlledDataRequest : Dictionary::DataRequest` has
  `append(QByteArray const &)`, `finishSuccess()`, `finishNotFound()`,
  `finishError(QString const &)`, `setFullDataException(QString const &)`,
  `setSliceException(QString const &)`, `setStreamCopyException(QString const &)`,
  `cancel()`; records cancellation/completion counts. It obeys the existing
  dataMutex/hasAnyData discipline and emits production signals, including
  instant-finish construction. It does **not** hide or override `getFullData()` or
  `getDataSlice()`; those base methods are nonvirtual.
- `TestSupport::controlledReaderFactory() -> DataRequestReaderFactory` and
  `ThrowingDataRequestReader : DataRequestReader` in the same dictionary_stub.hh/.cc:
  the factory runs on GUI, associates a ControlledDataRequest with its separately
  shared, mutex-protected probe/control state, and returns a reader that retains
  only that plain state and a `makeDataRequestReader()` production delegate. Requests
  outside this fixture use the production reader. The reader never retains a producer
  pointer/reference or the request's `sptr`. Construct the real handler with this
  factory overload; it passes that same reader to its real live device. This is the
  only route by which the three setters affect production reads.
  `TestSupport::ReadOperation` is `{FullData, Slice, StreamCopy}`;
  `ControlledDataRequest::readProbe(ReadOperation) const -> ReadProbeSnapshot` returns
  synchronized `entered`, `thrown`, `returned` counts and `callingThreads` for that
  operation since its most recent setter. Record caller identities with
  `QThread::currentThreadId()` (`QtCore/qthread.h:32`). Each setter arms the next
  matching reader operation once with a nonempty unique message and resets its probe;
  it neither changes producer bytes/state nor calls a reader itself. The corresponding
  override records entry, consumes the flag, records a throw and throws a
  `std::runtime_error` carrying the configured message as UTF-8 before
  forwarding/copying; otherwise it delegates to the production reader and records a
  successful return. Shared probe state survives
  the producer/handler record until the device releases its reader, and access from
  GUI setters/probes and IO copy calls is serialized by its own mutex. `StreamCopy`
  consults only this state and its supplied byte span, never the GUI producer.
  All flags, forced throws and counters live only in test support; production sources
  contain the ordinary reader dependency and catch boundaries, with no forced-failure
  hook. The three per-boundary acceptance cases are mandatory in §6.3.
- `HttpFixture` in http_fixture.hh/.cc: `bool start(QHostAddress const &, quint16 port=0)`,
  `QUrl url(QString const & path) const`, `QVector<Request> requests() const`,
  `void release(QString const & path)`. It serves fixed route bytes, redirects, header
  variants, request barriers and proxy challenges; records method/Host/path without secrets.
- `FakePortal` in fake_portal.hh/.cc: `bool start(QDBusConnection const &, uint version)`,
  `void setScenario(QString const &)`, `QVector<Call> calls() const`,
  `void emitActivation(QString const & session, QString const & id, QString const & token)`,
  `void loseService()`. Exports exact portal/request/session wire interfaces and
  asserts no second Bind on a session.

Pure suites require readable fixtures and working locale/iconv probes, with 60 s
per-binary wall limit. GUI/WebEngine suites require an X display, a functioning
Chromium renderer, actual document+JS handshake and bindable fixture servers; limit
120 s per binary. Use QSignalSpy::wait/QTRY_VERIFY_WITH_TIMEOUT with an aggregate
elapsed deadline, not many independent 120 s waits. The outer wrapper adds a 900 s
suite command bound; each test binary also has its own wall-time watchdog in support.

Portal tests launch `dbus-daemon --session --nofork --print-address=1` with QProcess,
connectToBus at that private address, verify connection/registration, and tear it
down after backend/request destruction. They never acquire the real desktop portal's
name on the user's session. A missing dbus-daemon/private bus is a required failure.
Browser servers listen on `127.0.0.1`, `127.0.0.2`, `127.0.0.3` port **0**, read back
serverPort, and build URLs dynamically. Failure to bind any required address fails.
These are literal IP hosts: distinct origins and distinct policy hosts without DNS
or Chromium command-line host rules. No fixed port or external website is required.

### 6.2 Fixture files and fixed expected content

| Exact fixture files | Required content/oracle |
| --- | --- |
| `tests/fixtures/encoding/vectors.json` | Fixed charset, bytesHex, expected UTF-16/scalar text, expected encode bytes, detectedBy and error rows; never compute expected output with the converter under test |
| `tests/fixtures/regex/cases.json` | Source location, family, old options, input, expected match/captures/replacement and new options for §4.3 |
| `tests/fixtures/linkrewrite/cases.json` | Every §4.2 positive/negative/idempotence example, quoted/unquoted/multiline tags, all four functions and engine-specific normalizations |
| `tests/fixtures/article/article.html` | Headword `fixture-headword`, gdfrom-dict-a and gdfrom-dict-b, early/late active callbacks, bres image, gdau link, long text and optional parts |
| `tests/fixtures/article/pixel.png` | Fixed valid 1×1 PNG used for bres/gdpicture/network image assertions |
| `tests/fixtures/article/style.css` | Fixed unique marker style, relative image rule, no external dependencies |
| `tests/fixtures/article/dom.html` | Deterministic word/selection/highlight/context/zoom targets with Latin, Cyrillic, CJK, combining marks, apostrophes and surrogate pairs |
| `tests/fixtures/website/first.html`, `tests/fixtures/website/second.html` | Named click/context targets, different heights, relative image/style, GET form, script-relative fetch; second navigation gets a new docId |
| `tests/fixtures/website/nested.html` | Nested named iframe and known border/offset; word target for coordinate recursion |
| `tests/fixtures/website/style.css`, `tests/fixtures/website/relative.js` | Visible CSS marker and `fetch('./data.json')`, request counts recorded by server |
| `tests/fixtures/website/data.json` | `{ "marker": "real-origin-fetch" }` |
| `tests/fixtures/print/two-pages.html` | Explicit two-page content with fixed page breaks and `snapshot-page-one/two` text |
| `tests/fixtures/config/legacy.xml` | Existing preferences with Qt Multimedia backend and untouched two-key shortcut strings; proxy credentials are harmless fixture-only values |
| `tests/fixtures/config/additive.xml` | New portal display section, partial binding and unknown ID cases, unknown backend variant |

HTTP routes serve the common pixel.png under image paths, the website files at
`/dir/...`, redirect `/redirect` to `/dir/first.html`, delayed `/slow-resource`,
`/deny-frame` with X-Frame-Options:DENY, `/deny-csp` with frame-ancestors 'none',
`/auth` with fixed Basic realm, and proxy forwarding/challenge scenarios. Requests
include final non-default port checks. The form action and fetch use the remote
frame's real URL, not gdlookup. Do not pass by merely checking generated HTML strings.

Minimum byte-exact Encoding vectors (hex → Unicode; reverse direction pinned too):
UTF-8 `e4b8ad`→中; UTF-16LE `2d4e`→中; UTF-16BE `4e2d`→中;
UTF-32LE `2d4e0000`→中; UTF-32BE `00004e2d`→中; UTF-8 `f09f9880`→😀;
SHIFT_JIS `93fa967b`→日本; BIG5 `a4a4a4e5`→中文;
BIG5-HKSCS includes the same common-plane vector plus a fixed extension scalar;
GB18030 `d6d0cec4`→中文 plus a fixed four-byte supplementary vector;
GB2312 `d6d0cec4`→中文; EUC-JP `c6fccbdc`→日本;
WINDOWS-1251 `cff0e8e2e5f2`→Привет.
ISO-8859-N non-ASCII vectors: 1/e9→é; 2/a1→Ą; 3/a1→Ħ; 4/a1→Ą;
5/b0→А; 6/c7→ا; 7/c1→Α; 8/e0→א; 9/d0→Ğ; 10/a1→Ą;
11/a1→ก; 13/c0→Ą; 14/a1→Ḃ; 15/a4→€; 16/a4→€.
ISO-8859-12 is deliberately unknown. Include BOM/no-BOM endian pairs, embedded NUL,
`ff ff` and `e2 82` recovery, malformed UTF-16 and unrepresentable/surrogate encoding,
stateful ISO-2022-JP final flush, output-buffer growth, unknown aliases, and parallel
conversion of the same immutable Encoding value. Extension vectors must be fixed
standard mappings in the JSON with expected scalars, not generated at test runtime.

### 6.3 Per-target build, guards, assertions and skip policy

Every target below has the two files `tests/NAME/NAME.pro` and `tests/NAME/NAME.cc`.
This table expands NAME to an exact path pair. “Core” means QT core/testlib plus
support; “Browser” adds gui/widgets/network/webenginecore/webenginewidgets/webchannel;
“Article” adds xml/sql/svg/help and the existing article maker/dictionary utility
closure. Link production sources, not copied substitute implementations. A small
shared `.pri` can list that closure; tests do not include `main.cc`/MainWindow merely
to reach a utility. All required assertions listed are pass/fail; optional skip is
limited to the explicitly marked live/manual cases in §6.6.

| NAME | Production code built / Qt modules | Required environment and fixtures | Required assertions |
| --- | --- | --- | --- |
| tst_encoding | encoding; Core; iconv | All named converters open; vectors.json; legacy/config and caller helper fixtures | Exact bytes/scalars, recovery grouping and flush; no descriptor sharing; detection conflicts/unknown header/meta order; locale fallback; URL placeholder intact on encode failure; EPWING no bad UTF-8 fallback; SLOB decode |
| tst_linkrewrite | linkrewrite, audiolink and engine-normalization helpers; core/gui/network | cases.json; no display unless icon materialization needed | Exact output/captures, absolute/protocol-relative untouched, Unicode/multiline, `gdlookup://localhost/cat?gdanchor=part` retains path/query and `dict` survives, encoded path is not double-encoded, second-pass idempotence, correct per-dialect retained step |
| tst_regex | migrated regex/FTS helpers, wildcard, save collector helpers; Core plus engine utility closure | regex/cases.json | Every pattern family, original options, zero-width termination, Unicode captures, greedy/minimal count distinction, wildcard slash/escape and invalid regex rejection |
| tst_article_schemes | articleurls/articleschemes; Browser/Article | Custom main registers before QApplication; renderer handshake | All 7 names/syntax/flags, encoded paths/hosts, no port/credential acceptance, document-vs-resource MIME, registration before profile |
| tst_article_load | real ArticleView + maker + scheme/profile closure; Browser/Article | FixtureDictionary, article files, real renderer | Visible headword via DOM, naturalWidth>0 bres image, blank=1, nonzero current reference group inherited by group-less `gdlookup` path and `bword:cat`, FTS dictionary subset and contexts retained through action handler, `dict` selection and StarDict `gdanchor` applied, `?dict=<known name>` preserved, malformed contexts failure, group-less ordinary DocumentLoad fails, gdpicture HTML with loaded embedded image, successful empty resource |
| tst_article_stream | ArticleStreamDevice/handler and production DataRequestReader, ControlledDataRequest/throwing reader support; Browser/Article | Renderer plus deterministic producer, injected reader factory, response observations and direct device reads | Real WebEngine consumer observes a live device while GUI appends, completes, cancels and tears down; thread observations prove GUI producer callbacks and Chromium IO-thread QIODevice calls use only the synchronized state/producer-free copy seam; first chunk is present before reply, at least two later appends arrive, bytes are complete and ordered, `size()==0`, backlog/atEnd transitions and cancellation are correct; for body `ABCDEFGHIJ`, `Range: bytes=0-` delivers `ABCDEFGHIJ`, `bytes=3-` delivers `DEFGHIJ`, `bytes=3-5` delivers `DEF`, and `bytes=-2` delivers `IJ`, each with exact 206/range metadata below and full buffer size 10; unparseable Range is full-body 200, unsatisfiable Range fails `RequestFailed` without reply, while no-Range is full-body 200 streaming; finishError-before-reply-with-partial-data fails without reply; append-then-finishError-after-reply drains then fails without EOF, emits exactly one handler/device failure notification and no successful completion; each of the three forced-failure setters must reach and throw through the real handler/device's production catch boundary, prove its configured operation/thread by probe counts, latch `producerFailed` once and meet the per-setter assertions below without successful EOF or a second job reply; zero-size reads, not-found vs empty, one reply/completion and teardown cancellation are asserted |
| tst_external_content | interceptor/profile/adapters + real article; Browser/Article | A=127.0.0.2, B=127.0.0.3 servers, two contributing dictionaries | A iframe own image/CSS/fetch observed; A→B blocked with preference on; internal bres still works; all succeed with preference off; absent/opaque initiator fallback; suffix/IP host cases; per-page host sets do not leak |
| tst_article_bridge | bridge/dom helpers + real ArticleView; Browser/Article | Parser-time callback fixture and controlled delayed JS callbacks | Early queue, post-ready stamped call, switch dictionary, old-generation inbound rejection, navigation in flight, stale callback cannot release new queue, close with queued callback; cached signals/getters; selection/FTS offsets |
| tst_website_frames | WebsiteFrames/bridge plus website emission; Browser/Article | Two origins/non-default ports, nested/first/second files and deny routes | Height applied/revealed, correct dict click/context text, word at 100%/200% zoom and nested border, remote image/style/form/fetch correct host+port; repeat instrumentation/nonce/height/word assertions after second navigation; blocked-frame hint; inline base/port fix |
| tst_article_history | ArticleHistory/coordinator + real page; Browser/Article | Long article, delayed image/layout growth, recorded offsets | A→B→back restores A's **outgoing** state with a fresh bridge generation; document cache disabled; forward B state; repeated URL distinct indices; reload top, forward truncation, redirect single commit, fragment no entry, ordinary navigation does not reset; capture timeout fallback and growth settle |
| tst_print_pipeline | printpipeline + QPdfDocument; Browser plus printsupport/pdf/pdfwidgets | Renderer, two-pages.html; QPrinter PDF output to temporary destination (no physical printer required) | Ready/pageCount=2 before preview, snapshot buffer alive across preview/print, page-layout change alters render dimensions with same bytes/page count, Error/empty bytes, cancel and tab close during generation, no partial export, no async work in paint |
| tst_save_article | savearticle plus collector/download closure; Browser/Article | Real toHtml, pixel/style, slow-resource barrier, temporary destination | Complete and HTML-only saves, local asset refs load, stable query filenames/dedup, cancel mid-download cancels every request/no owned leftovers, navigation/tab close abort, existing destination intact on failure, asset rename then HTML commit ordering |
| tst_portal_hotkeys | portalhotkeys/hotkeybackend/hotkeysyntax/config utility; core/gui/widgets/dbus | Private daemon, FakePortal v1/v2, harmless config fixtures | Fast Response before method result, delayed return/path, denied/partial/empty, Close before Response/no completion wait, timeout, Session.Closed, late edit-created session closed, second edit, service loss, shutdown, token forwarded unchanged, ListShortcuts wire-type/duplicate rejection, startup where List returns historical `clipboard-lookup` while the current display policy exposes only `main-window` and only `main-window` is currently bindable, both before and after Bind, historical entries filtered from display without session teardown or activation, listing revision guard; one Bind/session; Bind returned IDs only a unique requested subset; Configure no Request lifecycle |
| tst_hotkeys | hotkeysyntax and extracted X11 matcher/config routines; core/gui/widgets | Fixed strings and key combinations; X11 matcher can run without RECORD server | One/two-key parse and round-trip, modifier/key handling, every serialization table category, Ctrl++ and comma, unsupported sequence preserved, stable ID/handle mapping, no false clipboard fallback; config backend/display migration incl no_ffmpeg |
| tst_htmlescape | htmlescape; Core | Fixed HTML/Unicode/NUL/linebreak cases | Escape/preformat expected bytes; migrated multiline/Unicode patterns preserve output |
| tst_langcoder | langcoder; Core | Fixed language names/codes and malformed inputs | Existing name/code mappings, case/Unicode paths, unknown result |
| tst_utf8_wstring | utf8, wstring, wstring_qt; Core | ASCII/UTF-8/supplementary/invalid vectors | Exact conversions and failure handling; forced failure hook proves root propagation |
| tst_platformwindow | platformwindow; gui/widgets/testlib | A working Xvfb screen and actual QPA; table-driven platform strings | screenAt/primary fallback, empty-screen pure helper branch, available vs full geometry, Wayland flags contain no X11 bypass, xcb preserves preference, null activation safe; no boolean focus promise |

For `tst_article_stream`, the Range oracle is fixed body `ABCDEFGHIJ` (S=10).
Observe the actual delivered bytes and loader response metadata, not just generated
headers, buffer size or expected offsets:

| Request | Delivered bytes | Status, Content-Range and response length |
| --- | --- | --- |
| No Range | `ABCDEFGHIJ`, including appends after reply | 200, no Content-Range; live device size remains 0 |
| `Range: bytes=0-` | `ABCDEFGHIJ` | 206, `Content-Range: bytes 0-9/10`, length 10 |
| `Range: bytes=3-` | `DEFGHIJ` | 206, `Content-Range: bytes 3-9/10`, length 7 |
| `Range: bytes=3-5` | `DEF` | 206, `Content-Range: bytes 3-5/10`, length 3 |
| `Range: bytes=-2` | `IJ` | 206, `Content-Range: bytes 8-9/10`, length 2 |
| `Range: bytes=-20` | `ABCDEFGHIJ` | 206, `Content-Range: bytes 0-9/10`, length 10 |
| `Range: bytes=invalid` | `ABCDEFGHIJ` after completion | 200, no Content-Range, length 10 |
| `Range: bytes=10-`, or `bytes=0-` against an empty resource | No successful body | One `RequestFailed`, zero reply calls; no truncated 200 |

For every satisfiable buffered row, also assert full backing `size()==10` and the
resolved `pos()` before handoff; those checks supplement, rather than replace, the
body/metadata oracle. Keep the real WebEngine consumer and direct-device checks in
this existing target; no additional suite or application fetch capability is added.

The same target must exercise all three setters using the injected test-support
factory with the real production handler/device and real WebEngine request/consumer.
For **each** row below, require `readProbe(operation)` to report `entered == 1`,
`thrown == 1`, `returned == 0` with the unique configured message reaching the handler's
single `producerFailed` notification/failed latch. The caller thread must match the
specified boundary. Setters and producer append/finish operations initiate the case;
the testcase must not directly call the throwing reader or catch its exception itself.
Correlate the probe and message with that request's actual terminal outcome: the
exception must be caught inside the linked production boundary, not by test code or
a substitute handler/device. No exception may escape the Qt callback, and no forced
failure may result in not-found, successful load/EOF, `completed` or
`readChannelFinished`. Later queued producer callbacks must not clear failure,
re-notify it or produce a second job reply/fail.

| Setter / probed operation | Required production route and timing | Required terminal assertions |
| --- | --- | --- |
| `setFullDataException` / FullData | Arm before completing a nonempty buffered request successfully; the real handler calls `reader->fullData(*request)` on GUI inside its buffered-copy try/catch | Exactly one `producerFailed`/handler failure latch with that message, one `fail(RequestFailed)`, zero reply calls; no successful buffered body/EOF or device success signal |
| `setSliceException` / Slice | First establish a live reply from an initial chunk; arm while the producer is unfinished, then append another chunk so the real device GUI pump calls `reader->dataSlice` inside its catch boundary | Exactly one device `failed` and one handler `producerFailed` for the same failure; previously staged bytes drain, then `-1` with `atEnd()==false` before cancellation; total reply calls stay 1 and no post-reply `fail()` occurs; later completion cannot produce successful EOF |
| `setStreamCopyException` / StreamCopy | Arm for a live request with staged bytes while the producer is unfinished; after the real handler replies, WebEngine's IO read calls `reader->copyStreamBytes` inside real `readData` try/catch | Exactly one device `failed` and one handler `producerFailed` for the same failure; copy offset does not advance on the throw; IO read returns `-1` with `atEnd()==false` and loader fails before cancellation; total reply calls stay 1 and no post-reply `fail()` or successful EOF occurs |

The live exception probes record Chromium IO only for StreamCopy and GUI only for
Slice; FullData records GUI. No live IO probe may observe any producer method call.
The default factory is also exercised with the real forwarding reader and successful
buffered/live payloads, so test substitution cannot replace production linkage.

Where a full engine source is too coupled to link in a pure suite, extract the
existing conversion helper to the module already responsible for it and link that
same production helper in both caller and test. Do not write a test-only copy of
EPWING/URL policy and mistake it for caller coverage. Feature-disabled EPWING code
is still compile-checked in the default build; tests must report which optional
build feature cases ran.

### 6.4 Root gate wiring and four probes

Use the approved root qmake recipe verbatim in structure:

```qmake
CHECK_MAKEFILE = Makefile.tests

tests-makefile.target = tests-makefile
tests-makefile.commands = $$QMAKE $$PWD/tests/tests.pro -o $$PWD/$$CHECK_MAKEFILE

tests-build.target = tests-build
tests-build.depends = tests-makefile
tests-build.commands = $(GD_TEST_WRAPPER) $(MAKE) -C $$PWD -f $$CHECK_MAKEFILE

tests-run.target = tests-run
tests-run.depends = tests-build
tests-run.commands = $(GD_TEST_WRAPPER) $(MAKE) -C $$PWD -f $$CHECK_MAKEFILE check

check.target = check
check.depends = tests-run
QMAKE_EXTRA_TARGETS += check tests-makefile tests-build tests-run
```

Use make-time `$(GD_TEST_WRAPPER)`, not qmake-time environment expansion. Each subdir
project's testcase check must propagate its binary's exit status; no ignored `-` prefix
or shell pipeline may swallow it. Root tests-run is wrapped separately from tests-build.

scripts/test-wrapper.sh: POSIX sh, fail on missing GD_TEST_WRAPPER_LOG only if logging
was explicitly requested with an unusable path; otherwise log invocation when supplied.
Append one line containing every argv as an escaped, unambiguous command description,
then `exec xvfb-run -a timeout 900 "$@"`. It must preserve argument boundaries and exit
status and must not background the command. In the gate script, put copy/log paths
under a temporary directory without spaces and pass an absolute wrapper path so
recursive makes resolve it consistently.

scripts/verify-test-gate.sh operates on disposable copies of the implementation tree,
not the active checkout. It uses a fresh log per probe and regenerates root Makefile
with GD_TEST_WRAPPER unset. It runs each probe to completion, prints command/exit/log
assertion, removes copies on exit, and returns nonzero unless **all** expected outcomes
and the clean pass occur:

1. Failure propagation: invoke wrapped root make check with GD_TEST_FORCE_FAILURE=1;
   require nonzero and tst_utf8_wstring's intentional assertion in output. The
   failure must come from executing that binary, not compilation or missing display.
2. Missing required environment: invoke with GD_TEST_FORCE_ENV_UNAVAILABLE=1; require
   nonzero plus a requiredEnvironment assertion; no QSKIP exit can satisfy it.
3. Wrapper observation: clean run must exit zero and its new wrapper log must have
   an argv entry including `-f Makefile.tests check` as separate tokens. A build entry
   alone (`-f Makefile.tests` without check) is insufficient.
4. Negative control: in another copy, remove **only** the wrapper prefix from
   tests-run.commands, regenerate with wrapper unset, rerun the observation check.
   The probe must report missing run marker while the wrapped build marker exists.
   The suite may itself pass; this probe's expected failure is the observation gate.

After mutations, an unmodified disposable copy must build and pass wrapped root check.
These are four probes with three mutations (forced testcase failure, forced environment,
unwrapped run). Do not mutate a tracked production input to implement a runtime test
hook. The script's own shell checks must fail on an unexpected exit/marker rather
than merely print a warning.

### 6.5 Qt6 audit and automated gates

scripts/check-qt6-only.sh accepts generated application Makefile/link-log paths as
arguments, prints its exact search commands, and returns nonzero for a banned token
in the defined scope. Scan `*.pro`, `*.pri`, `*.cc`, `*.hh`, `*.c`, `*.h`, `*.cpp`,
`*.mm`, `*.ui`, `*.qrc`, excluding docs, help, locale, tests/fixtures, qtsingleapplication,
winlibs, maclibs and opencc. Include `.cpp` because QtSingleApplication uses that suffix,
but keep the design's vendor exclusion for the broad textual gate. A second check
explicitly scans the small compiled vendor closure after its Qt6 adaptations (§7.3).

Banned first-party input tokens: QRegExp, QTextCodec, QDesktopWidget, QX11Info,
QWebView, QWebPage, QWebFrame, QWebElement, QWebSecurityOrigin, QWebInspector,
QMediaContent, MAKE_QTMULTIMEDIA_PLAYER, IS_QT_5, Qt4x5, qt4x5.hh, webkit,
qt6-5compat, Qt5Compat. Reject QT_VERSION/QT_MAJOR_VERSION/QT_MINOR_VERSION branching;
this fixed Qt6 target needs no compatibility branch. The audit script contains its
own token list but `.sh` is outside the compiled-input scan.

Inspect the **actual application linker command** from verbose build output, not
only LIBS variables; reject Qt5 libraries, Qt6Core5Compat and Qt6Multimedia/Widgets,
QtWebKit/QtWebKitWidgets and Qt5X11Extras. A missing/empty link command fails the audit.
Do not count text in this specification or fixed legacy-pattern fixture data.

Required automated acceptance commands, recorded with exit status:

- Default `qmake6 && make -j$(nproc)` with verbose application link capture.
- `GD_TEST_WRAPPER=<absolute scripts/test-wrapper.sh> make check` with wrapper log.
- `scripts/verify-test-gate.sh` with all four probe outcomes and clean pass.
- `scripts/check-qt6-only.sh <Makefile> <application-link-log>`.
- A separate `no_ffmpeg_player` build and its config/audio tests: no internal player,
  external playback path available, stored backend text preserved.
- `timeout 30 xvfb-run -a ./goldendict`: expected 124 with startup-ready evidence,
  no crash, no renderer initialization error. Supplemental to the tests above.

Do not broaden a required suite's skip policy to make one environment pass. Optional
live KDE/physical printer tests can be recorded unavailable, but acceptance still
requires the manual environment gates below on the target machine.

### 6.6 Live gates and parity checklist

Record platform name and selected backend at startup, without activation tokens or
passwords. Native live Wayland acceptance requires platformName beginning wayland,
a portal-reported bound single shortcut, a rendered dictionary article, a successful
lookup, popup raised via that shortcut, and audio played. Record portal descriptions
and actual observable results. Test explicit `QT_QPA_PLATFORM=xcb` separately with
RECORD and two-key defaults; never infer that from the headless smoke.

Manual parity uses representative StarDict, Babylon/BGL, MDict, DSL, Zim and website
fixtures: article and picture rendering, group/muted dictionaries, main/frame word
lookup and selection, middle-click tab, FTS highlight/navigation, find count, zoom,
context copy/resource download, complete/HTML save, print preview/direct print/PDF,
DevTools, audio/external playback, A→B→back scroll/active article, preferences, and
all four backend capability states. Confirm inline mode for a frame-denying site,
remote form/fetch origins, no Wayland hover/focus-restore claim, and snapshot print
page-size behaviour. The final acceptance record must distinguish automated results
from these manual observations.

## 7. Migration and refactor mechanics

### 7.1 Ordered regex conversion, including signal/type changes

1. Add qmake test skeleton, fixtures and pure Encoding/LinkRewrite interfaces first.
   Inventory each live pattern with source location and options. Baseline QRegExp
   source files are listed below; do not begin with a blind textual substitution.
2. Select the Qt6-compatible side of version branches. Preserve both behavioural
   expectations in fixtures where the two historical branches differ, choosing the
   approved-design behaviour explicitly. Remove the unselected branch and its includes.
3. Move the common patterns into LinkRewrite, keeping capture numbers, then convert
   one family at a time with §4.3 options and exact fixed output/capture tests.
4. Convert mutable cap/indexIn loops to match objects/globalMatch; retain source
   lifetime and zero-width progress. Convert string overloads directly.
5. Change FTS matching types across ftshelpers, fulltextsearch, ArticleView and
   MainWindow together. Introduce FtsHighlightSpec before removing patternSyntax/
   caseSensitivity consumers. Register the custom type when a queued signal requires
   it; use typed connections. Migrate the URL producer/parser together.
6. Keep format-specific Zim lookup and website inline resolution in their rightful
   owners. Do not turn LinkRewrite into a generic HTML browser or change indexing.
7. Compile pure targets, then engine closure, then browser tests. Keep the regex ledger
   until the final audit has zero live legacy references, including comment spellings
   in compiled input files. Legacy names may remain in excluded fixture metadata.

Baseline exact regex file set (each anchor is a QRegExp match in the unchanged source):

```text
aard.cc:416                articleview.cc:485          articleview.hh:113
bgl.cc:34                  btreeidx.cc:191             dictdfiles.cc:357
dictheadwords.cc:8         dictionary.cc:28            dictserver.cc:108
dsl.cc:53                  dsl_details.cc:147          epwing_book.cc:856
folding.cc:8               ftshelpers.cc:69            ftshelpers.hh:5
fulltextsearch.cc:599      fulltextsearch.hh:6         gls.cc:834
htmlescape.cc:10           hunspell.cc:16              langcoder.cc:328
mainwindow.cc:3004         mainwindow.hh:430           mdictparser.cc:35
mdx.cc:887                 mediawiki.cc:540            sdict.cc:30
slob.cc:34                 stardict.cc:43              website.cc:16
xdxf2html.cc:721           xdxf.cc:42                  zim.cc:883
nsis/gen_uninstall/gen_uninstall.cc:13
```

The NSIS helper's patterns at `nsis/gen_uninstall/gen_uninstall.cc:13–16` are migrated
mechanically to keep the textual Qt6-only tree; Windows packaging execution remains
out of scope. Compiled vendor `qtsingleapplication/src/qtlocalpeer.cpp:81` is a separate
QRegExp consumer excluded from the design's 34-file first-party inventory but must
also compile without Qt5Compat.

### 7.2 Ordered codec conversion across all eight files

| File and baseline anchor | Exact migration |
| --- | --- |
| `website.cc:8`, `:113`, `:140`, `:219`, `:458–492` | Remove codecForHtml helper and include; use forHtml(body, raw Content-Type) then decode; byName+encode for each URL placeholder with per-placeholder catch; fix inline resolver separately |
| `epwing_book.hh:15`, `:71`, `:123–129` | Include encoding.hh; replace three owned codec pointers by Encoding values; const Encoding& accessors, no pointer ownership/null dereference |
| `epwing_book.cc:400–423`, `:553`, `:1025–1035` | Construct ISO-8859-1/GB2312/EUC-JP values, locale filename decode, QTextStream setEncoding(QStringConverter::Utf8), search exception→next codepage |
| `indexedzip.cc:136` | Encoding::forLocale().decode(QByteArray of raw filename); preserve raw filesystem bytes for file opening separately from display text |
| `gddebug.hh:6`, `:32` | Remove QTextCodec forward declaration/gdCodecForLocale; consumers include encoding.hh |
| `gddebug.cc:21–65`, `:135` | Delete Utf8CodecForLocaleReplacer/mutex and codec getter; existing log functions use explicit UTF-8, no process-global codec mutation |
| `loaddictionaries.cc:384–386` | Delete Qt4-only setCodecForCStrings block; UTF-8 input is explicit at producers |
| `slob.cc:31`, `:135`, `:191`, `:306`, `:1128` | Store Encoding value in SLOB reader, resolve header name once (unknown→UTF-8/log), accessor returns const reference, decode all text consistently |

Add byte-exact fixtures before converting a caller. Do not reuse the mutable existing
Iconv wrapper for Encoding; its callers outside this migration retain their existing
behaviour. URL template loop keeps existing placeholders, including the historical
ISO11/12 skip (`website.cc:478–491`); converter availability/tests may cover ISO-8859-11
without adding a new substitution feature.

### 7.3 Compatibility deletion and other Qt6 API mechanics

First replace every Qt4x5 call with the direct Qt6 operation, then remove includes,
then delete qt4x5.hh. Do not keep a renamed compatibility namespace.

| Wrapper/removed API | Direct result |
| --- | --- |
| keepEmptyParts/skipEmptyParts | Qt::KeepEmptyParts / Qt::SkipEmptyParts |
| middleButton/escape | Qt::MiddleButton / QString::toHtmlEscaped |
| AtomicInt::loadAcquire | object's loadAcquire |
| queryItemValue/encodedQueryItemValue | QUrlQuery(url).queryItemValue(key,FullyDecoded/FullyEncoded), explicit Latin1 conversion only for encoded bytes |
| add/remove/set query items | QUrlQuery temporary, addQueryItem/removeAllQueryItems/setQueryItems, url.setQuery |
| ensureLeadingSlash/fullPath/path/fragment/setFragment | Explicit leading slash where required; ArticleUrls::resourcePath for resource semantics; decoded QUrl path/fragment and DecodedMode for setters |
| Dom::size_type | Concrete Qt DOM count type, checked integer loop bound |
| Process::startDetached | QProcess::splitCommand followed by program/argument startDetached; empty command fails |
| Mutex recursive constructor | Mutex derives QRecursiveMutex; use templated QMutexLocker<Mutex> or type deduction, never pass it as QMutex* |
| QString::midRef / QStringRef | QStringView(source).mid/sliced for local views, owning mid/toString when stored; no view into a temporary or mutated source |
| QChar(int) | QChar(char16_t(value)) for verified BMP code units; QChar::fromUcs4/append full scalars for >FFFF; no blind truncation |
| toTime_t/fromTime_t | toSecsSinceEpoch/fromSecsSinceEpoch; range-check narrowing to existing uint32 index/ZIP timestamp fields; no index format widening |
| QTextStream::setCodec | setEncoding(QStringConverter::Utf8) at UTF-8 output sites |
| QLayout::setMargin | setContentsMargins with same value on four sides |
| Qt6 QList iterator mismatch | Derive iterator from the actual container, no QList<T*>→QList<const T*> implicit assumption |
| QObject findChild static assertion | Use the QObject-derived concrete pointer type and matching object-name overload; no cast-based suppression |

Collapse QT_VERSION/IS_QT_5 branches to their Qt6 meaning while preserving independent
Q_OS_*, HAVE_X11 and feature guards. The older condition may select a Qt5 API also
removed in Qt6; verify the resulting API rather than blindly keeping the newer arm.
First-party compatibility files requiring edits, with their baseline first anchor:

```text
aard.cc:31                 about.cc:47                 articleinspector.cc:3
articleinspector.hh:6      article_maker.cc:1115        article_netmgr.cc:19
article_netmgr.hh:9        articleview.cc:24           articlewebview.cc:17
bgl.cc:36                  btreeidx.cc:17              categorized_logging.hh:9
config.cc:25               config.hh:230               delegate.cc:13
dictdfiles.cc:25           dictheadwords.cc:14         dictionarybar.cc:215
dictionary.cc:25           dictionary.hh:429           dictserver.cc:13
dsl.cc:50                  epwing_book.cc:558          epwing.cc:499
extlineedit.cc:8           favoritespanewidget.cc:989   ffmpegaudio.cc:454
folding.cc:5               forvo.cc:252                ftshelpers.cc:17
fulltextsearch.cc:15       fulltextsearch.hh:189       gdappstyle.cc:6
gdappstyle.hh:9            gddebug.cc:11               gestures.cc:11
gestures.hh:4              gls.cc:35                   groups_widgets.cc:549
helpwindow.cc:15           hotkeywrapper.cc:30         hotkeywrapper.hh:218
htmlescape.cc:7            hunspell.cc:242             initializing.cc:8
initializing.hh:10         langcoder.cc:356            loaddictionaries.cc:384
lsa.cc:283                 main.cc:42                  mainstatusbar.cc:46
maintabwidget.cc:71        mainwindow.cc:175           mdictparser.cc:32
mdx.cc:41                  mediawiki.cc:17             programs.cc:93
scanflag.cc:7              scanpopup.cc:15             sdict.cc:32
slob.cc:39                 sounddir.cc:225             sources.cc:77
stardict.cc:50             translatebox.cc:105         treeview.cc:11
voiceengines.cc:94         website.cc:13               xdxf2html.cc:19
xdxf.cc:521                zim.cc:39                   zipsounds.cc:276
```

Additional exact mechanical inputs outside that compatibility list are
`mutex.hh:13`, `mutex.cc`, `zipfile.cc:245`, `zipfile.hh`, `dsl_details.cc`,
`ftshelpers.hh`, `mainwindow.hh`, `articleview.hh`, `articlewebview.hh`,
`keyboardstate.cc:45`, `broken_xrecord.cc:18`, and the codec/audio/UI/build files
already in §2.3. Existing public API remains except the specified Qt6 type changes;
existing ownership/thread affinity remains. These are edits, not wholesale rewrites
of unrelated algorithms. `ffmpegaudioplayer.hh` remains: the existing implementation
is inline there and ffmpegaudio.cc supplies its engine (`goldendict.pro:299`, `:366`,
`:499`); do not add a fictitious required ffmpegaudioplayer.cc source.

Compiled vendor closure is explicitly adapted too:
`qtsingleapplication/src/qtsingleapplication.pri:5` loses the version guard;
`qtsingleapplication/src/qtsingleapplication.h:71` and
`qtsingleapplication/src/qtsingleapplication.cpp:173` select Qt6 widgets paths;
`qtsingleapplication/src/qtlocalpeer.cpp:81` gets an explicit QRegularExpression include
and pattern object; `:122` loses the Qt-version part of its Unix condition. Its removed
raw-pointer checksum call at
`qtsingleapplication/src/qtlocalpeer.cpp:85` becomes `qChecksum(QByteArrayView(idc))`,
retaining Qt::ChecksumIso3309 and therefore the existing single-instance socket name.
Keep platform-only branches; do not rewrite vendored examples, bundled win/mac libraries
or OpenCC data. No external Qt5 binary of QtSingleApplication may be linked accidentally.

### 7.4 Exact goldendict.pro module and flag contract

Set:

```qmake
TEMPLATE = app
TARGET = goldendict
VERSION = 1.5.1+git
CONFIG += c++17 exceptions rtti stl
QT = core gui widgets xml network sql printsupport help svg \
     webenginecore webenginewidgets webchannel dbus pdf pdfwidgets
```

No webkit/webkitwidgets/x11extras/multimedia/core5compat module. svgwidgets is not
required: the existing SVG use is QSvgRenderer (`dsl.cc:62`, `:966`). Remove every
qmake QT_MAJOR_VERSION/QT_MINOR_VERSION branch, make wildcard.hh/.cc unconditional
(`goldendict.pro:553–556`), and remove lrelease-qt4 fallback in favour of Qt6 lrelease.
Keep current version-file generation/install/translation rules after their Qt6 path
adaptations; no build-system migration is part of this spec.

Retain existing options `zim_support`, `chinese_conversion_support`,
`no_chinese_conversion_support` where already used, `no_ffmpeg_player`, `old_hunspell`,
`no_extra_tiff_handler`, `no_epwing_support`, `verbose_build_output` and platform-only
build options. Remove no_qtmultimedia_player and MAKE_QTMULTIMEDIA_PLAYER everywhere.
Keep MAKE_FFMPEG_PLAYER conditional on !no_ffmpeg_player; MAKE_ZIM_SUPPORT,
MAKE_CHINESE_CONVERSION_SUPPORT, OLD_HUNSPELL_INTERFACE, MAKE_EXTRA_TIFF_HANDLER,
NO_EPWING_SUPPORT and HAVE_X11 preserve their existing meanings. HAVE_X11 means the
backend was compiled, not the running session is X11.

Linux dependencies retain z/bz2/lzo2, vorbisfile/vorbis/ogg/hunspell; ffmpeg+ao packages
only when enabled, libeb unless disabled, TIFF unless disabled, OpenCC when enabled,
X11/Xtst for compiled RECORD. iconv is supplied by glibc on the target (do not force
-liconv on glibc); preserve other platforms' link rules without validating those builds.
For zim_support link lzma; use `PKGCONFIG += libzstd` when packagesExist(libzstd), else
`LIBS += -lzstd`. Do not link both needlessly.

Optional KDE detection: allow KWINDOWSYSTEM_INCLUDEPATH/KWINDOWSYSTEM_LIBDIR qmake
variables; default Linux header probe is `/usr/include/KF6/KWindowSystem/kwindowsystem.h`
and default library search uses the compiler's system paths for KF6WindowSystem.
If the header and library are found, add its include path, `-lKF6WindowSystem`, and
HAVE_KWINDOWSYSTEM; otherwise omit all three and keep best-effort requestActivate.
No optional KDE absence may break core Wayland startup.

Add every new production .hh/.cc from §2.2 to HEADERS/SOURCES and new JS entries to
resources.qrc. Delete qt4x5 and multimedia file entries. Keep ffmpegaudioplayer.hh and
ffmpegaudio.hh/.cc with existing feature guards. Add root gate targets from §6.4;
tests themselves are not added to the application's SOURCES.

### 7.5 Integration ordering constraints for the planner

The following dependency order is normative even if work is split across lanes:

1. Test harness and the production `DataRequestReader`/factory signatures in
   `articleschemes.hh`, then the test-support reader factory, shared probes and all
   three `ControlledDataRequest` setters; pure conversion fixtures and
   Encoding/LinkRewrite/URL/FTS value interfaces. Setters alone are not a completed
   fixture: establish their association with the per-job injected reader first.
2. Pure codec/regex/Qt6 utility conversion and compatibility deletion, keeping affected
   caller/signal/URL interfaces integrated together. Establish a Qt6-compilable closure.
3. Pre-app scheme registration, handler/device and profile scaffolding; implement the
   stateless production reader, per-job factory wiring, GUI full/slice catch boundaries
   and producer-free IO copy catch boundary before handler reply integration. Establish
   the mutex-protected device state and GUI cancellation/lifetime handoff, then prove
   all three forced operations through those real boundaries with the §6.3 probe,
   single-failure and no-success/no-second-reply assertions. Implement final-size Range
   resolution and pre-reply buffer positioning (ordinary, suffix, invalid and
   unsatisfiable cases) before Range reply integration; prove delivered bytes and
   metadata, including `bytes=0-`, nonzero starts and `bytes=-2`. Prove real
   gdlookup/bres/gdpicture loading and the zero-size live WebEngine stream contract
   before migrating complex article actions.
4. Bridge and frame helper spike with two origins and **second frame navigation**;
   prove nonce/staleness and coordinates before broad UI parity work depends on it.
5. Navigation/history/cached signals, then the `ArticleUrls` UserAction/retained-handler dispatch contract (`group`, FTS subset, `dict` and `gdanchor`), then save/find/FTS/print/DevTools on that bridge.
6. Desktop backend/syntax/config/platform work can proceed independently of renderer
   internals, but constructor injection, shutdown order and preferences integration
   need a single integration owner. Within portal work, validate ListShortcuts wire shape,
   historical-display filtering and display revisions before applying BindShortcuts
   subsets; listing must not gate the one-bind-per-session path.
7. Profile/interceptor/proxy/auth/cache and page-host snapshot policy must land before
   external-content acceptance; a global host union is never an interim accepted state.
8. Complete root gate mutation evidence and Qt6-only audit, then live gates/docs.

Do not schedule the 33 synchronous JS sites as isolated mechanical replacements.
They depend on the queue, cached getters, capture barrier and view lifetime. Do not
let a print-preview lane call asynchronous WebEngine print inside paintRequested.
Do not delete QNAM entirely: downloads/inline websites still require it. Do not let
audio config migration overwrite stored text in a no_ffmpeg_player build.

## 8. Risks and concrete detection

| Risk / implementation signal | Detection that must fail or produce explicit evidence |
| --- | --- |
| Parser silently changes because HTML dot stops at newline or Unicode classes become ASCII | tst_regex source-tagged fixed captures/output plus tst_linkrewrite multilingual/multiline cases |
| Encoding depends on ICU-enabled Qt or silently transliterates | No Qt5Compat/QStringDecoder codec path in source audit; exact vectors and unrepresentable cases in tst_encoding |
| Stream drops final bytes because completion wins read race | tst_article_stream finishes before draining final chunk and compares full payload/EOF sequence |
| Immediate DataRequest finishes before connections exist | tst_article_load instant resource and tst_article_stream constructor-completed cases |
| Bootstrap only stamps queued calls, not later one-argument calls | tst_article_bridge post-ready and post-dictionary-switch notifications must update current generation |
| Old callback advances a new queue or touches deleted view | tst_article_bridge reset/delayed callback/destruction with guarded completion counts |
| iframe discovered only by id, reinstrumented once, or never remeasured | tst_website_frames checks id+name and repeats full assertions after navigation with changed docId/height |
| Cross-origin workaround changes base/origin or drops port | Two real origin servers assert Host/port, relative form/image/CSS/fetch; no re-serving frame content |
| Coordinate conversion doubles scroll or zoom | Nested frame, nonzero border, scrolled article and 200% zoom word fixture |
| Enabled-dictionary union grants A access to B | tst_external_content loads both dictionary contributions then verifies A→B blocked |
| IO interceptor touches widgets/config/filesystem | Review interceptRequest's dependency closure and immutable-policy fields; tests replace policy during repeated requests without UI callback waits |
| Missing initiator treated as privileged gdlookup | Direct decision table fixture plus real custom-document requests in tst_external_content |
| QWebEngine cache 0 enables automatic cache | tst_external_content/profile assertions check NoCache for nonpositive preference and runtime transition |
| Origin authentication edits proxy credentials | Origin/proxy challenge fixtures compare all proxy fields before/after and check no origin persistence |
| Save appears successful before downloads or publishes half a resource set | tst_save_article barrier, injected write/rename failure, destination reopen and cleanup checks |
| History captured after navigation or keyed by URL | tst_article_history outgoing-state values and duplicate-URL indices; ordinary navigation must retain A entry |
| PDF buffer dies while document/preview still uses it | tst_print_pipeline preview→page change→print after snapshot callback scope ends |
| Preview regeneration accidentally reflows or calls asynchronous print | tst_print_pipeline holds snapshot hash/page count constant and checks synchronous render metrics |
| Portal Response races method return or edit leaks session | FakePortal fast-response/alternate-path/late-session scenarios; inspect Close calls and current generation |
| Portal binds twice, assumes requested trigger, or treats Configure as request | FakePortal rejects second Bind; subset output drives display; v2 Configure has no Response |
| Global shortcut state falsely describes clipboard polling as fallback | tst_hotkeys capability text assertions and manual absent/denied Wayland preference states |
| Xlib used on native Wayland because HAVE_X11 is true | tst_platformwindow runtime guard, native live Wayland trace; explicit xcb RECORD validation separately |
| Test gate only wraps build or missing environment skips | Four probes in verify-test-gate.sh; broken tests-run negative control must fail observation |
| Compiled vendor source reintroduces removed Qt APIs | Vendor closure audit/build, notably qtlocalpeer.cpp; actual application linker inspection |
| Exit destroys profile before pages or waits indefinitely for clear | Cache-clear success/timeout fixture plus shutdown review; one exitCachesReady within 5 s bound |
| no_ffmpeg build damages preference portability | Separate build and legacy.xml round-trip exact backend text check |
| Native desktop focus cannot be forced | Live portal activation observation; documented best-effort request, no boolean success assertion |
| Chromium/runtime packaging overhead | README lists QtWebEngine runtime requirements; renderer handshake gate fails missing runtime rather than passing blank-window smoke |

## 9. Out of scope and documented parity limits

The migration does not add CMake, parallel Qt4/Qt5 support, Windows/macOS build
validation, dictionary/indexing redesign, FTS index-format changes, visual restyling,
Wayland InputCapture hover translation, a Wayland surface-export helper, or config
schema changes beyond additive portal display data and the specified in-memory audio
migration. Existing platform-specific code is retained only with mechanically corrected
Qt6 APIs; its presence is not a validated release for those platforms.

Document in README.md and docs/wayland-limitations.md:

- Website iframe mode honours the site's X-Frame-Options/CSP restrictions; inline mode
  remains the existing escape hatch, with its URL-base/port bug corrected.
- Portal shortcuts accept one combination; existing two-key defaults/sequences remain
  stored and visibly unavailable until changed. Clipboard-change scanning and explicit
  lookup remain separate behaviours. Hover translation and X11 focus restoration
  remain X11-only.
- Portal dialogs are unparented on Wayland because parent_window is empty. Activation
  requests may be declined by the compositor.
- Print preview and direct print render a frozen PDF snapshot. New printer layout
  metrics change scale/render resolution without HTML reflow.
- QtMultimedia internal playback and WebKit browser plugins are unavailable; ffmpeg
  and external playback remain, with the specified audio-preference migration.
- Chromium treats HttpCachingProxy as an HTTP proxy; retained QNAM downloads resolve
  independently under the same settings. Third-party response headers are honoured.

No unresolved design option, alternate backend, new permission workflow or placeholder
implementation is authorized by this specification. Acceptance requires the concrete
behaviour and evidence in §§5–8.
