#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <future>
#include <thread>
#include <vector>

#include "encoding.hh"
#include "gddebug.hh"
#include "support/fixtures.hh"

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
    void localeValueIsDecodeOnly();
    void localeTransitionShimDecodes();
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

    QTest::newRow( "meta-first-recognised" )
        << QByteArrayLiteral( "<meta charset=windows-1251><META charset = 'Shift_JIS'>" )
        << QByteArray()
        << QStringLiteral( "WINDOWS-1251" ) << Encoding::DetectedBy::Meta;
    QTest::newRow( "meta-unknown-then-known" )
        << QByteArrayLiteral( "<meta charset=unknown-codepage><meta charset=''>"
                              "<meta charset=GB2312><meta charset=EUCJP>" )
        << QByteArray()
        << QStringLiteral( "GB2312" ) << Encoding::DetectedBy::Meta;

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

void TstEncoding::localeTransitionShimDecodes()
{
    // Keep the exact pointer spelling and overloads used by indexedzip.cc and
    // epwing_book.cc until their owning task migrates the callers.
    QTextCodec * const codec = gdCodecForLocale();
    QVERIFY( codec );
    const Encoding locale = Encoding::forLocale();
    const QByteArray input = QByteArray::fromHex( "4100ffff42" );
    QCOMPARE( codec->toUnicode( input.constData(), input.size() ), locale.decode( input ) );
    QCOMPARE( codec->toUnicode( input ), locale.decode( input ) );
    QCOMPARE( codec->toUnicode( "locale" ), locale.decode( QByteArrayLiteral( "locale" ) ) );
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
