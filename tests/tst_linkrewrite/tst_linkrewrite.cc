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
 * idempotence for every rule. The named tests also state the two structural
 * invariants the ledger rests on: every span is classified in the original
 * input (a rewritten value is never rescanned as markup or CSS), and the two
 * serialization contexts of the audio wrapper are escaped separately -- the
 * script context has two terminators, so the HTML `</script` end tag is escaped
 * as well as the JavaScript string delimiter.
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
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <optional>
#include <string>

#include "audiolink.hh"
#include "linkrewrite.hh"
#include "website.hh"
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
/// failure message instead of silently collapsing the difference. A very long
/// value keeps its head and tail: an unbounded message can be truncated by the
/// test runner in the middle, which would hide the whole comparison.
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

    constexpr qsizetype Head = 220;
    constexpr qsizetype Tail = 120;

    if ( shown.size() > Head + Tail )
        return shown.left( Head ) + QStringLiteral( "..." ) + shown.right( Tail );

    return shown;
}

QString diff( QString const & id, QString const & what, QString const & actual, QString const & expected )
{
    return QStringLiteral( "%1: %2 mismatch\n  actual  : %3\n  expected: %4" )
        .arg( id, what, display( actual ), display( expected ) );
}

/// Returns the source text of the first `<script ...>` element: everything up to
/// the first `</script` sequence the HTML parser accepts as that element's end
/// tag, i.e. one followed by whitespace, `/` or `>`. A second such sequence
/// inside the body is exactly the injection the audio wrapper must not allow,
/// whatever its case.
std::optional<QString> scriptText( QString const & markup )
{
    static QRegularExpression const startTag( QStringLiteral( "<script(?:\\s[^>]*)?>" ),
        QRegularExpression::CaseInsensitiveOption );
    static QRegularExpression const endTag( QStringLiteral( "</script(?=[\\s/>])" ),
        QRegularExpression::CaseInsensitiveOption );

    QRegularExpressionMatch const open = startTag.match( markup );
    if ( !open.hasMatch() )
        return std::nullopt;

    qsizetype const bodyStart = open.capturedEnd();
    QRegularExpressionMatch const close = endTag.match( markup, bodyStart );
    if ( !close.hasMatch() )
        return std::nullopt;

    return markup.mid( bodyStart, close.capturedStart() - bodyStart );
}

/// Decodes the escape grammar `jsStringBody` emits for the wrapper's
/// double-quoted JavaScript literal. The point is not that the spelling is
/// inert, but that it still evaluates to the original URL: `\x3C` and `\x3E`
/// must survive as the characters the player needs.
std::optional<QString> decodeJsStringLiteral( QString const & literal )
{
    QString decoded;
    decoded.reserve( literal.size() );

    for ( qsizetype index = 0; index < literal.size(); ++index )
    {
        QChar const character = literal.at( index );

        if ( character != QLatin1Char( '\\' ) )
        {
            decoded += character;
            continue;
        }

        if ( ++index >= literal.size() )
            return std::nullopt;

        QChar const escape = literal.at( index );

        switch ( escape.unicode() )
        {
            case '\\':
                decoded += QLatin1Char( '\\' );
                break;
            case '"':
                decoded += QLatin1Char( '"' );
                break;
            case '\'':
                decoded += QLatin1Char( '\'' );
                break;
            case 'n':
                decoded += QLatin1Char( '\n' );
                break;
            case 'r':
                decoded += QLatin1Char( '\r' );
                break;
            case 't':
                decoded += QLatin1Char( '\t' );
                break;
            case 'x':
            case 'u':
            {
                qsizetype const digits = escape == QLatin1Char( 'x' ) ? 2 : 4;
                if ( index + digits >= literal.size() )
                    return std::nullopt;

                bool ok = false;
                ushort const value = literal.mid( index + 1, digits ).toUShort( &ok, 16 );
                if ( !ok )
                    return std::nullopt;

                decoded += QChar( value );
                index += digits;
                break;
            }
            default:
                return std::nullopt;
        }
    }

    return decoded;
}

