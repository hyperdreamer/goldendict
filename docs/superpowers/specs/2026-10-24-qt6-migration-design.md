# GoldenDict Qt6 Migration — Design

- **Date:** 2026-10-24
- **Repository:** `hyperdreamer/goldendict` @ `b4f3fcdc` (branch `master`)
- **Topic slug:** `qt6-migration`
- **Target environment:** Arch Linux, KDE Plasma 6 on **Wayland**, Qt **6.12**, GCC 16
- **Revision:** 2 (addresses design review 1: 3 blockers, 14 majors, 2 minors)

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
| Regex surface | **248 `QRegExp` occurrences in 34 files** |
| Legacy encodings | 8 files with `QTextCodec`; web pages, EPWING (ISO8859-1/GB2312/EUC-JP), web-site encoding detection |
| Article view | `articleview.cc` is 93 KB; 33 synchronous `evaluateJavaScript` calls; 12 `QWebElement` uses; `hitTestContent`, `frameAt`, `QWebPage::FindFlags` incl. `HighlightAllOccurrences` |
| C++→JS callback | `addToJavaScriptWindowObject("articleview", jsProxy)` (`articleview.cc:1175`); generated HTML calls `articleview.onJsActiveArticleChanged(...)` (`article_maker.cc:188`, `:638`) |
| Article documents | `gdlookup://localhost?word=…&group=…&…` is the article document URL; `article_netmgr.cc:392` serves it as `text/html` from `ArticleMaker` |
| Internal schemes | `gdlookup`, `bres`, `gdau`, `gdvideo`, `gico` (`article_netmgr.cc:455`), `gdpicture` (`:520`), `qrcx` remapping (`:258`) |
| Website frames | `website.cc:502` emits `<iframe id="gdexpandframe-…" src="<remote url>" style="display:none">`; `articleview.cc:539` enumerates child frames, measures `contentsSize()`, sets height, shows it, and injects listeners |
| X11 coupling | `QX11Info` in 5 files; `hotkeywrapper.cc` uses `XRecordEnableContext` with an Xlib `Display*`; `mainwindow.cc` X focus/`XSendEvent` |
| Startup forces XWayland | `main.cc:261-268`: when `XDG_SESSION_TYPE=wayland`, `setenv("QT_QPA_PLATFORM","xcb",1)` |
| Screen geometry | `QDesktopWidget` in 4 files |
| Audio | ffmpeg is the default internal backend (`"FFmpeg+libao"`); QtMultimedia backend is `"Qt Multimedia"` (`config.hh:216`, `:221`) |
| Proxy / cache | application proxy + `proxyAuthenticationRequired` handlers (`mainwindow.cc:787`, `1290-1335`); `QNetworkDiskCache` (`:1345`), cleared at exit (`:1108`) |
| Tests | **None.** No `tests/` directory, no unit harness; `.travis.yml` is Qt5-era |
| Build system | qmake only (`.pro`), no CMake |

Environment facts that shape the design (all checked on this machine):

- Qt 6.12 with `webenginewidgets`, `webenginecore`, `5compat`, `multimedia`, `help`,
  `svg`, `svgwidgets`, `webchannel`, `dbus`, `tools`.
- `xdg-desktop-portal 1.22.1` + `xdg-desktop-portal-kde 6.8.1`, and
  `org.freedesktop.portal.GlobalShortcuts` is present with `ConfigureShortcuts`
  (interface version 2).
- `kwindowsystem 6.31` with headers, exposing
  `KWindowSystem::setCurrentXdgActivationToken(const QString&)`.
- `hunspell 1.7.5`, `libopencc 1.4.1`, `libeb 4.4.3`, `libzim 9.8.1`, ffmpeg/ao/vorbis,
  `zstd 1.5.7` **with** `/usr/lib/pkgconfig/libzstd.pc`.
- `Xvfb`/`xvfb-run` present.
- Qt 6.12 `QStringDecoder` accepts named legacy codecs here (`Shift-JIS`, `Big5`,
  `Big5-HKSCS`, `GB18030`, `GB2312`, `EUC-JP`, `windows-1251`, `ISO-8859-11` all
  construct valid decoders) because this Qt build has ICU support enabled. That is a
  build-feature-dependent capability, not a Qt guarantee (§6.2).

## 2. Decisions

| ID | Decision | Rationale |
| --- | --- | --- |
| **B** | **Qt6-only, full modernization.** No Qt4/Qt5 compatibility layer, no `qt6-5compat` dependency. | Deprecated-but-working shims would leave the riskiest code (parser regexes, legacy encodings) frozen in a module that can disappear at any Qt release. |
| **B1** | **Full article-view parity** on QtWebEngine, including scan-popup word-under-cursor, FTS highlighting, find bar, zoom, context menu, print, DevTools. | The scan popup is the product's signature feature; QtWebEngine already ships printing and DevTools, so partial scope saves little. |
| **W2** | **Global shortcuts via XDG Desktop Portal `GlobalShortcuts`**, with the existing X11 RECORD backend retained for X11 sessions and automatic selection. Hover-translation stays X11-only. | The portal is the only sanctioned Wayland path for global shortcuts; the desktop provides it. |
| **P1** | **Keep qmake**, rewritten for Qt 6, Linux first-class. Windows/macOS blocks updated best-effort but not compiled or verified. | One build definition; a CMake port is a follow-up project, not a migration prerequisite. |
| **T1** | **Add a Qt Test suite** for every pure-logic module the migration touches, plus build gates, deterministic WebEngine fixture tests, a fake-portal lifecycle test, an Xvfb startup smoke test, and a live Wayland gate. | Without it, converting 248 regexes and 8 codec sites is unverifiable. |
| **K4** | **Drop the QtMultimedia internal audio backend**; keep ffmpeg as the internal player and "external program" as-is. | `qt6-multimedia` is an extra system dependency whose only unique value is a slower, less capable player; ffmpeg is already the required default. |
| **K5** | **One encoding implementation on iconv**, not `QStringDecoder`. | `QStringDecoder` can decode these charsets only when the Qt build enables ICU, which makes correctness and test fixtures depend on a packaging decision; iconv is already a mandatory dependency of the dictionary engines. Single implementation, single behaviour, testable byte-exactly. |

## 3. Goals

1. `qmake6 && make -j$(nproc)` produces a working `goldendict` binary on this machine
   with no `qt6-5compat` and no Qt5/Qt4 branches.
2. The application starts **natively on Wayland** (`platformName() == "wayland"`),
   renders articles, performs lookups, plays audio, and runs its scan popup.
