# GoldenDict Qt6 and native Wayland Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the deterministic
> subagent-driven-development controller to implement this plan task-by-task.
> Leave the checkboxes unchanged; controller state records execution.

**Goal:** Deliver the approved Qt6-only GoldenDict migration, preserving dictionary/index formats and article workflows on native KDE Plasma 6 Wayland and explicit X11.

**Architecture:** Build independently testable production closures first, then connect the shared WebEngine profile, cancellable scheme jobs, generation-stamped bridge and frame helpers to ArticleView. Application history and save/print transactions consume that asynchronous renderer; one desktop integration owner connects both render and hotkey systems to MainWindow, popup, preferences and shutdown. The final application switch and acceptance gates integrate the completed closures.

**Tech Stack:** Linux, Qt 6.12, GCC 16, C++17, qmake6, QtWebEngine, QtWebChannel, QtDBus, QtPdf, Qt Test, iconv from glibc, existing ffmpeg/libao and X11 RECORD, optional KF6WindowSystem.

### Authority and compilation boundary

Read `docs/superpowers/specs/2026-10-24-qt6-migration-spec.md` and the revision-6 design beside it. The specification's SD decisions, including the corrected SD-06/SD-16 Range and reader contracts, control implementation. Where sketches differ, every producer and consumer uses the spec's exact `registerArticleSchemes()`, `ArticleProfileController`, page-local `ArticleRequestInterceptor`, `ArticleBridge`, `WebsiteFrames` and `ArticleHistory` interfaces. References to specification sections below are requirements, not permission to invent interfaces.

The baseline root `qmake6` fails on unavailable WebKit/X11Extras before compilation. Tasks 1–33 establish Qt6-compilable production/test closures without selecting an incomplete renderer in the root application. Every task must compile its entire affected closure and pass every test it has introduced; an expected red run never constitutes a committable result. Task 34 atomically switches the root application after all consumers exist, and Tasks 34–38 additionally require the whole application to build. This explicitly distinguishes the inherited root failure from a newly broken target; do not claim a whole-application Qt6 build for an earlier closure task or create interim disabled product features to obtain one.

### File allocation and concurrent lanes

- `build`: only `goldendict.pro`, deletion of `qt4x5.hh`, and `README.md`.
- `mechanize`: mutex/string/time/Qt widget utility and compiled QtSingleApplication files listed in Tasks 5–7; never article view or hotkeys.
- `codec`: Encoding, LinkRewrite, parser/dictionary/FTS files listed in Tasks 2–3 and 8–10, plus `tst_encoding`, `tst_linkrewrite`, `tst_regex`, `tst_htmlescape`, `tst_langcoder` and their encoding/link/regex fixtures.
- `webcore`: articleurls, articleschemes, articleprofile, article_netmgr, article_maker and `tst_article_schemes`, `tst_article_stream`, `tst_external_content`.
- `webview`: articlewebview, articlebridge, websiteframes, articleview, articleinspector, resources.qrc, the three production JS files, `tst_article_load`, `tst_article_bridge`, `tst_website_frames`.
- `workflow`: articlehistory, savearticle, printpipeline, `tst_article_history`, `tst_save_article`, `tst_print_pipeline`.
- `desktop`: hotkey/config/audio/startup/MainWindow/popup/preferences/platform consumers and `tst_hotkeys`, `tst_portal_hotkeys`, `tst_platformwindow`, config fixtures.
- `tests`: aggregate/support projects, shared fixture implementations, browser/print fixture data, `tst_utf8_wstring`, three gate scripts and acceptance/limitations documents.

Each lane removes legacy API/version references in its own files. The build lane deletes the shared compatibility header only after all owners have removed its consumers. Feature test projects and sources belong to their feature lane; the tests lane alone owns shared support. Do not edit another lane's files to satisfy an integration dependency. The numbered dependency graph, including the preceding task in each lane, is authoritative.

### Coverage and task map

| Specification requirement | Implementing tasks |
| --- | --- |
| §§2.4,4.1,7.2 Encoding and caller policies | 2,8 |
| §§4.2–4.3,7.1 regex/link/inline/FTS semantics | 3,8–10,24–25 |
| §§2.5–2.6,3.1,4.4 URL/reader/stream/Range | 4,14–17,23 |
| §§2.7,3.2,4.5–4.7 async bridge/frame parity | 19–20,22–25,29 |
| §§2.8–2.9,3.3,3.5,4.9–4.10 workflows | 21–23,27–29 |
| §4.8 policy/profile/proxy/auth/cache | 18,22,26,33 |
| §§2.10,3.4,4.11–4.13 desktop/config/audio | 11–13,30–33 |
| §§6.1–6.3 all 18 targets/support/fixtures | 1–4,8–29,30–33 |
| §§6.4–6.6 gates/live/parity and §9 limits | 34–38 |
| §§7.3–7.4 mechanics/vendor/options | 5–13,15,22,29,33–34 |
| §5 diagnostics/runtime startup readiness | owner tasks throughout, 33,35,37–38 |

## Global Constraints

- Target: Linux, Qt 6.12, GCC 16, KDE Plasma 6, native Wayland; explicit X11 remains supported.
- `TEMPLATE = app`, `TARGET = goldendict`, `VERSION = 1.5.1+git`, `CONFIG += c++17 exceptions rtti stl` in the final root project.
- Final modules are `core gui widgets xml network sql printsupport help svg webenginecore webenginewidgets webchannel dbus pdf pdfwidgets`; no Qt5Compat, QtMultimedia, QtWebKit or X11Extras dependency.
- Preserve dictionary formats, indexing algorithms, on-disk signatures and `FtsHelpers::CurrentFtsFormatVersion`; do not widen timestamp/index fields.
- No synchronous JS-result shim or nested event loop is introduced. Every asynchronous article operation has a document generation and a guarded owner.
- One named `goldendict-articles` profile serves tabs, popup and DevTools; every page dies before it. Registration of all seven schemes precedes application construction.
- New domain failures use specified signals/results except `Encoding::encode`'s `exUnrepresentable`; reader exceptions are caught at their production handler/device boundaries. Destructors do not throw.
- GUI owns jobs, producers, connections, cancellation and handler maps. Chromium IO-thread device methods use only mutex-protected device state and a producer-free reader; cancellation runs after releasing its lock.
- Reader failure switches/probes exist only in `tests/support/dictionary_stub.hh/.cc`. Do not hide the nonvirtual DataRequest getters or introduce production forced-failure switches.
- Any Range field selects completion buffering. Resolve/seek ordinary and suffix ranges before reply while retaining full backing size; unsatisfiable ranges fail `RequestFailed`. No-Range media uses a sequential live device with `size()==0` and a full 200 body.
- Script generations start at 1; keep the stamped facade after readiness, install bootstrap/DOM/frame scripts before controlled navigation, and disable BackForwardCacheEnabled. Stale results cannot advance a new queue.
- Frame identity includes generation, verified native child path/value and document nonce. Prove second-frame-navigation reinstrumentation before history/save/find/print integration.
- Page-local immutable `ArticleRequestPolicy` uses only contributing dictionary hosts; `ArticleRequestInterceptor::setPolicy(std::shared_ptr<ArticleRequestPolicy const>)` atomically publishes it for that page. No global union, URL-keyed shared permissions, widget/config/filesystem access or blocking GUI invocation on the interceptor's IO thread.
- GUI attachment seam: ArticleProfileController is the QObject parent of its named QWebEngineProfile. ArticleWebView::configure, temporary createWindow pages and DevTools call the owning controller's attachPage before loading. attachPage owns one direct-child ArticleRequestInterceptor named `gdArticleRequestInterceptor` per page; GUI consumers obtain that QObject child and checked dynamic_cast to the concrete type (RTTI is enabled). No new public profile wrapper/constructor parameter or cross-thread widget lookup. Controller refreshes blockExternal on copies of each current snapshot, preserving its page's hosts/file mappings.
- `FtsHighlightSpec` is defined once in webcore-owned articleurls.hh, including its complete spec fields, and consumed read-only by the codec/renderer lanes. Task 8 makes shared FTS declarations Qt6-safe before archiving dictionary callers; Task 9 converts wildcard/engine implementations; Task 10 removes fulltextsearch.cc's MainWindow dependency. No early closure includes an unconverted MainWindow or ArticleView.
- Tasks 14–21 exercise the real profile/bridge/frame/history modules with QWebEnginePage before ArticleView switches in Task 22. Task 14 provides the production reader delegate before Task 15 links throwing-reader support. History keys are actual `QWebEngineHistory::currentItemIndex()` values and distinguish absent state from zero offsets; the view owns the single navigation coordinator.
- Capture deadline is 500 ms with the latest 100 ms-debounced state fallback. Frame sampling is 250 ms, three equal heights or 10 s. PDF generation timeout is 30 s. Exit cache barrier and portal request deadlines are 5 s.
- Save HTML publication is the transaction commit point after asset-directory rename. QSaveFile direct-write fallback is disabled; cancellation/failure preserves preexisting destinations and removes only owned output.
- Use `InternalPlayerBackend::defaultBackend()`; keep its ffmpeg factory private. In `no_ffmpeg_player` builds preserve stored backend text verbatim, hide internal playback and retain external playback.
- Portal binding IDs are `main-window` and `clipboard-lookup`; one BindShortcuts attempt per session; no recovery retry, restore token or Wayland X11 fallback. ListShortcuts historical display data never activates IDs or applies the Bind subset rule.
- Respect an explicitly chosen QPA. HAVE_X11 is a build capability, never evidence of active X11. Activation is best-effort and returns void; never log credentials or activation tokens.
- Required tests fail on unavailable environments through QVERIFY2; no required QSKIP. Pure binaries have a 60 s watchdog, Browser/Article binaries 120 s, aggregate command 900 s. Use aggregate deadlines inside tests.
- Private portal bus: `dbus-daemon --session --nofork --print-address=1`; browser fixtures bind port 0 on `127.0.0.1`, `127.0.0.2`, `127.0.0.3`. Never acquire the real portal name on the desktop bus.
- Root `make check` must build/run exactly the 18 specified targets. Its wrapper is expanded as `$(GD_TEST_WRAPPER)` at make time and wraps both tests-build and tests-run separately.
- Only the approved three scripts are new executable gate scripts: `scripts/test-wrapper.sh`, `scripts/verify-test-gate.sh`, `scripts/check-qt6-only.sh`. All four probes must be proven before any live acceptance task.
- Every task's green run includes its named suite plus the accumulated `qmake6 tests/tests.pro -o tests/Makefile && make -C tests -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check`; after Task 34 also run `qmake6 && make -j$(nproc)` and wrapped root `make check`.
- Until Task 34 compile the changed production closure with the test-only archive mode from Task 1 where a full application consumer has not switched. Never commit a newly broken closure, unimplemented product method or a fake passing testcase.
- Ownership is a write allowlist. Consumed interfaces may be read, not modified. Resolve cross-lane changes in the named owner task before the consuming task begins.
- Commit only listed source/test paths with each task's exact conventional message. Keep generated objects, Makefiles, logs, user dictionaries and private credentials outside commits.

## Task 1: Establish the test aggregate, environment guards and pure baseline

**Implementer tier:** Advanced
**Lane:** tests
**Depends on:** none
**Ownership:**
- `tests/tests.pro`
- `tests/testsupport.pri`
- `tests/support/fixtures.hh`
- `tests/support/fixtures.cc`
- `tests/tst_utf8_wstring/tst_utf8_wstring.pro`
- `tests/tst_utf8_wstring/tst_utf8_wstring.cc`

**Files:** Create every owned file. Test production `utf8.cc`, `wstring.cc`, `wstring_qt.cc` without modifying them.

**Interfaces:** Consumes `Utf8::encode(gd::wstring const &) -> std::string`, `Utf8::decode(std::string const &) -> gd::wstring`, `gd::toQString(wstring const &) -> QString`, `gd::toWString(QString const &) -> wstring`. Produces `TestSupport::requiredEnvironment(QStringList * reasons) -> bool`, shared watchdog/fixture lookup setup, and the qmake aggregate/compile-closure contracts below.

- [ ] **Step 1: Write the failing harness and conversion tests.** Implement initTestCase with `QStringList reasons; QVERIFY2(TestSupport::requiredEnvironment(&reasons), qPrintable(reasons.join('\n')));`. Assert ASCII, embedded NUL, `f09f9880` supplementary conversion and invalid UTF-8 exception; add `QVERIFY2(qEnvironmentVariable("GD_TEST_FORCE_FAILURE") != "1", "GD_TEST_FORCE_FAILURE intentional assertion");` only in this target. First red run must fail because fixtures support is absent.
- [ ] **Step 2: Confirm red.** Run `qmake6 tests/tst_utf8_wstring/tst_utf8_wstring.pro -o tests/tst_utf8_wstring/Makefile && make -C tests/tst_utf8_wstring -j$(nproc) && xvfb-run -a timeout 60 tests/tst_utf8_wstring/tst_utf8_wstring`; record the missing-support compile/link diagnostic.
- [ ] **Step 3: Implement support and the aggregate.** `requiredEnvironment` returns false with reasons for `GD_TEST_FORCE_ENV_UNAVAILABLE=1`, missing fixture files or suite-selected prerequisites. Testsupport supplies root includes, feature definitions matching the application, per-target object/MOC/UI directories, and a wall watchdog that terminates a wedged binary even if its event loop stalls. All `.pro` files are app/testcase/c++17/testlib projects. In ordinary aggregate mode, discover existing `tst_*/tst_*.pro` files and register each as a subdir project; add newly implemented suites automatically, never generate passing placeholder suites. Final exact-18 enforcement lands in Task 35. Provide test-only `GD_COMPILE_CLOSURE=1` mode with TEMPLATE=lib/CONFIG+=staticlib and `GD_COMPILE_SOURCES`, `GD_COMPILE_HEADERS`, `GD_COMPILE_FORMS` repository-relative input lists, translated against the repository root and including testsupport. This creates a syntax/MOC/UI-checked archive, not an application or additional testcase. Core baseline code is:

```cpp
const QString text = QString::fromUtf8("A\xf0\x9f\x98\x80", 5);
QCOMPARE(gd::toQString(gd::toWString(text)), text);
const gd::wstring wide = Utf8::decode(std::string("\xf0\x9f\x98\x80", 4));
QCOMPARE(Utf8::encode(wide), std::string("\xf0\x9f\x98\x80", 4));
```

- [ ] **Step 4: Confirm green and falsify the guard.** Repeat Step 2, then `qmake6 tests/tests.pro -o tests/Makefile && make -C tests -j$(nproc) && xvfb-run -a timeout 900 make -C tests check`. Run `GD_TEST_FORCE_FAILURE=1 xvfb-run -a timeout 60 tests/tst_utf8_wstring/tst_utf8_wstring` and `GD_TEST_FORCE_ENV_UNAVAILABLE=1 xvfb-run -a timeout 60 tests/tst_utf8_wstring/tst_utf8_wstring`; each must fail at the specified assertion, while the unforced run passes. Check test-only archive mode on `utf8.cc wstring.cc wstring_qt.cc`.
- [ ] **Step 5: Commit.** Run `git add tests/tests.pro tests/testsupport.pri tests/support/fixtures.hh tests/support/fixtures.cc tests/tst_utf8_wstring && git commit -m "test(qt6): establish guarded qmake test harness"`.

## Task 2: Implement immutable iconv Encoding with fixed scalar oracles

**Implementer tier:** Capable
**Lane:** codec
**Depends on:** Task 1
**Ownership:**
- `encoding.hh`
- `encoding.cc`
- `linkrewrite.hh`
- `gddebug.hh`
- `gddebug.cc`
- `tests/tst_encoding/tst_encoding.pro`
- `tests/tst_encoding/tst_encoding.cc`
- `tests/fixtures/encoding/vectors.json`

**Files:** Create the owned modules/tests/JSON and migrate `gddebug.hh/.cc` now so this foundational Core closure can link the real UTF-8 logger. `linkrewrite.hh` establishes only the exact pure declarations consumed in Task 3; no linked implementation is claimed yet.

**Interfaces:** Produces `Encoding() noexcept`, `static std::unique_ptr<Encoding> byName(QString const &)`, `static Encoding utf8()`, `static Encoding forLocale() noexcept`, `static Encoding forHtml(QByteArray const &, QByteArray const &, QString const & = QStringLiteral("UTF-8"), DetectedBy * = nullptr)`, `bool isValid() const noexcept`, `QString name() const`, `QString decode(QByteArray const &) const noexcept`, `QByteArray encode(QString const &) const`, `DetectedBy {Header,Bom,Meta,XmlDeclaration,Fallback}` and `Encoding::exUnrepresentable`. Also declares the four exact `LinkRewrite` functions in Task 3. Consumes Task 1 test setup and existing `DEF_EX` macros; no shared mutable Iconv object.

- [ ] **Step 1: Write fixed-vector failures.** Populate every minimum vector from spec §6.2, including ISO-8859-11 and deliberately unknown ISO-8859-12. Fix BIG5-HKSCS extension `8745 -> U+27267` and GB18030 supplementary `9439fc36 -> U+1F600` against published mapping data before committing; expected JSON must never be computed with Encoding. Cover explicit-endian BOM/no-BOM, generic UTF-16/32 BOM behavior separately, embedded NUL, output growth, ISO-2022-JP flush, two bad bytes vs incomplete suffix, all detection conflicts/order and parallel calls on one value. Add these direct assertions:

```cpp
QCOMPARE(Encoding::utf8().decode(QByteArray::fromHex("ffff")), QString(2, QChar(0xfffd)));
QCOMPARE(Encoding::utf8().decode(QByteArray::fromHex("e282")), QString(QChar(0xfffd)));
auto sjis = Encoding::byName(QStringLiteral("Shift_JIS"));
QVERIFY(sjis);
QCOMPARE(sjis->decode(QByteArray::fromHex("93fa967b")), QString::fromUtf8("日本"));
QCOMPARE(sjis->encode(QString::fromUtf8("日本")).toHex(), QByteArray("93fa967b"));
QVERIFY_EXCEPTION_THROWN(Encoding::byName("ISO-8859-1")->encode(QString::fromUtf8("中")), Encoding::exUnrepresentable);
```

- [ ] **Step 2: Confirm red.** Run `qmake6 tests/tst_encoding/tst_encoding.pro -o tests/tst_encoding/Makefile && make -C tests/tst_encoding -j$(nproc) && xvfb-run -a timeout 60 tests/tst_encoding/tst_encoding`; require missing Encoding implementation or a concrete vector mismatch.
- [ ] **Step 3: Implement spec §4.1.** Store canonical name/decode-only marker only; private RAII iconv descriptors convert via UTF-32LE scalars and validate surrogate/scalar bounds. E2BIG grows without consuming input; EILSEQ appends one replacement and skips exactly one byte/reset; EINVAL adds one replacement for the tail; flush errors follow the specified direction's policy. Encoder validates before conversion, rejects nonreversible conversion and returns no partial output. Detection inspects only the first 1024 raw bytes, honors first recognized candidates and matching initial BOM rules. Locale fallback is CODESET, then C/POSIX→ASCII, otherwise UTF-8; decode-only encode throws. No `//IGNORE`/`//TRANSLIT`, global locale mutation, Qt codec dependency or forced failure hook. Remove `gdCodecForLocale`, QTextCodec, Utf8CodecForLocaleReplacer and global codec mutation from gddebug.hh/.cc; retain every logging entry signature and existing synchronization, using explicit `QString::vasprintf(...).toUtf8()` output. Link those real logger sources in the test. Task 8 migrates the remaining locale callers to Encoding::forLocale.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and the accumulated tests. Assert all converter names open, descriptors are independent, unknown aliases fall back as specified and diagnostic rate is at most one recovery warning per call. Caller-policy tests are added against production callers in Task 8.
- [ ] **Step 5: Commit.** `git add encoding.hh encoding.cc linkrewrite.hh gddebug.hh gddebug.cc tests/tst_encoding tests/fixtures/encoding && git commit -m "feat(encoding): add immutable iconv scalar conversion"`.

## Task 3: Centralize link rules and establish the source-tagged regex ledger

**Implementer tier:** Advanced
**Lane:** codec
**Depends on:** Task 2
**Ownership:**
- `linkrewrite.hh`
- `linkrewrite.cc`
- `audiolink.hh`
- `audiolink.cc`
- `tests/tst_linkrewrite/tst_linkrewrite.pro`
- `tests/tst_linkrewrite/tst_linkrewrite.cc`
- `tests/tst_regex/tst_regex.pro`
- `tests/tst_regex/tst_regex.cc`
- `tests/fixtures/linkrewrite/cases.json`
- `tests/fixtures/regex/cases.json`

**Files:** Create LinkRewrite implementation, both test projects/sources and fixtures; modify audiolink only if extracting its existing wrapper is required. Baseline anchors: StarDict 468–632; dictdfiles 348–399; dictserver 813–887.

**Interfaces:** Consumes `Encoding::utf8()` only if text decoding is necessary and the existing addAudioLink output contract. Produces `LinkRewrite::applyGdLookup(QString const &) -> QString`, `applyBres(QString const &, QString const &) -> QString`, `applyGico(QString const &, QString const &) -> QString`, `applyGdau(QString const &, QString const &) -> QString`. Ledger rows contain source/family/old options/input/expected captures and replacement/new options; `tst_regex` loads fixed rows and links actual production helpers as their owners migrate them.

- [ ] **Step 1: Write failures for all §4.2 examples and §4.3 semantic traps.** Assert exact output/quotes/whitespace, idempotence, absolute/protocol-relative/fragment/data exclusions, root-relative resources, qualified icons and two separate multiline audio matches. Preserve `dict`, `gdanchor` and fragments structurally:

