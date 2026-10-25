/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Centralized link-rule suite.
 *
 * The four rules that produce the `gdlookup://`, `bres://`, `gico://` and
 * `gdau://` forms used to be duplicated with subtly different patterns in
 * stardict.cc, zim.cc, dictdfiles.cc, dictserver.cc and website.cc. This suite
 * pins the common contract: exact output, preserved quotes/whitespace, the
 * absolute/protocol-relative/fragment/data exclusions, dictionary-root
 * resources, qualified icons, the production audio wrapper, and second-pass
 * idempotence for every rule.
 *
 * Fixed expected values live in fixtures/linkrewrite/cases.json so a caller
 * cannot "fix" a regression by editing the suite alone; the named tests below
 * restate the pins the task brief requires with the production calls in view.
 */

#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <string>

#include "audiolink.hh"
#include "linkrewrite.hh"
#include "support/fixtures.hh"

namespace
{

constexpr char const * FixtureFile = "linkrewrite/cases.json";

/// Fixture sections in the order they are reported, mapped to the production
/// entry point each row exercises.
struct Section
{
    char const * name;
    char const * defaultFunction;
};

constexpr Section Sections[] = {
    { "gdLookup", "gdLookup" },
    { "bres", "bres" },
    { "gico", "gico" },
    { "gdau", "gdau" },
    { "engineSeam", "gdLookup" },
};

QJsonObject loadCases()
{
    QFile file( TestSupport::fixturePath( QString::fromLatin1( FixtureFile ) ) );

    if ( !file.open( QIODevice::ReadOnly ) )
        return QJsonObject();

    const QJsonDocument document = QJsonDocument::fromJson( file.readAll() );

    return document.isObject() ? document.object() : QJsonObject();
}

/// The single production dispatch the ledger rows use.
QString applyProduction( QString const & function, QString const & input, QString const & dictId )
{
    if ( function == QLatin1String( "gdLookup" ) )
        return LinkRewrite::applyGdLookup( input );

    if ( function == QLatin1String( "bres" ) )
        return LinkRewrite::applyBres( input, dictId );

    if ( function == QLatin1String( "gico" ) )
        return LinkRewrite::applyGico( input, dictId );

    if ( function == QLatin1String( "gdau" ) )
        return LinkRewrite::applyGdau( input, dictId );

    return QStringLiteral( "<unknown function " ) + function + QLatin1Char( '>' );
}

/// Renders a string so a control character or a trailing space is visible in a
/// failure message instead of silently collapsing the difference.
QString display( QString const & text )
{
    QString shown;

    for ( QChar const character : text )
    {
        if ( character == QLatin1Char( '\n' ) )
            shown += QLatin1String( "\\n" );
        else if ( character == QLatin1Char( '\t' ) )
            shown += QLatin1String( "\\t" );
        else if ( character.unicode() < 0x20 || character.unicode() == 0x7f )
            shown += QStringLiteral( "\\x%1" ).arg( int( character.unicode() ), 2, 16, QLatin1Char( '0' ) );
        else
            shown += character;
    }

    return shown;
}

QString diff( QString const & id, QString const & what, QString const & actual, QString const & expected )
{
    return QStringLiteral( "%1: %2 mismatch\n  actual  : %3\n  expected: %4" )
        .arg( id, what, display( actual ), display( expected ) );
}

} // namespace

class TstLinkRewrite : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();

    void fixtureLedgerIsComplete();
    void fixtureRowsMatchProduction();

    void gdLookupPreservesStructureAndIsIdempotent();
    void gdLookupNormalizesOnlyThePathHeadword();

    void bresPrefixesRelativeResources();
    void bresLeavesAbsoluteProtocolRelativeAndFragmentTargetsAlone();
    void bresTreatsRootRelativeTargetsAsDictionaryRoot();

    void gicoQualifiesOnlyExplicitUnqualifiedIcons();

    void gdauUsesTheProductionAudioWrapper();
    void gdauKeepsTwoMultilineAudioElementsSeparate();
    void gdauAddsThePlayIconOnlyWhenTheBodyHasNone();
    void audioScriptContractIsPinned();

    void quotedAttributeContentIsNotScannedAsMarkup();
    void nonHtmlTextAndForeignMarkupAreUntouched();
};

