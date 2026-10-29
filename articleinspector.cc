#include "articleinspector.hh"
#include "articleprofile.hh"
#include <QCloseEvent>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <algorithm>

std::list<ArticleInspector *> ArticleInspector::openedInspectors;

ArticleInspector::ArticleInspector( QWebEnginePage * inspected, Config::Class * config, QWidget * parent ):
  QWidget( parent, Qt::Window ), cfg( config ), inspectedPage( inspected )
{
  auto controller = inspected ? qobject_cast<ArticleProfileController *>( inspected->profile()->parent() ) : nullptr;
  if( !cfg || !controller ) throw exInit();
  setObjectName( QStringLiteral( "gdArticleInspector" ) );
  setWindowTitle( tr( "Article inspector" ) );
  setAttribute( Qt::WA_DeleteOnClose );
  auto layout = new QVBoxLayout( this );
  layout->setContentsMargins( 0, 0, 0, 0 );
  auto view = new QWebEngineView( this );
  toolsPage = new QWebEnginePage( inspected->profile(), view );
  controller->attachPage( toolsPage ); // Policy/auth/settings precede DevTools loading.
  view->setPage( toolsPage );
  layout->addWidget( view );
  inspected->setDevToolsPage( toolsPage );
  connect( inspected, &QObject::destroyed, this, [this] { close(); } );
}

ArticleInspector::~ArticleInspector()
{
  beforeClosed();
  destroyPage();
}
QWebEnginePage * ArticleInspector::page() const noexcept { return toolsPage.data(); }
void ArticleInspector::destroyPage()
{
  if( inspectedPage && inspectedPage->devToolsPage() == toolsPage ) inspectedPage->setDevToolsPage( nullptr );
  delete toolsPage.data();
}
void ArticleInspector::beforeClosed()
{
  auto item = std::find( openedInspectors.begin(), openedInspectors.end(), this );
  if( item == openedInspectors.end() ) return;
  openedInspectors.erase( item );
  cfg->inspectorGeometry = saveGeometry();
}
void ArticleInspector::closeEvent( QCloseEvent * event )
{
  beforeClosed();
  // Close retires the page immediately, before DeferredDelete can race a reopen.
  destroyPage();
  QWidget::closeEvent( event );
}
void ArticleInspector::showEvent( QShowEvent * event )
{
  if( std::find( openedInspectors.begin(), openedInspectors.end(), this ) == openedInspectors.end() )
  {
    if( openedInspectors.empty() ) restoreGeometry( cfg->inspectorGeometry );
    else setGeometry( openedInspectors.front()->geometry() );
    openedInspectors.push_back( this );
  }
  QWidget::showEvent( event );
}
