/* Real Chromium startup, DOM and asynchronous retirement contracts. */
#include <QtTest>
#include <QApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QClipboard>
#include <QCheckBox>
#include <QFileDialog>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QNetworkProxy>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWebChannel>
#include <QWebEngineFindTextResult>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <limits>

#include "articlebridge.hh"
#include "articleprofile.hh"
#include "articlewebview.hh"
#include "articleinspector.hh"
#include "articleview.hh"
#include "websiteframes.hh"
#include "articleschemes.hh"
#include "article_netmgr.hh"
#include "articleurls.hh"
#include "support/dictionary_stub.hh"
#include "support/fixtures.hh"

namespace {
#define WAIT_FOR(condition) \
    QVERIFY2(QTest::qWaitFor([&] { return bool(condition); }, int(qMax<qint64>(0, deadline.remainingTime()))), #condition)

struct Completion
{
    ArticleBridge::Result callback;
    QVariant value;
    void deliver() const { callback(value); }
};
QMap<QString, Completion> held;
QStringList dispatched;
bool holdInteraction = false;
QList<Completion> interactions;

// Native find results are real engine values. Production callbacks for a
// selected query (or a selected production script) may be held and delivered
// after a newer query/document exists; the value itself is always the one the
// engine produced for that exact call.
struct FindCompletion
{
    std::function<void(QWebEngineFindTextResult const &)> callback;
    void deliver(QWebEngineFindTextResult const &value) const { callback(value); }
};
bool holdFind = false;
QString holdFindQuery;
QStringList findQueries;
QMap<QString, QWebEngineFindTextResult> findResults;
QMap<QString, FindCompletion> heldFind;
QString holdScriptMarker;
QList<Completion> heldScripts;
QList<Completion> &heldHighlights = heldScripts;

void observeFind(QString const &query, QWebEnginePage::FindFlags const &flags,
                 std::function<void(QWebEngineFindTextResult const &)> const &callback,
                 std::function<void(std::function<void(QWebEngineFindTextResult const &)> const &)> const &execute)
{
    if (query.isEmpty() || !callback) { execute(callback); return; }
    Q_UNUSED(flags)
    findQueries.append(query);
    if (holdFind && query == holdFindQuery) {
        heldFind.insert(query, FindCompletion{callback});
        return;
    }
    execute([query, callback](QWebEngineFindTextResult const &value) {
        findResults.insert(query, value);
        callback(value);
    });
}

// Link wrapping affects only the production call sites in this executable. It
// still evaluates on the real page; only delivery of tagged results is held.
void observe(QString const &script, ArticleBridge::Result const &callback,
             std::function<void(ArticleBridge::Result)> const &execute)
{
    if (holdInteraction && script.contains("document.elementFromPoint(")) {
        execute([callback](QVariant const &value) { interactions.append({callback, value}); });
        return;
    }
    if (!holdScriptMarker.isEmpty() && script.contains(holdScriptMarker)) {
        execute([callback](QVariant const &value) { heldScripts.append({callback, value}); });
        return;
    }
    if (!script.startsWith("/*t19:")) { execute(callback); return; }
    auto label = script.mid(6, script.indexOf("*/") - 6);
    dispatched.append(label);
    if (label.startsWith("hold-"))
        execute([callback, label](QVariant const &value) { held.insert(label, {callback, value}); });
    else execute(callback);
}

QString quote(QString const &value)
{
    auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}
QVariant evaluate(QWebEnginePage &page, QString const &script, QDeadlineTimer const &deadline)
{
    struct Answer { bool done = false; QVariant value; };
    auto result = std::make_shared<Answer>();
    page.runJavaScript(script, [result](QVariant const &value) { result->value = value; result->done = true; });
    if (!QTest::qWaitFor([&] { return result->done; }, int(qMax<qint64>(0, deadline.remainingTime()))))
        QTest::qFail("real page JavaScript deadline", __FILE__, __LINE__);
    return result->value;
}
QString document(QString const &body = {})
{
    return QStringLiteral(R"HTML(<!doctype html><html><head><meta charset="utf-8">
<script>
window.gdArticleContents = ['dict-a','dict-b','absent'];
window.gdCurrentArticle = 'gdfrom-dict-a';
window.gdAudioLinks = []; gdAudioLinks.first = 'gdau://dict-a/first';
window.parserFacade = !!window.articleview;
window.parserHelpers = ['gdWordAtPoint','gdSaveSelection','gdRestoreSelection',
 'gdHighlightAll','gdClearHighlights','gdScrollToHit','gdFrameRect','gdMeasureFrame',
 'gdCaptureState','gdRestoreArticleState'].every(name => typeof window[name] === 'function');
if (window.articleview) {
 articleview.onJsActiveArticleChanged('gdfrom-absent');
 articleview.onJsActiveArticleChanged('gdfrom-dict-b');
 articleview.onJsActiveArticleChanged('gdfrom-dict-a');
}
</script></head><body style="margin:0;font:20px sans-serif">
<div class="gdactivearticle" id="gdfrom-dict-a"><p id="word">rock’<b>n</b>’roll 中文 é alpha</p></div>
<div id="gdfrom-dict-b"><p id="second">Second article</p></div>
%1</body></html>)HTML").arg(body);
}
struct Browser
{
    Config::Class cfg;
    sptr<TestSupport::FixtureDictionary> dictionary{new TestSupport::FixtureDictionary};
    std::vector<sptr<Dictionary::Class>> dictionaries{dictionary};
    std::vector<Instances::Group> groups;
    ArticleMaker maker{dictionaries, groups, {}, {}};
    ArticleNetworkAccessManager resources{nullptr, dictionaries, maker,
        cfg.preferences.disallowContentFromOtherSites, cfg.preferences.hideGoldenDictHeader};
    std::unique_ptr<ArticleProfileController> controller;
    Browser()
    {
        cfg.preferences.maxNetworkCacheSize = 0;
        cfg.preferences.disallowContentFromOtherSites = false;
        controller = std::make_unique<ArticleProfileController>(cfg, resources, nullptr);
    }
    QUrl article()
    {
        dictionary->setArticleBody("<p id='word'>parser-time article</p>");
        ArticleUrls::Lookup lookup;
        lookup.phrase = Config::InputPhrase::fromPhrase(QStringLiteral("bridge"));
        lookup.group = 0;
        return ArticleUrls::lookupDocument(lookup);
    }
};
struct InteractionFixture
{
    struct Player final : AudioPlayerInterface {
        QString play(char const *, int) override { return {}; }
        void stop() override {}
    };
    Config::Class cfg;
    sptr<TestSupport::FixtureDictionary> a{new TestSupport::FixtureDictionary};
    sptr<TestSupport::FixtureDictionary> b{new TestSupport::FixtureDictionary("dict-b", "Dictionary B")};
    std::vector<sptr<Dictionary::Class>> dictionaries{a,b};
    Instances::Groups groups;
    ArticleMaker maker{dictionaries, groups, {}, {}};
    ArticleNetworkAccessManager resources{nullptr, dictionaries, maker,
        cfg.preferences.disallowContentFromOtherSites, cfg.preferences.hideGoldenDictHeader};
    AudioPlayerPtr player{new Player};
    QAction search;
    std::unique_ptr<ArticleProfileController> controller;
    InteractionFixture() {
        cfg.preferences.maxNetworkCacheSize = 0;
        cfg.preferences.disallowContentFromOtherSites = false;
        cfg.preferences.doubleClickTranslates = true;
        Instances::Group group("Interaction group");
        group.id = 17; group.dictionaries = dictionaries; groups.push_back(group);
        a->setArticleBody("<p id='click-word' style='font:24px monospace'>alpha</p>"
                          "<p id='spaces'> &nbsp; &#x2003; </p>");
        b->setArticleBody("<a id='context-link' href='bword:copied-link'>copied link</a>"
                          "<div id='background' style='width:110px;height:45px;background-image:url(bres://dict-b/fixture.bin)'></div>"
                          "<img id='image' src='bres://dict-b/fixture.png' width='100' height='45'>");
        b->setResources({{"fixture.bin", QByteArray("resource-body")}, {"fixture.png", QByteArray::fromBase64(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a4KcAAAAASUVORK5CYII=")}});
        controller = std::make_unique<ArticleProfileController>(cfg, resources, nullptr);
    }
    std::unique_ptr<ArticleView> view() {
        auto result = std::make_unique<ArticleView>(nullptr, *controller->profile(), resources, player,
                                                   dictionaries, groups, false, cfg, search);
        result->resize(800,600); result->show(); result->showDefinition("initial",17);
        return result;
    }
};
QPoint point(QWebEnginePage &page, QString const &id, QDeadlineTimer const &deadline) {
    auto result = evaluate(page, "(() => {const e=document.getElementById(" + quote(id) +
        ");e.scrollIntoView({block:'center'});const r=e.getBoundingClientRect();"
        "return [r.x+Math.min(r.width/2,25),r.y+r.height/2];})()", deadline).toList();
    return result.size() == 2 ? QPoint(qRound(result[0].toDouble()),qRound(result[1].toDouble())) : QPoint();
}
bool focusAt(ArticleWebView &web, QPoint const &position, QDeadlineTimer const &deadline) {
    QApplication::setActiveWindow(web.window()); web.setFocus();
    evaluate(*web.page(), "window.__gdTestClicked=false;document.addEventListener('click',"
        "()=>window.__gdTestClicked=true,{once:true})",deadline);
    QTest::mouseClick(web.focusProxy(),Qt::LeftButton,Qt::NoModifier,position);
    return QTest::qWaitFor([&] { return evaluate(*web.page(),"window.__gdTestClicked && document.hasFocus()",deadline).toBool(); },
        int(qMax<qint64>(0,deadline.remainingTime())));
}
QAction *action(QMenu *menu, QString const &text) {
    if (menu) for (auto a : menu->actions()) if (a->text() == text) return a;
    return nullptr;
}
QMenu *visibleMenu(ArticleView &view) {
    for (auto menu : view.findChildren<QMenu*>()) if (menu->isVisible()) return menu;
    return nullptr;
}
// The find/FTS bars are the retained ArticleView surface: their widgets are
// addressed by object name, never by a new view API.
QLineEdit *searchLine(ArticleView &view) { return view.findChild<QLineEdit*>("searchText"); }
QToolButton *searchTool(ArticleView &view, QString const &name) { return view.findChild<QToolButton*>(name); }
QFrame *ftsSearchFrame(ArticleView &view) { return view.findChild<QFrame*>("ftsSearchFrame"); }
QLabel *ftsSearchLabel(ArticleView &view) { return view.findChild<QLabel*>("ftsSearchStatusLabel"); }
QString statusText(QSignalSpy const &spy)
{
    return spy.isEmpty() ? QString() : spy.last().at(0).toString();
}
void typeQuery(QLineEdit *line, QString const &text)
{
    QTest::keyClick(line, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(line, text);
}
// Marks are enumerated in document order, which is the navigation order.
QStringList markTexts(QWebEnginePage &page, QDeadlineTimer const &deadline)
{
    return evaluate(page, "Array.from(document.querySelectorAll('mark[data-gd-highlight]'))"
                          ".map(mark => mark.textContent)", deadline).toStringList();
}
// The active match is also the selection, so a finished FTS state is the selection
// plus every mark in document order. The count label and the hit activation are
// written by two serialized operations, so no single instant describes it.
QString finishedFtsState(QWebEnginePage &page, QDeadlineTimer const &deadline)
{
    return evaluate(page, R"JS((() => {
        const marks = Array.from(document.querySelectorAll('mark[data-gd-highlight]'))
            .map(mark => mark.textContent).join(',');
        return getSelection().toString() + '|' + marks;
      })())JS", deadline).toString();
}
QList<int> markOffsets(QString const &rootId, QWebEnginePage &page, QDeadlineTimer const &deadline)
{
    auto values = evaluate(page, "(() => {const root=document.getElementById(" + quote(rootId) + ");"
        "return Array.from(root.querySelectorAll('mark[data-gd-highlight]')).map(mark => {"
        "const range=document.createRange();range.selectNodeContents(root);range.setEndBefore(mark);"
        "return range.toString().length;});})()", deadline).toList();
    QList<int> offsets;
    for (auto const &value : values) offsets.append(value.toInt());
    return offsets;
}
// A lone surrogate is corrupted text, not a mark boundary. Returns false when
// the document text or any mark contains one.
bool unicodeScalarsIntact(QWebEnginePage &page, QDeadlineTimer const &deadline)
{
    return evaluate(page, R"JS((() => {
        const intact = text => {
          for (let i = 0; i < text.length; ++i) {
            const unit = text.charCodeAt(i);
            if (unit >= 0xd800 && unit <= 0xdbff) {
              if (i + 1 >= text.length) return false;
              const low = text.charCodeAt(++i);
              if (low < 0xdc00 || low > 0xdfff) return false;
            } else if (unit >= 0xdc00 && unit <= 0xdfff) return false;
          }
          return true;
        };
        return [document.body.textContent, ...Array.from(
          document.querySelectorAll('mark[data-gd-highlight]')).map(mark => mark.textContent)].every(intact);
      })())JS", deadline).toBool();
}
// The shipped article sheet colours the find/FTS selection, which is the active
// match among the highlighted ones.
bool selectionRulePresent(QWebEnginePage &page, QDeadlineTimer const &deadline)
{
    return evaluate(page, R"JS((() => {
        for (const sheet of Array.from(document.styleSheets)) {
          let rules; try { rules = Array.from(sheet.cssRules); } catch (_) { continue; }
          for (const rule of rules)
            if ((rule.selectorText || '').indexOf('::selection') >= 0 &&
                (rule.style.backgroundColor === 'rgb(204, 0, 0)' ||
                 rule.style.background === 'rgb(204, 0, 0)')) return true;
        }
        return false;
      })())JS", deadline).toBool();
}
}

// Exact GNU ld wrappers for the Qt 6.12 page/frame overloads used by the bridge.
extern "C" void realPageRun(QWebEnginePage *, QString const &, quint32, ArticleBridge::Result const &)
    asm("__real__ZN14QWebEnginePage13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE");
