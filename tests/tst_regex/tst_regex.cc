/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Source-tagged regex migration ledger suite.
 *
 * `fixtures/regex/cases.json` is the machine-readable half of the migration
 * ledger: one row per pattern family with its source location, old engine and
 * options, the migrated pattern and options, and fixed expected
 * matches/captures/replacement. This suite compiles every row and checks the
 * pinned expectations, then states the traps the migration must not walk into
 * with their own test names so a regression is not buried in a data loop.
 *
 * The engine helpers that own these patterns are converted by their owner tasks
 * (Task 9 for the wildcard and engine implementations); a row whose consumer has
 * not moved yet names its `productionOwner` and is a fixed expectation for that
 * task, not a test-only copy of its logic.
 */

#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include "support/fixtures.hh"

namespace
{

constexpr char const * FixtureFile = "regex/cases.json";

/// Hard bound on one scan. globalMatch advances a zero-length match by itself,
/// so hitting this bound means a pattern failed to progress and the suite must
/// fail instead of hanging.
constexpr int MaxMatchesPerScan = 10000;

/// Floor for the ledger size: it must keep covering every pinned family rather
/// than silently shrinking to a handful of rows.
constexpr int MinimumLedgerRows = 35;

QJsonArray loadRows()
{
    QFile file( TestSupport::fixturePath( QString::fromLatin1( FixtureFile ) ) );

    if ( !file.open( QIODevice::ReadOnly ) )
        return QJsonArray();

    const QJsonDocument document = QJsonDocument::fromJson( file.readAll() );

    if ( !document.isObject() )
        return QJsonArray();

    return document.object().value( QStringLiteral( "rows" ) ).toArray();
}

/// Renders a string so whitespace and control characters show up in messages.
QString display( QString const & text )
{
    QString shown;

    for ( QChar const character : text )
    {
        if ( character == QLatin1Char( '\n' ) )
            shown += QLatin1String( "\\n" );
        else if ( character == QLatin1Char( '\t' ) )
            shown += QLatin1String( "\\t" );
        else if ( character == QLatin1Char( '\\' ) )
            shown += QLatin1String( "\\\\" );
        else if ( character.unicode() < 0x20 || character.unicode() == 0x7f )
            shown += QStringLiteral( "\\x%1" ).arg( int( character.unicode() ), 2, 16, QLatin1Char( '0' ) );
        else
            shown += character;
    }

    return shown;
}

QString renderList( QStringList const & values )
{
    QStringList shown;

    for ( QString const & value : values )
        shown << QStringLiteral( "\"" ) + display( value ) + QStringLiteral( "\"" );

    return QStringLiteral( "[" ) + shown.join( QStringLiteral( ", " ) ) + QStringLiteral( "]" );
}

QRegularExpression::PatternOptions optionsFor(
    QJsonArray const & names, QStringList & problems, QString const & rowId )
{
    QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;

    for ( QJsonValue const & value : names )
    {
        QString const name = value.toString();

        if ( name == QLatin1String( "CaseInsensitive" ) )
            options |= QRegularExpression::CaseInsensitiveOption;
        else if ( name == QLatin1String( "DotMatchesEverything" ) )
            options |= QRegularExpression::DotMatchesEverythingOption;
        else if ( name == QLatin1String( "UseUnicodeProperties" ) )
            options |= QRegularExpression::UseUnicodePropertiesOption;
        else if ( name == QLatin1String( "InvertedGreediness" ) )
            options |= QRegularExpression::InvertedGreedinessOption;
        else if ( name == QLatin1String( "Multiline" ) )
            options |= QRegularExpression::MultilineOption;
        else
            problems << QStringLiteral( "%1: unknown migrated option '%2'" ).arg( rowId, name );
    }

    return options;
}

QRegularExpression::WildcardConversionOptions wildcardConversionFor( QString const & mode, bool * known )
{
    *known = true;

    if ( mode == QLatin1String( "nonpath" ) )
        return QRegularExpression::NonPathWildcardConversion;

    if ( mode == QLatin1String( "unanchored" ) )
        return QRegularExpression::UnanchoredWildcardConversion;

    if ( mode == QLatin1String( "unanchored-nonpath" ) )
        return QRegularExpression::UnanchoredWildcardConversion |
               QRegularExpression::NonPathWildcardConversion;

    *known = false;

    return QRegularExpression::DefaultWildcardConversion;
}

/// One scan of `expression` over `subject`. `terminated` reports whether the
/// scan reached the end of the subject inside MaxMatchesPerScan.
void scan( QRegularExpression const & expression,
    QString const & subject,
    bool anchored,
    QStringList * matches,
    QList<QStringList> * captures,
    QList<int> * starts,
    bool * terminated )
{
    *terminated = true;

    if ( anchored )
    {
        QRegularExpressionMatch const match = expression.match( subject );

        if ( !match.hasMatch() )
            return;

        *matches << match.captured( 0 );
        *captures << match.capturedTexts();
        *starts << match.capturedStart( 0 );

        return;
    }

    QRegularExpressionMatchIterator iterator = expression.globalMatch( subject );

    while ( iterator.hasNext() )
    {
        if ( matches->size() >= MaxMatchesPerScan )
        {
            *terminated = false;
            return;
        }

        QRegularExpressionMatch const match = iterator.next();

        *matches << match.captured( 0 );
        *captures << match.capturedTexts();
        *starts << match.capturedStart( 0 );
    }
}

void checkRow( QJsonObject const & row, QStringList & problems )
{
    QString const id = row.value( QStringLiteral( "id" ) ).toString();
    QString const pattern = row.value( QStringLiteral( "pattern" ) ).toString();
    QString const subject = row.value( QStringLiteral( "subject" ) ).toString();
    bool const expectInvalid = row.value( QStringLiteral( "expectInvalid" ) ).toBool( false );
    bool const anchored = row.value( QStringLiteral( "anchored" ) ).toBool( false );

    QRegularExpression::PatternOptions const options =
        optionsFor( row.value( QStringLiteral( "newOptions" ) ).toArray(), problems, id );

    QString expressionText = pattern;
    QString const wildcardMode = row.value( QStringLiteral( "wildcard" ) ).toString();

    if ( !wildcardMode.isEmpty() )
    {
        bool knownMode = false;
        QRegularExpression::WildcardConversionOptions const conversion =
            wildcardConversionFor( wildcardMode, &knownMode );

        if ( !knownMode )
        {
            problems << QStringLiteral( "%1: unknown wildcard mode '%2'" ).arg( id, wildcardMode );
            return;
        }

        expressionText = QRegularExpression::wildcardToRegularExpression( pattern, conversion );
    }

    if ( anchored )
        expressionText = QRegularExpression::anchoredPattern( expressionText );

    QRegularExpression const expression( expressionText, options );

    if ( expectInvalid )
    {
        if ( expression.isValid() )
            problems << QStringLiteral( "%1: pattern '%2' was expected to be rejected but compiled to %3" )
                            .arg( id, display( pattern ), display( expressionText ) );
        else if ( expression.errorString().isEmpty() )
            problems << QStringLiteral( "%1: an invalid pattern carries no error text" ).arg( id );

        return;
    }

    if ( !expression.isValid() )
    {
        problems << QStringLiteral( "%1: migrated pattern '%2' does not compile (%3): %4" )
                        .arg( id, display( pattern ), display( expressionText ), expression.errorString() );
        return;
    }

    QStringList matches;
    QList<QStringList> captures;
    QList<int> starts;
    bool terminated = true;

    scan( expression, subject, anchored, &matches, &captures, &starts, &terminated );

    if ( !terminated )
    {
        problems << QStringLiteral(
            "%1: the scan produced more than %2 matches on %3, so it did not terminate" )
                        .arg( id )
                        .arg( MaxMatchesPerScan )
                        .arg( display( subject ) );
        return;
    }

    if ( row.contains( QStringLiteral( "expectedMatches" ) ) )
    {
        QStringList expected;

        for ( QJsonValue const & value : row.value( QStringLiteral( "expectedMatches" ) ).toArray() )
            expected << value.toString();

        if ( matches != expected )
            problems << QStringLiteral( "%1: matches mismatch\n  actual  : %2\n  expected: %3" )
                            .arg( id, renderList( matches ), renderList( expected ) );
    }

    if ( row.contains( QStringLiteral( "expectedMatchCount" ) ) &&
         matches.size() != row.value( QStringLiteral( "expectedMatchCount" ) ).toInt() )
    {
        problems << QStringLiteral( "%1: expected %2 match(es), got %3 (%4)" )
                        .arg( id )
                        .arg( row.value( QStringLiteral( "expectedMatchCount" ) ).toInt() )
                        .arg( matches.size() )
                        .arg( renderList( matches ) );
    }

    if ( row.contains( QStringLiteral( "expectedCaptures" ) ) )
    {
        QList<QStringList> expected;

        for ( QJsonValue const & matchValue : row.value( QStringLiteral( "expectedCaptures" ) ).toArray() )
        {
            QStringList one;

            for ( QJsonValue const & captureValue : matchValue.toArray() )
                one << captureValue.toString();

            expected << one;
        }

        if ( captures.size() != expected.size() )
        {
            problems << QStringLiteral( "%1: expected captures for %2 match(es), got %3" )
                            .arg( id )
                            .arg( expected.size() )
                            .arg( captures.size() );
        }
        else
        {
            for ( int index = 0; index < captures.size(); ++index )
            {
                if ( captures.at( index ) != expected.at( index ) )
                    problems << QStringLiteral(
                        "%1: capture mismatch in match %2\n  actual  : %3\n  expected: %4" )
                                    .arg( id )
                                    .arg( index )
                                    .arg( renderList( captures.at( index ) ),
                                        renderList( expected.at( index ) ) );
            }
        }
    }

    if ( row.contains( QStringLiteral( "expectedQStringCount" ) ) )
    {
        qsizetype const counted = subject.count( expression );
        int const expected = row.value( QStringLiteral( "expectedQStringCount" ) ).toInt();

        if ( counted != expected )
            problems << QStringLiteral( "%1: QString::count reported %2, expected %3" )
                            .arg( id )
                            .arg( counted )
                            .arg( expected );
    }

    if ( row.contains( QStringLiteral( "replacement" ) ) )
    {
        QString prepared = subject;
        prepared.replace( expression, row.value( QStringLiteral( "replacement" ) ).toString() );

        QString const expected = row.value( QStringLiteral( "expectedPrepared" ) ).toString();

        if ( prepared != expected )
            problems << QStringLiteral( "%1: replacement mismatch\n  actual  : %2\n  expected: %3" )
                            .arg( id, display( prepared ), display( expected ) );
    }

    if ( row.contains( QStringLiteral( "reverseSearchOffset" ) ) )
    {
        int const offset = row.value( QStringLiteral( "reverseSearchOffset" ) ).toInt();
        bool found = false;
        QString chosen;

        for ( int index = 0; index < starts.size(); ++index )
        {
            if ( starts.at( index ) <= offset )
            {
                found = true;
                chosen = matches.at( index );
            }
        }

        if ( row.contains( QStringLiteral( "expectedReverseMatch" ) ) )
        {
            QString const expected = row.value( QStringLiteral( "expectedReverseMatch" ) ).toString();

            if ( !found || chosen != expected )
                problems << QStringLiteral( "%1: reverse search at %2 chose '%3' (found=%4), expected '%5'" )
                                .arg( id )
                                .arg( offset )
                                .arg( display( chosen ) )
                                .arg( found )
                                .arg( display( expected ) );
        }
        else if ( row.value( QStringLiteral( "expectedReverseMatchCount" ) ).toInt() == 0 && found )
        {
            problems << QStringLiteral( "%1: reverse search at %2 unexpectedly chose '%3'" )
                            .arg( id )
                            .arg( offset )
                            .arg( display( chosen ) );
        }
    }
}

} // namespace