void TstLinkRewrite::initTestCase()
{
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
    QVERIFY2( TestSupport::fixtureExists( QString::fromLatin1( FixtureFile ) ),
        qPrintable( QStringLiteral( "missing fixture %1" )
                .arg( TestSupport::fixturePath( QString::fromLatin1( FixtureFile ) ) ) ) );

    TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
}

void TstLinkRewrite::cleanupTestCase()
{
    TestSupport::clearWatchdog();
}

void TstLinkRewrite::fixtureLedgerIsComplete()
{
    // The ledger is the oracle: if it is empty, truncated or reuses an id, the
    // row-matching test below would pass by checking nothing.
    QJsonObject const cases = loadCases();
    QVERIFY2( !cases.isEmpty(), "linkrewrite/cases.json is not a JSON object" );
    QVERIFY2( !cases.value( QStringLiteral( "notes" ) ).toArray().isEmpty(),
        "the ledger lost its provenance notes" );

    QStringList problems;
    QSet<QString> seenIds;
    int rowCount = 0;

    for ( Section const & section : Sections )
    {
        QJsonArray const rows = cases.value( QString::fromLatin1( section.name ) ).toArray();

        if ( rows.isEmpty() )
        {
            problems
                << QStringLiteral( "section '%1' has no rows" ).arg( QString::fromLatin1( section.name ) );
            continue;
        }

        for ( QJsonValue const & value : rows )
        {
            QJsonObject const row = value.toObject();
            ++rowCount;

            QString const id = row.value( QStringLiteral( "id" ) ).toString();
            QString const input = row.value( QStringLiteral( "input" ) ).toString();
            QString const expected = row.value( QStringLiteral( "expected" ) ).toString();
            QString const function = row.value( QStringLiteral( "function" ) )
                                         .toString( QString::fromLatin1( section.defaultFunction ) );

            if ( id.isEmpty() )
                problems
                    << QStringLiteral( "%1: row without an id" ).arg( QString::fromLatin1( section.name ) );

            if ( seenIds.contains( id ) )
                problems << QStringLiteral( "%1: duplicate id" ).arg( id );

            seenIds.insert( id );

            if ( input.isEmpty() )
                problems << QStringLiteral( "%1: row without an input" ).arg( id );

            if ( expected.isEmpty() )
                problems << QStringLiteral( "%1: row without an expected value" ).arg( id );

            if ( function != QLatin1String( "gdLookup" ) && function != QLatin1String( "bres" ) &&
                 function != QLatin1String( "gico" ) && function != QLatin1String( "gdau" ) )
                problems << QStringLiteral( "%1: unknown function '%2'" ).arg( id, function );

            if ( function != QLatin1String( "gdLookup" ) &&
                 row.value( QStringLiteral( "dictId" ) ).toString().isNull() )
                problems << QStringLiteral( "%1: dictionary-dependent row without a dictId" ).arg( id );
        }
    }

    // A ledger this small cannot cover the pinned rules; the floor only guards
    // against a fixture that was emptied or truncated.
    if ( rowCount < 40 )
        problems << QStringLiteral( "only %1 rows; the ledger must keep every pinned case" ).arg( rowCount );

    QVERIFY2( problems.isEmpty(), qPrintable( problems.join( '\n' ) ) );
}

void TstLinkRewrite::fixtureRowsMatchProduction()
{
    QJsonObject const cases = loadCases();

    QStringList problems;
    int rowCount = 0;

    for ( Section const & section : Sections )
    {
        QJsonArray const rows = cases.value( QString::fromLatin1( section.name ) ).toArray();

        for ( QJsonValue const & value : rows )
        {
            QJsonObject const row = value.toObject();
            ++rowCount;

            QString const id = row.value( QStringLiteral( "id" ) ).toString();
            QString const input = row.value( QStringLiteral( "input" ) ).toString();
            QString const expected = row.value( QStringLiteral( "expected" ) ).toString();
            QString const dictId = row.value( QStringLiteral( "dictId" ) ).toString();
            QString const function = row.value( QStringLiteral( "function" ) )
                                         .toString( QString::fromLatin1( section.defaultFunction ) );

            QString const actual = applyProduction( function, input, dictId );

            if ( actual != expected )
            {
                problems << diff( QStringLiteral( "%1/%2" ).arg( QString::fromLatin1( section.name ), id ),
                    QStringLiteral( "output" ),
                    actual,
                    expected );
                continue;
            }

            if ( row.value( QStringLiteral( "twice" ) ).toBool( false ) )
            {
                QString const again = applyProduction( function, expected, dictId );

                if ( again != expected )
                    problems << diff( id, QStringLiteral( "second pass" ), again, expected );
            }
        }
    }

    QVERIFY2( rowCount > 0, "the fixture ledger has no rows" );
    QVERIFY2( problems.isEmpty(),
        qPrintable( QStringLiteral( "%1 of %2 ledger rows failed:\n%3" )
                .arg( problems.size() )
                .arg( rowCount )
                .arg( problems.join( '\n' ) ) ) );
}

