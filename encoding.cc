#include "encoding.hh"

#include "gddebug.hh"

#include <QChar>
#include <QLocale>
#include <QtGlobal>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iconv.h>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <langinfo.h>
#endif

namespace {

class IconvDescriptor
{
    iconv_t descriptor_;

public:
    IconvDescriptor( QString const & target, QString const & source ):
        descriptor_( (iconv_t)-1 )
    {
        const QByteArray targetBytes = target.toLatin1();
        const QByteArray sourceBytes = source.toLatin1();
        descriptor_ = iconv_open( targetBytes.constData(), sourceBytes.constData() );

        if ( descriptor_ == (iconv_t)-1 )
            throw std::runtime_error( "iconv_open failed" );
    }

    ~IconvDescriptor() noexcept
    {
        if ( descriptor_ != (iconv_t)-1 )
            iconv_close( descriptor_ );
    }

    iconv_t get() const noexcept { return descriptor_; }

    IconvDescriptor( IconvDescriptor const & ) = delete;
    IconvDescriptor & operator=( IconvDescriptor const & ) = delete;
};

bool isAsciiSpace( QChar const character )
{
    const ushort value = character.unicode();
    return value == ' ' || value == '\t' || value == '\r' || value == '\n'
        || value == '\f' || value == '\v';
}

QString trimAscii( QString value )
{
    int first = 0;
    int last = value.size();

    while ( first < last && isAsciiSpace( value.at( first ) ) )
        ++first;
    while ( last > first && isAsciiSpace( value.at( last - 1 ) ) )
        --last;

    return value.mid( first, last - first );
}

QString asciiUpper( QString value )
{
    for ( QChar & character : value )
    {
        const ushort code = character.unicode();
        if ( code >= 'a' && code <= 'z' )
            character = QChar( static_cast< ushort >( code - 'a' + 'A' ) );
    }

    return value;
}

bool isAsciiSeparator( QChar const character )
{
    return character == QLatin1Char( '-' ) || character == QLatin1Char( ' ' );
}

QString canonicalNameFor( QString const & input )
{
    const QString name = asciiUpper( trimAscii( input ) );

    if ( name.isEmpty() )
        return QString();

    if ( name == QLatin1String( "UTF8" ) || name == QLatin1String( "UTF-8" ) )
        return QStringLiteral( "UTF-8" );
    if ( name == QLatin1String( "UTF16" ) || name == QLatin1String( "UTF-16" ) )
        return QStringLiteral( "UTF-16" );
    if ( name == QLatin1String( "UTF32" ) || name == QLatin1String( "UTF-32" ) )
        return QStringLiteral( "UTF-32" );
    if ( name == QLatin1String( "UTF-16LE" ) || name == QLatin1String( "UTF-16BE" )
         || name == QLatin1String( "UTF-32LE" ) || name == QLatin1String( "UTF-32BE" ) )
        return name;

    if ( name == QLatin1String( "LATIN1" ) || name == QLatin1String( "ISO8859-1" )
         || name == QLatin1String( "ISO 8859-1" ) )
        return QStringLiteral( "ISO-8859-1" );

    if ( name == QLatin1String( "BIG-5" ) || name == QLatin1String( "BIG5" ) )
        return QStringLiteral( "BIG5" );
    if ( name == QLatin1String( "BIG5-HKSCS" ) )
        return QStringLiteral( "BIG5-HKSCS" );
    if ( name == QLatin1String( "SHIFT-JIS" ) || name == QLatin1String( "SHIFT_JIS" )
         || name == QLatin1String( "SJIS" ) )
        return QStringLiteral( "SHIFT_JIS" );
    if ( name == QLatin1String( "WINDOWS-1251" ) || name == QLatin1String( "CP1251" ) )
        return QStringLiteral( "WINDOWS-1251" );
    if ( name == QLatin1String( "EUC-JP" ) || name == QLatin1String( "EUCJP" ) )
        return QStringLiteral( "EUC-JP" );

    if ( name == QLatin1String( "GB2312" ) || name == QLatin1String( "GB18030" )
         || name == QLatin1String( "ISO-2022-JP" ) )
        return name;

    if ( name == QLatin1String( "ASCII" ) || name == QLatin1String( "US-ASCII" )
         || name == QLatin1String( "ANSI_X3.4-1968" ) || name == QLatin1String( "ISO646-US" ) )
        return QStringLiteral( "ASCII" );

    // ISO[ -]?8859[ -]?N is deliberately parsed without using a locale or a
    // host-specific alias table. The final iconv probe decides support.
    if ( name.startsWith( QLatin1String( "ISO" ) ) )
    {
        int position = 3;
        if ( position < name.size() && isAsciiSeparator( name.at( position ) ) )
            ++position;

        if ( name.mid( position, 4 ) != QLatin1String( "8859" ) )
            return QString();
        position += 4;

        if ( position < name.size() && isAsciiSeparator( name.at( position ) ) )
            ++position;

        const int numberStart = position;
        while ( position < name.size() && name.at( position ).isDigit() )
            ++position;

        if ( numberStart == position || position != name.size() )
            return QString();

        bool numberOk = false;
        const int number = name.mid( numberStart ).toInt( &numberOk );
        if ( !numberOk || number <= 0 )
            return QString();

        return QStringLiteral( "ISO-8859-%1" ).arg( number );
    }

    return QString();
}

bool canOpenDecode( QString const & canonicalName )
{
    try
    {
        IconvDescriptor descriptor( QStringLiteral( "UTF-32LE" ), canonicalName );
        return true;
    }
    catch ( ... )
    {
        return false;
    }
}

bool canOpenBothDirections( QString const & canonicalName )
{
    try
    {
        IconvDescriptor decoder( QStringLiteral( "UTF-32LE" ), canonicalName );
        IconvDescriptor encoder( canonicalName, QStringLiteral( "UTF-32LE" ) );
        return true;
    }
    catch ( ... )
    {
        return false;
    }
}

void appendScalar( QString & result, std::uint32_t scalar, bool & warningEmitted,
                   QString const & sourceName )
{
    if ( scalar > 0x10ffffU || ( scalar >= 0xd800U && scalar <= 0xdfffU ) )
    {
        result.append( QChar( 0xfffd ) );
        if ( !warningEmitted )
        {
            gdWarning( "Encoding decode recovered an invalid scalar in %s",
                       sourceName.toLatin1().constData() );
            warningEmitted = true;
        }
        return;
    }

    if ( scalar <= 0xffffU )
    {
        result.append( QChar( static_cast< ushort >( scalar ) ) );
        return;
    }

    scalar -= 0x10000U;
    result.append( QChar( static_cast< ushort >( 0xd800U + ( scalar >> 10 ) ) ) );
    result.append( QChar( static_cast< ushort >( 0xdc00U + ( scalar & 0x3ffU ) ) ) );
}

QString decodeWithIconv( QString const & sourceName, QByteArray const & input,
                         bool & warningEmitted )
{
    IconvDescriptor descriptor( QStringLiteral( "UTF-32LE" ), sourceName );
    std::vector< char > output( 64 );
    char * outputPointer = output.data();
    size_t outputLeft = output.size();
    char * inputPointer = const_cast< char * >( input.constData() );
    size_t inputLeft = static_cast< size_t >( input.size() );
    QString result;

    auto drainOutput = [&]() {
        const size_t used = output.size() - outputLeft;
        const size_t complete = used - used % 4;

        for ( size_t offset = 0; offset < complete; offset += 4 )
        {
            const auto byte = [&]( size_t index ) -> std::uint32_t {
                return static_cast< std::uint32_t >(
                    static_cast< unsigned char >( output[ offset + index ] ) );
            };
            const std::uint32_t scalar = byte( 0 ) | ( byte( 1 ) << 8 )
                | ( byte( 2 ) << 16 ) | ( byte( 3 ) << 24 );
            appendScalar( result, scalar, warningEmitted, sourceName );
        }

        const size_t remainder = used - complete;
        if ( remainder != 0 )
            std::memmove( output.data(), output.data() + complete, remainder );

        outputPointer = output.data() + remainder;
        outputLeft = output.size() - remainder;
    };

    auto growOutput = [&]() {
        const size_t used = output.size() - outputLeft;
        if ( output.size() > std::numeric_limits< size_t >::max() / 2 )
            throw std::runtime_error( "iconv output buffer overflow" );
        output.resize( output.size() * 2 );
        outputPointer = output.data() + used;
        outputLeft = output.size() - used;
    };

    auto warnRecovery = [&]() {
        if ( !warningEmitted )
        {
            gdWarning( "Encoding decode recovered invalid input in %s",
                       sourceName.toLatin1().constData() );
            warningEmitted = true;
        }
    };

    while ( inputLeft != 0 )
    {
        errno = 0;
        const size_t converted = iconv( descriptor.get(), &inputPointer, &inputLeft,
                                        &outputPointer, &outputLeft );

        if ( converted != static_cast< size_t >( -1 ) )
        {
            drainOutput();
            continue;
        }

        if ( errno == E2BIG )
        {
            growOutput();
            drainOutput();
            continue;
        }

        if ( errno == EILSEQ )
        {
            drainOutput();
            result.append( QChar( 0xfffd ) );
            warnRecovery();
            ++inputPointer;
            --inputLeft;
            iconv( descriptor.get(), nullptr, nullptr, nullptr, nullptr );
            continue;
        }

        if ( errno == EINVAL )
        {
            drainOutput();
            result.append( QChar( 0xfffd ) );
            warnRecovery();
            inputLeft = 0;
            break;
        }

        throw std::runtime_error( "iconv decode failed" );
    }

    for ( ;; )
    {
        errno = 0;
        const size_t flushed = iconv( descriptor.get(), nullptr, nullptr,
                                      &outputPointer, &outputLeft );

        if ( flushed != static_cast< size_t >( -1 ) )
        {
            drainOutput();
            break;
        }

        if ( errno == E2BIG )
        {
            growOutput();
            drainOutput();
            continue;
        }

        drainOutput();
        result.append( QChar( 0xfffd ) );
        warnRecovery();
        break;
    }

    if ( output.size() - outputLeft != 0 )
    {
        // A UTF-32LE target must end on a scalar boundary. Treat an unexpected
        // partial target unit as one malformed sequence rather than truncating it.
        result.append( QChar( 0xfffd ) );
        warnRecovery();
    }

    return result;
}

QByteArray stripMatchingBom( QString const & sourceName, QByteArray const & data )
{
    if ( sourceName == QLatin1String( "UTF-8" ) && data.startsWith( QByteArray::fromHex( "efbbbf" ) ) )
        return data.mid( 3 );
    if ( sourceName == QLatin1String( "UTF-16LE" ) && data.startsWith( QByteArray::fromHex( "fffe" ) ) )
        return data.mid( 2 );
    if ( sourceName == QLatin1String( "UTF-16BE" ) && data.startsWith( QByteArray::fromHex( "feff" ) ) )
        return data.mid( 2 );
    if ( sourceName == QLatin1String( "UTF-32LE" ) && data.startsWith( QByteArray::fromHex( "fffe0000" ) ) )
        return data.mid( 4 );
    if ( sourceName == QLatin1String( "UTF-32BE" ) && data.startsWith( QByteArray::fromHex( "0000feff" ) ) )
        return data.mid( 4 );
    return data;
}

std::string unrepresentableContext( QString const & target, size_t scalarIndex )
{
    return QStringLiteral( "for %1 at scalar index %2" )
        .arg( target )
        .arg( static_cast< qulonglong >( scalarIndex ) )
        .toStdString();
}

void throwUnrepresentable( QString const & target, size_t scalarIndex )
{
    throw Encoding::exUnrepresentable( unrepresentableContext( target, scalarIndex ) );
}

QByteArray makeUtf32Le( QString const & text, QString const & targetName )
{
    QByteArray scalars;
    scalars.reserve( text.size() * 4 );
    size_t scalarIndex = 0;

    for ( qsizetype position = 0; position < text.size(); ++position, ++scalarIndex )
    {
        const ushort first = text.at( position ).unicode();
        std::uint32_t scalar = first;

        if ( first >= 0xd800U && first <= 0xdbffU )
        {
            if ( position + 1 >= text.size() )
                throwUnrepresentable( targetName, scalarIndex );

            const ushort second = text.at( position + 1 ).unicode();
            if ( second < 0xdc00U || second > 0xdfffU )
                throwUnrepresentable( targetName, scalarIndex );

            scalar = 0x10000U + ( static_cast< std::uint32_t >( first - 0xd800U ) << 10 )
                + static_cast< std::uint32_t >( second - 0xdc00U );
            ++position;
        }
        else if ( first >= 0xdc00U && first <= 0xdfffU )
        {
            throwUnrepresentable( targetName, scalarIndex );
        }

        scalars.append( static_cast< char >( scalar & 0xffU ) );
        scalars.append( static_cast< char >( ( scalar >> 8 ) & 0xffU ) );
        scalars.append( static_cast< char >( ( scalar >> 16 ) & 0xffU ) );
        scalars.append( static_cast< char >( ( scalar >> 24 ) & 0xffU ) );

    }

    return scalars;
}

bool isNameByte( char byte )
{
    const unsigned char value = static_cast< unsigned char >( byte );
    return ( value >= 'a' && value <= 'z' ) || ( value >= 'A' && value <= 'Z' )
        || ( value >= '0' && value <= '9' ) || value == '-' || value == '_' || value == ':';
}

bool isHtmlSpace( char byte )
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' || byte == '\f';
}

