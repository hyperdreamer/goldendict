#include "wstring_qt.hh"
#include <QVector>

namespace gd
{
  #ifdef __WIN32

  QString toQString( wstring const & in )
  {
    // wstring is basic_string<unsigned int> here, so the full scalar sequence
    // has to be converted explicitly by length: the terminating NUL must not
    // cut an embedded NUL or a supplementary-plane character short. The storage
    // is handed over as char32_t so the current explicit-length overload is
    // selected: QString::fromUcs4( uint const *, qsizetype ) is deprecated since
    // Qt 6.0 and a hard error when QT_DISABLE_DEPRECATED_UP_TO reaches 6.0.
    return QString::fromUcs4( reinterpret_cast< char32_t const * >( in.data() ),
                              static_cast< qsizetype >( in.size() ) );
  }

  #else

  QString toQString( wstring const & in )
  {
    return QString::fromStdWString( in );
  }

  #endif

  wstring toWString( QString const & in )
  {
    QVector< unsigned int > v = in.toUcs4();

    // Fix for QString instance which contains non-BMP characters
    // Qt will created unexpected null characters may confuse btree indexer.
    // Related: https://bugreports.qt-project.org/browse/QTBUG-25536
    qsizetype n = v.size();
    while ( n > 0 && v[ n - 1 ] == 0 ) n--;
    if ( n != v.size() )
      v.resize( n );

    return wstring( reinterpret_cast< wchar const * >( v.constData() ),
                    static_cast< size_t >( v.size() ) );
  }

  wstring normalize( const wstring & str )
  {
    return gd::toWString( gd::toQString( str ).normalized( QString::NormalizationForm_C ) );
  }

}
