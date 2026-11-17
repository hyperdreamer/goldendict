#include "popupdismiss.hh"

#include <QApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QWidget>
#include <QWindow>
#include <utility>

namespace PopupDismiss {

Action onFocusMoved( bool visible, bool pinned, bool armed,
                     FocusDestination destination, bool applicationActive )
{
  return visible && !pinned && armed &&
         ( destination == FocusDestination::Foreign ||
           ( destination == FocusDestination::None && !applicationActive ) )
           ? Action::Hide : Action::Ignore;
}

bool needsExplicitActivation( Qt::WindowFlags flags )
{
  return ( flags & Qt::WindowType_Mask ) != Qt::Popup;
}

bool armedAfterShow( QWindow const * ownWindow, QWindow const * focusWindow )
{
  return ownWindow != nullptr && ownWindow == focusWindow;
}

bool isOwnedPopup( QWidget const * owner, QWidget const * candidate )
{
  if( !owner || !candidate || owner == candidate ) return false;
  for( QWidget const * parent = candidate->parentWidget(); parent;
       parent = parent->parentWidget() )
    if( parent == owner ) return true;
  return false;
}

bool isPopupWindowType( Qt::WindowType type ) { return type == Qt::Popup; }

} // namespace PopupDismiss