QByteArray asciiLowerBytes( QByteArray value )
{
    for ( char & byte : value )
    {
        const unsigned char code = static_cast< unsigned char >( byte );
        if ( code >= 'A' && code <= 'Z' )
            byte = static_cast< char >( code - 'A' + 'a' );
    }
    return value;
}

QByteArray trimAsciiBytes( QByteArray value )
{
    int first = 0;
    int last = value.size();
    while ( first < last && isHtmlSpace( value.at( first ) ) )
        ++first;
    while ( last > first && isHtmlSpace( value.at( last - 1 ) ) )
        --last;
    return value.mid( first, last - first );
}

QByteArray readHtmlValue( QByteArray const & input, int & position, int end )
{
    while ( position < end && isHtmlSpace( input.at( position ) ) )
        ++position;

    if ( position >= end )
        return QByteArray();

    const char quote = input.at( position );
    if ( quote == '\'' || quote == '"' )
    {
        ++position;
        const int start = position;
        while ( position < end && input.at( position ) != quote )
            ++position;
        const QByteArray result = input.mid( start, position - start );
        if ( position < end )
            ++position;
        return result;
    }

    const int start = position;
    while ( position < end && !isHtmlSpace( input.at( position ) )
            && input.at( position ) != '>' )
        ++position;
    return input.mid( start, position - start );
}

