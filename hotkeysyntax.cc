/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Hotkey text conversions; see hotkeysyntax.hh and specification §4.11.
 */

#include "hotkeysyntax.hh"

#include <QCoreApplication>
#include <QKeySequence>
#include <QStringList>

namespace HotkeySyntax {

namespace {

QString reason( char const * text )
{
  return QCoreApplication::translate( "HotkeySyntax", text );
}

void setError( QString * error, QString const & text )
{
  if( error )
    *error = text;
}

/// Modifiers the portal accepts, in the canonical serialization order. Anything
/// outside this set (KeypadModifier, GroupSwitchModifier, ...) is unsupported and
/// never turned into an invented trigger.
struct ModifierName
{
  Qt::KeyboardModifier modifier;
  char const * name;
};

ModifierName const modifierNames[] = {
  { Qt::ControlModifier, "CTRL" },
  { Qt::AltModifier, "ALT" },
  { Qt::ShiftModifier, "SHIFT" },
  { Qt::MetaModifier, "LOGO" },
};

Qt::KeyboardModifiers supportedModifiers()
{
  return Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier;
}

/// The X keysym name of one ordinary key, or an empty string for a key the
/// specification does not map. Keysym names are the standard X ones; an unknown
/// key must never get an invented name.
QString keysymName( Qt::Key key )
{
  if( key >= Qt::Key_A && key <= Qt::Key_Z )
    return QString( QChar( 'a' + ( key - Qt::Key_A ) ) );

  if( key >= Qt::Key_0 && key <= Qt::Key_9 )
    return QString( QChar( '0' + ( key - Qt::Key_0 ) ) );

  if( key >= Qt::Key_F1 && key <= Qt::Key_F35 )
    return QStringLiteral( "F" ) + QString::number( key - Qt::Key_F1 + 1 );

  switch( key )
  {
    case Qt::Key_Space:      return QStringLiteral( "space" );
    case Qt::Key_Escape:     return QStringLiteral( "Escape" );
    case Qt::Key_Tab:        return QStringLiteral( "Tab" );
    case Qt::Key_Backspace:  return QStringLiteral( "BackSpace" );
    case Qt::Key_Return:     return QStringLiteral( "Return" );
    case Qt::Key_Insert:     return QStringLiteral( "Insert" );
    case Qt::Key_Delete:     return QStringLiteral( "Delete" );
    case Qt::Key_Home:       return QStringLiteral( "Home" );
    case Qt::Key_End:        return QStringLiteral( "End" );
    case Qt::Key_Left:       return QStringLiteral( "Left" );
    case Qt::Key_Right:      return QStringLiteral( "Right" );
    case Qt::Key_Up:         return QStringLiteral( "Up" );
    case Qt::Key_Down:       return QStringLiteral( "Down" );
    case Qt::Key_PageUp:     return QStringLiteral( "Prior" );
    case Qt::Key_PageDown:   return QStringLiteral( "Next" );
    case Qt::Key_Pause:      return QStringLiteral( "Pause" );
    case Qt::Key_Print:      return QStringLiteral( "Print" );
    case Qt::Key_Menu:       return QStringLiteral( "Menu" );

    // ASCII punctuation: the standard X keysym name, not the character itself.
    case Qt::Key_Plus:        return QStringLiteral( "plus" );
    case Qt::Key_Comma:       return QStringLiteral( "comma" );
    case Qt::Key_Minus:       return QStringLiteral( "minus" );
    case Qt::Key_Period:      return QStringLiteral( "period" );
    case Qt::Key_Slash:       return QStringLiteral( "slash" );
    case Qt::Key_Semicolon:   return QStringLiteral( "semicolon" );
    case Qt::Key_Colon:       return QStringLiteral( "colon" );
    case Qt::Key_Equal:       return QStringLiteral( "equal" );
    case Qt::Key_BracketLeft: return QStringLiteral( "bracketleft" );
    case Qt::Key_BracketRight:return QStringLiteral( "bracketright" );
    case Qt::Key_Backslash:   return QStringLiteral( "backslash" );
    case Qt::Key_Apostrophe:  return QStringLiteral( "apostrophe" );
    case Qt::Key_QuoteDbl:    return QStringLiteral( "quotedbl" );
    case Qt::Key_QuoteLeft:   return QStringLiteral( "grave" );
    case Qt::Key_AsciiTilde:  return QStringLiteral( "asciitilde" );
    case Qt::Key_Exclam:      return QStringLiteral( "exclam" );
    case Qt::Key_At:          return QStringLiteral( "at" );
    case Qt::Key_NumberSign:  return QStringLiteral( "numbersign" );
    case Qt::Key_Dollar:      return QStringLiteral( "dollar" );
    case Qt::Key_Percent:     return QStringLiteral( "percent" );
    case Qt::Key_AsciiCircum: return QStringLiteral( "asciicircum" );
    case Qt::Key_Ampersand:   return QStringLiteral( "ampersand" );
    case Qt::Key_Asterisk:    return QStringLiteral( "asterisk" );
    case Qt::Key_ParenLeft:   return QStringLiteral( "parenleft" );
    case Qt::Key_ParenRight:  return QStringLiteral( "parenright" );
    case Qt::Key_Underscore:  return QStringLiteral( "underscore" );
    case Qt::Key_BraceLeft:   return QStringLiteral( "braceleft" );
    case Qt::Key_BraceRight:  return QStringLiteral( "braceright" );
    case Qt::Key_Less:        return QStringLiteral( "less" );
    case Qt::Key_Greater:     return QStringLiteral( "greater" );
    case Qt::Key_Question:    return QStringLiteral( "question" );
    case Qt::Key_Bar:         return QStringLiteral( "bar" );

    default:                  return QString();
  }
}

/// True for the keys that only carry modifiers: such a value is not a shortcut.
bool isModifierKey( Qt::Key key )
{
  switch( key )
  {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
    case Qt::Key_unknown:
      return true;
    default:
      return false;
  }
}

/// Backtab is Shift+Tab in the portal format, so its implicit Shift is merged
/// into the modifier set exactly once.
Qt::Key portalKey( Qt::Key key, Qt::KeyboardModifiers * modifiers )
{
  if( key == Qt::Key_Backtab )
  {
    *modifiers |= Qt::ShiftModifier;
    return Qt::Key_Tab;
  }
  return key;
}

} // namespace

bool parse( QString const & stored, Config::HotKey * out, QString * error )
{
  QKeySequence const sequence = QKeySequence::fromString( stored, QKeySequence::PortableText );

  if( sequence.isEmpty() )
  {
    setError( error, reason( "The hotkey is empty or not a recognisable key sequence." ) );
    return false;
  }

  if( sequence.count() > 2 )
  {
    setError( error, reason( "Only one or two key combinations are supported." ) );
    return false;
  }

  QKeyCombination const first = sequence[ 0 ];
  if( first.key() == Qt::Key_unknown )
  {
    setError( error, reason( "The hotkey contains a key this system does not recognise." ) );
    return false;
  }

  Config::HotKey parsed;
  parsed.modifiers = first.keyboardModifiers();
  parsed.key1 = int( first.key() );

  if( sequence.count() > 1 )
  {
    QKeyCombination const second = sequence[ 1 ];
    if( second.key() == Qt::Key_unknown )
    {
      setError( error, reason( "The second key of the hotkey is not recognised." ) );
      return false;
    }
    // The stored value keeps the modifiers in the first combination only; the
    // second one contributes its key, as the existing format always has.
    parsed.key2 = int( second.key() );
  }

  if( out )
    *out = parsed;
  return true;
}

bool preferredTrigger( Config::HotKey const & key, QString * trigger, QString * error )
{
  if( key.key1 == 0 )
  {
    setError( error, reason( "The hotkey has no key, only modifiers." ) );
    return false;
  }

  if( key.key2 != 0 )
  {
    setError( error, reason( "A two-key shortcut cannot be used as a global portal shortcut." ) );
    return false;
  }

  Qt::KeyboardModifiers const unsupported = key.modifiers & ~supportedModifiers();
  if( unsupported != Qt::KeyboardModifiers() )
  {
    setError( error, reason( "The hotkey uses a modifier the portal does not support." ) );
    return false;
  }

  Qt::Key const rawKey = Qt::Key( key.key1 );
  if( rawKey == Qt::Key_Enter )
  {
    setError( error, reason( "The keypad Enter key has no supported portal name." ) );
    return false;
  }

  if( isModifierKey( rawKey ) )
  {
    setError( error, reason( "The hotkey has no ordinary key, only modifiers." ) );
    return false;
  }

  QStringList parts;

  Qt::KeyboardModifiers modifiers = key.modifiers;
  Qt::Key const mappedKey = portalKey( rawKey, & modifiers );

  for( ModifierName const & entry : modifierNames )
  {
    if( modifiers & entry.modifier )
      parts << QString::fromLatin1( entry.name );
  }

  QString const name = keysymName( mappedKey );
  if( name.isEmpty() )
  {
    setError( error, reason( "The key of this hotkey has no portal (X keysym) name." ) );
    return false;
  }

  parts << name;

  if( trigger )
    *trigger = parts.join( QLatin1Char( '+' ) );
  return true;
}

QString serialize( Config::HotKey const & key )
{
  return key.toKeySequence().toString( QKeySequence::PortableText );
}

} // namespace HotkeySyntax
