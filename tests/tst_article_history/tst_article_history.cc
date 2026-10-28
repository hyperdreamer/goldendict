/* Indexed application history: the engine's own item indices, absent state
 * versus a zero offset, and the lifecycle of a slot when the engine truncates or
 * replaces the document that used to occupy an index.
 *
 * The value module is WebEngine-free; this suite supplies a real QWebEnginePage
 * so every index it feeds the module is one the engine actually reported, and
 * Coordinator probes below also observe ArticleView's own map calls while its
 * real renderer captures and restores those entries.
 */

#include <QtTest>
#include <QApplication>
#include <QDeadlineTimer>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QTimer>
#include <QPointer>

#include <memory>

#include "articlehistory.hh"
#include "articlebridge.hh"
#include "article_maker.hh"
#include "article_netmgr.hh"
#include "articleprofile.hh"
#include "articleschemes.hh"
#include "articleview.hh"
#include "articlewebview.hh"
#include "audioplayerinterface.hh"
#include "support/dictionary_stub.hh"
#include "support/http_fixture.hh"
#include "articleurls.hh"
#include "support/fixtures.hh"

namespace
{

/// Per-step budget of the real-page walk. Every wait below is state-based -- it
/// returns as soon as the engine reports the expected item or emits its terminal
/// load signal -- so the budget only bounds a wedged renderer.
constexpr int StepBudgetMs = 15000;

/// Every document of the real-page walk is served from this one URL, so the
/// engine itself produces duplicate URLs at different item indices.
QUrl const documentUrl( QStringLiteral( "https://a.test/history" ) );

QUrl const lookupUrl( QStringLiteral( "gdlookup://localhost/A?group=1" ) );

QString articleDocument( QString const & marker )
{
  return QStringLiteral( "<!doctype html><meta charset='utf-8'><title>%1</title>"
                         "<body style='margin:0;height:4000px'><p id='%1'>%1</p></body>" )
    .arg( marker );
}

ArticleState state( QString const & article, double sx, double sy, bool restoreScroll = true )
{
  ArticleState value;
  value.currentArticle = article;
  value.sx = sx;
  value.sy = sy;
  value.restoreScroll = restoreScroll;
  return value;
}

/// Reads document.title of the live document, so a mirrored index is backed by
/// the document the engine is actually showing. An empty result means the
/// renderer did not answer inside the budget; the helper records that failure.
QString liveTitle( QWebEnginePage & page )
{
  struct Answer
  {
    bool done = false;
    QString value;
  };

  auto answer = std::make_shared< Answer >();

  page.runJavaScript( QStringLiteral( "document.title" ),
                      [answer]( QVariant const & value ) {
                        answer->value = value.toString();
                        answer->done = true;
                      } );

  if( !QTest::qWaitFor( [answer] { return answer->done; }, StepBudgetMs ) )
  {
    QTest::qFail( "the live document never answered a title read", __FILE__, __LINE__ );
    return {};
  }

  return answer->value;
}

QVariant evaluate( QWebEnginePage * page, QString const & script )
{
  struct Answer { bool done = false; QVariant value; };
  auto answer = std::make_shared< Answer >();
  page->runJavaScript( script, [answer]( QVariant const & value ) { answer->value = value; answer->done = true; } );
  if( !QTest::qWaitFor( [answer] { return answer->done; }, StepBudgetMs ) )
    QTest::qFail( "real history renderer did not return a JavaScript result", __FILE__, __LINE__ );
  return answer->value;
}

class Player final: public AudioPlayerInterface
{
public:
  QString play( const char *, int ) override { return {}; }
  void stop() override {}
};

struct Fixture
{
  Config::Class config;
  sptr< TestSupport::FixtureDictionary > a{ new TestSupport::FixtureDictionary };
  sptr< TestSupport::FixtureDictionary > b{ new TestSupport::FixtureDictionary( "dict-b", "Dictionary B" ) };
  std::vector< sptr< Dictionary::Class > > dictionaries{ a, b };
  Instances::Groups groups;
  ArticleMaker maker{ dictionaries, groups, {}, {} };
  ArticleNetworkAccessManager resources{ nullptr, dictionaries, maker,
    config.preferences.disallowContentFromOtherSites, config.preferences.hideGoldenDictHeader };
  std::unique_ptr< ArticleProfileController > profile;
  AudioPlayerPtr player{ new Player };
  QAction search;
  Fixture()
  {
    config.preferences.maxNetworkCacheSize = 0;
    config.preferences.disallowContentFromOtherSites = false;
    Instances::Group group( "Fixture group" );
    group.id = 17; group.name = "Fixture group"; group.dictionaries = dictionaries; groups.push_back( group );
    profile = std::make_unique< ArticleProfileController >( config, resources, nullptr );
    a->setArticleBody( "<p id='fixture-headword'>fixture-headword</p><div style='height:2400px'></div>" );
    b->setArticleBody( "<p>second dictionary</p><div style='height:2400px'></div>" );
  }
  std::unique_ptr< ArticleView > view()
  {
    auto result = std::make_unique< ArticleView >( nullptr, *profile->profile(), resources, player, dictionaries,
                                                  groups, false, config, search, nullptr, nullptr );
    QWebEngineScript observer;
    observer.setName( "history-restore-observer" );
    observer.setInjectionPoint( QWebEngineScript::DocumentCreation );
    observer.setWorldId( QWebEngineScript::MainWorld );
    observer.setSourceCode( "history.scrollRestoration='manual';window.__historyRestoreCalls=0;"
      "const nativeHistoryScroll=window.scrollTo.bind(window);window.scrollTo=(...args)=>{"
      "if(window.__gdViewRestoreGeneration===window.__gdDocumentGeneration)++window.__historyRestoreCalls;"
      "return nativeHistoryScroll(...args)};" );
    result->findChild< ArticleWebView * >()->page()->scripts().insert( observer );
    result->resize( 800, 600 ); result->show(); return result;
  }
};

bool loaded( ArticleView & view, QSignalSpy const & pages, int before, QString const & word )
{
  return pages.size() > before && view.getPhrase().phrase == word && !view.getArticlesList().isEmpty();
}

struct CapturedEntry { int index, engineIndex; ArticleState value; };
struct CommittedEntry { int index; QUrl url; NavigationKind kind; };
struct RestoredEntry { int index; bool present; ArticleState value; };
QPointer< QWebEnginePage > watchedPage;
QVector< CapturedEntry > capturedEntries;
QVector< CommittedEntry > committedEntries;
QVector< RestoredEntry > restoredEntries;
bool holdCaptures = false;
struct HeldCapture { std::function< void( QVariant const & ) > callback; QVariant value; };
QVector< HeldCapture > heldCaptures;

} // namespace