/// Decodes the character references `htmlAttributeValue` emits, so the anchor's
/// `href` and the script literal are proven to carry the same target.
std::optional<QString> decodeHtmlAttributeValue( QString const & value )
{
    QString decoded;
    decoded.reserve( value.size() );

    for ( qsizetype index = 0; index < value.size(); ++index )
    {
        QChar const character = value.at( index );

        if ( character != QLatin1Char( '&' ) )
        {
            decoded += character;
            continue;
        }

        qsizetype const semicolon = value.indexOf( QLatin1Char( ';' ), index );
        if ( semicolon < 0 )
            return std::nullopt;

        QString const reference = value.mid( index + 1, semicolon - index - 1 );

        if ( reference == QLatin1String( "amp" ) )
            decoded += QLatin1Char( '&' );
        else if ( reference == QLatin1String( "quot" ) )
            decoded += QLatin1Char( '"' );
        else if ( reference == QLatin1String( "lt" ) )
            decoded += QLatin1Char( '<' );
        else if ( reference == QLatin1String( "gt" ) )
            decoded += QLatin1Char( '>' );
        else if ( reference.startsWith( QLatin1Char( '#' ) ) )
        {
            bool ok = false;
            ushort const code = reference.mid( 1 ).toUShort( &ok );
            if ( !ok )
                return std::nullopt;

            decoded += QChar( code );
        }
        else
            return std::nullopt;

        index = semicolon;
    }

    return decoded;
}


} // namespace

class TstLinkRewrite : public QObject
{
    Q_OBJECT

  private slots:
    void websiteRewritesRetainedBaselineBehaviour();
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

    void cssUrlTextIsOnlyRewrittenWhereCssLives();
    void cssUrlValuesAreClassifiedOnceFromTheOriginalInput();
    void completeTagNamesDecideResourceTags();
    void attributePaddingIsPreservedAroundTheRewrittenTarget();
    void quotedCssUrlsKeepParenthesesAndEscapes();
    void audioTagBoundariesAreRealTags();
    void audioSourceTextCannotBecomeScript();
    void audioSourceCannotTerminateTheWrapperScript();
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

    // The runner truncates a long failure message, so only the first few details
    // are printed; the count in the header is the authoritative total.
    constexpr int ReportedProblems = 8;
    QString details = problems.mid( 0, ReportedProblems ).join( '\n' );

    if ( problems.size() > ReportedProblems )
        details += QStringLiteral( "\n... and %1 more failing row(s)" ).arg( problems.size() - ReportedProblems );

    QVERIFY2( problems.isEmpty(),
        qPrintable( QStringLiteral( "%1 of %2 ledger rows failed:\n%3" )
                .arg( problems.size() )
                .arg( rowCount )
                .arg( details ) ) );
}


















