/* Real remote origins, native frame identities, and per-document reinstrumentation. */
#include <QtTest>
#include <QApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <algorithm>
#include <limits>
#include "articlebridge.hh"
#include "websiteframes.hh"
#include "articleschemes.hh"
#include "website.hh"
#include "support/fixtures.hh"
#include "support/http_fixture.hh"

namespace {
#define WAIT_FOR(condition) \
    QVERIFY2(QTest::qWaitFor([&] { return bool(condition); }, int(qMax<qint64>(0, deadline.remainingTime()))), #condition)
struct Completion { ArticleBridge::Result callback; QVariant value; };
QVector<Completion> held;
QString hold;
bool reverseChildren = false, oscillate = false;
int discoveries = 0, samples = 0, invalidForwards = 0;
QElapsedTimer observationClock;
QVector<qint64> sampleTimes;
QString quote(QString const &value)
{
    auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}
QString object(QVariantMap const &value)
{
    return QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact));
}
QVariant evaluate(QWebEngineFrame frame, QString const &script, QDeadlineTimer const &deadline)
{
    struct Answer { bool done = false; QVariant value; };
    auto result = std::make_shared<Answer>();
    frame.runJavaScript(script, QWebEngineScript::MainWorld, [result](QVariant const &value) { result->value = value; result->done = true; });
    if (!QTest::qWaitFor([&] { return result->done; }, int(qMax<qint64>(0, deadline.remainingTime()))))
        QTest::qFail("real frame JavaScript deadline", __FILE__, __LINE__);
    return result->value;
}
void barrier(ArticleBridge &bridge, QDeadlineTimer const &deadline)
{
    auto done = std::make_shared<bool>(false);
    bridge.run("/*test-barrier*/true", [done](QVariant const &) { *done = true; });
    if (!QTest::qWaitFor([&] { return *done; }, int(qMax<qint64>(0, deadline.remainingTime()))))
        QTest::qFail("bridge queue barrier deadline", __FILE__, __LINE__);
}
QString document(QUrl const &url, QString const &extra = {})
{
    return QStringLiteral("<!doctype html><meta charset='utf-8'><script>"
      "gdArticleContents=['dict-a','dict-b'];gdCurrentArticle='gdfrom-dict-a';</script>"
      "<body style='margin:0'><div id='gdfrom-dict-a'>%1</div>"
      "<div id='gdfrom-dict-b'></div>%2</body>")
      .arg(WebSite::detail::websiteIframeMarkup("dict-a", url.toString()), extra);
}
QVariantMap readyAt(QSignalSpy const &events, int index, int depth = 1)
{
    for (auto const &args : events)
    {
        auto event = args[0].toMap();
        if (event.value("gdFrameEvent").toString() == "ready" && event.value("path").toList().size() == depth && --index < 0)
            return event;
    }
    return {};
}
int eventCount(QSignalSpy const &events, QString const &kind, QString const &nonce = {})
{
    int count = 0;
    for (auto const &args : events)
    {
        auto value = args[0].toMap();
        if (value.value("gdFrameEvent").toString() == kind &&
            (nonce.isEmpty() || value.value("docId").toString() == nonce)) ++count;
    }
    return count;
}
bool threeEqual(QSignalSpy const &events, QString const &nonce)
{
    QVector<double> heights;
    for (auto const &args : events)
    {
        auto event = args[0].toMap();
        if (event.value("gdFrameEvent").toString() == "measure" && event.value("docId").toString() == nonce)
            heights.append(event.value("height").toDouble());
    }
    return heights.size() >= 3 && heights[heights.size() - 1] == heights[heights.size() - 2] &&
        heights[heights.size() - 2] == heights[heights.size() - 3];
}
QWebEngineFrame remote(QWebEnginePage &page, QString const &marker, QDeadlineTimer const &deadline)
{
    QWebEngineFrame found;
    std::function<void(QWebEngineFrame)> visit = [&](QWebEngineFrame frame) {
        if (frame.url().path() == marker) { found = frame; return; }
        for (auto const &child : frame.children()) visit(child);
    };
    if (!QTest::qWaitFor([&] { visit(page.mainFrame()); return found.isValid(); }, int(qMax<qint64>(0, deadline.remainingTime()))))
        QTest::qFail("native remote frame deadline", __FILE__, __LINE__);
    return found;
}
QPointF glyph(QWebEngineFrame frame, QString const &selector, QDeadlineTimer const &deadline)
{
    auto value = evaluate(frame, QStringLiteral("(()=>{const e=document.querySelector(%1),r=document.createRange();"
        "r.setStart(e.firstChild,1);r.setEnd(e.firstChild,2);const b=r.getBoundingClientRect();"
        "return {x:b.x+b.width/2,y:b.y+b.height/2}})()").arg(quote(selector)), deadline).toMap();
    return {value.value("x").toDouble(), value.value("y").toDouble()};
}
QPointF contentOrigin(QWebEngineFrame parent, QString const &selector, QDeadlineTimer const &deadline)
{
    auto value = evaluate(parent, QStringLiteral("(()=>{const e=document.querySelector(%1),r=e.getBoundingClientRect(),"
        "s=getComputedStyle(e);return {x:r.x+e.clientLeft+(parseFloat(s.paddingLeft)||0),"
        "y:r.y+e.clientTop+(parseFloat(s.paddingTop)||0)}})()").arg(quote(selector)), deadline).toMap();
    return {value.value("x").toDouble(), value.value("y").toDouble()};
}
QString wordAt(WebsiteFrames &frames, QPointF point, QDeadlineTimer const &deadline)
{
    struct Answer { bool done = false; QString word; int calls = 0; };
    auto result = std::make_shared<Answer>();
    frames.wordAtPoint(point, [result](QString const &word) { result->done = true; result->word = word; ++result->calls; });
    if (!QTest::qWaitFor([&] { return result->done; }, int(qMax<qint64>(0, deadline.remainingTime()))))
        QTest::qFail("word lookup deadline", __FILE__, __LINE__);
    if (result->calls != 1) QTest::qFail("word callback was not exactly once", __FILE__, __LINE__);
    return result->word;
}
struct Browser
{
    QWebEngineProfile profile;
    QWebEnginePage page{&profile};
    QWebEngineView view;
    QObject owner;
    ArticleBridge bridge{&page, &owner};
    WebsiteFrames frames{&page, &bridge};
    Browser()
    {
        // Match the article profile's local-content policy and stable origin.
        page.settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
        view.resize(1300, 1100); view.setPage(&page); view.show();
    }
    void load(QUrl const &url, QString const &extra = {})
    {
        page.setHtml(document(url, extra), QUrl("gdlookup://localhost/"));
    }
    bool visible(QDeadlineTimer const &deadline)
    {
        return evaluate(page.mainFrame(), "(()=>{const e=document.querySelector('iframe');return e&&"
            "e.style.display==='block'&&parseFloat(e.style.height)>0})()", deadline).toBool();
    }
};
}

