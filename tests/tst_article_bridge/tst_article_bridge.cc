/* Real Chromium startup, DOM and asynchronous retirement contracts. */
#include <QtTest>
#include <QApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <limits>

#include "articlebridge.hh"
#include "articleprofile.hh"
#include "articlewebview.hh"
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

// Link wrapping affects only the production call sites in this executable. It
// still evaluates on the real page; only delivery of tagged results is held.
void observe(QString const &script, ArticleBridge::Result const &callback,
             std::function<void(ArticleBridge::Result)> const &execute)
{
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
    void cleanup() { held.clear(); dispatched.clear(); }
    void cleanupTestCase() { TestSupport::clearWatchdog(); }

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
