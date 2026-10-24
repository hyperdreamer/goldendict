# GoldenDict Qt6 Migration — Design

- **Date:** 2026-10-24
- **Repository:** `hyperdreamer/goldendict` @ `b4f3fcdc` (branch `master`)
- **Topic slug:** `qt6-migration`
- **Target environment:** Arch Linux, KDE Plasma 6 on **Wayland**, Qt **6.12**, GCC 16

## 1. Background

This fork builds against Qt 4 or Qt 5 and uses QtWebKit for article rendering. The
target desktop environment is Plasma 6 on Wayland, where the Qt 5 build works only
through compatibility layers and QtWebKit is dead upstream. Qt 6 has no WebKit at
all, and Qt 6 removed or relocated a large part of the API surface this codebase
uses.

Measured starting state (all figures produced by running the tools, not estimated):

| Fact | Evidence |
| --- | --- |
| `qmake6` fails before compiling | `Project ERROR: Unknown module(s) in QT: webkitwidgets x11extras` |
| Legacy API surface, non-UI | After fixing only `mutex.hh`: **147 compile errors in 21 files** (`QChar(int)` ×66, `QString::midRef` ×37, `QRegExp` ×9, `QString::split/count/filter(QRegExp)` ×10, `QDateTime::toTime_t` ×4, `QTextStream::setCodec`, `QLayout::setMargin`, `QStringRef`, `findChild` static assert, `QList` iterator mismatch) |
| Qt-version conditionals | 406 `QT_VERSION` references in 55 files; 27 `IS_QT_5` uses; 13 Qt4-only branches |
| Regex surface | **248 `QRegExp` occurrences in 34 files** |
| Legacy encodings | 8 files with `QTextCodec`; web pages, EPWING (ISO8859-1/GB2312/EUC-JP), web-site encoding detection |
| Article view | `articleview.cc` is 93 KB; 33 synchronous `evaluateJavaScript` calls; 12 `QWebElement` uses; `hitTestContent`, `frameAt`, `QWebPage::FindFlags` incl. `HighlightAllOccurrences` |
| X11 coupling | `QX11Info` in 5 files; `hotkeywrapper.cc` uses `XRecordEnableContext`; `mainwindow.cc` X focus/`XSendEvent` |
| Screen geometry | `QDesktopWidget` in 4 files |
| Audio | `QMediaContent`/`setMedia` in `multimediaaudioplayer.cc`; ffmpeg player is the default backend |
| Tests | **None.** No `tests/` directory, no unit harness; `.travis.yml` is Qt5-era |
| Build system | qmake only (`.pro`), no CMake |

Environment facts that shape the design: `qt6-webengine 6.11.2`, `qt6-5compat 6.12.0`,
`qt6-multimedia`, `qt6-help`, `qt6-svg`, `qt6-tools` installed; `xdg-desktop-portal
1.22.1` + `xdg-desktop-portal-kde 6.8.1` + `kglobalaccel 6.31` present, including
`org.freedesktop.portal.GlobalShortcuts`; `hunspell 1.7.5`, `libopencc 1.4.1`,
`libeb 4.4.3`, `libzim 9.8.1`, ffmpeg/ao/vorbis present; `zstd` pkg-config missing;
`Xvfb`/`xvfb-run` available for headless verification.

## 2. Decisions

| ID | Decision | Rationale |
| --- | --- | --- |
| **B** | **Qt6-only, full modernization.** No Qt4/Qt5 compatibility layer, no `qt6-5compat` dependency. | Deprecated-but-working shims would leave the riskiest code (parser regexes, legacy encodings) frozen in a module that can disappear at any Qt release. |
| **B1** | **Full article-view parity** on QtWebEngine, including scan-popup word-under-cursor, FTS highlighting, find bar, zoom, context menu, print, DevTools. | The scan popup is the product's signature feature; QtWebEngine already ships printing and DevTools, so partial scope saves little. |
| **W2** | **Global shortcuts via XDG Desktop Portal `GlobalShortcuts`**, with the existing X11 RECORD backend retained for X11/XWayland sessions and automatic selection. Hover-translation stays X11-only. | The portal is the only sanctioned Wayland path for global shortcuts; the desktop provides it. |
| **P1** | **Keep qmake**, rewritten for Qt 6, Linux first-class. Windows/macOS blocks updated best-effort but not compiled or verified. | One build definition; CMake duplication is a follow-up project, not a migration prerequisite. |
| **T1** | **Add a Qt Test suite** for every pure-logic module the migration touches, plus build gates, an Xvfb startup smoke test, and a manual on-screen parity checklist. | Without it, converting 248 regexes and 8 codec sites is unverifiable. |
| **K4** | **Drop the QtMultimedia internal audio backend**; keep ffmpeg as the internal player and "external program" as-is. Remove the `MAKE_QTMULTIMEDIA_PLAYER` plumbing and the backend selector entry. | `qt6-multimedia` is an extra system dependency whose only unique value is a slower, less capable player; ffmpeg is already the required default and is compiled in. Existing configs that select the QtMultimedia backend migrate silently to ffmpeg. |

