#include "popupdismiss.hh"
#include "support/fixtures.hh"
#include <QtTest>
#include <QApplication>
#include <QGuiApplication>
#include <QWindow>
#include <QWidget>
#include <QDialog>
#include <QSignalSpy>
#include <QPointer>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <type_traits>
#include <memory>

using Action = PopupDismiss::Action;
using Destination = PopupDismiss::FocusDestination;
static_assert( std::is_same_v< decltype( &PopupDismiss::onFocusMoved ),
  Action (*)( bool, bool, bool, Destination, bool ) > );
static_assert( std::is_same_v< decltype( &PopupDismiss::needsExplicitActivation ),
  bool (*)( Qt::WindowFlags ) > );
static_assert( std::is_same_v< decltype( &PopupDismiss::armedAfterShow ),
  bool (*)( QWindow const *, QWindow const * ) > );
static_assert( std::is_same_v< decltype( &PopupDismiss::isOwnedPopup ),
  bool (*)( QWidget const *, QWidget const * ) > );
static_assert( std::is_same_v< decltype( &PopupDismiss::isPopupWindowType ),
  bool (*)( Qt::WindowType ) > );

class TstPopupDismiss : public QObject {
  Q_OBJECT
private slots:
  void initTestCase()
  {
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ),
              qPrintable( reasons.join( '\n' ) ) );
    QString displayReason;
    QVERIFY2( TestSupport::prerequisiteAvailable( QStringLiteral( "display" ),
                                                &displayReason ),
              qPrintable( displayReason ) );
    TestSupport::installWatchdog( TestSupport::PureWatchdogSeconds );
  }
  void cleanupTestCase() { TestSupport::clearWatchdog(); }

  void focusMoveRule_data()
  {
    QTest::addColumn< bool >( "visible" );
    QTest::addColumn< bool >( "pinned" );
    QTest::addColumn< bool >( "armed" );
    QTest::addColumn< int >( "destination" );
    QTest::addColumn< bool >( "applicationActive" );
    QTest::addColumn< int >( "expected" );
    struct Row { char const * name; bool v, p, a; Destination d; bool active; Action expected; };
    Row const rows[] = {
      { "0-0-0-OwnWindow-0", 0,0,0, Destination::OwnWindow,0,Action::Ignore },
      { "0-0-0-OwnWindow-1", 0,0,0, Destination::OwnWindow,1,Action::Ignore },
      { "0-0-0-OwnPopup-0", 0,0,0, Destination::OwnPopup,0,Action::Ignore },
      { "0-0-0-OwnPopup-1", 0,0,0, Destination::OwnPopup,1,Action::Ignore },
      { "0-0-0-Foreign-0", 0,0,0, Destination::Foreign,0,Action::Ignore },
      { "0-0-0-Foreign-1", 0,0,0, Destination::Foreign,1,Action::Ignore },
      { "0-0-0-None-0", 0,0,0, Destination::None,0,Action::Ignore },
      { "0-0-0-None-1", 0,0,0, Destination::None,1,Action::Ignore },
      { "0-0-1-OwnWindow-0", 0,0,1, Destination::OwnWindow,0,Action::Ignore },
      { "0-0-1-OwnWindow-1", 0,0,1, Destination::OwnWindow,1,Action::Ignore },
      { "0-0-1-OwnPopup-0", 0,0,1, Destination::OwnPopup,0,Action::Ignore },
      { "0-0-1-OwnPopup-1", 0,0,1, Destination::OwnPopup,1,Action::Ignore },
      { "0-0-1-Foreign-0", 0,0,1, Destination::Foreign,0,Action::Ignore },
      { "0-0-1-Foreign-1", 0,0,1, Destination::Foreign,1,Action::Ignore },
      { "0-0-1-None-0", 0,0,1, Destination::None,0,Action::Ignore },
      { "0-0-1-None-1", 0,0,1, Destination::None,1,Action::Ignore },
      { "0-1-0-OwnWindow-0", 0,1,0, Destination::OwnWindow,0,Action::Ignore },
      { "0-1-0-OwnWindow-1", 0,1,0, Destination::OwnWindow,1,Action::Ignore },
      { "0-1-0-OwnPopup-0", 0,1,0, Destination::OwnPopup,0,Action::Ignore },
      { "0-1-0-OwnPopup-1", 0,1,0, Destination::OwnPopup,1,Action::Ignore },
      { "0-1-0-Foreign-0", 0,1,0, Destination::Foreign,0,Action::Ignore },
      { "0-1-0-Foreign-1", 0,1,0, Destination::Foreign,1,Action::Ignore },
      { "0-1-0-None-0", 0,1,0, Destination::None,0,Action::Ignore },
      { "0-1-0-None-1", 0,1,0, Destination::None,1,Action::Ignore },
      { "0-1-1-OwnWindow-0", 0,1,1, Destination::OwnWindow,0,Action::Ignore },
      { "0-1-1-OwnWindow-1", 0,1,1, Destination::OwnWindow,1,Action::Ignore },
      { "0-1-1-OwnPopup-0", 0,1,1, Destination::OwnPopup,0,Action::Ignore },
      { "0-1-1-OwnPopup-1", 0,1,1, Destination::OwnPopup,1,Action::Ignore },
      { "0-1-1-Foreign-0", 0,1,1, Destination::Foreign,0,Action::Ignore },
      { "0-1-1-Foreign-1", 0,1,1, Destination::Foreign,1,Action::Ignore },
      { "0-1-1-None-0", 0,1,1, Destination::None,0,Action::Ignore },
      { "0-1-1-None-1", 0,1,1, Destination::None,1,Action::Ignore },
      { "1-0-0-OwnWindow-0", 1,0,0, Destination::OwnWindow,0,Action::Ignore },
      { "1-0-0-OwnWindow-1", 1,0,0, Destination::OwnWindow,1,Action::Ignore },
      { "1-0-0-OwnPopup-0", 1,0,0, Destination::OwnPopup,0,Action::Ignore },
      { "1-0-0-OwnPopup-1", 1,0,0, Destination::OwnPopup,1,Action::Ignore },
      { "1-0-0-Foreign-0", 1,0,0, Destination::Foreign,0,Action::Ignore },
      { "1-0-0-Foreign-1", 1,0,0, Destination::Foreign,1,Action::Ignore },
      { "1-0-0-None-0", 1,0,0, Destination::None,0,Action::Ignore },
      { "1-0-0-None-1", 1,0,0, Destination::None,1,Action::Ignore },
      { "1-0-1-OwnWindow-0", 1,0,1, Destination::OwnWindow,0,Action::Ignore },
      { "1-0-1-OwnWindow-1", 1,0,1, Destination::OwnWindow,1,Action::Ignore },
      { "1-0-1-OwnPopup-0", 1,0,1, Destination::OwnPopup,0,Action::Ignore },
      { "1-0-1-OwnPopup-1", 1,0,1, Destination::OwnPopup,1,Action::Ignore },
      { "1-0-1-Foreign-0", 1,0,1, Destination::Foreign,0,Action::Hide },
      { "1-0-1-Foreign-1", 1,0,1, Destination::Foreign,1,Action::Hide },
      { "1-0-1-None-0", 1,0,1, Destination::None,0,Action::Hide },
      { "1-0-1-None-1", 1,0,1, Destination::None,1,Action::Ignore },
      { "1-1-0-OwnWindow-0", 1,1,0, Destination::OwnWindow,0,Action::Ignore },
      { "1-1-0-OwnWindow-1", 1,1,0, Destination::OwnWindow,1,Action::Ignore },
      { "1-1-0-OwnPopup-0", 1,1,0, Destination::OwnPopup,0,Action::Ignore },
      { "1-1-0-OwnPopup-1", 1,1,0, Destination::OwnPopup,1,Action::Ignore },
      { "1-1-0-Foreign-0", 1,1,0, Destination::Foreign,0,Action::Ignore },
      { "1-1-0-Foreign-1", 1,1,0, Destination::Foreign,1,Action::Ignore },
      { "1-1-0-None-0", 1,1,0, Destination::None,0,Action::Ignore },
      { "1-1-0-None-1", 1,1,0, Destination::None,1,Action::Ignore },
      { "1-1-1-OwnWindow-0", 1,1,1, Destination::OwnWindow,0,Action::Ignore },
      { "1-1-1-OwnWindow-1", 1,1,1, Destination::OwnWindow,1,Action::Ignore },
      { "1-1-1-OwnPopup-0", 1,1,1, Destination::OwnPopup,0,Action::Ignore },
      { "1-1-1-OwnPopup-1", 1,1,1, Destination::OwnPopup,1,Action::Ignore },
      { "1-1-1-Foreign-0", 1,1,1, Destination::Foreign,0,Action::Ignore },
      { "1-1-1-Foreign-1", 1,1,1, Destination::Foreign,1,Action::Ignore },
      { "1-1-1-None-0", 1,1,1, Destination::None,0,Action::Ignore },
      { "1-1-1-None-1", 1,1,1, Destination::None,1,Action::Ignore },
    };
    static_assert( sizeof( rows ) / sizeof( rows[0] ) == 64 );
    for( auto const & r : rows )
      QTest::newRow( r.name ) << r.v << r.p << r.a << int( r.d ) << r.active << int( r.expected );
  }
  void focusMoveRule()
  {
    QFETCH( bool, visible ); QFETCH( bool, pinned ); QFETCH( bool, armed );
    QFETCH( int, destination ); QFETCH( bool, applicationActive ); QFETCH( int, expected );
    QCOMPARE( int( PopupDismiss::onFocusMoved( visible, pinned, armed,
                   Destination( destination ), applicationActive ) ), expected );
  }
  void explicitActivationRule_data()
  {
    QTest::addColumn< Qt::WindowFlags >( "flags" ); QTest::addColumn< bool >( "expected" );
    QTest::newRow( "popup" ) << Qt::WindowFlags( Qt::Popup ) << false;
    QTest::newRow( "popup-frameless" ) << ( Qt::Popup | Qt::FramelessWindowHint ) << false;
    QTest::newRow( "popup-stays-on-top" ) << ( Qt::Popup | Qt::WindowStaysOnTopHint ) << false;
    QTest::newRow( "popup-both-hints" ) << ( Qt::Popup | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint ) << false;
    QTest::newRow( "tool" ) << Qt::WindowFlags( Qt::Tool ) << true;
    QTest::newRow( "managed-tool" ) << ( Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint ) << true;
    QTest::newRow( "window" ) << Qt::WindowFlags( Qt::Window ) << true;
    QTest::newRow( "dialog" ) << Qt::WindowFlags( Qt::Dialog ) << true;
    QTest::newRow( "tool-bypass" ) << ( Qt::Tool | Qt::X11BypassWindowManagerHint ) << true;
  }
  void explicitActivationRule()
  {
    QFETCH( Qt::WindowFlags, flags ); QFETCH( bool, expected );
    QCOMPARE( PopupDismiss::needsExplicitActivation( flags ), expected );
  }
  void ownedPopupRule_data()
  {
    QTest::addColumn< QString >( "graph" ); QTest::addColumn< bool >( "expected" );
    struct Row { char const * graph; bool expected; };
    Row const rows[] = { { "both-null",false }, { "null-owner",false },
      { "null-candidate",false }, { "self",false }, { "popup-self",false },
      { "child-popup",true }, { "grandchild-popup",true }, { "nested-popup",true },
      { "sibling",false }, { "unrelated-top-level",false }, { "other-window-menu",false } };
    for( auto const & r : rows ) QTest::newRow( r.graph ) << QString::fromLatin1( r.graph ) << r.expected;
  }
  void ownedPopupRule()
  {
    QFETCH( QString, graph ); QFETCH( bool, expected );
    QWidget common, other;
    QWidget owner( &common ), sibling( &common ), child( &owner );
    QWidget popupOwner( nullptr, Qt::Popup );
    QWidget direct( &owner, Qt::Popup ), grandchild( &child, Qt::Popup );
    QWidget nested( &direct, Qt::Popup ), foreign( &other, Qt::Popup );
    QCOMPARE( owner.parentWidget(), &common ); QCOMPARE( sibling.parentWidget(), &common );
    QCOMPARE( direct.parentWidget(), &owner ); QCOMPARE( child.parentWidget(), &owner );
    QCOMPARE( grandchild.parentWidget(), &child ); QCOMPARE( nested.parentWidget(), &direct );
    QCOMPARE( foreign.parentWidget(), &other );
    QWidget const * a = &owner; QWidget const * b = nullptr;
    if( graph == "both-null" ) a = nullptr;
    else if( graph == "null-owner" ) { a = nullptr; b = &direct; }
    else if( graph == "null-candidate" ) b = nullptr;
    else if( graph == "self" ) b = &owner;
    else if( graph == "popup-self" ) { a = &popupOwner; b = &popupOwner; }
    else if( graph == "child-popup" ) b = &direct;
    else if( graph == "grandchild-popup" ) b = &grandchild;
    else if( graph == "nested-popup" ) b = &nested;
    else if( graph == "sibling" ) b = &sibling;
    else if( graph == "unrelated-top-level" ) b = &other;
    else if( graph == "other-window-menu" ) b = &foreign;
    else QFAIL( "Unknown ownership row" );
    if( graph != "self" && graph != "popup-self" ) QVERIFY( !a || a != b );
    QCOMPARE( PopupDismiss::isOwnedPopup( a, b ), expected );
  }
  void showArmingRule_data()
  {
    QTest::addColumn< int >( "identity" ); QTest::addColumn< bool >( "expected" );
    QTest::newRow( "same" ) << 0 << true;
    QTest::newRow( "different" ) << 1 << false;
    QTest::newRow( "null-own" ) << 2 << false;
    QTest::newRow( "null-focus" ) << 3 << false;
    QTest::newRow( "both-null" ) << 4 << false;
  }
  void showArmingRule()
  {
    QFETCH( int, identity ); QFETCH( bool, expected );
    QWindow A, B;
    QWindow const * const owners[] = { &A, &A, nullptr, &A, nullptr };
    QWindow const * const focused[] = { &A, &B, &A, nullptr, nullptr };
    QCOMPARE( PopupDismiss::armedAfterShow( owners[identity], focused[identity] ), expected );
  }
  void popupWindowTypeRule_data()
  {
    QTest::addColumn< int >( "type" ); QTest::addColumn< bool >( "expected" );
    QTest::newRow( "popup" ) << int( Qt::Popup ) << true;
    QTest::newRow( "tooltip" ) << int( Qt::ToolTip ) << false;
    QTest::newRow( "dialog" ) << int( Qt::Dialog ) << false;
    QTest::newRow( "tool" ) << int( Qt::Tool ) << false;
    QTest::newRow( "window" ) << int( Qt::Window ) << false;
  }
  void popupWindowTypeRule()
  {
    QFETCH( int, type ); QFETCH( bool, expected );
    QCOMPARE( PopupDismiss::isPopupWindowType( Qt::WindowType( type ) ), expected );
  }
};
QTEST_MAIN( TstPopupDismiss )
#include "tst_popup_dismiss.moc"