struct HtmlAttribute
{
    QByteArray name;
    QByteArray value;
};

std::vector< HtmlAttribute > parseHtmlAttributes( QByteArray const & input, int begin, int end )
{
    std::vector< HtmlAttribute > attributes;
    int position = begin;

    while ( position < end )
    {
        while ( position < end && ( isHtmlSpace( input.at( position ) ) || input.at( position ) == '/' ) )
            ++position;
        if ( position >= end )
            break;

        const int nameStart = position;
        while ( position < end && !isHtmlSpace( input.at( position ) )
                && input.at( position ) != '=' && input.at( position ) != '/'
                && input.at( position ) != '>' )
            ++position;
        if ( nameStart == position )
        {
            ++position;
            continue;
        }

        HtmlAttribute attribute;
        attribute.name = asciiLowerBytes( input.mid( nameStart, position - nameStart ) );
        while ( position < end && isHtmlSpace( input.at( position ) ) )
            ++position;

        if ( position < end && input.at( position ) == '=' )
        {
            ++position;
            attribute.value = readHtmlValue( input, position, end );
        }
        attributes.push_back( std::move( attribute ) );
    }

    return attributes;
}

std::vector< QByteArray > charsetParameters( QByteArray const & input )
{
    std::vector< QByteArray > candidates;
    int position = 0;
    const int end = input.size();

    while ( position < end )
    {
        while ( position < end && ( isHtmlSpace( input.at( position ) ) || input.at( position ) == ';' ) )
            ++position;

        const int nameStart = position;
        while ( position < end && input.at( position ) != '=' && input.at( position ) != ';' )
            ++position;
        const QByteArray name = asciiLowerBytes( trimAsciiBytes( input.mid( nameStart, position - nameStart ) ) );
        if ( position >= end )
            break;
        if ( input.at( position ) == ';' )
        {
            ++position;
            continue;
        }

        ++position; // '='
        while ( position < end && isHtmlSpace( input.at( position ) ) )
            ++position;

        QByteArray value;
        if ( position < end && ( input.at( position ) == '\'' || input.at( position ) == '"' ) )
        {
            const char quote = input.at( position++ );
            while ( position < end && input.at( position ) != quote )
            {
                // HTTP quoted-pairs protect both a quote and a separator in
                // an unrelated parameter's value. Single quotes are accepted
                // as well for the legacy Content-Type spellings we support.
                if ( input.at( position ) == '\\' && position + 1 < end )
                    ++position;
                value.append( input.at( position++ ) );
            }
            if ( position >= end )
                break;
            ++position;
            while ( position < end && isHtmlSpace( input.at( position ) ) )
                ++position;
        }
        else
        {
            const int valueStart = position;
            while ( position < end && input.at( position ) != ';' )
                ++position;
            value = input.mid( valueStart, position - valueStart );
        }

        if ( name == QByteArrayLiteral( "charset" )
             && ( position == end || input.at( position ) == ';' ) )
            candidates.push_back( trimAsciiBytes( value ) );

        // Discard any malformed suffix without treating its text as another
        // parameter. Only a top-level semicolon starts the next parameter.
        while ( position < end && input.at( position ) != ';' )
            ++position;
    }

    return candidates;
}