## 3. Goals

1. `qmake6 && make -j$(nproc)` produces a working `goldendict` binary on this machine
   with no `qt6-5compat` and no Qt5/Qt4 branches.
2. The application starts, renders articles, performs lookups, plays audio, and runs
   its scan popup under Plasma 6 Wayland.
3. Global shortcuts work on Wayland through the portal, and continue to work on X11
   through the existing RECORD implementation.
4. The article view preserves the WebKit-era behaviour set: word-under-cursor
   translation, FTS search with highlight-all, find bar, zoom, context menu actions,
   print, and the DevTools inspector.
5. Every migration-sensitive module (regex rewriting, encoding conversion, link
   rewriting, hotkey matching) is covered by automated tests that fail if the
   behaviour regresses.
6. No silent feature loss: anything that cannot be preserved on Wayland is disabled
   with a visible, explanatory UI state and documented.

## 4. Non-goals

- CMake migration; Windows/macOS build validation; parallel Qt5 support.
- Re-architecting the dictionary engines, indexing (`btreeidx`), or FTS indexing.
- Wayland hover-translation via portal `InputCapture` (a separate project with a
  different interaction model).
- Changing the on-disk config format beyond the additive migration described in §9.
- Rewriting golden-ratio UI layouts or visual restyling.

## 5. Architecture overview

The migration is organised in five layers, from least to most coupled to the
desktop:

```text
  ┌──────────────────────────────────────────────────────────────────┐
  │ 5. Desktop integration: hotkeys (X11 RECORD | portal), windows,  │
  │    screen geometry, activation                                    │
  ├──────────────────────────────────────────────────────────────────┤
  │ 4. Article view: QWebEngineView + custom schemes + JS bridge      │
  ├──────────────────────────────────────────────────────────────────┤
  │ 3. Platform-neutral services: Encoding (iconv/QStringDecoder),    │
  │    LinkRewrite, Regex migration, audio (ffmpeg)                   │
  ├──────────────────────────────────────────────────────────────────┤
  │ 2. Dictionary engines (unchanged algorithms, migrated API calls)  │
  ├──────────────────────────────────────────────────────────────────┤
  │ 1. Build: goldendict.pro for Qt 6, tests/ subprojects, docs       │
  └──────────────────────────────────────────────────────────────────┘
```

Each layer is independently testable: layers 1–3 build and run headless, layer 4
needs an Xvfb display, layer 5 needs a live session.

### 5.1 New and rewritten modules

| Module | Kind | Responsibility |
| --- | --- | --- |
| `encoding.hh/.cc` | new | Named character-set conversion and HTML/HTTP charset sniffing; replaces all `QTextCodec` use |
| `linkrewrite.hh/.cc` | new | The `gdlookup://`, `bres://`, `gdau://` HTML rewriting rules currently duplicated in `stardict.cc`, `zim.cc`, `dictdfiles.cc`, `dictserver.cc`, `website.cc` |
| `articlewebview.hh/.cc` | rewritten | `ArticleWebView : QWebEngineView` + `ArticleWebPage : QWebEnginePage` |
| `articlejs.hh/.cc` | new | JS bridge: serialised script dispatch, response routing, the embedded JS helper library |
| `articleschemes.hh/.cc` | new | `QWebEngineUrlScheme` registration and the `bres://` / `gdau://` scheme handlers, plus the request interceptor that replaces the per-origin host whitelist |
| `hotkeybackend.hh` | new | Abstract hotkey backend interface (`X11RecordBackend`, `PortalBackend`) |
| `portalhotkeys.cc/hh` | new | D-Bus client for `org.freedesktop.portal.GlobalShortcuts` |
| `platformwindow.hh/.cc` | new | Screen geometry (`QScreen`) and window-flag/activation policy per `QGuiApplication::platformName()` |
| `qt4x5.hh` | **deleted** | Its Qt4 branches and `IS_QT_5` helpers are no longer meaningful |
| `article_netmgr.cc/hh` | rewritten | Keeps `Dictionary::WebMultimediaDownload` (a standalone QNAM consumer) and the host whitelist data; loses the `QNetworkAccessManager` subresource override and `QWebSecurityOrigin` accounting |
| `multimediaaudioplayer.*` | **deleted** | Decision K4 |

