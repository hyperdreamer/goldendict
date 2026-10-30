/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#ifndef GOLDENDICT_PRINTPIPELINE_HH_INCLUDED
#define GOLDENDICT_PRINTPIPELINE_HH_INCLUDED

#include <QByteArray>
#include <QObject>
#include <QPageLayout>
#include <QPointer>
#include <QTimer>
#include <QWebEngineFrame>
#include <QVector>
#include <memory>

class ArticleBridge;
class QBuffer;
class QPdfDocument;
class QPrinter;
class QPrintPreviewDialog;
class QWebEnginePage;
class QWebEngineView;

struct PrintPipelinePolicy
{
  static constexpr int generationTimeoutMs = 30000;
  // This timer only rechecks document/frame state; elapsed time never releases
  // the Ready barrier. Events also trigger checks without waiting for a pulse.
  static constexpr int readinessCheckIntervalMs = 250;
};

/// Captures the selected view once, then borrows printers only for an action.
/// The immutable PDF outlives every synchronous paint and its preview dialog.
class PrintPipeline : public QObject
{
  Q_OBJECT
public:
  enum class State { Idle, Generating, LoadingPdf, Ready, Previewing, Printing, Failed, Cancelled };
  Q_ENUM( State )
  PrintPipeline( QWebEngineView *, ArticleBridge *, QObject * = nullptr );
  ~PrintPipeline() override;
  State state() const noexcept;
  void snapshot( QPageLayout const & );
  void preview( QPrinter * );
  bool paint( QPrinter * );
  void exportPdf( QString const &, QPageLayout const & );
  void cancel() noexcept;

signals:
  void ready( int pageCount );
  void failed( QString const & );
  void cancelled( QString const & );

private:
  friend class PrintPipelineTest;
  QPointer<QWebEngineView> view;
  QPointer<QWebEnginePage> page;
  QPointer<ArticleBridge> bridge;
  QPointer<QPrintPreviewDialog> dialog;
  State currentState = State::Idle;
  quint64 operation = 0, exportOperation = 0, readinessOperation = 0;
  int generation = 0;
  bool checking = false, snapshotRequested = false;
  QString exportDestination;
  QPageLayout snapshotLayout;
  QTimer generationTimer, readinessTimer;
  // Destruction is explicitly ordered: dialog, document, buffer, bytes.
  QByteArray bytes;
  std::unique_ptr<QBuffer> buffer;
  std::unique_ptr<QPdfDocument> document;

  bool generating() const noexcept;
  bool current( quint64 ) const noexcept;
  void beginSnapshot( QPageLayout const &, QString const & = {} );
  void checkReadiness();
  void verifyReadiness( quint64, quint64, QVector<QPair<QWebEngineFrame, QString>> const & );
  void requestSnapshot( quint64 );
  void loadSnapshot( quint64, QByteArray const & );
  void pdfStatusChanged( quint64 );
  bool publish( QString const &, QByteArray const & );
  void clearSnapshot();
  void fail( QString const &, bool retainSnapshot = false );
  void cancelWithReason( QString const & ) noexcept;
};
#endif
