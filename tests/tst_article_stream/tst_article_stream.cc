/* GoldenDict stream contract tests. Real Qt jobs and renderer; test-only link
 * observers forward every observed call to its original implementation. */
#include <QtTest>
#include <QApplication>
#include <QDeadlineTimer>
#include <QNetworkProxy>
#include <QPointer>
#include <QWebEngineLoadingInfo>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestJob>

#include <atomic>
#include <functional>
#include <future>
#include <thread>
#include <type_traits>

#include "articleschemes.hh"
#include "article_netmgr.hh"
#include "support/dictionary_stub.hh"
#include "support/fixtures.hh"

using TestSupport::ControlledDataRequest;
using TestSupport::ReadOperation;

namespace {
struct Observation
{
    QPointer<QWebEngineUrlRequestJob> job;
    QPointer<ArticleStreamDevice> device;
    int replies = 0, failures = 0, completed = 0, channelFinished = 0;
    QList<QWebEngineUrlRequestJob::Error> errors;
    QStringList deviceErrors;
    QList<Qt::HANDLE> deviceErrorThreads;
    QList<Qt::HANDLE> producerErrorThreads;
    bool sequential = false, opened = false, jobOwned = false, guiAffinity = false;
    qint64 size = -1, backlog = -1;
};
QHash<QWebEngineUrlRequestJob *, std::shared_ptr<Observation>> observations;
std::atomic<int> ioProducerCalls{0};
void observeProducer()
{
    if (qApp && QThread::currentThread() != qApp->thread())
        ++ioProducerCalls;
}
}

// --wrap observes references from the production translation units. The real
// methods are always called; no fake job or producer getter is substituted.
extern "C" {
void __real__ZN23QWebEngineUrlRequestJob5replyERK10QByteArrayP9QIODevice(
    QWebEngineUrlRequestJob *, QByteArray const &, QIODevice *);
void __wrap__ZN23QWebEngineUrlRequestJob5replyERK10QByteArrayP9QIODevice(
    QWebEngineUrlRequestJob *job, QByteArray const &mime, QIODevice *device)
{
    auto o = observations.value(job);
    if (o) {
        ++o->replies;
        o->sequential = device->isSequential();
        o->opened = device->isOpen() && device->openMode() == (QIODevice::ReadOnly | QIODevice::Unbuffered);
        o->jobOwned = device->parent() == job;
        o->guiAffinity = device->thread() == qApp->thread();
        o->size = device->size();
        o->backlog = device->bytesAvailable();
        o->device = qobject_cast<ArticleStreamDevice *>(device);
        if (o->device) {
            QObject::connect(o->device, &ArticleStreamDevice::completed, job, [o] { ++o->completed; });
            QObject::connect(o->device, &QIODevice::readChannelFinished, job, [o] { ++o->channelFinished; });
            QObject::connect(o->device, &ArticleStreamDevice::failed, job,
                             [o](QString const &message) {
                                 o->deviceErrors.append(message);
                                 o->deviceErrorThreads.append(QThread::currentThreadId());
                             });
        }
    }
    __real__ZN23QWebEngineUrlRequestJob5replyERK10QByteArrayP9QIODevice(job, mime, device);
}
void __real__ZN23QWebEngineUrlRequestJob4failENS_5ErrorE(QWebEngineUrlRequestJob *, QWebEngineUrlRequestJob::Error);
void __wrap__ZN23QWebEngineUrlRequestJob4failENS_5ErrorE(QWebEngineUrlRequestJob *job, QWebEngineUrlRequestJob::Error error)
{
    if (auto o = observations.value(job)) {
        ++o->failures;
        o->errors.append(error);
    }
    __real__ZN23QWebEngineUrlRequestJob4failENS_5ErrorE(job, error);
}
long __real__ZN10Dictionary11DataRequest8dataSizeEv(Dictionary::DataRequest *);
long __wrap__ZN10Dictionary11DataRequest8dataSizeEv(Dictionary::DataRequest *request)
{ observeProducer(); return __real__ZN10Dictionary11DataRequest8dataSizeEv(request); }
void __real__ZN10Dictionary11DataRequest12getDataSliceEmmPv(Dictionary::DataRequest *, size_t, size_t, void *);
void __wrap__ZN10Dictionary11DataRequest12getDataSliceEmmPv(Dictionary::DataRequest *request, size_t offset, size_t size, void *out)
{ observeProducer(); __real__ZN10Dictionary11DataRequest12getDataSliceEmmPv(request, offset, size, out); }
std::vector<char> &__real__ZN10Dictionary11DataRequest11getFullDataEv(Dictionary::DataRequest *);
std::vector<char> &__wrap__ZN10Dictionary11DataRequest11getFullDataEv(Dictionary::DataRequest *request)
{ observeProducer(); return __real__ZN10Dictionary11DataRequest11getFullDataEv(request); }
bool __real__ZN10Dictionary7Request10isFinishedEv(Dictionary::Request *);
bool __wrap__ZN10Dictionary7Request10isFinishedEv(Dictionary::Request *request)
{ observeProducer(); return __real__ZN10Dictionary7Request10isFinishedEv(request); }
QString __real__ZN10Dictionary7Request14getErrorStringEv(Dictionary::Request *);
QString __wrap__ZN10Dictionary7Request14getErrorStringEv(Dictionary::Request *request)
{ observeProducer(); return __real__ZN10Dictionary7Request14getErrorStringEv(request); }
}

