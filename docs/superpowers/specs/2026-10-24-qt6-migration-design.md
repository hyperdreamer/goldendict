# GoldenDict Qt6 Migration — Design

- **Date:** 2026-10-24
- **Repository:** `hyperdreamer/goldendict` @ `b4f3fcdc` (branch `master`)
- **Topic slug:** `qt6-migration`
- **Target environment:** Arch Linux, KDE Plasma 6 on **Wayland**, Qt **6.12**, GCC 16
- **Revision:** 6 (addresses reviews 1–5; all blockers and majors resolved)

## 1. Background

This fork builds against Qt 4 or Qt 5 and uses QtWebKit for article rendering. The
target desktop environment is Plasma 6 on Wayland, where the Qt 5 build works only
through XWayland and QtWebKit is dead upstream. Qt 6 has no WebKit at all, and Qt 6
removed or relocated a large part of the API surface this codebase uses.

Measured starting state (repeatable commands, not estimates):

| Fact | Evidence |
| --- | --- |
| `qmake6` fails before compiling | `Project ERROR: Unknown module(s) in QT: webkitwidgets x11extras` |
| Legacy API surface, non-UI | After fixing only `mutex.hh`: **147 compile errors in 21 files** (`QChar(int)` ×66, `QString::midRef` ×37, `QRegExp` ×9, `QString::split/count/filter(QRegExp)` ×10, `QDateTime::toTime_t` ×4, `QTextStream::setCodec`, `QLayout::setMargin`, `QStringRef`, `findChild` static assert, `QList` iterator mismatch) |
| Qt-version conditionals | 406 `QT_VERSION` references in 55 files; 27 `IS_QT_5` uses; 13 Qt4-only branches |
| Regex surface | **248 `QRegExp` occurrences in 34 files**, including the save-article resource collector (`mainwindow.cc:3599`) |
| Legacy encodings | 8 files with `QTextCodec`; web pages, EPWING (ISO8859-1/GB2312/EUC-JP), and a locale codec `gdCodecForLocale` consumed by `indexedzip.cc:136`, `epwing_book.cc:423`, `gddebug.cc:135` |
| Article view | `articleview.cc` is 93 KB; 33 synchronous `evaluateJavaScript` calls; 12 `QWebElement` uses; `hitTestContent`, `frameAt`, `QWebPage::FindFlags` incl. `HighlightAllOccurrences` |
| C++→JS callback | `addToJavaScriptWindowObject("articleview", jsProxy)` (`articleview.cc:1175`); generated HTML calls `articleview.onJsActiveArticleChanged(...)` from an inline `<script>` (`article_maker.cc:638`) |
| Article documents | `gdlookup://localhost?word=…&group=…&…` is the article document URL; `article_netmgr.cc:392` serves it as `text/html` from `ArticleMaker` |
| Picture documents | `gdpicture://…` is **also a document**: `article_netmgr.cc:520-525` returns `text/html` from `ArticleMaker::makePicturePage`, whose body is an `<img src="bres://…">` |
| Internal schemes | `gdlookup`, `bres`, `gdau`, `gdvideo`, `gico` (`article_netmgr.cc:455`), `gdpicture` (`:520`), `qrcx` remapping (`:258`) |
| Website frames | `website.cc:494-515` emits `<iframe id="gdexpandframe-…" src="<remote url>" style="display:none">`; `articleview.cc:539-560` enumerates child frames, measures `contentsSize()`, sets height, reveals it and injects click/context listeners |
| Website inline mode | The `inside_iframe == false` path fetches the page through QNAM (`website.cc:233`) and rewrites only `a|link|img|script`; its `root` drops any non-default port and stylesheet links are redirected to `bres` (`website.cc:227`, `:285`) |
| X11 coupling | `QX11Info` in 5 files; `hotkeywrapper.cc` uses `XRecordEnableContext` with an Xlib `Display*`; `mainwindow.cc` X focus/`XSendEvent` |
| Startup forces XWayland | `main.cc:261-268`: when `XDG_SESSION_TYPE=wayland`, `setenv("QT_QPA_PLATFORM","xcb",1)` |
| Screen geometry | `QDesktopWidget` in 4 files |
| Audio | ffmpeg is the default internal backend (`"FFmpeg+libao"`); QtMultimedia backend is `"Qt Multimedia"` (`config.hh:216`, `:221`) |
| Proxy auth | `MainWindow::proxyAuthentication(const QNetworkProxy &, QAuthenticator *)` (`mainwindow.cc:4703-4752`) stores credentials into the **proxy** preference fields and calls `QNetworkProxy::setApplicationProxy` |
| Cache | MiB preference → bytes, `<= 0` means "no cache at all" (`mainwindow.cc:1347-1350`); installed on `articleNetMgr` (`:1366`); cleared at exit (`:1108`) |
| Print preview | `QPrintPreviewDialog::paintRequested` connected directly to a **synchronous** `ArticleView::print(QPrinter*)` (`mainwindow.cc:3568-3600`) |
| Tests | **None.** No `tests/` directory, no unit harness; `.travis.yml` is Qt5-era |
| Build system | qmake only (`.pro`), no CMake |

Environment facts that shape the design (checked on this machine):

- Qt 6.12 with `webenginewidgets`, `webenginecore`, `webchannel`, `dbus`, `pdf`,
  `pdfwidgets`, `5compat`, `multimedia`, `help`, `svg`, `svgwidgets`, `tools`.
- `xdg-desktop-portal 1.22.1` + `xdg-desktop-portal-kde 6.8.1`;
  `org.freedesktop.portal.GlobalShortcuts` present with `ConfigureShortcuts`
  (interface version 2).
- `kwindowsystem 6.31` with headers exposing
  `KWindowSystem::setCurrentXdgActivationToken(const QString&)`.
- `hunspell 1.7.5`, `libopencc 1.4.1`, `libeb 4.4.3`, `libzim 9.8.1`, ffmpeg/ao/vorbis,
  `zstd 1.5.7` **with** `/usr/lib/pkgconfig/libzstd.pc`.
- `Xvfb`/`xvfb-run` present.
- Qt 6.12 `QStringDecoder` accepts named legacy codecs here (`Shift-JIS`, `Big5`,
  `Big5-HKSCS`, `GB18030`, `GB2312`, `EUC-JP`, `windows-1251`, `ISO-8859-11`) because
  this build has ICU enabled — a build-feature-dependent capability, not a Qt
  guarantee (§6.2, decision K5).

## 2. Decisions

| ID | Decision | Rationale |
| --- | --- | --- |
| **B** | **Qt6-only, full modernization.** No Qt4/Qt5 compatibility layer, no `qt6-5compat` dependency. | Deprecated-but-working shims would leave the riskiest code (parser regexes, legacy encodings) frozen in a module that can disappear at any Qt release. |
| **B1** | **Full article-view parity** on QtWebEngine. | The scan popup is the product's signature feature. |
| **W2** | **Global shortcuts via XDG Desktop Portal `GlobalShortcuts`**, with the X11 RECORD backend retained for X11 sessions and automatic selection. | The portal is the only sanctioned Wayland path for global shortcuts. |
| **P1** | **Keep qmake**, rewritten for Qt 6, Linux first-class. | One build definition; a CMake port is a follow-up project. |
| **T1** | **Add a Qt Test suite** plus build, WebEngine fixture, fake-portal and live gates. | Without it, converting 248 regexes and 8 codec sites is unverifiable. |
| **K4** | **Drop the QtMultimedia internal audio backend**; keep ffmpeg and external playback. | `qt6-multimedia` adds no unique value; ffmpeg is the required default. |
| **K5** | **One encoding implementation on iconv**, not `QStringDecoder`. | `QStringDecoder` covers these charsets only with an ICU-enabled Qt build; iconv is already mandatory. One implementation, one behaviour, byte-exact fixtures. |
| **K6** | **Website frames keep their real remote origin and are instrumented frame-locally** through `QWebEngineFrame::runJavaScript`, instead of being re-served through an application origin. | The embedder may execute script in any frame, including cross-origin ones, so no origin surgery is needed; re-serving would change the site's own URL base, cookies, CORS and script semantics. |
| **K7** | **Printing goes through a PDF snapshot**: `printToPdf` → `QPdfDocument` → synchronous `paintRequested` painting. | `QPrintPreviewDialog` paints synchronously while WebEngine printing is asynchronous (§6.5.9). |

