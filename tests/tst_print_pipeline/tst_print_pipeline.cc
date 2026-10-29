/* Frozen PDF snapshots through the production renderer and Qt PDF decoder. */
#include <QtTest>
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QFile>
#include <QNetworkProxy>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPrinter>
#include <QPrintPreviewDialog>
#include <QSaveFile>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWebEngineFrame>

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
#define WAIT_FOR(condition) \
  QVERIFY2( QTest::qWaitFor( [&] { return bool(condition); }, int(qMax<qint64>(0, deadline.remainingTime())) ), #condition )

using PdfResult = std::function<void(QByteArray const &)>;
struct Snapshot { PdfResult callback; QByteArray bytes; };
QList<Snapshot> snapshots;
bool holdSnapshots = false;
int snapshotCalls = 0, readinessChecks = 0, frameReadinessChecks = 0, verificationChecks = 0, loadCalls = 0;
bool replaceNonce = false;
QList<QSize> renderSizes;
QList<QRectF> paintedRects;
QList<int> renderedPages;
QPdfDocument * observedDocument = nullptr;
QIODevice * observedDevice = nullptr;
QPrinter * observedPrinter = nullptr;
QSet<QPainter *> printerPainters;

/// Observes that no additional snapshot appears for `windowMs` while the pipeline is
/// still waiting. A deferred, unguarded release opportunity (for example a short timer
/// armed when a snapshot begins) would fire inside this window, so the barrier is only
/// pinned if the count stays put across it.
bool snapshotStaysAt( int expected, int windowMs )
{
  QDeadlineTimer deadline( windowMs );
  while( !deadline.hasExpired() )
  {
    if( snapshotCalls != expected ) return false;
    QTest::qWait( 20 );
  }
  return snapshotCalls == expected;
}
bool failRender = false, failBegin = false, failNewPage = false, failEnd = false;
int begins = 0, newPages = 0, ends = 0;
enum class LoadMode { Normal, Silent, Queued, Held };
LoadMode loadMode = LoadMode::Normal;
QList<std::function<void()>> pendingLoads;
QString observedDestination;
bool fallbackDisabled = false, failCommit = false;
int commits = 0;
QByteArray beforeCommit;

QByteArray readFile( QString const & path )
{
  QFile file( path );
  return file.open( QIODevice::ReadOnly ) ? file.readAll() : QByteArray();
}
bool writeFile( QString const & path, QByteArray const & bytes )
{
  QFile file( path );
  return file.open( QIODevice::WriteOnly ) && file.write( bytes ) == bytes.size();
}
QStringList entries( QTemporaryDir const & output )
{
  return QDir( output.path() ).entryList( QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name );
}
QPageLayout layout()
{
  return QPageLayout( QPageSize( QPageSize::A4 ), QPageLayout::Portrait, QMarginsF() );
}
class TestPrinter : public QPrinter
{
public:
  bool newPage() override
  {
    ++newPages;
    return failNewPage ? false : QPrinter::newPage();
  }
};
QByteArray hash( QByteArray const & bytes ) { return QCryptographicHash::hash( bytes, QCryptographicHash::Sha256 ); }

QVariant evaluate( QWebEngineFrame frame, QString const & script )
{
  struct Answer { bool done = false; QVariant value; };
  auto answer = std::make_shared<Answer>();
  frame.runJavaScript( script, [answer]( QVariant const & value ) { answer->value = value; answer->done = true; } );
  if( !QTest::qWaitFor( [answer] { return answer->done; }, BudgetMs ) )
    QTest::qFail( "the real document did not answer", __FILE__, __LINE__ );
  return answer->value;
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
  std::unique_ptr<ArticleWebView> ownedRenderer{std::make_unique<ArticleWebView>()};
  ArticleWebView & renderer = *ownedRenderer;
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
  bool start() { return bridge && http.start( QHostAddress( "127.0.0.1" ) ); }
  bool load()
  {
    if( !start() ) return false;
    QSignalSpy loaded( &renderer, &QWebEngineView::loadFinished );
    renderer.load( http.url( "/print/two-pages.html" ) );
    return QTest::qWaitFor( [&] { return !loaded.isEmpty(); }, BudgetMs ) && loaded.first().first().toBool();
  }
};
void configurePrinter( QPrinter & printer, QString const & path )
{
  printer.setResolution( 96 );
  printer.setOutputFormat( QPrinter::PdfFormat );
  printer.setOutputFileName( path );
  printer.setPageLayout( QPageLayout( QPageSize( QPageSize::A4 ), QPageLayout::Portrait,
                                    QMarginsF( 10, 12, 10, 12 ) ) );
  observedPrinter = &printer;
}
}

extern "C" void realViewPdf( QWebEngineView *, PdfResult const &, QPageLayout const &, QPageRanges const & )
  asm("__real__ZN14QWebEngineView10printToPdfERKSt8functionIFvRK10QByteArrayEERK11QPageLayoutRK11QPageRanges");
extern "C" void wrapViewPdf( QWebEngineView *, PdfResult const &, QPageLayout const &, QPageRanges const & )
  asm("__wrap__ZN14QWebEngineView10printToPdfERKSt8functionIFvRK10QByteArrayEERK11QPageLayoutRK11QPageRanges");