## 6. Component design

### 6.1 Build configuration (`goldendict.pro`)

```qmake
TEMPLATE = app
TARGET = goldendict
VERSION = 1.5.1+git
CONFIG += c++17 exceptions rtti stl

QT += core gui widgets xml network svg sql printsupport help \
      webenginewidgets webenginecore
```

(`multimedia` is deliberately absent — decision K4 removes the QtMultimedia audio
backend, so the module is no longer needed.)

Rules:

- Remove every `QT_MAJOR_VERSION`/`QT_MINOR_VERSION` branch, the `webkit`,
  `webkitwidgets` and `x11extras` modules, and the Qt4 `else` arms.
- `CONFIG += c++17` is mandatory (Qt 6 headers require C++17).
- Add `SOURCES`/`HEADERS` entries for the new modules; drop the deleted ones.
- Keep the optional-feature switches (`zim_support`, `chinese_conversion_support`,
  `no_ffmpeg_player`, `old_hunspell`, `no_extra_tiff_handler`) — `no_qtmultimedia_player`
  is removed with K4.
- `zim_support` keeps `LIBS += -llzma -lzstd`; `libzstd` is present here (`zstd
  1.5.7`) but ships no `.pc` file, so detection must use `-l` linkage rather than
  `PKGCONFIG`.
- `tests/tests.pro` is a `TEMPLATE = subdirs` project; `make check` builds and runs
  every test.

### 6.2 `Encoding` — character conversion

Qt 6's `QStringConverter` supports **only** Unicode encodings and Latin-1; the
legacy encodings this program needs (Shift-JIS, Big5, Big5-HKSCS, GB18030, GB2312,
EUC-JP, ISO-8859-\*, Windows-1251) are *not* available there. `qt6-5compat`'s
`QTextCodec` would cover them but is excluded by decision B, so the program builds
its own conversion layer on **iconv**, which is already a mandatory dependency
wrapped by `Iconv`.

```cpp
// encoding.hh
class Encoding
{
public:
  explicit Encoding( QString const & name );     // throws exUnknownCodec

  QString decode( char const * data, size_t size ) const;
  QByteArray encode( QString const & text ) const;
  QString name() const;

  /// Codec aware of <meta charset> / http-equiv Content-Type / XML declaration,
  /// falling back to the given default.
  static Encoding html( QByteArray const & head, QString const & fallback = "UTF-8" );

  /// Codec named by a Content-Type header value, or nullptr when absent/unknown.
  static std::unique_ptr< Encoding > fromContentType( QByteArray const & contentType );

private:
  class Impl;
  std::shared_ptr< Impl > impl;
};
```

Behaviour contract:

1. `decode()` replaces invalid byte sequences with U+FFFD rather than throwing, so a
   malformed page never aborts a lookup.
2. FAST PATH: UTF-8, UTF-16LE/BE, UTF-32LE/BE and Latin-1 delegate to
   `QStringDecoder`.
3. `html()` inspects, in order: a `Content-Type` charset parameter, `<meta
   charset=…>`, `<meta http-equiv="Content-Type" content="…charset=…">`, and an XML
   declaration, matching the sequence `QTextCodec::codecForHtml` provided.
4. `encode()` throws only for characters unrepresentable in the target charset,
   which callers handle by falling back to UTF-8 percent-encoding.

Call sites converted: `website.cc` (page decode + the `%25GD1251%25`,
`%25GDBIG5%25`, `%25GDBIG5HKSCS%25`, `%25GDSHIFTJIS%25`, `%25GDGBK%25` and
`%25GDISO1%25`…`%25GDISO16%25` URL substitutions, where ISO 8859-11 and -12 are
skipped because those charsets do not exist), `epwing_book.cc` (ISO8859-1/GB2312/EUC-JP),
and the dictionary engines that currently reach for `QTextCodec` indirectly.