```cpp
const QString source = "<a href=\"gdlookup://localhost/cat?dict=Known%20Name&gdanchor=part#frag\">x</a>";
QCOMPARE(LinkRewrite::applyGdLookup(source), source);
QCOMPARE(LinkRewrite::applyGdLookup(LinkRewrite::applyGdLookup(source)), source);
QCOMPARE(LinkRewrite::applyBres("<img src='a.png'>", "d"), QString("<img src='bres://d/a.png'>"));
QCOMPARE(LinkRewrite::applyGico("gico://d/dicticon.png", "other"), QString("gico://d/dicticon.png"));
```

- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_linkrewrite/tst_linkrewrite.pro -o tests/tst_linkrewrite/Makefile && make -C tests/tst_linkrewrite -j$(nproc) && xvfb-run -a timeout 60 tests/tst_linkrewrite/tst_linkrewrite`. Generate/run `tst_regex` with `qmake6 tests/tst_regex/tst_regex.pro -o tests/tst_regex/Makefile && make -C tests/tst_regex -j$(nproc) && xvfb-run -a timeout 60 tests/tst_regex/tst_regex`; require missing link functions or ledger mismatch, never a missing environment.
- [ ] **Step 3: Implement immutable-span rewriting.** Move the named patterns/capture order first, compile/validate QRegularExpression, use match objects/globalMatch and explicit lazy/Unicode/dot options. Parse lookup suffix as URI, decode/re-encode only path headword once, preserve query and fragment components. Use original audio BODY/addAudioLink output and only add the icon when BODY lacks img. Pin quote/backslash/Unicode `makeAudioLinkScript` output, retaining `gdAudioLinks` and its `.first` field; bridge/bootstrap work consumes that same map. Emitted replacements are never rescanned. Put no Zim lookup callback, DICT brace parser or remote HTML resolver in LinkRewrite.
- [ ] **Step 4: Confirm green.** Repeat both commands and accumulated tests. Ledger must distinguish greedy `"a1b22"` count 2 from inverted/minimal count 3, anchored `abc` rejecting `abc\n`, zero-width termination, Unicode captures and literal `\xHHHH` migration expectations. Task 9 connects engine-specific expectations to production normalizers.
- [ ] **Step 5: Commit.** `git add linkrewrite.hh linkrewrite.cc audiolink.hh audiolink.cc tests/tst_linkrewrite tests/tst_regex tests/fixtures/linkrewrite tests/fixtures/regex && git commit -m "refactor(links): centralize tested Qt6 link rewriting"`.

## Task 4: Define URL, FTS and reader contracts and implement pure URL dispatch

**Implementer tier:** Advanced
**Lane:** webcore
**Depends on:** Task 1, Task 2
**Ownership:**
- `articleurls.hh`
- `articleurls.cc`
- `articleschemes.hh`
- `tests/tst_article_schemes/tst_article_schemes.pro`
- `tests/tst_article_schemes/tst_article_schemes.cc`

**Files:** Create all owned files. This suite initially links pure articleurls; handler/device/browser coverage is added by Tasks 14–17. Header-only reader declarations are syntax-checked without generating incomplete QObject implementations.

**Interfaces:** Define exactly `FtsHighlightSpec {QString sourcePattern; QRegularExpression expression; bool wildcards=false; bool matchCase=false;}` once in articleurls.hh, and the spec §2.5 `ArticleUrls::Use`, `Kind`, `Lookup`, `Parsed` types with every field, including `Lookup::phrase` as Config::InputPhrase and `Lookup::highlight` as the complete FtsHighlightSpec value. Define exactly `Parsed parse(QUrl const &, Use)`, `QUrl lookupDocument(Lookup const &)`, `QUrl pictureDocument(QString const &, QString const &)`, `QByteArray encodeContexts(QMap<QString,QString> const &)`, `bool decodeContexts(QByteArray const &, QMap<QString,QString> *, QString *)`, `QString resourcePath(QUrl const &)`. Include config.hh for InputPhrase; its declarations compile on Qt6 before Task 11 converts config.cc. Do not include dictionary.hh, ftshelpers.hh, MainWindow or ArticleView in the pure URL closure. Define the shared `NavigationKind {NewLookup,LinkClick,Back,Forward,Reload,Redirect}` enum here for ArticleWebPage and ArticleHistory to consume without a circular header dependency. Declare exactly `DataRequestReader::{fullData(Dictionary::DataRequest &) const -> QByteArray, dataSlice(Dictionary::DataRequest &, size_t,size_t,void *) const -> void, copyStreamBytes(char *,char const *,qint64) const -> void}`, nonthrowing virtual destructor, `makeDataRequestReader() -> std::shared_ptr<DataRequestReader const>` and `DataRequestReaderFactory = std::function<std::shared_ptr<DataRequestReader const>(sptr<Dictionary::DataRequest> const &)>`. Declare `registerArticleSchemes()` and forward-declare Dictionary::DataRequest/resource manager plus handler/device/interceptor classes, using existing sptr.hh. Tasks 14,16,18 add their complete spec §2.6 QObject declarations together with implementations, so MOC never generates a vtable for unfinished virtual methods. Task 14 supplies the production reader delegate after the dictionary closure compiles.

- [ ] **Step 1: Add URL tests covering both modes.** Test blank, checked unsigned group/zero, explicit dictionaries, literal plus, Unicode/percent paths, all scalar duplicates, invalid ports/credentials, resource query suffix, full picture→bres components and context version/status/trailing-byte rejection. Key assertions:

```cpp
QCOMPARE(ArticleUrls::parse(QUrl("gdlookup://localhost/cat"), ArticleUrls::Use::UserAction).kind, ArticleUrls::Kind::LookupAction);
QCOMPARE(ArticleUrls::parse(QUrl("gdlookup://localhost/cat"), ArticleUrls::Use::DocumentLoad).kind, ArticleUrls::Kind::Invalid);
const auto action = ArticleUrls::parse(QUrl("bword:cat?dict=Known%20Name&gdanchor=part"), ArticleUrls::Use::UserAction);
QCOMPARE(action.lookup.dictionaryName, QString("Known Name"));
QCOMPARE(action.lookup.anchor, QString("part"));
QCOMPARE(ArticleUrls::resourcePath(QUrl("bres://d/a%20b.png?q=x+y")), QString("a b.png?q=x+y"));
```

- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_schemes/tst_article_schemes.pro -o tests/tst_article_schemes/Makefile && make -C tests/tst_article_schemes -j$(nproc) && xvfb-run -a timeout 60 tests/tst_article_schemes/tst_article_schemes`; require missing URL implementation/assertion failure.
- [ ] **Step 3: Implement §4.4 pure parsing/production.** Pin context QDataStream to Qt_5_0, strict Base64/status/no trailing bytes, clear output on error. Keep action fields until the retained handler resolves reference context; never fabricate group 0 for an ordinary group-less DocumentLoad. Use NonPathWildcardConversion and separate wildcard/source fields. Dictionary IDs are canonical hosts; only one structural leading slash is stripped. Parser errors use Parsed.error/result, not domain throws.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated tests; round-trip contexts and every lookup field, while malformed contexts/duplicate scalar keys fail. Compile static assertions for reader/factory signatures; do not link nonexistent handler bodies yet.
- [ ] **Step 5: Commit.** `git add articleurls.hh articleurls.cc articleschemes.hh tests/tst_article_schemes && git commit -m "feat(article): define validated URL and reader contracts"`.

## Task 5: Adapt recursive locking, scalar utilities and fixed-width time APIs

**Implementer tier:** Standard
**Lane:** mechanize
**Depends on:** Task 1
**Ownership:**
- `mutex.hh`
- `mutex.cc`
- `zipfile.hh`
- `zipfile.cc`
- `wstring.hh`
- `wstring.cc`
- `wstring_qt.hh`
- `wstring_qt.cc`
- `utf8.hh`
- `utf8.cc`
- `categorized_logging.hh`

**Files:** Modify owned existing files at mutex.hh:13, zipfile.cc:245 and scalar/time/compatibility occurrences. Test using Task 1's existing suite without editing its files.

**Interfaces:** Preserve existing Mutex API while deriving from `QRecursiveMutex`; callers use `QMutexLocker<Mutex>` or deduction. Preserve `gd::toQString(wstring const &) -> QString`, `gd::toWString(QString const &) -> wstring`, Utf8 signatures and ZIP on-disk uint32 fields.

- [ ] **Step 1: Write a failing temporary closure probe outside tracked source.** Compile a Qt Test probe with `Mutex mutex; { QMutexLocker<Mutex> first(&mutex); QMutexLocker<Mutex> second(&mutex); }` and supplementary `gd::toQString(Utf8::decode("\xf0\x9f\x98\x80")) == QString::fromUtf8("😀")`. Add ZIP timestamp boundary probes for 0, UINT32_MAX and an out-of-range qint64; assert checked rejection rather than truncation. Build the probe against production files, never copied implementations.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tests.pro GD_COMPILE_CLOSURE=1 'GD_COMPILE_SOURCES=mutex.cc zipfile.cc wstring.cc wstring_qt.cc utf8.cc' 'GD_COMPILE_HEADERS=mutex.hh zipfile.hh' -o /tmp/gd-qt6-utility.Makefile && make -f /tmp/gd-qt6-utility.Makefile -j$(nproc)`; record the removed recursive-mutex/time/string API diagnostic. Run the temporary probe under `xvfb-run -a timeout 60` after its qmake6 build.
- [ ] **Step 3: Apply direct Qt6 mechanics.** Preserve recursion/thread behavior, use full-scalar conversion for >FFFF, avoid QStringView into temporaries, replace seconds APIs with checked narrowing. Remove owned version wrappers/includes, retaining platform guards. The essential lock form is:

```cpp
class Mutex : public QRecursiveMutex {
public:
  class Lock;
};
// Retain the existing Lock's public methods and store a locker for Mutex,
// rather than passing a QRecursiveMutex-derived object as QMutex *.
```

- [ ] **Step 4: Confirm green.** Repeat the archive/probe commands and `qmake6 tests/tst_utf8_wstring/tst_utf8_wstring.pro -o tests/tst_utf8_wstring/Makefile && make -C tests/tst_utf8_wstring -j$(nproc) && xvfb-run -a timeout 60 tests/tst_utf8_wstring/tst_utf8_wstring`; run accumulated tests. Archive outputs live in /tmp and are not committed.
- [ ] **Step 5: Commit.** `git add mutex.hh mutex.cc zipfile.hh zipfile.cc wstring.hh wstring.cc wstring_qt.hh wstring_qt.cc utf8.hh utf8.cc categorized_logging.hh && git commit -m "refactor(qt6): adapt locking scalar and time utilities"`.

## Task 6: Adapt widget utilities and the compiled single-instance vendor closure

**Implementer tier:** Advanced
**Lane:** mechanize
**Depends on:** Task 5
**Ownership:**
- `about.cc`
- `delegate.cc`
- `dictionarybar.cc`
- `extlineedit.cc`
- `favoritespanewidget.cc`
- `gdappstyle.hh`
- `gdappstyle.cc`
- `gestures.hh`
- `gestures.cc`
- `groups_widgets.cc`
- `helpwindow.cc`
- `initializing.hh`
- `initializing.cc`
- `mainstatusbar.cc`
- `maintabwidget.cc`
- `treeview.cc`
- `qtsingleapplication/src/qtsingleapplication.pri`
- `qtsingleapplication/src/qtsingleapplication.h`
- `qtsingleapplication/src/qtsingleapplication.cpp`
- `qtsingleapplication/src/qtlocalpeer.cpp`
- `qtsingleapplication/src/qtlockedfile_win.cpp`

**Files:** Modify the owned compatibility anchors in spec §7.3; forms remain unchanged and are generated into the temporary archive build.

**Interfaces:** Preserve public widget/application signatures, single-instance peer socket identity and platform-only branches. Consume direct Qt6 APIs `Qt::MiddleButton`, `QString::toHtmlEscaped`, `QProcess::splitCommand`, `QProcess::startDetached(program,args)`, `qChecksum(QByteArrayView,Qt::ChecksumType)`; no external Qt5 QtSingleApplication binary.

- [ ] **Step 1: Write temporary production-closure assertions.** Assert empty detached command is rejected, quoted arguments retain boundaries, existing four-side margins remain equal and single-instance socket checksum uses Iso3309. Pin the checksum to a literal independently obtained for the fixture app ID, and verify a second peer connects to the first. Use QTest pointer events for tab middle-click behavior. No tracked feature test file changes in this task.
- [ ] **Step 2: Confirm red.** Run `qmake6 tests/tests.pro GD_COMPILE_CLOSURE=1 'GD_COMPILE_SOURCES=about.cc delegate.cc dictionarybar.cc extlineedit.cc favoritespanewidget.cc gdappstyle.cc gestures.cc groups_widgets.cc helpwindow.cc initializing.cc mainstatusbar.cc maintabwidget.cc treeview.cc qtsingleapplication/src/qtsingleapplication.cpp qtsingleapplication/src/qtlocalpeer.cpp' 'GD_COMPILE_FORMS=about.ui initializing.ui' -o /tmp/gd-qt6-widgets.Makefile && make -f /tmp/gd-qt6-widgets.Makefile -j$(nproc)`; record actual legacy API failure. Build/run the temporary test with qmake6, `make -j$(nproc)` and `xvfb-run -a timeout 60`.
- [ ] **Step 3: Convert only owned APIs.** Select Qt6 branches and remove Qt-version comments/conditions, keep Q_OS conditions. Replace Qt4x5 wrappers directly; use QObject-derived findChild pointer types. In qtlocalpeer use:

```cpp
const QRegularExpression expression(QStringLiteral("[^a-zA-Z]"));
Q_ASSERT(expression.isValid());
prefix.remove(expression);
// Keep the existing peer ID construction; change only the checksum API:
const auto checksum = qChecksum(QByteArrayView(idc), Qt::ChecksumIso3309);
```

Copy the exact pattern already at qtlocalpeer.cpp:81 into the constructor; preserve the resulting socket name. Remove qtsingleapplication.pri's version guard and choose widgets includes in the two named vendor files. Do not edit vendor examples/data. In uncompiled qtlockedfile_win.cpp, convert its remaining version-guarded Qt branch mechanically, retaining Windows-only guards; this satisfies source coverage without claiming a Windows build.
- [ ] **Step 4: Confirm green.** Repeat archive and temporary behavioral tests; run accumulated `make -C tests check` under `xvfb-run -a timeout 900`. Audit these four vendor inputs explicitly for removed regex/checksum/version APIs; later broad audit exclusions must not hide them.
- [ ] **Step 5: Commit.** `git add about.cc delegate.cc dictionarybar.cc extlineedit.cc favoritespanewidget.cc gdappstyle.hh gdappstyle.cc gestures.hh gestures.cc groups_widgets.cc helpwindow.cc initializing.hh initializing.cc mainstatusbar.cc maintabwidget.cc treeview.cc qtsingleapplication/src/qtsingleapplication.pri qtsingleapplication/src/qtsingleapplication.h qtsingleapplication/src/qtsingleapplication.cpp qtsingleapplication/src/qtlocalpeer.cpp qtsingleapplication/src/qtlockedfile_win.cpp && git commit -m "refactor(qt6): modernize widget and single-instance APIs"`.

## Task 7: Adapt remaining non-parser engines and playback utilities

**Implementer tier:** Standard
**Lane:** mechanize
**Depends on:** Task 6, Task 4
**Ownership:**
- `forvo.cc`
- `lsa.cc`
- `programs.cc`
- `sounddir.cc`
- `voiceengines.cc`
- `zipsounds.cc`
- `ffmpegaudio.cc`
- `ffmpegaudio.hh`
- `ffmpegaudioplayer.hh`

**Files:** Modify owned §7.3 anchors and all owned compatibility wrappers; do not create ffmpegaudioplayer.cc, which does not exist.

**Interfaces:** Preserve engine methods, player factory/engine ownership and worker cancellation. Consume `ArticleUrls::resourcePath(QUrl const &) -> QString` where full resource semantics are required, QUrlQuery decoded setters, `QAtomicInt::loadAcquire()` and concrete DOM count types.

- [ ] **Step 1: Add temporary fixture assertions against the real migrated helpers.** Assert encoded resource names preserve Unicode, literal plus and query suffix; checked DOM loops visit all nodes, cancellation exits ffmpeg work, and detached quoted program commands retain arguments. Test known-BMP QChar values and supplementary paths separately. Use existing utility tests for unchanged shared conversion behavior. Include:

```cpp
QCOMPARE(ArticleUrls::resourcePath(QUrl("bres://d/%F0%9F%98%80+a.wav?q=x+y")),
         QString::fromUtf8("😀+a.wav?q=x+y"));
QCOMPARE(QProcess::splitCommand(QStringLiteral("player \"two words.wav\"")),
         QStringList({QStringLiteral("player"), QStringLiteral("two words.wav")}));
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tests.pro GD_COMPILE_CLOSURE=1 'GD_COMPILE_SOURCES=forvo.cc lsa.cc programs.cc sounddir.cc voiceengines.cc zipsounds.cc ffmpegaudio.cc' -o /tmp/gd-qt6-engines.Makefile && make -f /tmp/gd-qt6-engines.Makefile -j$(nproc)`; record a removed wrapper/API diagnostic. Run the temporary fixture probe via `xvfb-run -a timeout 60` after `qmake6` and `make -j$(nproc)`.
- [ ] **Step 3: Make direct API substitutions.** Construct QUrlQuery values explicitly, preserve one leading slash, replace atomic wrappers on the original atomic, keep ffmpeg feature guards. Use `QStringView(source).mid(...)` only within source lifetime and checked scalar conversion; preserve index formats and libeb/ffmpeg behavior. Remove owned legacy include/branch/comment spellings. `epwing.cc` includes ftshelpers.hh and is owned/compiled by codec Task 9 after its codec and FTS declarations exist, so this task has no early QRegExp-header dependency.
- [ ] **Step 4: Confirm green.** Repeat the archive/probe and accumulated suite commands. Re-run archive with `CONFIG+=no_ffmpeg_player` to prove feature-guarded code remains compilable; this compile check supplements the real application variant in Task 35.
- [ ] **Step 5: Commit.** `git add forvo.cc lsa.cc programs.cc sounddir.cc voiceengines.cc zipsounds.cc ffmpegaudio.cc ffmpegaudio.hh ffmpegaudioplayer.hh && git commit -m "refactor(qt6): adapt engine and ffmpeg utility APIs"`.

## Task 8: Migrate all legacy encoding callers and the inline website resolver

**Implementer tier:** Capable
**Lane:** codec
**Depends on:** Task 3, Task 4, Task 5, Task 6
**Ownership:**
- `epwing_book.hh`
- `epwing_book.cc`
- `indexedzip.cc`
- `ftshelpers.hh`
- `fulltextsearch.hh`
- `loaddictionaries.cc`
- `slob.cc`
- `website.cc`
- `website.hh`
- `encoding.hh`
- `encoding.cc`
- `tests/tst_encoding/tst_encoding.pro`
- `tests/tst_encoding/tst_encoding.cc`
- `tests/tst_linkrewrite/tst_linkrewrite.pro`
- `tests/tst_linkrewrite/tst_linkrewrite.cc`
- `tests/fixtures/encoding/vectors.json`
- `tests/fixtures/linkrewrite/cases.json`

**Files:** Modify the eight-file codec inventory in §7.2, website.cc:113–300/458–515 and SLOB's conversion member/accessor. Extract existing caller conversion policy to private helpers in owned production modules when needed for pure-test linkage; declare their concrete helper signatures in those modules before using them in tests.

**Interfaces:** Consume the complete Encoding contract from Task 2 and LinkRewrite signatures from Task 3. Produce inline `WebSite::resolveInlineUrls(QString const & html,QUrl const & documentUrl) -> QString` in website.hh as the extracted production resolver; website.cc's HTTP completion path and tst_linkrewrite call that identical helper. Keep the helper's dependencies within Core/Gui/Network and forward declarations; the pure resolver test includes the production header without listing website.hh for MOC or linking website.cc/its complete engine. The default archive separately compiles the whole production engine, so both helper behavior and caller compilation are required. EPWING's three codec accessors become `Encoding const &` under their existing names; SLOB owns an Encoding value. Logging's entry signatures and Qt6 implementation are already established in Task 2; finish remaining gdCodecForLocale call sites here. Website output supplies both `id` and `name` equal to `gdexpandframe-DICTID`; frames retain their actual remote src.

- [ ] **Step 1: Add failing production caller tests.** Exercise URL placeholder failure leaving that placeholder intact while another succeeds; EUC-JP→GB2312 EPWING retry/no-match without UTF-8 fallback; SLOB legacy decode; raw ZIP filename bytes preserved separately from display; logging emits UTF-8 without locale mutation. Add tokenizer cases for comments/script text, mixed case, quotes/unquoted attributes, first valid base, src/href/action/poster/srcset, CSS url/import and data/javascript/mailto/tel/fragment exclusions. For base `http://127.0.0.1:43127/dir/page?q=1`, assert `../img.png` resolves to `/img.png` on port 43127, `?q=2` preserves `/dir/page`, base `/other/` changes relative root, empty form action resolves to document URL and stylesheet resource mapping preserves the complete absolute URL. The extracted production resolver has this exact first oracle:

