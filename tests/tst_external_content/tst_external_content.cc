/* Real-page policy/profile contracts. The link observers retain real Qt
 * request objects and forward only validated redirects to Chromium. */
#include <QtTest>
#include <QApplication>
#include <QAuthenticator>
#include <QDeadlineTimer>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkDiskCache>
#include <QNetworkProxy>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebEngineFrame>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#include "articleprofile.hh"
#include "articleschemes.hh"
#include "article_netmgr.hh"
#include "articleurls.hh"
#include "config.hh"
#include "support/dictionary_stub.hh"
#include "support/fixtures.hh"
#include "support/http_fixture.hh"

using TestSupport::HttpFixture;
namespace {
struct CallbackGate
{
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> entered{false};
    bool released = false, timedOut = false;
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; changed.notify_all(); }
};
struct Observation
{
    bool substitute = false;
    QUrl request, firstParty, initiator, expectedRedirect;
    int requests = 0, firstParties = 0, initiators = 0, blocks = 0, redirects = 0;
    bool blocked = false;
    QUrl redirected;
    QString refused;
    QSet<Qt::HANDLE> threads;
    std::shared_ptr<CallbackGate> gate;
};
std::mutex observationMutex;
QMap<QUrl, std::shared_ptr<Observation>> observations;
std::optional<QList<QNetworkProxy>> systemProxies;
QList<QNetworkProxy> constructionProxies;
QList<QNetworkProxy> applicationProxies;
std::atomic<int> proxyTargetResolutions{0};
std::atomic<int> ioConfigCalls{0}, ioFileCalls{0}, ioWidgetCalls{0};
bool forbiddenIo() { return qApp && QThread::currentThread() != qApp->thread(); }
int cacheClearCalls = 0;
QtMessageHandler previousHandler = nullptr;
QStringList policyWarnings;
void recordPolicyWarning(QtMsgType type, QMessageLogContext const &context, QString const &message)
{
    if (type == QtWarningMsg && message.startsWith("ArticlePolicy:")) {
        std::lock_guard<std::mutex> lock(observationMutex);
        policyWarnings.append(message);
    }
    if (previousHandler) previousHandler(type, context, message);
}

enum class ClearMode { Real, Immediate, Hold };
ClearMode clearMode = ClearMode::Real;
}

extern "C" {
QString __real__ZN6Config18getNetworkCacheDirEv();
QString __wrap__ZN6Config18getNetworkCacheDirEv()
{
    if (forbiddenIo()) { ++ioConfigCalls; return {}; }
    return __real__ZN6Config18getNetworkCacheDirEv();
}
bool __real__ZN12ArticleMaker14adjustFilePathER7QString(QString &);
bool __wrap__ZN12ArticleMaker14adjustFilePathER7QString(QString &path)
{
    if (forbiddenIo()) { ++ioFileCalls; return false; }
    return __real__ZN12ArticleMaker14adjustFilePathER7QString(path);
}
bool __real__ZN9QFileInfo6existsERK7QString(QString const &);
bool __wrap__ZN9QFileInfo6existsERK7QString(QString const &path)
{
    if (forbiddenIo()) { ++ioFileCalls; return false; }
    return __real__ZN9QFileInfo6existsERK7QString(path);
}
bool __real__ZNK9QFileInfo6existsEv(QFileInfo const *);
bool __wrap__ZNK9QFileInfo6existsEv(QFileInfo const *file)
{
    if (forbiddenIo()) { ++ioFileCalls; return false; }
    return __real__ZNK9QFileInfo6existsEv(file);
}
QWidget *__real__ZN12QApplication12activeWindowEv();
QWidget *__wrap__ZN12QApplication12activeWindowEv()
{
    if (forbiddenIo()) { ++ioWidgetCalls; return nullptr; }
    return __real__ZN12QApplication12activeWindowEv();
}
QString __real__ZNK4QUrl4hostE6QFlagsINS_25ComponentFormattingOptionEE(QUrl const *, QUrl::ComponentFormattingOptions);
QString __wrap__ZNK4QUrl4hostE6QFlagsINS_25ComponentFormattingOptionEE(QUrl const *url, QUrl::ComponentFormattingOptions options)
{
    auto host = __real__ZNK4QUrl4hostE6QFlagsINS_25ComponentFormattingOptionEE(url, options);
    // Test-only upstream DNS: Chromium must use a non-loopback hostname to
    // exercise an application proxy. HttpFixture still forwards real bytes to
    // its real 127.0.0.2 listener; no proxy CLI switch or external DNS is used.
    if (host == QLatin1String("proxy-target.invalid")) {
        ++proxyTargetResolutions;
        return QStringLiteral("127.0.0.2");
    }
    return host;
}
QUrl __real__ZNK24QWebEngineUrlRequestInfo10requestUrlEv(QWebEngineUrlRequestInfo const *);
QUrl __real__ZNK24QWebEngineUrlRequestInfo13firstPartyUrlEv(QWebEngineUrlRequestInfo const *);
QUrl __real__ZNK24QWebEngineUrlRequestInfo9initiatorEv(QWebEngineUrlRequestInfo const *);
void __real__ZN24QWebEngineUrlRequestInfo5blockEb(QWebEngineUrlRequestInfo *, bool);
void __real__ZN24QWebEngineUrlRequestInfo8redirectERK4QUrl(QWebEngineUrlRequestInfo *, QUrl const &);

QUrl __wrap__ZNK24QWebEngineUrlRequestInfo10requestUrlEv(QWebEngineUrlRequestInfo const *info)
{
    auto const real = __real__ZNK24QWebEngineUrlRequestInfo10requestUrlEv(info);
    std::shared_ptr<Observation> o;
    QUrl result;
    {
        std::lock_guard<std::mutex> lock(observationMutex);
        o = observations.value(real);
        if (!o) return real;
        ++o->requests;
        o->threads.insert(QThread::currentThreadId());
        result = o->substitute ? o->request : real;
    }
    if (o->gate && QThread::currentThread() != qApp->thread()) {
        auto &gate = *o->gate;
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        gate.timedOut = !gate.changed.wait_for(lock, std::chrono::seconds(8), [&] { return gate.released; });
    }
    return result;
}
QUrl __wrap__ZNK24QWebEngineUrlRequestInfo13firstPartyUrlEv(QWebEngineUrlRequestInfo const *info)
{
    auto const real = __real__ZNK24QWebEngineUrlRequestInfo13firstPartyUrlEv(info);
    auto const request = __real__ZNK24QWebEngineUrlRequestInfo10requestUrlEv(info);
    std::lock_guard<std::mutex> lock(observationMutex);
    auto o = observations.value(request);
    if (!o) return real;
    ++o->firstParties;
    return o->substitute ? o->firstParty : real;
}
QUrl __wrap__ZNK24QWebEngineUrlRequestInfo9initiatorEv(QWebEngineUrlRequestInfo const *info)
{
    auto const real = __real__ZNK24QWebEngineUrlRequestInfo9initiatorEv(info);
    auto const request = __real__ZNK24QWebEngineUrlRequestInfo10requestUrlEv(info);
    std::lock_guard<std::mutex> lock(observationMutex);
    auto o = observations.value(request);
    if (!o) return real;
    ++o->initiators;
    return o->substitute ? o->initiator : real;
}
void __wrap__ZN24QWebEngineUrlRequestInfo5blockEb(QWebEngineUrlRequestInfo *info, bool block)
{
    auto const request = __real__ZNK24QWebEngineUrlRequestInfo10requestUrlEv(info);
    {
        std::lock_guard<std::mutex> lock(observationMutex);
        if (auto o = observations.value(request)) { ++o->blocks; o->blocked = block; }
    }
    __real__ZN24QWebEngineUrlRequestInfo5blockEb(info, block);
}
void __wrap__ZN24QWebEngineUrlRequestInfo8redirectERK4QUrl(QWebEngineUrlRequestInfo *info, QUrl const &url)
{
    auto const request = __real__ZNK24QWebEngineUrlRequestInfo10requestUrlEv(info);
    bool refused = false;
    {
        std::lock_guard<std::mutex> lock(observationMutex);
        if (auto o = observations.value(request)) {
            ++o->redirects;
            o->redirected = url;
            // A mutant must fail an assertion, never feed a bad URL or redirect
            // cycle into a third-party loader and take the suite down with it.
            if (!url.isValid() || !url.isLocalFile() || url != o->expectedRedirect) {
                o->refused = "unexpected redirect refused before loader handoff";
                refused = true;
            }
        } else refused = !url.isValid();
    }
    if (refused) __real__ZN24QWebEngineUrlRequestInfo5blockEb(info, true);
    else __real__ZN24QWebEngineUrlRequestInfo8redirectERK4QUrl(info, url);
}
void __real__ZN17QWebEngineProfile14clearHttpCacheEv(QWebEngineProfile *);
void __wrap__ZN17QWebEngineProfile14clearHttpCacheEv(QWebEngineProfile *profile)
{
    ++cacheClearCalls;
    if (clearMode == ClearMode::Real) __real__ZN17QWebEngineProfile14clearHttpCacheEv(profile);
    else if (clearMode == ClearMode::Immediate)
        QMetaObject::invokeMethod(profile, "clearHttpCacheCompleted", Qt::DirectConnection);
}
void __real__ZN17QWebEngineProfileC1ERK7QStringP7QObject(QWebEngineProfile *, QString const &, QObject *);
void __wrap__ZN17QWebEngineProfileC1ERK7QStringP7QObject(QWebEngineProfile *profile, QString const &name, QObject *parent)
{
    constructionProxies.append(QNetworkProxy::applicationProxy());
    __real__ZN17QWebEngineProfileC1ERK7QStringP7QObject(profile, name, parent);
}
void __real__ZN13QNetworkProxy19setApplicationProxyERKS_(QNetworkProxy const &);
void __wrap__ZN13QNetworkProxy19setApplicationProxyERKS_(QNetworkProxy const &proxy)
{
    applicationProxies.append(proxy);
    __real__ZN13QNetworkProxy19setApplicationProxyERKS_(proxy);
}
QList<QNetworkProxy> __real__ZN20QNetworkProxyFactory19systemProxyForQueryERK18QNetworkProxyQuery(QNetworkProxyQuery const &);
QList<QNetworkProxy> __wrap__ZN20QNetworkProxyFactory19systemProxyForQueryERK18QNetworkProxyQuery(QNetworkProxyQuery const &query)
{
    return systemProxies ? *systemProxies
        : __real__ZN20QNetworkProxyFactory19systemProxyForQueryERK18QNetworkProxyQuery(query);
}
}

