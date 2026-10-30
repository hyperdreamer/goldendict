/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Source-tagged regex migration ledger suite.
 *
 * `fixtures/regex/cases.json` is the machine-readable half of the migration
 * ledger: one row per pattern family with its source location, old engine and
 * options, the migrated pattern and options, and fixed expected
 * matches/captures/replacement/parts. This suite compiles every row and checks
 * the pinned expectations, then states the traps the migration must not walk into
 * with their own test names so a regression is not buried in a data loop.
 *
 * The engine helpers that own these patterns are converted by their owner tasks
 * (Task 9 for the dictionary parser families, Task 10 for the FTS normalizers); a
 * row whose consumer has not moved yet names its `productionOwner` and is a fixed
 * expectation for that task, not a test-only copy of its logic. A row already
 * exercised by a production-linked suite names that `productionRoute` instead. A
 * row with neither is an explicit generic demonstration anchored to a fixture pin.
 *
 * The FTS half added by Task 10 links ftshelpers.cc, fulltextsearch.cc and
 * articleurls.cc as production sources: parseSearchString() builds the
 * FtsHighlightSpec, the URL contract has to reproduce the same compiled
 * expression, and the production dialog is constructed with a plain QWidget
 * parent. The suite therefore needs a QApplication, which the aggregate supplies
 * through xvfb.
 *
 * Wildcard rows are deliberately split: rows with `wildcard` drive
 * QRegularExpression::wildcardToRegularExpression (the API the owner uses), rows
 * with `expectedNormalizerPattern` pin the *preserved* headword grammar of
 * wildcard.cc, and rows marked `converterDifference` record the places where the
 * converter reads a spelling differently. A converter demonstration is never
 * treated as the required production outcome.
 */

#include <QtTest>

#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStringList>
#include <QWidget>

#include <vector>

#include "articleurls.hh"
#include "config.hh"
#include "ftshelpers.hh"
#include "fulltextsearch.hh"
#include "instances.hh"
#include "wildcard.hh"
#include "support/fixtures.hh"

#ifdef GD_SAVE_COLLECTOR_AVAILABLE
#include "savearticle.hh"
#endif

namespace
{

constexpr char const * FixtureFile = "regex/cases.json";

/// Hard bound on one scan. globalMatch advances a zero-length match by itself,
/// so hitting this bound means a pattern failed to progress and the suite must
/// fail instead of hanging.
constexpr int MaxMatchesPerScan = 10000;

/// Floor for the ledger size: it must keep covering every pinned family rather
/// than silently shrinking to a handful of rows.
constexpr int MinimumLedgerRows = 55;

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

/// Translates a reported reverse-search limit into an explicit subject-relative
/// index. QRegExp's negative limit counts back from the end of the subject (-1 is
/// the last character position, -2 the one before it), so it is translated here
/// instead of being handed to a match offset that would mean something else.
qsizetype subjectOffsetFor( int limit, QString const & subject )
{
    if ( limit >= 0 )
        return limit;

    qsizetype const translated = subject.size() + limit;

    return translated < 0 ? 0 : translated;
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
    QString const normalizerPattern = row.value( QStringLiteral( "expectedNormalizerPattern" ) ).toString();

    if ( !normalizerPattern.isEmpty() )
    {
        // The preserved headword grammar: the authored pattern is the literal the
        // production normalizer must keep producing. It is never computed from a
        // converter call, so a change in either grammar shows up here.
        if ( !wildcardMode.isEmpty() )
        {
            problems << QStringLiteral( "%1: a preserved normalizer row cannot also drive the Qt converter" )
                            .arg( id );
            return;
        }

        if ( row.value( QStringLiteral( "converterDiffers" ) ).toBool( false ) )
        {
            QString const converted = QRegularExpression::wildcardToRegularExpression(
                pattern, QRegularExpression::NonPathWildcardConversion );

            if ( converted == normalizerPattern )
                problems << QStringLiteral( "%1: the Qt converter no longer differs from the preserved "
                                            "normalizer grammar; the recorded difference is stale" )
                                .arg( id );
        }

        QString const actualPattern = wildcardsToRegexp( pattern );
        if ( actualPattern != normalizerPattern )
            problems << QStringLiteral( "%1: production wildcard grammar changed: got %2, expected %3" )
                            .arg( id, display( actualPattern ), display( normalizerPattern ) );
        expressionText = actualPattern;
    }
    else if ( !wildcardMode.isEmpty() )
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

    if ( row.contains( QStringLiteral( "splitMode" ) ) )
    {
        QString const mode = row.value( QStringLiteral( "splitMode" ) ).toString();
        QStringList expected;

        for ( QJsonValue const & value : row.value( QStringLiteral( "expectedParts" ) ).toArray() )
            expected << value.toString();

        if ( mode != QLatin1String( "keepEmpty" ) && mode != QLatin1String( "skipEmpty" ) )
            problems << QStringLiteral( "%1: unknown split mode '%2'" ).arg( id, mode );
        else
        {
            QStringList const parts = subject.split( expression,
                mode == QLatin1String( "keepEmpty" ) ? Qt::KeepEmptyParts : Qt::SkipEmptyParts );

            if ( parts != expected )
                problems << QStringLiteral( "%1: split mismatch\n  actual  : %2\n  expected: %3" )
                                .arg( id, renderList( parts ), renderList( expected ) );
        }
    }

    if ( row.value( QStringLiteral( "filterList" ) ).toBool( false ) )
    {
        QStringList input;

        for ( QJsonValue const & value : row.value( QStringLiteral( "list" ) ).toArray() )
            input << value.toString();

        QStringList expected;

        for ( QJsonValue const & value : row.value( QStringLiteral( "expectedParts" ) ).toArray() )
            expected << value.toString();

        QStringList const filtered = input.filter( expression );

        if ( filtered != expected )
            problems << QStringLiteral( "%1: filter mismatch\n  actual  : %2\n  expected: %3" )
                            .arg( id, renderList( filtered ), renderList( expected ) );
    }

    if ( row.contains( QStringLiteral( "reverseSearchOffset" ) ) )
    {
        int const limit = row.value( QStringLiteral( "reverseSearchOffset" ) ).toInt();
        qsizetype const offset = subjectOffsetFor( limit, subject );
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
                problems << QStringLiteral( "%1: reverse search at limit %2 (subject offset %3) chose '%4' "
                                            "(found=%5), expected '%6'" )
                                .arg( id )
                                .arg( limit )
                                .arg( offset )
                                .arg( display( chosen ) )
                                .arg( found )
                                .arg( display( expected ) );
        }
        else if ( row.value( QStringLiteral( "expectedReverseMatchCount" ) ).toInt() == 0 && found )
        {
            problems << QStringLiteral( "%1: reverse search at limit %2 (subject offset %3) unexpectedly "
                                        "chose '%4'" )
                            .arg( id )
                            .arg( limit )
                            .arg( offset )
                            .arg( display( chosen ) );
        }
    }
}

/// One scan of `expression` over `subject`, reported as "start+length" pairs so a
/// comparison never depends on the conversion that produced the expression.
QStringList matchSpans( QRegularExpression const & expression, QString const & subject )
{
    QStringList spans;

    QRegularExpressionMatchIterator iterator = expression.globalMatch( subject );

    while ( iterator.hasNext() )
    {
        QRegularExpressionMatch const match = iterator.next();
        spans << QStringLiteral( "%1+%2" ).arg( match.capturedStart() ).arg( match.capturedLength() );
    }

    return spans;
}

} // namespace