```cpp
const QUrl base("http://127.0.0.1:43127/dir/page?q=1");
QCOMPARE(WebSite::resolveInlineUrls("<img src=\"../img.png\"><form action=\"\">", base),
         QString("<img src=\"http://127.0.0.1:43127/img.png\"><form action=\"http://127.0.0.1:43127/dir/page?q=1\">"));
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_encoding/tst_encoding.pro -o tests/tst_encoding/Makefile && make -C tests/tst_encoding -j$(nproc) && xvfb-run -a timeout 60 tests/tst_encoding/tst_encoding`; repeat for `tests/tst_linkrewrite/tst_linkrewrite.pro`. The failure must identify old caller policy or resolver behavior, not a test-only policy copy.
- [ ] **Step 3: Convert callers and tokenize URL spans.** Replace owned pointer codecs with immutable values and Encoding::forLocale; use setEncoding(QStringConverter::Utf8) without locale writes. Consume spec §2.5 `FtsHighlightSpec` from articleurls.hh; make ftshelpers.hh/fulltextsearch.hh QRegExp-free with QRegularExpression declarations and no MainWindow/ArticleView include. Task 10 supplies their algorithms. Preserve the website substitution loop's ISO11/12 skip. Resolver edits only URL-valued spans and resolves against final response/base/stylesheet URLs with QUrl::resolved; iframe HTML bypasses it. Keep website name/id emission for Task 20. Pattern validation follows §4.3 for any remaining owned parser regex. Convert any remaining owned Qt6 scalar/time/view APIs now. Task 8's own source archive must pass before commit; Task 9 later recompiles its consumers, rather than deferring this task's red result.
- [ ] **Step 4: Confirm green.** Repeat both suites, compile `epwing_book.cc indexedzip.cc loaddictionaries.cc slob.cc website.cc` through `qmake6 tests/tests.pro GD_COMPILE_CLOSURE=1 'GD_COMPILE_SOURCES=epwing_book.cc indexedzip.cc loaddictionaries.cc slob.cc website.cc' 'GD_COMPILE_FORMS=fulltextsearch.ui' -o /tmp/gd-qt6-codec-callers.Makefile && make -f /tmp/gd-qt6-codec-callers.Makefile -j$(nproc)`, then accumulated tests. Verify ftshelpers.hh/fulltextsearch.hh are independently Qt6-syntax-safe. No changed module can remain uncompiled pending a later task.
- [ ] **Step 5: Commit.** `git add epwing_book.hh epwing_book.cc indexedzip.cc ftshelpers.hh fulltextsearch.hh loaddictionaries.cc slob.cc website.cc website.hh encoding.hh encoding.cc tests/tst_encoding tests/tst_linkrewrite tests/fixtures/encoding tests/fixtures/linkrewrite && git commit -m "refactor(encoding): migrate callers and preserve website URL bases"`.

## Task 9: Convert dictionary parser regex families without index changes

**Implementer tier:** Capable
**Lane:** codec
**Depends on:** Task 8, Task 7
**Ownership:**
- `aard.cc`
- `bgl.cc`
- `btreeidx.cc`
- `dictheadwords.cc`
- `dictionary.hh`
- `dictionary.cc`
- `dictdfiles.cc`
- `dictserver.cc`
- `dsl.cc`
- `dsl_details.cc`
- `epwing.cc`
- `folding.cc`
- `gls.cc`
- `htmlescape.cc`
- `hunspell.cc`
- `langcoder.cc`
- `mdictparser.cc`
- `mdx.cc`
- `mediawiki.cc`
- `sdict.cc`
- `stardict.cc`
- `xdxf2html.cc`
- `xdxf.cc`
- `zim.cc`
- `nsis/gen_uninstall/gen_uninstall.cc`
- `wildcard.hh`
- `wildcard.cc`
- `tests/tst_regex/tst_regex.pro`
- `tests/tst_regex/tst_regex.cc`
- `tests/tst_linkrewrite/tst_linkrewrite.pro`
- `tests/tst_linkrewrite/tst_linkrewrite.cc`
- `tests/fixtures/regex/cases.json`
- `tests/fixtures/linkrewrite/cases.json`
- `tests/tst_htmlescape/tst_htmlescape.pro`
- `tests/tst_htmlescape/tst_htmlescape.cc`
- `tests/tst_langcoder/tst_langcoder.pro`
- `tests/tst_langcoder/tst_langcoder.cc`

**Files:** Modify every owned regex/compatibility anchor from §§7.1/7.3. Add both pure utility targets. Do not change indexing layouts/signatures or version constants.

**Interfaces:** Consume all four exact LinkRewrite functions and ArticleUrls resource-path semantics. Preserve all engine public signatures; extract an existing format normalizer into its owning production module when a test cannot link the full engine, and link that same normalizer in caller/test. Preserve Dictionary::DataRequest's nonvirtual getters and `CurrentFtsFormatVersion`.

- [ ] **Step 1: Add source-tagged expected rows before each family changes.** Pin StarDict bword cleanup, `cat#part`→lookup path plus gdanchor, DICT braces/phonetic span, Zim title/underscore/.html/SLASH decisions, multiline audio/resource matches, reverse/negative-offset search, absent captures, Unicode digit/word classes and wildcard grammar. Add `Html::escape(std::string const &)`, `Html::preformat(std::string const &,bool)` and LangCoder's fixed known/unknown/case fixtures; expected strings/codes are literals. For the production escape entry point:

```cpp
QCOMPARE(Html::escape(std::string("<&>")), std::string("&lt;&amp;&gt;"));
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_regex/tst_regex.pro -o tests/tst_regex/Makefile && make -C tests/tst_regex -j$(nproc) && xvfb-run -a timeout 60 tests/tst_regex/tst_regex`, then run the newly generated `tst_htmlescape` and `tst_langcoder` projects with `qmake6`, `make -j$(nproc)` and `xvfb-run -a timeout 60`; record legacy compile failures or exact expectation mismatches.
- [ ] **Step 3: Convert complete families and callers.** Use validated QRegularExpression, match objects/globalMatch, source-safe captures, DotMatchesEverythingOption for multiline HTML and UseUnicodePropertiesOption where required. Preserve full wildcard behavior with NonPathWildcardConversion; convert wildcard.hh/.cc here, retaining its escape grammar, because gls.cc/mdx.cc include that header. Compile epwing.cc here after Task 8's Encoding/FTS declarations are available. Apply format normalization before common URI rewrite; structural query/fragment data cannot become headword text. Convert wrapper/QChar/view/time/container APIs in these same owned files. Reject invalid built-ins visibly and user regex before launching work. Convert the NSIS patterns mechanically without claiming a Windows build.
- [ ] **Step 4: Confirm green.** Re-run all named suites plus `tst_linkrewrite` and accumulated tests. Compile every owned engine source using the test archive mode, including completed codec callers from Task 8, default libeb and a `CONFIG+=zim_support` closure; check optional hunspell/OpenCC/TIFF guards. Compare index signature/version declarations to the baseline and assert no layout/version diff. No caller test may use a duplicate test-only normalization implementation.
- [ ] **Step 5: Commit.** Stage exactly the ownership paths with `git add` and run `git commit -m "refactor(regex): port dictionary parser families to Qt6"`.

## Task 10: Carry FTS source syntax and compiled expressions through typed APIs

**Implementer tier:** Advanced
**Lane:** codec
**Depends on:** Task 9, Task 4
**Ownership:**
- `ftshelpers.hh`
- `ftshelpers.cc`
- `fulltextsearch.hh`
- `fulltextsearch.cc`
- `tests/tst_regex/tst_regex.pro`
- `tests/tst_regex/tst_regex.cc`
- `tests/fixtures/regex/cases.json`

**Files:** Modify ftshelpers.hh:5/49–126, fulltextsearch.hh:187/233, corresponding matching/signal emitters and their compatibility branches.

**Interfaces:** Consume `FtsHighlightSpec` from articleurls.hh and complete Task 8's typed FTS declarations. Matching parameters become `QRegularExpression &`/`QRegularExpression const &` at the existing parseSearchString/checkArticles/combinedIndexSearch/fullIndexSearch/fullSearch positions. The FTS result signal's original searchRegExp parameter becomes `FtsHighlightSpec const &`; keep every other signal parameter/order. Downstream exact consumers are `ArticleView::showDefinition(QString const &,QStringList const &,FtsHighlightSpec const &,unsigned,bool)` and MainWindow::showTranslationFor's corresponding FtsHighlightSpec parameter; those consumers switch in Tasks 22/33 before entering the Qt6 closure. Give the FTS dialog a trailing optional QString initial-translate-text constructor argument and `helpRequested(QString const &)` signal, preserving old calls through an empty default. Task 33 supplies the actual initial text and connects help. Save-collector Core coverage is prepared here against the production inline `SaveArticleDetail::ResourceSpan {qsizetype offset,length; QUrl url;}` and `collectResourceSpans(QString const &,QUrl const &) -> QVector<ResourceSpan>` in workflow-owned savearticle.hh, delivered by Task 27. Add a qmake `exists($$PWD/../../savearticle.hh)` feature define `GD_SAVE_COLLECTOR_AVAILABLE` and guard only these new helper calls with that define; early runs make no save-coverage claim. At Task 27 reconfigure this project and require the named saveCollector test to run against the real helper; Task 35 rejects an absent define/test. Existing regex/FTS cases always run. Consume wildcard.hh/.cc read-only from Task 9.

- [ ] **Step 1: Add failing production FTS fixtures.** Test source regex vs wildcard mode separately, match-case option recompilation, invalid/empty input, slash-containing headwords, escaped star/backslash/brackets, legacy hex/octal captures and PCRE UTF-16 offsets containing surrogate pairs. Assert source text and wildcards survive URL round-trip through `ArticleUrls::lookupDocument`/`parse`. This pins the shared value and scalar-safe matching; run it through the production FTS path as well as the URL round-trip:

```cpp
FtsHighlightSpec highlight;
highlight.sourcePattern = QStringLiteral("a*/b");
highlight.wildcards = true;
highlight.expression = QRegularExpression(QRegularExpression::wildcardToRegularExpression(
    highlight.sourcePattern, QRegularExpression::NonPathWildcardConversion));
QVERIFY(highlight.expression.isValid());
QVERIFY(highlight.expression.match(QStringLiteral("alpha/b")).hasMatch());
const QString text = QString::fromUtf8("😀中");
const auto match = QRegularExpression(QString::fromUtf8("中")).match(text);
QCOMPARE(match.capturedStart(), 2);
QCOMPARE(match.capturedLength(), 1);
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_regex/tst_regex.pro -o tests/tst_regex/Makefile && make -C tests/tst_regex -j$(nproc) && xvfb-run -a timeout 60 tests/tst_regex/tst_regex`; identify the old patternSyntax/type mismatch.
- [ ] **Step 3: Convert workers/signals as one closure.** Retain existing normalization/case folding and index algorithm. Register FtsHighlightSpec if queued delivery needs a metatype. Store sourcePattern/wildcards/matchCase explicitly, compile QRegularExpression with the exact options and validated hex syntax. Keep all fulltextsearch→worker emissions/receivers typed; no QRegularExpression wildcard-property guess. Do not add an old-regex adapter to the new API. Remove fulltextsearch.cc's mainwindow.hh include, constructor static_cast<MainWindow *>(parent)->getTranslateLineText() and help qobject_cast<MainWindow *>(parentWidget()) calls: use the supplied initial text and emit the existing help ID. Test the production dialog with a plain QWidget parent. No unconverted ArticleView/MainWindow header may enter this closure.
- [ ] **Step 4: Confirm green.** Repeat Step 2, archive-build `ftshelpers.cc fulltextsearch.cc wildcard.cc` with fulltextsearch.ui and the converted engine closure via `qmake6 tests/tests.pro GD_COMPILE_CLOSURE=1 'GD_COMPILE_SOURCES=ftshelpers.cc fulltextsearch.cc wildcard.cc' 'GD_COMPILE_FORMS=fulltextsearch.ui' -o /tmp/gd-qt6-fts.Makefile && make -f /tmp/gd-qt6-fts.Makefile -j$(nproc)`, then accumulated suites. Verify CurrentFtsFormatVersion is unchanged.
- [ ] **Step 5: Commit.** `git add ftshelpers.hh ftshelpers.cc fulltextsearch.hh fulltextsearch.cc tests/tst_regex tests/fixtures/regex && git commit -m "refactor(fts): carry typed Qt6 highlight specifications"`.

## Task 11: Add hotkey syntax, capability contracts and portable audio config migration

**Implementer tier:** Advanced
**Lane:** desktop
**Depends on:** Task 1, Task 4, Task 6, Task 7, Task 8
**Ownership:**
- `hotkeybackend.hh`
- `hotkeysyntax.hh`
- `hotkeysyntax.cc`
- `config.hh`
- `config.cc`
- `audioplayerfactory.cc`
- `multimediaaudioplayer.hh`
- `multimediaaudioplayer.cc`
- `tests/tst_hotkeys/tst_hotkeys.pro`
- `tests/tst_hotkeys/tst_hotkeys.cc`
- `tests/fixtures/config/legacy.xml`
- `tests/fixtures/config/additive.xml`

**Files:** Create syntax/backend declarations and tests/config fixtures; modify config.cc:109–120/887–925/1764–1773, config.hh:192–241 and audio factory. Delete multimedia player files; root source entries are removed at the Task 34 cutover, not used by interim Qt6 closures.

**Interfaces:** Produce exact HotkeyBinding, PortalBindingDisplay and HotkeyBackend state/signals from §2.10, including `bool isSupported() const noexcept`, `void bind(QVector<HotkeyBinding> const &)`, `void unbind()`, `State state() const noexcept`, `activated(QString const &,QString const &)`, `bindingsChanged(QVector<PortalBindingDisplay> const &)`, `stateChanged(State,QString const &)`. Produce `HotkeySyntax::parse(QString const &,Config::HotKey *,QString *) -> bool`, `preferredTrigger(Config::HotKey const &,QString *,QString *) -> bool`, `serialize(Config::HotKey const &) -> QString`. Put PortalBindingDisplay where both config/backend can include it without a cyclic definition; retain its exact global type/name.

- [ ] **Step 1: Add failing parse/config/audio tests.** Pin one/two combinations, Ctrl++/comma, modifier-only/unsupported keys, every named/punctuation/F1–F35 mapping in §4.11, Ctrl+Alt+G→CTRL+ALT+g and Meta+Shift+F5→SHIFT+LOGO+F5. Test legacy and unknown audio backend migration via public defaultBackend only, additive XML missing/unknown/duplicate/escaped display records, untouched unsupported shortcut text and existing save timing. Begin with these real syntax calls:

```cpp
Config::HotKey key;
QString trigger, error;
QVERIFY(HotkeySyntax::parse(QStringLiteral("Ctrl+Alt+G"), &key, &error));
QVERIFY(HotkeySyntax::preferredTrigger(key, &trigger, &error));
QCOMPARE(trigger, QStringLiteral("CTRL+ALT+g"));
QVERIFY(HotkeySyntax::parse(QStringLiteral("Meta+Shift+F5"), &key, &error));
QVERIFY(HotkeySyntax::preferredTrigger(key, &trigger, &error));
QCOMPARE(trigger, QStringLiteral("SHIFT+LOGO+F5"));
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_hotkeys/tst_hotkeys.pro -o tests/tst_hotkeys/Makefile && make -C tests/tst_hotkeys -j$(nproc) && xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys`; require missing syntax/migration behavior.
- [ ] **Step 3: Implement §4.11/4.13.** Use QKeySequence count/indexed QKeyCombination and PortableText, never comma splitting or old bitmasks. Return unsupported reasons without modifying stored text. Add Config::Class::hotkeys.portalBindings, last-wins known display IDs and raw-text markers cleared only on recorder edits. Remove qtmultimedia/isQtmultimedia plumbing/factory selection. With MAKE_FFMPEG_PLAYER migrate in memory to defaultBackend; without it retain backend text exactly and set runtime internal use unavailable. Do not save on load or change proxy schema.
- [ ] **Step 4: Confirm green in both variants.** Repeat Step 2, then `qmake6 tests/tst_hotkeys/tst_hotkeys.pro CONFIG+=no_ffmpeg_player -o /tmp/gd-hotkeys-noffmpeg.Makefile && make -f /tmp/gd-hotkeys-noffmpeg.Makefile -j$(nproc)` and run that output under `xvfb-run -a timeout 60`. Assert byte-identical stored backend text, external path available and no internal player in that variant; run accumulated tests.
- [ ] **Step 5: Commit.** `git add hotkeybackend.hh hotkeysyntax.hh hotkeysyntax.cc config.hh config.cc audioplayerfactory.cc multimediaaudioplayer.hh multimediaaudioplayer.cc tests/tst_hotkeys tests/fixtures/config && git commit -m "feat(desktop): add hotkey syntax and portable config migration"`.

## Task 12: Implement platform-safe geometry, flags and activation helpers

**Implementer tier:** Standard
**Lane:** desktop
**Depends on:** Task 11
**Ownership:**
- `platformwindow.hh`
- `platformwindow.cc`
- `tests/tst_platformwindow/tst_platformwindow.pro`
- `tests/tst_platformwindow/tst_platformwindow.cc`

**Files:** Create owned module/test pair; preserve helpers' public signatures and no owned native resources.

**Interfaces:** `PlatformWindow::screenForPoint(QPoint const &) -> QScreen *`, `availableGeometryForPoint(QPoint const &) -> QRect`, `popupFlags(QString const &,Config::ScanPopupWindowFlags,bool) -> Qt::WindowFlags`, `applyActivation(QWidget *,QString const &) -> void`, and under HAVE_X11 `x11Display() -> Display *`. Consume Config flags and optional HAVE_KWINDOWSYSTEM.

- [ ] **Step 1: Write failing table tests.** Assert Wayland always Tool/Frameless/StaysOnTop and never X11Bypass, xcb retains stored Popup/Tool/bypass combinations, screenAt fallback/available-vs-full geometry, an internal pure empty-screen branch, null activation safety, and null x11Display for actual non-xcb QPA. No boolean focus-success assertion. Use the production flag function directly:

```cpp
const auto flags = PlatformWindow::popupFlags(
    QStringLiteral("wayland"), static_cast<Config::ScanPopupWindowFlags>(0), true);
QVERIFY(flags.testFlag(Qt::Tool));
QVERIFY(flags.testFlag(Qt::FramelessWindowHint));
QVERIFY(flags.testFlag(Qt::WindowStaysOnTopHint));
QVERIFY(!flags.testFlag(Qt::X11BypassWindowManagerHint));
PlatformWindow::applyActivation(nullptr, QString());
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_platformwindow/tst_platformwindow.pro -o tests/tst_platformwindow/Makefile && make -C tests/tst_platformwindow -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_platformwindow/tst_platformwindow`.
- [ ] **Step 3: Implement direct Qt6 platform calls.** Use QGuiApplication::screenAt then primaryScreen; get native Xlib display only after active platform starts with xcb and the native interface exists. Set a nonempty activation token with KWindowSystem before windowHandle()->requestActivate, retain void best-effort behavior and never log token. Without KDE helper call requestActivate. Avoid any X11 method on native Wayland.
- [ ] **Step 4: Confirm green.** Repeat Step 2, build without HAVE_X11 and without HAVE_KWINDOWSYSTEM, and run `QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_platformwindow/tst_platformwindow` for the actual non-xcb display guard while keeping required Xvfb assertions in the xcb run. Run accumulated tests.
- [ ] **Step 5: Commit.** `git add platformwindow.hh platformwindow.cc tests/tst_platformwindow && git commit -m "feat(platform): add Wayland-safe window helpers"`.

## Task 13: Retain X11 RECORD behind runtime platform checks

**Implementer tier:** Advanced
**Lane:** desktop
**Depends on:** Task 12, Task 7
**Ownership:**
- `hotkeywrapper.hh`
- `hotkeywrapper.cc`
- `keyboardstate.cc`
- `broken_xrecord.cc`
- `scanflag.cc`
- `translatebox.cc`
- `hotkeyedit.cc`
- `hotkeybackend.hh`
- `tests/tst_hotkeys/tst_hotkeys.pro`
- `tests/tst_hotkeys/tst_hotkeys.cc`
- `tests/tst_platformwindow/tst_platformwindow.pro`
- `tests/tst_platformwindow/tst_platformwindow.cc`

**Files:** Modify wrapper.hh:63–80/218–255, runtime X11 consumers, scanflag.cc:65 and translatebox.cc:293. MainWindow/popup changes remain exclusively Task 33.

**Interfaces:** Produce `X11RecordBackend : HotkeyBackend`, constructor `(QObject * parent=nullptr)`, required noexcept state/support accessors, `bind(QVector<HotkeyBinding> const &)`, `unbind()`. Preserve `HotkeyWrapper::setGlobalKey(int,int,Qt::KeyboardModifiers,int) -> bool`, `unregister()`, `QHotkeyApplication` and non-Linux session hooks. Consume all PlatformWindow signatures; stable IDs map to recorder integer handles.

- [ ] **Step 1: Add failing matcher and capability tests.** Pin one/two-key timing/modifiers/double-Ctrl+C, stable ID/handle mapping, empty activation token on X11, no false clipboard fallback, null display disabling with explanation, actual non-xcb display path performing zero Xlib calls. Matcher tests link extracted real matcher routines without requiring a RECORD server; the live RECORD proof is Task 37. Add a named `nonXcbCapability` case and run it under actual offscreen QPA:

```cpp
QVERIFY(!QGuiApplication::platformName().startsWith(QStringLiteral("xcb")));
X11RecordBackend backend;
QSignalSpy activations(&backend, &HotkeyBackend::activated);
QVERIFY(!backend.isSupported());
backend.bind({{QStringLiteral("main-window"), QStringLiteral("Main window"), QStringLiteral("Ctrl+Alt+G")}});
backend.unbind();
QCOMPARE(activations.count(), 0);
```