int htmlTagEnd( QByteArray const & source, int position )
{
    const int end = source.size();
    while ( position < end )
    {
        const char byte = source.at( position );
        if ( byte == '>' )
            return position;
        if ( byte == '=' )
        {
            ++position;
            readHtmlValue( source, position, end );
        }
        else if ( byte == '\'' || byte == '"' )
            readHtmlValue( source, position, end );
        else
            ++position;
    }
    return -1;
}

struct MetaCharsetCandidates
{
    std::vector< QByteArray > direct;
    std::vector< QByteArray > httpContentType;
};

MetaCharsetCandidates metaCharsetCandidates( QByteArray const & source )
{
    const QByteArray lower = asciiLowerBytes( source );
    MetaCharsetCandidates candidates;
    int position = 0;

    while ( position < lower.size() )
    {
        const int start = lower.indexOf( '<', position );
        if ( start < 0 || start + 1 >= lower.size() )
            break;

        if ( lower.mid( start, 4 ) == QByteArrayLiteral( "<!--" ) )
        {
            const int commentEnd = lower.indexOf( QByteArrayLiteral( "-->" ), start + 4 );
            if ( commentEnd < 0 )
                break;
            position = commentEnd + 3;
            continue;
        }

        const int nameStart = start + 1;
        const char first = lower.at( nameStart );
        if ( !( first >= 'a' && first <= 'z' ) && first != '/' && first != '!' && first != '?' )
        {
            position = nameStart;
            continue;
        }

        int afterName = nameStart;
        while ( afterName < lower.size() && isNameByte( lower.at( afterName ) ) )
            ++afterName;
        const bool isMeta = lower.mid( nameStart, afterName - nameStart ) == QByteArrayLiteral( "meta" )
            && ( afterName == lower.size() || isHtmlSpace( lower.at( afterName ) )
                 || lower.at( afterName ) == '/' || lower.at( afterName ) == '>' );

        // Consume every whole tag, including unrelated tags, so a '<meta'
        // inside an attribute can never start a declaration of its own.
        const int end = htmlTagEnd( source, afterName );
        if ( end < 0 )
            break;
        position = end + 1;
        if ( !isMeta )
            continue;

        const std::vector< HtmlAttribute > attributes = parseHtmlAttributes( source, afterName, end );
        bool httpContentType = false;
        for ( HtmlAttribute const & attribute : attributes )
        {
            if ( attribute.name == QByteArrayLiteral( "http-equiv" )
                 && asciiLowerBytes( trimAsciiBytes( attribute.value ) )
                     == QByteArrayLiteral( "content-type" ) )
                httpContentType = true;
        }

        // Keep the two meta declaration forms in their own precedence levels;
        // preserve source order within each level.
        for ( HtmlAttribute const & attribute : attributes )
        {
            if ( attribute.name == QByteArrayLiteral( "charset" ) )
                candidates.direct.push_back( attribute.value );
            else if ( httpContentType && attribute.name == QByteArrayLiteral( "content" ) )
            {
                const std::vector< QByteArray > parsed = charsetParameters( attribute.value );
                candidates.httpContentType.insert( candidates.httpContentType.end(),
                                                   parsed.begin(), parsed.end() );
            }
        }
    }

    return candidates;
}

