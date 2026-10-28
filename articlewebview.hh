/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#ifndef GOLDENDICT_ARTICLEWEBVIEW_HH_INCLUDED
#define GOLDENDICT_ARTICLEWEBVIEW_HH_INCLUDED

#include <QPointer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include "articleurls.hh"

class ArticleBridge;
class QContextMenuEvent;
class QMouseEvent;
namespace Config { struct Class; }

class ArticleWebPage : public QWebEnginePage
{
  Q_OBJECT
public:
  ArticleWebPage( QWebEngineProfile * profile, QObject * parent = nullptr );
  ~ArticleWebPage() override;
signals:
  void userLink( QUrl const & url, bool newTab );
  void navigationRequested( QUrl const & url, NavigationKind kind );
protected:
  bool acceptNavigationRequest( QUrl const & url, NavigationType type, bool isMainFrame ) override;
  QWebEnginePage * createWindow( WebWindowType type ) override;
private:
  friend class ArticleWebView;
  friend class ArticleBridgeTest;
  bool navigationPermit = true, controlledNavigation = false;
};

/// Owns its configured page and bridge; borrows the controller's shared profile.
class ArticleWebView : public QWebEngineView
{
  Q_OBJECT
public:
  explicit ArticleWebView( QWidget * parent = nullptr );
  ~ArticleWebView() override;
  void configure( QWebEngineProfile * profile, Config::Class * config );
  void setSelectionBySingleClick( bool enabled );
  bool isMidButtonPressed() const noexcept;
  /// Modifiers of the latest mouse press: Chromium's navigation callback reports
  /// no modifier state, so the click itself is the authority for Alt/Ctrl/Shift.
  Qt::KeyboardModifiers pressedModifiers() const noexcept;
  void triggerPageAction( QWebEnginePage::WebAction action, bool checked = false );
  // Controlled entry points install the next generation before asking Chromium
  // to navigate. The ArticleView history coordinator can capture state first.
  void load( QUrl const & url );
  void navigate( QUrl const & url, NavigationKind kind );
  void setHtml( QString const & html, QUrl const & baseUrl = QUrl() );
signals:
  void doubleClicked( QPoint position );
  void userLink( QUrl const & url, bool newTab );
  void navigationRequested( QUrl const & url, NavigationKind kind );
protected:
  bool event( QEvent * event ) override;
  bool eventFilter( QObject * watched, QEvent * event ) override;
  void focusInEvent( QFocusEvent * event ) override;
  void wheelEvent( QWheelEvent * event ) override;
  void contextMenuEvent( QContextMenuEvent * event ) override;
private:
  friend class ArticleBridge;
  friend class ArticleBridgeTest;
  QPointer<ArticleWebPage> ownedPage;
  QPointer<ArticleBridge> bridge;
  QPointer<QWebEngineView> inspector;
  Config::Class * config = nullptr;
  bool midButtonPressed = false, selectionBySingleClick = false, navigated = false;
  bool syntheticClick = false;
  Qt::KeyboardModifiers pressModifiers;
  struct Context
  {
    QPoint position;
    QString selectedText, linkText;
    QUrl linkUrl, mediaUrl;
    int generation = 0;
  } context;
  void installPage( QWebEngineProfile * profile );
  void recreatePage( ArticleBridge * exhausted );
  bool prepareNavigation();
  void watchChildren( QObject * object );
};
#endif