3. Global shortcuts work on Wayland through the portal and on X11 through the
   retained RECORD implementation.
4. The article view preserves the WebKit-era behaviour set: article documents,
   dictionary/resource/audio URL serving, word-under-cursor translation, FTS search
   with highlight-all, find bar, zoom, context menu actions, save-article, print and
   print preview, and the DevTools inspector.
5. Every migration-sensitive module (regex rewriting, encoding conversion, link
   rewriting, hotkey matching, scheme dispatch, history state) is covered by
   automated tests that fail if behaviour regresses.
6. No silent feature loss: anything that cannot be preserved is disabled with a
   visible, explanatory UI state and documented.

## 4. Non-goals

- CMake migration; Windows/macOS build validation; parallel Qt5 support.
- Re-architecting the dictionary engines, indexing (`btreeidx`), or FTS indexing.
- Wayland hover-translation via portal `InputCapture` (separate project).
- Changing the on-disk config format beyond the additive migration in §9.
- Visual restyling.

## 5. Architecture overview

```text
  ┌──────────────────────────────────────────────────────────────────┐
  │ 5. Desktop integration: hotkeys (X11 RECORD | portal), windows,  │
  │    screen geometry, activation tokens                             │
  ├──────────────────────────────────────────────────────────────────┤
  │ 4. Article view: QWebEngineView + scheme handlers + QWebChannel   │
  ├──────────────────────────────────────────────────────────────────┤
  │ 3. Platform-neutral services: Encoding (iconv), LinkRewrite,      │
  │    regex migration, article history, audio (ffmpeg)               │
  ├──────────────────────────────────────────────────────────────────┤
  │ 2. Dictionary engines (unchanged algorithms, migrated API calls)  │
  ├──────────────────────────────────────────────────────────────────┤
  │ 1. Build: goldendict.pro for Qt 6, tests/ subprojects, docs       │
  └──────────────────────────────────────────────────────────────────┘
```

### 5.1 New and rewritten modules

| Module | Kind | Responsibility |
| --- | --- | --- |
| `encoding.hh/.cc` | new | Named character-set conversion and HTML/BOM charset detection; replaces all `QTextCodec` use |
| `linkrewrite.hh/.cc` | new | The `gdlookup://`, `bres://`, `gico://`, `gdau://` HTML rewriting rules currently duplicated in `stardict.cc`, `zim.cc`, `dictdfiles.cc`, `dictserver.cc`, `website.cc` |
| `articlewebview.hh/.cc` | rewritten | `ArticleWebView : QWebEngineView` + `ArticleWebPage : QWebEnginePage` |
| `articlebridge.hh/.cc` | new | Bidirectional page bridge: `QWebChannel` object named `articleview`, script injection, generation-stamped async operations |
| `articleurls.hh/.cc` | new | The single URL dispatch matrix (§6.5.1) and its producers |
| `articleschemes.hh/.cc` | new | `QWebEngineUrlScheme` registration, `gdlookup`/`bres`/`gdau`/`gdvideo`/`gico`/`gdpicture` handlers, request interceptor |
| `articlehistory.hh/.cc` | new | Application-owned per-history-entry article state (replaces WebKit `QWebHistoryItem::setUserData`) |
| `hotkeybackend.hh` | new | Abstract hotkey backend interface (`X11RecordBackend`, `PortalBackend`) |
| `portalhotkeys.cc/hh` | new | D-Bus client for `org.freedesktop.portal.GlobalShortcuts` |
| `platformwindow.hh/.cc` | new | Screen geometry (`QScreen`), window-flag/activation policy per `QGuiApplication::platformName()`, xdg activation token application |
| `qt4x5.hh` | **deleted** | Qt4 branches and `IS_QT_5` helpers are no longer meaningful |
| `article_netmgr.cc/hh` | rewritten | Keeps `Dictionary::WebMultimediaDownload` (standalone QNAM consumer); loses the QNAM subresource override and `QWebSecurityOrigin` accounting |
| `multimediaaudioplayer.*` | **deleted** | Decision K4 |

## 6. Component design

### 6.1 Build configuration (`goldendict.pro`)

```qmake
TEMPLATE = app
TARGET = goldendict
VERSION = 1.5.1+git
CONFIG += c++17 exceptions rtti stl

QT += core gui widgets xml network sql printsupport help svg \
      webenginecore webenginewidgets webchannel dbus
```

Rules:

- Remove every `QT_MAJOR_VERSION`/`QT_MINOR_VERSION` branch and the `webkit`,
  `webkitwidgets`, `x11extras`, `multimedia` modules.
- `CONFIG += c++17` is mandatory (Qt 6 headers require C++17).
- Add `SOURCES`/`HEADERS` entries for the new modules; drop the deleted ones.
- Keep `zim_support`, `chinese_conversion_support`, `no_ffmpeg_player`,
  `old_hunspell`, `no_extra_tiff_handler`; remove `no_qtmultimedia_player`.
- `zim_support` keeps `LIBS += -llzma -lzstd`; `/usr/lib/pkgconfig/libzstd.pc` exists,
  so `PKGCONFIG += libzstd` is equally valid and is used when present, falling back to
  direct `-l` linkage.
- `kwindowsystem` is an **optional** dependency: when `pkg-config --exists
  kwindowsystem` (or `KF6WindowSystem` CMake config is present) the build defines
  `HAVE_KWINDOWSYSTEM` and links it for xdg-activation-token handling (§6.6.3).
- Test wiring, so that the documented gate is real:

```qmake
# tests/tests.pro — TEMPLATE = subdirs over the testcase projects
check.depends = sub-tests
sub-tests.target = sub-tests
QMAKE_EXTRA_TARGETS += sub-tests check
```

`tests/tests.pro` lists every testcase project; each is a `TEMPLATE = app` with
`QT += testlib` and `CONFIG += testcase`. `make check` in the root therefore builds
and runs all tests and fails the make invocation on any failure. Documented
equivalent for scripted use: `qmake6 tests/tests.pro -o Makefile.tests && make -f
Makefile.tests check`.

### 6.2 `Encoding` — character conversion

Legacy dictionary and web content requires Shift-JIS, Big5, Big5-HKSCS, GB18030,
GB2312, EUC-JP, Windows-1251 and ISO-8859-x. `QStringConverter`'s *enum* covers only
Unicode + Latin-1; its **named** constructors do cover the rest when the Qt build
enables ICU (verified on this machine). Decision **K5** uses iconv instead so the
behaviour does not depend on a Qt build feature, and so the program has exactly one
conversion implementation, whose exact byte mappings are pinned by fixtures.