Require `QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_hotkeys/tst_hotkeys nonXcbCapability` in Step 4 in addition to the xcb matcher run.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_hotkeys/tst_hotkeys.pro -o tests/tst_hotkeys/Makefile && make -C tests/tst_hotkeys -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys`; re-run platformwindow suite and require obsolete indexed-key/native-display behavior to fail.
- [ ] **Step 3: Adapt recorder and consumers.** Keep blocking XRecordEnableContext on its worker's separately owned display, not the Qt GUI display. Native root is DefaultRootWindow(display), never cast an xcb_connection_t. Guard both active QPA and non-null display for X11 paths. Route screen/flags through PlatformWindow and parse recorder combinations through Qt6 APIs. Remove owned Qt-version helpers while keeping Q_OS/HAVE_X11 guards.
- [ ] **Step 4: Confirm green.** Repeat both suites and archive-build `hotkeywrapper.cc keyboardstate.cc broken_xrecord.cc scanflag.cc translatebox.cc hotkeyedit.cc` with scanflag.ui. Run accumulated suites; do not infer live two-key RECORD success from a headless matcher pass.
- [ ] **Step 5: Commit.** `git add hotkeywrapper.hh hotkeywrapper.cc keyboardstate.cc broken_xrecord.cc scanflag.cc translatebox.cc hotkeyedit.cc hotkeybackend.hh tests/tst_hotkeys tests/tst_platformwindow && git commit -m "refactor(x11): guard RECORD and native desktop calls by QPA"`.

## Task 14: Register schemes and decouple ArticleNetworkAccessManager from WebKit

**Implementer tier:** Capable
**Lane:** webcore
**Depends on:** Task 4, Task 9, Task 10
**Ownership:**
- `articleschemes.hh`
- `articleschemes.cc`
- `article_netmgr.hh`
- `article_netmgr.cc`
- `article_maker.hh`
- `article_maker.cc`
- `tests/tst_article_schemes/tst_article_schemes.pro`
- `tests/tst_article_schemes/tst_article_schemes.cc`

**Files:** Create articleschemes.cc and complete the handler declaration with its implementation. Convert ArticleMaker's Qt4x5 include/URL/scalar/time anchors; preserve generated dictionary HTML and existing one-argument `window.articleview.onJsActiveArticleChanged` calls. No shared fixture or unconverted ArticleView is required: registration/completed-resource tests use the real empty ArticleMaker and DataRequestInstant.

**Interfaces:** Exactly `registerArticleSchemes() -> void`, idempotent before application construction; `ArticleSchemeHandler(ArticleNetworkAccessManager &,QObject * =nullptr)` and `(ArticleNetworkAccessManager &,DataRequestReaderFactory,QObject * =nullptr)`, destructor, `requestStarted(QWebEngineUrlRequestJob *) override`, signal `producerFailed(QUrl const &,QString const &)`. Preserve resource manager constructor, `getResource(QUrl const &,QString &) -> sptr<Dictionary::DataRequest>` and maker access. Implement `makeDataRequestReader()` and its exact Task 4 virtuals. Remove SecurityWhiteList, Origins, AllowFrameReply, WebSecurityOrigin and renderer Referer policy; retain QNAM only for explicit resources/downloads. Keep ArticleStreamDevice forward-declared until Task 16 supplies all of its methods.

- [ ] **Step 1: Add failing registration/completed-resource tests.** A custom main calls registerArticleSchemes before QApplication. Its order is:

```cpp
int main(int argc, char **argv) {
  registerArticleSchemes();
  registerArticleSchemes(); // Idempotence is part of the production contract.
  QApplication app(argc, argv);
  TstArticleSchemes test;
  return QTest::qExec(&test, argc, argv);
}
```

Test all seven names `gdlookup,bres,gdau,gdvideo,gdpicture,gdicon,gico` with HostAndPort/defaultPort=0 and exactly SecureScheme|LocalScheme|LocalAccessAllowed|CorsEnabled|FetchApiAllowed. Reject invalid credentials/ports/methods, strict group-less DocumentLoad and unknown icons; assert blank=1 HTML, qrc content, MIME distinctions, successful empty resource and real instant-finish bytes. Exercise the real forwarding reader; a destroyed job cancels once. Prove rendering and JS with a real QWebEnginePage without ArticleView.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_schemes/tst_article_schemes.pro -o tests/tst_article_schemes/Makefile && make -C tests/tst_article_schemes -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_article_schemes/tst_article_schemes`; expect missing registration/handler or retained WebKit dependency.
- [ ] **Step 3: Implement registration and buffered GUI coordinator.** Registration uses no application-dependent API. Every application URL passes ArticleUrls::parse(DocumentLoad); initialize each job's reader factory before state inspection, connect producer signals and immediately recheck terminal state. Use spec §3.1's full job record and one-reply/failure/cancel latches. A producer error wins over partial bytes; not-found requires terminal size -1 without error. Catch fullData at the real handler boundary, open/seek a complete job-parented QBuffer before reply, never delete it independently. Job destruction disconnects/removes/cancels GUI producer state after locks are released. Preserve QNAM HTTP consumers without renderer policy. Convert ArticleMaker wrappers directly to QUrlQuery/Qt6 operations and report known emitted file URLs through its existing generation/snapshot path for later GUI policy fix-ups. Streaming replaces buffered gdau/gdvideo in Task 16 before root integration.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated tests. Compile article_maker.cc/article_netmgr.cc/articleschemes.cc against the actual converted dictionary/FTS closure with no WebKit headers/libs. Browser main verifies pre-application registration and real document+JS handshake. The production reader delegate must be fully linkable before Task 15 starts.
- [ ] **Step 5: Commit.** `git add articleschemes.hh articleschemes.cc article_netmgr.hh article_netmgr.cc article_maker.hh article_maker.cc tests/tst_article_schemes && git commit -m "feat(webcore): register schemes and serve buffered article resources"`.

## Task 15: Supply deterministic dictionary readers, HTTP origins and private portal fixtures

**Implementer tier:** Advanced
**Lane:** tests
**Depends on:** Task 1, Task 4, Task 9, Task 11, Task 14
**Ownership:**
- `tests/testsupport.pri`
- `tests/article-test.pri`
- `tests/support/dictionary_stub.hh`
- `tests/support/dictionary_stub.cc`
- `tests/support/http_fixture.hh`
- `tests/support/http_fixture.cc`
- `tests/support/fake_portal.hh`
- `tests/support/fake_portal.cc`
- `tests/support/fixtures.hh`
- `tests/support/fixtures.cc`
- `tests/fixtures/article/*`
- `tests/fixtures/website/*`
- `tests/fixtures/print/two-pages.html`
- `tests/tst_utf8_wstring/tst_utf8_wstring.pro`
- `tests/tst_utf8_wstring/tst_utf8_wstring.cc`

**Files:** Create every §2.12 support pair and browser/print fixture path in §6.2. Extend support self-tests in the existing tst_utf8_wstring; no nineteenth suite. Link the real production reader from Task 14, so no unresolved factory/archive workaround is needed.

**Interfaces:** `FixtureDictionary : Dictionary::Class`; `ControlledDataRequest::{append(QByteArray const &),finishSuccess(),finishNotFound(),finishError(QString const &),setFullDataException(QString const &),setSliceException(QString const &),setStreamCopyException(QString const &),cancel()}`; `readProbe(TestSupport::ReadOperation) const -> ReadProbeSnapshot {entered,thrown,returned,callingThreads}`; `ReadOperation::{FullData,Slice,StreamCopy}`; `TestSupport::controlledReaderFactory() -> DataRequestReaderFactory`; `ThrowingDataRequestReader : DataRequestReader`. Exact §6.1 `HttpFixture::{start(QHostAddress const &,quint16=0),url(QString const &) const,requests() const,release(QString const &)}` and `FakePortal::{start(QDBusConnection const &,uint),setScenario(QString const &),calls() const,emitActivation(QString const &,QString const &,QString const &),loseService()}`.

- [ ] **Step 1: Write failing fixture self-tests.** Assert all three IP binds have nonzero ephemeral ports, redirects/Host/barriers work, PNG is valid 1×1, print content has the fixed two-page markers, and the portal is isolated on a private bus. Controlled requests obey dataMutex/hasAnyData and instant completion. Each setter arms one unique message/reset probe without changing producer bytes or invoking a getter. The factory captures only shared mutex-protected plain probe state plus the production delegate in its reader, with no retained producer. Test normal forwarding here; forced exceptions must later originate inside the real production boundaries in Task 16. The real HTTP fixture bind self-test is:

```cpp
for (const QString &host : {QStringLiteral("127.0.0.1"), QStringLiteral("127.0.0.2"), QStringLiteral("127.0.0.3")}) {
  HttpFixture server;
  QVERIFY(server.start(QHostAddress(host), 0));
  const QUrl address = server.url(QStringLiteral("/dir/first.html"));
  QCOMPARE(address.host(), host);
  QVERIFY(address.port() > 0);
}
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_utf8_wstring/tst_utf8_wstring.pro -o tests/tst_utf8_wstring/Makefile && make -C tests/tst_utf8_wstring -j$(nproc) && xvfb-run -a timeout 60 tests/tst_utf8_wstring/tst_utf8_wstring`; require missing support or a violated fixture assertion.
- [ ] **Step 3: Implement support and fixed content.** Reader operation probes use currentThreadId and one-shot exceptions with separately locked state; FullData/Slice receive a producer only for that call and StreamCopy uses only its supplied span. No nonvirtual getter hiding. Create article.html/first+second+nested website pages/dom.html/style.css/pixel.png/relative.js/data.json and two-pages.html exactly as §6.2, including fixture-headword, gdfrom-dict-a/b, early/late callbacks, multilingual/supplementary text and real-origin-fetch. Serve /redirect, /slow-resource, /deny-frame, /deny-csp, /auth, relative resources and real proxy forwarding/challenges. FakePortal exports exact typed wire interfaces and fast/late/alternate/list/bind/loss scenarios, asserting one Bind/session. Include the named `early-alternate-response` scenario used by Task 30: emit a valid success Response on the eventual alternate request path before returning that path from CreateSession. Supply every schema/subset/configure/error/edit/late-cleanup scenario enumerated in Tasks 30–32 now, so those consumers never need to edit shared support. Start `dbus-daemon --session --nofork --print-address=1` with QProcess and use its printed address; destroy requests/backend before daemon. `tests/article-test.pri` takes a per-suite production closure, supplies required feature modules/UI/MOC and never includes main.cc/MainWindow to reach a helper.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated tests. Verify fixture paths and each bind/bus/prerequisite failure is a required failure. Inspect the real linked makeDataRequestReader; force-failure route/thread proof is mandatory in Task 16. Pure-suite watchdog remains 60 s; browser self-probes added to browser targets use 120 s.
- [ ] **Step 5: Commit.** `git add tests/testsupport.pri tests/article-test.pri tests/support tests/fixtures/article tests/fixtures/website tests/fixtures/print tests/tst_utf8_wstring && git commit -m "test(fixtures): add controlled readers origins and private portal"`.

## Task 16: Implement the thread-safe streaming device and real reader-failure boundaries

**Implementer tier:** Capable
**Lane:** webcore
**Depends on:** Task 15, Task 14, Task 5
**Ownership:**
- `articleschemes.hh`
- `articleschemes.cc`
- `tests/tst_article_stream/tst_article_stream.pro`
- `tests/tst_article_stream/tst_article_stream.cc`
- `tests/tst_article_schemes/tst_article_schemes.pro`
- `tests/tst_article_schemes/tst_article_schemes.cc`

**Files:** Implement ArticleStreamDevice in owned scheme files and wire media handlers to it. Add the dedicated stream suite with all three production reader-failure routes; retain completed-resource coverage in scheme tests.

**Interfaces:** Exact spec §2.6 `ArticleStreamDevice(sptr<Dictionary::DataRequest> const &,std::shared_ptr<DataRequestReader const>,QObject * jobParent)`, destructor, `isSequential() const`, `size() const`, `bytesAvailable() const`, `atEnd() const`, `close() override`, `cancel() noexcept`, protected readData/writeData, signals `completed()` and `failed(QString const &)`. GUI construction/pump owns producer handle/connections separately from the mutex-protected IO state. IO methods/readers never retain, copy, dereference or release that handle. Consume TestSupport::controlledReaderFactory and exact ReadOperation/ReadProbeSnapshot; keep the specified device interface as the sole public streaming API.

- [ ] **Step 1: Write failing stream/state tests.** Fixed live bytes must include an initial chunk before reply and at least two later appends observed by the real WebEngine consumer in order. Assert size()==0, backlog transitions, read(maxSize==0)==0, negative size==-1, unfinished empty==0, successful drained completion==-1/atEnd true, cancellation==-1/atEnd true, and failed drained==-1/atEnd false. Distinguish unknown existence, empty success and producer error with partial bytes before/after reply. For each uniquely armed FullData/Slice/StreamCopy case require entered==1, thrown==1, returned==0 and exactly its expected calling thread: GUI, GUI, Chromium IO respectively. Correlate the unique message with one producerFailed and the real loader's terminal failure. Trigger FullData via successful buffered completion, Slice via an append after live reply, StreamCopy via real IO after reply with staged bytes. No direct invocation/catch of the throwing reader satisfies acceptance. For the real StreamCopy loader outcome assert the production-side probe, using the ControlledDataRequest fixture's `readProbe`:

```cpp
const auto probe = request->readProbe(TestSupport::ReadOperation::StreamCopy);
QCOMPARE(probe.entered, 1);
QCOMPARE(probe.thrown, 1);
QCOMPARE(probe.returned, 0);
QCOMPARE(probe.callingThreads.size(), 1);
QVERIFY(*probe.callingThreads.cbegin() != QThread::currentThreadId());
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_stream/tst_article_stream.pro -o tests/tst_article_stream/Makefile && make -C tests/tst_article_stream -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_article_stream/tst_article_stream`; run the scheme suite with the same Browser budget. Require missing device behavior or production failure-boundary assertion.
- [ ] **Step 3: Implement §3.1.** Separate GUI producer ownership/pump from synchronized IO bytes/offset/terminal state. Positive deltas call reader->dataSlice on GUI inside the device catch boundary; only successful copies publish progress. Open ReadOnly|Unbuffered on GUI before reply. IO readData calls producer-free copyStreamBytes under the state mutex; advance offset only on success and publish failure/wakeup after unlocking. No-Range gdau/gdvideo is sequential size 0/full 200; completed buffered resources and Range use a separate full-size QBuffer, not a second mode in this device. Catch FullData in the real handler and Slice/StreamCopy in their real device boundaries. Before reply, one producerFailed/fail(RequestFailed), zero reply; after reply, one failed forwarded to producerFailed, drain prior bytes then -1/atEnd false, no second fail/reply. readChannelFinished/completed occur only on success. Copy throw leaves offset unchanged and cannot escape Qt. Loader close/cancel marshals GUI cancellation after lock release; device stays job-owned until job deletion.
- [ ] **Step 4: Confirm green.** Repeat both suites and accumulated tests. Check entered/thrown/returned==1/1/0 for each unique forced operation, GUI FullData/Slice and actual Chromium IO StreamCopy, zero IO producer calls, exact terminal notification counts and no successful EOF after failure. Run direct-device interleaving/cancel/reentrancy probes as supplementary evidence. Exercise successful default-factory buffered/live payloads so substitution cannot replace production linkage. Task 17 adds browser Range semantics.
- [ ] **Step 5: Commit.** `git add articleschemes.hh articleschemes.cc tests/tst_article_stream tests/tst_article_schemes && git commit -m "feat(webcore): stream request bytes with guarded failure boundaries"`.

## Task 17: Pin Chromium Range semantics against real scheme jobs

**Implementer tier:** Advanced
**Lane:** webcore
**Depends on:** Task 16
**Ownership:**
- `articleschemes.hh`
- `articleschemes.cc`
- `tests/tst_article_schemes/tst_article_schemes.pro`
- `tests/tst_article_schemes/tst_article_schemes.cc`
- `tests/tst_article_stream/tst_article_stream.pro`
- `tests/tst_article_stream/tst_article_stream.cc`

**Files:** Extend handler completion/Range branch and real browser tests; retain tests for direct device/reader invariants.

**Interfaces:** Consume `QWebEngineUrlRequestJob::requestHeaders`, `DataRequestReader::fullData`, full-backing QIODevice final mode and ArticleUrls dispatch. No new public API. Parse Range only for the specified readiness/positioning behavior; Chromium chooses status/bounds from headers and full backing size.

- [ ] **Step 1: Add the exact §6.3 Range oracle to tst_article_stream.** Real WebEngine requests use fixed full body ABCDEFGHIJ (size 10): no Range→ABCDEFGHIJ/200/no Content-Range/live size 0; bytes=0-→ABCDEFGHIJ/206/bytes 0-9/10/length 10; bytes=3-→DEFGHIJ/206/bytes 3-9/10/length 7; bytes=3-5→DEF/206/bytes 3-5/10/length 3; bytes=-2→IJ/206/bytes 8-9/10/length 2; bytes=-20→ABCDEFGHIJ/206/bytes 0-9/10/length 10; bytes=invalid→completed full body/200/no Content-Range/length 10. bytes=10- and bytes=0- against empty content fail RequestFailed with zero reply. Each satisfiable buffered row also checks full backing size 10 and resolved position before handoff. Observe delivered bytes and actual loader metadata, not generated headers alone. Any Range field selects completion buffering; unparseable/multiple/overflow forms fall back to full 200. Include cancellation and fullData-failure probes. Fixed data rows use the real fetch's observed body/status/header, with empty header meaning absent:

```cpp
QTest::addColumn<QByteArray>("range");
QTest::addColumn<QByteArray>("body");
QTest::addColumn<int>("status");
QTest::addColumn<QByteArray>("contentRange");
QTest::newRow("open-zero") << QByteArray("bytes=0-") << QByteArray("ABCDEFGHIJ") << 206 << QByteArray("bytes 0-9/10");
QTest::newRow("open-three") << QByteArray("bytes=3-") << QByteArray("DEFGHIJ") << 206 << QByteArray("bytes 3-9/10");
QTest::newRow("bounded") << QByteArray("bytes=3-5") << QByteArray("DEF") << 206 << QByteArray("bytes 3-5/10");
QTest::newRow("suffix") << QByteArray("bytes=-2") << QByteArray("IJ") << 206 << QByteArray("bytes 8-9/10");
QTest::newRow("long-suffix") << QByteArray("bytes=-20") << QByteArray("ABCDEFGHIJ") << 206 << QByteArray("bytes 0-9/10");
QTest::newRow("invalid-full") << QByteArray("bytes=invalid") << QByteArray("ABCDEFGHIJ") << 200 << QByteArray();
```

Keep the no-header live case separate from an empty Range field, and keep unsatisfiable rows as terminal-failure/zero-reply tests.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_stream/tst_article_stream.pro -o tests/tst_article_stream/Makefile && make -C tests/tst_article_stream -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_article_stream/tst_article_stream`; require concrete Range/status/body mismatch before changing code.
- [ ] **Step 3: Implement corrected SD-06/16.** Branch on presence of Range at request start; wait for producer terminal completion and fullData snapshot, resolve ordinary/suffix start against checked full length, reject unsatisfiable bounds as RequestFailed, open complete backing data and position before reply. Never shorten the buffer, report range-only size, manually fake partial status or use an in-progress producer length as final size. Preserve non-Range sequential behavior.
- [ ] **Step 4: Confirm green.** Repeat Step 2, scheme suite and accumulated tests. Verify every exact body/status/Content-Range/length row through real QWebEnginePage/QWebEngineUrlRequestJob; supplementary direct checks prove full size/position. Malformed Range returns full 200, parsed unsatisfiable Range never replies, and cancellation releases producer ownership.
- [ ] **Step 5: Commit.** `git add articleschemes.hh articleschemes.cc tests/tst_article_schemes tests/tst_article_stream && git commit -m "fix(webcore): buffer full media for Chromium range requests"`.

## Task 18: Own the shared profile, immutable page policy and network barriers

**Implementer tier:** Capable
**Lane:** webcore
**Depends on:** Task 17, Task 11
**Ownership:**
- `articleprofile.hh`
- `articleprofile.cc`
- `articleschemes.hh`
- `articleschemes.cc`
- `tests/tst_external_content/tst_external_content.pro`
- `tests/tst_external_content/tst_external_content.cc`

**Files:** Create profile module/test pair; implement the exact page-local ArticleRequestInterceptor in schemes. Test using real QWebEnginePage directly so this task does not require the not-yet-converted ArticleWebPage. Task 22 switches ArticleView; Task 26 adds complete ArticleView policy acceptance.

**Interfaces:** Exact spec §2.9 `ArticleProfileController(Config::Class &,ArticleNetworkAccessManager &,QWidget *)`, destructor, `profile() const noexcept -> QWebEngineProfile *`, `attachPage(QWebEnginePage *)`, `applyPreferences()`, `clearForExit()`, signal `exitCachesReady(bool timedOut)`. Exact ProxyAuthAdapter/OriginAuthAdapter constructors/authenticate methods from §2.9. Exact §2.6 `ArticleRequestPolicy {bool blockExternal=false; QSet<QString> dictionaryHosts; QMap<QString,QUrl> localFileRedirects;}` and `ArticleRequestInterceptor(QObject * =nullptr)`, `setPolicy(std::shared_ptr<ArticleRequestPolicy const>)`, `interceptRequest(QWebEngineUrlRequestInfo &)`. Controller owns shared profile/handler/auth adapters; each attached page owns its own interceptor. Publish through that interceptor before navigation; no ArticleProfile wrapper, registerPage hook, URL-keyed shared policy or alternative shutdown API.

- [ ] **Step 1: Add failing shared-profile/browser tests.** Assert storage name goldendict-articles, NoPersistentCookies, exact cache/storage paths and local/remote/JS/LocalStorage settings, shared handler and distinct page-owned interceptors. Two pages with the same URL but different snapshots must remain isolated. Pin §4.8's ordered sameSite/initiator/first-party rules using retained getHostBase heuristic, DNS-boundary subdomains, .co.uk, trailing dot, IPv4/IPv6, malicious suffixes and port independence. Opaque/absent initiator has no dictionary-host privilege; frame A→A succeeds and A→unrelated B fails. Precomputed file redirects are values, with no IO filesystem access. Test applyPreferences proxy/cache/UA/live policy replacement, genuine direct/proxy auth and cancellation, and clearForExit once on signal or 5 s timeout while pages still live.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_external_content/tst_external_content.pro -o tests/tst_external_content/Makefile && make -C tests/tst_external_content -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_external_content/tst_external_content`; require profile/policy behavior failures.
- [ ] **Step 3: Implement §§3.5/4.8.** Configure application proxy before named profile construction; explicit NoProxy/system empty fallback/HttpProxy/Socks5/HttpCachingProxy mappings and retained QNAM settings follow the spec. Set NoPersistentCookies, webengine cache/storage paths, positive-MiB saturated DiskHttpCache versus nonpositive NoCache, default-UA/hide-header behavior. attachPage installs that page's interceptor and auth connections without including ArticleWebPage. Parent the profile to this controller; name its page-owned interceptor gdArticleRequestInterceptor and make attachment idempotent. Maintain GUI-guarded attached pages and access the interceptor's current immutable snapshot privately for preference replacement, preserving per-page hosts/mappings. Interceptor atomically loads one immutable snapshot per callback; canonical hosts use getHostBase sameSite plus exact IP/DNS-boundary rules in the ordered table. Internal schemes/file fix-ups/data/about/blob obey that table; no union or keyed retirement grace. GUI computes file mappings and swaps snapshots before navigation/on preferences changes. Auth adapters borrow QAuthenticator only during signal delivery and never cross-write origin/proxy credentials. clearForExit clears QNAM, connects WebEngine clearHttpCacheCompleted before calling clear, and emits exitCachesReady once on signal or 5 s deadline; repeated exits join. MainWindow later destroys pages before controller/profile. The attachment mechanism is pinned by this real-page test, with cfg/resources supplied by the fixture:

