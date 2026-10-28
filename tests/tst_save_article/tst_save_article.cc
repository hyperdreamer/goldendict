/* Atomic save publication through the real ArticleView renderer. */
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWebEnginePage>
#include <QWebEngineProfile>

#include <memory>

#include "articleprofile.hh"
#include "articleview.hh"
#include "savearticle.hh"
#include "support/dictionary_stub.hh"
#include "support/fixtures.hh"

namespace
{
constexpr int StepBudgetMs = 15000;

QByteArray onePixelPng()
{
  return QByteArray::fromBase64( QByteArrayLiteral(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=" ) );
}

struct Fixture
{
  struct Player final : AudioPlayerInterface
  {
    QByteArray played;
    QString play( char const * bytes, int size ) override
    { played = QByteArray( bytes, size ); return {}; }
    void stop() override {}
  };

  Config::Class config;
  sptr< TestSupport::FixtureDictionary > a{ new TestSupport::FixtureDictionary };
  sptr< TestSupport::FixtureDictionary > b{
    new TestSupport::FixtureDictionary( "dict-b", QStringLiteral( "Fixture Dictionary B" ) ) };
  std::vector< sptr< Dictionary::Class > > dictionaries{ a, b };
  Instances::Groups groups;
  ArticleMaker maker{ dictionaries, groups, {}, {} };
  ArticleNetworkAccessManager resources{ nullptr, dictionaries, maker,
    config.preferences.disallowContentFromOtherSites, config.preferences.hideGoldenDictHeader };
  std::unique_ptr< ArticleProfileController > controller;
  AudioPlayerPtr player{ new Player };
  QAction search;

  Fixture()
  {
    config.preferences.maxNetworkCacheSize = 0;
    config.preferences.disallowContentFromOtherSites = false;
    Instances::Group group( "Fixture group" );
    group.id = 17;
    group.name = "Fixture group";
    group.dictionaries = dictionaries;
    groups.push_back( group );
    controller = std::make_unique< ArticleProfileController >( config, resources, nullptr );
    a->setArticleBody( QByteArrayLiteral(
      "<p id='save-headword'>save-headword</p>"
      "<img id='saved-image' src='bres://dict-a/image.png'>" ) );
    a->setResources( { { QStringLiteral( "image.png" ), onePixelPng() } } );
  }

  std::unique_ptr< ArticleView > view()
  {
    auto result = std::make_unique< ArticleView >( nullptr, *controller->profile(), resources, player,
      dictionaries, groups, false, config, search, nullptr, nullptr );
    result->resize( 800, 600 );
    result->show();
    return result;
  }
};

bool loadFixture( ArticleView & view )
{
  QSignalSpy loaded( &view, &ArticleView::pageLoaded );
  view.showDefinition( QStringLiteral( "save-headword" ), 17 );
  if( !QTest::qWaitFor( [&] { return !loaded.isEmpty(); }, StepBudgetMs ) ) return false;

  auto * page = view.findChild< QWebEnginePage * >();
  if( !page ) return false;
  bool scriptFinished = false;
  page->runJavaScript( QStringLiteral(
    "document.querySelectorAll('.gddicticon').forEach(function (node) { node.remove(); });" ),
    [&]( QVariant const & ) { scriptFinished = true; } );
  return QTest::qWaitFor( [&] { return scriptFinished; }, StepBudgetMs );
}

bool waitForTerminal( SaveArticle & save, QSignalSpy const & saved, QSignalSpy const & failed,
                      QSignalSpy const & cancelled )
{
  Q_UNUSED( save );
  return QTest::qWaitFor( [&] {
    return saved.count() + failed.count() + cancelled.count() != 0;
  }, StepBudgetMs );
}

QStringList assetDirectories( QTemporaryDir const & output )
{
  return QDir( output.path() ).entryList( { QStringLiteral( "*_files-*" ) }, QDir::Dirs );
}

QByteArray readFile( QString const & path )
{
  QFile file( path );
  return file.open( QIODevice::ReadOnly ) ? file.readAll() : QByteArray();
}
} // namespace

class SaveArticleTest final : public QObject
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

  void cleanupTestCase()
  {
    TestSupport::clearWatchdog();
  }

  void collectorPreservesCanonicalOccurrencesAndUtf16Spans()
  {
    QString const html = QStringLiteral(
      "<img src=\"bres://d/a.png?q=1\"><img src=\"bres://d/a.png?q=2\">"
      "<img src=\"bres://d/a.png?q=1\">" );
    QVector< SaveArticleDetail::ResourceSpan > const spans =
      SaveArticleDetail::collectResourceSpans( html, QUrl( "gdlookup://localhost/A?group=1" ) );

    QCOMPARE( spans.size(), 3 );
    QCOMPARE( spans.at( 0 ).url, QUrl( "bres://d/a.png?q=1" ) );
    QCOMPARE( spans.at( 1 ).url, QUrl( "bres://d/a.png?q=2" ) );
    QCOMPARE( spans.at( 2 ).url, spans.at( 0 ).url );
    QCOMPARE( html.mid( spans.at( 0 ).offset, spans.at( 0 ).length ),
              QStringLiteral( "bres://d/a.png?q=1" ) );
  }

  void completeSavePublishesAssetsBeforeCommittedHtml()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY2( loadFixture( *view ), "the fixture article did not load" );

    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const destination = output.filePath( QStringLiteral( "article.html" ) );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved );
    QSignalSpy failed( &save, &SaveArticle::failed );
    QSignalSpy cancelled( &save, &SaveArticle::cancelled );