namespace {
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

struct Browser
{
    Config::Class cfg;
    sptr<TestSupport::FixtureDictionary> dictionary{new TestSupport::FixtureDictionary};
    std::vector<sptr<Dictionary::Class>> dictionaries{dictionary};
    std::vector<Instances::Group> groups;
    ArticleMaker maker{dictionaries, groups, {}, {}};
    ArticleNetworkAccessManager resources{nullptr, dictionaries, maker,
        cfg.preferences.disallowContentFromOtherSites, cfg.preferences.hideGoldenDictHeader};
    std::unique_ptr<ArticleProfileController> controller;
    Browser(bool blocking = true, bool proxy = false)
    {
        cfg.preferences.maxNetworkCacheSize = 0;
        cfg.preferences.disallowContentFromOtherSites = blocking;
        cfg.preferences.proxyServer.enabled = proxy;
        if (!proxy) controller = std::make_unique<ArticleProfileController>(cfg, resources, nullptr);
    }
    void construct() { controller = std::make_unique<ArticleProfileController>(cfg, resources, nullptr); }
    ArticleRequestInterceptor *attach(QWebEnginePage &page)
    {
        controller->attachPage(&page);
        return dynamic_cast<ArticleRequestInterceptor *>(page.findChild<QObject *>("gdArticleRequestInterceptor", Qt::FindDirectChildrenOnly));
    }
    void publish(QWebEnginePage &page, QSet<QString> const &hosts, bool blocking = true,
                 QMap<QString, QUrl> const &files = {})
    {
        auto policy = std::make_shared<ArticleRequestPolicy>();
        policy->blockExternal = blocking;
        policy->dictionaryHosts = hosts;
        policy->localFileRedirects = files;
        attach(page)->setPolicy(policy);
    }
    QUrl article(QByteArray const &body)
    {
        dictionary->setArticleBody(body);
        ArticleUrls::Lookup lookup;
        lookup.phrase = Config::InputPhrase::fromPhrase(QStringLiteral("policy"));
        lookup.group = 0;
        return ArticleUrls::lookupDocument(lookup);
    }
};

std::shared_ptr<Observation> observe(QUrl const &actual)
{
    auto o = std::make_shared<Observation>();
    std::lock_guard<std::mutex> lock(observationMutex);
    observations.insert(actual, o);
    return o;
}
Observation snapshot(std::shared_ptr<Observation> const &o)
{
    std::lock_guard<std::mutex> lock(observationMutex);
    return *o;
}

class WorkerInterceptor final : public QWebEngineUrlRequestInterceptor
{
    QWebEnginePage &page;
    ArticleRequestInterceptor &production;
public:
    std::shared_ptr<CallbackGate> gate;
    std::function<void()> replace;
    bool publicationReached = false;
    WorkerInterceptor(QWebEnginePage &p, ArticleRequestInterceptor &target)
        : page(p), production(target) { page.setUrlRequestInterceptor(this); }
    ~WorkerInterceptor() override { page.setUrlRequestInterceptor(nullptr); }
    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        // This Qt build dispatches page interceptors on GUI. The worker keeps
        // the REAL request alive for the duration of the forwarded callback,
        // proving the stricter IO contract without manufacturing private Qt data.
        std::thread worker([&] { production.interceptRequest(info); });
        if (gate && replace) {
            {
                std::unique_lock<std::mutex> lock(gate->mutex);
                publicationReached = gate->changed.wait_for(lock, std::chrono::seconds(7), [&] { return gate->entered.load(); });
            }
            if (publicationReached) replace(); // GUI publishes while worker owns old snapshot
            gate->release();
            replace = {};
        }
        worker.join();
    }
};

