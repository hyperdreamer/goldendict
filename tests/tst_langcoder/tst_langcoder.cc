/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * The language-code helpers (langcoder.cc) that the dictionary backends use to
 * label their dictionaries. These are fixed expectations for the two-language
 * code form and the English-name lookup, including the unknown-code path.
 */

#include <QtTest>
#include "support/fixtures.hh"
#include "langcoder.hh"

class TstLangCoder: public QObject
{
  Q_OBJECT

private slots:
  void initTestCase();
  void mapsTwoLetterCodes();
  void decodesKnownIds();
  void unknownIdsAreEmpty();
  void code3LookupsResolve();
  void recognisesNamePairsCaseInsensitively();
  void recognisesFilenamePairsThroughProductionEntryPoint();
};

void TstLangCoder::initTestCase()
{
  QStringList reasons;
  QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
}

void TstLangCoder::mapsTwoLetterCodes()
{
  QCOMPARE( LangCoder::code2toInt( "en" ), quint32( 'e' | ( quint32( 'n' ) << 8 ) ) );
  QCOMPARE( LangCoder::intToCode2( LangCoder::code2toInt( "de" ) ), QStringLiteral( "de" ) );
}

void TstLangCoder::decodesKnownIds()
{
  QCOMPARE( LangCoder::decode( LangCoder::code2toInt( "en" ) ), QStringLiteral( "English" ) );
  QCOMPARE( LangCoder::decode( LangCoder::code2toInt( "ru" ) ), QStringLiteral( "Russian" ) );
}

void TstLangCoder::unknownIdsAreEmpty()
{
  QCOMPARE( LangCoder::decode( LangCoder::code2toInt( "zz" ) ), QString() );
}

void TstLangCoder::code3LookupsResolve()
{
  QCOMPARE( LangCoder::findIdForLanguageCode3( "eng" ), LangCoder::code2toInt( "en" ) );
}

void TstLangCoder::recognisesNamePairsCaseInsensitively()
{
  // The production lookup folds case before matching, so mixed-case input must
  // resolve to the same pair as the lower-case form.
  QPair< quint32, quint32 > const mixed = LangCoder::findIdsForName( QStringLiteral( "EN-Ru" ) );
  QCOMPARE( mixed, LangCoder::findIdsForName( QStringLiteral( "en-ru" ) ) );
  QCOMPARE( mixed.first, LangCoder::code2toInt( "en" ) );
  QCOMPARE( mixed.second, LangCoder::code2toInt( "ru" ) );

  QVERIFY( LangCoder::findIdsForName( QStringLiteral( "not a pair" ) ).first == 0 );
}

void TstLangCoder::recognisesFilenamePairsThroughProductionEntryPoint()
{
  // findIdsForFilename goes through QFileInfo, so the directory must not leak
  // into the match and the file name's case must not matter.
  QPair< quint32, quint32 > const fromPath =
    LangCoder::findIdsForFilename( QStringLiteral( "/tmp/some/De-En.dict" ) );
  QCOMPARE( fromPath.first, LangCoder::code2toInt( "de" ) );
  QCOMPARE( fromPath.second, LangCoder::code2toInt( "en" ) );
}

QTEST_MAIN( TstLangCoder )
#include "tst_langcoder.moc"