## 3. Goals

1. `qmake6 && make -j$(nproc)` produces a working `goldendict` binary with no
   `qt6-5compat` and no Qt5/Qt4 branches.
2. The application starts **natively on Wayland** (`platformName() == "wayland"`),
   renders articles, performs lookups, plays audio, and runs its scan popup.
3. Global shortcuts work on Wayland through the portal and on X11 through RECORD.
4. The article view preserves the WebKit-era behaviour set: article and picture
   documents, resource/audio/icon URL serving, word-under-cursor translation (main
   frame and website frames), FTS highlight, find bar, zoom, context menu, save
   article, print and print preview, DevTools.
5. Migration-sensitive modules are covered by tests that fail on regression.
6. No silent feature loss: anything that cannot be preserved is disabled with a
   visible explanation and documented.

## 4. Non-goals

- CMake migration; Windows/macOS build validation; parallel Qt5 support.
- Re-architecting dictionary engines, indexing, or FTS indexing.
- Wayland hover-translation via portal `InputCapture`.
- Config format changes beyond the additive migration in §9.
- Visual restyling.

## 5. Architecture overview

```text
  ┌──────────────────────────────────────────────────────────────────┐
  │ 5. Desktop integration: hotkeys (X11 RECORD | portal), windows,  │
  │    screen geometry, activation tokens                             │
  ├──────────────────────────────────────────────────────────────────┤
  │ 4. Article view: QWebEngineView + scheme handlers + QWebChannel   │
  │    + frame-local instrumentation                                  │
  ├──────────────────────────────────────────────────────────────────┤
  │ 3. Platform-neutral services: Encoding (iconv), LinkRewrite,      │
  │    regex migration, article history, print pipeline, audio        │
  ├──────────────────────────────────────────────────────────────────┤
  │ 2. Dictionary engines (unchanged algorithms, migrated API calls)  │
  ├──────────────────────────────────────────────────────────────────┤
  │ 1. Build: goldendict.pro for Qt 6, tests/ subprojects, docs       │
  └──────────────────────────────────────────────────────────────────┘
```

### 5.1 New and rewritten modules

| Module | Kind | Responsibility |
| --- | --- | --- |
| `encoding.hh/.cc` | new | Named charset conversion, HTML/BOM detection, locale encoding; replaces all `QTextCodec` use |
| `linkrewrite.hh/.cc` | new | The `gdlookup://`, `bres://`, `gico://`, `gdau://` HTML rewriting rules currently duplicated in `stardict.cc`, `zim.cc`, `dictdfiles.cc`, `dictserver.cc`, `website.cc` |
| `articlewebview.hh/.cc` | rewritten | `ArticleWebView : QWebEngineView` + `ArticleWebPage : QWebEnginePage` |
| `articlebridge.hh/.cc` | new | Bidirectional page bridge: synchronous JS facade, `QWebChannel` object named `articleview`, generation-stamped operations |
| `articleurls.hh/.cc` | new | The single URL dispatch matrix (§6.5.1) and its producers |
| `articleschemes.hh/.cc` | new | `QWebEngineUrlScheme` registration, all internal scheme handlers, request interceptor |
| `websiteframes.hh/.cc` | new | Frame discovery, frame-local measurement/instrumentation, coordinate conversion |
| `articlehistory.hh/.cc` | new | Application-owned per-history-entry article state |
| `printpipeline.hh/.cc` | new | PDF snapshot → `QPdfDocument` → preview/printer rendering |
| `hotkeybackend.hh` | new | Abstract hotkey backend interface (`X11RecordBackend`, `PortalBackend`) |
| `portalhotkeys.cc/hh` | new | D-Bus client for `org.freedesktop.portal.GlobalShortcuts` |
| `platformwindow.hh/.cc` | new | Screen geometry, window-flag/activation policy, xdg activation token |
| `qt4x5.hh` | **deleted** | Qt4 branches and `IS_QT_5` helpers |
| `article_netmgr.cc/hh` | rewritten | Keeps `Dictionary::WebMultimediaDownload` and the resource-fetch helpers; loses the QNAM subresource override and `QWebSecurityOrigin` accounting |
| `multimediaaudioplayer.*` | **deleted** | Decision K4 |

## 6. Component design

### 6.1 Build configuration (`goldendict.pro`)

```qmake
TEMPLATE = app
TARGET = goldendict
VERSION = 1.5.1+git
CONFIG += c++17 exceptions rtti stl

QT += core gui widgets xml network sql printsupport help svg \
      webenginecore webenginewidgets webchannel dbus pdf pdfwidgets
```

Rules:

- Remove every `QT_MAJOR_VERSION`/`QT_MINOR_VERSION` branch and the `webkit`,
  `webkitwidgets`, `x11extras`, `multimedia` modules.
- `CONFIG += c++17` is mandatory.
- Keep `zim_support`, `chinese_conversion_support`, `no_ffmpeg_player`,
  `old_hunspell`, `no_extra_tiff_handler`; remove `no_qtmultimedia_player`.
- `zim_support` uses `PKGCONFIG += libzstd` when the `.pc` exists (it does here),
  otherwise falls back to `-lzstd`.
- `kwindowsystem` is optional: when available, define `HAVE_KWINDOWSYSTEM` and link it
  for activation-token support (§6.6.3).

**Root `make check` gate (must actually build and run the tests).** The root project
is `TEMPLATE = app`, so it supplies no subdirs recursion; the recipe below is explicit:

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

- `tests/tests.pro` is `TEMPLATE = subdirs`; every testcase project is
  `TEMPLATE = app`, `QT += testlib`, `CONFIG += testcase`, which makes
  `make -f Makefile.tests check` build each test and run it, propagating failures.
- `GD_TEST_WRAPPER` uses **make-time** expansion (`$(GD_TEST_WRAPPER)`, not
  `$$(...)`, which qmake would expand when generating the Makefile). It is therefore
  supplied at invocation time, not at qmake time:
  `GD_TEST_WRAPPER=scripts/test-wrapper.sh make check` runs the suite under a display
  and a wall-clock bound.
- `scripts/test-wrapper.sh` appends its argv to `$GD_TEST_WRAPPER_LOG` and then execs
  `xvfb-run -a timeout 900 "$@"`; the log therefore proves which invocations the
  wrapper actually received.
- **Required suites fail, they do not skip.** `QSKIP` does not fail a test binary
  (`qtestcase.h:42-59`), so every required suite starts with a `QVERIFY(envAvailable())`
  guard (WebEngine initialises, D-Bus session reachable, fixture servers bindable).
  A missing environment therefore produces a **failure**, and only genuinely optional
  cases use `QSKIP`.
- **Gate verification:** `scripts/verify-test-gate.sh` generates the root Makefile with
  `GD_TEST_WRAPPER` **unset**, then performs four probes on a copy of the tree, each
  asserting the root `make check` outcome:
  1. an intentionally failing testcase → non-zero (failure propagation);
  2. a required-environment test with the environment forced unavailable → non-zero
     (skip cannot mask a missing prerequisite);
  3. `GD_TEST_WRAPPER_LOG` must contain an entry whose command line includes
     `-f Makefile.tests check` → the **check** execution itself was wrapped, not just
     the build step;
  4. removing the wrapper from the `tests-run` recipe only → probe 3 must fail, while
     the build-generated marker still exists. This proves probe 3 cannot be satisfied by
     the wrapped build.
  The unmodified tree must then pass. The gate counts as wired only when all three
  mutations behave as stated.