```cpp
ArticleProfileController controller(cfg, resources, nullptr);
QWebEnginePage first(controller.profile()), second(controller.profile());
controller.attachPage(&first);
controller.attachPage(&second);
auto *a = dynamic_cast<ArticleRequestInterceptor *>(
    first.findChild<QObject *>("gdArticleRequestInterceptor"));
auto *b = dynamic_cast<ArticleRequestInterceptor *>(
    second.findChild<QObject *>("gdArticleRequestInterceptor"));
QVERIFY(a && b && a != b);
QCOMPARE(first.profile(), second.profile());
QCOMPARE(controller.profile()->parent(), &controller);
```
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated suites. Bind the real local proxy/auth fixture and verify a genuine 407 reaches proxyAuthenticationRequired, the configured fallback fills credentials and cancellation terminates; a boolean unit slot alone is insufficient. Run thread-affinity checks forbidding cfg/widgets/filesystem access in IO callbacks.
- [ ] **Step 5: Commit.** `git add articleprofile.hh articleprofile.cc articleschemes.hh articleschemes.cc tests/tst_external_content && git commit -m "feat(profile): isolate article policy proxy auth and cache lifetime"`.

## Task 19: Establish generation-stamped bridge startup before document navigation

**Implementer tier:** Frontier
**Lane:** webview
**Depends on:** Task 18, Task 15, Task 10
**Ownership:**
- `articlewebview.hh`
- `articlewebview.cc`
- `articlebridge.hh`
- `articlebridge.cc`
- `resources.qrc`
- `scripts/article-bootstrap.js`
- `scripts/article-dom.js`
- `tests/tst_article_bridge/tst_article_bridge.pro`
- `tests/tst_article_bridge/tst_article_bridge.cc`

**Files:** Create exact WebEngine view/page and bridge declarations from §§2.7/2.11, implementations for profile/page/context-menu/bridge startup, scripts and resource aliases. Existing ArticleView is not switched yet.

**Interfaces:** Exact §2.11 `ArticleWebPage(QWebEngineProfile *,QObject * =nullptr)`, destructor, protected acceptNavigationRequest/createWindow; signals `userLink(QUrl const &,bool newTab)`, `navigationRequested(QUrl const &,NavigationKind)`. Exact ArticleWebView configure(QWebEngineProfile *,Config::Class *), setSelectionBySingleClick(bool), isMidButtonPressed() const noexcept, triggerPageAction(WebAction,bool=false), doubleClicked(QPoint). Exact §2.7 `ArticleBridge(QWebEnginePage *,QObject * viewParent)`, destructor, `generation() const noexcept -> int`, `reset()`, `run(QString const &,Result={})`, `runInFrame(QWebEngineFrame const &,QString const &,Result={})`, with Result=std::function<void(QVariant const &)>. Only slots are `onJsActiveArticleChanged(QString,int)`, `onJsReadingState(QVariantMap,int)`, `onJsFrameEvent(QVariantMap,int)`; signals activeArticleChanged, readingStateChanged, frameEvent, generationChanged(int), bridgeReady(int). Bridge owns WebChannel registered as articleview and borrows page. Helper tests use QObject viewParent without including old ArticleView. Public JS helpers/ready fields are exactly spec §4.6, not a replacement command/gdDom method protocol.

- [ ] **Step 1: Write custom-main failing real-page bridge tests.** Call registerArticleSchemes before app. Pin the three exact slot signatures with generation last, and permanent facade methods of the same names. An early parser-time active callback and a post-ready callback both arrive stamped. Ready snapshot is `{kind:"ready",currentArticle,sx,sy,articles,hasAudio}`, then debounced kind:"state" updates; reject unknown kinds/duplicate-ready/stale generation and reconcile parser-time IDs against current gdfrom- article membership. Test navigation during queued JS, invalid QVariant, stale operation callback and destruction without releasing a newer queue. Add INT_MAX generation exhaustion checks against the checked private increment/page recreation path; never wrap. With a real fixture page and its bridge, pin the public surface/stale inbound gate:

```cpp
QVERIFY(bridge.metaObject()->indexOfSlot("onJsActiveArticleChanged(QString,int)") >= 0);
QVERIFY(bridge.metaObject()->indexOfSlot("onJsReadingState(QVariantMap,int)") >= 0);
QVERIFY(bridge.metaObject()->indexOfSlot("onJsFrameEvent(QVariantMap,int)") >= 0);
const int retired = bridge.generation();
QSignalSpy active(&bridge, &ArticleBridge::activeArticleChanged);
bridge.reset();
QCOMPARE(bridge.generation(), retired + 1);
bridge.onJsActiveArticleChanged(QStringLiteral("gdfrom-dict-a"), retired);
QCOMPARE(active.count(), 0);
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_bridge/tst_article_bridge.pro -o tests/tst_article_bridge/Makefile && make -C tests/tst_article_bridge -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_article_bridge/tst_article_bridge`; require missing bridge or early-callback/generation failure.
- [ ] **Step 3: Implement permanent stamped facade and ready barrier.** Embed installed :/qtwebchannel/qwebchannel.js synchronously into gd-bootstrap-G. Inject gd-bootstrap-G and gd-dom-G at DocumentCreation/MainWorld/top document; pure helpers exist immediately and gdOnBridgeReady runs after helper definition, DOMContentLoaded and channel callback, idempotently waiting for DOM+channel. Keep window.articleview installed after readiness and append generation as the last argument to all three inbound calls. Preserve existing gdArticleContents/gdCurrentArticle/gdAudioLinks producers. Implement spec §4.6 gdWordAtPoint, gdSaveSelection/gdRestoreSelection, gdHighlightAll/gdClearHighlights/gdScrollToHit, gdFrameRect/gdMeasureFrame and gdCaptureState/gdRestoreArticleState; gd-dom helpers must not interpret Qt PCRE as JS RegExp. Queue entries hold generation/operation/frame/script/callback/latch, dispatch one at a time after Ready, and validate QPointer+page+generation+operation before callback/next dispatch. reset enters Installing for the next checked int generation and replaces only this generation's scripts before navigation, never from loadStarted; exhaustion asks the owning view to recreate page/bridge before another load. Disable BFCache on every application page. Configure a view-owned page once and call its profile-parent ArticleProfileController::attachPage before any load; attach temporary createWindow pages the same way, capture the first target once and delete the temporary page. Connect userLink/navigationRequested and copy ephemeral context data during events. Task 20 completes the third gd-frames-G script before any ArticleView integration.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated suites. Real DOM assertions pin safe JSON quoting, actual attributes, audio map compatibility and capture/restore state. A plain QObject parent is sufficient because the bridge emits signals and never dereferences an ArticleView. Verify Installing→ChannelPending→Ready, one in-flight operation, stale callback isolation and cached-change emission. Task 22 adds the actual ArticleView consumer.
- [ ] **Step 5: Commit.** `git add articlewebview.hh articlewebview.cc articlebridge.hh articlebridge.cc resources.qrc scripts/article-bootstrap.js scripts/article-dom.js tests/tst_article_bridge && git commit -m "feat(webview): add generation-safe WebChannel startup"`.

## Task 20: Reinstrument native website frames across subsequent navigations

**Implementer tier:** Frontier
**Lane:** webview
**Depends on:** Task 19, Task 8
**Ownership:**
- `websiteframes.hh`
- `websiteframes.cc`
- `scripts/website-frame.js`
- `resources.qrc`
- `tests/tst_website_frames/tst_website_frames.pro`
- `tests/tst_website_frames/tst_website_frames.cc`
- `scripts/article-bootstrap.js`
- `scripts/article-dom.js`

**Files:** Create the frame coordinator, MainWorld frame-local helpers, resources and real-origin tests. Consume website.cc name/id emission from Task 8 read-only. Keep helpers in each document's real remote origin.

**Interfaces:** Exact §2.7 `WebsiteFrameKey {QVector<int> childPath; QStringList namePath; QString docId; int generation=0;}`; `WebsiteFrames(QWebEnginePage *,ArticleBridge *,QObject * =nullptr)`, destructor, `reset(int)`, `discover()`, `wordAtPoint(QPointF const &,std::function<void(QString const &)>)`; slot `receive(QVariantMap const &)`, signals `frameText(QString const &,QString const &,bool contextMenu)`, `frameUnavailable(QString const &,QString const &)`. Exact §4.7 message `{gdFrameEvent:"ready"|"click"|"context"|"measure"|"resize",generation,name,path:[{name,index},...],docId,text,height,width}` with relevant fields only. Frame scripts run through ArticleBridge::runInFrame and report via postMessage/stamped onJsFrameEvent.

- [ ] **Step 1: Add failing first/second/nested navigation tests before consumers integrate.** Load the actual HTTP-origin iframe; assert relative.js/data.json and real-origin-fetch succeed on its host/port and no HTML was serialized to a custom origin. Navigate it to second.html, then a different dictionary path; reinjection, size measurement, enclosing link text and word hit work again with fresh docId. Reject arbitrary same-name native frames, spoofed ancestor segments, stale docIds and nonfinite/over-limit measurements. Verify 250 ms sampling/3-stable-or-10 s, new-document rearming, same-height resize suppression and actual framing denial's 10 s hint under one aggregate test deadline. Capture the two actual `ArticleBridge::frameEvent(QVariantMap const &)` ready payloads as firstReady/secondReady; assert their identity fields:

```cpp
QCOMPARE(firstReady.value("gdFrameEvent").toString(), QStringLiteral("ready"));
QCOMPARE(secondReady.value("gdFrameEvent").toString(), QStringLiteral("ready"));
QCOMPARE(firstReady.value("name").toString(), QStringLiteral("gdexpandframe-dict-a"));
QCOMPARE(secondReady.value("name").toString(), firstReady.value("name").toString());
QVERIFY(!firstReady.value("docId").toString().isEmpty());
QVERIFY(!secondReady.value("docId").toString().isEmpty());
QVERIFY(firstReady.value("docId") != secondReady.value("docId"));
QCOMPARE(firstReady.value("generation"), secondReady.value("generation"));
```

The top document remains unchanged between these two subframe loads; pair these checks with the native path/nonce validation and actual word/height observations.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_website_frames/tst_website_frames.pro -o tests/tst_website_frames/Makefile && make -C tests/tst_website_frames -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_website_frames/tst_website_frames`; record second-navigation/instrumentation/coordinates failure.
- [ ] **Step 3: Implement native frame discovery and relay.** Install gd-frames-G at DocumentCreation/MainWorld/RunsOnSubFrames=true, replacing it with bootstrap/DOM scripts before top-level loads. Each child document generates a fresh random docId and helper instance on navigation; no remote subframe receives WebChannel transport. Parent relays verify event.source against actual iframe.contentWindow, append only their own verified DOM path segment and derive root dictionary membership at top. Cap text at 64 Ki UTF-16, path depth 32, positive measurements at 2^24. Rediscover native mainFrame().children() recursively; uniquely correlate nonce, verify value.isValid plus unchanged childPath/value equality on callback, and keep native child indices distinct from DOM window.frames order. Reorder/replacement invalidates discovery. On ready sample immediately/250 ms, reveal valid height, stop at three equal samples or 10 s; changed resize re-arms a bounded run, same height cannot loop. No ready by 10 s produces the specified visible inline-mode hint. Coordinates are logical view/CSS viewport pixels: divide by zoom, subtract parent rect origin and border at each boundary, never apply device pixel ratio or duplicate scroll offsets. Use gdWordAtPoint in the deepest verified frame, with bounded callback/deadline and empty result on invalid identity.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated tests. First/second navigation must produce distinct docIds with working height/click/context/word assertions at 100%/200% zoom and nested borders. Actual CSP/X-Frame-Options denial keeps the real origin and produces the timed hint; arbitrary same-name frames/stale nonces cannot change state. Native caret APIs and fallback return a word/empty within the bound without corrupting selection.
- [ ] **Step 5: Commit.** `git add websiteframes.hh websiteframes.cc scripts/website-frame.js resources.qrc scripts/article-bootstrap.js scripts/article-dom.js tests/tst_website_frames && git commit -m "feat(frames): preserve native origins across reinstrumentation"`.

## Task 21: Model indexed application history and bounded asynchronous state capture

**Implementer tier:** Advanced
**Lane:** workflow
**Depends on:** Task 20, Task 4
**Ownership:**
- `articlehistory.hh`
- `articlehistory.cc`
- `tests/tst_article_history/tst_article_history.pro`
- `tests/tst_article_history/tst_article_history.cc`

**Files:** Create the history value module/test target. It stores integer keys and never owns QWebEngineHistoryItem values or performs navigation; a real QWebEnginePage in the test supplies actual indices. ArticleView connects capture/navigation in Task 22.

**Interfaces:** Consume shared `NavigationKind` from articleurls.hh. Define exactly `ArticleState {QString currentArticle; double sx=0,sy=0; bool restoreScroll=true;}` and `ArticleHistory::{reset(),commit(QUrl const &,int itemIndex,NavigationKind),stateFor(int,ArticleState *) const -> bool,setCurrent(int,ArticleState const &),truncateAfter(int)}`. Private entries are `{index,finalUrl,navigationKind,hasState,ArticleState}` keyed by integer, with absent state distinct from zero offsets. Do not expose a competing push/backTarget/currentIndex API.

- [ ] **Step 1: Write failing indexed-history tests.** Mirror real engine indices/count using a controlled real page and pure entry tests. Pin duplicate URLs at different indices, stateFor false for absent/failed slots versus true for recorded sx=sy=0, outgoing setCurrent, commit once for final redirected URL, forward truncation after Back then new load, preserved Back/Forward entries, reload identity/restoreScroll=false and ordinary commit not resetting prior state. No second capture coordinator is implemented here. The zero-versus-absent regression is directly executable:

```cpp
ArticleHistory history;
ArticleState result;
const QUrl repeated("gdlookup://localhost/A?group=1");
history.commit(repeated, 0, NavigationKind::NewLookup);
QVERIFY(!history.stateFor(0, &result));
ArticleState zero;
zero.currentArticle = QStringLiteral("gdfrom-dict-a");
history.setCurrent(0, zero);
history.commit(repeated, 1, NavigationKind::NewLookup);
QVERIFY(!history.stateFor(1, &result));
QVERIFY(history.stateFor(0, &result));
QCOMPARE(result.sx, 0.0);
QCOMPARE(result.sy, 0.0);
QCOMPARE(result.currentArticle, QStringLiteral("gdfrom-dict-a"));
history.truncateAfter(0);
QVERIFY(!history.stateFor(1, &result));
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_history/tst_article_history.pro -o tests/tst_article_history/Makefile && make -C tests/tst_article_history -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_article_history/tst_article_history`; require missing history implementation or a specific index/absent-state mismatch.
- [ ] **Step 3: Implement spec §§2.8/3.3 value map.** Key every operation by supplied engine itemIndex, preserve URL as metadata, create/retain absent-state slots without copying prior state, truncate only after explicit outgoing index, and reset only when the owning view actually clears engine history. Reload preserves identity and starts at top; duplicate URLs never coalesce. Capture, generation and restore orchestration belongs exclusively to Task 22's ArticleView.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated tests. The workflow-owned project uses Browser modules and a 120 s watchdog now; Task 27 later adds the actual ArticleView closure without any webview lane edits to this test. Task 22 must prove the same end-to-end history contract in its own tst_article_load.
- [ ] **Step 5: Commit.** `git add articlehistory.hh articlehistory.cc tests/tst_article_history && git commit -m "feat(history): track indexed article snapshots"`.

## Task 22: Switch ArticleView to coordinated asynchronous document workflows

**Implementer tier:** Frontier
**Lane:** webview
**Depends on:** Task 20, Task 21, Task 10, Task 13
**Ownership:**
- `articleview.hh`
- `articleview.cc`
- `articleview.ui`
- `articlewebview.hh`
- `articlewebview.cc`
- `articlebridge.hh`
- `articlebridge.cc`
- `tests/tst_article_load/tst_article_load.pro`
- `tests/tst_article_load/tst_article_load.cc`
- `tests/tst_article_bridge/tst_article_bridge.pro`
- `tests/tst_article_bridge/tst_article_bridge.cc`

**Files:** Replace articleview.hh:8–14/34–43/117 and articleview.cc's WebKit object ownership. This is the coordinated switch of all 33 synchronous result sites, not 33 isolated edits. Obsolete save/print implementation is removed with the old renderer; its supported async public wrappers are introduced with completed production dependencies in Task 29, before the full application's consumers switch.

**Interfaces:** Retain every existing ArticleView constructor parameter/default in its original order, inserting exactly `QWebEngineProfile & profile` before ArticleNetworkAccessManager & per spec §2.11; do not add an ArticleProfile wrapper. Implement the exact spec showDefinition FtsHighlightSpec overload, non-const async `toHtml(std::function<void(QString const &)>)`, `captureState(std::function<void(ArticleState const &)>)`, cached getArticlesList/getCurrentArticle/getActiveArticleId/hasSound and articlesChanged/soundAvailabilityChanged signals. Preserve other existing public operations/signals. ArticleView owns one view-owned ArticleWebPage, ArticleBridge, WebsiteFrames and ArticleHistory; the bridge owns WebChannel. ArticleWebView::configure calls the profile-parent controller's attachPage before the first load; helper tests use the same owner/attachment path. The view obtains the direct-child gdArticleRequestInterceptor with checked dynamic_cast and publishes its immutable page policy before navigation. `captureState` reads gdCaptureState and converts finite doubles to ArticleState, with the single coordinator's 500 ms fallback. Internal JS work uses ArticleBridge::run/runInFrame, not a second public bridge protocol.

- [ ] **Step 1: Write failing real ArticleView load/lifetime tests.** Load FixtureDictionary article/resource; assert fixture-headword, gdfrom-dict-a/b, existing load/active/title/icon signals, all showDefinition/showDictionary/picture/reload variants and actual article/audio cache changes. Navigate with in-flight JS/capture/action, replace rapid pending intent and destroy owner; stale results must neither perform a second engine action nor touch a fresh queue. Prove A→B→Back restores A's outgoing currentArticle/sx/sy with fresh bridge generation, Forward restores B, duplicate URLs have distinct indices, absent state differs from recorded zero offsets, reload returns top, forward truncates, redirect commits once, fragment adds no engine item, ordinary loads never reset history and image/frame-induced layout settles within the stated bound. Test 500 ms fallback from the latest 100 ms-debounced snapshot without a live capture reply. Once fixture A is ready, this uses the real async public call; shared result storage stays valid if the assertion times out:

```cpp
auto captured = std::make_shared<std::optional<ArticleState>>();
view.captureState([captured](ArticleState const &state) { captured->emplace(state); });
QTRY_VERIFY_WITH_TIMEOUT(captured->has_value(), 1000);
QCOMPARE(captured->value().currentArticle, QStringLiteral("gdfrom-dict-a"));
QVERIFY(std::isfinite(captured->value().sx));
QVERIFY(std::isfinite(captured->value().sy));
```

Include optional/cmath explicitly. Test timeout/fallback with the same callback surface, without exposing a competing navigation API.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_load/tst_article_load.pro -o tests/tst_article_load/Makefile && make -C tests/tst_article_load -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_article_load/tst_article_load`; record WebKit compile or coordinated-workflow assertions.
- [ ] **Step 3: Implement the asynchronous ownership switch and one coordinator.** Account for each of the 33 old synchronous sites in a workflow table in this task's test source: before-navigation capture, post-ready restore, caches, user actions, word/context, find, frame and workflow calls. Convert the UI promoted base to QWebEngineView now. One coordinator stores NavigationKind and latest intent, captures outgoing state once, setCurrent on the outgoing engine index, then bridge.reset/clear caches/frames.reset/publish policy/install scripts/navigate. Stop uncontrolled main-frame navigation and queue the intent after returning; permit exactly one explicit reissue, sharing generation across redirects. Successful final load commits once at currentItemIndex/count; failed load creates no inherited state; new load after Back truncates after outgoing index. Back/Forward preserve entries, Reload keeps identity/restoreScroll=false, fragments scroll by JS without an engine item, and reset only accompanies actual engine clear. After load+bridge Ready, restore current article and settle scrollHeight across two equal animation frames, bounded by 10 frames or 500 ms; apply once per generation and reject NaN/Infinity. Do not wait indefinitely on remote frame readiness. Publish hosts only from dictionaries contributing this page and GUI-computed file mappings before load; all read results and continuations are owner/page/generation/operation guarded. Keep every newly linked method functional; completed save/print dependencies are integrated in Task 29 before root consumers switch.
- [ ] **Step 4: Confirm green.** Repeat Step 2, bridge/frame/history suites and accumulated tests. Assert one capture/action for the latest intent, no stale queue drain, controller attachment/page-local policy before first request and explicit page destruction before bridge. Compile complete migrated renderer/UI with the real profile/history/dictionary closure. Tasks 23–25 add interaction assertions; load/bridge/history behavior must already pass here.
- [ ] **Step 5: Commit.** `git add articleview.hh articleview.cc articleview.ui articlewebview.hh articlewebview.cc articlebridge.hh articlebridge.cc tests/tst_article_load tests/tst_article_bridge && git commit -m "refactor(article): switch document workflows to Qt6 async APIs"`.

