#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <future>
#include <thread>
#include <vector>

#include "encoding.hh"
#include "gddebug.hh"
#include "epwing_book.hh"
#include "support/fixtures.hh"
#include "website.hh"
// The SLOB reader and formatter are private to their production module. Include
// that module so these tests exercise the header-selection boundary directly.
#include "slob.cc"
#include <clocale>

namespace {

QJsonArray loadVectors()
{
    QFile file( TestSupport::fixturePath( QStringLiteral( "encoding/vectors.json" ) ) );
    if ( !file.open( QIODevice::ReadOnly ) )
        return QJsonArray();

    const QJsonDocument document = QJsonDocument::fromJson( file.readAll() );
    if ( !document.isObject() )
        return QJsonArray();

    return document.object().value( QStringLiteral( "vectors" ) ).toArray();
}

QString jsonString( QJsonObject const & object, QString const & key )
{
    return object.value( key ).toString();
}

} // namespace

class TstEncoding : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void allNamedConvertersOpen();
    void edgeVectorInventory();
    void fixedVectors_data();
    void fixedVectors();
    void aliasesAndUnknownNames();
    void encoderRejectsUnrepresentableText();
    void htmlDetectionPrecedenceAndBounds();
    void htmlDetectionSameLevelSourceOrder_data();
    void htmlDetectionSameLevelSourceOrder();
    void recoveryWarnsOncePerOperation();
    void localeValueIsDecodeOnly();
    void localeTransitionApiIsRetired();
    void ownedParserPatternsAreValid();
    void invalidOwnedParserPatternFailsOperation_data();
    void invalidOwnedParserPatternFailsOperation();
    void slobArticleFormattingPreservesNul();
    void websiteTemplateLeavesFailedEncodingPlaceholder();
    void websiteTemplateWarnsForEveryFailedEncoding();
    void websiteUnavailablePlaceholderCodecIsDiagnosed();
    void epwingRetryDoesNotFallBackToUtf8();
    void epwingSkippedCharsetIsDiagnosed();
    void slobHeaderCharsetFallback_data();
    void slobHeaderCharsetFallback();
    void slobLegacyDecodePreservesBytes();
    void zipRawFilenameIsNotDisplayText();
    void loggingEmitsUtf8WithoutLocaleMutation();
    void immutableValueConvertsInParallel();
};

void TstEncoding::initTestCase()
{
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
    TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
}

void TstEncoding::cleanupTestCase()
{
    TestSupport::clearWatchdog();
}

void TstEncoding::allNamedConvertersOpen()
{
    const QStringList names = {
        QStringLiteral( "UTF-8" ), QStringLiteral( "UTF-16" ), QStringLiteral( "UTF-32" ),
        QStringLiteral( "UTF-16LE" ), QStringLiteral( "UTF-16BE" ),
        QStringLiteral( "UTF-32LE" ), QStringLiteral( "UTF-32BE" ),
        QStringLiteral( "SHIFT_JIS" ), QStringLiteral( "BIG5" ), QStringLiteral( "BIG5-HKSCS" ),
        QStringLiteral( "GB18030" ), QStringLiteral( "GB2312" ), QStringLiteral( "EUC-JP" ),
        QStringLiteral( "WINDOWS-1251" ),
        QStringLiteral( "ISO-8859-1" ), QStringLiteral( "ISO-8859-2" ),
        QStringLiteral( "ISO-8859-3" ), QStringLiteral( "ISO-8859-4" ),
        QStringLiteral( "ISO-8859-5" ), QStringLiteral( "ISO-8859-6" ),
        QStringLiteral( "ISO-8859-7" ), QStringLiteral( "ISO-8859-8" ),
        QStringLiteral( "ISO-8859-9" ), QStringLiteral( "ISO-8859-10" ),
        QStringLiteral( "ISO-8859-11" ), QStringLiteral( "ISO-8859-13" ),
        QStringLiteral( "ISO-8859-14" ), QStringLiteral( "ISO-8859-15" ),
        QStringLiteral( "ISO-8859-16" )
    };

    for ( QString const & name : names )
    {
        const std::unique_ptr< Encoding > encoding = Encoding::byName( name );
        QVERIFY2( encoding, qPrintable( QStringLiteral( "converter did not open: %1" ).arg( name ) ) );
        QVERIFY2( encoding->isValid(), qPrintable( name ) );
        QCOMPARE( encoding->name(), name );
    }

    QVERIFY( !Encoding::byName( QStringLiteral( "" ) ) );
    QVERIFY( !Encoding::byName( QStringLiteral( "ISO-8859-12" ) ) );
}