extern "C" void wrapViewPdf( QWebEngineView * view, PdfResult const & callback, QPageLayout const & pageLayout,
                              QPageRanges const & ranges )
{
  ++snapshotCalls;
  bool const hold = holdSnapshots;
  realViewPdf( view, [callback, hold]( QByteArray const & bytes ) {
    if( hold ) snapshots.append( {callback, bytes} );
    else callback( bytes );
  }, pageLayout, ranges );
}
extern "C" void realPagePdf( QWebEnginePage *, PdfResult const &, QPageLayout const &, QPageRanges const & )
  asm("__real__ZN14QWebEnginePage10printToPdfERKSt8functionIFvRK10QByteArrayEERK11QPageLayoutRK11QPageRanges");
extern "C" void wrapPagePdf( QWebEnginePage *, PdfResult const &, QPageLayout const &, QPageRanges const & )
  asm("__wrap__ZN14QWebEnginePage10printToPdfERKSt8functionIFvRK10QByteArrayEERK11QPageLayoutRK11QPageRanges");
extern "C" void wrapPagePdf( QWebEnginePage * page, PdfResult const & callback, QPageLayout const & pageLayout,
                              QPageRanges const & ranges )
{
  ++snapshotCalls;
  realPagePdf( page, callback, pageLayout, ranges );
}
extern "C" void realFrameJs( QWebEngineFrame *, QString const &, quint32, ArticleBridge::Result const & )
  asm("__real__ZN15QWebEngineFrame13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE");
extern "C" void wrapFrameJs( QWebEngineFrame *, QString const &, quint32, ArticleBridge::Result const & )
  asm("__wrap__ZN15QWebEngineFrame13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE");
extern "C" void wrapFrameJs( QWebEngineFrame * frame, QString const & script, quint32 world,
                              ArticleBridge::Result const & callback )
{
  bool const readiness = script.contains( "/*gd-print-ready*/" );
  bool const child = !frame->isMainFrame();
  bool const verification = script.contains( "/*gd-print-verify*/" );
  QString request = script;
  if( verification && replaceNonce )
  {
    replaceNonce = false;
    request.prepend( "window.__gdWebsiteFrame.docId+='-replacement';" );
  }
  realFrameJs( frame, request, world, [callback, readiness, verification, child]( QVariant const & value ) {
    if( readiness ) { ++readinessChecks; if( child ) ++frameReadinessChecks; }
    if( verification ) ++verificationChecks;
    if( callback ) callback( value );
  } );
}
extern "C" void realLoad( QPdfDocument *, QIODevice * ) asm("__real__ZN12QPdfDocument4loadEP9QIODevice");
extern "C" void wrapLoad( QPdfDocument *, QIODevice * ) asm("__wrap__ZN12QPdfDocument4loadEP9QIODevice");
extern "C" void wrapLoad( QPdfDocument * document, QIODevice * device )
{
  ++loadCalls;
  observedDocument = document; observedDevice = device;
  if( loadMode == LoadMode::Silent )
  {
    auto const blocked = document->blockSignals( true );
    realLoad( document, device );
    document->blockSignals( blocked );
  }
  else if( loadMode == LoadMode::Queued || loadMode == LoadMode::Held )
  {
    QPointer<QPdfDocument> guard( document ); QPointer<QIODevice> buffer( device );
    auto load = [guard, buffer] { if( guard && buffer ) realLoad( guard, buffer ); };
    if( loadMode == LoadMode::Held ) pendingLoads.append( load );
    else QTimer::singleShot( 0, document, load );
  }
  else realLoad( document, device );
}
extern "C" QImage realRender( QPdfDocument *, int, QSize, QPdfDocumentRenderOptions )
  asm("__real__ZN12QPdfDocument6renderEi5QSize25QPdfDocumentRenderOptions");
extern "C" QImage wrapRender( QPdfDocument *, int, QSize, QPdfDocumentRenderOptions )
  asm("__wrap__ZN12QPdfDocument6renderEi5QSize25QPdfDocumentRenderOptions");
extern "C" QImage wrapRender( QPdfDocument * document, int page, QSize size, QPdfDocumentRenderOptions options )
{
  renderSizes.append( size ); renderedPages.append( page );
  if( failRender ) return {};
  return realRender( document, page, size, options );
}
extern "C" bool realBegin( QPainter *, QPaintDevice * ) asm("__real__ZN8QPainter5beginEP12QPaintDevice");
extern "C" bool wrapBegin( QPainter *, QPaintDevice * ) asm("__wrap__ZN8QPainter5beginEP12QPaintDevice");
extern "C" bool wrapBegin( QPainter * painter, QPaintDevice * device )
{
  if( observedPrinter && device == observedPrinter )
  {
    ++begins;
    if( failBegin ) return false;
    printerPainters.insert( painter );
  }
  return realBegin( painter, device );
}
extern "C" void realDraw( QPainter *, QPointF const &, QImage const & )
  asm("__real__ZN8QPainter9drawImageERK7QPointFRK6QImage");
