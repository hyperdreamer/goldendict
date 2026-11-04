#ifndef WAYLANDSELECTIONLOGIC_HH_INCLUDED
#define WAYLANDSELECTIONLOGIC_HH_INCLUDED
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>
class Monotonic final
{
public:
  static qint64 nowMs() noexcept;
};
namespace WaylandSelection {
inline constexpr int MaxPayloadBytes = 1024 * 1024;
inline constexpr int DebounceMs = 200;
inline constexpr int IdleTimeoutMs = 250;
inline constexpr int TransferTimeoutMs = 2000;
inline constexpr int ActivationTimeoutMs = 300;
QString chooseMimeType( QStringList const & offered );
class PayloadAccumulator
{
public:
  explicit PayloadAccumulator( int cap = MaxPayloadBytes );
  void append( QByteArray const & chunk );
  bool full() const noexcept;
  bool truncated() const noexcept;
  QByteArray payload() const;
private:
  int cap_;
  QByteArray data_;
  bool truncated_ = false;
};
QString decodePayload( QByteArray const & raw );
bool clipboardStampAdvances( bool modeIsClipboard ) noexcept;
QString chooseText( QString const & selection, qint64 capturedAt,
                    QString const & clipboard, qint64 clipboardChangedAt );
enum class ActivationPlan { UseCached, AwaitPending };
ActivationPlan planActivation( bool hasPendingCapture ) noexcept;
using ReadToken = quint64;
struct ChannelOffer
{
  quint64 id = 0;
  operator bool() const noexcept { return id != 0; }
};
class OfferState
{
public:
  void pendingOffer( quint64 offerId, QStringList const & mimeTypes );
  ChannelOffer takeForPrimary();
  ChannelOffer discardForClipboard();
  bool beginMimeAttempt();
  QString currentMimeType() const;
  void mimeAttemptFailed() noexcept;
  ReadToken beginRead() noexcept;
  bool accepts( ReadToken token ) const noexcept;
  void retire() noexcept;
  void completed( ReadToken token ) noexcept;
private:
  quint64 pendingId_ = 0;
  QStringList pendingMimeTypes_;
  quint64 primaryId_ = 0;
  QStringList mimeTypes_;
  int nextMime_ = 0;
  int currentMime_ = -1;
  ReadToken nextToken_ = 0;
  ReadToken activeToken_ = 0;
};
} // namespace WaylandSelection
#endif // WAYLANDSELECTIONLOGIC_HH_INCLUDED