```cpp
// encoding.hh
class Encoding
{
public:
  DEF_EX( Ex, "Encoding error", std::exception )
  DEF_EX( exUnrepresentable, "Character not representable in target encoding", Ex )

  /// Returns nullptr when the charset name is unknown or unsupported.
  static std::unique_ptr< Encoding > byName( QString const & name );
  static Encoding utf8();       ///< Never throws; always available.

  enum class Preference { Header, Bom, Meta };

  /// Detection order (matching QTextCodec::codecForHtml plus the HTTP header the
  /// caller may supply): explicit header charset, UTF BOM, <meta charset>,
  /// <meta http-equiv="Content-Type">, XML declaration, then `fallback`.
  static Encoding forHtml( QByteArray const & body,
                           QByteArray const & contentTypeHeader,
                           QString const & fallback = QStringLiteral( "UTF-8" ),
                           Preference * detectedBy = nullptr );

  bool isValid() const;                       ///< false only for the null encoding
  QString name() const;

  /// Invalid or truncated byte sequences become U+FFFD; this never throws.
  QString decode( QByteArray const & data ) const;

  /// Throws exUnrepresentable when a character has no mapping.
  QByteArray encode( QString const & text ) const;
};
```

Contract details:

1. Each call opens and closes its own `iconv_t`, so an `Encoding` value is immutable,
   copyable and safe to use from several threads concurrently. The mutable-state
   wrapper `Iconv` is **not** used here.
2. `decode()` maps `EILSEQ` (invalid sequence) and a final `EINVAL` (truncated
   sequence) to U+FFFD and continues, so a malformed page cannot abort a lookup.
3. `encode()` throws `exUnrepresentable` on `EILSEQ`; callers fall back to UTF-8
   percent-encoding, which is what the current `%25GD…%25` substitutions do.
4. Charset aliases are resolved through an explicit table (`"Big-5"`, `"Big5"`,
   `"Shift-JIS"`, `"SJIS"`, `"ISO 8859-n"`, `"Windows-1251"`, …) so the names used in
   `config` and in web templates keep working; unknown names return `nullptr` and the
   caller uses UTF-8, as today (`website.cc:219`).
5. BOM detection precedes HTML metadata (this is what `codecForHtml` did via
   `codecForUtfText`); the HTTP `Content-Type` header, when supplied, wins over both.

Call sites converted: `website.cc` (`codecForHtml` replacement at `:140`, decode at
`:219`, and the URL substitutions for `%25GD1251%25`, `%25GDBIG5%25`,
`%25GDBIG5HKSCS%25`, `%25GDSHIFTJIS%25`, `%25GDGBK%25`, `%25GDISO1%25`…`%25GDISO16%25`
where ISO 8859-11 and -12 are skipped), `epwing_book.cc` (ISO8859-1, GB2312, EUC-JP),
and any remaining `QTextCodec` reference.

### 6.3 `LinkRewrite` — one owner for the article link rules

Four engines repeat near-identical regexes that rewrite dictionary HTML into
`gdlookup://` / `bres://` / `gico://` / `gdau://` form, with subtly different patterns
(`dictdfiles.cc` and `dictserver.cc` even carry both a `QRegularExpression` and a
`QRegExp` version of the same rule). Consolidating them is both the safest way to
convert the regexes and a reduction in duplication.

```cpp
// linkrewrite.hh
namespace LinkRewrite
{
/// Rewrites relative/anchor links into gdlookup://localhost/<word> form.
QString applyGdLookup( QString const & html );

/// Prefixes resource references (src=, href=, url()) with bres://<dictId>/.
QString applyBres( QString const & html, QString const & dictId );

/// Rewrites icon references into gico://<dictId>/<name>.
QString applyGico( QString const & html, QString const & dictId );

/// Rewrites audio sources into gdau://<dictId>/<name>.
QString applyGdau( QString const & html, QString const & dictId );
}
```

The exact patterns move here verbatim from their current homes and are then
converted to `QRegularExpression` once, with tests pinning the previous behaviour.

### 6.4 Regex migration rulebook (248 sites, 34 files)

Every rule below was verified by compiling and running against the installed
Qt 6.12 (`QRegExp` from Qt5Compat, `QRegularExpression` from QtCore), not taken from
memory:

| Difference | Measured behaviour | Required conversion rule |
| --- | --- | --- |
| `.` and newlines | `QRegExp("a.b").indexIn("a\nb") == 0`; `QRegularExpression("a.b").match("a\nb")` has no match | Add `DotMatchesEverythingOption` for every pattern that scans HTML or multi-line text — otherwise rewrite silently stops matching across newlines |
| Unicode character classes | `QRegExp("\\w+")` matches `привет`; `QRegularExpression("\\w+")` matches nothing; with `UseUnicodePropertiesOption` it matches all 6 characters. `QRegExp("\\d+")` matches U+0660; `QRegularExpression("\\d+")` does not; with the option it matches both digits | Add `UseUnicodePropertiesOption` wherever the original matched non-ASCII words or digits |
| Anchor semantics | `QRegExp("abc").indexIn("xabc") == 1`, `exactMatch("abc") == 1` | `indexIn` → `match()`; `exactMatch` → `QRegularExpression::anchoredPattern()` (or explicit full-length check) |
| Empty pattern | `QRegExp("").indexIn("abc")` matches at offset 0 with zero length; `QRegularExpression("")` matches at every position | Keep explicit `isEmpty()` guards; never rely on "empty matches nothing" |
| Minimal matching | `QRegExp::setMinimal(true)` makes `\d+` match one digit; `QRegularExpression(..., InvertedGreedinessOption)` reproduces it | Prefer explicit lazy quantifiers (`+?`); use `InvertedGreedinessOption` only for mechanical, test-pinned conversions |
| Wildcard syntax | `QRegExp` wildcard mode matches `a.txt` against `*.txt`; `QRegularExpression::wildcardToRegularExpression("*.txt")` returns `\A(?:(?s:[^/]*\.txt))\z` | Use `wildcardToRegularExpression()` and pin wildcard cases with tests, including the anchoring and `[^/]` differences |
| `RegExp2` syntax | `website.cc:311` selects `QRegExp::RegExp2` | Review site-by-site; RegExp2 differences (e.g. `indexIn` vs partial match, alternation scope) do not map 1:1 onto PCRE defaults |
| String API | `QString::split/count/filter(QRegExp)` do **not** exist (verified compile failure), while `QString::split(QRegularExpression)`, `QString::count(QRegularExpression)` and `QStringList::filter(QRegularExpression)` exist and work (`filter("^ab")` → `abc,abd`, `count("\\d+")` on `a1b22` → `3`, split → `a|b|c`) | Use the `QRegularExpression` overloads directly; do not hand-roll loops |
| Captures | `capturedRef()`/`cap(n)` are QRegExp/Qt5 spellings | `capturedView()` for views, `captured(n)` for owning strings; replacement back-references `\1` keep working with `QString::replace(QRegularExpression, …)` |
| Case sensitivity | `QRegExp` is case-sensitive by default | Keep defaults; where the original used `setCaseSensitivity`, map to `CaseInsensitiveOption` |

