/* Atomic save publication through the real ArticleView renderer. */
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWebEnginePage>
#include <QWebEngineProfile>

#include <memory>

#include "articleprofile.hh"
#include "articleschemes.hh"
#include "articleview.hh"
#include "savearticle.hh"
#include "support/dictionary_stub.hh"
#include "support/fixtures.hh"
#include "support/http_fixture.hh"

namespace
{
constexpr int StepBudgetMs = 15000;

struct Snapshot
{
  std::function<void( QString const & )> callback;
  QString html;
};
bool holdSnapshots = false;
QList< Snapshot > snapshots;
QString observedDestination;
QStringList publicationEvents;
QString publishedAssets;
bool fallbackDisabled = false;
bool failAssetRename = false;
bool failHtmlCommit = false;
bool assetsPresentAtCommit = false;
QByteArray htmlBeforeCommit;

QVariant evaluate( ArticleView & view, QString const & script )
{
  struct Answer { bool done = false; QVariant value; };
  auto answer = std::make_shared< Answer >();
  auto * page = view.findChild< QWebEnginePage * >();
  if( !page ) return {};
  page->runJavaScript( script, [answer]( QVariant const & value ) {
    answer->value = value; answer->done = true;
  } );
  if( !QTest::qWaitFor( [answer] { return answer->done; }, StepBudgetMs ) )
    QTest::qFail( "the live page did not answer", __FILE__, __LINE__ );
  return answer->value;
}

bool writeFile( QString const & path, QByteArray const & bytes )
{
  QFile file( path );
  return file.open( QIODevice::WriteOnly ) && file.write( bytes ) == bytes.size();
}

QStringList outputEntries( QTemporaryDir const & output )
{
  return QDir( output.path() ).entryList( QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name );
}

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
      "<img id='saved-image' src='bres://dict-a/image.png'>"
      "<link rel='stylesheet' href='bres://dict-a/style.css'>" ) );
    a->setResources( { { QStringLiteral( "image.png" ), onePixelPng() },
                      { QStringLiteral( "style.css" ), QByteArrayLiteral( "#save-headword{color:rgb(12,34,56)}" ) } } );
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

  // Keep this fixture independent of shipped CSS's external Wikimedia image.
  // Its own image and stylesheet still pass through the real save collector.
  evaluate( view, QStringLiteral(
    "document.querySelectorAll('head style,.gddicticon,script').forEach(node=>node.remove());"
    "document.body.removeAttribute('onload'); true" ) );
  return true;
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

extern "C" bool realRename( QDir *, QString const &, QString const & )
  asm( "__real__ZN4QDir6renameERK7QStringS2_" );
extern "C" bool wrapRename( QDir *, QString const &, QString const & )
  asm( "__wrap__ZN4QDir6renameERK7QStringS2_" );
extern "C" bool wrapRename( QDir * dir, QString const & from, QString const & to )
{
  bool const watched = !observedDestination.isEmpty() && from.endsWith( QStringLiteral( "/assets" ) );
  if( watched && failAssetRename )
  {
    // A real destination collision: Qt must refuse to overwrite this directory.
    QDir().mkpath( to );
    writeFile( QDir( to ).filePath( QStringLiteral( "foreign" ) ), "unowned" );
  }
  bool const result = realRename( dir, from, to );
  if( watched )
  {
    publishedAssets = to;
    publicationEvents.append( result ? QStringLiteral( "assets" ) : QStringLiteral( "rename-failed" ) );
  }
  return result;
}

extern "C" bool realOpen( QSaveFile *, QIODevice::OpenMode )
  asm( "__real__ZN9QSaveFile4openE6QFlagsIN13QIODeviceBase12OpenModeFlagEE" );
extern "C" bool wrapOpen( QSaveFile *, QIODevice::OpenMode )
  asm( "__wrap__ZN9QSaveFile4openE6QFlagsIN13QIODeviceBase12OpenModeFlagEE" );
extern "C" bool wrapOpen( QSaveFile * file, QIODevice::OpenMode mode )
{
  if( file->fileName() == observedDestination ) fallbackDisabled = !file->directWriteFallback();
  return realOpen( file, mode );
}

extern "C" bool realCommit( QSaveFile * ) asm( "__real__ZN9QSaveFile6commitEv" );
extern "C" bool wrapCommit( QSaveFile * ) asm( "__wrap__ZN9QSaveFile6commitEv" );
extern "C" bool wrapCommit( QSaveFile * file )
{
  bool const watched = file->fileName() == observedDestination;
  QString const parent = QFileInfo( file->fileName() ).absolutePath();
  QFile::Permissions const permissions = QFileInfo( parent ).permissions();
  if( watched )
  {
    assetsPresentAtCommit = QFileInfo( publishedAssets ).isDir();
    htmlBeforeCommit = readFile( observedDestination );
    publicationEvents.append( QStringLiteral( "html" ) );
    if( failHtmlCommit ) QFile::setPermissions( parent, QFile::ReadOwner | QFile::ExeOwner );
  }
  bool const result = realCommit( file );
  if( watched && failHtmlCommit ) QFile::setPermissions( parent, permissions );
  return result;
}