void realSetCurrent( ArticleHistory *, int, ArticleState const & )
  asm( "__real__ZN14ArticleHistory10setCurrentEiRK12ArticleState" );
void wrapSetCurrent( ArticleHistory *, int, ArticleState const & )
  asm( "__wrap__ZN14ArticleHistory10setCurrentEiRK12ArticleState" );
void wrapSetCurrent( ArticleHistory * history, int index, ArticleState const & value )
{
  if( watchedPage ) capturedEntries.append( { index, watchedPage->history()->currentItemIndex(), value } );
  realSetCurrent( history, index, value );
}
void realCommit( ArticleHistory *, QUrl const &, int, NavigationKind )
  asm( "__real__ZN14ArticleHistory6commitERK4QUrli14NavigationKind" );
void wrapCommit( ArticleHistory *, QUrl const &, int, NavigationKind )
  asm( "__wrap__ZN14ArticleHistory6commitERK4QUrli14NavigationKind" );
void wrapCommit( ArticleHistory * history, QUrl const & url, int index, NavigationKind kind )
{
  if( watchedPage ) committedEntries.append( { index, url, kind } );
  realCommit( history, url, index, kind );
}
bool realStateFor( ArticleHistory const *, int, ArticleState * )
  asm( "__real__ZNK14ArticleHistory8stateForEiP12ArticleState" );
bool wrapStateFor( ArticleHistory const *, int, ArticleState * )
  asm( "__wrap__ZNK14ArticleHistory8stateForEiP12ArticleState" );
bool wrapStateFor( ArticleHistory const * history, int index, ArticleState * value )
{
  bool const present = realStateFor( history, index, value );
  if( watchedPage ) restoredEntries.append( { index, present, present && value ? *value : ArticleState{} } );
  return present;
}
void realJavaScript( QWebEnginePage *, QString const &, quint32, std::function< void( QVariant const & ) > const & )
  asm( "__real__ZN14QWebEnginePage13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE" );
void wrapJavaScript( QWebEnginePage *, QString const &, quint32, std::function< void( QVariant const & ) > const & )
  asm( "__wrap__ZN14QWebEnginePage13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE" );
void wrapJavaScript( QWebEnginePage * page, QString const & source, quint32 world,
                     std::function< void( QVariant const & ) > const & callback )
{
  if( holdCaptures && page == watchedPage && source.contains( QLatin1String( "gdCaptureState()" ) ) )
    realJavaScript( page, source, world, [callback]( QVariant const & value ) { heldCaptures.append( { callback, value } ); } );
  else realJavaScript( page, source, world, callback );
}

