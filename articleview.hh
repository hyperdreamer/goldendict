/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */
#ifndef __ARTICLEVIEW_HH_INCLUDED__
#define __ARTICLEVIEW_HH_INCLUDED__

#include <QAction>
#include <QFrame>
#include <QMap>
#include <QSet>
#include <QUrl>
#include <functional>
#include <list>
#include <memory>
#include "article_netmgr.hh"
#include "articlehistory.hh"
#include "audioplayerinterface.hh"
#include "instances.hh"
#include "groupcombobox.hh"
#include "ui_articleview.h"

class QWebEngineProfile;
class ArticleBridge;
class ResourceToSaveHandler;

/// GUI owner of the article document, asynchronous operations and indexed history.
class ArticleView: public QFrame
{
  Q_OBJECT
public:
  static constexpr int CaptureDeadlineMs = 500;
  static constexpr int CaptureDebounceMs = 100;
  using Contexts = QMap<QString, QString>;
  ArticleView( QWidget * parent, QWebEngineProfile & profile,
               ArticleNetworkAccessManager &, AudioPlayerPtr const &,
               std::vector<sptr<Dictionary::Class>> const & allDictionaries,
               Instances::Groups const &, bool popupView, Config::Class const & cfg,
               QAction & openSearchAction, QAction * dictionaryBarToggled = nullptr,
               GroupComboBox const * groupComboBox = nullptr );
  ~ArticleView() override;
  QSize minimumSizeHint() const override;
  void setGroupComboBox( GroupComboBox const * );
  static QString scrollToFromDictionaryId( QString const & );
  void showDefinition( Config::InputPhrase const &, unsigned group,
                       QString const & scrollTo = {}, Contexts const & contexts = {} );
  void showDefinition( QString const &, unsigned group,
                       QString const & scrollTo = {}, Contexts const & contexts = {} );
  void showDefinition( QString const &, QStringList const & dictionaryIds,
                       FtsHighlightSpec const &, unsigned group, bool ignoreDiacritics );
  void showAnticipation();
  void openLink( QUrl const &, QUrl const & referrer,
                 QString const & scrollTo = {}, Contexts const & contexts = {} );
  void updateMutedContents();
  bool canGoBack();
  bool canGoForward();
  void setSelectionBySingleClick( bool );
  void reload();
  bool hasSound();
  void playSound();
  void setZoomFactor( qreal factor ) { ui.definition->setZoomFactor( factor ); }
  void toHtml( std::function<void(QString const &)> callback );
  void captureState( std::function<void(ArticleState const &)> callback );
  QString getTitle();
  Config::InputPhrase getPhrase() const;
  bool closeSearch();
  bool isSearchOpened();
  void jumpToDictionary( QString const &, bool force );
  QStringList getArticlesList();
  QString getCurrentArticle();
  QString getActiveArticleId();
  ResourceToSaveHandler * saveResource( QUrl const &, QString const & fileName );
  ResourceToSaveHandler * saveResource( QUrl const &, QUrl const & ref, QString const & fileName );
  unsigned getViewGroup();
signals:
  void iconChanged( ArticleView *, QIcon const & );
  void titleChanged( ArticleView *, QString const & );
  void pageLoaded( ArticleView * );
  void articlesChanged( QStringList const & dictionaryIds );
  void soundAvailabilityChanged( bool available );
  void openLinkInNewTab( QUrl const &, QUrl const &, QString const &, ArticleView::Contexts const & );
  void showDefinitionInNewTab( QString const &, unsigned, QString const &, ArticleView::Contexts const & );
  void sendWordToHistory( QString const & );
  void typingEvent( QString const & );
  void statusBarMessage( QString const &, int timeout = 0, QPixmap const & = QPixmap() );
  void showDictsPane();
  void activeArticleChanged( ArticleView const *, QString const & );
  void forceAddWordToHistory( QString const & );
  void closePopupMenu();
  void setExpandMode( bool );
  void sendWordToInputLine( QString const & );
  void storeResourceSavePath( QString const & );
  void zoomIn();
  void zoomOut();
public slots:
  void back();
  void forward();
  void focus() { ui.definition->setFocus( Qt::ShortcutFocusReason ); }
  void on_searchPrevious_clicked();
  void on_searchNext_clicked();
  bool handleF3( QObject *, QEvent * );
  void receiveExpandOptionalParts( bool );
  void switchExpandOptionalParts();
  void selectCurrentArticle();
private slots:
  void loadFinished( bool );
  void handleTitleChanged( QString const & );
  void handleUrlChanged( QUrl const & );
  void linkClicked( QUrl const & );
  void linkHovered( QString const & );
  void contextMenuRequested( QPoint const & );
  void resourceDownloadFinished();
  void pasteTriggered();
  void moveOneArticleUp();
  void moveOneArticleDown();
  void openSearch();
  void on_searchText_textEdited();
  void on_searchText_returnPressed();
  void on_searchCloseButton_clicked();
  void on_searchCaseSensitive_clicked();
  void on_highlightAllButton_clicked();
  void on_ftsSearchPrevious_clicked();
  void on_ftsSearchNext_clicked();
  void doubleClicked( QPoint, Qt::KeyboardModifiers );
  void audioPlayerError( QString const & );
  void copyAsText();
  void inspect();
private:
  friend class ArticleBridgeTest;
  static constexpr int InteractionDeadlineMs = 500;
  struct State;
  struct Capture;
  struct Hit;
  void requestHit( QPoint const &, std::function<void(QVariantMap const &)> );
  void finishHit( std::shared_ptr<Hit> const &, QVariantMap const & );
  bool isFramedArticle( QString const & ) const;
  std::unique_ptr<State> d;
  ArticleNetworkAccessManager & articleNetMgr;
  AudioPlayerPtr const & audioPlayer;
  std::vector<sptr<Dictionary::Class>> const & allDictionaries;
  Instances::Groups const & groups;
  bool popupView;
  Config::Class const & cfg;
  Ui::ArticleView ui;
  QAction pasteAction, articleUpAction, articleDownAction, goBackAction, goForwardAction,
          selectCurrentArticleAction, copyAsTextAction, inspectAction;
  QAction & openSearchAction;
  QAction * dictionaryBarToggled;
  GroupComboBox const * groupComboBox;
  bool searchIsOpened = false, expandOptionalParts = false, ftsSearchIsOpened = false;
  int ftsPosition = 0, ftsCount = 0;
  std::list<sptr<Dictionary::DataRequest>> resourceDownloadRequests;
  QUrl resourceDownloadUrl;
  QSet<QString> desktopOpenedTempFiles;
  unsigned getGroup( QUrl const & ) const;
  QString getMutedForGroup( unsigned );
  bool setCurrentArticle( QString const &, bool moveToIt = false );
  void bindDocument();
  void requestNavigation( QUrl const &, NavigationKind );
  void performNavigation();
  void finishLoad();
  void finishCapture( std::shared_ptr<Capture> const &, ArticleState const &, bool timedOut );
  void updateReadingState( QVariantMap const & );
  void clearDocumentCaches();
  void publishPolicy( QUrl const & );
  void resolveFileUrls();
  void run( QString const &, std::function<void(QVariant const &)> = {} );
  bool isExternalLink( QUrl const & ) const;
  void tryMangleWebsiteClickedUrl( QUrl &, Contexts & );
  void load( QUrl const & );
  void cleanupTemp();
  bool eventFilter( QObject *, QEvent * ) override;
  void performFindOperation( bool restart, bool backwards, bool checkHighlight = false );
  void highlightFTSResults( int attempt = 0 );
  void performFtsFindOperation( bool backwards );
  void reloadStyleSheet();
protected:
  void showEvent( QShowEvent * ) override;
};

class ResourceToSaveHandler: public QObject
{
  Q_OBJECT
public:
  explicit ResourceToSaveHandler( ArticleView *, QString const & fileName );
  void addRequest( sptr<Dictionary::DataRequest> );
  bool isEmpty() { return downloadRequests.empty(); }
  ~ResourceToSaveHandler() override;
signals:
  void done();
  void statusBarMessage( QString const &, int timeout = 0, QPixmap const & = QPixmap() );
public slots:
  void downloadFinished();
private:
  std::list<sptr<Dictionary::DataRequest>> downloadRequests;
  QString fileName;
  bool alreadyDone = false;
};
#endif