### 6.3 `LinkRewrite` — one owner for the article link rules

Four engines currently repeat near-identical regexes that rewrite dictionary HTML
into `gdlookup://` / `bres://` / `gdau://` form, with subtly different patterns
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
QString applyBres( QString const & html, std::string const & dictId );

/// Rewrites audio sources into gdau://<dictId>/<name>.
QString applyGdau( QString const & html, std::string const & dictId );
}
```

The exact patterns move here verbatim from their current homes and are then
converted to `QRegularExpression` once, with tests pinning the previous behaviour.

### 6.4 Regex migration rulebook (248 sites, 34 files)

`QRegExp` → `QRegularExpression`, with these mandatory rules:

| Case | Rule |
| --- | --- |
| Pattern syntax | QRegExp is a "wildcard-ish" hybrid; QRegularExpression is PCRE. `(?:…)`, lookarounds and `\p{…}` are now available, but anchors, `.` not matching newline, and greedy quantifiers behave identically. Each converted pattern is tested against the pre-change behaviour, not assumed equivalent. |
| Replacement strings | `QString::replace(QRegExp, repl)` → `QString::replace(QRegularExpression, repl)`. Back-references `\1` keep working; `capturedRef()` sites become `capturedView()`/`captured()`. |
| Iteration | `QRegExp::indexIn` loops → `QRegularExpressionMatchIterator`; `cap(n)` → `captured(n)`. |
| No string overloads | `QString::split/count/filter/indexOf/replace(QRegExp)` do not exist in Qt 6: use `QRegularExpression` overloads (`QString::split` takes one, `count` → loop over `globalMatch`, `filter` → `QStringList::filter` returns matches of a plain substring pattern, so rebuild with an explicit loop). |
| Empty pattern | `QRegExp()` default-constructed matches nothing; `QRegularExpression()` matches everything. Replace `isEmpty()` guards explicitly. |
| Case-insensitive flag | `Qt::CaseInsensitive` → `QRegularExpression::CaseInsensitiveOption`. |

### 6.5 Article view on QtWebEngine

#### Structure

```cpp
class ArticleWebPage : public QWebEnginePage
{
protected:
  bool acceptNavigationRequest( QUrl const & url, NavigationType type, bool isMainFrame ) override;
  void javaScriptConsoleMessage( JavaScriptConsoleMessageLevel, QString const & message,
                                 int line, QString const & source ) override;
  QWebEnginePage * createWindow( WebWindowType type ) override;
};

class ArticleWebView : public QWebEngineView   // keeps the existing public surface
{
  // event overrides preserved: mousePress/Release/DoubleClick, wheel, focusIn
  // signals: doubleClicked(QPoint)
  // setSelectionBySingleClick(), isMidButtonPressed()
};
```

`QWebPage::WebAction` call sites map to `QWebEnginePage::WebAction` (the enum values
`Copy`, `SelectAll`, `CopyLinkToClipboard`, `CopyImageToClipboard`, `InspectElement`
all exist). `page()->setLinkDelegationPolicy(...)` is replaced by
`acceptNavigationRequest()` returning `false` for `gdlookup://` main-frame
navigations after dispatching the lookup.

#### Custom schemes replace the QNetworkAccessManager override

QtWebEngine's Chromium stack never consults a `QNetworkAccessManager` for
subresources, so the three internal schemes become first-class engine schemes.

```cpp
// main(), BEFORE the QApplication instance is constructed
QWebEngineUrlScheme bres( "bres" );
bres.setSyntax( QWebEngineUrlScheme::Syntax::HostAndPort );
bres.setFlags( QWebEngineUrlScheme::LocalAccessAllowed | QWebEngineUrlScheme::CorsEnabled );
QWebEngineUrlScheme::registerScheme( bres );   // same for "gdau", "gdlookup"
```

- `bres://<dictId>/<path>` → `BresSchemeHandler` resolves through the existing
  `Dictionary::Request` plumbing and returns a `QWebEngineUrlSchemeHandler`-owned
  `QIODevice` wrapping the resource bytes.
- `gdau://<dictId>/<name>` → audio bytes; the article view keeps playing them through
  `AudioPlayerFactory`, so the handler returns the resource and the existing code path
  is unchanged.
