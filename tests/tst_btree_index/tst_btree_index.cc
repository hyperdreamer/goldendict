// Btree index trust-boundary suite.
//
// Every slot builds a real index with the production builder, then either reads
// it back or mutates the on-disk bytes to model a corrupt, truncated or
// foreign index. The assertions are made through the public API only:
//   * BtreeIndexing::BtreeIndex::indexInfoIsSane()   -- the shared predicate
//   * BtreeIndex::openIndex()                        -- inert on failure
//   * BtreeIndex::findArticles()                     -- catching lookup
//   * BtreeIndex::findArticlesChecked()              -- propagating lookup
//
// The crash this suite exists for was an empty-vector front() inside readNode();
// under -D_GLIBCXX_ASSERTIONS (Arch's default) that aborted the whole process.
// Every slot below therefore asserts an exception or an inert index instead of
// process death, and the suite itself staying alive is part of the result.

#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QByteArray>

#include <zlib.h>

#include <cstring>

#include <string>
#include <vector>

#include "btreeidx.hh"
#include "config.hh"
#include "utf8.hh"
#include "chunkedstorage.hh"
#include "file.hh"
#include "mutex.hh"
#include "wstring.hh"

using gd::wstring;
using std::vector;
using BtreeIndexing::BtreeIndex;
using BtreeIndexing::IndexInfo;
using BtreeIndexing::IndexedWords;

// Test doubles. dictionary.cc consults the portable-layout configuration, which a
// pure suite never exercises; defining the two accessors here keeps the suite
// free of the whole configuration subsystem and its QDom dependency.
namespace Config {

bool isPortableVersion() throw() { return false; }

QString getPortableVersionDictionaryDir() throw() { return QString(); }

} // namespace Config

