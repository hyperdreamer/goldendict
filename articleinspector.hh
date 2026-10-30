#ifndef ARTICLEINSPECTOR_HH
#define ARTICLEINSPECTOR_HH

#include <QPointer>
#include <QWidget>
#include <list>
#include "config.hh"
#include "ex.hh"

class QCloseEvent;
class QShowEvent;
class QWebEnginePage;
class QWebEngineView;

/// Owns the DevTools page; borrows the inspected page and its shared profile.
class ArticleInspector : public QWidget
{
  Q_OBJECT
public:
  DEF_EX( exInit, "Article inspector failed to init", std::exception )
  explicit ArticleInspector( QWebEnginePage *, Config::Class *, QWidget * parent = nullptr );
  ~ArticleInspector() override;
  QWebEnginePage * page() const noexcept;
public slots:
  void beforeClosed();
protected:
  void showEvent( QShowEvent * ) override;
  void closeEvent( QCloseEvent * ) override;
private:
  Config::Class * cfg;
  QPointer<QWebEnginePage> inspectedPage, toolsPage;
  void destroyPage();
  static std::list<ArticleInspector *> openedInspectors;
};
#endif
