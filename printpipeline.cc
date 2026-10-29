/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#include "printpipeline.hh"
#include "articlebridge.hh"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QPaintEngine>
#include <QPainter>
#include <QPdfDocument>
#include <QPrinter>
#include <QPrintPreviewDialog>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QWebEngineFrame>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {
struct FrameState
{
  QWebEngineFrame frame;
  QVector<int> path;
  QString nonce;
};
QWebEngineFrame atPath( QWebEnginePage * page, QVector<int> const & path )
{
  auto frame = page->mainFrame();
  for( int index : path )
  {
    if( !frame.isValid() ) return {};
    auto const children = frame.children();
    if( index >= children.size() ) return {};
    frame = children.at( index );
  }
  return frame;
}
}

PrintPipeline::PrintPipeline( QWebEngineView * target, ArticleBridge * channel, QObject * parent ):
  QObject( parent ), view( target ), page( target ? target->page() : nullptr ), bridge( channel )
{
  generationTimer.setSingleShot( true );
  generationTimer.setTimerType( Qt::PreciseTimer );
  readinessTimer.setInterval( PrintPipelinePolicy::readinessCheckIntervalMs );
  connect( &generationTimer, &QTimer::timeout, this, [this] {
    if( generating() ) fail( QStringLiteral( "PDF generation timed out after 30 s." ) );
  } );
  connect( &readinessTimer, &QTimer::timeout, this, &PrintPipeline::checkReadiness );
  if( bridge )
  {
    connect( bridge, &ArticleBridge::generationChanged, this, [this]( int next ) {
      if( generating() && next != generation )
        cancelWithReason( QStringLiteral( "The article changed while generating its PDF." ) );
    } );
    connect( bridge, &ArticleBridge::bridgeReady, this, [this] { checkReadiness(); } );
    connect( bridge, &ArticleBridge::frameEvent, this, [this]( QVariantMap const & event ) {
      // A child navigation does not increment the top document generation.
      // Retire checks that may still contain that child's previous document.
      if( event.value( "gdFrameEvent" ).toString() == QLatin1String( "ready" ) &&
          currentState == State::Generating && !snapshotRequested )
      { ++readinessOperation; checking = false; }
      checkReadiness();
    } );
    connect( bridge, &QObject::destroyed, this, [this] {
      cancelWithReason( QStringLiteral( "The article was closed." ) );
    } );
  }
  if( view ) connect( view, &QObject::destroyed, this, [this] {
    cancelWithReason( QStringLiteral( "The article tab was closed." ) );
  } );
  if( page )
  {
    connect( page, &QObject::destroyed, this, [this] {
      cancelWithReason( QStringLiteral( "The article page was closed." ) );
    } );
    connect( page, &QWebEnginePage::loadFinished, this, [this]( bool ok ) {
      if( generating() && !ok ) fail( QStringLiteral( "The article failed to load." ) );
      else checkReadiness();
    } );
    connect( page, &QWebEnginePage::renderProcessTerminated, this, [this] {
      if( generating() ) fail( QStringLiteral( "The article renderer stopped during PDF generation." ) );
    } );
  }
}

