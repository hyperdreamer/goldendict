#!/bin/sh
set -eu
[ "$#" -eq 0 ] || { echo 'wayland-selection-live: no arguments accepted' >&2; exit 1; }
for tool in qmake6 cc c++ make pkg-config kwin_wayland dbus-run-session timeout python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "wayland-selection-live: missing prerequisite $tool" >&2; exit 1; }
done
pkg-config --exists wayland-client || { echo 'wayland-selection-live: missing prerequisite wayland-client' >&2; exit 1; }
if [ "${GD_SELECTION_LIVE_SESSION:-0}" != 1 ]; then
    export GD_SELECTION_LIVE_SESSION=1
    exec timeout 120 dbus-run-session -- "$0"
fi
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d /tmp/gd-selection-live.XXXXXXXX)
cleanup() { rm -rf -- "$tmp"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
mkdir -m 700 "$tmp/runtime" "$tmp/config" "$tmp/cache" "$tmp/data"
export XDG_RUNTIME_DIR="$tmp/runtime" XDG_CONFIG_HOME="$tmp/config"
export XDG_CACHE_HOME="$tmp/cache" XDG_DATA_HOME="$tmp/data"
cat > "$tmp/selection-live.pro" <<'GD_PRO'
TEMPLATE = app
TARGET = selection-live
CONFIG += c++17 console link_pkgconfig
QT = core gui testlib
PKGCONFIG += wayland-client
DEFINES += HAVE_WAYLAND_DATA_CONTROL
INCLUDEPATH += $$GD_REPO_ROOT $$GD_REPO_ROOT/protocols/ext-data-control-v1
SOURCES += $$GD_REPO_ROOT/waylandselection.cc \
           $$GD_REPO_ROOT/waylandselectionlogic.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           $$GD_REPO_ROOT/protocols/ext-data-control-v1/ext-data-control-v1-protocol.c \
           selection-live.cc
HEADERS += $$GD_REPO_ROOT/waylandselection.hh \
           $$GD_REPO_ROOT/waylandselectionlogic.hh
GD_PRO
cat > "$tmp/selection-live.cc" <<'GD_CPP'
#include "waylandselection.hh"
#include "waylandselectionlogic.hh"
#include "ext-data-control-v1-client-protocol.h"
#include <wayland-client.h>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QTimer>
#include <QtTest>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <unistd.h>
#include <vector>
using WaylandSelection::MaxPayloadBytes;
static QString const Utf8 = "text/plain;charset=utf-8";
static QString const Plain = "text/plain";
static void jsonOut( QJsonObject const & object )
{
  QByteArray const bytes = QJsonDocument( object ).toJson( QJsonDocument::Compact ) + '\n';
  std::fwrite( bytes.constData(), 1, size_t( bytes.size() ), stdout ); std::fflush( stdout );
}
class Helper : public QObject
{
  struct Source;
  struct Writer { int fd=-1, serial=0; QString mime; qint64 received=0, bytes=0; ~Writer(){ if(fd>=0) ::close(fd); } };
  struct Source { Helper * helper; int serial; ext_data_control_source_v1 * proxy; QJsonObject schedule; };
  struct Ack { int id; };
  wl_display * display=nullptr;
  wl_registry * registry=nullptr;
  ext_data_control_manager_v1 * manager=nullptr;
  wl_seat * seat=nullptr;
  ext_data_control_device_v1 * device=nullptr;
  std::unique_ptr<QSocketNotifier> displayNotifier, stdinNotifier;
  QByteArray commands;
  std::vector<std::unique_ptr<Source>> sources;
  std::vector<std::shared_ptr<Writer>> writers;
  static void global( void * data, wl_registry * r, uint32_t name, char const * interface, uint32_t version )
  {
    auto * h=static_cast<Helper *>(data);
    if(!h->manager && std::strcmp(interface,"ext_data_control_manager_v1")==0)
      h->manager=static_cast<ext_data_control_manager_v1 *>(wl_registry_bind(r,name,&ext_data_control_manager_v1_interface,1));
    else if(!h->seat && std::strcmp(interface,"wl_seat")==0)
    {
      h->seat=static_cast<wl_seat *>(wl_registry_bind(r,name,&wl_seat_interface,std::min(version,2u)));
      static wl_seat_listener const listener{
        [](void*,wl_seat*,uint32_t){}, [](void*,wl_seat*,char const*){} };
      wl_seat_add_listener(h->seat,&listener,h);
    }
  }
  static void removed(void*,wl_registry*,uint32_t) {}
  static void ackDone( void * data, wl_callback * callback, uint32_t )
  {
    std::unique_ptr<Ack> ack(static_cast<Ack *>(data)); wl_callback_destroy(callback);
    jsonOut({{"event","ack"},{"id",ack->id}});
  }
  void closeWriter( std::shared_ptr<Writer> const & w )
  {
    if(w->fd<0) return;
    ::close(w->fd); w->fd=-1; jsonOut({{"event","closed"},{"serial",w->serial},{"mime",w->mime},{"bytes",double(w->bytes)}});
  }
  void writeChunk( std::shared_ptr<Writer> const & w, QByteArray const & bytes )
  {
    if(w->fd<0) return;
    qsizetype sent=0;
    while(sent<bytes.size())
    {
      ssize_t const n=::write(w->fd,bytes.constData()+sent,size_t(bytes.size()-sent));
      if(n>0) { sent+=n; w->bytes+=n; continue; }
      if(n<0 && errno==EINTR) continue;
      int const error=n<0?errno:EIO;
      jsonOut({{"event","write-error"},{"serial",w->serial},{"mime",w->mime},{"errno",error},{"bytes",int(sent)},{"totalBytes",double(w->bytes)}});
      closeWriter(w); return;
    }
    jsonOut({{"event","write"},{"serial",w->serial},{"mime",w->mime},{"bytes",int(sent)},{"totalBytes",double(w->bytes)}});
  }
  static void send( void * data, ext_data_control_source_v1 *, char const * mime, int fd )
  {
    auto * source=static_cast<Source *>(data); auto * h=source->helper;
    auto w=std::make_shared<Writer>(); w->fd=fd; w->serial=source->serial; w->mime=QString::fromUtf8(mime); w->received=Monotonic::nowMs();
    h->writers.push_back(w);
    int const flags=fcntl(fd,F_GETFL);
    jsonOut({{"event","receive"},{"serial",source->serial},{"mime",QString::fromUtf8(mime)},
             {"fdFlags",flags},{"nonblock",bool(flags&O_NONBLOCK)},{"bytes",0},{"receiveTimeMs",double(w->received)}});
    QJsonObject const schedule=source->schedule.value(QString::fromUtf8(mime)).toObject();
    for(auto const & item:schedule.value("chunks").toArray())
    {
      auto const chunk=item.toObject(); QByteArray bytes;
      if(chunk.contains("repeat")) bytes=QByteArray(chunk.value("count").toInt(),chunk.value("repeat").toString().toLatin1().at(0));
      else bytes=chunk.value("data").toString().toUtf8();
      QTimer::singleShot(chunk.value("at").toInt(),h,[h,w,bytes]{h->writeChunk(w,bytes);});
    }
    int const end=schedule.value("close").toInt(-1);
    if(end>=0) QTimer::singleShot(end,h,[h,w]{h->closeWriter(w);});
  }
  static void cancelled( void * data, ext_data_control_source_v1 * proxy )
  {
    auto * source=static_cast<Source *>(data);
    ext_data_control_source_v1_destroy(proxy); source->proxy=nullptr;
    // Existing writer schedules intentionally survive supersession to observe EPIPE.
  }
  bool command( QJsonObject const & c )
  {
    QString const action=c.value("command").toString(); int const id=c.value("id").toInt();
    if(action=="quit") { QCoreApplication::quit(); return true; }
    if(action=="release")
    {
      for(auto const & w:writers) if(w->serial==c.value("serial").toInt()) closeWriter(w);
      jsonOut({{"event","ack"},{"id",id}}); return true;
    }
    if(action!="set" && action!="clear") return false;
    ext_data_control_source_v1 * proxy=nullptr;
    if(action=="set")
    {
      auto value=std::make_unique<Source>(); value->helper=this; value->serial=c.value("serial").toInt();
      value->schedule=c.value("schedule").toObject();
      value->proxy=ext_data_control_manager_v1_create_data_source(manager); proxy=value->proxy;
      if(!proxy) return false;
      static ext_data_control_source_v1_listener const listener{send,cancelled};
      ext_data_control_source_v1_add_listener(proxy,&listener,value.get());
      for(auto const & mime:c.value("mimes").toArray())
        ext_data_control_source_v1_offer(proxy,mime.toString().toUtf8().constData());
      sources.push_back(std::move(value));
    }
    if(c.value("channel").toString()=="primary") ext_data_control_device_v1_set_primary_selection(device,proxy);
    else ext_data_control_device_v1_set_selection(device,proxy);
    wl_callback * sync=wl_display_sync(display); if(!sync) return false;
    static wl_callback_listener const listener{ackDone};
    wl_callback_add_listener(sync,&listener,new Ack{id});
    return wl_display_flush(display)>=0 || errno==EAGAIN;
  }
public:
  ~Helper() override
  {
    for(auto const & w:writers) closeWriter(w);
    if(displayNotifier) displayNotifier->setEnabled(false);
    for(auto const & s:sources) if(s->proxy) ext_data_control_source_v1_destroy(s->proxy);
    if(device) ext_data_control_device_v1_destroy(device);
    if(seat) wl_seat_destroy(seat);
    if(manager) ext_data_control_manager_v1_destroy(manager);
    if(registry) wl_registry_destroy(registry);
    if(display) wl_display_disconnect(display);
  }
  bool init()
  {
    std::signal(SIGPIPE,SIG_IGN);
    display=wl_display_connect(nullptr); if(!display) return false;
    registry=wl_display_get_registry(display); if(!registry) return false;
    static wl_registry_listener const listener{global,removed}; wl_registry_add_listener(registry,&listener,this);
    // Helper-only discovery may block; it runs in its own child and has the outer wall watchdog.
    if(wl_display_roundtrip(display)<0 || !manager || !seat) return false;
    device=ext_data_control_manager_v1_get_data_device(manager,seat); if(!device) return false;
    static ext_data_control_device_v1_listener const deviceListener{
      [](void*,ext_data_control_device_v1*,ext_data_control_offer_v1 * offer){
        static ext_data_control_offer_v1_listener const l{[](void*,ext_data_control_offer_v1*,char const*){}};
        ext_data_control_offer_v1_add_listener(offer,&l,nullptr);
      },
      [](void*,ext_data_control_device_v1*,ext_data_control_offer_v1 * offer){if(offer) ext_data_control_offer_v1_destroy(offer);},
      [](void*,ext_data_control_device_v1*){QCoreApplication::exit(1);},
      [](void*,ext_data_control_device_v1*,ext_data_control_offer_v1 * offer){if(offer) ext_data_control_offer_v1_destroy(offer);}
    };
    ext_data_control_device_v1_add_listener(device,&deviceListener,this);
    displayNotifier=std::make_unique<QSocketNotifier>(wl_display_get_fd(display),QSocketNotifier::Read);
    connect(displayNotifier.get(),&QSocketNotifier::activated,this,[this]{
      if(wl_display_dispatch(display)<0) QCoreApplication::exit(1);
      else if(wl_display_flush(display)<0 && errno!=EAGAIN) QCoreApplication::exit(1);
    });
    int const flags=fcntl(STDIN_FILENO,F_GETFL); if(flags<0 || fcntl(STDIN_FILENO,F_SETFL,flags|O_NONBLOCK)<0) return false;
    stdinNotifier=std::make_unique<QSocketNotifier>(STDIN_FILENO,QSocketNotifier::Read);
    connect(stdinNotifier.get(),&QSocketNotifier::activated,this,[this]{
      char buffer[4096]; ssize_t n=::read(STDIN_FILENO,buffer,sizeof buffer);
      if(n==0) {QCoreApplication::quit();return;}
      if(n<0) {if(errno!=EINTR && errno!=EAGAIN) QCoreApplication::exit(1);return;}
      commands.append(buffer,n);
      for(qsizetype end;(end=commands.indexOf('\n'))>=0;)
      {
        QJsonParseError error; auto const document=QJsonDocument::fromJson(commands.left(end),&error); commands.remove(0,end+1);
        if(error.error!=QJsonParseError::NoError || !document.isObject() || !command(document.object()))
        {std::fprintf(stderr,"helper command/protocol failure\n");QCoreApplication::exit(1);return;}
      }
    });
    if(wl_display_flush(display)<0 && errno!=EAGAIN) return false;
    jsonOut({{"event","ready"}}); return true;
  }
};
#define REQUIRE(x) do { if(!(x)) { std::fprintf(stderr,"assertion line %d: %s\n",__LINE__,#x); return false; } } while(false)
class Live : public QObject
{
public:
  struct Answer { quint64 id; QString text; qint64 stamp, at; };
  QProcess helper;
  QByteArray incoming;
  QList<QJsonObject> events;
  QStringList logs;
  QList<Answer> answers;
  std::unique_ptr<WaylandSelectionSource> source;
  std::unique_ptr<QSignalSpy> stateSpy, capabilitySpy;
  std::function<void(QString const &)> logHook;
  int commandId=0;
  bool wait( std::function<bool()> const & predicate, int ms=5000 )
  {
    qint64 const deadline=Monotonic::nowMs()+ms;
    while(!predicate() && Monotonic::nowMs()<deadline)
    {QCoreApplication::processEvents(QEventLoop::AllEvents,5);QTest::qWait(1);}
    return predicate();
  }
  bool until(qint64 time) {return wait([=]{return Monotonic::nowMs()>=time;},int(std::max<qint64>(1,time-Monotonic::nowMs()+100)));}
  int send(QJsonObject command)
  {
    int const id=++commandId; command.insert("id",id);
    helper.write(QJsonDocument(command).toJson(QJsonDocument::Compact)+'\n');return id;
  }
  bool ack(int id)
  {
    return wait([&]{for(auto const & e:events) if(e.value("event")=="ack" && e.value("id").toInt()==id) return true;return false;});
  }
  bool clear(QString const & channel) {return ack(send({{"command","clear"},{"channel",channel}}));}
  bool set(int serial, QJsonObject const & schedule, QStringList const & mimes={Utf8}, QString const & channel="primary")
  {
    QJsonArray advertised;for(auto const & mime:mimes)advertised.append(mime);
    return ack(send({{"command","set"},{"serial",serial},{"channel",channel},{"mimes",advertised},{"schedule",schedule}}));
  }
  static QJsonObject transfer(QJsonArray const & chunks,int close) {return {{"chunks",chunks},{"close",close}};}
  static QJsonObject text(QString const & value,int close=0) {return {{Utf8,transfer({QJsonObject{{"at",0},{"data",value}}},close)}};}
  QList<QJsonObject> receives(int serial) const
  {QList<QJsonObject> r;for(auto const & e:events)if(e.value("event")=="receive"&&e.value("serial").toInt()==serial)r.append(e);return r;}
  bool event(int serial,QString const & name) const
  {for(auto const & e:events)if(e.value("event")==name&&e.value("serial").toInt()==serial)return true;return false;}
  int captures() const {int n=0;for(auto const & l:logs)if(l.startsWith("selection-captured "))++n;return n;}
  QList<Answer> forId(quint64 id) const {QList<Answer> r;for(auto const & a:answers)if(a.id==id)r.append(a);return r;}
  bool init()
  {
    connect(&helper,&QProcess::readyReadStandardOutput,this,[this]{
      incoming+=helper.readAllStandardOutput();
      for(qsizetype end;(end=incoming.indexOf('\n'))>=0;)
      {
        QJsonParseError error;auto doc=QJsonDocument::fromJson(incoming.left(end),&error);incoming.remove(0,end+1);
        if(error.error!=QJsonParseError::NoError||!doc.isObject()) {QCoreApplication::exit(1);return;}
        auto e=doc.object();e.insert("_at",double(Monotonic::nowMs()));events.append(e);
      }
    });
    helper.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    helper.start(QCoreApplication::applicationFilePath(),{"--helper"});
    return wait([&]{for(auto const & e:events)if(e.value("event")=="ready")return true;return false;});
  }
  ~Live() override
  {
    source.reset();
    if(helper.state()!=QProcess::NotRunning)
    {
      send({{"command","quit"}});
      if(!helper.waitForFinished(500)){helper.kill();helper.waitForFinished(1000);}
    }
  }
  bool reset(bool start=true)
  {
    source.reset();stateSpy.reset();capabilitySpy.reset();logHook={};
    REQUIRE(clear("primary")); REQUIRE(clear("clipboard"));
    events.clear();logs.clear();answers.clear();
    source=std::make_unique<WaylandSelectionSource>();
    stateSpy=std::make_unique<QSignalSpy>(source.get(),&WaylandSelectionSource::stateChanged);
    capabilitySpy=std::make_unique<QSignalSpy>(source.get(),&WaylandSelectionSource::capabilityChanged);
    connect(source.get(),&WaylandSelectionSource::captureFinished,this,[this](quint64 id,QString const & text,qint64 stamp){answers.append({id,text,stamp,Monotonic::nowMs()});});
    if(start)
    {
      source->start();source->start();
      REQUIRE(wait([&]{return source->state()==WaylandSelectionSource::State::Available && source->primarySelectionSeen();}));
    }
    return true;
  }
  bool sourceBecomesAvailable()
  {
    REQUIRE(reset()); REQUIRE(source->reason().isEmpty());REQUIRE(source->primarySelectionSeen());
    REQUIRE(logs.count("selection-source: state=Starting reason=")==1);
    REQUIRE(logs.count("selection-source: state=Available reason=")==1);
    REQUIRE(logs.count("selection-source: primary-selection-seen=1")==1);
    REQUIRE(logs.indexOf("selection-source: state=Starting reason=")<logs.indexOf("selection-source: state=Available reason="));
    REQUIRE(stateSpy->isValid()&&capabilitySpy->isValid());
    int available=0;for(auto const & s:*stateSpy)if(qvariant_cast<WaylandSelectionSource::State>(s[0])==WaylandSelectionSource::State::Available)++available;
    REQUIRE(available==1);return true;
  }
  bool multiChunkCompletesAtEof()
  {
    REQUIRE(reset());
    REQUIRE(set(2,{{Utf8,transfer({QJsonObject{{"at",0},{"data","first-"}},QJsonObject{{"at",40},{"data","second-"}},QJsonObject{{"at",80},{"data","third"}}},120)}}));
    REQUIRE(wait([&]{return receives(2).size()==1;}));qint64 const t=qint64(receives(2)[0].value("_at").toDouble());
    REQUIRE(!receives(2)[0].value("nonblock").toBool());
    REQUIRE(until(t+60));REQUIRE(source->lastText().isEmpty()&&captures()==0);
    REQUIRE(until(t+100));REQUIRE(source->lastText().isEmpty()&&captures()==0);
    REQUIRE(wait([&]{return source->lastText()=="first-second-third";}));
    source->requestCapture(1);REQUIRE(forId(1).size()==1);
    REQUIRE(forId(1)[0].text=="first-second-third"&&forId(1)[0].stamp>0);
    REQUIRE(captures()==1);REQUIRE(logs.contains("selection-captured mime=text/plain;charset=utf-8 bytes=18 truncated=0"));return true;
  }
  bool tenSelectionsDebounceToOneRead()
  {
    REQUIRE(reset());
    for(int i=0;i<10;++i)
    {REQUIRE(set(100+i,text("burst-"+QString::number(i))));REQUIRE(until(Monotonic::nowMs()+10));}
    REQUIRE(wait([&]{return source->lastText()=="burst-9"&&!source->hasPendingCapture();}));
    for(int i=0;i<9;++i)REQUIRE(receives(100+i).isEmpty());
    REQUIRE(receives(109).size()==1);source->requestCapture(1);REQUIRE(forId(1).size()==1);
    auto const a=forId(1)[0];REQUIRE(a.text=="burst-9"&&a.stamp>0);
    REQUIRE(qint64(receives(109)[0].value("_at").toDouble())-a.stamp>=200);return true;
  }
  bool overCapIsTruncated()
  {
    REQUIRE(reset());REQUIRE(set(4,{{Utf8,transfer({QJsonObject{{"at",0},{"repeat","x"},{"count",MaxPayloadBytes+8192}}},0)}}));
    REQUIRE(wait([&]{return source->lastText().size()==MaxPayloadBytes;}));
    REQUIRE(source->lastText()==QString(MaxPayloadBytes,'x'));
    REQUIRE(logs.contains("selection-captured mime=text/plain;charset=utf-8 bytes=1048576 truncated=1"));return true;
  }
  bool exactCapIsNotTruncated()
  {
    REQUIRE(reset());REQUIRE(set(5,{{Utf8,transfer({QJsonObject{{"at",0},{"repeat","y"},{"count",MaxPayloadBytes}}},0)}}));
    REQUIRE(wait([&]{return source->lastText().size()==MaxPayloadBytes;}));
    REQUIRE(source->lastText()==QString(MaxPayloadBytes,'y'));
    REQUIRE(logs.contains("selection-captured mime=text/plain;charset=utf-8 bytes=1048576 truncated=0"));return true;
  }
  bool supersededCaptureCannotPublish()
  {
    REQUIRE(reset());REQUIRE(set(61,{{Utf8,transfer({QJsonObject{{"at",0},{"data","old"}},QJsonObject{{"at",350},{"data","-late"}}},400)}}));
    REQUIRE(wait([&]{return source->hasPendingCapture();}));source->requestCapture(1);
    REQUIRE(wait([&]{return receives(61).size()==1&&event(61,"write");}));
    REQUIRE(set(62,text("new-value")));REQUIRE(wait([&]{return source->lastText()=="new-value";}));
    REQUIRE(until(Monotonic::nowMs()+450));
    REQUIRE(source->lastText()=="new-value");REQUIRE(source->state()==WaylandSelectionSource::State::Available);
    REQUIRE(captures()==1);REQUIRE(event(61,"write-error")||event(61,"closed"));
    source->requestCapture(2);REQUIRE(forId(2).size()==1&&forId(2)[0].text=="new-value"&&forId(2)[0].stamp>0);
    REQUIRE(forId(1).size()==1&&forId(1)[0].text=="new-value");return true;
  }
  bool receiptStampLetsNewCopyWin()
  {
    REQUIRE(reset(false));REQUIRE(set(71,text("selected-before-copy")));
    qint64 copyStamp=0;int copyAck=0;bool beforeReceive=false;
    auto clearHook=qScopeGuard([&]{logHook={};});
    logHook=[&](QString const & line){if(line=="selection-source: primary-selection-seen=1")QTimer::singleShot(10,this,[&]{
      copyStamp=Monotonic::nowMs();beforeReceive=receives(71).isEmpty();
      QJsonArray mimes{Utf8};copyAck=send({{"command","set"},{"channel","clipboard"},{"serial",72},{"mimes",mimes},{"schedule",text("copied-after-selection")}});
    });};
    source->start();
    REQUIRE(wait([&]{return copyStamp>0&&source->lastText()=="selected-before-copy"&&!source->hasPendingCapture();}));
    REQUIRE(copyAck>0&&ack(copyAck));REQUIRE(beforeReceive);REQUIRE(receives(71).size()==1&&receives(72).isEmpty());
    source->requestCapture(1);REQUIRE(forId(1).size()==1);auto const a=forId(1)[0];
    REQUIRE(a.stamp>0&&a.stamp<copyStamp);
    REQUIRE(copyStamp<qint64(receives(71)[0].value("_at").toDouble()));
    REQUIRE(WaylandSelection::chooseText(a.text,a.stamp,"copied-after-selection",copyStamp)=="copied-after-selection");return true;
  }
  bool zeroByteIdleRetriesNextMime()
  {
    REQUIRE(reset());REQUIRE(set(8,{{Utf8,transfer({},-1)},{Plain,transfer({QJsonObject{{"at",0},{"data","retry-success"}}},0)}},{Utf8,Plain}));
    REQUIRE(wait([&]{return source->lastText()=="retry-success"&&!source->hasPendingCapture();}));
    auto const r=receives(8);REQUIRE(r.size()==2&&r[0].value("mime")==Utf8&&r[1].value("mime")==Plain);
    REQUIRE(logs.contains("selection-captured mime=text/plain bytes=13 truncated=0"));
    source->requestCapture(1);REQUIRE(forId(1).size()==1&&forId(1)[0].text=="retry-success");return true;
  }
  bool partialIdleRetiresWithoutRetry()
  {
    REQUIRE(reset());REQUIRE(set(901,text("old-cache")));REQUIRE(wait([&]{return source->lastText()=="old-cache"&&!source->hasPendingCapture();}));
    REQUIRE(set(9,{{Utf8,transfer({QJsonObject{{"at",0},{"data","incomplete"}}},-1)},{Plain,transfer({QJsonObject{{"at",0},{"data","must-not-read"}}},0)}},{Utf8,Plain}));
    REQUIRE(wait([&]{return source->hasPendingCapture();}));source->requestCapture(1);
    REQUIRE(wait([&]{return receives(9).size()==1;}));qint64 const t=qint64(receives(9)[0].value("_at").toDouble());
    REQUIRE(wait([&]{return !source->hasPendingCapture();},300));
    REQUIRE(source->lastText()=="old-cache"&&forId(1).size()==1&&forId(1)[0].stamp==0&&forId(1)[0].text.isEmpty());
    REQUIRE(until(t+600));REQUIRE(receives(9).size()==1);REQUIRE(logs.contains("selection-ignored reason=timeout"));return true;
  }
  bool absoluteDeadlineSurvivesProgress()
  {
    REQUIRE(reset());QJsonArray chunks;for(int i=0;i<35;++i)chunks.append(QJsonObject{{"at",i*100},{"data","x"}});
    REQUIRE(set(10,{{Utf8,transfer({},-1)},{Plain,transfer(chunks,-1)}},{Utf8,Plain}));
    REQUIRE(wait([&]{return receives(10).size()>=1;}));qint64 const first=qint64(receives(10)[0].value("_at").toDouble());
    REQUIRE(wait([&]{return receives(10).size()==2;}));REQUIRE(receives(10)[1].value("mime")==Plain);
    REQUIRE(until(first+1800));REQUIRE(source->hasPendingCapture());
    REQUIRE(wait([&]{return !source->hasPendingCapture();},int(std::max<qint64>(1,first+2050-Monotonic::nowMs()))));
    REQUIRE(Monotonic::nowMs()<=first+2050);REQUIRE(source->lastText().isEmpty()&&captures()==0);
    REQUIRE(logs.contains("selection-ignored reason=timeout"));
    REQUIRE(wait([&]{return event(10,"write-error");},200));
    bool ep = false;
    for( auto const & e : events ) if( e.value("event")=="write-error" && e.value("serial").toInt()==10 && e.value("errno").toInt()==EPIPE ) ep = true;
    REQUIRE(ep);return true;
  }
  bool overlappingRequestsReuseTransfer()
  {
    REQUIRE(reset());REQUIRE(set(11,{{Utf8,transfer({QJsonObject{{"at",0},{"data","over-"}},QJsonObject{{"at",150},{"data","lap-"}},QJsonObject{{"at",300},{"data","result"}}},350)}}));
    REQUIRE(wait([&]{return source->hasPendingCapture();}));qint64 const first=Monotonic::nowMs();source->requestCapture(101);
    REQUIRE(wait([&]{return receives(11).size()==1;}));qint64 const receive=qint64(receives(11)[0].value("_at").toDouble());
    REQUIRE(std::abs(receive-first)<=10);
    qint64 second=0;
    QTimer::singleShot(int(std::max<qint64>(0,first+100-Monotonic::nowMs())),this,[&]{second=Monotonic::nowMs();source->requestCapture(102);});
    REQUIRE(wait([&]{return second>0&&forId(101).size()==1;}));
    REQUIRE(forId(101)[0].text.isEmpty()&&forId(101)[0].stamp==0);
    REQUIRE(forId(101)[0].at-second<=50);
    REQUIRE(wait([&]{return forId(102).size()==1;}));auto const a=forId(102)[0];
    REQUIRE(a.text=="over-lap-result"&&a.stamp>0);REQUIRE(a.at-second>=240&&a.at-second<300);
    REQUIRE(receives(11).size()==1);REQUIRE(until(first+500));REQUIRE(forId(101).size()==1&&forId(102).size()==1);return true;
  }
  bool activationDeadlineSuppressesCache()
  {
    REQUIRE(reset());REQUIRE(set(121,text("old-cache")));REQUIRE(wait([&]{return source->lastText()=="old-cache"&&!source->hasPendingCapture();}));
    REQUIRE(set(122,{{Utf8,transfer({QJsonObject{{"at",0},{"data","new-"}},QJsonObject{{"at",100},{"data","pend"}},QJsonObject{{"at",200},{"data","ing"}}},400)}}));
    REQUIRE(wait([&]{return source->hasPendingCapture();}));qint64 const called=Monotonic::nowMs();source->requestCapture(201);
    REQUIRE(wait([&]{return forId(201).size()==1;},350));auto const a=forId(201)[0];
    REQUIRE(a.at-called<=350&&a.text.isEmpty()&&a.stamp==0);REQUIRE(logs.contains("selection-timeout"));
    REQUIRE(WaylandSelection::chooseText(a.text,a.stamp,"fallback",1)=="fallback");
    REQUIRE(wait([&]{return source->lastText()=="new-pending"&&!source->hasPendingCapture();}));
    REQUIRE(forId(201).size()==1);return true;
  }
  bool stopSuppressesLateCompletion()
  {
    REQUIRE(reset());REQUIRE(set(13,text("held",-1)));REQUIRE(wait([&]{return source->hasPendingCapture();}));source->requestCapture(301);
    REQUIRE(wait([&]{return receives(13).size()==1&&event(13,"write");}));
    int const captured=captures();auto const frozen=source->state();QString const reason=source->reason();
    source->stop();source->stop();REQUIRE(!source->hasPendingCapture());
    REQUIRE(ack(send({{"command","release"},{"serial",13}})));REQUIRE(until(Monotonic::nowMs()+400));
    REQUIRE(forId(301).isEmpty()&&captures()==captured);source->start();source->requestCapture(302);
    REQUIRE(until(Monotonic::nowMs()+50));REQUIRE(forId(302).isEmpty());REQUIRE(receives(13).size()==1);
    REQUIRE(source->state()==frozen&&source->reason()==reason);return true;
  }
};
static Live * observed=nullptr;
static void message(QtMsgType,QMessageLogContext const &,QString const & text)
{
  if(observed&&text.startsWith("selection-"))
  {observed->logs.append(text);if(observed->logHook)observed->logHook(text);}
  QByteArray const bytes=text.toUtf8();std::fprintf(stderr,"%s\n",bytes.constData());
}
int main(int argc,char **argv)
{
  if(argc>1&&std::strcmp(argv[1],"--helper")==0)
  {
    QCoreApplication app(argc,argv);Helper helper;
    if(!helper.init()){std::fprintf(stderr,"helper: missing ext protocol or connection failure\n");return 1;}
    return app.exec();
  }
  QGuiApplication app(argc,argv);qRegisterMetaType<WaylandSelectionSource::State>();
  Live runner;observed=&runner;auto const previous=qInstallMessageHandler(message);
  auto restore=qScopeGuard([&]{observed=nullptr;qInstallMessageHandler(previous);});
  struct Case {char const * name;bool(Live::*run)();};
  Case const cases[]={
    {"sourceBecomesAvailable",&Live::sourceBecomesAvailable},
    {"multiChunkCompletesAtEof",&Live::multiChunkCompletesAtEof},
    {"tenSelectionsDebounceToOneRead",&Live::tenSelectionsDebounceToOneRead},
    {"overCapIsTruncated",&Live::overCapIsTruncated},
    {"exactCapIsNotTruncated",&Live::exactCapIsNotTruncated},
    {"supersededCaptureCannotPublish",&Live::supersededCaptureCannotPublish},
    {"receiptStampLetsNewCopyWin",&Live::receiptStampLetsNewCopyWin},
    {"zeroByteIdleRetriesNextMime",&Live::zeroByteIdleRetriesNextMime},
    {"partialIdleRetiresWithoutRetry",&Live::partialIdleRetiresWithoutRetry},
    {"absoluteDeadlineSurvivesProgress",&Live::absoluteDeadlineSurvivesProgress},
    {"overlappingRequestsReuseTransfer",&Live::overlappingRequestsReuseTransfer},
    {"activationDeadlineSuppressesCache",&Live::activationDeadlineSuppressesCache},
    {"stopSuppressesLateCompletion",&Live::stopSuppressesLateCompletion}
  };
  if(!runner.init()){std::fprintf(stderr,"wayland-selection-live: FAIL sourceBecomesAvailable\n");return 1;}
  for(auto const & test:cases)
  {
    if(!(runner.*test.run)()){std::fprintf(stderr,"wayland-selection-live: FAIL %s\n",test.name);return 1;}
    std::printf("%s: PASS\n",test.name);std::fflush(stdout);
  }
  std::puts("wayland-selection-live: PASS (13 cases)");return 0;
}
GD_CPP
if ! (cd "$tmp" && qmake6 selection-live.pro "GD_REPO_ROOT=$root" && make -j2) > "$tmp/build.log" 2>&1; then
    tail -n 60 "$tmp/build.log" >&2
    echo 'wayland-selection-live: FAIL build' >&2
    exit 1
fi
python3 - "$tmp" <<'GD_PY'
import os, pathlib, signal, subprocess, sys, time
root = pathlib.Path(sys.argv[1])
children = []
def interrupted(signum, frame):
    raise SystemExit(128 + signum)
for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(sig, interrupted)
def stop(process):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            print('wayland-selection-live: child did not reap within cleanup bound', file=sys.stderr)
    except ProcessLookupError:
        pass
try:
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', KWIN_COMPOSE='Q', LIBGL_ALWAYS_SOFTWARE='1')
    with (root / 'kwin.log').open('wb') as log:
        compositor = subprocess.Popen(['kwin_wayland', '--virtual', '--no-kactivities', '--no-global-shortcuts', '--socket', 'gd-selection-live'], env=env, stdout=log, stderr=log, start_new_session=True)
        children.append(compositor)
        deadline = time.monotonic() + 5
        socket = root / 'runtime/gd-selection-live'
        while not socket.exists() and compositor.poll() is None and time.monotonic() < deadline:
            time.sleep(0.01)
        if not socket.exists():
            print((root / 'kwin.log').read_text(errors='replace'), file=sys.stderr)
            print('wayland-selection-live: FAIL compositor-startup', file=sys.stderr)
            raise SystemExit(1)
        env.update(QT_QPA_PLATFORM='wayland', WAYLAND_DISPLAY='gd-selection-live')
        runner = subprocess.Popen(['timeout', '60', str(root / 'selection-live')], env=env, start_new_session=True)
        children.append(runner)
        try:
            status = runner.wait(timeout=62)
        except subprocess.TimeoutExpired:
            status = 124
        raise SystemExit(status if status >= 0 else 128 - status)
finally:
    for process in reversed(children):
        stop(process)
GD_PY