- `gdlookup://localhost/<word>` is *navigation*, not a resource: intercepted in
  `acceptNavigationRequest`, where the URL is decoded into a word lookup exactly as
  `ArticleView::linkClicked` does today.

The per-origin host whitelist that `article_netmgr.cc` enforced through
`QWebSecurityOrigin` becomes `ArticleUrlRequestInterceptor : QWebEngineUrlRequestInterceptor`,
installed on the page with `setUrlRequestInterceptor`, which cancels requests to
remote hosts outside the whitelist and allows `bres`/`gdau`/local files.

#### Asynchronous JavaScript: one bridge, no scattered callbacks

The 33 synchronous `evaluateJavaScript` calls cannot become 33 nested callbacks
without destroying readability. Instead:

```cpp
// articlejs.hh
class ArticleJs : public QObject
{
public:
  /// Runs `script` after every previously queued script. The callback receives the
  /// JSON-ified result. Ordering is guaranteed; scripts never run concurrently.
  void run( QString const & script, std::function< void ( QJsonValue ) > callback = {} );

  /// Cancels all pending scripts and invalidates their callbacks (page navigation).
  void reset();
};
```

`reset()` is called from `ArticleWebView`'s `loadStarted` handler, so results that
belong to a previous article are dropped instead of corrupting the new one.

The DOM algorithms that WebKit exposed natively move into a bundled helper library
(`resources.qrc` → `:/article-helpers.js`, injected on `loadFinished`):

| Former WebKit API | Replacement |
| --- | --- |
| `frame->evaluateJavaScript("…")` returning a value | `ArticleJs::run(script, cb)` |
| `QWebElement::geometry()`, `render()` + `QImage` to find the word under the cursor | `gdWordAtPoint(x, y)` — pure JS using `document.elementFromPoint`, `Range.getClientRects`, and temporary `<span>` wrapping; returns the word string directly |
| `QWebElement::setInnerXml` / restore | folded into `gdWordAtPoint`'s save/restore |
| selection save/restore around FTS navigation | `gdSaveSelection()`, `gdRestoreSelection()` JS state |
| `QWebPage::FindFlags` + `HighlightAllOccurrences` | `gdHighlightAll(words, matchCase)` / `gdClearHighlights()` / `gdScrollToHit(i)` |
| `frameAt()` for iframes | `gdWordAtPoint` handles frames through `document.elementFromPoint` on the main frame; cross-origin frames are not supported (matches current behaviour, which also cannot read them) |
| `QWebHitTestResult` for the context menu | `QWebEngineView::lastContextMenuRequest()` (`QWebEngineContextMenuRequest`: `linkUrl`, `mediaUrl`, `selectedText`, `mediaType`), plus one JS call for the enclosing element's text |

`findText()` is retained only for the find bar; FTS keeps its own highlight index,
because QtWebEngine has no highlight-all mode.

#### Print and DevTools

- Print: `QWebEnginePage::printToPdf()` (the existing "Print" action is rewired; the
  WebKit print path used `QWebPage::print`).
- DevTools: `QWebEnginePage::setDevToolsPage()` shown in a separate window created on
  first use, triggered by `triggerAction(QWebEnginePage::InspectElement)`.

#### Zoom, audio, and article lifecycle

`setZoomFactor` replaces WebKit's `setTextSizeMultiplier`; the existing zoom actions,
progress indication, "back/forward" history (now `QWebEngineHistory`), and
`article-size` handling keep their behaviour. Audio links continue to route through
`AudioPlayerFactory` (ffmpeg), and the article view keeps its own
`WebMultimediaDownload` for "save image/audio".

### 6.6 Desktop integration

#### Hotkey backends

```cpp
// hotkeybackend.hh
struct HotkeyBinding
{
  QString id;             // "scan-popup", "show-main-window"
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
  void activated( QString const & id );
};

/// Chooses a backend once at startup; never silently falls back after a failure.
std::unique_ptr< HotkeyBackend > createHotkeyBackend( QObject * parent );
```

Selection rule (evaluated once at startup and logged):

1. **X11 platforms** (`platformName` starts with `xcb`): `X11RecordBackend`, which is
   the existing `HotkeyWrapper` code ported from `QX11Info::display()` to
   `QGuiApplication::nativeInterface< QNativeInterface::QX11Application >()->connection()`;
   the application root window comes from `DefaultRootWindow(connection)` instead of
   `QX11Info::appRootWindow()`.