void TstEncoding::edgeVectorInventory()
{
    const QStringList requiredIds = {
        QStringLiteral( "utf8-no-bom" ), QStringLiteral( "utf8-bom" ),
        QStringLiteral( "utf16le-no-bom" ), QStringLiteral( "utf16le-bom" ),
        QStringLiteral( "utf16be-no-bom" ), QStringLiteral( "utf16be-bom" ),
        QStringLiteral( "utf32le-no-bom" ), QStringLiteral( "utf32le-bom" ),
        QStringLiteral( "utf32be-no-bom" ), QStringLiteral( "utf32be-bom" ),
        QStringLiteral( "utf16-generic-no-bom" ), QStringLiteral( "utf16-generic-le-bom" ),
        QStringLiteral( "utf16-generic-be-bom" ),
        QStringLiteral( "utf32-generic-no-bom" ), QStringLiteral( "utf32-generic-le-bom" ),
        QStringLiteral( "utf32-generic-be-bom" ),
        QStringLiteral( "utf8-embedded-nul" ), QStringLiteral( "utf8-bad-bytes" ),
        QStringLiteral( "utf8-incomplete-tail" ), QStringLiteral( "utf8-bad-bytes-between-text" ),
        QStringLiteral( "invalid-value-utf8-fallback" ), QStringLiteral( "invalid-value-bad-bytes" ),
        QStringLiteral( "invalid-value-bom" ), QStringLiteral( "invalid-value-consecutive-boms" ),
        QStringLiteral( "invalid-value-interior-bom" ),
        QStringLiteral( "invalid-value-incompatible-bom" ), QStringLiteral( "utf8-interior-bom" ),
        QStringLiteral( "latin1-incompatible-bom" ),
        QStringLiteral( "utf16le-unpaired-high" ), QStringLiteral( "utf16le-unpaired-low" ),
        QStringLiteral( "utf16le-incomplete-unit" ),
        QStringLiteral( "utf16be-unpaired-high" ), QStringLiteral( "utf16be-unpaired-low" ),
        QStringLiteral( "utf16be-incomplete-unit" ),
        QStringLiteral( "iso2022jp-final-flush" ), QStringLiteral( "iso2022jp-unflushed-input" ),
        QStringLiteral( "utf8-output-growth" )
    };
    QSet< QString > ids;
    for ( QJsonValue const & value : loadVectors() )
    {
        const QString id = jsonString( value.toObject(), QStringLiteral( "id" ) );
        if ( !id.isEmpty() )
        {
            QVERIFY2( !ids.contains( id ), qPrintable( QStringLiteral( "duplicate fixture: %1" ).arg( id ) ) );
            ids.insert( id );
        }
    }
    for ( QString const & id : requiredIds )
        QVERIFY2( ids.contains( id ), qPrintable( QStringLiteral( "missing edge fixture: %1" ).arg( id ) ) );
}

void TstEncoding::fixedVectors_data()
{
    QTest::addColumn< QJsonObject >( "vector" );
    const QJsonArray vectors = loadVectors();
    QVERIFY2( !vectors.isEmpty(), "encoding/vectors.json must contain fixed vectors" );

    for ( QJsonValue const & value : vectors )
    {
        const QJsonObject vector = value.toObject();
        for ( QString const & key : { QStringLiteral( "encoding" ), QStringLiteral( "bytes" ),
                                      QStringLiteral( "text" ) } )
            QVERIFY2( vector.value( key ).isString(), qPrintable( QStringLiteral( "missing string field: %1" ).arg( key ) ) );
        QVERIFY2( vector.value( QStringLiteral( "encoded" ) ).isString()
                      || vector.value( QStringLiteral( "encodeError" ) ).toBool(),
                  "each fixture must specify reverse bytes or an encoding failure" );
        const QString id = vector.contains( QStringLiteral( "id" ) )
            ? jsonString( vector, QStringLiteral( "id" ) )
            : jsonString( vector, QStringLiteral( "encoding" ) ) + QLatin1Char( '-' )
                + jsonString( vector, QStringLiteral( "bytes" ) );
        QTest::newRow( qPrintable( id ) ) << vector;
    }
}

void TstEncoding::fixedVectors()
{
    QFETCH( QJsonObject, vector );
    const QString name = jsonString( vector, QStringLiteral( "encoding" ) );
    const bool invalidValue = vector.value( QStringLiteral( "invalidValue" ) ).toBool();
    const std::unique_ptr< Encoding > encoding = invalidValue
        ? std::make_unique< Encoding >() : Encoding::byName( name );
    QVERIFY2( encoding, qPrintable( name ) );
    QCOMPARE( encoding->isValid(), !invalidValue );

    const int repeat = vector.value( QStringLiteral( "repeat" ) ).toInt( 1 );
    QVERIFY( repeat > 0 );
    const QByteArray bytes = QByteArray::fromHex( jsonString( vector, QStringLiteral( "bytes" ) ).toLatin1() ).repeated( repeat );
    const QString expected = jsonString( vector, QStringLiteral( "text" ) ).repeated( repeat );
    QCOMPARE( encoding->decode( bytes ), expected );

    if ( vector.value( QStringLiteral( "encodeError" ) ).toBool() )
        QVERIFY_THROWS_EXCEPTION( Encoding::exUnrepresentable, encoding->encode( expected ) );
    else
        QCOMPARE( encoding->encode( expected ),
                  QByteArray::fromHex( jsonString( vector, QStringLiteral( "encoded" ) ).toLatin1() ).repeated( repeat ) );
}

