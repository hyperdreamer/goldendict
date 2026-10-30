/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#ifndef GOLDENDICT_ARTICLEBRIDGE_HH_INCLUDED
#define GOLDENDICT_ARTICLEBRIDGE_HH_INCLUDED

#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QVariant>
#include <QWebEngineFrame>
#include <functional>
#include <memory>
#include <optional>

class QWebChannel;
class QWebEnginePage;
class ArticleWebView;

/// One document's channel and serial asynchronous JavaScript queue. The page is
/// borrowed; the view destroys it before destroying this QObject.
class ArticleBridge : public QObject
{
  Q_OBJECT
public:
  using Result = std::function<void( QVariant const & )>;
  ArticleBridge( QWebEnginePage * page, QObject * viewParent );
  ~ArticleBridge() override;
  int generation() const noexcept;
  void reset();
  void run( QString const & script, Result callback = {} );
  void runInFrame( QWebEngineFrame const & frame, QString const & script, Result callback = {} );
  void retireInFlightFrame();
public slots:
  void onJsActiveArticleChanged( QString id, int generation );
  void onJsReadingState( QVariantMap state, int generation );
  void onJsFrameEvent( QVariantMap event, int generation );
signals:
  void activeArticleChanged( QString const & id );
  void readingStateChanged( QVariantMap const & state );
  void frameEvent( QVariantMap const & event );
  void generationChanged( int generation );
  void bridgeReady( int generation );
private:
  friend class ArticleView;
  friend class ArticleBridgeTest;
  friend class ArticleWebView;
  enum class State { Installing, ChannelPending, Ready, Closed };
  struct Operation
  {
    int generation;
    quint64 number;
    std::optional<QWebEngineFrame> frame;
    QString script;
    Result callback;
    bool completed = false;
  };
  QPointer<QWebEnginePage> page;
  QWebChannel * channel;
  State state = State::Installing;
  int currentGeneration = 1;
  quint64 nextOperation = 0;
  QQueue<std::shared_ptr<Operation>> queue;
  std::shared_ptr<Operation> inFlight;
  QStringList installedScripts, pendingActive, articles;
  QString currentArticle;
  QVariantMap readingState;
  bool hasAudio = false;
  bool advanceGeneration();
  void installScripts();
  void retire();
  void enqueue( std::optional<QWebEngineFrame> frame, QString const & script, Result callback, int deadlineMs = 0 );
  void dispatch();
  void complete( std::shared_ptr<Operation> const & operation, QVariant const & value );
  void retireOperation( std::shared_ptr<Operation> const & operation );
  bool accepts( int generation ) const noexcept;
  void setActive( QString const & id );
};
#endif