void TstLinkRewrite::gdLookupPreservesStructureAndIsIdempotent()
{
    // A structured lookup target must keep its query items and fragment intact:
    // the action handler still needs `dict`, `gdanchor` and `#frag`.
    QString const source =
        QStringLiteral( "<a href=\"gdlookup://localhost/cat?dict=Known%20Name&gdanchor=part#frag\">x</a>" );

    QCOMPARE( LinkRewrite::applyGdLookup( source ), source );
    QCOMPARE( LinkRewrite::applyGdLookup( LinkRewrite::applyGdLookup( source ) ), source );

    // The structured components stay decodable to the same key/value pairs.
    QString const rewritten = LinkRewrite::applyGdLookup( source );
    QRegularExpressionMatch const match =
        QRegularExpression( QStringLiteral( "<a href=\"([^\"]*)\">" ) ).match( rewritten );
    QVERIFY( match.hasMatch() );

    QUrl const url( match.captured( 1 ) );
    QCOMPARE( url.host(), QStringLiteral( "localhost" ) );
    QCOMPARE( url.path(), QStringLiteral( "/cat" ) );
    QCOMPARE( url.fragment(), QStringLiteral( "frag" ) );

    QUrlQuery const query( url );
    QCOMPARE( query.queryItemValue( QStringLiteral( "dict" ), QUrl::FullyDecoded ),
        QStringLiteral( "Known Name" ) );
    QCOMPARE(
        query.queryItemValue( QStringLiteral( "gdanchor" ), QUrl::FullyDecoded ), QStringLiteral( "part" ) );

    // A query that carries only an anchor keeps its boundary as well.
    QString const anchorOnly = QStringLiteral( "<a href=\"gdlookup://localhost/cat?gdanchor=part\">x</a>" );
    QCOMPARE( LinkRewrite::applyGdLookup( anchorOnly ), anchorOnly );
}

void TstLinkRewrite::gdLookupNormalizesOnlyThePathHeadword()
{
    // Markup and entity cleanup plus one encoding pass belong to the path
    // headword; the query and fragment are never treated as headword text.
    QCOMPARE(
        LinkRewrite::applyGdLookup( QStringLiteral( "<a href=\"gdlookup://localhost/red&nbsp;fox\">x</a>" ) ),
        QStringLiteral( "<a href=\"gdlookup://localhost/red%20fox\">x</a>" ) );

    // An already encoded path is decoded once and re-encoded, never doubled.
    QCOMPARE(
        LinkRewrite::applyGdLookup( QStringLiteral( "<a href=\"gdlookup://localhost/red%20fox\">x</a>" ) ),
        QStringLiteral( "<a href=\"gdlookup://localhost/red%20fox\">x</a>" ) );

    // A query value that itself looks percent-encoded stays byte-identical, so
    // the handler decodes it exactly once.
    QString const withEncodedQuery =
        QStringLiteral( "<a href=\"gdlookup://localhost/cat?dict=Known%20Name\">x</a>" );
    QCOMPARE( LinkRewrite::applyGdLookup( withEncodedQuery ), withEncodedQuery );

    // Unicode headwords survive as canonical UTF-8 percent encoding.
    QCOMPARE( LinkRewrite::applyGdLookup( QStringLiteral( "<a href=\"gdlookup://localhost/日本語\">x</a>" ) ),
        QStringLiteral( "<a href=\"gdlookup://localhost/%E6%97%A5%E6%9C%AC%E8%AA%9E\">x</a>" ) );

    // A `+` is a literal headword character, not an encoded space.
    QString const plus = QStringLiteral( "<a href=\"gdlookup://localhost/C++\">x</a>" );
    QCOMPARE( LinkRewrite::applyGdLookup( plus ),
        QStringLiteral( "<a href=\"gdlookup://localhost/C%2B%2B\">x</a>" ) );
}