class TstRegex : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();

    void ledgerProvenanceIsComplete();
    void ledgerRowsMatchMigratedSemantics();
    void requiredPinnedRowsArePresent();

    void greedyAndMinimalCountsAreDistinguished();
    void anchoredPatternRejectsTrailingNewline();
    void zeroWidthMatchingTerminates();
    void unicodeCapturesPreserveExactText();
    void literalHexAndOctalTokenMigrationIsPinned();
    void unmatchedOptionalCaptureIsEmpty();
    void invalidPatternIsRejectedWithDiagnostic();
};

void TstRegex::initTestCase()
{
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
    QVERIFY2( TestSupport::fixtureExists( QString::fromLatin1( FixtureFile ) ),
        qPrintable( QStringLiteral( "missing fixture %1" )
                .arg( TestSupport::fixturePath( QString::fromLatin1( FixtureFile ) ) ) ) );

    TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
}

void TstRegex::cleanupTestCase()
{
    TestSupport::clearWatchdog();
}

void TstRegex::ledgerProvenanceIsComplete()
{
    QJsonArray const rows = loadRows();
    QVERIFY2( !rows.isEmpty(), "regex/cases.json carries no rows" );

    QStringList problems;
    QSet<QString> seenIds;
    QStringList families;

    for ( QJsonValue const & value : rows )
    {
        QJsonObject const row = value.toObject();

        QString const id = row.value( QStringLiteral( "id" ) ).toString();
        QString const family = row.value( QStringLiteral( "family" ) ).toString();
        QString const source = row.value( QStringLiteral( "source" ) ).toString();
        QString const oldEngine = row.value( QStringLiteral( "oldEngine" ) ).toString();
        QString const oldOptions = row.value( QStringLiteral( "oldOptions" ) ).toString();
        QString const pattern = row.value( QStringLiteral( "pattern" ) ).toString();
        QString const migration = row.value( QStringLiteral( "migration" ) ).toString();

        if ( id.isEmpty() || family.isEmpty() || source.isEmpty() || oldEngine.isEmpty() ||
             oldOptions.isEmpty() || pattern.isEmpty() || migration.isEmpty() )
            problems << QStringLiteral(
                "%1: incomplete row (id/family/source/oldEngine/oldOptions/pattern/migration)" )
                            .arg( id.isEmpty() ? QStringLiteral( "<no id>" ) : id );

        if ( seenIds.contains( id ) )
            problems << QStringLiteral( "%1: duplicate row id" ).arg( id );

        seenIds.insert( id );

        // A row is either anchored to a real production consumer
        // (`file.cc:LINE`) or it is an explicit ledger pin, which must say so.
        if ( !source.contains( QLatin1String( ".cc:" ) ) && !source.contains( QLatin1String( ".hh:" ) ) &&
             !source.startsWith( QLatin1String( "tests/fixtures/regex/cases.json#" ) ) )
            problems << QStringLiteral( "%1: source '%2' is neither a file:line anchor nor a ledger pin" )
                            .arg( id, source );

        if ( oldEngine != QLatin1String( "QRegExp" ) && oldEngine != QLatin1String( "QRegularExpression" ) &&
             oldEngine != QLatin1String( "QString" ) )
            problems << QStringLiteral( "%1: unknown old engine '%2'" ).arg( id, oldEngine );

        if ( !families.contains( family ) )
            families << family;
    }

    if ( rows.size() < MinimumLedgerRows )
        problems
            << QStringLiteral( "only %1 rows; the ledger must keep every pattern family" ).arg( rows.size() );

    // The families the migration must keep covered (spec 4.3).
    for ( QString const & required : { QStringLiteral( "HTML tags/resources" ),
              QStringLiteral( "Unicode words/digits" ),
              QStringLiteral( "exactMatch / anchored filters" ),
              QStringLiteral( "Empty and zero-width" ),
              QStringLiteral( "Minimal matching" ),
              QStringLiteral( "String split/count/list filter" ),
              QStringLiteral( "RegExp2/escape grammar" ),
              QStringLiteral( "Replacement captures" ),
              QStringLiteral( "Find index/captures/reverse searches" ),
              QStringLiteral( "Wildcards" ) } )
    {
        if ( !families.contains( required ) )
            problems << QStringLiteral( "family '%1' has no ledger row" ).arg( required );
    }

    QVERIFY2( problems.isEmpty(), qPrintable( problems.join( '\n' ) ) );
}