PrintPipeline::~PrintPipeline()
{
  ++operation; ++exportOperation;
  generationTimer.stop(); readinessTimer.stop();
  clearSnapshot();
}
PrintPipeline::State PrintPipeline::state() const noexcept { return currentState; }
bool PrintPipeline::generating() const noexcept
{
  return currentState == State::Generating || currentState == State::LoadingPdf;
}
bool PrintPipeline::current( quint64 token ) const noexcept
{
  return operation == token && generating() && view && page && bridge &&
         view->page() == page && bridge->generation() == generation;
}
void PrintPipeline::clearSnapshot()
{
  delete dialog.data();
  dialog.clear();
  document.reset();
  buffer.reset();
  bytes.clear();
}
void PrintPipeline::fail( QString const & reason, bool retainSnapshot )
{
  if( currentState == State::Failed || currentState == State::Cancelled ) return;
  auto const previous = currentState;
  ++operation; ++exportOperation;
  generationTimer.stop(); readinessTimer.stop();
  checking = false;
  exportDestination.clear();
  currentState = State::Failed;
  if( retainSnapshot && dialog && previous == State::Printing )
  {
    // paintRequested is on Qt's preview rendering stack. Its dialog must not
    // be destroyed from inside that stack, including by a failed-signal slot.
    auto const token = operation;
    QMetaObject::invokeMethod( this, [this, token, reason] {
      if( token != operation || currentState != State::Failed ) return;
      delete dialog.data(); dialog.clear();
      emit failed( reason );
    }, Qt::QueuedConnection );
    return;
  }
  if( retainSnapshot ) { delete dialog.data(); dialog.clear(); }
  else clearSnapshot();
  qWarning().noquote() << "PrintPipeline: failed state=" << int( previous ) << ": " << reason;
  emit failed( reason );
}
void PrintPipeline::cancelWithReason( QString const & reason ) noexcept
{
  if( currentState == State::Idle || currentState == State::Cancelled ||
      ( currentState == State::Failed && !document ) ) return;
  ++operation; ++exportOperation;
  generationTimer.stop(); readinessTimer.stop();
  checking = false;
  exportDestination.clear();
  currentState = State::Cancelled;
  clearSnapshot();
  qWarning().noquote() << "PrintPipeline: cancelled: " << reason;
  emit cancelled( reason );
}
void PrintPipeline::cancel() noexcept
{
  cancelWithReason( QStringLiteral( "Printing was cancelled." ) );
}
void PrintPipeline::snapshot( QPageLayout const & layout )
{
  // The UI disables duplicate snapshot actions while one is being generated.
  if( generating() || currentState == State::Printing || currentState == State::Previewing ) return;
  beginSnapshot( layout );
}
void PrintPipeline::exportPdf( QString const & destination, QPageLayout const & layout )
{
  if( currentState == State::Printing || currentState == State::Previewing ) return;
  // A new export replaces its predecessor. Neither old PDF callbacks nor a
  // reentrant ready handler can publish the replaced destination.
  beginSnapshot( layout, destination );
  if( destination.isEmpty() ) fail( QStringLiteral( "No PDF destination was supplied." ) );
}
void PrintPipeline::beginSnapshot( QPageLayout const & layout, QString const & destination )
{
  ++operation; ++exportOperation;
  generationTimer.stop(); readinessTimer.stop();
  clearSnapshot();
  checking = false; snapshotRequested = false;
  snapshotLayout = layout;
  exportDestination = destination;
  currentState = State::Generating;
  if( !view || !page || !bridge || view->page() != page || !layout.isValid() )
  {
    fail( QStringLiteral( "The article or print layout is unavailable." ) );
    return;
  }
  generation = bridge->generation();
  generationTimer.start( PrintPipelinePolicy::generationTimeoutMs );
  readinessTimer.start();
  checkReadiness();
}