class TstRegex : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();

    void ledgerProvenanceIsComplete();
    void sourceTagsCarryTheirOriginalPattern();
    void ledgerRowsMatchMigratedSemantics();
    void requiredPinnedRowsArePresent();

    void greedyAndMinimalCountsAreDistinguished();
    void overlappingQStringCountIsDistinguished();
    void splitAndFilterModesArePinned();
    void negativeReverseLimitsAreTranslated();
    void wildcardNormalizerGrammarIsPinned();
    void anchoredPatternRejectsTrailingNewline();
    void zeroWidthMatchingTerminates();
    void unicodeCapturesPreserveExactText();
    void literalHexAndOctalTokenMigrationIsPinned();
    void unmatchedOptionalCaptureIsEmpty();
    void builtInPatternValidationReportsItsError();
    void invalidPatternIsRejectedWithDiagnostic();

    void ftsSourceModesCompileDifferentExpressions();
    void ftsMatchCaseRecompilesTheExpression();
    void ftsInvalidUserPatternIsAValidationError();
    void ftsEmptyUserPatternKeepsLegacyMatchAll();
    void ftsWildcardHighlightRoundTripsThroughUrlContract();
    void ftsWordModesKeepQtWordClassification();
    void ftsConnectorPunctuationIsNotAWordCharacter();
    void ftsArticleSeparatorKeepsItsOwnWordClass();
    void ftsRegexpModeKeepsNewlineAndMinimalSemantics();
    void ftsEscapeTokensAreStrippedFromIndexWords();
    void ftsSurrogatePairsKeepScalarSafeOffsets();
    void ftsArticleIndexKeepsScalarWords();
    void ftsResultHighlightIsRebuiltFromHilites();
    void ftsDialogUsesSuppliedTextWithAPlainParent();

#ifdef GD_SAVE_COLLECTOR_AVAILABLE
    void saveCollector();
#endif
};