namespace {
class ObservedHandler final : public ArticleSchemeHandler
{
public:
    using ArticleSchemeHandler::ArticleSchemeHandler;
    std::shared_ptr<Observation> last;
    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        last = std::make_shared<Observation>();
        last->job = job;
        observations.insert(job, last);
        connect(job, &QObject::destroyed, qApp, [job] { observations.remove(job); });
        ArticleSchemeHandler::requestStarted(job);
        if (last->replies == 0 && last->job) {
            for (auto device : job->findChildren<ArticleStreamDevice *>()) {
                last->device = device;
                auto o = last;
                connect(device, &ArticleStreamDevice::failed, job, [o](QString const &message) {
                    o->deviceErrors.append(message);
                    o->deviceErrorThreads.append(QThread::currentThreadId());
                });
                connect(device, &ArticleStreamDevice::completed, job, [o] { ++o->completed; });
                connect(device, &QIODevice::readChannelFinished, job, [o] { ++o->channelFinished; });
            }
        }
    }
};

struct Browser
{
    sptr<TestSupport::FixtureDictionary> dictionary{new TestSupport::FixtureDictionary};
    std::vector<sptr<Dictionary::Class>> dictionaries{dictionary};
    std::vector<Instances::Group> groups;
    ArticleMaker maker{dictionaries, groups, {}, {}};
    bool blockExternal = true, hideHeader = false;
    ArticleNetworkAccessManager resources{nullptr, dictionaries, maker, blockExternal, hideHeader};
    std::function<void(ControlledDataRequest &)> onCreated;
    int factoryCalls = 0;
    std::unique_ptr<ObservedHandler> handler;
    QWebEngineProfile profile;
    QWebEnginePage page{&profile};
    std::unique_ptr<QSignalSpy> producerFailed;
    QList<bool> loadResults;
    QList<QWebEngineLoadingInfo> loading;

    explicit Browser(bool controlled = true)
    {
        dictionary->setResourcePending(true);
        if (controlled) {
            auto factory = TestSupport::controlledReaderFactory();
            handler = std::make_unique<ObservedHandler>(resources,
                [this, factory](sptr<Dictionary::DataRequest> const &request) {
                    ++factoryCalls;
                    if (onCreated) onCreated(*dynamic_cast<ControlledDataRequest *>(request.get()));
                    return factory(request);
                });
        } else {
            handler = std::make_unique<ObservedHandler>(resources);
        }
        producerFailed = std::make_unique<QSignalSpy>(handler.get(), &ArticleSchemeHandler::producerFailed);
        QObject::connect(handler.get(), &ArticleSchemeHandler::producerFailed, handler.get(), [this] {
            observation()->producerErrorThreads.append(QThread::currentThreadId());
        });
        for (QByteArray const scheme : {"bres", "gdau", "gdvideo"})
            profile.installUrlSchemeHandler(scheme, handler.get());
        page.settings()->setAttribute(QWebEngineSettings::BackForwardCacheEnabled, false);
        QObject::connect(&page, &QWebEnginePage::loadFinished, &page, [this](bool ok) { loadResults.append(ok); });
        QObject::connect(&page, &QWebEnginePage::loadingChanged, &page,
                         [this](QWebEngineLoadingInfo const &info) { loading.append(info); });
    }
    ~Browser() { if (handler) profile.removeUrlSchemeHandler(handler.get()); }
    sptr<ControlledDataRequest> request() { return dictionary->lastResourceRequest(); }
    std::shared_ptr<Observation> observation() { return handler->last; }
    void load(char const *url) { page.load(QUrl(QString::fromLatin1(url))); }
    bool failed() const
    {
        return !loadResults.isEmpty() && !loadResults.last() && !loading.isEmpty()
            && loading.last().status() == QWebEngineLoadingInfo::LoadFailedStatus
            && loading.last().errorCode() == -2
            && loading.last().errorString().contains(QLatin1String("ERR_FAILED"));
    }
};