void TstLinkRewrite::websiteRewritesRetainedBaselineBehaviour()
{
    const QUrl page( QStringLiteral( "http://127.0.0.1:43127/dir/page?q=1" ) );

    // Relative, root-relative and protocol-relative targets become absolute by
    // string concatenation, which is the retained baseline behaviour: the port is
    // not carried and ".." is not resolved. The body text itself is preserved.
    QCOMPARE( WebSite::detail::rewriteWebsiteLinks(
                  QStringLiteral( "<p>body text</p><a href='../x.html'>x</a>"
                                  "<img src='/img/a.png'><script src='//cdn.example.test/s.js'>" ),
                  page, QStringLiteral( "dict" ) ),
              QStringLiteral( "<p>body text</p><a href='http://127.0.0.1/dir/../x.html'>x</a>"
                              "<img src='http://127.0.0.1/img/a.png'>"
                              "<script src='http://cdn.example.test/s.js'>" ) );

    // Absolute, data:, mailto:, javascript: and fragment targets are left alone.
    QCOMPARE( WebSite::detail::rewriteWebsiteLinks(
                  QStringLiteral( "<a href='https://other.test/z'>z</a><img src='data:image/png;base64,AA'>"
                                  "<a href='mailto:a@b.c'>m</a><a href='javascript:void(0)'>j</a>"
                                  "<a href='#frag'>f</a>" ),
                  page, QStringLiteral( "dict" ) ),
              QStringLiteral( "<a href='https://other.test/z'>z</a><img src='data:image/png;base64,AA'>"
                              "<a href='mailto:a@b.c'>m</a><a href='javascript:void(0)'>j</a>"
                              "<a href='#frag'>f</a>" ) );

    // Absolute stylesheet links are remapped to the dictionary resource scheme.
    // The retained pattern needs an attribute after the closing quote, and it
    // rewrites scheme and host into one path segment: both quirks are shipped
    // behaviour, so the fixture pins them rather than correcting them.
    QCOMPARE( WebSite::detail::rewriteWebsiteLinks(
                  QStringLiteral( "<link rel='stylesheet' href='https://cdn.test/site.css' type='text/css'>" ),
                  page, QStringLiteral( "dict" ) ),
              QStringLiteral( "<link rel='stylesheet' href='bres://dict/https/cdn.test/site.css' type='text/css'>" ) );

    // Unclosed span/div wrappers are still closed.
    QCOMPARE( WebSite::detail::rewriteWebsiteLinks( QStringLiteral( "<span><div>t" ), page,
                                                    QStringLiteral( "dict" ) ),
              QStringLiteral( "<span><div>t</span></div>" ) );

    // The inline frame carries the identity in both id and name.
    const QString frame = WebSite::detail::websiteIframeMarkup(
        QStringLiteral( "dict-id" ), QStringLiteral( "https://example.test/page" ) );
    QVERIFY( frame.contains( QStringLiteral( "id=\"gdexpandframe-dict-id\"" ) ) );
    QVERIFY( frame.contains( QStringLiteral( "name=\"gdexpandframe-dict-id\"" ) ) );
    QVERIFY( frame.contains( QStringLiteral( "src=\"https://example.test/page\"" ) ) );
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

void TstLinkRewrite::cssUrlTextIsOnlyRewrittenWhereCssLives()
{
    // The word `url(` inside a descriptive attribute, inside a resource path or
    // inside a query string is text, not CSS: only the classified resource
    // attribute is rewritten, and the surrounding bytes keep their spelling.
    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "<img alt=\"url(a.png)\" src=\"a.png\">" ),
                  QStringLiteral( "d" ) ),
        QStringLiteral( "<img alt=\"url(a.png)\" src=\"bres://d/a.png\">" ) );

    QString const absolute = QStringLiteral( "<img src=\"https://example.org/url(a.png)\">" );
    QCOMPARE( LinkRewrite::applyBres( absolute, QStringLiteral( "d" ) ), absolute );

    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "<img src=\"url(a.png)\">" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<img src=\"bres://d/url(a.png)\">" ) );

    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "<img src=\"/a.png?caption=url(b.png)\">" ),
                  QStringLiteral( "d" ) ),
        QStringLiteral( "<img src=\"bres://d/a.png?caption=url(b.png)\">" ) );

    // A `title` attribute is never a stylesheet, while the `style` attribute of
    // the same tag is: both are classified independently in one pass.
    QCOMPARE( LinkRewrite::applyBres(
                  QStringLiteral( "<div title=\"url(a.png)\" style=\"background:url(b.png)\"></div>" ),
                  QStringLiteral( "d" ) ),
        QStringLiteral( "<div title=\"url(a.png)\" style=\"background:url(bres://d/b.png)\"></div>" ) );
}

void TstLinkRewrite::cssUrlValuesAreClassifiedOnceFromTheOriginalInput()
{
    // The whole point of the single edit set: a value the CSS rule produced is
    // not offered to the attribute rule again, and vice versa. `url(a.png)` as a
    // resource target therefore gains exactly one prefix, not two.
    QString const source = QStringLiteral( "<img src=\"url(a.png)\"><div style=\"background:url(b.png)\"></div>" );
    QString const expected =
        QStringLiteral( "<img src=\"bres://d/url(a.png)\"><div style=\"background:url(bres://d/b.png)\"></div>" );

    QCOMPARE( LinkRewrite::applyBres( source, QStringLiteral( "d" ) ), expected );
    QCOMPARE( LinkRewrite::applyBres( expected, QStringLiteral( "d" ) ), expected );

    // A second pass cannot grow a prefix either: the emitted `bres://` target is
    // already classified as absolute.
    QVERIFY( !LinkRewrite::applyBres( expected, QStringLiteral( "d" ) )
                  .contains( QStringLiteral( "bres://d/bres://" ) ) );
}