void TstRegex::initTestCase()
{
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
    QVERIFY2( TestSupport::fixtureExists( QString::fromLatin1( FixtureFile ) ),
        qPrintable( QStringLiteral( "missing fixture %1" )
                .arg( TestSupport::fixturePath( QString::fromLatin1( FixtureFile ) ) ) ) );

    // The FTS dialog is a real QWidget dialog: its constructor test needs a
    // QApplication and a display. Required, never skipped, so an environment
    // without one fails with a reason instead of silently passing.
    QString displayReason;
    QVERIFY2( TestSupport::prerequisiteAvailable( QStringLiteral( "display" ), &displayReason ),
        qPrintable( displayReason ) );

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

void TstRegex::sourceTagsCarryTheirOriginalPattern()
{
    // A production file:line tag must tell a later owner what to preserve: the
    // exact original pattern text and either the task that owns the consumer or
    // the production suite that already exercises the behaviour. A row without a
    // live consumer is anchored to a fixture pin instead of being attached to an
    // unrelated production pattern.
    QJsonArray const rows = loadRows();
    QVERIFY2( !rows.isEmpty(), "regex/cases.json carries no rows" );

    QStringList problems;
    int ownerRows = 0;
    int routeRows = 0;
    int pinRows = 0;
    int productionRows = 0;

    for ( QJsonValue const & value : rows )
    {
        QJsonObject const row = value.toObject();
        QString const id = row.value( QStringLiteral( "id" ) ).toString();
        QString const source = row.value( QStringLiteral( "source" ) ).toString();

        if ( source.startsWith( QLatin1String( "tests/fixtures/regex/cases.json#" ) ) )
        {
            ++pinRows;
            continue;
        }

        ++productionRows;

        QString const oldPattern = row.value( QStringLiteral( "oldPattern" ) ).toString();

        if ( oldPattern.isEmpty() )
            problems << QStringLiteral( "%1: a production-tagged row must record the original pattern "
                                        "at '%2'" )
                            .arg( id, source );

        QString const owner = row.value( QStringLiteral( "productionOwner" ) ).toString();
        QString const route = row.value( QStringLiteral( "productionRoute" ) ).toString();

        if ( owner.isEmpty() && route.isEmpty() )
            problems << QStringLiteral( "%1: '%2' names neither the task that owns this consumer nor a "
                                        "production-linked suite" )
                            .arg( id, source );

        if ( !owner.isEmpty() )
            ++ownerRows;

        if ( !route.isEmpty() )
        {
            ++routeRows;

            if ( route != QLatin1String( "linkrewrite" ) )
                problems << QStringLiteral( "%1: unknown production route '%2'" ).arg( id, route );
        }

        // A recorded converter difference must be marked, so it is never read as
        // the required production outcome.
        if ( row.value( QStringLiteral( "converterDifference" ) ).toBool( false ) &&
             !row.contains( QStringLiteral( "wildcard" ) ) )
            problems << QStringLiteral( "%1: a converter difference must drive the converter" ).arg( id );
    }

    // Every mapping kind must be exercised, so this check cannot pass by giving
    // every row the same label.
    if ( ownerRows < 5 )
        problems << QStringLiteral( "only %1 rows name a deferred production owner" ).arg( ownerRows );

    if ( routeRows < 4 )
        problems << QStringLiteral( "only %1 rows name a production-linked route" ).arg( routeRows );

    if ( pinRows < 6 )
        problems << QStringLiteral( "only %1 rows are explicit fixture pins; generic demonstrations must be "
                                    "anchored, not attached to unrelated production patterns" )
                        .arg( pinRows );

    if ( productionRows < 10 )
        problems << QStringLiteral( "only %1 rows carry a production anchor" ).arg( productionRows );

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
        QStringLiteral( "reverse-search-limit-on-match-start" ),
        QStringLiteral( "reverse-search-negative-limit-counts-from-end" ),
        QStringLiteral( "reverse-search-negative-limit-no-match" ),
        QStringLiteral( "qstring-count-overlaps-are-not-matched-characters" ),
        QStringLiteral( "split-keep-empty-parts" ),
        QStringLiteral( "split-skip-empty-parts" ),
        QStringLiteral( "filter-word-minimum-length-ucp" ),
        QStringLiteral( "fts-escape-token-hex-capture" ),
        QStringLiteral( "fts-escape-token-octal-capture" ),
        QStringLiteral( "wildcard-nonpath-star-crosses-slash" ),
        QStringLiteral( "wildcard-normalizer-unmatched-bracket-is-literal" ),
        QStringLiteral( "wildcard-normalizer-escaped-star-is-literal" ),
        QStringLiteral( "qt-wildcard-converter-escaped-star-differs" ),
        QStringLiteral( "qt-wildcard-converter-unmatched-bracket-is-invalid" ),
        QStringLiteral( "invalid-builtin-pattern-is-rejected" ),
        QStringLiteral( "unicode-combining-mc-mark-is-not-pcre-word" ),
        QStringLiteral( "unicode-combining-me-mark-is-not-pcre-word" ),
        QStringLiteral( "unicode-combining-mark-stays-inside-the-word" ),
        QStringLiteral( "space-nbsp-needs-the-unicode-option" ),
        QStringLiteral( "space-nbsp-with-the-unicode-option" ),
        QStringLiteral( "space-em-space-with-the-unicode-option" ),
        QStringLiteral( "space-separators-stay-separators-in-fts" ),
        QStringLiteral( "dot-needs-the-everything-option-for-newlines" ),
        QStringLiteral( "dot-matches-newline-with-the-everything-option" ),
        QStringLiteral( "fts-escape-token-short-hex-is-not-a-token" ),
        QStringLiteral( "fts-symbol-set-is-removed-from-index-words" ),
        QStringLiteral( "fts-word-class-excludes-connector-punctuation" ),
        QStringLiteral( "fts-word-class-keeps-underscore" ),
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
    // greedy pattern. The raw QString::count overload reports three for *both*
    // configurations here because it counts overlapping matches, which is a
    // third, different measure and is pinned separately by
    // overlappingQStringCountIsDistinguished().
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

void TstRegex::overlappingQStringCountIsDistinguished()
{
    // QString::count(QRegularExpression) counts *overlapping* matches; it is not
    // a run count and not a character count. Enumerating matches with globalMatch
    // is the only way to get the non-overlapping runs, so both measures are
    // pinned here instead of describing one as the other (spec 4.3 states 2 for
    // the greedy example below; Qt 6.12 returns 3).
    QString const overlapping = QStringLiteral( "aaa" );

    int runs = 0;
    QRegularExpressionMatchIterator iterator =
        QRegularExpression( QStringLiteral( "aa" ) ).globalMatch( overlapping );

    while ( iterator.hasNext() )
    {
        ++runs;
        iterator.next();
    }

    QCOMPARE( runs, 1 );
    QCOMPARE( overlapping.count( QRegularExpression( QStringLiteral( "aa" ) ) ), qsizetype( 2 ) );

    // Two runs, four matched characters: neither overload reports the character
    // total.
    QString const twoRuns = QStringLiteral( "abab" );
    QCOMPARE( twoRuns.count( QRegularExpression( QStringLiteral( "ab" ) ) ), qsizetype( 2 ) );

    QStringList matched;
    QRegularExpressionMatchIterator pairIterator =
        QRegularExpression( QStringLiteral( "ab" ) ).globalMatch( twoRuns );

    while ( pairIterator.hasNext() )
        matched << pairIterator.next().captured( 0 );

    QCOMPARE( matched.size(), 2 );
    QCOMPARE( matched.join( QString() ).size(), qsizetype( 4 ) );
}

void TstRegex::splitAndFilterModesArePinned()
{
    // Both split modes are named and fixed: a swap is a failure, not a style
    // choice. KeepEmptyParts keeps the trailing empty run, SkipEmptyParts drops
    // it.
    QString const words = QStringLiteral( "a b  c " );
    QRegularExpression const separators( QStringLiteral( "\\W+" ) );

    QStringList const keep = words.split( separators, Qt::KeepEmptyParts );
    QStringList const skip = words.split( separators, Qt::SkipEmptyParts );

    QCOMPARE( keep, QStringList( { QStringLiteral( "a" ), QStringLiteral( "b" ), QStringLiteral( "c" ),
        QString() } ) );
    QCOMPARE( skip,
        QStringList( { QStringLiteral( "a" ), QStringLiteral( "b" ), QStringLiteral( "c" ) } ) );

    QStringList const lines =
        QStringLiteral( "line1\n\nline2" ).split( QRegularExpression( QStringLiteral( "[\\r\\n]" ) ),
            Qt::KeepEmptyParts );
    QCOMPARE( lines, QStringList( { QStringLiteral( "line1" ), QString(), QStringLiteral( "line2" ) } ) );

    // list.filter keeps the whole item a match occurs in, and both its case and
    // Unicode options are observable.
    QStringList const names{ QStringLiteral( "Alpha" ), QStringLiteral( "beta" ), QStringLiteral( "ALPHABET" ) };
    QCOMPARE( names.filter( QRegularExpression( QStringLiteral( "^alpha" ),
                             QRegularExpression::CaseInsensitiveOption ) ),
        QStringList( { QStringLiteral( "Alpha" ), QStringLiteral( "ALPHABET" ) } ) );
    QVERIFY( names.filter( QRegularExpression( QStringLiteral( "^alpha" ) ) ).isEmpty() );

    QStringList const unicodeWords{
        QString::fromUtf8( "Привет" ), QStringLiteral( "a" ), QString::fromUtf8( "日本語です" )
    };
    QCOMPARE( unicodeWords.filter( QRegularExpression( QStringLiteral( "\\w{4,}" ),
                                 QRegularExpression::UseUnicodePropertiesOption ) ),
        QStringList( { QString::fromUtf8( "Привет" ), QString::fromUtf8( "日本語です" ) } ) );
    QVERIFY( unicodeWords.filter( QRegularExpression( QStringLiteral( "\\w{4,}" ) ) ).isEmpty() );
}

void TstRegex::negativeReverseLimitsAreTranslated()
{
    // A negative reverse limit counts back from the end of the subject (-1 is the
    // last character position). It must be translated before it is compared with
    // a match start; passing it through would silently select nothing.
    QString const subject = QStringLiteral( "a1 b22 c333" );
    QRegularExpression const digits( QStringLiteral( "\\d+" ) );

    QStringList starts;
    QRegularExpressionMatchIterator iterator = digits.globalMatch( subject );

    while ( iterator.hasNext() )
        starts << QString::number( iterator.next().capturedStart( 0 ) );

    QCOMPARE( starts, QStringList( { QStringLiteral( "1" ), QStringLiteral( "4" ), QStringLiteral( "8" ) } ) );

    struct Case
    {
        int limit;
        qsizetype offset;
        char const * expected;
    };

    Case const cases[] = {
        { -1, subject.size() - 1, "333" },
        { -4, subject.size() - 4, "22" },
        { 4, 4, "22" },
        { 3, 3, "1" },
    };

    for ( Case const & one : cases )
    {
        qsizetype const translated = subjectOffsetFor( one.limit, subject );
        QCOMPARE( translated, one.offset );

        QString chosen;
        bool found = false;

        QRegularExpressionMatchIterator scan = digits.globalMatch( subject );

        while ( scan.hasNext() )
        {
            QRegularExpressionMatch const match = scan.next();

            if ( match.capturedStart( 0 ) <= translated )
            {
                found = true;
                chosen = match.captured( 0 );
            }
        }

        QVERIFY2( found, one.expected );
        QCOMPARE( chosen, QString::fromLatin1( one.expected ) );
    }

    // The driver's own translation is pinned directly: -1 is the last character
    // position, and a limit before the subject start is clamped to 0 instead of
    // becoming a wildly out-of-range offset.
    QCOMPARE( subjectOffsetFor( -1, subject ), subject.size() - 1 );
    QCOMPARE( subjectOffsetFor( -4, subject ), subject.size() - 4 );
    QCOMPARE( subjectOffsetFor( 4, subject ), qsizetype( 4 ) );
    QCOMPARE( subjectOffsetFor( -20, QStringLiteral( "abc 1" ) ), qsizetype( 0 ) );

    // A subject without a match is an empty result rather than an error.
    QRegularExpressionMatchIterator none = digits.globalMatch( QStringLiteral( "abc" ) );
    QVERIFY( !none.hasNext() );
}

void TstRegex::wildcardNormalizerGrammarIsPinned()
{
    // wildcard.cc keeps its own escape grammar for headword patterns. Its outputs
    // are authored here, not computed by the converter: an unmatched bracket is
    // escaped and matches literally, and '\*' is a literal star.
    QRegularExpression const literalBracket( QStringLiteral( "\\[" ) );
    QVERIFY( literalBracket.isValid() );
    QVERIFY( literalBracket.match( QStringLiteral( "[" ) ).hasMatch() );

    QRegularExpression const literalStar( QStringLiteral( "a\\*b" ) );
    QVERIFY( literalStar.isValid() );
    QVERIFY( literalStar.match( QStringLiteral( "a*b" ) ).hasMatch() );
    QVERIFY( !literalStar.match( QStringLiteral( "aXb" ) ).hasMatch() );

    // Qt's converter reads both spellings differently, and that difference is
    // recorded as a converter difference rather than as the required outcome.
    QString const convertedBracket =
        QRegularExpression::wildcardToRegularExpression( QStringLiteral( "[" ),
            QRegularExpression::NonPathWildcardConversion );
    QCOMPARE( convertedBracket, QStringLiteral( "\\A(?:(?s:[))\\z" ) );
    QVERIFY( !QRegularExpression( convertedBracket ).isValid() );

    QString const convertedStar =
        QRegularExpression::wildcardToRegularExpression( QStringLiteral( "a\\*b" ),
            QRegularExpression::NonPathWildcardConversion );
    QCOMPARE( convertedStar, QStringLiteral( "\\A(?:(?s:a\\\\.*b))\\z" ) );
    QVERIFY( convertedStar != QStringLiteral( "a\\*b" ) );
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

void TstRegex::builtInPatternValidationReportsItsError()
{
    QTest::ignoreMessage( QtWarningMsg,
        QRegularExpression( "^Invalid built-in regular expression .*\\[:.*$" ) );
    QRegularExpression const invalid = Regex::validated( QStringLiteral( "[" ) );
    QVERIFY( !invalid.isValid() );
    QVERIFY( !invalid.errorString().isEmpty() );
    QRegularExpression const valid = Regex::validated( QStringLiteral( "\\d+" ),
        QRegularExpression::UseUnicodePropertiesOption );
    QVERIFY( valid.isValid() );
    QVERIFY( valid.match( QStringLiteral( "١٢" ) ).hasMatch() );
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

    // The same rule applies to a converter-produced pattern: an unmatched bracket
    // makes wildcardToRegularExpression emit an unterminated class, which a caller
    // of the converter must reject. This is a recorded converter difference - the
    // preserved headword grammar escapes the bracket instead and stays valid (see
    // wildcardNormalizerGrammarIsPinned).
    QString const converted = QRegularExpression::wildcardToRegularExpression(
        QStringLiteral( "[" ), QRegularExpression::NonPathWildcardConversion );
    QRegularExpression const fromWildcard( converted );

    QVERIFY( !fromWildcard.isValid() );
    QVERIFY( !fromWildcard.errorString().isEmpty() );
}

void TstRegex::ftsSourceModesCompileDifferentExpressions()
{
    // The wildcard flag is a value, not a property of the compiled object: the
    // same source text has to produce different expressions in the two modes and
    // neither may be guessed from the other one.
    QStringList indexWords, searchWords;
    bool hasCJK = false;

    // "a*b" is the discriminator: as a regular expression it is "zero or more
    // a, then b", as a wildcard it is "a, then anything, then b".
    FtsHighlightSpec regexpSpec;
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "a*b" ), indexWords, searchWords,
                                            regexpSpec, FTS::RegExp, false, -1, hasCJK ) );
    QCOMPARE( regexpSpec.sourcePattern, QStringLiteral( "a*b" ) );
    QVERIFY( !regexpSpec.wildcards );
    QVERIFY( regexpSpec.expression.isValid() );
    QCOMPARE( regexpSpec.expression.pattern(), QStringLiteral( "a*b" ) );
    // Unanchored as before, so the discriminating observation is the span:
    // "a*b" as a regular expression can only find the trailing 'b'.
    QString const modeSubject = QStringLiteral( "aaaxxxb" );
    QCOMPARE( matchSpans( regexpSpec.expression, modeSubject ),
              QStringList( { QStringLiteral( "6+1" ) } ) );

    FtsHighlightSpec wildcardSpec;
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "a*b" ), indexWords, searchWords,
                                            wildcardSpec, FTS::Wildcards, false, -1, hasCJK ) );
    // Both modes keep the user's text verbatim; only the compiled expression
    // differs, which is exactly why the specification carries both.
    QCOMPARE( wildcardSpec.sourcePattern, regexpSpec.sourcePattern );
    QVERIFY( wildcardSpec.wildcards );
    QVERIFY( wildcardSpec.expression.isValid() );
    QVERIFY( wildcardSpec.expression.pattern() != regexpSpec.expression.pattern() );
    // The same subject under wildcard syntax is one run from the first 'a' to
    // the last 'b'.
    QCOMPARE( matchSpans( wildcardSpec.expression, modeSubject ),
              QStringList( { QStringLiteral( "0+7" ) } ) );

    // NonPathWildcardConversion: a dictionary headword may contain '/', so the
    // star has to cross it. The subject puts the slash inside the starred run,
    // which a path-aware conversion refuses to cross.
    QCOMPARE( matchSpans( wildcardSpec.expression, QStringLiteral( "x/a/xxb" ) ),
              QStringList( { QStringLiteral( "2+5" ) } ) );

    // UnanchoredWildcardConversion: the highlight has to match inside article
    // text instead of only as a whole subject.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "a*/b" ), indexWords, searchWords,
                                            wildcardSpec, FTS::Wildcards, false, -1, hasCJK ) );
    QVERIFY( wildcardSpec.expression.match( QStringLiteral( "alpha/b" ) ).hasMatch() );
    QCOMPARE( matchSpans( wildcardSpec.expression, QStringLiteral( "see alpha/b now" ) ),
              QStringList( { QStringLiteral( "4+7" ) } ) );
}