void TstRegex::ledgerRowsMatchMigratedSemantics()
{
    QJsonArray const rows = loadRows();
    QVERIFY2( !rows.isEmpty(), "regex/cases.json carries no rows" );

    QStringList problems;

    for ( QJsonValue const & value : rows )
        checkRow( value.toObject(), problems );

    QVERIFY2( problems.isEmpty(),
        qPrintable( QStringLiteral( "%1 ledger row(s) failed:\n%2" )
                .arg( problems.size() )
                .arg( problems.join( '\n' ) ) ) );
}

void TstRegex::requiredPinnedRowsArePresent()
{
    // Deleting a pinned row must fail the suite: these ids are the executable
    // statement of the migration's trap list.
    QStringList const required = {
        QStringLiteral( "html-audio-two-multiline-matches-inverted-greediness" ),
        QStringLiteral( "html-audio-two-multiline-matches-explicit-lazy" ),
        QStringLiteral( "greedy-digit-count-two-matches" ),
        QStringLiteral( "inverted-greediness-digit-count-three-matches" ),
        QStringLiteral( "lazy-digit-count-three-matches" ),
        QStringLiteral( "anchored-whole-string-accepted" ),
        QStringLiteral( "anchored-rejects-trailing-newline" ),
        QStringLiteral( "zero-width-lookahead-terminates" ),
        QStringLiteral( "zero-width-star-terminates" ),
        QStringLiteral( "unicode-word-cyrillic-with-ucp" ),
        QStringLiteral( "unicode-digit-arabic-indic" ),
        QStringLiteral( "hex-legacy-four-digit-token-is-naive" ),
        QStringLiteral( "hex-migrated-brace-token" ),
        QStringLiteral( "octal-legacy-three-digit-token-is-naive" ),
        QStringLiteral( "octal-migrated-o-brace" ),
        QStringLiteral( "reverse-search-last-match-before-limit" ),
        QStringLiteral( "wildcard-nonpath-star-crosses-slash" ),
        QStringLiteral( "wildcard-unmatched-bracket-is-invalid" ),
        QStringLiteral( "invalid-builtin-pattern-is-rejected" ),
    };

    QStringList present;

    for ( QJsonValue const & value : loadRows() )
        present << value.toObject().value( QStringLiteral( "id" ) ).toString();

    QStringList missing;

    for ( QString const & id : required )
    {
        if ( !present.contains( id ) )
            missing << id;
    }

    QVERIFY2( missing.isEmpty(),
        qPrintable(
            QStringLiteral( "missing ledger rows: %1" ).arg( missing.join( QLatin1String( ", " ) ) ) ) );
}