class DialogAnswer final : public QObject
{
public:
    bool accept = true;
    int dialogs = 0;
    QString user, password, label, initialUser, initialPassword;
    std::function<void()> duringShow;
    DialogAnswer() { qApp->installEventFilter(this); }
    ~DialogAnswer() override { qApp->removeEventFilter(this); }
    bool eventFilter(QObject *object, QEvent *event) override
    {
        auto dialog = qobject_cast<QDialog *>(object);
        if (!dialog || event->type() != QEvent::Show) return false;
        if (!dialog->findChild<QLineEdit *>("userEdit")) return false;
        ++dialogs;
        label = dialog->findChild<QLabel *>("label")->text();
        initialUser = dialog->findChild<QLineEdit *>("userEdit")->text();
        initialPassword = dialog->findChild<QLineEdit *>("passwordEdit")->text();
        QPointer<QDialog> alive(dialog);
        QTimer::singleShot(0, dialog, [this, alive] {
            if (duringShow) duringShow();
            if (!alive) return;
            if (accept) {
                alive->findChild<QLineEdit *>("userEdit")->setText(user);
                alive->findChild<QLineEdit *>("passwordEdit")->setText(password);
                alive->accept();
            } else alive->reject();
        });
        return false;
    }
};

class CountingCache final : public QNetworkDiskCache
{
public:
    int clears = 0;
    using QNetworkDiskCache::QNetworkDiskCache;
    void clear() override { ++clears; QNetworkDiskCache::clear(); }
};
}

