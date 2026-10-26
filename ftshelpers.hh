#ifndef __FTSHELPERS_HH_INCLUDED__
#define __FTSHELPERS_HH_INCLUDED__

#include <QString>
#include <QRegularExpression>
#include "articleurls.hh"
#include <QRunnable>
#include <QSemaphore>
#include <QList>

#include "dictionary.hh"
#include "btreeidx.hh"
#include "fulltextsearch.hh"
#include "chunkedstorage.hh"
#include "folding.hh"
#include "wstring_qt.hh"

#include <string>

namespace FtsHelpers
{

enum
{
  FtsSignature = 0x58535446, // FTSX on little-endian, XSTF on big-endian
  CurrentFtsFormatVersion = 2 + BtreeIndexing::FormatVersion + Folding::Version,
};

#pragma pack(push,1)

struct FtsIdxHeader
{
  uint32_t signature; // First comes the signature, FTSX
  uint32_t formatVersion; // File format version
  uint32_t chunksOffset; // The offset to chunks' storage
  uint32_t indexBtreeMaxElements; // Two fields from IndexInfo
  uint32_t indexRootOffset;
  uint32_t wordCount; // Number of unique words this dictionary has
}
#ifndef _MSC_VER
__attribute__((packed))
#endif
;

#pragma pack(pop)

bool ftsIndexIsOldOrBad( std::string const & indexFile,
                         BtreeIndexing::BtreeDictionary * dict );

/// Compiles a search/highlight source pattern the way the article URL contract
/// does (articleurls.cc): a wildcard source is first converted with
/// UnanchoredWildcardConversion | NonPathWildcardConversion, so the result can
/// match inside an article and can cross a '/' in a dictionary headword. The
/// expression then keeps DotMatchesEverythingOption (QRegExp's '.' matched
/// '\n'), UseUnicodePropertiesOption (QRegExp's own word/digit tables),
/// MultilineOption and InvertedGreedinessOption (QRegExp's setMinimal(true));
/// `matchCase` is folded into the pattern options, never into the source text.
QRegularExpression compileHighlight( QString const & sourcePattern, bool wildcards,
                                     bool matchCase );

/// Normalizes the search line into index words and a highlight specification.
///
/// The specification carries the original source text, the wildcard flag and the
/// case folding separately from the compiled expression, because a
/// QRegularExpression cannot remember wildcard syntax. `hasCJK` reports CJK
/// text, which whole-word/plain-text modes refuse; `false` is returned when no
/// usable search can start (no index word, CJK in a word mode, or an invalid
/// user pattern in RegExp/Wildcards mode). On failure the specification is left
/// empty or holds the invalid expression, so a caller can display its error.
bool parseSearchString( QString const & str, QStringList & IndexWords,
                        QStringList & searchWords,
                        FtsHighlightSpec & searchRegExp, int searchMode,
                        bool matchCase,
                        int distanceBetweenWords,
                        bool & hasCJK );

void parseArticleForFts( uint32_t articleAddress, QString & articleText,
                         QMap< QString, QVector< uint32_t > > & words,
                         bool handleRoundBrackets = false );

void makeFTSIndex( BtreeIndexing::BtreeDictionary * dict, QAtomicInt & isCancelled );

bool isCJKChar( ushort ch );

class FTSResultsRequest;

class FTSResultsRequestRunnable : public QRunnable
{
  FTSResultsRequest & r;
  QSemaphore & hasExited;

public:

  FTSResultsRequestRunnable( FTSResultsRequest & r_,
                             QSemaphore & hasExited_ ) : r( r_ ),
                                                         hasExited( hasExited_ )
  {}

  ~FTSResultsRequestRunnable()
  {
    hasExited.release();
  }

  virtual void run();
};

class FTSResultsRequest : public Dictionary::DataRequest
{
  BtreeIndexing::BtreeDictionary & dict;

  QString searchString;
  int searchMode;
  bool matchCase;
  int distanceBetweenWords;
  int maxResults;
  bool hasCJK;
  bool ignoreWordsOrder;
  bool ignoreDiacritics;
  int wordsInIndex;

  QAtomicInt isCancelled;
  QSemaphore hasExited;

  QList< FTS::FtsHeadword > * foundHeadwords;

  void checkArticles( QVector< uint32_t > const & offsets,
                      QStringList const & words,
                      QRegularExpression const & searchRegexp = QRegularExpression() );

  void indexSearch( BtreeIndexing::BtreeIndex & ftsIndex,
                    sptr< ChunkedStorage::Reader > chunks,
                    QStringList & indexWords,
                    QStringList & searchWords );

  void combinedIndexSearch( BtreeIndexing::BtreeIndex & ftsIndex,
                            sptr< ChunkedStorage::Reader > chunks,
                            QStringList & indexWords,
                            QStringList & searchWords,
                            QRegularExpression const & regexp );

  void fullIndexSearch( BtreeIndexing::BtreeIndex & ftsIndex,
                        sptr< ChunkedStorage::Reader > chunks,
                        QStringList & indexWords,
                        QStringList & searchWords,
                        QRegularExpression const & regexp );

  void fullSearch( QStringList & searchWords, QRegularExpression const & regexp );

public:

  FTSResultsRequest( BtreeIndexing::BtreeDictionary & dict_, QString const & searchString_,
                     int searchMode_, bool matchCase_, int distanceBetweenWords_, int maxResults_,
                     bool ignoreWordsOrder_, bool ignoreDiacritics_, QThreadPool * ftsThreadPoolPtr ):
    dict( dict_ ),
    searchString( searchString_ ),
    searchMode( searchMode_ ),
    matchCase( matchCase_ ),
    distanceBetweenWords( distanceBetweenWords_ ),
    maxResults( maxResults_ ),
    hasCJK( false ),
    ignoreWordsOrder( ignoreWordsOrder_ ),
    ignoreDiacritics( ignoreDiacritics_ ),
    wordsInIndex( 0 )
  {
    if( ignoreDiacritics_ )
      searchString = gd::toQString( Folding::applyDiacriticsOnly( gd::toWString( searchString_ ) ) );

    foundHeadwords = new QList< FTS::FtsHeadword >;
    ftsThreadPoolPtr->start(
      new FTSResultsRequestRunnable( *this, hasExited ), -100 );
  }

  void run();

  virtual void cancel()
  {
    isCancelled.ref();
  }

  ~FTSResultsRequest()
  {
    isCancelled.ref();
    if( foundHeadwords )
      delete foundHeadwords;
    hasExited.acquire();
  }
};

} // namespace

#endif // __FTSHELPERS_HH_INCLUDED__
