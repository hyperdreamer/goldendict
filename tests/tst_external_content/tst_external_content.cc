/* Real-page policy/profile contracts. The link observers retain real Qt
 * request objects and forward only validated redirects to Chromium. */
#include <QtTest>
#include <QApplication>
#include <QAuthenticator>
#include <QAction>
#include <QDeadlineTimer>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkDiskCache>
#include <QNetworkProxy>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QPointer>
#include <QTemporaryDir>
#include <QTemporaryFile>
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

#include "articlebridge.hh"
#include "articleprofile.hh"
#include "articleschemes.hh"
#include "article_netmgr.hh"
#include "articleurls.hh"
#include "articleview.hh"
#include "articlewebview.hh"
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
    QUrl request, firstParty, initiator, expectedRedirect, realInitiator;
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
    o->realInitiator = real;
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

struct ArticlePlayer final : AudioPlayerInterface
{
    QString play(char const *, int) override { return {}; }
    void stop() override {}
};

struct RealArticleFixture
{
    Config::Class controllerConfig;
    Config::Class firstConfig;
    Config::Class secondConfig;
    sptr<TestSupport::FixtureDictionary> dictionary{new TestSupport::FixtureDictionary};
    sptr<TestSupport::FixtureDictionary> otherDictionary{new TestSupport::FixtureDictionary("dict-b", "Fixture Dictionary B")};
    std::vector<sptr<Dictionary::Class>> dictionaries{dictionary, otherDictionary};
    Instances::Groups groups;
    ArticleMaker maker{dictionaries, groups, {}, {}};
    ArticleNetworkAccessManager resources{nullptr, dictionaries, maker,
        controllerConfig.preferences.disallowContentFromOtherSites,
        controllerConfig.preferences.hideGoldenDictHeader};
    std::unique_ptr<ArticleProfileController> controller;
    AudioPlayerPtr player{new ArticlePlayer};
    QAction search;
    HttpFixture originA, originB, originC;

    explicit RealArticleFixture(bool blocking = true, bool proxy = false)
    {
        controllerConfig.preferences.maxNetworkCacheSize = 0;
        controllerConfig.preferences.disallowContentFromOtherSites = blocking;
        controllerConfig.preferences.proxyServer.enabled = proxy;
        firstConfig = controllerConfig;
        secondConfig = controllerConfig;

        Instances::Group group(QStringLiteral("Fixture group"));
        group.id = 17;
        group.dictionaries = dictionaries;
        groups.push_back(group);

        dictionary->setResources({{QStringLiteral("fixture-image.png"), HttpFixture::pixelPng()}});
        if (!proxy) construct();
    }

    void construct()
    {
        controller = std::make_unique<ArticleProfileController>(controllerConfig, resources, nullptr);
    }

    void publishHost(Config::Class & config, QString const & name, QUrl const & origin)
    {
        config.webSites.clear();
        config.webSites.append(Config::WebSite(
            QStringLiteral("dict-a"), name, origin.toString(), true, {}, true));
    }

    std::unique_ptr<ArticleView> view(Config::Class & config)
    {
        auto result = std::make_unique<ArticleView>(nullptr, *controller->profile(), resources, player,
                                                    dictionaries, groups, false, config, search,
                                                    nullptr, nullptr);
        result->resize(800, 600);
        result->show();
        return result;
    }

    static QString entryBody()
    {
        // ArticleMaker supplies the real document header, dictionary wrappers,
        // bootstrap and footer. Keep the backend body substantial and avoid
        // nesting the standalone HTTP fixture's duplicate dictionary IDs.
        return QStringLiteral(
            "<h1 id='headword'>fixture-headword</h1>"
            "<p id='multilingual'>Привет 中文 العربية 😀 é</p>"
            "<p id='long-text'>%1</p>"
            "<div class='dsl_opt'>Optional dictionary material</div>")
            .arg(QStringLiteral("A complete dictionary entry with pronunciation, meaning and usage. ").repeated(100));
    }

    QByteArray articleBody(QUrl const & fileUrl) const
    {
        QString const a = originA.url("/").toString();
        QString const b = originB.url("/").toString();
        QString const c = originC.url("/").toString();
        QString const pixel = QString::fromLatin1(HttpFixture::pixelPng().toBase64());
        QString const probes = QStringLiteral(
            "<section id='external-policy-probes'>"
            "<style id='remote-import'>"
            "@import url('%1article/style.css');"
            "@import url('%2article/style.css');"
            "@import url('%3article/style.css');"
            "</style>"
            "<p id='remote-style-marker' class='gd-fixture-marker'>external stylesheet marker</p>"
            "<img id='remote-a' src='%1external-a.png'>"
            "<img id='remote-b' src='%2external-b.png'>"
            "<img id='remote-c' src='%3external-c.png'>"
            "<iframe name='frame-a' src='%1deny-frame'></iframe>"
            "<iframe name='frame-b' src='%2deny-csp'></iframe>"
            "<img id='file-image' src='%4'>"
            "<img id='data-image' src='data:image/png;base64,%5'>"
            "<img id='internal-image' src='bres://dict-a/fixture-image.png'>"
            "<img id='unknown-image' src='unknown-fixture://example/blocked.png'>"
            "<iframe id='about-frame' src='about:blank'></iframe>"
            "<img id='blob-image' alt='blob image'>"
            "<script>(function () {"
            "var image = document.getElementById('blob-image');"
            "var blob = new Blob(['<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\"><rect width=\"1\" height=\"1\" fill=\"red\"/></svg>'], {type:'image/svg+xml'});"
            "image.src = URL.createObjectURL(blob);"
            "})();</script>"
            "</section>")
            .arg(a, b, c, fileUrl.toString(QUrl::FullyEncoded), pixel);
        return (entryBody() + probes).toUtf8();
    }
};