void TstEncoding::aliasesAndUnknownNames()
{
    const QList< QPair< QString, QString > > aliases = {
        { QStringLiteral( " UTF8 " ), QStringLiteral( "UTF-8" ) },
        { QStringLiteral( "utf16" ), QStringLiteral( "UTF-16" ) },
        { QStringLiteral( "UTF32" ), QStringLiteral( "UTF-32" ) },
        { QStringLiteral( "Latin1" ), QStringLiteral( "ISO-8859-1" ) },
        { QStringLiteral( "ISO8859-1" ), QStringLiteral( "ISO-8859-1" ) },
        { QStringLiteral( "ISO 8859-1" ), QStringLiteral( "ISO-8859-1" ) },
        { QStringLiteral( "Big-5" ), QStringLiteral( "BIG5" ) },
        { QStringLiteral( "Big5-HKSCS" ), QStringLiteral( "BIG5-HKSCS" ) },
        { QStringLiteral( "Shift-JIS" ), QStringLiteral( "SHIFT_JIS" ) },
        { QStringLiteral( "Shift_JIS" ), QStringLiteral( "SHIFT_JIS" ) },
        { QStringLiteral( "SJIS" ), QStringLiteral( "SHIFT_JIS" ) },
        { QStringLiteral( "Windows-1251" ), QStringLiteral( "WINDOWS-1251" ) },
        { QStringLiteral( "CP1251" ), QStringLiteral( "WINDOWS-1251" ) },
        { QStringLiteral( "EUCJP" ), QStringLiteral( "EUC-JP" ) },
        { QStringLiteral( "ISO 8859 11" ), QStringLiteral( "ISO-8859-11" ) }
    };

    for ( auto const & alias : aliases )
    {
        const std::unique_ptr< Encoding > encoding = Encoding::byName( alias.first );
        QVERIFY2( encoding, qPrintable( alias.first ) );
        QCOMPARE( encoding->name(), alias.second );
    }

    QVERIFY( !Encoding::byName( QStringLiteral( "unknown-codepage" ) ) );
    QVERIFY( !Encoding::byName( QStringLiteral( "ISO-8859-12" ) ) );
    QVERIFY( !Encoding::byName( QStringLiteral( "  " ) ) );

    const auto sjis = Encoding::byName( QStringLiteral( "Shift_JIS" ) );
    QVERIFY( sjis );
    const QString japanese = QString::fromUtf8( "\xe6\x97\xa5\xe6\x9c\xac" );
    QCOMPARE( sjis->decode( QByteArray::fromHex( "93fa967b" ) ), japanese );
    QCOMPARE( sjis->encode( japanese ).toHex(), QByteArrayLiteral( "93fa967b" ) );

    const auto latin1 = Encoding::byName( QStringLiteral( "ISO-8859-1" ) );
    QVERIFY( latin1 );
    QVERIFY_THROWS_EXCEPTION( Encoding::exUnrepresentable,
                              latin1->encode( QString::fromUtf8( "\xe4\xb8\xad" ) ) );
}

void TstEncoding::encoderRejectsUnrepresentableText()
{
    const auto latin1 = Encoding::byName( QStringLiteral( "ISO-8859-1" ) );
    QVERIFY( latin1 );
    QVERIFY_THROWS_EXCEPTION( Encoding::exUnrepresentable,
                              latin1->encode( QString::fromUtf8( "\u4e2d" ) ) );
    QVERIFY_THROWS_EXCEPTION( Encoding::exUnrepresentable,
                              latin1->encode( QString( QChar( 0xd800 ) ) ) );

    const auto locale = Encoding::forLocale();
    QVERIFY( locale.isValid() );
    QVERIFY_THROWS_EXCEPTION( Encoding::exUnrepresentable, locale.encode( QStringLiteral( "x" ) ) );
}

void TstEncoding::htmlDetectionPrecedenceAndBounds()
{
    Encoding::DetectedBy detected = Encoding::DetectedBy::Fallback;

    const auto fromHeader = Encoding::forHtml( QByteArray::fromHex( "efbbbfe4b8ad" ),
                                               QByteArrayLiteral( "text/html; Charset = windows-1251" ),
                                               QStringLiteral( "ISO-8859-1" ), &detected );
    QVERIFY( fromHeader.isValid() );
    QCOMPARE( fromHeader.name(), QStringLiteral( "WINDOWS-1251" ) );
    QCOMPARE( detected, Encoding::DetectedBy::Header );

    const auto fromUtf32Bom = Encoding::forHtml( QByteArray::fromHex( "0000feff00004e2d" ), {}, {}, &detected );
    QCOMPARE( fromUtf32Bom.name(), QStringLiteral( "UTF-32BE" ) );
    QCOMPARE( detected, Encoding::DetectedBy::Bom );

    const auto fromMeta = Encoding::forHtml( QByteArrayLiteral( "<html><META foo=x charset = 'windows-1251'>" ),
                                             {}, {}, &detected );
    QCOMPARE( fromMeta.name(), QStringLiteral( "WINDOWS-1251" ) );
    QCOMPARE( detected, Encoding::DetectedBy::Meta );

    const auto fromHttpEquiv = Encoding::forHtml(
        QByteArrayLiteral( "<meta content=\"text/html; charset=Shift_JIS\" http-equiv = Content-Type>" ),
        {}, {}, &detected );
    QCOMPARE( fromHttpEquiv.name(), QStringLiteral( "SHIFT_JIS" ) );
    QCOMPARE( detected, Encoding::DetectedBy::Meta );

    const auto directMetaWins = Encoding::forHtml(
        QByteArrayLiteral( "<meta content=\"text/html; charset=Shift_JIS\" http-equiv=Content-Type>"
                           "<meta charset=GB2312>" ),
        {}, {}, &detected );
    QCOMPARE( directMetaWins.name(), QStringLiteral( "GB2312" ) );
    QCOMPARE( detected, Encoding::DetectedBy::Meta );

    const auto fromXml = Encoding::forHtml( QByteArrayLiteral( "<?xml version='1.0' encoding = \"EUCJP\"?>" ),
                                            {}, {}, &detected );
    QCOMPARE( fromXml.name(), QStringLiteral( "EUC-JP" ) );
    QCOMPARE( detected, Encoding::DetectedBy::XmlDeclaration );

    const auto unicode = Encoding::forHtml( QByteArrayLiteral( "<meta charset=unicode>" ), {}, {}, &detected );
    QCOMPARE( unicode.name(), QStringLiteral( "UTF-8" ) );
    QCOMPARE( detected, Encoding::DetectedBy::Meta );

    const auto unknownHeader = Encoding::forHtml( QByteArrayLiteral( "<meta charset=GB2312>" ),
                                                   QByteArrayLiteral( "text/html; charset=not-a-codepage" ),
                                                   QStringLiteral( "ISO-8859-1" ), &detected );
    QCOMPARE( unknownHeader.name(), QStringLiteral( "GB2312" ) );
    QCOMPARE( detected, Encoding::DetectedBy::Meta );

    const auto badFallback = Encoding::forHtml( QByteArrayLiteral( "plain" ), {},
                                                QStringLiteral( "ISO-8859-12" ), &detected );
    QCOMPARE( badFallback.name(), QStringLiteral( "UTF-8" ) );
    QCOMPARE( detected, Encoding::DetectedBy::Fallback );

    QByteArray beyondLimit( 1024, ' ' );
    beyondLimit += QByteArrayLiteral( "<meta charset=windows-1251>" );
    const auto bounded = Encoding::forHtml( beyondLimit, {}, QStringLiteral( "ISO-8859-1" ), &detected );
    QCOMPARE( bounded.name(), QStringLiteral( "ISO-8859-1" ) );
    QCOMPARE( detected, Encoding::DetectedBy::Fallback );
}

