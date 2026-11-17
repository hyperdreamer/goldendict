#include "imagepayload.hh"
#include "tiff.hh"

#include <QBuffer>

namespace ImagePayload {

QImage decodeWithQt( char const * bytes, std::size_t size )
{
  if ( !size )
    return QImage();
  return QImage::fromData( reinterpret_cast< uchar const * >( bytes ), size );
}

QImage decodeWithGdTiff( char const * bytes, std::size_t size )
{
  if ( !size )
    return QImage();
#ifdef MAKE_EXTRA_TIFF_HANDLER
  QImage image;
  GdTiff::tiffToQImage( bytes, size, image );
  return image;
#else
  (void) bytes;
  return QImage();
#endif
}

QByteArray tiffToBmp( char const * bytes, std::size_t size )
{
  if ( !size )
    return QByteArray();
  QImage image = decodeWithQt( bytes, size );
  if ( image.isNull() )
    image = decodeWithGdTiff( bytes, size );
  if ( image.isNull() )
    return QByteArray();
  QBuffer buffer;
  if ( !buffer.open( QIODevice::WriteOnly ) || !image.save( &buffer, "BMP" ) )
    return QByteArray();
  return buffer.data();
}

}