Existing precedent to reuse: `articleview.cc:2591` already builds
`QRegularExpression` with `DotMatchesEverythingOption | UseUnicodePropertiesOption |
InvertedGreedinessOption`, and is the model for the conversions above.

### 6.5 Article view on QtWebEngine

#### 6.5.1 URL dispatch matrix (replaces the QNAM override)

QtWebEngine's Chromium stack never consults a `QNetworkAccessManager` for
subresources, so every internal scheme becomes a registered engine scheme with a
handler, and the *meaning* of each URL is stated explicitly:

| Scheme | Produced by | Consumed by | Served as |
| --- | --- | --- | --- |
| `gdlookup://localhost?word=…&group=…&…` | `ArticleView` (document load), `ArticleMaker` links, dictionary link rewriting | **document** in the main frame or a new tab | `text/html` from `ArticleMaker::makeDefinitionFor` / `makeEmptyPage` |
| `gdlookup://localhost/…` (link click) | dictionary HTML | user activation | converted into an `ArticleView` lookup action; the engine navigation is refused |
| `bres://<dictId>/<path>` | `LinkRewrite::applyBres` | `<img>`, `<link>`, `<script>`, CSS `url()` | resource bytes with MIME from the resource name |
| `gdau://<dictId>/<name>` | `LinkRewrite::applyGdau` | audio link clicks and `<audio>` | audio bytes |
| `gico://<dictId>/<name>` | `LinkRewrite::applyGico` | `<img>` dictionary icons | image bytes |
| `gdvideo://<dictId>/<name>` | dictionary HTML | `<video>` | video bytes |
| `gdpicture://…` | dictionary HTML | picture view | image bytes |
| `qrcx://…` | resource remapping (`article_netmgr.cc:258`) | `qrc:` resources | resource bytes |
| `http(s)://…` | web dictionaries, remote images | normal engine requests | engine default, subject to §6.5.7 |

The article **document itself** is always a `gdlookup` URL. Only the `ArticleView`
issues document loads; a `gdlookup` navigation initiated by a user click is
intercepted before it reaches the engine and turned into an `ArticleView` action.
This removes the ambiguity that made the first revision's rule unimplementable.

#### 6.5.2 Scheme registration

```cpp
// main(), BEFORE the QApplication instance is constructed
static void registerArticleSchemes()
{
  struct SchemeSpec { char const * name; bool hostSyntax; };
  // Verified: Syntax::Host registers successfully; Syntax::HostAndPort with the
  // default port -1 is silently rejected (schemeByName() returns an empty name),
  // and HostAndPort with an explicit default port also registers.
  for ( auto const & spec : { SchemeSpec{"gdlookup", true}, SchemeSpec{"bres", true},
                              SchemeSpec{"gdau", true},     SchemeSpec{"gico", true},
                              SchemeSpec{"gdvideo", true},  SchemeSpec{"gdpicture", true},
                              SchemeSpec{"gdweb", true},    SchemeSpec{"qrcx", true} } )
  {
    QWebEngineUrlScheme scheme( spec.name );
    scheme.setSyntax( spec.hostSyntax ? QWebEngineUrlScheme::Syntax::Host
                                      : QWebEngineUrlScheme::Syntax::Path );
    scheme.setFlags( QWebEngineUrlScheme::LocalAccessAllowed
                   | QWebEngineUrlScheme::LocalScheme
                   | QWebEngineUrlScheme::CorsEnabled
                   | QWebEngineUrlScheme::SecureScheme );
    QWebEngineUrlScheme::registerScheme( scheme );
  }
}
```

Rules:

1. All schemes use `Syntax::Host` (portless `scheme://dictionary-id/path` URLs, which
   is what every producer emits). `HostAndPort` is never used; if a future scheme
   needs a port, `setDefaultPort()` is mandatory and the URL/origin consequences are
   documented with the call.
2. Registration happens before `QApplication` and before any `QWebEngineProfile`.
3. `tst_article_schemes` asserts `schemeByName(name).name()` equals the registered
   name for every scheme, that percent-encoded paths survive `QUrl`, and that the
   flags allow local resource access.

#### 6.5.3 Document and resource serving pipeline

`ArticleSchemeHandler` implements `QWebEngineUrlSchemeHandler::requestStarted`.
For each job it creates a `Dictionary::DataRequest`, connects its update/finish
signals, and writes into a job-owned `QBuffer`:

- MIME type comes from the request kind: `gdlookup` → `text/html`, others from the
  resource path extension (the current code does this in `article_netmgr.cc`).
- Buffered delivery (write the whole payload on completion) is the baseline; jobs
  that are known to stream (large media) forward incremental updates.
- Cancellation: `job->destroyed()` deletes the device and disconnects the request,
  mirroring the current `ArticleResourceAdapter` lifetime rules
  (`article_netmgr.cc:543`, `:567`).
- Failure: unknown/unresolvable resources `fail(QWebEngineUrlRequestJob::UrlNotFound)`;
  a valid request whose content is empty returns an empty successful reply.
- `gdlookup` documents get their response built exactly as `article_netmgr.cc:392-452`
  does today, including the `blank=1` empty page, `dictionaries=`, `muted=`,
  `contexts=`, `ignore_diacritics` and `punctuation_suffix` parameters.

`tst_article_load` renders a definition for a fixture dictionary end to end and
asserts the resulting HTML contains the expected headword and that its `bres`
resources resolve — without re-dispatching the lookup.

#### 6.5.4 Two-way JavaScript bridge

The generated article HTML calls back into C++ (`articleview.onJsActiveArticleChanged`),
so the bridge is bidirectional and is installed before any inline script runs.

