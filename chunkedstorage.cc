/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "chunkedstorage.hh"
#include <zlib.h>
#include <string.h>

namespace ChunkedStorage {

enum
{
  ChunkMaxSize = 65536 // Can't be more since it would overflow the address
};

Writer::Writer( File::Class & f ):
  file( f ), chunkStarted( false ), bufferUsed( 0 )
{
  // Create a sratchpad at the beginning of file. We use it to write chunk
  // table if it would fit, in order to save some seek times.

  char zero[ 4096 ];

  memset( zero, 0, sizeof( zero ) );

  scratchPadOffset = file.tell();
  scratchPadSize = sizeof( zero );

  file.write( zero, sizeof( zero ) );
}

uint32_t Writer::startNewBlock()
{
  if ( bufferUsed >= ChunkMaxSize )
  {
    // Need to flush first.
    saveCurrentChunk();
  }

  chunkStarted = true;

  // The address is comprised of the offset within the chunk (in lower
  // 16 bits, always fits there since ChunkMaxSize-1 does) and the
  // number of the chunk, which is therefore limited to be 65535 max.
  return bufferUsed | ( (uint32_t)offsets.size() << 16 );
}

void Writer::addToBlock( void const * data, size_t size )
{
  if ( !size )
    return;

  if ( buffer.size() - bufferUsed < size )
    buffer.resize( bufferUsed + size );

  memcpy( &buffer.front() + bufferUsed, data, size );

  bufferUsed += size;

  chunkStarted = false;
}

void Writer::saveCurrentChunk()
{
  size_t maxCompressedSize = compressBound( bufferUsed );

  if ( bufferCompressed.size() < maxCompressedSize )
    bufferCompressed.resize( maxCompressedSize );

  unsigned long compressedSize = bufferCompressed.size();

  if ( compress( &bufferCompressed.front(), &compressedSize,
                 &buffer.front(), bufferUsed ) != Z_OK )
    throw exFailedToCompressChunk();

  offsets.push_back( file.tell() );

  file.write( (uint32_t) bufferUsed );
  file.write( (uint32_t) compressedSize );
  file.write( &bufferCompressed.front(), compressedSize );

  bufferUsed = 0;

  chunkStarted = false;
}

uint32_t Writer::finish()
{
  if ( bufferUsed || chunkStarted )
    saveCurrentChunk();

  bool useScratchPad = false;
  uint32_t savedOffset = 0;

  if ( scratchPadSize >= offsets.size() * sizeof( uint32_t ) + sizeof( uint32_t ) )
  {
    useScratchPad = true;
    savedOffset = file.tell();
    file.seek( scratchPadOffset );
  }

  uint32_t offset = file.tell();

  file.write( (uint32_t) offsets.size() );

  if ( offsets.size() )
    file.write( &offsets.front(), offsets.size() * sizeof( uint32_t ) );

  if ( useScratchPad )
    file.seek( savedOffset );

  offsets.clear();
  chunkStarted = false;

  return offset;
}

Reader::Reader( File::Class & f, uint32_t offset ): file( f )
{
  file.seek( offset );

  uint32_t size =  file.read< uint32_t >();
  if ( size == 0 )
    return;

  // The table length comes from the file, so it may only be trusted once the
  // whole table is known to fit. Validate the product in 64 bits before it sizes
  // an allocation or a read.
  qint64 const fileSize = file.file().size();
  uint64_t const remaining = fileSize < 0 || (uint64_t)fileSize < (uint64_t)offset + 4u
                               ? 0u
                               : (uint64_t)fileSize - (uint64_t)offset - 4u;

  if ( (uint64_t)size > remaining / sizeof( uint32_t ) )
    throw exAddressOutOfRange();

  offsets.resize( size );
  file.read( &offsets.front(), offsets.size() * sizeof( uint32_t ) );
}

char * Reader::getBlock( uint32_t address, vector< char > & chunk )
{
  size_t chunkIdx = address >> 16;

  if ( chunkIdx >= offsets.size() )
    throw exAddressOutOfRange();

  // Read and decompress the chunk
  {
    qint64 const recordOffset = offsets[ chunkIdx ];

    qint64 const fileSize = file.file().size();

    if ( fileSize < 0 || recordOffset > fileSize - (qint64)sizeof( uint32_t ) * 2 )
      throw exFailedToDecompressChunk();

    file.seek( recordOffset );

    uint32_t const uncompressedSize = file.read< uint32_t >();
    uint32_t const compressedSize = file.read< uint32_t >();

    // A chunk always carries a real zlib stream, so a zero compressed size is
    // corrupt data, not an empty block. An advertised output larger than the
    // deflate format can ever produce from that payload is likewise corrupt.
    // Both are rejected before anything is resized or read.
    if ( !compressedSize )
      throw exFailedToDecompressChunk();

    if ( (uint64_t)compressedSize > (uint64_t)fileSize - (uint64_t)recordOffset -
                                     (uint64_t)sizeof( uint32_t ) * 2 )
      throw exFailedToDecompressChunk();

    if ( (uint64_t)uncompressedSize > (uint64_t)compressedSize * 1032u + 64u )
      throw exFailedToDecompressChunk();

    vector< unsigned char > compressedData( compressedSize );

    file.read( &compressedData.front(), compressedData.size() );

    // Effective storage of a corrupt or absent block: allocate at least one byte
    // so no zero-length buffer is ever handed to the decompressor, and resize the
    // result only after the stream has been shown to match the advertised length.
    vector< char > scratch( uncompressedSize ? uncompressedSize : 1 );

    unsigned long decompressedLength = uncompressedSize;

    if ( uncompress( (unsigned char *)&scratch.front(),
                     &decompressedLength,
                     &compressedData.front(),
                     compressedData.size() ) != Z_OK ||
         decompressedLength != uncompressedSize )
      throw exFailedToDecompressChunk();

    chunk.assign( scratch.begin(), scratch.begin() + uncompressedSize );
  }

  size_t offsetInChunk = address & 0xffFF;

  if ( offsetInChunk > chunk.size() ) // It can be equal to for 0-sized blocks
    throw exAddressOutOfRange();

  // A legal zero-length block may leave the chunk empty; returning data() keeps
  // the one-past-the-end result without forming a reference to an empty vector.
  char * base = chunk.empty() ? chunk.data() : &chunk.front();

  return base + offsetInChunk;
}

}