void TstRegex::ftsMatchCaseRecompilesTheExpression()
{
    QStringList indexWords, searchWords;
    bool hasCJK = false;

    FtsHighlightSpec sensitive, insensitive;
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "cat" ), indexWords, searchWords,
                                            sensitive, FTS::RegExp, true, -1, hasCJK ) );
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "cat" ), indexWords, searchWords,
                                            insensitive, FTS::RegExp, false, -1, hasCJK ) );

    // The source text is never case-folded or re-spelled; the case flag is a
    // compilation option, so the identical text recompiles.
    QCOMPARE( sensitive.sourcePattern, QStringLiteral( "cat" ) );
    QCOMPARE( insensitive.sourcePattern, sensitive.sourcePattern );
    QVERIFY( sensitive.matchCase );
    QVERIFY( !insensitive.matchCase );
    QCOMPARE( insensitive.expression.pattern(), sensitive.expression.pattern() );

    QVERIFY( bool( insensitive.expression.patternOptions() & QRegularExpression::CaseInsensitiveOption ) );
    QVERIFY( !bool( sensitive.expression.patternOptions() & QRegularExpression::CaseInsensitiveOption ) );

    QVERIFY( insensitive.expression.match( QStringLiteral( "CAT" ) ).hasMatch() );
    QVERIFY( !sensitive.expression.match( QStringLiteral( "CAT" ) ).hasMatch() );
    QVERIFY( sensitive.expression.match( QStringLiteral( "cat" ) ).hasMatch() );
}