// Observe real production calls before forwarding. Invalid native handles are
// rejected here as well: a regression must fail a test, never crash Chromium.
extern "C" void realFrameRun(QWebEngineFrame *, QString const &, quint32, ArticleBridge::Result const &)
    asm("__real__ZN15QWebEngineFrame13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE");
extern "C" void wrapFrameRun(QWebEngineFrame *, QString const &, quint32, ArticleBridge::Result const &)
    asm("__wrap__ZN15QWebEngineFrame13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE");
extern "C" void wrapFrameRun(QWebEngineFrame *frame, QString const &source, quint32 world, ArticleBridge::Result const &callback)
{
    if (!frame->isValid()) { ++invalidForwards; callback({}); return; }
    QString script = source;
    if (source.startsWith("/*gd-frame-discover*/")) ++discoveries;
    if (source.startsWith("/*gd-frame-sample*/"))
    {
        ++samples; sampleTimes.append(observationClock.elapsed());
        if (oscillate) script.replace("const m=gdMeasureFrame();", QString("document.getElementById('height-target').style.height='%1px';const m=gdMeasureFrame();").arg(300 + (samples % 2) * 100));
    }
    if (!hold.isEmpty() && source.startsWith(hold))
        realFrameRun(frame, script, world, [callback](QVariant const &value) { held.append({callback, value}); });
    else realFrameRun(frame, script, world, callback);
}
extern "C" QList<QWebEngineFrame> realChildren(QWebEngineFrame const *) asm("__real__ZNK15QWebEngineFrame8childrenEv");
extern "C" QList<QWebEngineFrame> wrapChildren(QWebEngineFrame const *) asm("__wrap__ZNK15QWebEngineFrame8childrenEv");
extern "C" QList<QWebEngineFrame> wrapChildren(QWebEngineFrame const *frame)
{
    auto result = realChildren(frame);
    if (reverseChildren) std::reverse(result.begin(), result.end());
    return result;
}

