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

class FocusLossCloser : public QObject {
  Q_OBJECT
public:
  explicit FocusLossCloser( QWidget * window, QObject * parent = nullptr );
  ~FocusLossCloser() override;

  void setPinned( bool pinned );
  bool isPinned() const;
  void setDismissHandler( std::function< void() > handler );

protected:
  bool eventFilter( QObject * watched, QEvent * event ) override;

private:
  FocusDestination destinationOf( QWindow const * focusWindow ) const;
  void dismiss( char const * reason );

  QPointer< QWidget > window;
  bool pinned = false;
  bool armed = false;
  bool dismissing = false;
  QMetaObject::Connection focusConnection;
  QMetaObject::Connection stateConnection;
  std::function< void() > dismissHandler;
};

} // namespace PopupDismiss

#endif // GOLDENDICT_POPUPDISMISS_HH_INCLUDED
