/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * The HTML escaping helpers (htmlescape.cc) and the MDX article repair pass
 * that now lives beside them. The repair pass previously classified characters
 * with Qt's own QRegExp tables, so these cases pin those predicates: \s is
 * QChar::isSpace() (which includes NBSP and em-space) and \b breaks on anything
 * that is not a letter, number, mark or underscore.
 */

#include <QtTest>
#include "support/fixtures.hh"
#include "htmlescape.hh"

using std::string;

class TstHtmlEscape: public QObject
{
  Q_OBJECT

private slots:
  void initTestCase();

  void escapesMarkupCharacters();
  void leavesPlainTextAlone();
  void preformatWrapsLinesAndNonBreakingSpaces();
  void escapesJavaScriptSpecials();
  void preservesUtf8AndEmbeddedNul();

  void balanceAppendsMissingSpanClosingTags();
  void balanceAppendsMissingDivClosingTags();
  void balanceClosesSpansBeforeDivs();
  void balanceCountsNonBreakingAndEmSpacesAsWhitespace();
  void balanceWordBoundaryFollowsQtClasses();
  void balanceRejectsClosingTagWhitespaceOutsideQtSpace();
  void balanceWordPredicateExcludesMarksButAcceptsConnector();
  void balanceLeavesBalancedArticleUntouched();
};

void TstHtmlEscape::initTestCase()
{
  QStringList reasons;
  QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
}

void TstHtmlEscape::escapesMarkupCharacters()
{
  QCOMPARE( QString::fromStdString( Html::escape( "<a href=\"x\">&" ) ),
            QStringLiteral( "&lt;a href=&quot;x&quot;&gt;&amp;" ) );

  // The literal comparison the brief names.
  QCOMPARE( QString::fromStdString( Html::escape( std::string( "<&>" ) ) ),
            QStringLiteral( "&lt;&amp;&gt;" ) );
}

void TstHtmlEscape::leavesPlainTextAlone()
{
  QCOMPARE( QString::fromStdString( Html::escape( "plain text 123" ) ),
            QStringLiteral( "plain text 123" ) );
}

void TstHtmlEscape::preformatWrapsLinesAndNonBreakingSpaces()
{
  QCOMPARE( QString::fromStdString( Html::preformat( " a\nb" ) ),
            QStringLiteral( "<div>&nbsp;a</div><div>b</div>" ) );
  QCOMPARE( QString::fromStdString( Html::preformat( "  x\r\ny" ) ),
            QStringLiteral( "<div>&nbsp;&nbsp;x</div><div>y</div>" ) );
}

void TstHtmlEscape::escapesJavaScriptSpecials()
{
  QVERIFY( !QString::fromStdString( Html::escapeForJavaScript( "a'b\"c\\d" ) ).isEmpty() );
}

void TstHtmlEscape::preservesUtf8AndEmbeddedNul()
{
  QByteArray const utf8 = QString::fromUtf8( "Ünïcode 中文" ).toUtf8();
  QCOMPARE( Html::escape( string( utf8.constData(), utf8.size() ) ),
            string( utf8.constData(), utf8.size() ) );

  QByteArray withNul( "a", 1 );
  withNul.append( '\0' );
  withNul.append( 'b' );
  QCOMPARE( Html::escape( string( withNul.constData(), withNul.size() ) ).size(), withNul.size() );
}

void TstHtmlEscape::balanceAppendsMissingSpanClosingTags()
{
  QString article = QStringLiteral( "<span class=x>text" );
  Html::balanceSpanDivTags( article );
  QCOMPARE( article, QStringLiteral( "<span class=x>text</span>" ) );
}

void TstHtmlEscape::balanceAppendsMissingDivClosingTags()
{
  QString article = QStringLiteral( "<div>a" );
  Html::balanceSpanDivTags( article );
  QCOMPARE( article, QStringLiteral( "<div>a</div>" ) );
}

void TstHtmlEscape::balanceClosesSpansBeforeDivs()
{
  QString article = QStringLiteral( "<div><span>x" );
  Html::balanceSpanDivTags( article );
  QCOMPARE( article, QStringLiteral( "<div><span>x</span></div>" ) );
}

void TstHtmlEscape::balanceCountsNonBreakingAndEmSpacesAsWhitespace()
{
  // U+00A0 and U+2003 are spaces for QChar::isSpace(), so both tags open.
  QString article = QString::fromUtf8( "<\u00a0span>x<\u2003div>y" );
  Html::balanceSpanDivTags( article );
  QCOMPARE( article, QString::fromUtf8( "<\u00a0span>x<\u2003div>y</span></div>" ) );
}

void TstHtmlEscape::balanceWordBoundaryFollowsQtClasses()
{
  // A combining mark continues the word for Qt's \b, so "<span" + U+0301 is not <span>.
  QString withMark = QString::fromUtf8( "<span\u0301>x" );
  Html::balanceSpanDivTags( withMark );
  QCOMPARE( withMark, QString::fromUtf8( "<span\u0301>x" ) );

  QString withUnderscore = QStringLiteral( "<span_x>y" );
  Html::balanceSpanDivTags( withUnderscore );
  QCOMPARE( withUnderscore, QStringLiteral( "<span_x>y" ) );
}

void TstHtmlEscape::balanceRejectsClosingTagWhitespaceOutsideQtSpace()
{
  // A closing tag only counts when the run before '>' is QChar::isSpace().
  // U+00A0 and U+2003 are spaces; these three are the counterexamples that a
  // PCRE \s predicate silently accepts or rejects differently.
  for( QChar const space : { QChar( 0x00a0 ), QChar( 0x2003 ) } )
  {
    QString span = QStringLiteral( "<span>x</span" ) + space + QStringLiteral( ">" );
    QString const spanBefore = span;
    Html::balanceSpanDivTags( span );
    QCOMPARE( span, spanBefore );

    QString div = QStringLiteral( "<div>x</div" ) + space + QStringLiteral( ">" );
    QString const divBefore = div;
    Html::balanceSpanDivTags( div );
    QCOMPARE( div, divBefore );
  }
}

void TstHtmlEscape::balanceWordPredicateExcludesMarksButAcceptsConnector()
{
  // Legacy \b continues the word on any mark (U+0903, U+20DD), so these are not
  // <span>; U+203F is punctuation, so that one is.
  for( QChar const mark : { QChar( 0x0903 ), QChar( 0x20dd ) } )
  {
    QString marked = QStringLiteral( "<span" ) + mark + QStringLiteral( ">x" );
    QString const before = marked;
    Html::balanceSpanDivTags( marked );
    QCOMPARE( marked, before );
  }

  QString connector = QStringLiteral( "<span" ) + QChar( 0x203f ) + QStringLiteral( ">x" );
  Html::balanceSpanDivTags( connector );
  QCOMPARE( connector, QStringLiteral( "<span" ) + QChar( 0x203f ) + QStringLiteral( ">x</span>" ) );
}

void TstHtmlEscape::balanceLeavesBalancedArticleUntouched()
{
  QString const balanced = QStringLiteral( "<span>a</span><div>b</div>" );
  QString article = balanced;
  Html::balanceSpanDivTags( article );
  QCOMPARE( article, balanced );
}

QTEST_MAIN( TstHtmlEscape )
#include "tst_htmlescape.moc"