// Poll Chromium asynchronously. Results belong to this view, native page and
// bridge generation; no late callback may update the next document's evidence.
class ArticleDomProbe
{
    struct State {
        QVariantMap value;
        int generation = 0;
        bool pending = false;
    };
    QPointer<ArticleWebView> view;
    QString frameName;
    std::shared_ptr<State> state = std::make_shared<State>();
public:
    explicit ArticleDomProbe(ArticleWebView *owner, QString const &frame = {}): view(owner), frameName(frame) {}
    QVariantMap sample()
    {
        if (!view) return {};
        QPointer<QWebEnginePage> page(view->page());
        QPointer<ArticleBridge> bridge(view->findChild<ArticleBridge *>());
        if (!page || !bridge) return {};
        int const generation = bridge->generation();
        if (generation != state->generation) {
            state = std::make_shared<State>();
            state->generation = generation;
        }
        if (state->pending) return state->value;
        state->pending = true;
        QString const script = QStringLiteral(R"JS(
        (function () {
            function width(id) {
                var element = document.getElementById(id);
                return element ? element.naturalWidth : -1;
            }
            var marker = document.getElementById('remote-style-marker');
            var about = document.getElementById('about-frame');
            return {
                headword: document.getElementById('headword') && document.getElementById('headword').textContent,
                remoteA: width('remote-a'),
                remoteB: width('remote-b'),
                remoteC: width('remote-c'),
                file: width('file-image'),
                data: width('data-image'),
                internal: width('internal-image'),
                unknown: width('unknown-image'),
                blob: width('blob-image'),
                stylesheetColor: marker ? getComputedStyle(marker).color : '',
                aboutDocument: !!(about && about.contentDocument),
                bodyLength: document.getElementById('long-text')?.textContent.length || 0,
                wrappers: document.querySelectorAll('#gdfrom-dict-a').length,
                ownedFile: width('owned-file'),
                ownedFileSource: document.getElementById('owned-file')?.getAttribute('src') || '',
                ownedAgain: width('owned-again'),
                foreignFile: width('foreign-file'),
                fileProbeDone: window.fileProbeDone === true,
                liveDone: window.livePolicyDone === true,
                liveToken: window.livePolicyToken || '',
                frameLocation: location.href,
                bodyText: document.body?.textContent || '',
                deniedMarker: document.body?.textContent.includes('deny-marker') || false,
                opaqueDone: window.opaqueDone === true,
                opaqueOrigin: window.origin,
                opaqueImage: width('opaque-image'),
                firstMarker: document.getElementById('first-marker')?.textContent || '',
                fetchMarker: document.getElementById('fetch-marker')?.textContent || '',
                relativeImage: width('relative-image'),
                sameOrigin: width('same-origin'),
                crossDone: window.crossDone === true,
                crossToken: window.crossToken || '',
                crossB: width('cross-b'),
                crossC: width('cross-c')
            };
        })()
    )JS");
        auto callback = [owner = view, page, bridge, generation, result = state](QVariant const &value) {
            if (!owner || !page || owner->page() != page || !bridge || bridge->generation() != generation) return;
            result->value = value.toMap();
            result->pending = false;
        };
        if (frameName.isEmpty()) page->runJavaScript(script, callback);
        else {
            auto frame = page->findFrameByName(frameName);
            if (!frame || !frame->isValid()) { state->pending = false; return {}; }
            frame->runJavaScript(script, callback);
        }
        return state->value;
    }
};