### 6.2 `Encoding` — character conversion

Legacy dictionary and web content requires Shift-JIS, Big5, Big5-HKSCS, GB18030,
GB2312, EUC-JP, Windows-1251 and ISO-8859-x, plus a locale encoding for filenames.
Decision **K5** implements all of it on iconv, with one implementation and
byte-exact fixtures.

```cpp
// encoding.hh
class Encoding
{
public:
  DEF_EX( Ex, "Encoding error", std::exception )
  DEF_EX( exUnrepresentable, "Character not representable in target encoding", Ex )

  static std::unique_ptr< Encoding > byName( QString const & name );  // nullptr if unknown
  static Encoding utf8();                                            // always valid
  static Encoding forLocale();                                       // decode-only; never throws

  enum class DetectedBy { Header, Bom, Meta, XmlDeclaration, Fallback };

  static Encoding forHtml( QByteArray const & body,
                           QByteArray const & contentTypeHeader,
                           QString const & fallback = QStringLiteral( "UTF-8" ),
                           DetectedBy * detectedBy = nullptr );

  bool isValid() const;
  QString name() const;
  QString decode( QByteArray const & data ) const;    // never throws
  QByteArray encode( QString const & text ) const;    // throws exUnrepresentable
};
```

#### Decoder contract (exact)

1. Each `decode()` call opens a private `iconv_t` and closes it afterwards; instances
   are immutable, copyable, thread-safe. The mutable `Iconv` wrapper is **not** used.
2. Loop: convert into a growing output buffer.
   - `E2BIG`: grow the output buffer and retry **without** consuming input.
   - `EILSEQ` (invalid sequence at `in`): emit exactly one U+FFFD, advance `in` by one
     byte, `iconv(cd, nullptr, nullptr, ...)` to reset shift state, continue. Advancing
     by one byte guarantees termination for every charset used here.
   - `EINVAL` (incomplete sequence at end of input): emit one U+FFFD and stop.
   - End of input: flush shift state with `iconv(cd, nullptr, nullptr, &out, &outLeft)`
     and append the result; a flush error is treated as `EILSEQ` (one U+FFFD).
3. Multi-byte characters are never split across a replacement: one invalid sequence
   produces one U+FFFD regardless of the number of bytes skipped.
4. `decode()` never throws; an invalid or unknown encoding is `utf8()`.

#### Encoder contract (exact)

1. Mirrors the decoder: private descriptor, `E2BIG` grow-and-retry, state flush at the
   end (stateful encoders such as ISO-2022-JP are not used today but the rule is
   defined).
2. `EILSEQ` throws `exUnrepresentable` with the offending characters left unencoded —
   callers decide the user-visible outcome (§ per-caller policy below).
3. An incomplete UTF-16 surrogate pair in the input also throws `exUnrepresentable`.

#### Detection

`forHtml()` precedence, matching `QTextCodec::codecForHtml` plus the HTTP header the
caller may supply: explicit `Content-Type` charset → UTF BOM (UTF-8/16/32) →
`<meta charset=…>` → `<meta http-equiv="Content-Type" content="…charset=…">` → XML
declaration → `fallback`. `detectedBy` reports which rule won (used by tests).

#### Per-caller output policy (no shared "fallback")

| Caller | On `exUnrepresentable` |
| --- | --- |
| URL template substitution (`website.cc` `%25GD…%25`) | Leave the placeholder unreplaced, exactly as today when the codec is unavailable; log once |
| EPWING search buffer (`epwing_book.cc:1025-1035`) | Skip that codepage, try the next one (EUC-JP, then GB2312); report "no match"; **never** send UTF-8 escapes to libeb |
| EPWING heading/display (`epwing_book.cc:156-236`) | Decode only; decoding never throws |
| Locale filenames (`indexedzip.cc:136`, `epwing_book.cc:423`, `gddebug.cc:135`) | `Encoding::forLocale()` decode-only; invalid bytes become U+FFFD |

`gdCodecForLocale()` returns `QTextCodec*` today and is replaced by
`Encoding::forLocale()`, whose charset comes from `nl_langinfo(CODESET)` with a
`QLocale::system().name()` fallback; it is documented as decode-only.

Call sites converted: `website.cc` (`codecForHtml` at `:140`, decode at `:219`, URL
substitutions at `:458-492`), `epwing_book.cc`, `indexedzip.cc`, `gddebug.cc`, plus any
remaining `QTextCodec` reference.

### 6.3 `LinkRewrite` — one owner for the article link rules

Four engines repeat near-identical regexes that rewrite dictionary HTML into
`gdlookup://` / `bres://` / `gico://` / `gdau://` form, with subtly different patterns
(`dictdfiles.cc` and `dictserver.cc` even carry both a `QRegularExpression` and a
`QRegExp` version of the same rule).

```cpp
namespace LinkRewrite
{
QString applyGdLookup( QString const & html );
QString applyBres( QString const & html, QString const & dictId );
QString applyGico( QString const & html, QString const & dictId );
QString applyGdau( QString const & html, QString const & dictId );
}
```

The exact patterns move here verbatim and are then converted once, with tests pinning
the previous behaviour.

### 6.4 Regex migration rulebook (248 sites, 34 files)

Every rule below was verified by compiling and running against the installed Qt 6.12
(`QRegExp` from Qt5Compat, `QRegularExpression` from QtCore):

| Difference | Measured behaviour | Required conversion rule |
| --- | --- | --- |
| `.` and newlines | `QRegExp("a.b").indexIn("a\nb") == 0` (matches); `QRegularExpression("a.b").match("a\nb")` has no match | Add `DotMatchesEverythingOption` for every pattern that scans HTML or multi-line text |
| Unicode classes | `QRegExp("\\w+")` matches `привет`; `QRegularExpression("\\w+")` does not; with `UseUnicodePropertiesOption` it matches all 6 characters. `QRegExp("\\d+")` matches U+0660, the plain QRegularExpression does not, the option makes it match | Add `UseUnicodePropertiesOption` wherever non-ASCII words/digits must match |
| Anchoring | `QRegExp("abc").indexIn("xabc") == 1`; `exactMatch` matches whole strings | `indexIn` → `match()`; `exactMatch` → `anchoredPattern()` or an explicit full-length check |
| Empty pattern | `QRegExp("").indexIn("abc")` matches at offset 0 with zero length; `QRegularExpression("")` matches at every position | Keep explicit `isEmpty()` guards |
| Minimal matching | `setMinimal(true)` makes `\d+` match one digit; `InvertedGreedinessOption` reproduces it | Prefer lazy quantifiers; use the option only for test-pinned mechanical conversions |
| Wildcard syntax | `QRegExp` wildcard `*.txt` matches `a.txt`; `wildcardToRegularExpression("*.txt")` yields `\A(?:(?s:[^/]*\.txt))\z` | Use `wildcardToRegularExpression()` and pin wildcard cases |
| `RegExp2` syntax | `website.cc:311` selects `QRegExp::RegExp2` | Review per site; RegExp2 differences do not map 1:1 onto PCRE defaults |
| String API | `QString::split/count/filter(QRegExp)` do not exist; `split/count(QRegularExpression)` and `QStringList::filter(QRegularExpression)` do and work (`filter("^ab")` → `abc,abd`, `count("\\d+")` on `a1b22` → `3`, split → `a|b|c`) | Use the `QRegularExpression` overloads; do not hand-roll loops |
| Captures | `capturedRef()`/`cap(n)` are legacy spellings | `capturedView()` for views, `captured(n)` for owning strings; `\1` replacement back-references still work |
| Case sensitivity | `QRegExp` is case-sensitive by default | Keep defaults; map `setCaseSensitivity` to `CaseInsensitiveOption` |

Precedent to reuse: `articleview.cc:2591` already builds `QRegularExpression` with
`DotMatchesEverythingOption | UseUnicodePropertiesOption | InvertedGreedinessOption`.

### 6.5 Article view on QtWebEngine