## Task 23: Preserve user-action dispatch, dictionary targeting and anchor precedence

**Implementer tier:** Advanced
**Lane:** webview
**Depends on:** Task 22, Task 17
**Ownership:**
- `articleview.hh`
- `articleview.cc`
- `articlewebview.hh`
- `articlewebview.cc`
- `tests/tst_article_load/tst_article_load.pro`
- `tests/tst_article_load/tst_article_load.cc`
- `tests/tst_article_bridge/tst_article_bridge.cc`

**Files:** Modify retained ArticleView::openLink/handleInternalAction and page/view navigation signal plumbing; consume the pure URL parser without editing it.

**Interfaces:** Consume exact `ArticleUrls::parse(QUrl const &,Use::UserAction)`, `Parsed::{kind,url,lookup,dictionaryId,resourceName,error}` and `Lookup::{dictionaryName,anchor,...}`; fragment stays on Parsed::url. Preserve retained openLink/word/new-tab/external dispatch signatures and reference-context resolution in ArticleView. ArticleWebPage emits `userLink(QUrl const &,bool)`/navigationRequested; it does not become a second dispatcher.

- [ ] **Step 1: Add failing click tests on the real view.** Cover group-less ordinary lookup using current reference/group, explicit dictionaries/context, display-name dict target, unknown target, gdpicture resource, gdau/gdvideo playback/save paths, external URL, fragment-only and gdanchor precedence. Ctrl and middle-button lookup create a new tab intent exactly once; Alt/double-click rules retain their old outcome. Invariants include no group-0 fabrication, no swallowed picture, no unconditional rejection of ordinary lookup and intact encoded query/fragment. After obtaining the real fixture anchor's logical viewport point `linkPoint`, exercise the renderer input path:

```cpp
auto *renderer = view.findChild<ArticleWebView *>();
QVERIFY(renderer);
auto *page = qobject_cast<ArticleWebPage *>(renderer->page());
QVERIFY(page);
QSignalSpy actions(page, &ArticleWebPage::userLink);
QWidget *target = renderer->focusProxy() ? renderer->focusProxy() : renderer;
QTest::mouseClick(target, Qt::LeftButton, Qt::ControlModifier,
                  target->mapFrom(renderer, linkPoint));
QTRY_COMPARE_WITH_TIMEOUT(actions.count(), 1, 3000);
QCOMPARE(actions.at(0).at(1).toBool(), true);
```

Also inspect FixtureDictionary's captured arguments and retained view/new-tab effect, so one signal alone cannot hide a dropped action.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_load/tst_article_load.pro -o tests/tst_article_load/Makefile && make -C tests/tst_article_load -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_article_load/tst_article_load`; require a specific dispatch/parameter assertion failure.
- [ ] **Step 3: Route once through retained semantics.** In acceptNavigationRequest distinguish main/child navigation, retain native ordinary HTTP frame navigations, and pass internal actions through parse(UserAction). Resolve display-name dictionary against current IDs, prefer explicit gdanchor over fragment, merge ordinary lookup with reference context only in retained C++ handler. Preserve audio/current article state and gdpicture's full bres URL. QPointer/generation-guard any pre-dispatch state capture and ignore a stale result without popping the new queue.
- [ ] **Step 4: Confirm green.** Repeat load/bridge suites and accumulated tests; assert each clicked action produces one retained effect with exact URL/group/IDs/anchor, and rejected malformed actions are diagnosed without changing history.
- [ ] **Step 5: Commit.** `git add articleview.hh articleview.cc articlewebview.hh articlewebview.cc tests/tst_article_load tests/tst_article_bridge/tst_article_bridge.cc && git commit -m "fix(article): preserve internal action dispatch and anchors"`.

## Task 24: Restore asynchronous selection, word hit testing and context menus

**Implementer tier:** Capable
**Lane:** webview
**Depends on:** Task 23
**Ownership:**
- `articleview.hh`
- `articleview.cc`
- `articlewebview.hh`
- `articlewebview.cc`
- `scripts/article-dom.js`
- `tests/tst_article_bridge/tst_article_bridge.pro`
- `tests/tst_article_bridge/tst_article_bridge.cc`
- `tests/tst_website_frames/tst_website_frames.cc`

**Files:** Modify former QWebElement/hitTestContent/frameAt/selection sites and input/context event flow; test fixed article/DOM/website fixtures read-only.

**Interfaces:** Consume exact gdWordAtPoint/gdSaveSelection/gdRestoreSelection and `WebsiteFrames::wordAtPoint(QPointF const &,std::function<void(QString const &)>)`, frameText's contextMenu flag, copied QWebEngineContextMenuRequest values and QWebEnginePage::selectedText. Keep helper-specific code private and preserve the exact named helper protocol. Preserve synchronous cache getters only for latest-state values; no blocking fresh JS result.

- [ ] **Step 1: Add failing real DOM/input assertions.** Pin selectedText/hasSelection/isFramedArticle cache, whitespace pruning, Alt and single/double-click lookup, safe optional-part selector execution, context link/image/background position and correct selected dictionary. For top+frame word hits assert Latin, Cyrillic, CJK, combining, supplementary, punctuation/whitespace, caretRangeFromPoint and caretPositionFromPoint null/throw/foreign-root fallback. Context request values are copied before the request object expires; pending menu closure cannot dereference a destroyed view. Execute this self-contained script through the real bridge in the ready dom.html document, and assert its callback string is `𐐀word` without any isolated surrogate:

```js
(() => {
  const node = document.createElement('span');
  node.textContent = '𐐀word';
  node.style.cssText = 'position:fixed;left:40px;top:40px;font:24px monospace;z-index:2147483647';
  document.body.appendChild(node);
  const range = document.createRange();
  range.setStart(node.firstChild, 0);
  range.setEnd(node.firstChild, 2);
  const rect = range.getBoundingClientRect();
  const word = gdWordAtPoint(rect.left + rect.width / 2, rect.top + rect.height / 2);
  node.remove();
  return word;
})()
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_bridge/tst_article_bridge.pro -o tests/tst_article_bridge/Makefile && make -C tests/tst_article_bridge -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_article_bridge/tst_article_bridge`; run website frame suite under the same budget.
- [ ] **Step 3: Implement interaction queues/caches.** Use renderer selectionChanged/bridge/frame messages for latest values; preserve mouse-press modifiers/position and queue only user intent with generation. Map context hit data via Qt6 request plus async DOM for missing semantics. JS wordAt tries native caret APIs then bounded Range geometry using property escapes and UTF-16-safe iteration; never split surrogate pairs or spin on zero width. Frames use the verified native coordinate route from Task 20. Reject stale async results without advancing the current operation queue.
- [ ] **Step 4: Confirm green.** Repeat both suites and accumulated tests. Ensure no QWebElement/hitTestContent/frameAt/evaluateJavaScript remains in the renderer closure, selection menus do not read stale request pointers, and per-click compound operations obey one overall deadline.
- [ ] **Step 5: Commit.** `git add articleview.hh articleview.cc articlewebview.hh articlewebview.cc scripts/article-dom.js tests/tst_article_bridge tests/tst_website_frames/tst_website_frames.cc && git commit -m "feat(article): restore async selection and word interactions"`.

## Task 25: Implement renderer find results and Unicode-safe FTS marks

**Implementer tier:** Advanced
**Lane:** webview
**Depends on:** Task 24, Task 10
**Ownership:**
- `articleview.hh`
- `articleview.cc`
- `scripts/article-dom.js`
- `tests/tst_article_bridge/tst_article_bridge.pro`
- `tests/tst_article_bridge/tst_article_bridge.cc`

**Files:** Replace WebKit FindFlags/HighlightAllOccurrences and FTS DOM mutation sites. Do not modify codec FTS or browser fixture sources.

**Interfaces:** Consume FtsHighlightSpec's sourcePattern/expression/wildcards/matchCase, QWebEnginePage::findText and QWebEngineFindTextResult::numberOfMatches/activeMatch, plus exact gdHighlightAll(precomputed spans,matchCase), gdClearHighlights/gdScrollToHit and selection save/restore. Preserve the existing ArticleView find-bar surface; no new ArticleView::findText overload or gdDom markFts protocol. Stamp native find with generation and a query ID changed on every text/options edit or close.

- [ ] **Step 1: Add failing browser find/FTS cases.** Assert count/index, forward/backward, zero matches, wrap and case options after typing overlapping queries; stale callbacks must not update a newer query or document. Add literal/regex/wildcard source serialization and C++ PCRE-offset→DOM Range fixtures containing supplementary/combining text. Clear/reapply marks without changing article text; active match CSS and navigation scroll must survive optional sections. In a ready browser fixture containing exactly two `testneedle` words, pin the actual native callback before testing ArticleView's stale-query UI guard:

```cpp
auto result = std::make_shared<std::optional<QWebEngineFindTextResult>>();
page.findText(QStringLiteral("testneedle"), {},
              [result](QWebEngineFindTextResult const &value) { result->emplace(value); });
QTRY_VERIFY_WITH_TIMEOUT(result->has_value(), 3000);
QCOMPARE(result->value().numberOfMatches(), 2);
QCOMPARE(result->value().activeMatch(), 1);
```

Include optional explicitly and retain result storage safely across cancelled callbacks.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_bridge/tst_article_bridge.pro -o tests/tst_article_bridge/Makefile && make -C tests/tst_article_bridge -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_article_bridge/tst_article_bridge`; require exact result/index/DOM expectation failure.
- [ ] **Step 3: Implement async search and marking.** Increment query ID on every text/options change or close, stamp callbacks with it and bridge generation, clear empty native find and display result count/active/not-found from QWebEngineFindTextResult. For FTS extract normalized visible text plus UTF-16 text-node offset map and snapshot version, compute QRegularExpression spans in C++ with the retained source options, then apply those spans in JS only against the same snapshot/version. Recapture/retry once on DOM mutation within the generation; on a second mutation clear marks and report “Article changed; run the search again.” Bracket with gdSaveSelection/gdRestoreSelection and preserve Unicode scalar boundaries, text structure and GoldenDict-only mark ownership. Never interpret Qt PCRE as JS RegExp; no byte offsets or HighlightAllOccurrences.
- [ ] **Step 4: Confirm green.** Repeat Step 2, codec regex suite and accumulated tests. Navigate while find/mark work is in flight and confirm current DOM/find UI unchanged by old callbacks. Assert all fixed DOM text and surrogate pairs remain intact.
- [ ] **Step 5: Commit.** `git add articleview.hh articleview.cc scripts/article-dom.js tests/tst_article_bridge && git commit -m "feat(search): restore async find and Unicode FTS marks"`.

## Task 26: Prove page-local external content policy with complete real articles

**Implementer tier:** Capable
**Lane:** webcore
**Depends on:** Task 18, Task 25
**Ownership:**
- `articleprofile.hh`
- `articleprofile.cc`
- `articleschemes.hh`
- `articleschemes.cc`
- `tests/tst_external_content/tst_external_content.pro`
- `tests/tst_external_content/tst_external_content.cc`

**Files:** Extend actual ArticleView acceptance tests in the owned suite; repair profile/interceptor within existing ownership if evidence requires it.

**Interfaces:** Consume ArticleView::showDefinition, `ArticleProfileController::attachPage/applyPreferences` and page-owned `ArticleRequestInterceptor::setPolicy(std::shared_ptr<ArticleRequestPolicy const>)`. ArticleView publishes the new snapshot before any navigation resource request; the controller refreshes live preferences without IO config/widget reads. No URL-keyed policy hooks.

- [ ] **Step 1: Add failing real-article policy tests.** Two live ArticleViews share the profile, including identical lookup URLs with different contributing hosts. Each page sees only its own dictionaryHosts snapshot. Pin canonical case/trailing-dot/port handling and SD-09 sameSite with DNS-boundary subdomains, .co.uk, exact IPv4/IPv6 and malicious suffixes. Application initiator may fetch its contributing website; remote A can fetch A's resources but cannot borrow dictionary B's privilege, and absent/opaque initiator cannot acquire the set. Check live preference swap, file redirect mappings, allowed internal/data/about/blob paths and FTP block. Include actual denied third-party image/CSS @import and frame/CSP fixtures, using HttpFixture requests plus DOM outcomes rather than only interceptor mocks. On the two real pages after identical lookup loads, assert attachment/policy identity separately from the observed HTTP denials:

```cpp
QCOMPARE(firstPage.profile(), secondPage.profile());
QCOMPARE(firstPage.url(), secondPage.url());
auto *firstPolicy = dynamic_cast<ArticleRequestInterceptor *>(
    firstPage.findChild<QObject *>("gdArticleRequestInterceptor"));
auto *secondPolicy = dynamic_cast<ArticleRequestInterceptor *>(
    secondPage.findChild<QObject *>("gdArticleRequestInterceptor"));
QVERIFY(firstPolicy && secondPolicy && firstPolicy != secondPolicy);
```

Both pages are obtained from the actual ArticleViews, rather than independently constructed stand-ins.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_external_content/tst_external_content.pro -o tests/tst_external_content/Makefile && make -C tests/tst_external_content -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_external_content/tst_external_content`; require a policy acceptance assertion failure.
- [ ] **Step 3: Complete page-local snapshot lifecycle.** Verify controller attachment before navigation, page ownership of each interceptor and atomic replacement of immutable snapshots for navigation/preferences. Apply the exact ordered §4.8 initiator/first-party rules; destruction retires only that page's interceptor, with no shared URL permission/reference counts. Auth/cache tests use the same real pages/profile and leave no stale callbacks or credentials in logs. Cross-lane wiring must already pass webview owner tests; do not edit ArticleView here.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated browser suites. Assert third-origin server observed no forbidden successful fetch, frame-origin policy still allows approved native website JS/fetch, proxy 407 path is genuine and explicit cancellation leaves no dialog/page callback. No live-desktop acceptance has occurred yet.
- [ ] **Step 5: Commit.** `git add articleprofile.hh articleprofile.cc articleschemes.hh articleschemes.cc tests/tst_external_content && git commit -m "test(policy): pin complete article external-content isolation"`.

## Task 27: Publish complete saves atomically and verify outgoing history on a real page

**Implementer tier:** Capable
**Lane:** workflow
**Depends on:** Task 21, Task 26
**Ownership:**
- `savearticle.hh`
- `savearticle.cc`
- `tests/tst_save_article/tst_save_article.pro`
- `tests/tst_save_article/tst_save_article.cc`
- `tests/tst_article_history/tst_article_history.pro`
- `tests/tst_article_history/tst_article_history.cc`

**Files:** Create save module and suite; complete the history suite's real-page closure. Existing MainWindow save collector remains until its extraction consumer changes in Task 33; this task moves its implementation into savearticle, without editing MainWindow.

**Interfaces:** `SaveArticle(ArticleView *,ArticleNetworkAccessManager &,QObject * =nullptr)`, `~SaveArticle()`, `State {Idle,Capturing,Collecting,Downloading,Publishing,Complete,Cancelled,Failed}`, `state() const noexcept -> State`, `start(QString const & htmlDestination,bool complete)`, `cancel() noexcept`; signals `saved(QString const &)`, `failed(QString const &)`, `cancelled(QString const &)`. Consume non-const `ArticleView::toHtml(std::function<void(QString const &)>)`, `ArticleBridge::generation() const noexcept -> int`/`generationChanged(int)`, guarded real resource/download handles and ArticleHistory's `setCurrent(int,ArticleState const &)`/`stateFor(int,ArticleState *) const -> bool`. Put the extracted pure `SaveArticleDetail::ResourceSpan {qsizetype offset,length; QUrl url;}` and inline `collectResourceSpans(QString const &,QUrl const &) -> QVector<ResourceSpan>` in savearticle.hh using only Core types/forward declarations. Both SaveArticle and Task 10's preconfigured Core regex target call that identical production helper; the pure suite does not instantiate/MOC/link the SaveArticle controller or include ArticleView.

- [ ] **Step 1: Add failures for the actual collector/transaction and real history.** Assert complete save reopens with local image/style resources, HTML-only has no asset directory, canonical encoded URL dedup preserves query distinction and stable `_qN` filenames, traversal paths cannot escape staging. Hold /slow-resource, cancel/navigation/tab-close and verify every request cancelled/no owned files while old HTML/assets remain. Inject filesystem failure at download/write/asset-rename/HTML-commit by temporary-path permissions/collisions, not production forced-failure hooks. Observe asset rename before HTML commit, failed commit removes only this operation's published assets and success emits once. Expand tst_article_history to real page A→B→back/forward with outgoing offsets/article, fresh bridge generation, disabled cache, duplicate URL indices, reload top, forward truncation, redirect single commit, fragment no entry, no ordinary reset, 500 ms capture fallback and bounded layout/image/frame growth settle. The same production collector used by the transaction has this pure oracle; spans cover URL text in UTF-16 coordinates and preserve every occurrence for rewriting:

```cpp
const QString html = "<img src=\"bres://d/a.png?q=1\"><img src=\"bres://d/a.png?q=2\"><img src=\"bres://d/a.png?q=1\">";
const auto spans = SaveArticleDetail::collectResourceSpans(html, QUrl("gdlookup://localhost/A?group=1"));
QCOMPARE(spans.size(), 3);
QCOMPARE(spans[0].url, QUrl("bres://d/a.png?q=1"));
QCOMPARE(spans[1].url, QUrl("bres://d/a.png?q=2"));
QCOMPARE(spans[2].url, spans[0].url);
QCOMPARE(html.mid(spans[0].offset, spans[0].length), QString("bres://d/a.png?q=1"));
```