void TstRegex::ftsInvalidUserPatternIsAValidationError()
{
    // A malformed user pattern is reported, never turned into an empty
    // expression that would match every article (the legacy QRegExp fallback).
    for ( int const mode : { FTS::RegExp, FTS::Wildcards } )
    {
        QStringList indexWords, searchWords;
        bool hasCJK = false;
        FtsHighlightSpec spec;

        // A previous valid search must not survive the failed parse.
        QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "cat" ), indexWords, searchWords,
                                                spec, mode, false, -1, hasCJK ) );
        QVERIFY( spec.expression.isValid() );

        QVERIFY( !FtsHelpers::parseSearchString( QStringLiteral( "[" ), indexWords, searchWords,
                                                 spec, mode, false, -1, hasCJK ) );
        // The source is kept so the dialog can show what was rejected, and the
        // invalid expression matches nothing.
        QCOMPARE( spec.sourcePattern, QStringLiteral( "[" ) );
        QVERIFY( !spec.expression.isValid() );
        QVERIFY( !spec.expression.errorString().isEmpty() );
        QVERIFY( indexWords.isEmpty() );
        QVERIFY( searchWords.isEmpty() );
    }

    // A failure before the pattern is compiled must not leave an earlier
    // search's specification behind: CJK text is refused by the word modes
    // before any expression is built.
    QStringList indexWords, searchWords;
    bool hasCJK = false;
    FtsHighlightSpec spec;
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "cat" ), indexWords, searchWords, spec,
                                            FTS::RegExp, false, -1, hasCJK ) );
    QCOMPARE( spec.sourcePattern, QStringLiteral( "cat" ) );
    QVERIFY( !FtsHelpers::parseSearchString( QString::fromUtf8( "\xe4\xb8\xad" ), indexWords,
                                             searchWords, spec, FTS::WholeWords, false, -1, hasCJK ) );
    QVERIFY( hasCJK );
    QVERIFY( spec.sourcePattern.isEmpty() );
    QVERIFY( spec.expression.pattern().isEmpty() );
}

void TstRegex::ftsEmptyUserPatternKeepsLegacyMatchAll()
{
    // The legacy QRegExp( "" ) was valid and matched every position, so an empty
    // search line stays a match-all expression; only a malformed pattern is a
    // validation error.
    QStringList indexWords, searchWords;
    bool hasCJK = false;
    FtsHighlightSpec spec;

    QVERIFY( FtsHelpers::parseSearchString( QString(), indexWords, searchWords, spec,
                                            FTS::RegExp, false, -1, hasCJK ) );
    QVERIFY( spec.sourcePattern.isEmpty() );
    QVERIFY( spec.expression.isValid() );
    QVERIFY( spec.expression.pattern().isEmpty() );
    QVERIFY( spec.expression.match( QStringLiteral( "anything at all" ) ).hasMatch() );
    QVERIFY( indexWords.isEmpty() );
    QVERIFY( searchWords.isEmpty() );
    QCOMPARE( matchSpans( spec.expression, QStringLiteral( "abc" ) ).size(), 4 );

    // The word modes still refuse an input without a usable index word.
    QVERIFY( !FtsHelpers::parseSearchString( QString(), indexWords, searchWords, spec,
                                             FTS::WholeWords, false, -1, hasCJK ) );
    QVERIFY( !FtsHelpers::parseSearchString( QString(), indexWords, searchWords, spec,
                                             FTS::PlainText, false, -1, hasCJK ) );
}

void TstRegex::ftsWildcardHighlightRoundTripsThroughUrlContract()
{
    // The dialog hands the page an FtsHighlightSpec, so the URL contract has to
    // carry the source text, the wildcard flag and the case flag, and reproduce
    // the same compiled expression on the way back in.
    for ( bool const matchCase : { false, true } )
    {
        QStringList indexWords, searchWords;
        bool hasCJK = false;
        FtsHighlightSpec productionSpec;
        QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "a*/b" ), indexWords, searchWords,
                                                productionSpec, FTS::Wildcards, matchCase, -1,
                                                hasCJK ) );
        QVERIFY( productionSpec.expression.isValid() );

        ArticleUrls::Lookup lookup;
        lookup.phrase = Config::InputPhrase::fromPhrase( QStringLiteral( "headword" ) );
        lookup.group = 3;
        lookup.groupSpecified = true;
        lookup.highlight = productionSpec;

        QUrl const url = ArticleUrls::lookupDocument( lookup );
        QCOMPARE( url.scheme(), QStringLiteral( "gdlookup" ) );

        ArticleUrls::Parsed const parsed = ArticleUrls::parse( url, ArticleUrls::Use::DocumentLoad );
        QCOMPARE( parsed.kind, ArticleUrls::Kind::LookupDocument );
        QVERIFY2( parsed.error.isEmpty(), qPrintable( parsed.error ) );
        QCOMPARE( parsed.lookup.highlight.sourcePattern, productionSpec.sourcePattern );
        QCOMPARE( parsed.lookup.highlight.wildcards, productionSpec.wildcards );
        QCOMPARE( parsed.lookup.highlight.matchCase, productionSpec.matchCase );
        QVERIFY( parsed.lookup.highlight.expression.isValid() );

        QString const subject = QStringLiteral( "see alpha/b now" );
        QStringList const expectedSpans = matchSpans( productionSpec.expression, subject );
        QVERIFY2( !expectedSpans.isEmpty(), "the wildcard highlight matches nothing at all" );
        QCOMPARE( matchSpans( parsed.lookup.highlight.expression, subject ), expectedSpans );
    }
}