// One aggregate deadline per case, including every JS round trip and wait.
#define WAIT_FOR(condition) \
    QVERIFY2(QTest::qWaitFor([&] { return bool(condition); }, int(qMax<qint64>(0, deadline.remainingTime()))), #condition)

QVariant javascript(QWebEnginePage &page, QString const &script, QDeadlineTimer const &deadline)
{
    struct Result { bool done = false; QVariant value; };
    auto result = std::make_shared<Result>();
    page.runJavaScript(script, [result](QVariant const &value) { result->value = value; result->done = true; });
    if (!QTest::qWaitFor([result] { return result->done; }, int(qMax<qint64>(0, deadline.remainingTime())))) return {};
    return result->value;
}

QByteArray padding() { return "<!--" + QByteArray(2048, 'x') + "-->"; }
QByteArray firstChunk()
{
    return "<!doctype html><html><head><meta charset='utf-8'><title>stream</title></head>"
           "<body><pre id='bytes'>ABC" + padding();
}
QString visibleBytes(Browser &b, QDeadlineTimer const &deadline)
{
    return javascript(b.page, QStringLiteral("document.getElementById('bytes')?.textContent || ''"), deadline).toString();
}

// A blocking observer for a direct lock probe, delegating every byte operation
// to the existing controlled reader. It owns only plain synchronized state.
struct CopyGate
{
    std::atomic<int> calls{0};
    std::promise<void> entered, release;
    std::shared_future<void> entry{entered.get_future().share()};
    std::shared_future<void> released{release.get_future().share()};
};
struct SliceGate
{
    std::atomic<int> calls{0};
    std::promise<void> entered, release;
    std::shared_future<void> entry{entered.get_future().share()};
    std::shared_future<void> released{release.get_future().share()};
    std::function<void()> duringCopy;
};

class SliceHookedReader final : public DataRequestReader
{
public:
    std::shared_ptr<DataRequestReader const> const delegate;
    std::shared_ptr<SliceGate> const gate;
    SliceHookedReader(std::shared_ptr<DataRequestReader const> reader, std::shared_ptr<SliceGate> gate_):
        delegate(std::move(reader)), gate(std::move(gate_)) {}
    QByteArray fullData(Dictionary::DataRequest &request) const override { return delegate->fullData(request); }
    void dataSlice(Dictionary::DataRequest &request, size_t offset, size_t size, void *out) const override
    {
        if (gate->calls.fetch_add(1) == 0 && gate->duringCopy)
            gate->duringCopy();
        delegate->dataSlice(request, offset, size, out);
    }
    void copyStreamBytes(char *out, char const *bytes, qint64 count) const override
    { delegate->copyStreamBytes(out, bytes, count); }
};

class GatedReader final : public DataRequestReader
{
public:
    std::shared_ptr<DataRequestReader const> const delegate;
    std::shared_ptr<CopyGate> const gate;
    GatedReader(std::shared_ptr<DataRequestReader const> reader, std::shared_ptr<CopyGate> gate_):
        delegate(std::move(reader)), gate(std::move(gate_)) {}
    QByteArray fullData(Dictionary::DataRequest &request) const override { return delegate->fullData(request); }
    void dataSlice(Dictionary::DataRequest &request, size_t offset, size_t size, void *out) const override
    { delegate->dataSlice(request, offset, size, out); }
    void copyStreamBytes(char *out, char const *bytes, qint64 count) const override
    {
        if (gate->calls.fetch_add(1) == 0) gate->entered.set_value();
        gate->released.wait();
        delegate->copyStreamBytes(out, bytes, count);
    }
};

class DirectDevice final : public ArticleStreamDevice
{
public:
    using ArticleStreamDevice::ArticleStreamDevice;
    using ArticleStreamDevice::readData;
    using ArticleStreamDevice::writeData;

};
std::shared_ptr<DataRequestReader const> controlledReader(sptr<ControlledDataRequest> const &request)
{ return TestSupport::controlledReaderFactory()(request); }
}

