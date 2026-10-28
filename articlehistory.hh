/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * The application's own history of the article documents the view has shown,
 * keyed by the engine's history item index.
 *
 * The engine owns its items. Nothing here stores, copies or borrows a
 * QWebEngineHistoryItem, and nothing here navigates: the view reports the index
 * the engine actually committed and the URL it ended up on, and later asks this
 * map what reading state belongs to an index it is about to show again. Because
 * the key is the engine's integer index instead of a URL, two documents that
 * share a URL stay two entries, and because a slot is created by the commit of a
 * successful document, a slot without state is a real answer -- "nothing was
 * captured for this document" -- and not the same answer as a recorded (0, 0)
 * offset, which is the top of a document the reader really was at.
 *
 * Asynchronous capture is bounded by ownership rather than by a timer here: a
 * capture that completes after its entry was truncated, or after the view
 * cleared the engine history, is dropped by setCurrent() instead of
 * resurrecting a stale key. The 500 ms capture deadline with the 100 ms
 * debounced fallback belongs to the navigation coordinator that performs the
 * capture, not to this value map.
 */

#ifndef GOLDENDICT_ARTICLEHISTORY_HH_INCLUDED
#define GOLDENDICT_ARTICLEHISTORY_HH_INCLUDED

#include <QMap>
#include <QString>
#include <QUrl>

#include "articleurls.hh"

/// The reading state recorded for one committed article document.
struct ArticleState
{
  /// Id of the article element the view last reported as active, for example
  /// `gdfrom-dict-a`. Empty when no article was active.
  QString currentArticle;

  /// View offsets of the reader. `0, 0` is the top of the document, which is a
  /// position the reader can be at; offsets arrive already validated as finite by
  /// the bridge that decodes them.
  double sx = 0, sy = 0;

  /// False when the document has to be shown at the top instead of at the
  /// recorded offsets -- a reload, or a document whose reader never moved.
  bool restoreScroll = true;
};

/// The state of every document the view committed, addressed by the engine's own
/// QWebEngineHistory item index.
class ArticleHistory
{
public:
  /// Forgets every entry.
  ///
  /// Only an actual engine history clear resets this map, in the same step as the
  /// engine clear itself. An ordinary load, a redirect, a reload and a bridge
  /// reset never do; those keep the entries they did not replace.
  void reset();

  /// Records the document the engine committed at `itemIndex`.
  ///
  /// `url` is metadata -- the final URL after redirects -- never the key, and the
  /// navigation kind is the one the coordinator recorded before it acted. A fresh
  /// index gains an entry without state: the document that just committed is not
  /// the document whose state was captured, so no state is copied into it and an
  /// unsuccessful load leaves its slot absent. An index that already carries a
  /// state keeps it, because the same index is only committed again for a reload
  /// of the same document or for the final target of its redirect. A reload is
  /// the one kind that changes the recorded state: it keeps the entry's identity
  /// and returns to the top, so the entry no longer asks for a scroll restore.
  void commit( QUrl const & url, int itemIndex, NavigationKind kind );

  /// Writes the state recorded at `itemIndex` to `out` and returns true.
  ///
  /// Returns false -- and leaves `out` untouched -- when the index has no entry or
  /// its entry has no state yet, so a caller can never mistake "no state was
  /// captured" for "restore the top". `out` may be null when only the answer
  /// matters.
  bool stateFor( int itemIndex, ArticleState * out ) const;

  /// Stores the captured state of the document at `itemIndex`.
  ///
  /// The index has to be one this map still holds: a capture that completes after
  /// its entry was truncated, or after the engine history was cleared and this map
  /// reset, is dropped rather than allowed to create the entry again with stale
  /// state. The view owns the in-flight capture and abandons it when it goes away;
  /// the map owns the key.
  void setCurrent( int itemIndex, ArticleState const & state );

  /// Drops every entry after `itemIndex`.
  ///
  /// Called with the outgoing index before a navigation that follows a Back, so
  /// the forward entries the engine is about to replace cannot survive as recorded
  /// state for the index the new document lands on. Entries at and before the
  /// outgoing index are kept, which is what makes a Back/Forward pair remember
  /// both sides.
  void truncateAfter( int itemIndex );

private:
  /// One entry, addressed by the engine's item index. `index` is carried in the
  /// entry as well because the map's key is the engine's index, not a dense
  /// counter, and a reader of the map should not have to assume they agree.
  struct Entry
  {
    int index = -1;
    QUrl finalUrl;
    NavigationKind navigationKind = NavigationKind::NewLookup;
    bool hasState = false;
    ArticleState state;
  };

  QMap< int, Entry > entries;
};

#endif