void TstRegex::ftsWordModesKeepQtWordClassification()
{
    // QRegExp split with its own word table, which counted every mark category as
    // a word character and every QChar::isSpace() member as a separator. PCRE's
    // \W under Unicode properties counts Mn but not Mc/Me, so the migrated
    // classes spell the marks out; these subjects are where the two disagree.
    // The query is NFC-normalized before it is split (unchanged baseline), so
    // the first word is the composed form of "cafe" + U+0301; the spacing mark
    // has no composed form and has to stay inside its own word.
    QString const input = QString::fromUtf8( "cafe\xcc\x81 abcd\xc2\xa0" "efgh\xe2\x80\x83"
                                             "a\xe0\xa4\x83" "bcd" );
    QStringList const expectedWords{
        QString::fromUtf8( "caf\xc3\xa9" ), QStringLiteral( "abcd" ), QStringLiteral( "efgh" ),
        QString::fromUtf8( "a\xe0\xa4\x83" "bcd" )
    };

    QStringList indexWords, searchWords;
    bool hasCJK = false;
    FtsHighlightSpec spec;

    QVERIFY( FtsHelpers::parseSearchString( input, indexWords, searchWords, spec,
                                            FTS::WholeWords, false, -1, hasCJK ) );
    QVERIFY( !hasCJK );
    QCOMPARE( searchWords, expectedWords );
    QCOMPARE( indexWords, expectedWords );
    // The composed highlight source is a regular expression, not a wildcard.
    QVERIFY( !spec.wildcards );
    QVERIFY( spec.expression.isValid() );

    // The whole sequence is found in article text written with the same
    // separators.
    QVERIFY( spec.expression.match( input ).hasMatch() );

    // Whole-word boundaries keep QRegExp's meaning: a spacing combining mark
    // belongs to the word, so it neither starts nor ends one. PCRE's \b would
    // accept both positions, because Mc is not a PCRE word character.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "cafe" ), indexWords, searchWords, spec,
                                            FTS::WholeWords, false, -1, hasCJK ) );
    QVERIFY( !spec.matchCase );
    QVERIFY( bool( spec.expression.patternOptions() & QRegularExpression::CaseInsensitiveOption ) );
    QVERIFY( spec.expression.match( QStringLiteral( "cafe!" ) ).hasMatch() );
    QVERIFY( spec.expression.match( QStringLiteral( "CAFE!" ) ).hasMatch() );
    QVERIFY( !spec.expression.match( QStringLiteral( "xcafe" ) ).hasMatch() );
    QVERIFY( !spec.expression.match( QString::fromUtf8( "cafe\xe0\xa4\x83" ) ).hasMatch() );
    QVERIFY( !spec.expression.match( QString::fromUtf8( "\xe0\xa4\x83" "cafe" ) ).hasMatch() );

    // Plain text keeps the surrounding (?:\w*) runs and has no boundaries.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "cafe" ), indexWords, searchWords, spec,
                                            FTS::PlainText, false, -1, hasCJK ) );
    QCOMPARE( matchSpans( spec.expression, QStringLiteral( "xcafe y" ) ),
              QStringList( { QStringLiteral( "0+5" ) } ) );
}

void TstRegex::ftsConnectorPunctuationIsNotAWordCharacter()
{
    // Qt's word table contains exactly one connector: the underscore. PCRE's \w
    // under Unicode properties also contains U+203F, U+2040, U+2054, U+FE33,
    // U+FE34, U+FE4D, U+FE4E, U+FE4F and U+FF3F, so a class spelled with \w
    // silently merges words that QRegExp kept apart. These are the connector
    // samples the legacy oracle and the migrated expression must agree on.
    // All nine connector-punctuation characters PCRE's \w adds and Qt's word
    // table lacks; sampling fewer would let two of them drift back.
    QChar const connectors[] = { QChar( 0x203f ), QChar( 0x2040 ), QChar( 0x2054 ),
                                 QChar( 0xfe33 ), QChar( 0xfe34 ), QChar( 0xfe4d ),
                                 QChar( 0xfe4e ), QChar( 0xfe4f ), QChar( 0xff3f ) };

    QStringList indexWords, searchWords;
    bool hasCJK = false;
    FtsHighlightSpec spec;

    for( QChar const connector : connectors )
    {
        QString const query = QStringLiteral( "alpha" ) + connector + QStringLiteral( "beta" );

        QVERIFY( FtsHelpers::parseSearchString( query, indexWords, searchWords, spec,
                                                FTS::WholeWords, false, -1, hasCJK ) );
        QVERIFY( !hasCJK );
        // QRegExp split on the connector, so the query is two words, not one.
        QCOMPARE( searchWords, ( QStringList{ QStringLiteral( "alpha" ), QStringLiteral( "beta" ) } ) );
        QCOMPARE( indexWords, searchWords );
    }

    // A single word must still match across a connector on either side, because
    // QRegExp's \b accepted those positions.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "cafe" ), indexWords, searchWords, spec,
                                            FTS::WholeWords, false, -1, hasCJK ) );
    for( QChar const connector : connectors )
    {
        QVERIFY2( spec.expression.match( connector + QStringLiteral( "cafe" ) ).hasMatch(),
                  qPrintable( QStringLiteral( "left boundary lost at U+%1" )
                                .arg( uint( connector.unicode() ), 4, 16, QLatin1Char( '0' ) ) ) );
        QVERIFY2( spec.expression.match( QStringLiteral( "cafe" ) + connector ).hasMatch(),
                  qPrintable( QStringLiteral( "right boundary lost at U+%1" )
                                .arg( uint( connector.unicode() ), 4, 16, QLatin1Char( '0' ) ) ) );
    }

    // The underscore is still a word character in both engines.
    QVERIFY( !spec.expression.match( QStringLiteral( "cafe_" ) ).hasMatch() );
}

void TstRegex::ftsArticleSeparatorKeepsItsOwnWordClass()
{
    // The article/index separator is a different, older contract: the Qt5 build
    // already split articles with QRegularExpression [^\\w\\p{M}]+ under
    // Unicode properties, where a connector joined the word. Correcting the
    // query class must not re-index existing articles, and no
    // CurrentFtsFormatVersion change would announce it if it did.
    QChar const connector( 0x203f );
    QString const article = QStringLiteral( "alpha" ) + connector + QStringLiteral( "beta" );
    QString text = article;
    QMap< QString, QVector< uint32_t > > words;

    FtsHelpers::parseArticleForFts( 1, text, words, false );

    QCOMPARE( words.keys(), QStringList{ article } );
    QCOMPARE( words.value( article ), QVector< uint32_t >{ 1u } );

    // The query path still treats the same text as two words, which is exactly
    // why the two separators must stay distinct.
    QStringList indexWords, searchWords;
    bool hasCJK = false;
    FtsHighlightSpec spec;
    QVERIFY( FtsHelpers::parseSearchString( article, indexWords, searchWords, spec,
                                            FTS::WholeWords, false, -1, hasCJK ) );
    QCOMPARE( searchWords, ( QStringList{ QStringLiteral( "alpha" ), QStringLiteral( "beta" ) } ) );
}

void TstRegex::ftsRegexpModeKeepsNewlineAndMinimalSemantics()
{
    QStringList indexWords, searchWords;
    bool hasCJK = false;
    FtsHighlightSpec spec;

    // QRegExp's '.' crossed a newline, so DotMatchesEverythingOption is part of
    // the compiled expression.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "a.b" ), indexWords, searchWords, spec,
                                            FTS::RegExp, false, -1, hasCJK ) );
    QVERIFY( bool( spec.expression.patternOptions() & QRegularExpression::DotMatchesEverythingOption ) );
    QVERIFY( spec.expression.match( QStringLiteral( "a\nb" ) ).hasMatch() );

    // QRegExp::setMinimal( true ) is reproduced by InvertedGreedinessOption, so
    // two tags stay two matches instead of one tag-spanning run.
    QVERIFY( bool( spec.expression.patternOptions() & QRegularExpression::InvertedGreedinessOption ) );
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "<b>.*</b>" ), indexWords, searchWords,
                                            spec, FTS::RegExp, false, -1, hasCJK ) );
    QCOMPARE( matchSpans( spec.expression, QStringLiteral( "<b>a</b><b>b</b>" ) ),
              QStringList( { QStringLiteral( "0+8" ), QStringLiteral( "8+8" ) } ) );

    // QRegExp's \w knew Unicode letters; UseUnicodePropertiesOption keeps that.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "\\w+" ), indexWords, searchWords,
                                            spec, FTS::RegExp, false, -1, hasCJK ) );
    QVERIFY( spec.expression.match( QString::fromUtf8( "Привет" ) ).hasMatch() );

    // MultilineOption is part of the same compile contract (articleurls.cc adds
    // it too), so '^' also matches after a line break.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "^b" ), indexWords, searchWords, spec,
                                            FTS::RegExp, false, -1, hasCJK ) );
    QVERIFY( spec.expression.match( QStringLiteral( "a\nb" ) ).hasMatch() );
}