void PrintPipeline::checkReadiness()
{
  if( currentState != State::Generating || snapshotRequested || checking || !current( operation ) ) return;
  if( page->isLoading() ) return;
  struct Check
  {
    QVector<FrameState> frames;
    int remaining = 0;
    bool ready = true;
  };
  auto check = std::make_shared<Check>();
  std::function<bool(QWebEngineFrame, QVector<int>)> collect = [&]( QWebEngineFrame frame, QVector<int> path ) {
    if( !frame.isValid() || path.size() > 32 || check->frames.size() >= 1024 ) return false;
    check->frames.append( {frame, path, {}} );
    auto children = frame.children();
    for( int index = 0; index < children.size(); ++index )
    {
      auto childPath = path; childPath.append( index );
      if( !collect( children.at( index ), childPath ) ) return false;
    }
    return true;
  };
  if( !collect( page->mainFrame(), {} ) ) return;
  checking = true;
  check->remaining = check->frames.size();
  auto const token = operation;
  auto const checkToken = ++readinessOperation;
  QPointer<PrintPipeline> guard( this );
  for( int index = 0; index < check->frames.size(); ++index )
  {
    auto const & node = check->frames.at( index );
    // Read the DOM in each native frame's own world, including cross-origin
    // frames. WebsiteFrames' nonce/ready state and applied root height are
    // observed rather than trusting an unverified postMessage as a barrier.
    QString const script = QStringLiteral( R"JS(/*gd-print-ready*/(()=>{
      const f=window.__gdWebsiteFrame;
      if(document.readyState!=='complete'||(document.fonts&&document.fonts.status!=='loaded'))return false;
      if(f&&(f.generation!==%1||!f.ready))return false;
      if(window===window.top&&(!window.__gdChannelReady||window.__gdDocumentGeneration!==%1))return false;
      if(window.gdFrameChildren&&window===window.top) {
        for(const c of gdFrameChildren()) {
          const element=document.getElementById(c.name);
          const unavailable=element&&element.nextElementSibling?.classList.contains('gd-website-frame-unavailable');
          if(c.root&&!unavailable&&(!c.docId||!element||element.style.display!=='block'||
             !(parseFloat(element.style.height)>0)))return false;
        }
      }
      return window.frames.length===%2&&f?f.docId:null;
    })())JS" ).arg( generation ).arg( node.frame.children().size() );
    bridge->runInFrame( node.frame, script, [guard, token, checkToken, check, index]( QVariant const & value ) {
      if( !guard || !guard->current( token ) || checkToken != guard->readinessOperation ||
          guard->currentState != State::Generating ) return;
      check->frames[index].nonce = value.metaType().id() == QMetaType::QString ? value.toString() : QString();
      check->ready = check->ready && !check->frames.at( index ).nonce.isEmpty();
      if( --check->remaining != 0 ) return;
      guard->checking = false;
      if( !check->ready || guard->page->isLoading() ) return;
      QVector<QPair<QWebEngineFrame, QString>> documents;
      for( auto const & frame : check->frames )
      {
        if( !frame.frame.isValid() || atPath( guard->page, frame.path ) != frame.frame ) return;
        documents.append( {frame.frame, frame.nonce} );
      }
      guard->verifyReadiness( token, checkToken, documents );
    } );
  }
}
void PrintPipeline::verifyReadiness( quint64 token, quint64 checkToken,
                                    QVector<QPair<QWebEngineFrame, QString>> const & frames )
{
  // Native handles survive same-frame navigation. A second pass validates the
  // document nonces observed by the first pass before releasing the barrier.
  struct Verification { int remaining; bool ready = true; };
  auto verification = std::make_shared<Verification>();
  verification->remaining = frames.size();
  checking = true;
  QPointer<PrintPipeline> guard( this );
  for( auto const & frame : frames )
  {
    auto const native = frame.first;
    QString const nonce = frame.second;
    int const children = native.children().size();
    QString const script = QStringLiteral( R"JS(/*gd-print-verify*/(()=>{
      const f=window.__gdWebsiteFrame;
      return {docId:f&&f.docId,children:window.frames.length,ready:document.readyState==='complete'&&
        (!document.fonts||document.fonts.status==='loaded')&&f&&f.ready&&f.generation===%1};
    })())JS" ).arg( generation );
    bridge->runInFrame( native, script, [guard, token, checkToken, verification, native, nonce, children]( QVariant const & value ) {
      if( !guard || !guard->current( token ) || checkToken != guard->readinessOperation ||
          guard->currentState != State::Generating ) return;
      auto const state = value.toMap();
      verification->ready = verification->ready && native.isValid() &&
        state.value( "docId" ).toString() == nonce && state.value( "ready" ).toBool() &&
        state.value( "children" ).toInt() == children;
      if( --verification->remaining != 0 ) return;
      guard->checking = false;
      if( verification->ready && !guard->page->isLoading() ) guard->requestSnapshot( token );
    } );
  }
}
void PrintPipeline::requestSnapshot( quint64 token )
{
  if( !current( token ) || snapshotRequested || currentState != State::Generating ) return;
  snapshotRequested = true;
  readinessTimer.stop();
  QPointer<PrintPipeline> guard( this );
  view->printToPdf( [guard, token]( QByteArray const & result ) {
    if( !guard || !guard->current( token ) ) return;
    // WebEngine may invoke its last callback from the page destructor. Let
    // destruction guards cancel the operation before interpreting that result.
    QMetaObject::invokeMethod( guard, [guard, token, result] {
      if( guard && guard->current( token ) ) guard->loadSnapshot( token, result );
    }, Qt::QueuedConnection );
  }, snapshotLayout );
}
void PrintPipeline::loadSnapshot( quint64 token, QByteArray const & result )
{
  if( !current( token ) || currentState != State::Generating ) return;
  if( result.isEmpty() ) { fail( QStringLiteral( "The renderer returned an empty PDF." ) ); return; }
  currentState = State::LoadingPdf;
  bytes = result;
  buffer = std::make_unique<QBuffer>( &bytes );
  if( !buffer->open( QIODevice::ReadOnly ) )
  { fail( QStringLiteral( "Unable to open the PDF snapshot buffer." ) ); return; }
  document = std::make_unique<QPdfDocument>();
  connect( document.get(), &QPdfDocument::statusChanged, this, [this, token] {
    // load() can emit Error synchronously. Defer destruction until its stack
    // unwinds; the explicit inspection below handles immediate Ready/Error.
    QMetaObject::invokeMethod( this, [this, token] { pdfStatusChanged( token ); }, Qt::QueuedConnection );
  } );
  document->load( buffer.get() );
  pdfStatusChanged( token );
}
void PrintPipeline::pdfStatusChanged( quint64 token )
{
  if( !current( token ) || currentState != State::LoadingPdf || !document ) return;
  if( document->status() == QPdfDocument::Status::Error )
  { fail( QStringLiteral( "Unable to decode the PDF snapshot." ) ); return; }
  if( document->status() != QPdfDocument::Status::Ready ) return;
  int const pages = document->pageCount();
  if( pages <= 0 ) { fail( QStringLiteral( "The PDF snapshot has no pages." ) ); return; }
  generationTimer.stop(); readinessTimer.stop();
  currentState = State::Ready;
  auto const exportToken = exportOperation;
  QString const destination = exportDestination;
  exportDestination.clear();
  QPointer<PrintPipeline> guard( this );
  emit ready( pages );
  if( !guard || token != operation || exportToken != exportOperation || currentState != State::Ready ) return;
  if( !destination.isEmpty() ) publish( destination, bytes );
}

