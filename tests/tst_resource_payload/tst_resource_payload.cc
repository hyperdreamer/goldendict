#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QColor>
#include <vector>
#include "config.hh"
#include "dictionary.hh"
#include "file.hh"
#include "fsencoding.hh"
#include "payload.hh"
#include "imagepayload.hh"
#include "bglresource.hh"
#include "fixtures.hh"

namespace Config {
bool isPortableVersion() throw() { return false; }
QString getPortableVersionDictionaryDir() throw() { return QString(); }
}

class ResourcePayloadDictionary: public Dictionary::Class
{
public:
  ResourcePayloadDictionary():
    Dictionary::Class( "resource-payload-test", std::vector< std::string >() ) {}
  std::string getName() throw() override { return "resource-payload-test"; }
  std::map< Dictionary::Property, std::string > getProperties() throw() override
  { return {}; }
  unsigned long getArticleCount() throw() override { return 0; }
  unsigned long getWordCount() throw() override { return 0; }
  sptr< Dictionary::WordSearchRequest > prefixMatch(
    gd::wstring const &, unsigned long ) THROW_SPEC( std::exception ) override
  { return new Dictionary::WordSearchRequestInstant; }
  sptr< Dictionary::DataRequest > getArticle(
    gd::wstring const &, std::vector< gd::wstring > const &,
    gd::wstring const &, bool ) THROW_SPEC( std::exception ) override
  { return new Dictionary::DataRequestInstant( true ); }
};