class ArticleHistoryTest: public QObject
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
    watchedPage.clear(); holdCaptures = false; heldCaptures.clear();
    capturedEntries.clear(); committedEntries.clear(); restoredEntries.clear();
  }

  void cleanupTestCase()
  {
    TestSupport::clearWatchdog();
  }

  /// The regression this module exists for: "no state recorded" and "recorded at
  /// the top" have to be different answers.
  void absentStateIsNotZeroOffsets()
  {
    ArticleHistory history;
    ArticleState result;
    const QUrl repeated( QStringLiteral( "gdlookup://localhost/A?group=1" ) );
    history.commit( repeated, 0, NavigationKind::NewLookup );
    QVERIFY( !history.stateFor( 0, &result ) );
    ArticleState zero;
    zero.currentArticle = QStringLiteral( "gdfrom-dict-a" );
    history.setCurrent( 0, zero );
    history.commit( repeated, 1, NavigationKind::NewLookup );
    QVERIFY( !history.stateFor( 1, &result ) );
    QVERIFY( history.stateFor( 0, &result ) );
    QCOMPARE( result.sx, 0.0 );
    QCOMPARE( result.sy, 0.0 );
    QCOMPARE( result.currentArticle, QStringLiteral( "gdfrom-dict-a" ) );
    history.truncateAfter( 0 );
    QVERIFY( !history.stateFor( 1, &result ) );
  }

  void absentStateLeavesTheCallersStateUntouched()
  {
    ArticleHistory history;
    history.commit( lookupUrl, 4, NavigationKind::NewLookup );

    ArticleState out = state( QStringLiteral( "sentinel" ), 3, 4, false );

    QVERIFY( !history.stateFor( 4, &out ) );      // slot without state
    QCOMPARE( out.currentArticle, QStringLiteral( "sentinel" ) );
    QCOMPARE( out.sx, 3.0 );
    QCOMPARE( out.sy, 4.0 );
    QVERIFY( !out.restoreScroll );

    QVERIFY( !history.stateFor( 9, &out ) );      // no slot at all
    QVERIFY( !history.stateFor( -1, &out ) );
    QVERIFY( !history.stateFor( 4, nullptr ) );

    // The engine reports -1 while it has no item at all; that is not a key, so
    // neither a commit nor a capture may create one.
    history.commit( lookupUrl, -1, NavigationKind::NewLookup );
    history.setCurrent( -1, state( QStringLiteral( "no-item" ), 1, 2 ) );
    QVERIFY( !history.stateFor( -1, &out ) );
    QVERIFY( !history.stateFor( -1, nullptr ) );

    history.setCurrent( 4, state( QStringLiteral( "gdfrom-dict-a" ), 0, 0 ) );
    QVERIFY( history.stateFor( 4, nullptr ) );    // the state exists; a null out is enough
  }

  /// The URL is metadata; the engine index is the key.
  void duplicateUrlsStayIndependentPerIndex()
  {
    ArticleHistory history;

    history.commit( lookupUrl, 0, NavigationKind::NewLookup );
    history.commit( lookupUrl, 1, NavigationKind::LinkClick );
    history.commit( lookupUrl, 2, NavigationKind::LinkClick );
    history.setCurrent( 0, state( QStringLiteral( "gdfrom-dict-a" ), 10, 20 ) );
    history.setCurrent( 2, state( QStringLiteral( "gdfrom-dict-b" ), 30, 40, false ) );

    ArticleState out;
    QVERIFY( history.stateFor( 0, &out ) );
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-a" ) );
    QCOMPARE( out.sx, 10.0 );
    QVERIFY( !history.stateFor( 1, &out ) );      // the duplicate URL shares nothing
    QVERIFY( history.stateFor( 2, &out ) );
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-b" ) );
    QCOMPARE( out.sy, 40.0 );
    QVERIFY( !out.restoreScroll );
  }

  void outgoingCaptureStaysOnItsIndex()
  {
    ArticleHistory history;
    history.commit( lookupUrl, 3, NavigationKind::NewLookup );
    history.setCurrent( 3, state( QStringLiteral( "gdfrom-dict-a" ), 5, 6 ) );

    // The view captured the document it leaves, then committed the document that
    // finished loading at the engine's next index.
    history.commit( lookupUrl, 4, NavigationKind::NewLookup );

    ArticleState out;
    QVERIFY( history.stateFor( 3, &out ) );
    QCOMPARE( out.sx, 5.0 );
    QCOMPARE( out.sy, 6.0 );
    QVERIFY( !history.stateFor( 4, &out ) );      // the new entry never inherited

    // A capture that arrives for an index the map does not hold is dropped: it
    // cannot create a slot the engine never committed.
    history.setCurrent( 5, state( QStringLiteral( "late-capture" ), 1, 2 ) );
    QVERIFY( !history.stateFor( 5, &out ) );
    QVERIFY( !history.stateFor( 5, nullptr ) );
  }

  void redirectCommitsOneFinalTarget()
  {
    ArticleHistory history;
    const QUrl finalTarget( QStringLiteral( "gdlookup://localhost/Final?group=1" ) );

    history.commit( lookupUrl, 0, NavigationKind::NewLookup );   // provisional load
    history.commit( finalTarget, 0, NavigationKind::Redirect );  // final URL, same item

    ArticleState out;
    QVERIFY( !history.stateFor( 0, &out ) );      // a redirect invents no state
    QVERIFY( !history.stateFor( 1, &out ) );

    history.setCurrent( 0, state( QStringLiteral( "gdfrom-dict-a" ), 11, 22 ) );
    history.commit( finalTarget, 0, NavigationKind::Redirect );

    QVERIFY( history.stateFor( 0, &out ) );       // the recorded state survives
    QCOMPARE( out.sx, 11.0 );
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-a" ) );
    QVERIFY( !history.stateFor( 1, &out ) );
  }

  void reloadKeepsIdentityAndReturnsToTop()
  {
    ArticleHistory history;
    history.commit( lookupUrl, 2, NavigationKind::NewLookup );

    ArticleState const reading = state( QStringLiteral( "gdfrom-dict-a" ), 120, 3400 );
    history.setCurrent( 2, reading );             // outgoing capture before the reload
    history.commit( lookupUrl, 2, NavigationKind::Reload );

    ArticleState out;
    QVERIFY( history.stateFor( 2, &out ) );
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-a" ) );  // identity kept
    QCOMPARE( out.sx, 0.0 );
    QCOMPARE( out.sy, 0.0 );
    QVERIFY( !out.restoreScroll );                // the reloaded document starts at the top

    // A later reading-state update records fresh offsets for a subsequent Back.
    history.setCurrent( 2, state( QStringLiteral( "gdfrom-dict-a" ), 15, 60 ) );
    QVERIFY( history.stateFor( 2, &out ) );
    QCOMPARE( out.sy, 60.0 );
    QVERIFY( out.restoreScroll );
  }

  void ordinaryCommitDoesNotResetRecordedState()
  {
    ArticleHistory history;
    history.commit( lookupUrl, 1, NavigationKind::NewLookup );
    history.setCurrent( 1, state( QStringLiteral( "gdfrom-dict-a" ), 7, 70 ) );

    history.commit( lookupUrl, 1, NavigationKind::LinkClick );
    history.commit( lookupUrl, 2, NavigationKind::NewLookup );

    ArticleState out;
    QVERIFY( history.stateFor( 1, &out ) );
    QCOMPARE( out.sx, 7.0 );
    QCOMPARE( out.sy, 70.0 );
    QVERIFY( out.restoreScroll );
    QVERIFY( !history.stateFor( 2, &out ) );
  }

  void backAndForwardPreserveBothSides()
  {
    ArticleHistory history;

    for( int index = 0; index < 3; ++index )
    {
      history.commit( lookupUrl, index, NavigationKind::NewLookup );
      history.setCurrent( index, state( QStringLiteral( "gdfrom-dict-%1" ).arg( index ), index * 10, index * 100 ) );
    }

    history.commit( lookupUrl, 1, NavigationKind::Back );  // arrival at the Back destination

    ArticleState out;
    for( int index = 0; index < 3; ++index )
    {
      QVERIFY2( history.stateFor( index, &out ),
                qPrintable( QStringLiteral( "Back dropped entry %1" ).arg( index ) ) );
      QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-%1" ).arg( index ) );
      QCOMPARE( out.sx, double( index * 10 ) );
    }

    history.commit( lookupUrl, 2, NavigationKind::Forward );

    for( int index = 0; index < 3; ++index )
      QVERIFY2( history.stateFor( index, &out ),
                qPrintable( QStringLiteral( "Forward dropped entry %1" ).arg( index ) ) );
  }

  void forwardTruncationReleasesTheReusedIndex()
  {
    ArticleHistory history;

    for( int index = 0; index < 3; ++index )
    {
      history.commit( lookupUrl, index, NavigationKind::NewLookup );
      history.setCurrent( index, state( QStringLiteral( "gdfrom-dict-%1" ).arg( index ), index + 1, index + 2 ) );
    }

    history.truncateAfter( 1 );

    ArticleState out;
    QVERIFY( history.stateFor( 0, &out ) );
    QVERIFY( history.stateFor( 1, &out ) );
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-1" ) );
    QVERIFY( !history.stateFor( 2, &out ) );      // the truncated forward entry is gone

    // The engine reuses that index for the document the new navigation commits.
    history.commit( lookupUrl, 2, NavigationKind::NewLookup );
    QVERIFY( !history.stateFor( 2, &out ) );      // and the slot starts absent
    QVERIFY( history.stateFor( 1, &out ) );
  }

  void resetDropsEverySlot()
  {
    ArticleHistory history;

    for( int index = 0; index < 3; ++index )
    {
      history.commit( lookupUrl, index, NavigationKind::NewLookup );
      history.setCurrent( index, state( QStringLiteral( "gdfrom-dict-%1" ).arg( index ), 1, 2 ) );
    }

    history.reset();

    ArticleState out;
    for( int index = 0; index < 3; ++index )
      QVERIFY2( !history.stateFor( index, &out ),
                qPrintable( QStringLiteral( "reset kept entry %1" ).arg( index ) ) );

    // The index space starts over: a commit after the clear is a fresh absent slot.
    history.commit( lookupUrl, 2, NavigationKind::NewLookup );
    QVERIFY( !history.stateFor( 2, &out ) );
  }

  /// The map never owns an engine item: the page that supplied the index can die
  /// first, and the recorded state is still readable and still detached.
  void replacedPageDoesNotOwnTheRecordedState()
  {
    QWebEngineProfile profile;
    ArticleHistory history;

    {
      std::unique_ptr< QWebEnginePage > page( new QWebEnginePage( &profile ) );
      QSignalSpy finished( page.get(), &QWebEnginePage::loadFinished );

      page->setHtml( articleDocument( QStringLiteral( "article-a" ) ), documentUrl );
      QVERIFY2( QTest::qWaitFor( [&] { return !finished.isEmpty(); }, StepBudgetMs ),
                "the page never finished its document" );
      QVERIFY( finished.constFirst().at( 0 ).toBool() );
      QCOMPARE( page->history()->currentItemIndex(), 0 );

      history.commit( page->url(), 0, NavigationKind::NewLookup );
      history.setCurrent( 0, state( QStringLiteral( "gdfrom-dict-a" ), 12, 34 ) );
    } // the page is destroyed while the history is still alive

    ArticleState out;
    QVERIFY( history.stateFor( 0, &out ) );
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-a" ) );
    QCOMPARE( out.sx, 12.0 );
    QCOMPARE( out.sy, 34.0 );

    history.reset();
    QVERIFY( !history.stateFor( 0, &out ) );
  }

  /// Observe the real coordinator's setCurrent and stateFor calls. No map is
  /// populated by this test: all captures and restores come from ArticleView.
  void realPageSaveOutgoingIndexMatchesEngine()
  {
    Fixture f;
    auto view = f.view();
    auto * page = view->findChild< ArticleWebView * >()->page();
    watchedPage = page;
    QSignalSpy pages( view.get(), &ArticleView::pageLoaded );
    QCOMPARE( page->history()->currentItemIndex(), -1 );
    QVERIFY( !page->settings()->testAttribute( QWebEngineSettings::BackForwardCacheEnabled ) );
    auto wait = [&]( int before, QString const & word ) {
      return QTest::qWaitFor( [&] { return loaded( *view, pages, before, word ); }, StepBudgetMs );
    };
    auto scroll = [&]( int offset ) {
      return QTest::qWaitFor( [&] {
        QString const expression = restoredEntries.isEmpty() || !restoredEntries.last().present ?
          QStringLiteral( "scrollY" ) : QStringLiteral( "window.__historyRestoreCalls>0?scrollY:-1" );
        return evaluate( page, expression ).toInt() == offset;
      }, StepBudgetMs );
    };
    view->showDefinition( "A", 17 ); QVERIFY( wait( 0, "A" ) );
    int const firstA = page->history()->currentItemIndex();
    QCOMPARE( firstA, 0 );
    QUrl const aUrl = page->url();
    evaluate( page, "history.scrollRestoration='manual';gdMakeArticleActive('dict-b');scrollTo(0,650)" );
    QVERIFY( scroll( 650 ) );
    int count = pages.size(); view->showDefinition( "B", 17 ); QVERIFY( wait( count, "B" ) );
    QCOMPARE( capturedEntries.last().index, firstA );
    QCOMPARE( capturedEntries.last().engineIndex, firstA );
    QCOMPARE( capturedEntries.last().value.sy, 650.0 );
    QCOMPARE( capturedEntries.last().value.currentArticle, QString( "gdfrom-dict-b" ) );
    int const bIndex = page->history()->currentItemIndex();
    QVERIFY( bIndex > firstA );
    QCOMPARE( restoredEntries.last().index, bIndex );
    QVERIFY( !restoredEntries.last().present );
    QVERIFY( scroll( 0 ) );
    evaluate( page, "history.scrollRestoration='manual'" );
    count = pages.size(); view->showDefinition( "A", 17 ); QVERIFY( wait( count, "A" ) );
    int const secondA = page->history()->currentItemIndex();
    QCOMPARE( page->url(), aUrl );
    QVERIFY( secondA > bIndex );
    QCOMPARE( capturedEntries.last().index, bIndex );
    QCOMPARE( capturedEntries.last().value.sy, 0.0 );
    QVERIFY( !restoredEntries.last().present );
    QVERIFY( scroll( 0 ) );
    evaluate( page, "history.scrollRestoration='manual';scrollTo(0,340)" );
    count = pages.size(); view->back(); QVERIFY( wait( count, "B" ) );
    QCOMPARE( page->history()->currentItemIndex(), bIndex );
    QVERIFY( restoredEntries.last().present );
    QCOMPARE( restoredEntries.last().value.sy, 0.0 ); // Recorded zero differs from fresh B above.
    QVERIFY( scroll( 0 ) );
    evaluate( page, "history.scrollRestoration='manual';scrollTo(0,380)" );
    count = pages.size(); view->back(); QVERIFY( wait( count, "A" ) );
    QCOMPARE( page->history()->currentItemIndex(), firstA );
    QVERIFY( scroll( 650 ) );
    QCOMPARE( evaluate( page, "gdCurrentArticle" ).toString(), QString( "gdfrom-dict-b" ) );
    count = pages.size(); view->forward(); QVERIFY( wait( count, "B" ) );
    QVERIFY( scroll( 380 ) );
    count = pages.size(); view->forward(); QVERIFY( wait( count, "A" ) );
    QCOMPARE( page->history()->currentItemIndex(), secondA );
    QVERIFY( scroll( 340 ) );
    count = pages.size(); view->reload(); QVERIFY( wait( count, "A" ) );
    QCOMPARE( page->history()->currentItemIndex(), secondA );
    QVERIFY( scroll( 0 ) );
    QVERIFY( !restoredEntries.last().value.restoreScroll );
    count = pages.size(); view->back(); QVERIFY( wait( count, "B" ) );
    QVERIFY( scroll( 380 ) );
    count = pages.size(); view->showDefinition( "C", 17 ); QVERIFY( wait( count, "C" ) );
    QVERIFY( !view->canGoForward() );
    QVERIFY( scroll( 0 ) );
    QVERIFY( !restoredEntries.last().present );
    int const fragmentIndex = page->history()->currentItemIndex(), commits = committedEntries.size();
    auto * bridge = view->findChild< ArticleBridge * >( "gdArticleBridge" );
    int const generation = bridge->generation();
    evaluate( page, "scrollTo(0,400)" );
    QUrl fragment = page->url(); fragment.setFragment( "fixture-headword" );
    view->openLink( fragment, page->url() );
    QVERIFY( QTest::qWaitFor( [&] { return evaluate( page, "scrollY" ).toInt() < 400; }, StepBudgetMs ) );
    QCOMPARE( page->history()->currentItemIndex(), fragmentIndex );
    QCOMPARE( committedEntries.size(), commits );
    QCOMPARE( bridge->generation(), generation );
    for( auto const & capture : capturedEntries ) QCOMPARE( capture.index, capture.engineIndex );
  }

  void realCoordinatorRedirectCommitsOneFinalItem()
  {
    TestSupport::HttpFixture server;
    QVERIFY2( server.start( QHostAddress::LocalHost, 0 ), qPrintable( server.errorString() ) );
    Fixture f; auto view = f.view(); auto * page = view->findChild< ArticleWebView * >()->page();
    watchedPage = page;
    QSignalSpy pages( view.get(), &ArticleView::pageLoaded );
    view->showDefinition( "A", 17 );
    QVERIFY( QTest::qWaitFor( [&] { return loaded( *view, pages, 0, "A" ); }, StepBudgetMs ) );
    int const oldIndex = page->history()->currentItemIndex(), commits = committedEntries.size();
    int const count = pages.size();
    auto * bridge = view->findChild< ArticleBridge * >( "gdArticleBridge" );
    int const generation = bridge->generation();
    page->load( server.url( "/redirect" ) );
    QVERIFY( QTest::qWaitFor( [&] { return pages.size() > count; }, StepBudgetMs ) );
    QCOMPARE( page->url(), server.url( "/dir/first.html" ) );
    QCOMPARE( page->history()->currentItemIndex(), oldIndex + 1 );
    QCOMPARE( bridge->generation(), generation + 1 );
    QCOMPARE( committedEntries.size(), commits + 1 );
    QCOMPARE( committedEntries.last().url, page->url() );
    QCOMPARE( committedEntries.last().index, page->history()->currentItemIndex() );
    QVERIFY( !restoredEntries.last().present );
  }

  void realCoordinatorBoundsLayoutImageAndFrameGrowth()
  {
    QDeadlineTimer deadline( StepBudgetMs );
    auto remaining = [&] { return int( qMax< qint64 >( 0, deadline.remainingTime() ) ); };
    Fixture f; auto view = f.view(); auto * page = view->findChild< ArticleWebView * >()->page();
    watchedPage = page;
    QSignalSpy pages( view.get(), &ArticleView::pageLoaded );
    view->showDefinition( "A", 17 );
    QVERIFY( QTest::qWaitFor( [&] { return loaded( *view, pages, 0, "A" ); }, remaining() ) );
    evaluate( page, "scrollTo(0,1650)" );
    int count = pages.size(); view->showDefinition( "B", 17 );
    QVERIFY( QTest::qWaitFor( [&] { return loaded( *view, pages, count, "B" ); }, remaining() ) );
    f.a->setResources( { { "growth.png", TestSupport::HttpFixture::pixelPng() } } );
    f.a->setArticleBody(
      "<p>fixture-headword</p><div id='growth'></div>"
      "<img id='growing-image' src='bres://dict-a/growth.png'>"
      "<iframe id='growing-frame' src='about:blank' style='height:10px'></iframe><script>"
      "window.__restores=0;window.__layoutFrames=0;"
      "const originalScroll=window.scrollTo.bind(window);window.scrollTo=(x,y)=>{"
      "if(y===1650)++window.__restores;originalScroll(x,y)};"
      "addEventListener('gd-article-ready',()=>requestAnimationFrame(function grow(){"
      "if(window.__gdViewRestoreGeneration&&!window.__restores)++window.__layoutFrames;"
      "window.__growthFrames=(window.__growthFrames||0)+1;"
      "document.getElementById('growth').style.height=(5000+window.__growthFrames*20)+'px';"
      "document.getElementById('growing-image').height=240+window.__growthFrames;"
      "document.getElementById('growing-frame').style.height=(320+window.__growthFrames)+'px';"
      "if(window.__growthFrames<60)requestAnimationFrame(grow);}));</script>" );
    f.b->setArticleBody( "<p>Second dictionary</p>" );
    count = pages.size(); view->back();
    QVERIFY( QTest::qWaitFor( [&] { return loaded( *view, pages, count, "A" ); }, remaining() ) );
    QVERIFY( QTest::qWaitFor( [&] { return evaluate( page, "window.__restores" ).toInt() == 1; }, remaining() ) );
    QCOMPARE( evaluate( page, "scrollY" ).toInt(), 1650 );
    QVERIFY( evaluate( page, "window.__layoutFrames" ).toInt() <= 12 );
    QVERIFY( evaluate( page, "document.getElementById('growing-image').height" ).toInt() > 240 );
    QVERIFY( evaluate( page, "document.getElementById('growing-frame').offsetHeight" ).toInt() > 320 );
    evaluate( page, "scrollTo(0,120);document.getElementById('growth').style.height='6000px';gdOnBridgeReady();"
                    "setTimeout(()=>window.__laterLayoutTurn=true,600)" );
    QVERIFY( QTest::qWaitFor( [&] { return evaluate( page, "window.__laterLayoutTurn===true" ).toBool(); }, remaining() ) );
    QCOMPARE( evaluate( page, "window.__restores" ).toInt(), 1 );
    QCOMPARE( evaluate( page, "scrollY" ).toInt(), 120 );
    count = pages.size(); view->showDefinition( "C", 17 );
    QVERIFY( QTest::qWaitFor( [&] { return loaded( *view, pages, count, "C" ); }, remaining() ) );
    f.a->setArticleBody( "<div style='height:5000px'>fixture-headword</div><script>"
      "window.__hiddenRestores=0;const nativeTimeout=window.setTimeout.bind(window);"
      "window.setTimeout=(callback,ms,...args)=>nativeTimeout(callback,ms===500?5000:ms,...args);"
      "const originalScroll=window.scrollTo.bind(window);window.scrollTo=(x,y)=>{"
      "++window.__hiddenRestores;originalScroll(x,y)};"
      "requestAnimationFrame(()=>window.__hiddenFrameRan=true);</script>" );
    view->hide();
    QVERIFY( QTest::qWaitFor( [&] { return !page->isVisible(); }, remaining() ) );
    count = pages.size(); view->back();
    QVERIFY( QTest::qWaitFor( [&] { return loaded( *view, pages, count, "A" ); }, remaining() ) );
    QVERIFY( QTest::qWaitFor( [&] { return evaluate( page, "window.__hiddenRestores" ).toInt() == 1; },
                            qMin( 1200, remaining() ) ) );
    QVERIFY( !evaluate( page, "window.__hiddenFrameRan===true" ).toBool() );
    QCOMPARE( evaluate( page, "scrollY" ).toInt(), 120 );
  }

  void realCoordinatorDeadlineUsesLatestStateAndRetiresLateResult()
  {
    Fixture f; auto view = f.view(); auto * page = view->findChild< ArticleWebView * >()->page();
    watchedPage = page;
    QSignalSpy pages( view.get(), &ArticleView::pageLoaded );
    view->showDefinition( "A", 17 );
    QVERIFY( QTest::qWaitFor( [&] { return loaded( *view, pages, 0, "A" ); }, StepBudgetMs ) );
    QCOMPARE( ArticleView::CaptureDeadlineMs, 500 );
    QCOMPARE( ArticleView::CaptureDebounceMs, 100 );
    auto * timer = view->findChild< QTimer * >( "gdCaptureDebounce" );
    QVERIFY( timer ); QCOMPARE( timer->interval(), ArticleView::CaptureDebounceMs );
    auto * bridge = view->findChild< ArticleBridge * >( "gdArticleBridge" );
    QSignalSpy reading( bridge, &ArticleBridge::readingStateChanged );
    evaluate( page, "history.scrollRestoration='manual';gdMakeArticleActive('dict-b');scrollTo(0,720)" );
    QVERIFY( QTest::qWaitFor( [&] { return !reading.isEmpty() && reading.last().at( 0 ).toMap().value( "sy" ).toInt() == 720 &&
                                         !timer->isActive(); }, StepBudgetMs ) );
    int const outgoing = page->history()->currentItemIndex();
    holdCaptures = true;
    int const count = pages.size();
    QElapsedTimer elapsed; elapsed.start();
    view->showDefinition( "B", 17 );
    QVERIFY( QTest::qWaitFor( [] { return heldCaptures.size() == 1; }, StepBudgetMs ) );
    QVERIFY( QTest::qWaitFor( [&] { return loaded( *view, pages, count, "B" ); }, StepBudgetMs ) );
    QVERIFY( elapsed.elapsed() >= ArticleView::CaptureDeadlineMs - 50 && elapsed.elapsed() < 2000 );
    QCOMPARE( capturedEntries.last().index, outgoing );
    QCOMPARE( capturedEntries.last().engineIndex, outgoing );
    QCOMPARE( capturedEntries.last().value.sy, 720.0 );
    QCOMPARE( capturedEntries.last().value.currentArticle, QString( "gdfrom-dict-b" ) );
    auto late = heldCaptures.takeFirst();
    auto fresh = std::make_shared< bool >( false );
    view->captureState( [fresh]( ArticleState const & ) { *fresh = true; } );
    QVERIFY( QTest::qWaitFor( [] { return heldCaptures.size() == 1; }, StepBudgetMs ) );
    bool tail = false;
    bridge->run( "42", [&]( QVariant const & value ) { tail = value.toInt() == 42; } );
    late.callback( late.value );
    QVERIFY( !*fresh && !tail );
    auto current = heldCaptures.takeFirst();
    current.callback( current.value );
    QVERIFY( QTest::qWaitFor( [&] { return *fresh && tail; }, StepBudgetMs ) );
  }

  /// A controlled real page: every index, count and truncation the module sees
  /// below is the engine's own, not a hand-written guess.
  void realEngineIndicesMirrorTheMap()
  {
    QWebEngineProfile profile;
    QWebEnginePage page( &profile );
    page.settings()->setAttribute( QWebEngineSettings::BackForwardCacheEnabled, false );

    QSignalSpy finished( &page, &QWebEnginePage::loadFinished );

    auto settle = [&]( int expectedCount, int expectedIndex ) {
      QDeadlineTimer const step( StepBudgetMs );
      return QTest::qWaitFor(
        [&] {
          return page.history()->count() == expectedCount && page.history()->currentItemIndex() == expectedIndex;
        },
        int( qMax< qint64 >( 0, step.remainingTime() ) ) );
    };

    auto load = [&]( QString const & marker, bool expected ) {
      finished.clear();
      page.setHtml( articleDocument( marker ), documentUrl );
      if( !QTest::qWaitFor( [&] { return !finished.isEmpty(); }, StepBudgetMs ) )
        return false;
      return finished.constFirst().at( 0 ).toBool() == expected;
    };

    auto reload = [&] {
      finished.clear();
      page.triggerAction( QWebEnginePage::Reload );
      return QTest::qWaitFor( [&] { return !finished.isEmpty(); }, StepBudgetMs );
    };

    auto goBack = [&] {
      finished.clear();
      page.history()->back();
      return QTest::qWaitFor( [&] { return !finished.isEmpty(); }, StepBudgetMs );
    };

    QCOMPARE( page.history()->count(), 0 );
    QCOMPARE( page.history()->currentItemIndex(), -1 );   // the engine has no item yet

    ArticleHistory history;
    ArticleState out;

    // 1. The first committed document, at the index the engine reports.
    QVERIFY2( load( QStringLiteral( "article-a" ), true ), "the first document did not load" );
    QVERIFY2( settle( 1, 0 ), "the engine did not settle on its first item" );
    QCOMPARE( liveTitle( page ), QStringLiteral( "article-a" ) );

    int const first = page.history()->currentItemIndex();
    history.commit( page.url(), first, NavigationKind::NewLookup );
    QVERIFY( !history.stateFor( first, &out ) );

    // The outgoing capture of the live document.
    history.setCurrent( first, state( QStringLiteral( "gdfrom-dict-a" ), 40, 1200 ) );

    // 2. A second document gets its own engine index even though the URL is the
    //    same one: the engine itself gives us duplicate URLs at distinct indices.
    QVERIFY2( load( QStringLiteral( "article-b" ), true ), "the second document did not load" );
    QVERIFY2( settle( 2, 1 ), "the engine did not append a second item" );
    QCOMPARE( liveTitle( page ), QStringLiteral( "article-b" ) );
    QCOMPARE( page.url(), documentUrl );

    int const second = page.history()->currentItemIndex();
    QCOMPARE( second, first + 1 );
    history.commit( page.url(), second, NavigationKind::NewLookup );
    QVERIFY( !history.stateFor( second, &out ) );         // never inherited the outgoing state
    QVERIFY( history.stateFor( first, &out ) );
    QCOMPARE( out.sx, 40.0 );
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-a" ) );

    // The reader stays in the second document for a while, so leaving it is what
    // gives that engine index its state.
    history.setCurrent( second, state( QStringLiteral( "gdfrom-dict-b" ), 25, 900 ) );

    // 3. A third document, then a real Back to the second one.
    QVERIFY2( load( QStringLiteral( "article-c" ), true ), "the third document did not load" );
    QVERIFY2( settle( 3, 2 ), "the engine did not append a third item" );
    int const third = page.history()->currentItemIndex();
    history.commit( page.url(), third, NavigationKind::NewLookup );
    history.setCurrent( third, state( QStringLiteral( "gdfrom-dict-c" ), 5, 6 ) );

    QVERIFY2( goBack(), "the engine never finished the Back navigation" );
    QCOMPARE( finished.constFirst().at( 0 ).toBool(), true );
    QVERIFY2( settle( 3, second ), "Back did not land on the second engine item" );
    QCOMPARE( liveTitle( page ), QStringLiteral( "article-b" ) );

    history.commit( page.url(), page.history()->currentItemIndex(), NavigationKind::Back );
    QVERIFY( history.stateFor( second, &out ) );          // the destination keeps its capture
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-b" ) );
    QCOMPARE( out.sx, 25.0 );
    QCOMPARE( out.sy, 900.0 );
    QVERIFY( history.stateFor( third, &out ) );           // the forward entry survives the Back
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-c" ) );

    // 4. A new document after that Back: the engine drops its forward entry and
    //    reuses the third index. The application truncates first, so the reused
    //    index must come back absent -- no stale state for a different document.
    history.truncateAfter( second );
    QVERIFY( !history.stateFor( third, &out ) );
    QVERIFY( history.stateFor( second, &out ) );

    QVERIFY2( load( QStringLiteral( "article-d" ), true ), "the document after Back did not load" );
    QVERIFY2( settle( 3, third ), "the engine did not reuse the truncated forward index" );
    QCOMPARE( page.history()->count(), 3 );
    QCOMPARE( liveTitle( page ), QStringLiteral( "article-d" ) );

    int const reused = page.history()->currentItemIndex();
    history.commit( page.url(), reused, NavigationKind::NewLookup );
    QVERIFY( !history.stateFor( reused, &out ) );
    QVERIFY( history.stateFor( second, &out ) );

    // 5. A real reload keeps the engine index and count; the entry keeps its
    //    identity and is restored from the top.
    history.setCurrent( reused, state( QStringLiteral( "gdfrom-dict-d" ), 70, 800 ) );
    QVERIFY2( reload(), "the engine never finished the reload" );
    QCOMPARE( finished.constFirst().at( 0 ).toBool(), true );
    QCOMPARE( page.history()->count(), 3 );
    QCOMPARE( page.history()->currentItemIndex(), reused );
    QCOMPARE( liveTitle( page ), QStringLiteral( "article-d" ) );

    history.commit( page.url(), reused, NavigationKind::Reload );
    QVERIFY( history.stateFor( reused, &out ) );
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-dict-d" ) );
    QCOMPARE( out.sx, 0.0 );
    QCOMPARE( out.sy, 0.0 );
    QVERIFY( !out.restoreScroll );

    // 6. A failed load still takes an engine index. Its application slot stays
    //    absent instead of inheriting the last successful document's state.
    //    The loopback port is bound and released first, so the connection is
    //    refused immediately instead of waiting for a network timeout.
    quint16 deadPort = 0;
    {
      QTcpServer probe;
      QVERIFY( probe.listen( QHostAddress::LocalHost, 0 ) );
      deadPort = probe.serverPort();
    }
    QVERIFY( deadPort != 0 );

    finished.clear();
    page.load( QUrl( QStringLiteral( "http://127.0.0.1:%1/" ).arg( deadPort ) ) );
    QVERIFY2( QTest::qWaitFor( [&] { return !finished.isEmpty(); }, StepBudgetMs ),
              "the failed load never reported a verdict" );
    QCOMPARE( finished.constFirst().at( 0 ).toBool(), false );
    QVERIFY2( settle( 4, 3 ), "the failed load did not take an engine item" );

    int const failed = page.history()->currentItemIndex();
    QCOMPARE( failed, 3 );
    history.commit( page.url(), failed, NavigationKind::NewLookup );
    QVERIFY( !history.stateFor( failed, &out ) );
    QVERIFY( history.stateFor( reused, &out ) );          // the successful entry is untouched
    QCOMPARE( out.sx, 0.0 );                              // (it is the reloaded top)
    QVERIFY( !out.restoreScroll );

    // 7. Only an actual engine history clear resets the map.
    page.history()->clear();
    history.reset();

    for( int index = 0; index < 4; ++index )
      QVERIFY2( !history.stateFor( index, &out ),
                qPrintable( QStringLiteral( "the history clear kept entry %1" ).arg( index ) ) );

    history.commit( page.url(), page.history()->currentItemIndex(), NavigationKind::NewLookup );
    QVERIFY( !history.stateFor( page.history()->currentItemIndex(), &out ) );
  }
};

int main( int argc, char ** argv )
{
  if( TestSupport::isWatchdogProbeProcess() )
    TestSupport::runWatchdogProbe();

  QTemporaryDir sandbox;
  if( !sandbox.isValid() )
    return 1;
  qputenv( "XDG_CACHE_HOME", sandbox.path().toUtf8() );
  qputenv( "XDG_DATA_HOME", sandbox.path().toUtf8() );

  registerArticleSchemes();
  QApplication app( argc, argv );
  QCoreApplication::setApplicationName( QStringLiteral( "GoldenDict" ) );
  QNetworkProxy::setApplicationProxy( QNetworkProxy::NoProxy );

  ArticleHistoryTest tests;
  return QTest::qExec( &tests, argc, argv );
}

#include "tst_article_history.moc"
