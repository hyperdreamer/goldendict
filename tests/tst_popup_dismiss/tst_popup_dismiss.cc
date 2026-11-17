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
#include <QFileInfo>
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

using Closer = PopupDismiss::FocusLossCloser;
static_assert( std::is_constructible_v< Closer, QWidget * > );
static_assert( std::is_constructible_v< Closer, QWidget *, QObject * > );
static_assert( std::is_same_v< decltype( &Closer::setPinned ), void (Closer::*)( bool ) > );
static_assert( std::is_same_v< decltype( &Closer::isPinned ), bool (Closer::*)() const > );
static_assert( std::is_same_v< decltype( &Closer::setDismissHandler ),
                             void (Closer::*)( std::function< void() > ) > );

namespace {
Qt::WindowFlags managedFlags()
{
  return Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
}
bool expectFocus( QWidget & widget )
{
  bool success = false;
  [&] {
    QVERIFY2( widget.isVisible(), "Focus destination must be visible" );
    QVERIFY2( widget.windowHandle(), "Focus destination must have a native handle" );
    QTRY_COMPARE( QGuiApplication::focusWindow(), widget.windowHandle() );
    success = true;
  }();
  return success;
}
bool activate( QWidget & widget )
{
  widget.show(); widget.activateWindow(); widget.raise();
  return expectFocus( widget );
}
struct Windows {
  QWidget first{ nullptr, managedFlags() };
  QWidget second, third;
  int count = 0;
  std::unique_ptr< Closer > closer;
  Windows() : closer( std::make_unique< Closer >( &first ) )
  {
    closer->setDismissHandler( [this] { ++count; first.hide(); } );
  }
};
struct ShowProbe : QObject {
  QWindow * ownAtShow = nullptr;
  QWindow * focusAtShow = nullptr;
  int spyCountAtShow = -1;
  QSignalSpy * spy = nullptr;
  bool eventFilter( QObject * watched, QEvent * event ) override
  {
    if( event->type() == QEvent::Show )
    {
      ownAtShow = static_cast< QWidget * >( watched )->windowHandle();
      focusAtShow = QGuiApplication::focusWindow();
      spyCountAtShow = spy->size();
    }
    return QObject::eventFilter( watched, event );
  }
};
QString source( char const * path )
{
  QString result;
  [&] {
    QString const root = TestSupport::repositoryRoot();
    QVERIFY2( !root.isEmpty(), "Cannot resolve real repository root" );
    QFile file( QDir( root ).filePath( QLatin1String( path ) ) );
    QVERIFY2( file.open( QIODevice::ReadOnly ), qPrintable( file.errorString() ) );
    result = QString::fromUtf8( file.readAll() );
    QVERIFY2( !result.isEmpty(), path );
  }();
  return result;
}
// Remove preprocessor directive lines before C++ normalisation. A directive
// begins at the first non-whitespace character of a physical line, outside a
// comment or literal, and every backslash-continued physical line belongs to
// it. Removed lines become a single newline each, so both sides of conditional
// compilation survive and no directive token joins the following statement.
QString stripPreprocessorDirectives( QString const & text )
{
  QString result, rawClose;
  bool block = false, lineComment = false, continuedDirective = false;
  QChar quote;
  for( qsizetype begin = 0; begin < text.size(); )
  {
    qsizetype const newline = text.indexOf( '\n', begin );
    qsizetype const end = newline < 0 ? text.size() : newline;
    QString const line = text.mid( begin, end - begin );
    QString physical = line;
    if( physical.endsWith( '\r' ) ) physical.chop( 1 );
    qsizetype nonspace = 0;
    while( nonspace < physical.size() && physical[nonspace].isSpace() ) ++nonspace;
    bool const directive = continuedDirective ||
      ( !block && !lineComment && quote.isNull() && rawClose.isEmpty() &&
        nonspace < physical.size() && physical[nonspace] == '#' );
    bool const continuedLine = physical.endsWith( '\\' );
    for( qsizetype i = 0; i < physical.size(); ++i )
    {
      QChar const c = physical[i];
      QChar const next = i + 1 < physical.size() ? physical[i+1] : QChar{};
      if( lineComment ) break;
      if( block )
      {
        if( c == '*' && next == '/' ) { block = false; ++i; }
        continue;
      }
      if( !rawClose.isEmpty() )
      {
        if( physical.mid( i, rawClose.size() ) == rawClose )
        { i += rawClose.size() - 1; rawClose.clear(); }
        continue;
      }
      if( !quote.isNull() )
      {
        if( c == '\\' ) ++i;
        else if( c == quote ) quote = QChar{};
        continue;
      }
      if( c == '/' && next == '/' ) { lineComment = true; ++i; continue; }
      if( c == '/' && next == '*' ) { block = true; ++i; continue; }
      if( c == 'R' && next == '"' )
      {
        qsizetype const open = physical.indexOf( '(', i + 2 );
        if( open >= i + 2 && open - i - 2 <= 16 )
        { rawClose = ')' + physical.mid( i + 2, open - i - 2 ) + '"'; i = open; continue; }
      }
      if( c == '"' || c == '\'' ) quote = c;
    }
    if( directive ) result += '\n';
    else { result += line; if( newline >= 0 ) result += '\n'; }
    continuedDirective = directive && continuedLine;
    if( !continuedLine ) lineComment = false;
    begin = newline < 0 ? text.size() : newline + 1;
  }
  return result;
}
// qmake scopes use their own syntax; C++ callers go through selectionBlock().
QString selectionRawBlock( QString const & text, QString const & anchor )
{
  qsizetype const at = text.indexOf( anchor );
  if( at < 0 ) return {};
  qsizetype const first = anchor.endsWith( '{' ) ? at + anchor.size() - 1
                                                : text.indexOf( '{', at + anchor.size() );
  if( first < 0 ) return {};
  int depth = 0;
  QChar quote;
  bool line = false, block = false, escape = false, lineStart = true;
  for( qsizetype i = first; i < text.size(); ++i )
  {
    QChar const c = text[i];
    QChar const next = i + 1 < text.size() ? text[i+1] : QChar{};
    if( line ) { if( c == '\n' ) { line = false; lineStart = true; } continue; }
    if( block ) { if( c == '*' && next == '/' ) { block = false; ++i; } continue; }
    if( !quote.isNull() )
    {
      if( escape ) escape = false;
      else if( c == '\\' ) escape = true;
      else if( c == quote ) quote = QChar{};
      continue;
    }
    if( c == '\\' ) { ++i; continue; }
    if( c == '#' && lineStart ) { line = true; continue; }
    if( c == '/' && next == '/' ) { line = true; ++i; continue; }
    if( c == '/' && next == '*' && text.indexOf( QStringLiteral( "*/" ), i + 2 ) >= 0 )
    { block = true; ++i; continue; }
    if( c == '"' || c == '\'' ) { quote = c; continue; }
    if( c == '{' ) { ++depth; lineStart = false; }
    else if( c == '}' && --depth == 0 ) return text.mid( first + 1, i - first - 1 );
    else if( c == '\n' ) lineStart = true;
    else if( c != ' ' && c != '\t' && c != '\r' ) lineStart = false;
  }
  return {};
}
// Tokenize the C++ fragments under review without changing string contents or
// token boundaries. Only braces around one simple controlled statement are
// optional: nested control flow and multi-statement blocks keep their scopes.
QStringList selectionTokens( QString const & text )
{
  // The identical pattern tst_hotkeys uses, written without a raw string: moc's
  // own lexer does not track raw-string delimiters and would lose the class
  // that follows any unbalanced quote in the pattern.
  QRegularExpression const lexer( QStringLiteral(
    "\"(?:\\\\.|[^\"\\\\])*\"|'(?:\\\\.|[^'\\\\])*'|//[^\\n]*"
    "|/\\*[\\s\\S]*?\\*/|[A-Za-z_][A-Za-z_0-9]*|[0-9]+|::|->|\\+\\+|--"
    "|&&|\\|\\||==|!=|<=|>=|\\+=|-=|<<|>>|\\S" ) );
  QStringList tokens;
  auto matches = lexer.globalMatch( text );
  while( matches.hasNext() )
  {
    QString const token = matches.next().captured();
    if( !token.startsWith( "//" ) && !token.startsWith( "/*" ) ) tokens.append( token );
  }
  return tokens;
}
QString selectionCode( QString const & text )
{
  QStringList tokens = selectionTokens( stripPreprocessorDirectives( text ) );
  for( qsizetype i = 0; i < tokens.size(); ++i )
  {
    qsizetype body = i + 1;
    if( tokens[i] == "if" || tokens[i] == "for" || tokens[i] == "while" )
    {
      if( body == tokens.size() || tokens[body] != "(" ) continue;
      int depth = 0;
      do
      {
        if( tokens[body] == "(" ) ++depth;
        else if( tokens[body] == ")" ) --depth;
        ++body;
      } while( body < tokens.size() && depth > 0 );
      if( depth != 0 ) continue;
    }
    else if( tokens[i] != "else" ) continue;
    if( body + 2 >= tokens.size() || tokens[body] != "{" ) continue;
    QString const first = tokens[body + 1];
    if( first == "if" || first == "for" || first == "while" || first == "switch" || first == "do" ) continue;
    qsizetype end = body + 1;
    int parentheses = 0, semicolons = 0;
    for( ; end < tokens.size() && tokens[end] != "{" && tokens[end] != "}"; ++end )
    {
      if( tokens[end] == "(" ) ++parentheses;
      else if( tokens[end] == ")" ) --parentheses;
      else if( tokens[end] == ";" && parentheses == 0 ) ++semicolons;
    }
    if( end < tokens.size() && tokens[end] == "}" && tokens[end - 1] == ";" &&
        parentheses == 0 && semicolons == 1 )
    {
      tokens.removeAt( end );
      tokens.removeAt( body );
    }
  }
  return tokens.join( ' ' );
}
qsizetype selectionFindTokens( QStringList const & tokens, QStringList const & wanted,
                              qsizetype from = 0 )
{
  if( wanted.isEmpty() ) return -1;
  for( qsizetype i = from; i + wanted.size() <= tokens.size(); ++i )
    if( tokens.mid( i, wanted.size() ) == wanted ) return i;
  return -1;
}
// Every C++ anchor and comparison uses the same lexer. Literal contents and
// token boundaries survive normalization; comments and syntax spacing do not.
bool selectionHas( QString const & text, QString const & needle )
{
  return selectionFindTokens( selectionTokens( selectionCode( text ) ),
                              selectionTokens( selectionCode( needle ) ) ) >= 0;
}
int selectionCount( QString const & text, QString const & needle )
{
  auto const tokens = selectionTokens( selectionCode( text ) );
  auto const wanted = selectionTokens( selectionCode( needle ) );
  int count = 0;
  for( qsizetype at = 0; ( at = selectionFindTokens( tokens, wanted, at ) ) >= 0;
       at += wanted.size() ) ++count;
  return count;
}
QString selectionBlock( QString const & text, QString const & anchor )
{
  auto const tokens = selectionTokens( selectionCode( text ) );
  auto const wanted = selectionTokens( selectionCode( anchor ) );
  qsizetype const at = selectionFindTokens( tokens, wanted );
  if( at < 0 ) return {};
  qsizetype open = wanted.last() == "{" ? at + wanted.size() - 1 : at + wanted.size();
  while( open < tokens.size() && tokens[open] != "{" ) ++open;
  if( open >= tokens.size() ) return {};
  int depth = 0;
  for( qsizetype end = open; end < tokens.size(); ++end )
  {
    if( tokens[end] == "{" ) ++depth;
    else if( tokens[end] == "}" && --depth == 0 )
      return tokens.mid( open + 1, end - open - 1 ).join( ' ' );
  }
  return {};
}

// Split only at statement boundaries, keeping nested controls, initializer lists
// and lambda bodies intact. This lets the contracts inspect direct actions and
// exits separately from the bodies of guarded branches.
qsizetype selectionStatementEnd( QStringList const & tokens, qsizetype at )
{
  if( at >= tokens.size() ) return at;
  auto balanced = [&]( qsizetype begin, QString const & open, QString const & close ) {
    int depth = 0;
    for( qsizetype i = begin; i < tokens.size(); ++i )
    {
      if( tokens[i] == open ) ++depth;
      else if( tokens[i] == close && --depth == 0 ) return i + 1;
    }
    return tokens.size();
  };
  if( tokens[at] == "{" ) return balanced( at, "{", "}" );
  if( tokens[at] == "else" ) return selectionStatementEnd( tokens, at + 1 );
  if( tokens[at] == "if" || tokens[at] == "for" || tokens[at] == "while" || tokens[at] == "switch" )
  {
    if( at + 1 >= tokens.size() || tokens[at + 1] != "(" ) return tokens.size();
    qsizetype end = selectionStatementEnd( tokens, balanced( at + 1, "(", ")" ) );
    if( tokens[at] == "if" && end < tokens.size() && tokens[end] == "else" )
      end = selectionStatementEnd( tokens, end + 1 );
    return end;
  }
  for( qsizetype i = at; i < tokens.size(); ++i )
  {
    if( tokens[i] == "(" ) i = balanced( i, "(", ")" ) - 1;
    else if( tokens[i] == "[" ) i = balanced( i, "[", "]" ) - 1;
    else if( tokens[i] == "{" ) i = balanced( i, "{", "}" ) - 1;
    else if( tokens[i] == ";" ) return i + 1;
  }
  return tokens.size();
}
QStringList selectionStatements( QString const & text )
{
  QStringList const tokens = selectionTokens( selectionCode( text ) );
  QStringList statements;
  for( qsizetype at = 0; at < tokens.size(); )
  {
    qsizetype const end = selectionStatementEnd( tokens, at );
    statements.append( tokens.mid( at, end - at ).join( ' ' ) );
    at = end;
  }
  return statements;
}
QString selectionDirectActions( QString const & text )
{
  QStringList actions;
  for( QString const & statement : selectionStatements( text ) )
    if( !statement.startsWith( "if " ) && !statement.startsWith( "else " ) &&
        !statement.startsWith( "for " ) && !statement.startsWith( "while " ) &&
        !statement.startsWith( "switch " ) && !statement.startsWith( "{ " ) )
      actions.append( statement );
  return actions.join( ' ' );
}
// A branch must occur at the current scope, with its action immediately after
// the condition. A matching action buried in a lambda cannot satisfy it.
QString selectionBranch( QString const & text, QString const & condition )
{
  QString const prefix = selectionCode( condition ) + ' ';
  for( QString const & statement : selectionStatements( text ) )
  {
    if( !statement.startsWith( prefix ) ) continue;
    QString const action = statement.mid( prefix.size() );
    if( action.startsWith( "{ " ) ) return selectionRawBlock( action, "{" );
    return action.left( action.indexOf( ';' ) + 1 );
  }
  return {};
}
bool selectionOrdered( QString const & text, QStringList const & needles )
{
  auto const tokens = selectionTokens( selectionCode( text ) );
  qsizetype after = 0;
  for( auto const & needle : needles )
  {
    auto const wanted = selectionTokens( selectionCode( needle ) );
    qsizetype const at = selectionFindTokens( tokens, wanted, after );
    if( at < 0 ) return false;
    after = at + wanted.size();
  }
  return true;
}
// Partition one top-level if/else-if/else chain into its arms, so a preserved
// branch is found at its own scope instead of by searching nested code.
struct Arm { QString guard, body; };
QList< Arm > conditionalArms( QString const & chain )
{
  QStringList const tokens = selectionTokens( selectionCode( chain ) );
  QList< Arm > arms;
  qsizetype at = 0;
  while( at < tokens.size() )
  {
    bool const finalElse = tokens[at] == "else" &&
                           at + 1 < tokens.size() && tokens[at+1] != "if";
    if( tokens[at] == "else" ) ++at;
    if( finalElse )
    {
      qsizetype const end = selectionStatementEnd( tokens, at );
      if( end != tokens.size() || end <= at ) return {};
      QString body = tokens.mid( at, end-at ).join( ' ' );
      if( body.startsWith( "{ " ) ) body = selectionRawBlock( body, "{" );
      arms.append( { "else", body } );
      return arms;
    }
    if( at >= tokens.size() || tokens[at] != "if" ||
        at + 1 >= tokens.size() || tokens[at+1] != "(" ) return {};
    qsizetype conditionEnd = at + 1;
    int depth = 0;
    do {
      if( tokens[conditionEnd] == "(" ) ++depth;
      else if( tokens[conditionEnd] == ")" ) --depth;
      ++conditionEnd;
    } while( conditionEnd < tokens.size() && depth );
    if( depth || conditionEnd >= tokens.size() ) return {};
    qsizetype const end = selectionStatementEnd( tokens, conditionEnd );
    if( end <= conditionEnd ) return {};
    QString body = tokens.mid( conditionEnd, end - conditionEnd ).join( ' ' );
    if( body.startsWith( "{ " ) ) body = selectionRawBlock( body, "{" );
    arms.append( { tokens.mid( at, conditionEnd-at ).join( ' ' ), body } );
    at = end;
  }
  return arms;
}
// An extracted scope is a precondition, never an optional check: a missing
// anchor fails the case instead of returning an empty successful comparison.
QString requiredBlock( QString const & text, QString const & anchor )
{
  QString const body = selectionBlock( text, anchor );
  [&] { QVERIFY2( !body.isEmpty(), qPrintable( "Missing scope: " + anchor ) ); }();
  return body;
}
QString requiredBranch( QString const & text, QString const & condition )
{
  QString const body = selectionBranch( text, condition );
  [&] { QVERIFY2( !body.isEmpty(), qPrintable( "Missing branch: " + condition ) ); }();
  return body;
}
// Locate a brace scope by its real code tokens, then return the untouched source
// between the braces. Offsets come from the token stream, so a comment or a
// string that merely contains the anchor cannot supply an executable scope, and
// preprocessor directives inside the scope survive for the directive tests.
QString rawCodeBlock( QString const & text, QString const & anchor )
{
  QRegularExpression const lexer( QStringLiteral(
    "\"(?:\\\\.|[^\"\\\\])*\"|'(?:\\\\.|[^'\\\\])*'|//[^\\n]*"
    "|/\\*[\\s\\S]*?\\*/|[A-Za-z_][A-Za-z_0-9]*|[0-9]+|::|->|\\+\\+|--"
    "|&&|\\|\\||==|!=|<=|>=|\\+=|-=|<<|>>|\\S" ) );
  QStringList tokens; QList< qsizetype > starts, ends;
  auto matches = lexer.globalMatch( text );
  while( matches.hasNext() )
  {
    auto const match = matches.next(); QString const token = match.captured();
    if( token.startsWith( "//" ) || token.startsWith( "/*" ) ) continue;
    tokens.append( token ); starts.append( match.capturedStart() ); ends.append( match.capturedEnd() );
  }
  QStringList const wanted = selectionTokens( selectionCode( anchor ) );
  if( wanted.isEmpty() ) return {};
  for( qsizetype i = 0; i + wanted.size() <= tokens.size(); ++i )
    if( tokens.mid( i, wanted.size() ) == wanted )
    {
      qsizetype open = i + wanted.size();
      while( open < tokens.size() && tokens[open] != "{" ) ++open;
      if( wanted.last() == "{" ) open = i + wanted.size() - 1;
      if( open >= tokens.size() ) return {};
      int depth = 0;
      for( qsizetype close = open; close < tokens.size(); ++close )
      {
        if( tokens[close] == "{" ) ++depth;
        else if( tokens[close] == "}" && --depth == 0 )
          return text.mid( ends[open], starts[close]-ends[open] );
      }
      return {};
    }
  return {};
}
// Return the physical lines nested inside one actual conditional directive,
// without its opening and closing directive lines.
QString directiveRegion( QString const & raw, QString const & opening )
{
  auto const physical = raw.split( '\n' );
  auto const stripped = stripPreprocessorDirectives( raw ).split( '\n' );
  if( physical.size() != stripped.size() ) return {};
  int depth = 0; bool found = false; QString result;
  for( qsizetype i = 0; i < physical.size(); ++i )
  {
    QString const line = physical[i].trimmed();
    bool const directive = line.startsWith( '#' ) && stripped[i].isEmpty();
    if( !found )
    {
      if( directive && line == opening ) { found = true; depth = 1; }
      continue;
    }
    if( directive )
    {
      if( QRegularExpression( "^#\\s*(if|ifdef|ifndef)\\b" ).match( line ).hasMatch() ) ++depth;
      else if( QRegularExpression( "^#\\s*endif\\b" ).match( line ).hasMatch() && --depth == 0 ) return result;
    }
    result += physical[i] + '\n';
  }
  return {}; // Missing or unmatched actual directive is a test failure at the caller.
}
bool hasInclude( QString const & raw, QString const & token )
{
  auto const tokens = selectionTokens( raw );
  auto const wanted = selectionTokens( "#include " + token );
  for( qsizetype i = 0; i + wanted.size() <= tokens.size(); ++i )
    if( tokens.mid( i, wanted.size() ) == wanted ) return true;
  return false;
}
// The build files are qmake, not C++: these helpers strip only real qmake
// comments, join physical continuations, and keep scope depth outside quoted
// strings. They never evaluate qmake and never apply the C++ normaliser.
QString qmakeUncomment( QString const & text )
{
  QString result; QChar quote; bool escaped = false, comment = false;
  for( QChar c : text )
  {
    if( c == '\n' ) { result += c; comment = false; escaped = false; continue; }
    if( comment ) continue;
    if( escaped ) { result += c; escaped = false; continue; }
    if( c == '\\' ) { result += c; escaped = true; continue; }
    if( !quote.isNull() ) { result += c; if( c == quote ) quote = QChar{}; continue; }
    if( c == '"' || c == '\'' ) { quote = c; result += c; continue; }
    if( c == '#' ) { comment = true; continue; }
    result += c;
  }
  return result;
}
QStringList qmakeLines( QString const & text )
{
  QStringList result; QString pending;
  for( QString line : qmakeUncomment( text ).split( '\n' ) )
  {
    line = line.trimmed();
    bool const continued = line.endsWith( '\\' );
    if( continued ) line.chop( 1 );
    pending += line + ' ';
    if( !continued ) { if( !pending.trimmed().isEmpty() ) result.append( pending.trimmed() ); pending.clear(); }
  }
  if( !pending.trimmed().isEmpty() ) result.append( pending.trimmed() );
  return result;
}
struct QmakeAssignment { int depth; QStringList values; };
QList< QmakeAssignment > qmakeAssignments( QString const & text, QString const & variable )
{
  QRegularExpression const assign( "^" + QRegularExpression::escape( variable ) + "\\s*\\+?=\\s*(.*)$" );
  QList< QmakeAssignment > result; int depth = 0;
  for( QString const & line : qmakeLines( text ) )
  {
    auto const match = assign.match( line );
    if( match.hasMatch() ) result.append( { depth, match.captured( 1 ).split( QRegularExpression( "\\s+" ), Qt::SkipEmptyParts ) } );
    QChar quote; bool escaped = false;
    for( QChar c : line )
    {
      if( escaped ) { escaped = false; continue; }
      if( c == '\\' ) { escaped = true; continue; }
      if( !quote.isNull() ) { if( c == quote ) quote = QChar{}; continue; }
      if( c == '"' || c == '\'' ) { quote = c; continue; }
      if( c == '{' ) ++depth;
      else if( c == '}' ) --depth;
    }
    if( depth < 0 ) return {};
  }
  return depth == 0 ? result : QList< QmakeAssignment >{};
}
} // namespace

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

  void hidesWhenFocusMovesToAnotherWindow()
  {
    Windows f;
    QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    QTRY_COMPARE( f.count, 1 ); QVERIFY( !f.first.isVisible() );
    QCoreApplication::processEvents(); QCOMPARE( f.count, 1 );
  }
  void pinnedPopupDoesNotHide()
  {
    Windows f; f.closer->setPinned( true ); QVERIFY( f.closer->isPinned() );
    QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
  }
  void ownedMenuKeepsPopupUntilFocusMovesAway()
  {
    Windows f; QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QWidget menu( &f.first ); menu.setWindowFlags( Qt::Popup );
    QCOMPARE( menu.parentWidget(), &f.first );
    QVERIFY( activate( menu ) );
    QCOMPARE( QApplication::activePopupWidget(), &menu );
    QVERIFY( menu.windowHandle() ); QCOMPARE( menu.windowHandle()->type(), Qt::Popup );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
    QSignalSpy path( qApp, &QGuiApplication::focusWindowChanged ); QVERIFY( path.isValid() );
    menu.close();
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    QTRY_COMPARE( f.count, 1 ); QVERIFY( !f.first.isVisible() );
    for( auto const & payload : path )
      qInfo() << "menu-to-foreign focus payload" << qvariant_cast< QWindow * >( payload[0] );
    QVERIFY( !path.isEmpty() );
  }
  void nestedOwnedMenuKeepsPopupUntilFocusMovesAway()
  {
    Windows f; QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QWidget menu( &f.first ); menu.setWindowFlags( Qt::Popup );
    QVERIFY( activate( menu ) );
    QCOMPARE( menu.parentWidget(), &f.first );
    QCOMPARE( QApplication::activePopupWidget(), &menu );
    QVERIFY( menu.windowHandle() ); QCOMPARE( menu.windowHandle()->type(), Qt::Popup );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
    QWidget submenu( &menu ); submenu.setWindowFlags( Qt::Popup );
    QCOMPARE( submenu.parentWidget(), &menu );
    QVERIFY( activate( submenu ) );
    QCOMPARE( QApplication::activePopupWidget(), &submenu );
    QVERIFY( submenu.windowHandle() ); QCOMPARE( submenu.windowHandle()->type(), Qt::Popup );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
    submenu.close(); menu.close();
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    QTRY_COMPARE( f.count, 1 ); QVERIFY( !f.first.isVisible() );
  }
  void ownDialogHidesPopupAndRemainsVisible()
  {
    Windows f;
    QVERIFY( activate( f.first ) );
    QCOMPARE( f.first.windowHandle()->type(), Qt::Tool );
    QVERIFY( f.first.windowFlags().testFlag( Qt::FramelessWindowHint ) );
    QVERIFY( f.first.windowFlags().testFlag( Qt::WindowStaysOnTopHint ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QDialog dialog( &f.first ); dialog.setModal( true ); dialog.show();
    QVERIFY( dialog.isModal() ); QCOMPARE( dialog.parentWidget(), &f.first );
    QTRY_COMPARE( QApplication::activeModalWidget(), &dialog );
    QVERIFY( expectFocus( dialog ) );
    QCOMPARE( dialog.windowHandle()->type(), Qt::Dialog );
    QTRY_COMPARE( f.count, 1 ); QVERIFY( !f.first.isVisible() ); QVERIFY( dialog.isVisible() );
  }
  void focusReturningFromMenuKeepsPopup()
  {
    Windows f; QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QWidget menu( &f.first ); menu.setWindowFlags( Qt::Popup );
    QCOMPARE( menu.parentWidget(), &f.first );
    QVERIFY( activate( menu ) );
    QCOMPARE( QApplication::activePopupWidget(), &menu );
    QVERIFY( menu.windowHandle() ); QCOMPARE( menu.windowHandle()->type(), Qt::Popup );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
    menu.close(); QVERIFY( activate( f.first ) );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
  }
  void applicationInactiveSignalUsesProductionConnection()
  {
    for( int mode = 0; mode != 3; ++mode )
    {
      Windows f;
      f.closer->setPinned( mode == 1 ); QCOMPARE( f.closer->isPinned(), mode == 1 );
      if( mode == 2 )
      {
        QVERIFY( activate( f.second ) );
        f.first.setAttribute( Qt::WA_ShowWithoutActivating ); f.first.show();
        QVERIFY( f.first.windowHandle() ); QVERIFY( f.first.isVisible() );
        QVERIFY( expectFocus( f.second ) );
      }
      else QVERIFY( activate( f.first ) );
      QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
      QVERIFY( QMetaObject::invokeMethod(
        qApp, "applicationStateChanged",
        Q_ARG( Qt::ApplicationState, Qt::ApplicationInactive ) ) );
      QCOMPARE( f.count, mode == 0 ? 1 : 0 );
      QCOMPARE( f.first.isVisible(), mode != 0 );
    }
  }
  void reshowWithoutFocusStaysUnarmed()
  {
    Windows f; QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    f.first.hide(); QVERIFY( !f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    f.first.setAttribute( Qt::WA_ShowWithoutActivating ); f.first.show();
    QVERIFY( f.first.isVisible() ); QVERIFY( f.first.windowHandle() );
    QVERIFY( expectFocus( f.second ) ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.third ) );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
  }
  void showWhileAlreadyFocusedArmsPopup()
  {
    QWidget first( nullptr, managedFlags() ), second;
    QVERIFY( activate( first ) );
    Closer closer( &first ); int count = 0;
    closer.setDismissHandler( [&] { ++count; first.hide(); } );
    QSignalSpy focusSignals( qApp, &QGuiApplication::focusWindowChanged ); QVERIFY( focusSignals.isValid() );
    ShowProbe probe; probe.spy = &focusSignals; first.installEventFilter( &probe );
    first.hide(); first.show();
    QVERIFY2( probe.ownAtShow, "Already-focused Show assumption: native handle disappeared" );
    QVERIFY2( probe.focusAtShow == probe.ownAtShow,
              "Already-focused Show assumption: QPA cleared own focus on Hide/Show" );
    QVERIFY( probe.spyCountAtShow >= 0 );
    QVERIFY( first.isVisible() ); QCOMPARE( count, 0 );
    for( auto const & payload : focusSignals )
      QVERIFY2( qvariant_cast< QWindow * >( payload[0] ) != probe.ownAtShow,
                "Already-focused Show assumption: an own-focus signal independently armed the closer" );
    QVERIFY( activate( second ) );
    for( auto const & payload : focusSignals )
      QVERIFY2( qvariant_cast< QWindow * >( payload[0] ) != probe.ownAtShow,
                "An own-focus signal armed the closer before the tested loss" );
    QTRY_COMPARE( count, 1 ); QVERIFY( !first.isVisible() );
    first.removeEventFilter( &probe );
  }
  void dismissHandlerIsNotReentered()
  {
    Windows f; bool inHandler = false, nestedForeign = false, driven = false;
    QObject observer;
    QObject::connect( qApp, &QGuiApplication::focusWindowChanged, &observer,
      [&]( QWindow * destination ) {
        if( inHandler && destination == f.second.windowHandle() ) nestedForeign = true;
      } );
    f.closer->setDismissHandler( [&] {
      ++f.count;
      if( f.count != 1 ) return; // A broken guard fails finitely.
      inHandler = true;
      driven = activate( f.first ) && activate( f.second ) && activate( f.first );
      inHandler = false;
    } );
    QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    f.second.show(); f.second.activateWindow(); f.second.raise();
    QTRY_COMPARE( f.count, 1 );
    QVERIFY( driven ); QVERIFY2( nestedForeign, "No eligible Foreign event delivered inside callback" );
    QVERIFY( expectFocus( f.first ) ); QCOMPARE( f.count, 1 );
  }
  void dismissHandlerMayDestroyCloser()
  {
    Windows f; QPointer< Closer > observed = f.closer.get();
    f.closer->setDismissHandler( [&] { ++f.count; f.closer.reset(); } );
    QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    QVERIFY( activate( f.second ) );
    QTRY_COMPARE( f.count, 1 ); QVERIFY( observed.isNull() );
    QVERIFY( !f.closer ); QVERIFY( f.first.isVisible() );
    QVERIFY( activate( f.first ) ); QVERIFY( activate( f.second ) ); QCOMPARE( f.count, 1 );
  }
  void destroyingArmedCloserDisconnectsSignals()
  {
    Windows f; QPointer< Closer > observed = f.closer.get();
    QVERIFY( activate( f.first ) );
    QVERIFY( f.first.isVisible() ); QCOMPARE( f.count, 0 );
    f.closer.reset(); QVERIFY( observed.isNull() ); QVERIFY( f.first.isVisible() );
    QVERIFY( activate( f.second ) ); QVERIFY( activate( f.first ) ); QVERIFY( activate( f.second ) );
    QVERIFY( QMetaObject::invokeMethod(
      qApp, "applicationStateChanged",
      Q_ARG( Qt::ApplicationState, Qt::ApplicationInactive ) ) );
    QCOMPARE( f.count, 0 ); QVERIFY( f.first.isVisible() );
  }

  void closerApplicationStateConnectionIsContextBound()
  {
    QString const directives = "#if A\n#define X \\\n one\n#endif\n#if B\r\n#define Y \\\r\n two\r\n#endif\r\n";
    QCOMPARE( stripPreprocessorDirectives( directives ), QString( 8, '\n' ) );
    QString const ordinary = "const char *s = \"#if literal\";\n// #if comment\n/*\n#if comment\n*/\nchar c = '#';\n";
    QVERIFY( !selectionHas( "auto s = \" activateWindow ( ) ; \";", "activateWindow();" ) );
    QVERIFY( !selectionOrdered( "auto s = \" activateWindow ( ) ; \";", { "activateWindow();" } ) );
    QVERIFY( selectionBlock( "auto s = \" FocusLossCloser :: dismiss { armed = false ; } \"",
                             "FocusLossCloser::dismiss" ).isEmpty() );
    QCOMPARE( stripPreprocessorDirectives( ordinary ), ordinary );
    QString const file = source( "popupdismiss.cc" ); QVERIFY( !file.isEmpty() );
    QString const ctor = requiredBlock( file, "FocusLossCloser::FocusLossCloser" ); QVERIFY( !ctor.isEmpty() );
    QVERIFY( selectionHas( ctor, "focusConnection = QObject::connect( qApp, &QGuiApplication::focusWindowChanged, this," ) );
    QVERIFY( selectionHas( ctor, "stateConnection = QObject::connect( qApp, &QGuiApplication::applicationStateChanged, this," ) );
    QString const state = requiredBlock( ctor, "[this]( Qt::ApplicationState state )" ); QVERIFY( !state.isEmpty() );
    QStringList const statements = selectionStatements( state ); QVERIFY( statements.size() >= 4 );
    QCOMPARE( statements[0], selectionCode( "if( state != Qt::ApplicationInactive ) return;" ) );
    QCOMPARE( statements[1], selectionCode( "if( !window ) return;" ) );
    QString const ignore = requiredBranch( state,
      "if( onFocusMoved( window->isVisible(), pinned, armed, FocusDestination::None, false ) == Action::Ignore )" );
    QVERIFY( !ignore.isEmpty() ); QVERIFY( selectionHas( ignore, "return;" ) );
    QVERIFY( !selectionHas( state, "applicationState(" ) );
    QVERIFY( !selectionHas( state, "focusWindow(" ) );
    QCOMPARE( selectionCount( state, "dismiss(" ), 1 );
    QVERIFY( selectionOrdered( selectionDirectActions( state ),
                              { "dismiss( \"application inactive\" );", "return;" } ) );
    QString const focus = requiredBlock( ctor, "[this]( QWindow * focusWindow )" ); QVERIFY( !focus.isEmpty() );
    auto const focusStatements = selectionStatements( focus ); QVERIFY( !focusStatements.isEmpty() );
    QCOMPARE( focusStatements[0], selectionCode( "if( !window ) return;" ) );
    auto const teardown = selectionStatements( requiredBlock( file, "FocusLossCloser::~FocusLossCloser" ) );
    QCOMPARE( teardown.size(), 3 );
    QCOMPARE( teardown[0], selectionCode( "QObject::disconnect( focusConnection );" ) );
    QCOMPARE( teardown[1], selectionCode( "QObject::disconnect( stateConnection );" ) );
    QCOMPARE( teardown[2], selectionCode( "if( window ) window->removeEventFilter( this );" ) );
    QCOMPARE( selectionStatements( requiredBlock( file, "FocusLossCloser::setPinned" ) ),
              QStringList{ selectionCode( "this->pinned = pinned;" ) } );
    QCOMPARE( selectionStatements( requiredBlock( file, "FocusLossCloser::isPinned" ) ),
              QStringList{ selectionCode( "return pinned;" ) } );
    QCOMPARE( selectionStatements( requiredBlock( file, "FocusLossCloser::setDismissHandler" ) ),
              QStringList{ selectionCode( "dismissHandler = std::move( handler );" ) } );
  }
  void closerShowAndHideMaintainArming()
  {
    QString const file = source( "popupdismiss.cc" ); QVERIFY( !file.isEmpty() );
    QString const filter = requiredBlock( file, "FocusLossCloser::eventFilter" ); QVERIFY( !filter.isEmpty() );
    QString const observed = requiredBranch( filter, "if( window && watched == window.data() )" ); QVERIFY( !observed.isEmpty() );
    auto const chain = selectionStatements( observed ); QCOMPARE( chain.size(), 1 );
    auto const arms = conditionalArms( chain.first() ); QCOMPARE( arms.size(), 2 );
    QCOMPARE( arms[0].guard, selectionCode( "if( event->type() == QEvent::Show )" ) );
    QCOMPARE( arms[1].guard, selectionCode( "if( event->type() == QEvent::Hide )" ) );
    QCOMPARE( selectionStatements( arms[0].body ), QStringList{ selectionCode(
      "armed = PopupDismiss::armedAfterShow( window->windowHandle(), QGuiApplication::focusWindow() );" ) } );
    QCOMPARE( selectionStatements( arms[1].body ), QStringList{ selectionCode( "armed = false;" ) } );
    QCOMPARE( selectionDirectActions( filter ), selectionCode( "return QObject::eventFilter( watched, event );" ) );
    QCOMPARE( selectionCount( filter, "return" ), 1 );
  }
  void destinationClassifierUsesOwnedPopupIdentityAndType()
  {
    QString const file = source( "popupdismiss.cc" ); QVERIFY( !file.isEmpty() );
    QString const body = requiredBlock( file, "FocusLossCloser::destinationOf" ); QVERIFY( !body.isEmpty() );
    QVERIFY( selectionOrdered( body, {
      "if( !window ) return FocusDestination::Foreign;",
      "if( !focusWindow ) return FocusDestination::None;",
      "if( focusWindow == window->windowHandle() ) return FocusDestination::OwnWindow;",
      "QWidget * const activePopup = QApplication::activePopupWidget();",
      "if( isOwnedPopup( window, activePopup ) )",
      "QWindow * const popupHandle = activePopup->windowHandle();",
      "if( popupHandle && focusWindow == popupHandle && isPopupWindowType( focusWindow->type() ) )",
      "return FocusDestination::OwnPopup;", "return FocusDestination::Foreign;" } ) );
    QString const owned = requiredBranch( body, "if( isOwnedPopup( window, activePopup ) )" ); QVERIFY( !owned.isEmpty() );
    auto const direct = selectionStatements( owned ); QCOMPARE( direct.size(), 2 );
    QCOMPARE( direct[0], selectionCode( "QWindow * const popupHandle = activePopup->windowHandle();" ) );
    QString const typed = requiredBranch( owned,
      "if( popupHandle && focusWindow == popupHandle && isPopupWindowType( focusWindow->type() ) )" );
    QVERIFY( !typed.isEmpty() );
    QCOMPARE( selectionStatements( typed ), QStringList{ selectionCode( "return FocusDestination::OwnPopup;" ) } );
    auto const statements = selectionStatements( body ); QVERIFY( !statements.isEmpty() );
    QCOMPARE( statements.last(), selectionCode( "return FocusDestination::Foreign;" ) );
    for( auto const & forbidden : { "transientParent(", "->parent(", "applicationState(", "isActiveWindow(", "winId(" } )
      QVERIFY2( !selectionHas( body, forbidden ), forbidden );
  }
  void ownPopupBranchPreservesArming()
  {
    QString const file = source( "popupdismiss.cc" ); QVERIFY( !file.isEmpty() );
    QString const ctor = requiredBlock( file, "FocusLossCloser::FocusLossCloser" ); QVERIFY( !ctor.isEmpty() );
    QString const focus = requiredBlock( ctor, "[this]( QWindow * focusWindow )" ); QVERIFY( !focus.isEmpty() );
    QString const popup = requiredBranch( focus, "if( destination == FocusDestination::OwnPopup )" ); QVERIFY( !popup.isEmpty() );
    QVERIFY( !selectionStatements( popup ).isEmpty() );
    for( QString const & token : selectionTokens( popup ) )
    {
      QVERIFY( token != "armed" );
      QVERIFY( !token.contains( "cancel", Qt::CaseInsensitive ) );
      QVERIFY( !token.contains( "clear", Qt::CaseInsensitive ) );
    }
    for( auto const & forbidden : { "dismiss(", "hide(", "show(", "setVisible(", "setHidden(" } )
      QVERIFY2( !selectionHas( popup, forbidden ), forbidden );
    QVERIFY( selectionHas( popup, "return;" ) );
  }
  void dismissInvokesFreshHandlerBehindSelfGuard()
  {
    QString const file = source( "popupdismiss.cc" ); QVERIFY( !file.isEmpty() );
    QString const body = requiredBlock( file, "FocusLossCloser::dismiss" ); QVERIFY( !body.isEmpty() );
    QStringList const statements = selectionStatements( body ); QVERIFY( statements.size() >= 8 );
    QCOMPARE( statements[0], selectionCode( "if( dismissing ) return;" ) );
    QCOMPARE( statements[1], selectionCode( "dismissing = true;" ) );
    QCOMPARE( statements[2], selectionCode( "QPointer< FocusLossCloser > self( this );" ) );
    QVERIFY( selectionOrdered( body, {
      "dismissing = true;", "QPointer< FocusLossCloser > self( this );",
      "auto handler = dismissHandler;", "if( handler ) handler();",
      "if( !self ) return;", "dismissing = false;", "armed = false;" } ) );
    QCOMPARE( selectionCount( body, "handler();" ), 1 );
    QCOMPARE( selectionCount( body, "dismissHandler" ), 1 );
    auto const tokens = selectionTokens( selectionCode( body ) );
    qsizetype const callAt = selectionFindTokens( tokens, selectionTokens( selectionCode( "handler();" ) ) );
    QVERIFY( callAt >= 0 );
    QString const after = tokens.mid( callAt + selectionTokens( selectionCode( "handler();" ) ).size() ).join( ' ' );
    auto const tail = selectionStatements( after ); QCOMPARE( tail.size(), 3 );
    QCOMPARE( tail[0], selectionCode( "if( !self ) return;" ) );
    QCOMPARE( tail[1], selectionCode( "dismissing = false;" ) );
    QCOMPARE( tail[2], selectionCode( "armed = false;" ) );
    QVERIFY( !selectionHas( after, "window" ) );
    QVERIFY( !selectionHas( body, "hide(" ) );
  }

  void scanPopupDeclaresCloserWithoutEventOverrides()
  {
    QString const header = source( "scanpopup.hh" ), file = source( "scanpopup.cc" );
    QVERIFY( !header.isEmpty() ); QVERIFY( !file.isEmpty() );
    QVERIFY( hasInclude( header, "\"popupdismiss.hh\"" ) );
    QVERIFY( hasInclude( header, "<memory>" ) );
    QString const declaration = requiredBlock( header, "class ScanPopup" ); QVERIFY( !declaration.isEmpty() );
    QVERIFY( selectionHas( declaration, "std::unique_ptr< PopupDismiss::FocusLossCloser > focusLossCloser;" ) );
    QVERIFY( !selectionHas( declaration, "event(" ) ); QVERIFY( !selectionHas( declaration, "changeEvent(" ) );
    QVERIFY( !selectionHas( file, "ScanPopup::event(" ) ); QVERIFY( !selectionHas( file, "ScanPopup::changeEvent(" ) );
    QVERIFY( selectionHas( declaration, "eventFilter(" ) );
    QVERIFY( selectionHas( file, "ScanPopup::eventFilter(" ) );
  }
  void constructorRestoresPinBeforeChangingFlags()
  {
    QString const file = source( "scanpopup.cc" ); QVERIFY( !file.isEmpty() );
    QString const ctor = requiredBlock( file, "ScanPopup::ScanPopup" ); QVERIFY( !ctor.isEmpty() );
    QString const setup = "ui.setupUi( this );";
    QString const create = "focusLossCloser = std::make_unique< PopupDismiss::FocusLossCloser >( this );";
    QString const handler = "focusLossCloser->setDismissHandler( [ this ] { hideWindow(); } );";
    QString const restore = "ui.pinButton->setChecked( cfg.pinPopupWindow );";
    QString const sync = "focusLossCloser->setPinned( ui.pinButton->isChecked() );";
    QVERIFY( selectionOrdered( ctor, { setup, create, handler, restore, sync, "setWindowFlags(" } ) );
    QStringList const statements = selectionStatements( ctor ); QVERIFY( statements.size() >= 3 );
    QCOMPARE( statements[0], selectionCode( setup ) );
    QCOMPARE( statements[1], selectionCode( create ) );
    QCOMPARE( statements[2], selectionCode( handler ) );
    auto const tokens = selectionTokens( selectionCode( ctor ) );
    qsizetype const restored = selectionFindTokens( tokens, selectionTokens( selectionCode( restore ) ) );
    qsizetype const synchronised = selectionFindTokens( tokens, selectionTokens( selectionCode( sync ) ) );
    qsizetype const firstFlags = selectionFindTokens( tokens, selectionTokens( selectionCode( "setWindowFlags(" ) ) );
    QVERIFY( restored >= 0 ); QVERIFY( synchronised > restored ); QVERIFY( firstFlags > synchronised );
    QCOMPARE( selectionCount( ctor, "focusLossCloser->setPinned(" ), 1 );
    QVERIFY( !selectionHas( ctor, "escapePressed();" ) );
  }
  void pinButtonSynchronizesBeforeFlags()
  {
    QString const file = source( "scanpopup.cc" ); QVERIFY( !file.isEmpty() );
    QString const body = requiredBlock( file, "ScanPopup::pinButtonClicked" ); QVERIFY( !body.isEmpty() );
    auto const statements = selectionStatements( body ); QVERIFY( statements.size() >= 5 );
    QCOMPARE( statements.first(), selectionCode( "focusLossCloser->setPinned( checked );" ) );
    auto const arms = conditionalArms( statements[1] ); QCOMPARE( arms.size(), 2 );
    QCOMPARE( arms[0].guard, selectionCode( "if( checked )" ) ); QCOMPARE( arms[1].guard, QString( "else" ) );
    QVERIFY( !arms[0].body.isEmpty() ); QVERIFY( !arms[1].body.isEmpty() );
    QVERIFY( selectionHas( arms[0].body, "setWindowFlags( flags );" ) );
    QVERIFY( selectionHas( arms[1].body, "setWindowFlags( unpinnedWindowFlags() );" ) );
    QCOMPARE( statements[2], selectionCode( "show();" ) );
  }
  void engagePopupPreservesScopedActivationPaths()
  {
    QString const file = source( "scanpopup.cc" ); QVERIFY( !file.isEmpty() );
    QString const body = requiredBlock( file, "ScanPopup::engagePopup" ); QVERIFY( !body.isEmpty() );
    QString chain;
    for( QString const & statement : selectionStatements( body ) )
      if( statement.startsWith( selectionCode( "if( !isVisible() )" ) + ' ' ) )
      { QVERIFY( chain.isEmpty() ); chain = statement; }
    QVERIFY( !chain.isEmpty() );
    auto const arms = conditionalArms( chain ); QCOMPARE( arms.size(), 3 );
    QCOMPARE( arms[0].guard, selectionCode( "if( !isVisible() )" ) );
    QCOMPARE( arms[1].guard, selectionCode( "if( ui.pinButton->isChecked() )" ) );
    QCOMPARE( arms[2].guard, selectionCode(
      "if( QGuiApplication::platformName() == \"xcb\" && ( windowFlags() & Qt::Tool ) == Qt::Tool )" ) );
    for( auto const & arm : arms ) QVERIFY( !arm.body.isEmpty() );
    QString const guard = "if( giveFocus || ( !ui.pinButton->isChecked() && PopupDismiss::needsExplicitActivation( windowFlags() ) ) )";
    QString const request = requiredBranch( arms[0].body, guard ); QVERIFY( !request.isEmpty() );
    QCOMPARE( selectionStatements( request ), QStringList( { selectionCode( "activateWindow();" ), selectionCode( "raise();" ) } ) );
    QCOMPARE( selectionDirectActions( request ), selectionCode( "activateWindow(); raise();" ) );
    auto const hiddenStatements = selectionStatements( arms[0].body ); QVERIFY( !hiddenStatements.isEmpty() );
    qsizetype show = -1, requestAt = -1;
    for( qsizetype i = 0; i < hiddenStatements.size(); ++i )
    {
      if( hiddenStatements[i] == selectionCode( "show();" ) ) { QVERIFY( show < 0 ); show = i; }
      if( hiddenStatements[i].startsWith( selectionCode( guard ) + ' ' ) ) { QVERIFY( requestAt < 0 ); requestAt = i; }
    }
    QVERIFY( show >= 0 ); QVERIFY( requestAt > show );
    QCOMPARE( selectionCount( body, "activateWindow(" ), 3 );
    for( auto const & arm : arms ) QCOMPARE( selectionCount( arm.body, "activateWindow(" ), 1 );
    QCOMPARE( selectionCount( body, "PopupDismiss::needsExplicitActivation(" ), 1 );
    QCOMPARE( selectionDirectActions( arms[1].body ), selectionCode( "show(); activateWindow(); raise();" ) );
    QCOMPARE( selectionDirectActions( arms[2].body ), selectionCode( "activateWindow(); raise();" ) );
    QString const rawFunction = rawCodeBlock( file, "ScanPopup::engagePopup" ); QVERIFY( !rawFunction.isEmpty() );
    QString const rawHidden = rawCodeBlock( rawFunction, "if( !isVisible() )" ); QVERIFY( !rawHidden.isEmpty() );
    QString const region = directiveRegion( rawHidden, "#ifdef ENABLE_SPWF_CUSTOMIZATION" ); QVERIFY( !region.isEmpty() );
    QString const giveFocus = requiredBranch( region,
      "if( QGuiApplication::platformName() == \"xcb\" && !ui.pinButton->isChecked() )" );
    QVERIFY( !giveFocus.isEmpty() );
    QCOMPARE( selectionStatements( giveFocus ), QStringList{ selectionCode( "giveFocus = true;" ) } );
    QVERIFY( selectionOrdered( arms[0].body, {
      "show();", "if( QGuiApplication::platformName() == \"xcb\" && !ui.pinButton->isChecked() )",
      "giveFocus = true;", guard } ) );
  }
  void unpinRequestsActivationAfterShow()
  {
    QString const file = source( "scanpopup.cc" ); QVERIFY( !file.isEmpty() );
    QString const body = requiredBlock( file, "ScanPopup::pinButtonClicked" ); QVERIFY( !body.isEmpty() );
    auto const statements = selectionStatements( body ); QCOMPARE( statements.size(), 5 );
    QCOMPARE( statements[0], selectionCode( "focusLossCloser->setPinned( checked );" ) );
    QVERIFY( statements[1].startsWith( selectionCode( "if( checked )" ) + ' ' ) );
    QCOMPARE( statements[2], selectionCode( "show();" ) );
    QString const guard = "if( !checked && PopupDismiss::needsExplicitActivation( windowFlags() ) )";
    QVERIFY( statements[3].startsWith( selectionCode( guard ) + ' ' ) );
    QString const request = requiredBranch( statements[3], guard ); QVERIFY( !request.isEmpty() );
    QCOMPARE( selectionStatements( request ), QStringList( { selectionCode( "activateWindow();" ), selectionCode( "raise();" ) } ) );
    QCOMPARE( selectionDirectActions( request ), selectionCode( "activateWindow(); raise();" ) );
    QCOMPARE( statements[4], selectionCode( "if( checked ) pinnedGeometry = saveGeometry();" ) );
    QCOMPARE( selectionCount( body, "activateWindow(" ), 1 );
    QVERIFY( selectionOrdered( body, { "show();", guard, "activateWindow();", "raise();",
                                      "if( checked ) pinnedGeometry = saveGeometry();" } ) );
  }

  void applicationProjectRegistersPopupDismiss()
  {
    QString const project = source( "goldendict.pro" ); QVERIFY( !project.isEmpty() );
    auto const headers = qmakeAssignments( project, "HEADERS" );
    auto const sources = qmakeAssignments( project, "SOURCES" );
    QVERIFY( !headers.isEmpty() ); QVERIFY( !sources.isEmpty() );
    auto assertRegistration = []( QList< QmakeAssignment > const & lists,
                                  QString const & before, QString const & added ) {
      int count = 0;
      for( auto const & list : lists )
        for( qsizetype i = 0; i < list.values.size(); ++i )
          if( list.values[i] == added )
          { ++count; QCOMPARE( list.depth, 0 ); QVERIFY( i > 0 ); QCOMPARE( list.values[i-1], before ); }
      QCOMPARE( count, 1 );
    };
    assertRegistration( headers, "platformwindow.hh", "popupdismiss.hh" );
    assertRegistration( sources, "platformwindow.cc", "popupdismiss.cc" );
    for( auto const & list : headers ) QVERIFY( !list.values.contains( "popupdismiss.cc" ) );
    for( auto const & list : sources ) QVERIFY( !list.values.contains( "popupdismiss.hh" ) );
  }
  void requiredSuiteInventoryIsExact()
  {
    QString const project = source( "tests/tests.pro" ); QVERIFY( !project.isEmpty() );
    QString const clean = qmakeUncomment( project );
    QString const ordinary = selectionRawBlock( clean, "} else {" ); QVERIFY( !ordinary.isEmpty() );
    auto const lists = qmakeAssignments( ordinary, "GD_REQUIRED_SUITE_NAMES" ); QCOMPARE( lists.size(), 1 );
    QCOMPARE( lists[0].depth, 0 );
    QStringList const expected = {
      "tst_article_bridge", "tst_article_history", "tst_article_load", "tst_article_schemes",
      "tst_article_stream", "tst_btree_index", "tst_encoding", "tst_external_content",
      "tst_hotkeys", "tst_htmlescape", "tst_langcoder", "tst_linkrewrite", "tst_platformwindow",
      "tst_popup_dismiss", "tst_portal_hotkeys", "tst_print_pipeline", "tst_regex",
      "tst_save_article", "tst_utf8_wstring", "tst_waylandselection", "tst_website_frames" };
    QCOMPARE( expected.size(), 21 ); QCOMPARE( lists[0].values, expected );
    QStringList discovered;
    QDir const tests( QDir( TestSupport::repositoryRoot() ).filePath( "tests" ) );
    QVERIFY( tests.exists() );
    for( auto const & directory : tests.entryInfoList( { "tst_*" }, QDir::Dirs | QDir::NoDotAndDotDot ) )
      for( auto const & file : QDir( directory.absoluteFilePath() ).entryInfoList( { "tst_*.pro" }, QDir::Files ) )
        discovered.append( file.completeBaseName() );
    discovered.sort(); QCOMPARE( discovered, expected );
    QCOMPARE( QSet< QString >( discovered.begin(), discovered.end() ).size(), discovered.size() );
    QCOMPARE( QSet< QString >( lists[0].values.begin(), lists[0].values.end() ).size(), lists[0].values.size() );
    QCOMPARE( discovered.count( "tst_popup_dismiss" ), 1 );
    QCOMPARE( expected[12], QString( "tst_platformwindow" ) );
    QCOMPARE( expected[13], QString( "tst_popup_dismiss" ) );
    QCOMPARE( expected[14], QString( "tst_portal_hotkeys" ) );
    QRegularExpression const countPattern( "!count\\s*\\(\\s*GD_TEST_NAMES\\s*,\\s*([0-9]+)\\s*\\)" );
    auto counts = countPattern.globalMatch( ordinary ); QVERIFY( counts.hasNext() );
    auto const count = counts.next(); QVERIFY( !counts.hasNext() );
    QCOMPARE( count.captured( 1 ).toInt(), int( lists[0].values.size() ) );
    QCOMPARE( count.captured( 1 ).toInt(), int( discovered.size() ) );
    auto const lines = qmakeLines( ordinary );
    QVERIFY( lines.contains( "!count(GD_TEST_NAMES, 21): error(\"Expected exactly 21 required suites; found: $$GD_TEST_NAMES\")" ) );
    QVERIFY( lines.contains( "message(\"tests/tests.pro: exact 21 suites: $$GD_TEST_NAMES\")" ) );
    QVERIFY( lines.contains( "!equals(GD_TEST_NAMES, $$GD_REQUIRED_SUITE_NAMES): error(\"Required suite inventory mismatch: $$GD_TEST_NAMES\")" ) );
    QVERIFY( project.split( '\n' ).contains( "# Ordinary mode is a closed acceptance inventory: exactly the 21 required suites." ) );
  }
};
QTEST_MAIN( TstPopupDismiss )
#include "tst_popup_dismiss.moc"