extern "C" void wrapPageRun(QWebEnginePage *, QString const &, quint32, ArticleBridge::Result const &)
    asm("__wrap__ZN14QWebEnginePage13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE");
extern "C" void wrapPageRun(QWebEnginePage *page, QString const &script, quint32 world,
                            ArticleBridge::Result const &callback)
{
    observe(script, callback, [&](ArticleBridge::Result cb) { realPageRun(page, script, world, cb); });
}
extern "C" void realFrameRun(QWebEngineFrame *, QString const &, quint32, ArticleBridge::Result const &)
    asm("__real__ZN15QWebEngineFrame13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE");
extern "C" void wrapFrameRun(QWebEngineFrame *, QString const &, quint32, ArticleBridge::Result const &)
    asm("__wrap__ZN15QWebEngineFrame13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE");
extern "C" void wrapFrameRun(QWebEngineFrame *frame, QString const &script, quint32 world,
                             ArticleBridge::Result const &callback)
{
    observe(script, callback, [&](ArticleBridge::Result cb) { realFrameRun(frame, script, world, cb); });
}

// The asynchronous find overload is the production call site for both bars.
extern "C" void realFindText(QWebEnginePage *, QString const &, QWebEnginePage::FindFlags,
                             std::function<void(QWebEngineFindTextResult const &)> const &)
    asm("__real__ZN14QWebEnginePage8findTextERK7QString6QFlagsINS_8FindFlagEERKSt8functionIFvRK24QWebEngineFindTextResultEE");
extern "C" void wrapFindText(QWebEnginePage *, QString const &, QWebEnginePage::FindFlags,
                             std::function<void(QWebEngineFindTextResult const &)> const &)
    asm("__wrap__ZN14QWebEnginePage8findTextERK7QString6QFlagsINS_8FindFlagEERKSt8functionIFvRK24QWebEngineFindTextResultEE");
extern "C" void wrapFindText(QWebEnginePage *page, QString const &query, QWebEnginePage::FindFlags flags,
                             std::function<void(QWebEngineFindTextResult const &)> const &callback)
{
    observeFind(query, flags, callback, [&](std::function<void(QWebEngineFindTextResult const &)> const &cb) {
        realFindText(page, query, flags, cb);
    });
}

class ArticleBridgeTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        TestSupport::installWatchdog(TestSupport::BrowserWatchdogSeconds);
        qputenv("GD_TEST_REQUIRED_PREREQUISITES", "display:webengine");
        QStringList reasons;
        QVERIFY2(TestSupport::requiredEnvironment(&reasons), qPrintable(reasons.join('\n')));
    }
    void cleanup()
    {
        holdInteraction = false; interactions.clear(); held.clear(); dispatched.clear();
        holdFind = false; holdFindQuery.clear(); findQueries.clear(); findResults.clear(); heldFind.clear();
        holdScriptMarker.clear(); heldScripts.clear();
    }
    void cleanupTestCase() { TestSupport::clearWatchdog(); }

    void doubleClickKeepsPressModifiersAfterDelivery()
    {
        QDeadlineTimer deadline(12000);
        InteractionFixture f; auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy tabs(view.get(), &ArticleView::showDefinitionInNewTab);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        QPoint p = point(*web->page(), "click-word", deadline);
        holdInteraction = true;
        QTest::mouseDClick(web->focusProxy(), Qt::LeftButton, Qt::ControlModifier, p);
        QTest::mouseRelease(web->focusProxy(), Qt::LeftButton, Qt::NoModifier, p);
        WAIT_FOR(!interactions.isEmpty());
        QVERIFY(!(QApplication::keyboardModifiers() & Qt::ControlModifier));
        holdInteraction = false; auto saved = interactions.takeFirst(); saved.deliver();
        WAIT_FOR(tabs.size() == 1);
        QCOMPARE(tabs.at(0).at(0).toString(), QString("alpha"));
        QCOMPARE(tabs.at(0).at(1).toUInt(), 17u);
        QCOMPARE(tabs.at(0).at(2).toString(), QString("gdfrom-dict-a"));
        QCOMPARE(view->getPhrase().phrase, QString("initial"));
    }
    void altClickAndWhitespaceSelection()
    {
        QDeadlineTimer deadline(12000);
        InteractionFixture f; f.cfg.preferences.doubleClickTranslates = false;
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        QPoint p = point(*web->page(), "click-word", deadline);
        QTest::mouseClick(web->focusProxy(), Qt::LeftButton, Qt::AltModifier, p);
        WAIT_FOR(view->getPhrase().phrase == "alpha");
        WAIT_FOR(pages.size() == 2);
        evaluate(*web->page(), "(() => {const r=document.createRange();r.selectNodeContents(document.getElementById('spaces'));"
            "const s=getSelection();s.removeAllRanges();s.addRange(r);})()", deadline);
        WAIT_FOR(web->hasSelection());
        QCOMPARE(web->selectedText().trimmed(), QString());
        f.cfg.preferences.doubleClickTranslates=true;
        QSignalSpy tabs(view.get(),&ArticleView::showDefinitionInNewTab);
        auto coordinates=evaluate(*web->page(),"(() => {const r=getSelection().getRangeAt(0).getBoundingClientRect();"
            "return [r.x+r.width/2,r.y+r.height/2];})()",deadline).toList();
        p=QPoint(qRound(coordinates.at(0).toDouble()),qRound(coordinates.at(1).toDouble())); holdInteraction=true;
        view->doubleClicked(p,Qt::ControlModifier);
        WAIT_FOR(!interactions.isEmpty()); holdInteraction=false; auto whitespace=interactions.takeFirst();
        QVERIFY(!whitespace.value.toMap().value("selection").toString().isEmpty());
        QVERIFY(whitespace.value.toMap().value("selection").toString().trimmed().isEmpty()); whitespace.deliver();
        QCOMPARE(tabs.size(),0); QCOMPARE(view->getPhrase().phrase,QString("alpha"));
        web->bridge->reset();
        QVERIFY(!web->hasSelection());
        QCOMPARE(web->selectedText(), QString());
    }

    void selectionClickDragAndGeneration()
    {
        QDeadlineTimer deadline(15000);
        InteractionFixture f; f.cfg.preferences.doubleClickTranslates = false;
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        QVERIFY(!view->isFramedArticle("dict-a"));
        view->setSelectionBySingleClick(true);
        QPoint p = point(*web->page(), "click-word", deadline);
        QTest::mouseClick(web->focusProxy(), Qt::LeftButton, Qt::NoModifier, p);
        WAIT_FOR(web->selectedText() == "alpha");
        QVERIFY(web->hasSelection());
        QCOMPARE(view->getPhrase().phrase, QString("initial"));
        evaluate(*web->page(), "getSelection().removeAllRanges()", deadline);
        WAIT_FOR(!web->hasSelection());
        auto begin = p - QPoint(24,0), end = p + QPoint(30,0);
        QTest::mousePress(web->focusProxy(), Qt::LeftButton, Qt::NoModifier, begin);
        QMouseEvent move(QEvent::MouseMove, end, web->focusProxy()->mapToGlobal(end),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(web->focusProxy(), &move);
        QTest::mouseRelease(web->focusProxy(), Qt::LeftButton, Qt::NoModifier, end);
        WAIT_FOR(web->hasSelection());
        QVERIFY(web->selectedText().contains("alp"));
        QSignalSpy doubles(web, &ArticleWebView::doubleClicked);
        evaluate(*web->page(), "document.getElementById('spaces').textContent='next-word'", deadline);
        web->bridge->reset();
        QCOMPARE(web->selectedText(), QString()); QVERIFY(!web->hasSelection());
        // A synthetic selection posted by release is stamped before the reset.
        QTest::mouseClick(web->focusProxy(), Qt::LeftButton, Qt::NoModifier, p);
        web->bridge->reset();
        auto barrier = std::make_shared<bool>(false);
        QTimer::singleShot(0, web, [barrier] { *barrier=true; }); WAIT_FOR(*barrier);
        QCOMPARE(doubles.size(), 0);
    }
    void contextCopiesLinkAndGuardsEveryAction()
    {
        QDeadlineTimer deadline(18000);
        InteractionFixture f; auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy requests(web, &QWidget::customContextMenuRequested);
        QSignalSpy tabs(view.get(), &ArticleView::openLinkInNewTab);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        QPoint p = point(*web->page(), "context-link", deadline);
        holdInteraction = true;
        QTest::mouseClick(web->focusProxy(), Qt::RightButton, Qt::NoModifier, p);
        WAIT_FOR(!requests.isEmpty() && !interactions.isEmpty());
        evaluate(*web->page(), "document.getElementById('context-link').href='bword:changed-link'", deadline);
        holdInteraction = false;
        auto copied = interactions.takeFirst(); auto alternate = copied.value.toMap();
        alternate["link"] = QString("bword:late-js-result"); copied.value = alternate; copied.deliver();
        WAIT_FOR(visibleMenu(*view));
        auto menu = visibleMenu(*view);
        QVERIFY(action(menu,"Copy Link")); QVERIFY(action(menu,"Open Link in New Tab"));
        action(menu,"Copy Link")->trigger();
        QCOMPARE(QApplication::clipboard()->text(), QString("bword:copied-link"));
        action(menu,"Open Link in New Tab")->trigger();
        QCOMPARE(tabs.size(),1);
        QCOMPARE(tabs.at(0).at(0).toUrl(), QUrl("bword:copied-link"));
        QCOMPARE(tabs.at(0).at(2).toString(), QString("gdfrom-dict-b"));
        QCOMPARE(QUrlQuery(tabs.at(0).at(1).toUrl()).queryItemValue("word"), QString("initial"));
        WAIT_FOR(view->getActiveArticleId() == "dict-b");
        // Retain actions as if trigger delivery was already queued when the menu
        // closed. They must each reject the new generation on their own.
        menu->setAttribute(Qt::WA_DeleteOnClose,false);
        view->showDefinition("new-document",17);
        WAIT_FOR(pages.size() == 2);
        QApplication::clipboard()->setText("new-page-clipboard");
        int const before = tabs.size();
        action(menu,"Copy Link")->trigger(); action(menu,"Select All")->trigger();
        action(menu,"Open Link in New Tab")->trigger(); action(menu,"Search")->trigger();
        auto barrier = std::make_shared<bool>(false);
        web->bridge->run("true",[barrier](QVariant const &) { *barrier=true; }); WAIT_FOR(*barrier);
        QCOMPARE(QApplication::clipboard()->text(),QString("new-page-clipboard"));
        QVERIFY(!web->hasSelection()); QVERIFY(!view->isSearchOpened()); QCOMPARE(tabs.size(),before);
        delete menu;
    }
    void contextSelectionLookupCopyAndSearch()
    {
        QDeadlineTimer deadline(15000);
        InteractionFixture f; auto view=f.view(); auto web=view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy tabs(view.get(), &ArticleView::showDefinitionInNewTab);
        QSignalSpy input(view.get(), &ArticleView::sendWordToInputLine);
        WAIT_FOR(!pages.isEmpty());
        QPoint p=point(*web->page(),"click-word",deadline);
        QVERIFY(focusAt(*web,p,deadline));
        evaluate(*web->page(),"(() => {const r=document.createRange();r.selectNodeContents(document.getElementById('click-word'));"
            "const s=getSelection();s.removeAllRanges();s.addRange(r);})()",deadline);
        WAIT_FOR(web->selectedText()=="alpha");
        QTest::mouseClick(web->focusProxy(),Qt::RightButton,Qt::NoModifier,p);
        WAIT_FOR(visibleMenu(*view)); auto menu=visibleMenu(*view);
        QVERIFY(action(menu,"Copy")); QVERIFY(action(menu,"Look up in New Tab"));
        evaluate(*web->page(),"getSelection().removeAllRanges()",deadline); WAIT_FOR(!web->hasSelection());
        action(menu,"Copy")->trigger(); QCOMPARE(QApplication::clipboard()->text(),QString("alpha"));
        action(menu,"Look up in New Tab")->trigger(); QCOMPARE(tabs.size(),1);
        QCOMPARE(tabs.at(0).at(0).toString(),QString("alpha"));
        action(menu,"Send to input line")->trigger(); QCOMPARE(input.size(),1);
        QCOMPARE(input.at(0).at(0).toString(),QString("alpha"));
        action(menu,"Search")->trigger(); QVERIFY(view->isSearchOpened());
        menu->close();
    }
    void contextResourceAndBackgroundSave()
    {
        QDeadlineTimer deadline(15000);
        InteractionFixture f; auto view=f.view(); auto web=view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        WAIT_FOR(!pages.isEmpty());
        QPoint p=point(*web->page(),"background",deadline);
        QTest::mouseClick(web->focusProxy(),Qt::RightButton,Qt::NoModifier,p);
        WAIT_FOR(visibleMenu(*view)); auto menu=visibleMenu(*view);
        QVERIFY(action(menu,"Save background..."));
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        action(menu,"Save background...")->trigger();
        WAIT_FOR(view->findChild<QFileDialog*>() && view->findChild<QFileDialog*>()->isVisible());
        auto dialog=view->findChild<QFileDialog*>();
        QCOMPARE(dialog->selectedFiles().value(0).section('/',-1),QString("fixture.bin"));
        QTemporaryDir output; QVERIFY(output.isValid());
        auto destination=output.filePath("saved.bin");
        QVERIFY(QMetaObject::invokeMethod(dialog,"fileSelected",Qt::DirectConnection,Q_ARG(QString,destination)));
        QFile file(destination); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),QByteArray("resource-body"));
        dialog->reject(); menu->close();
        WAIT_FOR(!visibleMenu(*view));
        p=point(*web->page(),"image",deadline);
        QTest::mouseClick(web->focusProxy(),Qt::RightButton,Qt::NoModifier,p);
        WAIT_FOR(visibleMenu(*view)); menu=visibleMenu(*view);
        QVERIFY(action(menu,"Save resource..."));
        menu->setAttribute(Qt::WA_DeleteOnClose,false);
        action(menu,"Save resource...")->trigger();
        WAIT_FOR(view->findChild<QFileDialog*>() && view->findChild<QFileDialog*>()->isVisible());
        QPointer<QFileDialog> pending=view->findChild<QFileDialog*>();
        pending->setAttribute(Qt::WA_DeleteOnClose,false);
        QCOMPARE(pending->selectedFiles().value(0).section('/',-1),QString("fixture.png"));
        auto imagePath=output.filePath("saved.png");
        QVERIFY(QMetaObject::invokeMethod(pending,"fileSelected",Qt::DirectConnection,Q_ARG(QString,imagePath)));
        QFile imageFile(imagePath); QVERIFY(imageFile.open(QIODevice::ReadOnly));
        QCOMPARE(imageFile.read(8),QByteArray::fromHex("89504e470d0a1a0a"));
        view->showDefinition("after-save",17); WAIT_FOR(pages.size()==2);
        QVERIFY(pending);
        QVERIFY(QMetaObject::invokeMethod(pending,"fileSelected",Qt::DirectConnection,
            Q_ARG(QString,output.filePath("stale.png"))));
        QVERIFY(!QFile::exists(output.filePath("stale.png"))); delete pending.data(); delete menu;
    }
    void delayedHitExpiryNavigationAndOwnerDestruction()
    {
        QDeadlineTimer deadline(15000);
        QCOMPARE(ArticleView::InteractionDeadlineMs,500);
        InteractionFixture f; auto view=f.view(); auto web=view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy tabs(view.get(), &ArticleView::showDefinitionInNewTab);
        WAIT_FOR(!pages.isEmpty()); QPoint p=point(*web->page(),"click-word",deadline);
        holdInteraction=true;
        QTest::mouseDClick(web->focusProxy(),Qt::LeftButton,Qt::ControlModifier,p);
        WAIT_FOR(!interactions.isEmpty());
        auto barrier=std::make_shared<bool>(false);
        web->bridge->run("true",[barrier](QVariant const &) { *barrier=true; });
        QVERIFY2(QTest::qWaitFor([&] { return *barrier; },3000),"interaction budget must retire a withheld result and release the queue");
        holdInteraction=false; interactions.takeFirst().deliver();
        QCOMPARE(tabs.size(),0); QCOMPARE(view->getPhrase().phrase,QString("initial"));
        holdInteraction=true;
        QTest::mouseDClick(web->focusProxy(),Qt::LeftButton,Qt::ControlModifier,p);
        WAIT_FOR(!interactions.isEmpty());
        view->showDefinition("after-hit",17); WAIT_FOR(pages.size()==2);
        holdInteraction=false; interactions.takeFirst().deliver();
        QCOMPARE(tabs.size(),0); QCOMPARE(view->getPhrase().phrase,QString("after-hit"));
        p=point(*web->page(),"context-link",deadline); holdInteraction=true;
        QTest::mouseClick(web->focusProxy(),Qt::RightButton,Qt::NoModifier,p); WAIT_FOR(!interactions.isEmpty());
        auto saved=interactions.takeFirst(); view.reset(); saved.deliver();
    }

    void invalidFrameAndStaleSelectionAreEmpty()
    {
        QDeadlineTimer deadline(15000);
        InteractionFixture f; auto view=f.view(); auto web=view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        WAIT_FOR(!pages.isEmpty());
        evaluate(*web->page(),"(() => {const i=document.createElement('iframe');i.id=i.name='gdexpandframe-dict-a';"
            "i.srcdoc='<p>untrusted-frame-word</p>';document.getElementById('click-word').append(i);"
            "window.dispatchEvent(new Event('gd-active-article'));})()",deadline);
        WAIT_FOR(view->isFramedArticle("dict-a")); QVERIFY(view->isFramedArticle("gdfrom-dict-a"));
        QPoint p=point(*web->page(),"gdexpandframe-dict-a",deadline);
        auto result=std::make_shared<QVariantMap>(); auto done=std::make_shared<bool>(false);
        int ticks=0; QTimer heartbeat; connect(&heartbeat,&QTimer::timeout,this,[&] { ++ticks; }); heartbeat.start(10);
        view->requestHit(p,[result,done](QVariantMap const &value) { *result=value; *done=true; });
        WAIT_FOR(*done); QCOMPARE(result->value("word").toString(),QString());
        QVERIFY(result->value("frame").toBool()); QVERIFY(ticks>0);
        QVERIFY(focusAt(*web,point(*web->page(),"spaces",deadline),deadline));
        evaluate(*web->page(),"(() => {const e=document.getElementById('spaces');e.textContent='old selection';"
            "const r=document.createRange();r.selectNodeContents(e);getSelection().removeAllRanges();getSelection().addRange(r);})()",deadline);
        WAIT_FOR(web->selectedText()=="old selection");
        p=point(*web->page(),"spaces",deadline); holdInteraction=true;
        QTest::mouseClick(web->focusProxy(),Qt::RightButton,Qt::NoModifier,p); WAIT_FOR(!interactions.isEmpty());
        view->showDefinition("selection-page",17);
        WAIT_FOR(pages.size()==2); QCOMPARE(web->selectedText(),QString());
        QVERIFY(!view->isFramedArticle("dict-a"));
        QVERIFY(focusAt(*web,point(*web->page(),"spaces",deadline),deadline));
        evaluate(*web->page(),"(() => {const e=document.getElementById('spaces');e.textContent='new selection';"
            "const r=document.createRange();r.selectNodeContents(e);getSelection().removeAllRanges();getSelection().addRange(r);})()",deadline);
        WAIT_FOR(web->selectedText()=="new selection");
        holdInteraction=false; interactions.takeFirst().deliver();
        QCOMPARE(web->selectedText(),QString("new selection")); QVERIFY(!visibleMenu(*view));
    }
    void contextSupersededInSameDocument()
    {
        QDeadlineTimer deadline(15000);
        InteractionFixture f; auto view=f.view(); auto web=view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        WAIT_FOR(!pages.isEmpty());
        auto p=point(*web->page(),"context-link",deadline);
        QTest::mouseClick(web->focusProxy(),Qt::RightButton,Qt::NoModifier,p);
        WAIT_FOR(visibleMenu(*view)); auto old=visibleMenu(*view); old->setAttribute(Qt::WA_DeleteOnClose,false);
        auto saved=action(old,"Copy Link"); QVERIFY(saved);
        p=point(*web->page(),"click-word",deadline); holdInteraction=true;
        QTest::mouseClick(web->focusProxy(),Qt::RightButton,Qt::NoModifier,p); WAIT_FOR(!interactions.isEmpty());
        QApplication::clipboard()->setText("latest context"); saved->trigger();
        QCOMPARE(QApplication::clipboard()->text(),QString("latest context"));
        holdInteraction=false; interactions.takeFirst().deliver(); WAIT_FOR(visibleMenu(*view));
        QVERIFY(!action(visibleMenu(*view),"Copy Link")); delete old;
    }

    void optionalSelectorsRunThroughView()
    {
        QDeadlineTimer deadline(12000);
        InteractionFixture f;
        f.a->setArticleBody("<span class='dsl_opt' id='optional' style='display:none'>optional word</span>"
            "<span class='hidden_expand_opt' id='expander'>+</span>");
        auto view=f.view(); auto web=view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(),&ArticleView::pageLoaded); WAIT_FOR(!pages.isEmpty());
        view->receiveExpandOptionalParts(true);
        auto state=std::make_shared<QVariant>();
        web->bridge->run("[getComputedStyle(document.getElementById('optional')).display,"
            "getComputedStyle(document.getElementById('expander')).display]",[state](QVariant const &v) { *state=v; });
        WAIT_FOR(state->isValid()); QCOMPARE(state->toStringList(),(QStringList{"inline","none"}));
        view->receiveExpandOptionalParts(false); *state=QVariant();
        web->bridge->run("[getComputedStyle(document.getElementById('optional')).display,"
            "getComputedStyle(document.getElementById('expander')).display]",[state](QVariant const &v) { *state=v; });
        WAIT_FOR(state->isValid()); QCOMPARE(state->toStringList(),(QStringList{"none","inline"}));
        QCOMPARE(pages.size(),1);
    }

    // Break: a main-frame navigation is not classified as a user action, a
    // website frame loses its real origin, or a redirect becomes a new action.
    void navigationClassificationDistinguishesActionsFramesAndRedirects()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view;
        view.configure(b.controller->profile(), &b.cfg);
        auto page = view.ownedPage.data();
        auto bridge = view.bridge.data();
        QSignalSpy links(&view, &ArticleWebView::userLink);
        QSignalSpy navigation(&view, &ArticleWebView::navigationRequested);
        QSignalSpy ready(bridge, &ArticleBridge::bridgeReady);
        view.setHtml(document()); WAIT_FOR(ready.size() == 1);
        auto internal = QUrl("gdlookup://localhost/cat?gdanchor=part");
        // A main-frame application link is refused and dispatched exactly once.
        QVERIFY(!page->acceptNavigationRequest(internal, QWebEnginePage::NavigationTypeLinkClicked, true));
        WAIT_FOR(links.size() == 1);
        QCOMPARE(links.at(0).at(0).toUrl(), internal);
        QCOMPARE(links.at(0).at(1).toBool(), false);
        // A website frame keeps its real origin: an ordinary navigation stays native.
        QVERIFY(page->acceptNavigationRequest(QUrl("https://example.invalid/page.html"),
                                              QWebEnginePage::NavigationTypeLinkClicked, false));
        QVERIFY(page->acceptNavigationRequest(QUrl("https://example.invalid/page.html"),
                                              QWebEnginePage::NavigationTypeFormSubmitted, false));
        QCOMPARE(links.size(), 1); QCOMPARE(navigation.size(), 0);
        // An application URL never becomes a website frame's document.
        QVERIFY(!page->acceptNavigationRequest(internal, QWebEnginePage::NavigationTypeOther, false));
        QCOMPARE(links.size(), 1); QCOMPARE(navigation.size(), 0);
        QVERIFY(!page->acceptNavigationRequest(internal, QWebEnginePage::NavigationTypeLinkClicked, false));
        WAIT_FOR(links.size() == 2);
        QCOMPARE(links.at(1).at(1).toBool(), false);
        // A script-initiated document change is a navigation, never a user click.
        auto scripted = QUrl("gdlookup://localhost/?word=x&group=3");
        QVERIFY(!page->acceptNavigationRequest(scripted, QWebEnginePage::NavigationTypeOther, true));
        WAIT_FOR(navigation.size() == 1);
        QCOMPARE(navigation.at(0).at(0).toUrl(), scripted);
        QCOMPARE(qvariant_cast<NavigationKind>(navigation.at(0).at(1)), NavigationKind::NewLookup);
        QCOMPARE(links.size(), 2);
        // The issued navigation has one consumed permit; its redirects are allowed.
        bridge->reset();
        view.load(b.article()); WAIT_FOR(ready.size() == 2);
        auto redirect = QUrl("gdlookup://localhost/?word=y&group=3");
        QVERIFY(page->acceptNavigationRequest(redirect, QWebEnginePage::NavigationTypeRedirect, true));
        QCOMPARE(links.size(), 2); QCOMPARE(navigation.size(), 1);
        // Alt locks the jump: the press carries the modifier Chromium's callback
        // does not report, so no action may be dispatched.
        QPoint const point(40, 40);
        QTest::mousePress(&view, Qt::LeftButton, Qt::AltModifier, point);
        QVERIFY(!page->acceptNavigationRequest(internal, QWebEnginePage::NavigationTypeLinkClicked, true));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::AltModifier, point);
        QCOMPARE(links.size(), 2);
        // Control raises one new-tab intent through the same captured press state.
        QTest::mousePress(&view, Qt::LeftButton, Qt::ControlModifier, point);
        QVERIFY(!page->acceptNavigationRequest(internal, QWebEnginePage::NavigationTypeLinkClicked, true));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::ControlModifier, point);
        WAIT_FOR(links.size() == 3);
        QCOMPARE(links.at(2).at(1).toBool(), true);
        // A bare click is one ordinary dispatch, not a new tab.
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(!page->acceptNavigationRequest(internal, QWebEnginePage::NavigationTypeLinkClicked, true));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, point);
        WAIT_FOR(links.size() == 4);
        QCOMPARE(links.at(3).at(1).toBool(), false);
    }

    // T22's bounded capture uses this same queue, including while the channel
    // is not ready. Expiry must retire only its own slot.
    void boundedCaptureExpiryCannotReleaseAnotherOperation()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view; view.configure(b.controller->profile(), &b.cfg);
        auto bridge = view.bridge.data();
        int pendingReplies = 0;
        bridge->enqueue({}, "/*t19:pending-expired*/ 41", [&](QVariant const &value) {
            QVERIFY(!value.isValid()); ++pendingReplies;
        }, 100);
        WAIT_FOR(pendingReplies == 1);
        QVERIFY(bridge->queue.isEmpty()); QVERIFY(dispatched.isEmpty());
        QSignalSpy ready(bridge, &ArticleBridge::bridgeReady);
        view.setHtml(document()); WAIT_FOR(ready.size() == 1);
        int expired = 0, trailing = 0;
        bridge->run("/*t19:hold-owner*/ 1"); WAIT_FOR(held.contains("hold-owner"));
        bridge->enqueue({}, "/*t19:queued-expired*/ 42", [&](QVariant const &value) {
            QVERIFY(!value.isValid()); ++expired;
        }, 100);
        bridge->run("/*t19:tail*/ 43", [&](QVariant const &value) { QCOMPARE(value.toInt(), 43); ++trailing; });
        WAIT_FOR(expired == 1);
        QVERIFY(!dispatched.contains("queued-expired")); QVERIFY(!trailing);
        held.take("hold-owner").deliver(); WAIT_FOR(trailing == 1);
        int freshReplies = 0;
        bridge->enqueue({}, "/*t19:hold-expired*/ 44", [&](QVariant const &value) {
            QVERIFY(!value.isValid()); ++expired;
        }, 100);
        WAIT_FOR(held.contains("hold-expired")); WAIT_FOR(expired == 2);
        auto retired = held.take("hold-expired");
        bridge->run("/*t19:hold-fresh*/ 45", [&](QVariant const &value) { QCOMPARE(value.toInt(), 45); ++freshReplies; });
        WAIT_FOR(held.contains("hold-fresh"));
        bridge->run("/*t19:after-fresh*/ 46", [&](QVariant const &) { ++trailing; });
        retired.deliver(); QCOMPARE(trailing, 1); QCOMPARE(freshReplies, 0);
        held.take("hold-fresh").deliver(); WAIT_FOR(freshReplies == 1 && trailing == 2);
    }

    void parserTimeAndPermanentFacade()
    {
        QDeadlineTimer deadline(20000);
        Browser b;
        QObject owner;
        QWebEnginePage page(b.controller->profile());
        b.controller->attachPage(&page);
        ArticleBridge bridge(&page, &owner);
        QCOMPARE(bridge.generation(), 1);
        QVERIFY(bridge.metaObject()->indexOfSlot("onJsActiveArticleChanged(QString,int)") >= 0);
        QVERIFY(bridge.metaObject()->indexOfSlot("onJsReadingState(QVariantMap,int)") >= 0);
        QVERIFY(bridge.metaObject()->indexOfSlot("onJsFrameEvent(QVariantMap,int)") >= 0);
        QVERIFY(bridge.metaObject()->indexOfSlot("onJsActiveArticleChanged(QString)") < 0);
        QSignalSpy ready(&bridge, &ArticleBridge::bridgeReady);
        QSignalSpy active(&bridge, &ArticleBridge::activeArticleChanged);
        QSignalSpy reading(&bridge, &ArticleBridge::readingStateChanged);
        page.load(b.article());
        WAIT_FOR(ready.size() == 1);
        QCOMPARE(active.size(), 1);
        QCOMPARE(active.constFirst().at(0).toString(), QStringLiteral("gdfrom-dict-a"));
        auto initial = reading.constFirst().at(0).toMap();
        QCOMPARE(initial.value("kind").toString(), QStringLiteral("ready"));
        QCOMPARE(initial.value("articles").toStringList(), QStringList{"dict-a"});
        for (auto name : {"onJsActiveArticleChanged", "onJsReadingState", "onJsFrameEvent"})
            QVERIFY(evaluate(page, QString("typeof articleview.%1 === 'function'").arg(name), deadline).toBool());
        QSignalSpy frames(&bridge, &ArticleBridge::frameEvent);
        evaluate(page, "articleview.onJsFrameEvent({tag:'post-ready'}); gdOnBridgeReady(); gdOnBridgeReady()", deadline);
        WAIT_FOR(frames.size() == 1);
        QCOMPARE(frames[0][0].toMap().value("tag").toString(), QStringLiteral("post-ready"));
        QCOMPARE(ready.size(), 1);
        int const retired = bridge.generation();
        bridge.reset();
        QCOMPARE(bridge.generation(), retired + 1);
        active.clear();
        bridge.onJsActiveArticleChanged(QStringLiteral("gdfrom-dict-a"), retired);
        QCOMPARE(active.size(), 0);
    }

    void parserCallbacksReconciledInOrder()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        QObject owner;
        QWebEnginePage page(b.controller->profile()); b.controller->attachPage(&page);
        ArticleBridge bridge(&page, &owner);
        QSignalSpy ready(&bridge, &ArticleBridge::bridgeReady);
        QSignalSpy active(&bridge, &ArticleBridge::activeArticleChanged);
        page.setHtml(document()); WAIT_FOR(ready.size() == 1);
        QVERIFY(evaluate(page, "parserHelpers && parserFacade", deadline).toBool());
        QCOMPARE(active.size(), 2);
        QCOMPARE(active[0][0].toString(), QStringLiteral("gdfrom-dict-b"));
        QCOMPARE(active[1][0].toString(), QStringLiteral("gdfrom-dict-a"));
        evaluate(page, "articleview.onJsActiveArticleChanged('gdfrom-dict-b')", deadline);
        WAIT_FOR(active.size() == 3);
        QCOMPARE(active.last()[0].toString(), QStringLiteral("gdfrom-dict-b"));
    }

    void installedBeforeNavigationAndReadyBarrier()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view;
        view.configure(b.controller->profile(), &b.cfg);
        auto page = view.ownedPage.data();
        auto bridge = view.bridge.data();
        QCOMPARE(page->parent(), &view);
        QCOMPARE(page->profile(), b.controller->profile());
        QCOMPARE(bridge->parent(), &view);
        QCOMPARE(page->webChannel()->parent(), bridge);
        QVERIFY(page->findChild<QObject *>("gdArticleRequestInterceptor", Qt::FindDirectChildrenOnly));
        QVERIFY(!page->settings()->testAttribute(QWebEngineSettings::BackForwardCacheEnabled));
        QCOMPARE(bridge->state, ArticleBridge::State::ChannelPending);
        QCOMPARE(page->scripts().count(), 2);
        for (auto const &script : page->scripts().toList()) {
            QCOMPARE(script.injectionPoint(), QWebEngineScript::DocumentCreation);
            QCOMPARE(script.worldId(), quint32(QWebEngineScript::MainWorld));
            QVERIFY(!script.runsOnSubFrames());
        }
        QWebEngineScript unrelated;
        unrelated.setName("unrelated"); unrelated.setSourceCode("window.unrelated = true");
        page->scripts().insert(unrelated);
        int callbacks = 0;
        bridge->run("/*t19:before-ready*/({parserFacade,parserHelpers,channel:__gdChannelReady,g:__gdDocumentGeneration})",
                    [&](QVariant const &value) {
            ++callbacks;
            auto object = value.toMap();
            QVERIFY(object.value("parserFacade").toBool());
            QVERIFY(object.value("parserHelpers").toBool());
            QVERIFY(object.value("channel").toBool());
            QCOMPARE(object.value("g").toInt(), bridge->generation());
        });
        QCOMPARE(callbacks, 0);
        QVERIFY(dispatched.isEmpty());
        QSignalSpy ready(bridge, &ArticleBridge::bridgeReady);
        QSignalSpy active(bridge, &ArticleBridge::activeArticleChanged);
        view.setHtml(document());
        WAIT_FOR(callbacks == 1);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(bridge->state, ArticleBridge::State::Ready);
        QCOMPARE(active.size(), 2); // Parser-time "absent" did not belong to this document.
        QCOMPARE(active[0][0].toString(), QStringLiteral("gdfrom-dict-b"));
        QCOMPARE(active[1][0].toString(), QStringLiteral("gdfrom-dict-a"));
        view.setHtml(document());
        QCOMPARE(bridge->generation(), 2);
        QVERIFY(page->scripts().find("gd-bootstrap-1").isEmpty());
        QVERIFY(page->scripts().find("gd-dom-1").isEmpty());
        QCOMPARE(page->scripts().find("gd-bootstrap-2").size(), 1);
        QCOMPARE(page->scripts().find("gd-dom-2").size(), 1);
        QCOMPARE(page->scripts().find("unrelated").size(), 1);
        WAIT_FOR(ready.size() == 2);
        QCOMPARE(evaluate(*page, "__gdDocumentGeneration", deadline).toInt(), 2);
        QVERIFY(evaluate(*page, "parserHelpers && parserFacade", deadline).toBool());
    }

    void snapshotsAndStaleInbound()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view;
        view.configure(b.controller->profile(), &b.cfg);
        auto bridge = view.bridge.data();
        QSignalSpy ready(bridge, &ArticleBridge::bridgeReady);
        QSignalSpy active(bridge, &ArticleBridge::activeArticleChanged);
        QSignalSpy reading(bridge, &ArticleBridge::readingStateChanged);
        QSignalSpy frames(bridge, &ArticleBridge::frameEvent);
        view.setHtml(document());
        WAIT_FOR(ready.size() == 1);
        QVERIFY(bridge->hasAudio); // Array's named "first" property, as emitted by ArticleMaker.
        auto snapshot = reading.last()[0].toMap();
        QCOMPARE(snapshot.value("articles").toStringList(), (QStringList{"dict-a", "dict-b"}));
        int const previous = bridge->generation();
        view.setHtml(document());
        WAIT_FOR(ready.size() == 2);
        active.clear(); reading.clear();
        bridge->onJsActiveArticleChanged("gdfrom-dict-b", previous);
        bridge->onJsFrameEvent({{"tag", "stale"}}, previous);
        snapshot["kind"] = "state"; snapshot["currentArticle"] = "gdfrom-dict-b";
        bridge->onJsReadingState(snapshot, previous);
        QCOMPARE(active.size(), 0); QCOMPARE(reading.size(), 0); QCOMPARE(frames.size(), 0);
        auto current = bridge->generation();
        for (auto kind : {"ready", "unknown"}) {
            auto invalid = snapshot; invalid["kind"] = kind;
            bridge->onJsReadingState(invalid, current);
        }
        for (auto key : {"sx", "hasAudio", "articles"}) {
            auto invalid = snapshot; invalid[key] = "malformed";
            bridge->onJsReadingState(invalid, current);
        }
        auto invalid = snapshot; invalid["sy"] = std::numeric_limits<double>::infinity();
        bridge->onJsReadingState(invalid, current);
        invalid = snapshot; invalid["articles"] = QStringList{"dict-a", "dict-a"};
        bridge->onJsReadingState(invalid, current);
        invalid = snapshot; invalid["currentArticle"] = "gdfrom-absent";
        bridge->onJsReadingState(invalid, current);
        bridge->onJsActiveArticleChanged("dict-b", current);
        bridge->onJsActiveArticleChanged("gdfrom-absent", current);
        QCOMPARE(active.size(), 0); QCOMPARE(reading.size(), 0); QCOMPARE(ready.size(), 2);
        evaluate(*view.page(), "gdCurrentArticle='gdfrom-dict-b'; articleview.onJsActiveArticleChanged(gdCurrentArticle)", deadline);
        WAIT_FOR(active.size() == 1 && reading.size() == 1);
        QCOMPARE(active[0][0].toString(), QStringLiteral("gdfrom-dict-b"));
        auto state = reading[0][0].toMap();
        QCOMPARE(state.value("kind").toString(), QStringLiteral("state"));
        QCOMPARE(state.value("currentArticle").toString(), QStringLiteral("gdfrom-dict-b"));
        bridge->onJsReadingState(state, current);
        bridge->onJsActiveArticleChanged("gdfrom-dict-b", current);
        QCOMPARE(reading.size(), 1); QCOMPARE(active.size(), 1); // Cached changes only.
    }

    void staleCompletionCannotReleaseNewQueue()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view;
        view.configure(b.controller->profile(), &b.cfg);
        auto bridge = view.bridge.data();
        QSignalSpy ready(bridge, &ArticleBridge::bridgeReady);
        view.setHtml(document()); WAIT_FOR(ready.size() == 1);
        int oldCallbacks = 0, newCallbacks = 0, tailCallbacks = 0;
        bridge->run("/*t19:hold-old*/41", [&](QVariant const &) { ++oldCallbacks; });
        bridge->run("/*t19:retired-queued*/99", [&](QVariant const &) { ++oldCallbacks; });
        WAIT_FOR(held.contains("hold-old"));
        QCOMPARE(dispatched, QStringList{"hold-old"});
        view.setHtml(document()); WAIT_FOR(ready.size() == 2);
        bridge->run("/*t19:hold-new*/42", [&](QVariant const &v) { QCOMPARE(v.toInt(), 42); ++newCallbacks; });
        bridge->run("/*t19:new-tail*/43", [&](QVariant const &v) { QCOMPARE(v.toInt(), 43); ++tailCallbacks; });
        WAIT_FOR(held.contains("hold-new"));
        auto newOperation = bridge->inFlight;
        held.take("hold-old").deliver();
        QCOMPARE(oldCallbacks, 0); QCOMPARE(newCallbacks, 0); QCOMPARE(tailCallbacks, 0);
        QCOMPARE(bridge->inFlight, newOperation);
        QCOMPARE(dispatched, (QStringList{"hold-old", "hold-new"}));
        held.take("hold-new").deliver();
        WAIT_FOR(tailCallbacks == 1);
        QCOMPARE(newCallbacks, 1); QCOMPARE(oldCallbacks, 0);
        QCOMPARE(dispatched, (QStringList{"hold-old", "hold-new", "new-tail"}));
    }

    void invalidResultAndDuplicateCompletion()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        QObject owner;
        QWebEnginePage page(b.controller->profile()); b.controller->attachPage(&page);
        ArticleBridge bridge(&page, &owner);
        QSignalSpy ready(&bridge, &ArticleBridge::bridgeReady);
        page.setHtml(document()); WAIT_FOR(ready.size() == 1);
        int invalidCallbacks = 0, tailCallbacks = 0;
        std::function<void()> replay;
        bridge.run("/*t19:hold-invalid*/void 0", [&](QVariant const &value) {
            QVERIFY(!value.isValid()); ++invalidCallbacks;
            if (invalidCallbacks == 1) replay(); // Reentrant duplicate while still in flight.
        });
        bridge.run("/*t19:hold-next*/7");
        bridge.run("/*t19:tail*/8", [&](QVariant const &) { ++tailCallbacks; });
        WAIT_FOR(held.contains("hold-invalid"));
        auto first = held.take("hold-invalid");
        replay = [&] { first.deliver(); };
        QVERIFY(!first.value.isValid());
        first.deliver();
        WAIT_FOR(held.contains("hold-next"));
        auto next = bridge.inFlight;
        first.deliver(); // Same generation, wrong operation and completed latch.
        QCOMPARE(invalidCallbacks, 1); QCOMPARE(tailCallbacks, 0);
        QCOMPARE(bridge.inFlight, next);
        QCOMPARE(dispatched, (QStringList{"hold-invalid", "hold-next"}));
        held.take("hold-next").deliver(); WAIT_FOR(tailCallbacks == 1);
    }

    void ownerAndPageDestruction()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        QWebEnginePage page(b.controller->profile()); b.controller->attachPage(&page);
        auto owner = std::make_unique<QObject>();
        QPointer<ArticleBridge> bridge = new ArticleBridge(&page, owner.get());
        QSignalSpy ready(bridge, &ArticleBridge::bridgeReady);
        page.setHtml(document()); WAIT_FOR(ready.size() == 1);
        int callbacks = 0;
        bridge->run("/*t19:hold-destroy-owner*/10", [&](QVariant const &) { ++callbacks; });
        WAIT_FOR(held.contains("hold-destroy-owner"));
        owner.reset();
        QVERIFY(!bridge); QVERIFY(!page.webChannel());
        QVERIFY(page.scripts().find("gd-bootstrap-1").isEmpty());
        held.take("hold-destroy-owner").deliver(); QCOMPARE(callbacks, 0);

        QObject secondOwner;
        auto secondPage = std::make_unique<QWebEnginePage>(b.controller->profile());
        b.controller->attachPage(secondPage.get());
        ArticleBridge secondBridge(secondPage.get(), &secondOwner);
        QSignalSpy secondReady(&secondBridge, &ArticleBridge::bridgeReady);
        secondPage->setHtml(document()); WAIT_FOR(secondReady.size() == 1);
        secondBridge.run("/*t19:hold-destroy-page*/11", [&](QVariant const &) { ++callbacks; });
        WAIT_FOR(held.contains("hold-destroy-page"));
        secondPage.reset();
        QCOMPARE(secondBridge.state, ArticleBridge::State::Closed);
        held.take("hold-destroy-page").deliver(); QCOMPARE(callbacks, 0);
        secondBridge.run("/*t19:after-destruction*/12");
        QVERIFY(!dispatched.contains("after-destruction"));
    }

    void callbackCanResetOrDestroyOwner()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        auto view = std::make_unique<ArticleWebView>();
        view->configure(b.controller->profile(), &b.cfg);
        QPointer<ArticleBridge> bridge = view->bridge;
        QSignalSpy ready(bridge, &ArticleBridge::bridgeReady);
        view->setHtml(document()); WAIT_FOR(ready.size() == 1);
        int callbacks = 0;
        bridge->run("/*t19:hold-reentrant*/1", [&](QVariant const &) {
            ++callbacks; view->setHtml(document());
            bridge->run("/*t19:after-reset*/2", [&](QVariant const &) { ++callbacks; });
        });
        bridge->run("/*t19:must-be-cancelled*/3", [&](QVariant const &) { ++callbacks; });
        WAIT_FOR(held.contains("hold-reentrant"));
        held.take("hold-reentrant").deliver();
        WAIT_FOR(callbacks == 2);
        QCOMPARE(bridge->generation(), 2);
        QVERIFY(!dispatched.contains("must-be-cancelled"));
        bridge->run("/*t19:hold-delete-in-callback*/4", [&](QVariant const &) { ++callbacks; view.reset(); });
        WAIT_FOR(held.contains("hold-delete-in-callback"));
        held.take("hold-delete-in-callback").deliver();
        QVERIFY(!bridge); QCOMPARE(callbacks, 3);
    }

    void resetSignalCanDestroyView()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        auto view = std::make_unique<ArticleWebView>();
        view->configure(b.controller->profile(), &b.cfg);
        QSignalSpy ready(view->bridge, &ArticleBridge::bridgeReady);
        view->setHtml(document()); WAIT_FOR(ready.size() == 1);
        QPointer<ArticleBridge> bridge = view->bridge;
        std::unique_ptr<ArticleWebView> replacement;
        connect(bridge, &ArticleBridge::generationChanged, b.controller.get(),
                [&](int) {
            view.reset();
            replacement = std::make_unique<ArticleWebView>();
            replacement->configure(b.controller->profile(), &b.cfg);
        });
        view->setHtml(document());
        QVERIFY(!view); QVERIFY(!bridge); QVERIFY(replacement);
        QVERIFY(!replacement->navigated);
        QVERIFY(replacement->page()->requestedUrl().isEmpty());
    }

    void resetSignalNavigationCannotBeOverwritten()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view;
        view.configure(b.controller->profile(), &b.cfg);
        QSignalSpy ready(view.bridge, &ArticleBridge::bridgeReady);
        view.setHtml(document(), QUrl("gdlookup://localhost/")); WAIT_FOR(ready.size() == 1);
        connect(view.bridge, &ArticleBridge::generationChanged, &view, [&](int) {
            view.setHtml(document("<p id='winner'>replacement</p>"), QUrl("gdlookup://localhost/"));
        }, Qt::SingleShotConnection);
        view.setHtml(document("<p id='winner'>superseded</p>"), QUrl("gdlookup://localhost/"));
        WAIT_FOR(ready.size() == 2);
        QCOMPARE(view.bridge->generation(), 3);
        QCOMPARE(evaluate(*view.page(), "document.getElementById('winner').textContent", deadline).toString(),
                 QStringLiteral("replacement"));
    }

    void multiplePreparationsBeforeLoad()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view;
        view.configure(b.controller->profile(), &b.cfg);
        QSignalSpy ready(view.bridge, &ArticleBridge::bridgeReady);
        QSignalSpy loaded(view.page(), &QWebEnginePage::loadFinished);
        view.setHtml(document(), QUrl("gdlookup://localhost/"));
        WAIT_FOR(ready.size() == 1 && loaded.size() == 1);
        view.bridge->reset();
        view.setHtml(document("<p id='winner'>replacement</p>"), QUrl("gdlookup://localhost/"));
        WAIT_FOR(ready.size() == 2);
        QCOMPARE(view.bridge->generation(), 3);
    }

    void validatesNativeFrameBeforeForwarding()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view;
        view.configure(b.controller->profile(), &b.cfg);
        auto bridge = view.bridge.data();
        QSignalSpy ready(bridge, &ArticleBridge::bridgeReady);
        view.setHtml(document("<iframe name='child' srcdoc='<p>child text</p>'></iframe>"));
        WAIT_FOR(ready.size() == 1);
        WAIT_FOR(view.page()->mainFrame().children().size() == 1);
        auto child = view.page()->mainFrame().children().first();
        int valid = 0, rejected = 0;
        bridge->runInFrame(child, "/*t19:valid-child*/typeof window.articleview", [&](QVariant const &v) {
            QCOMPARE(v.toString(), QStringLiteral("undefined")); ++valid;
        });
        WAIT_FOR(valid == 1);
        QWebEnginePage foreign(b.controller->profile()); b.controller->attachPage(&foreign);
        QSignalSpy loaded(&foreign, &QWebEnginePage::loadFinished);
        foreign.setHtml("<p>foreign</p>"); WAIT_FOR(!loaded.isEmpty());
        auto reject = [&](QVariant const &v) { QVERIFY(!v.isValid()); ++rejected; };
        bridge->runInFrame(foreign.mainFrame(), "/*t19:foreign*/99", reject);
        bridge->runInFrame(QWebEngineFrame{}, "/*t19:invalid*/99", reject);
        QCOMPARE(rejected, 2);
        QVERIFY(!dispatched.contains("foreign")); QVERIFY(!dispatched.contains("invalid"));
        evaluate(*view.page(), "document.querySelector('iframe').remove()", deadline);
        WAIT_FOR(!child.isValid());
        bridge->runInFrame(child, "/*t19:detached*/99", reject);
        QCOMPARE(rejected, 3); QVERIFY(!dispatched.contains("detached"));
        bridge->run("/*t19:after-rejected-frame*/44", [&](QVariant const &v) { QCOMPARE(v.toInt(), 44); ++valid; });
        WAIT_FOR(valid == 2);
    }

    void checkedGenerationExhaustionRecreatesPage()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view;
        view.configure(b.controller->profile(), &b.cfg);
        QPointer<QWebEnginePage> oldPage = view.page();
        QPointer<ArticleBridge> oldBridge = view.bridge;
        QSignalSpy ready(oldBridge, &ArticleBridge::bridgeReady);
        view.setHtml(document()); WAIT_FOR(ready.size() == 1);
        int callbacks = 0;
        oldBridge->run("/*t19:hold-exhausted*/1", [&](QVariant const &) { ++callbacks; });
        WAIT_FOR(held.contains("hold-exhausted"));
        oldBridge->currentGeneration = std::numeric_limits<int>::max() - 1;
        oldBridge->reset();
        QCOMPARE(oldBridge->generation(), std::numeric_limits<int>::max());
        QSignalSpy generations(oldBridge, &ArticleBridge::generationChanged);
        view.setHtml(document());
        QVERIFY(!oldPage);
        QVERIFY(view.bridge != oldBridge);
        QCOMPARE(view.bridge->generation(), 1);
        QCOMPARE(generations.size(), 0); // No signed overflow, zero or negative stamp.
        if (oldBridge) QCOMPARE(oldBridge->state, ArticleBridge::State::Closed);
        held.take("hold-exhausted").deliver(); QCOMPARE(callbacks, 0);
        QSignalSpy newReady(view.bridge, &ArticleBridge::bridgeReady);
        WAIT_FOR(newReady.size() == 1);
        QCOMPARE(evaluate(*view.page(), "__gdDocumentGeneration", deadline).toInt(), 1);
        WAIT_FOR(!oldBridge);

        QObject owner;
        QWebEnginePage page(b.controller->profile()); b.controller->attachPage(&page);
        ArticleBridge standalone(&page, &owner);
        standalone.currentGeneration = std::numeric_limits<int>::max();
        QTest::ignoreMessage(QtWarningMsg, "ArticleBridge: generation exhausted; owning view must recreate page and bridge");
        standalone.reset();
        QCOMPARE(standalone.generation(), std::numeric_limits<int>::max());
        QCOMPARE(standalone.state, ArticleBridge::State::Closed);
        QVERIFY(!page.webChannel());
    }

    // Break: anonymous DevTools widget, lost geometry, second page or deferred reopen leak.
    void inspectorReopenKeepsOneAttachedPageAndGeometry()
    {
        QDeadlineTimer deadline(12000);
        Browser b;
        QWidget geometrySource; geometrySource.setGeometry(70, 80, 640, 480);
        b.cfg.inspectorGeometry = geometrySource.saveGeometry();
        ArticleWebView view; view.configure(b.controller->profile(), &b.cfg);
        QSignalSpy ready(view.bridge, &ArticleBridge::bridgeReady);
        view.load(b.article()); WAIT_FOR(ready.size() == 1);
        auto pageCount = [&] {
            return view.findChildren<QWebEnginePage*>().size() + b.controller->findChildren<QWebEnginePage*>().size();
        };
        auto countStays = [&](int expected) {
            QDeadlineTimer observation(400);
            while (!observation.hasExpired()) {
                if (pageCount() != expected) return false;
                QTest::qWait(10);
            }
            return pageCount() == expected;
        };
        QCOMPARE(pageCount(), 1);
        view.triggerPageAction(QWebEnginePage::InspectElement);
        auto inspector = view.findChild<QWidget*>("gdArticleInspector"); QVERIFY(inspector);
        QCOMPARE(QString::fromLatin1(inspector->metaObject()->className()), QStringLiteral("ArticleInspector"));
        WAIT_FOR(inspector->isVisible()); QCOMPARE(inspector->size(), QSize(640, 480));
        QPointer<QWebEnginePage> tools(view.page()->devToolsPage()); QVERIFY(tools);
        QCOMPARE(tools->profile(), view.page()->profile());
        QVERIFY(tools->findChild<QObject*>("gdArticleRequestInterceptor", Qt::FindDirectChildrenOnly));
        QVERIFY(!tools->settings()->testAttribute(QWebEngineSettings::BackForwardCacheEnabled));
        for (int i = 0; i < 3; ++i) view.triggerPageAction(QWebEnginePage::InspectElement);
        QCOMPARE(view.page()->devToolsPage(), tools.data());
        QVERIFY(countStays(2));
        inspector->resize(720, 520);
        QPointer<QWidget> oldInspector(inspector);
        inspector->close();
        // Reopen before WA_DeleteOnClose's DeferredDelete gets processed.
        view.triggerPageAction(QWebEnginePage::InspectElement);
        WAIT_FOR(!oldInspector && !tools);
        inspector = view.findChild<QWidget*>("gdArticleInspector"); QVERIFY(inspector);
        WAIT_FOR(inspector->isVisible()); QCOMPARE(inspector->size(), QSize(720, 520));
        QVERIFY(countStays(2));
        QPointer<QWebEnginePage> reopened(view.page()->devToolsPage()); QVERIFY(reopened);
        QPointer<QWidget> reopenedWidget(inspector);
        inspector->close(); WAIT_FOR(!reopenedWidget && !reopened);
        QVERIFY(!view.page()->devToolsPage()); QVERIFY(countStays(1));
        QVERIFY(!b.cfg.inspectorGeometry.isEmpty());
    }

    void controlledNavigationAndPopupAttachment()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view;
        view.configure(b.controller->profile(), &b.cfg);
        auto page = view.ownedPage.data();
        QSignalSpy ready(view.bridge, &ArticleBridge::bridgeReady);
        view.load(b.article()); WAIT_FOR(ready.size() == 1);
        QSignalSpy links(&view, &ArticleWebView::userLink);
        QSignalSpy navigation(&view, &ArticleWebView::navigationRequested);
        auto url = QUrl("gdlookup://localhost/?word=next");
        QVERIFY(!page->acceptNavigationRequest(url, QWebEnginePage::NavigationTypeLinkClicked, true));
        WAIT_FOR(links.size() == 1);
        QCOMPARE(links[0][0].toUrl(), url); QVERIFY(!links[0][1].toBool());
        QVERIFY(!page->acceptNavigationRequest(url, QWebEnginePage::NavigationTypeReload, true));
        WAIT_FOR(navigation.size() == 1);
        QCOMPARE(qvariant_cast<NavigationKind>(navigation[0][1]), NavigationKind::Reload);
        QCOMPARE(view.bridge->generation(), 1);
        auto popup = page->createWindow(QWebEnginePage::WebBrowserTab);
        QPointer<QWebEnginePage> popupGuard(popup);
        QCOMPARE(popup->profile(), page->profile());
        QVERIFY(popup->findChild<QObject *>("gdArticleRequestInterceptor", Qt::FindDirectChildrenOnly));
        QVERIFY(!popup->settings()->testAttribute(QWebEngineSettings::BackForwardCacheEnabled));
        popup->load(url);
        WAIT_FOR(links.size() == 2 && !popupGuard);
        QVERIFY(links[1][1].toBool());
        view.triggerPageAction(QWebEnginePage::InspectElement);
        QVERIFY(view.inspector);
        QCOMPARE(view.inspector->page()->profile(), page->profile());
        QVERIFY(view.inspector->page()->findChild<QObject *>("gdArticleRequestInterceptor", Qt::FindDirectChildrenOnly));
        auto stalePopup = page->createWindow(QWebEnginePage::WebBrowserTab);
        QPointer<QWebEnginePage> staleGuard(stalePopup);
        view.bridge->reset(); stalePopup->load(url);
        WAIT_FOR(!staleGuard); QCOMPARE(links.size(),2);
    }

    // A UTF-16 caret can land inside a supplementary letter; the production
    // helper must measure the whole scalar and return the complete word.
    void supplementaryWordThroughReadyBridge()
    {
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view;
        view.resize(720, 480); view.show();
        view.configure(b.controller->profile(), &b.cfg);
        QFile fixture(TestSupport::repositoryRoot() + "/tests/fixtures/article/dom.html");
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QSignalSpy ready(view.bridge, &ArticleBridge::bridgeReady);
        view.setHtml(QString::fromUtf8(fixture.readAll())); WAIT_FOR(ready.size() == 1);
        auto answer = std::make_shared<std::optional<QString>>();
        view.bridge->run(QString::fromUtf8(R"JS((() => {
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
        })())JS"), [answer](QVariant const &value) { *answer = value.toString(); });
        WAIT_FOR(answer->has_value());
        QCOMPARE(**answer, QString::fromUtf8("𐐀word"));
        QVERIFY(!(**answer).contains(QChar(0xfffd)));
        for (qsizetype i = 0; i < (**answer).size(); ++i)
            if ((**answer).at(i).isSurrogate()) {
                QVERIFY((**answer).at(i).isHighSurrogate());
                QVERIFY(i + 1 < (**answer).size() && (**answer).at(++i).isLowSurrogate());
            }
    }

    void wordFallbackThroughRealDom_data()
    {
        QTest::addColumn<QString>("caretSetup");
        QTest::newRow("native") << QString();
        QTest::newRow("range-only") << QString("document.caretPositionFromPoint=undefined;");
        QTest::newRow("position-null") << QString("document.caretPositionFromPoint=()=>null;");
        QTest::newRow("position-throws") << QString("document.caretPositionFromPoint=()=>{throw Error('unavailable')};");
        QTest::newRow("both-null") << QString("document.caretPositionFromPoint=()=>null;document.caretRangeFromPoint=()=>null;");
        QTest::newRow("both-throw") << QString("document.caretPositionFromPoint=document.caretRangeFromPoint=()=>{throw Error('unavailable')};");
        QTest::newRow("foreign-root") << QString("const foreign=document.createElement('span').attachShadow({mode:'open'});"
            "foreign.textContent='foreign';document.caretPositionFromPoint=()=>({offsetNode:foreign.firstChild,offset:1});"
            "document.caretRangeFromPoint=()=>null;");
    }
    void wordFallbackThroughRealDom()
    {
        QFETCH(QString, caretSetup);
        QDeadlineTimer deadline(10000);
        Browser b;
        ArticleWebView view; view.resize(720, 480); view.show();
        view.configure(b.controller->profile(), &b.cfg);
        QFile fixture(TestSupport::repositoryRoot() + "/tests/fixtures/article/dom.html");
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QSignalSpy ready(view.bridge, &ArticleBridge::bridgeReady);
        view.setHtml(QString::fromUtf8(fixture.readAll())); WAIT_FOR(ready.size() == 1);
        auto answer = std::make_shared<std::optional<QStringList>>();
        view.bridge->run("(() => {" + caretSetup + QString::fromUtf8(R"JS(
          const result = [];
          for (const [id, offset, length] of [['word-latin',1,1],['word-cyrillic',1,1],
              ['word-cjk',1,1],['word-combining',0,2],['word-apostrophe',1,1]]) {
            const n=document.getElementById(id).firstChild, r=document.createRange();
            r.setStart(n,offset);r.setEnd(n,offset+length);const b=r.getBoundingClientRect();
            result.push(gdWordAtPoint(b.x+b.width/2,b.y+b.height/2));
          }
          const n=document.createElement('span');n.textContent='𐐀word !  ';
          n.style.cssText='position:fixed;left:40px;top:40px;font:24px monospace;z-index:2147483647';
          document.body.append(n);
          for (const [start,end] of [[0,2],[7,8],[8,9]]) {
            const r=document.createRange();r.setStart(n.firstChild,start);r.setEnd(n.firstChild,end);
            const b=r.getBoundingClientRect();result.push(gdWordAtPoint(b.x+b.width/2,b.y+b.height/2));
          }
          n.remove();result.push(gdWordAtPoint(-10,-10));return result;
        })())JS"), [answer](QVariant const &value) { *answer = value.toStringList(); });
        WAIT_FOR(answer->has_value());
        QCOMPARE(**answer, (QStringList{QStringLiteral("latin"), QString::fromUtf8("Привет"),
            QString::fromUtf8("中文词条"), QString::fromUtf8("é"), QString::fromUtf8("rock’n’roll"),
            QString::fromUtf8("𐐀word"), QString(), QString(), QString()}));
    }

    // The engine's asynchronous result is the contract source for counts and the
    // active index; the UI guard only decides whether a result is still current.
    void nativeFindCountsAndStaleQueryGuard()
    {
        QDeadlineTimer deadline(18000);
        {
            Browser b;
            ArticleWebView view;
            view.resize(720, 480); view.show();
            view.configure(b.controller->profile(), &b.cfg);
            QSignalSpy ready(view.bridge, &ArticleBridge::bridgeReady);
            view.setHtml(document("<p id='needles'>testneedle and testneedle here</p>"));
            WAIT_FOR(ready.size() == 1);
            auto result = std::make_shared<std::optional<QWebEngineFindTextResult>>();
            view.page()->findText(QStringLiteral("testneedle"), {},
                                  [result](QWebEngineFindTextResult const &value) { result->emplace(value); });
            QTRY_VERIFY_WITH_TIMEOUT(result->has_value(), 3000);
            QCOMPARE(result->value().numberOfMatches(), 2);
            QCOMPARE(result->value().activeMatch(), 1);
        }

        InteractionFixture f;
        f.a->setArticleBody(QByteArray("<p id='n1'>testneedle alpha</p>"
                                       "<p id='n2'>beta testneedle</p>"));
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy status(view.get(), &ArticleView::statusBarMessage);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        f.search.trigger(); QVERIFY(view->isSearchOpened());
        auto line = searchLine(*view); QVERIFY(line);
        typeQuery(line, "beta");
        WAIT_FOR(statusText(status) == "1 of 1 matches");
        // Hold the engine result of one query while a newer query owns the bar.
        holdFind = true; holdFindQuery = "beta";
        searchTool(*view, "searchNext")->click();
        WAIT_FOR(heldFind.contains("beta"));
        holdFind = false;
        typeQuery(line, "testneedle");
        QTest::qWait(1500);
        qDebug() << "DEBUG1 status" << status.size()
                 << (status.isEmpty() ? QString() : status.last().at(0).toString())
                 << "finds" << findQueries << "held" << heldFind.keys()
                 << "results" << findResults.keys() << findResults.value("testneedle").numberOfMatches()
                 << findResults.value("testneedle").activeMatch();
        WAIT_FOR(statusText(status) == "1 of 2 matches");
        QCOMPARE(findResults.value("beta").numberOfMatches(), 1);
        int const current = status.size();
        auto stale = heldFind.take("beta");
        stale.deliver(findResults.value("beta"));
        auto barrier = std::make_shared<bool>(false);
        web->bridge->run("true", [barrier](QVariant const &) { *barrier = true; });
        WAIT_FOR(*barrier);
        QCOMPARE(status.size(), current);
        QCOMPARE(statusText(status), QString("1 of 2 matches"));
        QCOMPARE(line->property("noResults").toBool(), false);
    }

    // Count/index, forward/backward, wrap, zero, case options and the highlight
    // all toggle all drive the same retained find bar.
    void findBarCountsOptionsWrapAndCase()
    {
        QDeadlineTimer deadline(15000);
        InteractionFixture f;
        f.a->setArticleBody(QByteArray("<p id='n1'>testneedle alpha</p>"
                                       "<p id='n2'>testneedle TESTNEEDLE</p>"));
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy status(view.get(), &ArticleView::statusBarMessage);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        f.search.trigger(); QVERIFY(view->isSearchOpened());
        auto line = searchLine(*view); QVERIFY(line);
        auto next = searchTool(*view, "searchNext"); auto previous = searchTool(*view, "searchPrevious");
        auto caseBox = view->findChild<QCheckBox*>("searchCaseSensitive");
        QVERIFY(next && previous && caseBox);
        typeQuery(line, "TESTNEEDLE");
        WAIT_FOR(statusText(status) == "1 of 3 matches");
        next->click(); WAIT_FOR(statusText(status) == "2 of 3 matches");
        next->click(); WAIT_FOR(statusText(status) == "3 of 3 matches");
        next->click(); WAIT_FOR(statusText(status) == "1 of 3 matches");
        previous->click(); WAIT_FOR(statusText(status) == "3 of 3 matches");
        caseBox->click(); WAIT_FOR(statusText(status) == "1 of 1 matches");
        QCOMPARE(line->property("noResults").toBool(), false);
        caseBox->click(); WAIT_FOR(statusText(status) == "1 of 3 matches");
        // Overlapping queries: a longer query replaces the shorter one's count.
        typeQuery(line, "test"); WAIT_FOR(statusText(status) == "1 of 3 matches");
        typeQuery(line, "testneedle"); WAIT_FOR(statusText(status) == "1 of 3 matches");
        typeQuery(line, "absentneedle");
        WAIT_FOR(statusText(status) == "Phrase not found");
        QCOMPARE(line->property("noResults").toBool(), true);
        typeQuery(line, "testneedle");
        WAIT_FOR(statusText(status) == "1 of 3 matches");
        QCOMPARE(line->property("noResults").toBool(), false);
        // Highlight all marks the retained literal query and never edits text.
        auto highlightAll = searchTool(*view, "highlightAllButton"); QVERIFY(highlightAll);
        highlightAll->click();
        WAIT_FOR(markTexts(*web->page(), deadline).size() == 3);
        QCOMPARE(markTexts(*web->page(), deadline),
                 (QStringList{QStringLiteral("testneedle"), QStringLiteral("testneedle"),
                              QStringLiteral("TESTNEEDLE")}));
        QCOMPARE(evaluate(*web->page(), "document.getElementById('n1').textContent", deadline).toString(),
                 QString("testneedle alpha"));
        QVERIFY(unicodeScalarsIntact(*web->page(), deadline));
        highlightAll->click();
        WAIT_FOR(markTexts(*web->page(), deadline).isEmpty());
        QCOMPARE(evaluate(*web->page(), "document.body.textContent.indexOf('testneedle')", deadline).toInt() >= 0, true);
        QVERIFY(unicodeScalarsIntact(*web->page(), deadline));
    }

    void ftsMarksUseCompiledSpecAcrossModes_data()
    {
        QTest::addColumn<QString>("body");
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<bool>("wildcards");
        QTest::addColumn<bool>("matchCase");
        QTest::addColumn<QStringList>("marks");

        QTest::newRow("literal")
            << QStringLiteral("alpha beta alpha") << QStringLiteral("alpha") << false << false
            << QStringList{QStringLiteral("alpha"), QStringLiteral("alpha")};
        QTest::newRow("literal-case-folded")
            << QStringLiteral("Alpha alpha ALPHA") << QStringLiteral("Alpha") << false << false
            << QStringList{QStringLiteral("Alpha"), QStringLiteral("alpha"), QStringLiteral("ALPHA")};
        QTest::newRow("literal-case-sensitive")
            << QStringLiteral("Alpha alpha ALPHA") << QStringLiteral("Alpha") << false << true
            << QStringList{QStringLiteral("Alpha")};
        // Branch reset is PCRE2 syntax; JavaScript RegExp cannot compile it, so
        // the spans must come from the typed spec's compiled expression.
        QTest::newRow("regex-pcre-only")
            << QStringLiteral("alpha beta gamma") << QStringLiteral("(?|alpha|beta)") << false << false
            << QStringList{QStringLiteral("alpha"), QStringLiteral("beta")};
        // The codec converts wildcards with Qt's unanchored, non-path
        // conversion: a backslash stays literal and '*' stays a wildcard.
        QTest::newRow("wildcard-codec-conversion")
            << QStringLiteral("a*b a\\z zb") << QStringLiteral("a\\*b") << true << false
            << QStringList{QStringLiteral("a\\z zb")};
    }
    void ftsMarksUseCompiledSpecAcrossModes()
    {
        QFETCH(QString, body);
        QFETCH(QString, pattern);
        QFETCH(bool, wildcards);
        QFETCH(bool, matchCase);
        QFETCH(QStringList, marks);
        QDeadlineTimer deadline(18000);
        InteractionFixture f;
        f.a->setArticleBody((QStringLiteral("<p id='fts'>") + body + QStringLiteral("</p>")).toUtf8());
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        // The typed spec survives the document URL exactly, and the parser is
        // what compiles it for both the search and the renderer.
        ArticleUrls::Lookup lookup;
        lookup.phrase = Config::InputPhrase::fromPhrase(QStringLiteral("fts"));
        lookup.group = 17; lookup.groupSpecified = true;
        lookup.dictionaries = QStringList{QStringLiteral("dict-a")};
        lookup.highlight.sourcePattern = pattern;
        lookup.highlight.wildcards = wildcards;
        lookup.highlight.matchCase = matchCase;
        auto url = ArticleUrls::lookupDocument(lookup);
        auto parsed = ArticleUrls::parse(url, ArticleUrls::Use::DocumentLoad);
        QCOMPARE(parsed.kind, ArticleUrls::Kind::LookupDocument);
        QCOMPARE(parsed.lookup.highlight.sourcePattern, pattern);
        QCOMPARE(parsed.lookup.highlight.wildcards, wildcards);
        QCOMPARE(parsed.lookup.highlight.matchCase, matchCase);
        QVERIFY(parsed.lookup.highlight.expression.isValid());
        QCOMPARE(parsed.lookup.highlight.expression.pattern(),
                 wildcards ? QRegularExpression::wildcardToRegularExpression(pattern,
                     QRegularExpression::UnanchoredWildcardConversion |
                     QRegularExpression::NonPathWildcardConversion)
                           : pattern);
        auto options = parsed.lookup.highlight.expression.patternOptions();
        QVERIFY(options.testFlag(QRegularExpression::DotMatchesEverythingOption));
        QVERIFY(options.testFlag(QRegularExpression::UseUnicodePropertiesOption));
        QVERIFY(options.testFlag(QRegularExpression::MultilineOption));
        QVERIFY(options.testFlag(QRegularExpression::InvertedGreedinessOption));
        QCOMPARE(options.testFlag(QRegularExpression::CaseInsensitiveOption), !matchCase);
        view->showDefinition("fts", QStringList{QStringLiteral("dict-a")},
                             parsed.lookup.highlight, 17, false);
        WAIT_FOR(pages.size() == 2);
        auto label = ftsSearchLabel(*view); QVERIFY(label);
        WAIT_FOR(!label->text().isEmpty());
        QCOMPARE(label->text(), QStringLiteral("1 of %1 matches").arg(marks.size()));
        QVERIFY(ftsSearchFrame(*view)->isVisible());
        QCOMPARE(markTexts(*web->page(), deadline), marks);
        QCOMPARE(evaluate(*web->page(), "document.getElementById('fts').textContent", deadline).toString(), body);
        QVERIFY(unicodeScalarsIntact(*web->page(), deadline));
        // Clearing and re-marking keeps the article text byte-identical.
        evaluate(*web->page(), "gdClearHighlights()", deadline);
        QVERIFY(markTexts(*web->page(), deadline).isEmpty());
        QCOMPARE(evaluate(*web->page(), "document.getElementById('fts').textContent", deadline).toString(), body);
    }

    void ftsUnicodeMarksPreserveTextAndScalars_data()
    {
        QTest::addColumn<QString>("body");
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<bool>("ignoreDiacritics");
        QTest::addColumn<QStringList>("marks");
        QTest::addColumn<QList<int>>("offsets");

        QString const emoji = QString::fromUtf8("\U0001F600");
        QString const deseret = QString::fromUtf8("\U00010400");
        QString const combining = QString(QChar(0x0301));
        QTest::newRow("supplementary-before")
            << (emoji + "alpha " + emoji + "alpha") << QStringLiteral("alpha") << false
            << QStringList{QStringLiteral("alpha"), QStringLiteral("alpha")} << QList<int>{2, 10};
        QTest::newRow("combining-before")
            << ("e" + combining + "clair alpha") << QStringLiteral("clair") << false
            << QStringList{QStringLiteral("clair")} << QList<int>{2};
        QTest::newRow("precomposed-folding")
            << QString::fromUtf8("caf\u00e9 cr\u00e8me") << QStringLiteral("cafe") << true
            << QStringList{QString::fromUtf8("caf\u00e9")} << QList<int>{0};
        QTest::newRow("decomposed-folding")
            << ("cafe" + combining + " au lait") << QString::fromUtf8("caf\u00e9") << true
            << QStringList{"cafe" + combining} << QList<int>{0};
        QTest::newRow("whole-scalar-pattern")
            << (emoji + QStringLiteral("alpha")) << QStringLiteral(".alpha") << false
            << QStringList{emoji + QStringLiteral("alpha")} << QList<int>{0};
        // The combining mark is removed from the normalized text, so a pattern
        // that continues past it maps back onto the original (mark included) and
        // the following visible character.
        QTest::newRow("combining-shift")
            << ("cafe" + combining + " next") << QStringLiteral("cafe.") << false
            << QStringList{"cafe" + combining + " "} << QList<int>{0};
        QTest::newRow("supplementary-pattern")
            << (deseret + QStringLiteral("word")) << deseret << false
            << QStringList{deseret} << QList<int>{0};
    }
    void ftsUnicodeMarksPreserveTextAndScalars()
    {
        QFETCH(QString, body);
        QFETCH(QString, pattern);
        QFETCH(bool, ignoreDiacritics);
        QFETCH(QStringList, marks);
        QFETCH(QList<int>, offsets);
        QDeadlineTimer deadline(18000);
        InteractionFixture f;
        f.a->setArticleBody((QStringLiteral("<p id='fts'>") + body + QStringLiteral("</p>")).toUtf8());
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        ArticleUrls::Lookup lookup;
        lookup.phrase = Config::InputPhrase::fromPhrase(QStringLiteral("fts"));
        lookup.group = 17; lookup.groupSpecified = true;
        lookup.dictionaries = QStringList{QStringLiteral("dict-a")};
        lookup.highlight.sourcePattern = pattern;
        auto parsed = ArticleUrls::parse(ArticleUrls::lookupDocument(lookup), ArticleUrls::Use::DocumentLoad);
        QVERIFY(parsed.lookup.highlight.expression.isValid());
        view->showDefinition("fts", QStringList{QStringLiteral("dict-a")},
                             parsed.lookup.highlight, 17, ignoreDiacritics);
        WAIT_FOR(pages.size() == 2);
        auto label = ftsSearchLabel(*view); QVERIFY(label);
        WAIT_FOR(!label->text().isEmpty());
        QCOMPARE(label->text(), QStringLiteral("1 of %1 matches").arg(marks.size()));
        QCOMPARE(markTexts(*web->page(), deadline), marks);
        QCOMPARE(markOffsets(QStringLiteral("fts"), *web->page(), deadline), offsets);
        QCOMPARE(evaluate(*web->page(), "document.getElementById('fts').textContent", deadline).toString(), body);
        QVERIFY(unicodeScalarsIntact(*web->page(), deadline));
        for (qsizetype i = 0; i < body.size(); ++i)
        {
            if (!body.at(i).isHighSurrogate()) continue;
            QVERIFY(i + 1 < body.size() && body.at(i + 1).isLowSurrogate());
        }
        evaluate(*web->page(), "gdClearHighlights()", deadline);
        QVERIFY(markTexts(*web->page(), deadline).isEmpty());
        QCOMPARE(evaluate(*web->page(), "document.getElementById('fts').textContent", deadline).toString(), body);
        QVERIFY(unicodeScalarsIntact(*web->page(), deadline));
    }

    // Marks, the active-match selection CSS and the hit navigation survive the
    // optional-part toggles, which only change presentation.
    void ftsNavigationSurvivesOptionalSections()
    {
        QDeadlineTimer deadline(20000);
        InteractionFixture f;
        f.a->setArticleBody(QByteArray("<p id='first'>testneedle one</p>"
            "<div style='height:1400px'></div>"
            "<p id='second'>testneedle two "
            "<span class='dsl_opt' style='display:none'>testneedle optional</span>"
            "<span class='hidden_expand_opt'>+</span></p>"));
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        ArticleUrls::Lookup lookup;
        lookup.phrase = Config::InputPhrase::fromPhrase(QStringLiteral("fts"));
        lookup.group = 17; lookup.groupSpecified = true;
        lookup.dictionaries = QStringList{QStringLiteral("dict-a")};
        lookup.highlight.sourcePattern = QStringLiteral("testneedle");
        auto parsed = ArticleUrls::parse(ArticleUrls::lookupDocument(lookup), ArticleUrls::Use::DocumentLoad);
        view->showDefinition("fts", QStringList{QStringLiteral("dict-a")},
                             parsed.lookup.highlight, 17, false);
        WAIT_FOR(pages.size() == 2);
        auto label = ftsSearchLabel(*view); QVERIFY(label);
        // The hidden optional match is not part of the visible text snapshot. The
        // count label is written before hit activation, so wait for the finished
        // state (label, marks and active match) rather than for the label alone.
        WAIT_FOR(label->text() == QStringLiteral("1 of 2 matches") &&
                 finishedFtsState(*web->page(), deadline) == QStringLiteral("testneedle|testneedle,testneedle"));
        QVERIFY(ftsSearchFrame(*view)->isVisible());
        QVERIFY(selectionRulePresent(*web->page(), deadline));
        QVERIFY(evaluate(*web->page(), "getSelection().toString() === 'testneedle'", deadline).toBool());
        searchTool(*view, "ftsSearchNext")->click();
        WAIT_FOR(label->text() == QStringLiteral("2 of 2 matches"));
        WAIT_FOR(evaluate(*web->page(), "scrollY > 500", deadline).toBool());
        auto inViewport = [&] {
            return evaluate(*web->page(), R"JS((() => {
                const marks = document.querySelectorAll('mark[data-gd-highlight]');
                const rect = marks[marks.length - 1].getBoundingClientRect();
                return rect.top >= 0 && rect.bottom <= innerHeight;
            })())JS", deadline).toBool();
        };
        WAIT_FOR(inViewport());
        QCOMPARE(evaluate(*web->page(), "getSelection().toString()", deadline).toString(), QString("testneedle"));
        QString const second = evaluate(*web->page(), "document.getElementById('second').textContent", deadline).toString();
        QCOMPARE(second, QString("testneedle two testneedle optional+"));
        view->receiveExpandOptionalParts(true);
        WAIT_FOR(evaluate(*web->page(),
            "getComputedStyle(document.querySelector('.dsl_opt')).display === 'inline'", deadline).toBool());
        QCOMPARE(markTexts(*web->page(), deadline).size(), 2);
        QVERIFY(evaluate(*web->page(),
            "Array.from(document.querySelectorAll('mark[data-gd-highlight]')).every(mark => mark.isConnected)",
            deadline).toBool());
        QCOMPARE(evaluate(*web->page(), "document.getElementById('second').textContent", deadline).toString(), second);
        QVERIFY(selectionRulePresent(*web->page(), deadline));
        QCOMPARE(evaluate(*web->page(), "getSelection().toString()", deadline).toString(), QString("testneedle"));
        QVERIFY(inViewport());
        QVERIFY(evaluate(*web->page(), "scrollY > 500", deadline).toBool());
        // A new run re-applies the same spans on the reloaded document without
        // touching its text.
        view->reload();
        WAIT_FOR(pages.size() == 3);
        WAIT_FOR(label->text() == QStringLiteral("1 of 2 matches"));
        QCOMPARE(evaluate(*web->page(), "document.getElementById('second').textContent", deadline).toString(), second);
        QCOMPARE(markTexts(*web->page(), deadline).size(), 2);
        QVERIFY(unicodeScalarsIntact(*web->page(), deadline));
    }

    // A DOM change between extraction and application re-extracts once; a second
    // change clears every mark and reports it instead of marking stale offsets.
    void ftsRetryRecapturesOnceThenReportsChange()
    {
        QDeadlineTimer deadline(30000);
        InteractionFixture f;
        f.a->setArticleBody(QByteArray("<p id='w'>testneedle one</p>"));
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy status(view.get(), &ArticleView::statusBarMessage);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        ArticleUrls::Lookup lookup;
        lookup.phrase = Config::InputPhrase::fromPhrase(QStringLiteral("fts"));
        lookup.group = 17; lookup.groupSpecified = true;
        lookup.dictionaries = QStringList{QStringLiteral("dict-a")};
        lookup.highlight.sourcePattern = QStringLiteral("testneedle");
        auto parsed = ArticleUrls::parse(ArticleUrls::lookupDocument(lookup), ArticleUrls::Use::DocumentLoad);
        auto const spec = parsed.lookup.highlight;
        auto label = ftsSearchLabel(*view); QVERIFY(label);
        holdScriptMarker = QStringLiteral("gdSaveSelection(");
        // One DOM change: the recapture applies the new snapshot and the user's
        // selection survives the retried marking operation.
        view->showDefinition("fts", QStringList{QStringLiteral("dict-a")}, spec, 17, false);
        WAIT_FOR(pages.size() == 2);
        WAIT_FOR(heldScripts.size() == 1);
        evaluate(*web->page(), "(() => {const n=document.getElementById('w').firstChild;"
            "const r=document.createRange();r.setStart(n,11);r.setEnd(n,14);"
            "const s=getSelection();s.removeAllRanges();s.addRange(r);return s.toString();})()", deadline);
        QCOMPARE(evaluate(*web->page(), "getSelection().toString()", deadline).toString(), QString("one"));
        evaluate(*web->page(), "document.getElementById('w').appendChild(document.createTextNode(' testneedle two'))",
                 deadline);
        heldScripts.takeFirst().deliver();
        WAIT_FOR(heldScripts.size() == 1);
        // Hold the application of the recaptured spans so the restored selection
        // is observable before hit activation replaces it.
        holdScriptMarker = QStringLiteral("gdRestoreSelection(");
        heldScripts.takeFirst().deliver();
        WAIT_FOR(heldScripts.size() == 1);
        QCOMPARE(markTexts(*web->page(), deadline), (QStringList{"testneedle", "testneedle"}));
        QCOMPARE(evaluate(*web->page(), "document.getElementById('w').textContent", deadline).toString(),
                 QString("testneedle one testneedle two"));
        QCOMPARE(evaluate(*web->page(), "getSelection().toString()", deadline).toString(), QString("one"));
        // Count label, marks and active match are written by two separate queued
        // operations, so one instant cannot describe the finished state: the
        // completion that carries the count queues hit activation behind every
        // operation already waiting (a debounced state capture or a frame probe,
        // as load produces). Queue one real operation here so the row always
        // covers that delay instead of hitting it by chance, then wait for the
        // whole described state rather than for the label alone.
        auto queued = std::make_shared<bool>(false);
        web->bridge->run(QStringLiteral("(() => {window.__gdQueuedProbe = true;return true;})()"),
                         [queued](QVariant const &) { *queued = true; });
        heldScripts.takeFirst().deliver();
        WAIT_FOR(label->text() == QStringLiteral("1 of 2 matches") &&
                 finishedFtsState(*web->page(), deadline) == QStringLiteral("testneedle|testneedle,testneedle"));
        QCOMPARE(evaluate(*web->page(), "getSelection().toString()", deadline).toString(),
                 QString("testneedle"));
        QCOMPARE(markTexts(*web->page(), deadline), (QStringList{"testneedle", "testneedle"}));
        QVERIFY(*queued);
        QVERIFY(unicodeScalarsIntact(*web->page(), deadline));
        // Two changes: the one retry is spent, so no stale offsets may mark.
        holdScriptMarker = QStringLiteral("gdSaveSelection(");
        f.a->setArticleBody(QByteArray("<p id='w'>testneedle one</p>"));
        view->showDefinition("fts", QStringList{QStringLiteral("dict-a")}, spec, 17, false);
        WAIT_FOR(pages.size() == 3);
        WAIT_FOR(heldScripts.size() == 1);
        evaluate(*web->page(), "document.getElementById('w').appendChild(document.createTextNode(' testneedle two'))",
                 deadline);
        heldScripts.takeFirst().deliver();
        WAIT_FOR(heldScripts.size() == 1);
        evaluate(*web->page(), "document.getElementById('w').appendChild(document.createTextNode(' testneedle three'))",
                 deadline);
        heldScripts.takeFirst().deliver();
        // The change report is emitted before the queue clears the marks, so the
        // abort is only complete once the status and the unmarked DOM agree.
        WAIT_FOR(statusText(status) == QStringLiteral("Article changed; run the search again.") &&
                 markTexts(*web->page(), deadline).isEmpty());
        QVERIFY(markTexts(*web->page(), deadline).isEmpty());
        QCOMPARE(evaluate(*web->page(), "document.getElementById('w').textContent", deadline).toString(),
                 QString("testneedle one testneedle two testneedle three"));
        QVERIFY(!ftsSearchFrame(*view)->isVisible());
        QVERIFY(unicodeScalarsIntact(*web->page(), deadline));
        holdScriptMarker.clear();
        heldScripts.clear();
    }

    // Document changes reset the count-bearing UI; callbacks from the previous
    // generation may arrive later but never touch the current document or bar.
    void staleFindAndFtsCannotLeakAcrossDocuments()
    {
        QDeadlineTimer deadline(30000);
        InteractionFixture f;
        f.a->setArticleBody(QByteArray("<p id='a'>alpha testneedle alpha</p>"));
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy status(view.get(), &ArticleView::statusBarMessage);
        WAIT_FOR(!pages.isEmpty() && view->getPhrase().phrase == "initial");
        auto label = ftsSearchLabel(*view); QVERIFY(label);
        ArticleUrls::Lookup lookup;
        lookup.phrase = Config::InputPhrase::fromPhrase(QStringLiteral("fts"));
        lookup.group = 17; lookup.groupSpecified = true;
        lookup.dictionaries = QStringList{QStringLiteral("dict-a")};
        lookup.highlight.sourcePattern = QStringLiteral("testneedle");
        auto parsed = ArticleUrls::parse(ArticleUrls::lookupDocument(lookup), ArticleUrls::Use::DocumentLoad);
        // A new document drops the count-bearing bar before any new result exists.
        f.a->setArticleBody(QByteArray("<p id='b'>testneedle and testneedle</p>"));
        view->showDefinition("fts", QStringList{QStringLiteral("dict-a")},
                             parsed.lookup.highlight, 17, false);
        WAIT_FOR(pages.size() == 2);
        WAIT_FOR(label->text() == QStringLiteral("1 of 2 matches"));
        QVERIFY(ftsSearchFrame(*view)->isVisible());
        holdScriptMarker = QStringLiteral("gdSaveSelection(");
        view->reload();
        WAIT_FOR(pages.size() == 3);
        WAIT_FOR(heldScripts.size() == 1);
        QVERIFY(!ftsSearchFrame(*view)->isVisible());
        QVERIFY(!label->text().contains(QStringLiteral("matches")));
        // The reloaded extraction is abandoned once the document is left again.
        f.a->setArticleBody(QByteArray("<p id='a'>alpha testneedle alpha</p>"));
        view->back();
        WAIT_FOR(pages.size() == 4);
        WAIT_FOR(view->getPhrase().phrase == "initial");
        int const beforeStatus = status.size();
        heldScripts.takeFirst().deliver();
        auto barrier = std::make_shared<bool>(false);
        web->bridge->run("true", [barrier](QVariant const &) { *barrier = true; });
        WAIT_FOR(*barrier);
        QTest::qWait(300);
        QVERIFY(heldScripts.isEmpty());
        QCOMPARE(status.size(), beforeStatus);
        QVERIFY(markTexts(*web->page(), deadline).isEmpty());
        QVERIFY(!ftsSearchFrame(*view)->isVisible());
        QVERIFY(!label->text().contains(QStringLiteral("matches")));
        // A native find result from the abandoned generation is equally inert.
        f.search.trigger(); QVERIFY(view->isSearchOpened());
        auto line = searchLine(*view); QVERIFY(line);
        holdFind = true; holdFindQuery = "testneedle";
        typeQuery(line, "testneedle");
        WAIT_FOR(heldFind.contains("testneedle"));
        view->reload();
        WAIT_FOR(pages.size() == 5);
        WAIT_FOR(view->getPhrase().phrase == "initial");
        // Snapshot only after the new generation is current: queued results of
        // the old query may legitimately land before this point.
        QTest::qWait(200);
        int const beforeReload = status.size();
        heldFind.take("testneedle").deliver(findResults.value("testneedle"));
        auto later = std::make_shared<bool>(false);
        web->bridge->run("true", [later](QVariant const &) { *later = true; });
        WAIT_FOR(*later);
        QCOMPARE(status.size(), beforeReload);
        QCOMPARE(line->property("noResults").toBool(), false);
        // The bar itself still serves the current document.
        typeQuery(line, "alpha");
        WAIT_FOR(statusText(status) == "1 of 2 matches");
    }

    void domHelpersQuotingSelectionHighlightsAndRestore()
    {
        QDeadlineTimer deadline(15000);
        Browser b;
        ArticleWebView view;
        view.resize(720, 480); view.show();
        view.configure(b.controller->profile(), &b.cfg);
        QSignalSpy ready(view.bridge, &ArticleBridge::bridgeReady);
        auto special = QString::fromUtf8("quotes '\" \\ \n 😀 \u2028 end");
        auto body = QStringLiteral("<p id='quoted' style='white-space:pre'></p><p id='matches'>😀alpha <b>ALPHA</b> alpha</p>"
            "<input value='alpha'><mark id='user-mark'>alpha</mark>"
            "<iframe id='frame' name='quoted-frame' style='border:3px solid' srcdoc='<p>frame</p>'></iframe>"
            "<div style='height:3000px'></div>");
        view.setHtml(document(body)); WAIT_FOR(ready.size() == 1);
        auto &page = *view.page();
        QCOMPARE(evaluate(page, "document.getElementById('quoted').textContent=" + quote(special), deadline).toString(), special);
        auto selected = evaluate(page, R"JS((() => {
            const range=document.createRange(); range.selectNodeContents(document.getElementById('quoted'));
            const s=getSelection(); s.removeAllRanges(); s.addRange(range);
            return gdSaveSelection();
        })())JS", deadline).toString();
        QVERIFY(!selected.isEmpty());
        evaluate(page, "getSelection().removeAllRanges(); gdRestoreSelection(" + quote(selected) + ")", deadline);
        QCOMPARE(evaluate(page, "getSelection().toString()", deadline).toString(), special);
        auto word = evaluate(page, R"JS((() => {
            const r=document.createRange(), n=document.getElementById('word').firstChild;
            r.setStart(n,1); r.setEnd(n,2); const b=r.getBoundingClientRect();
            return gdWordAtPoint(b.x+b.width/2,b.y+b.height/2);
        })())JS", deadline).toString();
        QCOMPARE(word, QString::fromUtf8("rock’n’roll"));
        QCOMPARE(evaluate(page, "getSelection().toString()", deadline).toString(), special);
        QCOMPARE(evaluate(page, "gdWordAtPoint(-10,-10)", deadline).toString(), QString{});
        evaluate(page, "gdClearHighlights()", deadline); // Captures the exact UTF-16 text/version.
        auto text = evaluate(page, "__gdTextSnapshot.text", deadline).toString();
        QRegularExpression pcre("(?<=😀)alpha|ALPHA"); // PCRE lookbehind, never passed to JS RegExp.
        QJsonArray spans;
        auto matches = pcre.globalMatch(text);
        while (matches.hasNext()) {
            auto match = matches.next();
            spans.append(QJsonObject{{"start", int(match.capturedStart())}, {"length", int(match.capturedLength())}});
        }
        QVERIFY(spans.size() >= 2);
        auto payload = QJsonDocument(QJsonObject{{"text", text}, {"spans", spans}}).toJson(QJsonDocument::Compact);
        QCOMPARE(evaluate(page, "gdHighlightAll(" + QString::fromUtf8(payload) + ",true)", deadline).toInt(), spans.size());
        QCOMPARE(evaluate(page, "document.querySelectorAll('mark[data-gd-highlight]').length", deadline).toInt(), spans.size());
        QVERIFY(evaluate(page, "document.querySelector('#matches b mark') !== null && document.querySelector('#user-mark').textContent === 'alpha'", deadline).toBool());
        QVERIFY(evaluate(page, "gdScrollToHit(0)", deadline).toBool());
        QVERIFY(!evaluate(page, "gdScrollToHit(999)", deadline).toBool());
        QCOMPARE(evaluate(page, "gdHighlightAll({text:'stale',spans:[{start:0,length:1}]},true)", deadline).toInt(), -1);
        QCOMPARE(evaluate(page, "document.querySelectorAll('mark[data-gd-highlight]').length", deadline).toInt(), 0);
        QVERIFY(evaluate(page, "document.getElementById('user-mark') !== null", deadline).toBool());
        auto rect = evaluate(page, "gdFrameRect('quoted-frame')", deadline).toMap();
        QCOMPARE(rect.value("borderLeft").toInt(), 3); QCOMPARE(rect.value("borderTop").toInt(), 3);
        QVERIFY(rect.value("width").toDouble() > 0);
        auto measure = evaluate(page, "gdMeasureFrame()", deadline).toMap();
        QVERIFY(!measure.value("docId").toString().isEmpty()); QVERIFY(measure.value("height").toDouble() >= 3000);
        QCOMPARE(evaluate(page, "gdFrameRect('absent')===null", deadline).toBool(), true);
        evaluate(page, "window.restored=gdRestoreArticleState({currentArticle:'gdfrom-dict-b',sx:0,sy:700})", deadline);
        WAIT_FOR(evaluate(page, "window.restored.completed", deadline).toBool());
        auto captured = evaluate(page, "gdCaptureState()", deadline).toMap();
        QCOMPARE(captured.value("currentArticle").toString(), QStringLiteral("gdfrom-dict-b"));
        QCOMPARE(captured.value("sy").toInt(), 700);
        QVERIFY(evaluate(page, "document.getElementById('gdfrom-dict-b').classList.contains('gdactivearticle')", deadline).toBool());
        evaluate(page, "window.restored=gdRestoreArticleState({currentArticle:'gdfrom-dict-a',sx:0,sy:700,restoreScroll:false})", deadline);
        WAIT_FOR(evaluate(page, "window.restored.completed", deadline).toBool());
        QCOMPARE(evaluate(page, "scrollY", deadline).toInt(), 0);
        evaluate(page, R"JS((() => {
            const r=document.createRange(); r.selectNodeContents(document.getElementById('quoted'));
            const s=getSelection(); s.removeAllRanges(); s.addRange(r);
            const token=gdSaveSelection(); s.removeAllRanges();
            ++window.__gdDocumentGeneration; gdRestoreSelection(token); --window.__gdDocumentGeneration;
        })())JS", deadline);
        QCOMPARE(evaluate(page, "getSelection().toString()", deadline).toString(), QString{});
        evaluate(page, "gdClearHighlights()", deadline);
        auto emojiText = evaluate(page, "__gdTextSnapshot.text", deadline).toString();
        int const emoji = emojiText.indexOf(QString::fromUtf8("😀"));
        QVERIFY(emoji >= 0);
        auto splitPair = QJsonDocument(QJsonObject{{"text", emojiText}, {"spans", QJsonArray{
            QJsonObject{{"start", emoji + 1}, {"length", 1}}}}}).toJson(QJsonDocument::Compact);
        QCOMPARE(evaluate(page, "gdHighlightAll(" + QString::fromUtf8(splitPair) + ",true)", deadline).toInt(), 0);
        QVERIFY(evaluate(page, "document.body.textContent.includes('😀')", deadline).toBool());
    }
};

int main(int argc, char **argv)
{
    if (TestSupport::isWatchdogProbeProcess()) TestSupport::runWatchdogProbe();
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) return 1;
    qputenv("XDG_CACHE_HOME", sandbox.path().toUtf8());
    qputenv("XDG_DATA_HOME", sandbox.path().toUtf8());
    registerArticleSchemes();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("GoldenDict");
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    ArticleBridgeTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_article_bridge.moc"
