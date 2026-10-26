/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include <QString>
#include <QTextDocumentFragment>

#include "wildcard.hh"

#include "htmlescape.hh"

namespace Html {

string escape( string const & str )
{
  string result( str );

  for( size_t x = result.size(); x--; )
    switch ( result[ x ] )
    {
      case '&':
        result.erase( x, 1 );
        result.insert( x, "&amp;" );
      break;

      case '<':
        result.erase( x, 1 );
        result.insert( x, "&lt;" );
      break;

      case '>':
        result.erase( x, 1 );
        result.insert( x, "&gt;" );
      break;

      case '"':
        result.erase( x, 1 );
        result.insert( x, "&quot;" );
      break;

      default:
      break;
    }

  return result;
}

static void storeLineInDiv( string & result, string const & line, bool baseRightToLeft )
{
  result += "<div";
  if( unescape( QString::fromUtf8( line.c_str(), line.size() ) ).isRightToLeft() != baseRightToLeft )
  {
    result += " dir=\"";
    result += baseRightToLeft ? "ltr\"" : "rtl\"";
  }
  result += ">";
  result += line + "</div>";
}

string preformat(string const & str , bool baseRightToLeft )
{
  string escaped = escape( str ), result, line;

  line.reserve( escaped.size() );
  result.reserve( escaped.size() );

  bool leading = true;

  for( char const * nextChar = escaped.c_str(); *nextChar; ++nextChar )
  {
    if ( leading )
    {
      if ( *nextChar == ' ' )
      {
        line += "&nbsp;";
        continue;
      }
      else
      if ( *nextChar == '\t' )
      {
        line += "&nbsp;&nbsp;&nbsp;&nbsp;";
        continue;
      }
    }

    if ( *nextChar == '\n' )
    {
      storeLineInDiv( result, line, baseRightToLeft );
      line.clear();
      leading = true;
      continue;
    }

    if ( *nextChar == '\r' )
      continue; // Just skip all \r

    line.push_back( *nextChar );

    leading = false;
  }

  if( !line.empty() )
    storeLineInDiv( result, line, baseRightToLeft );

  return result;
}

string escapeForJavaScript( string const & str )
{
  string result( str );

  for( size_t x = result.size(); x--; )
    switch ( result[ x ] )
    {
      case '\\':
      case '"':
      case '\'':
        result.insert( x, 1, '\\' );
      break;

      case '\n':
        result.erase( x, 1 );
        result.insert( x, "\\n" );
      break;

      case '\r':
        result.erase( x, 1 );
        result.insert( x, "\\r" );
      break;

      case '\t':
        result.erase( x, 1 );
        result.insert( x, "\\t" );
      break;

      default:
      break;
    }

  return result;
}

QString unescape( QString const & str, bool saveFormat )
{
  // Does it contain HTML? If it does, we need to strip it
  if ( str.contains( '<' ) || str.contains( '&' ) )
  {
    QString tmp = str;
    if( !saveFormat )
    {
        tmp.replace( Regex::validated( "<(?:\\s*/?(?:div|h[1-6r]|q|p(?![alr])|br|li(?![ns])|td|blockquote|[uo]l|pre|d[dl]|nav|address))[^>]{0,}>",
                                         QRegularExpression::CaseInsensitiveOption ), " " );
        tmp.remove( Regex::validated( "<[^>]*>" ) );
    }
    return QTextDocumentFragment::fromHtml( tmp.trimmed() ).toPlainText();
  }
  return str;
}

string unescapeUtf8( const string &str, bool saveFormat )
{
  return string( unescape( QString::fromUtf8( str.c_str(), str.size() ) ).toUtf8().data(), saveFormat );
}


void balanceSpanDivTags( QString & article )
{
  // Check for unclosed <span> and <div>. The original QRegExp patterns classified
  // characters with Qt's own tables (\s is QChar::isSpace(); \b breaks on any
  // character that is not a letter, number, mark or underscore), which PCRE does
  // not reproduce, so the counts use those classes directly.
  auto isMdxWordChar = []( QChar c ) {
    return c.isLetterOrNumber() || c.isMark() || c == QLatin1Char( '_' );
  };

  auto countMdxOpen = [ & ]( QString const & name ) {
    qsizetype const length = name.size();
    int count = 0;
    for( qsizetype i = 0; i + length < article.size(); ++i )
    {
      if( article[ i ] != QLatin1Char( '<' ) )
        continue;
      qsizetype j = i + 1;
      while( j < article.size() && article[ j ].isSpace() )
        ++j;
      qsizetype k = 0;
      while( k < length && j + k < article.size() && article[ j + k ].toLower() == name.at( k ).toLower() )
        ++k;
      if( k != length )
        continue;
      qsizetype const after = j + length;
      if( after >= article.size() || !isMdxWordChar( article[ after ] ) )
        ++count;
    }
    return count;
  };

  auto countMdxClose = [ & ]( QString const & name ) {
    qsizetype const length = name.size();
    int count = 0;
    for( qsizetype i = 0; i + length < article.size(); ++i )
    {
      if( article[ i ] != QLatin1Char( '<' ) )
        continue;
      qsizetype j = i + 1;
      while( j < article.size() && article[ j ].isSpace() )
        ++j;
      if( j >= article.size() || article[ j ] != QLatin1Char( '/' ) )
        continue;
      ++j;
      qsizetype k = 0;
      while( k < length && j + k < article.size() && article[ j + k ].toLower() == name.at( k ).toLower() )
        ++k;
      if( k != length )
        continue;
      j += length;
      while( j < article.size() && article[ j ].isSpace() )
        ++j;
      if( j < article.size() && article[ j ] == QLatin1Char( '>' ) )
        ++count;
    }
    return count;
  };

  int openTags = countMdxOpen( QStringLiteral( "span" ) );
  int closedTags = countMdxClose( QStringLiteral( "span" ) );
  while( openTags > closedTags )
  {
    article += "</span>";
    closedTags += 1;
  }

  openTags = countMdxOpen( QStringLiteral( "div" ) );
  closedTags = countMdxClose( QStringLiteral( "div" ) );
  while( openTags > closedTags )
  {
    article += "</div>";
    closedTags += 1;
  }
}

}