#### 6.5.1 URL dispatch matrix (replaces the QNAM override)

| Scheme | Kind | Produced by | Served as |
| --- | --- | --- | --- |
| `gdlookup://localhost?word=…&group=…&…` | **document** (main frame or tab) | `ArticleView` document loads; `ArticleMaker` links | `text/html` from `ArticleMaker::makeDefinitionFor` / `makeEmptyPage` |
| `gdpicture://<dictId>/<path>` | **document** (HTML wrapper) | picture links in article HTML (`articleview.cc:1212`) | `text/html` from `ArticleMaker::makePicturePage`, whose `<img src>` is rewritten to `bres://` |
| `gdlookup://localhost/…` (link click) | user action | dictionary HTML | converted into an `ArticleView` lookup; the engine navigation is refused |
| `bres://<dictId>/<path>` | resource | `LinkRewrite::applyBres`, picture wrapper | bytes with MIME from the resource name |
| `gdau://<dictId>/<name>` | resource | `LinkRewrite::applyGdau` | audio bytes |
| `gico://<dictId>/<name>` | resource | `LinkRewrite::applyGico` | image bytes |
| `gdvideo://<dictId>/<name>` | resource | dictionary HTML | video bytes |
| `qrcx://…` | resource | resource remapping (`article_netmgr.cc:258`) | `qrc:` bytes |
| `http(s)://…`, `file://…` | engine request | web dictionaries, remote images/frames | engine default, subject to §6.5.7 |

Two schemes are **documents** (`gdlookup`, `gdpicture`); the rest are resources. MIME
type is decided by the scheme's kind, not by file extension alone.

#### 6.5.2 Scheme registration

```cpp
// main(), BEFORE the QApplication instance is constructed
static void registerArticleSchemes()
{
  for ( char const * name : { "gdlookup", "gdpicture", "bres", "gdau",
                              "gico", "gdvideo", "qrcx" } )
  {
    QWebEngineUrlScheme scheme( name );
    // Verified: Syntax::Host registers; Syntax::HostAndPort with the default port -1
    // is silently rejected (schemeByName() returns an empty name).
    scheme.setSyntax( QWebEngineUrlScheme::Syntax::Host );
    scheme.setFlags( QWebEngineUrlScheme::LocalAccessAllowed
                   | QWebEngineUrlScheme::LocalScheme
                   | QWebEngineUrlScheme::CorsEnabled
                   | QWebEngineUrlScheme::SecureScheme );
    QWebEngineUrlScheme::registerScheme( scheme );
  }
}
```

Rules: portless `scheme://dictionary-id/path` URLs only; registration before
`QApplication` and before any profile; `tst_article_schemes` asserts every
`schemeByName(name)` returns the registered name, that percent-encoded hosts/paths
survive `QUrl`, and that the flags allow local access. `CorsEnabled` is *not* treated as
an access grant (§6.5.7).

#### 6.5.3 Serving pipeline, ownership, and cancellation

`ArticleSchemeHandler::requestStarted(QWebEngineUrlRequestJob * job)`:

1. Parse the URL through `ArticleUrls` (scheme kind → document vs resource → target).
2. Create the backing `Dictionary::DataRequest` and take shared ownership:
   `m_requests[job] = sptr<DataRequest>(req)`.
3. Device ownership: the handler creates the reply device **owned by the job**
   (`device->setParent(job)`), writes the payload on completion, and calls
   `job->reply(mimeType, device)` (`qwebengineurlrequestjob.h:48`).
   For known-large media it instead installs `ArticleStreamDevice`, a `QIODevice` that
   follows the existing `ArticleResourceReply` contract (`article_netmgr.cc:596-630`).
   The rule is **independent of producer completion**:
   - `readData(maxSize)`: if the unread backlog is positive, copy and return
     `min(maxSize, backlog)` — even when the producer has already finished, so a
     finished-but-undrained reply still delivers its bytes;
   - return `0` only when the backlog is empty and the request is unfinished
     ("wait for more"), plus zero when `maxSize == 0`;
   - return `-1` only when the request is finished and the backlog is drained — the
     terminal state for a sequential reply device;
   - `isSequential()` is `true`; `bytesAvailable()` reports the unread backlog and
     becomes `0` exactly when `atEnd()` becomes `true` (i.e. only after the terminal
     `-1` condition is reachable);
   - `readyRead()` is emitted on every appended chunk and once more on completion, so a
     consumer that already read everything still gets woken for the final `-1`;
   - cancellation/destruction releases the `DataRequest` and makes further reads
     return `-1`.
   `tst_article_stream` drives the ordering that matters: the final chunk is appended
   **and** `finished` is raised *before* the consumer drains that chunk. It then asserts
   the full payload comes out, that `0` was returned only while unfinished-and-empty,
   that exactly one `-1` terminates the stream, and that the job completes exactly
   once.