void TstLinkRewrite::bresPrefixesRelativeResources()
{
    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "<img src='a.png'>" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<img src='bres://d/a.png'>" ) );

    QCOMPARE( LinkRewrite::applyBres(
                  QStringLiteral( "<link href=\"style.css\" rel=\"stylesheet\">" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<link href=\"bres://d/style.css\" rel=\"stylesheet\">" ) );

    QCOMPARE( LinkRewrite::applyBres(
                  QStringLiteral( "<script src=\"js/x.js\"></script>" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<script src=\"bres://d/js/x.js\"></script>" ) );

    // The relative spelling is retained, including a parent-directory step in a
    // CSS url().
    QCOMPARE(
        LinkRewrite::applyBres( QStringLiteral( "a{background:url(../a.png)}" ), QStringLiteral( "d" ) ),
        QStringLiteral( "a{background:url(bres://d/../a.png)}" ) );

    // Quote and spacing style of an inline style attribute is preserved.
    QCOMPARE(
        LinkRewrite::applyBres(
            QStringLiteral( "<div style=\"background: url( 'img/a.png' )\"></div>" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<div style=\"background: url( 'bres://d/img/a.png' )\"></div>" ) );

    // A second pass changes nothing: the emitted prefix is already qualified.
    QString const once = LinkRewrite::applyBres(
        QStringLiteral( "<img src=\"a.png\"><img src=\"b.png\">" ), QStringLiteral( "d" ) );
    QCOMPARE( once, QStringLiteral( "<img src=\"bres://d/a.png\"><img src=\"bres://d/b.png\">" ) );
    QCOMPARE( LinkRewrite::applyBres( once, QStringLiteral( "d" ) ), once );
}

void TstLinkRewrite::bresLeavesAbsoluteProtocolRelativeAndFragmentTargetsAlone()
{
    QStringList const untouched = {
        QStringLiteral( "<img src=\"https://example.org/a.png\">" ),
        QStringLiteral( "<img src=\"http://example.org/a.png\">" ),
        QStringLiteral( "<script src=\"ftp://example.org/a.js\"></script>" ),
        QStringLiteral( "<img src=\"data:image/png;base64,AAAA\">" ),
        QStringLiteral( "<img src=\"qrcx://localhost/icons/playsound.png\">" ),
        QStringLiteral( "<img src=\"//cdn.example.org/a.png\">" ),
        QStringLiteral( "<img src=\"#frag\">" ),
        QStringLiteral( "<img src=\"bres://other/a.png\">" ),
        QStringLiteral( "url(data:image/png;base64,AAAA)" ),
        QStringLiteral( "url(https://example.org/a.png)" ),
    };

    for ( QString const & source : untouched )
        QCOMPARE( LinkRewrite::applyBres( source, QStringLiteral( "d" ) ), source );
}

void TstLinkRewrite::bresTreatsRootRelativeTargetsAsDictionaryRoot()
{
    // `/a.png` is the dictionary root, so exactly one slash is dropped; the old
    // `bres://d//a.png` spelling is never produced.
    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "<img src=\"/a.png\">" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<img src=\"bres://d/a.png\">" ) );

    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "url(/a/b.png)" ), QStringLiteral( "d" ) ),
        QStringLiteral( "url(bres://d/a/b.png)" ) );

    QString const prefixed =
        LinkRewrite::applyBres( QStringLiteral( "<img src=\"/a.png\">" ), QStringLiteral( "d" ) );
    QVERIFY( !prefixed.contains( QStringLiteral( "bres://d//" ) ) );
}