void TstRegex::ftsEscapeTokensAreStrippedFromIndexWords()
{
    QStringList indexWords, searchWords;
    bool hasCJK = false;
    FtsHighlightSpec spec;

    // A recognized escape token is removed before the index words are made.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "\\dwerty" ), indexWords, searchWords,
                                            spec, FTS::RegExp, false, -1, hasCJK ) );
    QCOMPARE( indexWords, QStringList( { QStringLiteral( "werty" ) } ) );

    // An escape the recognizer does not know stays in the searched text. \Z is
    // a valid PCRE anchor (so the pattern compiles) and is not one of the
    // recognized FTS tokens; index words are lowercased as before.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "\\Zwerty" ), indexWords, searchWords,
                                            spec, FTS::RegExp, false, -1, hasCJK ) );
    QCOMPARE( indexWords, QStringList( { QStringLiteral( "zwerty" ) } ) );

    // The legacy \xHHHH token needs exactly four hex digits: a shorter spelling
    // is not a token, so its text is searched instead of being dropped.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "\\x0041" ), indexWords, searchWords,
                                            spec, FTS::RegExp, false, -1, hasCJK ) );
    QVERIFY( indexWords.isEmpty() );
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "\\x041" ), indexWords, searchWords,
                                            spec, FTS::RegExp, false, -1, hasCJK ) );
    QCOMPARE( indexWords, QStringList( { QStringLiteral( "x041" ) } ) );

    // Same for the three-digit octal token: it needs all three digits, so a
    // two-digit spelling is not a token and its text is searched.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "\\0101" ), indexWords, searchWords,
                                            spec, FTS::RegExp, false, -1, hasCJK ) );
    QVERIFY( indexWords.isEmpty() );
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "\\01abcdef" ), indexWords, searchWords,
                                            spec, FTS::RegExp, false, -1, hasCJK ) );
    QCOMPARE( indexWords, QStringList( { QStringLiteral( "01abcdef" ) } ) );

    // A symbol set is replaced by a separator, so its contents are not searched.
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "alpha [beta] gamma" ), indexWords,
                                            searchWords, spec, FTS::RegExp, false, -1, hasCJK ) );
    QCOMPARE( indexWords, QStringList( { QStringLiteral( "alpha" ), QStringLiteral( "gamma" ) } ) );
}

void TstRegex::ftsSurrogatePairsKeepScalarSafeOffsets()
{
    // A supplementary scalar is two UTF-16 units, so the CJK scalar behind it
    // starts at offset 2 and is one unit long; the pin is the plan's fixed value.
    const QString text = QString::fromUtf8( "\xf0\x9f\x98\x80" "\xe4\xb8\xad" );
    QRegularExpressionMatch const match =
        QRegularExpression( QString::fromUtf8( "\xe4\xb8\xad" ) ).match( text );
    QCOMPARE( match.capturedStart(), qsizetype( 2 ) );
    QCOMPARE( match.capturedLength(), qsizetype( 1 ) );

    // The same offsets come out of the production specification.
    QStringList indexWords, searchWords;
    bool hasCJK = false;
    FtsHighlightSpec spec;
    QVERIFY( FtsHelpers::parseSearchString( QString::fromUtf8( "\xe4\xb8\xad" ), indexWords,
                                            searchWords, spec, FTS::RegExp, false, -1, hasCJK ) );
    QVERIFY( hasCJK );
    QRegularExpressionMatch const productionMatch = spec.expression.match( text );
    QVERIFY( productionMatch.hasMatch() );
    QCOMPARE( productionMatch.capturedStart(), qsizetype( 2 ) );
    QCOMPARE( productionMatch.capturedLength(), qsizetype( 1 ) );
    QCOMPARE( productionMatch.captured( 0 ), QString::fromUtf8( "\xe4\xb8\xad" ) );

    // The index-word builder keeps a supplementary CJK scalar together instead
    // of emitting its two surrogates as separate words.
    QVERIFY( FtsHelpers::parseSearchString( QString::fromUtf8( "\xf0\xa0\x80\x80" "abcd" ),
                                            indexWords, searchWords, spec, FTS::RegExp, false, -1,
                                            hasCJK ) );
    QVERIFY( hasCJK );
    QString const astral = QString::fromUtf8( "\xf0\xa0\x80\x80" );
    QCOMPARE( astral.size(), qsizetype( 2 ) );
    QVERIFY( indexWords.contains( astral ) );
    QVERIFY2( !indexWords.contains( QString( QChar( 0xD840 ) ) ),
              "the surrogate pair was split into a lone high surrogate word" );
    QVERIFY2( !indexWords.contains( QString( QChar( 0xDC00 ) ) ),
              "the surrogate pair was split into a lone low surrogate word" );
}

void TstRegex::ftsArticleIndexKeepsScalarWords()
{
    // The index builder stores a supplementary CJK scalar as one word, exactly
    // as the baseline did. Splitting its surrogate halves would corrupt the
    // on-disk index without changing the format version.
    QMap< QString, QVector< uint32_t > > words;
    QString const astral = QString::fromUtf8( "\xf0\xa0\x80\x80" );
    QString const article = astral + QStringLiteral( "abcd" );
    QString articleText( article );
    FtsHelpers::parseArticleForFts( 7, articleText, words, false );

    QVERIFY( words.contains( astral ) );
    QVERIFY2( !words.contains( QString( QChar( 0xD840 ) ) ),
              "the surrogate pair was indexed as a lone high surrogate" );
    QVERIFY2( !words.contains( QString( QChar( 0xDC00 ) ) ),
              "the surrogate pair was indexed as a lone low surrogate" );
    QCOMPARE( words.value( astral ), QVector< uint32_t >( { 7 } ) );

    // An ASCII word without CJK is still indexed as before.
    QMap< QString, QVector< uint32_t > > asciiWords;
    QString asciiText( QStringLiteral( "Hello world" ) );
    FtsHelpers::parseArticleForFts( 9, asciiText, asciiWords, false );
    QVERIFY( asciiWords.contains( QStringLiteral( "hello" ) ) );
    QVERIFY( asciiWords.contains( QStringLiteral( "world" ) ) );
}

