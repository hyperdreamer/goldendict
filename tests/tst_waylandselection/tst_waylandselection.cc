#include <QtTest>
#include "support/fixtures.hh"
#include "waylandselectionlogic.hh"
using namespace WaylandSelection;

class TstWaylandSelection : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanupTestCase();
  void mimePreferenceOrder();
  void payloadClampIsAppliedWhileReading();
  void decodeReplacesInvalidUtf8();
  void decodeStripsNul();
  void chooseTextUsesRecency();
  void clipboardStampIgnoresNonClipboardModes();
  void mimeAttemptsAdvanceAndExhaust();
  void payloadAtCapIsNotTruncated();
  void payloadOverCapIsTruncated();
  void offerSupersessionRetiresToken();
  void clipboardOfferIsDiscarded();
  void retiredOfferIsNeverReused();
  void nullOfferClearsWithoutRead();
  void activationPlanPromotesPendingCapture();
};
void TstWaylandSelection::initTestCase()
{
  QStringList reasons;
  QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
  TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
}
void TstWaylandSelection::cleanupTestCase() { TestSupport::clearWatchdog(); }
void TstWaylandSelection::mimePreferenceOrder()
{
  QStringList offered{ "text/html", "text/plain", "UTF8_STRING", "text/plain;charset=UTF-8",
                       "image/png", "text/plain;charset=utf-8", "text/plain;charset=utf-8" };
  for( QString const & expected : QStringList{ "text/plain;charset=utf-8", "text/plain;charset=UTF-8",
                                              "UTF8_STRING", "text/plain" } )
  {
    QCOMPARE( chooseMimeType( offered ), expected );
    offered.removeAll( expected );
  }
  QVERIFY( chooseMimeType( {} ).isEmpty() );
  QVERIFY( chooseMimeType( { "image/png", "text/html", "TEXT/PLAIN" } ).isEmpty() );
}
void TstWaylandSelection::payloadClampIsAppliedWhileReading()
{
  PayloadAccumulator p;
  p.append( QByteArray( 400000, 'a' ) );
  QCOMPARE( p.payload().size(), 400000 ); QVERIFY( !p.full() ); QVERIFY( !p.truncated() );
  p.append( QByteArray( 400000, 'b' ) );
  QCOMPARE( p.payload().size(), 800000 ); QVERIFY( !p.full() ); QVERIFY( !p.truncated() );
  p.append( QByteArray( 400000, 'c' ) );
  QByteArray const prefix = QByteArray( 400000, 'a' ) + QByteArray( 400000, 'b' ) + QByteArray( 248576, 'c' );
  QCOMPARE( p.payload(), prefix ); QVERIFY( p.full() ); QVERIFY( p.truncated() );
  p.append( QByteArray( 17, 'd' ) );
  QCOMPARE( p.payload(), prefix ); QVERIFY( p.truncated() );
  PayloadAccumulator one;
  one.append( QByteArray( MaxPayloadBytes + 17, 'x' ) );
  QCOMPARE( one.payload(), QByteArray( MaxPayloadBytes, 'x' ) ); QVERIFY( one.truncated() );
  PayloadAccumulator small( 3 );
  small.append( "ab" ); small.append( "c" );
  QCOMPARE( small.payload(), QByteArray( "abc" ) ); QVERIFY( small.full() ); QVERIFY( !small.truncated() );
  small.append( {} ); QVERIFY( !small.truncated() );
  PayloadAccumulator zero( 0 );
  QVERIFY( zero.full() ); QVERIFY( !zero.truncated() );
  zero.append( {} ); QVERIFY( !zero.truncated() );
  zero.append( "x" ); QVERIFY( zero.truncated() ); QVERIFY( zero.payload().isEmpty() );
}
void TstWaylandSelection::decodeReplacesInvalidUtf8()
{
  QCOMPARE( decodePayload( QByteArray::fromHex( "61ff62" ) ), QString( "a" ) + QChar( 0xfffd ) + "b" );
  QCOMPARE( decodePayload( QByteArray::fromHex( "c3a9" ) ), QString( QChar( 0x00e9 ) ) );
}
void TstWaylandSelection::decodeStripsNul()
{
  QString const text = decodePayload( QByteArray::fromHex( "0061006200" ) );
  QCOMPARE( text, QString( "ab" ) ); QVERIFY( !text.contains( QChar( 0 ) ) );
}
void TstWaylandSelection::chooseTextUsesRecency()
{
  QCOMPARE( chooseText( "selected", 20, "copied", 10 ), QString( "selected" ) );
  QCOMPARE( chooseText( "selected", 10, "copied", 20 ), QString( "copied" ) );
  QCOMPARE( chooseText( "selected", 20, "copied", 20 ), QString( "selected" ) );
  QCOMPARE( chooseText( " \t ", 30, "copied", 20 ), QString( "copied" ) );
  QCOMPARE( chooseText( "selected", 1, "copied", 0 ), QString( "selected" ) );
  QCOMPARE( chooseText( "selected", 10, " \t ", 20 ), QString( "selected" ) );
  QVERIFY( chooseText( " \n", 0, {}, 0 ).isEmpty() );
  QCOMPARE( chooseText( " selected ", 20, "copied", 10 ), QString( " selected " ) );
  QCOMPARE( chooseText( {}, 0, " copied ", 20 ), QString( " copied " ) );
}
void TstWaylandSelection::clipboardStampIgnoresNonClipboardModes()
{
  // The booleans represent Clipboard, Selection and FindBuffer. No clipboard object is linked.
  bool const isClipboard[] = { true, false, false };
  bool const expected[] = { true, false, false };
  for( int i = 0; i != 3; ++i ) QCOMPARE( clipboardStampAdvances( isClipboard[i] ), expected[i] );
}
void TstWaylandSelection::mimeAttemptsAdvanceAndExhaust()
{
  OfferState s;
  s.pendingOffer( 1, { "text/plain", "UTF8_STRING", "text/plain;charset=UTF-8",
                       "text/plain;charset=utf-8", "text/plain", "image/png" } );
  QCOMPARE( s.takeForPrimary().id, quint64( 1 ) );
  for( QString const & mime : QStringList{ "text/plain;charset=utf-8", "text/plain;charset=UTF-8",
                                          "UTF8_STRING", "text/plain" } )
  {
    QVERIFY( s.beginMimeAttempt() ); QCOMPARE( s.currentMimeType(), mime );
    QVERIFY( s.beginMimeAttempt() ); QCOMPARE( s.currentMimeType(), mime );
    s.mimeAttemptFailed();
  }
  QVERIFY( !s.beginMimeAttempt() ); QVERIFY( s.currentMimeType().isEmpty() );
  s.retire(); QVERIFY( !s.beginMimeAttempt() );
}
void TstWaylandSelection::payloadAtCapIsNotTruncated()
{
  PayloadAccumulator p;
  for( int i = 0; i != MaxPayloadBytes / 4096; ++i ) p.append( QByteArray( 4096, 'a' ) );
  QCOMPARE( p.payload(), QByteArray( MaxPayloadBytes, 'a' ) );
  QVERIFY( p.full() ); QVERIFY( !p.truncated() );
  p.append( {} ); QVERIFY( !p.truncated() );
}
void TstWaylandSelection::payloadOverCapIsTruncated()
{
  PayloadAccumulator p;
  p.append( QByteArray( MaxPayloadBytes, 'a' ) ); p.append( "b" );
  QCOMPARE( p.payload(), QByteArray( MaxPayloadBytes, 'a' ) ); QVERIFY( p.full() ); QVERIFY( p.truncated() );
  PayloadAccumulator one;
  one.append( QByteArray( MaxPayloadBytes + 1, 'a' ) );
  QCOMPARE( one.payload(), p.payload() ); QVERIFY( one.full() ); QVERIFY( one.truncated() );
}
void TstWaylandSelection::offerSupersessionRetiresToken()
{
  OfferState s; s.pendingOffer( 1, { "text/plain" } ); s.takeForPrimary();
  QVERIFY( s.beginMimeAttempt() ); auto const a = s.beginRead(); QVERIFY( a > 0 ); QVERIFY( s.accepts( a ) );
  s.pendingOffer( 2, { "text/plain" } ); QCOMPARE( s.takeForPrimary().id, quint64( 2 ) );
  QVERIFY( !s.accepts( a ) ); QVERIFY( s.beginMimeAttempt() ); auto const b = s.beginRead();
  QVERIFY( b > a ); QVERIFY( s.accepts( b ) );
  s.completed( a ); QVERIFY( s.accepts( b ) ); s.completed( b ); QVERIFY( !s.accepts( b ) );
}
void TstWaylandSelection::clipboardOfferIsDiscarded()
{
  OfferState s; s.pendingOffer( 1, { "text/plain" } ); s.takeForPrimary();
  QVERIFY( s.beginMimeAttempt() ); auto const a = s.beginRead();
  s.pendingOffer( 2, { "text/plain" } ); QCOMPARE( s.discardForClipboard().id, quint64( 2 ) );
  QVERIFY( s.accepts( a ) ); QCOMPARE( s.beginRead(), ReadToken( 0 ) );
  OfferState clipboardOnly; clipboardOnly.pendingOffer( 2, { "text/plain" } );
  QCOMPARE( clipboardOnly.discardForClipboard().id, quint64( 2 ) );
  QVERIFY( !clipboardOnly.beginMimeAttempt() ); QCOMPARE( clipboardOnly.beginRead(), ReadToken( 0 ) );
}
void TstWaylandSelection::retiredOfferIsNeverReused()
{
  OfferState s; s.pendingOffer( 1, { "text/plain" } ); s.takeForPrimary();
  QVERIFY( s.beginMimeAttempt() ); auto const token = s.beginRead(); s.retire(); s.retire();
  QVERIFY( !s.accepts( token ) ); QVERIFY( !s.beginMimeAttempt() );
  QCOMPARE( s.beginRead(), ReadToken( 0 ) ); QVERIFY( s.currentMimeType().isEmpty() );
  OfferState exhausted; exhausted.pendingOffer( 1, { "text/plain" } ); exhausted.takeForPrimary();
  QVERIFY( exhausted.beginMimeAttempt() ); exhausted.mimeAttemptFailed();
  QVERIFY( !exhausted.beginMimeAttempt() ); QCOMPARE( exhausted.beginRead(), ReadToken( 0 ) );
}
void TstWaylandSelection::nullOfferClearsWithoutRead()
{
  OfferState s; s.pendingOffer( 1, { "text/plain" } ); s.takeForPrimary();
  QVERIFY( s.beginMimeAttempt() ); auto const token = s.beginRead();
  s.pendingOffer( 0, {} ); ChannelOffer const offer = s.takeForPrimary();
  QCOMPARE( offer.id, quint64( 0 ) ); QVERIFY( !offer ); QVERIFY( !s.accepts( token ) );
  QVERIFY( !s.beginMimeAttempt() ); QCOMPARE( s.beginRead(), ReadToken( 0 ) );
  QVERIFY( !ChannelOffer{} ); QVERIFY( ChannelOffer{7} );
}
void TstWaylandSelection::activationPlanPromotesPendingCapture()
{
  QCOMPARE( planActivation( true ), ActivationPlan::AwaitPending );
  QCOMPARE( planActivation( false ), ActivationPlan::UseCached );
  qint64 ( *const clock )() noexcept = &Monotonic::nowMs;
  qint64 const first = clock(); qint64 const second = clock();
  QVERIFY( first > 0 ); QVERIFY( second >= first );
}
QTEST_GUILESS_MAIN( TstWaylandSelection )
#include "tst_waylandselection.moc"