void TstRegex::greedyAndMinimalCountsAreDistinguished()
{
    // "a1b22" has two greedy runs ("1", "22") and three minimal runs
    // ("1", "2", "2"). The approved design's measured count of three belongs to
    // the minimal/inverted configuration and must never be asserted for the
    // greedy pattern; QString::count counts matched characters (three) in both
    // configurations, which is a third, different measure.
    QString const subject = QStringLiteral( "a1b22" );

    QStringList greedy;
    QRegularExpressionMatchIterator greedyIterator =
        QRegularExpression( QStringLiteral( "\\d+" ) ).globalMatch( subject );

    while ( greedyIterator.hasNext() )
        greedy << greedyIterator.next().captured( 0 );

    QStringList const expectedGreedy{ QStringLiteral( "1" ), QStringLiteral( "22" ) };
    QCOMPARE( greedy, expectedGreedy );

    QStringList inverted;
    QRegularExpressionMatchIterator invertedIterator =
        QRegularExpression( QStringLiteral( "\\d+" ), QRegularExpression::InvertedGreedinessOption )
            .globalMatch( subject );

    while ( invertedIterator.hasNext() )
        inverted << invertedIterator.next().captured( 0 );

    QStringList const expectedMinimal{ QStringLiteral( "1" ), QStringLiteral( "2" ), QStringLiteral( "2" ) };
    QCOMPARE( inverted, expectedMinimal );

    QStringList lazy;
    QRegularExpressionMatchIterator lazyIterator =
        QRegularExpression( QStringLiteral( "\\d+?" ) ).globalMatch( subject );

    while ( lazyIterator.hasNext() )
        lazy << lazyIterator.next().captured( 0 );

    QCOMPARE( lazy, inverted );

    QCOMPARE( subject.count( QRegularExpression( QStringLiteral( "\\d+" ) ) ), qsizetype( 3 ) );
}

