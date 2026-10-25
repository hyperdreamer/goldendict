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
 *  - one tokenizing scan of the immutable source. Every tag, attribute value
 *    and CSS `url()` site is identified in the original text and classified
 *    once; the collected set of non-overlapping span edits is then applied in a
 *    single pass, so a replacement is never rescanned and a rule can never edit
 *    text another rule emitted;
 *  - only the classified value span is edited: tag names, attribute names,
 *    quotes, surrounding whitespace and every unmatched byte are preserved;
 *  - `url(...)` is recognized only where CSS actually lives - outside tags and
 *    inside `style` attribute values - so the word "url(" inside a descriptive
 *    attribute, a resource path or a query string keeps its bytes;
 *  - tag names are compared as complete, case-folded names, so a custom element
 *    such as `<img-preview>` is never treated as `<img>`;
 *  - a target is absolute when QUrl identifies a scheme, it begins `//`, or it
 *    is a fragment, and an absolute target is left intact;
 *  - common rules are idempotent: a second application is a no-op.
 */

#include "linkrewrite.hh"

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QUrl>

#include <algorithm>
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

/// A start tag. The tag-name class consumes every character an HTML tag name may
/// contain, so the name is complete before the attribute region begins: a
/// custom element (`<img-preview>`) or a prefixed name (`<svg:image>`) can never
/// be classified as the element whose letters it shares. The attribute region is
/// quote-aware, so a `>` inside a quoted value cannot end the tag, and an
/// unquoted region may span lines.
QRegularExpression const & tagRule()
{
    static QRegularExpression const rule = compileRule(
        QStringLiteral( "<(?<tag>[A-Za-z][A-Za-z0-9:_-]*)(?<attrs>(?:\"[^\"]*\"|'[^']*'|[^>\"'])*)>" ) );

    return rule;
}

/// An end tag. Used to find the real boundary of an element: text that only
/// looks like `</audio>` inside a quoted attribute belongs to a start tag and is
/// therefore never seen here.
QRegularExpression const & endTagRule()
{
    static QRegularExpression const rule =
        compileRule( QStringLiteral( "</\\s*(?<tag>[A-Za-z][A-Za-z0-9:_-]*)\\s*>" ) );

    return rule;
}