class ResourcePayloadTests: public QObject
{
  Q_OBJECT
private slots:
  void payloadAssign_emptyLeavesVectorEmpty()
  {
    std::vector< char > data = { 'o', 'l', 'd' };
    Payload::assign( data, nullptr, 0 );
    QVERIFY( data.empty() );
  }
  void payloadAssign_copiesExactBytes()
  {
    char const bytes[] = { char(0x41), char(0x00), char(0xff) };
    std::vector< char > data;
    Payload::assign( data, bytes, sizeof(bytes) );
    QCOMPARE( data.size(), std::size_t(3) );
    QVERIFY( data == std::vector< char >( bytes, bytes + sizeof(bytes) ) );
  }
  void payloadAssign_replacesLongerContent()
  {
    std::vector< char > data = { 'o', 'l', 'd' };
    char const bytes[] = { char(0x78), char(0x79) };
    Payload::assign( data, bytes, sizeof(bytes) );
    QCOMPARE( data.size(), std::size_t(2) );
    QVERIFY( data == std::vector< char >( bytes, bytes + sizeof(bytes) ) );
  }
  void payloadAppend_emptyIsNoOp()
  {
    std::vector< char > data;
    Payload::append( data, nullptr, 0 );
    QVERIFY( data.empty() );
  }
  void payloadAppend_appendsExactly()
  {
    std::vector< char > data = { char(0x61) };
    char const bytes[] = { char(0x62), char(0x63) };
    Payload::append( data, bytes, sizeof(bytes) );
    QCOMPARE( data.size(), std::size_t(3) );
    QCOMPARE( QByteArray(data.data(), int(data.size())), QByteArray::fromHex("616263") );
  }
  void loadFromFile_zeroByteFileIsEmpty()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    QFile file( dir.filePath("empty") );
    QVERIFY( file.open(QIODevice::WriteOnly) );
    file.close();
    std::vector< char > data;
    File::loadFromFile( FsEncoding::encode(file.fileName()), data );
    QVERIFY( data.empty() );
  }
  void loadFromFile_readsExactBytes()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    QFile file( dir.filePath("bytes") );
    QByteArray const bytes = QByteArray::fromHex("00ff3100");
    QVERIFY( file.open(QIODevice::WriteOnly) );
    QCOMPARE( file.write(bytes), qint64(bytes.size()) );
    file.close();
    std::vector< char > data;
    File::loadFromFile( FsEncoding::encode(file.fileName()), data );
    QCOMPARE( data.size(), std::size_t(4) );
    QCOMPARE( QByteArray(data.data(), int(data.size())), bytes );
  }
  void setIsolatedCssPayload_commentOnlyYieldsEmpty()
  {
    ResourcePayloadDictionary dict;
    QString css = "/* only */";
    std::vector< char > data = { char(0x6f), char(0x6c), char(0x64) };
    dict.setIsolatedCssPayload( css, data );
    QVERIFY( css.isEmpty() );
    QVERIFY( data.empty() );
  }
  void setIsolatedCssPayload_pageOnlyYieldsEmpty()
  {
    ResourcePayloadDictionary dict;
    QString css = "@page { size: A4; }";
    std::vector< char > data = { char(0x6f), char(0x6c), char(0x64) };
    dict.setIsolatedCssPayload( css, data );
    QVERIFY( css.isEmpty() );
    QVERIFY( data.empty() );
  }
  void setIsolatedCssPayload_prefixesOrdinaryRules()
  {
    ResourcePayloadDictionary dict;
    QString css = ".entry { color: red; }";
    std::vector< char > data;
    dict.setIsolatedCssPayload( css, data );
    QString const expected = "#gdfrom-resource-payload-test .entry { color: red; }";
    QCOMPARE( css, expected );
    QCOMPARE( data.size(), std::size_t(expected.toUtf8().size()) );
    QCOMPARE( QByteArray(data.data(), int(data.size())), expected.toUtf8() );
  }
  void tiffToBmp_emptyInputYieldsEmpty()
  {
    QVERIFY( ImagePayload::decodeWithQt(nullptr, 0).isNull() );
    QVERIFY( ImagePayload::decodeWithGdTiff(nullptr, 0).isNull() );
    QVERIFY( ImagePayload::tiffToBmp(nullptr, 0).isEmpty() );
  }
  void tiffToBmp_undecodableInputYieldsEmpty()
  {
    QByteArray const bytes = QByteArray::fromHex("0001");
    QVERIFY( ImagePayload::decodeWithQt(bytes.constData(), bytes.size()).isNull() );
    QVERIFY( ImagePayload::tiffToBmp(bytes.constData(), bytes.size()).isEmpty() );
  }
  void tiffToBmp_producesBmpFromValidTiff()
  {
    QFile file( TestSupport::repositoryRoot()
                + "/tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff" );
    QVERIFY( file.open(QIODevice::ReadOnly) );
    QByteArray const bytes = file.readAll();
    QCOMPARE( bytes.size(), qsizetype(124) );
    QVERIFY( ImagePayload::decodeWithQt(bytes.constData(), bytes.size()).isNull() );
    QByteArray const bmp = ImagePayload::tiffToBmp(bytes.constData(), bytes.size());
#ifdef MAKE_EXTRA_TIFF_HANDLER
    QVERIFY( !bmp.isEmpty() );
    QImage image = QImage::fromData(
      reinterpret_cast<uchar const *>(bmp.constData()), bmp.size(), "BMP" );
    QVERIFY( !image.isNull() );
    QCOMPARE( image.width(), 2 );
    QCOMPARE( image.height(), 2 );
    QCOMPARE( image.pixelColor(0, 0), QColor(Qt::white) );
    QCOMPARE( image.pixelColor(1, 0), QColor(Qt::black) );
    QCOMPARE( image.pixelColor(0, 1), QColor(Qt::black) );
    QCOMPARE( image.pixelColor(1, 1), QColor(Qt::white) );
#else
    QVERIFY( bmp.isEmpty() );
#endif
  }
  void gdTiffDecodesCommittedOneBitFixture()
  {
    QFile file( TestSupport::repositoryRoot()
                + "/tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff" );
    QVERIFY( file.open(QIODevice::ReadOnly) );
    QByteArray const bytes = file.readAll();
    QCOMPARE( bytes.size(), qsizetype(124) );
    QImage image = ImagePayload::decodeWithGdTiff(bytes.constData(), bytes.size());
#ifdef MAKE_EXTRA_TIFF_HANDLER
    QVERIFY( !image.isNull() );
    QCOMPARE( image.width(), 2 );
    QCOMPARE( image.height(), 2 );
    QCOMPARE( image.pixelColor(0, 0), QColor(Qt::white) );
    QCOMPARE( image.pixelColor(1, 0), QColor(Qt::black) );
    QCOMPARE( image.pixelColor(0, 1), QColor(Qt::black) );
    QCOMPARE( image.pixelColor(1, 1), QColor(Qt::white) );
#else
    QVERIFY( image.isNull() );
#endif
  }
  void bglFindResource_emptyNameSkippedAfterOffset()
  {
    QByteArray const bytes = QByteArray::fromHex(
      "000000000100000009000000696d6167652e74696688776655" );
    QCOMPARE( bytes.size(), qsizetype(25) );
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    QFile file( dir.filePath("directory") );
    QVERIFY( file.open(QIODevice::WriteOnly) );
    QCOMPARE( file.write(bytes), qint64(bytes.size()) );
    file.close();
    File::Class idx( FsEncoding::encode(file.fileName()), "rb" );
    uint32_t offset = 0;
    idx.seek(0);
    QVERIFY( !BglResource::findResource(idx, 2, std::string(), offset) );
    QCOMPARE( offset, uint32_t(0) );
    idx.seek(0);
    QVERIFY( BglResource::findResource(idx, 2, "image.tif", offset) );
    QCOMPARE( offset, uint32_t(0x55667788) );
  }
  void bglDecompress_validEmptyStreamSucceedsEmpty()
  {
    QByteArray const bytes = QByteArray::fromHex("789c030000000001");
    std::vector< char > compressed(bytes.constData(), bytes.constData() + bytes.size());
    std::vector< char > data;
    QVERIFY( BglResource::decompress(compressed, 0, data) );
    QVERIFY( data.empty() );
  }
  void bglDecompress_corruptStreamFails()
  {
    QByteArray const bytes = QByteArray::fromHex("789c030000000000");
    std::vector< char > compressed(bytes.constData(), bytes.constData() + bytes.size());
    std::vector< char > data;
    QVERIFY( !BglResource::decompress(compressed, 1, data) );
  }
  void bglDecompress_exactNonEmptyRoundTrip()
  {
    QByteArray const bytes = QByteArray::fromHex("789c4b4c4a0600024d0127");
    std::vector< char > compressed(bytes.constData(), bytes.constData() + bytes.size());
    std::vector< char > data;
    QVERIFY( BglResource::decompress(compressed, 3, data) );
    QCOMPARE( data.size(), std::size_t(3) );
    QCOMPARE( QByteArray(data.data(), int(data.size())), QByteArray::fromHex("616263") );
  }
  void bglDecompress_advertisedLengthMismatchFails()
  {
    QByteArray const bytes = QByteArray::fromHex("789c4b4c4a0600024d0127");
    std::vector< char > compressed(bytes.constData(), bytes.constData() + bytes.size());
    std::vector< char > data;
    QVERIFY( !BglResource::decompress(compressed, 2, data) );
    QVERIFY( !BglResource::decompress(compressed, 4, data) );
  }
  void bglDecompress_emptyCompressedInputFails()
  {
    std::vector< char > const compressed;
    std::vector< char > data;
    QVERIFY( !BglResource::decompress(compressed, 1, data) );
    QVERIFY( !BglResource::decompress(compressed, 0, data) );
  }
};

QTEST_GUILESS_MAIN( ResourcePayloadTests )
#include "tst_resource_payload.moc"
