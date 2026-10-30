/* This file is (c) 2014 Abs62
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "epwing_charmap.hh"

namespace Epwing {

EpwingCharmap & EpwingCharmap::instance()
{
  static EpwingCharmap ec;
  return ec;
}

QByteArray EpwingCharmap::mapToUtf8( QString const & code )
{
  if( charMap.contains( code ) )
    return QString( charMap[ code ] ).toUtf8();

  return QByteArray();
}

void EpwingCharmap::addEntry( QString const & code, QChar ch )
{
  charMap[ code ] = ch;
}

EpwingCharmap::EpwingCharmap()
{
  addEntry( "na121", QChar( 0x00E1 ) );
  addEntry( "na122", QChar( 0x00E0 ) );
  addEntry( "na12a", QChar( 0x01DD ) );
  addEntry( "na12e", QChar( 0x00E9 ) );
  addEntry( "na12f", QChar( 0x00E8 ) );
  addEntry( "na134", QChar( 0x00ED ) );
  addEntry( "na135", QChar( 0x00EC ) );
  addEntry( "na136", QChar( 0x00F3 ) );
  addEntry( "na137", QChar( 0x00F2 ) );
  addEntry( "na13e", QChar( 0x00FA ) );
  addEntry( "na13f", QChar( 0x00F9 ) );
  addEntry( "na143", QChar( 0x00E6 ) );
  addEntry( "na144", QChar( 0x01FD ) );
  addEntry( "na149", QChar( 0x0067 ) );
  addEntry( "na157", QChar( 0x00E9 ) );
  addEntry( "na16a", QChar( 0x00C1 ) );
  addEntry( "na16b", QChar( 0x00C0 ) );
  addEntry( "na16d", QChar( 0x00C9 ) );
  addEntry( "na171", QChar( 0x00FD ) );
  addEntry( "na172", QChar( 0x1EF3 ) );
  addEntry( "na17d", QChar( 0x00DA ) );
  addEntry( "na235", QChar( 0x002F ) );
  addEntry( "na240", QChar( 0x0154 ) );
  addEntry( "na244", QChar( 0x0179 ) );
  addEntry( "na627", QChar( 0x00A7 ) );
  addEntry( "na62b", QChar( 0x00AB ) );
  addEntry( "na637", QChar( 0x30FB ) );
  addEntry( "na63b", QChar( 0x00BB ) );
  addEntry( "na646", QChar( 0x00C6 ) );
  addEntry( "na647", QChar( 0x00C7 ) );
  addEntry( "na649", QChar( 0x00C9 ) );
  addEntry( "na660", QChar( 0x00E0 ) );
  addEntry( "na662", QChar( 0x00E2 ) );
  addEntry( "na664", QChar( 0x00E4 ) );
  addEntry( "na667", QChar( 0x00E7 ) );
  addEntry( "na668", QChar( 0x00E8 ) );
  addEntry( "na669", QChar( 0x00E9 ) );
  addEntry( "na66a", QChar( 0x00EA ) );
  addEntry( "na66b", QChar( 0x00EB ) );
  addEntry( "na66e", QChar( 0x00EE ) );
  addEntry( "na66f", QChar( 0x00EF ) );
  addEntry( "na671", QChar( 0x00F1 ) );
  addEntry( "na674", QChar( 0x00F4 ) );
  addEntry( "na676", QChar( 0x00F6 ) );
  addEntry( "na67b", QChar( 0x00FB ) );
  addEntry( "na67c", QChar( 0x00FC ) );
  addEntry( "na722", QChar( 0x0152 ) );
  addEntry( "na723", QChar( 0x0153 ) );
  addEntry( "na73e", QChar( 0x0101 ) );
  addEntry( "na73f", QChar( 0x0103 ) );
  addEntry( "na740", QChar( 0x0113 ) );
  addEntry( "na741", QChar( 0x0115 ) );
  addEntry( "na742", QChar( 0x012B ) );
  addEntry( "na743", QChar( 0x012D ) );
  addEntry( "na744", QChar( 0x014F ) );
  addEntry( "na746", QChar( 0x016B ) );
  addEntry( "na747", QChar( 0x01D4 ) );
  addEntry( "na74b", QChar( 0x0227 ) );
  addEntry( "na753", QChar( 0x1EBD ) );
  addEntry( "na823", QChar( 0x2026 ) );
  addEntry( "wa424", QChar( 0x00E6 ) );
  addEntry( "wa460", QChar( 0x2460 ) );
  addEntry( "wa461", QChar( 0x2461 ) );
  addEntry( "wa462", QChar( 0x2462 ) );
  addEntry( "wa463", QChar( 0x2463 ) );
  addEntry( "wa464", QChar( 0x2464 ) );
}

} // namespace Epwing