    save.start( destination, true );
    QVERIFY2( waitForTerminal( save, saved, failed, cancelled ), "save did not reach a terminal signal" );
    QCOMPARE( saved.count(), 1 );
    QCOMPARE( failed.count(), 0 );
    QCOMPARE( cancelled.count(), 0 );
    QCOMPARE( save.state(), SaveArticle::State::Complete );
    QVERIFY( QFileInfo::exists( destination ) );

    QStringList const assets = assetDirectories( output );
    QCOMPARE( assets.size(), 1 );
    QDirIterator files( output.filePath( assets.first() ), QDir::Files, QDirIterator::Subdirectories );
    QVERIFY2( files.hasNext(), "complete save did not publish a resource file" );
    QString const html = QString::fromUtf8( readFile( destination ) );
    QVERIFY( html.contains( assets.first() ) );
    QVERIFY( !html.contains( QStringLiteral( "bres://" ) ) );

    QWebEnginePage savedPage( fixture.controller->profile() );
    QSignalSpy loaded( &savedPage, &QWebEnginePage::loadFinished );
    savedPage.load( QUrl::fromLocalFile( destination ) );
    QVERIFY2( QTest::qWaitFor( [&] { return !loaded.isEmpty(); }, StepBudgetMs ),
              "the saved HTML did not load in a real page" );
    QVERIFY( loaded.constFirst().at( 0 ).toBool() );
    bool widthReady = false;
    int naturalWidth = 0;
    savedPage.runJavaScript( QStringLiteral( "document.getElementById('saved-image').naturalWidth" ),
      [&]( QVariant const & value ) { naturalWidth = value.toInt(); widthReady = true; } );
    QVERIFY2( QTest::qWaitFor( [&] { return widthReady; }, StepBudgetMs ),
              "the saved page did not report the image width" );
    QCOMPARE( naturalWidth, 1 );
  }

  void htmlOnlyDoesNotCreateAssetDirectory()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY2( loadFixture( *view ), "the fixture article did not load" );

    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const destination = output.filePath( QStringLiteral( "article.html" ) );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved );
    QSignalSpy failed( &save, &SaveArticle::failed );
    QSignalSpy cancelled( &save, &SaveArticle::cancelled );

    save.start( destination, false );
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( save.state(), SaveArticle::State::Complete );
    QCOMPARE( saved.count(), 1 );
    QVERIFY( QFileInfo::exists( destination ) );
    QVERIFY( assetDirectories( output ).isEmpty() );
  }

  void cancellationPreservesOnlyPreexistingOutputs()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY2( loadFixture( *view ), "the fixture article did not load" );
    fixture.a->setResourcePending( true );

    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const destination = output.filePath( QStringLiteral( "article.html" ) );
    QByteArray const oldHtml = QByteArrayLiteral( "old complete html" );
    QFile oldFile( destination );
    QVERIFY( oldFile.open( QIODevice::WriteOnly ) );
    QVERIFY( oldFile.write( oldHtml ) == oldHtml.size() );
    oldFile.close();
    QDir oldAssets( output.filePath( QStringLiteral( "article_files-preexisting" ) ) );
    QVERIFY( oldAssets.mkpath( QStringLiteral( "dict-a" ) ) );
    QFile oldAsset( oldAssets.filePath( QStringLiteral( "dict-a/old.bin" ) ) );
    QVERIFY( oldAsset.open( QIODevice::WriteOnly ) );
    QVERIFY( oldAsset.write( "old asset" ) == 9 );
    oldAsset.close();

    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved );
    QSignalSpy failed( &save, &SaveArticle::failed );
    QSignalSpy cancelled( &save, &SaveArticle::cancelled );
    save.start( destination, true );

    QVERIFY2( QTest::qWaitFor( [&] { return bool( fixture.a->lastResourceRequest() ); }, StepBudgetMs ),
              "save did not create the pending resource request" );
    auto request = fixture.a->lastResourceRequest();
    save.cancel();
    QCOMPARE( save.state(), SaveArticle::State::Cancelled );
    QCOMPARE( cancelled.count(), 1 );
    QCOMPARE( saved.count(), 0 );
    QVERIFY( request->cancelCount() > 0 );
    QCOMPARE( readFile( destination ), oldHtml );
    QVERIFY( QFileInfo::exists( oldAsset.fileName() ) );
    QVERIFY( assetDirectories( output ).contains( QStringLiteral( "article_files-preexisting" ) ) );
    QVERIFY( failed.isEmpty() );
  }

  void failedRequiredResourcePreservesDestination()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY2( loadFixture( *view ), "the fixture article did not load" );
    fixture.a->setResources( {} );

    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const destination = output.filePath( QStringLiteral( "article.html" ) );
    QByteArray const oldHtml = QByteArrayLiteral( "keep this html" );
    QFile oldFile( destination );
    QVERIFY( oldFile.open( QIODevice::WriteOnly ) );
    QVERIFY( oldFile.write( oldHtml ) == oldHtml.size() );
    oldFile.close();

    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved );
    QSignalSpy failed( &save, &SaveArticle::failed );
    QSignalSpy cancelled( &save, &SaveArticle::cancelled );
    save.start( destination, true );

    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( save.state(), SaveArticle::State::Failed );
    QCOMPARE( saved.count(), 0 );
    QCOMPARE( failed.count(), 1 );
    QCOMPARE( cancelled.count(), 0 );
    QCOMPARE( readFile( destination ), oldHtml );
    QVERIFY( assetDirectories( output ).isEmpty() );
  }
};

int main( int argc, char ** argv )
{
  if( TestSupport::isWatchdogProbeProcess() )
    TestSupport::runWatchdogProbe();

  QTemporaryDir sandbox;
  if( !sandbox.isValid() ) return 1;
  qputenv( "XDG_CACHE_HOME", sandbox.path().toUtf8() );
  qputenv( "XDG_DATA_HOME", sandbox.path().toUtf8() );

  QApplication app( argc, argv );
  QCoreApplication::setApplicationName( QStringLiteral( "GoldenDict" ) );
  QNetworkProxy::setApplicationProxy( QNetworkProxy::NoProxy );

  SaveArticleTest tests;
  return QTest::qExec( &tests, argc, argv );
}

#include "tst_save_article.moc"
