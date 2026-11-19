/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef __BGLRESOURCE_HH_INCLUDED__
#define __BGLRESOURCE_HH_INCLUDED__

#include "file.hh"
#include <cstddef>
#include <stdint.h>
#include <string>
#include <vector>

namespace BglResource {
struct DirectoryEntry
{
  std::string name;
  uint32_t offset;
};
void readDirectoryEntry( File::Class & idx, DirectoryEntry & entry );
bool findResource( File::Class & idx, uint32_t count,
                   std::string const & lowercaseName, uint32_t & offset );
bool decompress( std::vector< char > const & compressed,
                 std::size_t advertisedSize, std::vector< char > & data );
}

#endif
