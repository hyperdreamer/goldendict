#include <QtTest>
#include <QApplication>
#include <QNetworkProxy>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include "articleschemes.hh"
#include "support/fixtures.hh"
#include "support/http_fixture.hh"

class ExternalContent : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase()
  {
    QStringList reasons;
    QVERIFY2(TestSupport::requiredEnvironment(&reasons), qPrintable(reasons.join('\n')));
    TestSupport::installWatchdog(TestSupport::BrowserWatchdogSeconds);
  }
  void cleanupTestCase() { TestSupport::clearWatchdog(); }
  void absentInitiatorCannotUseDictionaryHosts()
  {
    TestSupport::HttpFixture origin;
    QVERIFY2(origin.start(QHostAddress("127.0.0.1")), qPrintable(origin.errorString()));
    QWebEngineProfile profile;
    QWebEnginePage page(&profile);
    ArticleRequestInterceptor interceptor(&page);
    auto policy = std::make_shared<ArticleRequestPolicy>();
    policy->blockExternal = true;
    policy->dictionaryHosts.insert("127.0.0.1");
    interceptor.setPolicy(policy);
    page.setUrlRequestInterceptor(&interceptor);
    QSignalSpy finished(&page, &QWebEnginePage::loadFinished);
    page.setHtml(QString("<img src='%1'>").arg(origin.url("/pixel.png").toString()));
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
    QVERIFY(finished.constLast().at(0).toBool());
    QCOMPARE(origin.requestCount("/pixel.png"), 0);
  }
};

int main(int argc, char **argv)
{
  registerArticleSchemes();
  QApplication app(argc, argv);
  QCoreApplication::setApplicationName("GoldenDict");
  QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
  ExternalContent tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "tst_external_content.moc"