class WebsiteFramesTest : public QObject
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
    void init()
    {
        held.clear(); hold.clear(); reverseChildren = false; oscillate = false;
        discoveries = samples = invalidForwards = 0; sampleTimes.clear(); observationClock.start();
    }
    void cleanup() { QCOMPARE(invalidForwards, 0); hold.clear(); held.clear(); }
    void cleanupTestCase() { TestSupport::clearWatchdog(); }

    void firstAndSecondNavigation()
    {
        QDeadlineTimer deadline(18000);
        TestSupport::HttpFixture origin, other;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        QVERIFY2(other.start(QHostAddress("127.0.0.3")), qPrintable(other.errorString()));
        Browser b;
        auto installed = b.page.scripts().find("gd-frames-1");
        QCOMPARE(installed.size(), 1);
        QCOMPARE(installed.first().injectionPoint(), QWebEngineScript::DocumentCreation);
        QCOMPARE(installed.first().worldId(), quint32(QWebEngineScript::MainWorld));
        QVERIFY(installed.first().runsOnSubFrames());
        QCOMPARE(b.page.scripts().count(), 3);
        QSignalSpy events(&b.bridge, &ArticleBridge::frameEvent), text(&b.frames, &WebsiteFrames::frameText);
        b.load(origin.url("/dir/first.html"));
        WAIT_FOR(!readyAt(events, 0).isEmpty());
        WAIT_FOR(b.visible(deadline));
        auto frame = remote(b.page, "/dir/first.html", deadline);
        WAIT_FOR(evaluate(frame, "document.documentElement.dataset.originFetch", deadline).toString() == "real-origin-fetch");
        QCOMPARE(frame.url(), origin.url("/dir/first.html"));
        QCOMPARE(evaluate(frame, "location.origin", deadline).toString(), origin.url("/").toString(QUrl::RemovePath));
        QCOMPARE(evaluate(frame, "typeof articleview+' '+typeof qt", deadline).toString(), QStringLiteral("undefined undefined"));
        QCOMPARE(evaluate(frame, "getComputedStyle(document.body).color", deadline).toString(), QStringLiteral("rgb(17, 34, 51)"));
        QVERIFY(evaluate(frame, "document.getElementById('relative-image').complete", deadline).toBool());
        for (auto path : {"/dir/relative.js", "/dir/data.json", "/dir/style.css", "/dir/pixel.png"})
            QVERIFY(origin.requestCount(path) > 0);
        for (auto const &request : origin.requests())
            QCOMPARE(request.host, QByteArray("127.0.0.2:") + QByteArray::number(origin.port()));
        auto firstReady = readyAt(events, 0);
        auto topDoc = evaluate(b.page.mainFrame(), "__gdWebsiteFrame.docId", deadline);
        double firstHeight = evaluate(b.page.mainFrame(), "parseFloat(document.querySelector('iframe').style.height)", deadline).toDouble();
        evaluate(frame, "window.name='page-changed-name';const a=document.getElementById('click-target');"
            "a.innerHTML='<span>enclosing <b>link</b> text</span>';a.addEventListener('click',e=>e.preventDefault());"
            "a.querySelector('b').click()", deadline);
        WAIT_FOR(text.size() == 1);
        QCOMPARE(text.last(), (QList<QVariant>{QStringLiteral("dict-a"), QStringLiteral("enclosing link text"), false}));
        QCOMPARE(evaluate(frame, "gdLastUrlText", deadline).toString(), QStringLiteral("enclosing link text"));
        for (double zoom : {1.0, 2.0})
        {
            b.page.setZoomFactor(zoom);
            auto point = (contentOrigin(b.page.mainFrame(), "iframe", deadline) + glyph(frame, "#word-target", deadline)) * zoom;
            QCOMPARE(wordAt(b.frames, point, deadline), QStringLiteral("wordword"));
        }
        b.page.setZoomFactor(1);
        auto context = glyph(frame, "#context-target", deadline);
        evaluate(frame, QString("document.getElementById('context-target').dispatchEvent(new MouseEvent('contextmenu',"
            "{bubbles:true,clientX:%1,clientY:%2}))").arg(context.x()).arg(context.y()), deadline);
        WAIT_FOR(text.size() == 2);
        QCOMPARE(text.last(), (QList<QVariant>{QStringLiteral("dict-a"), QStringLiteral("contextword"), true}));
        // A real relative form navigates only the child; its top generation is unchanged.
        evaluate(frame, "document.getElementById('get-form').requestSubmit()", deadline);
        WAIT_FOR(!readyAt(events, 1).isEmpty());
        auto secondReady = readyAt(events, 1);
        QCOMPARE(firstReady.value("gdFrameEvent").toString(), QStringLiteral("ready"));
        QCOMPARE(secondReady.value("gdFrameEvent").toString(), QStringLiteral("ready"));
        QCOMPARE(firstReady.value("name").toString(), QStringLiteral("gdexpandframe-dict-a"));
        QCOMPARE(secondReady.value("name").toString(), firstReady.value("name").toString());
        QVERIFY(!firstReady.value("docId").toString().isEmpty());
        QVERIFY(!secondReady.value("docId").toString().isEmpty());
        QVERIFY(firstReady.value("docId") != secondReady.value("docId"));
        QCOMPARE(firstReady.value("generation"), secondReady.value("generation"));
        QCOMPARE(evaluate(b.page.mainFrame(), "__gdWebsiteFrame.docId", deadline), topDoc);
        WAIT_FOR(eventCount(events, "measure", secondReady.value("docId").toString()) >= 3);
        frame = remote(b.page, "/dir/second.html", deadline);
        QCOMPARE(frame.url().query(), QStringLiteral("q=form-query"));
        WAIT_FOR(evaluate(frame, "document.documentElement.dataset.originFetch", deadline).toString() == "real-origin-fetch");
        double secondHeight = evaluate(b.page.mainFrame(), "parseFloat(document.querySelector('iframe').style.height)", deadline).toDouble();
        QVERIFY(secondHeight != firstHeight);
        for (double zoom : {1.0, 2.0})
        {
            b.page.setZoomFactor(zoom);
            QCOMPARE(wordAt(b.frames, (contentOrigin(b.page.mainFrame(), "iframe", deadline) + glyph(frame, "#word-target", deadline)) * zoom, deadline), QStringLiteral("wordword"));
        }
        b.page.setZoomFactor(1);
        evaluate(frame, "const a=document.getElementById('second-click-target');a.addEventListener('click',e=>e.preventDefault());a.click()", deadline);
        WAIT_FOR(text.size() == 3);
        QCOMPARE(text.last()[1].toString(), QStringLiteral("back to the first page"));
        context = glyph(frame, "#context-target", deadline);
        evaluate(frame, QString("document.getElementById('context-target').dispatchEvent(new MouseEvent('contextmenu',"
            "{bubbles:true,clientX:%1,clientY:%2}))").arg(context.x()).arg(context.y()), deadline);
        WAIT_FOR(text.size() == 4);
        QCOMPARE(text.last()[1].toString(), QStringLiteral("contextword"));
        QElapsedTimer recovery; recovery.start();
        evaluate(b.page.mainFrame(), "(()=>{const f=document.querySelector('iframe');f.name=f.id='gdexpandframe-dict-b';"
            "document.getElementById('gdfrom-dict-b').appendChild(f);f.src=" + quote(other.url("/dir/first.html").toString()) + "})()", deadline);
        WAIT_FOR(!readyAt(events, 2).isEmpty());
        auto third = readyAt(events, 2);
        QCOMPARE(third.value("name").toString(), QStringLiteral("gdexpandframe-dict-b"));
        QVERIFY(third.value("docId") != secondReady.value("docId"));
        WAIT_FOR(eventCount(events, "measure", third.value("docId").toString()) >= 3);
        QVERIFY2(recovery.elapsed() < 1500, "third-document reinstrumentation must use frame retirement, not the timeout backstop");
        frame = remote(b.page, "/dir/first.html", deadline);
        QCOMPARE(frame.url().host(), QStringLiteral("127.0.0.3"));
        QCOMPARE(wordAt(b.frames, contentOrigin(b.page.mainFrame(), "iframe", deadline) + glyph(frame, "#word-target", deadline), deadline), QStringLiteral("wordword"));
        evaluate(frame, "const a=document.getElementById('click-target');a.addEventListener('click',e=>e.preventDefault());a.click()", deadline);
        WAIT_FOR(text.size() == 5);
        QCOMPARE(text.last()[0].toString(), QStringLiteral("dict-b"));
        QCOMPARE(eventCount(events, "ready"), 3);
    }

    void nestedBordersAndNativeOrdering_data()
    {
        QTest::addColumn<double>("zoom");
        QTest::newRow("100-percent") << 1.0;
        QTest::newRow("200-percent") << 2.0;
    }
    void nestedBordersAndNativeOrdering()
    {
        QFETCH(double, zoom);
        QDeadlineTimer deadline(14000);
        TestSupport::HttpFixture a, b, c;
        QVERIFY2(a.start(QHostAddress("127.0.0.1")), qPrintable(a.errorString()));
        QVERIFY2(b.start(QHostAddress("127.0.0.2")), qPrintable(b.errorString()));
        QVERIFY2(c.start(QHostAddress("127.0.0.3")), qPrintable(c.errorString()));
        Browser browser;
        QSignalSpy events(&browser.bridge, &ArticleBridge::frameEvent), text(&browser.frames, &WebsiteFrames::frameText);
        browser.page.setZoomFactor(zoom);
        browser.load(b.url("/dir/nested.html"), "<iframe name='unrelated' src='" + a.url("/dir/first.html").toString() + "'></iframe>");
        WAIT_FOR(!readyAt(events, 0, 2).isEmpty());
        WAIT_FOR(browser.visible(deadline));
        auto parent = remote(browser.page, "/dir/nested.html", deadline);
        evaluate(parent, "document.getElementById('nested-frame').name='';document.getElementById('nested-frame').src=" + quote(c.url("/dir/second.html").toString()), deadline);
        WAIT_FOR(!readyAt(events, 1, 2).isEmpty());
        auto nestedReady = readyAt(events, 1, 2);
        QCOMPARE(nestedReady.value("path").toList()[1].toMap().value("name").toString(), QString());
        WAIT_FOR(eventCount(events, "measure", nestedReady.value("docId").toString()) >= 3);
        auto nested = remote(browser.page, "/dir/second.html", deadline);
        QCOMPARE(nested.url().host(), QStringLiteral("127.0.0.3"));
        evaluate(browser.page.mainFrame(), "document.querySelector('iframe').style.border='7px solid';"
            "document.querySelector('iframe').style.padding='6px';document.body.style.paddingTop='200px';scrollTo(0,250)", deadline);
        // Deliberately make the native children order differ from window.frames.
        reverseChildren = true;
        browser.frames.discover();
        int before = discoveries;
        WAIT_FOR(discoveries >= before + 8);
        auto point = (contentOrigin(browser.page.mainFrame(), "#gdexpandframe-dict-a", deadline) +
            contentOrigin(parent, "#nested-frame", deadline) + glyph(nested, "#word-target", deadline)) * zoom;
        QCOMPARE(wordAt(browser.frames, point, deadline), QStringLiteral("wordword"));
        auto nestedContext = glyph(nested, "#context-target", deadline);
        evaluate(nested, QString("document.getElementById('context-target').dispatchEvent(new MouseEvent('contextmenu',"
            "{bubbles:true,clientX:%1,clientY:%2}))").arg(nestedContext.x()).arg(nestedContext.y()), deadline);
        WAIT_FOR(text.size() == 1);
        QCOMPARE(text.first(), (QList<QVariant>{QStringLiteral("dict-a"), QStringLiteral("contextword"), true}));
        // Both native caret implementations leave a pre-existing selection intact.
        auto original = evaluate(nested, "(()=>{const r=document.createRange();r.selectNodeContents(document.querySelector('h1'));"
            "getSelection().removeAllRanges();getSelection().addRange(r);return getSelection().toString()})()", deadline);
        QCOMPARE(wordAt(browser.frames, point, deadline), QStringLiteral("wordword"));
        QCOMPARE(evaluate(nested, "getSelection().toString()", deadline), original);
        evaluate(nested, "document.caretPositionFromPoint=undefined", deadline);
        QCOMPARE(wordAt(browser.frames, point, deadline), QStringLiteral("wordword"));
        QCOMPARE(evaluate(nested, "getSelection().toString()", deadline), original);
        evaluate(nested, "document.caretRangeFromPoint=undefined", deadline);
        QCOMPARE(wordAt(browser.frames, point, deadline), QString());
        QCOMPARE(wordAt(browser.frames, QPointF(-100, -100), deadline), QString());
        QCOMPARE(wordAt(browser.frames, QPointF(std::numeric_limits<double>::infinity(), 0), deadline), QString());
    }

    void samplingAndResize()
    {
        QDeadlineTimer deadline(15000);
        TestSupport::HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        Browser b;
        QSignalSpy events(&b.bridge, &ArticleBridge::frameEvent);
        b.load(origin.url("/dir/first.html"));
        WAIT_FOR(!readyAt(events, 0).isEmpty());
        auto nonce = readyAt(events, 0).value("docId").toString();
        WAIT_FOR(threeEqual(events, nonce));
        barrier(b.bridge, deadline);
        int settled = samples, probes = discoveries;
        WAIT_FOR(discoveries >= probes + 8);
        QCOMPARE(samples, settled);
        QVERIFY(sampleTimes.size() >= 3);
        for (int i = sampleTimes.size() - 2; i < sampleTimes.size(); ++i)
        {
            QVERIFY2(sampleTimes[i] - sampleTimes[i - 1] >= 235, "sampling must use the 250ms timer");
            QVERIFY2(sampleTimes[i] - sampleTimes[i - 1] < 450, "sampling must rearm every 250ms");
        }
        auto frame = remote(b.page, "/dir/first.html", deadline);
        auto size = evaluate(frame, "gdMeasureFrame()", deadline).toMap();
        evaluate(frame, "parent.postMessage({gdFrameEvent:'resize',generation:__gdDocumentGeneration,docId:__gdWebsiteFrame.docId,"
            "path:[],height:gdMeasureFrame().height,width:gdMeasureFrame().width},'*')", deadline);
        probes = discoveries;
        WAIT_FOR(discoveries >= probes + 8);
        QCOMPARE(samples, settled);
        evaluate(frame, "document.getElementById('height-target').style.height='800px'", deadline);
        WAIT_FOR(samples >= settled + 3 && threeEqual(events, nonce));
        barrier(b.bridge, deadline);
        double height = evaluate(b.page.mainFrame(), "parseFloat(document.querySelector('iframe').style.height)", deadline).toDouble();
        QVERIFY(height > size.value("height").toDouble());
        settled = samples; probes = discoveries;
        WAIT_FOR(discoveries >= probes + 8);
        QCOMPARE(samples, settled);
    }

    void rejectsSpoofsAndStaleDocuments()
    {
        QDeadlineTimer deadline(14000);
        TestSupport::HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        Browser b;
        QSignalSpy events(&b.bridge, &ArticleBridge::frameEvent), text(&b.frames, &WebsiteFrames::frameText);
        b.load(origin.url("/dir/first.html"), "<iframe name='gdexpandframe-dict-a' src='" + origin.url("/dir/second.html").toString() + "'></iframe>");
        WAIT_FOR(!readyAt(events, 0).isEmpty());
        auto first = readyAt(events, 0);
        WAIT_FOR(eventCount(events, "measure", first.value("docId").toString()) >= 3);
        auto frame = remote(b.page, "/dir/first.html", deadline);
        auto rogue = remote(b.page, "/dir/second.html", deadline);
        evaluate(rogue, "parent.postMessage({gdFrameEvent:'click',generation:1,docId:__gdWebsiteFrame.docId,path:[],text:'rogue'},'*')", deadline);
        auto forged = first; forged["gdFrameEvent"] = "click"; forged["text"] = "forged";
        auto path = forged.value("path").toList(); path.append(QVariantMap{{"name", "invented-ancestor"}, {"index", 0}}); forged["path"] = path;
        // Deliver through the actual relay and the actual WebChannel entry point.
        evaluate(frame, "parent.postMessage(" + object(forged) + ",'*')", deadline);
        forged = first; forged["gdFrameEvent"] = "click"; forged["text"] = "forged";
        forged["path"] = QVariantList{QVariantMap{{"name", "gdexpandframe-dict-a"}, {"index", 1}}};
        evaluate(b.page.mainFrame(), "articleview.onJsFrameEvent(" + object(forged) + ")", deadline);
        int probes = discoveries;
        WAIT_FOR(discoveries >= probes + 12);
        QCOMPARE(text.size(), 0);
        int before = samples;
        auto validSize = evaluate(frame, "gdMeasureFrame()", deadline).toMap();
        auto validHeight = evaluate(b.page.mainFrame(), "document.querySelector('iframe').style.height", deadline);
        for (auto bad : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 16777217.0})
        {
            auto message = first; message["gdFrameEvent"] = "measure"; message["height"] = bad; message["width"] = validSize["width"];
            b.bridge.onJsFrameEvent(message, b.bridge.generation());
            message["gdFrameEvent"] = "resize"; b.bridge.onJsFrameEvent(message, b.bridge.generation());
            message["height"] = validSize["height"]; message["width"] = bad; b.bridge.onJsFrameEvent(message, b.bridge.generation());
        }
        evaluate(frame, "parent.postMessage({gdFrameEvent:'resize',generation:1,docId:__gdWebsiteFrame.docId,"
            "path:[],height:Infinity,width:100},'*');parent.postMessage({gdFrameEvent:'unknown',generation:1,"
            "docId:__gdWebsiteFrame.docId,path:[],text:'bad'},'*')", deadline);
        probes = discoveries; WAIT_FOR(discoveries >= probes + 12);
        QCOMPARE(samples, before);
        QCOMPARE(evaluate(b.page.mainFrame(), "document.querySelector('iframe').style.height", deadline), validHeight);
        // Pin text truncation through the real frame, not a conversion helper.
        evaluate(frame, "const a=document.createElement('a');a.textContent='x'.repeat(65540);document.body.append(a);a.click()", deadline);
        WAIT_FOR(text.size() == 1);
        QCOMPARE(text.last()[1].toString().size(), 65536);
        evaluate(b.page.mainFrame(), "document.querySelector('iframe').src=" + quote(origin.url("/dir/nested.html").toString()), deadline);
        WAIT_FOR(!readyAt(events, 1).isEmpty());
        auto second = readyAt(events, 1);
        WAIT_FOR(eventCount(events, "measure", second.value("docId").toString()) >= 3);
        int oldMeasures = eventCount(events, "measure", first.value("docId").toString());
        auto stale = first; stale["gdFrameEvent"] = "click"; stale["text"] = "stale";
        b.bridge.onJsFrameEvent(stale, b.bridge.generation());
        stale["gdFrameEvent"] = "ready"; b.bridge.onJsFrameEvent(stale, b.bridge.generation());
        stale["gdFrameEvent"] = "resize"; stale["height"] = 10000; stale["width"] = 100; b.bridge.onJsFrameEvent(stale, b.bridge.generation());
        probes = discoveries; WAIT_FOR(discoveries >= probes + 12);
        QCOMPARE(text.size(), 1);
        QCOMPARE(eventCount(events, "measure", first.value("docId").toString()), oldMeasures);
    }

    void frameOperationTimeoutReleasesQueue()
    {
        QDeadlineTimer deadline(14000);
        TestSupport::HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        Browser b;
        QSignalSpy events(&b.bridge, &ArticleBridge::frameEvent);
        b.load(origin.url("/dir/first.html"));
        WAIT_FOR(!readyAt(events, 0).isEmpty()); WAIT_FOR(b.visible(deadline));
        auto frame = remote(b.page, "/dir/first.html", deadline);
        auto point = contentOrigin(b.page.mainFrame(), "iframe", deadline) + glyph(frame, "#word-target", deadline);
        hold = "/*gd-frame-word*/";
        struct Lookup { int callbacks = 0; };
        auto lookup = std::make_shared<Lookup>();
        b.frames.wordAtPoint(point, [lookup](QString const &) { ++lookup->callbacks; });
        WAIT_FOR(held.size() == 1);
        WAIT_FOR(lookup->callbacks == 1);

        bool barrierDone = false;
        QElapsedTimer elapsed; elapsed.start();
        b.bridge.run("/*test-timeout-barrier*/true", [&barrierDone](QVariant const &) { barrierDone = true; });
        QVERIFY2(QTest::qWaitFor([&] { return barrierDone; }, 5000),
                 "a lost frame completion must retire before the queue deadline");
        QVERIFY(elapsed.elapsed() < 4000);
        QCOMPARE(held.size(), 1);

        auto delayed = held.takeFirst();
        hold.clear();
        delayed.callback(delayed.value);
        barrier(b.bridge, deadline);
        QCOMPARE(lookup->callbacks, 1);
    }

    void boundedLookupAndRetirement()
    {
        QDeadlineTimer deadline(14000);
        TestSupport::HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        Browser b;
        QSignalSpy events(&b.bridge, &ArticleBridge::frameEvent), text(&b.frames, &WebsiteFrames::frameText);
        b.load(origin.url("/dir/first.html"));
        WAIT_FOR(!readyAt(events, 0).isEmpty()); WAIT_FOR(b.visible(deadline));
        auto frame = remote(b.page, "/dir/first.html", deadline);
        auto point = contentOrigin(b.page.mainFrame(), "iframe", deadline) + glyph(frame, "#word-target", deadline);
        QCOMPARE(wordAt(b.frames, point, deadline), QStringLiteral("wordword"));
        hold = "/*gd-frame-word*/";
        struct Lookup { int callbacks = 0; QString answer; };
        auto lookup = std::make_shared<Lookup>();
        QElapsedTimer elapsed; elapsed.start();
        b.frames.wordAtPoint(point, [lookup](QString const &word) { lookup->answer = word; ++lookup->callbacks; });
        WAIT_FOR(held.size() == 1);
        WAIT_FOR(lookup->callbacks == 1);
        QVERIFY(elapsed.elapsed() < 1000);
        QCOMPARE(lookup->answer, QString());
        auto delayed = held.takeFirst(); hold.clear(); delayed.callback(delayed.value);
        barrier(b.bridge, deadline); QCOMPARE(lookup->callbacks, 1);
        // Hold a real frame-local result and remove its frame before delivery.
        hold = "/*gd-frame-event-identity*/";
        evaluate(frame, "const a=document.createElement('a');a.textContent='retired';document.body.append(a);a.click()", deadline);
        WAIT_FOR(held.size() == 1);
        evaluate(b.page.mainFrame(), "document.querySelector('iframe').remove()", deadline);
        delayed = held.takeFirst(); hold.clear(); delayed.callback(delayed.value);
        barrier(b.bridge, deadline);
        QCOMPARE(text.size(), 0);
        QCOMPARE(wordAt(b.frames, point, deadline), QString());
        // Reset abandons pending callbacks and replaces, rather than duplicates, the script.
        b.bridge.reset();
        QCOMPARE(b.page.scripts().find("gd-frames-1").size(), 0);
        QCOMPARE(b.page.scripts().find("gd-frames-2").size(), 1);
        QCOMPARE(b.page.scripts().count(), 3);
        b.load(origin.url("/dir/second.html"));
        WAIT_FOR(!readyAt(events, 1).isEmpty()); WAIT_FOR(b.visible(deadline));
        auto fresh = readyAt(events, 1);
        QCOMPARE(fresh.value("generation").toInt(), 2);
        frame = remote(b.page, "/dir/second.html", deadline);
        QCOMPARE(wordAt(b.frames, contentOrigin(b.page.mainFrame(), "iframe", deadline) + glyph(frame, "#word-target", deadline), deadline), QStringLiteral("wordword"));
    }

    void resetDuringCompletionKeepsNewestGeneration()
    {
        QDeadlineTimer deadline(10000);
        TestSupport::HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        Browser b;
        QSignalSpy events(&b.bridge, &ArticleBridge::frameEvent);
        b.load(origin.url("/dir/first.html"));
        WAIT_FOR(!readyAt(events, 0).isEmpty()); WAIT_FOR(b.visible(deadline));
        auto frame = remote(b.page, "/dir/first.html", deadline);
        auto point = contentOrigin(b.page.mainFrame(), "iframe", deadline) + glyph(frame, "#word-target", deadline);
        QCOMPARE(wordAt(b.frames, point, deadline), QStringLiteral("wordword"));
        hold = "/*gd-frame-word*/";
        struct Lookup { int callbacks = 0; QString answer; };
        auto lookup = std::make_shared<Lookup>();
        QPointer<ArticleBridge> bridge(&b.bridge);
        b.frames.wordAtPoint(point, [bridge, lookup](QString const &word) {
            lookup->answer = word; ++lookup->callbacks;
            if (bridge) bridge->reset();
        });
        WAIT_FOR(held.size() == 1);
        b.bridge.reset();
        QCOMPARE(lookup->callbacks, 1);
        QCOMPARE(lookup->answer, QString());
        QCOMPARE(b.bridge.generation(), 3);
        QCOMPARE(b.page.scripts().find("gd-frames-2").size(), 0);
        QCOMPARE(b.page.scripts().find("gd-frames-3").size(), 1);
        QCOMPARE(b.page.scripts().count(), 3);
        auto delayed = held.takeFirst(); hold.clear(); delayed.callback(delayed.value);
        b.load(origin.url("/dir/second.html"));
        WAIT_FOR(!readyAt(events, 1).isEmpty()); WAIT_FOR(b.visible(deadline));
        QCOMPARE(readyAt(events, 1).value("generation").toInt(), 3);
        QCOMPARE(lookup->callbacks, 1);
    }

    void framingDenial_data()
    {
        QTest::addColumn<QString>("route");
        QTest::newRow("X-Frame-Options") << QStringLiteral("/deny-frame");
        QTest::newRow("CSP-frame-ancestors") << QStringLiteral("/deny-csp");
    }
    void framingDenial()
    {
        QFETCH(QString, route);
        QDeadlineTimer deadline(15000);
        TestSupport::HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        Browser b;
        QSignalSpy unavailable(&b.frames, &WebsiteFrames::frameUnavailable);
        QElapsedTimer elapsed; elapsed.start();
        b.load(origin.url(route));
        WAIT_FOR(origin.requestCount(route) == 1);
        WAIT_FOR(unavailable.size() == 1);
        QVERIFY(elapsed.elapsed() >= 10000);
        QVERIFY(elapsed.elapsed() < 14000);
        QCOMPARE(unavailable.first()[0].toString(), QStringLiteral("dict-a"));
        QVERIFY(unavailable.first()[1].toString().contains("inline mode"));
        QVERIFY(evaluate(b.page.mainFrame(), "(()=>{const e=document.querySelector('.gd-website-frame-unavailable');"
            "return e&&e.getBoundingClientRect().height>0&&e.textContent.includes('inline mode')})()", deadline).toBool());
        QCOMPARE(evaluate(b.page.mainFrame(), "document.querySelector('iframe').src", deadline).toString(), origin.url(route).toString());
        int probes = discoveries; WAIT_FOR(discoveries >= probes + 8);
        QCOMPARE(unavailable.size(), 1);
    }

    void unstableMeasurementsStopAtTenSeconds()
    {
        QDeadlineTimer deadline(15000);
        TestSupport::HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        Browser b;
        QSignalSpy events(&b.bridge, &ArticleBridge::frameEvent);
        oscillate = true;
        b.load(origin.url("/dir/first.html"));
        WAIT_FOR(samples >= 35);
        int probes = discoveries;
        WAIT_FOR(observationClock.elapsed() >= sampleTimes.first() + 10500 && discoveries >= probes + 8);
        oscillate = false;
        int stopped = samples; probes = discoveries;
        WAIT_FOR(discoveries >= probes + 8);
        QCOMPARE(samples, stopped);
        QVERIFY(sampleTimes.last() - sampleTimes.first() >= 9500);
        QVERIFY(sampleTimes.last() - sampleTimes.first() < 10300);
        QVERIFY(stopped >= 38 && stopped <= 41);
    }
};
int main(int argc, char **argv)
{
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    registerArticleSchemes();
    QApplication app(argc, argv);
    WebsiteFramesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_website_frames.moc"
