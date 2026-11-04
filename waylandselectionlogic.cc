#include "waylandselectionlogic.hh"
#include <algorithm>
#include <chrono>
#include <limits>
qint64 Monotonic::nowMs() noexcept
{
  using Clock = std::chrono::steady_clock;
  static auto const epoch = Clock::now();
  return 1 + std::chrono::duration_cast<std::chrono::milliseconds>( Clock::now() - epoch ).count();
}
namespace WaylandSelection {
namespace {
QStringList acceptedMimes( QStringList const & offered )
{
  QStringList result;
  for( auto const * literal : { "text/plain;charset=utf-8", "text/plain;charset=UTF-8", "UTF8_STRING", "text/plain" } )
    if( offered.contains( QLatin1String( literal ) ) ) result.append( QLatin1String( literal ) );
  return result;
}
}
QString chooseMimeType( QStringList const & offered ) { return acceptedMimes( offered ).value( 0 ); }
PayloadAccumulator::PayloadAccumulator( int cap ) : cap_( cap ) { Q_ASSERT( cap >= 0 ); }
void PayloadAccumulator::append( QByteArray const & chunk )
{
  qsizetype const remaining = cap_ - data_.size();
  qsizetype const kept = std::min( remaining, chunk.size() );
  if( chunk.size() > remaining ) truncated_ = true;
  if( kept ) data_.append( chunk.constData(), kept );
}
bool PayloadAccumulator::full() const noexcept { return data_.size() == cap_; }
bool PayloadAccumulator::truncated() const noexcept { return truncated_; }
QByteArray PayloadAccumulator::payload() const { return data_; }
QString decodePayload( QByteArray const & raw )
{
  QString text = QString::fromUtf8( raw.constData(), raw.size() );
  text.remove( QChar( 0 ) );
  return text;
}
bool clipboardStampAdvances( bool modeIsClipboard ) noexcept { return modeIsClipboard; }
QString chooseText( QString const & selection, qint64 capturedAt,
                    QString const & clipboard, qint64 clipboardChangedAt )
{
  Q_ASSERT( capturedAt >= 0 && clipboardChangedAt >= 0 );
  bool const hasSelection = !selection.trimmed().isEmpty();
  bool const hasClipboard = !clipboard.trimmed().isEmpty();
  if( hasSelection && capturedAt >= clipboardChangedAt ) return selection;
  if( hasClipboard ) return clipboard;
  if( hasSelection ) return selection;
  return {};
}
ActivationPlan planActivation( bool pending ) noexcept
{
  return pending ? ActivationPlan::AwaitPending : ActivationPlan::UseCached;
}
void OfferState::pendingOffer( quint64 id, QStringList const & types )
{
  pendingId_ = id;
  pendingMimeTypes_ = id ? types : QStringList{};
}
ChannelOffer OfferState::takeForPrimary()
{
  quint64 const id = pendingId_;
  QStringList const types = pendingMimeTypes_;
  retire();
  pendingId_ = 0;
  pendingMimeTypes_.clear();
  primaryId_ = id;
  mimeTypes_ = id ? acceptedMimes( types ) : QStringList{};
  return { id };
}
ChannelOffer OfferState::discardForClipboard()
{
  ChannelOffer const result{ pendingId_ };
  pendingId_ = 0;
  pendingMimeTypes_.clear();
  return result;
}
bool OfferState::beginMimeAttempt()
{
  if( !primaryId_ ) return false;
  if( currentMime_ >= 0 ) return true;
  if( nextMime_ >= mimeTypes_.size() ) return false;
  currentMime_ = nextMime_++;
  return true;
}
QString OfferState::currentMimeType() const { return currentMime_ < 0 ? QString{} : mimeTypes_.value( currentMime_ ); }
void OfferState::mimeAttemptFailed() noexcept
{
  if( currentMime_ < 0 ) return;
  activeToken_ = 0;
  currentMime_ = -1;
}
ReadToken OfferState::beginRead() noexcept
{
  if( !primaryId_ || currentMime_ < 0 || activeToken_ ) return 0;
  Q_ASSERT( nextToken_ != std::numeric_limits<ReadToken>::max() );
  activeToken_ = ++nextToken_;
  return activeToken_;
}
bool OfferState::accepts( ReadToken token ) const noexcept { return primaryId_ && token && token == activeToken_; }
void OfferState::retire() noexcept
{
  primaryId_ = 0; nextMime_ = 0; currentMime_ = -1; activeToken_ = 0;
}
void OfferState::completed( ReadToken token ) noexcept { if( accepts( token ) ) retire(); }
} // namespace WaylandSelection