std::vector< QByteArray > xmlEncodingCandidates( QByteArray const & source )
{
    const QByteArray lower = asciiLowerBytes( source );
    std::vector< QByteArray > candidates;
    int position = 0;

    while ( position < lower.size() )
    {
        const int start = lower.indexOf( QByteArrayLiteral( "<?xml" ), position );
        if ( start < 0 )
            break;

        const int afterName = start + 5;
        if ( afterName < lower.size() && isNameByte( lower.at( afterName ) ) )
        {
            position = afterName;
            continue;
        }

        int end = lower.indexOf( QByteArrayLiteral( "?>" ), afterName );
        if ( end < 0 )
            end = lower.indexOf( '>', afterName );
        if ( end < 0 )
            break;

        const std::vector< HtmlAttribute > attributes = parseHtmlAttributes( source, afterName, end );
        for ( HtmlAttribute const & attribute : attributes )
        {
            if ( attribute.name == QByteArrayLiteral( "encoding" ) )
                candidates.push_back( attribute.value );
        }
        position = end + 1;
    }

    return candidates;
}

QByteArray bomName( QByteArray const & body )
{
    if ( body.startsWith( QByteArray::fromHex( "0000feff" ) ) )
        return QByteArrayLiteral( "UTF-32BE" );
    if ( body.startsWith( QByteArray::fromHex( "fffe0000" ) ) )
        return QByteArrayLiteral( "UTF-32LE" );
    if ( body.startsWith( QByteArray::fromHex( "efbbbf" ) ) )
        return QByteArrayLiteral( "UTF-8" );
    if ( body.startsWith( QByteArray::fromHex( "feff" ) ) )
        return QByteArrayLiteral( "UTF-16BE" );
    if ( body.startsWith( QByteArray::fromHex( "fffe" ) ) )
        return QByteArrayLiteral( "UTF-16LE" );
    return QByteArray();
}