```cpp
class ArticleBridge : public QObject   // exposed on the QWebChannel as "articleview"
{
  Q_OBJECT
public:
  explicit ArticleBridge( ArticleView & view, QObject * parent = nullptr );

public slots:                              // JS → C++
  void onJsActiveArticleChanged( QString const & id );

signals:                                   // C++ → JS (helper library API)
  void articleChanging();

public:
  /// Generation-stamped request/response over runJavaScript.
  using Result = std::function< void ( QJsonValue ) >;
  void run( QString const & script, Result result = {}, int frameId = -1 );
  void reset();                            // called on loadStarted and on page destruction

  int documentGeneration() const;
};
```

Implementation contract:

1. Transport is `QWebChannel` (`QT += webchannel`): the channel's `qt.webChannelTransport`
   client script and `/qtwebchannel/qwebchannel.js` are injected with a
   `QWebEngineScript` at `DocumentCreation` in the `MainWorld` world, followed by the
   helper library that opens the channel and publishes `window.articleview` **before**
   the article body's inline scripts run. `article_maker.cc` therefore needs no change
   to its call sites.
2. Every request carries the current `documentGeneration`. A completion whose
   generation no longer matches the page's is dropped without invoking its callback
   and without blocking the queue; `reset()` increments the generation, clears the
   queue, and marks outstanding requests abandoned. Chromium may still execute an
   abandoned script; the design does not assume cancellation, only ignored results.
3. Script dispatch is serialised through one queue so ordering matches the old
   synchronous program order.
4. Page/object lifetime: the bridge is parented to the view and is destroyed after the
   page; in-flight callbacks capture a `QPointer` guard.

`tst_article_bridge` covers: notification at document creation,
`onJsActiveArticleChanged` before and after a dictionary switch, navigation while a
script is in flight followed by a script for the new document, and page destruction
with a queued callback.

#### 6.5.5 DOM algorithms moved into JavaScript

| Former WebKit API | Replacement |
| --- | --- |
| `frame->evaluateJavaScript("…")` returning a value | `ArticleBridge::run(script, cb)` |
| `QWebElement::geometry()`, `render()` + `QImage` to find the word under the cursor (`articleview.cc:2899-3010`) | `gdWordAtPoint(x, y)` — one JS call using `document.caretRangeFromPoint`/`elementFromPoint`, `Range.getClientRects`, and temporary `<span>` wrapping; it saves and restores the DOM and selection internally and returns the word string |
| selection save/restore around FTS navigation | `gdSaveSelection()`, `gdRestoreSelection()` JS state |
| `QWebPage::FindFlags` + `HighlightAllOccurrences` | `gdHighlightAll(words, matchCase)`, `gdClearHighlights()`, `gdScrollToHit(i)` |
| `frameAt()` / `QWebHitTestResult` for context menus | `QWebEngineView::lastContextMenuRequest()` (`linkUrl`, `mediaUrl`, `selectedText`, `mediaType`) plus one JS call for the enclosing element's text |
| `QWebFrame::contentsSize()` for website frames (§6.5.6) | `gdMeasureFrame()` over the same-origin frame; `gdExpandFrame(name)` sets height and displays it |

#### 6.5.6 Website frames (`gdexpandframe-*`)

Web dictionaries with `inside_iframe` emit a hidden iframe pointing at a remote URL
(`website.cc:494-515`), and the article view measures and reveals it
(`articleview.cc:539-560`). With WebEngine, a cross-origin frame cannot be measured or
scripted from the parent page — and because the frame starts `display:none`, the
article would be invisible.

Resolution: serve the frame from **the same origin as the article document**. The
iframe's `src` becomes `gdlookup://localhost/website-frame?dict=<id>&url=<percent-encoded>`;
the `gdlookup` handler fetches that URL through the application's `QNetworkAccessManager`
(the existing `WebSiteArticleRequest` code path, which already rewrites relative links
to absolute and strips framing restrictions) and returns the HTML. Consequences:

- Parent and frame share the `gdlookup://localhost` origin, so
  `document.getElementById('gdexpandframe-…').contentDocument` is readable: height
  measurement, `display:block`, and the click/context listeners (`gdLastUrlText`) keep
  working.
- Because the content is fetched and re-served by us, the current response-header
  handling (dropping `X-Frame-Options`, `article_netmgr.cc:118-120`) still applies —
  something a `QWebEngineUrlRequestInterceptor` cannot do for arbitrary HTTP
  responses.
- Frame-internal subresources load from their absolute URLs (the existing rewrite)
  and are subject to §6.5.7.
- Dictionary attribution: the frame URL carries `dict=<id>`, so a click inside the
  frame maps to the right dictionary without guessing from the frame count.

`tst_website_frames` covers a same-origin fixture frame and an http fixture frame
(served by a local test server), including late frame navigation and a changing
content height.

#### 6.5.7 Network policy and interception

Inventory of behaviour removed with the QNAM override, and its replacement:

| Current behaviour | Replacement |
| --- | --- |
| `QWebSecurityOrigin::addAccessWhitelistEntry` per origin (`article_netmgr.cc:298`) | Not needed: `bres`/`gico`/`gdau` are registered `CorsEnabled` schemes, and same-origin frame content is served by us |
| "Disallow content from other sites" (`article_netmgr.cc:317`, default **false**, `config.cc:255`) | `ArticleRequestInterceptor` cancels remote `http(s)` requests when the preference is enabled, allowing only the hosts that the current page's dictionary whitelisted; default remains allow |
| Local file path adjustment (`article_netmgr.cc:338`) | Same path fix-up applied in the scheme handler and to `file://` URLs in `articleurls.cc` |
| User-agent override (`article_netmgr.cc:358`) | `QWebEngineProfile::setHttpUserAgent()` (global) and per-request `User-Agent` for scheme-handled fetches |
| Response header filtering, incl. dropping `X-Frame-Options` (`article_netmgr.cc:118-120`) | Preserved for content we fetch ourselves (website frames, dictionary resources, `gdweb`); **not** applicable to third-party subresources loaded directly by Chromium — documented as a constraint |
| Redirect handling in `WebSiteArticleRequest` | Unchanged: it is our own QNAM fetch, not engine traffic |

The interceptor is installed with `QWebEnginePage::setUrlRequestInterceptor`, runs on
the IO thread, and therefore only reads an immutable snapshot of the policy
(atomics), never widgets or config objects.

#### 6.5.8 Profile, proxy, cache, and authentication

- One `QWebEngineProfile` owned by `ArticleView`, created with a disk cache under the
  existing application cache directory, `setHttpCacheType` mapped from the current
  preference, and `setHttpCacheMaximumSize` from the current cache-size preference.