using HtmlResult = std::function<void( QString const & )>;
extern "C" void realHtml( QWebEnginePage const *, HtmlResult const & )
  asm( "__real__ZNK14QWebEnginePage6toHtmlERKSt8functionIFvRK7QStringEE" );
extern "C" void wrapHtml( QWebEnginePage const *, HtmlResult const & )
  asm( "__wrap__ZNK14QWebEnginePage6toHtmlERKSt8functionIFvRK7QStringEE" );
extern "C" void wrapHtml( QWebEnginePage const * page, HtmlResult const & callback )
{
  if( holdSnapshots )
    realHtml( page, [callback]( QString const & html ) { snapshots.append( { callback, html } ); } );
  else
    realHtml( page, callback );
}

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

  void cleanup()
  {
    holdSnapshots = false;
    snapshots.clear();
    observedDestination.clear();
    publicationEvents.clear();
    publishedAssets.clear();
    fallbackDisabled = failAssetRename = failHtmlCommit = assetsPresentAtCommit = false;
    htmlBeforeCommit.clear();
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

  void collectorResolvesEntitiesCssAndLegacyFamilies()
  {
    QString const html = QString::fromUtf8(
      "\xf0\x9f\x8c\x8d<!-- <img src='bres://ignored/comment.png'> -->"
      "<img data-src='https://ignored/lazy.png' src='bres://d/a.png?q=1&amp;x=2'>"
      "<style>/*url('bres://ignored/comment.css')*/body{background:url('https://a.test/bg.png')}</style>"
      "<div style=\"background:url('bres://d/inline.png')\"></div>"
      "<script>let markup=\"<img src='https://ignored/text.png'>\";let css=\"url('https://ignored/text.css')\";"
      "let audio='gdau://d/a.wav';</script>"
      "<a href='gdvideo://d/movie.webm'>video</a><img src='gico://d/icon.png'>"
      "<img src='qrcx://localhost/icons/blank.png'><a href='https://ignored/navigation'>link</a>" );
    auto const spans = SaveArticleDetail::collectResourceSpans( html, QUrl( "https://a.test/article" ) );
    QCOMPARE( spans.size(), 7 );
    QCOMPARE( spans.at( 0 ).url, QUrl( "bres://d/a.png?q=1&x=2" ) );
    QCOMPARE( html.mid( spans.at( 0 ).offset, spans.at( 0 ).length ), QString( "bres://d/a.png?q=1&amp;x=2" ) );
    QCOMPARE( spans.at( 1 ).url, QUrl( "https://a.test/bg.png" ) );
    QCOMPARE( spans.at( 2 ).url, QUrl( "bres://d/inline.png" ) );
    QCOMPARE( spans.at( 3 ).url, QUrl( "gdau://d/a.wav" ) );
    QCOMPARE( spans.at( 4 ).url, QUrl( "gdvideo://d/movie.webm" ) );
    QCOMPARE( spans.at( 5 ).url, QUrl( "gico://d/icon.png" ) );
    QCOMPARE( spans.at( 6 ).url, QUrl( "qrcx://localhost/icons/blank.png" ) );
    for( auto const & span : spans )
    {
      QVERIFY( span.offset >= 0 && span.offset + span.length <= html.size() );
      QVERIFY( html.mid( span.offset, span.length ).contains( span.url.fileName() ) );
    }
  }

  void canonicalDownloadsKeepQueriesFragmentsAndDistinctPaths()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    fixture.a->setResources( { { "image.png?q=1", "query-one" }, { "image.png?q=2", "query-two" },
                              { "image.png_q1", "literal-name" } } );
    evaluate( *view, QStringLiteral(
      "document.body.innerHTML='<img src=\"bres://dict-a/%69mage.png?q=1#first\">"
      "<img src=\"bres://dict-a/image.png?q=2\"><img src=\"bres://dict-a/image.png?q=1#second\">"
      "<img src=\"bres://dict-a/image.png_q1\">';true" ) );
    QVERIFY( QTest::qWaitFor( [&] {
      return evaluate( *view, "Array.from(document.images).every(image=>image.complete)" ).toBool();
    }, StepBudgetMs ) );
    fixture.a->clearObservations();
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const destination = output.filePath( "article.html" );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved ), failed( &save, &SaveArticle::failed ),
               cancelled( &save, &SaveArticle::cancelled );
    save.start( destination, true );
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( saved.size(), 1 );
    QCOMPARE( fixture.a->requestedResources().size(), 3 );
    QCOMPARE( fixture.a->requestedResources().count( "image.png?q=1" ), 1 );
    QStringList const assets = assetDirectories( output );
    QCOMPARE( assets.size(), 1 );
    QString const root = output.filePath( assets.first() + "/dict-a/" );
    QCOMPARE( readFile( root + "image.png_q1" ), QByteArray( "query-one" ) );
    QCOMPARE( readFile( root + "image.png_q2" ), QByteArray( "query-two" ) );
    QCOMPARE( readFile( root + "image.png_q1_r1" ), QByteArray( "literal-name" ) );
    QByteArray const html = readFile( destination );
    QVERIFY( html.contains( "image.png_q1#first" ) );
    QVERIFY( html.contains( "image.png_q1#second" ) );
    QVERIFY( !html.contains( "bres://" ) );
  }

  void traversalNeverEscapesStaging()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    fixture.a->setResources( { { "../../escape.bin", "must never be written" } } );
    evaluate( *view, "document.body.innerHTML=\"<img src='bres://dict-a/%2e%2e/%2e%2e/escape.bin'>\"" );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const destination = output.filePath( "article.html" );
    QVERIFY( writeFile( destination, "keep html" ) );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved ), failed( &save, &SaveArticle::failed ),
               cancelled( &save, &SaveArticle::cancelled );
    save.start( destination, true );
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( save.state(), SaveArticle::State::Failed );
    QCOMPARE( failed.size(), 1 );
    QVERIFY( failed.first().first().toString().contains( "escapes" ) );
    QCOMPARE( readFile( destination ), QByteArray( "keep html" ) );
    QCOMPARE( outputEntries( output ), QStringList{ "article.html" } );
  }

  void completeSavePublishesAssetsBeforeCommittedHtml()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY2( loadFixture( *view ), "the fixture article did not load" );

    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const destination = output.filePath( QStringLiteral( "article.html" ) );
    auto controller = std::make_unique< SaveArticle >( view.get(), fixture.resources );
    SaveArticle & save = *controller;
    QSignalSpy saved( &save, &SaveArticle::saved );
    QSignalSpy failed( &save, &SaveArticle::failed );
    QSignalSpy cancelled( &save, &SaveArticle::cancelled );

    observedDestination = destination;
    QVERIFY( writeFile( destination, "previous complete html" ) );
    save.start( destination, true );
    QVERIFY2( waitForTerminal( save, saved, failed, cancelled ), "save did not reach a terminal signal" );
    QCOMPARE( saved.count(), 1 );
    QCOMPARE( failed.count(), 0 );
    QCOMPARE( cancelled.count(), 0 );
    QCOMPARE( save.state(), SaveArticle::State::Complete );
    QCOMPARE( publicationEvents, ( QStringList{ "assets", "html" } ) );
    QVERIFY( assetsPresentAtCommit );
    QVERIFY( fallbackDisabled );
    QCOMPARE( htmlBeforeCommit, QByteArray( "previous complete html" ) );
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
    auto color = std::make_shared< QString >();
    savedPage.runJavaScript( QStringLiteral( "getComputedStyle(document.getElementById('save-headword')).color" ),
      [color]( QVariant const & value ) { *color = value.toString(); } );
    QVERIFY( QTest::qWaitFor( [color] { return !color->isEmpty(); }, StepBudgetMs ) );
    QCOMPARE( *color, QStringLiteral( "rgb(12, 34, 56)" ) );
    save.cancel();
    QCOMPARE( save.state(), SaveArticle::State::Complete );
    QCOMPARE( saved.count(), 1 );
    QVERIFY( QFileInfo( output.filePath( assets.first() ) ).isDir() );
    controller.reset();
    QVERIFY( QFileInfo( output.filePath( assets.first() ) ).isDir() );
    QVERIFY( QFileInfo::exists( destination ) );
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

  void cancellationPreservesOnlyPreexistingOutputs_data()
  {
    QTest::addColumn< QString >( "action" );
    QTest::newRow( "explicit" ) << QStringLiteral( "cancel" );
    QTest::newRow( "navigation" ) << QStringLiteral( "navigate" );
    QTest::newRow( "tab-close" ) << QStringLiteral( "close" );
  }

  void cancellationPreservesOnlyPreexistingOutputs()
  {
    QFETCH( QString, action );
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY2( loadFixture( *view ), "the fixture article did not load" );
    evaluate( *view, "document.querySelectorAll('link').forEach(node=>node.remove());"
                     "document.body.insertAdjacentHTML('beforeend',\"<img src='bres://dict-b/second.png'>\")" );
    QVERIFY( QTest::qWaitFor( [&] {
      return evaluate( *view, "Array.from(document.images).every(image=>image.complete)" ).toBool();
    }, StepBudgetMs ) );
    fixture.a->clearObservations(); fixture.b->clearObservations();
    fixture.a->setResourcePending( true ); fixture.b->setResourcePending( true );

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

    QVERIFY2( QTest::qWaitFor( [&] {
      return fixture.a->lastResourceRequest() && fixture.b->lastResourceRequest();
    }, StepBudgetMs ), "save did not create both pending resource requests" );
    auto request = fixture.a->lastResourceRequest();
    auto second = fixture.b->lastResourceRequest();
    if( action == QLatin1String( "navigate" ) ) view->showDefinition( "next", 17 );
    else if( action == QLatin1String( "close" ) ) view.reset();
    else save.cancel();
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( save.state(), SaveArticle::State::Cancelled );
    QCOMPARE( cancelled.count(), 1 );
    QCOMPARE( saved.count(), 0 );
    QVERIFY( request->cancelCount() > 0 );
    QVERIFY( second->cancelCount() > 0 );
    QCOMPARE( readFile( destination ), oldHtml );
    QVERIFY( QFileInfo::exists( oldAsset.fileName() ) );
    QVERIFY( assetDirectories( output ).contains( QStringLiteral( "article_files-preexisting" ) ) );
    QVERIFY( failed.isEmpty() );
    QCOMPARE( readFile( oldAsset.fileName() ), QByteArray( "old asset" ) );
    QCOMPARE( outputEntries( output ), ( QStringList{ "article.html", "article_files-preexisting" } ) );
  }

  void reusedControllerCancelsOnViewClose()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved );
    QSignalSpy failed( &save, &SaveArticle::failed );
    QSignalSpy cancelled( &save, &SaveArticle::cancelled );
    save.start( output.filePath( "first.html" ), false );
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( saved.size(), 1 );
    fixture.a->clearObservations();
    fixture.a->setResourcePending( true );
    save.start( output.filePath( "second.html" ), true );
    QVERIFY( QTest::qWaitFor( [&] { return bool( fixture.a->lastResourceRequest() ); }, StepBudgetMs ) );
    auto request = fixture.a->lastResourceRequest();
    view.reset();
    QCOMPARE( save.state(), SaveArticle::State::Cancelled );
    QCOMPARE( cancelled.size(), 1 );
    QVERIFY( failed.isEmpty() );
    QVERIFY( request->cancelCount() > 0 );
    QCOMPARE( outputEntries( output ), QStringList{ "first.html" } );
  }

  void cancelledOrDestroyedCaptureCannotPublish()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    holdSnapshots = true;
    struct Slot
    {
      alignas( SaveArticle ) unsigned char storage[ sizeof( SaveArticle ) ];
      SaveArticle * save = nullptr;
      ~Slot() { if( save ) save->~SaveArticle(); }
      void construct( ArticleView * view, ArticleNetworkAccessManager & resources )
      {
        if( save ) save->~SaveArticle();
        save = new ( storage ) SaveArticle( view, resources );
      }
    } slot;
    slot.construct( view.get(), fixture.resources );
    slot.save->start( output.filePath( "old.html" ), true );
    QVERIFY( QTest::qWaitFor( [] { return snapshots.size() == 1; }, StepBudgetMs ) );
    auto snapshot = snapshots.takeFirst();
    // Reuse the exact controller address with a new operation counter. A raw
    // this/token continuation would mistake this different owner for its own.
    slot.construct( view.get(), fixture.resources );
    slot.save->start( output.filePath( "new.html" ), false );
    QVERIFY( QTest::qWaitFor( [] { return snapshots.size() == 1; }, StepBudgetMs ) );
    snapshot.callback( snapshot.html );
    QCOMPARE( slot.save->state(), SaveArticle::State::Capturing );
    QVERIFY( outputEntries( output ).isEmpty() );
    auto fresh = snapshots.takeFirst();
    fresh.callback( fresh.html );
    QVERIFY( QTest::qWaitFor( [&] { return slot.save->state() == SaveArticle::State::Complete; }, StepBudgetMs ) );
    QCOMPARE( outputEntries( output ), QStringList{ "new.html" } );
  }

  void twoSavesToOneTargetDoNotDamageEachOther()
  {
    // Two independent SaveArticle instances writing the same HTML destination, both
    // *provably* past staging at the same time: each waits for its own distinct
    // resource request, both report Downloading, and two staging trees exist before
    // either is cancelled. A survivor that loses its staged assets to the peer's
    // cleanup cannot publish, so this row fails if cleanup is not operation-bounded.
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY2( loadFixture( *view ), "the fixture article did not load" );
    evaluate( *view, "document.querySelectorAll('link').forEach(node=>node.remove());"
                     "document.body.insertAdjacentHTML('beforeend',\"<img src='bres://dict-b/second.png'>\")" );
    QVERIFY( QTest::qWaitFor( [&] {
      return evaluate( *view, "Array.from(document.images).every(image=>image.complete)" ).toBool();
    }, StepBudgetMs ) );
    fixture.a->clearObservations(); fixture.b->clearObservations();
    fixture.a->setResourcePending( true ); fixture.b->setResourcePending( true );

    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const target = output.filePath( "shared.html" );

    auto stagingPaths = [ &output ]() {
      QStringList paths;
      for( QString const & name : QDir( output.path() )
                                    .entryList( QStringList{ QStringLiteral( ".*goldendict-save-*" ) },
                                                QDir::Dirs | QDir::Hidden ) )
        paths.append( output.filePath( name ) );
      return paths;
    };

    SaveArticle first( view.get(), fixture.resources );
    QSignalSpy firstSaved( &first, &SaveArticle::saved ), firstFailed( &first, &SaveArticle::failed );
    first.start( target, true );
    QVERIFY2( QTest::qWaitFor( [&] {
      return fixture.a->lastResourceRequest() && fixture.b->lastResourceRequest();
    }, StepBudgetMs ), "the first save did not request both dictionaries' resources" );
    sptr< TestSupport::ControlledDataRequest > const firstA = fixture.a->lastResourceRequest();
    sptr< TestSupport::ControlledDataRequest > const firstB = fixture.b->lastResourceRequest();
    QVERIFY( QTest::qWaitFor( [&] { return first.state() == SaveArticle::State::Downloading; }, StepBudgetMs ) );

    SaveArticle second( view.get(), fixture.resources );
    QSignalSpy secondSaved( &second, &SaveArticle::saved ), secondCancelled( &second, &SaveArticle::cancelled ),
               secondFailed( &second, &SaveArticle::failed );
    second.start( target, true );
    // The second operation must reach its own staging tree with its *own* requests: the
    // first operation's handles still answer a bare non-null test.
    QVERIFY2( QTest::qWaitFor( [&] {
      auto const a = fixture.a->lastResourceRequest();
      auto const b = fixture.b->lastResourceRequest();
      return a && b && a != firstA && b != firstB;
    }, StepBudgetMs ), "the second save did not issue its own resource requests" );
    QVERIFY( QTest::qWaitFor( [&] { return second.state() == SaveArticle::State::Downloading; }, StepBudgetMs ) );
    QCOMPARE( first.state(), SaveArticle::State::Downloading );
    QStringList const treesBeforeCancel = stagingPaths();
    QVERIFY2( treesBeforeCancel.size() >= 2,
              qPrintable( QStringLiteral( "expected two live staging trees, saw %1" )
                            .arg( treesBeforeCancel.size() ) ) );

    second.cancel();
    QVERIFY( QTest::qWaitFor( [&] { return !secondCancelled.isEmpty(); }, StepBudgetMs ) );

    // Nothing has been downloaded yet, so the survivor's staging tree cannot have been
    // recreated by a later mkpath: it must be on disk *now*, or its peer's cleanup
    // damaged it and the subsequent publication would be a silent recovery.
    bool survivorTreeSurvived = false;
    for( QString const & tree : treesBeforeCancel )
      survivorTreeSurvived = survivorTreeSurvived || QDir( tree ).exists();
    QVERIFY2( survivorTreeSurvived,
              "cancelling one save removed the peer's staging tree as well" );

    // The survivor publishes from *its* staged assets, which must have survived the
    // cancelled peer's cleanup.
    firstA->finishSuccess();
    firstB->finishSuccess();
    QVERIFY( QTest::qWaitFor( [&] { return !firstSaved.isEmpty() || !firstFailed.isEmpty(); }, StepBudgetMs ) );
    QVERIFY2( firstFailed.isEmpty(), qPrintable( firstFailed.value( 0 ).value( 0 ).toString() ) );
    QCOMPARE( firstSaved.size(), 1 );
    QCOMPARE( secondCancelled.size(), 1 );
    QVERIFY( secondFailed.isEmpty() );
    QVERIFY( secondSaved.isEmpty() );

    QFile published( target );
    QVERIFY( published.open( QIODevice::ReadOnly ) );
    QVERIFY( QString::fromUtf8( published.readAll() ).contains( QStringLiteral( "<html" ), Qt::CaseInsensitive ) );
    QStringList const entries = outputEntries( output );
    QCOMPARE( entries.count( QStringLiteral( "shared.html" ) ), 1 );
    for( QString const & entry : entries )
      QVERIFY2( !entry.contains( QStringLiteral( "goldendict-save" ) ),
                qPrintable( QStringLiteral( "staging path left behind: %1" ).arg( entry ) ) );
  }

  void supersededSnapshotCannotFinishNewOperation()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    holdSnapshots = true;
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved ), failed( &save, &SaveArticle::failed ),
               cancelled( &save, &SaveArticle::cancelled );
    save.start( output.filePath( "old.html" ), true );
    QVERIFY( QTest::qWaitFor( [] { return snapshots.size() == 1; }, StepBudgetMs ) );
    auto old = snapshots.takeFirst();
    save.start( output.filePath( "new.html" ), false );
    QVERIFY( QTest::qWaitFor( [] { return snapshots.size() == 1; }, StepBudgetMs ) );
    old.callback( old.html );
    QCOMPARE( save.state(), SaveArticle::State::Capturing );
    QVERIFY( outputEntries( output ).isEmpty() );
    auto fresh = snapshots.takeFirst();
    fresh.callback( fresh.html );
    QVERIFY( QTest::qWaitFor( [&] { return !saved.isEmpty(); }, StepBudgetMs ) );
    QCOMPARE( saved.size(), 1 );
    QCOMPARE( cancelled.size(), 1 );
    QVERIFY( failed.isEmpty() );
    QCOMPARE( outputEntries( output ), QStringList{ "new.html" } );
  }

  void relativeResourcesUseTheLiveHttpPageBase()
  {
    TestSupport::HttpFixture server;
    QVERIFY2( server.start( QHostAddress::LocalHost, 0 ), qPrintable( server.errorString() ) );
    Fixture fixture; auto view = fixture.view(); QVERIFY( loadFixture( *view ) );
    auto * page = view->findChild< QWebEnginePage * >();
    QSignalSpy loaded( page, &QWebEnginePage::loadFinished );
    page->load( server.url( "/dir/first.html" ) );
    QVERIFY( QTest::qWaitFor( [&] {
      return !loaded.isEmpty() && loaded.last().first().toBool() && !page->isLoading() &&
             page->url() == server.url( "/dir/first.html" );
    }, StepBudgetMs ) );
    QVERIFY2( evaluate( *view, "!!document.getElementById('relative-image')" ).toBool(),
              qPrintable( evaluate( *view, "document.documentElement.outerHTML" ).toString() ) );
    // /dir/pixel.png is deliberately absent in the shared HTTP fixture.
    evaluate( *view, "document.getElementById('relative-image').setAttribute('src','../pixel.png')" );
    QVERIFY( QTest::qWaitFor( [&] {
      return evaluate( *view, "document.getElementById('relative-image').naturalWidth" ).toInt() == 1;
    }, StepBudgetMs ) );
    evaluate( *view, "document.querySelectorAll('script').forEach(node=>node.remove())" );
    QTemporaryDir output; QVERIFY( output.isValid() );
    QString const destination = output.filePath( "article.html" );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved ), failed( &save, &SaveArticle::failed ),
               cancelled( &save, &SaveArticle::cancelled );
    save.start( destination, true );
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( saved.size(), 1 );
    QStringList const assets = assetDirectories( output ); QCOMPARE( assets.size(), 1 );
    QString const root = output.filePath( assets.first() + "/127.0.0.1/" );
    QCOMPARE( readFile( root + "pixel.png" ), TestSupport::HttpFixture::pixelPng() );
    QCOMPARE( readFile( root + "dir/style.css" ), TestSupport::HttpFixture::fixtureBytes( "website/style.css" ) );
    QWebEnginePage reopened( fixture.controller->profile() );
    QSignalSpy reopenedLoad( &reopened, &QWebEnginePage::loadFinished );
    reopened.load( QUrl::fromLocalFile( destination ) );
    QVERIFY( QTest::qWaitFor( [&] { return !reopenedLoad.isEmpty(); }, StepBudgetMs ) );
    auto width = std::make_shared< int >( -1 );
    reopened.runJavaScript( "document.getElementById('relative-image').naturalWidth",
                            [width]( QVariant const & value ) { *width = value.toInt(); } );
    QVERIFY( QTest::qWaitFor( [width] { return *width >= 0; }, StepBudgetMs ) );
    QCOMPARE( *width, 1 );
  }

  void httpResourceSuccessAndFailure_data()
  {
    QTest::addColumn< QString >( "path" );
    QTest::addColumn< bool >( "success" );
    QTest::newRow( "successful-http" ) << QStringLiteral( "/pixel.png" ) << true;
    QTest::newRow( "failed-http" ) << QStringLiteral( "/required-missing" ) << false;
  }

  void httpResourceSuccessAndFailure()
  {
    QFETCH( QString, path ); QFETCH( bool, success );
    TestSupport::HttpFixture server;
    QVERIFY2( server.start( QHostAddress::LocalHost, 0 ), qPrintable( server.errorString() ) );
    Fixture fixture; auto view = fixture.view(); QVERIFY( loadFixture( *view ) );
    evaluate( *view, QStringLiteral( "document.body.innerHTML=\"<img src='%1'>\"" )
                        .arg( server.url( path ).toString() ) );
    QVERIFY( QTest::qWaitFor( [&] {
      return evaluate( *view, "document.images[0].complete" ).toBool();
    }, StepBudgetMs ) );
    QTemporaryDir output; QVERIFY( output.isValid() );
    QString const destination = output.filePath( "article.html" );
    QVERIFY( writeFile( destination, "previous html" ) );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved ), failed( &save, &SaveArticle::failed ),
               cancelled( &save, &SaveArticle::cancelled );
    save.start( destination, true );
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( saved.size(), success ? 1 : 0 );
    QCOMPARE( failed.size(), success ? 0 : 1 );
    if( success )
    {
      QStringList const assets = assetDirectories( output ); QCOMPARE( assets.size(), 1 );
      QCOMPARE( readFile( output.filePath( assets.first() + "/127.0.0.1/pixel.png" ) ),
                TestSupport::HttpFixture::pixelPng() );
    }
    else
    {
      QVERIFY( failed.first().first().toString().contains( server.url( path ).toString() ) );
      QCOMPARE( readFile( destination ), QByteArray( "previous html" ) );
      QCOMPARE( outputEntries( output ), QStringList{ "article.html" } );
    }
  }

  void slowHttpCancellationAbortsReply()
  {
    TestSupport::HttpFixture server;
    QVERIFY2( server.start( QHostAddress::LocalHost, 0 ), qPrintable( server.errorString() ) );
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    evaluate( *view, QStringLiteral( "document.getElementById('saved-image').src='%1'" )
                        .arg( server.url( "/slow-resource" ).toString() ) );
    QVERIFY( QTest::qWaitFor( [&] { return server.heldRequestCount( "/slow-resource" ) == 1; }, StepBudgetMs ) );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved ), failed( &save, &SaveArticle::failed ),
               cancelled( &save, &SaveArticle::cancelled );
    bool aborted = false;
    connect( &fixture.resources, &QNetworkAccessManager::finished, &save, [&]( QNetworkReply * reply ) {
      if( reply->url().path() == QLatin1String( "/slow-resource" ) )
        aborted = reply->error() == QNetworkReply::OperationCanceledError;
    } );
    save.start( output.filePath( "article.html" ), true );
    QVERIFY( QTest::qWaitFor( [&] { return server.heldRequestCount( "/slow-resource" ) == 2; }, StepBudgetMs ) );
    save.cancel();
    QVERIFY( aborted );
    QCOMPARE( cancelled.size(), 1 );
    QVERIFY( saved.isEmpty() && failed.isEmpty() );
    QVERIFY( outputEntries( output ).isEmpty() );
    QVERIFY( QTest::qWaitFor( [&] { return server.heldRequestCount( "/slow-resource" ) == 1; }, StepBudgetMs ) );
    server.release( "/slow-resource" );
  }

  void publicationFailuresPreserveForeignOutputs_data()
  {
    QTest::addColumn< bool >( "renameFailure" );
    QTest::newRow( "asset-rename-collision" ) << true;
    QTest::newRow( "html-commit-permission" ) << false;
  }

  void publicationFailuresPreserveForeignOutputs()
  {
    QFETCH( bool, renameFailure );
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const destination = output.filePath( "article.html" );
    QVERIFY( writeFile( destination, "old html" ) );
    QVERIFY( QDir().mkpath( output.filePath( "article_files-foreign" ) ) );
    QVERIFY( writeFile( output.filePath( "article_files-foreign/old.bin" ), "old assets" ) );
    observedDestination = destination;
    failAssetRename = renameFailure;
    failHtmlCommit = !renameFailure;
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved );
    QSignalSpy failed( &save, &SaveArticle::failed );
    QSignalSpy cancelled( &save, &SaveArticle::cancelled );
    save.start( destination, true );
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( save.state(), SaveArticle::State::Failed );
    QCOMPARE( failed.size(), 1 );
    QVERIFY( saved.isEmpty() );
    QCOMPARE( readFile( destination ), QByteArray( "old html" ) );
    QCOMPARE( readFile( output.filePath( "article_files-foreign/old.bin" ) ), QByteArray( "old assets" ) );
    if( renameFailure )
    {
      QCOMPARE( readFile( QDir( publishedAssets ).filePath( "foreign" ) ), QByteArray( "unowned" ) );
      QCOMPARE( publicationEvents, QStringList{ "rename-failed" } );
      QVERIFY( QDir( publishedAssets ).removeRecursively() );
    }
    else
    {
      QVERIFY( assetsPresentAtCommit );
      QVERIFY( fallbackDisabled );
      QVERIFY( !QFileInfo::exists( publishedAssets ) );
      QCOMPARE( publicationEvents, ( QStringList{ "assets", "html" } ) );
    }
    QCOMPARE( outputEntries( output ), ( QStringList{ "article.html", "article_files-foreign" } ) );
  }

  void downloadWriteFailureDoesNotAcceptDirectoryAsResource()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    evaluate( *view, "document.querySelectorAll('link').forEach(node=>node.remove())" );
    fixture.a->clearObservations();
    fixture.a->setResourcePending( true );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved );
    QSignalSpy failed( &save, &SaveArticle::failed );
    QSignalSpy cancelled( &save, &SaveArticle::cancelled );
    save.start( output.filePath( "article.html" ), true );
    QVERIFY( QTest::qWaitFor( [&] { return bool( fixture.a->lastResourceRequest() ); }, StepBudgetMs ) );
    // Collide with the pending destination inside this operation's staging.
    QStringList const staging = outputEntries( output );
    QCOMPARE( staging.size(), 1 );
    QString const root = output.filePath( staging.first() + "/assets/dict-a/" );
    QVERIFY( QDir().mkpath( root + "image.png" ) );
    auto request = fixture.a->lastResourceRequest();
    request->append( QByteArrayLiteral( "resource bytes" ) );
    request->finishSuccess();
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( save.state(), SaveArticle::State::Failed );
    QCOMPARE( failed.size(), 1 );
    QVERIFY( saved.isEmpty() );
    QVERIFY( failed.first().first().toString().contains( "bres://dict-a/" ) );
    QVERIFY( outputEntries( output ).isEmpty() );
  }

  void cancellationRetiresQueuedPublication()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    evaluate( *view, "document.querySelectorAll('link').forEach(node=>node.remove())" );
    fixture.a->clearObservations();
    fixture.a->setResourcePending( true );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const destination = output.filePath( "article.html" );
    QVERIFY( writeFile( destination, "old html" ) );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved ), failed( &save, &SaveArticle::failed ),
               cancelled( &save, &SaveArticle::cancelled );
    save.start( destination, true );
    QVERIFY( QTest::qWaitFor( [&] { return bool( fixture.a->lastResourceRequest() ); }, StepBudgetMs ) );
    auto request = fixture.a->lastResourceRequest();
    request->append( onePixelPng() );
    request->finishSuccess();
    QCOMPARE( save.state(), SaveArticle::State::Publishing );
    save.cancel();
    bool retired = false;
    QMetaObject::invokeMethod( &save, [&] { retired = true; }, Qt::QueuedConnection );
    QVERIFY( QTest::qWaitFor( [&] { return retired; }, StepBudgetMs ) );
    QCOMPARE( cancelled.size(), 1 );
    QVERIFY( saved.isEmpty() && failed.isEmpty() );
    QCOMPARE( readFile( destination ), QByteArray( "old html" ) );
    QCOMPARE( outputEntries( output ), QStringList{ "article.html" } );
  }

  void failedResourceCancelsOtherPendingDownloads()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    evaluate( *view, "document.querySelectorAll('link').forEach(node=>node.remove());"
                     "document.body.insertAdjacentHTML('beforeend',\"<img src='bres://dict-b/missing.png'>\")" );
    QVERIFY( QTest::qWaitFor( [&] {
      return evaluate( *view, "Array.from(document.images).every(image=>image.complete)" ).toBool();
    }, StepBudgetMs ) );
    fixture.a->clearObservations();
    fixture.a->setResourcePending( true );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved ), failed( &save, &SaveArticle::failed ),
               cancelled( &save, &SaveArticle::cancelled );
    save.start( output.filePath( "article.html" ), true );
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( failed.size(), 1 );
    QVERIFY( failed.first().first().toString().contains( "bres://dict-b/missing.png" ) );
    auto request = fixture.a->lastResourceRequest();
    QVERIFY( request );
    QVERIFY( request->cancelCount() > 0 );
    QVERIFY( saved.isEmpty() && cancelled.isEmpty() );
    QVERIFY( outputEntries( output ).isEmpty() );
  }

  void htmlOpenFailureRollsBackPublishedAssets()
  {
    Fixture fixture;
    auto view = fixture.view();
    QVERIFY( loadFixture( *view ) );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    QString const destination = output.filePath( "article.html" );
    QVERIFY( QDir().mkdir( destination ) );
    QVERIFY( writeFile( QDir( destination ).filePath( "foreign" ), "keep" ) );
    observedDestination = destination;
    SaveArticle save( view.get(), fixture.resources );
    QSignalSpy saved( &save, &SaveArticle::saved ), failed( &save, &SaveArticle::failed ),
               cancelled( &save, &SaveArticle::cancelled );
    save.start( destination, true );
    QVERIFY( waitForTerminal( save, saved, failed, cancelled ) );
    QCOMPARE( failed.size(), 1 );
    QCOMPARE( readFile( QDir( destination ).filePath( "foreign" ) ), QByteArray( "keep" ) );
    QVERIFY( !QFileInfo::exists( publishedAssets ) );
    QVERIFY( saved.isEmpty() );
    QCOMPARE( outputEntries( output ), QStringList{ "article.html" } );
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

  registerArticleSchemes();
  QApplication app( argc, argv );
  QCoreApplication::setApplicationName( QStringLiteral( "GoldenDict" ) );
  QNetworkProxy::setApplicationProxy( QNetworkProxy::NoProxy );

  SaveArticleTest tests;
  return QTest::qExec( &tests, argc, argv );
}

#include "tst_save_article.moc"