class TstArticleStream : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStringList reasons;
        QVERIFY2(TestSupport::requiredEnvironment(&reasons), qPrintable(reasons.join('\n')));
        for (QString const token : {QStringLiteral("display"), QStringLiteral("webengine")}) {
            QString reason;
            QVERIFY2(TestSupport::prerequisiteAvailable(token, &reason), qPrintable(reason));
        }
        TestSupport::installWatchdog(TestSupport::BrowserWatchdogSeconds);
    }
    void cleanupTestCase() { TestSupport::clearWatchdog(); }
    void init() { ioProducerCalls = 0; }
    void cleanup() { QCOMPARE(ioProducerCalls.load(), 0); }

    void directStateAndSuccess()
    {
        QDeadlineTimer deadline(5000);
        QObject job;
        sptr<ControlledDataRequest> request(new ControlledDataRequest);
        DirectDevice device(request, controlledReader(request), &job);
        QSignalSpy ready(&device, &QIODevice::readyRead), completed(&device, &ArticleStreamDevice::completed);
        QSignalSpy finished(&device, &QIODevice::readChannelFinished), failed(&device, &ArticleStreamDevice::failed);
        for (auto signal : {&QIODevice::readyRead, &QIODevice::readChannelFinished})
            connect(&device, signal, &job, [&] { QVERIFY(device.bytesAvailable() >= 0); device.atEnd(); });
        connect(&device, &ArticleStreamDevice::completed, &job, [&] { device.bytesAvailable(); device.atEnd(); });
        QVERIFY(device.isOpen());
        QCOMPARE(device.openMode(), QIODevice::ReadOnly | QIODevice::Unbuffered);
        QVERIFY(device.isSequential());
        QCOMPARE(device.size(), 0);
        QCOMPARE(device.bytesAvailable(), 0);
        QVERIFY(!device.atEnd());
        char bytes[16] = {};
        QCOMPARE(device.readData(bytes, 0), 0);
        QCOMPARE(device.readData(bytes, -1), -1);
        QCOMPARE(device.readData(bytes, 16), 0);
        QCOMPARE(device.writeData(bytes, 0), -1);
        request->append("ABC");
        WAIT_FOR(ready.count() == 1);
        QCOMPARE(device.bytesAvailable(), 3);
        QCOMPARE(device.readData(bytes, 2), 2);
        QCOMPARE(QByteArray(bytes, 2), QByteArray("AB"));
        QCOMPARE(device.bytesAvailable(), 1);
        request->append("DEFGHIJ");
        WAIT_FOR(ready.count() == 2);
        QCOMPARE(device.bytesAvailable(), 8);
        request->finishSuccess();
        WAIT_FOR(completed.count() == 1);
        QCOMPARE(ready.count(), 3);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!device.atEnd());
        QCOMPARE(device.readData(bytes, 16), 8);
        QCOMPARE(QByteArray(bytes, 8), QByteArray("CDEFGHIJ"));
        QCOMPARE(device.bytesAvailable(), 0);
        QCOMPARE(device.size(), 0);
        QCOMPARE(device.readData(bytes, 0), 0);
        QCOMPARE(device.readData(bytes, 16), -1);
        QVERIFY(device.atEnd());
        emit request->updated();
        emit request->finished();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(completed.count(), 1);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(ready.count(), 3);
        QCOMPARE(failed.count(), 0);
    }

    void directFailurePreservesBytes_data()
    {
        QTest::addColumn<int>("operation");
        QTest::newRow("slice") << int(ReadOperation::Slice);
        QTest::newRow("stream-copy") << int(ReadOperation::StreamCopy);
        QTest::newRow("producer") << -1;
    }
    void directFailurePreservesBytes()
    {
        QFETCH(int, operation);
        QDeadlineTimer deadline(5000);
        QObject job;
        sptr<ControlledDataRequest> request(new ControlledDataRequest);
        request->append("ABC");
        DirectDevice device(request, controlledReader(request), &job);
        QSignalSpy failed(&device, &ArticleStreamDevice::failed), completed(&device, &ArticleStreamDevice::completed);
        QSignalSpy finished(&device, &QIODevice::readChannelFinished);
        // Every notification reenters synchronized operations: emission while
        // holding the state mutex would deadlock this real slot chain.
        int reentries = 0;
        connect(&device, &ArticleStreamDevice::failed, &job, [&](QString const &) {
            ++reentries;
            QCOMPARE(device.bytesAvailable(), 3);
            QVERIFY(!device.atEnd());
        });
        QString const message = QStringLiteral("direct unique failure %1").arg(operation);
        char bytes[16] = {};
        if (operation == int(ReadOperation::Slice)) {
            request->setSliceException(message);
            request->append("DEF");
        } else if (operation == int(ReadOperation::StreamCopy)) {
            request->setStreamCopyException(message);
            std::thread io([&] { QCOMPARE(device.readData(bytes, 3), -1); });
            io.join();
        } else request->finishError(message);
        WAIT_FOR(failed.count() == 1);
        QCOMPARE(failed.first().first().toString(), message);
        QCOMPARE(reentries, 1);
        QCOMPARE(device.bytesAvailable(), 3); // Throw must not publish/advance.
        if (operation == int(ReadOperation::StreamCopy)) {
            request->setStreamCopyException(QStringLiteral("later failure must keep the first latch"));
            QCOMPARE(device.readData(bytes, 3), -1);
            QCoreApplication::sendPostedEvents();
            QCOMPARE(failed.count(), 1);
            QCOMPARE(failed.first().first().toString(), message);
            QCOMPARE(device.bytesAvailable(), 3);
        }
        QCOMPARE(device.readData(bytes, 0), 0);
        QVERIFY(!device.atEnd());
        QCOMPARE(device.readData(bytes, 16), 3);
        QCOMPARE(QByteArray(bytes, 3), QByteArray("ABC"));
        QCOMPARE(device.bytesAvailable(), 0);
        QCOMPARE(device.readData(bytes, 16), -1);
        QVERIFY(!device.atEnd());
        request->finishSuccess();
        emit request->updated();
        emit request->finished();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(finished.count(), 0);
    }

    void cancellationRunsOnGuiAfterUnlock()
    {
        QDeadlineTimer deadline(5000);
        QObject job;
        sptr<ControlledDataRequest> request(new ControlledDataRequest);
        DirectDevice device(request, controlledReader(request), &job);
        int reentries = 0;
        connect(request.get(), &Dictionary::DataRequest::finished, &job, [&] {
            QCOMPARE(QThread::currentThread(), qApp->thread());
            QVERIFY(device.atEnd());
            QCOMPARE(device.bytesAvailable(), 0);
            device.cancel(); // Cancellation must already be latched.
            ++reentries;
        }, Qt::DirectConnection);
        std::thread io([&] {
            device.close();
            QVERIFY(device.atEnd());
            char byte;
            QCOMPARE(device.readData(&byte, 0), -1);
            QCOMPARE(device.readData(&byte, 1), -1);
        });
        io.join();
        QCOMPARE(request->cancelCount(), 0); // Worker must only queue GUI work.
        WAIT_FOR(request->cancelCount() == 1);
        QCOMPARE(reentries, 1);
        device.cancel();
        device.close();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(request->cancelCount(), 1);
    }

    void mutexSerializesIoState_data()
    {
        QTest::addColumn<QString>("operation");
        for (char const *row : {"read-data", "backlog", "end", "close"})
            QTest::newRow(row) << QString::fromLatin1(row);
    }
    void mutexSerializesIoState()
    {
        QFETCH(QString, operation);
        QDeadlineTimer deadline(5000);
        QObject job;
        sptr<ControlledDataRequest> request(new ControlledDataRequest);
        request->append("ABC");
        auto gate = std::make_shared<CopyGate>();
        DirectDevice device(request, std::make_shared<GatedReader>(controlledReader(request), gate), &job);
        std::thread first([&] { char byte; device.readData(&byte, 1); });
        // Pumping GUI events here would dispatch publish(), which also needs
        // the mutex held by the gated reader. Wait without an event loop.
        bool const entered = gate->entry.wait_for(std::chrono::milliseconds(qMax<qint64>(0, deadline.remainingTime())))
                             == std::future_status::ready;
        std::promise<void> attempted, finished;
        auto attemptedFuture = attempted.get_future(), finishedFuture = finished.get_future();
        std::thread second([&] {
            attempted.set_value();
            if (operation == QLatin1String("read-data")) { char byte; device.readData(&byte, 1); }
            else if (operation == QLatin1String("backlog")) device.bytesAvailable();
            else if (operation == QLatin1String("end")) device.atEnd();
            else device.close();
            finished.set_value();
        });
        attemptedFuture.wait();
        bool const escaped = finishedFuture.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
        int const concurrentCopies = gate->calls.load();
        gate->release.set_value();
        first.join();
        second.join();
        QVERIFY(entered);
        QCOMPARE(concurrentCopies, 1);
        QVERIFY2(!escaped, "IO operation escaped the byte-state mutex while another copy held it");
    }

    void producerFinalisingDuringSliceLosesNoBytes()
    {
        // F-1: the producer appends its final bytes and finishes *while* the slice
        // is being copied. The device must observe that completion and copy the
        // rest, not mark the stream complete with the stale snapshot.
        QObject job;
        sptr<ControlledDataRequest> request(new ControlledDataRequest);
        request->append("ABC");
        auto gate = std::make_shared<SliceGate>();
        gate->duringCopy = [request] {
            request->append("DEF");
            request->finishSuccess();
        };
        DirectDevice device(request, std::make_shared<SliceHookedReader>(controlledReader(request), gate), &job);
        QSignalSpy completed(&device, &ArticleStreamDevice::completed);

        // The device performs its initial staging in the constructor.
        QTRY_VERIFY_WITH_TIMEOUT(completed.size() == 1, 5000);

        char bytes[16];
        QByteArray received;
        for (;;) {
            qint64 const count = device.readData(bytes, sizeof bytes);
            if (count <= 0) break;
            received.append(bytes, qint64(count));
        }
        QCOMPARE(received, QByteArray("ABCDEF"));
    }

    void failureDuringCompletionNotificationWins()
    {
        // F-2: a slot reacting to the completion notification drives a copy that
        // throws. Failure latches first, so the device must not also report success.
        QObject job;
        sptr<ControlledDataRequest> request(new ControlledDataRequest);
        request->append("ABC");
        request->finishSuccess();
        auto gate = std::make_shared<SliceGate>();
        DirectDevice device(request, std::make_shared<SliceHookedReader>(controlledReader(request), gate), &job);
        QSignalSpy completed(&device, &ArticleStreamDevice::completed);
        QSignalSpy failed(&device, &ArticleStreamDevice::failed);

        char bytes[8];
        bool readFromReadyRead = false;
        connect(&device, &QIODevice::readyRead, &device, [&] {
            if (readFromReadyRead) return;
            readFromReadyRead = true;
            // readData copies through copyStreamBytes: arming a copy failure is what
            // the completion notification's slot can trigger.
            request->setStreamCopyException(QStringLiteral("late copy failure"));
            while (device.readData(bytes, sizeof bytes) > 0) {}
        });

        QTRY_VERIFY_WITH_TIMEOUT(failed.size() == 1, 5000);
        QCOMPARE(completed.size(), 0);
    }

    void interleavedProducerAndConsumer()
    {
        QDeadlineTimer deadline(5000);
        QObject job;
        sptr<ControlledDataRequest> request(new ControlledDataRequest);
        DirectDevice device(request, controlledReader(request), &job);
        std::atomic<bool> stop{false}, done{false};
        QByteArray received;
        std::thread io([&] {
            char bytes[7];
            while (!stop.load()) {
                qint64 const count = device.readData(bytes, sizeof bytes);
                if (count > 0) received.append(bytes, count);
                else if (count < 0) { done = true; break; }
                else std::this_thread::yield();
            }
        });
        // Always join even if an assertion would otherwise return early.
        struct Join { std::thread &thread; std::atomic<bool> &stop;
            ~Join() { stop = true; thread.join(); } } join{io, stop};
        QByteArray expected;
        for (int i = 0; i < 64; ++i) {
            QByteArray const chunk = QByteArray::number(i) + ":ABCDEFGHIJ;";
            expected += chunk;
            request->append(chunk);
            QCoreApplication::sendPostedEvents();
        }
        request->finishSuccess();
        WAIT_FOR(done.load());
        // done release/acquire publishes the IO-only byte array.
        QCOMPARE(received, expected);
        auto const copy = request->readProbe(ReadOperation::StreamCopy);
        QVERIFY(copy.entered > 0);
        QCOMPARE(copy.thrown, 0);
        QCOMPARE(copy.returned, copy.entered);
        QCOMPARE(copy.callingThreads.size(), 1);
        QVERIFY(*copy.callingThreads.cbegin() != QThread::currentThreadId());
        auto const slice = request->readProbe(ReadOperation::Slice);
        QCOMPARE(slice.callingThreads, QSet<Qt::HANDLE>{QThread::currentThreadId()});
    }

    void realLiveConsumer_data()
    {
        QTest::addColumn<QString>("scheme");
        QTest::addColumn<bool>("controlled");
        QTest::newRow("audio-controlled") << QStringLiteral("gdau") << true;
        QTest::newRow("video-controlled") << QStringLiteral("gdvideo") << true;
        QTest::newRow("audio-default") << QStringLiteral("gdau") << false;
    }
    void realLiveConsumer()
    {
        QFETCH(QString, scheme);
        QFETCH(bool, controlled);
        QDeadlineTimer deadline(10000);
        Browser b(controlled);
        // Controlled case publishes its initial chunk during the real GUI
        // factory callback, before the handler's first inspect/reply.
        b.onCreated = [](ControlledDataRequest &request) { request.append(firstChunk()); };
        b.page.load(QUrl(scheme + QStringLiteral("://dict-a/live.html")));
        WAIT_FOR(b.request());
        if (!controlled) b.request()->append(firstChunk());
        WAIT_FOR(b.observation() && b.observation()->replies == 1);
        auto o = b.observation();
        QVERIFY(o->device);
        QVERIFY(o->sequential && o->opened && o->jobOwned && o->guiAffinity);
        QCOMPARE(o->size, 0);
        QCOMPARE(o->backlog, firstChunk().size());
        WAIT_FOR(visibleBytes(b, deadline) == QLatin1String("ABC"));
        QVERIFY(b.loadResults.isEmpty());
        b.request()->append("DEF" + padding());
        WAIT_FOR(visibleBytes(b, deadline) == QLatin1String("ABCDEF"));
        QVERIFY(b.loadResults.isEmpty());
        b.request()->append("GHIJ" + padding() + "</pre></body></html>");
        WAIT_FOR(visibleBytes(b, deadline) == QLatin1String("ABCDEFGHIJ"));
        b.request()->finishSuccess();
        WAIT_FOR(!b.loadResults.isEmpty());
        QCOMPARE(b.loadResults, QList<bool>{true});
        QCOMPARE(b.loading.last().status(), QWebEngineLoadingInfo::LoadSucceededStatus);
        QCOMPARE(b.loading.last().errorCode(), 200);
        QVERIFY(!b.loading.last().responseHeaders().contains(QByteArrayLiteral("content-range")));
        QCOMPARE(o->replies, 1);
        QCOMPARE(o->failures, 0);
        QCOMPARE(o->completed, 1);
        QCOMPARE(o->channelFinished, 1);
        QCOMPARE(o->deviceErrors.size(), 0);
        QCOMPARE(b.producerFailed->count(), 0);
        if (controlled) {
            QCOMPARE(b.factoryCalls, 1);
            QVERIFY(b.request()->readProbe(ReadOperation::FullData).untouched());
            auto const slice = b.request()->readProbe(ReadOperation::Slice);
            QCOMPARE(slice.entered, 3);
            QCOMPARE(slice.callingThreads, QSet<Qt::HANDLE>{QThread::currentThreadId()});
            auto const copy = b.request()->readProbe(ReadOperation::StreamCopy);
            QVERIFY(copy.entered >= 3);
            QCOMPARE(copy.thrown, 0);
            QCOMPARE(copy.returned, copy.entered);
            QCOMPARE(copy.callingThreads.size(), 1);
            QVERIFY(*copy.callingThreads.cbegin() != QThread::currentThreadId());
        }
    }

    void realReaderFailure_data()
    {
        QTest::addColumn<int>("operation");
        QTest::newRow("full-data-gui") << int(ReadOperation::FullData);
        QTest::newRow("slice-gui-after-reply") << int(ReadOperation::Slice);
        QTest::newRow("stream-copy-chromium-io") << int(ReadOperation::StreamCopy);
    }
    void realReaderFailure()
    {
        QFETCH(int, operation);
        QDeadlineTimer deadline(10000);
        Browser b;
        QString const message = QStringLiteral("real production boundary %1 / unique").arg(operation);
        b.load(operation == int(ReadOperation::FullData) ? "bres://dict-a/failure.html" : "gdau://dict-a/failure.html");
        WAIT_FOR(b.request());
        if (operation == int(ReadOperation::FullData)) {
            b.request()->setFullDataException(message);
            b.request()->append("<p>must never reply</p>");
            b.request()->finishSuccess();
        } else {
            b.request()->append(firstChunk());
            WAIT_FOR(b.observation() && b.observation()->replies == 1);
            WAIT_FOR(visibleBytes(b, deadline) == QLatin1String("ABC"));
            if (operation == int(ReadOperation::Slice)) b.request()->setSliceException(message);
            else b.request()->setStreamCopyException(message);
            b.request()->append("DEF" + padding());
        }
        WAIT_FOR(b.producerFailed->count() == 1);
        WAIT_FOR(b.failed());
        QCOMPARE(b.producerFailed->first().at(0).toUrl(), b.loading.last().url());
        QCOMPARE(b.producerFailed->first().at(1).toString(), message);
        const auto probe = b.request()->readProbe(static_cast<ReadOperation>(operation));
        QCOMPARE(probe.entered, 1);
        QCOMPARE(probe.thrown, 1);
        QCOMPARE(probe.returned, 0);
        QCOMPARE(probe.callingThreads.size(), 1);
        if (operation == int(ReadOperation::StreamCopy))
            QVERIFY(*probe.callingThreads.cbegin() != QThread::currentThreadId());
        else QCOMPARE(*probe.callingThreads.cbegin(), QThread::currentThreadId());
        auto o = b.observation();
        QCOMPARE(o->replies, operation == int(ReadOperation::FullData) ? 0 : 1);
        QCOMPARE(o->failures, operation == int(ReadOperation::FullData) ? 1 : 0);
        if (operation == int(ReadOperation::FullData))
            QCOMPARE(o->errors, QList<QWebEngineUrlRequestJob::Error>{QWebEngineUrlRequestJob::RequestFailed});
        else QCOMPARE(o->deviceErrors, QStringList{message});
        QCOMPARE(o->producerErrorThreads, QList<Qt::HANDLE>{QThread::currentThreadId()});
        if (operation != int(ReadOperation::FullData))
            QCOMPARE(o->deviceErrorThreads, QList<Qt::HANDLE>{QThread::currentThreadId()});
        QCOMPARE(o->completed, 0);
        QCOMPARE(o->channelFinished, 0);
        emit b.request()->updated();
        emit b.request()->finished();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(b.producerFailed->count(), 1);
        QCOMPARE(o->replies + o->failures, 1);
    }

    void preReplyOutcomes_data()
    {
        QTest::addColumn<QString>("outcome");
        for (char const *row : {"unknown-not-found", "empty-success", "partial-error", "initial-slice-error"})
            QTest::newRow(row) << QString::fromLatin1(row);
    }
    void preReplyOutcomes()
    {
        QFETCH(QString, outcome);
        QDeadlineTimer deadline(7000);
        Browser b;
        QString const message = QStringLiteral("before reply / ") + outcome;
        if (outcome == QLatin1String("partial-error")) b.onCreated = [&](ControlledDataRequest &request) {
            request.append("partial"); request.finishError(message);
        };
        if (outcome == QLatin1String("initial-slice-error")) b.onCreated = [&](ControlledDataRequest &request) {
            request.setSliceException(message); request.append("partial");
        };
        b.load("gdau://dict-a/empty.html");
        WAIT_FOR(b.request());
        auto o = b.observation();
        if (outcome == QLatin1String("empty-success") || outcome == QLatin1String("unknown-not-found")) {
            QCOMPARE(o->replies, 0);
            QCOMPARE(o->failures, 0);
            QCOMPARE(b.request()->dataSize(), -1);
            if (outcome == QLatin1String("empty-success")) b.request()->finishSuccess();
            else b.request()->finishNotFound();
        }
        WAIT_FOR(!b.loadResults.isEmpty());
        if (outcome == QLatin1String("empty-success")) {
            QCOMPARE(b.loadResults, QList<bool>{true});
            QCOMPARE(o->replies, 1);
            QCOMPARE(o->failures, 0);
            QCOMPARE(o->size, 0);
            QCOMPARE(o->completed, 1);
            QCOMPARE(o->channelFinished, 1);
            QCOMPARE(b.producerFailed->count(), 0);
        } else {
            QCOMPARE(b.loadResults, QList<bool>{false});
            QCOMPARE(o->replies, 0);
            QCOMPARE(o->failures, 1);
            QCOMPARE(o->completed, 0);
            QCOMPARE(o->channelFinished, 0);
            bool const missing = outcome == QLatin1String("unknown-not-found");
            QCOMPARE(o->errors.first(), missing ? QWebEngineUrlRequestJob::UrlNotFound : QWebEngineUrlRequestJob::RequestFailed);
            QCOMPARE(b.producerFailed->count(), missing ? 0 : 1);
            if (!missing) QCOMPARE(b.producerFailed->first().at(1).toString(), message);
            if (outcome == QLatin1String("initial-slice-error")) {
                QCOMPARE(o->deviceErrors, QStringList{message});
                QCOMPARE(o->deviceErrorThreads, QList<Qt::HANDLE>{QThread::currentThreadId()});
                auto const probe = b.request()->readProbe(ReadOperation::Slice);
                QCOMPARE(probe.entered, 1);
                QCOMPARE(probe.thrown, 1);
                QCOMPARE(probe.returned, 0);
                QCOMPARE(probe.callingThreads, QSet<Qt::HANDLE>{QThread::currentThreadId()});
            }
        }
    }

    void realProducerErrorAfterReply()
    {
        QDeadlineTimer deadline(7000);
        Browser b;
        b.load("gdvideo://dict-a/producer-error.html");
        WAIT_FOR(b.request());
        b.request()->append(firstChunk());
        WAIT_FOR(visibleBytes(b, deadline) == QLatin1String("ABC"));
        auto o = b.observation();
        QString const message = QStringLiteral("after reply partial producer error");
        b.request()->finishError(message);
        WAIT_FOR(b.failed());
        QCOMPARE(o->deviceErrors, QStringList{message});
        QCOMPARE(b.producerFailed->count(), 1);
        QCOMPARE(b.producerFailed->first().at(1).toString(), message);
        QCOMPARE(o->completed, 0);
        QCOMPARE(o->channelFinished, 0);
        QCOMPARE(o->failures, 0);
        QCOMPARE(o->replies, 1);
    }

    void bufferedWaitsForSuccessfulCompletion()
    {
        QDeadlineTimer deadline(7000);
        Browser b;
        b.load("bres://dict-a/wait.html");
        WAIT_FOR(b.request());
        auto o = b.observation();
        QCOMPARE(o->replies + o->failures, 0);
        b.request()->append("<p id='bytes'>ABCDEFGHIJ</p>");
        QCoreApplication::sendPostedEvents();
        QCOMPARE(o->replies + o->failures, 0);
        QVERIFY(b.request()->readProbe(ReadOperation::FullData).untouched());
        b.request()->finishSuccess();
        WAIT_FOR(!b.loadResults.isEmpty());
        QCOMPARE(b.loadResults, QList<bool>{true});
        QCOMPARE(visibleBytes(b, deadline), QStringLiteral("ABCDEFGHIJ"));
        QCOMPARE(o->replies, 1);
        QCOMPARE(o->failures, 0);
        QVERIFY(!o->sequential);
        auto const probe = b.request()->readProbe(ReadOperation::FullData);
        QCOMPARE(probe.entered, 1);
        QCOMPARE(probe.returned, 1);
        QCOMPARE(probe.thrown, 0);
        QCOMPARE(probe.callingThreads, QSet<Qt::HANDLE>{QThread::currentThreadId()});
    }

    void defaultBufferedPayload()
    {
        QDeadlineTimer deadline(7000);
        Browser b(false);
        b.dictionary->setResourcePending(false);
        b.dictionary->setResources({{QStringLiteral("buffer.html"), "<p id='bytes'>ABCDEFGHIJ</p>"}});
        b.load("bres://dict-a/buffer.html");
        WAIT_FOR(!b.loadResults.isEmpty());
        QCOMPARE(b.loadResults, QList<bool>{true});
        QCOMPARE(visibleBytes(b, deadline), QStringLiteral("ABCDEFGHIJ"));
        QVERIFY(!b.observation()->sequential);
        QCOMPARE(b.observation()->size, 28);
        QCOMPARE(b.observation()->replies, 1);
        QCOMPARE(b.observation()->failures, 0);
    }

    void cancellationReleasesProducerBeforeDeviceDeletion()
    {
        QObject job;
        sptr<ControlledDataRequest> request(new ControlledDataRequest);
        auto lifetime = request->lifetimeToken();
        DirectDevice device(request, controlledReader(request), &job);
        QSignalSpy ready(&device, &QIODevice::readyRead), completed(&device, &ArticleStreamDevice::completed);
        QSignalSpy finished(&device, &QIODevice::readChannelFinished), failed(&device, &ArticleStreamDevice::failed);
        request->append("queued update must not resurrect a cancelled device");
        device.cancel();
        QCOMPARE(request->cancelCount(), 1);
        request.reset();
        QVERIFY(lifetime.expired());
        QCoreApplication::sendPostedEvents();
        QVERIFY(device.atEnd());
        QCOMPARE(device.bytesAvailable(), 0);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(finished.count(), 0);
        QCOMPARE(failed.count(), 0);
    }

    void handlerTeardownLeavesDeviceJobOwned()
    {
        QDeadlineTimer deadline(7000);
        Browser b;
        b.load("gdau://dict-a/handler-teardown.html");
        WAIT_FOR(b.request());
        auto request = b.request();
        auto lifetime = request->lifetimeToken();
        request->append(firstChunk());
        WAIT_FOR(visibleBytes(b, deadline) == QLatin1String("ABC"));
        auto o = b.observation();
        b.profile.removeUrlSchemeHandler(b.handler.get());
        b.handler.reset();
        QCOMPARE(request->cancelCount(), 1);
        QVERIFY(o->job && o->device);
        QCOMPARE(o->device->parent(), o->job.data());
        b.dictionary->clearObservations();
        request.reset();
        QVERIFY(lifetime.expired());
        b.page.triggerAction(QWebEnginePage::Stop);
        WAIT_FOR(o->job.isNull());
        QVERIFY(o->device.isNull());
        QCOMPARE(o->completed, 0);
        QCOMPARE(o->channelFinished, 0);
    }

    void jobTeardownCancelsOnceAndReleasesProducer()
    {
        QDeadlineTimer deadline(7000);
        Browser b;
        b.load("gdau://dict-a/teardown.html");
        WAIT_FOR(b.request());
        auto request = b.request();
        auto lifetime = request->lifetimeToken();
        request->append(firstChunk());
        WAIT_FOR(visibleBytes(b, deadline) == QLatin1String("ABC"));
        auto o = b.observation();
        b.page.triggerAction(QWebEnginePage::Stop);
        WAIT_FOR(request->cancelCount() == 1);
        WAIT_FOR(o->job.isNull());
        QVERIFY(o->device.isNull());
        QCOMPARE(o->replies, 1);
        QCOMPARE(o->failures, 0);
        QCOMPARE(o->completed, 0);
        QCOMPARE(o->channelFinished, 0);
        b.dictionary->clearObservations();
        request.reset();
        QVERIFY(lifetime.expired());
    }
};

int main(int argc, char **argv)
{
    if (TestSupport::isWatchdogProbeProcess()) TestSupport::runWatchdogProbe();
    registerArticleSchemes();
    QApplication app(argc, argv);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    TstArticleStream suite;
    return QTest::qExec(&suite, argc, argv);
}
#include "tst_article_stream.moc"
