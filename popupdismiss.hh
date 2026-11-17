/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file. */

#ifndef GOLDENDICT_POPUPDISMISS_HH_INCLUDED
#define GOLDENDICT_POPUPDISMISS_HH_INCLUDED

#include <QEvent>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <Qt>

#include <functional>

class QWidget;
class QWindow;

namespace PopupDismiss {

enum class Action { Ignore, Hide };

enum class FocusDestination {
  OwnWindow,
  OwnPopup,
  Foreign,
  None,
};

Action onFocusMoved( bool visible, bool pinned, bool armed,
                     FocusDestination destination, bool applicationActive );

bool needsExplicitActivation( Qt::WindowFlags flags );
bool armedAfterShow( QWindow const * ownWindow, QWindow const * focusWindow );
bool isOwnedPopup( QWidget const * owner, QWidget const * candidate );
bool isPopupWindowType( Qt::WindowType type );

} // namespace PopupDismiss

#endif // GOLDENDICT_POPUPDISMISS_HH_INCLUDED