- Cache clearing at exit uses `clearHttpCache()` and its
  `clearHttpCacheCompleted()` signal instead of the QNAM cache clear.
- Proxy: Chromium does not read `QNetworkProxy::setApplicationProxy()`. Startup maps
  the configured mode to `QTWEBENGINE_CHROMIUM_FLAGS=--proxy-server=…` **before** the
  application object exists:
  - "system proxy" and "no proxy": handled by Chromium's own resolution;
  - "custom HTTP/SOCKS5": translated from config into `--proxy-server`;
  - changing the proxy therefore requires an application restart; the preferences
    page states this instead of silently ignoring it.
- Credentials: `QWebEnginePage::proxyAuthenticationRequired` and
  `authenticationRequired` are wired to the existing `MainWindow::proxyAuthentication`
  dialog path.
- `WebMultimediaDownload` (downloads started from context menu actions) keeps using
  QNAM with the application proxy; the design states that these two stacks are
  configured from the same preferences but resolve independently.

#### 6.5.9 Save, find, and print

| Operation | Current (sync) | WebEngine implementation |
| --- | --- | --- |
| `toHtml()` for "save article" (`articleview.cc:1751`, `mainwindow.cc:3695`) | returns `QString` | `page()->toHtml(cb)`; the save flow becomes asynchronous with a cancellation token, and a navigation/close during collection aborts the save with a message |
| Find bar (`articleview.cc:2428-2470`) | synchronous `findText` boolean | `findText(sub, flags, callback)`; results are delivered in request order, `QWebEngineFindTextResult` drives the "n of m"/not-found state, and stale results (a newer query was issued) are ignored |
| FTS highlight-all (`articleview.cc:2709`) | `HighlightAllOccurrences` | JS highlighter with its own match index and `gdScrollToHit` |
| Print (`articleview.cc:1768`, `mainwindow.cc:3568-3581`) | `QWebPage::print(QPrinter*)` inside print preview | `QWebEngineView::print(QPrinter*)` + `printFinished(bool)`; the printer object outlives the call, preview keeps its existing dialog, and failure is reported instead of assumed success |
| PDF export | `QWebPage::printToPdf` | `QWebEngineView::printToPdf` (kept as the separate export path) |

#### 6.5.10 Article state and history

WebKit stored `currentArticle`, `sx`, `sy` inside each history item
(`articleview.cc:866`, restored at `:580`). `QWebEngineHistoryItem` has no
application-data API, so state moves to `ArticleHistory`:

```cpp
struct ArticleState { QString currentArticle; double sx = 0; double sy = 0; };

class ArticleHistory
{
public:
  void reset();                                   // new document / tab
  void push( QUrl const & url, ArticleState const & );   // on committed navigation
  bool stateFor( int historyIndex, ArticleState * out ) const;
  void setCurrent( int historyIndex, ArticleState const & );  // scroll updates
  void truncateAfter( int historyIndex );         // forward-history truncation
};
```

Rules:

- Identity is the **history index** plus the committed URL, not the URL alone, so
  repeated visits to the same `gdlookup` URL keep distinct state.
- `push` is driven by `QWebEnginePage::urlChanged`/`loadFinished` and mirrors
  `QWebEngineHistory::count()`/`currentIndex()`; a navigation that truncates forward
  history also truncates `ArticleHistory`.
- Reload keeps the current entry's state; the "reload resets scroll" behaviour at
  `articleview.cc:1702` is preserved explicitly rather than inferred.
- Scroll capture is debounced JS (`gdGetScroll`) at the same points the old code read
  `sx`/`sy`.

The synchronous getters `getArticlesList`, `getCurrentArticle` and `hasSound`
(`articleview.cc:726`, `:741`, `:1721`) become a cached-state model: the article list
and sound availability are updated from the same JS results that drive the UI, held in
`ArticleView`, and exposed as signals consumed by `mainwindow.cc:2041`, `:2088`.
No nested event loop is used anywhere.

### 6.6 Desktop integration

#### 6.6.1 Hotkey backends

```cpp
// hotkeybackend.hh
struct HotkeyBinding
{
  QString id;             // "scan-popup", "show-main-window", "clipboard"
  QString description;
  QString sequence;       // e.g. "Ctrl+C,Ctrl+C" or "Ctrl+Alt+G"
};

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

1. **X11** (`platformName()` starts with `xcb`): `X11RecordBackend`. The Xlib handle
   comes from `QGuiApplication::nativeInterface<QNativeInterface::QX11Application>()->display()`
   (a `Display*`; `connection()` returns an `xcb_connection_t*` and is **not** usable
   for RECORD/Xlib calls), the root window from `DefaultRootWindow(display)`, and a
   null interface disables the backend with a warning. The same accessor replaces
   `QX11Info` in `keyboardstate.cc`, `mainwindow.cc` and `scanpopup.cc`.
2. **Wayland**: `PortalBackend`, unless the portal service is absent.

Hover-translation and focus-restore remain X11-only and are documented as such.

#### 6.6.2 Portal backend lifecycle

Interface facts that the implementation must honour (from
`/usr/share/dbus-1/interfaces/org.freedesktop.portal.GlobalShortcuts.xml`):

- `CreateSession(a{sv} options) → o request_handle`; the session path arrives later in
  the `Response` signal (`session_handle`, type `s` because of a historical typo).
- **An application can only attempt to bind shortcuts of a session once.**
- `BindShortcuts(o session, a(sa{sv}) shortcuts, s parent_window, a{sv} options)` —
  options only contain `handle_token`; the result is a **subset** of the requested
  shortcuts with `trigger_description` and `trigger_description`-derived properties.
  Empty and partial results are legal.
- `ListShortcuts(o session, a{sv} options)` reports what is currently bound.
- `Activated(o session, s shortcut_id, t timestamp, a{sv} options)` where
  `options["activation_token"]` is "a token that can be used to activate a window in
  response to the shortcut getting activated".
- `ConfigureShortcuts` exists only in interface **version 2**; `ShortcutsChanged`
  notifies about external changes.
- `org.freedesktop.portal.Request` requires subscribing to `Response` **before**
  invoking the method; a `handle_token` makes the handle predictable for that
  subscription.

State machine:

```text
UNSUPPORTED ──service present──▶ SESSION_CREATING ──Response(session_handle)──▶ SESSION_READY
SESSION_READY ──BindShortcuts(once)──▶ BINDING ──Response(subset)──▶ BOUND | PARTIAL | DENIED
BOUND/PARTIAL ──user edits in preferences──▶ new session (close old) ──▶ SESSION_CREATING
any state ──ServiceUnregistered / Response error / session closed──▶ UNAVAILABLE
shutdown ──Session.Close──▶ closed
```

Rules:

1. Exactly one bind attempt per session. Editing shortcuts closes the session and
   creates a new one; on a version-2 portal `ConfigureShortcuts` may be offered as the
   edit path instead.
2. No restore token is invented or persisted: the portal decides whether to show a
   confirmation dialog. `ListShortcuts` is used to display the currently bound
   triggers; `hotkeys.portalBindings` stores only what `BindShortcuts` reported, for
   UI display.
3. Activation tokens are forwarded: `activated(id, token)` carries
   `options["activation_token"]` to the consumer (§6.6.3).
4. Request cancellation, service disappearance, session closure and application exit
   are all handled states; each is logged once and reflected in the preferences UI.
5. `parent_window` uses the existing XDG window-identifier helper for the main window
   (and the empty string when none has been exported yet).

`tst_portal_hotkeys` runs against a fake portal on a private D-Bus session
(`dbus-daemon` started by the test) and covers: fast response, delayed response,
denial, partial binding, session closed mid-flight, second edit (new session), service
disappearance, and shutdown.

#### 6.6.3 Startup, platform choice, and activation

- Remove the forced `QT_QPA_PLATFORM=xcb` in `main.cc:261-268`. The application runs
  on its platform of choice, and the user's own `QT_QPA_PLATFORM` is never
  overwritten.
- `platformwindow.hh` provides:

```cpp
QScreen * screenForPoint( QPoint global );          // QGuiApplication::screenAt
QRect availableGeometryForPoint( QPoint global );
Qt::WindowFlags popupFlags();                       // Wayland: Tool|FramelessWindowHint|WindowStaysOnTopHint
                                                    // X11: existing flags incl. X11BypassWindowManagerHint
