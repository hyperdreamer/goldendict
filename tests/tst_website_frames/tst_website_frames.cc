/* Real remote origins and per-document reinstrumentation. */
#include <QtTest>
#include <QApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include "articlebridge.hh"
#include "articleschemes.hh"
#include "support/fixtures.hh"
#include "support/http_fixture.hh"

namespace {
#define WAIT_FOR(condition) \
    QVERIFY2(QTest::qWaitFor([&] { return bool(condition); }, int(qMax<qint64>(0, deadline.remainingTime()))), #condition)
QString quote(QString const &value)
{
    auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}
QVariant evaluate(QWebEngineFrame frame, QString const &script, QDeadlineTimer const &deadline)
{
    struct Answer { bool done = false; QVariant value; };
    auto result = std::make_shared<Answer>();
    frame.runJavaScript(script, [result](QVariant const &value) { result->value = value; result->done = true; });
    if (!QTest::qWaitFor([&] { return result->done; }, int(qMax<qint64>(0, deadline.remainingTime()))))
        QTest::qFail("real frame JavaScript deadline", __FILE__, __LINE__);
    return result->value;
}
QString document(QUrl const &url)
{
    return QStringLiteral("<!doctype html><meta charset='utf-8'><script>"
      "gdArticleContents=['dict-a'];gdCurrentArticle='gdfrom-dict-a';</script>"
      "<body style='margin:0'><div id='gdfrom-dict-a'><iframe id='gdexpandframe-dict-a' "
      "name='gdexpandframe-dict-a' style='width:600px;height:600px;border:5px solid;display:none' "
      "src='%1'></iframe></div></body>").arg(url.toString().toHtmlEscaped());
}
QVariantMap readyAt(QSignalSpy const &events, int index)
{
    for (auto const &args : events)
    {
        auto event = args[0].toMap();
        if (event.value("gdFrameEvent").toString() == "ready" && --index < 0) return event;
    }
    return {};
}
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
    void cleanupTestCase() { TestSupport::clearWatchdog(); }
    void firstAndSecondNavigation()
    {
        QDeadlineTimer deadline(12000);
        TestSupport::HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        QWebEngineProfile profile;
        QWebEnginePage page(&profile);
        QWebEngineView view;
        view.resize(1000, 900); view.setPage(&page); view.show();
        QObject owner;
        ArticleBridge bridge(&page, &owner);
        QSignalSpy events(&bridge, &ArticleBridge::frameEvent);
        page.setHtml(document(origin.url("/dir/first.html")));
        WAIT_FOR(origin.requestCount("/dir/data.json") == 1);
        WAIT_FOR(page.mainFrame().children().size() == 1);
        auto frame = page.mainFrame().children().first();
        QCOMPARE(frame.url(), origin.url("/dir/first.html"));
        QCOMPARE(evaluate(frame, "location.origin", deadline).toString(), origin.url("/").toString(QUrl::RemovePath));
        QCOMPARE(evaluate(frame, "document.documentElement.dataset.originFetch", deadline).toString(), QStringLiteral("real-origin-fetch"));
        WAIT_FOR(!readyAt(events, 0).isEmpty());
        auto firstReady = readyAt(events, 0);
        evaluate(page.mainFrame(), "document.querySelector('iframe').src=" + quote(origin.url("/dir/second.html").toString()), deadline);
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
