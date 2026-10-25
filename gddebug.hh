#ifndef __GDDEBUG_HH_INCLUDED__
#define __GDDEBUG_HH_INCLUDED__

#include <QFile>

// Transition shim for indexedzip.cc and epwing_book.cc. Task 8, which owns
// those callers, removes this adapter, the legacy type alias and the entry
// point when it switches them to Encoding::forLocale(). This is not a Qt codec:
// it exposes only decoding and delegates every call to the decode-only value.
class GdLocaleDecodeOnlyCodec
{
public:
  QString toUnicode( char const * data, qsizetype size ) const noexcept;
  QString toUnicode( char const * data ) const noexcept;
  QString toUnicode( QByteArray const & data ) const noexcept;
};
using QTextCodec = GdLocaleDecodeOnlyCodec;

QTextCodec * gdCodecForLocale() noexcept;

#ifdef NO_CONSOLE
  #define GD_DPRINTF(...) do {} while( 0 )
  #define GD_FDPRINTF(...) do {} while( 0 )
#else
  #ifdef NO_GD_DPRINTF
    #define GD_DPRINTF(...) do {} while( 0 )
  #else
    #define GD_DPRINTF(...) printf(__VA_ARGS__)
  #endif
  #define GD_FDPRINTF(...) fprintf(__VA_ARGS__)
#endif

void gdWarning(const char *, ...) /* print warning message */
#if defined(Q_CC_GNU) && !defined(__INSURE__)
    __attribute__ ((format (printf, 1, 2)))
#endif
;

void gdDebug(const char *, ...)
#if defined(Q_CC_GNU) && !defined(__INSURE__)
    __attribute__ ((format (printf, 1, 2)))
#endif
;

extern QFile * logFilePtr;

#endif // __GDDEBUG_HH_INCLUDED__