4. Cancellation: `connect(job, &QObject::destroyed, ...)` → `req->cancel()` (the current
   code calls `cancel()` in `ArticleResourceAdapter`'s destructor, `article_netmgr.cc:567`),
   then disconnect and erase the `m_requests` entry. Job destruction also destroys the
   device, so the streaming device must tolerate being deleted mid-request.
5. Failure: unresolvable resource → `job->fail(QWebEngineUrlRequestJob::UrlNotFound)`;
   a valid request with empty content → successful empty reply.
6. `gdlookup` documents are produced exactly as `article_netmgr.cc:392-452` does today
   (`blank=1`, `word`, `punctuation_suffix`, `group`, `dictionaries`, `muted`,
   `contexts`, `ignore_diacritics`); `gdpicture` documents call `makePicturePage` with
   the `bres://` image URL.

`tst_article_load` renders a fixture definition end to end, asserts the headword
appears, asserts a `bres` image resolves, and asserts a `gdpicture` document renders its
wrapper with the embedded image.

#### 6.5.4 Two-way JavaScript bridge

The generated HTML calls `articleview.onJsActiveArticleChanged(...)` from an inline
script, and `qwebchannel.js` publishes registered objects only after an asynchronous
`init` round trip (`/usr/share/qt6/webchannel/qwebchannel.js:161-172`). A synchronous
facade is therefore mandatory.

Installation, per document load, before any page script:

```js
// 1) injected at DocumentCreation, MainWorld — runs before every page script
(function () {
  const q = (window.__gdPending = []);
  window.articleview = { onJsActiveArticleChanged(id) { q.push(id); } };
  window.__gdDocumentGeneration = <GENERATION>;
})();
// 2) qwebchannel.js, then:
new QWebChannel(qt.webChannelTransport, function (channel) {
  const real = channel.objects.articleview;       // C++ ArticleBridge
  const pending = window.__gdPending || [];
  window.articleview = real;                      // swap in the real object
  for (const id of pending) real.onJsActiveArticleChanged(id, window.__gdDocumentGeneration);
  window.__gdPending = null;
  if (window.gdOnBridgeReady) window.gdOnBridgeReady();
});
```

C++ contract:

- `<GENERATION>` is substituted per load; the script is re-inserted on every navigation
  (`page()->scripts()`), so a page can never report a previous document's generation.
- `ArticleBridge::onJsActiveArticleChanged(QString id, int generation)` drops events
  whose generation differs from the current one — inbound and outbound guards are
  symmetric.
- Outbound operations are serialised through one queue and stamped with the generation;
  a completion whose generation is stale is dropped without invoking its callback and
  without blocking the queue. `reset()` increments the generation, clears the queue and
  marks outstanding requests abandoned. Chromium may still execute an abandoned script;
  only results are ignored.
- Lifetime: `ArticleBridge` is parented to the view, destroyed after the page; callbacks
  hold a `QPointer` guard.
- `article_maker.cc` call sites are unchanged.

`tst_article_bridge` covers: parser-time notification before the channel is ready (queued
and delivered), notification after a dictionary switch, old-document notification
arriving after navigation (rejected), navigation during an in-flight outbound script,
and page destruction with a queued callback.

#### 6.5.5 DOM algorithms moved into JavaScript

| Former WebKit API | Replacement |
| --- | --- |
| `frame->evaluateJavaScript("…")` returning a value | `ArticleBridge::run(script, cb)` |
| `QWebElement::geometry()`/`render()` + `QImage` word lookup | `gdWordAtPoint(x, y)` in the main frame and, for iframes, in each frame via `QWebEngineFrame::runJavaScript` with frame-local coordinates |
| selection save/restore for FTS | `gdSaveSelection()`, `gdRestoreSelection()` |
| `FindFlags` + `HighlightAllOccurrences` | `gdHighlightAll(words, matchCase)`, `gdClearHighlights()`, `gdScrollToHit(i)` |
| `frameAt()` / `QWebHitTestResult` | `QWebEngineView::lastContextMenuRequest()` plus one JS call for the enclosing element's text |
| `QWebFrame::contentsSize()` | `gdMeasureFrame()` executed in that frame; result applied to the iframe element from the main frame |

Frame-local word lookup (replacing `frameAt()` at `articleview.cc:2927`): the click
point is transformed from view coordinates to frame-local coordinates by the *frame
element's* bounding rectangle obtained from the main frame
(`gdFrameRect(name)`), then divided by `page()->zoomFactor()`; the word is produced by
running `gdWordAtPoint` **inside that frame**. Nested frames recurse through
`QWebEngineFrame::children()`, applying each frame's own offset.

#### 6.5.6 Website frames (`gdexpandframe-*`) — decision K6

A web dictionary with `inside_iframe` emits a hidden iframe pointing at the remote URL
(`website.cc:494-515`); the article view measures it, reveals it and injects click
handlers (`articleview.cc:539-560`). Under WebEngine the frame stays **cross-origin and
untouched**, and instrumentation is done frame-locally:

1. **Discovery:** `page()->mainFrame().children()`; a frame whose `name()` starts with
   `gdexpandframe-` is a website frame; the dictionary id is the suffix after
   `gdexpandframe-`.
2. **Instrumentation is installed by script injection, not once per frame.** The frame
   helper is a `QWebEngineScript` with `InjectionPoint::DocumentCreation`,
   `ScriptWorldId::MainWorld` and **`setRunsOnSubFrames(true)`**, re-inserted per
   document with the current generation. Chromium injects it into every frame
   document — same-origin, cross-origin, and **after every subsequent frame
   navigation** — which is exactly the reinstrumentation that a one-shot
   `frame.runJavaScript()` call cannot provide (`QWebEngineFrame` has no
   `loadFinished` signal). The helper:
   - installs the click/context listeners the old code injected (`gdLastUrlText`),
   - posts `{gdFrameEvent: "ready" | "click" | "context", name, text, docId}` to
     `window.parent` with a per-document `docId`,
   - installs `gdMeasureFrame()` and reports height changes.
   The main frame listens for `message` events, forwards them to the bridge together
   with the frame name and generation, and re-arms height measurement whenever a
   `ready` event arrives with a new `docId`.
3. **Measurement:** on every `ready`, and on a 250 ms timer, the main frame asks the
   frame for `gdMeasureFrame()` shape data and applies it with
   `document.getElementById(name).height = h; … .style.display = 'block'`; polling stops
   after the height is stable for three samples or 10 s, and restarts on the next
   `ready`. Because the frame is a different origin, all reporting goes through
   `postMessage`, never direct DOM access.
4. **Coordinates:** as in §6.5.5 — frame rect from the parent, zoom-divided, then
   frame-local hit testing inside the frame.
5. **Attribution:** the frame name carries the dictionary id, so a click maps to the
   right dictionary without guessing.
6. **X-Frame-Options limitation:** Chromium honours `X-Frame-Options`/CSP framing
   directives, which the old QNAM layer stripped (`article_netmgr.cc:118-120`). Sites
   that forbid framing therefore cannot be shown in iframe mode. This is a documented
   B1 parity limit with an existing escape hatch: the per-dictionary "inline" mode
   (`inside_iframe = false`) still fetches the page through QNAM and injects it, and the
   preferences page says so.
7. Iframe mode's inline request rewriting (`website.cc:225-300`) is **not** used for
   frames any more (the document loads from its real origin), so the port-dropping
   `root` construction (`website.cc:227`) no longer affects frames; it remains only for
   inline mode, where it is fixed to use `QUrl::resolved` (preserving port, query and
   base) and `QUrl`-based rewriting instead of the four-tag regex.

`tst_website_frames` runs two local HTTP servers on **non-default ports**: one
same-origin fixture and one cross-origin fixture with relative images, a stylesheet, a
form and a script-relative `fetch`, plus a second navigation inside the frame. Asserts:
height applied, frame visible, click text forwarded with the right dictionary id,
word-under-cursor inside the frame, and that relative resources resolve against the
frame's real origin and port.

#### 6.5.7 Origin and network policy

| Concern | Contract |
| --- | --- |
| Custom-scheme subresources | `bres`/`gico`/`gdau`/`gdvideo`/`qrcx` are loaded as subresources (`img`, `link`, `script`, media). `CorsEnabled` only permits CORS *to* those schemes; since no application JS issues `fetch`/`XHR` at them (audio is played through links/`AudioPlayerFactory`, not fetched), no CORS headers are required. If a future caller does fetch them, the handler must answer with `Access-Control-Allow-Origin` — noted, not assumed |
| Website-frame content | Loads from its real origin under Chromium's normal rules; the application does not mediate or rewrite it (K6) |
| External-content blocking | When `disallowContentFromOtherSites` is enabled, `ArticleRequestInterceptor::interceptRequest` decides **per request** with a rule that treats the initiating document as first-class (`info.firstPartyUrl()` and `info.initiator()`, `qwebengineurlrequestinfo.h:70-72`). Let `initiatorHost` be the host of `info.initiator()` when its scheme is `http`/`https`, otherwise the host of `info.firstPartyUrl()`. A request is allowed when **any** holds: (a) its host is same-site with `initiatorHost` — this is the case for a website frame loading its own images/CSS/fetch, whose initiator is the remote frame origin, not the `gdlookup` article; (b) its host is same-site with the first-party host; (c) its initiator uses an application scheme (`gdlookup`, `bres`, `gico`) **and** its host belongs to the dictionary set used to build *that* page. The per-page dictionary set and the policy snapshot are installed with `QWebEnginePage::setUrlRequestInterceptor` (`qwebenginepage.h:320`) so each page carries its own policy even though the profile is shared; a global union of enabled dictionaries is never used. An absent or opaque initiator falls back to (b) only. Default remains **allow** (`config.cc:255`). `tst_external_content` embeds dictionary A (unrelated domain) inside a real `gdlookup` article document with two dictionaries enabled and asserts: A's own image/CSS/`fetch` succeed, A's request to B's domain is blocked, the same page still loads its `bres` resources, and everything loads when the preference is off |
| Local files | `file://` URLs are not routed through scheme handlers; path fix-up (the old `article_netmgr.cc:338` logic) is applied by the **interceptor** using `QWebEngineUrlRequestInfo::redirect()` (`qwebengineurlrequestinfo.h:79`) |
| User agent | `QWebEngineProfile::setHttpUserAgent()` from the existing preference |
| Response-header filtering (incl. `X-Frame-Options`) | Only possible for content we fetch ourselves (dictionary resources, inline-mode website fetches); third-party subresources and framed sites are subject to their own headers. Documented limitation |
| Interceptor threading | Runs on the Chromium IO thread; reads an immutable policy snapshot (atomics + a copied host set), never widgets or live config |

#### 6.5.8 Profile, proxy, cache, and authentication

- **One shared named `QWebEngineProfile`** owned by `MainWindow` and used by every
  `ArticleView` (tabs and popup), constructed with an explicit storage name and cache
  path under `Config::getNetworkCacheDir()`. Pages are destroyed before the profile
  (§10).
- **Proxy:** Qt WebEngine reads `QNetworkProxy::applicationProxy()` and tracks changes,
  so the existing `QNetworkProxy::setApplicationProxy()` path is kept and no
  `--proxy-server` flag or application restart is needed. Mode mapping is explicit:
  `NoProxy` → `QNetworkProxy::NoProxy`, which genuinely bypasses system/environment
  proxies; system → the existing system-proxy query; `HttpProxy`/`Socks5Proxy` → the
  same proxy types; `HttpCachingProxy` → treated as an HTTP proxy by Chromium (semantic
  difference documented in the preferences UI). At startup, explicitly set
  `NoProxy` when the preference disables proxying, instead of leaving the default,
  which would fall back to system configuration.
- **Cache:** positive MiB → `setHttpCacheMaximumSize(mib << 20)`; non-positive →
  `setHttpCacheType(NoCache)` (zero is *automatic sizing*, not "disabled", per
  `qwebengineprofile.cpp:627`). Changing the preference at runtime updates the pixel
  size or switches to `NoCache`.
- **Clear on exit:** clears **both** stacks — the retained QNAM caches
  (`articleNetMgr.cache()->clear()`) and the profile's `clearHttpCache()`, waiting for
  `clearHttpCacheCompleted()`; shutdown is held (session-manager cancel or explicit
  quit sequence) until both complete, with a bounded 5 s timeout.
- **Authentication:** two separate adapters, because the existing handler
  (`mainwindow.cc:4703-4752`) edits proxy preference fields and
  `QNetworkProxy::setApplicationProxy`:
  - `ProxyAuthAdapter` wraps the existing proxy credential logic
    (`QWebEnginePage::proxyAuthenticationRequired`).
  - `OriginAuthAdapter` handles `QWebEnginePage::authenticationRequired` with a
    generic credential dialog, scoped per origin/realm, held only for the lifetime of
    the request; it never reads or writes proxy configuration.
- Retained `WebMultimediaDownload` (context-menu downloads) keeps using QNAM with the
  same application proxy; both stacks are configured from one preference set but
  resolve independently, which is stated in the preferences help text.

#### 6.5.9 Save, find, and print (decision K7)

| Operation | Implementation |
| --- | --- |
| Save article (`toHtml` at `articleview.cc:1751`, collector at `mainwindow.cc:3599-3695`) | `page()->toHtml(cb)` → existing resource filtering/collection rewritten with `QRegularExpression` → downloads complete → **atomic publication** (write to a temp directory, rename into place); cancellation token cancels every in-flight download and removes the temp directory; navigation or tab close during collection aborts with a message |
| Find bar (`articleview.cc:2428-2470`) | `findText(sub, flags, callback)`; results carry a monotonically increasing query id, stale results are ignored, `QWebEngineFindTextResult` drives "n of m"/not-found |
| FTS highlight | JS highlighter with its own match index (`gdHighlightAll`, `gdScrollToHit`) |
| Print preview (`mainwindow.cc:3568-3600`) | PDF snapshot first, using the installed APIs: `printToPdf(cb)` supplies `QByteArray` (`qwebengineview.h:93-96`); the bytes are copied into a `QBuffer` **owned by the print controller** and kept alive for the document's whole lifetime; `QPdfDocument::load(QIODevice*)` (`qpdfdocument.h:77`) is called on it, `status()` is observed until `Ready` (`Error` is reported to the user), `pageCount()` is then valid, and `paintRequested` paints synchronously with `QImage img = doc.render(page, size, options)` (`qpdfdocument.h:96`). Preview is opened only after `Ready`, so WebEngine's asynchronous printer is never on the paint path |
| Direct print | Same snapshot: render each page into the `QPrinter` inside the print action |
| Regeneration | Changing page size/orientation re-renders the same snapshot at the new metrics; the snapshot is not re-flowed (WebKit reflowed). Documented as a B1 parity note |
| PDF export | `printToPdf` (unchanged separate action) |
| Lifetime | The snapshot is owned by the print controller, which keeps it alive across preview and print, and is torn down with the tab; closing the tab during generation cancels and reports |

`tst_print_pipeline` covers: preview contains the expected page count before printing;
page-size change regenerates; cancel during generation leaves no partial file; tab close
during generation aborts cleanly. `tst_save_article` covers a normal save, a save
cancelled mid-download (no temp directory left), and a navigate-during-collection abort.

#### 6.5.10 Article state and history

WebKit stored `currentArticle`, `sx`, `sy` inside each history item
(`articleview.cc:866`, restored at `:580`). `QWebEngineHistoryItem` has no
application-data API, so state moves to `ArticleHistory`:

```cpp
struct ArticleState { QString currentArticle; double sx = 0; double sy = 0; bool restoreScroll = true; };

class ArticleHistory
{
public:
  void reset();                                            // only when history is cleared
  void commit( QUrl const & url, int itemIndex, NavigationKind );  // on loadFinished(true)
  bool stateFor( int itemIndex, ArticleState * out ) const;
  void setCurrent( int itemIndex, ArticleState const & );
  void truncateAfter( int itemIndex );
};

enum class NavigationKind { NewLookup, LinkClick, Back, Forward, Reload, Redirect };
```

Rules:

1. `reset()` is called only when the page history is genuinely cleared (new tab, blank
   page), never on an ordinary navigation — that would destroy the A→B→back state the
   history exists for.
2. `commit()` runs once per successful load (`loadFinished(true)`) and mirrors
   `QWebEngineHistory::count()` / **`currentItemIndex()`** (the installed accessor;
   `currentIndex()` does not exist). Redirects commit once, at the final URL. Fragment
   navigation commits no new entry and keeps scroll as-is.
3. `NavigationKind` is recorded by the caller that initiates the navigation, because the
   engine does not expose it on commit. `Reload` sets `restoreScroll = false` for that
   entry (preserving the current "reload returns to top" behaviour at
   `articleview.cc:1710`); `Back`/`Forward`/`LinkClick`/`NewLookup` restore when a
   recorded scroll exists.
4. Scroll restoration after layout growth: after `loadFinished`, a JS settle loop
   (`requestAnimationFrame` up to 10 frames, or 500 ms) restores `sx`/`sy` on the first
   frame whose `document.documentElement.scrollHeight` is stable, so late images do not
   clamp the restored offset.
5. Duplicate URLs are distinguished by `itemIndex`, never by URL.
6. Forward-history truncation calls `truncateAfter()`.
7. **Capture timing:** `setCurrent()` records the live article id and scroll offsets
   **before** every navigation that leaves the current document — before a new lookup,
   before back and before forward — at the same points the old code saved them
   (`articleview.cc:886`, `:1691`, `:1698`). Relying on the `commit()` of the *next*
   load would record the new document's state instead of the outgoing one. `commit()`
   then only creates/associates the entry; scroll updates during reading are debounced
   through `setCurrent()` on the current index.

Cached UI state: `getArticlesList`, `getCurrentArticle` and `hasSound`
(`articleview.cc:726`, `:741`, `:1721`) read `ArticleView`'s cached state, which is
updated from the same JS results that drive the UI, is scoped to the document
generation, and is exposed as `articlesChanged` / `soundAvailabilityChanged` signals
consumed by `mainwindow.cc:2041`, `:2088`. No nested event loop.

### 6.6 Desktop integration

#### 6.6.1 Hotkey backends

```cpp
struct HotkeyBinding { QString id, description, sequence; };

class HotkeyBackend : public QObject
{
  Q_OBJECT
public:
  virtual bool isSupported() const = 0;
  virtual void bind( QVector< HotkeyBinding > const & ) = 0;
  virtual void unbind() = 0;
signals:
  void activated( QString const & id, QString const & activationToken );
};

std::unique_ptr< HotkeyBackend > createHotkeyBackend( QObject * parent );
```

Selection happens once at startup, is logged, and is never silently retried:

1. **X11** (`platformName()` starts with `xcb`): `X11RecordBackend`. The Xlib handle comes
   from `nativeInterface<QNativeInterface::QX11Application>()->display()` (`Display*`;
   `connection()` returns `xcb_connection_t*` and is unusable for RECORD), the root
   window from `DefaultRootWindow(display)`; a null interface disables the backend with
   a warning. The same accessor replaces `QX11Info` in `keyboardstate.cc`,
   `mainwindow.cc` and `scanpopup.cc`.
2. **Wayland**: `PortalBackend`, unless the portal service is absent.

Hover-translation and focus-restore remain X11-only and are documented.

#### 6.6.2 Portal backend lifecycle

Interface facts that the implementation honours
(`/usr/share/dbus-1/interfaces/org.freedesktop.portal.GlobalShortcuts.xml`):

- `CreateSession(a{sv} options) → o request_handle`; the session path arrives later in
  the `Response` signal (`session_handle`, typed `s` for historical reasons).
- **An application can only attempt to bind shortcuts of a session once.**
- `BindShortcuts(o session, a(sa{sv}) shortcuts, s parent_window, a{sv} options)` —
  options only contain `handle_token`; the result is a **subset**, each entry carrying
  `trigger_description`. Empty and partial results are legal.
  Each input shortcut is `(id, {"description": s, "preferred_trigger": s})`, where
  `preferred_trigger` uses the XDG shortcuts specification format
  (`CTRL+ALT+g`), converted from the application's `Ctrl+Alt+G` notation by
  `HotkeySyntax`, which also enforces the single-combination restriction.
- `ListShortcuts(o session, a{sv})` reports current bindings.
- `Activated(o session, s shortcut_id, t timestamp, a{sv} options)` carries
  `options["activation_token"]`.
- `ConfigureShortcuts` exists only in interface version 2.
- `Request.Response` must be subscribed **before** the method call; `Request.Close`
  cancels and produces **no** `Response`.

State machine and ownership:

```text
UNSUPPORTED ──service present──▶ SESSION_CREATING ──Response(session_handle)──▶ SESSION_READY
                                 │ Request.Close / timeout / error ──▶ UNAVAILABLE
SESSION_READY ──BindShortcuts(once)──▶ BINDING ──Response(subset)──▶ BOUND | PARTIAL | DENIED
BOUND/PARTIAL ──edit──▶ Request.Close(outstanding) + Session.Close(old) ──▶ SESSION_CREATING
any ──ServiceUnregistered / session closed / error──▶ UNAVAILABLE
shutdown ──Session.Close──▶ closed
```

Rules:

1. Every request is a `PortalRequest` object that owns its request handle, is subscribed
   before the invocation, and is closed with `Request.Close` on cancellation or
   timeout (5 s). A timeout moves the backend to `UNAVAILABLE`.
2. **Generation guard:** each session has a monotonically increasing generation; the
   session handle, the generation and the request handle are recorded together. Any
   `Response`/`Activated`/`ShortcutsChanged` for a non-current session or generation is
   ignored. A session created by a late `Response` after the user already edited the
   binding is closed immediately (no orphan session).
3. Exactly one bind attempt per session; editing creates a new session (v1) or uses
   `ConfigureShortcuts` (v2 interface, detected at runtime).
4. No restore token is invented or persisted. `hotkeys.portalBindings` stores only what
   `BindShortcuts` reported, for UI display; `ListShortcuts` seeds the display at
   startup.
5. `parent_window`: the application has no existing XDG window-identifier helper, and
   Qt exposes no public Wayland surface export. The design passes **the empty string**
   (valid: "no parent"), and documents the portal dialog as unparented. On X11 the
   window id may be passed as `x11:<hex>` when available. An export helper is explicitly
   out of scope.
6. Activation tokens are forwarded unchanged to `activated(id, token)`.
7. Shutdown closes the session; session closure, service loss and denial are normal
   states with UI consequences, logged once.

`tst_portal_hotkeys` runs against a fake portal on a private D-Bus session and covers:
fast and delayed responses, denial, partial binding, `Request.Close` before response,
session closed mid-flight, late response after an edit (orphan session closed), second
edit, service disappearance, and shutdown.

#### 6.6.3 Startup, platform choice, and activation

- Remove the forced `QT_QPA_PLATFORM=xcb` block in `main.cc:261-268`; never overwrite a
  user-supplied platform.
- `platformwindow.hh` provides `screenForPoint()`, `availableGeometryForPoint()`,
  `popupFlags()` (Wayland: `Tool|FramelessWindowHint|WindowStaysOnTopHint`; X11: the
  existing flags including `X11BypassWindowManagerHint`) and
  `applyActivation(QWidget*, QString const & token)`, which — with
  `HAVE_KWINDOWSYSTEM` — calls `KWindowSystem::setCurrentXdgActivationToken(token)`
  (which writes `XDG_ACTIVATION_TOKEN`, consumed by Qt's Wayland `requestActivate()`),
  then `windowHandle()->requestActivate()`. Without KWindowSystem it calls
  `requestActivate()` only. Activation is **best-effort** and reports no boolean.

#### 6.6.4 Clipboard behaviour (no false fallback)

| Behaviour | Today | Wayland without portal |
| --- | --- | --- |
| Global double-`Ctrl+C` shortcut | registered on `HotkeyWrapper` (`mainwindow.cc:3183`) | **Unavailable** — it is a global hotkey, not clipboard polling |
| Clipboard-change scanning | `scanpopup.cc:533`, when enabled | Unchanged |
| Explicit clipboard lookup | tray/popup action, `scanpopup.cc:483` | Unchanged |

The hotkey preferences page states which of these is active for the current backend.

### 6.7 Audio

- Keep `ffmpegaudio.cc` / `ffmpegaudioplayer.cc`; only API-level fixes.
- Delete `multimediaaudioplayer.*`, the `MAKE_QTMULTIMEDIA_PLAYER` plumbing and the
  `no_qtmultimedia_player` switch.
- Config migration (§9) normalizes the real stored value `"Qt Multimedia"` →
  `InternalPlayerBackend::ffmpeg().uiName()` (`"FFmpeg+libao"`); unknown names fall back
  to ffmpeg and log.
- `no_ffmpeg_player` remains supported: the internal player is unavailable, the
  preferences UI hides it, external playback works, and the stored value is left
  untouched in that build.

## 7. Concurrency and threading

No structural change. `QThreadPool`, `QRunnable`, `QSemaphore`, `QMutex` and
`QAtomicInt` all exist in Qt 6. Mechanical fixes:

- `Mutex : public QMutex` constructed with `QMutex(Recursive)` → `QRecursiveMutex`
  (Qt 6 removed the constructor argument). This accounted for 71 probe errors.
- `QList<T*>::const_iterator` uses relying on implicit `QList<T>` → `QList<const T>`
  need explicit types (Qt 6 unified `QList`/`QVector`).

The bridge serialises script dispatch on the GUI thread; scheme handlers own their
`DataRequest` connections on the GUI thread; the request interceptor runs on the
Chromium IO thread and reads only immutable snapshots.

## 8. Testing strategy

### 8.1 Suites (`tests/`, wired to `make check` per §6.1)

| Test | Covers |
| --- | --- |
| `tst_encoding` | Byte-exact decode/encode fixtures (not just round-trips) for UTF-8/16/32 with and without BOM, Latin-1, Shift-JIS, Big5, Big5-HKSCS, GB18030, GB2312, EUC-JP, Windows-1251, ISO-8859-1…10 and 13…16; invalid byte counts → replacement grouping; truncated sequence; state flush; `exUnrepresentable` per caller policy (URL placeholder left intact, EPWING no-match); `forHtml` precedence; `forLocale` |
| `tst_linkrewrite` | Rules moved out of `stardict`/`zim`/`dictdfiles`/`dictserver`/`website`: `gdlookup` links, `bres`/`gico`/`gdau` prefixing, absolute/protocol-relative URLs untouched, multiline HTML |
| `tst_regex` | Per-pattern pre-change expectations, incl. newline-in-dot, Unicode word/digit, empty pattern, anchored `exactMatch`, minimal matching, wildcard, and the `website.cc` RegExp2 pattern |
| `tst_article_schemes` | Registration for every scheme, percent-encoded hosts/paths, flags |
| `tst_article_load` | `gdlookup` document from a fixture dictionary with `bres` resources; `blank=1`; group/dictionaries/contexts; `gdpicture` wrapper document |
| `tst_article_stream` | `ArticleStreamDevice` contract: `0` before completion, `-1` once drained after completion, `readyRead` per chunk and on completion, `bytesAvailable`, cancellation, single job completion |
| `tst_external_content` | Block policy per §6.5.7: two enabled dictionaries on unrelated domains — A's page blocks B's host, allows its own and same-site resources, and allows everything when the preference is off |
| `tst_article_bridge` | Parser-time notification queued before channel ready; post-switch notification; stale-generation rejection; navigation during in-flight script; destruction |
| `tst_website_frames` | Two local servers on non-default ports (same-origin and cross-origin): relative image/CSS/form/script-relative fetch, height measurement and reveal, click-text forwarding with dictionary id, frame-local word lookup, **and the same instrumentation assertions again after a second frame navigation** |
| `tst_article_history` | A→B→back, repeated identical URLs, reload (no scroll restore), forward truncation, scroll restore after content growth |
| `tst_print_pipeline` | PDF snapshot readiness (`Ready` before preview), page count before preview, page-size regeneration, error status, cancel, tab close |
| `tst_save_article` | Normal save, cancel mid-download (no leftovers), navigate-during-collection |
| `tst_portal_hotkeys` | Fake-portal lifecycle per §6.6.2 |
| `tst_hotkeys` | Binding parser/matcher, `HotkeySyntax` conversion, single-combination restriction |
| `tst_htmlescape`, `tst_langcoder`, `tst_utf8_wstring` | Existing pure utilities |
| `tst_platformwindow` | Screen lookup, popup flags per platform name |

### 8.2 Gates

1. **Build:** `qmake6 && make -j$(nproc)` → exit 0.
2. **Tests:** root `make check` (§6.1 recipe) builds and runs every testcase through the
   `GD_TEST_WRAPPER`-prefixed commands and propagates failure. Per-test limits: 60 s for
   pure tests, 120 s for WebEngine tests, enforced with `QSignalSpy::wait()` and
   `QTRY_VERIFY` inside the test, plus the suite-level `timeout 900`. A required suite
   whose environment is missing **fails** via its `QVERIFY(envAvailable())` guard (§6.1);
   only optional cases may `QSKIP`.
3. **Gate verification:** `scripts/verify-test-gate.sh` proves an intentionally failing
   testcase makes the root `make check` fail, and that the clean tree passes.
4. **Qt6-only audit:** `scripts/check-qt6-only.sh` greps the *compiled inputs*
   (`*.pro`, `*.pri`, `*.cc`, `*.hh`, `*.c`, `*.h`, `*.mm`, `*.ui`, `*.qrc`, excluding
   `qtsingleapplication/`, `winlibs/`, `maclibs/`, `opencc/`) and the generated link
   line for `QRegExp`, `QTextCodec`, `QDesktopWidget`, `QX11Info`, `QWebView`,
   `QWebPage`, `QMediaContent`, `IS_QT_5`, `webkit`, `qt6-5compat`. It excludes `docs/`
   (including this design), `help/`, `locale/` and fixture data, and prints the exact
   command.
5. **Headless startup smoke:** `timeout 30 xvfb-run -a ./goldendict` exits 124 with no
   crash and no QtWebEngine initialisation error. Supplemental only.
6. **Live Wayland gate (manual, recorded):** the log shows `platformName() == "wayland"`,
   the portal backend selected, a bound shortcut, a rendered article, a lookup, the scan
   popup raised through the portal shortcut, and audio played. A separate explicit
   `QT_QPA_PLATFORM=xcb` run validates RECORD.

### 8.3 Manual parity checklist

Article rendering (StarDict/Babylon/MDict/BGL/DSL/Zim/website fixtures), picture links,
word selection in the main frame and inside website frames, middle-click new tab, FTS
highlight and navigation, find bar, zoom, context menu, save article with resources,
print preview and print, DevTools, audio, history with scroll restore, dictionary
groups, preferences dialogs, and the four hotkey preference states.

## 9. Configuration migration

- `preferences.internalPlayerBackend == "Qt Multimedia"` → `"FFmpeg+libao"` in memory on
  load (via `InternalPlayerBackend::ffmpeg().uiName()`), keeping storage format and save
  timing; unknown values fall back to ffmpeg and log; `no_ffmpeg_player` builds leave the
  value untouched.
- New additive keys: `hotkeys.portalBindings` (reported bindings, for display only).
- Hotkey strings keep the existing format; sequences the portal cannot bind are
  preserved verbatim and reported as unavailable on Wayland.
- Proxy preference unchanged; the "caching proxy" mode is documented as an HTTP proxy
  for Chromium.

## 10. Error handling and failure modes

| Failure | Behaviour |
| --- | --- |
| Unknown charset, invalid or truncated bytes | U+FFFD substitution, one log line, lookup continues |
| Unrepresentable output | Per-caller policy (§6.2): URL placeholder left intact, EPWING codepage skipped |
| Scheme job cancelled/destroyed | `req->cancel()`, disconnect, device destroyed with the job |
| Portal absent/denied/version < 1 | Hotkeys disabled with an explanation; clipboard scanning and explicit lookup remain |
| `BindShortcuts` subset/empty | Bindings reported unavailable; new session only when the user edits |
| Portal request timeout / session closed / service loss | `UNAVAILABLE`, bindings cleared, UI updated, single log line, no retry loop |
| Website frame blocked by `X-Frame-Options` | Frame stays empty; log + preferences hint to use inline mode |
| Article navigates while scripts are queued | Generation bump drops stale inbound and outbound events |
| Save cancelled / navigated | Temp directory removed; no partial output |
| Print generation cancelled / tab closed | Snapshot discarded, user informed, no partial file |
| WebEngine missing at runtime | Startup fails with a clear message |
| Exit with clearing enabled | Both caches cleared, `clearHttpCacheCompleted` awaited (5 s bound), then quit |

## 11. Risks

| Risk | Mitigation |
| --- | --- |
| Regex conversion changes parser behaviour silently | Consolidate duplicates into `LinkRewrite`, convert rule by rule, pin with `tst_regex`/`tst_linkrewrite`; per-pattern flags (`DotMatchesEverythingOption`, `UseUnicodePropertiesOption`) |
| iconv mapping differs from previous `QTextCodec` tables | Byte-exact fixtures per charset; unknown names degrade to UTF-8 |
| Word-under-cursor JS rewrite changes selection semantics | Deterministic JS harness page plus the manual checklist |
| Frame instrumentation depends on `QWebEngineFrame` script execution across origins | `tst_website_frames` with two real servers before UI work lands, incl. reinstrumentation after the second navigation; fallback is inline mode |
| `X-Frame-Options`-protected sites cannot be framed | Documented parity limit + inline-mode escape hatch |
| Portal UX differs from the recorder widget | Capability-driven UI; no binding shown active unless the portal reported it |
| Proxy semantics differ for `HttpCachingProxy` | Documented; verified by a proxy fixture |
| Print no longer reflows on page-size change | Documented parity note; snapshot re-rendered at new metrics |
| Chromium dependency size | README requirement note |

## 12. Documentation deliverables

- `README.md`: Qt 6 requirements, native-Wayland operation, `make check`, removed options,
  proxy note.
- `docs/superpowers/specs/2026-10-24-qt6-migration-design.md` (this document).
- `docs/wayland-limitations.md`: hover-translation, multi-key hotkey sequences, focus
  restore, `X-Frame-Options` framing, print reflow.