void TstLinkRewrite::completeTagNamesDecideResourceTags()
{
    // Tag names are compared as complete, case-folded names. A custom element or
    // a prefixed name that merely starts with a resource tag is its own element
    // and keeps its attributes.
    QStringList const untouched = {
        QStringLiteral( "<img-preview src=\"a.png\">" ),
        QStringLiteral( "<scriptlet src=\"a.js\"></scriptlet>" ),
        QStringLiteral( "<link-preview href=\"style.css\">" ),
        QStringLiteral( "<svg:image src=\"a.png\"></svg:image>" ),
    };

    for ( QString const & source : untouched )
        QCOMPARE( LinkRewrite::applyBres( source, QStringLiteral( "d" ) ), source );

    QString const customAnchor = QStringLiteral( "<abbr-preview href=\"gdlookup://localhost/red fox\">x</abbr-preview>" );
    QCOMPARE( LinkRewrite::applyGdLookup( customAnchor ), customAnchor );

    QString const customAudio = QStringLiteral( "<audio-preview src=\"a.wav\">x</audio-preview>" );
    QCOMPARE( LinkRewrite::applyGdau( customAudio, QStringLiteral( "d" ) ), customAudio );

    // The real elements still match, whatever their case.
    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "<IMG SRC=\"a.png\">" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<IMG SRC=\"bres://d/a.png\">" ) );
}

void TstLinkRewrite::attributePaddingIsPreservedAroundTheRewrittenTarget()
{
    // Only the target span is edited; the padding stays outside it, so a padded
    // relative name is not silently changed into a different resource path.
    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "<img src=\" a.png \" >" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<img src=\" bres://d/a.png \" >" ) );

    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "<img src=\" /a.png \" >" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<img src=\" bres://d/a.png \" >" ) );

    QString const paddedIcon =
        LinkRewrite::applyGico( QStringLiteral( "<img src=\" gico:/dicticon.png \" >" ), QStringLiteral( "d" ) );
    QCOMPARE( paddedIcon, QStringLiteral( "<img src=\" gico://d/dicticon.png \" >" ) );

    // Each of the padded forms is stable on the second pass.
    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "<img src=\" bres://d/a.png \" >" ), QStringLiteral( "d" ) ),
        QStringLiteral( "<img src=\" bres://d/a.png \" >" ) );
    QCOMPARE( LinkRewrite::applyGico( paddedIcon, QStringLiteral( "d" ) ), paddedIcon );
}

void TstLinkRewrite::quotedCssUrlsKeepParenthesesAndEscapes()
{
    // Parentheses inside a quoted URL are part of the filename and do not
    // terminate the value.
    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "a{background:url(\"image(1).png\")}" ),
                  QStringLiteral( "d" ) ),
        QStringLiteral( "a{background:url(\"bres://d/image(1).png\")}" ) );

    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "a{background:url('image(1).png')}" ),
                  QStringLiteral( "d" ) ),
        QStringLiteral( "a{background:url('bres://d/image(1).png')}" ) );

    // An escaped delimiter inside the quotes does not end the value either.
    QCOMPARE( LinkRewrite::applyBres( QStringLiteral( "a{background:url(\"a\\\"b'.png\")}" ),
                  QStringLiteral( "d" ) ),
        QStringLiteral( "a{background:url(\"bres://d/a\\\"b'.png\")}" ) );

    // Two adjacent values stay two values: an unquoted one cannot absorb the
    // parenthesis of its neighbour.
    QCOMPARE( LinkRewrite::applyBres(
                  QStringLiteral( "a{background:url(a.png)}b{background:url(b.png)}" ), QStringLiteral( "d" ) ),
        QStringLiteral( "a{background:url(bres://d/a.png)}b{background:url(bres://d/b.png)}" ) );
}

void TstLinkRewrite::audioTagBoundariesAreRealTags()
{
    QString const closingInTitle =
        QStringLiteral( "<audio src=\"a.wav\"><span title=\"</audio>\">Play</span></audio>" );
    QString const rewritten = LinkRewrite::applyGdau( closingInTitle, QStringLiteral( "d" ) );

    // The element ends at the real closing tag, so the whole body stays inside
    // the wrapper and nothing is left dangling after it.
    QVERIFY( rewritten.endsWith( QStringLiteral( "<span title=\"</audio>\">Play</span>"
                                                 " <img src=\"qrcx://localhost/icons/playsound.png\""
                                                 " border=\"0\" alt=\"Play\"></a></span>" ) ) );
    QVERIFY( !rewritten.endsWith( QStringLiteral( "</audio>" ) ) );

    // A real `img` start tag suppresses the icon in every spelling the tag
    // scanner recognizes, including mixed case and a multiline tag.
    QString const uppercaseImage =
        QStringLiteral( "<audio src=\"a.wav\">Play <IMG SRC=\"x.png\"></audio>" );
    QVERIFY( !LinkRewrite::applyGdau( uppercaseImage, QStringLiteral( "d" ) )
                  .contains( QStringLiteral( "icons/playsound.png" ) ) );

    QString const multilineImage =
        QStringLiteral( "<audio src=\"a.wav\">Play <img\nsrc=\"x.png\"></audio>" );
    QVERIFY( !LinkRewrite::applyGdau( multilineImage, QStringLiteral( "d" ) )
                  .contains( QStringLiteral( "icons/playsound.png" ) ) );

    // The letters of an image tag inside a quoted value are text, so the body
    // genuinely has no image element and the icon is added.
    QString const quotedImageText =
        QStringLiteral( "<audio src=\"a.wav\"><span title=\"<img src=x>\">Play</span></audio>" );
    QCOMPARE( LinkRewrite::applyGdau( quotedImageText, QStringLiteral( "d" ) )
                  .count( QStringLiteral( "icons/playsound.png" ) ),
        qsizetype( 1 ) );
}