/// Comments, CDATA sections, processing instructions and declarations. They are
/// consumed as one unit so the markup they carry is not treated as live tags.
QRegularExpression const & skippedRule()
{
    static QRegularExpression const rule =
        compileRule( QStringLiteral( "<!--(?s:.*?)-->|<![^>]*>|<\\?[^>]*>" ) );

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

/// One CSS `url(...)` value.
///  - a quoted value runs to its matching quote and may contain parentheses and
///    backslash escapes, so `url("image(1).png")` keeps its whole filename and
///    `url("a\"b.png")` keeps its escaped delimiter;
///  - an unquoted value may not contain a quote, a parenthesis, whitespace or a
///    second `(`, so one url() can never absorb the url() next to it.
QRegularExpression const & cssUrlRule()
{
    static QRegularExpression const rule = compileRule(
        QStringLiteral( "(?<![A-Za-z0-9_-])url\\(\\s*"
                        "(?:\"(?<double>(?:[^\"\\\\]|\\\\.)*)\""
                        "|'(?<single>(?:[^'\\\\]|\\\\.)*)'"
                        "|(?<bare>[^\"'()\\s][^\"'()]*?))\\s*\\)" ),
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

/// The ASCII whitespace HTML itself allows around an attribute value or a CSS
/// token. QChar::isSpace() would also strip Unicode spaces that are part of a
/// resource name, so the six ASCII spellings are listed explicitly.
bool isHtmlSpace( QChar character )
{
    return character == QLatin1Char( ' ' ) || character == QLatin1Char( '\t' ) ||
        character == QLatin1Char( '\n' ) || character == QLatin1Char( '\r' ) ||
        character == QLatin1Char( '\f' ) || character == QLatin1Char( '\v' );
}

/// Removes the ASCII whitespace HTML and CSS use as separators, leaving Unicode
/// spaces (which are part of a resource name) alone.
QString trimHtmlSpace( QString const & value )
{
    qsizetype begin = 0;
    qsizetype end = value.size();

    while ( begin < end && isHtmlSpace( value.at( begin ) ) )
        ++begin;

    while ( end > begin && isHtmlSpace( value.at( end - 1 ) ) )
        --end;

    return value.mid( begin, end - begin );
}

struct SpanEdit
{
    qsizetype start;
    qsizetype length;
    QString replacement;
};

/// Applies edits collected against one immutable source. Overlapping or
/// out-of-range edits are dropped instead of corrupting the output; a rule that
/// produces them is a programming error, which the warning reports.
QString applyEdits( QString const & source, QList<SpanEdit> edits )
{
    if ( edits.isEmpty() )
        return source;

    std::stable_sort( edits.begin(), edits.end(), []( SpanEdit const & left, SpanEdit const & right ) {
        return left.start < right.start;
    } );

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

/// One attribute value of one start tag, identified in the immutable source.
struct AttributeSite
{
    QString name;          ///< lowercased attribute name
    qsizetype valueStart;  ///< first byte of the value body, quotes excluded
    qsizetype valueLength;
    qsizetype coreStart;   ///< value body without the surrounding ASCII padding
    qsizetype coreLength;
    QString core;          ///< the value a rule classifies, padding excluded
};

/// One tag token of the immutable source.
struct TagSite
{
    enum Kind
    {
        Start,
        End,
        Other ///< comment, declaration or processing instruction
    };

    Kind kind = Other;
    QString name;    ///< lowercased tag name; empty for Other
    qsizetype start; ///< offset of `<`
    qsizetype end;   ///< offset one past `>`
    QList<AttributeSite> attributes; ///< start tags only
};

AttributeSite makeAttributeSite( QString const & source,
    QString const & name,
    qsizetype valueStart,
    qsizetype valueLength )
{
    qsizetype begin = valueStart;
    qsizetype end = valueStart + valueLength;

    while ( begin < end && isHtmlSpace( source.at( begin ) ) )
        ++begin;

    while ( end > begin && isHtmlSpace( source.at( end - 1 ) ) )
        --end;

    AttributeSite site;
    site.name = name.toLower();
    site.valueStart = valueStart;
    site.valueLength = valueLength;
    site.coreStart = begin;
    site.coreLength = end - begin;
    site.core = source.mid( begin, end - begin );

    return site;
}

/// Tokenizes `source` once. Every start tag carries the attribute sites found in
/// its own attribute region; end tags and skipped constructs are recorded so a
/// later rule can locate real element boundaries.
QList<TagSite> scanMarkup( QString const & source )
{
    QList<TagSite> tags;

    QRegularExpression const & startRule = tagRule();
    QRegularExpression const & endRule = endTagRule();
    QRegularExpression const & skipRule = skippedRule();
    QRegularExpression const & attributes = attributeRule();

    if ( !startRule.isValid() || !endRule.isValid() || !skipRule.isValid() || !attributes.isValid() )
        return tags;

    constexpr QRegularExpression::MatchType Match = QRegularExpression::NormalMatch;
    constexpr QRegularExpression::MatchOptions Anchored = QRegularExpression::AnchorAtOffsetMatchOption;

    qsizetype offset = 0;
    int tokenCount = 0;

    while ( offset < source.size() )
    {
        qsizetype const open = source.indexOf( QLatin1Char( '<' ), offset );

        if ( open < 0 )
            break;

        if ( ++tokenCount > MaxMatchesPerScan )
            break;

        QRegularExpressionMatch const skip = skipRule.match( source, open, Match, Anchored );

        if ( skip.hasMatch() )
        {
            tags.append( TagSite{ TagSite::Other, QString(), open, skip.capturedEnd(), {} } );
            offset = skip.capturedEnd();
            continue;
        }

        QRegularExpressionMatch const end = endRule.match( source, open, Match, Anchored );

        if ( end.hasMatch() )
        {
            tags.append( TagSite{ TagSite::End, end.captured( QStringLiteral( "tag" ) ).toLower(), open,
                end.capturedEnd(), {} } );
            offset = end.capturedEnd();
            continue;
        }

        QRegularExpressionMatch const begin = startRule.match( source, open, Match, Anchored );

        if ( !begin.hasMatch() )
        {
            // A `<` that opens neither an end tag nor a start tag is text, unless
            // it is the leftover of a malformed closing or opening tag: then the
            // rest of that construct is markup, not content, and no tag inside it
            // may be treated as live.
            if ( open + 1 < source.size() && source.at( open + 1 ) == QLatin1Char( '/' ) )
            {
                qsizetype const close = source.indexOf( QLatin1Char( '>' ), open );

                if ( close < 0 )
                    break;

                tags.append( TagSite{ TagSite::Other, QString(), open, close + 1, {} } );
                offset = close + 1;
                continue;
            }

            offset = open + 1;
            continue;
        }

        TagSite tag;
        tag.kind = TagSite::Start;
        tag.name = begin.captured( QStringLiteral( "tag" ) ).toLower();
        tag.start = open;
        tag.end = begin.capturedEnd();

        QString const attributeText = begin.captured( QStringLiteral( "attrs" ) );

        if ( !attributeText.isEmpty() )
        {
            qsizetype const attributeTextStart = begin.capturedStart( QStringLiteral( "attrs" ) );
            QRegularExpressionMatchIterator matches = attributes.globalMatch( attributeText );
            int attributeCount = 0;

            while ( matches.hasNext() )
            {
                QRegularExpressionMatch const attribute = matches.next();

                if ( ++attributeCount > MaxMatchesPerScan )
                    break;

                qsizetype bodyStart = -1;
                qsizetype bodyLength = 0;

                if ( attribute.capturedStart( QStringLiteral( "double" ) ) >= 0 )
                {
                    bodyStart = attribute.capturedStart( QStringLiteral( "double" ) );
                    bodyLength = attribute.capturedLength( QStringLiteral( "double" ) );
                }
                else if ( attribute.capturedStart( QStringLiteral( "single" ) ) >= 0 )
                {
                    bodyStart = attribute.capturedStart( QStringLiteral( "single" ) );
                    bodyLength = attribute.capturedLength( QStringLiteral( "single" ) );
                }
                else if ( attribute.capturedStart( QStringLiteral( "bare" ) ) >= 0 )
                {
                    bodyStart = attribute.capturedStart( QStringLiteral( "bare" ) );
                    bodyLength = attribute.capturedLength( QStringLiteral( "bare" ) );
                }

                if ( bodyStart < 0 )
                    continue;

                tag.attributes.append( makeAttributeSite( source,
                    attribute.captured( QStringLiteral( "name" ) ),
                    attributeTextStart + bodyStart,
                    bodyLength ) );
            }
        }

        tags.append( tag );
        offset = tag.end;
    }

    return tags;
}

/// Visits every CSS `url(...)` value that starts inside [from, to) and appends
/// only its value span, so quotes and surrounding whitespace survive.
template <typename Handler>
void collectCssUrlEdits( QString const & source,
    qsizetype from,
    qsizetype to,
    Handler const & handler,
    QList<SpanEdit> & edits )
{
    QRegularExpression const & rule = cssUrlRule();

    if ( !rule.isValid() || from >= to )
        return;

    QRegularExpressionMatchIterator matches = rule.globalMatch( source, from );
    int count = 0;

    while ( matches.hasNext() )
    {
        QRegularExpressionMatch const match = matches.next();

        if ( ++count > MaxMatchesPerScan )
            return;

        if ( match.capturedStart() >= to )
            return;

        qsizetype valueStart = -1;
        qsizetype valueLength = 0;
        bool quoted = false;

        if ( match.capturedStart( QStringLiteral( "double" ) ) >= 0 )
        {
            valueStart = match.capturedStart( QStringLiteral( "double" ) );
            valueLength = match.capturedLength( QStringLiteral( "double" ) );
            quoted = true;
        }
        else if ( match.capturedStart( QStringLiteral( "single" ) ) >= 0 )
        {
            valueStart = match.capturedStart( QStringLiteral( "single" ) );
            valueLength = match.capturedLength( QStringLiteral( "single" ) );
            quoted = true;
        }
        else if ( match.capturedStart( QStringLiteral( "bare" ) ) >= 0 )
        {
            valueStart = match.capturedStart( QStringLiteral( "bare" ) );
            valueLength = match.capturedLength( QStringLiteral( "bare" ) );
        }

        if ( valueStart < 0 || valueStart + valueLength > to )
            continue;

        QString const value = source.mid( valueStart, valueLength );
        std::optional<QString> const replacement = handler( value, quoted );

        if ( replacement )
            edits.append( { valueStart, valueLength, *replacement } );
    }
}

/// Rewrites one immutable source with a single non-overlapping edit set built
/// from the original spans: attribute values through `onAttribute`, CSS `url()`
/// values of the text outside tags and of `style` attribute values through
/// `onCssUrl` (which receives the value and whether CSS quoted it).
template <typename AttributeHandler, typename CssHandler>
QString rewriteMarkup( QString const & source,
    AttributeHandler const & onAttribute,
    CssHandler const & onCssUrl )
{
    QList<TagSite> const tags = scanMarkup( source );
    QList<SpanEdit> edits;
    qsizetype textStart = 0;

    for ( TagSite const & tag : tags )
    {
        collectCssUrlEdits( source, textStart, tag.start, onCssUrl, edits );

        for ( AttributeSite const & attribute : tag.attributes )
        {
            if ( attribute.name == QLatin1String( "style" ) )
            {
                // A style attribute carries CSS, and only its CSS values are
                // rewritten; the attribute value itself is not a URL.
                collectCssUrlEdits( source,
                    attribute.valueStart,
                    attribute.valueStart + attribute.valueLength,
                    onCssUrl,
                    edits );
                continue;
            }

            std::optional<QString> const replacement = onAttribute( tag.name, attribute );

            if ( replacement )
                edits.append( { attribute.coreStart, attribute.coreLength, *replacement } );
        }

        textStart = tag.end;
    }

    collectCssUrlEdits( source, textStart, source.size(), onCssUrl, edits );

    return applyEdits( source, edits );
}

std::optional<QString> noCssRule( QString const &, bool )
{
    return std::nullopt;
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

/// Classifies `value` as a relative resource, already trimmed by the caller, so
/// the emitted spelling keeps every byte that is not the dropped root slash.
std::optional<QString> bresTarget( QString const & value, QString const & prefix )
{
    if ( isAbsoluteTarget( value ) )
        return std::nullopt;

    // Root-relative means the dictionary root: `/a.png` is `bres://dict/a.png`,
    // never `bres://dict//a.png`.
    if ( value.startsWith( QLatin1Char( '/' ) ) )
        return prefix + value.mid( 1 );

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
    QString const target = value;

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

/// Escapes one resource name for the body of a double-quoted JavaScript string
/// literal that is embedded in a generated `<script>` element. Two grammars end
/// at this text, not one: the JavaScript literal ends at an unescaped `"`, and
/// the HTML script element ends at a case-insensitive `</script` end tag
/// (`<!--`/`-->` additionally select the tokenizer's escaped script states). The
/// angle brackets are therefore escaped for the HTML terminator exactly as the
/// quote is escaped for the literal, and `\uXXXX` cannot reintroduce either
/// spelling. Single quotes are deliberately left for `makeAudioLinkScript`, which
/// keeps its own escaping contract and therefore the pinned wrapper output.
QString jsStringBody( QString const & value )
{
    QString escaped;
    escaped.reserve( value.size() + 8 );

    for ( QChar const character : value )
    {
        ushort const code = character.unicode();

        switch ( code )
        {
            case '\\':
                escaped += QLatin1String( "\\\\" );
                break;

            case '"':
                escaped += QLatin1String( "\\\"" );
                break;

            // The wrapper is emitted inside a `<script>` element, so `<` and `>`
            // are as load-bearing as the string delimiter: a raw `</script`
            // would end the HTML element early, and `<!--` / `-->` would change
            // the tokenizer state. `\x3C` / `\x3E` evaluate to the original
            // characters, so the URL the player receives is unchanged.
            case '<':
                escaped += QLatin1String( "\\x3C" );
                break;

            case '>':
                escaped += QLatin1String( "\\x3E" );
                break;

            case '\n':
                escaped += QLatin1String( "\\n" );
                break;

            case '\r':
                escaped += QLatin1String( "\\r" );
                break;

            case '\t':
                escaped += QLatin1String( "\\t" );
                break;

            default:
                // Remaining control characters and the line/paragraph separators
                // cannot appear unescaped: they would either break the literal or
                // terminate the script statement.
                if ( code < 0x20 || code == 0x7f || code == 0x2028 || code == 0x2029 )
                    escaped += QStringLiteral( "\\u%1" ).arg( code, 4, 16, QLatin1Char( '0' ) );
                else
                    escaped += character;
        }
    }

    return escaped;
}

/// Escapes one resource name for a double-quoted HTML attribute value. The two
/// characters that can leave *this* quoting context are `&` and `"`, and the angle
/// brackets would open a tag if the value were ever re-embedded; a control
/// character becomes a character reference so a raw NUL can never be replaced by
/// the parser before the resource name is read. A single quote is deliberately
/// left alone: it is a legal filename character inside double quotes and keeping
/// it byte-identical preserves the canonical output of safe inputs.
QString htmlAttributeValue( QString const & value )
{
    QString escaped;
    escaped.reserve( value.size() + 8 );

    for ( QChar const character : value )
    {
        ushort const code = character.unicode();

        switch ( code )
        {
            case '&':
                escaped += QLatin1String( "&amp;" );
                break;

            case '"':
                escaped += QLatin1String( "&quot;" );
                break;

            case '<':
                escaped += QLatin1String( "&lt;" );
                break;

            case '>':
                escaped += QLatin1String( "&gt;" );
                break;

            default:
                if ( code < 0x20 || code == 0x7f )
                    escaped += QStringLiteral( "&#%1;" ).arg( code );
                else
                    escaped += character;
        }
    }

    return escaped;
}

/// Builds the existing canonical audio wrapper. The two contexts the target is
/// serialized into are escaped separately: the script receives a JavaScript
/// string literal, the generated anchor receives an HTML attribute value, so
/// neither a delimiter nor a control character in dictionary text can leave the
/// context it was written into.
QString audioWrapper( QString const & dictId, QString const & source, QString const & body, bool bodyHasImage )
{
    QString const target = QStringLiteral( "gdau://" ) + dictId + QLatin1Char( '/' ) + source;

    std::string const literal = "\"" + jsStringBody( target ).toStdString() + "\"";
    std::string const dictionaryId = dictId.toStdString();

    QString markup = QString::fromStdString( addAudioLink( literal, dictionaryId ) );
    markup += QStringLiteral( "<span class=\"sdict_h_wav\"><a href=\"" );
    markup += htmlAttributeValue( target );
    markup += QStringLiteral( "\">" );
    markup += body;

    if ( !bodyHasImage )
        markup += QStringLiteral( " <img src=\"qrcx://localhost/icons/playsound.png\""
                                  " border=\"0\" alt=\"Play\">" );

    markup += QLatin1String( "</a></span>" );

    return markup;
}

/// Returns the first attribute of `tag` named `name`, or a null optional when the
/// tag carries no such attribute.
std::optional<AttributeSite> attributeOf( TagSite const & tag, QString const & name )
{
    for ( AttributeSite const & attribute : tag.attributes )
    {
        if ( attribute.name == name )
            return attribute;
    }

    return std::nullopt;
}

} // namespace

QString applyGdLookup( QString const & html )
{
    return rewriteMarkup(
        html,
        []( QString const & tagName, AttributeSite const & site ) -> std::optional<QString>
        {
            if ( tagName != QLatin1String( "a" ) || site.name != QLatin1String( "href" ) )
                return std::nullopt;

            return gdLookupTarget( site.core );
        },
        noCssRule );
}

QString applyBres( QString const & html, QString const & dictId )
{
    QString const prefix = QStringLiteral( "bres://" ) + dictId + QLatin1Char( '/' );

    return rewriteMarkup(
        html,
        [&prefix]( QString const & tagName, AttributeSite const & site ) -> std::optional<QString>
        {
            bool isResourceTarget = false;

            if ( tagName == QLatin1String( "img" ) || tagName == QLatin1String( "script" ) )
                isResourceTarget = site.name == QLatin1String( "src" );
            else if ( tagName == QLatin1String( "link" ) )
                isResourceTarget = site.name == QLatin1String( "href" );

            if ( !isResourceTarget )
                return std::nullopt;

            return bresTarget( site.core, prefix );
        },
        [&prefix]( QString const & value, bool quoted ) -> std::optional<QString>
        {
            // Whitespace around an unquoted CSS value is CSS syntax, not part of
            // the name; inside quotes every byte belongs to the resource.
            return bresTarget( quoted ? value : trimHtmlSpace( value ), prefix );
        } );
}

QString applyGico( QString const & html, QString const & dictId )
{
    return rewriteMarkup(
        html,
        [&dictId]( QString const &, AttributeSite const & site ) -> std::optional<QString>
        {
            if ( site.name != QLatin1String( "src" ) && site.name != QLatin1String( "href" ) )
                return std::nullopt;

            return gicoTarget( site.core, dictId );
        },
        noCssRule );
}

QString applyGdau( QString const & html, QString const & dictId )
{
    QList<TagSite> const tags = scanMarkup( html );
    QList<SpanEdit> edits;
    qsizetype coveredUntil = 0;

    for ( qsizetype index = 0; index < tags.size(); ++index )
    {
        TagSite const & tag = tags.at( index );

        if ( tag.kind != TagSite::Start )
            continue;

        // Already part of an audio element this pass replaced.
        if ( tag.start < coveredUntil )
            continue;

        if ( tag.name != QLatin1String( "audio" ) )
            continue;

        std::optional<AttributeSite> const source = attributeOf( tag, QStringLiteral( "src" ) );

        if ( !source )
            continue;

        // Absolute and protocol-relative audio is the engine's business, and an
        // already qualified `gdau://` link is a no-op second pass.
        if ( isAbsoluteTarget( source->core ) )
            continue;

        // The element ends at the next real end tag. Quoted attribute text of
        // the intervening tags is part of those tag tokens, so a `</audio>`
        // spelled inside a `title` value cannot terminate the element.
        qsizetype closing = -1;

        for ( qsizetype next = index + 1; next < tags.size(); ++next )
        {
            if ( tags.at( next ).kind == TagSite::End && tags.at( next ).name == QLatin1String( "audio" ) )
            {
                closing = next;
                break;
            }
        }

        if ( closing < 0 )
            continue;

        TagSite const & endTag = tags.at( closing );
        QString const body = html.mid( tag.end, endTag.start - tag.end );

        // A real `img` start tag in BODY suppresses the play icon; the same
        // letters inside a quoted value do not, and `<IMG`/multiline `<img`
        // still count as the element they are.
        bool bodyHasImage = false;

        for ( qsizetype inner = index + 1; inner < closing; ++inner )
        {
            if ( tags.at( inner ).kind == TagSite::Start && tags.at( inner ).name == QLatin1String( "img" ) )
            {
                bodyHasImage = true;
                break;
            }
        }

        edits.append( { tag.start, endTag.end - tag.start,
            audioWrapper( dictId, source->core, body, bodyHasImage ) } );

        coveredUntil = endTag.end;
        index = closing;
    }

    return applyEdits( html, edits );
}

} // namespace LinkRewrite
