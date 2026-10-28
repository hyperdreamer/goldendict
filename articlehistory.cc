/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * The indexed application history of the article view. Pure bookkeeping: integer
 * keys, no engine object, no navigation, no timer.
 */

#include "articlehistory.hh"

void ArticleHistory::reset()
{
  // Only an actual engine history clear lands here, together with the clear
  // itself, so there is nothing to keep.
  entries.clear();
}

void ArticleHistory::commit( QUrl const & url, int itemIndex, NavigationKind kind )
{
  // The engine has no item at a negative index; there is nothing to key.
  if( itemIndex < 0 )
    return;

  Entry & entry = entries[ itemIndex ];

  entry.index = itemIndex;
  entry.finalUrl = url;
  entry.navigationKind = kind;

  // The state is deliberately not touched by a commit:
  //
  // * a fresh entry keeps its absent state, because the document that just
  //   committed is not the document that was captured, and an unsuccessful load
  //   must not inherit the state of the document it replaced;
  // * an entry that already carries state keeps it, because the same engine index
  //   is only committed again for a reload or for the final target of a redirect,
  //   both of which belong to the same document -- except that a reload returns
  //   the reader to the top and therefore stops asking for a scroll restore.
  if( kind == NavigationKind::Reload && entry.hasState )
  {
    entry.state.sx = 0;
    entry.state.sy = 0;
    entry.state.restoreScroll = false;
  }
}

bool ArticleHistory::stateFor( int itemIndex, ArticleState * out ) const
{
  QMap< int, Entry >::const_iterator const entry = entries.constFind( itemIndex );

  if( entry == entries.constEnd() || !entry->hasState )
    return false; // Absent state; `out` is not written, not even with zeros.

  if( out )
    *out = entry->state;

  return true;
}

void ArticleHistory::setCurrent( int itemIndex, ArticleState const & state )
{
  QMap< int, Entry >::iterator const entry = entries.find( itemIndex );

  // A capture only ever refines an entry the engine actually committed. An
  // abandoned capture whose entry was truncated (or cleared) is dropped here
  // instead of recreating the key with state that no longer describes it.
  if( entry == entries.end() )
    return;

  entry->hasState = true;
  entry->state = state;
}

void ArticleHistory::truncateAfter( int itemIndex )
{
  for( QMap< int, Entry >::iterator entry = entries.upperBound( itemIndex ); entry != entries.end(); )
    entry = entries.erase( entry );
}