2. **Wayland platforms**: `PortalBackend`. If `org.freedesktop.portal.Desktop` is
   absent or `CreateSession` is denied, the hotkey preferences page shows an
   explanatory message and the scan popup keeps working through its
   double-`Ctrl+C`/clipboard trigger.

Portal contract (`org.freedesktop.portal.GlobalShortcuts` 1.x):

```text
CreateSession(options{handle_token, session_handle_token}) -> Response(handle)
BindShortcuts(session, [(id, description, preferred_trigger)], parent_window,
              options{handle_token, shortcut_to_restore?}) -> Response(shortcuts)
Activated(session, id, timestamp, options) -> emit activated(id)
```

Behaviour rules:

- The portal accepts **single key combinations only**. Any configured hotkey that is
  a *sequence* (e.g. the default `Ctrl+C,Ctrl+C`) cannot be bound; the preferences
  page marks such bindings "not available on this desktop" and keeps the sequence
  editable for X11.
- The portal owns the actual key; the description returned by `BindShortcuts` and the
  saved-restore token are persisted in config so a later run re-binds the same combos
  without re-prompting.
- A missing/denied portal is a normal state, not an error dialog.

#### Screen geometry

`QDesktopWidget` disappears:

```cpp
// platformwindow.hh
QScreen * screenForPoint( QPoint global );
QRect availableGeometryForPoint( QPoint global );
```

Call sites: `mainwindow.cc` (restoring window position), `translatebox.cc`
(completer popup placement), `scanpopup.cc` and `scanflag.cc` (popup placement).

#### Window flags and activation

`Qt::X11BypassWindowManagerHint` is meaningless on Wayland and `QWidget::activateWindow()`
can be refused. `platformwindow.hh` exposes:

```cpp
Qt::WindowFlags popupFlags();       // Wayland: Tool | FramelessWindowHint | WindowStaysOnTopHint
                                    // X11:     existing flags including X11BypassWindowManagerHint
bool requestActivation( QWidget * ); // Wayland: QWindow::requestActivate(); X11: existing path
```

The `mainwindow.cc` X focus dance (`XGetInputFocus` / `XSendEvent`) is compiled only
for the X11 path and skipped on Wayland, where the compositor decides focus; the
"remember and restore previous focus" feature is documented as X11-only.

### 6.7 Audio

`ffmpegaudio.cc` and `ffmpegaudioplayer.cc` need only API-level fixes (`avcodec`
already updated by the recent FFMpeg 8.0 commit). `multimediaaudioplayer.*` and the
`MAKE_QTMULTIMEDIA_PLAYER`/`no_qtmultimedia_player` switches are deleted; config
values naming the QtMultimedia backend are rewritten to ffmpeg at load time (§9).

## 7. Concurrency and threading

No structural change. `QThreadPool`, `QRunnable`, `QSemaphore`, `QMutex` and
`QAtomicInt` all exist in Qt 6. Two mechanical fixes:

- `Mutex : public QMutex` constructed with `QMutex(Recursive)` → `QRecursiveMutex`
  (Qt 6 removed the constructor argument; `QMutex` is now non-recursive). This single
  line accounted for 71 of the probe's compile errors.
- `QList<T*>::const_iterator` uses that relied on implicit `QList<T>` →
  `QList<const T>` conversion need explicit types, because Qt 6 unified
  `QList`/`QVector`.

## 8. Testing strategy

### 8.1 Unit tests (`tests/`, `make check`)

| Test | Covers |
| --- | --- |
| `tst_encoding` | Round-trips for UTF-8/16/32, Latin-1, Shift-JIS, Big5, Big5-HKSCS, GB18030, GB2312, EUC-JP, Windows-1251, ISO-8859-1…10 and 13…16; invalid-sequence replacement; `Encoding::html()` precedence (header > meta charset > http-equiv > XML decl); `fromContentType` |
| `tst_linkrewrite` | Every rule moved out of `stardict`/`zim`/`dictdfiles`/`dictserver`: `gdlookup` link conversion, `bres://` prefixing of `src`/`href`/`url()`, `gdau://` audio, protocol-relative and absolute URLs left untouched |
| `tst_regex` | Table-driven cases for every converted pattern with pre-change expected outputs, including the `QRegExp`-vs-PCRE divergence corners found during conversion |
| `tst_htmlescape` | `htmlescape.cc` entities, existing behaviour |
| `tst_langcoder` | Language-name/code mapping tables |
| `tst_utf8_wstring` | `utf8.cc` / `wstring.cc` conversions |
| `tst_hotkeys` | Binding parser and matcher (sequence expansion, modifier normalisation, duplicate detection) with a fake backend |
| `tst_platformwindow` | Screen lookup selection for multi-screen point queries (offscreen platform) |

