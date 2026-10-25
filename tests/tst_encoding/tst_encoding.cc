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
    void fixedVectors();
    void aliasesAndUnknownNames();
    void recoveryProducesOneReplacementPerStep();
    void endianBomAndEmbeddedNul();
    void statefulFlushAndOutputGrowth();
    void encoderRejectsUnrepresentableText();
    void htmlDetectionPrecedenceAndBounds();
    void localeValueIsDecodeOnly();
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

void TstEncoding::fixedVectors()
{
    const QJsonArray vectors = loadVectors();
    QCOMPARE( vectors.size(), 28 );

    for ( QJsonValue const & value : vectors )
    {
        const QJsonObject vector = value.toObject();
        const QString name = jsonString( vector, QStringLiteral( "encoding" ) );
        const std::unique_ptr< Encoding > encoding = Encoding::byName( name );
        QVERIFY2( encoding, qPrintable( name ) );

        const QByteArray bytes = QByteArray::fromHex( jsonString( vector, QStringLiteral( "bytes" ) ).toLatin1() );
        const QString expected = jsonString( vector, QStringLiteral( "text" ) );
        QCOMPARE( encoding->decode( bytes ), expected );

        const QByteArray encoded = encoding->encode( expected );
        QCOMPARE( encoded.toHex(), jsonString( vector, QStringLiteral( "encoded" ) ).toLatin1() );
    }
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

void TstEncoding::recoveryProducesOneReplacementPerStep()
{
    QCOMPARE( Encoding::utf8().decode( QByteArray::fromHex( "ffff" ) ), QString( 2, QChar( 0xfffd ) ) );
    QCOMPARE( Encoding::utf8().decode( QByteArray::fromHex( "e282" ) ), QString( QChar( 0xfffd ) ) );

    const QByteArray badUtf8 = QByteArray::fromHex( "41ffff42" );
    QCOMPARE( Encoding::utf8().decode( badUtf8 ), QStringLiteral( "A\ufffd\ufffdB" ) );

    const Encoding invalid;
    QCOMPARE( invalid.decode( QByteArray::fromHex( "e282ac" ) ), QStringLiteral( "\u20ac" ) );
    QCOMPARE( invalid.decode( QByteArray::fromHex( "ffff" ) ), QString( 2, QChar( 0xfffd ) ) );
}

void TstEncoding::endianBomAndEmbeddedNul()
{
    const Encoding utf8 = Encoding::utf8();
    QCOMPARE( utf8.decode( QByteArray::fromHex( "efbbbfe4b8ad" ) ), QStringLiteral( "\u4e2d" ) );

    const auto utf16le = Encoding::byName( QStringLiteral( "UTF-16LE" ) );
    const auto utf16be = Encoding::byName( QStringLiteral( "UTF-16BE" ) );
    const auto utf32le = Encoding::byName( QStringLiteral( "UTF-32LE" ) );
    const auto utf32be = Encoding::byName( QStringLiteral( "UTF-32BE" ) );
    QVERIFY( utf16le && utf16be && utf32le && utf32be );

    QCOMPARE( utf16le->decode( QByteArray::fromHex( "2d4e" ) ), QStringLiteral( "\u4e2d" ) );
    QCOMPARE( utf16be->decode( QByteArray::fromHex( "4e2d" ) ), QStringLiteral( "\u4e2d" ) );
    QCOMPARE( utf32le->decode( QByteArray::fromHex( "2d4e0000" ) ), QStringLiteral( "\u4e2d" ) );
    QCOMPARE( utf32be->decode( QByteArray::fromHex( "00004e2d" ) ), QStringLiteral( "\u4e2d" ) );
    QCOMPARE( utf16le->decode( QByteArray::fromHex( "fffe2d4e" ) ), QStringLiteral( "\u4e2d" ) );
    QCOMPARE( utf16be->decode( QByteArray::fromHex( "feff4e2d" ) ), QStringLiteral( "\u4e2d" ) );
    QCOMPARE( utf32le->decode( QByteArray::fromHex( "fffe00002d4e0000" ) ), QStringLiteral( "\u4e2d" ) );
    QCOMPARE( utf32be->decode( QByteArray::fromHex( "0000feff00004e2d" ) ), QStringLiteral( "\u4e2d" ) );

    const auto generic16 = Encoding::byName( QStringLiteral( "UTF-16" ) );
    const auto generic32 = Encoding::byName( QStringLiteral( "UTF-32" ) );
    QVERIFY( generic16 && generic32 );
    QCOMPARE( generic16->decode( QByteArray::fromHex( "fffe2d4e" ) ), QStringLiteral( "\u4e2d" ) );
    QCOMPARE( generic32->decode( QByteArray::fromHex( "0000feff00004e2d" ) ), QStringLiteral( "\u4e2d" ) );

    const QByteArray withNul( "a\0b", 3 );
    const QString embeddedNul = QStringLiteral( "a" ) + QChar( 0 ) + QStringLiteral( "b" );
    QCOMPARE( utf8.decode( withNul ), embeddedNul );
    QCOMPARE( utf8.encode( embeddedNul ), withNul );
}

void TstEncoding::statefulFlushAndOutputGrowth()
{
    const auto iso2022 = Encoding::byName( QStringLiteral( "ISO-2022-JP" ) );
    QVERIFY( iso2022 );
    const QString japanese = QStringLiteral( "\u65e5\u672c" );
    const QByteArray expected = QByteArray::fromHex( "1b2442467c4b5c1b2842" );
    QCOMPARE( iso2022->encode( japanese ), expected );
    QCOMPARE( iso2022->decode( expected ), japanese );

    QString large;
    large.reserve( 120000 );
    for ( int i = 0; i < 40000; ++i )
        large.append( QStringLiteral( "abc" ) );
    QCOMPARE( Encoding::utf8().decode( Encoding::utf8().encode( large ) ), large );
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

void TstEncoding::localeValueIsDecodeOnly()
{
    const Encoding locale = Encoding::forLocale();
    QVERIFY( locale.isValid() );
    QCOMPARE( locale.decode( QByteArrayLiteral( "locale" ) ), QStringLiteral( "locale" ) );
    QVERIFY_THROWS_EXCEPTION( Encoding::exUnrepresentable, locale.encode( QStringLiteral( "locale" ) ) );
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