std::unique_ptr< Encoding > htmlCandidate( QByteArray const & candidate, bool & warningEmitted )
{
    const QByteArray trimmed = trimAsciiBytes( candidate );
    QString name = QString::fromLatin1( trimmed );
    if ( asciiUpper( name ) == QLatin1String( "UNICODE" ) )
        name = QStringLiteral( "UTF-8" );

    std::unique_ptr< Encoding > encoding = Encoding::byName( name );
    if ( encoding )
        return encoding;

    if ( !warningEmitted )
    {
        gdWarning( "Ignoring unknown HTML charset candidate: %s", trimmed.constData() );
        warningEmitted = true;
    }
    return nullptr;
}

} // namespace

Encoding::Encoding() noexcept:
    canonicalName_(),
    decodeOnly_( false )
{
}

Encoding::Encoding( QString canonicalName, bool decodeOnly ) noexcept:
    canonicalName_( std::move( canonicalName ) ),
    decodeOnly_( decodeOnly )
{
}

std::unique_ptr< Encoding > Encoding::byName( QString const & name )
{
    const QString canonical = canonicalNameFor( name );
    if ( canonical.isEmpty() || !canOpenBothDirections( canonical ) )
        return nullptr;

    return std::unique_ptr< Encoding >( new Encoding( canonical, false ) );
}

Encoding Encoding::utf8()
{
    return Encoding( QStringLiteral( "UTF-8" ), false );
}

