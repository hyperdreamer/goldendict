/* This file is (c) 2014 Abs62
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "fulltextsearch.hh"
#include "ftshelpers.hh"
#include "wstring_qt.hh"
#include "file.hh"
#include "gddebug.hh"
#include "folding.hh"

#include <vector>
#include <string>

#include <QVector>

#include "wildcard.hh"

using std::vector;
using std::string;

DEF_EX( exUserAbort, "User abort", Dictionary::Ex )

namespace FtsHelpers
{

bool ftsIndexIsOldOrBad( string const & indexFile,
                         BtreeIndexing::BtreeDictionary * dict )
{
  File::Class idx( indexFile, "rb" );

  FtsIdxHeader header;

  return idx.readRecords( &header, sizeof( header ), 1 ) != 1 ||
         header.signature != FtsSignature ||
         header.formatVersion != CurrentFtsFormatVersion + dict->getFtsIndexVersion();
}

namespace
{

/// QRegExp classified text with Qt's own Unicode tables: a word character is a
/// letter, a number, any mark or the underscore — and nothing else.
///
/// PCRE's `\w` under UseUnicodePropertiesOption is not that table in two ways:
/// it omits the Mc/Me mark categories, and it includes every connector
/// punctuation character (U+203F, U+2040, U+2054, U+FE33, U+FE34, U+FE4D,
/// U+FE4E, U+FE4F, U+FF3F) rather than only the underscore. The class is
/// therefore spelled out for every path that replaced a QRegExp.
QString const wordCharacterClass = QStringLiteral( "[\\p{L}\\p{N}\\p{M}_]" );
QString const notWordCharacterClass = QStringLiteral( "[^\\p{L}\\p{N}\\p{M}_]" );

/// The article/index tokenizer is a different, older contract: it already used
/// QRegularExpression in the Qt5 build, where `[\w\p{M}]` matched both engines
/// identically. It keeps that spelling so this conversion does not silently
/// change an existing behaviour that no index-format bump would announce.
QString const articleWordCharacterClass = QStringLiteral( "[\\w\\p{M}]" );

/// Compiles one of the built-in classifiers. An invalid built-in is reported by
/// Regex::validated() and can never match, instead of silently becoming an
/// empty expression.
QRegularExpression builtIn( QString const & pattern,
                           QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption )
{
  return Regex::validated( pattern, options | QRegularExpression::UseUnicodePropertiesOption );
}

/// A run of non-word text, the separator QRegExp's "\\W+" used.
QRegularExpression const & nonWordRunRegExp()
{
  static QRegularExpression const expression =
    builtIn( notWordCharacterClass + QStringLiteral( "+" ) );
  return expression;
}

/// The index-word filter: FTS::MinimumWordSize characters or more of a word,
/// still measured in UTF-16 code units as before.
QRegularExpression const & indexWordRegExp()
{
  static QRegularExpression const expression =
    builtIn( wordCharacterClass + QStringLiteral( "{" )
             + QString::number( FTS::MinimumWordSize ) + QStringLiteral( ",}" ) );
  return expression;
}

/// Removes a user's symbol set such as [abc] from the text searched in the index.
QRegularExpression const & symbolSetRegExp()
{
  static QRegularExpression const expression =
    builtIn( QStringLiteral( "\\[[^\\]]+\\]" ), QRegularExpression::CaseInsensitiveOption );
  return expression;
}

/// Recognizes the RegExp-mode escape tokens the index build strips out. Exactly
/// four hex digits and exactly three octal digits are required, so a shorter
/// spelling stays part of the searched text (pinned by tst_regex).
QRegularExpression const & escapeTokenRegExp()
{
  static QRegularExpression const expression =
    builtIn( QStringLiteral( "\\\\[afnrtvdDwWsSbB]|\\\\x([0-9A-Fa-f]{4})|\\\\0([0-7]{3})" ) );
  return expression;
}

/// Word splits of an article. These are the older article/index separators: the
/// Qt5 build already split articles with `[^\\w\\p{M}]+` under Unicode
/// properties, so they keep that spelling — including PCRE's connector
/// punctuation — because switching them to the explicit query class would
/// re-index existing articles without a CurrentFtsFormatVersion change.
QRegularExpression const & splitWithoutBracketsRegExp()
{
  static QRegularExpression const expression =
    builtIn( QStringLiteral( "[^\\w\\p{M}]+" ) );
  return expression;
}

QRegularExpression const & splitWithBracketsRegExp()
{
  static QRegularExpression const expression =
    builtIn( QStringLiteral( "[^\\w\\(\\)\\p{M}]+" ) );
  return expression;
}

/// The DSL round-bracket grammar; callers pad the capture list to six entries,
/// the arity QRegExp reported.
QRegularExpression const & bracketRegExp()
{
  static QRegularExpression const expression = builtIn(
    QStringLiteral( "(\\(" ) + articleWordCharacterClass + QStringLiteral( "+\\)){0,1}(" )
      + articleWordCharacterClass + QStringLiteral( "+)(\\(" ) + articleWordCharacterClass
      + QStringLiteral( "+\\)){0,1}(" ) + articleWordCharacterClass + QStringLiteral( "+){0,1}(\\(" )
      + articleWordCharacterClass + QStringLiteral( "+\\)){0,1}" ) );
  return expression;
}

/// The highlight pattern for a word list. QRegExp's "\\b" was a boundary in
/// Qt's word table, which counts every mark as a word character; PCRE's "\\b"
/// follows `\w` and would accept a boundary before an Mc/Me mark, so the
/// boundaries are spelled with the same explicit class as the rest of the file.
/// The gap between words keeps the whole pattern minimal through
/// InvertedGreedinessOption (QRegExp's setMinimal(true)).
QString makeHiliteRegExpString( QStringList const & words, int searchMode, int distanceBetweenWords )
{
  QString const openWord = QStringLiteral( "(?:" ) + wordCharacterClass + QStringLiteral( "*)" );
  QString const leftBound = searchMode == FTS::WholeWords
                              ? QStringLiteral( "(?<!" ) + wordCharacterClass + QStringLiteral( ")" )
                              : openWord;
  QString const rightBound = searchMode == FTS::WholeWords
                               ? QStringLiteral( "(?!" ) + wordCharacterClass + QStringLiteral( ")" )
                               : openWord;

  QString stripWords( QStringLiteral( "(?:" ) + notWordCharacterClass + QStringLiteral( "+" )
                      + wordCharacterClass + QStringLiteral( "+){0," ) );
  if( distanceBetweenWords >= 0 )
    stripWords += QString::number( distanceBetweenWords );
  stripWords += QStringLiteral( "}" ) + notWordCharacterClass + QStringLiteral( "+" );

  QString searchString( "(" );

  for( int x = 0; x < words.size(); x++ )
  {
    if( x )
      searchString += stripWords;

    searchString += leftBound + words[ x ] + rightBound;
  }

  searchString += ")";
  return searchString;
}

} // namespace

QRegularExpression compileHighlight( QString const & sourcePattern, bool wildcards,
                                     bool matchCase )
{
  QString const pattern = wildcards
    ? QRegularExpression::wildcardToRegularExpression(
        sourcePattern,
        QRegularExpression::UnanchoredWildcardConversion | QRegularExpression::NonPathWildcardConversion )
    : sourcePattern;

  QRegularExpression::PatternOptions options = QRegularExpression::DotMatchesEverythingOption
                                              | QRegularExpression::UseUnicodePropertiesOption
                                              | QRegularExpression::MultilineOption
                                              | QRegularExpression::InvertedGreedinessOption;

  if( !matchCase )
    options |= QRegularExpression::CaseInsensitiveOption;

  return QRegularExpression( pattern, options );
}

bool parseSearchString( QString const & str, QStringList & indexWords,
                        QStringList & searchWords,
                        FtsHighlightSpec & searchRegExp, int searchMode,
                        bool matchCase,
                        int distanceBetweenWords,
                        bool & hasCJK )
{
  searchWords.clear();
  indexWords.clear();

  // A failed parse must never leave a previous search's pattern behind.
  searchRegExp = FtsHighlightSpec();

  hasCJK = false;
  for( int x = 0; x < str.size(); x++ )
    if( isCJKChar( str.at( x ).unicode() ) )
    {
      hasCJK = true;
      break;
    }

  if( searchMode == FTS::WholeWords || searchMode == FTS::PlainText )
  {
    if( hasCJK )
      return false;

    // Make words list for search in article text
    searchWords = str.normalized( QString::NormalizationForm_C )
                     .split( nonWordRunRegExp(), Qt::SkipEmptyParts );

    // Make words list for index search
    QStringList list = str.normalized( QString::NormalizationForm_C )
                          .toLower().split( nonWordRunRegExp(), Qt::SkipEmptyParts );
    indexWords = list.filter( indexWordRegExp() );
    indexWords.removeDuplicates();

    // Make regexp for results hilite

    QStringList allWords = str.split( nonWordRunRegExp(), Qt::SkipEmptyParts );
    QString searchString = makeHiliteRegExpString( allWords, searchMode, distanceBetweenWords );

    searchRegExp.sourcePattern = searchString;
    searchRegExp.wildcards = false;
    searchRegExp.matchCase = matchCase;
    searchRegExp.expression = compileHighlight( searchString, false, matchCase );

    return !indexWords.isEmpty();
  }
  else
  {
    // Make words list for index search

    QString tmp = str;

    // Remove RegExp commands
    if( searchMode == FTS::RegExp )
      tmp.replace( escapeTokenRegExp(), " " );

    // Remove all symbol sets
    tmp.replace( symbolSetRegExp(), " " );

    QStringList list = tmp.normalized( QString::NormalizationForm_C )
                          .toLower().split( nonWordRunRegExp(), Qt::SkipEmptyParts );

    if( hasCJK )
    {
      QStringList wordList, hieroglyphList;
      for( int i = 0; i < list.size(); i ++ )
      {
        QString word = list.at( i );

        // Check for CJK symbols in word
        bool parsed = false;
        QString hieroglyph;
        for( int x = 0; x < word.size(); x++ )
          if( isCJKChar( word.at( x ).unicode() ) )
          {
            parsed = true;
            hieroglyph.append( word[ x ] );

            if( QChar( word.at( x ) ).isHighSurrogate()
                &&  QChar( word[ x + 1 ] ).isLowSurrogate() )
              hieroglyph.append( word[ ++x ] );

            hieroglyphList.append( hieroglyph );
            hieroglyph.clear();
          }

        // If word don't contains CJK symbols put it in list as is
        if( !parsed )
          wordList.append( word );
      }

      indexWords = wordList.filter( indexWordRegExp() );
      indexWords.removeDuplicates();

      hieroglyphList.removeDuplicates();
      indexWords += hieroglyphList;
    }
    else
    {
      indexWords = list.filter( indexWordRegExp() );
      indexWords.removeDuplicates();
    }

    // The source text is carried verbatim; the wildcard flag selects the
    // conversion when the expression is compiled.
    searchRegExp.sourcePattern = str;
    searchRegExp.wildcards = ( searchMode == FTS::Wildcards );
    searchRegExp.matchCase = matchCase;
    searchRegExp.expression = compileHighlight( str, searchRegExp.wildcards, matchCase );

    // An invalid user pattern is a validation error, never a silent empty
    // pattern that would match every article.
    return searchRegExp.expression.isValid();
  }
}

void parseArticleForFts( uint32_t articleAddress, QString & articleText,
                         QMap< QString, QVector< uint32_t > > & words,
                         bool handleRoundBrackets )
{
  if( articleText.isEmpty() )
    return;

  QRegularExpression const & regBrackets = bracketRegExp();

  QStringList articleWords = articleText.normalized( QString::NormalizationForm_C )
                                        .split( handleRoundBrackets ? splitWithBracketsRegExp() : splitWithoutBracketsRegExp(),
                                                Qt::SkipEmptyParts );

  QSet< QString > setOfWords;
  setOfWords.reserve( articleWords.size() );

  for( int x = 0; x < articleWords.size(); x++ )
  {
    QString word = articleWords.at( x ).toLower();

    bool hasCJK = false;
    QString hieroglyph;

    // If word contains CJK symbols we add to index only these symbols separately
    for( int y = 0; y < word.size(); y++ )
      if( isCJKChar( word.at( y ).unicode() ) )
      {
        hasCJK = true;
        hieroglyph.append( word[ y ] );

        if( QChar( word.at( y ) ).isHighSurrogate()
            &&  QChar( word[ y + 1 ] ).isLowSurrogate() )
          hieroglyph.append( word[ ++y ] );

        if( !setOfWords.contains( hieroglyph ) )
        {
          setOfWords.insert( hieroglyph );
          words[ hieroglyph ].push_back( articleAddress );
        }

        hieroglyph.clear();
      }

    if( !hasCJK )
    {
      // Else we add word to index as is
      if( word.size() < FTS::MinimumWordSize )
        continue;

      if( handleRoundBrackets && ( word.indexOf( '(' ) >= 0 || word.indexOf( ')' ) >= 0 ) )
      {
        // Special handle for words with round brackets - DSL feature
        QStringList list;

        QStringList oldVariant = word.split( splitWithoutBracketsRegExp(), Qt::SkipEmptyParts );
        for( QStringList::iterator it = oldVariant.begin(); it != oldVariant.end(); ++it )
          if( it->size() >= FTS::MinimumWordSize && !list.contains( *it ) )
            list.append( *it );

        QRegularExpressionMatch match = regBrackets.match( word );
        if( match.hasMatch() )
        {
          QStringList parts = match.capturedTexts();
          // Add empty strings for the arity QRegExp reported
          for( int i = match.lastCapturedIndex() + 1; i < 6; i++ )
            parts.append( QString() );
          QString parsedWord = parts[ 2 ] + parts[ 4 ]; // Brackets removed

          if( parsedWord.size() >= FTS::MinimumWordSize && !list.contains( parsedWord ) )
            list.append( parsedWord );

          parsedWord = parts[ 1 ].remove( '(' ).remove( ')' )
                       + parts[ 2 ]
                       + parts[ 3 ].remove( '(' ).remove( ')' )
                       + parts[ 4 ]
                       + parts[ 5 ].remove( '(' ).remove( ')' ); // Brackets expansed

          if( parsedWord.size() >= FTS::MinimumWordSize && !list.contains( parsedWord ) )
            list.append( parsedWord );
        }

        for( QStringList::iterator it = list.begin(); it != list.end(); ++it )
        {
          if( !setOfWords.contains( *it ) )
          {
            setOfWords.insert( *it );
            words[ *it ].push_back( articleAddress );
          }
        }
      }
      else
      if( !setOfWords.contains( word ) )
      {
        setOfWords.insert( word );
        words[ word ].push_back( articleAddress );
      }
    }
  }
}

void makeFTSIndex( BtreeIndexing::BtreeDictionary * dict, QAtomicInt & isCancelled )
{
  Mutex::Lock _( dict->getFtsMutex() );

  if( isCancelled.loadAcquire() )
    throw exUserAbort();

  File::Class ftsIdx( dict->ftsIndexName(), "wb" );

  FtsIdxHeader ftsIdxHeader;
  memset( &ftsIdxHeader, 0, sizeof( ftsIdxHeader ) );

  // We write a dummy header first. At the end of the process the header
  // will be rewritten with the right values.

  ftsIdx.write( ftsIdxHeader );

  ChunkedStorage::Writer chunks( ftsIdx );

  BtreeIndexing::IndexedWords indexedWords;

  QSet< uint32_t > setOfOffsets;
  setOfOffsets.reserve( dict->getArticleCount() );

  dict->findArticleLinks( 0, &setOfOffsets, 0, &isCancelled );

  if( isCancelled.loadAcquire() )
    throw exUserAbort();

  QVector< uint32_t > offsets;
  offsets.resize( setOfOffsets.size() );
  uint32_t * ptr = &offsets.front();

  for( QSet< uint32_t >::ConstIterator it = setOfOffsets.constBegin();
       it != setOfOffsets.constEnd(); ++it )
  {
    *ptr = *it;
    ptr++;
  }

  // Free memory
  setOfOffsets.clear();
  setOfOffsets.squeeze();

  if( isCancelled.loadAcquire() )
    throw exUserAbort();

  dict->sortArticlesOffsetsForFTS( offsets, isCancelled );

  QMap< QString, QVector< uint32_t > > ftsWords;

  bool needHandleBrackets;
  {
    QString name = QString::fromUtf8( dict->getDictionaryFilenames()[ 0 ].c_str() ).toLower();
    needHandleBrackets = name.endsWith( ".dsl" ) || name.endsWith( "dsl.dz" );
  }

  // index articles for full-text search
  for( int i = 0; i < offsets.size(); i++ )
  {
    if( isCancelled.loadAcquire() )
      throw exUserAbort();

    QString headword, articleStr;

    dict->getArticleText( offsets.at( i ), headword, articleStr );

    parseArticleForFts( offsets.at( i ), articleStr, ftsWords, needHandleBrackets );
  }

  // Free memory
  offsets.clear();
  offsets.squeeze();

# define BUF_SIZE 20000
  QVector< QPair< gd::wstring, uint32_t > > wordsWithOffsets;
  wordsWithOffsets.reserve( BUF_SIZE );

  QMap< QString, QVector< uint32_t > >::iterator it = ftsWords.begin();
  while( it != ftsWords.end() )
  {
    if( isCancelled.loadAcquire() )
      throw exUserAbort();

    uint32_t offset = chunks.startNewBlock();
    uint32_t size = it.value().size();

    chunks.addToBlock( &size, sizeof(uint32_t) );
    chunks.addToBlock( it.value().data(), size * sizeof(uint32_t) );

    wordsWithOffsets.append( QPair< gd::wstring, uint32_t >( gd::toWString( it.key() ), offset ) );

    it = ftsWords.erase( it );

    if( wordsWithOffsets.size() >= BUF_SIZE )
    {
      for( int i = 0; i < wordsWithOffsets.size(); i++ )
      {
        if( isCancelled.loadAcquire() )
          throw exUserAbort();
        indexedWords.addSingleWord( wordsWithOffsets[ i ].first, wordsWithOffsets[ i ].second );
      }
      wordsWithOffsets.clear();
    }
  }

  // Free memory
  ftsWords.clear();

  for( int i = 0; i < wordsWithOffsets.size(); i++ )
  {
    if( isCancelled.loadAcquire() )
      throw exUserAbort();
    indexedWords.addSingleWord( wordsWithOffsets[ i ].first, wordsWithOffsets[ i ].second );
  }
#undef BUF_SIZE

  // Free memory
  wordsWithOffsets.clear();
  wordsWithOffsets.squeeze();

  ftsIdxHeader.chunksOffset = chunks.finish();
  ftsIdxHeader.wordCount = indexedWords.size();

  if( isCancelled.loadAcquire() )
    throw exUserAbort();

  BtreeIndexing::IndexInfo ftsIdxInfo = BtreeIndexing::buildIndex( indexedWords, ftsIdx );

  // Free memory
  indexedWords.clear();

  ftsIdxHeader.indexBtreeMaxElements = ftsIdxInfo.btreeMaxElements;
  ftsIdxHeader.indexRootOffset = ftsIdxInfo.rootOffset;

  ftsIdxHeader.signature = FtsHelpers::FtsSignature;
  ftsIdxHeader.formatVersion = FtsHelpers::CurrentFtsFormatVersion + dict->getFtsIndexVersion();

  ftsIdx.rewind();
  ftsIdx.writeRecords( &ftsIdxHeader, sizeof(ftsIdxHeader), 1 );
}

bool isCJKChar( ushort ch )
{
  if( ( ch >= 0x3400 && ch <= 0x9FFF )
      || ( ch >= 0xF900 && ch <= 0xFAFF )
      || ( ch >= 0xD800 && ch <= 0xDFFF ) )
    return true;

  return false;
}

void FTSResultsRequestRunnable::run()
{
  r.run();
}

void FTSResultsRequest::checkArticles( QVector< uint32_t > const & offsets,
                                       QStringList const & words,
                                       QRegularExpression const & searchRegexp )
{
  int results = 0;
  QString headword, articleText;
  QList< uint32_t > offsetsForHeadwords;
  QVector< QStringList > hiliteRegExps;

  QString id = QString::fromUtf8( dict.getId().c_str() );
  bool needHandleBrackets;
  {
    QString name = QString::fromUtf8( dict.getDictionaryFilenames()[ 0 ].c_str() ).toLower();
    needHandleBrackets = name.endsWith( ".dsl" ) || name.endsWith( ".dsl.dz" );
  }

  QRegularExpression const & regBrackets = bracketRegExp();

  if( searchMode == FTS::Wildcards || searchMode == FTS::RegExp )
  {
    // RegExp mode

    // The expression already carries the source syntax, the wildcard
    // conversion, the case flag and every grading option; it is never
    // re-derived here, and parseSearchString() has already refused an invalid
    // user pattern, so this is the only expression the search can run with.
    for( int i = 0; i < offsets.size(); i++ )
    {
      if( isCancelled.loadAcquire() )
        break;

      dict.getArticleText( offsets.at( i ), headword, articleText );
      articleText = articleText.normalized( QString::NormalizationForm_C );

      if( ignoreDiacritics )
        articleText = gd::toQString( Folding::applyDiacriticsOnly( gd::toWString( articleText ) ) );

      if( articleText.contains( searchRegexp ) )
      {
        if( headword.isEmpty() )
          offsetsForHeadwords.append( offsets.at( i ) );
        else
          foundHeadwords->append( FTS::FtsHeadword( headword, id, QStringList(), matchCase ) );

        results++;
        if( maxResults > 0 && results >= maxResults )
          break;
      }
    }
  }
  else
  {
    // Words mode

    QRegularExpression const & splitWithBrackets = splitWithBracketsRegExp();
    QRegularExpression const & splitWithoutBrackets = splitWithoutBracketsRegExp();

    Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QVector< QPair< QString, bool > > wordsList;
    if( ignoreWordsOrder )
    {
      for( QStringList::const_iterator it = words.begin(); it != words.end(); ++it )
        wordsList.append( QPair< QString, bool >( *it, true ) );
    }

    for( int i = 0; i < offsets.size(); i++ )
    {
      if( isCancelled.loadAcquire() )
        break;

      int pos = 0;
      int matchWordNom = 0;
      int unmatchWordNom = 0;
      int nextNotFoundPos = 0;

      QVector< QStringList > allOrders;
      QStringList order;

      if( ignoreWordsOrder )
      {
        for( int i = 0; i < wordsList.size(); i++ )
          wordsList[ i ].second = true;
      }

      dict.getArticleText( offsets.at( i ), headword, articleText );

      articleText = articleText.normalized( QString::NormalizationForm_C );

      if( ignoreDiacritics )
        articleText = gd::toQString( Folding::applyDiacriticsOnly( gd::toWString( articleText ) ) );

      QStringList articleWords = articleText.split( needHandleBrackets ? splitWithBrackets : splitWithoutBrackets,
                                                    Qt::SkipEmptyParts );

      int wordsNum = articleWords.length();
      while ( pos < wordsNum )
      {
          QString s = articleWords[ pos ];
          bool breakSearch = false;

          QStringList parsedWords;
          if( needHandleBrackets && ( s.indexOf( '(' ) >= 0 || s.indexOf( ')' ) >= 0 ) )
          {
            // Handle brackets
            QRegularExpressionMatch match_brackets = regBrackets.match( s );
            if( match_brackets.hasMatch() )
            {
              QStringList parts = match_brackets.capturedTexts();
              // Add empty strings for the arity QRegExp reported
              for( int i = match_brackets.lastCapturedIndex() + 1; i < 6; i++ )
                parts.append( QString() );
              QString word = parts[ 2 ] + parts[ 4 ]; // Brackets removed
              parsedWords.append( word );

              word = parts[ 1 ].remove( '(' ).remove( ')' )
                     + parts[ 2 ]
                     + parts[ 3 ].remove( '(' ).remove( ')' )
                     + parts[ 4 ]
                     + parts[ 5 ].remove( '(' ).remove( ')' ); // Brackets expansed
              parsedWords.append( word );
            }
            else
              parsedWords = s.split( splitWithoutBrackets, Qt::SkipEmptyParts );
          }
          else
            parsedWords.append( s );

          int n;
          for( n = 0; n < parsedWords.size(); n++ )
          {
            if( ignoreWordsOrder )
            {
              int i;
              for( i = 0; i < wordsList.size(); i++ )
              {
                if( wordsList.at( i ).second )
                {
                  if( ( searchMode == FTS::WholeWords && parsedWords.at( n ).compare( wordsList.at( i ).first, cs ) == 0 )
                      || ( searchMode == FTS::PlainText && parsedWords.at( n ).contains( wordsList.at( i ).first, cs ) ) )
                  {
                    wordsList[ i ].second = false;

                    if( parsedWords.size() > 1 )
                    {
                      QString wordToHilite = s;
                      while( !wordToHilite.isEmpty() && ( wordToHilite.at( 0 ) == '(' || wordToHilite.at( 0 ) == ')' ) )
                        wordToHilite.remove( 0, 1 );
                      while( !wordToHilite.isEmpty() && ( wordToHilite.endsWith( '(' ) || wordToHilite.endsWith( ')' ) ) )
                        wordToHilite.chop( 1 );
                      order.append( wordToHilite.replace( '(', "\\(" ).replace( ')', "\\)" ) );
                    }
                    else
                      order.append( wordsList.at( i ).first );

                    break;
                  }
                }
              }
              if( i < wordsList.size() )
              {
                // Word found

                matchWordNom += 1;

                if( matchWordNom == 1 )
                {
                  // Store position to remake search if sequence will not be found
                  nextNotFoundPos = pos + 1;
                }

                if( matchWordNom >= words.size() )
                {
                  // All words are found
                  // Store found words sequence and continue search
                  // It's nesessary for hilite search results

                  // Check if such sequence already presented
                  int x;
                  for( x = 0; x < allOrders.size(); x++ )
                  {
                    if( allOrders[ x ] == order )
                      break;
                  }
                  if( x >= allOrders.size() )
                    allOrders.append( order );

                  order.clear();

                  matchWordNom = 0;
                  unmatchWordNom = 0;
                  for( int i = 0; i < wordsList.size(); i++ )
                    wordsList[ i ].second = true;
                  nextNotFoundPos = 0;

                  break;
                }

                unmatchWordNom = 0;
                break;
              }
              else
              if( matchWordNom > 0 && n >= parsedWords.size() - 1 )
              {
                unmatchWordNom += 1;
                if( distanceBetweenWords >= 0 && unmatchWordNom > distanceBetweenWords )
                {
                  // Sequence broken, clear all counters
                  matchWordNom = 0;
                  unmatchWordNom = 0;
                  for( int i = 0; i < wordsList.size(); i++ )
                    wordsList[ i ].second = true;
                  order.clear();
                }
              }
            }
            else
            {
              if( ( searchMode == FTS::WholeWords && parsedWords.at( n ).compare( words.at( matchWordNom ), cs ) == 0 )
                  || ( searchMode == FTS::PlainText && parsedWords.at( n ).contains( words.at( matchWordNom ), cs ) ) )
              {
                matchWordNom += 1;

                if( matchWordNom == 1 )
                {
                  // Store position to remake search if sequence will not be found
                  nextNotFoundPos = pos + 1;
                }

                if( needHandleBrackets )
                {
                  if( parsedWords.size() > 1 )
                  {
                    QString wordToHilite = s;
                    while( !wordToHilite.isEmpty() && ( wordToHilite.at( 0 ) == '(' || wordToHilite.at( 0 ) == ')' ) )
                      wordToHilite.remove( 0, 1 );
                    while( !wordToHilite.isEmpty() && ( wordToHilite.endsWith( '(' ) || wordToHilite.endsWith( ')' ) ) )
                      wordToHilite.chop( 1 );
                    order.append( wordToHilite.replace( '(', "\\(" ).replace( ')', "\\)" ) );
                  }
                  else
                    order.append( words.at( matchWordNom - 1 ) );
                }

                if( matchWordNom >= words.size() )
                {
                  // All words are found
                  if( needHandleBrackets )
                  {
                    if( allOrders.isEmpty() )
                      allOrders.append( words );

                    // Check if such sequence already presented
                    int x;
                    for( x = 0; x < allOrders.size(); x++ )
                    {
                      if( allOrders[ x ] == order )
                        break;
                    }
                    if( x >= allOrders.size() )
                      allOrders.append( order );

                    matchWordNom = 0;
                    unmatchWordNom = 0;
                    order.clear();
                    nextNotFoundPos = 0;
                  }
                  else
                    breakSearch = true;
                  break;
                }
                unmatchWordNom = 0;
                break;
              }
              else
              if( matchWordNom > 0 && n >= parsedWords.size() - 1 )
              {
                unmatchWordNom += 1;
                if( distanceBetweenWords >= 0 && unmatchWordNom > distanceBetweenWords )
                {
                  matchWordNom = 0;
                  unmatchWordNom = 0;
                  if( needHandleBrackets )
                    order.clear();
                }
              }
            }
          }
          if( breakSearch )
            break;
          if( nextNotFoundPos > 0 && matchWordNom == 0 )
          {
            pos = nextNotFoundPos;
            nextNotFoundPos = 0;
          }
          else
            pos += 1;
      }

      if( !allOrders.isEmpty() || matchWordNom >= words.size() )
      {
        QStringList hiliteReg;
        if( !allOrders.isEmpty() )
        {
          for( int i = 0; i < allOrders.size(); i++ )
          {
            QString hiliteStr = makeHiliteRegExpString( allOrders.at( i ), searchMode, distanceBetweenWords );
            hiliteReg.append( hiliteStr );
          }
          allOrders.clear();
        }
        if( headword.isEmpty() )
        {
          offsetsForHeadwords.append( offsets.at( i ) );
          hiliteRegExps.append( hiliteReg );
        }
        else
          foundHeadwords->append( FTS::FtsHeadword( headword, id, hiliteReg, matchCase ) );

        results++;
        if( maxResults > 0 && results >= maxResults )
          break;
      }
    }
  }
  if( !offsetsForHeadwords.isEmpty() )
  {
    QVector< QString > headwords;
    dict.getHeadwordsFromOffsets( offsetsForHeadwords, headwords, &isCancelled );
    for( int x = 0; x < headwords.size(); x++ )
      foundHeadwords->append( FTS::FtsHeadword( headwords.at( x ), id, x < hiliteRegExps.size() ? hiliteRegExps.at( x ) : QStringList(), matchCase ) );
  }
}

void FTSResultsRequest::indexSearch( BtreeIndexing::BtreeIndex & ftsIndex,
                                     sptr< ChunkedStorage::Reader > chunks,
                                     QStringList & indexWords,
                                     QStringList & searchWords )
{
  // Find articles which contains all requested words

  vector< BtreeIndexing::WordArticleLink > links;
  QSet< uint32_t > setOfOffsets, tmp;
  uint32_t size;

  if( indexWords.isEmpty() )
    return;

  int n = indexWords.length();
  for( int i = 0; i < n; i++ )
  {
    if( isCancelled.loadAcquire() )
      return;

    tmp.clear();

    links = ftsIndex.findArticles( gd::toWString( indexWords.at( i ) ), ignoreDiacritics );
    for( unsigned x = 0; x < links.size(); x++ )
    {

      if( isCancelled.loadAcquire() )
        return;

      vector< char > chunk;
      char * linksPtr;
      {
        Mutex::Lock _( dict.getFtsMutex() );
        linksPtr = chunks->getBlock( links[ x ].articleOffset, chunk );
      }

      memcpy( &size, linksPtr, sizeof(uint32_t) );
      linksPtr += sizeof(uint32_t);
      for( uint32_t y = 0; y < size; y++ )
      {
        tmp.insert( *( reinterpret_cast< uint32_t * >( linksPtr ) ) );
        linksPtr += sizeof(uint32_t);
      }
    }

    links.clear();

    if( i == 0 )
      setOfOffsets = tmp;
    else
      setOfOffsets = setOfOffsets.intersect( tmp );
  }

  tmp.clear();

  if( setOfOffsets.isEmpty() )
    return;

  QVector< uint32_t > offsets;
  offsets.resize( setOfOffsets.size() );
  uint32_t * ptr = &offsets.front();

  for( QSet< uint32_t >::ConstIterator it = setOfOffsets.constBegin();
       it != setOfOffsets.constEnd(); ++it )
  {
    *ptr = *it;
    ptr++;
  }

  setOfOffsets.clear();

  dict.sortArticlesOffsetsForFTS( offsets, isCancelled );

  checkArticles( offsets, searchWords );
}

void FTSResultsRequest::combinedIndexSearch( BtreeIndexing::BtreeIndex & ftsIndex,
                                             sptr< ChunkedStorage::Reader > chunks,
                                             QStringList & indexWords,
                                             QStringList & searchWords,
                                             QRegularExpression const & regexp )
{
  // Special case - combination of index search for hieroglyphs
  // and full index search for other words

  QSet< uint32_t > setOfOffsets;
  uint32_t size;

  if( isCancelled.loadAcquire() )
    return;

  if( indexWords.isEmpty() )
    return;

  QStringList wordsList, hieroglyphsList;

  for( int x = 0; x < indexWords.size(); x++ )
  {
    QString const & word = indexWords.at( x );
    if( isCJKChar( word[ 0 ].unicode() ) )
      hieroglyphsList.append( word );
    else
      wordsList.append( word );
  }

  QVector< QSet< uint32_t > > allWordsLinks;

  int n = wordsList.size();
  if( !hieroglyphsList.isEmpty() )
    n += 1;

  allWordsLinks.resize( n );
  int wordNom = 0;

  if( !hieroglyphsList.empty() )
  {
    QSet< uint32_t > tmp;
    vector< BtreeIndexing::WordArticleLink > links;

    for( int i = 0; i < hieroglyphsList.size(); i++ )
    {
      links = ftsIndex.findArticles( gd::toWString( hieroglyphsList.at( i ) ) );
      for( unsigned x = 0; x < links.size(); x++ )
      {

        if( isCancelled.loadAcquire() )
          return;

        vector< char > chunk;
        char * linksPtr;
        {
          Mutex::Lock _( dict.getFtsMutex() );
          linksPtr = chunks->getBlock( links[ x ].articleOffset, chunk );
        }

        memcpy( &size, linksPtr, sizeof(uint32_t) );
        linksPtr += sizeof(uint32_t);
        for( uint32_t y = 0; y < size; y++ )
        {
          tmp.insert( *( reinterpret_cast< uint32_t * >( linksPtr ) ) );
          linksPtr += sizeof(uint32_t);
        }
      }

      links.clear();

      if( i == 0 )
        setOfOffsets = tmp;
      else
        setOfOffsets = setOfOffsets.intersect( tmp );
    }

    allWordsLinks[ wordNom ] = setOfOffsets;
    setOfOffsets.clear();
    wordNom += 1;
  }

  if( !wordsList.isEmpty() )
  {
    QVector< BtreeIndexing::WordArticleLink > links;
    links.reserve( wordsInIndex );
    ftsIndex.findArticleLinks( &links, 0, 0, &isCancelled );

    for( int x = 0; x < links.size(); x++ )
    {
      if( isCancelled.loadAcquire() )
        return;

      QString word = QString::fromUtf8( links[ x ].word.data(), links[ x ].word.size() );

      if( ignoreDiacritics )
        word = gd::toQString( Folding::applyDiacriticsOnly( gd::toWString( word ) ) );

      for( int i = 0; i < wordsList.size(); i++ )
      {
        if( word.length() >= wordsList.at( i ).length() && word.contains( wordsList.at( i ) ) )
        {
          vector< char > chunk;
          char * linksPtr;
          {
            Mutex::Lock _( dict.getFtsMutex() );
            linksPtr = chunks->getBlock( links[ x ].articleOffset, chunk );
          }

          memcpy( &size, linksPtr, sizeof(uint32_t) );
          linksPtr += sizeof(uint32_t);
          for( uint32_t y = 0; y < size; y++ )
          {
            allWordsLinks[ wordNom ].insert( *( reinterpret_cast< uint32_t * >( linksPtr ) ) );
            linksPtr += sizeof(uint32_t);
          }
          wordNom += 1;
          if( searchMode == FTS::PlainText || searchMode == FTS::WholeWords )
            break;
        }
      }
    }

    links.clear();
  }

  for( int i = 0; i < allWordsLinks.size(); i++ )
  {
    if( i == 0 )
      setOfOffsets = allWordsLinks.at( i );
    else
      setOfOffsets = setOfOffsets.intersect( allWordsLinks.at( i ) );
  }

  if( setOfOffsets.isEmpty() )
    return;

  allWordsLinks.clear();

  QVector< uint32_t > offsets;
  offsets.resize( setOfOffsets.size() );
  uint32_t * ptr = &offsets.front();

  for( QSet< uint32_t >::ConstIterator it = setOfOffsets.constBegin();
       it != setOfOffsets.constEnd(); ++it )
  {
    *ptr = *it;
    ptr++;
  }

  setOfOffsets.clear();

  dict.sortArticlesOffsetsForFTS( offsets, isCancelled );

  checkArticles( offsets, searchWords, regexp );
}

void FTSResultsRequest::fullIndexSearch( BtreeIndexing::BtreeIndex & ftsIndex,
                                         sptr< ChunkedStorage::Reader > chunks,
                                         QStringList & indexWords,
                                         QStringList & searchWords,
                                         QRegularExpression const & regexp )
{
  QSet< uint32_t > setOfOffsets;
  uint32_t size;
  QVector< BtreeIndexing::WordArticleLink > links;

  if( isCancelled.loadAcquire() )
    return;

  if( indexWords.isEmpty() )
    return;

  links.reserve( wordsInIndex );
  ftsIndex.findArticleLinks( &links, 0, 0, &isCancelled );

  QVector< QSet< uint32_t > > allWordsLinks;
  allWordsLinks.resize( indexWords.size() );

  for( int x = 0; x < links.size(); x++ )
  {
    if( isCancelled.loadAcquire() )
      return;

    QString word = QString::fromUtf8( links[ x ].word.data(), links[ x ].word.size() );

    if( ignoreDiacritics )
      word = gd::toQString( Folding::applyDiacriticsOnly( gd::toWString( word ) ) );

    for( int i = 0; i < indexWords.size(); i++ )
    {
      if( word.length() >= indexWords.at( i ).length() && word.contains( indexWords.at( i ) ) )
      {
        vector< char > chunk;
        char * linksPtr;
        {
          Mutex::Lock _( dict.getFtsMutex() );
          linksPtr = chunks->getBlock( links[ x ].articleOffset, chunk );
        }

        memcpy( &size, linksPtr, sizeof(uint32_t) );
        linksPtr += sizeof(uint32_t);
        for( uint32_t y = 0; y < size; y++ )
        {
          allWordsLinks[ i ].insert( *( reinterpret_cast< uint32_t * >( linksPtr ) ) );
          linksPtr += sizeof(uint32_t);
        }
        if( searchMode == FTS::PlainText )
          break;
      }
    }
  }

  links.clear();

  for( int i = 0; i < allWordsLinks.size(); i++ )
  {
    if( i == 0 )
      setOfOffsets = allWordsLinks.at( i );
    else
      setOfOffsets = setOfOffsets.intersect( allWordsLinks.at( i ) );
  }

  if( setOfOffsets.isEmpty() )
    return;

  allWordsLinks.clear();

  QVector< uint32_t > offsets;
  offsets.resize( setOfOffsets.size() );
  uint32_t * ptr = &offsets.front();

  for( QSet< uint32_t >::ConstIterator it = setOfOffsets.constBegin();
       it != setOfOffsets.constEnd(); ++it )
  {
    *ptr = *it;
    ptr++;
  }

  setOfOffsets.clear();

  dict.sortArticlesOffsetsForFTS( offsets, isCancelled );

  checkArticles( offsets, searchWords, regexp );
}

void FTSResultsRequest::fullSearch( QStringList & searchWords, QRegularExpression const & regexp )
{
  // Whole file survey

  if( isCancelled.loadAcquire() )
    return;

  QSet< uint32_t > setOfOffsets;
  setOfOffsets.reserve( dict.getArticleCount() );
  dict.findArticleLinks( 0, &setOfOffsets, 0, &isCancelled );

  if( isCancelled.loadAcquire() )
    return;

  QVector< uint32_t > offsets;
  offsets.resize( setOfOffsets.size() );
  uint32_t * ptr = &offsets.front();

  for( QSet< uint32_t >::ConstIterator it = setOfOffsets.constBegin();
       it != setOfOffsets.constEnd(); ++it )
  {
    *ptr = *it;
    ptr++;
  }

  if( isCancelled.loadAcquire() )
    return;

  setOfOffsets.clear();

  dict.sortArticlesOffsetsForFTS( offsets, isCancelled );

  if( isCancelled.loadAcquire() )
    return;

  checkArticles( offsets, searchWords, regexp );
}

void FTSResultsRequest::run()
{
  if( isCancelled.loadAcquire() )
  {
    finish();
    return;
  }

  if ( dict.ensureInitDone().size() )
  {
    setErrorString( QString::fromUtf8( dict.ensureInitDone().c_str() ) );
    finish();
    return;
  }

  try
  {
    QStringList indexWords, searchWords;
    FtsHighlightSpec searchRegExp;

    if( !FtsHelpers::parseSearchString( searchString, indexWords, searchWords, searchRegExp,
                                        searchMode, matchCase, distanceBetweenWords, hasCJK ) )
    {
      finish();
      return;
    }

    if( dict.haveFTSIndex() && !indexWords.isEmpty() )
    {
      FtsIdxHeader ftsIdxHeader;
      BtreeIndexing::BtreeIndex ftsIndex;
      sptr< ChunkedStorage::Reader > chunks;

      File::Class ftsIdx( dict.ftsIndexName(), "rb" );

      {
        Mutex::Lock _( dict.getFtsMutex() );

        ftsIdxHeader = ftsIdx.read< FtsIdxHeader >();

        wordsInIndex = ftsIdxHeader.wordCount;

        ftsIndex.openIndex( BtreeIndexing::IndexInfo( ftsIdxHeader.indexBtreeMaxElements,
                                                      ftsIdxHeader.indexRootOffset ),
                            ftsIdx, dict.getFtsMutex() );

        chunks = new ChunkedStorage::Reader( ftsIdx, ftsIdxHeader.chunksOffset );
      }

      if( hasCJK )
        combinedIndexSearch( ftsIndex, chunks, indexWords, searchWords, searchRegExp.expression );
      else
      {
        if( searchMode == FTS::WholeWords )
          indexSearch( ftsIndex, chunks, indexWords, searchWords );
        else
          fullIndexSearch( ftsIndex, chunks, indexWords, searchWords, searchRegExp.expression );
      }
    }
    else
    {
      fullSearch( searchWords, searchRegExp.expression );
    }

    if( foundHeadwords && foundHeadwords->size() > 0 )
    {
      Mutex::Lock _( dataMutex );
      data.resize( sizeof( foundHeadwords ) );
      memcpy( &data.front(), &foundHeadwords, sizeof( foundHeadwords ) );
      foundHeadwords = 0;
      hasAnyData = true;
    }
  }
  catch( std::exception &ex )
  {
    gdWarning( "FTS: Failed full-text search for \"%s\", reason: %s\n",
               dict.getName().c_str(), ex.what() );
    // Results not loaded -- we don't set the hasAnyData flag then
  }

  finish();
}

} // namespace

