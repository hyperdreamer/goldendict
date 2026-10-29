/* Frozen PDF snapshots through the production renderer and Qt PDF decoder. */
#include <QtTest>
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QNetworkProxy>
#include <QPdfDocument>
#include <QPrinter>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "article_netmgr.hh"
#include "articlebridge.hh"
#include "articleprofile.hh"
#include "articleschemes.hh"
#include "articlewebview.hh"
#include "printpipeline.hh"
#include "websiteframes.hh"
#include "support/dictionary_stub.hh"
#include "support/fixtures.hh"
#include "support/http_fixture.hh"

namespace {
constexpr int BudgetMs = 15000;
QPageLayout layout()
{
  return QPageLayout( QPageSize( QPageSize::A4 ), QPageLayout::Portrait, QMarginsF() );
}
struct Browser
{
  Config::Class config;
  sptr<TestSupport::FixtureDictionary> dictionary{new TestSupport::FixtureDictionary};
  std::vector<sptr<Dictionary::Class>> dictionaries{dictionary};
  Instances::Groups groups;
  ArticleMaker maker{dictionaries, groups, {}, {}};
  ArticleNetworkAccessManager resources{nullptr, dictionaries, maker,
    config.preferences.disallowContentFromOtherSites, config.preferences.hideGoldenDictHeader};
  std::unique_ptr<ArticleProfileController> controller;
  TestSupport::HttpFixture http;
  ArticleWebView renderer;
  ArticleBridge * bridge = nullptr;
  std::unique_ptr<WebsiteFrames> frames;
  Browser()
  {
    config.preferences.maxNetworkCacheSize = 0;
    config.preferences.disallowContentFromOtherSites = false;
    controller = std::make_unique<ArticleProfileController>( config, resources, nullptr );
    renderer.configure( controller->profile(), &config );
    bridge = renderer.findChild<ArticleBridge *>();
    if( bridge ) frames = std::make_unique<WebsiteFrames>( renderer.page(), bridge );
    renderer.resize( 800, 600 );
    renderer.show();
  }
  bool load()
  {
    if( !bridge || !http.start( QHostAddress( "127.0.0.1" ) ) ) return false;
    QSignalSpy loaded( &renderer, &QWebEngineView::loadFinished );
    renderer.load( http.url( "/print/two-pages.html" ) );
    return QTest::qWaitFor( [&] { return !loaded.isEmpty(); }, BudgetMs ) && loaded.first().first().toBool();
  }
};
}

class PrintPipelineTest final : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase()
  {
    TestSupport::installWatchdog( TestSupport::BrowserWatchdogSeconds );
    qputenv( "GD_TEST_REQUIRED_PREREQUISITES", "display:webengine" );
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
  }
  void frozenSnapshotPaint()
  {
    Browser b;
    QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QSignalSpy ready( &pipeline, &PrintPipeline::ready );
    QSignalSpy failed( &pipeline, &PrintPipeline::failed );
    pipeline.snapshot( layout() );
    QVERIFY( QTest::qWaitFor( [&] { return ready.count() + failed.count() > 0; }, BudgetMs ) );
    QCOMPARE( failed.count(), 0 );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Ready );
    QCOMPARE( ready.first().first().toInt(), 2 );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QPrinter printer;
    printer.setResolution( 96 );
    printer.setOutputFormat( QPrinter::PdfFormat );
    printer.setOutputFileName( output.filePath( "painted.pdf" ) );
    QVERIFY( pipeline.paint( &printer ) );
    QVERIFY( QFileInfo( printer.outputFileName() ).size() > 0 );
    QPdfDocument painted;
    QCOMPARE( painted.load( printer.outputFileName() ), QPdfDocument::Error::None );
    QCOMPARE( painted.pageCount(), 2 );
  }
};

int main( int argc, char ** argv )
{
  QNetworkProxy::setApplicationProxy( QNetworkProxy::NoProxy );
  registerArticleSchemes();
  QApplication app( argc, argv );
  PrintPipelineTest test;
  return QTest::qExec( &test, argc, argv );
}
#include "tst_print_pipeline.moc"