extern "C" void wrapDraw( QPainter *, QPointF const &, QImage const & )
  asm("__wrap__ZN8QPainter9drawImageERK7QPointFRK6QImage");
extern "C" void wrapDraw( QPainter * painter, QPointF const & origin, QImage const & image )
{
  if( printerPainters.contains( painter ) ) paintedRects.append( QRectF( origin, image.size() ) );
  realDraw( painter, origin, image );
}
extern "C" bool realEnd( QPainter * ) asm("__real__ZN8QPainter3endEv");
extern "C" bool wrapEnd( QPainter * ) asm("__wrap__ZN8QPainter3endEv");
extern "C" bool wrapEnd( QPainter * painter )
{
  bool const watched = printerPainters.remove( painter );
  if( watched ) ++ends;
  bool const ok = realEnd( painter );
  return watched && failEnd ? false : ok;
}
extern "C" bool realOpen( QSaveFile *, QIODevice::OpenMode )
  asm("__real__ZN9QSaveFile4openE6QFlagsIN13QIODeviceBase12OpenModeFlagEE");
extern "C" bool wrapOpen( QSaveFile *, QIODevice::OpenMode )
  asm("__wrap__ZN9QSaveFile4openE6QFlagsIN13QIODeviceBase12OpenModeFlagEE");
extern "C" bool wrapOpen( QSaveFile * file, QIODevice::OpenMode mode )
{
  if( file->fileName() == observedDestination ) fallbackDisabled = !file->directWriteFallback();
  return realOpen( file, mode );
}
extern "C" bool realCommit( QSaveFile * ) asm("__real__ZN9QSaveFile6commitEv");
extern "C" bool wrapCommit( QSaveFile * ) asm("__wrap__ZN9QSaveFile6commitEv");
extern "C" bool wrapCommit( QSaveFile * file )
{
  if( file->fileName() == observedDestination )
  {
    ++commits; beforeCommit = readFile( observedDestination );
    if( failCommit ) file->cancelWriting();
  }
  return realCommit( file );
}