Encoding Encoding::forLocale() noexcept
{
    try
    {
#if defined(Q_OS_UNIX)
        const char * codeset = nl_langinfo( CODESET );
        if ( codeset != nullptr && *codeset != '\0' )
        {
            const QString canonical = canonicalNameFor( QString::fromLatin1( codeset ) );
            if ( !canonical.isEmpty() && canOpenDecode( canonical ) )
                return Encoding( canonical, true );
        }
#endif

        const QString localeName = QLocale::system().name();
        if ( localeName == QLatin1String( "C" ) || localeName == QLatin1String( "POSIX" ) )
        {
            const QString ascii = QStringLiteral( "ASCII" );
            if ( canOpenDecode( ascii ) )
                return Encoding( ascii, true );
        }
    }
    catch ( ... )
    {
    }

    return Encoding( QStringLiteral( "UTF-8" ), true );
}

Encoding Encoding::forHtml( QByteArray const & body, QByteArray const & contentTypeHeader,
                            QString const & fallback, DetectedBy * detectedBy )
{
    if ( detectedBy )
        *detectedBy = DetectedBy::Fallback;

    bool warningEmitted = false;
    const std::vector< QByteArray > headerCandidates = charsetParameters( contentTypeHeader );
    for ( QByteArray const & candidate : headerCandidates )
    {
        const std::unique_ptr< Encoding > encoding = htmlCandidate( candidate, warningEmitted );
        if ( encoding )
        {
            if ( detectedBy )
                *detectedBy = DetectedBy::Header;
            return *encoding;
        }
    }

    const QByteArray bom = bomName( body );
    if ( !bom.isEmpty() )
    {
        const std::unique_ptr< Encoding > encoding = htmlCandidate( bom, warningEmitted );
        if ( encoding )
        {
            if ( detectedBy )
                *detectedBy = DetectedBy::Bom;
            return *encoding;
        }
    }

    const QByteArray prefix = body.left( 1024 );
    const MetaCharsetCandidates metaCandidates = metaCharsetCandidates( prefix );
    for ( QByteArray const & candidate : metaCandidates.direct )
    {
        const std::unique_ptr< Encoding > encoding = htmlCandidate( candidate, warningEmitted );
        if ( encoding )
        {
            if ( detectedBy )
                *detectedBy = DetectedBy::Meta;
            return *encoding;
        }
    }
    for ( QByteArray const & candidate : metaCandidates.httpContentType )
    {
        const std::unique_ptr< Encoding > encoding = htmlCandidate( candidate, warningEmitted );
        if ( encoding )
        {
            if ( detectedBy )
                *detectedBy = DetectedBy::Meta;
            return *encoding;
        }
    }

    const std::vector< QByteArray > xmlCandidates = xmlEncodingCandidates( prefix );
    for ( QByteArray const & candidate : xmlCandidates )
    {
        const std::unique_ptr< Encoding > encoding = htmlCandidate( candidate, warningEmitted );
        if ( encoding )
        {
            if ( detectedBy )
                *detectedBy = DetectedBy::XmlDeclaration;
            return *encoding;
        }
    }

    const std::unique_ptr< Encoding > fallbackEncoding = htmlCandidate( fallback.toLatin1(), warningEmitted );
    if ( fallbackEncoding )
    {
        if ( detectedBy )
            *detectedBy = DetectedBy::Fallback;
        return *fallbackEncoding;
    }

    if ( detectedBy )
        *detectedBy = DetectedBy::Fallback;
    return utf8();
}

bool Encoding::isValid() const noexcept
{
    return !canonicalName_.isEmpty();
}

QString Encoding::name() const
{
    return canonicalName_;
}

QString Encoding::decode( QByteArray const & data ) const noexcept
{
    if ( data.isEmpty() )
        return QString();

    bool warningEmitted = false;
    if ( canonicalName_.isEmpty() )
    {
        gdWarning( "Invalid Encoding value; decoding through UTF-8" );
        warningEmitted = true;
    }
    else
    {
        try
        {
            return decodeWithIconv( canonicalName_, stripMatchingBom( canonicalName_, data ),
                                    warningEmitted );
        }
        catch ( ... )
        {
            if ( !warningEmitted )
            {
                gdWarning( "Encoding descriptor unavailable for %s; decoding through UTF-8",
                           canonicalName_.toLatin1().constData() );
                warningEmitted = true;
            }
        }
    }

    // Invalid values and descriptor failures share normalization and recovery.
    try
    {
        const QString sourceName = QStringLiteral( "UTF-8" );
        const QByteArray input = stripMatchingBom( sourceName, data );
        try
        {
            return decodeWithIconv( sourceName, input, warningEmitted );
        }
        catch ( ... )
        {
            // Qt consumes one initial UTF-8 BOM itself; use the original
            // bytes so a second consecutive U+FEFF remains ordinary data.
            return QString::fromUtf8( data );
        }
    }
    catch ( ... )
    {
        return QString();
    }
}

