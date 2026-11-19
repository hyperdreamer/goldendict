#ifndef __IMAGEPAYLOAD_HH_INCLUDED__
#define __IMAGEPAYLOAD_HH_INCLUDED__

#include <cstddef>
#include <QByteArray>
#include <QImage>

namespace ImagePayload {

QImage decodeWithQt( char const * bytes, std::size_t size );
QImage decodeWithGdTiff( char const * bytes, std::size_t size );
QByteArray tiffToBmp( char const * bytes, std::size_t size );

}

#endif // __IMAGEPAYLOAD_HH_INCLUDED__