bool PrintPipeline::publish( QString const & destination, QByteArray const & data )
{
  QSaveFile output( destination );
  output.setDirectWriteFallback( false );
  if( !output.open( QIODevice::WriteOnly ) || output.write( data ) != data.size() || !output.commit() )
  {
    fail( QStringLiteral( "Unable to publish the PDF: %1" ).arg( output.errorString() ), true );
    return false;
  }
  return true;
}
void PrintPipeline::preview( QPrinter * printer )
{
  if( currentState != State::Ready || !document || document->pageCount() <= 0 ) return;
  if( !printer ) { fail( QStringLiteral( "No printer was supplied." ), true ); return; }
  auto * preview = new QPrintPreviewDialog( printer, view );
  dialog = preview;
  currentState = State::Previewing;
  auto const token = operation;
  preview->setAttribute( Qt::WA_DeleteOnClose );
  connect( preview, &QPrintPreviewDialog::paintRequested, this, [this, token]( QPrinter * target ) {
    if( token == operation && currentState == State::Previewing ) paint( target );
  } );
  connect( preview, &QObject::destroyed, this, [this, token] {
    if( token == operation && currentState == State::Previewing ) currentState = State::Ready;
  } );
  preview->open(); // Nonblocking: no nested event loop and no retained printer.
}

bool PrintPipeline::paint( QPrinter * printer )
{
  if( ( currentState != State::Ready && currentState != State::Previewing ) || !document ||
      document->status() != QPdfDocument::Status::Ready || document->pageCount() <= 0 ) return false;
  if( !printer ) { fail( QStringLiteral( "No printer was supplied." ), true ); return false; }
  State const previous = currentState;
  currentState = State::Printing;
  QString const destination = printer->outputFileName();
  std::unique_ptr<QTemporaryFile> staged;
  // Preview uses a picture engine, even for a PDF-format printer. Only the
  // actual PDF output engine writes a staged file for atomic publication.
  if( printer->outputFormat() == QPrinter::PdfFormat && printer->paintEngine()->type() == QPaintEngine::Pdf )
  {
    staged = std::make_unique<QTemporaryFile>( QDir( QFileInfo( destination ).absolutePath() )
      .filePath( QStringLiteral( ".goldendict-print-XXXXXX.pdf" ) ) );
    if( destination.isEmpty() || !staged->open() )
    { fail( QStringLiteral( "Unable to stage the printed PDF." ), true ); return false; }
    staged->close();
    printer->setOutputFileName( staged->fileName() );
  }
  printer->setFullPage( false );
  QRect const printable( QPoint(), printer->pageLayout().paintRectPixels( printer->resolution() ).size() );
  QPainter painter;
  QString error;
  if( printable.isEmpty() || !painter.begin( printer ) ) error = QStringLiteral( "Unable to begin printing." );
  else
  {
    int const count = document->pageCount();
    for( int index = 0; index < count; ++index )
    {
      QSizeF const points = document->pagePointSize( index );
      if( points.isEmpty() || !std::isfinite( points.width() ) || !std::isfinite( points.height() ) )
      { error = QStringLiteral( "The PDF page has invalid dimensions." ); break; }
      QSize const size = points.scaled( printable.size(), Qt::KeepAspectRatio ).toSize();
      QImage const image = document->render( index, size );
      if( image.isNull() ) { error = QStringLiteral( "Unable to render a PDF page." ); break; }
      if( index > 0 && !printer->newPage() )
      { error = QStringLiteral( "Unable to start the next printed page." ); break; }
      QPoint const origin( ( printable.width() - size.width() ) / 2,
                           ( printable.height() - size.height() ) / 2 );
      painter.drawImage( origin, image );
    }
    if( !painter.end() && error.isEmpty() ) error = QStringLiteral( "Unable to finish printing." );
  }
  if( staged ) printer->setOutputFileName( destination );
  if( !error.isEmpty() ) { fail( error, true ); return false; }
  currentState = previous;
  if( staged )
  {
    QFile output( staged->fileName() );
    if( !output.open( QIODevice::ReadOnly ) )
    { fail( QStringLiteral( "Unable to read the printed PDF." ), true ); return false; }
    QByteArray const painted = output.readAll();
    if( painted.isEmpty() )
    { fail( QStringLiteral( "The printer produced an empty PDF." ), true ); return false; }
    return publish( destination, painted );
  }
  return true;
}