The download map deduplicates canonical URLs separately; it must still rewrite all three spans.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_save_article/tst_save_article.pro -o tests/tst_save_article/Makefile && make -C tests/tst_save_article -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_save_article/tst_save_article`; build/run `tests/tst_article_history/tst_article_history.pro` with the same qmake6/make/120 s command pattern. Fail on missing save behavior or a specific outgoing-state assertion.
- [ ] **Step 3: Extract and implement §4.9 transaction.** Capture generation/operation, collect URL spans from immutable HTML and download every required resource before publishing. Create same-filesystem unique staging/assets and final `<stem>_files-<operationId>` paths. Rename assets first, then QSaveFile UTF-8 HTML with `setDirectWriteFallback(false)`; commit success is Complete, after which cancel never retracts output. Before commit, any abort/failure invalidates callbacks, cancels all handles and removes only owned staging/final-uncommitted assets. Keep the pure collector in savearticle.hh, shared with the codec-owned preconfigured regex suite. Re-run `qmake6 tests/tst_regex/tst_regex.pro -o tests/tst_regex/Makefile && make -C tests/tst_regex -j$(nproc) && xvfb-run -a timeout 60 tests/tst_regex/tst_regex saveCollector`; require the real helper and every source-tagged save family, with no cross-lane edits. History tests link the existing real coordinator; change neither ArticleView nor a test-only copy here. Its owning webview tasks must have passed the same documented restoration contract before this task starts.
- [ ] **Step 4: Confirm green.** Repeat both suites and accumulated tests. The history suite now uses Browser/Article modules and 120 s watchdog. Reopen saved HTML in a real renderer and assert image naturalWidth>0, not just existence/string output. Confirm failing required resources never report Complete.
- [ ] **Step 5: Commit.** `git add savearticle.hh savearticle.cc tests/tst_save_article tests/tst_article_history && git commit -m "feat(save): publish cancellable article transactions atomically"`.

## Task 28: Build PDF snapshots with a Ready barrier and synchronous printer painting

**Implementer tier:** Capable
**Lane:** workflow
**Depends on:** Task 27, Task 20
**Ownership:**
- `printpipeline.hh`
- `printpipeline.cc`
- `tests/tst_print_pipeline/tst_print_pipeline.pro`
- `tests/tst_print_pipeline/tst_print_pipeline.cc`

**Files:** Create exact print module/target using real renderer and QPdfDocument; no asynchronous WebEngine call inside paintRequested.

**Interfaces:** `PrintPipeline(QWebEngineView *,ArticleBridge *,QObject * =nullptr)`, `~PrintPipeline()`, `State {Idle,Generating,LoadingPdf,Ready,Previewing,Printing,Failed,Cancelled}`, `state() const noexcept -> State`, `snapshot(QPageLayout const &)`, `preview(QPrinter *)`, `paint(QPrinter *) -> bool`, `exportPdf(QString const &,QPageLayout const &)`, `cancel() noexcept`; signals `ready(int pageCount)`, `failed(QString const &)`, `cancelled(QString const &)`. Consume `ArticleBridge::generation()`, generationChanged, QWebEngineView::printToPdf callback, QPdfDocument::load/render/status/pagePointSize and QPrinter PDF output.

- [ ] **Step 1: Write failing snapshot/render tests.** Fixed two-pages.html yields Ready/pageCount=2 before preview. Hash bytes/page count through preview close, printer page-size/orientation changes and a second paint; render dimensions must change while bytes/hash/pageCount stay fixed. Test immediate Ready/Error state as well as signal path, empty/invalid bytes through a private production loading helper, 30 s generation timeout, navigation/tab close cancellation while generating, Ready snapshot surviving later navigation, null image/begin/newPage failure, atomic export failure and no partial file. A temporary QPrinter::PdfFormat destination covers direct paint without physical hardware. With the fixed two-page ArticleWebView `renderer` and its ArticleBridge fixture:

```cpp
PrintPipeline pipeline(&renderer, &bridge);
QSignalSpy ready(&pipeline, &PrintPipeline::ready);
const QPageLayout layout(QPageSize(QPageSize::A4), QPageLayout::Portrait, QMarginsF());
pipeline.snapshot(layout);
QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 40000);
QCOMPARE(pipeline.state(), PrintPipeline::State::Ready);
QCOMPARE(ready.at(0).at(0).toInt(), 2);
QTemporaryDir output;
QVERIFY(output.isValid());
QPrinter printer;
printer.setOutputFormat(QPrinter::PdfFormat);
printer.setOutputFileName(output.filePath("painted.pdf"));
QVERIFY(pipeline.paint(&printer));
QVERIFY(QFileInfo(printer.outputFileName()).size() > 0);
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_print_pipeline/tst_print_pipeline.pro -o tests/tst_print_pipeline/Makefile && make -C tests/tst_print_pipeline -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_print_pipeline/tst_print_pipeline`; require missing pipeline or readiness/buffer-lifetime assertion.
- [ ] **Step 3: Implement §§2.8/3.5/4.10.** Capture selected view at action invocation, own immutable snapshot bytes/QBuffer/QPdfDocument, connect status before load and inspect current status for immediate completion. Only Ready with pageCount>0 opens preview/enables print. paint uses aspect-fit images at printable resolution, centers each page, tests begin/newPage/end and retains no QPrinter after the action/dialog. Preview geometry continues using config supplied by its owner. Destroy dialog then document then buffer/bytes. Export uses a separate operation ID and QSaveFile with direct fallback disabled. Duplicate operations/cancel invalidate only matching callbacks; retain frozen Ready bytes across later navigation until replacement/tab close.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated suites. Instrument actual WebEngine snapshot invocations in the test's connections/operation observation: page-layout changes during paint must cause zero new asynchronous snapshots. Print/pdf error emits visible failed once; no callback accesses bytes after close.
- [ ] **Step 5: Commit.** `git add printpipeline.hh printpipeline.cc tests/tst_print_pipeline && git commit -m "feat(print): render frozen PDF snapshots through a Ready barrier"`.

## Task 29: Integrate save, print, DevTools and explicit renderer teardown

**Implementer tier:** Capable
**Lane:** webview
**Depends on:** Task 25, Task 28, Task 26
**Ownership:**
- `articleview.hh`
- `articleview.cc`
- `articleview.ui`
- `articlewebview.hh`
- `articlewebview.cc`
- `articleinspector.hh`
- `articleinspector.cc`
- `articlebridge.hh`
- `articlebridge.cc`
- `tests/tst_article_load/tst_article_load.pro`
- `tests/tst_article_load/tst_article_load.cc`
- `tests/tst_article_bridge/tst_article_bridge.cc`

**Files:** Complete ArticleView workflow wrappers and QWebEngine inspector, update promoted view base metadata and all remaining owned Qt-version/include/API anchors.

**Interfaces:** Produce exact ArticleView `saveArticle(QString const &,bool)`, `printPreview(QPrinter *)`, `print(QPrinter *)`, `exportPdf(QString const &,QPageLayout const &)`, async `toHtml(std::function<void(QString const &)>)`, `captureState(std::function<void(ArticleState const &)>)`, cached `getArticlesList() -> QStringList`, `getCurrentArticle() -> QString`, `getActiveArticleId() -> QString`, `hasSound() -> bool`, and `articlesChanged(QStringList const &)`, `soundAvailabilityChanged(bool)`. Produce ArticleInspector : QWidget with `(QWebEnginePage *,Config::Class *,QWidget * =nullptr)`, destructor and existing beforeClosed slot, retaining its geometry/config behavior. Consume exact SaveArticle/PrintPipeline APIs and profile-parent controller attachment for DevTools pages.

- [ ] **Step 1: Add failing integration/lifetime tests.** Real ArticleView wrappers save/export/preview/direct-paint through the selected view, status signals reflect pipeline outcomes, DevTools page shares inspected profile and geometry restores. Open inspector then close tab with JS/save/print pending; assert inspector closes before inspected page, page before bridge, profile still alive, producer callbacks inert. Cached articles/audio return empty/false after reset, emit only actual changes and feed action enablement after readiness. Record the real page/bridge destruction signals without retaining the objects, then reset the unique ArticleView owner:

```cpp
QPointer<QWebEnginePage> pageGuard(renderer->page());
QPointer<ArticleBridge> bridgeGuard(bridge);
auto destruction = std::make_shared<QStringList>();
QObject::connect(pageGuard.data(), &QObject::destroyed, [destruction] { destruction->append("page"); });
QObject::connect(bridgeGuard.data(), &QObject::destroyed, [destruction] { destruction->append("bridge"); });
viewOwner.reset();
QVERIFY(pageGuard.isNull());
QVERIFY(bridgeGuard.isNull());
QCOMPARE(*destruction, QStringList({QStringLiteral("page"), QStringLiteral("bridge")}));
```

Here renderer/bridge are the real view's borrowed children captured before reset; additionally observe the inspector-first/profile-last order in the same test.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_article_load/tst_article_load.pro -o tests/tst_article_load/Makefile && make -C tests/tst_article_load -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 120 tests/tst_article_load/tst_article_load`; run article bridge suite and require wrapper/inspector/lifetime failure.
- [ ] **Step 3: Connect completed pipelines.** ArticleView owns one of each, routes old product actions through their async completion/error state, preserves existing public operations not explicitly changed, and prevents duplicate actions while busy. Inspector owns its WebEngineView/page on inspected profile, attaches that page through the owning controller before load, and uses setDevToolsPage. Before view teardown stop frame timers, cancel generation work/pipelines, close DevTools, explicitly destroy page, then bridge. `articleview.ui` retains ArticleWebView promoted name/header but changes base to QWebEngineView. Remove ArticleViewJsProxy, attachToJavaScript, WebKit FindFlags, sync toHtml, WebKit-only helpers, legacy branch/comment spellings; keep no missing-method stubs.
- [ ] **Step 4: Confirm green.** Repeat load/bridge/frame suites, save/print suites and accumulated tests. Archive-build complete renderer sources and generated articleview.ui, and assert every one of the 33 former synchronous sites has a named completed async workflow/cached-state replacement.
- [ ] **Step 5: Commit.** `git add articleview.hh articleview.cc articleview.ui articlewebview.hh articlewebview.cc articleinspector.hh articleinspector.cc articlebridge.hh articlebridge.cc tests/tst_article_load tests/tst_article_bridge/tst_article_bridge.cc && git commit -m "feat(article): integrate save print DevTools and teardown"`.

## Task 30: Implement deadline-bounded PortalRequest early-response correlation

**Implementer tier:** Frontier
**Lane:** desktop
**Depends on:** Task 13, Task 15
**Ownership:**
- `portalhotkeys.hh`
- `portalhotkeys.cc`
- `tests/tst_portal_hotkeys/tst_portal_hotkeys.pro`
- `tests/tst_portal_hotkeys/tst_portal_hotkeys.cc`

**Files:** Create portal module and test target; implement PortalRequest first. PortalBackend pure declarations may be included, but the request suite links no unfinished backend definition.

**Interfaces:** `PortalRequest(QDBusConnection const &,quint64 generation,QObject * =nullptr)`, `~PortalRequest()`, `start(QDBusMessage,QString const & handleToken)`, `cancel() noexcept`; signals `response(quint64,QString const & requestPath,uint,QVariantMap const &)`, `failed(quint64,QString const &)`. Consume private FakePortal, service-owner identity, predicted/returned request paths and `Request.Close`; returned session_handle is a string validated as object path.

- [ ] **Step 1: Write failing private-bus race tests.** FakePortal sends Response before method result on predicted and alternate paths; assert completion once only for this request/current owner. Unrelated, duplicate, stale-owner and invalid-type messages cannot complete it. Delay actual path beyond cancel and require Close immediately when known. No Response after Close is legal. Test no method result, no Response, malformed method return, method error and service owner change under one 5 s wall deadline. Test a retired CreateSession response's late session must still be closed without public activation/completion. Against the private bus and FakePortal's `early-alternate-response` scenario, invoke the real request:

```cpp
portal.setScenario(QStringLiteral("early-alternate-response"));
const QString token = QStringLiteral("gd_test_request");
QDBusMessage message = QDBusMessage::createMethodCall(
    "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
    "org.freedesktop.portal.GlobalShortcuts", "CreateSession");
message << QVariantMap{{"handle_token", token}, {"session_handle_token", QStringLiteral("gd_test_session")}};
PortalRequest request(bus, 7);
QSignalSpy responses(&request, &PortalRequest::response);
QSignalSpy failures(&request, &PortalRequest::failed);
request.start(message, token);
QTRY_COMPARE_WITH_TIMEOUT(responses.count() + failures.count(), 1, 5500);
QCOMPARE(failures.count(), 0);
QCOMPARE(responses.at(0).at(0).toULongLong(), qulonglong(7));
QCOMPARE(responses.at(0).at(2).toUInt(), uint(0));
```

Use a separate elapsed-deadline test for the no-method/no-response cases and verify returned-path cleanup through FakePortal::calls().
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_portal_hotkeys/tst_portal_hotkeys.pro -o tests/tst_portal_hotkeys/Makefile && make -C tests/tst_portal_hotkeys -j$(nproc) && xvfb-run -a timeout 120 tests/tst_portal_hotkeys/tst_portal_hotkeys`; require missing request implementation or correlated-response assertion.
- [ ] **Step 3: Implement §4.12 request state machine.** Subscribe empty-path Response match before asyncCall, retain current owner and bounded candidate envelopes with message path for only the request's lifetime. Generate valid unique handle token, derive predicted path from bus unique name, and reconcile actual path from method result before accepting buffered Response once. Deadline starts before invocation and covers both stages. Cancel/timeout retires public completions and closes known handles once; retain only bounded cleanup observers/envelopes needed to reconcile a late method path or old CreateSession session and close it immediately. Owner change invalidates handles/envelopes and stops owner-specific cleanup. No nested event loop or blocking pendingCall wait.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated suites. Assert stopwatch deadline includes delayed method return, no completion wait after Close, no private service name on desktop bus and all daemon/request processes cleanly end on shutdown.
- [ ] **Step 5: Commit.** `git add portalhotkeys.hh portalhotkeys.cc tests/tst_portal_hotkeys && git commit -m "feat(portal): correlate early responses within request deadlines"`.

## Task 31: Validate portal sessions and historical shortcut display before binding

**Implementer tier:** Frontier
**Lane:** desktop
**Depends on:** Task 30
**Ownership:**
- `portalhotkeys.hh`
- `portalhotkeys.cc`
- `hotkeybackend.hh`
- `tests/tst_portal_hotkeys/tst_portal_hotkeys.pro`
- `tests/tst_portal_hotkeys/tst_portal_hotkeys.cc`

**Files:** Implement session/version/list/revision portions of PortalBackend and explicit QtDBus structure/list marshalling.

**Interfaces:** `PortalBackend(QDBusConnection const &,QObject * =nullptr)`, destructor, `isSupported() const noexcept -> bool`, `state() const noexcept -> HotkeyBackend::State`, `bind(QVector<HotkeyBinding> const &)`, `unbind()`, `configure(QString const &)`. Register wire `a(sa{sv})` as a structure `{QString id; QVariantMap properties;}` and list, CreateSession result `session_handle` string, ListShortcuts input `QDBusObjectPath` plus `{handle_token}`. Consume Task 30 PortalRequest and config/UI display policy independent of current bindable IDs.

- [ ] **Step 1: Add failing schema and historical-display tests.** Test version uint v1/v2/absent/wrong type, CreateSession invalid string/object path, malformed list tuple/map/property types and duplicate IDs → Unavailable. Historical startup case: List reports `clipboard-lookup`, current display policy exposes only `main-window`, only `main-window` bindable; both early and after Bind the historical entry is ignored without session teardown, activation or comparison with Bind's requested subset. Simulate slow List returning after newer Bind/ShortcutsChanged and assert display revision prevents overwrite. Bind need not wait for List response. Define production wire types/operators in portalhotkeys.hh/.cc with ADL-visible operators, not a QVariantList substitute:

```cpp
namespace PortalWire {
struct Shortcut { QString id; QVariantMap properties; };
using Shortcuts = QList<Shortcut>;
QDBusArgument &operator<<(QDBusArgument &out, Shortcut const &value) {
  out.beginStructure(); out << value.id << value.properties; out.endStructure(); return out;
}
QDBusArgument const &operator>>(QDBusArgument const &in, Shortcut &value) {
  in.beginStructure(); in >> value.id >> value.properties; in.endStructure(); return in;
}
}
Q_DECLARE_METATYPE(PortalWire::Shortcut)
Q_DECLARE_METATYPE(PortalWire::Shortcuts)
```

Put declarations/metatypes in the header and operator bodies in the .cc; register both before portal calls and assert the registered list signature:

```cpp
qDBusRegisterMetaType<PortalWire::Shortcut>();
qDBusRegisterMetaType<PortalWire::Shortcuts>();
QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<PortalWire::Shortcuts>())),
         QByteArray("a(sa{sv})"));
```
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_portal_hotkeys/tst_portal_hotkeys.pro -o tests/tst_portal_hotkeys/Makefile && make -C tests/tst_portal_hotkeys -j$(nproc) && xvfb-run -a timeout 120 tests/tst_portal_hotkeys/tst_portal_hotkeys`; require historical-display/schema assertion failure.
- [ ] **Step 3: Implement the complete linkable session/display baseline.** Subscribe Activated/ShortcutsChanged/Session.Closed/owner loss before create. Validate all list entries' wire shape/duplicate IDs before projecting historical IDs through independent display policy/revision; listings never populate active bindings or delay Bind. Implement every declared PortalBackend method now: one real Bind(session,a(sa{sv}),"",{handle_token}) with bindAttempted set first, unique requested-subset validation, exact Bound/Partial/Denied/Unavailable/Closed transitions, generation edits, guarded activation and asynchronous Close; configure sends the actual v2 method with no Request lifecycle or gives the specified v1/unavailable explanation. Close late old sessions. Tests exercise real successful Bind and loss paths; Task 32 adds the adversarial matrix and factory. No unfinished virtual/public method or fabricated binding result is committed.
- [ ] **Step 4: Confirm green.** Repeat Step 2 and accumulated suites. All PortalBackend methods must link and execute against the private daemon. Pin actual one-Bind wire transcript, a valid activation only after current Bind, historical listings before/after Bind, no listing delay and once-only session/request teardown. This task completes independently before Task 32 begins.
- [ ] **Step 5: Commit.** `git add portalhotkeys.hh portalhotkeys.cc hotkeybackend.hh tests/tst_portal_hotkeys && git commit -m "feat(portal): validate session and historical listing policy"`.

## Task 32: Complete one-bind portal states, configuration and backend selection

**Implementer tier:** Frontier
**Lane:** desktop
**Depends on:** Task 31
**Ownership:**
- `portalhotkeys.hh`
- `portalhotkeys.cc`
- `hotkeybackend.hh`
- `hotkeybackend.cc`
- `tests/tst_portal_hotkeys/tst_portal_hotkeys.pro`
- `tests/tst_portal_hotkeys/tst_portal_hotkeys.cc`
- `tests/tst_hotkeys/tst_hotkeys.pro`
- `tests/tst_hotkeys/tst_hotkeys.cc`

**Files:** Add backend factory implementation and complete portal subset/configure/edit/loss states.

**Interfaces:** `createHotkeyBackend(QObject *) -> std::unique_ptr<HotkeyBackend>`; exact PortalBackend methods/HotkeyBackend signals from Tasks 11/31. Bind wire `(session, [(id,{description,preferred_trigger}),...], "", {handle_token})`; Configure v2 wire `(session,"",{activation_token when nonempty})` has an async method result and no Request.Response lifecycle. Consume preferredTrigger's exact XDG mapping, not localized trigger_description.

- [ ] **Step 1: Add failing full fake-portal matrix.** Require unique requested returned subset only, unknown/duplicate/wrong-type Bind IDs fail protocol, proper subset Partial, all Bound, empty or codes1/2 Denied. Test requested trigger differing from reported description, empty description→Assigned by portal, all unsupported→no empty manufactured binding, two edits/new generations, late old session Close, service loss/Session.Closed/timeout/shutdown. Only bound current-session IDs activate and activation_token forwards unchanged; Deactivated never looks up. Configure on v2 existing session never sends Bind/Request and errors update reason. v1 configure disabled. Factory selection table xcb/wayland/other, no Wayland X11 fallback/no clipboard polling fallback. Under actual offscreen QPA, pin the concrete unsupported factory result:

```cpp
QVERIFY(!QGuiApplication::platformName().startsWith(QStringLiteral("xcb")));
QVERIFY(!QGuiApplication::platformName().startsWith(QStringLiteral("wayland")));
QObject owner;
auto backend = createHotkeyBackend(&owner);
QVERIFY(backend);
QCOMPARE(backend->parent(), &owner);
QVERIFY(!backend->isSupported());
QCOMPARE(backend->state(), HotkeyBackend::State::Unsupported);
backend.reset();
```

