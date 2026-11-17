#ifndef __PAYLOAD_HH_INCLUDED__
#define __PAYLOAD_HH_INCLUDED__

#include <cstddef>
#include <cstring>
#include <vector>

/// Byte copies over a dictionary resource payload buffer.
///
/// `assign` makes the destination hold exactly the given bytes, replacing
/// whatever it held before; `append` adds the given bytes past the current end
/// of the destination and keeps the existing content.
namespace Payload {

inline void assign( std::vector< char > & destination,
                    char const * bytes, std::size_t size )
{
  destination.resize( size );
  std::memcpy( &destination.front(), bytes, size );
}

inline void append( std::vector< char > & destination,
                    char const * bytes, std::size_t size )
{
  std::size_t const oldSize = destination.size();
  destination.resize( oldSize + size );
  std::memcpy( &destination.front() + oldSize, bytes, size );
}

}

#endif