void TstRegex::anchoredPatternRejectsTrailingNewline()
{
    QRegularExpression const anchored( QRegularExpression::anchoredPattern( QStringLiteral( "abc" ) ) );

    QVERIFY( anchored.isValid() );
    QVERIFY( anchored.match( QStringLiteral( "abc" ) ).hasMatch() );
    QVERIFY( !anchored.match( QStringLiteral( "xabc" ) ).hasMatch() );

    // anchoredPattern uses \z, not $, so a trailing newline is part of the
    // subject and must be rejected. A `$`-terminated pattern would accept it.
    QVERIFY( !anchored.match( QStringLiteral( "abc\n" ) ).hasMatch() );
    QVERIFY( QRegularExpression( QStringLiteral( "^abc$" ) ).match( QStringLiteral( "abc\n" ) ).hasMatch() );
}

void TstRegex::zeroWidthMatchingTerminates()
{
    // A loop that advances only by capturedLength() would never move for a
    // zero-length match. globalMatch advances on its own; this test bounds the
    // scan so a regression is a failure instead of a wedged binary.
    struct Case
    {
        char const * pattern;
        char const * subject;
        int expectedMatches;
    };

    Case const cases[] = {
        { "(?=a)", "aaa", 3 },
        { "x*", "abc", 4 },
        { "", "abc", 4 },
    };

    for ( Case const & one : cases )
    {
        QRegularExpression const expression( QString::fromLatin1( one.pattern ) );
        QVERIFY2( expression.isValid(), one.pattern );

        int count = 0;
        QRegularExpressionMatchIterator iterator =
            expression.globalMatch( QString::fromLatin1( one.subject ) );

        while ( iterator.hasNext() )
        {
            if ( count > MaxMatchesPerScan )
                QFAIL( qPrintable( QStringLiteral( "scan of '%1' did not terminate" )
                        .arg( QString::fromLatin1( one.pattern ) ) ) );

            QRegularExpressionMatch const match = iterator.next();
            QCOMPARE( match.capturedLength( 0 ), qsizetype( 0 ) );
            ++count;
        }

        QCOMPARE( count, one.expectedMatches );
    }
}