void TstEncoding::htmlDetectionSameLevelSourceOrder_data()
{
    QTest::addColumn< QByteArray >( "body" );
    QTest::addColumn< QByteArray >( "header" );
    QTest::addColumn< QString >( "expectedName" );
    QTest::addColumn< Encoding::DetectedBy >( "expectedSource" );

    QTest::newRow( "header-first-recognised" )
        << QByteArray()
        << QByteArrayLiteral( "text/html; charset=Shift_JIS; CHARSET = 'GB2312'" )
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Header;
    QTest::newRow( "header-unknown-then-known" )
        << QByteArray()
        << QByteArrayLiteral( "text/html; charset=unknown-codepage; charset=; charset=EUCJP; charset=UTF-8" )
        << QStringLiteral( "EUC-JP" ) << Encoding::DetectedBy::Header;

    QTest::newRow( "header-quoted-unrelated-parameter" )
        << QByteArray()
        << QByteArrayLiteral( "text/html; note=\"charset='BIG5'\"; charset=UTF-8" )
        << QStringLiteral( "UTF-8" ) << Encoding::DetectedBy::Header;
    QTest::newRow( "header-single-quoted-unrelated-parameter" )
        << QByteArray()
        << QByteArrayLiteral( "text/html; note='charset=\"BIG5\"'; charset=UTF-8" )
        << QStringLiteral( "UTF-8" ) << Encoding::DetectedBy::Header;
    QTest::newRow( "header-escaped-quote-and-separator" )
        << QByteArray()
        << QByteArrayLiteral( "text/html; note=\"a\\\"; charset='BIG5'\"; charset=UTF-8" )
        << QStringLiteral( "UTF-8" ) << Encoding::DetectedBy::Header;
    QTest::newRow( "header-escaped-charset-value" )
        << QByteArray()
        << QByteArrayLiteral( "text/html; charset=\"Shift\\_JIS\"" )
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Header;
    QTest::newRow( "header-unquoted-apostrophe" )
        << QByteArray()
        << QByteArrayLiteral( "text/html; note=O'Reilly; charset=Shift_JIS" )
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Header;
    QTest::newRow( "header-actual-parameter-name" )
        << QByteArray()
        << QByteArrayLiteral( "text/html; x-charset=BIG5; charset=UTF-8" )
        << QStringLiteral( "UTF-8" ) << Encoding::DetectedBy::Header;

    QTest::newRow( "meta-first-recognised" )
        << QByteArrayLiteral( "<meta charset=windows-1251><META charset = 'Shift_JIS'>" )
        << QByteArray()
        << QStringLiteral( "WINDOWS-1251" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-unknown-then-known" )
        << QByteArrayLiteral( "<meta charset=unknown-codepage><meta charset=''>"
                              "<meta charset=GB2312><meta charset=EUCJP>" )
        << QByteArray()
        << QStringLiteral( "GB2312" ) << Encoding::DetectedBy::Meta;

    QTest::newRow( "meta-single-quoted-greater-than" )
        << QByteArrayLiteral( "<meta data-note='a>b' charset='Shift_JIS'>" )
        << QByteArray()
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-double-quoted-greater-than" )
        << QByteArrayLiteral( "<meta data-note=\"a>b\" charset=Shift_JIS>" )
        << QByteArray()
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-inside-comment" )
        << QByteArrayLiteral( "<!-- <meta charset=BIG5> --><meta charset=Shift_JIS>" )
        << QByteArray()
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-inside-double-quoted-attribute" )
        << QByteArrayLiteral( "<div data-note=\"<meta charset=BIG5>\"></div><meta charset=Shift_JIS>" )
        << QByteArray()
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-inside-single-quoted-attribute" )
        << QByteArrayLiteral( "<div data-note='<meta charset=\"BIG5\">'></div><meta charset=Shift_JIS>" )
        << QByteArray()
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-unquoted-attribute-separators" )
        << QByteArrayLiteral( "<meta data-note=x;,/charset=BIG5 charset=UTF-8>" )
        << QByteArray()
        << QStringLiteral( "UTF-8" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-attribute-name-boundary" )
        << QByteArrayLiteral( "<meta data@charset=BIG5 charset=UTF-8>" )
        << QByteArray()
        << QStringLiteral( "UTF-8" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-unquoted-attribute-apostrophe" )
        << QByteArrayLiteral( "<div data-note=O'Reilly></div><meta charset=Shift_JIS>" )
        << QByteArray()
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-unterminated-comment" )
        << QByteArrayLiteral( "<!-- > <meta charset=BIG5>" )
        << QByteArray()
        << QStringLiteral( "ISO-8859-1" ) << Encoding::DetectedBy::Fallback;
    QTest::newRow( "meta-unterminated-quoted-tag" )
        << QByteArrayLiteral( "<meta charset=BIG5 data-note='a>b" )
        << QByteArray()
        << QStringLiteral( "ISO-8859-1" ) << Encoding::DetectedBy::Fallback;
    QTest::newRow( "meta-quoted-tag-at-scan-bound" )
        << QByteArrayLiteral( "<meta data-note='a>b' charset=Shift_JIS>" ).rightJustified( 1024, ' ' )
        << QByteArray()
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-quoted-tag-crosses-scan-bound" )
        << QByteArrayLiteral( "<meta data-note='a>b' charset=Shift_JIS>" ).rightJustified( 1025, ' ' )
        << QByteArray()
        << QStringLiteral( "ISO-8859-1" ) << Encoding::DetectedBy::Fallback;

    QTest::newRow( "http-equiv-first-recognised" )
        << QByteArrayLiteral( "<meta http-equiv=Content-Type content='text/html; charset=EUCJP'>"
                              "<META content='text/html; charset=windows-1251' http-equiv=Content-Type>" )
        << QByteArray()
        << QStringLiteral( "EUC-JP" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "http-equiv-unknown-then-known" )
        << QByteArrayLiteral( "<meta http-equiv=Content-Type content='text/html; charset=unknown-codepage'>"
                              "<meta content='text/html; charset=' http-equiv=Content-Type>"
                              "<meta content='text/html; charset=Shift_JIS' http-equiv=Content-Type>"
                              "<meta http-equiv=Content-Type content='text/html; charset=UTF-8'>" )
        << QByteArray()
        << QStringLiteral( "SHIFT_JIS" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "http-equiv-content-parameters-first-recognised" )
        << QByteArrayLiteral( "<meta http-equiv=Content-Type content='text/html; charset=GB2312; charset=EUCJP'>" )
        << QByteArray()
        << QStringLiteral( "GB2312" ) << Encoding::DetectedBy::Meta;

    QTest::newRow( "xml-first-recognised" )
        << QByteArrayLiteral( "<?xml encoding='EUCJP'?><?xml encoding='Shift_JIS'?>" )
        << QByteArray()
        << QStringLiteral( "EUC-JP" ) << Encoding::DetectedBy::XmlDeclaration;
    QTest::newRow( "xml-unknown-then-known" )
        << QByteArrayLiteral( "<?xml encoding='unknown-codepage'?><?xml encoding=''?>"
                              "<?xml encoding='GB2312'?><?xml encoding='UTF-8'?>" )
        << QByteArray()
        << QStringLiteral( "GB2312" ) << Encoding::DetectedBy::XmlDeclaration;
}

void TstEncoding::htmlDetectionSameLevelSourceOrder()
{
    QFETCH( QByteArray, body );
    QFETCH( QByteArray, header );
    QFETCH( QString, expectedName );
    QFETCH( Encoding::DetectedBy, expectedSource );
    Encoding::DetectedBy detected = Encoding::DetectedBy::Fallback;
    const Encoding result = Encoding::forHtml( body, header, QStringLiteral( "ISO-8859-1" ), &detected );
    QCOMPARE( result.name(), expectedName );
    QCOMPARE( detected, expectedSource );
}

void TstEncoding::recoveryWarnsOncePerOperation()
{
    QLoggingCategory * const category = QLoggingCategory::defaultCategory();
    const bool wasEnabled = category->isWarningEnabled();
    category->setEnabled( QtWarningMsg, true );
    const auto restoreWarnings = qScopeGuard( [=]() { category->setEnabled( QtWarningMsg, wasEnabled ); } );
    QTest::failOnWarning();

    QTest::ignoreMessage( QtWarningMsg, "Invalid Encoding value; decoding through UTF-8" );
    QCOMPARE( Encoding().decode( QByteArray::fromHex( "efbbbf41ffff42" ) ),
              QStringLiteral( "A\ufffd\ufffdB" ) );

    QTest::ignoreMessage( QtWarningMsg, "Encoding decode recovered invalid input in UTF-8" );
    QCOMPARE( Encoding::utf8().decode( QByteArray::fromHex( "ffff" ) ),
              QStringLiteral( "\ufffd\ufffd" ) );

    QTest::ignoreMessage( QtWarningMsg, "Ignoring unknown HTML charset candidate: unknown-header" );
    QCOMPARE( Encoding::forHtml( QByteArrayLiteral( "<meta charset=unknown-meta>" ),
                                QByteArrayLiteral( "text/html; charset=unknown-header; charset=" ),
                                QStringLiteral( "unknown-fallback" ) ).name(),
              QStringLiteral( "UTF-8" ) );
}

void TstEncoding::localeValueIsDecodeOnly()
{
    const Encoding locale = Encoding::forLocale();
    QVERIFY( locale.isValid() );
    QCOMPARE( locale.decode( QByteArrayLiteral( "locale" ) ), QStringLiteral( "locale" ) );
    for ( QString const & text : { QStringLiteral( "locale" ), QString() } )
    {
        try
        {
            locale.encode( text );
            QFAIL( "encoding with a decode-only locale value must throw" );
        }
        catch ( Encoding::exUnrepresentable const & error )
        {
            const QString message = QString::fromUtf8( error.what() );
            QVERIFY2( message.contains( QStringLiteral( "decode-only" ), Qt::CaseInsensitive ),
                      qPrintable( message ) );
            QVERIFY2( message.contains( locale.name() ), qPrintable( message ) );
            QVERIFY2( message.contains( QStringLiteral( "scalar index 0" ) ), qPrintable( message ) );
        }
    }

    const Encoding invalid;
    try
    {
        invalid.encode( QStringLiteral( "locale" ) );
        QFAIL( "encoding with an invalid value must throw" );
    }
    catch ( Encoding::exUnrepresentable const & error )
    {
        const QString message = QString::fromUtf8( error.what() );
        QVERIFY2( message.contains( QStringLiteral( "<invalid>" ) ), qPrintable( message ) );
        QVERIFY2( !message.contains( QStringLiteral( "decode-only" ), Qt::CaseInsensitive ),
                  qPrintable( message ) );
    }
}

void TstEncoding::localeTransitionApiIsRetired()
{
    for ( const QString & name : { QStringLiteral( "gddebug.hh" ), QStringLiteral( "gddebug.cc" ) } )
    {
        QFile source( TestSupport::repositoryRoot() + '/' + name );
        QVERIFY( source.open( QIODevice::ReadOnly ) );
        const QByteArray text = source.readAll();
        for ( const QByteArray & retired : { QByteArray( "GdLocaleDecodeOnlyCodec" ),
                                            QByteArray( "QText" ) + "Codec", QByteArray( "gdCodecForLocale" ) } )
            QVERIFY2( !text.contains( retired ), qPrintable( name + ": " + QString::fromLatin1( retired ) ) );
    }
}

void TstEncoding::ownedParserPatternsAreValid()
{
    const auto & patterns = Slob::detail::articlePatterns();
    const QRegularExpression * slobPatterns[] = { &patterns.resourceSource, &patterns.resourceLink,
        &patterns.link, &patterns.pathPrefix, &patterns.htmlSuffix, &patterns.texImage,
        &patterns.texFraction, &patterns.texSpaces, &patterns.texMultiplier };
    for ( const auto * pattern : slobPatterns )
        QVERIFY2( pattern->isValid(), qPrintable( pattern->pattern() + ": " + pattern->errorString() ) );
    QVERIFY( patterns.resourceSource.match( QStringLiteral( "<img src=\"foo.png\">" ) ).hasMatch() );
    QCOMPARE( patterns.link.match( QStringLiteral( "<a href=\"dir/title.html\">" ) ).captured( 3 ),
              QStringLiteral( "dir/title.html" ) );
    QVERIFY( patterns.texFraction.match( QStringLiteral( "\\dfrac" ) ).hasMatch() );
    QCOMPARE( patterns.texMultiplier.match( QStringLiteral( "*{3}{cr}" ) ).captured( 3 ), QStringLiteral( "cr" ) );
    QVERIFY( Epwing::Book::detail::badLinksPattern().isValid() );
    QVERIFY( Epwing::Book::detail::badLinksPattern().match( QStringLiteral( "#v1" ) ).hasMatch() );
    QVERIFY( Epwing::Book::detail::pronunciationPattern().isValid() );
    QVERIFY( Epwing::Book::detail::pronunciationPattern().match( QStringLiteral( "word/pron/" ) ).hasMatch() );
    QVERIFY( Epwing::Book::detail::referenceOpenPattern().isValid() );
    QVERIFY( Epwing::Book::detail::referenceOpenPattern().match( QStringLiteral( "<R1>" ) ).hasMatch() );
    QVERIFY( Epwing::Book::detail::referenceClosePattern().isValid() );
    QVERIFY( Epwing::Book::detail::referenceClosePattern().match( QStringLiteral( "</R1>" ) ).hasMatch() );
}

void TstEncoding::invalidOwnedParserPatternFailsOperation_data()
{
    QTest::addColumn< bool >( "slob" );
    QTest::newRow( "slob" ) << true;
    QTest::newRow( "epwing" ) << false;
}

void TstEncoding::invalidOwnedParserPatternFailsOperation()
{
    QFETCH( bool, slob );
    QTest::failOnWarning();
    const QRegularExpression invalid( QStringLiteral( "(" ) );
    const QByteArray diagnostic = ( slob ? QByteArray( "Slob" ) : QByteArray( "Epwing" ) )
        + ": invalid pattern '(': " + invalid.errorString().toUtf8();
    QTest::ignoreMessage( QtWarningMsg, diagnostic.constData() );
    bool failed = false;
    try
    {
        if ( slob ) Slob::detail::parserPattern( QStringLiteral( "(" ) );
        else Epwing::Book::detail::parserPattern( QStringLiteral( "(" ) );
    }
    catch ( Slob::exCantDecodeFile const & error )
    {
        QVERIFY( slob );
        QVERIFY( QString::fromUtf8( error.what() ).contains( invalid.errorString() ) );
        failed = true;
    }
    catch ( Epwing::exEpwing const & error )
    {
        QVERIFY( !slob );
        QVERIFY( QString::fromUtf8( error.what() ).contains( invalid.errorString() ) );
        failed = true;
    }
    QVERIFY2( failed, "an invalid built-in must fail its operation in release builds" );
}

void TstEncoding::slobArticleFormattingPreservesNul()
{
    const auto codec = Encoding::byName( QStringLiteral( "WINDOWS-1251" ) );
    QVERIFY( codec );
    const QByteArray decoded = EncodingCallers::slobTextToUtf8( *codec, QByteArray::fromHex( "cf00f0" ) );
    const std::string input( decoded.constData(), decoded.size() );
    const Slob::RefEntry entry{};
    const auto output = Slob::detail::convertArticle( input, entry, "dict", QString(), QString() );
    QCOMPARE( QByteArray( output.data(), output.size() ), decoded + "<br style=\"clear:both;\" />" );
    const std::string markup = input + "<a href=\"dir/A_B.html\">link</a><img src=\"icon.png\">";
    const auto rewritten = Slob::detail::convertArticle( markup, entry, "dict", QString(), QString() );
    QCOMPARE( QByteArray( rewritten.data(), rewritten.size() ), decoded
        + "<a href=\"gdlookup://localhost/A%20B\" >link</a><img src=\"bres://dict/icon.png\"><br style=\"clear:both;\" />" );
}

void TstEncoding::websiteUnavailablePlaceholderCodecIsDiagnosed()
{
    QByteArray result( "%25GDUNKNOWN%25|%25GDWORD%25" );
    QTest::failOnWarning();
    QTest::ignoreMessage( QtWarningMsg,
        "Encoding: URL placeholder %25GDUNKNOWN%25 not representable in x-final-unknown" );
    WebSite::detail::replaceUrlPlaceholder( result, QString::fromUtf8( "中" ), "%25GDUNKNOWN%25",
                                           QStringLiteral( "x-final-unknown" ) );
    QCOMPARE( WebSite::detail::substituteUrlPlaceholders( result, QString::fromUtf8( "中" ) ),
              QByteArray( "%25GDUNKNOWN%25|%E4%B8%AD" ) );
}

void TstEncoding::websiteTemplateLeavesFailedEncodingPlaceholder()
{
    const QString word = QString::fromUtf8( "\xe4\xb8\xad" );
    const QByteArray templ = QByteArrayLiteral( "%25GD1251%25|%25GDBIG5%25|%25GDWORD%25" );

    QCOMPARE( WebSite::detail::substituteUrlPlaceholders( templ, word ),
              QByteArrayLiteral( "%25GD1251%25|%A4%A4|%E4%B8%AD" ) );
}

void TstEncoding::websiteTemplateWarnsForEveryFailedEncoding()
{
    QTest::failOnWarning();
    QTest::ignoreMessage( QtWarningMsg,
        "Encoding: URL placeholder %25GD1251%25 not representable in Windows-1251" );
    QTest::ignoreMessage( QtWarningMsg,
        "Encoding: URL placeholder %25GDISO1%25 not representable in ISO-8859-1" );
    QCOMPARE( WebSite::detail::substituteUrlPlaceholders(
                  "%25GD1251%25|%25GD1251%25|%25GDISO1%25|%25GDBIG5%25|%25GDWORD%25",
                  QString::fromUtf8( "中" ) ),
              QByteArray( "%25GD1251%25|%25GD1251%25|%25GDISO1%25|%A4%A4|%E4%B8%AD" ) );
}

void TstEncoding::epwingSkippedCharsetIsDiagnosed()
{
    QTest::failOnWarning();
    const auto euc = Encoding::byName( QStringLiteral( "EUC-JP" ) );
    const auto gb = Encoding::byName( QStringLiteral( "GB2312" ) );
    QVERIFY( euc && gb );
    QTest::ignoreMessage( QtWarningMsg, "Encoding: EPWING search skipped charset=EUC-JP" );
    QCOMPARE( EncodingCallers::epwingSearchBytes( *euc, QString::fromUtf8( "汉" ) ), QByteArray() );
    QCOMPARE( EncodingCallers::epwingSearchBytes( *gb, QString::fromUtf8( "汉" ) ).toHex(), QByteArray( "baba" ) );
    QTest::ignoreMessage( QtWarningMsg, "Encoding: EPWING search skipped charset=EUC-JP" );
    QCOMPARE( EncodingCallers::epwingSearchBytes( *euc, QString::fromUtf8( "😀" ) ), QByteArray() );
    QTest::ignoreMessage( QtWarningMsg, "Encoding: EPWING search skipped charset=GB2312" );
    QCOMPARE( EncodingCallers::epwingSearchBytes( *gb, QString::fromUtf8( "😀" ) ), QByteArray() );
}

void TstEncoding::slobHeaderCharsetFallback_data()
{
    QTest::addColumn< QByteArray >( "charset" );
    QTest::addColumn< QByteArray >( "label" );
    QTest::addColumn< QString >( "expectedName" );
    QTest::addColumn< QString >( "expectedLabel" );
    QTest::newRow( "utf8" ) << QByteArray( "UTF-8" ) << QByteArray::fromHex( "d09fd180" )
        << QStringLiteral( "UTF-8" ) << QString::fromUtf8( "Пр" );
    QTest::newRow( "cp1251" ) << QByteArray( "WINDOWS-1251" ) << QByteArray::fromHex( "cff0" )
        << QStringLiteral( "WINDOWS-1251" ) << QString::fromUtf8( "Пр" );
    QTest::newRow( "unknown" ) << QByteArray( "x-final-unknown" ) << QByteArray::fromHex( "d09fd180" )
        << QStringLiteral( "UTF-8" ) << QString::fromUtf8( "Пр" );
    QTest::newRow( "unknown-malformed" ) << QByteArray( "x-final-unknown" ) << QByteArray::fromHex( "41ff42" )
        << QStringLiteral( "UTF-8" ) << QStringLiteral( "A\ufffdB" );
}

void TstEncoding::slobHeaderCharsetFallback()
{
    QFETCH( QByteArray, charset );
    QFETCH( QByteArray, label );
    QFETCH( QString, expectedName );
    QFETCH( QString, expectedLabel );
    QTemporaryDir directory;
    QVERIFY( directory.isValid() );
    QByteArray bytes = QByteArray::fromHex( "212d31534c4f421f" ) + QByteArray( 16, '\0' );
    const auto tiny = [&]( QByteArray const & value ) { bytes += char( value.size() ); bytes += value; };
    const auto big32 = [&]( quint32 value ) {
        value = qToBigEndian( value );
        bytes.append( reinterpret_cast< char const * >( &value ), sizeof( value ) );
    };
    const auto big64 = [&]( quint64 value ) {
        value = qToBigEndian( value );
        bytes.append( reinterpret_cast< char const * >( &value ), sizeof( value ) );
    };
    tiny( charset ); tiny( "none" );
    bytes += char( 1 ); tiny( "label" ); tiny( label );
    bytes += char( 0 ); // no content types or blobs
    big32( 0 );
    const quint64 storeOffset = bytes.size() + 8 + 8 + 4;
    big64( storeOffset ); big64( storeOffset + 4 ); big32( 0 ); big32( 0 );
    const QString path = directory.filePath( QStringLiteral( "header.slob" ) );
    QFile output( path );
    QVERIFY( output.open( QIODevice::WriteOnly ) );
    QCOMPARE( output.write( bytes ), bytes.size() );
    output.close();
    QTest::failOnWarning();
    if ( charset == "x-final-unknown" )
        QTest::ignoreMessage( QtWarningMsg, "Encoding: unknown charset x-final-unknown; using UTF-8" );
    if ( label.contains( char( 0xff ) ) )
        QTest::ignoreMessage( QtWarningMsg, "Encoding decode recovered invalid input in UTF-8" );
    Slob::SlobFile reader;
    try { reader.open( path ); }
    catch ( std::exception const & error ) { QFAIL( error.what() ); }
    QCOMPARE( reader.getCodec().name(), expectedName );
    QCOMPARE( reader.getDictionaryName(), expectedLabel );
}

void TstEncoding::epwingRetryDoesNotFallBackToUtf8()
{
    const auto euc = Encoding::byName( QStringLiteral( "EUC-JP" ) );
    const auto gb = Encoding::byName( QStringLiteral( "GB2312" ) );
    QVERIFY( euc );
    QVERIFY( gb );

    // Traditional Han has no GB2312 representation; simplified Han requires
    // the second (GB2312) attempt. Neither failure may become UTF-8 bytes.
    const QString traditional = QString::fromUtf8( "\xe6\xbc\xa2" );
    QCOMPARE( EncodingCallers::epwingSearchBytes( *euc, traditional ).toHex(), QByteArrayLiteral( "b4c1" ) );
    QCOMPARE( EncodingCallers::epwingSearchBytes( *gb, traditional ), QByteArray() );
    const QString han = QString::fromUtf8( "\xe6\xb1\x89" );
    QCOMPARE( EncodingCallers::epwingSearchBytes( *euc, han ), QByteArray() );
    QCOMPARE( EncodingCallers::epwingSearchBytes( *gb, han ).toHex(), QByteArrayLiteral( "baba" ) );
    const QString emoji = QString::fromUtf8( "\xf0\x9f\x98\x80" );
    QCOMPARE( EncodingCallers::epwingSearchBytes( *euc, emoji ), QByteArray() );
    QCOMPARE( EncodingCallers::epwingSearchBytes( *gb, emoji ), QByteArray() );
}

void TstEncoding::slobLegacyDecodePreservesBytes()
{
    const auto windows1251 = Encoding::byName( QStringLiteral( "WINDOWS-1251" ) );
    QVERIFY( windows1251 );
    const QByteArray raw = QByteArray::fromHex( "cff0e8e2e5f2" );
    const QByteArray snapshot = raw;

    QCOMPARE( EncodingCallers::slobTextToUtf8( *windows1251, raw ).toHex(),
              QByteArrayLiteral( "d09fd180d0b8d0b2d0b5d182" ) );
    QCOMPARE( EncodingCallers::slobTextToUtf8( *windows1251, QByteArray::fromHex( "cf00f0" ) ).toHex(),
              QByteArrayLiteral( "d09f00d180" ) );
    QCOMPARE( raw, snapshot );
    QCOMPARE( raw.toHex(), QByteArrayLiteral( "cff0e8e2e5f2" ) );
}

void TstEncoding::zipRawFilenameIsNotDisplayText()
{
    const QByteArray raw = QByteArray::fromHex( "cff0e8e2e5f22e747874" );
    const auto codec = Encoding::byName( QStringLiteral( "WINDOWS-1251" ) );
    QVERIFY( codec );
    QCOMPARE( EncodingCallers::zipFilenameForDisplay( *codec, raw ).toUtf8().toHex(),
              QByteArrayLiteral( "d09fd180d0b8d0b2d0b5d1822e747874" ) );
    QCOMPARE( raw.toHex(), QByteArrayLiteral( "cff0e8e2e5f22e747874" ) );
}

void TstEncoding::loggingEmitsUtf8WithoutLocaleMutation()
{
    const QByteArray processLocale = std::setlocale( LC_ALL, nullptr );
    const QString encodingName = Encoding::forLocale().name();
    QTest::ignoreMessage( QtWarningMsg, "UTF-8: \xe4\xb8\xad" );
    const QByteArray message = QString::fromUtf8( "\xe4\xb8\xad" ).toUtf8();
    gdWarning( "UTF-8: %s", message.constData() );
    QCOMPARE( QByteArray( std::setlocale( LC_ALL, nullptr ) ), processLocale );
    QCOMPARE( Encoding::forLocale().name(), encodingName );
}

void TstEncoding::immutableValueConvertsInParallel()
{
    const Encoding encoding = Encoding::utf8();
    constexpr int threadCount = 8;
    std::vector< std::future< QString > > futures;
    futures.reserve( threadCount );

    for ( int i = 0; i < threadCount; ++i )
    {
        futures.emplace_back( std::async( std::launch::async, [ encoding ]() {
            const QByteArray input = QByteArrayLiteral( "parallel ") + QByteArray::fromHex( "f09f9880" );
            return encoding.decode( input );
        } ) );
    }

    for ( auto & future : futures )
        QCOMPARE( future.get(), QString::fromUtf8( "parallel \xf0\x9f\x98\x80" ) );
}

int main( int argc, char *argv[] )
{
    if ( TestSupport::isWatchdogProbeProcess() )
        TestSupport::runWatchdogProbe();

    QCoreApplication app( argc, argv );
    TstEncoding testCase;
    return QTest::qExec( &testCase, argc, argv );
}

#include "tst_encoding.moc"