void TstRegex::ftsResultHighlightIsRebuiltFromHilites()
{
    // A words-mode result carries the matched word sequences, not a wildcard
    // source, so its specification is rebuilt; every other result keeps the
    // search's own specification unchanged.
    QStringList indexWords, searchWords;
    bool hasCJK = false;
    FtsHighlightSpec searchSpec;
    QVERIFY( FtsHelpers::parseSearchString( QStringLiteral( "alpha beta" ), indexWords, searchWords,
                                            searchSpec, FTS::WholeWords, true, -1, hasCJK ) );
    QVERIFY( searchSpec.matchCase );
    QVERIFY( !searchSpec.wildcards );
    QVERIFY( !bool( searchSpec.expression.patternOptions() & QRegularExpression::CaseInsensitiveOption ) );

    FtsHighlightSpec const passthrough = FTS::makeResultHighlight( QStringList(), true, searchSpec );
    QCOMPARE( passthrough.sourcePattern, searchSpec.sourcePattern );
    QCOMPARE( passthrough.wildcards, searchSpec.wildcards );
    QCOMPARE( passthrough.matchCase, searchSpec.matchCase );
    QCOMPARE( passthrough.expression.pattern(), searchSpec.expression.pattern() );
    QCOMPARE( passthrough.expression.patternOptions(), searchSpec.expression.patternOptions() );

    QStringList const hilites{ QStringLiteral( "Alpha" ), QStringLiteral( "beta" ) };
    FtsHighlightSpec const rebuilt = FTS::makeResultHighlight( hilites, true, searchSpec );
    QCOMPARE( rebuilt.sourcePattern, QStringLiteral( "Alpha|beta" ) );
    QVERIFY( !rebuilt.wildcards );
    QVERIFY( rebuilt.matchCase );
    QVERIFY( rebuilt.expression.isValid() );
    // The compiled expression carries a search's grading options, not a bare
    // compile: minimal matching and Unicode word properties.
    QVERIFY( bool( rebuilt.expression.patternOptions() & QRegularExpression::InvertedGreedinessOption ) );
    QVERIFY( bool( rebuilt.expression.patternOptions() & QRegularExpression::UseUnicodePropertiesOption ) );
    QVERIFY( !bool( rebuilt.expression.patternOptions() & QRegularExpression::CaseInsensitiveOption ) );
    QVERIFY( rebuilt.expression.match( QStringLiteral( "Alpha" ) ).hasMatch() );
    QVERIFY( !rebuilt.expression.match( QStringLiteral( "ALPHA" ) ).hasMatch() );

    // A case-insensitive result recompiles with the option instead of folding
    // the source text.
    FtsHighlightSpec const folded = FTS::makeResultHighlight( hilites, false, searchSpec );
    QCOMPARE( folded.sourcePattern, rebuilt.sourcePattern );
    QVERIFY( bool( folded.expression.patternOptions() & QRegularExpression::CaseInsensitiveOption ) );
    QVERIFY( folded.expression.match( QStringLiteral( "ALPHA" ) ).hasMatch() );
}

void TstRegex::ftsDialogUsesSuppliedTextWithAPlainParent()
{
    // The dialog is constructed with a plain QWidget parent: it must take its
    // starting text from the caller and emit help as a signal. A
    // static_cast<MainWindow *>(parent) would crash here, and the old
    // qobject_cast help path would silently do nothing.
    QWidget parent;
    Config::Class cfg;
    std::vector< sptr< Dictionary::Class > > dictionaries;
    std::vector< Instances::Group > groups;
    FTS::FtsIndexing indexing( dictionaries );

    FTS::FullTextSearchDialog dialog( &parent, cfg, dictionaries, groups, indexing,
                                      QStringLiteral( "initial text" ) );
    QLineEdit * searchLine = dialog.findChild< QLineEdit * >( QStringLiteral( "searchLine" ) );
    QVERIFY( searchLine );
    QCOMPARE( searchLine->text(), QStringLiteral( "initial text" ) );
    // The whole line is selected, ready to be replaced by typing.
    QCOMPARE( searchLine->selectedText(), QStringLiteral( "initial text" ) );

    // The help request leaves the dialog as a signal carrying the page id the
    // main window used to be told directly.
    QSignalSpy helpSpy( &dialog, SIGNAL( helpRequested( QString ) ) );
    QVERIFY( helpSpy.isValid() );
    QPushButton * helpButton = dialog.findChild< QPushButton * >( QStringLiteral( "helpButton" ) );
    QVERIFY( helpButton );
    helpButton->click();
    QCOMPARE( helpSpy.count(), 1 );
    QCOMPARE( helpSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "Full-text search" ) );

    QAction * helpAction = nullptr;
    for ( QAction * action : dialog.findChildren< QAction * >() )
        if ( action->shortcut() == QKeySequence( QStringLiteral( "F1" ) ) )
            helpAction = action;
    QVERIFY( helpAction );
    helpAction->trigger();
    QCOMPARE( helpSpy.count(), 2 );
    QCOMPARE( helpSpy.at( 1 ).at( 0 ).toString(), QStringLiteral( "Full-text search" ) );

    // Old call sites keep compiling through the empty default.
    FTS::FullTextSearchDialog legacyDialog( &parent, cfg, dictionaries, groups, indexing );
    QLineEdit * legacyLine = legacyDialog.findChild< QLineEdit * >( QStringLiteral( "searchLine" ) );
    QVERIFY( legacyLine );
    QVERIFY( legacyLine->text().isEmpty() );

    // The result signal carries the specification value, not a pattern string.
    void ( FTS::FullTextSearchDialog::*showTranslation )( QString const &, QStringList const &,
                                                          FtsHighlightSpec const &, bool ) =
        &FTS::FullTextSearchDialog::showTranslationFor;
    QVERIFY( showTranslation != nullptr );
}

#ifdef GD_SAVE_COLLECTOR_AVAILABLE
void TstRegex::saveCollector()
{
    // Task 27 delivers the production helper this target was preconfigured for:
    // SaveArticleDetail::collectResourceSpans() in savearticle.hh. Both the
    // saving transaction and this Core target have to call that one helper, so
    // the spans are asserted on production output only.
    QString const html = QStringLiteral(
        "<html><head><link rel=\"stylesheet\" href=\"styles/entry.css\"></head>"
        "<body><img src=\"images/pic.png\"><img src=\"images/pic.png\"></body></html>" );
    QUrl const base( QStringLiteral( "https://dict.example/articles/entry.html" ) );
    QVector< SaveArticleDetail::ResourceSpan > const spans =
        SaveArticleDetail::collectResourceSpans( html, base );

    QVERIFY2( !spans.isEmpty(), "the save collector found no resource spans" );

    bool sawImage = false;
    bool sawStylesheet = false;

    for ( SaveArticleDetail::ResourceSpan const & span : spans )
    {
        QVERIFY( span.offset >= 0 );
        QVERIFY( span.length >= 0 );
        QVERIFY( span.offset + span.length <= html.size() );
        QVERIFY2( span.url.isValid() && !span.url.isRelative(),
                  qPrintable( QStringLiteral( "unresolved resource URL: %1" ).arg( span.url.toString() ) ) );
        // The span is a UTF-16 range over the document text, so the attribute
        // value is visible inside it; a byte offset or a stale view would not be.
        QVERIFY2( html.mid( span.offset, span.length ).contains( span.url.fileName() ),
                  qPrintable( QStringLiteral( "span %1+%2 does not cover '%3'" )
                                  .arg( span.offset )
                                  .arg( span.length )
                                  .arg( span.url.fileName() ) ) );

        if ( span.url == QUrl( QStringLiteral( "https://dict.example/articles/images/pic.png" ) ) )
            sawImage = true;
        if ( span.url == QUrl( QStringLiteral( "https://dict.example/articles/styles/entry.css" ) ) )
            sawStylesheet = true;
    }

    QVERIFY2( sawImage, "the image resource was not resolved against the document URL" );
    QVERIFY2( sawStylesheet, "the stylesheet resource was not resolved against the document URL" );
}
#endif

QTEST_MAIN( TstRegex )

#include "tst_regex.moc"
