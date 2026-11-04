#include "waylandselection.hh"
#include "waylandselectionlogic.hh"
#include "hotkeybackend.hh"
#include "gddebug.hh"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <utility>
#ifdef HAVE_WAYLAND_DATA_CONTROL
#include <QHash>
#include <QSocketNotifier>
#include <QVector>
#include <wayland-client.h>
#include "ext-data-control-v1-client-protocol.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif
using namespace WaylandSelection;
class WaylandSelectionSource::Impl
{
public:
  WaylandSelectionSource * q;
  State current;
  QString explanation, cache;
  qint64 cacheStamp = 0;
  bool primarySeen = false, started = false, ownerStopped = false;
  quint64 lastRequestId = 0, interestedId = 0;
  qint64 activationCalledAt = 0, activationDeadline = 0;
  QTimer debounce, transferWake, activation;
  explicit Impl( WaylandSelectionSource * owner ) : q( owner ),
    current( scanPopupCaptureSupported( kHaveWaylandDataControl, QGuiApplication::platformName() )
               ? State::Starting : State::Unsupported )
  {
    for( QTimer * timer : { &debounce, &transferWake, &activation } )
    { timer->setSingleShot( true ); timer->setTimerType( Qt::PreciseTimer ); }
    QObject::connect( &activation, &QTimer::timeout, q, [this] { expireInterest(); } );
#ifdef HAVE_WAYLAND_DATA_CONTROL
    QObject::connect( &debounce, &QTimer::timeout, q, [this] { if( live() ) promote(); } );
#endif
  }
  void assertThread() const { Q_ASSERT( QThread::currentThread() == q->thread() ); }
  static char const * stateName( State state )
  {
    switch( state )
    {
      case State::Unsupported: return "Unsupported";
      case State::Starting: return "Starting";
      case State::Available: return "Available";
      case State::Unavailable: return "Unavailable";
    }
    Q_UNREACHABLE();
  }
  void publishState()
  {
    gdDebug( "selection-source: state=%s reason=%s", stateName( current ), explanation.toUtf8().constData() );
    emit q->stateChanged( current, explanation );
  }
  bool pending() const noexcept
  {
#ifdef HAVE_WAYLAND_DATA_CONTROL
    return !ownerStopped && primaryId != 0;
#else
    return false;
#endif
  }
  void armActivation()
  {
    if( interestedId ) activation.start( int( std::max<qint64>( 1, activationDeadline - Monotonic::nowMs() ) ) );
  }
  void answerInterest( QString text, qint64 stamp )
  {
    quint64 const id = std::exchange( interestedId, 0 );
    activation.stop();
    if( id && !ownerStopped ) emit q->captureFinished( id, text, text.isEmpty() ? 0 : stamp );
  }
  void expireInterest()
  {
    assertThread();
    if( ownerStopped || !interestedId ) return;
    if( Monotonic::nowMs() < activationDeadline ) { armActivation(); return; }
    gdDebug( "selection-timeout" );
    answerInterest( {}, 0 );
  }
  void requestCapture( quint64 id )
  {
    assertThread();
    if( ownerStopped ) return;
    Q_ASSERT( id && id > lastRequestId );
    lastRequestId = id;
    quint64 const superseded = interestedId;
    bool const await = planActivation( pending() ) == ActivationPlan::AwaitPending;
    interestedId = id;
    activationCalledAt = Monotonic::nowMs();
    activationDeadline = activationCalledAt + ActivationTimeoutMs;
    armActivation();
    if( superseded )
    {
      emit q->captureFinished( superseded, QString(), 0 );
    }
    if( ownerStopped || interestedId != id ) return;
    if( !await ) { answerInterest( cache, cache.isEmpty() ? 0 : cacheStamp ); return; }
#ifdef HAVE_WAYLAND_DATA_CONTROL
    if( primaryId && !readToken ) promote();
#endif
  }
  void start()
  {
    assertThread();
    if( started || ownerStopped ) return;
    started = true;
    publishState();
    if( ownerStopped || current == State::Unsupported ) return;
#ifdef HAVE_WAYLAND_DATA_CONTROL
    connection = std::make_shared<Connection>();
    connection->listenerContext = this;
    connection->display = wl_display_connect( nullptr );
    if( !connection->display ) { fail( "cannot connect to Wayland display" ); return; }
    connection->fd = wl_display_get_fd( connection->display );
    if( connection->fd < 0 ) { fail( "Wayland display error" ); return; }
    displayRead = std::make_unique<QSocketNotifier>( connection->fd, QSocketNotifier::Read );
    displayWrite = std::make_unique<QSocketNotifier>( connection->fd, QSocketNotifier::Write );
    displayWrite->setEnabled( false );
    QObject::connect( displayRead.get(), &QSocketNotifier::activated, q,
                      [this] { dispatchDisplayReadable(); } );
    QObject::connect( displayWrite.get(), &QSocketNotifier::activated, q,
                      [this] { dispatchDisplayWritable(); } );
    registry = wl_display_get_registry( connection->display );
    if( !registry ) { fail( "cannot get Wayland registry" ); return; }
    static wl_registry_listener const registryListener{ registryGlobal, registryGlobalRemoved };
    wl_registry_add_listener( registry, &registryListener, connection->listenerContext );
    sync = wl_display_sync( connection->display );
    if( !sync ) { fail( "cannot create Wayland sync callback" ); return; }
    static wl_callback_listener const syncListener{ discoveryDone };
    wl_callback_add_listener( sync, &syncListener, connection->listenerContext );
    flushRequests();
#endif
  }
  void stop()
  {
    assertThread();
    ownerStopped = true;
    interestedId = 0;
    debounce.stop(); transferWake.stop(); activation.stop();
#ifdef HAVE_WAYLAND_DATA_CONTROL
    dispose();
#endif
  }
#ifdef HAVE_WAYLAND_DATA_CONTROL
  struct Connection
  {
    wl_display * display = nullptr;
    Impl * listenerContext = nullptr;
    int fd = -1, dispatchDepth = 0;
    quint64 generation = 1;
    bool prepared = false, retired = false, destroyed_ = false;
    void destroyConnection()
    {
      if( destroyed_ ) return;
      Q_ASSERT( dispatchDepth == 0 );
      destroyed_ = true;
      if( !display ) return;
      if( prepared ) { wl_display_cancel_read( display ); prepared = false; }
      if( wl_display_get_error( display ) == 0 ) (void)wl_display_flush( display );
      wl_display_disconnect( display );
      display = nullptr; listenerContext = nullptr; fd = -1;
    }
    ~Connection() { destroyConnection(); }
  };
  struct Seat { uint32_t name, version; };
  struct Offer { quint64 id; QStringList mimes; };
  std::shared_ptr<Connection> connection;
  quint64 failedRequestId = 0;
  bool terminalNotificationPending = false;
  std::unique_ptr<QSocketNotifier> displayRead, displayWrite, pipeRead;
  wl_registry * registry = nullptr;
  wl_callback * sync = nullptr;
  ext_data_control_manager_v1 * manager = nullptr;
  wl_seat * seat = nullptr;
  ext_data_control_device_v1 * device = nullptr;
  uint32_t managerName = 0, seatNameId = 0;
  QVector<Seat> seats;
  bool discoveryComplete = false, multipleSeatsLogged = false;
  QHash<ext_data_control_offer_v1 *, Offer> offers;
  OfferState offersState;
  quint64 nextOfferId = 0, primaryId = 0;
  ReadToken readToken = 0;
  int readEnd = -1, bytesObserved = 0;
  qint64 offerStamp = 0, transferStartedAt = 0, transferDeadline = 0, lastProgressAt = 0;
  PayloadAccumulator payload;
  bool live() const { return !ownerStopped && connection && !connection->retired && connection->display; }
  bool healthy() const { return connection && connection->display && wl_display_get_error( connection->display ) == 0; }
  ext_data_control_offer_v1 * proxyFor( quint64 id ) const
  {
    for( auto it = offers.cbegin(); it != offers.cend(); ++it ) if( it.value().id == id ) return it.key();
    return nullptr;
  }
  void destroyOffer( quint64 id )
  {
    auto * proxy = proxyFor( id );
    if( !proxy ) return;
    offers.remove( proxy );
    if( healthy() ) ext_data_control_offer_v1_destroy( proxy );
    else wl_proxy_destroy( reinterpret_cast<wl_proxy *>( proxy ) );
  }
  void closeRead()
  {
    transferWake.stop();
    QObject::disconnect( &transferWake, nullptr, q, nullptr );
    if( pipeRead ) { pipeRead->setEnabled( false ); pipeRead.reset(); }
    if( readEnd >= 0 ) { close( readEnd ); readEnd = -1; }
    readToken = 0;
  }
  void retirePrimary()
  {
    offersState.retire();
    debounce.stop();
    closeRead();
    quint64 const id = std::exchange( primaryId, 0 );
    destroyOffer( id );
    transferStartedAt = 0; transferDeadline = 0; bytesObserved = 0;
  }
  void destroyDeviceAndSeat()
  {
    if( device )
    {
      if( healthy() ) ext_data_control_device_v1_destroy( device );
      else wl_proxy_destroy( reinterpret_cast<wl_proxy *>( device ) );
      device = nullptr;
    }
    if( seat ) { wl_seat_destroy( seat ); seat = nullptr; }
    seatNameId = 0;
  }
  void destroyProtocol()
  {
    for( auto const & offer : offers.values() ) destroyOffer( offer.id );
    primaryId = 0;
    destroyDeviceAndSeat();
    if( manager )
    {
      if( healthy() ) ext_data_control_manager_v1_destroy( manager );
      else wl_proxy_destroy( reinterpret_cast<wl_proxy *>( manager ) );
      manager = nullptr;
    }
    if( sync ) { wl_callback_destroy( sync ); sync = nullptr; }
    if( registry ) { wl_registry_destroy( registry ); registry = nullptr; }
    managerName = 0;
  }
  void dispose()
  {
    assertThread();
    auto const c = connection;
    if( !c ) return;
    if( !c->retired )
    {
      c->retired = true;
      ++c->generation;
      offersState.retire();
      interestedId = 0;
      debounce.stop(); transferWake.stop(); activation.stop();
      closeRead();
      if( displayRead ) displayRead->setEnabled( false );
      if( displayWrite ) displayWrite->setEnabled( false );
      displayRead.reset(); displayWrite.reset();
      destroyProtocol();
      c->listenerContext = nullptr;
      if( terminalNotificationPending )
      {
        terminalNotificationPending = false;
        if( !ownerStopped ) publishState();
        quint64 const id = std::exchange( failedRequestId, 0 );
        if( id && !ownerStopped ) emit q->captureFinished( id, QString(), 0 );
      }
    }
    if( c->dispatchDepth == 0 ) c->destroyConnection();
    else QTimer::singleShot( 0, QCoreApplication::instance(), [c] { c->destroyConnection(); } );
  }
  void fail( char const * reason )
  {
    if( ownerStopped || current == State::Unavailable ) return;
    current = State::Unavailable; explanation = QLatin1String( reason );
    failedRequestId = std::exchange( interestedId, 0 );
    terminalNotificationPending = true;
    dispose();
  }
  bool flushRequests()
  {
    assertThread();
    if( !live() ) return false;
    for( ;; )
    {
      if( wl_display_get_error( connection->display ) ) { fail( "Wayland display error" ); return false; }
      int const result = wl_display_flush( connection->display );
      if( wl_display_get_error( connection->display ) ) { fail( "Wayland display error" ); return false; }
      if( result >= 0 ) { displayWrite->setEnabled( false ); return true; }
      if( errno == EINTR ) continue;
      if( errno == EAGAIN ) { displayWrite->setEnabled( true ); return true; }
      fail( "Wayland display error" ); return false;
    }
  }
  bool dispatchPending( std::shared_ptr<Connection> const & c, quint64 generation )
  {
    ++c->dispatchDepth;
    int const result = wl_display_dispatch_pending( c->display );
    --c->dispatchDepth;
    if( c->retired || c->generation != generation ) { c->destroyConnection(); return false; }
    if( wl_display_get_error( c->display ) ) { fail( "Wayland display error" ); return false; }
    if( result < 0 ) return flushRequests();
    return true;
  }
  void dispatchDisplayReadable()
  {
    assertThread();
    if( !live() ) return;
    auto const c = connection;
    quint64 const generation = c->generation;
    displayRead->setEnabled( false );
    if( wl_display_prepare_read( c->display ) != 0 )
    {
      if( wl_display_get_error( c->display ) ) { fail( "Wayland display error" ); return; }
      // EAGAIN and the no-display-error errno branch both dispatch once, never read.
      if( !dispatchPending( c, generation ) ) return;
      if( flushRequests() && live() ) displayRead->setEnabled( true );
      return;
    }
    c->prepared = true;
    pollfd ready{ c->fd, POLLIN, 0 };
    if( ::poll( &ready, 1, 0 ) <= 0 || !( ready.revents & ( POLLIN | POLLHUP | POLLERR ) ) )
    {
      wl_display_cancel_read( c->display ); c->prepared = false;
      if( flushRequests() && live() ) displayRead->setEnabled( true );
      return;
    }
    ++c->dispatchDepth;
    int const result = wl_display_read_events( c->display );
    --c->dispatchDepth;
    c->prepared = false;
    if( c->retired || c->generation != generation ) { c->destroyConnection(); return; }
    if( result < 0 || wl_display_get_error( c->display ) ) { fail( "Wayland display error" ); return; }
    if( !dispatchPending( c, generation ) ) return;
    if( flushRequests() && live() ) displayRead->setEnabled( true );
  }
  void dispatchDisplayWritable()
  {
    assertThread();
    if( ownerStopped || !connection || connection->retired ) return;
    flushRequests();
  }
  bool bindSeat( Seat const & candidate )
  {
    seat = static_cast<wl_seat *>( wl_registry_bind( registry, candidate.name, &wl_seat_interface,
                                                    std::min( candidate.version, 2u ) ) );
    if( !seat ) { fail( "cannot bind wl_seat" ); return false; }
    seatNameId = candidate.name;
    static wl_seat_listener const listener{ seatCapabilities, seatName };
    wl_seat_add_listener( seat, &listener, this );
    return flushRequests();
  }
  void maybeAvailable()
  {
    if( !live() ) return;
    if( manager && seat && !device )
    {
      device = ext_data_control_manager_v1_get_data_device( manager, seat );
      if( !device ) { fail( "cannot create ext_data_control_device_v1" ); return; }
      static ext_data_control_device_v1_listener const listener{ dataOffer, selection, deviceFinished, primarySelection };
      ext_data_control_device_v1_add_listener( device, &listener, this );
      if( !flushRequests() ) return;
    }
    if( discoveryComplete && device && current == State::Starting )
    { current = State::Available; explanation.clear(); publishState(); }
  }
  static void registryGlobal( void * data, wl_registry * registry,
                              uint32_t name, char const * interface, uint32_t version )
  {
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    if( std::strcmp( interface, "ext_data_control_manager_v1" ) == 0 && !self->manager )
    {
      self->manager = static_cast<ext_data_control_manager_v1 *>( wl_registry_bind(
        registry, name, &ext_data_control_manager_v1_interface, 1 ) );
      if( !self->manager ) { self->fail( "cannot bind ext_data_control_manager_v1" ); return; }
      self->managerName = name;
      if( !self->flushRequests() ) return;
    }
    else if( std::strcmp( interface, "wl_seat" ) == 0 )
    {
      self->seats.append( {name, version} );
      if( self->seats.size() > 1 && !self->multipleSeatsLogged )
      { self->multipleSeatsLogged = true; gdDebug( "selection-source: multiple seats; watching first seat" ); }
      if( !self->seat && !self->bindSeat( self->seats.first() ) ) return;
    }
    self->maybeAvailable();
  }
  static void registryGlobalRemoved( void * data, wl_registry * registry, uint32_t name )
  {
    Q_UNUSED( registry )
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    if( name == self->managerName ) { self->fail( "manager removed" ); return; }
    self->seats.erase( std::remove_if( self->seats.begin(), self->seats.end(),
                       [name]( Seat const & value ) { return value.name == name; } ), self->seats.end() );
    if( name != self->seatNameId ) return;
    self->retirePrimary();
    self->destroyDeviceAndSeat();
    if( self->seats.isEmpty() ) { self->fail( "seat removed" ); return; }
    if( !self->bindSeat( self->seats.first() ) ) return;
    self->answerInterest( {}, 0 );
    self->maybeAvailable();
  }
  static void discoveryDone( void * data, wl_callback * callback, uint32_t callbackData )
  {
    Q_UNUSED( callbackData )
    auto * self = static_cast<Impl *>( data ); self->assertThread();
    wl_callback_destroy( callback ); self->sync = nullptr;
    if( !self->live() ) return;
    self->discoveryComplete = true;
    if( !self->manager ) { self->fail( "no ext_data_control_manager_v1" ); return; }
    self->maybeAvailable();
  }
  static void dataOffer( void * data, ext_data_control_device_v1 * device,
                         ext_data_control_offer_v1 * offer )
  {
    Q_UNUSED( device )
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    self->offers.insert( offer, { ++self->nextOfferId, {} } );
    static ext_data_control_offer_v1_listener const listener{ offeredMime };
    ext_data_control_offer_v1_add_listener( offer, &listener, self );
  }
  static void offeredMime( void * data, ext_data_control_offer_v1 * offer, char const * mimeType )
  {
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    auto it = self->offers.find( offer );
    if( it != self->offers.end() ) it.value().mimes.append( QString::fromUtf8( mimeType ) );
  }
  static void selection( void * data, ext_data_control_device_v1 * device,
                         ext_data_control_offer_v1 * offer )
  {
    Q_UNUSED( device )
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    Offer const candidate = self->offers.value( offer, {0,{}} );
    self->offersState.pendingOffer( candidate.id, candidate.mimes );
    auto const discarded = self->offersState.discardForClipboard();
    self->destroyOffer( discarded.id );
    self->flushRequests();
  }
  static void primarySelection( void * data, ext_data_control_device_v1 * device,
                                ext_data_control_offer_v1 * offer )
  {
    qint64 const receivedAt = Monotonic::nowMs();
    Q_UNUSED( device )
    auto * self = static_cast<Impl *>( data ); self->assertThread(); if( !self->live() ) return;
    if( !self->primarySeen )
    {
      self->primarySeen = true;
      gdDebug( "selection-source: primary-selection-seen=1" );
      if( self->current == State::Available ) emit self->q->capabilityChanged();
    }
    Offer const candidate = self->offers.value( offer, {0,{}} );
    self->retirePrimary();
    self->offersState.pendingOffer( candidate.id, candidate.mimes );
    self->primaryId = self->offersState.takeForPrimary().id;
    self->offerStamp = receivedAt;
    if( !self->flushRequests() ) return;
    if( !offer ) { self->expireInterest(); self->answerInterest( self->cache, self->cacheStamp ); return; }
    if( !self->primaryId || chooseMimeType( candidate.mimes ).isEmpty() )
    {
      gdDebug( "selection-ignored reason=no-text-mime" );
      self->dropCapture( false ); return;
    }
    if( self->interestedId ) { self->expireInterest(); self->promote(); }
    else self->debounce.start( DebounceMs );
  }
  static void deviceFinished( void * data, ext_data_control_device_v1 * device )
  {
    Q_UNUSED( device )
    auto * self = static_cast<Impl *>( data ); self->assertThread();
    if( self->live() ) self->fail( "device finished" );
  }
  static void seatCapabilities( void * data, wl_seat * seat, uint32_t capabilities )
  {
    Q_UNUSED( seat ) Q_UNUSED( capabilities )
    static_cast<Impl *>( data )->assertThread();
  }
  static void seatName( void * data, wl_seat * seat, char const * name )
  {
    Q_UNUSED( seat ) Q_UNUSED( name )
    static_cast<Impl *>( data )->assertThread();
  }
  void dropCapture( bool timeout )
  {
    if( timeout ) gdDebug( "selection-ignored reason=timeout" );
    retirePrimary();
    flushRequests();
    expireInterest();
    answerInterest( {}, 0 );
  }
  void promote()
  {
    if( !live() || !primaryId || readToken ) return;
    debounce.stop();
    if( !transferStartedAt )
    {
      transferStartedAt = Monotonic::nowMs();
      transferDeadline = transferStartedAt + TransferTimeoutMs;
    }
    startMimeAttempt();
  }
  bool startMimeAttempt()
  {
    assertThread();
    if( !live() || !primaryId ) return false;
    if( Monotonic::nowMs() >= transferDeadline ) { dropCapture( true ); return false; }
    if( !offersState.beginMimeAttempt() ) { dropCapture( false ); return false; }
    int fds[2] = { -1, -1 };
    if( pipe2( fds, O_CLOEXEC ) != 0 )
    { gdDebug( "selection-transfer-error operation=pipe errno=%d", errno ); dropCapture( false ); return false; }
    int const oldFlags = fcntl( fds[0], F_GETFL );
    if( oldFlags < 0 || fcntl( fds[0], F_SETFL, oldFlags | O_NONBLOCK ) < 0 )
    {
      int const error = errno; close( fds[0] ); close( fds[1] );
      gdDebug( "selection-transfer-error operation=fcntl errno=%d", error ); dropCapture( false ); return false;
    }
    readEnd = fds[0]; bytesObserved = 0; payload = PayloadAccumulator{};
    readToken = offersState.beginRead();
    ReadToken const token = readToken;
    lastProgressAt = Monotonic::nowMs();
    QByteArray const mime = offersState.currentMimeType().toUtf8();
    ext_data_control_offer_v1_receive( proxyFor( primaryId ), mime.constData(), fds[1] );
    close( fds[1] );
    if( !flushRequests() ) return false;
    pipeRead = std::make_unique<QSocketNotifier>( readEnd, QSocketNotifier::Read );
    QObject::connect( pipeRead.get(), &QSocketNotifier::activated, q, [this, token] { drainPipe( token ); } );
    QObject::connect( &transferWake, &QTimer::timeout, q, [this, token] { checkTransferDeadline( token ); } );
    checkTransferDeadline( token );
    return offersState.accepts( token );
  }
  void checkTransferDeadline( ReadToken token )
  {
    assertThread();
    if( ownerStopped || !offersState.accepts( token ) ) return;
    qint64 const now = Monotonic::nowMs();
    if( now >= transferDeadline ) { dropCapture( true ); return; }
    if( now >= lastProgressAt + IdleTimeoutMs )
    {
      if( bytesObserved == 0 )
      {
        offersState.mimeAttemptFailed(); closeRead();
        if( !offersState.beginMimeAttempt() ) { dropCapture( true ); return; }
        startMimeAttempt();
      }
      else dropCapture( true );
      return;
    }
    transferWake.start( int( std::max<qint64>( 1, std::min( transferDeadline, lastProgressAt + IdleTimeoutMs ) - now ) ) );
  }
  void complete( ReadToken token )
  {
    if( !offersState.accepts( token ) ) { gdDebug( "selection-ignored reason=stale-token" ); return; }
    checkTransferDeadline( token );
    if( !offersState.accepts( token ) ) return;
    cache = decodePayload( payload.payload() );
    cacheStamp = cache.isEmpty() ? 0 : offerStamp;
    gdDebug( "selection-captured mime=%s bytes=%lld truncated=%d", offersState.currentMimeType().toUtf8().constData(),
             static_cast<long long>( payload.payload().size() ), payload.truncated() ? 1 : 0 );
    offersState.completed( token );
    closeRead();
    quint64 const id = std::exchange( primaryId, 0 );
    destroyOffer( id );
    transferStartedAt = 0; transferDeadline = 0;
    flushRequests();
    expireInterest();
    answerInterest( cache, cacheStamp );
  }
  void drainPipe( ReadToken token )
  {
    assertThread();
    if( ownerStopped ) return;
    if( !offersState.accepts( token ) ) { gdDebug( "selection-ignored reason=stale-token" ); return; }
    pipeRead->setEnabled( false );
    for( ;; )
    {
      checkTransferDeadline( token );
      if( !offersState.accepts( token ) ) return;
      char buffer[16384];
      int const remaining = MaxPayloadBytes + 1 - bytesObserved;
      ssize_t const count = ::read( readEnd, buffer, std::min<int>( sizeof buffer, remaining ) );
      if( count > 0 )
      {
        payload.append( QByteArray( buffer, count ) );
        bytesObserved += int( count ); lastProgressAt = Monotonic::nowMs();
        if( bytesObserved == MaxPayloadBytes + 1 ) { complete( token ); return; }
        continue;
      }
      if( count == 0 ) { complete( token ); return; }
      if( errno == EINTR ) continue;
      if( errno == EAGAIN || errno == EWOULDBLOCK )
      { checkTransferDeadline( token ); if( offersState.accepts( token ) ) pipeRead->setEnabled( true ); return; }
      int const error = errno;
      gdDebug( "selection-transfer-error operation=read errno=%d", error );
      if( bytesObserved == 0 )
      { offersState.mimeAttemptFailed(); closeRead(); startMimeAttempt(); }
      else dropCapture( false );
      return;
    }
  }
#endif // HAVE_WAYLAND_DATA_CONTROL
};
WaylandSelectionSource::WaylandSelectionSource( QObject * parent ) : QObject( parent ), d( std::make_unique<Impl>( this ) ) {}
WaylandSelectionSource::~WaylandSelectionSource() { stop(); }
WaylandSelectionSource::State WaylandSelectionSource::state() const noexcept { return d->current; }
QString WaylandSelectionSource::reason() const { return d->explanation; }
bool WaylandSelectionSource::primarySelectionSeen() const noexcept { return d->primarySeen; }
QString WaylandSelectionSource::lastText() const { return d->cache; }
bool WaylandSelectionSource::hasPendingCapture() const noexcept { return d->pending(); }
void WaylandSelectionSource::start() { d->start(); }
void WaylandSelectionSource::stop() { d->stop(); }
void WaylandSelectionSource::requestCapture( quint64 requestId ) { d->requestCapture( requestId ); }