namespace {

constexpr quint32 kMaxElements = 8192;   // BtreeMaxElements, the fan-out ceiling

QByteArray zlibCompress( QByteArray const & payload )
{
  uLongf bound = compressBound( payload.size() );
  QByteArray out( int( bound ), Qt::Uninitialized );
  if ( compress2( reinterpret_cast< Bytef * >( out.data() ), &bound,
                  reinterpret_cast< Bytef const * >( payload.constData() ),
                  payload.size(), Z_BEST_COMPRESSION ) != Z_OK )
    return {};
  out.resize( int( bound ) );
  return out;
}

QByteArray zlibDecompress( char const * data, int size, int expected )
{
  QByteArray out( expected, Qt::Uninitialized );
  uLongf produced = uLongf( expected );
  if ( uncompress( reinterpret_cast< Bytef * >( out.data() ), &produced,
                   reinterpret_cast< Bytef const * >( data ), uLong( size ) ) != Z_OK )
    return {};
  if ( int( produced ) != expected )
    return {};
  return out;
}

void putU32( QByteArray & buf, int at, quint32 value )
{
  Q_ASSERT( at >= 0 && at + 4 <= buf.size() );
  buf[ at + 0 ] = char( value         & 0xff );
  buf[ at + 1 ] = char( ( value >> 8  ) & 0xff );
  buf[ at + 2 ] = char( ( value >> 16 ) & 0xff );
  buf[ at + 3 ] = char( ( value >> 24 ) & 0xff );
}

quint32 getU32( QByteArray const & buf, int at )
{
  return quint32( quint8( buf[ at ] ) ) |
         ( quint32( quint8( buf[ at + 1 ] ) ) << 8 ) |
         ( quint32( quint8( buf[ at + 2 ] ) ) << 16 ) |
         ( quint32( quint8( buf[ at + 3 ] ) ) << 24 );
}

/// A built index plus the exact bytes that are on disk, so a slot can mutate
/// them without going through the builder again.
struct Built
{
  QString path;
  IndexInfo info { 0, 0 };
  QByteArray bytes;
};

/// Builds an index holding `count` words named word00000.. into `path`.
/// Ownership of the temporary directory belongs to the caller.
Built buildIndex( QString const & path, int count )
{
  Built built;
  built.path = path;

  {
    File::Class file( path.toStdString().c_str(), "wb" );

    IndexedWords words;
    for ( int i = 0; i < count; ++i )
    {
      QString const digits = QString( "%1" ).arg( i, 5, 10, QLatin1Char( '0' ) );
      wstring const word = wstring( L"word" ) + digits.toStdWString();
      words.addSingleWord( word, uint32_t( i ) );
    }

    built.info = BtreeIndexing::buildIndex( words, file );
  }

  {
    File::Class file( path.toStdString().c_str(), "rb" );
    file.seekEnd();
    built.bytes.resize( int( file.tell() ) );
    file.rewind();
    if ( !built.bytes.isEmpty() )
      file.read( built.bytes.data(), built.bytes.size() );
  }

  return built;
}

void writeBytes( QString const & path, QByteArray const & bytes )
{
  File::Class file( path.toStdString().c_str(), "wb" );
  file.write( bytes.constData(), bytes.size() );
}

/// Writes a *record* (uncompressed size, compressed size, payload) plus an
/// optional trailing next-leaf link, and returns the record's offset.
int appendRecord( QByteArray & target, QByteArray const & payload, bool withLink, quint32 link )
{
  QByteArray const compressed = zlibCompress( payload );
  int const at = target.size();
  target.append( QByteArray( 8, '\0' ) );
  putU32( target, at, quint32( payload.size() ) );
  putU32( target, at + 4, quint32( compressed.size() ) );
  target.append( compressed );
  if ( withLink )
  {
    target.append( QByteArray( 4, '\0' ) );
    putU32( target, at + 8 + compressed.size(), link );
  }
  return at;
}

QByteArray leafPayload( QByteArray const & word, quint32 articleOffset )
{
  QByteArray payload;
  payload.append( QByteArray( 4, '\0' ) );        // entry count, patched below
  putU32( payload, 0, 1 );

  QByteArray chain;
  chain.append( word );
  chain.append( '\0' );                           // word terminator
  chain.append( '\0' );                           // empty prefix
  QByteArray offset( 4, '\0' );
  putU32( offset, 0, articleOffset );
  chain.append( offset );

  QByteArray framed( 4, '\0' );
  putU32( framed, 0, quint32( chain.size() ) );
  payload.append( framed );
  payload.append( chain );
  return payload;
}

/// A leaf record whose payload the caller supplies verbatim.
void expectInert( QString const & path, IndexInfo const & info, char const * what )
{
  File::Class file( path.toStdString().c_str(), "rb" );
  Mutex mutex;

  QVERIFY2( !BtreeIndex::indexInfoIsSane( info, file, mutex ), what );

  BtreeIndex index;
  index.openIndex( info, file, mutex );   // must return normally, not throw

  bool threw = false;
  try
  {
    index.findArticlesChecked( L"word00000" );
  }
  catch ( std::exception & )
  {
    threw = true;
  }
  QVERIFY2( threw, what );
}

} // namespace

class TestBtreeIndex : public QObject
{
  Q_OBJECT

private slots:

  // ---- the positive control: a real index reads back -------------------
  void builds_and_reads_back()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    Built built = buildIndex( dir.filePath( "index" ), 200 );

    QVERIFY( built.info.btreeMaxElements >= 1 );
    QVERIFY( built.info.btreeMaxElements <= kMaxElements );

    File::Class file( built.path.toStdString().c_str(), "rb" );
    Mutex mutex;

    QVERIFY( BtreeIndex::indexInfoIsSane( built.info, file, mutex ) );

    BtreeIndex index;
    index.openIndex( built.info, file, mutex );

    std::vector< BtreeIndexing::WordArticleLink > const chain =
        index.findArticlesChecked( L"word00007" );

