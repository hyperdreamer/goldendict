/* Indexed application history: the engine's own item indices, absent state
 * versus a zero offset, and the lifecycle of a slot when the engine truncates or
 * replaces the document that used to occupy an index.
 *
 * The value module is WebEngine-free; this suite supplies a real QWebEnginePage
 * so every index it feeds the module is one the engine actually reported, and
 * every truncation the module has to survive is one the engine actually did.
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

#include <memory>

#include "articlehistory.hh"
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

} // namespace

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

  /// Saving reads the outgoing document's engine item, not a URL-derived or dense
  /// application counter. The real page supplies both the zero item and the
  /// duplicate-URL successor used by the save/history boundary.
  void realPageSaveOutgoingIndexMatchesEngine()
  {
    QWebEngineProfile profile;
    QWebEnginePage page( &profile );
    page.settings()->setAttribute( QWebEngineSettings::BackForwardCacheEnabled, false );
    QSignalSpy finished( &page, &QWebEnginePage::loadFinished );

    auto load = [&]( QString const & marker ) {
      finished.clear();
      page.setHtml( articleDocument( marker ), documentUrl );
      if( !QTest::qWaitFor( [&] { return !finished.isEmpty(); }, StepBudgetMs ) ) return false;
      return finished.constFirst().at( 0 ).toBool();
    };

    ArticleHistory history;
    ArticleState out;
    QCOMPARE( page.history()->currentItemIndex(), -1 );
    QVERIFY( !history.stateFor( -1, &out ) );

    QVERIFY2( load( QStringLiteral( "save-a" ) ), "the save source document did not load" );
    QCOMPARE( page.history()->currentItemIndex(), 0 );
    int const saveOutgoing = page.history()->currentItemIndex();
    history.commit( page.url(), saveOutgoing, NavigationKind::NewLookup );
    history.setCurrent( saveOutgoing, state( QStringLiteral( "gdfrom-save-a" ), 0, 0 ) );

    QVERIFY( history.stateFor( saveOutgoing, &out ) );
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-save-a" ) );
    QCOMPARE( out.sx, 0.0 );
    QCOMPARE( out.sy, 0.0 );

    QVERIFY2( load( QStringLiteral( "save-b" ) ), "the next document did not load" );
    int const next = page.history()->currentItemIndex();
    QCOMPARE( next, saveOutgoing + 1 );
    QCOMPARE( page.url(), documentUrl );
    history.commit( page.url(), next, NavigationKind::NewLookup );
    QVERIFY( !history.stateFor( next, &out ) );
    QVERIFY( history.stateFor( saveOutgoing, &out ) );
    QCOMPARE( out.currentArticle, QStringLiteral( "gdfrom-save-a" ) );
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

  QApplication app( argc, argv );
  QCoreApplication::setApplicationName( QStringLiteral( "GoldenDict" ) );
  QNetworkProxy::setApplicationProxy( QNetworkProxy::NoProxy );

  ArticleHistoryTest tests;
  return QTest::qExec( &tests, argc, argv );
}

#include "tst_article_history.moc"