### 8.2 Gates

1. `qmake6 && make -j$(nproc)` → exit 0, no `qt6-5compat` in `lrelease`/link line.
2. `make check` → every test passes.
3. No `QRegExp`, `QTextCodec`, `QDesktopWidget`, `QX11Info`, `QWebView`, `QMediaContent`,
   `IS_QT_5` or `webkit` references anywhere in the tree (`grep` gate).
4. Headless startup smoke: `timeout 20 xvfb-run -a ./goldendict` runs until the
   timeout kills it (exit 124) with no `Segmentation fault`, no
   `Could not initialize`/`Failed to create` message from QtWebEngine, and the
   expected startup log lines present.
5. Live session check on Plasma 6 Wayland: application starts, an article renders,
   a lookup works, scan popup opens, portal shortcut activates it.

### 8.3 Manual parity checklist

Recorded as an audit artifact: article rendering (BBC/Babylon/MDict samples), word
selection, middle-click new tab, FTS highlight, find bar, zoom, print, DevTools,
audio playback, history navigation, dictionary groups, preferences dialogs.

## 9. Configuration migration

Existing `config` files must keep working.

- `preferences.internalPlayerBackend == "qtmultimedia"` → rewritten to `"ffmpeg"` on
  load, without touching the file until the user next saves.
- New keys are additive and defaulted: `hotkeys.portalRestoreToken`,
  `hotkeys.portalBindings`.
- Hotkey strings are stored in the existing format; portal-incompatible sequences are
  preserved verbatim and only reported as unavailable on Wayland.

## 10. Error handling and failure modes

| Failure | Behaviour |
| --- | --- |
| Unknown/unsupported charset in a page or dictionary | `Encoding` decodes with U+FFFD replacement and logs a warning; the lookup continues |
| Portal absent, denied, or version < 1 | Hotkeys disabled with an explanatory label; scan popup remains reachable via clipboard trigger; logged once |
| `BindShortcuts` returns fewer bindings than requested | Missing bindings are labelled unavailable; never retried in a loop |
| Custom scheme handler cannot resolve a resource | Returns a 404-equivalent empty reply and logs; the article still renders |
| JS helper missing/erroring | `ArticleJs` surfaces the console message and the affected feature degrades (e.g. word-under-cursor returns empty) rather than crashing |
| Article navigates while scripts are queued | `ArticleJs::reset()` drops stale callbacks |
| WebEngine not available at runtime (missing shared lib) | Startup fails with a clear message rather than a blank window |

## 11. Risks

| Risk | Mitigation |
| --- | --- |
| Regex conversion changes parser behaviour silently (248 sites, no prior tests) | Consolidate duplicates into `LinkRewrite`, convert one rule at a time, pin each with `tst_regex`/`tst_linkrewrite` using pre-change expectations |
| Legacy encodings differ between iconv and `QTextCodec` (esp. Big5-HKSCS, EUC-JP) | `tst_encoding` round-trips every charset the app names; keep the codec-name mapping table explicit |
| WebEngine word-under-cursor JS rewrite changes selection semantics | Manual checklist on three article formats plus a deterministic JS behaviour test harness page |
| Article resources fail through custom schemes (CORS, local access) | Scheme flags verified early in implementation (spike before the rest of the port) |
| Portal shortcut UX differs from the existing recorder widget | Preferences pages show backend-specific capability and never pretend a binding is active when it is not |
| Chromium dependency size/vendor policy | Documented in README requirements; not a code decision |

## 12. Documentation deliverables

- `README.md`: Qt 6 requirements, Wayland notes, `make check`, removed options
  (`no_qtmultimedia_player`, Qt5 build instructions).
- `docs/superpowers/specs/2026-10-24-qt6-migration-design.md` (this document).
- A short `docs/wayland-limitations.md` listing the deliberate platform gaps
  (hover-translation, multi-key sequences, focus restore).