void TstRegex::unicodeCapturesPreserveExactText()
{
    QRegularExpression const words(
        QStringLiteral( "\\w+" ), QRegularExpression::UseUnicodePropertiesOption );

    QRegularExpressionMatchIterator iterator = words.globalMatch( QString::fromUtf8( "Привет 日本語 ٠١٢" ) );
    QStringList captured;

    while ( iterator.hasNext() )
        captured << iterator.next().captured( 0 );

    QStringList const expected{
        QString::fromUtf8( "Привет" ), QString::fromUtf8( "日本語" ), QString::fromUtf8( "٠١٢" ) };
    QCOMPARE( captured, expected );

    // Without the option the same pattern is ASCII-only: the row exists so a
    // caller cannot mistake one configuration for the other.
    QRegularExpression const asciiWords( QStringLiteral( "\\w+" ) );
    QVERIFY( !asciiWords.match( QString::fromUtf8( "Привет" ) ).hasMatch() );

    // A supplementary-plane scalar is one match, not two surrogates.
    QRegularExpression const astralExpression( QStringLiteral( "\\x{1F600}" ) );
    QRegularExpressionMatch const astral = astralExpression.match( QString::fromUtf8( "\xf0\x9f\x98\x80" ) );
    QVERIFY( astral.hasMatch() );
    QCOMPARE( astral.captured( 0 ).size(), qsizetype( 2 ) );
}