QByteArray Encoding::encode( QString const & text ) const
{
    if ( !isValid() )
        throw exUnrepresentable( unrepresentableContext( QStringLiteral( "<invalid>" ), 0 ) );

    if ( decodeOnly_ )
        throw exUnrepresentable( unrepresentableContext( canonicalName_, 0 )
                                 + ": decode-only locale encoding cannot encode" );

    const QByteArray scalars = makeUtf32Le( text, canonicalName_ );
    if ( scalars.isEmpty() )
        return QByteArray();

    try
    {
        IconvDescriptor descriptor( canonicalName_, QStringLiteral( "UTF-32LE" ) );
        std::vector< char > output( 64 );
        char * outputPointer = output.data();
        size_t outputLeft = output.size();
        char * inputPointer = const_cast< char * >( scalars.constData() );
        size_t inputLeft = static_cast< size_t >( scalars.size() );

        auto growOutput = [&]() {
            const size_t used = output.size() - outputLeft;
            if ( output.size() > std::numeric_limits< size_t >::max() / 2 )
                throw exUnrepresentable( unrepresentableContext( canonicalName_, 0 ) );
            output.resize( output.size() * 2 );
            outputPointer = output.data() + used;
            outputLeft = output.size() - used;
        };

        auto currentScalarIndex = [&]() -> size_t {
            const size_t offset = static_cast< size_t >( inputPointer - scalars.constData() );
            return offset / 4;
        };

        while ( inputLeft != 0 )
        {
            errno = 0;
            const size_t converted = iconv( descriptor.get(), &inputPointer, &inputLeft,
                                            &outputPointer, &outputLeft );
            if ( converted != static_cast< size_t >( -1 ) )
            {
                if ( converted != 0 )
                    throwUnrepresentable( canonicalName_, currentScalarIndex() );
                continue;
            }

            if ( errno == E2BIG )
            {
                growOutput();
                continue;
            }

            throwUnrepresentable( canonicalName_, currentScalarIndex() );
        }

        for ( ;; )
        {
            errno = 0;
            const size_t flushed = iconv( descriptor.get(), nullptr, nullptr,
                                          &outputPointer, &outputLeft );
            if ( flushed != static_cast< size_t >( -1 ) )
            {
                if ( flushed != 0 )
                    throwUnrepresentable( canonicalName_, text.size() );
                break;
            }

            if ( errno == E2BIG )
            {
                growOutput();
                continue;
            }

            throwUnrepresentable( canonicalName_, text.size() );
        }

        const size_t used = output.size() - outputLeft;
        return QByteArray( output.data(), static_cast< int >( used ) );
    }
    catch ( exUnrepresentable const & )
    {
        throw;
    }
    catch ( ... )
    {
        throw exUnrepresentable( unrepresentableContext( canonicalName_, 0 ) );
    }

}

QByteArray EncodingCallers::epwingSearchBytes( Encoding const & codec, QString const & word )
{
    if ( !codec.isValid() )
        return {};
    try
    {
        return codec.encode( word );
    }
    catch ( Encoding::exUnrepresentable const & )
    {
        // A failed search encoding is no candidate. In mixed books the caller
        // still tries GB2312 after EUC-JP; UTF-8 is never a search fallback.
        return {};
    }
}

QByteArray EncodingCallers::slobTextToUtf8( Encoding const & codec, QByteArray const & bytes )
{
    return codec.decode( bytes ).toUtf8();
}

QString EncodingCallers::zipFilenameForDisplay( Encoding const & locale, QByteArray const & bytes )
{
    return locale.decode( bytes );
}
