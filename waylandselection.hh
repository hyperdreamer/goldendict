#ifndef WAYLANDSELECTION_HH_INCLUDED
#define WAYLANDSELECTION_HH_INCLUDED
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <memory>
#ifdef HAVE_WAYLAND_DATA_CONTROL
inline constexpr bool kHaveWaylandDataControl = true;
#else
inline constexpr bool kHaveWaylandDataControl = false;
#endif
class WaylandSelectionSource : public QObject
{
  Q_OBJECT
public:
  enum class State { Unsupported, Starting, Available, Unavailable };
  explicit WaylandSelectionSource( QObject * parent = nullptr );
  ~WaylandSelectionSource() override;
  State state() const noexcept;
  QString reason() const;
  bool primarySelectionSeen() const noexcept;
  QString lastText() const;
  bool hasPendingCapture() const noexcept;
public slots:
  void start();
  void stop();
  void requestCapture( quint64 requestId );
signals:
  void stateChanged( State state, QString const & reason );
  void capabilityChanged();
  void captureFinished( quint64 requestId, QString const & text, qint64 capturedAt );
private:
  class Impl;
  std::unique_ptr< Impl > d;
};
struct SelectionStatus
{
  WaylandSelectionSource::State state = WaylandSelectionSource::State::Unsupported;
  QString reason;
  bool primarySelectionSeen = false;
};
Q_DECLARE_METATYPE( WaylandSelectionSource::State )
#endif // WAYLANDSELECTION_HH_INCLUDED