bool realArticleLoaded(ArticleView & view, QSignalSpy const & pages, int before, QString const & word)
{
    return pages.size() > before && view.getPhrase().phrase == word && !view.getArticlesList().isEmpty();
}
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
        row("ftp-blocked", "ftp://example.org/file.png", "gdlookup://localhost/", "gdlookup://localhost/", {}, false);
        row("ftp-flag-off", "ftp://example.org/file.png", "gdlookup://localhost/", "gdlookup://localhost/", {}, true, false);
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
        QDeadlineTimer deadline(15000);
        RealArticleFixture f;
        QVERIFY2(f.originA.start(QHostAddress("127.0.0.1")), qPrintable(f.originA.errorString()));
        f.publishHost(f.firstConfig, "contributing", f.originA.url("/"));
        QUrl const blockedUrl = f.originA.url("/opaque-blocked.png");
        auto blocked = observe(blockedUrl);
        QString const opaqueDocument = QStringLiteral(
            "<img id='opaque-image' onload='window.opaqueDone=true' onerror='window.opaqueDone=true' src='%1'>")
            .arg(blockedUrl.toString());
        f.dictionary->setArticleBody((RealArticleFixture::entryBody() + QStringLiteral(
            "<iframe name='opaque' sandbox='allow-scripts' srcdoc=\"%1\"></iframe>")
            .arg(opaqueDocument.toHtmlEscaped())).toUtf8());
        auto view = f.view(f.firstConfig);
        auto *web = view->findChild<ArticleWebView *>();
        QVERIFY(web);
        QSignalSpy loaded(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("opaque-initiator", QStringList{"dict-a"}, FtsHighlightSpec{}, 17, false);
        WAIT_FOR(realArticleLoaded(*view, loaded, 0, "opaque-initiator"));
        QCOMPARE(view->getArticlesList(), QStringList{"dict-a"});
        ArticleDomProbe opaque(web, "opaque");
        WAIT_FOR(opaque.sample().value("opaqueDone").toBool());
        QCOMPARE(opaque.sample().value("opaqueOrigin").toString(), "null");
        QCOMPARE(opaque.sample().value("opaqueImage").toInt(), 0);
        QCOMPARE(f.originA.requestCount("/opaque-blocked.png"), 0);
        auto result = snapshot(blocked);
        QCOMPARE(result.blocks, 1);
        QVERIFY(result.initiators > 0);
        QVERIFY(result.realInitiator.scheme().isEmpty());
        QVERIFY(!result.substitute);

        f.controllerConfig.preferences.disallowContentFromOtherSites = false;
        f.controller->applyPreferences();
        auto frame = web->page()->findFrameByName("opaque");
        QVERIFY(frame && frame->isValid());
        frame->runJavaScript(QStringLiteral(
            "window.opaqueDone=false;document.getElementById('opaque-image').src='%1'")
            .arg(f.originA.url("/opaque-allowed.png").toString()));
        WAIT_FOR(opaque.sample().value("opaqueDone").toBool()
                 && opaque.sample().value("opaqueImage").toInt() == 1);
        QCOMPARE(f.originA.requestCount("/opaque-allowed.png"), 1);
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

    void completeArticlePolicyIsPageLocal()
    {
        QDeadlineTimer deadline(30000);
        RealArticleFixture f;
        QVERIFY2(f.originA.start(QHostAddress("127.0.0.1")), qPrintable(f.originA.errorString()));
        QVERIFY2(f.originB.start(QHostAddress("127.0.0.2")), qPrintable(f.originB.errorString()));
        QVERIFY2(f.originC.start(QHostAddress("127.0.0.3")), qPrintable(f.originC.errorString()));

        f.publishHost(f.firstConfig, QStringLiteral("A"), f.originA.url("/"));
        f.publishHost(f.secondConfig, QStringLiteral("B"), f.originB.url("/"));
        // This configured website did not contribute to either lookup.
        f.firstConfig.webSites.append(Config::WebSite("dict-b", "unrelated", f.originC.url("/").toString(), true, {}, true));
        f.secondConfig.webSites.append(f.firstConfig.webSites.constLast());
        QTemporaryFile local(QDir(Config::getConfigDir()).filePath("t26-article-XXXXXX.png"));
        QVERIFY(local.open());
        QCOMPARE(local.write(HttpFixture::pixelPng()), qint64(HttpFixture::pixelPng().size()));
        local.flush();
        QUrl const emittedFile = QUrl::fromLocalFile('/' + QFileInfo(local.fileName()).fileName());
        f.dictionary->setArticleBody(f.articleBody(emittedFile));
        auto unknown = observe(QUrl("unknown-fixture://example/blocked.png"));

        auto first = f.view(f.firstConfig);
        auto second = f.view(f.secondConfig);
        auto *firstWeb = first->findChild<ArticleWebView *>();
        auto *secondWeb = second->findChild<ArticleWebView *>();
        QVERIFY(firstWeb && secondWeb);
        QWebEnginePage *firstPage = firstWeb->page();
        QWebEnginePage *secondPage = secondWeb->page();
        QVERIFY(firstPage && secondPage);
        QSignalSpy firstPages(first.get(), &ArticleView::pageLoaded);
        QSignalSpy secondPages(second.get(), &ArticleView::pageLoaded);

        first->showDefinition("policy", QStringList{"dict-a"}, FtsHighlightSpec{}, 17, false);
        second->showDefinition("policy", QStringList{"dict-a"}, FtsHighlightSpec{}, 17, false);
        WAIT_FOR(realArticleLoaded(*first, firstPages, 0, "policy"));
        WAIT_FOR(realArticleLoaded(*second, secondPages, 0, "policy"));

        QCOMPARE(firstPage->profile(), secondPage->profile());
        QCOMPARE(firstPage->url(), secondPage->url());
        auto *firstPolicy = dynamic_cast<ArticleRequestInterceptor *>(
            firstPage->findChild<QObject *>("gdArticleRequestInterceptor"));
        auto *secondPolicy = dynamic_cast<ArticleRequestInterceptor *>(
            secondPage->findChild<QObject *>("gdArticleRequestInterceptor"));
        QVERIFY(firstPolicy && secondPolicy && firstPolicy != secondPolicy);
        QCOMPARE(firstPolicy->parent(), firstPage);
        QCOMPARE(secondPolicy->parent(), secondPage);

        WAIT_FOR(f.originA.requestCount("/external-a.png") == 1
                 && f.originB.requestCount("/external-b.png") == 1
                 && f.originC.requestCount("/external-c.png") == 0
                 && f.originA.requestCount("/article/style.css") == 1
                 && f.originB.requestCount("/article/style.css") == 1
                 && f.originC.requestCount("/article/style.css") == 0
                 && f.originA.requestCount("/deny-frame") == 1
                 && f.originB.requestCount("/deny-csp") == 1);
        ArticleDomProbe firstDom(firstWeb), secondDom(secondWeb);
        WAIT_FOR(firstDom.sample().value("file").toInt() == 1);
        WAIT_FOR(secondDom.sample().value("file").toInt() == 1);
        auto firstState = firstDom.sample();
        auto secondState = secondDom.sample();
        QVERIFY(firstState.value("bodyLength").toInt() > 4096);
        QCOMPARE(firstState.value("wrappers").toInt(), 1);
        QCOMPARE(first->getArticlesList(), QStringList{"dict-a"});
        QCOMPARE(second->getArticlesList(), QStringList{"dict-a"});
        QCOMPARE(firstState.value("headword").toString(), QStringLiteral("fixture-headword"));
        QCOMPARE(secondState.value("headword").toString(), QStringLiteral("fixture-headword"));
        QCOMPARE(firstState.value("remoteA").toInt(), 1);
        QCOMPARE(firstState.value("remoteB").toInt(), 0);
        QCOMPARE(firstState.value("remoteC").toInt(), 0);
        QCOMPARE(secondState.value("remoteA").toInt(), 0);
        QCOMPARE(secondState.value("remoteB").toInt(), 1);
        QCOMPARE(secondState.value("remoteC").toInt(), 0);
        QCOMPARE(firstState.value("stylesheetColor").toString(), QStringLiteral("rgb(1, 2, 3)"));
        QCOMPARE(secondState.value("stylesheetColor").toString(), QStringLiteral("rgb(1, 2, 3)"));
        QCOMPARE(firstState.value("file").toInt(), 1);
        QCOMPARE(firstState.value("internal").toInt(), 1);
        QCOMPARE(firstState.value("data").toInt(), 1);
        QCOMPARE(firstState.value("blob").toInt(), 1);
        QCOMPARE(firstState.value("unknown").toInt(), 0);
        QCOMPARE(snapshot(unknown).blocks, 2);
        QVERIFY(firstState.value("aboutDocument").toBool());

        // Apply preferences to the current documents. Fresh URLs and image
        // completion events make a missing controller refresh observable.
        auto liveImages = [&](QWebEnginePage &page, QString const &token) {
            page.runJavaScript(QStringLiteral(R"JS((() => {
                window.livePolicyDone = false; window.livePolicyToken = '%1';
                let remaining = 3;
                for (const [id, src] of [['remote-a','%2'], ['remote-b','%3'], ['remote-c','%4']]) {
                    const image = document.getElementById(id);
                    image.onload = image.onerror = () => { if (--remaining === 0) window.livePolicyDone = true; };
                    image.src = src + '?live-policy=%1';
                }
            })())JS").arg(token, f.originA.url("/external-a.png").toString(),
                        f.originB.url("/external-b.png").toString(), f.originC.url("/external-c.png").toString()));
        };
        auto liveFinished = [](ArticleDomProbe &probe, QString const &token) {
            auto const state = probe.sample();
            return state.value("liveToken").toString() == token && state.value("liveDone").toBool();
        };
        f.originA.clearRequests();
        f.originB.clearRequests();
        f.originC.clearRequests();
        f.controllerConfig.preferences.disallowContentFromOtherSites = false;
        f.controller->applyPreferences();
        liveImages(*firstPage, "allowed");
        liveImages(*secondPage, "allowed");
        WAIT_FOR(liveFinished(firstDom, "allowed"));
        WAIT_FOR(liveFinished(secondDom, "allowed"));
        QCOMPARE(f.originA.requestCount("/external-a.png"), 2);
        QCOMPARE(f.originB.requestCount("/external-b.png"), 2);
        QCOMPARE(f.originC.requestCount("/external-c.png"), 2);
        for (auto const &state : {firstDom.sample(), secondDom.sample()}) {
            QCOMPARE(state.value("remoteA").toInt(), 1);
            QCOMPARE(state.value("remoteB").toInt(), 1);
            QCOMPARE(state.value("remoteC").toInt(), 1);
        }

        f.originA.clearRequests();
        f.originB.clearRequests();
        f.originC.clearRequests();
        f.controllerConfig.preferences.disallowContentFromOtherSites = true;
        f.controller->applyPreferences();
        liveImages(*firstPage, "restricted");
        liveImages(*secondPage, "restricted");
        WAIT_FOR(liveFinished(firstDom, "restricted"));
        WAIT_FOR(liveFinished(secondDom, "restricted"));
        QCOMPARE(f.originA.requestCount("/external-a.png"), 1);
        QCOMPARE(f.originB.requestCount("/external-b.png"), 1);
        QCOMPARE(f.originC.requestCount("/external-c.png"), 0);
        QCOMPARE(firstDom.sample().value("remoteA").toInt(), 1);
        QCOMPARE(firstDom.sample().value("remoteB").toInt(), 0);
        QCOMPARE(secondDom.sample().value("remoteA").toInt(), 0);
        QCOMPARE(secondDom.sample().value("remoteB").toInt(), 1);

        f.originA.clearRequests();
        f.originB.clearRequests();
        f.originC.clearRequests();
        f.firstConfig.webSites[0].url = f.originC.url("/").toString();
        int const beforeRefresh = firstPages.size();
        first->showDefinition("policy-refresh", QStringList{"dict-a"}, FtsHighlightSpec{}, 17, false);
        WAIT_FOR(realArticleLoaded(*first, firstPages, beforeRefresh, "policy-refresh"));
        WAIT_FOR(f.originA.requestCount("/external-a.png") == 0
                 && f.originB.requestCount("/external-b.png") == 0
                 && f.originC.requestCount("/external-c.png") == 1
                 && f.originA.requestCount("/article/style.css") == 0
                 && f.originB.requestCount("/article/style.css") == 0
                 && f.originC.requestCount("/article/style.css") == 1);
        WAIT_FOR(firstDom.sample().value("remoteC").toInt() == 1);
        auto refreshed = firstDom.sample();
        QCOMPARE(refreshed.value("remoteA").toInt(), 0);
        QCOMPARE(refreshed.value("remoteB").toInt(), 0);
        QCOMPARE(refreshed.value("remoteC").toInt(), 1);
        liveImages(*secondPage, "after-other-navigation");
        WAIT_FOR(liveFinished(secondDom, "after-other-navigation"));
        QCOMPARE(f.originA.requestCount("/external-a.png"), 0);
        QCOMPARE(f.originB.requestCount("/external-b.png"), 1);
        QCOMPARE(f.originC.requestCount("/external-c.png"), 1);

        QPointer<ArticleRequestInterceptor> retired(firstPolicy), survivor(secondPolicy);
        QPointer<QWebEnginePage> retiredPage(firstPage);
        first.reset();
        QVERIFY(retired.isNull());
        QVERIFY(retiredPage.isNull());
        QCOMPARE(survivor.data(), secondPolicy);
        liveImages(*secondPage, "after-other-destruction");
        WAIT_FOR(liveFinished(secondDom, "after-other-destruction"));
        QCOMPARE(f.originA.requestCount("/external-a.png"), 0);
        QCOMPARE(f.originB.requestCount("/external-b.png"), 2);
        QCOMPARE(f.originC.requestCount("/external-c.png"), 1);
    }

    void completeArticleFileMappingsArePageLocal()
    {
        QDeadlineTimer deadline(20000);
        RealArticleFixture f;
        QTemporaryFile firstFile(QDir(Config::getConfigDir()).filePath("t26-first-XXXXXX.png"));
        QTemporaryFile secondFile(QDir(Config::getConfigDir()).filePath("t26-second-XXXXXX.png"));
        QVERIFY(firstFile.open());
        QVERIFY(secondFile.open());
        QCOMPARE(firstFile.write(HttpFixture::pixelPng()), qint64(HttpFixture::pixelPng().size()));
        QCOMPARE(secondFile.write(HttpFixture::pixelPng()), qint64(HttpFixture::pixelPng().size()));
        firstFile.flush();
        secondFile.flush();
        QUrl firstEmitted = QUrl::fromLocalFile('/' + QFileInfo(firstFile.fileName()).fileName());
        QUrl secondEmitted = QUrl::fromLocalFile('/' + QFileInfo(secondFile.fileName()).fileName());
        // A fresh URL avoids Blink's decoded-image cache. Contexts publish
        // these aliases before navigation; the body exercises the GUI retry.
        firstEmitted.setQuery("policy-probe=one");
        secondEmitted.setQuery("policy-probe=two");
        QUrl const firstFixed = QUrl::fromLocalFile(QFileInfo(firstFile.fileName()).canonicalFilePath());
        QUrl const secondFixed = QUrl::fromLocalFile(QFileInfo(secondFile.fileName()).canonicalFilePath());
        QVERIFY(!QFile::exists(firstEmitted.toLocalFile()));
        QVERIFY(!QFile::exists(secondEmitted.toLocalFile()));
        f.firstConfig.webSites.clear();
        f.secondConfig.webSites.clear();

        f.dictionary->setArticleBody(
            QStringLiteral("<h1 id='headword'>file-one</h1><img id='owned-file' src='%1'>")
                .arg(firstEmitted.adjusted(QUrl::RemoveQuery).toString(QUrl::FullyEncoded)).toUtf8());
        auto first = f.view(f.firstConfig);
        auto second = f.view(f.secondConfig);
        auto *firstWeb = first->findChild<ArticleWebView *>();
        auto *secondWeb = second->findChild<ArticleWebView *>();
        QVERIFY(firstWeb && secondWeb);
        auto *firstPage = firstWeb->page();
        auto *secondPage = secondWeb->page();
        ArticleDomProbe firstDom(firstWeb), secondDom(secondWeb);
        QSignalSpy firstPages(first.get(), &ArticleView::pageLoaded);
        QSignalSpy secondPages(second.get(), &ArticleView::pageLoaded);
        first->showDefinition("file-one", 17, {}, {{"local-file", firstEmitted.toString(QUrl::FullyEncoded)}});
        WAIT_FOR(realArticleLoaded(*first, firstPages, 0, "file-one"));
        WAIT_FOR(firstDom.sample().value("ownedFile").toInt() == 1
                 && firstDom.sample().value("ownedFileSource").toString() == firstFixed.toString(QUrl::FullyEncoded));

        f.dictionary->setArticleBody(
            QStringLiteral("<h1 id='headword'>file-two</h1><img id='owned-file' src='%1'>")
                .arg(secondEmitted.adjusted(QUrl::RemoveQuery).toString(QUrl::FullyEncoded)).toUtf8());
        second->showDefinition("file-two", 17, {}, {{"local-file", secondEmitted.toString(QUrl::FullyEncoded)}});
        WAIT_FOR(realArticleLoaded(*second, secondPages, 0, "file-two"));
        WAIT_FOR(secondDom.sample().value("ownedFile").toInt() == 1
                 && secondDom.sample().value("ownedFileSource").toString() == secondFixed.toString(QUrl::FullyEncoded));

        auto firstRedirect = observe(firstEmitted);
        auto secondRedirect = observe(secondEmitted);
        firstRedirect->expectedRedirect = firstFixed;
        secondRedirect->expectedRedirect = secondFixed;
        auto probeFiles = [](QWebEnginePage &page, QUrl const &own, QUrl const &foreign) {
            page.runJavaScript(QStringLiteral(
                "window.fileProbeDone=false;var remaining=2;"
                "for (const [id,src] of [['owned-again','%1'],['foreign-file','%2']]) {"
                "document.getElementById(id)?.remove();"
                "const image=new Image();image.id=id;"
                "image.onload=image.onerror=()=>{if (--remaining===0) window.fileProbeDone=true;};"
                "image.src=src;document.body.appendChild(image);}")
                .arg(own.toString(QUrl::FullyEncoded), foreign.toString(QUrl::FullyEncoded)));
        };
        f.controllerConfig.preferences.disallowContentFromOtherSites = false;
        f.controller->applyPreferences();
        probeFiles(*firstPage, firstEmitted, secondEmitted);
        probeFiles(*secondPage, secondEmitted, firstEmitted);
        WAIT_FOR(firstDom.sample().value("fileProbeDone").toBool());
        WAIT_FOR(secondDom.sample().value("fileProbeDone").toBool());
        QCOMPARE(snapshot(firstRedirect).redirects, 1);
        QCOMPARE(snapshot(secondRedirect).redirects, 1);
        QCOMPARE(firstDom.sample().value("ownedAgain").toInt(), 1);
        QCOMPARE(secondDom.sample().value("ownedAgain").toInt(), 1);
        QCOMPARE(firstDom.sample().value("foreignFile").toInt(), 0);
        QCOMPARE(secondDom.sample().value("foreignFile").toInt(), 0);
        QVERIFY(snapshot(firstRedirect).refused.isEmpty());
        QVERIFY(snapshot(secondRedirect).refused.isEmpty());

        // Navigation must replace, rather than union, the retired document's
        // mappings. The other page retains its own current map.
        f.dictionary->setArticleBody("<h1 id='headword'>no files</h1><p>Next dictionary entry</p>");
        int const before = firstPages.size();
        first->showDefinition("no-files", QStringList{"dict-a"}, FtsHighlightSpec{}, 17, false);
        WAIT_FOR(realArticleLoaded(*first, firstPages, before, "no-files"));
        probeFiles(*firstPage, firstEmitted, secondEmitted);
        WAIT_FOR(firstDom.sample().value("fileProbeDone").toBool());
        QCOMPARE(firstDom.sample().value("ownedAgain").toInt(), 0);
        QCOMPARE(firstDom.sample().value("foreignFile").toInt(), 0);
        QCOMPARE(snapshot(firstRedirect).redirects, 1);
        QCOMPARE(snapshot(secondRedirect).redirects, 1);
        QCOMPARE(secondDom.sample().value("ownedFile").toInt(), 1);
    }
    void realFrameOwnOriginAndCrossOriginBarrier()
    {
        QDeadlineTimer deadline(20000);
        RealArticleFixture f;
        auto &a = f.originA, &b = f.originB, &third = f.originC;
        QVERIFY2(a.start(QHostAddress("127.0.0.1")), qPrintable(a.errorString()));
        QVERIFY2(b.start(QHostAddress("127.0.0.2")), qPrintable(b.errorString()));
        QVERIFY2(third.start(QHostAddress("127.0.0.3")), qPrintable(third.errorString()));
        f.publishHost(f.firstConfig, "A", a.url("/"));
        f.firstConfig.webSites.append(Config::WebSite("dict-b", "B", b.url("/").toString(), true, {}, true));
        f.dictionary->setArticleBody((RealArticleFixture::entryBody() + QStringLiteral(
            "<img id='application-b' src='%1'>"
            "<iframe id='gdexpandframe-dict-a' name='gdexpandframe-dict-a' src='%2'></iframe>"
            "<iframe name='xfo' src='%3'></iframe>"
            "<iframe name='csp' src='%4'></iframe>")
            .arg(b.url("/application-b.png").toString(), a.url("/redirect").toString(),
                 a.url("/deny-frame").toString(), a.url("/deny-csp").toString())).toUtf8());
        auto view = f.view(f.firstConfig);
        auto *web = view->findChild<ArticleWebView *>();
        QVERIFY(web);
        auto *page = web->page();
        QSignalSpy done(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("frame-policy", 17);
        WAIT_FOR(realArticleLoaded(*view, done, 0, "frame-policy"));
        QCOMPARE(view->getArticlesList(), (QStringList{"dict-a", "dict-b"}));
        QCOMPARE(b.requestCount("/application-b.png"), 1);
        QCOMPARE(a.requestCount("/redirect"), 1);
        QCOMPARE(a.requestCount("/dir/first.html"), 1);
        QCOMPARE(a.requestCount("/dir/style.css"), 1);
        QVERIFY(a.requestCount("/dir/pixel.png") >= 1);
        WAIT_FOR(a.requestCount("/dir/data.json") == 1);
        QCOMPARE(a.requestCount("/deny-frame"), 1);
        QCOMPARE(a.requestCount("/deny-csp"), 1);
        ArticleDomProbe remote(web, "gdexpandframe-dict-a"), xfo(web, "xfo"), csp(web, "csp");
        WAIT_FOR(remote.sample().value("fetchMarker").toString() == "real-origin-fetch");
        QCOMPARE(remote.sample().value("firstMarker").toString(), "first page");
        WAIT_FOR(xfo.sample().value("frameLocation").toString().startsWith("chrome-error:"));
        WAIT_FOR(csp.sample().value("frameLocation").toString().startsWith("chrome-error:"));
        QVERIFY(!xfo.sample().value("deniedMarker").toBool());
        QVERIFY(!csp.sample().value("deniedMarker").toBool());

        auto frame = page->findFrameByName("gdexpandframe-dict-a");
        QVERIFY(frame && frame->isValid());
        auto crossImages = [&](QString const &token) {
            frame->runJavaScript(QStringLiteral(R"JS((() => {
                window.crossDone=false; window.crossToken='%1'; let remaining=3;
                for (const [id,src] of [['cross-b','%2'],['cross-c','%3'],['same-origin','%4']]) {
                    document.getElementById(id)?.remove();
                    const image=new Image(); image.id=id;
                    image.onload=image.onerror=()=>{if (--remaining===0) window.crossDone=true;};
                    image.src=src+'?phase=%1'; document.body.append(image);
                }
            })())JS").arg(token, b.url("/cross-b.png").toString(), third.url("/cross-c.png").toString(),
                         a.url("/frame-own.png").toString()));
        };
        crossImages("blocked");
        WAIT_FOR(remote.sample().value("crossToken").toString() == "blocked" && remote.sample().value("crossDone").toBool());
        // B contributed to the real article, but the HTTP frame may not borrow
        // the application's dictionary privilege. The unrelated third host
        // must not receive any request either.
        QCOMPARE(a.requestCount("/frame-own.png"), 1);
        QCOMPARE(remote.sample().value("sameOrigin").toInt(), 1);
        QCOMPARE(b.requestCount("/cross-b.png"), 0);
        QCOMPARE(third.requests().size(), 0);
        QCOMPARE(remote.sample().value("crossB").toInt(), 0);
        QCOMPARE(remote.sample().value("crossC").toInt(), 0);
        f.controllerConfig.preferences.disallowContentFromOtherSites = false;
        f.controller->applyPreferences();
        crossImages("allowed");
        WAIT_FOR(remote.sample().value("crossToken").toString() == "allowed" && remote.sample().value("crossDone").toBool());
        QCOMPARE(b.requestCount("/cross-b.png"), 1);
        QCOMPARE(third.requestCount("/cross-c.png"), 1);
        QCOMPARE(remote.sample().value("crossB").toInt(), 1);
        QCOMPARE(remote.sample().value("crossC").toInt(), 1);
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
        RealArticleFixture f(false);
        f.controllerConfig.preferences.proxyServer.user = "preserve-proxy";
        f.controllerConfig.preferences.proxyServer.password = "preserve-proxy-secret";
        f.dictionary->setArticleBody((RealArticleFixture::entryBody() + QStringLiteral("<iframe name='authenticated' src='%1'></iframe>")
            .arg(origin.url("/auth").toString())).toUtf8());
        auto view = f.view(f.firstConfig);
        auto *web = view->findChild<ArticleWebView *>();
        QVERIFY(web);
        auto *page = web->page();
        QSignalSpy auth(page, &QWebEnginePage::authenticationRequired), proxy(page, &QWebEnginePage::proxyAuthenticationRequired);
        DialogAnswer answer; answer.accept = accept;
        answer.user = HttpFixture::originUser(); answer.password = HttpFixture::originPassword();
        QSignalSpy done(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("origin-authentication", 17);
        WAIT_FOR(realArticleLoaded(*view, done, 0, "origin-authentication"));
        QCOMPARE(auth.size(), 1); QCOMPARE(proxy.size(), 0); QCOMPARE(answer.dialogs, 1);
        QVERIFY(answer.label.contains("http://127.0.0.1:" + QString::number(origin.port())));
        QVERIFY(answer.label.contains(HttpFixture::originRealm()));
        QCOMPARE(f.controllerConfig.preferences.proxyServer.user, "preserve-proxy");
        QCOMPARE(f.controllerConfig.preferences.proxyServer.password, "preserve-proxy-secret");
        QCOMPARE(origin.requests().constLast().originAuthorized, accept);
        QVERIFY(!QApplication::activeModalWidget());
        if (accept) {
            ArticleDomProbe authenticated(web, "authenticated");
            WAIT_FOR(authenticated.sample().value("bodyText").toString().contains("origin-authenticated"));
        }
        QPointer<QWebEnginePage> retired(page);
        view.reset();
        QVERIFY(retired.isNull());
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
        RealArticleFixture f(false, true);
        auto &p = f.controllerConfig.preferences.proxyServer;
        p.type = Config::ProxyServer::HttpConnect; p.host = "127.0.0.1"; p.port = proxy.port();
        f.construct();
        // Deliberately configure saved fallback AFTER the proxy was applied.
        // Chromium must receive a real 407 on this article's own page.
        if (accept) { p.user = HttpFixture::proxyUser(); p.password = HttpFixture::proxyPassword(); }
        auto target = origin.url("/");
        target.setHost("proxy-target.invalid");
        f.dictionary->setArticleBody((RealArticleFixture::entryBody() + QStringLiteral("<iframe name='proxied' src='%1'></iframe>")
            .arg(target.toString())).toUtf8());
        auto view = f.view(f.firstConfig);
        auto *web = view->findChild<ArticleWebView *>();
        QVERIFY(web);
        auto *page = web->page();
        QSignalSpy challenge(page, &QWebEnginePage::proxyAuthenticationRequired), direct(page, &QWebEnginePage::authenticationRequired);
        DialogAnswer answer; answer.accept = false;
        QSignalSpy done(view.get(), &ArticleView::pageLoaded);
        int const resolutions = proxyTargetResolutions.load();
        view->showDefinition("proxy-authentication", 17);
        WAIT_FOR(realArticleLoaded(*view, done, 0, "proxy-authentication"));
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
            ArticleDomProbe proxied(web, "proxied");
            WAIT_FOR(proxied.sample().value("bodyText").toString().contains("goldendict-http-fixture"));
        } else {
            QCOMPARE(answer.dialogs, 1);
            QCOMPARE(origin.requestCount("/"), 0);
            QVERIFY(p.user.isEmpty()); QVERIFY(p.password.isEmpty());
        }
        QVERIFY(!QApplication::activeModalWidget());
        QPointer<QWebEnginePage> retired(page);
        view.reset();
        QVERIFY(retired.isNull());
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
        RealArticleFixture f(false);
        f.dictionary->setArticleBody(RealArticleFixture::entryBody().toUtf8());
        auto view = f.view(f.firstConfig);
        QSignalSpy loaded(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("cache-exit", 17);
        WAIT_FOR(realArticleLoaded(*view, loaded, 0, "cache-exit"));
        auto &b = f;
        auto *web = view->findChild<ArticleWebView *>();
        QVERIFY(web);
        auto *page = web->page();
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
        QVERIFY(dynamic_cast<ArticleRequestInterceptor *>(page->findChild<QObject *>("gdArticleRequestInterceptor")));
        QCOMPARE(page->profile(), b.controller->profile());
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