class PrintPipelineTest final : public QObject
{
  Q_OBJECT
  bool awaitReady( PrintPipeline & pipeline )
  {
    QSignalSpy ready( &pipeline, &PrintPipeline::ready );
    QSignalSpy failed( &pipeline, &PrintPipeline::failed );
    pipeline.snapshot( layout() );
    bool const done = QTest::qWaitFor( [&] { return ready.count() + failed.count() > 0; }, BudgetMs );
    return done && failed.isEmpty() && ready.size() == 1 && ready.first().first().toInt() == 2;
  }
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
    holdSnapshots = false; snapshots.clear(); snapshotCalls = readinessChecks = frameReadinessChecks = verificationChecks = loadCalls = 0;
    replaceNonce = false;
    renderSizes.clear(); renderedPages.clear(); paintedRects.clear(); observedDocument = nullptr; observedDevice = nullptr;
    observedPrinter = nullptr; printerPainters.clear();
    failRender = failBegin = failNewPage = failEnd = false;
    begins = newPages = ends = 0;
    loadMode = LoadMode::Normal; pendingLoads.clear();
    observedDestination.clear(); fallbackDisabled = failCommit = false; commits = 0; beforeCommit.clear();
  }
  void frozenSnapshotPaint()
  {
    QDeadlineTimer deadline( BudgetMs );
    Browser b;
    QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Idle );
    TestPrinter printer;
    QVERIFY( !pipeline.paint( &printer ) );
    pipeline.preview( &printer );
    QVERIFY( !pipeline.dialog );
    QVERIFY( awaitReady( pipeline ) );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Ready );
    QCOMPARE( pipeline.document.get(), observedDocument );
    QCOMPARE( pipeline.buffer.get(), observedDevice );
    QVERIFY( pipeline.buffer->isOpen() );
    QVERIFY( pipeline.buffer->isReadable() );
    QVERIFY( !pipeline.buffer->isWritable() );
    QCOMPARE( pipeline.document->getAllText( 0 ).text().trimmed().startsWith( "snapshot-page-one" ), true );
    QVERIFY( pipeline.document->getAllText( 1 ).text().contains( "snapshot-page-two" ) );
    QByteArray const frozenHash = hash( pipeline.bytes );
    QByteArray const frozenBytes = pipeline.bytes;
    int const calls = snapshotCalls;
    QCOMPARE( calls, 1 );
    QTemporaryDir output;
    QVERIFY( output.isValid() );
    configurePrinter( printer, output.filePath( "painted.pdf" ) );
    QVERIFY( pipeline.paint( &printer ) );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Ready );
    QCOMPARE( begins, 1 ); QCOMPARE( ends, 1 ); QCOMPARE( newPages, 1 );
    QCOMPARE( renderedPages, QList<int>( {0, 1} ) );
    QCOMPARE( renderSizes.size(), 2 );
    QSize const portrait = renderSizes.first();
    QSize const printable = printer.pageLayout().paintRectPixels( printer.resolution() ).size();
    QVERIFY( portrait.width() <= printable.width() && portrait.height() <= printable.height() );
    QCOMPARE( paintedRects.size(), 2 );
    for( auto const & rect : paintedRects )
    {
      QVERIFY( qAbs( rect.center().x() - printable.width() / 2.0 ) <= 1 );
      QVERIFY( qAbs( rect.center().y() - printable.height() / 2.0 ) <= 1 );
    }
    QSizeF const points = pipeline.document->pagePointSize( 0 );
    QVERIFY( qAbs( double(portrait.width()) / portrait.height() - points.width() / points.height() ) < 0.01 );
    QPdfDocument painted;
    QCOMPARE( painted.load( printer.outputFileName() ), QPdfDocument::Error::None );
    QCOMPARE( painted.pageCount(), 2 );
    QImage first = painted.render( 0, QSize( 400, 560 ) );
    QVERIFY( !first.isNull() );
    bool ink = false;
    for( int y = 0; y < first.height() && !ink; ++y )
      for( int x = 0; x < first.width(); ++x ) if( first.pixelColor( x, y ).lightness() < 100 ) { ink = true; break; }
    QVERIFY( ink );
    painted.close();
    renderSizes.clear(); renderedPages.clear();
    pipeline.preview( &printer );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Previewing );
    WAIT_FOR( !renderSizes.isEmpty() );
    QVERIFY( pipeline.dialog );
    pipeline.dialog->reject();
    WAIT_FOR( pipeline.state() == PrintPipeline::State::Ready );
    QVERIFY( !pipeline.dialog );
    QCOMPARE( pipeline.bytes, frozenBytes );
    QCOMPARE( hash( pipeline.bytes ), frozenHash );
    QCOMPARE( pipeline.document->pageCount(), 2 );
    renderSizes.clear(); renderedPages.clear();
    printer.setPageSize( QPageSize( QPageSize::A5 ) );
    printer.setPageOrientation( QPageLayout::Landscape );
    printer.setOutputFileName( output.filePath( "landscape.pdf" ) );
    QVERIFY( pipeline.paint( &printer ) );
    QCOMPARE( renderSizes.size(), 2 );
    QVERIFY( renderSizes.first() != portrait );
    QCOMPARE( pipeline.bytes, frozenBytes );
    QCOMPARE( hash( pipeline.bytes ), frozenHash );
    QCOMPARE( pipeline.document->pageCount(), 2 );
    QCOMPARE( snapshotCalls, calls );
    QSignalSpy navigated( &b.renderer, &QWebEngineView::loadFinished );
    b.renderer.load( b.http.url( "/dir/second.html" ) );
    WAIT_FOR( !navigated.isEmpty() );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Ready );
    QCOMPARE( pipeline.bytes, frozenBytes );
    printer.setOutputFileName( output.filePath( "after-navigation.pdf" ) );
    QVERIFY( pipeline.paint( &printer ) );
    QCOMPARE( snapshotCalls, calls );
    QCOMPARE( pipeline.bytes, frozenBytes );
    QCOMPARE( entries( output ), QStringList( {"after-navigation.pdf", "landscape.pdf", "painted.pdf"} ) );
  }
  void documentReadyBarrier()
  {
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.start() );
    QString html = QString::fromUtf8( TestSupport::HttpFixture::fixtureBytes( "print/two-pages.html" ) );
    html.replace( "</body>", QStringLiteral( "<img src='%1'></body>" ).arg( b.http.url( "/slow-resource" ).toString() ) );
    b.renderer.setHtml( html, b.http.url( "/print/waiting.html" ) );
    WAIT_FOR( b.http.heldRequestCount( "/slow-resource" ) == 1 );
    QVERIFY( b.renderer.page()->isLoading() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QSignalSpy ready( &pipeline, &PrintPipeline::ready );
    pipeline.snapshot( layout() );
    QCOMPARE( pipeline.generationTimer.interval(), PrintPipelinePolicy::generationTimeoutMs );
    QCOMPARE( pipeline.readinessTimer.interval(), PrintPipelinePolicy::readinessCheckIntervalMs );
    QVERIFY( evaluate( b.renderer.page()->mainFrame(), "document.readyState!=='complete'" ).toBool() );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Generating );
    QCOMPARE( snapshotCalls, 0 );
    b.http.release( "/slow-resource" );
    WAIT_FOR( !ready.isEmpty() );
    QCOMPARE( ready.first().first().toInt(), 2 );
    QCOMPARE( snapshotCalls, 1 );
  }
  void nativeFrameReadyBarrier()
  {
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.start() );
    QSignalSpy loaded( &b.renderer, &QWebEngineView::loadFinished );
    b.renderer.setHtml( QStringLiteral(
      "<!doctype html><script>gdArticleContents=['dict-a'];</script><div id='gdfrom-dict-a'>"
      "<iframe id='gdexpandframe-dict-a' name='gdexpandframe-dict-a' style='width:100%;display:none' src='%1'></iframe></div>" )
      .arg( b.http.url( "/dir/first.html" ).toString() ), QUrl( "gdlookup://localhost/" ) );
    WAIT_FOR( !loaded.isEmpty() );
    WAIT_FOR( evaluate( b.renderer.page()->mainFrame(),
      "document.getElementById('gdexpandframe-dict-a').style.display==='block'" ).toBool() );
    auto frame = b.renderer.page()->findFrameByName( "gdexpandframe-dict-a" );
    QVERIFY( frame.has_value() );
    QVERIFY( evaluate( *frame,
      "(()=>{const f=new FontFace('PrintBarrier','url(/slow-resource)');document.fonts.add(f);"
      "document.fonts.load('20px PrintBarrier').catch(()=>{});return true})()" ).toBool() );
    WAIT_FOR( b.http.heldRequestCount( "/slow-resource" ) == 1 );
    QVERIFY( !b.renderer.page()->isLoading() );
    QCOMPARE( evaluate( b.renderer.page()->mainFrame(), "document.readyState" ).toString(), QStringLiteral( "complete" ) );
    QCOMPARE( evaluate( *frame, "document.fonts.status" ).toString(), QStringLiteral( "loading" ) );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QSignalSpy ready( &pipeline, &PrintPipeline::ready );
    pipeline.snapshot( layout() );
    WAIT_FOR( frameReadinessChecks > 0 );
    auto drained = std::make_shared<bool>( false );
    b.bridge->run( "/*test-print-barrier*/true", [drained]( QVariant const & ) { *drained = true; } );
    WAIT_FOR( *drained );
    QCOMPARE( snapshotCalls, 0 );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Generating );
    // The barrier is only pinned if no snapshot appears across a deferred release
    // opportunity; a short unguarded timer would fire inside this window.
    QVERIFY2( snapshotStaysAt( 0, 1000 ),
              "a snapshot was released while the pipeline was not ready" );

    b.http.release( "/slow-resource" );
    WAIT_FOR( !ready.isEmpty() );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Ready );
    QCOMPARE( snapshotCalls, 1 );
    QVERIFY( pipeline.document->getAllText( 0 ).text().contains( "relative-link" ) ||
             pipeline.document->getAllText( 0 ).text().contains( "first" ) );
  }
  void documentNonceBarrier()
  {
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QSignalSpy ready( &pipeline, &PrintPipeline::ready );
    replaceNonce = true;
    pipeline.snapshot( layout() );
    // Observe precisely one state check; prevent the retry pulse from masking
    // acceptance of an obsolete document nonce in the first check.
    pipeline.readinessTimer.stop();
    WAIT_FOR( verificationChecks == 1 );
    QVERIFY( evaluate( b.renderer.page()->mainFrame(), "__gdWebsiteFrame.docId.endsWith('-replacement')" ).toBool() );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Generating );
    QCOMPARE( snapshotCalls, 0 ); QVERIFY( ready.isEmpty() );
    // The barrier is only pinned if no snapshot appears across a deferred release
    // opportunity; a short unguarded timer would fire inside this window.
    QVERIFY2( snapshotStaysAt( 0, 1000 ),
              "a snapshot was released while the pipeline was not ready" );

    pipeline.checkReadiness();
    WAIT_FOR( ready.count() == 1 );
    QCOMPARE( snapshotCalls, 1 );
  }
  void pdfLoading_data()
  {
    QTest::addColumn<int>( "mode" ); QTest::addColumn<QByteArray>( "result" ); QTest::addColumn<bool>( "success" );
    QTest::newRow( "immediate-ready-without-signal" ) << int( LoadMode::Silent ) << QByteArray( "real" ) << true;
    QTest::newRow( "queued-ready-signal" ) << int( LoadMode::Queued ) << QByteArray( "real" ) << true;
    QTest::newRow( "immediate-error-without-signal" ) << int( LoadMode::Silent ) << QByteArray( "invalid PDF" ) << false;
    QTest::newRow( "queued-error-signal" ) << int( LoadMode::Queued ) << QByteArray( "invalid PDF" ) << false;
    QTest::newRow( "empty-renderer-result" ) << int( LoadMode::Normal ) << QByteArray() << false;
  }
  void pdfLoading()
  {
    QFETCH( int, mode ); QFETCH( QByteArray, result ); QFETCH( bool, success );
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QSignalSpy ready( &pipeline, &PrintPipeline::ready ); QSignalSpy failed( &pipeline, &PrintPipeline::failed );
    holdSnapshots = true; pipeline.snapshot( layout() );
    WAIT_FOR( snapshots.size() == 1 );
    auto const captured = snapshots.first();
    QVERIFY( captured.bytes.startsWith( "%PDF-" ) );
    loadMode = LoadMode( mode );
    // The exact private loading helper is the production callback's target.
    // Replacing only these bytes exercises the real QPdfDocument error paths.
    pipeline.loadSnapshot( pipeline.operation, success ? captured.bytes : result );
    WAIT_FOR( ready.count() + failed.count() == 1 );
    QCOMPARE( ready.count(), success ? 1 : 0 );
    QCOMPARE( failed.count(), success ? 0 : 1 );
    QCOMPARE( pipeline.state(), success ? PrintPipeline::State::Ready : PrintPipeline::State::Failed );
    if( success )
    {
      QCOMPARE( pipeline.document->status(), QPdfDocument::Status::Ready );
      QCOMPARE( pipeline.document->pageCount(), 2 );
      QVERIFY( pipeline.buffer->isOpen() );
    }
    else { QVERIFY( !pipeline.document ); QVERIFY( !pipeline.buffer ); QVERIFY( pipeline.bytes.isEmpty() ); }
    captured.callback( captured.bytes ); // Duplicate/late success cannot undo Error or emit ready twice.
    QCoreApplication::sendPostedEvents( &pipeline, QEvent::MetaCall );
    QCOMPARE( ready.count() + failed.count(), 1 );
  }
  void generationTimeout_data()
  {
    QTest::addColumn<bool>( "loading" );
    QTest::newRow( "hung-renderer" ) << false;
    QTest::newRow( "hung-pdf-load" ) << true;
  }
  void generationTimeout()
  {
    QFETCH( bool, loading );
    QDeadlineTimer deadline( PrintPipelinePolicy::generationTimeoutMs + BudgetMs );
    QCOMPARE( PrintPipelinePolicy::generationTimeoutMs, 30000 );
    Browser b; QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QSignalSpy ready( &pipeline, &PrintPipeline::ready ); QSignalSpy failed( &pipeline, &PrintPipeline::failed );
    QTemporaryDir output; QVERIFY( output.isValid() );
    QString const destination = output.filePath( "timeout.pdf" );
    QVERIFY( writeFile( destination, "existing" ) );
    holdSnapshots = true;
    pipeline.exportPdf( destination, layout() );
    QCOMPARE( pipeline.generationTimer.interval(), PrintPipelinePolicy::generationTimeoutMs );
    WAIT_FOR( snapshots.size() == 1 );
    QVERIFY( pipeline.generationTimer.isActive() );
    if( loading )
    {
      int const originalTimer = pipeline.generationTimer.timerId();
      loadMode = LoadMode::Held;
      auto const generated = snapshots.first(); generated.callback( generated.bytes );
      WAIT_FOR( pipeline.state() == PrintPipeline::State::LoadingPdf );
      QCOMPARE( pipeline.generationTimer.timerId(), originalTimer );
      QVERIFY( pipeline.generationTimer.isActive() );
    }
    WAIT_FOR( failed.size() == 1 );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Failed );
    QVERIFY( failed.first().first().toString().contains( "timed out" ) );
    QVERIFY( ready.isEmpty() ); QVERIFY( !pipeline.generationTimer.isActive() );
    auto const late = snapshots.first(); late.callback( late.bytes );
    QCoreApplication::sendPostedEvents( &pipeline, QEvent::MetaCall );
    QCOMPARE( failed.size(), 1 ); QVERIFY( ready.isEmpty() );
    QCOMPARE( readFile( destination ), QByteArray( "existing" ) );
    QCOMPARE( entries( output ), QStringList( {"timeout.pdf"} ) );
  }
  void cancellation_data()
  {
    QTest::addColumn<QString>( "cause" ); QTest::addColumn<bool>( "loading" );
    for( QString const & cause : {QStringLiteral("cancel"), QStringLiteral("navigation"), QStringLiteral("tab-close")} )
    {
      QTest::newRow( qPrintable( cause + "-generating" ) ) << cause << false;
      QTest::newRow( qPrintable( cause + "-loading" ) ) << cause << true;
    }
  }
  void cancellation()
  {
    QFETCH( QString, cause ); QFETCH( bool, loading );
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QSignalSpy ready( &pipeline, &PrintPipeline::ready ); QSignalSpy failed( &pipeline, &PrintPipeline::failed );
    QSignalSpy cancelled( &pipeline, &PrintPipeline::cancelled );
    QTemporaryDir output; QVERIFY( output.isValid() );
    QString const destination = output.filePath( "cancelled.pdf" );
    QVERIFY( writeFile( destination, "existing" ) );
    holdSnapshots = true; pipeline.exportPdf( destination, layout() );
    WAIT_FOR( snapshots.size() == 1 );
    auto const late = snapshots.first();
    if( loading )
    {
      loadMode = LoadMode::Held;
      late.callback( late.bytes );
      WAIT_FOR( pipeline.state() == PrintPipeline::State::LoadingPdf );
      QCOMPARE( pendingLoads.size(), 1 );
    }
    if( cause == "cancel" ) pipeline.cancel();
    else if( cause == "navigation" ) b.renderer.load( b.http.url( "/dir/second.html" ) );
    else b.ownedRenderer.reset();
    QCOMPARE( pipeline.state(), PrintPipeline::State::Cancelled );
    QCOMPARE( cancelled.count(), 1 );
    QVERIFY( !pipeline.document ); QVERIFY( !pipeline.buffer ); QVERIFY( pipeline.bytes.isEmpty() );
    late.callback( late.bytes );
    for( auto const & pending : pendingLoads ) pending();
    pipeline.cancel();
    QCoreApplication::sendPostedEvents( &pipeline, QEvent::MetaCall );
    QVERIFY( ready.isEmpty() ); QVERIFY( failed.isEmpty() ); QCOMPARE( cancelled.count(), 1 );
    QCOMPARE( readFile( destination ), QByteArray( "existing" ) );
    QCOMPARE( entries( output ), QStringList( {"cancelled.pdf"} ) );
  }
  void ownerDestructionAndReplacement()
  {
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.load() );
    auto pipeline = std::make_unique<PrintPipeline>( &b.renderer, b.bridge );
    holdSnapshots = true;
    pipeline->snapshot( layout() ); WAIT_FOR( snapshots.size() == 1 );
    auto const retired = snapshots.first();
    pipeline.reset(); retired.callback( retired.bytes ); // A live Qt result after its controller dies.
    pipeline = std::make_unique<PrintPipeline>( &b.renderer, b.bridge );
    QTemporaryDir output; QVERIFY( output.isValid() );
    QString const oldPath = output.filePath( "old.pdf" ), newPath = output.filePath( "new.pdf" );
    QVERIFY( writeFile( oldPath, "old destination" ) );
    QSignalSpy ready( pipeline.get(), &PrintPipeline::ready );
    pipeline->exportPdf( oldPath, layout() ); WAIT_FOR( snapshots.size() == 2 );
    auto const old = snapshots.at( 1 );
    pipeline->exportPdf( newPath, layout() ); WAIT_FOR( snapshots.size() == 3 );
    auto const fresh = snapshots.at( 2 );
    old.callback( old.bytes );
    QCoreApplication::sendPostedEvents( pipeline.get(), QEvent::MetaCall );
    QCOMPARE( pipeline->state(), PrintPipeline::State::Generating );
    QVERIFY( ready.isEmpty() ); QVERIFY( !QFileInfo::exists( newPath ) );
    fresh.callback( fresh.bytes );
    WAIT_FOR( ready.count() == 1 );
    QCOMPARE( readFile( oldPath ), QByteArray( "old destination" ) );
    QCOMPARE( readFile( newPath ), fresh.bytes );
    QCOMPARE( entries( output ), QStringList( {"new.pdf", "old.pdf"} ) );
    int const calls = snapshotCalls;
    pipeline->snapshot( layout() ); WAIT_FOR( snapshots.size() == 4 );
    pipeline->snapshot( layout() );
    QCOMPARE( snapshotCalls, calls + 1 );
    pipeline->cancel();
  }
  void reentrantReadyRetiresExport()
  {
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QTemporaryDir output; QVERIFY( output.isValid() );
    QString const destination = output.filePath( "retired.pdf" );
    QVERIFY( writeFile( destination, "existing" ) );
    QSignalSpy cancelled( &pipeline, &PrintPipeline::cancelled );
    connect( &pipeline, &PrintPipeline::ready, &pipeline, [&] { pipeline.cancel(); } );
    pipeline.exportPdf( destination, layout() );
    WAIT_FOR( cancelled.size() == 1 );
    QCOMPARE( readFile( destination ), QByteArray( "existing" ) );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Cancelled );
    QCOMPARE( entries( output ), QStringList( {"retired.pdf"} ) );
  }
  void independentExportRetirement()
  {
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QTemporaryDir output; QVERIFY( output.isValid() );
    QString const destination = output.filePath( "retired.pdf" );
    QVERIFY( writeFile( destination, "existing" ) );
    QSignalSpy ready( &pipeline, &PrintPipeline::ready );
    connect( &pipeline, &PrintPipeline::ready, &pipeline, [&] { ++pipeline.exportOperation; } );
    pipeline.exportPdf( destination, layout() );
    WAIT_FOR( ready.size() == 1 );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Ready );
    QCOMPARE( pipeline.document->pageCount(), 2 );
    QCOMPARE( readFile( destination ), QByteArray( "existing" ) );
    QCOMPARE( entries( output ), QStringList( {"retired.pdf"} ) );
  }
  void printerFailure_data()
  {
    QTest::addColumn<QString>( "failure" );
    for( auto const * mode : {"render", "begin", "new-page", "end", "commit", "null-printer"} )
      QTest::newRow( mode ) << QString::fromLatin1( mode );
  }
  void printerFailure()
  {
    QFETCH( QString, failure );
    Browser b; QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge ); QVERIFY( awaitReady( pipeline ) );
    QByteArray const frozenHash = hash( pipeline.bytes );
    QTemporaryDir output; QVERIFY( output.isValid() );
    observedDestination = output.filePath( "destination.pdf" );
    QVERIFY( writeFile( observedDestination, "existing" ) );
    QVERIFY( writeFile( output.filePath( ".goldendict-print-foreign.pdf" ), "foreign" ) );
    TestPrinter printer; configurePrinter( printer, observedDestination );
    QSignalSpy failed( &pipeline, &PrintPipeline::failed );
    failRender = failure == "render"; failBegin = failure == "begin";
    failNewPage = failure == "new-page"; failEnd = failure == "end"; failCommit = failure == "commit";
    QVERIFY( !pipeline.paint( failure == "null-printer" ? nullptr : &printer ) );
    QCOMPARE( failed.count(), 1 ); QCOMPARE( pipeline.state(), PrintPipeline::State::Failed );
    QVERIFY( !failed.first().first().toString().isEmpty() );
    QCOMPARE( hash( pipeline.bytes ), frozenHash );
    QVERIFY( pipeline.buffer->isOpen() ); QCOMPARE( pipeline.document->pageCount(), 2 );
    QCOMPARE( readFile( observedDestination ), QByteArray( "existing" ) );
    QCOMPARE( printer.outputFileName(), observedDestination );
    QCOMPARE( readFile( output.filePath( ".goldendict-print-foreign.pdf" ) ), QByteArray( "foreign" ) );
    QCOMPARE( entries( output ), QStringList( {".goldendict-print-foreign.pdf", "destination.pdf"} ) );
    if( failure == "commit" ) { QVERIFY( fallbackDisabled ); QCOMPARE( commits, 1 ); }
    if( failure == "render" ) { QCOMPARE( begins, 1 ); QCOMPARE( ends, 1 ); }
    if( failure == "begin" ) { QCOMPARE( begins, 1 ); QVERIFY( renderSizes.isEmpty() ); QCOMPARE( ends, 0 ); }
    if( failure == "new-page" ) QCOMPARE( newPages, 1 );
    if( failure == "end" ) QCOMPARE( ends, 1 );
    QCOMPARE( snapshotCalls, 1 );
  }
  void previewFailureIsDeferred()
  {
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.load() );
    auto pipeline = std::make_unique<PrintPipeline>( &b.renderer, b.bridge );
    QVERIFY( awaitReady( *pipeline ) );
    QSignalSpy failed( pipeline.get(), &PrintPipeline::failed );
    QTemporaryDir output; QVERIFY( output.isValid() );
    TestPrinter printer; configurePrinter( printer, output.filePath( "unused.pdf" ) );
    failRender = true;
    connect( pipeline.get(), &PrintPipeline::failed, this, [&] { pipeline.reset(); } );
    pipeline->preview( &printer );
    WAIT_FOR( !pipeline );
    QCOMPARE( failed.count(), 1 );
    QCOMPARE( renderedPages, QList<int>( {0} ) );
    QCOMPARE( snapshotCalls, 1 );
    QVERIFY( entries( output ).isEmpty() );
  }
  void exportAtomicity_data()
  {
    QTest::addColumn<bool>( "commitFailure" ); QTest::addColumn<bool>( "existing" );
    QTest::newRow( "replace" ) << false << true;
    QTest::newRow( "create" ) << false << false;
    QTest::newRow( "failed-replace" ) << true << true;
    QTest::newRow( "failed-create" ) << true << false;
  }
  void exportAtomicity()
  {
    QFETCH( bool, commitFailure ); QFETCH( bool, existing );
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QTemporaryDir output; QVERIFY( output.isValid() );
    observedDestination = output.filePath( "export.pdf" );
    if( existing ) QVERIFY( writeFile( observedDestination, "existing" ) );
    QVERIFY( writeFile( output.filePath( "foreign" ), "foreign" ) );
    failCommit = commitFailure;
    QSignalSpy ready( &pipeline, &PrintPipeline::ready ); QSignalSpy failed( &pipeline, &PrintPipeline::failed );
    pipeline.exportPdf( observedDestination, layout() );
    WAIT_FOR( commits == 1 || !failed.isEmpty() );
    QCOMPARE( commits, 1 ); QVERIFY( fallbackDisabled );
    QCOMPARE( beforeCommit, existing ? QByteArray( "existing" ) : QByteArray() );
    QCOMPARE( ready.size(), 1 ); QCOMPARE( failed.size(), commitFailure ? 1 : 0 );
    QCOMPARE( readFile( output.filePath( "foreign" ) ), QByteArray( "foreign" ) );
    if( commitFailure )
    {
      QCOMPARE( pipeline.state(), PrintPipeline::State::Failed );
      QCOMPARE( QFileInfo::exists( observedDestination ), existing );
      QCOMPARE( readFile( observedDestination ), existing ? QByteArray( "existing" ) : QByteArray() );
    }
    else
    {
      QCOMPARE( pipeline.state(), PrintPipeline::State::Ready );
      QCOMPARE( readFile( observedDestination ), pipeline.bytes );
      QPdfDocument exported;
      QCOMPARE( exported.load( observedDestination ), QPdfDocument::Error::None );
      QCOMPARE( exported.pageCount(), 2 );
      QVERIFY( exported.getAllText( 1 ).text().contains( "snapshot-page-two" ) );
    }
    QCOMPARE( entries( output ), !commitFailure || existing ? QStringList( {"export.pdf", "foreign"} ) : QStringList( {"foreign"} ) );
  }
  void exportOpenFailure()
  {
    QDeadlineTimer deadline( BudgetMs );
    Browser b; QVERIFY( b.load() );
    PrintPipeline pipeline( &b.renderer, b.bridge );
    QTemporaryDir output; QVERIFY( output.isValid() );
    QSignalSpy failed( &pipeline, &PrintPipeline::failed );
    pipeline.exportPdf( output.filePath( "absent/export.pdf" ), layout() );
    WAIT_FOR( failed.count() == 1 );
    QCOMPARE( pipeline.state(), PrintPipeline::State::Failed );
    QVERIFY( entries( output ).isEmpty() );
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