void applyActivation( QWidget * widget, QString const & xdgActivationToken );
```

- `applyActivation` applies the portal token and then asks for activation:
  with `HAVE_KWINDOWSYSTEM` it calls
  `KWindowSystem::setCurrentXdgActivationToken(token)` followed by
  `widget->windowHandle()->requestActivate()`; without it, it calls
  `requestActivate()` only. Activation is documented as **best-effort**: no compositor
  guarantees focus, and the design does not report a boolean "activated" result.
- Missing or expired tokens are ignored; the window is still raised and shown.

#### 6.6.4 Clipboard behaviour (no false fallback)

Three distinct behaviours exist today and stay distinct:

| Behaviour | Today | On Wayland without portal |
| --- | --- | --- |
| Global double-`Ctrl+C` shortcut | registered on `HotkeyWrapper` (`mainwindow.cc:3183`) with `enableClipboardHotkey` | **Unavailable.** It is a global hotkey, not clipboard polling; the design does not claim it survives |
| Clipboard-change scanning | `scanpopup.cc:533`, only when scanning is enabled | Unchanged (Qt clipboard notifications work) |
| Explicit lookup of the clipboard | tray menu / popup action, `scanpopup.cc:483` | Unchanged |

The preferences page explains, in the hotkey section, which of these is active for
the current backend.

### 6.7 Audio

- Keep `ffmpegaudio.cc` / `ffmpegaudioplayer.cc` (already updated for FFMpeg 8.0);
  only API-level fixes are expected.
- Delete `multimediaaudioplayer.*`, the `MAKE_QTMULTIMEDIA_PLAYER` plumbing and the
  `no_qtmultimedia_player` switch.
- Config migration (§9) normalizes the persisted backend name using the real stored
  values: `"Qt Multimedia"` → `InternalPlayerBackend::ffmpeg().uiName()`
  (`"FFmpeg+libao"`). Unknown names fall back to ffmpeg and are logged.
- `no_ffmpeg_player` remains a supported build switch; in that build the internal
  player is unavailable, the preferences UI hides the internal-player option, and
  external playback stays fully functional. The audio player factory keeps its
  existing fallback behaviour, and the configuration value is left untouched in that
  build.

## 7. Concurrency and threading

No structural change. `QThreadPool`, `QRunnable`, `QSemaphore`, `QMutex` and
`QAtomicInt` all exist in Qt 6. Mechanical fixes:

- `Mutex : public QMutex` constructed with `QMutex(Recursive)` → `QRecursiveMutex`
  (Qt 6 removed the constructor argument; `QMutex` is non-recursive). This single line
  accounted for 71 of the probe's compile errors.
- `QList<T*>::const_iterator` uses that relied on implicit `QList<T>` →
  `QList<const T>` conversion need explicit types, because Qt 6 unified
  `QList`/`QVector`.

The article bridge serialises script dispatch on the GUI thread; the request
interceptor runs on the Chromium IO thread and therefore only reads immutable
snapshots. Scheme handlers run on the GUI thread and own their `DataRequest`
connections there.

## 8. Testing strategy

### 8.1 Unit and fixture tests (`tests/`, `make check`)

| Test | Covers |
| --- | --- |
| `tst_encoding` | Byte-exact decode/encode fixtures (not just round-trips) for UTF-8/16/32 (with and without BOM), Latin-1, Shift-JIS, Big5, Big5-HKSCS, GB18030, GB2312, EUC-JP, Windows-1251, ISO-8859-1…10 and 13…16; invalid and truncated sequences → U+FFFD; `exUnrepresentable`; `forHtml` precedence (header > BOM > meta charset > http-equiv > XML declaration > fallback) |
| `tst_linkrewrite` | Every rule moved out of `stardict`/`zim`/`dictdfiles`/`dictserver`/`website`: `gdlookup` links, `bres`/`gico`/`gdau` prefixing, protocol-relative and absolute URLs untouched, multiline HTML (the dot/newline case) |
| `tst_regex` | Table-driven cases per converted pattern with pre-change expected outputs, including newline-in-dot, Unicode word/digit matches, empty pattern, anchored `exactMatch`, minimal matching, wildcard anchoring, and the `website.cc` RegExp2 pattern |
| `tst_article_schemes` | Registration succeeds for every scheme (non-empty `schemeByName`), percent-encoded hosts/paths survive `QUrl`, flags allow local access |
| `tst_article_load` | `gdlookup` document served from a fixture dictionary reaches rendered HTML with its `bres` resources resolved; `blank=1`; group/dictionaries/contexts parameters |
| `tst_article_bridge` | Active-article notification at document creation and after a dictionary switch; navigation during an in-flight script; page destruction with a queued callback; generation staleness |
| `tst_website_frames` | Same-origin fixture frame and http fixture frame: height measurement, reveal, click-text capture, late navigation |
| `tst_article_history` | A→B→back, repeated identical URLs, reload, forward-history truncation, scroll restore after content growth |
| `tst_portal_hotkeys` | Fake-portal lifecycle per §6.6.2 (fast/slow/denied/partial/closed/second edit/service loss/shutdown) |
| `tst_hotkeys` | Binding parser and matcher: sequence expansion, modifier normalisation, duplicates, portal single-combo restriction |
| `tst_htmlescape`, `tst_langcoder`, `tst_utf8_wstring` | Existing pure utilities |
| `tst_platformwindow` | Screen lookup for multi-screen points, popup flag policy per platform name (offscreen platform) |

WebEngine-dependent tests run under `xvfb-run -a` with
`QTWEBENGINE_CHROMIUM_FLAGS="--no-sandbox --disable-gpu"` and a bounded per-test
timeout. Excluded automatically when the `offscreen` platform is the only choice and
WebEngine cannot initialise — reported as skipped, never as passed.

### 8.2 Gates

1. **Build:** `qmake6 && make -j$(nproc)` → exit 0.
2. **Tests:** `make check` (root target, §6.1) → every testcase builds and passes.
3. **Qt6-only audit:** `scripts/check-qt6-only.sh` — greps the **compiled inputs**
   (`*.pro`, `*.pri`, `*.cc`, `*.hh`, `*.c`, `*.h`, `*.mm`, `*.ui`, `*.qrc` excluding
   `qtsingleapplication/`, `winlibs/`, `maclibs/`, `opencc/`) and the generated
   `Makefile` link line for `QRegExp`, `QTextCodec`, `QDesktopWidget`, `QX11Info`,
   `QWebView`, `QWebPage`, `QMediaContent`, `IS_QT_5`, `webkit`, `qt6-5compat`. It
   excludes `docs/` (including this design), `help/`, `locale/` and test fixture data,
   and publishes the exact command it runs.
4. **Headless startup smoke:** `timeout 30 xvfb-run -a ./goldendict` runs until the
   timeout kills it (exit 124) with no crash, no QtWebEngine initialisation error, and
   the startup log lines present. Supplemental only — it proves the process starts,
   not that an article rendered.
5. **Live Wayland gate (manual, recorded as an audit artifact):** start under Plasma 6
   Wayland and assert the log shows `platformName() == "wayland"`, the portal backend
   selected, a bound shortcut, an article rendered, a lookup performed, the scan popup
   raised by the portal shortcut, and audio played. A separate explicit
   `QT_QPA_PLATFORM=xcb` run validates the RECORD backend.

### 8.3 Manual parity checklist

Article rendering (StarDict/Babylon/MDict/BGL/DSL/website fixtures), word selection,
middle-click new tab, FTS highlight and navigation, find bar, zoom, context menu
actions, save article with resources, print and print preview, DevTools, audio
playback, history navigation with scroll restore, dictionary groups, preferences
dialogs, and the four hotkey preferences states.

## 9. Configuration migration

- `preferences.internalPlayerBackend == "Qt Multimedia"` → rewritten to `"FFmpeg+libao"`
  in memory on load, using `InternalPlayerBackend::ffmpeg().uiName()` so the stored
  format and save timing are unchanged; unknown values fall back to ffmpeg and log.
- New additive keys: `hotkeys.portalBindings` (what the portal reported, for display).
  No invented restore token.
- Hotkey strings keep the existing format; sequences that the portal cannot bind are
  preserved verbatim and reported as unavailable on Wayland.

## 10. Error handling and failure modes

| Failure | Behaviour |
| --- | --- |
| Unknown charset, invalid or truncated bytes | `decode()` substitutes U+FFFD and logs once; the lookup continues |
| Unrepresentable output character | `encode()` throws; the caller percent-encodes UTF-8 as before |
| Portal absent, denied, or interface version < 1 | Hotkeys disabled with an explanatory label; clipboard scanning and explicit clipboard lookup remain; logged once |
| `BindShortcuts` returns a subset or empty set | Bindings reported as unavailable; a new session is only created when the user edits the binding |
| Portal session closed / service disappears | Backend moves to `UNAVAILABLE`, bindings cleared, UI updated; no retry loop |
| Custom scheme cannot resolve a resource | `UrlNotFound` job failure and a log line; the rest of the article still renders |
| Article navigates while scripts are queued | Generation bump drops stale callbacks; the new document's scripts proceed |
| Save-article navigation/close during `toHtml` | Save aborted with a message; no partial file written |
| Print failure | `printFinished(false)` reported to the user |
| WebEngine missing at runtime | Startup fails with a clear message instead of a blank window |

## 11. Risks

| Risk | Mitigation |
| --- | --- |
| Regex conversion changes parser behaviour silently (248 sites, no prior tests) | Consolidate duplicates into `LinkRewrite`, convert one rule at a time, pin each with `tst_regex`/`tst_linkrewrite` using pre-change expectations; the rulebook's flags (`DotMatchesEverythingOption`, `UseUnicodePropertiesOption`) are applied per pattern, not globally |
| iconv mapping differs from the previous `QTextCodec` tables (esp. Big5-HKSCS, EUC-JP) | Byte-exact fixtures per charset in `tst_encoding`; unknown names degrade to UTF-8 exactly as today |
| WebEngine word-under-cursor JS rewrite changes selection semantics | Deterministic JS harness page with known text/geometry plus the manual checklist |
| Cross-origin website frames stay invisible if origin sharing fails | Same-origin `gdlookup` frame serving is implemented and tested **first** (spike before the rest of the article port) |
| Article resources fail through custom schemes (CORS, local access) | `tst_article_schemes` + `tst_article_load` gate scheme flags and serving before UI work lands |
| Portal UX differs from the existing recorder widget | Backend capability is reflected in the preferences UI; no binding is shown as active unless the portal reported it |
| Proxy/cache behaviour now spans two network stacks | Startup translation is explicit, restart requirement documented, proxy-auth wired for both stacks, cache/clear tested |
| Chromium dependency size | Documented in README requirements; not a code decision |

## 12. Documentation deliverables

- `README.md`: Qt 6 requirements, native-Wayland operation, `make check`, removed
  options (`no_qtmultimedia_player`), proxy restart note.
- `docs/superpowers/specs/2026-10-24-qt6-migration-design.md` (this document).
- `docs/wayland-limitations.md`: hover-translation, multi-key hotkey sequences, focus
  restore, `X-Frame-Options` of third-party subresources.