void TstRegex::literalHexAndOctalTokenMigrationIsPinned()
{
    // Qt6/PCRE2 read two hex digits: the legacy QRegExp token `\x0041` (which
    // meant 'A') is NUL followed by the literal text "41".
    QString const nulThen41 = QString( QChar( 0 ) ) + QStringLiteral( "41" );

    QRegularExpression const legacyHex( QStringLiteral( "\\x0041" ) );
    QCOMPARE( legacyHex.match( nulThen41 ).captured( 0 ), nulThen41 );
    QVERIFY( !legacyHex.match( QStringLiteral( "A" ) ).hasMatch() );

    // The migrated spelling matches the intended character and rejects the
    // two-digit misreading.
    QRegularExpression const migratedHex( QStringLiteral( "\\x{0041}" ) );
    QCOMPARE( migratedHex.match( QStringLiteral( "A" ) ).captured( 0 ), QStringLiteral( "A" ) );
    QVERIFY( !migratedHex.match( nulThen41 ).hasMatch() );

    // Same hazard for the three-digit octal token: `\0101` was 'A' in QRegExp,
    // but PCRE2 takes at most two octal digits after `\0`.
    QString const backspaceThen1 = QString( QChar( 0x08 ) ) + QStringLiteral( "1" );

    QRegularExpression const legacyOctal( QStringLiteral( "\\0101" ) );
    QVERIFY( !legacyOctal.match( QStringLiteral( "A" ) ).hasMatch() );
    QCOMPARE( legacyOctal.match( backspaceThen1 ).captured( 0 ), backspaceThen1 );

    QRegularExpression const migratedOctal( QStringLiteral( "\\o{101}" ) );
    QCOMPARE( migratedOctal.match( QStringLiteral( "A" ) ).captured( 0 ), QStringLiteral( "A" ) );
}

void TstRegex::unmatchedOptionalCaptureIsEmpty()
{
    // An absent optional group is empty for a replacement backreference and has
    // no subject position; capturedTexts() stops at the last captured group, so
    // code that indexes it blindly must not assume the group list is padded.
    QRegularExpression const expression( QStringLiteral( "^(a)?b$" ) );
    QRegularExpressionMatch const match = expression.match( QStringLiteral( "b" ) );

    QVERIFY( match.hasMatch() );
    QCOMPARE( match.captured( 1 ), QString() );
    QCOMPARE( match.capturedStart( 1 ), qsizetype( -1 ) );
    QCOMPARE( match.lastCapturedIndex(), 0 );

    QString const assembled = QStringLiteral( "[" ) + match.captured( 1 ) + QStringLiteral( "]" );
    QCOMPARE( assembled, QStringLiteral( "[]" ) );
}

void TstRegex::invalidPatternIsRejectedWithDiagnostic()
{
    // An invalid built-in pattern is a programming failure: it must be detected
    // at construction, carry pattern and error for the runtime log, and must not
    // quietly match everything.
    QRegularExpression const broken( QStringLiteral( "([" ) );

    QVERIFY( !broken.isValid() );
    QVERIFY( !broken.errorString().isEmpty() );
    QCOMPARE( broken.pattern(), QStringLiteral( "([" ) );
    QVERIFY( !broken.match( QStringLiteral( "anything" ) ).hasMatch() );

    // The same rule applies to a pattern produced by the wildcard conversion:
    // an unmatched bracket yields an invalid expression the caller must reject.
    QString const converted = QRegularExpression::wildcardToRegularExpression(
        QStringLiteral( "[" ), QRegularExpression::NonPathWildcardConversion );
    QRegularExpression const fromWildcard( converted );

    QVERIFY( !fromWildcard.isValid() );
    QVERIFY( !fromWildcard.errorString().isEmpty() );
}

QTEST_GUILESS_MAIN( TstRegex )

#include "tst_regex.moc"