void TstLinkRewrite::audioSourceTextCannotBecomeScript()
{
    // The source text is serialized separately for its two contexts: a
    // JavaScript string literal in the script and an HTML attribute value in the
    // generated anchor. Neither a delimiter nor a statement break can leave the
    // context it was written into.
    QString const injected = QStringLiteral( "<audio src='a\";globalThis.injected=1;//'></audio>" );
    QString const rewritten = LinkRewrite::applyGdau( injected, QStringLiteral( "d" ) );

    QVERIFY2( !rewritten.contains( QStringLiteral( "a\";globalThis" ) ),
        "the source quote must not terminate the emitted JavaScript string" );
    QVERIFY2( rewritten.contains( QStringLiteral( "a\\\";globalThis.injected=1;//" ) ),
        "the escaped spelling must be the one that reaches the script" );
    QVERIFY2( !rewritten.contains( QStringLiteral( "href=\"gdau://d/a\";" ) ),
        "the HTML attribute must not be terminated by the source quote" );
    QVERIFY2( rewritten.contains( QStringLiteral( "href=\"gdau://d/a&quot;;globalThis.injected=1;//\"" ) ),
        "the attribute spelling must be HTML-escaped" );

    // A plain double quote in a single-quoted source stays a filename character
    // and breaks neither the script nor the anchor.
    QString const quotedName = LinkRewrite::applyGdau(
        QStringLiteral( "<audio src='a\"b.wav'></audio>" ), QStringLiteral( "d" ) );
    QVERIFY( quotedName.contains( QStringLiteral( "\"gdau://d/a\\\"b.wav\";" ) ) );
    QVERIFY( quotedName.contains( QStringLiteral( "href=\"gdau://d/a&quot;b.wav\"" ) ) );

    // A backslash is escaped for the script and kept verbatim in the attribute.
    QString const backslash = LinkRewrite::applyGdau(
        QStringLiteral( "<audio src=\"a\\b.wav\"></audio>" ), QStringLiteral( "d" ) );
    QVERIFY( backslash.contains( QStringLiteral( "\"gdau://d/a\\\\b.wav\";" ) ) );
    QVERIFY( backslash.contains( QStringLiteral( "href=\"gdau://d/a\\b.wav\"" ) ) );

    // A control character in the source is escaped in both contexts instead of
    // reaching the script or the attribute as a raw byte.
    QString const controlInput =
        QStringLiteral( "<audio src=\"a" ) + QChar( 0x01 ) + QStringLiteral( "b.wav\"></audio>" );
    QString const control = LinkRewrite::applyGdau( controlInput, QStringLiteral( "d" ) );

    QVERIFY( !control.contains( QChar( 0x01 ) ) );
    QVERIFY( control.contains( QStringLiteral( "\"gdau://d/a\\u0001b.wav\";" ) ) );
    QVERIFY( control.contains( QStringLiteral( "href=\"gdau://d/a&#1;b.wav\"" ) ) );
}