    QCOMPARE( int( chain.size() ), 1 );
    QCOMPARE( chain[ 0 ].articleOffset, 7u );
  }

  // ---- the driver of the original abort -------------------------------
  void foreign_sounddir_header_is_not_sane()
  {
    // The exact first six words of the foreign 28-byte sound-directory header:
    // signature, version, sound count, chunk offset, then the two fields this
    // reader misinterprets as IndexInfo.
    QTemporaryDir dir;
    QString const path = dir.filePath( "foreign" );
    QByteArray bytes( 28, '\0' );
    putU32( bytes, 0,  0x58524453 );
    putU32( bytes, 4,  11 );
    putU32( bytes, 8,  142000 );
    putU32( bytes, 12, 28 );
    putU32( bytes, 16, 2057158 );
    putU32( bytes, 20, 378 );
    putU32( bytes, 24, 2053531 );
    writeBytes( path, bytes );

    File::Class file( path.toStdString().c_str(), "rb" );
    Mutex mutex;

    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( 2057158, 378 ), file, mutex ) );
    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( 378, 2057158 ), file, mutex ) );
  }

  void open_index_terminates()
  {
    QTemporaryDir dir;
    Built built = buildIndex( dir.filePath( "index" ), 200 );

    File::Class file( built.path.toStdString().c_str(), "rb" );
    Mutex mutex;

    BtreeIndex index;
    index.openIndex( built.info, file, mutex );   // a recursion here never returns
    QVERIFY( !index.findArticlesChecked( L"absent" ).size() );
  }

  void insane_index_info_is_inert()
  {
    QTemporaryDir dir;
    Built built = buildIndex( dir.filePath( "index" ), 200 );
    expectInert( built.path, IndexInfo( 2057158, 378 ),
                 "a foreign IndexInfo must leave the index inert" );
    expectInert( built.path, IndexInfo( 378, 2057158 ), "swapped fields must be inert" );
  }

  void empty_leaf_is_only_legal_at_the_root()
  {
    QTemporaryDir dir;
    QString const path = dir.filePath( "empty" );

    QByteArray bytes;
    int const at = appendRecord( bytes, QByteArray( 4, '\0' ), true, 0 );
    writeBytes( path, bytes );

    File::Class file( path.toStdString().c_str(), "rb" );
    Mutex mutex;

    // A 4-byte empty leaf is a legal empty tree root.
    QVERIFY( BtreeIndex::indexInfoIsSane( IndexInfo( 64, quint32( at ) ), file, mutex ) );
  }

  // ---- record stage ---------------------------------------------------
  void zeroed_root_record_is_inert()
  {
    QTemporaryDir dir;
    Built built = buildIndex( dir.filePath( "index" ), 200 );

    QByteArray bytes = built.bytes;
    // The original abort: both size words zero.
    putU32( bytes, int( built.info.rootOffset ), 0 );
    putU32( bytes, int( built.info.rootOffset ) + 4, 0 );
    writeBytes( built.path, bytes );

    expectInert( built.path, built.info, "a zeroed node record must be inert, not fatal" );
  }

  void short_root_record_is_inert()
  {
    QTemporaryDir dir;
    Built built = buildIndex( dir.filePath( "index" ), 200 );

    for ( quint32 shortSize : { 1u, 2u, 3u } )
    {
      QByteArray bytes = built.bytes;
      putU32( bytes, int( built.info.rootOffset ), shortSize );
      writeBytes( built.path, bytes );

      expectInert( built.path, built.info,
                   "an inflated size below one uint32_t must be inert" );
    }
  }

  void out_of_range_root_offset_is_rejected()
  {
    QTemporaryDir dir;
    Built built = buildIndex( dir.filePath( "index" ), 200 );

    File::Class file( built.path.toStdString().c_str(), "rb" );
    Mutex mutex;

    quint32 const past = quint32( built.bytes.size() + 1024 );
    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( built.info.btreeMaxElements, past ),
                                          file, mutex ) );
  }

  void oversized_btree_size_rejected()
  {
    QTemporaryDir dir;
    Built built = buildIndex( dir.filePath( "index" ), 200 );

    File::Class file( built.path.toStdString().c_str(), "rb" );
    Mutex mutex;

    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( 0, built.info.rootOffset ), file, mutex ) );
    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( kMaxElements + 1, built.info.rootOffset ),
                                           file, mutex ) );

    // The boundary values this build can actually produce stay acceptable.
    QVERIFY( BtreeIndex::indexInfoIsSane( built.info, file, mutex ) );
  }

  // ---- shape stage ----------------------------------------------------
  void truncated_node_offsets_array_is_inert()
  {
    QTemporaryDir dir;
    QString const path = dir.filePath( "node" );

    // An internal node marker with a payload far too small for m+1 offsets.
    QByteArray payload( 4, '\0' );
    putU32( payload, 0, 0xffffffff );

    QByteArray bytes;
    int const at = appendRecord( bytes, payload, false, 0 );
    writeBytes( path, bytes );

    File::Class file( path.toStdString().c_str(), "rb" );
    Mutex mutex;
    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( 64, quint32( at ) ), file, mutex ) );
  }

  void unterminated_separator_is_inert()
  {
    QTemporaryDir dir;
    QString const path = dir.filePath( "node" );

    quint32 const m = 64;
    QByteArray payload( 4 + int( m + 1 ) * 4, '\0' );
    putU32( payload, 0, 0xffffffff );
    payload.append( "abc" );            // no NUL terminator before the end

    QByteArray bytes;
    int const at = appendRecord( bytes, payload, false, 0 );
    writeBytes( path, bytes );

    File::Class file( path.toStdString().c_str(), "rb" );
    Mutex mutex;
    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( m, quint32( at ) ), file, mutex ) );
  }

  void absurd_leaf_entry_count_is_inert()
  {
    QTemporaryDir dir;
    QString const path = dir.filePath( "leaf" );

    QByteArray payload( 12, '\0' );
    putU32( payload, 0, 0x00ffffff );   // far more entries than m, or than fit
    QByteArray bytes;
    int const at = appendRecord( bytes, payload, true, 0 );
    writeBytes( path, bytes );

    File::Class file( path.toStdString().c_str(), "rb" );
    Mutex mutex;
    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( 64, quint32( at ) ), file, mutex ) );
  }

  void leaf_framing_not_exact_is_inert()
  {
    QTemporaryDir dir;
    QString const path = dir.filePath( "leaf" );

    QByteArray payload = leafPayload( "alpha", 1 );
    payload.append( "trailing" );       // framing no longer consumes the buffer
    QByteArray bytes;
    int const at = appendRecord( bytes, payload, true, 0 );
    writeBytes( path, bytes );

    File::Class file( path.toStdString().c_str(), "rb" );
    Mutex mutex;
    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( 64, quint32( at ) ), file, mutex ) );
  }

  void zero_size_chain_is_inert()
  {
    QTemporaryDir dir;
    QString const path = dir.filePath( "leaf" );

    QByteArray payload( 8, '\0' );
    putU32( payload, 0, 1 );            // one entry
    putU32( payload, 4, 0 );            // zero-size chain
    QByteArray bytes;
    int const at = appendRecord( bytes, payload, true, 0 );
    writeBytes( path, bytes );

    File::Class file( path.toStdString().c_str(), "rb" );
    Mutex mutex;
    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( 64, quint32( at ) ), file, mutex ) );
  }

  void truncated_inner_chain_record_is_inert()
  {
    QTemporaryDir dir;
    QString const path = dir.filePath( "leaf" );

    // A chain that ends after the word and prefix but before the 4-byte offset.
    QByteArray chain;
    chain.append( "alpha" );
    chain.append( '\0' );
    chain.append( '\0' );

    QByteArray payload( 8, '\0' );
    putU32( payload, 0, 1 );
    putU32( payload, 4, quint32( chain.size() ) );
    payload.append( chain );

    QByteArray bytes;
    int const at = appendRecord( bytes, payload, true, 0 );
    writeBytes( path, bytes );

    File::Class file( path.toStdString().c_str(), "rb" );
    Mutex mutex;
    QVERIFY( !BtreeIndex::indexInfoIsSane( IndexInfo( 64, quint32( at ) ), file, mutex ) );
  }

  // ---- a corrupt descendant must be contained, not fatal ---------------
  void corrupt_child_node_is_contained()
  {
    QTemporaryDir dir;
    Built built = buildIndex( dir.filePath( "index" ), 200 );

    // Read the root record straight out of the file, exactly as the traversal
    // would, and take its first child offset.
    quint32 const rootBytes = getU32( built.bytes, int( built.info.rootOffset ) );
    quint32 const rootCompressed = getU32( built.bytes, int( built.info.rootOffset ) + 4 );

    QByteArray const root =
        zlibDecompress( built.bytes.constData() + built.info.rootOffset + 8,
                        int( rootCompressed ), int( rootBytes ) );
    QVERIFY( !root.isEmpty() );
    QCOMPARE( getU32( root, 0 ), 0xffffffffu );   // the root of a 200-word tree is internal

    // Children are written before the root, so the very first child legitimately
    // starts at file offset 0. Take the second one, which must be a real offset.
    quint32 const childOffset = getU32( root, 8 );
    QVERIFY( childOffset != 0 );
    QVERIFY( childOffset < built.info.rootOffset );

    // Child i is entered for any target at or below separator i-1, so the first
    // separator is exactly the word that steers the traversal into the child we
    // just damaged.
    int const separatorsAt = 4 + int( built.info.btreeMaxElements + 1 ) * 4;
    QByteArray const firstSeparator =
        root.mid( separatorsAt, root.indexOf( '\0', separatorsAt ) - separatorsAt );
    QVERIFY( !firstSeparator.isEmpty() );
    wstring const target = Utf8::decode(
        std::string( firstSeparator.constData(), size_t( firstSeparator.size() ) ) );

    // Zero the child's record: the root stays valid, so openIndex succeeds and
    // only the traversal reaches the damaged node.
    QByteArray bytes = built.bytes;
    putU32( bytes, int( childOffset ), 0 );
    putU32( bytes, int( childOffset ) + 4, 0 );
    writeBytes( built.path, bytes );

    File::Class file( built.path.toStdString().c_str(), "rb" );
    Mutex mutex;

    QVERIFY( BtreeIndex::indexInfoIsSane( built.info, file, mutex ) );

    BtreeIndex index;
    index.openIndex( built.info, file, mutex );

    bool threw = false;
    try
    {
      index.findArticlesChecked( target );
    }
    catch ( std::exception & )
    {
      threw = true;
    }
    QVERIFY2( threw, "a corrupt descendant must surface as an error, not a fatal abort" );

    // The catching lookup keeps its documented contract: no throw, no results.
    QVERIFY( index.findArticles( target ).empty() );
  }

  // ---- chunked storage -------------------------------------------------
  void zero_size_chunk_throws()
  {
    QTemporaryDir dir;
    QString const path = dir.filePath( "chunks" );

    // Table with a single entry, then a record with no compressed stream.
    QByteArray bytes;
    bytes.append( QByteArray( 4, '\0' ) );
    putU32( bytes, 0, 1 );                       // one chunk offset
    bytes.append( QByteArray( 4, '\0' ) );
    quint32 const recordAt = quint32( bytes.size() );
    putU32( bytes, 4, recordAt );
    bytes.append( QByteArray( 8, '\0' ) );       // uncompressedSize 0, compressedSize 0
    writeBytes( path, bytes );

    File::Class file( path.toStdString().c_str(), "rb" );
    ChunkedStorage::Reader reader( file, 0 );

    vector< char > chunk;
    bool threw = false;
    try
    {
      reader.getBlock( 0, chunk );
    }
    catch ( std::exception & )
    {
      threw = true;
    }
    QVERIFY2( threw, "a zero-size chunk record must throw, never abort" );
  }

  void writer_oversized_chunk_reads_back()
  {
    QTemporaryDir dir;
    QString const path = dir.filePath( "chunks" );

    QByteArray block;
    block.resize( 65537 );                       // one byte past ChunkMaxSize
    for ( int i = 0; i < block.size(); ++i )
      block[ i ] = char( ( i * 31 + 7 ) & 0x7f );

    quint32 tableOffset = 0;
    {
      File::Class file( path.toStdString().c_str(), "wb" );
      ChunkedStorage::Writer writer( file );
      writer.startNewBlock();
      writer.addToBlock( block.constData(), block.size() );
      tableOffset = writer.finish();
    }

    File::Class file( path.toStdString().c_str(), "rb" );
    ChunkedStorage::Reader reader( file, tableOffset );

    vector< char > chunk;
    char * data = reader.getBlock( 0, chunk );
    QCOMPARE( int( chunk.size() ), block.size() );
    QVERIFY( data == chunk.data() );
    QVERIFY( memcmp( data, block.constData(), size_t( block.size() ) ) == 0 );
  }
};

QTEST_MAIN( TestBtreeIndex )
#include "tst_btree_index.moc"