Run this as named `unsupportedFactory` with `QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_hotkeys/tst_hotkeys unsupportedFactory` in Step 4. Actual xcb and private-bus Wayland selection probes remain required independently.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_portal_hotkeys/tst_portal_hotkeys.pro -o tests/tst_portal_hotkeys/Makefile && make -C tests/tst_portal_hotkeys -j$(nproc) && xvfb-run -a timeout 120 tests/tst_portal_hotkeys/tst_portal_hotkeys`; run tst_hotkeys under its 60 s bound and require subset/factory/configure assertions.
- [ ] **Step 3: Implement exact backend state transitions.** Set bindAttempted before the single invocation, use parent_window="", send only handle_token in Bind options, register wire types explicitly and drive active subset solely from Bind/valid Changed. Increment generation before retiring edit requests/session; timeout/loss/error is idempotent, asynchronous Close and no retry/restore token. Explicit user edit starts fresh. Factory returns concrete disabled backend with reason for unsupported capability, never null/exception; MainWindow will reset its unique owner before QObject child teardown. Avoid persisting/logging activation tokens or guessing activation from timestamps.
- [ ] **Step 4: Confirm green.** Repeat both suites and accumulated tests. FakePortal's one-Bind/session invariant must remain active across every scenario; List historical-filter cases from Task 31 still pass before/after binding. Test actual native Wayland factory selection in Task 37, separately from the table.
- [ ] **Step 5: Commit.** `git add portalhotkeys.hh portalhotkeys.cc hotkeybackend.hh hotkeybackend.cc tests/tst_portal_hotkeys tests/tst_hotkeys && git commit -m "feat(hotkeys): complete portal binding states and backend selection"`.

## Task 33: Integrate desktop consumers, startup readiness and shutdown in one owner

**Implementer tier:** Capable
**Lane:** desktop
**Depends on:** Task 32, Task 29, Task 26, Task 10
**Ownership:**
- `main.cc`
- `mainwindow.hh`
- `mainwindow.cc`
- `scanpopup.hh`
- `scanpopup.cc`
- `preferences.hh`
- `preferences.cc`
- `preferences.ui`
- `sources.cc`
- `sources.ui`
- `config.hh`
- `config.cc`
- `tests/tst_hotkeys/tst_hotkeys.pro`
- `tests/tst_hotkeys/tst_hotkeys.cc`
- `tests/tst_platformwindow/tst_platformwindow.cc`

**Files:** Modify all owned constructor/capability/config/Qt6 anchors. Extract existing MainWindow save collector into SaveArticle usage; no duplicate collector stays in MainWindow. FTS signal consumers and Qt-version wrappers switch here atomically before this consumer closure is selected by root qmake.

**Interfaces:** MainWindow owns `ArticleProfileController(Config::Class &,ArticleNetworkAccessManager &,QWidget *)`, unique HotkeyBackend and shared profile. Inject `QWebEngineProfile &` before ArticleNetworkAccessManager in ArticleView and ScanPopup, retaining every other existing parameter/default. Consume ArticleView cache signals, FtsHighlightSpec const &, SaveArticle/PrintPipeline wrappers, HotkeyBackend states/IDs/tokens, PlatformWindow helpers and `ArticleProfileController::{applyPreferences(),clearForExit(),exitCachesReady(bool)}`. Startup calls `registerArticleSchemes()` before app, no forced QT_QPA_PLATFORM=xcb; backend derives from actual QPA.

- [ ] **Step 1: Add failing capability/config integration tests and compile assertions.** Assert all four UI capability groups, per-ID descriptions/reasons, raw unsupported sequence survives unedited OK/cancel, recorder edit clears raw marker, no_ffmpeg stored backend preserved and internal controls hidden, plugins unavailable text retains value, HttpCachingProxy Chromium note and inline/print parity notes. Verify constructor signatures/typed FTS connections by compiling full MainWindow/popup/preferences closure. Add shutdown/cache readiness tests through the production controller and view closure; widget action capture cannot switch to a newly selected tab during print.
- [ ] **Step 2: Confirm red.** `qmake6 tests/tst_hotkeys/tst_hotkeys.pro -o tests/tst_hotkeys/Makefile && make -C tests/tst_hotkeys -j$(nproc) && QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys`; then `qmake6 tests/tests.pro GD_COMPILE_CLOSURE=1 'GD_COMPILE_SOURCES=main.cc mainwindow.cc scanpopup.cc preferences.cc sources.cc' 'GD_COMPILE_FORMS=mainwindow.ui scanpopup.ui preferences.ui sources.ui' -o /tmp/gd-qt6-desktop.Makefile && make -f /tmp/gd-qt6-desktop.Makefile -j$(nproc)`; require old constructors/WebKit/FTS or UI capability mismatch.
- [ ] **Step 3: Connect all application consumers.** Register schemes before QHotkeyApplication; respect inherited QPA. Preserve this pre-application order in main.cc:

```cpp
registerArticleSchemes();
QHotkeyApplication app(argc, argv);
```

After app initialize locale/config/logging, apply proxy before profile, construct resource manager/profile controller/backend, then tabs/popup. First controlled blank load+bridge handshake gates startup readiness; emit exactly one `startup-ready platform=<actual-QPA> backend=<actual-backend>` diagnostic only after that barrier. Initialization error/renderer failure before it shows dialog/stderr and exits nonzero. Do not invent a Qt initialization-success API. Log actual platform/backend, never token/password. Single-combination portal activation passes token to applyActivation before action; clipboard-change scanning/existing lookup remain separate, hover/focus restoration disabled on Wayland. Preferences receive state/bindings updates, configure action only v2/live session and changed valid settings restart one generation. Save/print delegate to selected view, cache-change signals update actions. Quit disables new work, cancels pipelines/hotkeys, joins asynchronous 5 s cache barrier, closes popup/tabs/DevTools, destroys pages before bridge/profile and resources/dictionaries last; repeated quit/session-manager requests join without hiding a running process. Convert all owned version/regex/time/view/container/widget APIs.
- [ ] **Step 4: Confirm green.** Repeat the named tests/archive command and accumulated suites. Validate no_ffmpeg config variant. Use profile/bridge tests for readiness/destruction rather than test-including MainWindow; root link and true startup are mandatory immediately in Task 34/35. Source audit confirms no forced-xcb block, HAVE_X11 runtime inference, global host union, direct ffmpeg private factory access or direct async print in preview paint.
- [ ] **Step 5: Commit.** `git add main.cc mainwindow.hh mainwindow.cc scanpopup.hh scanpopup.cc preferences.hh preferences.cc preferences.ui sources.cc sources.ui config.hh config.cc tests/tst_hotkeys tests/tst_platformwindow/tst_platformwindow.cc && git commit -m "feat(desktop): integrate Qt6 startup preferences and shutdown"`.

## Task 34: Switch the root application to the completed Qt6 source closure

**Implementer tier:** Advanced
**Lane:** build
**Depends on:** Task 7, Task 10, Task 17, Task 26, Task 28, Task 29, Task 33
**Ownership:**
- `goldendict.pro`
- `qt4x5.hh`
- `README.md`

**Files:** Modify root qmake/README, delete compatibility header after all consumers are gone. Test feature/source modules but never add tests to application SOURCES.

**Interfaces:** Final qmake contract: TEMPLATE=app/TARGET=goldendict/VERSION=1.5.1+git, CONFIG c++17 exceptions rtti stl and the exact module set in Global Constraints. Consume every new production `.hh/.cc` from spec §2.2, all rewritten view/UI/resource modules and tests aggregate. Produce make targets `tests-makefile`, `tests-build`, `tests-run`, `check` with §6.4 recipes and make-time `$(GD_TEST_WRAPPER)` separately on build/run.

- [ ] **Step 1: Write failing source/module/gate checks.** Assert required Qt6 modules/sources/resources, absence of old modules/options/branch tokens/compat header, wildcard unconditional, retained ffmpegaudioplayer.hh engine guards and default libeb. Assert root generated tests-build/tests-run both contain make-time wrapper and tests-run ends in `-f Makefile.tests check`. Test optional KDE header+library detection (both required), glibc no forced -liconv, zim lzma/libzstd selection and feature-option retention. The current qmake6 failure is the expected red for root cutover.
- [ ] **Step 2: Confirm red.** Run `qmake6` and record the old `Unknown module(s) ... webkitwidgets x11extras` failure; run the inline source/Makefile assertions before implementation, requiring missing Qt6 source/module/wrapper contract. No previously green Qt6 closure may fail.
- [ ] **Step 3: Implement §7.4 root and verbatim-structure §6.4 gate.** Set `QT = core gui widgets xml network sql printsupport help svg webenginecore webenginewidgets webchannel dbus pdf pdfwidgets`; remove every qmake Qt-major/minor branch and lrelease-qt4 fallback. Add all spec production sources/headers/JS, remove multimedia+qt4x5 entries/files, keep install/translations/version generation. Retain every named existing option/macro, ffmpeg+ao only enabled, default libeb/TIFF/hunspell/OpenCC as specified, X11/Xtst for compiled RECORD. Support KWINDOWSYSTEM_INCLUDEPATH/LIBDIR and default `/usr/include/KF6/KWindowSystem/kwindowsystem.h`; add include/lib/define only when both probes succeed. zim uses lzma and pkg-config libzstd else -lzstd once. README explains modules/runtime, build/test commands, removed switches and all §9 limitations. Use this exact root recipe structure:

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
- [ ] **Step 4: Confirm green application and tests.** `qmake6 CONFIG+=verbose_build_output && make -j$(nproc) > /tmp/gd-qt6-application-link.log 2>&1` must link the real application; keep that nonempty actual linker output for Task 35 and show the log on failure. Run `QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make check` before the wrapper script exists, then inspect Makefile recipes/Qt6-only libraries and `test ! -e qt4x5.hh`. Test KDE helper absence and configured explicit include/lib discovery. Any remaining compiler defect goes to its owning lane before this task can complete; no allowlist bypass or undefined-symbol/stub workaround.
- [ ] **Step 5: Commit.** `git add goldendict.pro qt4x5.hh README.md && git commit -m "build(qt6): switch application modules and wire root test gate"`.

## Task 35: Prove the four root-gate probes and Qt6-only application audit

**Implementer tier:** Capable
**Lane:** tests
**Depends on:** Task 15, Task 34
**Ownership:**
- `scripts/test-wrapper.sh`
- `scripts/verify-test-gate.sh`
- `scripts/check-qt6-only.sh`
- `tests/tests.pro`
- `tests/testsupport.pri`
- `tests/support/fixtures.hh`
- `tests/support/fixtures.cc`

**Files:** Create three executable POSIX scripts; finalize aggregate/support exact-18 enforcement. Do not mutate active production code for probes.

**Interfaces:** Wrapper logs escaped argv when GD_TEST_WRAPPER_LOG supplied, then `exec xvfb-run -a timeout 900 "$@"`. Gate verifier makes independent disposable copies/fresh logs and requires four expected probe outcomes plus an unmodified clean wrapped root check. Qt6 audit accepts `<Makefile> <application-link-log>`, prints exact searches and rejects missing/empty actual application link. Consumes all 18 per-feature projects and compile-time feature guards.

- [ ] **Step 1: Write failing script/aggregate assertions.** Verify exact sorted set: tst_encoding,tst_linkrewrite,tst_regex,tst_article_schemes,tst_article_load,tst_article_stream,tst_external_content,tst_article_bridge,tst_website_frames,tst_article_history,tst_print_pipeline,tst_save_article,tst_portal_hotkeys,tst_hotkeys,tst_htmlescape,tst_langcoder,tst_utf8_wstring,tst_platformwindow. Missing suite/prerequisite is a failure, never discovery omission/QSKIP. Script contract tests pass commands with spaces/literal shell metacharacters and nonzero exit status. A disposable probe removes only tests-run wrapper prefix; run observation must fail even if suite/build pass. Reconfigure the Core regex target and require `GD_SAVE_COLLECTOR_AVAILABLE` in its generated build plus executed saveCollector assertions against savearticle.hh; no test implementation copy. Before implementation, this wrapper contract must fail; afterward it must preserve both argv boundaries and exit37:

```sh
gd_wrapper_probe_log=$(mktemp)
trap 'rm -f "$gd_wrapper_probe_log"' 0
set +e
GD_TEST_WRAPPER_LOG="$gd_wrapper_probe_log" scripts/test-wrapper.sh sh -c \
  'test "$#" -eq 2 || exit 91; test "$1" = "two words" || exit 92; test "$2" = "literal;meta" || exit 93; exit 37' \
  sh 'two words' 'literal;meta'
gd_wrapper_probe_status=$?
test "$gd_wrapper_probe_status" -eq 37 || exit 1
test -s "$gd_wrapper_probe_log" || exit 1
```

Implement the specified escaped single-line optional logging before the wrapper's exact tail:

```sh
exec xvfb-run -a timeout 900 "$@"
```
- [ ] **Step 2: Confirm red.** Run `scripts/verify-test-gate.sh` and `scripts/check-qt6-only.sh Makefile /tmp/gd-qt6-application-link.log`; record missing-script/contract failure. Use a disposable aggregate copy with one required target removed and assert its qmake/check cannot silently pass.
- [ ] **Step 3: Implement §§6.4–6.5 exactly.** Each disposable probe regenerates root Makefile with GD_TEST_WRAPPER unset. Forced binary failure requires tst_utf8_wstring intentional assertion; forced env failure requires requiredEnvironment assertion; clean wrapper observation requires separate argv `-f Makefile.tests check`; negative-control copy removes only tests-run prefix and must show build marker but no run marker. Print command/exit/marker result, fail unexpected outcomes and clean copies via trap. Wrapper remains foreground/exec, supports absent optional log, fails an explicitly unusable log path. Audit exact specified extensions/exclusions, separately scan the four compiled vendor inputs and inspect actual app link for Qt5/Compat/Multimedia/WebKit/X11Extras. Reject version branching but retain independent OS/feature guards. Add strict final suite-name validation while keeping test-only archive mode separate from default aggregate. RequiredEnvironment checks real renderer+JS/bus/fixture readiness.
- [ ] **Step 4: Confirm green and required variants.** Run `qmake6 CONFIG+=verbose_build_output && make clean && make -j$(nproc) > /tmp/gd-qt6-application-link.log 2>&1` to capture a fresh actual application link, then `QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER="$PWD/scripts/test-wrapper.sh" GD_TEST_WRAPPER_LOG=/tmp/gd-qt6-wrapper.log make check`, `scripts/verify-test-gate.sh`, `scripts/check-qt6-only.sh Makefile /tmp/gd-qt6-application-link.log`. Build separate disposable/shadow no_ffmpeg_player tree with `qmake6 CONFIG+='no_ffmpeg_player verbose_build_output' && make -j$(nproc)` and wrapped `make check`; assert no internal player, external path, verbatim backend text. Run separately configured retained-option builds: `zim_support` (lzma/zstd), `no_epwing_support no_extra_tiff_handler no_chinese_conversion_support`, old_hunspell only with the corresponding supported header API; preserve unavailable optional-dependency outcomes explicitly. Confirm default libeb and optional KDE-absent build. No live acceptance can start until all four gate probes and required default/no_ffmpeg results pass.
- [ ] **Step 5: Commit.** `git add scripts/test-wrapper.sh scripts/verify-test-gate.sh scripts/check-qt6-only.sh tests/tests.pro tests/testsupport.pri tests/support/fixtures.hh tests/support/fixtures.cc && git commit -m "test(gate): prove wrapper propagation and Qt6-only linkage"`.

## Task 36: Document verified parity limits and the acceptance evidence format

**Implementer tier:** Standard
**Lane:** tests
**Depends on:** Task 35
**Ownership:**
- `docs/wayland-limitations.md`

**Files:** Create the one new limitations/acceptance document specified by §2.12. Read README for consistency but only the build lane edits it.

**Interfaces:** Consumes §9 approved parity limits and actual default/no_ffmpeg/four-probe/link audit output. Produces a document with concrete capability statements and automated evidence fields `command`, `exit`, `platform`, `backend`, `observation`, `evidence-path`; live evidence is populated in Task 37 rather than represented as a passed unchecked box.

- [ ] **Step 1: Write failing documentation assertions.** A Python assertion against this path must require the six §9 topic groups: iframe header denial/inline base+port; one-combination portal/preserved two-key/clipboard separation; unparented dialog and best-effort activation; frozen PDF/no HTML reflow; removed multimedia/plugins with remaining audio; HttpCachingProxy/QNAM independence. Require native Wayland vs explicit xcb evidence to be separate and forbid a claim of compositor-guaranteed focus. Require actual four-probe exits/markers in recorded automated evidence. Run this complete topic/evidence checker during red and green:

```bash
python3 - <<'PY'
from pathlib import Path
text = Path('docs/wayland-limitations.md').read_text().lower()
for terms in [('iframe', 'port'), ('portal', 'clipboard', 'two-key'),
              ('dialog', 'activation'), ('pdf', 'reflow'),
              ('multimedia', 'audio'), ('httpcachingproxy', 'qnam')]:
    assert all(term in text for term in terms), terms
for field in ['command:', 'exit:', 'platform:', 'backend:', 'observation:', 'evidence-path:']:
    assert field in text, field
assert 'wayland' in text and 'xcb' in text
assert 'four' in text and 'probe' in text
assert 'compositor-guaranteed focus' not in text
PY
```
- [ ] **Step 2: Confirm red.** `python3 -c 'from pathlib import Path; p=Path("docs/wayland-limitations.md"); assert p.is_file(), "missing Qt6 parity and acceptance document"'` must fail before creation. A present document lacking any required topic/evidence fails the full assertions.
- [ ] **Step 3: Write the approved behavior and observed automated results.** State Wayland hover/focus-restore limits, empty parent_window, unsupported sequences remain stored until edited, portal-reported labels only, Clipboard-mode behavior and known framing fallback. Include exact build/test/audit command results from Task 35; do not include secrets or invent desktop observations. Physical printer/KDE optional tests may be unavailable but a missing required live target gate is visibly incomplete. Link source spec and README, keeping implementation jargon out of normal product instructions.
- [ ] **Step 4: Confirm green.** Re-run Python topic/evidence assertions and `scripts/check-qt6-only.sh Makefile /tmp/gd-qt6-application-link.log`; run required whole-app/aggregate checks from Global Constraints. Documentation must agree with actual UI/proxy/print behavior and distinguish automated from manual evidence.
- [ ] **Step 5: Commit.** `git add docs/wayland-limitations.md && git commit -m "docs(qt6): describe verified platform and snapshot limits"`.

## Task 37: Run native Wayland and explicit X11 live parity acceptance

**Implementer tier:** Advanced
**Lane:** tests
**Depends on:** Task 36
**Ownership:**
- `docs/wayland-limitations.md`

**Files:** Append actual live observations to the owned acceptance document; no production edits in this task. Use representative fixture dictionaries, not a user's data mutation.

**Interfaces:** Consumes a verified Task 35 gate, platform/backend startup diagnostics, portal-reported trigger descriptions, runtime article/lookup/popup/audio operations and §6.6 parity checklist. Produces native target-machine evidence, explicit xcb RECORD evidence and truthful optional hardware availability entries.

- [ ] **Step 1: Establish the failing acceptance assertion before the live run.** Native success requires platform starts wayland, PortalBackend, a portal-reported Bound/Partial active single ID, rendered article, lookup, popup raised by that shortcut and actual audio. Explicit xcb requires platform starts xcb, X11RecordBackend and real one/two-key defaults/double Ctrl+C. A startup-ready headless result alone cannot satisfy either. Python-check the document for those evidence fields and expect absent observations to fail. The live-gate marker check is:

```bash
python3 - <<'PY'
from pathlib import Path
text = Path('docs/wayland-limitations.md').read_text()
assert 'Wayland live result: PASS' in text, 'native Wayland observations missing'
assert 'X11 RECORD live result: PASS' in text, 'actual X11 RECORD observations missing'
for field in ['command:', 'exit:', 'platform:', 'backend:', 'observation:', 'evidence-path:']:
    assert field in text.lower(), field
PY
```

Markers are written only from the observed checklist in Step 3; the checks do not substitute for interacting with the target desktop.
- [ ] **Step 2: Confirm red.** Run `python3 -c 'from pathlib import Path; s=Path("docs/wayland-limitations.md").read_text(); assert "Wayland live result: PASS" in s and "X11 RECORD live result: PASS" in s, "required live target gates have not been observed"'`. Do not convert missing target access into a false pass or optional skip.
- [ ] **Step 3: Execute §6.6 acceptance on the target session.** Run `QT_QPA_PLATFORM=wayland ./goldendict` and verify the actual logged QPA/backend and blank+bridge startup handshake. Choose a supported single trigger, observe portal-reported description and invoke real popup/lookup/audio. Check absent/denied/unsupported-multikey capability explanations while clipboard-change and explicit lookup still work. Test representative StarDict/BGL/MDict/DSL/Zim/website article+picture, group/muted, main/frame word/selection, middle tab, FTS/find/zoom/context/download, complete/HTML saves, preview/direct PDF/DevTools/audio/external, outgoing history, preferences, remote form/fetch real origin and inline fallback after frame denial. Then run `QT_QPA_PLATFORM=xcb ./goldendict` and prove real RECORD one/two-key defaults separately. Record physical printer and optional KDE-helper observations or unavailable, without substituting for native required gates. Every result has actual command/time/platform/observable evidence; never write PASS from expectation.
- [ ] **Step 4: Confirm green.** Repeat Step 2 only after observing all required evidence, review full parity checklist with no unaccounted mandatory row and run the global application/tests checks. If target session/portal/audio/xcb cannot be exercised, report the exact incomplete live gate and keep this task incomplete; no extra approval rule is created and implementation tasks already authorized continue independently where possible.
- [ ] **Step 5: Commit.** `git add docs/wayland-limitations.md && git commit -m "test(acceptance): record native Wayland and X11 parity"`.

## Task 38: Run and record the complete migration gate

**Implementer tier:** Capable
**Lane:** tests
**Depends on:** Task 37, Task 34, Task 28, Task 29, Task 33
**Ownership:**
- `docs/wayland-limitations.md`

**Files:** Update the acceptance evidence section only; production fixes route to their lane owners and require a fresh complete gate before this task completes.

**Interfaces:** Consumes every completed lane, all 18 suites, approved gate scripts, actual linker command and prior live acceptance. Produces a final recorded gate with no unsupported completion claim and no required skip.

- [ ] **Step 1: Add final-evidence assertions before running.** Require fresh default build exit0, wrapped root check exit0 with executed-run marker, all four gate probes/clean pass, Qt6 audit exit0 with actual nonempty linker command, no_ffmpeg variant/config checks, startup timeout124 plus actual controlled-document/bridge readiness and no crash/renderer error. Required Wayland/xcb observations from Task 37 must exist separately. The previous task's build log is insufficient evidence of this final state.
- [ ] **Step 2: Confirm red.** `python3 -c 'from pathlib import Path; s=Path("docs/wayland-limitations.md").read_text(); assert "Final migration gate: PASS" in s, "fresh complete migration gate not recorded"'` must fail before the final run. Do not prewrite a PASS marker to satisfy this assertion.
- [ ] **Step 3: Execute the complete gate in order and inspect every exit.** Run the commands below; capture build output while preserving make's status (use a shell with pipefail if tee is used), retain the actual application link line separately and never overwrite it with a tests-only or no-op build log.

```bash
qmake6 CONFIG+=verbose_build_output
make clean
make -j$(nproc) > /tmp/gd-qt6-final-build.log 2>&1
QT_QPA_PLATFORM=xcb GD_TEST_WRAPPER="$PWD/scripts/test-wrapper.sh" GD_TEST_WRAPPER_LOG=/tmp/gd-qt6-final-wrapper.log make check
scripts/verify-test-gate.sh
scripts/check-qt6-only.sh Makefile /tmp/gd-qt6-final-build.log
QT_QPA_PLATFORM=xcb timeout 30 xvfb-run -a ./goldendict > /tmp/gd-qt6-final-startup.log 2>&1
```

The final smoke's expected exit is 124; inspect its actual blank-document+bridge readiness diagnostics and absence of crash/initialization errors before accepting it. Use a command driver that handles 124 explicitly rather than `set -e` suppressing evidence. Rebuild/run the separate no_ffmpeg_player variant/config tests from Task 35 against this final source revision, then inspect suite count, requiredEnvironment/no-QSKIP policy, vendor audit, index version invariants and git diff for only authorized migration files.
- [ ] **Step 4: Confirm green from fresh evidence.** Require `rg -q '^startup-ready platform=xcb backend=' /tmp/gd-qt6-final-startup.log`; its absence fails even with timeout124. Record command/exit and artifact paths, 18 suites passing, four exact probe outcomes, default/variant results, startup124+ready and live evidence references. Only then append `Final migration gate: PASS`. Re-run Steps 1–2 assertions plus `scripts/check-qt6-only.sh Makefile /tmp/gd-qt6-final-build.log` after recording. No code-changing commit may follow without repeating the gate.
- [ ] **Step 5: Commit.** `git add docs/wayland-limitations.md && git commit -m "test(qt6): record the complete migration acceptance gate"`.