void TstLinkRewrite::audioSourceCannotTerminateTheWrapperScript()
{
    // The wrapper is not only a JavaScript string literal: it is emitted inside a
    // generated `<script>` element. Escaping the quote was necessary but not
    // sufficient — the HTML terminator is a second delimiter, and `<!--`/`-->`
    // select the tokenizer's escaped script states. Each payload therefore has
    // to be unable to spell `<` or `>` in the script body at all, while the
    // escaped spelling still decodes to the exact URL the player must receive.
    struct ScriptPayload
    {
        char const * source;      // article text that used to leave the script
        char const * jsBody;      // its exact escaped spelling in the literal
        char const * attribute;   // its exact escaped spelling in the `href`
    };

    ScriptPayload const payloads[] = {
        { "</script><script>globalThis.injected=1</script>",
            "\\x3C/script\\x3E\\x3Cscript\\x3EglobalThis.injected=1\\x3C/script\\x3E",
            "&lt;/script&gt;&lt;script&gt;globalThis.injected=1&lt;/script&gt;" },
        { "</SCRIPT ><script>globalThis.injected=1</script>",
            "\\x3C/SCRIPT \\x3E\\x3Cscript\\x3EglobalThis.injected=1\\x3C/script\\x3E",
            "&lt;/SCRIPT &gt;&lt;script&gt;globalThis.injected=1&lt;/script&gt;" },
        { "<!--<script>globalThis.injected=1</script>",
            "\\x3C!--\\x3Cscript\\x3EglobalThis.injected=1\\x3C/script\\x3E",
            "&lt;!--&lt;script&gt;globalThis.injected=1&lt;/script&gt;" },
        { "--><script>globalThis.injected=1</script>",
            "--\\x3E\\x3Cscript\\x3EglobalThis.injected=1\\x3C/script\\x3E",
            "--&gt;&lt;script&gt;globalThis.injected=1&lt;/script&gt;" },
    };

    for ( ScriptPayload const & payload : payloads )
    {
        QString const source = QString::fromLatin1( payload.source );
        QString const target = QStringLiteral( "gdau://d/" ) + source;
        QString const rewritten = LinkRewrite::applyGdau(
            QStringLiteral( "<audio src='" ) + source + QStringLiteral( "'></audio>" ),
            QStringLiteral( "d" ) );

        // One element, one terminator: the wrapper's own. A second start tag or a
        // second case-insensitive `</script` would be a script the parser runs
        // independently of the wrapper.
        QCOMPARE( rewritten.count( QStringLiteral( "<script" ), Qt::CaseInsensitive ), qsizetype( 1 ) );
        QCOMPARE( rewritten.count( QStringLiteral( "</script" ), Qt::CaseInsensitive ), qsizetype( 1 ) );

        std::optional<QString> const body = scriptText( rewritten );
        QVERIFY2( body.has_value(), qPrintable( QStringLiteral( "no closed script for %1" ).arg( source ) ) );

        // The HTML terminator and the comment markers cannot be spelled inside the
        // script body, whatever their case.
        QVERIFY2( !body->contains( QLatin1Char( '<' ) ) && !body->contains( QLatin1Char( '>' ) ),
            qPrintable( QStringLiteral( "raw angle bracket in the script body for %1: %2" )
                            .arg( source, *body ) ) );
        QVERIFY( !body->contains( QStringLiteral( "<!--" ) ) );
        QVERIFY( !body->contains( QStringLiteral( "-->" ) ) );
        QVERIFY( body->contains( QStringLiteral( "\\x3C" ) ) );
        QVERIFY( body->contains( QStringLiteral( "\\x3E" ) ) );

        // The escaped spelling is the one the wrapper consumes, and it decodes to
        // the untouched target rather than merely to something inert.
        QString const expectedScript = QString::fromStdString( addAudioLink(
            "\"gdau://d/" + std::string( payload.jsBody ) + "\"", "d" ) );
        QVERIFY2( rewritten.startsWith( expectedScript ),
            qPrintable( diff( source, QStringLiteral( "script wrapper" ),
                rewritten.left( expectedScript.size() ), expectedScript ) ) );
        std::optional<QString> const decoded = decodeJsStringLiteral(
            QStringLiteral( "gdau://d/" ) + QString::fromLatin1( payload.jsBody ) );
        QVERIFY2( decoded.has_value(), qPrintable( source ) );
        QCOMPARE( *decoded, target );

        // The anchor carries the same target through the HTML attribute context.
        QRegularExpressionMatch const href =
            QRegularExpression( QStringLiteral( "<a href=\"([^\"]*)\">" ) ).match( rewritten );
        QVERIFY( href.hasMatch() );
        std::optional<QString> const attribute = decodeHtmlAttributeValue( href.captured( 1 ) );
        QVERIFY2( attribute.has_value(), qPrintable( href.captured( 1 ) ) );
        QCOMPARE( *attribute, target );
        QCOMPARE( href.captured( 1 ), QStringLiteral( "gdau://d/" ) + QString::fromLatin1( payload.attribute ) );
    }
}

QTEST_GUILESS_MAIN( TstLinkRewrite )

#include "tst_linkrewrite.moc"
