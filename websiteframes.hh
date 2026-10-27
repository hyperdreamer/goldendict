/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#ifndef GOLDENDICT_WEBSITEFRAMES_HH_INCLUDED
#define GOLDENDICT_WEBSITEFRAMES_HH_INCLUDED

#include <QObject>
#include <QPointF>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <functional>
#include <memory>

class QWebEnginePage;
class ArticleBridge;

struct WebsiteFrameKey
{
  QVector<int> childPath;
  QStringList namePath;
  QString docId;
  int generation = 0;
};

/// Borrows the page and bridge. Native frame values are used only after checking
/// their complete child path and the current document's independently read nonce.
class WebsiteFrames : public QObject
{
  Q_OBJECT
public:
  WebsiteFrames( QWebEnginePage *, ArticleBridge *, QObject * = nullptr );
  ~WebsiteFrames() override;
  void reset( int generation );
  void discover();
  void wordAtPoint( QPointF const &, std::function<void( QString const & )> );
public slots:
  void receive( QVariantMap const & );
signals:
  void frameText( QString const &, QString const &, bool contextMenu );
  void frameUnavailable( QString const &, QString const & );
private:
  struct State;
  std::unique_ptr<State> d;
};
#endif
