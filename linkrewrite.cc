/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Centralized link rewriting.
 *
 * The dictionary engines all emit the same four internal URL schemes, but the
 * rules that produce them used to be duplicated with subtly different regular
 * expressions in stardict.cc, gls.cc, zim.cc, dictdfiles.cc, dictserver.cc,
 * website.cc and mdx.cc. They move here once, together with the evidence for
 * their behaviour (tests/fixtures/linkrewrite/cases.json), so an engine keeps
 * only its format-specific normalization: StarDict `bword:`/`#anchor` links,
 * DICT brace references, Zim title localization and inline-website URL
 * resolution still belong to the engine that recognizes the dialect.
 *
 * Contract of every rule in this file:
 *  - one scan over immutable source spans, so a replacement is never rescanned
 *    by the rule that produced it;
 *  - only the URL value span is edited: tag names, attribute names, quotes,
 *    surrounding whitespace and every unmatched byte are preserved;
 *  - a target is absolute when QUrl identifies a scheme, it begins `//`, or it
 *    is a fragment, and an absolute target is left intact;
 *  - common rules are idempotent: a second application is a no-op.
 */

#include "linkrewrite.hh"

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QUrl>

#include <optional>
#include <string>

#include "audiolink.hh"

namespace LinkRewrite
{

namespace
{

/// Hard bound on one scan. No rule here can match an empty string, so this can
/// only be reached by a programming error; it is still enforced so a broken
/// pattern fails the transform instead of wedging an article render.
constexpr int MaxMatchesPerScan = 100000;

QLatin1String const gdLookupScheme( "gdlookup://" );
QLatin1String const gicoScheme( "gico:" );
QLatin1String const audioClosingTag( "</audio>" );

QRegularExpression compileRule( QString const & pattern,
    QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption )
{
    QRegularExpression expression( pattern, options );

    if ( !expression.isValid() )
    {
        // A built-in pattern is a programming failure, not user input: report the
        // pattern and the error, and let the caller fail the current transform
        // instead of matching everything.
        qWarning( "LinkRewrite: invalid pattern '%s': %s",
            qPrintable( pattern ),
            qPrintable( expression.errorString() ) );
    }

    return expression;
}

/// A start tag with a quote-aware attribute region: a `>` inside a quoted value
/// cannot end the tag, and an unquoted region may span lines.
QRegularExpression const & tagRule()
{
    static QRegularExpression const rule = compileRule(
        QStringLiteral( "<(?<tag>[A-Za-z][A-Za-z0-9]*)(?<attrs>(?:\"[^\"]*\"|'[^']*'|[^>\"'])*)>" ) );

    return rule;
}

/// One attribute with a value in double quotes, single quotes, or bare form.
QRegularExpression const & attributeRule()
{
    static QRegularExpression const rule = compileRule(
        QStringLiteral( "(?<name>[A-Za-z_:][-A-Za-z0-9_:.]*)\\s*=\\s*"
                        "(?<value>\"(?<double>[^\"]*)\"|'(?<single>[^']*)'|(?<bare>[^\\s\"'=<>`]+))" ) );

    return rule;
}

/// A CSS `url(...)` value. The value class excludes quotes and parentheses so
/// one url() can never swallow the next one on the same line; a value that does
/// contain an unescaped parenthesis is left untouched rather than mis-sliced.
QRegularExpression const & cssUrlRule()
{
    static QRegularExpression const rule =
        compileRule( QStringLiteral( "url\\(\\s*(?<quote>[\"']?)(?<url>[^\"'()]*?)\\k<quote>\\s*\\)" ),
            QRegularExpression::CaseInsensitiveOption );

    return rule;
}

/// Old-dictionary markup cleanup for a lookup headword: drop tags, turn a
/// non-breaking space entity into a space, and collapse whitespace.
QRegularExpression const & headwordTagRule()
{
    static QRegularExpression const rule = compileRule( QStringLiteral( "<[^>]*>" ) );

    return rule;
}

struct SpanEdit
{
    qsizetype start;
    qsizetype length;
    QString replacement;
};

/// Applies edits collected against one immutable source. Edits are emitted in
/// scan order; an edit that overlaps an earlier one is dropped so a rule can
/// never corrupt a span it already replaced.
QString applyEdits( QString const & source, QList<SpanEdit> const & edits )
{
    if ( edits.isEmpty() )
        return source;

    QString result;
    qsizetype cut = 0;

    for ( SpanEdit const & edit : edits )
    {
        if ( edit.start < cut || edit.start + edit.length > source.size() )
        {
            qWarning( "LinkRewrite: dropping an overlapping or out-of-range edit at %lld",
                static_cast<long long>( edit.start ) );
            continue;
        }

        result += source.mid( cut, edit.start - cut );
        result += edit.replacement;
        cut = edit.start + edit.length;
    }

    result += source.mid( cut );

    return result;
}

struct AttributeSite
{
    QString tagName;       ///< lowercased
    QString attributeName; ///< lowercased
    QString value;         ///< the value without its quotes
    qsizetype valueStart;  ///< absolute offset of the value body in the source
    qsizetype valueLength;
};

/// Visits every attribute value of every start tag in `source`.
template <typename Handler> QString rewriteAttributeValues( QString const & source, Handler const & handler )
{
    QRegularExpression const & tags = tagRule();
    QRegularExpression const & attributes = attributeRule();

    if ( !tags.isValid() || !attributes.isValid() )
        return source;

    QList<SpanEdit> edits;
    QRegularExpressionMatchIterator tagMatches = tags.globalMatch( source );
    int tagCount = 0;

    while ( tagMatches.hasNext() )
    {
        QRegularExpressionMatch const tag = tagMatches.next();

        if ( ++tagCount > MaxMatchesPerScan )
            return source;

        QString const attributeText = tag.captured( QStringLiteral( "attrs" ) );

        if ( attributeText.isEmpty() )
            continue;

        qsizetype const attributeTextStart = tag.capturedStart( QStringLiteral( "attrs" ) );
        QString const tagName = tag.captured( QStringLiteral( "tag" ) ).toLower();

        QRegularExpressionMatchIterator attributeMatches = attributes.globalMatch( attributeText );
        int attributeCount = 0;

        while ( attributeMatches.hasNext() )
        {
            QRegularExpressionMatch const attribute = attributeMatches.next();

            if ( ++attributeCount > MaxMatchesPerScan )
                return source;

            qsizetype bodyStart = -1;
            qsizetype bodyLength = 0;
            QString body;

            if ( attribute.capturedStart( QStringLiteral( "double" ) ) >= 0 )
            {
                bodyStart = attribute.capturedStart( QStringLiteral( "double" ) );
                bodyLength = attribute.capturedLength( QStringLiteral( "double" ) );
                body = attribute.captured( QStringLiteral( "double" ) );
            }
            else if ( attribute.capturedStart( QStringLiteral( "single" ) ) >= 0 )
            {
                bodyStart = attribute.capturedStart( QStringLiteral( "single" ) );
                bodyLength = attribute.capturedLength( QStringLiteral( "single" ) );
                body = attribute.captured( QStringLiteral( "single" ) );
            }
            else if ( attribute.capturedStart( QStringLiteral( "bare" ) ) >= 0 )
            {
                bodyStart = attribute.capturedStart( QStringLiteral( "bare" ) );
                bodyLength = attribute.capturedLength( QStringLiteral( "bare" ) );
                body = attribute.captured( QStringLiteral( "bare" ) );
            }

            if ( bodyStart < 0 )
                continue;

            AttributeSite site;
            site.tagName = tagName;
            site.attributeName = attribute.captured( QStringLiteral( "name" ) ).toLower();
            site.value = body;
            site.valueStart = attributeTextStart + bodyStart;
            site.valueLength = bodyLength;

            std::optional<QString> const replacement = handler( site );

            if ( replacement )
                edits.append( { site.valueStart, site.valueLength, *replacement } );
        }
    }

    return applyEdits( source, edits );
}

/// Visits every CSS `url(...)` value in `source`.
template <typename Handler> QString rewriteCssUrlValues( QString const & source, Handler const & handler )
{
    QRegularExpression const & rule = cssUrlRule();

    if ( !rule.isValid() )
        return source;

    QList<SpanEdit> edits;
    QRegularExpressionMatchIterator matches = rule.globalMatch( source );
    int count = 0;

    while ( matches.hasNext() )
    {
        QRegularExpressionMatch const match = matches.next();

        if ( ++count > MaxMatchesPerScan )
            return source;

        std::optional<QString> const replacement = handler( match.captured( QStringLiteral( "url" ) ) );

        if ( replacement )
            edits.append( { match.capturedStart( QStringLiteral( "url" ) ),
                match.capturedLength( QStringLiteral( "url" ) ),
                *replacement } );
    }

    return applyEdits( source, edits );
}

/// True when a resource target must not be prefixed: an empty value, a
/// protocol-relative reference, a fragment, or anything QUrl identifies as
/// having a scheme (http, data, qrcx, bres, gico, gdau, gdvideo, mailto, ...).
bool isAbsoluteTarget( QString const & target )
{
    if ( target.isEmpty() )
        return true;

    if ( target.startsWith( QLatin1String( "//" ) ) )
        return true;

    if ( target.startsWith( QLatin1Char( '#' ) ) )
        return true;

    return !QUrl( target ).scheme().isEmpty();
}

std::optional<QString> bresTarget( QString const & value, QString const & prefix )
{
    // Classification ignores surrounding whitespace; the emitted value keeps the
    // original bytes, so an unusual but explicit spelling is not "normalized".
    QString const classified = value.trimmed();

    if ( isAbsoluteTarget( classified ) )
        return std::nullopt;

    // Root-relative means the dictionary root: `/a.png` is `bres://dict/a.png`,
    // never `bres://dict//a.png`.
    if ( classified.startsWith( QLatin1Char( '/' ) ) )
        return prefix + classified.mid( 1 );

    // A relative target keeps its spelling before the prefix, including `./`
    // and `../` steps.
    return prefix + value;
}

/// Normalizes the path headword of an existing `gdlookup://localhost/` link.
std::optional<QString> gdLookupTarget( QString const & target )
{
    if ( !target.startsWith( gdLookupScheme, Qt::CaseInsensitive ) )
        return std::nullopt;

    // The authority is `localhost`; anything else (a port, another host, a
    // userinfo part) is not a lookup link this rule understands.
    QString const afterScheme = target.mid( gdLookupScheme.size() );
    qsizetype authorityEnd = afterScheme.size();

    for ( qsizetype index = 0; index < afterScheme.size(); ++index )
    {
        QChar const character = afterScheme.at( index );

        if ( character == QLatin1Char( '/' ) || character == QLatin1Char( '?' ) ||
             character == QLatin1Char( '#' ) )
        {
            authorityEnd = index;
            break;
        }
    }

    if ( afterScheme.left( authorityEnd ).compare( QLatin1String( "localhost" ), Qt::CaseInsensitive ) != 0 )
        return std::nullopt;

    QString const tail = afterScheme.mid( authorityEnd );

    // Split the structural suffix (`?query#fragment`) off the path before any
    // decoding, so `?`, `=`, `&` and `#` are never part of the headword. The
    // suffix keeps its original bytes.
    qsizetype pathEnd = tail.size();

    for ( qsizetype index = 0; index < tail.size(); ++index )
    {
        QChar const character = tail.at( index );

        if ( character == QLatin1Char( '?' ) || character == QLatin1Char( '#' ) )
        {
            pathEnd = index;
            break;
        }
    }

    QString const path = tail.left( pathEnd );

    if ( path.isEmpty() || !path.startsWith( QLatin1Char( '/' ) ) )
        return std::nullopt;

    QString const suffix = tail.mid( pathEnd );

    // Decode the path once, apply the old markup/entity/whitespace cleanup, then
    // re-encode once: `%20` never becomes `%2520` and a second pass is stable.
    QString headword = QUrl::fromPercentEncoding( path.mid( 1 ).toUtf8() );

    headword.replace( headwordTagRule(), QStringLiteral( " " ) );
    headword.replace( QLatin1String( "&nbsp;" ), QStringLiteral( " " ) );
    headword = headword.simplified();

    QString const encoded = QString::fromLatin1( QUrl::toPercentEncoding( headword ) );

    return QStringLiteral( "gdlookup://localhost/" ) + encoded + suffix;
}

/// Qualifies an explicitly unqualified icon target (`gico:/PATH`,
/// `gico:///PATH`) and leaves a qualified `gico://DICT/PATH` alone.
std::optional<QString> gicoTarget( QString const & value, QString const & dictId )
{
    QString const target = value.trimmed();

    if ( !target.startsWith( gicoScheme, Qt::CaseInsensitive ) )
        return std::nullopt;

    qsizetype position = gicoScheme.size();
    int slashCount = 0;

    while ( position < target.size() && target.at( position ) == QLatin1Char( '/' ) )
    {
        ++position;
        ++slashCount;
    }

    // `gico:/PATH` (empty authority, one slash) and `gico:///PATH` (empty
    // authority, three slashes) are the only unqualified forms. `gico://HOST/`
    // is already qualified, and `gico:PATH` is not an icon target.
    if ( slashCount != 1 && slashCount != 3 )
        return std::nullopt;

    return QStringLiteral( "gico://" ) + dictId + QLatin1Char( '/' ) + target.mid( position );
}

QString audioWrapper( QString const & dictId, QString const & source, QString const & body )
{
    // The href is passed to the existing writer with its surrounding quotes,
    // exactly as the engines did before the rules moved here.
    std::string const href = "\"gdau://" + dictId.toStdString() + "/" + source.toStdString() + "\"";
    std::string const dictionaryId = dictId.toStdString();

    QString markup = QString::fromStdString( addAudioLink( href, dictionaryId ) );
    markup += QStringLiteral( "<span class=\"sdict_h_wav\"><a href=" );
    markup += QString::fromStdString( href );
    markup += QLatin1Char( '>' );
    markup += body;

    // Keep the original check verbatim: a body that already carries an `<img `
    // tag gets no second play icon.
    if ( body.indexOf( QLatin1String( "<img " ) ) < 0 )
        markup += QStringLiteral( " <img src=\"qrcx://localhost/icons/playsound.png\""
                                  " border=\"0\" alt=\"Play\">" );

    markup += QLatin1String( "</a></span>" );

    return markup;
}

/// Returns the value of the first `name` attribute of one tag match, or a null
/// optional when the tag carries no such attribute.
std::optional<QString> attributeOf( QRegularExpressionMatch const & tag, QString const & name )
{
    QRegularExpression const & attributes = attributeRule();

    if ( !attributes.isValid() )
        return std::nullopt;

    QString const attributeText = tag.captured( QStringLiteral( "attrs" ) );
    QRegularExpressionMatchIterator matches = attributes.globalMatch( attributeText );

    while ( matches.hasNext() )
    {
        QRegularExpressionMatch const attribute = matches.next();

        if ( attribute.captured( QStringLiteral( "name" ) ).compare( name, Qt::CaseInsensitive ) != 0 )
            continue;

        if ( attribute.capturedStart( QStringLiteral( "double" ) ) >= 0 )
            return attribute.captured( QStringLiteral( "double" ) );

        if ( attribute.capturedStart( QStringLiteral( "single" ) ) >= 0 )
            return attribute.captured( QStringLiteral( "single" ) );

        return attribute.captured( QStringLiteral( "bare" ) );
    }

    return std::nullopt;
}

} // namespace

QString applyGdLookup( QString const & html )
{
    return rewriteAttributeValues( html,
        []( AttributeSite const & site ) -> std::optional<QString>
        {
            if ( site.tagName != QLatin1String( "a" ) || site.attributeName != QLatin1String( "href" ) )
                return std::nullopt;

            return gdLookupTarget( site.value );
        } );
}

QString applyBres( QString const & html, QString const & dictId )
{
    QString const prefix = QStringLiteral( "bres://" ) + dictId + QLatin1Char( '/' );

    QString const withCssUrls = rewriteCssUrlValues(
        html, [&prefix]( QString const & value ) { return bresTarget( value, prefix ); } );

    return rewriteAttributeValues( withCssUrls,
        [&prefix]( AttributeSite const & site ) -> std::optional<QString>
        {
            bool isResourceTarget = false;

            if ( site.tagName == QLatin1String( "img" ) || site.tagName == QLatin1String( "script" ) )
                isResourceTarget = site.attributeName == QLatin1String( "src" );
            else if ( site.tagName == QLatin1String( "link" ) )
                isResourceTarget = site.attributeName == QLatin1String( "href" );

            if ( !isResourceTarget )
                return std::nullopt;

            return bresTarget( site.value, prefix );
        } );
}

QString applyGico( QString const & html, QString const & dictId )
{
    return rewriteAttributeValues( html,
        [&dictId]( AttributeSite const & site ) -> std::optional<QString>
        {
            if ( site.attributeName != QLatin1String( "src" ) &&
                 site.attributeName != QLatin1String( "href" ) )
                return std::nullopt;

            return gicoTarget( site.value, dictId );
        } );
}

QString applyGdau( QString const & html, QString const & dictId )
{
    QRegularExpression const & tags = tagRule();

    if ( !tags.isValid() )
        return html;

    QList<SpanEdit> edits;
    QRegularExpressionMatchIterator tagMatches = tags.globalMatch( html );
    int tagCount = 0;
    qsizetype coveredUntil = 0;

    while ( tagMatches.hasNext() )
    {
        QRegularExpressionMatch const tag = tagMatches.next();

        if ( ++tagCount > MaxMatchesPerScan )
            return html;

        // Already part of an audio element this pass replaced.
        if ( tag.capturedStart() < coveredUntil )
            continue;

        if ( tag.captured( QStringLiteral( "tag" ) )
                 .compare( QLatin1String( "audio" ), Qt::CaseInsensitive ) != 0 )
            continue;

        std::optional<QString> const source = attributeOf( tag, QStringLiteral( "src" ) );

        if ( !source )
            continue;

        QString const & target = *source;

        // Absolute and protocol-relative audio is the engine's business, and an
        // already qualified `gdau://` link is a no-op second pass.
        if ( isAbsoluteTarget( target.trimmed() ) )
            continue;

        qsizetype const closingStart =
            html.indexOf( audioClosingTag, tag.capturedEnd(), Qt::CaseInsensitive );

        if ( closingStart < 0 )
            continue;

        qsizetype const closingEnd = closingStart + audioClosingTag.size();
        QString const body = html.mid( tag.capturedEnd(), closingStart - tag.capturedEnd() );

        edits.append(
            { tag.capturedStart(), closingEnd - tag.capturedStart(), audioWrapper( dictId, target, body ) } );

        coveredUntil = closingEnd;
    }

    return applyEdits( html, edits );
}

} // namespace LinkRewrite