void TstLinkRewrite::gicoQualifiesOnlyExplicitUnqualifiedIcons()
{
    // Already qualified icons are never re-qualified, whatever dictionary the
    // pass was called for.
    QCOMPARE( LinkRewrite::applyGico( QStringLiteral( "gico://d/dicticon.png" ), QStringLiteral( "other" ) ),
        QStringLiteral( "gico://d/dicticon.png" ) );

    // Only URL-valued attributes of the article markup are rewritten; a bare
    // URL string is not a document and stays untouched.
    QCOMPARE(
        LinkRewrite::applyGico( QStringLiteral( "<img src=\"gico:/dicticon.png\">" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<img src=\"gico://d/dicticon.png\">" ) );

    QCOMPARE( LinkRewrite::applyGico(
                  QStringLiteral( "<img src=\"gico:///dicticon.png\">" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<img src=\"gico://d/dicticon.png\">" ) );

    QString const bareTarget = QStringLiteral( "gico:/dicticon.png" );
    QCOMPARE( LinkRewrite::applyGico( bareTarget, QStringLiteral( "d" ) ), bareTarget );

    // Arbitrary images never become icons.
    QString const image = QStringLiteral( "<img src=\"a.png\">" );
    QCOMPARE( LinkRewrite::applyGico( image, QStringLiteral( "d" ) ), image );

    // The generated per-dictionary icon markup stays byte-identical.
    QString const generated =
        QStringLiteral( "<span class=\"gddicticon\"><img src=\"gico://d/dicticon.png\"></span>" );
    QCOMPARE( LinkRewrite::applyGico( generated, QStringLiteral( "d" ) ), generated );
}

void TstLinkRewrite::gdauUsesTheProductionAudioWrapper()
{
    // The wrapper is the existing audiolink.cc output, not a second copy of the
    // script: the audio player and the bridge consume `gdAudioLinks`.
    QString const expectedWrapper =
        QString::fromStdString( addAudioLink( "\"gdau://d/a.wav\"", "d" ) ) +
        QStringLiteral( "<span class=\"sdict_h_wav\"><a href=\"gdau://d/a.wav\">"
                        " <img src=\"qrcx://localhost/icons/playsound.png\" border=\"0\" alt=\"Play\">"
                        "</a></span>" );

    QCOMPARE(
        LinkRewrite::applyGdau( QStringLiteral( "<audio src=\"a.wav\"></audio>" ), QStringLiteral( "d" ) ),
        expectedWrapper );

    QVERIFY( expectedWrapper.contains( QStringLiteral( "gdAudioLinks.first" ) ) );
    QVERIFY( expectedWrapper.contains( QStringLiteral( "gdAudioLinks['d']" ) ) );

    // An already qualified or foreign audio source is left alone.
    QStringList const untouched = {
        QStringLiteral( "<audio src=\"gdau://d/a.wav\">x</audio>" ),
        QStringLiteral( "<audio src=\"https://example.org/a.wav\">x</audio>" ),
        QStringLiteral( "<audio src=\"//cdn.example.org/a.wav\">x</audio>" ),
        QStringLiteral( "<audio src=\"a.wav\">x" ),
        QStringLiteral( "<audio controls>x</audio>" ),
    };

    for ( QString const & source : untouched )
        QCOMPARE( LinkRewrite::applyGdau( source, QStringLiteral( "d" ) ), source );
}

void TstLinkRewrite::gdauKeepsTwoMultilineAudioElementsSeparate()
{
    QString const source =
        QStringLiteral( "<audio src=\"a.wav\">\nA\n</audio>\n<audio src=\"b.wav\">B</audio>" );
    QString const rewritten = LinkRewrite::applyGdau( source, QStringLiteral( "d" ) );

    // Two elements are two wrappers, and each body stays with its own source.
    QCOMPARE( rewritten.count( QStringLiteral( "<span class=\"sdict_h_wav\">" ) ), qsizetype( 2 ) );
    QCOMPARE( rewritten.count( QStringLiteral( "gdau://d/a.wav" ) ), qsizetype( 3 ) );
    QCOMPARE( rewritten.count( QStringLiteral( "gdau://d/b.wav" ) ), qsizetype( 3 ) );
    QVERIFY( rewritten.contains( QStringLiteral( "<a href=\"gdau://d/a.wav\">\nA\n <img" ) ) );
    QVERIFY( rewritten.contains( QStringLiteral( "<a href=\"gdau://d/b.wav\">B <img" ) ) );
    QVERIFY( !rewritten.contains( QStringLiteral( "<audio" ) ) );

    // Multiline bodies matched by a DotMatches-free scan still terminate.
    QCOMPARE( LinkRewrite::applyGdau( rewritten, QStringLiteral( "d" ) ), rewritten );
}

void TstLinkRewrite::gdauAddsThePlayIconOnlyWhenTheBodyHasNone()
{
    QString const withImage = QStringLiteral( "<audio src=\"a.wav\">Play <img src=\"x.png\"></audio>" );
    QString const rewritten = LinkRewrite::applyGdau( withImage, QStringLiteral( "d" ) );

    QVERIFY(
        rewritten.contains( QStringLiteral( "<a href=\"gdau://d/a.wav\">Play <img src=\"x.png\"></a>" ) ) );
    QVERIFY( !rewritten.contains( QStringLiteral( "icons/playsound.png" ) ) );

    // A plain body gets exactly one play icon.
    QString const plain = LinkRewrite::applyGdau(
        QStringLiteral( "<audio src=\"a.wav\">click</audio>" ), QStringLiteral( "d" ) );
    QCOMPARE( plain.count( QStringLiteral( "icons/playsound.png" ) ), qsizetype( 1 ) );
}

void TstLinkRewrite::audioScriptContractIsPinned()
{
    // The unchanged writer's escaping contract: a single quote is escaped for
    // the inline script, and both the per-dictionary slot and the `.first`
    // fallback field are emitted. The bootstrap/bridge work consumes the same
    // map, so this output may not be reformatted.
    std::string const script = makeAudioLinkScript( "\"it's.wav\"", "d" );

    QCOMPARE( QString::fromStdString( script ),
        QStringLiteral( "gdAudioLinks.first = gdAudioLinks.first || \"it\\'s.wav\";"
                        "gdAudioLinks['d'] = gdAudioLinks['d'] || \"it\\'s.wav\";" ) );

    QCOMPARE( QString::fromStdString( addAudioLink( "\"it's.wav\"", "d" ) ),
        QStringLiteral( "<script type=\"text/javascript\">"
                        "gdAudioLinks.first = gdAudioLinks.first || \"it\\'s.wav\";"
                        "gdAudioLinks['d'] = gdAudioLinks['d'] || \"it\\'s.wav\";"
                        "</script>" ) );
}

void TstLinkRewrite::quotedAttributeContentIsNotScannedAsMarkup()
{
    // A `>` or a nested-looking tag inside a quoted attribute value must not end
    // the tag or be rewritten as markup: the scan edits value spans of one
    // immutable source and never rescans what it emitted.
    QString const source = QStringLiteral( "<img src=\"a.png\" alt=\"<img src=b.png>\">" );
    QString const rewritten = LinkRewrite::applyBres( source, QStringLiteral( "d" ) );

    QCOMPARE( rewritten, QStringLiteral( "<img src=\"bres://d/a.png\" alt=\"<img src=b.png>\">" ) );
    QCOMPARE( LinkRewrite::applyBres( rewritten, QStringLiteral( "d" ) ), rewritten );
}

void TstLinkRewrite::nonHtmlTextAndForeignMarkupAreUntouched()
{
    QString const plainText = QStringLiteral( "plain text <b>bold</b> 1 < 2 > 0" );
    QCOMPARE( LinkRewrite::applyBres( plainText, QStringLiteral( "d" ) ), plainText );
    QCOMPARE( LinkRewrite::applyGdLookup( plainText ), plainText );
    QCOMPARE( LinkRewrite::applyGico( plainText, QStringLiteral( "d" ) ), plainText );

    // An anchor href is not a bres resource and a link href is not a lookup.
    QString const anchor = QStringLiteral( "<a href=\"page.html\">x</a>" );
    QCOMPARE( LinkRewrite::applyBres( anchor, QStringLiteral( "d" ) ), anchor );

    QString const link = QStringLiteral( "<link href=\"gdlookup://localhost/red fox\">" );
    QCOMPARE( LinkRewrite::applyGdLookup( link ), link );
}

QTEST_GUILESS_MAIN( TstLinkRewrite )

#include "tst_linkrewrite.moc"
