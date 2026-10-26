/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef __WEBSITE_HH_INCLUDED__
#define __WEBSITE_HH_INCLUDED__

#include "dictionary.hh"
#include "config.hh"
#include <QNetworkAccessManager>
#include <QNetworkReply>

/// Support for any web sites via a templated url.

#include "encoding.hh"
#include "gddebug.hh"

namespace WebSite {


/// Template URL substitutions. Kept here (and nowhere else) because they are the
/// only part of the website engine that the encoding migration has to pin: a
/// placeholder whose charset is unavailable, or whose text cannot be represented,
/// is left untouched with one diagnostic instead of being replaced by escapes.
namespace detail {

/// Replaces one template placeholder. A placeholder whose charset is unavailable,
/// or whose text has no mapping in it, is left untouched with one diagnostic --
/// the behaviour the encoding contract pins.
inline void replaceUrlPlaceholder( QByteArray & result, QString const & word,
                                   QByteArray const & placeholder, QString const & name )
{
  if ( !result.contains( placeholder ) ) return;
  const auto warn = [&] {
    qWarning( "Encoding: URL placeholder %s not representable in %s",
              placeholder.constData(), qPrintable( name ) );
  };
  const auto codec = Encoding::byName( name );
  if ( !codec ) { warn(); return; }
  try { result.replace( placeholder, codec->encode( word ).toPercentEncoding() ); }
  catch ( Encoding::exUnrepresentable const & ) { warn(); }
}

inline QByteArray substituteUrlPlaceholders( QByteArray const & urlTemplate, QString const & word )
{
  QByteArray result = urlTemplate;
  result.replace( "%25GDWORD%25", word.toUtf8().toPercentEncoding() );
  const auto replace = [&]( QByteArray const & placeholder, QString const & name ) {
    replaceUrlPlaceholder( result, word, placeholder, name );
  };
  replace( "%25GD1251%25", QStringLiteral( "Windows-1251" ) );
  replace( "%25GDBIG5%25", QStringLiteral( "Big-5" ) );
  replace( "%25GDBIG5HKSCS%25", QStringLiteral( "Big5-HKSCS" ) );
  replace( "%25GDSHIFTJIS%25", QStringLiteral( "Shift-JIS" ) );
  replace( "%25GDGBK%25", QStringLiteral( "GB18030" ) );
  for ( int x = 1; x <= 16; ++x )
  {
    replace( QString( "%25GDISO%1%25" ).arg( x ).toLatin1(), QString( "ISO-8859-%1" ).arg( x ) );
    if ( x == 10 ) x = 12; // Preserve the caller's ISO11/12 skip.
  }
  return result;
}

} // namespace detail


using std::vector;
using std::string;

vector< sptr< Dictionary::Class > > makeDictionaries( Config::WebSites const &,
                                                      QNetworkAccessManager & )
    THROW_SPEC( std::exception );

/// Exposed here for moc
class WebSiteDataRequestSlots: public Dictionary::DataRequest
{
  Q_OBJECT

protected slots:

  virtual void requestFinished( QNetworkReply * )
  {}
};

}

#endif
