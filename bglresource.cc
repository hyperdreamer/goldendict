/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "bglresource.hh"

#include <cctype>
#include <zlib.h>

namespace BglResource {

void readDirectoryEntry( File::Class & idx, DirectoryEntry & entry )
{
  entry.name.resize( idx.read< uint32_t >() );
  if ( !entry.name.empty() )
    idx.read( &entry.name.front(), entry.name.size() );
  entry.offset = idx.read< uint32_t >();
}

bool findResource( File::Class & idx, uint32_t count,
                   std::string const & lowercaseName, uint32_t & offset )
{
  while ( count-- )
  {
    DirectoryEntry entry;
    readDirectoryEntry( idx, entry );
    if ( entry.name.empty() )
      continue;
    for ( std::string::iterator i = entry.name.begin(); i != entry.name.end(); ++i )
      *i = std::tolower( *i );
    if ( entry.name == lowercaseName )
    {
      offset = entry.offset;
      return true;
    }
  }
  return false;
}

bool decompress( std::vector< char > const & compressed,
                 std::size_t advertisedSize, std::vector< char > & data )
{
  data.resize( advertisedSize );
  if ( compressed.empty() )
    return false;
  Bytef scratch;
  uLongf actualLength = advertisedSize ? data.size() : 1;
  Bytef * destination = advertisedSize
    ? reinterpret_cast< Bytef * >( &data.front() ) : &scratch;
  int const result = uncompress(
    destination, &actualLength,
    reinterpret_cast< Bytef const * >( &compressed.front() ), compressed.size() );
  return result == Z_OK && actualLength == advertisedSize;
}

}