class ExternalContent : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStringList reasons;
        QVERIFY2(TestSupport::requiredEnvironment(&reasons), qPrintable(reasons.join('\n')));
        QVERIFY2(TestSupport::loopbackOriginsBindable(&reasons), qPrintable(reasons.join('\n')));
        TestSupport::installWatchdog(TestSupport::BrowserWatchdogSeconds);
    }
    void cleanupTestCase()
    {
        QCOMPARE(ioConfigCalls.load(), 0); QCOMPARE(ioFileCalls.load(), 0); QCOMPARE(ioWidgetCalls.load(), 0);
        TestSupport::clearWatchdog();
    }
    void cleanup()
    {
        std::lock_guard<std::mutex> lock(observationMutex);
        observations.clear();
        clearMode = ClearMode::Real;
        systemProxies.reset();
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void sharedProfileAndAttachment()
    {
        Browser b;
        auto &controller = *b.controller;
        QWebEnginePage first(controller.profile()), second(controller.profile());
        first.settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        auto a = b.attach(first), c = b.attach(second);
        QVERIFY(a && c && a != c);
        QCOMPARE(a->parent(), &first);
        QCOMPARE(c->parent(), &second);
        QCOMPARE(first.profile(), second.profile());
        QCOMPARE(controller.profile()->parent(), &controller);
        QCOMPARE(controller.profile()->storageName(), "goldendict-articles");
        QVERIFY(!controller.profile()->isOffTheRecord());
        QCOMPARE(controller.profile()->persistentCookiesPolicy(), QWebEngineProfile::NoPersistentCookies);
        QCOMPARE(controller.profile()->cachePath(), QDir(Config::getNetworkCacheDir()).filePath("webengine/cache"));
        QCOMPARE(controller.profile()->persistentStoragePath(), QDir(Config::getNetworkCacheDir()).filePath("webengine/storage"));
        auto handler = controller.profile()->urlSchemeHandler("gdlookup");
        QVERIFY(dynamic_cast<ArticleSchemeHandler const *>(handler));
        for (QByteArray name : {"gdpicture", "bres", "gdau", "gdvideo", "gico", "qrcx"})
            QCOMPARE(controller.profile()->urlSchemeHandler(name), handler);
        for (auto settings : {controller.profile()->settings(), first.settings(), second.settings()}) {
            QVERIFY(settings->testAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls));
            QVERIFY(settings->testAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls));
            QVERIFY(settings->testAttribute(QWebEngineSettings::JavascriptEnabled));
            QVERIFY(settings->testAttribute(QWebEngineSettings::LocalStorageEnabled));
            QVERIFY(!settings->testAttribute(QWebEngineSettings::PluginsEnabled));
            QVERIFY(!settings->testAttribute(QWebEngineSettings::BackForwardCacheEnabled));
        }
        controller.attachPage(&first);
        QCOMPARE(b.attach(first), a);
        QCOMPARE(first.findChildren<QObject *>("gdArticleRequestInterceptor", Qt::FindDirectChildrenOnly).size(), 1);
    }

    void policyTable_data()
    {
        QTest::addColumn<QUrl>("request");
        QTest::addColumn<QUrl>("firstParty");
        QTest::addColumn<QUrl>("initiator");
        QTest::addColumn<QSet<QString>>("hosts");
        QTest::addColumn<bool>("blocking");
        QTest::addColumn<bool>("allowed");
        auto row = [](char const *name, char const *r, char const *f, char const *i,
                      QSet<QString> hosts, bool allowed, bool blocking = true) {
            QTest::newRow(name) << QUrl(r) << QUrl(f) << QUrl(i) << hosts << blocking << allowed;
        };
        row("equal", "http://example.org/a", "http://example.org/", "", {}, true);
        row("ports-independent", "https://example.org:123/a", "http://example.org:456/", "", {}, true);
        row("equal-bases", "https://www.example.org/a", "http://img.example.org/", "", {}, true);
        row("retained-one-label-only", "https://x.a.example.org/a", "http://y.b.example.org/", "", {}, false);
        row("dns-subdomain", "http://deep.img.example.org/", "https://example.org/", "", {}, true);
        row("dns-superdomain", "http://example.org/", "https://deep.img.example.org/", "", {}, true);
        row("co-uk-bases", "http://www.foobar.co.uk/", "http://img.foobar.co.uk/", "", {}, true);
        row("co-uk-unrelated", "http://foobar.co.uk/", "http://other.co.uk/", "", {}, false);
        row("short-final-heuristic", "http://x.foobar.info/", "http://y.foobar.info/", "", {}, true);
        row("short-label-no-chop", "http://www.abc.org/", "http://img.abc.org/", "", {}, false);
        row("trailing-dot-case", "http://WWW.EXAMPLE.ORG./", "https://www.example.org/", "", {}, true);
        row("malicious-prefix", "http://evil-example.org/", "http://example.org/", "", {}, false);
        row("malicious-suffix", "http://example.org.evil/", "http://example.org/", "", {}, false);
        row("ip-equal", "http://127.0.0.1:19/", "http://127.0.0.1:23/", "", {}, true);
        row("ip-unequal", "http://127.0.0.2/", "http://127.0.0.1/", "", {}, false);
        row("ip-not-dns-suffix", "http://x.127.0.0.1/", "http://127.0.0.1/", "", {}, false);
        row("ip-v6-equal", "http://[::1]:19/", "http://[0:0:0:0:0:0:0:1]:23/", "", {}, true);
        row("ip-v6-unequal", "http://[2001:db8::2]/", "http://[2001:db8::1]/", "", {}, false);
        row("absent-no-host-privilege", "http://dict.example/", "gdlookup://localhost/", "", {"dict.example"}, false);
        row("opaque-no-host-privilege", "http://dict.example/", "gdlookup://localhost/", "null", {"dict.example"}, false);
        row("opaque-data", "http://dict.example/", "gdlookup://localhost/", "data:text/html,x", {"dict.example"}, false);
        row("empty-hosts-never-equal", "http:///", "about:blank", "", {}, false);
        row("concrete-initiator", "http://dict.example/", "gdlookup://localhost/", "http://dict.example/", {}, true);
        row("first-party-with-concrete", "http://first.example/", "http://first.example/", "http://other.unrelated.net/", {}, true);
        row("frame-cannot-use-dictionaries", "http://dict-b.second.org/", "gdlookup://localhost/", "http://dict-a.first.org/", {"dict-b.second.org"}, false);
        row("application-dictionary", "http://dict.example/", "gdlookup://localhost/", "gdlookup://localhost", {"dict.example"}, true);
        row("application-dictionary-subdomain", "http://img.dict.example/", "gdlookup://localhost/", "bres://fixture-a", {"dict.example"}, true);
        row("application-dictionary-dot-case", "http://dict.example/", "gdlookup://localhost/", "gico://fixture-a", {"DICT.EXAMPLE."}, true);
        row("dictionary-not-host-base", "http://b.example.org/", "gdlookup://localhost/", "gdlookup://localhost", {"a.example.org"}, false);
        row("dictionary-not-superdomain", "http://example.org/", "gdlookup://localhost/", "gdlookup://localhost", {"sub.example.org"}, false);
        row("dictionary-boundary", "http://evil-dict.example/", "gdlookup://localhost/", "gdlookup://localhost", {"dict.example"}, false);
        row("dictionary-ip-exact", "http://127.0.0.2/", "gdlookup://localhost/", "gdlookup://localhost", {"127.0.0.1"}, false);
        row("gdpicture-no-network-privilege", "http://dict.example/", "gdlookup://localhost/", "gdpicture://fixture-a", {"dict.example"}, false);
        for (char const *scheme : {"bres", "gico", "gdau", "gdvideo", "gdpicture"}) {
            QByteArray const name = QByteArray("internal-") + scheme;
            QByteArray const url = QByteArray(scheme) + "://fixture-a/pixel.png";
            row(name, url, "", "", {}, true);
        }
        row("internal-qrcx", "qrcx://localhost/article-style.css", "", "", {}, true);
        row("internal-lookup", "gdlookup://localhost/?word=x&group=0", "", "", {}, true);
        row("invalid-lookup-always-denied", "gdlookup://evil/?word=x&group=0", "", "", {}, false, false);
        row("invalid-resource-always-denied", "bres:///missing-host.png", "", "", {}, false, false);
        row("file-without-mapping", "file:///no-scan-needed.png", "", "", {}, true);
        row("data", "data:text/plain,hello", "", "", {}, true);
        row("about", "about:blank", "", "", {}, true);
        row("blob", "blob:https://dict.example/token", "", "", {}, true);
        row("unknown-blocked", "unhandled://example.org/x", "", "", {}, false);
        row("unknown-flag-off", "unhandled://example.org/x", "", "", {}, true, false);
        row("https-flag-off", "https://unrelated.example/x", "", "", {}, true, false);
        row("invalid-url-flag-off", "http://[", "", "", {}, false, false);
        row("empty-scheme-flag-off", "relative-image.png", "", "", {}, false, false);
        row("empty-url-flag-off", "", "", "", {}, false, false);
    }
    void policyTable()
    {
        QFETCH(QUrl, request); QFETCH(QUrl, firstParty); QFETCH(QUrl, initiator);
        QFETCH(QSet<QString>, hosts); QFETCH(bool, blocking); QFETCH(bool, allowed);
        QDeadlineTimer deadline(10000);
        HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.1")), qPrintable(origin.errorString()));
        Browser b(blocking);
        QWebEnginePage page(b.controller->profile());
        b.publish(page, hosts, blocking);
        auto const actual = origin.url("/matrix.png");
        auto o = observe(actual);
        o->substitute = true; o->request = request; o->firstParty = firstParty; o->initiator = initiator;
        QSignalSpy finished(&page, &QWebEnginePage::loadFinished);
        page.load(actual);
        WAIT_FOR(!finished.isEmpty());
        auto result = snapshot(o);
        QVERIFY(result.requests > 0); // production calls the observed URL entry point
        QCOMPARE(result.blocks, allowed ? 0 : 1);
        QCOMPARE(result.blocked, !allowed);
        QCOMPARE(result.redirects, 0);
        QVERIFY(result.refused.isEmpty());
        QCOMPARE(origin.requestCount("/matrix.png"), allowed ? 1 : 0);
    }

    void sameUrlPoliciesAndLivePreferences()
    {
        QDeadlineTimer deadline(15000);
        HttpFixture a, c;
        QVERIFY2(a.start(QHostAddress("127.0.0.1")), qPrintable(a.errorString()));
        QVERIFY2(c.start(QHostAddress("127.0.0.2")), qPrintable(c.errorString()));
        Browser b;
        QWebEnginePage first(b.controller->profile()), second(b.controller->profile());
        b.publish(first, {"127.0.0.1"});
        b.publish(second, {"127.0.0.2"});
        auto url = b.article(QString("<img src='%1'><img src='%2'><img src='bres://dict-a/pixel.png'>")
            .arg(a.url("/a.png").toString(), c.url("/b.png").toString()).toUtf8());
        QSignalSpy one(&first, &QWebEnginePage::loadFinished), two(&second, &QWebEnginePage::loadFinished);
        first.load(url);
        WAIT_FOR(!one.isEmpty());
        QCOMPARE(a.requestCount("/a.png"), 1); QCOMPARE(c.requestCount("/b.png"), 0);
        second.load(url);
        WAIT_FOR(!two.isEmpty());
        QCOMPARE(first.url(), second.url());
        QCOMPARE(a.requestCount("/a.png"), 1); QCOMPARE(c.requestCount("/b.png"), 1);
        QVERIFY(b.dictionary->requestedResources().contains("pixel.png"));
        // Updating only the preference preserves the different host sets.
        b.cfg.preferences.disallowContentFromOtherSites = false;
        b.controller->applyPreferences();
        auto reload = [&](QWebEnginePage &page, QSignalSpy &spy) {
            spy.clear(); page.triggerAction(QWebEnginePage::ReloadAndBypassCache);
        };
        reload(first, one); WAIT_FOR(!one.isEmpty());
        QCOMPARE(a.requestCount("/a.png"), 2); QCOMPARE(c.requestCount("/b.png"), 2);
        b.cfg.preferences.disallowContentFromOtherSites = true;
        b.controller->applyPreferences();
        reload(first, one); WAIT_FOR(!one.isEmpty());
        QCOMPARE(a.requestCount("/a.png"), 3); QCOMPARE(c.requestCount("/b.png"), 2);
        reload(second, two); WAIT_FOR(!two.isEmpty());
        QCOMPARE(a.requestCount("/a.png"), 3); QCOMPARE(c.requestCount("/b.png"), 3);
    }

    void immutableSnapshotDuringReplacement()
    {
        QDeadlineTimer deadline(12000);
        HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.1")), qPrintable(origin.errorString()));
        Browser b;
        QWebEnginePage page(b.controller->profile());
        auto interceptor = b.attach(page); QVERIFY(interceptor);
        auto policy = std::make_shared<ArticleRequestPolicy>();
        policy->blockExternal = true; policy->dictionaryHosts = {"allowed.dictionary.invalid"};
        std::weak_ptr<ArticleRequestPolicy const> old = policy;
        interceptor->setPolicy(policy); policy.reset();
        auto const actual = origin.url("/latch.png");
        auto o = observe(actual);
        o->substitute = true; o->request = QUrl("http://allowed.dictionary.invalid/image.png");
        o->firstParty = o->initiator = QUrl("gdlookup://localhost");
        auto gate = std::make_shared<CallbackGate>(); o->gate = gate;
        struct Release { std::shared_ptr<CallbackGate> gate; ~Release() { gate->release(); } } release{gate};
        WorkerInterceptor worker(page, *interceptor);
        worker.gate = gate;
        bool retained = false;
        worker.replace = [&] { b.publish(page, {}); retained = !old.expired(); };
        QSignalSpy done(&page, &QWebEnginePage::loadFinished);
        page.load(actual); WAIT_FOR(!done.isEmpty());
        QVERIFY(worker.publicationReached); QVERIFY(retained);
        QVERIFY(!snapshot(o).threads.contains(QThread::currentThreadId()));
        QCOMPARE(origin.requestCount("/latch.png"), 1);
        QCOMPARE(snapshot(o).blocks, 0);
        WAIT_FOR(old.expired());
        { std::lock_guard<std::mutex> lock(gate->mutex); QVERIFY(!gate->timedOut); }
        auto second = origin.url("/new-snapshot.png");
        auto o2 = observe(second); o2->substitute = true;
        o2->request = o->request; o2->firstParty = o->firstParty; o2->initiator = o->initiator;
        done.clear(); page.load(second); WAIT_FOR(!done.isEmpty());
        QCOMPARE(snapshot(o2).blocks, 1); QCOMPARE(origin.requestCount("/new-snapshot.png"), 0);
        interceptor->setPolicy(nullptr); // null publication restores the default value policy
        done.clear(); page.load(second); WAIT_FOR(!done.isEmpty());
        QCOMPARE(origin.requestCount("/new-snapshot.png"), 1);
        QCOMPARE(ioConfigCalls.load(), 0); QCOMPARE(ioFileCalls.load(), 0); QCOMPARE(ioWidgetCalls.load(), 0);
    }

    void absentInitiatorCannotUseDictionaryHosts()
    {
        QDeadlineTimer deadline(10000);
        HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.1")), qPrintable(origin.errorString()));
        Browser b;
        QWebEnginePage page(b.controller->profile());
        b.publish(page, {"127.0.0.1"});
        QSignalSpy finished(&page, &QWebEnginePage::loadFinished);
        page.setHtml(QString("<img src='%1'>").arg(origin.url("/opaque.png").toString()));
        WAIT_FOR(!finished.isEmpty());
        QVERIFY(finished.constLast().at(0).toBool());
        QCOMPARE(origin.requestCount("/opaque.png"), 0);
    }

    void diagnosticsFollowSnapshotAndRedactCredentials()
    {
        QDeadlineTimer deadline(12000);
        HttpFixture origin;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        Browser b;
        QWebEnginePage page(b.controller->profile()); b.publish(page, {});
        auto actual = origin.url("/diagnostic.png");
        auto o = observe(actual); o->substitute = true; o->request = actual;
        o->request.setUserName("url-user"); o->request.setPassword("url-password");
        o->firstParty = o->initiator = QUrl("gdlookup://localhost");
        auto article = b.article(QString("<img src='%1'>").arg(actual.toString()).toUtf8());
        policyWarnings.clear(); previousHandler = qInstallMessageHandler(recordPolicyWarning);
        struct Restore { ~Restore() { qInstallMessageHandler(previousHandler); previousHandler = nullptr; } } restore;
        auto warnings = [] { std::lock_guard<std::mutex> lock(observationMutex); return policyWarnings; };
        QSignalSpy done(&page, &QWebEnginePage::loadFinished);
        page.load(article); WAIT_FOR(!done.isEmpty());
        QCOMPARE(warnings().size(), 1);
        done.clear(); page.triggerAction(QWebEnginePage::ReloadAndBypassCache); WAIT_FOR(!done.isEmpty());
        QCOMPARE(warnings().size(), 1);
        b.publish(page, {});
        done.clear(); page.triggerAction(QWebEnginePage::ReloadAndBypassCache); WAIT_FOR(!done.isEmpty());
        QCOMPARE(warnings().size(), 2); QCOMPARE(origin.requestCount("/diagnostic.png"), 0);
        for (auto const &message : warnings()) {
            QVERIFY(!message.contains("url-user")); QVERIFY(!message.contains("url-password"));
        }
    }

    void realFrameOwnOriginAndCrossOriginBarrier()
    {
        QDeadlineTimer deadline(15000);
        HttpFixture a, c, third;
        QVERIFY2(a.start(QHostAddress("127.0.0.1")), qPrintable(a.errorString()));
        QVERIFY2(c.start(QHostAddress("127.0.0.2")), qPrintable(c.errorString()));
        QVERIFY2(third.start(QHostAddress("127.0.0.3")), qPrintable(third.errorString()));
        Browser b;
        QWebEnginePage page(b.controller->profile());
        b.publish(page, {"127.0.0.1", "127.0.0.2", "127.0.0.3"});
        auto url = b.article(QString("<iframe name='remote' src='%1'></iframe><iframe src='%2'></iframe><iframe src='%3'></iframe>")
            .arg(a.url("/redirect").toString(), third.url("/deny-frame").toString(), third.url("/deny-csp").toString()).toUtf8());
        QSignalSpy done(&page, &QWebEnginePage::loadFinished);
        page.load(url); WAIT_FOR(!done.isEmpty());
        QCOMPARE(a.requestCount("/redirect"), 1);
        QCOMPARE(a.requestCount("/dir/first.html"), 1);
        QCOMPARE(a.requestCount("/dir/style.css"), 1);
        // The fixture references the pixel from both HTML and CSS; with
        // NoCache Chromium can legitimately issue two requests.
        QVERIFY(a.requestCount("/dir/pixel.png") >= 1);
        WAIT_FOR(a.requestCount("/dir/data.json") == 1);
        QCOMPARE(third.requestCount("/deny-frame"), 1);
        QCOMPARE(third.requestCount("/deny-csp"), 1);
        auto frame = page.findFrameByName("remote"); QVERIFY(frame && frame->isValid());
        QString const cross = c.url("/cross.png").toString();
        frame->runJavaScript(QString("window.crossDone=false;let img=new Image;img.onload=img.onerror=()=>window.crossDone=true;img.src='%1';document.body.append(img)").arg(cross));
        // The state is owned by the real remote frame, not an arbitrary delay.
        auto remoteState = std::make_shared<bool>(false);
        WAIT_FOR(([&] {
            frame->runJavaScript("window.crossDone===true", [remoteState](QVariant const &v) { *remoteState = v.toBool(); });
            return *remoteState;
        })());
        QCOMPARE(c.requestCount("/cross.png"), 0);
        b.cfg.preferences.disallowContentFromOtherSites = false;
        b.controller->applyPreferences();
        frame->runJavaScript(QString("let img2=new Image;img2.src='%1';document.body.append(img2)").arg(c.url("/unblocked.png").toString()));
        WAIT_FOR(c.requestCount("/unblocked.png") == 1);
    }

    void precomputedFilesSurvivePreferenceReplacement()
    {
        QDeadlineTimer deadline(10000);
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QString const path = temp.filePath("fixed file.txt");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("file-map-marker"), 15); file.close();
        QUrl const missing = QUrl::fromLocalFile(temp.filePath("absent file.txt"));
        QUrl const fixed = QUrl::fromLocalFile(path);
        Browser b;
        QWebEnginePage page(b.controller->profile());
        b.publish(page, {}, true, {{missing.toString(QUrl::FullyEncoded), fixed}});
        auto o = observe(missing); o->expectedRedirect = fixed;
        b.cfg.preferences.disallowContentFromOtherSites = false;
        b.controller->applyPreferences();
        WorkerInterceptor worker(page, *b.attach(page));
        QSignalSpy done(&page, &QWebEnginePage::loadFinished);
        page.load(missing); WAIT_FOR(!done.isEmpty());
        auto result = snapshot(o);
        QCOMPARE(result.redirects, 1); QCOMPARE(result.redirected, fixed); QVERIFY(result.refused.isEmpty());
        QVERIFY(!result.threads.contains(QThread::currentThreadId()));
        QCOMPARE(javascript(page, "document.body.textContent", deadline).toString().trimmed(), "file-map-marker");
    }

    void rejectedFileMappings_data()
    {
        QTest::addColumn<QUrl>("fixed");
        QTest::newRow("invalid") << QUrl("http://[");
        QTest::newRow("invalid-local") << QUrl("file://[");
        QTest::newRow("remote") << QUrl("https://example.org/not-a-file");
        QTest::newRow("relative") << QUrl("not-a-file");
        QTest::newRow("self") << QUrl();
    }
    void rejectedFileMappings()
    {
        QFETCH(QUrl, fixed);
        QDeadlineTimer deadline(10000);
        QTemporaryDir temp; QVERIFY(temp.isValid());
        auto missing = QUrl::fromLocalFile(temp.filePath("missing file.txt"));
        if (fixed.isEmpty()) fixed = missing;
        Browser b;
        QWebEnginePage page(b.controller->profile());
        b.publish(page, {}, true, {{missing.toString(QUrl::FullyEncoded), fixed}});
        auto o = observe(missing); // expectedRedirect is empty: refuse any mutation before forwarding
        QSignalSpy done(&page, &QWebEnginePage::loadFinished);
        page.load(missing); WAIT_FOR(!done.isEmpty());
        QCOMPARE(snapshot(o).redirects, 0); QVERIFY(snapshot(o).refused.isEmpty());
        QVERIFY(!done.constLast().at(0).toBool());
    }

    void preferences_data()
    {
        QTest::addColumn<int>("mode"); QTest::addColumn<int>("expected");
        QTest::newRow("no-proxy") << -1 << int(QNetworkProxy::NoProxy);
        QTest::newRow("socks5") << int(Config::ProxyServer::Socks5) << int(QNetworkProxy::Socks5Proxy);
        QTest::newRow("http-connect") << int(Config::ProxyServer::HttpConnect) << int(QNetworkProxy::HttpProxy);
        QTest::newRow("http-get") << int(Config::ProxyServer::HttpGet) << int(QNetworkProxy::HttpCachingProxy);
        QTest::newRow("system-empty") << 3 << int(QNetworkProxy::NoProxy);
        QTest::newRow("system-proxy") << 4 << int(QNetworkProxy::HttpProxy);
    }
    void preferences()
    {
        QFETCH(int, mode); QFETCH(int, expected);
        // Qt's current default UA omits the application name. Choose a name
        // present in that real default to make removal/restoration falsifiable.
        struct RestoreName { QString name = QCoreApplication::applicationName();
            ~RestoreName() { QCoreApplication::setApplicationName(name); } } restoreName;
        QCoreApplication::setApplicationName("Mozilla");
        Browser b(true, true);
        auto &p = b.cfg.preferences.proxyServer;
        p.enabled = mode >= 0; p.useSystemProxy = mode >= 3;
        p.type = Config::ProxyServer::Type(qMax(0, mode));
        p.host = "127.0.0.3"; p.port = 3137; p.user = "proxy-pref"; p.password = "proxy-pref-secret";
        if (mode == 3) systemProxies = QList<QNetworkProxy>{};
        if (mode == 4) {
            systemProxies = QList<QNetworkProxy>{QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.2", 3138),
                                                 QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.3", 3139)};
            p.systemProxyUser = "system-pref"; p.systemProxyPassword = "system-pref-secret";
        }
        b.resources.setProxy(QNetworkProxy::NoProxy);
        int const before = constructionProxies.size(), selectionsBefore = applicationProxies.size();
        b.construct();
        QVERIFY(applicationProxies.size() > selectionsBefore);
        QCOMPARE(int(applicationProxies[selectionsBefore].type()), expected);
        QCOMPARE(int(applicationProxies.constLast().type()), expected);
        QCOMPARE(constructionProxies.size(), before + 1);
        QCOMPARE(int(constructionProxies.constLast().type()), expected);
        QCOMPARE(int(QNetworkProxy::applicationProxy().type()), expected);
        QCOMPARE(b.resources.proxy().type(), QNetworkProxy::DefaultProxy);
        if (mode >= 0 && mode < 3) {
            QCOMPARE(QNetworkProxy::applicationProxy().hostName(), p.host);
            QCOMPARE(QNetworkProxy::applicationProxy().port(), p.port);
            QCOMPARE(QNetworkProxy::applicationProxy().user(), p.user);
            QCOMPARE(QNetworkProxy::applicationProxy().password(), p.password);
        }
        if (mode == 4) {
            QCOMPARE(QNetworkProxy::applicationProxy().hostName(), "127.0.0.2");
            QCOMPARE(QNetworkProxy::applicationProxy().port(), quint16(3138));
            QCOMPARE(QNetworkProxy::applicationProxy().user(), p.systemProxyUser);
            QCOMPARE(QNetworkProxy::applicationProxy().password(), p.systemProxyPassword);
        }
        auto profile = b.controller->profile();
        QString const defaultAgent = profile->httpUserAgent();
        QVERIFY(!defaultAgent.isEmpty());
        QVERIFY(defaultAgent.contains(QCoreApplication::applicationName()));
        b.cfg.preferences.hideGoldenDictHeader = true;
        b.cfg.preferences.maxNetworkCacheSize = std::numeric_limits<int>::max();
        b.controller->applyPreferences();
        QString hidden = defaultAgent; hidden.remove(QCoreApplication::applicationName());
        QCOMPARE(profile->httpUserAgent(), hidden);
        QCOMPARE(profile->httpCacheType(), QWebEngineProfile::DiskHttpCache);
        QCOMPARE(profile->httpCacheMaximumSize(), std::numeric_limits<int>::max());
        auto cache = qobject_cast<QNetworkDiskCache *>(b.resources.cache()); QVERIFY(cache);
        QCOMPARE(cache->maximumCacheSize(), qint64(std::numeric_limits<int>::max()));
        QCOMPARE(cache->cacheDirectory(), QDir(Config::getNetworkCacheDir()).absolutePath() + '/');
        b.cfg.preferences.maxNetworkCacheSize = 3;
        b.cfg.preferences.hideGoldenDictHeader = false;
        b.controller->applyPreferences();
        QCOMPARE(profile->httpCacheMaximumSize(), 3 << 20);
        QCOMPARE(cache->maximumCacheSize(), qint64(3 << 20));
        QCOMPARE(profile->httpUserAgent(), defaultAgent);
        for (int size : {0, -1}) {
            b.cfg.preferences.maxNetworkCacheSize = size;
            b.controller->applyPreferences();
            QCOMPARE(profile->httpCacheType(), QWebEngineProfile::NoCache);
            QCOMPARE(profile->httpCacheMaximumSize(), 3 << 20); // disabling never passes zero as a size
            QVERIFY(!b.resources.cache());
        }
        p.enabled = false;
        b.controller->applyPreferences();
        QCOMPARE(QNetworkProxy::applicationProxy().type(), QNetworkProxy::NoProxy);
    }

    void realOriginAuthentication_data()
    { QTest::addColumn<bool>("accept"); QTest::newRow("accept") << true; QTest::newRow("cancel") << false; }
    void realOriginAuthentication()
    {
        QFETCH(bool, accept);
        QDeadlineTimer deadline(12000);
        HttpFixture origin; QVERIFY2(origin.start(QHostAddress("127.0.0.1")), qPrintable(origin.errorString()));
        Browser b(false);
        b.cfg.preferences.proxyServer.user = "preserve-proxy";
        b.cfg.preferences.proxyServer.password = "preserve-proxy-secret";
        QWebEnginePage page(b.controller->profile()); b.attach(page); b.controller->attachPage(&page);
        QSignalSpy auth(&page, &QWebEnginePage::authenticationRequired), proxy(&page, &QWebEnginePage::proxyAuthenticationRequired);
        DialogAnswer answer; answer.accept = accept;
        answer.user = HttpFixture::originUser(); answer.password = HttpFixture::originPassword();
        QSignalSpy done(&page, &QWebEnginePage::loadFinished);
        page.load(origin.url("/auth")); WAIT_FOR(!done.isEmpty());
        QCOMPARE(auth.size(), 1); QCOMPARE(proxy.size(), 0); QCOMPARE(answer.dialogs, 1);
        QVERIFY(answer.label.contains("http://127.0.0.1:" + QString::number(origin.port())));
        QVERIFY(answer.label.contains(HttpFixture::originRealm()));
        QCOMPARE(b.cfg.preferences.proxyServer.user, "preserve-proxy");
        QCOMPARE(b.cfg.preferences.proxyServer.password, "preserve-proxy-secret");
        QCOMPARE(origin.requests().constLast().originAuthorized, accept);
        if (accept) QVERIFY(javascript(page, "document.body.textContent", deadline).toString().contains("origin-authenticated"));
    }

    void realProxyAuthentication_data()
    { QTest::addColumn<bool>("accept"); QTest::newRow("saved-fallback") << true; QTest::newRow("cancel") << false; }
    void realProxyAuthentication()
    {
        QFETCH(bool, accept);
        QDeadlineTimer deadline(15000);
        HttpFixture origin, proxy;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        QVERIFY2(proxy.start(QHostAddress("127.0.0.1")), qPrintable(proxy.errorString()));
        Browser b(false, true);
        auto &p = b.cfg.preferences.proxyServer;
        p.type = Config::ProxyServer::HttpConnect; p.host = "127.0.0.1"; p.port = proxy.port();
        b.construct();
        // Deliberately configure saved fallback AFTER the proxy was applied.
        // Chromium must receive a real 407 and signal the page, then use it.
        if (accept) { p.user = HttpFixture::proxyUser(); p.password = HttpFixture::proxyPassword(); }
        QWebEnginePage page(b.controller->profile()); b.attach(page);
        QSignalSpy challenge(&page, &QWebEnginePage::proxyAuthenticationRequired), direct(&page, &QWebEnginePage::authenticationRequired);
        DialogAnswer answer; answer.accept = false;
        QSignalSpy done(&page, &QWebEnginePage::loadFinished);
        int const resolutions = proxyTargetResolutions.load();
        auto target = origin.url("/");
        target.setHost("proxy-target.invalid");
        page.load(target); WAIT_FOR(!done.isEmpty());
        QVERIFY2(challenge.size() > 0, "a genuine HTTP proxy 407 must reach the attached page");
        QCOMPARE(direct.size(), 0);
        QVERIFY(!proxy.requests().isEmpty()); QVERIFY(proxy.requests().first().proxyRequest);
        QVERIFY(!proxy.requests().first().proxied);
        if (accept) {
            QVERIFY(proxy.requests().constLast().proxied);
            QVERIFY(proxyTargetResolutions.load() > resolutions);
            QCOMPARE(answer.dialogs, 0);
            QCOMPARE(QNetworkProxy::applicationProxy().user(), HttpFixture::proxyUser());
            QCOMPARE(QNetworkProxy::applicationProxy().password(), HttpFixture::proxyPassword());
            QCOMPARE(origin.requestCount("/"), 1);
            QVERIFY(javascript(page, "document.body.textContent", deadline).toString().contains("goldendict-http-fixture"));
        } else {
            QCOMPARE(answer.dialogs, 1);
            QCOMPARE(origin.requestCount("/"), 0);
            QVERIFY(p.user.isEmpty()); QVERIFY(p.password.isEmpty());
        }
    }

    void retainedNetworkAuthentication_data()
    {
        QTest::addColumn<bool>("proxy"); QTest::addColumn<bool>("accept"); QTest::addColumn<bool>("systemSaved");
        QTest::addColumn<bool>("wrongSaved");
        QTest::newRow("origin-accept") << false << true << false << false;
        QTest::newRow("origin-cancel") << false << false << false << false;
        QTest::newRow("proxy-prompt-accept") << true << true << false << false;
        QTest::newRow("proxy-cancel") << true << false << false << false;
        QTest::newRow("proxy-system-fallback") << true << true << true << false;
        QTest::newRow("proxy-wrong-saved-prompt") << true << true << false << true;
        QTest::newRow("proxy-wrong-saved-cancel") << true << false << false << true;
    }
    void retainedNetworkAuthentication()
    {
        QFETCH(bool, proxy); QFETCH(bool, accept); QFETCH(bool, systemSaved); QFETCH(bool, wrongSaved);
        QDeadlineTimer deadline(12000);
        HttpFixture origin, gateway;
        QVERIFY2(origin.start(QHostAddress("127.0.0.2")), qPrintable(origin.errorString()));
        QVERIFY2(gateway.start(QHostAddress("127.0.0.1")), qPrintable(gateway.errorString()));
        Browser b(false, true);
        auto &p = b.cfg.preferences.proxyServer;
        p.enabled = proxy; p.type = Config::ProxyServer::HttpConnect; p.host = "127.0.0.1"; p.port = gateway.port();
        if (!proxy) { p.user = "unrelated-proxy-user"; p.password = "unrelated-proxy-password"; }
        b.construct();
        if (systemSaved) {
            p.useSystemProxy = true;
            p.systemProxyUser = HttpFixture::proxyUser(); p.systemProxyPassword = HttpFixture::proxyPassword();
        }
        if (wrongSaved) { p.user = "wrong-saved-user"; p.password = "wrong-saved-password"; }
        QSignalSpy direct(&b.resources, &QNetworkAccessManager::authenticationRequired);
        QSignalSpy proxied(&b.resources, &QNetworkAccessManager::proxyAuthenticationRequired);
        DialogAnswer answer; answer.accept = accept;
        answer.user = proxy ? HttpFixture::proxyUser() : HttpFixture::originUser();
        answer.password = proxy ? HttpFixture::proxyPassword() : HttpFixture::originPassword();
        QUrl target = origin.url(proxy ? "/" : "/auth");
        if (proxy) target.setHost("proxy-target.invalid");
        auto reply = b.resources.get(QNetworkRequest(target));
        QSignalSpy done(reply, &QNetworkReply::finished);
        WAIT_FOR(!done.isEmpty());
        QCOMPARE(direct.size(), proxy ? 0 : 1); QCOMPARE(proxied.size(), proxy ? (wrongSaved ? 2 : 1) : 0);
        QCOMPARE(answer.dialogs, systemSaved ? 0 : 1);
        QCOMPARE(reply->error(), accept ? QNetworkReply::NoError
            : proxy ? QNetworkReply::ProxyAuthenticationRequiredError : QNetworkReply::AuthenticationRequiredError);
        if (accept) QVERIFY(reply->readAll().contains(proxy ? "goldendict-http-fixture" : "origin-authenticated"));
        if (!proxy) { QCOMPARE(p.user, "unrelated-proxy-user"); QCOMPARE(p.password, "unrelated-proxy-password"); }
        else if (systemSaved) { QVERIFY(p.user.isEmpty()); QCOMPARE(p.systemProxyUser, answer.user); }
        else if (accept) { QCOMPARE(p.user, answer.user); QCOMPARE(p.password, answer.password); QVERIFY(p.systemProxyUser.isEmpty()); }
        else {
            QCOMPARE(p.user, wrongSaved ? QStringLiteral("wrong-saved-user") : QString());
            QCOMPARE(p.password, wrongSaved ? QStringLiteral("wrong-saved-password") : QString());
        }
        if (wrongSaved) {
            QCOMPARE(answer.initialUser, QStringLiteral("wrong-saved-user"));
            QCOMPARE(answer.initialPassword, QStringLiteral("wrong-saved-password"));
        }
        if (!proxy) { QVERIFY(answer.initialUser.isEmpty()); QVERIFY(answer.initialPassword.isEmpty()); }
        reply->deleteLater();
    }

    void retiringAuthAdapterDuringDialog()
    {
        for (bool proxy : {false, true}) {
            Config::Class cfg;
            QAuthenticator auth; auth.setRealm("retiring-realm");
            std::unique_ptr<QObject> adapter = proxy
                ? std::unique_ptr<QObject>(new ProxyAuthAdapter(cfg, nullptr))
                : std::unique_ptr<QObject>(new OriginAuthAdapter(nullptr));
            DialogAnswer answer; answer.accept = true;
            answer.user = "retired-user"; answer.password = "retired-password";
            answer.duringShow = [&] { adapter.reset(); };
            if (proxy) static_cast<ProxyAuthAdapter *>(adapter.get())->authenticate(QUrl("https://example.org/"), &auth, "proxy.invalid");
            else static_cast<OriginAuthAdapter *>(adapter.get())->authenticate(QUrl("https://example.org/"), &auth);
            QCOMPARE(answer.dialogs, 1); QVERIFY(!adapter); QVERIFY(auth.isNull());
            QVERIFY(cfg.preferences.proxyServer.user.isEmpty()); QVERIFY(cfg.preferences.proxyServer.password.isEmpty());
        }
    }

    void exitBarrier_data()
    { QTest::addColumn<int>("mode"); QTest::newRow("real-completion") << 0; QTest::newRow("immediate-completion") << 1; QTest::newRow("five-second-timeout") << 2; }
    void exitBarrier()
    {
        QFETCH(int, mode);
        QDeadlineTimer deadline(8500);
        Browser b(false);
        QWebEnginePage page(b.controller->profile()); b.attach(page);
        auto cache = new CountingCache(&b.resources);
        cache->setCacheDirectory(Config::getNetworkCacheDir());
        b.resources.setCache(cache);
        cache->clears = 0;
        QSignalSpy finished(b.controller.get(), &ArticleProfileController::exitCachesReady);
        clearMode = ClearMode(mode);
        int const before = cacheClearCalls;
        QElapsedTimer elapsed; elapsed.start();
        if (mode == 2) QTest::ignoreMessage(QtWarningMsg, "ArticleCache: exit clear timed out after 5000 ms");
        b.controller->clearForExit();
        QCOMPARE(cache->clears, 1);
        b.controller->clearForExit();
        WAIT_FOR(!finished.isEmpty());
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.constFirst().at(0).toBool(), mode == 2);
        QCOMPARE(cacheClearCalls, before + 1);
        if (mode == 2) { QVERIFY(elapsed.elapsed() >= 4990); QVERIFY(elapsed.elapsed() < 7000); }
        QMetaObject::invokeMethod(b.controller->profile(), "clearHttpCacheCompleted", Qt::DirectConnection);
        b.controller->clearForExit();
        QCOMPARE(finished.size(), 1); QCOMPARE(cacheClearCalls, before + 1);
        // The barrier completed while the page and its interceptor remained live.
        QVERIFY(b.attach(page)); QCOMPARE(page.profile(), b.controller->profile());
    }

    void ownershipAndWrongOrder()
    {
        QDeadlineTimer deadline(10000);
        Browser b(false);
        QPointer<QWebEngineProfile> profile = b.controller->profile();
        QPointer<ArticleRequestInterceptor> interceptor;
        {
            QWebEnginePage page(profile);
            interceptor = b.attach(page);
            delete interceptor.data();
            QVERIFY(!interceptor);
            interceptor = b.attach(page); QVERIFY(interceptor);
            b.controller.reset(); // invalid caller order: profile still outlives page
            QVERIFY(profile);
            QSignalSpy done(&page, &QWebEnginePage::loadFinished);
            page.setHtml("<p>owner-retired</p>"); WAIT_FOR(!done.isEmpty());
            QVERIFY(done.constLast().at(0).toBool());
        }
        QVERIFY(!interceptor);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        WAIT_FOR(!profile);
        Browser normal;
        QPointer<ArticleRequestInterceptor> child;
        { QWebEnginePage page(normal.controller->profile()); child = normal.attach(page); }
        QVERIFY(!child);
        normal.controller->applyPreferences(); // no stale destroyed-page pointer
        QPointer<QWebEngineProfile> normalProfile = normal.controller->profile();
        normal.controller.reset();
        QVERIFY(!normalProfile);
    }
};

int main(int argc, char **argv)
{
    if (TestSupport::isWatchdogProbeProcess()) TestSupport::runWatchdogProbe();
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) return 1;
    qputenv("XDG_CACHE_HOME", sandbox.path().toUtf8());
    qputenv("XDG_DATA_HOME", sandbox.path().toUtf8());
    registerArticleSchemes();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("GoldenDict");
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    ExternalContent tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_external_content.moc"
