/* Real ArticleView contracts. Each test names the production break it kills.
 *
 * Synchronous-site workflow table (baseline e603c375, articleview.cc line):
 *  #  Old site                              Coordinated async owner/workflow
 * 01  199 safe eval (728/743/819 callers)     inbound article/active caches, WebsiteFrames
 * 02  552 frame height                      WebsiteFrames verified native frame sampling
 * 03  557 frame display                     WebsiteFrames stable-height/cutoff operation
 * 04  560 frame click variable              stamped website-frame instrumentation
 * 05  561 frame click listener              WebsiteFrames verified frameText events
 * 06  562 frame context listener            WebsiteFrames verified context events
 * 07  578 article count                     post-load/Ready ArticleBridge operation
 * 08  613 history current/scroll restore    indexed state, bounded RAF/timer restore
 * 09  667 generated anchor                  same bounded apply-once restore operation
 * 10  678 ordinary anchor                   same restore; never races a later scroll
 * 11  775 current-article scroll            guarded setCurrentArticle operation
 * 12  777 make article active               guarded setCurrentArticle + inbound cache
 * 13  785 select article                    guarded gdSelectArticle operation
 * 14  795 isFramedArticle                   WebsiteFrames native frame identity
 * 15  878 capture scrollX                   one bounded, shared gdCaptureState operation
 * 16  879 capture scrollY                   same capture, finite doubles, debounced fallback
 * 17 1233 fragment scroll                   guarded DOM scroll, no engine history item
 * 18 1723 audio availability                generation-local reading-state audio cache
 * 19 1734 current audio link                one guarded asynchronous audio-link read
 * 20 1742 first audio fallback              same read, resource continuation is view-owned
 * 21 1753 document HTML                     generation/page/owner-guarded toHtml callback
 * 22 1758 document title                    cached QWebEnginePage title + changed signal
 * 23 1778 context hit test                  copied engine request + guarded word operation
 * 24 2263 search clear selection            guarded bridge operation
 * 25 2323 double-click hit test             guarded image read + WebsiteFrames wordAtPoint
 * 26 2440 find clear selection              query-stamped findText/bridge operation
 * 27 2620 FTS clear selection               guarded text snapshot/mark workflow
 * 28 2695 FTS save range                    gdSaveSelection, query + generation stamped
 * 29 2751 FTS restore range                 gdRestoreSelection in the same guarded workflow
 * 30 2790 FTS save next range               gdScrollToHit, persistent JS-owned selection
 * 31 2935 selection hit test                asynchronous gdWordAtPoint/native-frame result
 * 32 2945 expand word selection             WebsiteFrames wordAtPoint/DOM word helper
 * 33 3013 select word range                 same asynchronous word/selection workflow
 *
 * Navigation capture always precedes Back/Forward/Reload/new actions. Save and
 * print use their completed asynchronous pipelines through the real view.
 */
#include <QtTest>
#include <QApplication>
#include <QDeadlineTimer>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPrinter>
#include <QPrintPreviewDialog>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QWebEngineHistory>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <cmath>
#include <optional>
#include <memory>
#include <limits>

#include "articleview.hh"
#include "wstring_qt.hh"
#include "articleschemes.hh"
#include "articlehistory.hh"
#include "articlebridge.hh"
#include "articlewebview.hh"
#include "articleprofile.hh"
#include "articleinspector.hh"
#include "printpipeline.hh"
#include "savearticle.hh"
#include "article_maker.hh"
#include "article_netmgr.hh"
#include "websiteframes.hh"
#include "support/dictionary_stub.hh"
#include "support/fixtures.hh"
#include "support/http_fixture.hh"

namespace {
#define WAIT_FOR(condition) \
    QVERIFY2(QTest::qWaitFor([&] { return bool(condition); }, int(qMax<qint64>(0, deadline.remainingTime()))), #condition)
struct Completion
{
    ArticleBridge::Result callback;
    QVariant value;
    void deliver() const { callback(value); }
};
QList<Completion> captures;
bool holdCapture = false;
int captureCalls = 0;
struct HtmlCompletion { std::function<void(QString const &)> callback; QString html; };
struct PdfCompletion { std::function<void(QByteArray const &)> callback; QByteArray bytes; };
QList<HtmlCompletion> htmlCompletions;
QList<PdfCompletion> pdfCompletions;
bool holdHtml = false, holdPdf = false;
int htmlCalls = 0, pdfCalls = 0;
QWebEngineView *pdfView = nullptr;
bool stays(std::function<bool()> const &condition, int milliseconds = 400)
{
    QDeadlineTimer window(milliseconds);
    while (!window.hasExpired()) {
        if (!condition()) return false;
        QTest::qWait(10);
    }
    return condition();
}
QByteArray readFile(QString const &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QPageLayout printLayout()
{
    return {QPageSize(QPageSize::A4), QPageLayout::Portrait, QMarginsF(10, 10, 10, 10)};
}
void configurePrinter(QPrinter &printer, QString const &destination)
{
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(destination);
    printer.setResolution(96);
    printer.setPageLayout(printLayout());
}
QString pdfText(QString const &path)
{
    QPdfDocument pdf;
    if (pdf.load(path) != QPdfDocument::Error::None) return {};
    QString text;
    for (int page = 0; page < pdf.pageCount(); ++page) text += pdf.getAllText(page).text();
    return text;
}
QVariant evaluate(QWebEnginePage *page, QString const &script)
{
    struct Result { bool done = false; QVariant value; };
    auto answer = std::make_shared<Result>();
    page->runJavaScript(script, [answer](QVariant const &value) { answer->value = value; answer->done = true; });
    if (!QTest::qWaitFor([&] { return answer->done; }, 3000))
        QTest::qFail("real renderer evaluation timed out", __FILE__, __LINE__);
    return answer->value;
}
// A desktop-services target so external dispatch is observed instead of launched.
struct DesktopTarget final : QObject
{
    Q_OBJECT
public:
    QList<QUrl> opened;
    QList<QByteArray> contents;
public slots:
    void handle(QUrl const &url)
    {
        opened.append(url);
        if (!url.isLocalFile()) return;
        QFile file(url.toLocalFile());
        contents.append(file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray());
    }
};
struct Fixture
{
    struct Player final : AudioPlayerInterface
    {
        QByteArray played;
        int stops = 0;
        QString play(char const *bytes, int size) override { played = QByteArray(bytes, size); return {}; }
        void stop() override { ++stops; }
    };
    Config::Class config;
    sptr<TestSupport::FixtureDictionary> a{new TestSupport::FixtureDictionary};
    sptr<TestSupport::FixtureDictionary> b{new TestSupport::FixtureDictionary("dict-b", "Fixture Dictionary B")};
    std::vector<sptr<Dictionary::Class>> dictionaries{a, b};
    Instances::Groups groups;
    ArticleMaker maker{dictionaries, groups, {}, {}};
    ArticleNetworkAccessManager resources{nullptr, dictionaries, maker,
        config.preferences.disallowContentFromOtherSites, config.preferences.hideGoldenDictHeader};
    std::unique_ptr<ArticleProfileController> controller;
    AudioPlayerPtr player{new Player};
    QAction search;
    Fixture()
    {
        config.preferences.maxNetworkCacheSize = 0;
        config.preferences.disallowContentFromOtherSites = false;
        Instances::Group group("Fixture group");
        group.id = 17; group.name = "Fixture group"; group.dictionaries = dictionaries;
        groups.push_back(group);
        controller = std::make_unique<ArticleProfileController>(config, resources, nullptr);
        a->setArticleBody("<p id='fixture-headword'>fixture-headword</p><div style='height:2200px'></div>"
                          "<a id='citation' name='citation'>citation</a>");
        b->setArticleBody("<p>Second dictionary</p><div style='height:2200px'></div>");
    }
    std::unique_ptr<ArticleView> view()
    {
        auto result = std::make_unique<ArticleView>(nullptr, *controller->profile(), resources, player,
                                                  dictionaries, groups, false, config, search, nullptr, nullptr);
        result->resize(800, 600); result->show();
        return result;
    }
};
// The status bar also carries hover notifications; only a real message is a diagnosis.
QString reported(QSignalSpy const &messages)
{
    for (int row = 0; row < messages.size(); ++row)
    {
        auto text = messages.at(row).at(0).toString();
        if (!text.isEmpty()) return text;
    }
    return {};
}
bool loaded(ArticleView &view, QSignalSpy const &spy, int before, QString const &word)
{
    return spy.size() > before && view.getPhrase().phrase == word && !view.getArticlesList().isEmpty();
}
}

// Only the delivery is held; the capture still runs on the real Chromium page.
extern "C" void realPageRun(QWebEnginePage *, QString const &, quint32, ArticleBridge::Result const &)
    asm("__real__ZN14QWebEnginePage13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE");
extern "C" void wrapPageRun(QWebEnginePage *, QString const &, quint32, ArticleBridge::Result const &)
    asm("__wrap__ZN14QWebEnginePage13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE");
extern "C" void wrapPageRun(QWebEnginePage *page, QString const &script, quint32 world,
                            ArticleBridge::Result const &callback)
{
    if (script == QStringLiteral("gdCaptureState()"))
    {
        ++captureCalls;
        if (holdCapture)
        {
            realPageRun(page, script, world, [callback](QVariant const &value) { captures.append({callback, value}); });
            return;
        }
    }
    realPageRun(page, script, world, callback);
}

extern "C" void realHtml(QWebEnginePage const *, std::function<void(QString const &)> const &)
    asm("__real__ZNK14QWebEnginePage6toHtmlERKSt8functionIFvRK7QStringEE");
extern "C" void wrapHtml(QWebEnginePage const *, std::function<void(QString const &)> const &)
    asm("__wrap__ZNK14QWebEnginePage6toHtmlERKSt8functionIFvRK7QStringEE");
extern "C" void wrapHtml(QWebEnginePage const *page, std::function<void(QString const &)> const &callback)
{
    ++htmlCalls;
    bool const hold = holdHtml;
    realHtml(page, [hold, callback](QString const &html) {
        if (hold) htmlCompletions.append({callback, html});
        else callback(html);
    });
}
extern "C" void realPdf(QWebEngineView *, std::function<void(QByteArray const &)> const &,
                         QPageLayout const &, QPageRanges const &)
    asm("__real__ZN14QWebEngineView10printToPdfERKSt8functionIFvRK10QByteArrayEERK11QPageLayoutRK11QPageRanges");
extern "C" void wrapPdf(QWebEngineView *, std::function<void(QByteArray const &)> const &,
                         QPageLayout const &, QPageRanges const &)
    asm("__wrap__ZN14QWebEngineView10printToPdfERKSt8functionIFvRK10QByteArrayEERK11QPageLayoutRK11QPageRanges");
extern "C" void wrapPdf(QWebEngineView *view, std::function<void(QByteArray const &)> const &callback,
                         QPageLayout const &layout, QPageRanges const &ranges)
{
    ++pdfCalls; pdfView = view;
    bool const hold = holdPdf;
    realPdf(view, [hold, callback](QByteArray const &bytes) {
        if (hold) pdfCompletions.append({callback, bytes});
        else callback(bytes);
    }, layout, ranges);
}

class ArticleLoadTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        TestSupport::installWatchdog(TestSupport::BrowserWatchdogSeconds);
        qputenv("GD_TEST_REQUIRED_PREREQUISITES", "display:webengine");
        QStringList reasons;
        QVERIFY2(TestSupport::requiredEnvironment(&reasons), qPrintable(reasons.join('\n')));
    }
    void cleanup()
    {
        holdCapture = false; captures.clear(); captureCalls = 0;
        holdHtml = holdPdf = false; htmlCompletions.clear(); pdfCompletions.clear();
        htmlCalls = pdfCalls = 0; pdfView = nullptr;
        // Desktop-services targets are per-test objects; never leave one behind.
        QDesktopServices::unsetUrlHandler("https");
        QDesktopServices::unsetUrlHandler("file");
    }
    void cleanupTestCase() { TestSupport::clearWatchdog(); }

    // Break: wrappers missing, bypass SaveArticle, discard complete mode or admit a duplicate.
    void saveWrappersPublishOnceAndForwardOutcomes()
    {
        QDeadlineTimer deadline(15000);
        Fixture f; auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QTemporaryDir output; QVERIFY(output.isValid());
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("save-wrapper", 17); WAIT_FOR(loaded(*view, pages, 0, "save-wrapper"));
        evaluate(web->page(), "document.querySelectorAll('head style,.gddicticon,script').forEach(e=>e.remove());"
                              "document.body.removeAttribute('onload');true");
        QSignalSpy status(view.get(), &ArticleView::statusBarMessage);
        auto plain = output.filePath("plain.html"), complete = output.filePath("complete.html");
        holdHtml = true;
        view->saveArticle(plain, false);
        auto save = view->findChild<SaveArticle*>(); QVERIFY(save);
        QSignalSpy saved(save, &SaveArticle::saved), failed(save, &SaveArticle::failed), cancelled(save, &SaveArticle::cancelled);
        WAIT_FOR(!htmlCompletions.isEmpty());
        int const calls = htmlCalls;
        view->saveArticle(output.filePath("duplicate.html"), true);
        QVERIFY(stays([&] { return htmlCalls == calls; }));
        holdHtml = false;
        auto html = htmlCompletions.takeFirst(); html.callback(html.html);
        WAIT_FOR(saved.size() == 1);
        QVERIFY(readFile(plain).contains("fixture-headword"));
        QVERIFY(!QFile::exists(output.filePath("duplicate.html")));
        QVERIFY(reported(status).contains("saved", Qt::CaseInsensitive));
        f.a->setResources({{"asset.bin", "complete-asset"}});
        evaluate(web->page(), "const i=document.createElement('object');i.data='bres://dict-a/asset.bin';document.body.append(i)");
        status.clear(); view->saveArticle(complete, true);
        WAIT_FOR(saved.size() == 2);
        auto directories = QDir(output.path()).entryList({"complete_files-*"}, QDir::Dirs);
        QCOMPARE(directories.size(), 1);
        auto assets = QDir(output.filePath(directories.first())).entryList(QDir::Files);
        QVERIFY(!assets.isEmpty());
        bool assetFound = false;
        for (auto const &asset : assets)
            assetFound |= readFile(output.filePath(directories.first() + '/' + asset)) == "complete-asset";
        QVERIFY(assetFound);
        QVERIFY(!readFile(complete).contains("bres://dict-a/asset.bin"));
        QVERIFY(!reported(status).isEmpty());
        status.clear(); view->saveArticle(output.filePath("missing/destination.html"), false);
        WAIT_FOR(!failed.isEmpty()); QVERIFY(reported(status).contains(failed.last()[0].toString()));
        holdHtml = true; status.clear();
        view->saveArticle(output.filePath("cancelled.html"), false); WAIT_FOR(!htmlCompletions.isEmpty());
        int const count = pages.size(); view->showDefinition("changed", 17);
        WAIT_FOR(!cancelled.isEmpty());
        QVERIFY(reported(status).contains(cancelled.last()[0].toString()));
        holdHtml = false; html = htmlCompletions.takeFirst(); html.callback(html.html);
        WAIT_FOR(loaded(*view, pages, count, "changed"));
        QVERIFY(stays([&] { return !QFile::exists(output.filePath("cancelled.html")); }));
        QCOMPARE(view->findChildren<SaveArticle*>().size(), 1);
    }

    // Break: wrong selected renderer, duplicate PDF generation or missing error/cancel forwarding.
    void exportWrapperUsesSelectedViewAndForwardsOutcomes()
    {
        QDeadlineTimer deadline(18000);
        Fixture f; auto first = f.view(), selected = f.view();
        QSignalSpy firstPages(first.get(), &ArticleView::pageLoaded), pages(selected.get(), &ArticleView::pageLoaded);
        first->showDefinition("wrong-view", 17); selected->showDefinition("selected-view", 17);
        WAIT_FOR(loaded(*first, firstPages, 0, "wrong-view") && loaded(*selected, pages, 0, "selected-view"));
        QTemporaryDir output; QVERIFY(output.isValid());
        auto path = output.filePath("selected.pdf");
        QSignalSpy status(selected.get(), &ArticleView::statusBarMessage);
        holdPdf = true;
        selected->exportPdf(path, printLayout());
        auto pipeline = selected->findChild<PrintPipeline*>(); QVERIFY(pipeline);
        QSignalSpy ready(pipeline, &PrintPipeline::ready), failed(pipeline, &PrintPipeline::failed), cancelled(pipeline, &PrintPipeline::cancelled);
        WAIT_FOR(!pdfCompletions.isEmpty());
        QCOMPARE(pdfView, selected->findChild<ArticleWebView*>());
        QCOMPARE(pdfCalls, 1);
        selected->exportPdf(output.filePath("duplicate.pdf"), printLayout());
        QPrinter printer; configurePrinter(printer, output.filePath("duplicate-print.pdf"));
        selected->print(&printer); selected->printPreview(&printer);
        QVERIFY(stays([&] { return pdfCalls == 1; }));
        holdPdf = false;
        auto pdf = pdfCompletions.takeFirst(); QVERIFY(pdf.bytes.startsWith("%PDF-")); pdf.callback(pdf.bytes);
        WAIT_FOR(QFileInfo(path).size() > 0);
        QCOMPARE(ready.size(), 1);
        QVERIFY(pdfText(path).contains("selected-view")); QVERIFY(!pdfText(path).contains("wrong-view"));
        QVERIFY(!reported(status).isEmpty());
        QVERIFY(!QFile::exists(output.filePath("duplicate.pdf")));
        status.clear(); selected->exportPdf(output.filePath("missing/file.pdf"), printLayout());
        WAIT_FOR(!failed.isEmpty()); QVERIFY(reported(status).contains(failed.last()[0].toString()));
        holdPdf = true; status.clear();
        selected->exportPdf(output.filePath("cancelled.pdf"), printLayout()); WAIT_FOR(!pdfCompletions.isEmpty());
        int const count = pages.size(); selected->showDefinition("new-generation", 17);
        WAIT_FOR(!cancelled.isEmpty()); QVERIFY(reported(status).contains(cancelled.last()[0].toString()));
        holdPdf = false; pdf = pdfCompletions.takeFirst(); pdf.callback(pdf.bytes);
        WAIT_FOR(loaded(*selected, pages, count, "new-generation"));
        QVERIFY(stays([&] { return !QFile::exists(output.filePath("cancelled.pdf")); }));
        QCOMPARE(selected->findChildren<PrintPipeline*>().size(), 1);
    }

    // Break: blocking preview, retained caller printer, preview bypass or no direct painting.
    void previewAndDirectPrintUseOneReadySnapshot()
    {
        QDeadlineTimer deadline(16000);
        Fixture f; auto view = f.view(); QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("preview-wrapper", 17); WAIT_FOR(loaded(*view, pages, 0, "preview-wrapper"));
        QTemporaryDir output; QVERIFY(output.isValid());
        holdPdf = true;
        {
            QPrinter borrowed; configurePrinter(borrowed, output.filePath("preview.pdf"));
            view->printPreview(&borrowed);
        }
        auto pipeline = view->findChild<PrintPipeline*>(); QVERIFY(pipeline);
        WAIT_FOR(!pdfCompletions.isEmpty());
        QCOMPARE(pipeline->state(), PrintPipeline::State::Generating);
        QVERIFY(!view->findChild<QPrintPreviewDialog*>());
        holdPdf = false; auto pdf = pdfCompletions.takeFirst(); pdf.callback(pdf.bytes);
        WAIT_FOR(view->findChild<QPrintPreviewDialog*>());
        QPointer<QPrintPreviewDialog> preview = view->findChild<QPrintPreviewDialog*>();
        WAIT_FOR(preview->isVisible()); QCOMPARE(pipeline->state(), PrintPipeline::State::Previewing);
        preview->close(); WAIT_FOR(!preview);
        QCOMPARE(pipeline->state(), PrintPipeline::State::Ready);
        QPrinter direct; auto painted = output.filePath("painted.pdf"); configurePrinter(direct, painted);
        QSignalSpy status(view.get(), &ArticleView::statusBarMessage);
        view->print(&direct);
        QVERIFY(readFile(painted).startsWith("%PDF-"));
        QCOMPARE(pdfCalls, 1); QVERIFY(!reported(status).isEmpty());
        status.clear(); QSignalSpy failed(pipeline, &PrintPipeline::failed);
        view->print(nullptr);
        QVERIFY(!reported(status).isEmpty());
        // A fresh direct action must also finish after its borrowed printer has gone.
        auto fresh = f.view(); QSignalSpy freshPages(fresh.get(), &ArticleView::pageLoaded);
        fresh->showDefinition("async-direct", 17); WAIT_FOR(loaded(*fresh, freshPages, 0, "async-direct"));
        auto asynchronous = output.filePath("asynchronous.pdf");
        {
            QPrinter temporary; configurePrinter(temporary, asynchronous);
            fresh->print(&temporary);
        }
        WAIT_FOR(QFileInfo(asynchronous).size() > 0);
        QVERIFY(readFile(asynchronous).startsWith("%PDF-"));
    }

    // Break: inspector/page outlives profile, page outlives bridge or pending producers publish after close.
    void teardownDestroysInspectorThenPageThenBridgeBeforeProfile()
    {
        QDeadlineTimer deadline(16000);
        Fixture f; auto viewOwner = f.view(); auto renderer = viewOwner->findChild<ArticleWebView*>();
        auto bridge = viewOwner->findChild<ArticleBridge*>();
        QSignalSpy pages(viewOwner.get(), &ArticleView::pageLoaded);
        viewOwner->showDefinition("closing", 17); WAIT_FOR(loaded(*viewOwner, pages, 0, "closing"));
        QTemporaryDir output; QVERIFY(output.isValid());
        holdHtml = holdPdf = true;
        viewOwner->saveArticle(output.filePath("late.html"), false);
        viewOwner->exportPdf(output.filePath("late.pdf"), printLayout());
        WAIT_FOR(!htmlCompletions.isEmpty() && !pdfCompletions.isEmpty());
        holdCapture = true;
        auto deliveries = std::make_shared<int>(0);
        viewOwner->captureState([deliveries](ArticleState const &) { ++*deliveries; });
        WAIT_FOR(!captures.isEmpty());
        bridge->run("42", [deliveries](QVariant const &) { ++*deliveries; });
        QVERIFY(QMetaObject::invokeMethod(viewOwner.get(), "inspect", Qt::DirectConnection));
        auto inspector = viewOwner->findChild<ArticleInspector*>(); QVERIFY(inspector);
        QPointer<QWebEnginePage> inspectorPage(renderer->page()->devToolsPage()); QVERIFY(inspectorPage);
        QCOMPARE(inspectorPage->profile(), renderer->page()->profile());
        QPointer<QWebEnginePage> pageGuard(renderer->page());
        QPointer<ArticleBridge> bridgeGuard(bridge);
        QPointer<QWebEngineProfile> profileGuard(f.controller->profile());
        QPointer<ArticleInspector> inspectorGuard(inspector);
        QPointer<WebsiteFrames> frameGuard(viewOwner->findChild<WebsiteFrames*>());
        QPointer<SaveArticle> saveGuard(viewOwner->findChild<SaveArticle*>());
        QPointer<PrintPipeline> printGuard(viewOwner->findChild<PrintPipeline*>());
        auto destruction = std::make_shared<QStringList>();
        auto validOrder = std::make_shared<bool>(true);
        connect(inspectorPage, &QObject::destroyed, [=] {
            *validOrder &= bool(profileGuard) && bool(pageGuard) && bool(bridgeGuard) && !frameGuard;
            destruction->append("inspector-page");
        });
        connect(inspector, &QObject::destroyed, [=] { destruction->append("inspector"); });
        connect(pageGuard, &QObject::destroyed, [=] {
            *validOrder &= !inspectorPage && !inspectorGuard && bool(profileGuard) && bool(bridgeGuard);
            destruction->append("page");
        });
        connect(bridgeGuard, &QObject::destroyed, [=] {
            *validOrder &= !pageGuard && bool(profileGuard); destruction->append("bridge");
        });
        connect(profileGuard, &QObject::destroyed, [=] {
            *validOrder &= !inspectorPage && !pageGuard && !bridgeGuard;
            destruction->append("profile");
        });
        viewOwner.reset();
        QVERIFY(inspectorPage.isNull()); QVERIFY(inspectorGuard.isNull());
        QVERIFY(pageGuard.isNull()); QVERIFY(bridgeGuard.isNull());
        QVERIFY(saveGuard.isNull()); QVERIFY(printGuard.isNull()); QVERIFY(profileGuard);
        QCOMPARE(*destruction, (QStringList{"inspector-page", "inspector", "page", "bridge"}));
        captures.takeFirst().deliver();
        auto html = htmlCompletions.takeFirst(); html.callback(html.html);
        auto pdf = pdfCompletions.takeFirst(); pdf.callback(pdf.bytes);
        f.controller.reset();
        WAIT_FOR(!profileGuard);
        QCOMPARE(*destruction, (QStringList{"inspector-page", "inspector", "page", "bridge", "profile"}));
        QVERIFY(*validOrder);
        QVERIFY(stays([&] { return *deliveries == 0 && !QFile::exists(output.filePath("late.html")) &&
                                                   !QFile::exists(output.filePath("late.pdf")); }));
    }

    // Break: default-profile page, missing attachPage or missing initial scripts.
    void realViewLoadsDictionaryAndPublishesCaches()
    {
        QDeadlineTimer deadline(12000);
        Fixture f;
        auto view = f.view();
        auto web = view->findChild<ArticleWebView*>();
        QVERIFY(web);
        QCOMPARE(web->page()->profile(), f.controller->profile());
        QVERIFY(web->page()->findChild<QObject*>("gdArticleRequestInterceptor", Qt::FindDirectChildrenOnly));
        QVERIFY(view->findChild<ArticleBridge*>());
        QVERIFY(view->findChild<WebsiteFrames*>());
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy titles(view.get(), &ArticleView::titleChanged);
        QSignalSpy icons(view.get(), &ArticleView::iconChanged);
        QSignalSpy active(view.get(), &ArticleView::activeArticleChanged);
        QSignalSpy articles(view.get(), &ArticleView::articlesChanged);
        QSignalSpy sounds(view.get(), &ArticleView::soundAvailabilityChanged);
        view->showDefinition("alpha", 17);
        WAIT_FOR(loaded(*view, pages, 0, "alpha"));
        QCOMPARE(view->getArticlesList(), (QStringList{"dict-a", "dict-b"}));
        QCOMPARE(view->getCurrentArticle(), QStringLiteral("gdfrom-dict-a"));
        QCOMPARE(view->getActiveArticleId(), QStringLiteral("dict-a"));
        QVERIFY(!view->hasSound());
        QVERIFY(!titles.isEmpty()); QVERIFY(!icons.isEmpty()); QVERIFY(!active.isEmpty()); QVERIFY(!articles.isEmpty());
        QCOMPARE(view->getTitle(), QStringLiteral("alpha"));
        QCOMPARE(evaluate(web->page(), "document.getElementById('fixture-headword').textContent").toString(),
                 QStringLiteral("fixture-headword"));
        evaluate(web->page(), "gdAudioLinks.first='gdau://dict-a/sound.wav'; window.dispatchEvent(new Event('gd-active-article'))");
        WAIT_FOR(view->hasSound());
        QVERIFY(!sounds.isEmpty());
        f.a->setResources({{"sound.wav", "fixture-audio"}});
        view->playSound();
        WAIT_FOR(static_cast<Fixture::Player*>(f.player.data())->played == "fixture-audio");
        auto captured = std::make_shared<std::optional<ArticleState>>();
        view->captureState([captured](ArticleState const &state) { captured->emplace(state); });
        WAIT_FOR(captured->has_value());
        QCOMPARE(captured->value().currentArticle, QStringLiteral("gdfrom-dict-a"));
        QVERIFY(std::isfinite(captured->value().sx)); QVERIFY(std::isfinite(captured->value().sy));
        auto html = std::make_shared<QString>();
        view->toHtml([html](QString const &value) { *html = value; });
        WAIT_FOR(html->contains("fixture-headword"));
        int count = pages.size();
        view->showDefinition(Config::InputPhrase("beta", "!"), 17, "gdfrom-dict-b");
        WAIT_FOR(loaded(*view, pages, count, "beta"));
        WAIT_FOR(view->getCurrentArticle() == "gdfrom-dict-b");
        QCOMPARE(view->getPhrase().punctuationSuffix, QStringLiteral("!"));
        WAIT_FOR(!view->hasSound());
    }

    // Break: wrong debounce/deadline consumer, zero/default fallback or live reply only.
    void captureDeadlineUsesLatestDebouncedState()
    {
        QCOMPARE(ArticleView::CaptureDeadlineMs, 500);
        QCOMPARE(ArticleView::CaptureDebounceMs, 100);
        QDeadlineTimer deadline(10000);
        Fixture f; auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        auto bridge = view->findChild<ArticleBridge*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("capture", 17); WAIT_FOR(loaded(*view, pages, 0, "capture"));
        QSignalSpy reading(bridge, &ArticleBridge::readingStateChanged);
        evaluate(web->page(), "gdMakeArticleActive('dict-b'); window.scrollTo(0,720); window.dispatchEvent(new Event('gd-active-article'))");
        WAIT_FOR(!reading.isEmpty());
        // Observe the production debounce timer reaching its idle state.
        QTimer *debounce = view->findChild<QTimer*>("gdCaptureDebounce");
        QVERIFY(debounce); QCOMPARE(debounce->interval(), 100);
        WAIT_FOR(!debounce->isActive());
        holdCapture = true;
        auto captured = std::make_shared<std::optional<ArticleState>>();
        auto concurrent = std::make_shared<std::optional<ArticleState>>();
        int calls = captureCalls;
        QElapsedTimer elapsed; elapsed.start();
        view->captureState([captured](ArticleState const &state) { captured->emplace(state); });
        view->captureState([concurrent](ArticleState const &state) { concurrent->emplace(state); });
        WAIT_FOR(!captures.isEmpty()); QVERIFY(!captured->has_value());
        QCOMPARE(captureCalls, calls + 1);
        WAIT_FOR(captured->has_value() && concurrent->has_value());
        QCOMPARE(concurrent->value().sy, captured->value().sy);
        QVERIFY2(elapsed.elapsed() >= 400 && elapsed.elapsed() < 1000, "production capture deadline must be 500ms");
        QCOMPARE(captured->value().currentArticle, QStringLiteral("gdfrom-dict-b"));
        QCOMPARE(captured->value().sy, 720.0);
        auto snapshot = captured->value();
        captures.takeFirst().deliver();
        QCOMPARE(captured->value().sy, snapshot.sy);
        // A timeout must retire its queue operation, so a fresh capture can run.
        holdCapture = false;
        evaluate(web->page(), "window.scrollTo(0,910)");
        captured->reset();
        view->captureState([captured](ArticleState const &state) { captured->emplace(state); });
        WAIT_FOR(captured->has_value()); QCOMPARE(captured->value().sy, 910.0);
    }

    // Break: capture incoming index, reset map on load, URL-keyed history, reload scroll restore.
    void backForwardReloadAndTruncationKeepIndexedOutgoingState()
    {
        QDeadlineTimer deadline(18000);
        Fixture f;
        // Chromium's own history scrolling must not satisfy the application's
        // restore assertions before the guarded restore operation has run.
        f.a->setArticleBody("<script>history.scrollRestoration='manual'</script><p id='fixture-headword'>fixture-headword</p><div style='width:2400px;height:2200px'></div>");
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        auto bridge = view->findChild<ArticleBridge*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
        evaluate(web->page(), "gdMakeArticleActive('dict-b'); window.scrollTo(125,650)");
        QCOMPARE(evaluate(web->page(), "scrollX").toInt(), 125);
        int count = pages.size(), gen = bridge->generation();
        view->showDefinition("B", 17); WAIT_FOR(loaded(*view, pages, count, "B"));
        QVERIFY(bridge->generation() > gen);
        evaluate(web->page(), "window.scrollTo(210,380)");
        int bIndex = web->history()->currentItemIndex();
        count = pages.size(); view->back(); WAIT_FOR(loaded(*view, pages, count, "A"));
        WAIT_FOR(evaluate(web->page(), "scrollY").toInt() == 650);
        QCOMPARE(evaluate(web->page(), "scrollX").toInt(), 125);
        QCOMPARE(view->getCurrentArticle(), QStringLiteral("gdfrom-dict-b"));
        QCOMPARE(web->history()->currentItemIndex(), bIndex - 1);
        count = pages.size(); view->forward(); WAIT_FOR(loaded(*view, pages, count, "B"));
        WAIT_FOR(evaluate(web->page(), "scrollY").toInt() == 380);
        QCOMPARE(evaluate(web->page(), "scrollX").toInt(), 210);
        count = pages.size(); view->reload(); WAIT_FOR(loaded(*view, pages, count, "B"));
        WAIT_FOR(evaluate(web->page(), "scrollY").toInt() == 0);
        QCOMPARE(evaluate(web->page(), "scrollX").toInt(), 0);
        QCOMPARE(web->history()->currentItemIndex(), bIndex);
        count = pages.size(); view->back(); WAIT_FOR(loaded(*view, pages, count, "A"));
        count = pages.size(); view->showDefinition("C", 17); WAIT_FOR(loaded(*view, pages, count, "C"));
        QVERIFY(!view->canGoForward());
        QCOMPARE(web->history()->currentItemIndex(), bIndex);
        QCOMPARE(evaluate(web->page(), "scrollY").toInt(), 0);
        evaluate(web->page(), "window.scrollTo(0,110)");
        count = pages.size(); view->showDefinition("D", 17); WAIT_FOR(loaded(*view, pages, count, "D"));
        count = pages.size(); view->showDefinition("C", 17); WAIT_FOR(loaded(*view, pages, count, "C"));
        QCOMPARE(web->history()->currentItemIndex(), bIndex + 2);
        evaluate(web->page(), "window.scrollTo(0,340)");
        count = pages.size(); view->back(); WAIT_FOR(loaded(*view, pages, count, "D"));
        count = pages.size(); view->back(); WAIT_FOR(loaded(*view, pages, count, "C"));
        WAIT_FOR(evaluate(web->page(), "scrollY").toInt() == 110);
        count = pages.size(); view->forward(); WAIT_FOR(loaded(*view, pages, count, "D"));
        count = pages.size(); view->forward(); WAIT_FOR(loaded(*view, pages, count, "C"));
        WAIT_FOR(evaluate(web->page(), "scrollY").toInt() == 340);
    }

    // Break: one continuation per intent instead of one capture / latest intent.
    void rapidIntentReplacementAndStaleCaptureCannotDrainFreshQueue()
    {
        QDeadlineTimer deadline(14000);
        Fixture f; auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        auto bridge = view->findChild<ArticleBridge*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
        int count = pages.size(), calls = captureCalls, index = web->history()->currentItemIndex();
        holdCapture = true;
        view->showDefinition("discarded", 17); WAIT_FOR(!captures.isEmpty());
        view->showDefinition("latest", 17);
        QCOMPARE(captureCalls, calls + 1);
        WAIT_FOR(loaded(*view, pages, count, "latest"));
        QCOMPARE(web->history()->currentItemIndex(), index + 1);
        auto old = captures.takeFirst();
        view->captureState([](ArticleState const &) {}); WAIT_FOR(!captures.isEmpty());
        auto fresh = captures.takeFirst();
        auto tail = std::make_shared<bool>(false);
        bridge->run("42", [tail](QVariant const &) { *tail = true; });
        old.deliver();
        QVERIFY(!*tail);
        fresh.deliver(); WAIT_FOR(*tail);
        QVERIFY(!evaluate(web->page(), "document.title").toString().contains("discarded"));
    }

    // Break: owner/generation guards omitted in capture, snapshot or action callbacks.
    void closeAbandonsCaptureAndQueuedActions()
    {
        QDeadlineTimer deadline(10000);
        Fixture f; auto view = f.view();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
        holdCapture = true;
        auto deliveries = std::make_shared<int>(0);
        view->captureState([deliveries](ArticleState const &) { ++*deliveries; });
        WAIT_FOR(!captures.isEmpty());
        view->showDefinition("never", 17);
        view->playSound();
        QPointer<ArticleBridge> bridge = view->findChild<ArticleBridge*>();
        auto web = view->findChild<ArticleWebView*>();
        bool pageFirst = false;
        connect(web->page(), &QObject::destroyed, this, [&] { pageFirst = bool(bridge); });
        view.reset(); QVERIFY(!bridge); QVERIFY(pageFirst);
        captures.takeFirst().deliver(); QCOMPARE(*deliveries, 0);
    }

    // Break: UserAction loses group, dictionary subset/contexts, dict or anchor.
    void lookupActionsPreserveReferenceContextAndFragmentsDoNotNavigate()
    {
        QDeadlineTimer deadline(15000);
        Fixture f; auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("A", QStringList{"dict-b"}, FtsHighlightSpec{}, 17, false);
        WAIT_FOR(loaded(*view, pages, 0, "A"));
        QCOMPARE(view->getArticlesList(), QStringList{"dict-b"});
        int count = pages.size();
        view->openLink(QUrl("bword:cat"), web->url(), {}, {{"dict-b", "fixture-context"}});
        WAIT_FOR(loaded(*view, pages, count, "cat"));
        QCOMPARE(view->getViewGroup(), 17U);
        QCOMPARE(view->getArticlesList(), QStringList{"dict-b"});
        QCOMPARE(ArticleUrls::parse(web->url(), ArticleUrls::Use::DocumentLoad).lookup.contexts.value("dict-b"),
                 QStringLiteral("fixture-context"));
        // The explicit FTS producer's established contract ignores context in
        // ArticleNetworkAccessManager; the action must retain it in the URL.
        count = pages.size();
        view->showDefinition("ordinary-context", 17, {}, {{"dict-b", "fixture-context"}});
        WAIT_FOR(loaded(*view, pages, count, "ordinary-context"));
        QCOMPARE(gd::toQString(f.b->lastLookup().context), QStringLiteral("fixture-context"));
        count = pages.size();
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, count, "A"));
        f.a->setArticleBody("<p id='fixture-headword'>fixture-headword</p><div style='height:2200px'></div>"
          "<a id='citation' name='citation'>citation</a><script>"
          "window.__fixtureFrames=[];window.__nativeRaf=window.requestAnimationFrame.bind(window);"
          "window.requestAnimationFrame=callback=>window.__fixtureFrames.push(callback);</script>");
        count = pages.size();
        view->openLink(QUrl("gdlookup://localhost/dog?dict=Fixture%20Dictionary%20B&gdanchor=citation"), web->url());
        WAIT_FOR(loaded(*view, pages, count, "dog"));
        WAIT_FOR(view->getCurrentArticle() == "gdfrom-dict-b");
        auto barrier = std::make_shared<bool>(false);
        view->findChild<ArticleBridge*>()->run("true", [barrier](QVariant const &) { *barrier = true; });
        WAIT_FOR(*barrier);
        // Run two complete frames after all post-load bridge operations were
        // installed. An independent anchor RAF must not precede a later restore.
        evaluate(web->page(), "for(let n=0;n<2;++n){const frame=window.__fixtureFrames.splice(0);"
                             "for(const callback of frame)callback(performance.now())}");
        WAIT_FOR(evaluate(web->page(), "scrollY").toInt() > 1000);
        evaluate(web->page(), "setTimeout(()=>window.__anchorCutoffObserved=true,600)");
        WAIT_FOR(evaluate(web->page(), "window.__anchorCutoffObserved").toBool());
        auto alignment = evaluate(web->page(), "(() => {const e=document.getElementById('citation');"
          "const before=e.getBoundingClientRect().top;e.scrollIntoView();"
          "return before-e.getBoundingClientRect().top})()").toDouble();
        QCOMPARE(qRound(alignment), 0);
        evaluate(web->page(), "window.requestAnimationFrame=window.__nativeRaf");
        int index = web->history()->currentItemIndex(), generation = view->findChild<ArticleBridge*>()->generation();
        auto fragment = web->url(); fragment.setFragment("fixture-headword");
        view->openLink(fragment, web->url());
        WAIT_FOR(evaluate(web->page(), "scrollY").toInt() < 250);
        QCOMPARE(web->history()->currentItemIndex(), index);
        QCOMPARE(view->findChild<ArticleBridge*>()->generation(), generation);
        count = pages.size();
        view->openLink(QUrl("gdlookup://localhost/kitten"), web->url());
        WAIT_FOR(loaded(*view, pages, count, "kitten")); QCOMPARE(view->getViewGroup(), 17U);
    }

    // Break: UI default page auto-load, broken bres/image or picture routing.
    void resourcesPictureBlankAndMalformedActions()
    {
        QDeadlineTimer deadline(14000);
        Fixture f;
        QByteArray pixel = QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII=");
        f.a->setResources({{"pixel.png", pixel}, {"empty", {}}});
        f.a->setArticleBody("<img id='pixel' src='bres://dict-a/pixel.png'><p>fixture-headword</p>");
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("image", 17); WAIT_FOR(loaded(*view, pages, 0, "image"));
        WAIT_FOR(evaluate(web->page(), "document.getElementById('pixel').naturalWidth").toInt() > 0);
        int count = pages.size();
        view->openLink(QUrl("gdpicture://dict-a/pixel.png"), web->url());
        WAIT_FOR(pages.size() > count && web->url().scheme() == "gdpicture");
        WAIT_FOR(evaluate(web->page(), "document.querySelector('img').naturalWidth").toInt() > 0);
        count = pages.size();
        QSignalSpy errors(view.get(), &ArticleView::statusBarMessage);
        view->openLink(QUrl("gdlookup://localhost/cat?contexts=broken"), web->url());
        QVERIFY(!errors.isEmpty()); QCOMPARE(pages.size(), count);
        auto handler = view->saveResource(QUrl("bres://dict-a/empty"), QString());
        QVERIFY(handler);
        count = pages.size();
        view->showAnticipation();
        WAIT_FOR(pages.size() > count);
        QCOMPARE(view->getArticlesList(), QStringList{}); QVERIFY(!view->hasSound());
    }

    // Break: redirect consumes another permit/generation, or a failed load
    // inherits the outgoing article's captured state.
    void redirectsCommitOnceAndFailedLoadsDoNotInheritState()
    {
        QDeadlineTimer deadline(18000);
        Fixture f; auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        auto bridge = view->findChild<ArticleBridge*>();
        TestSupport::HttpFixture server;
        QVERIFY2(server.start(QHostAddress("127.0.0.1")), qPrintable(server.errorString()));
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy generations(bridge, &ArticleBridge::generationChanged);
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
        evaluate(web->page(), "gdMakeArticleActive('dict-b');scrollTo(0,420)");
        int count = pages.size(), genCount = generations.size(), index = web->history()->currentItemIndex();
        // Direct page loads are refused then queued through the view coordinator.
        web->page()->load(server.url("/redirect"));
        WAIT_FOR(pages.size() == count + 1);
        QCOMPARE(web->url(), server.url("/dir/first.html"));
        QCOMPARE(generations.size(), genCount + 1);
        QCOMPARE(web->history()->currentItemIndex(), index + 1);
        QCOMPARE(server.requestCount("/redirect"), 1);
        QCOMPARE(server.requestCount("/dir/first.html"), 1);
        QCOMPARE(view->getArticlesList(), QStringList{});
        QCOMPARE(evaluate(web->page(), "scrollY").toInt(), 0);
        QSignalSpy finishes(web, &QWebEngineView::loadFinished);
        count = pages.size(); genCount = generations.size();
        web->page()->load(server.url("/missing"));
        WAIT_FOR(server.requestCount("/missing") == 1 && !web->page()->isLoading());
        WAIT_FOR(!finishes.isEmpty());
        QVERIFY(!finishes.last()[0].toBool()); QCOMPARE(pages.size(), count);
        QCOMPARE(generations.size(), genCount + 1);
        count = pages.size(); view->back(); WAIT_FOR(pages.size() > count);
        QCOMPARE(web->url(), server.url("/dir/first.html"));
        count = pages.size(); view->back(); WAIT_FOR(loaded(*view, pages, count, "A"));
        WAIT_FOR(evaluate(web->page(), "scrollY").toInt() == 420);
        QCOMPARE(view->getCurrentArticle(), QStringLiteral("gdfrom-dict-b"));
    }

    // Break: restore applied before layout settles, repeatedly, or without the
    // wall/frame bound. The restored offset is impossible before the RAF growth.
    void restoreWaitsForLayoutAndAppliesOnlyOnce()
    {
        QDeadlineTimer deadline(16000);
        Fixture f;
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
        evaluate(web->page(), "history.scrollRestoration='manual';scrollTo(0,1650)");
        int count = pages.size(); view->showDefinition("B", 17); WAIT_FOR(loaded(*view, pages, count, "B"));
        f.a->setResources({{"growth.png", TestSupport::HttpFixture::pixelPng()}});
        f.a->setArticleBody("<p>fixture-headword</p><div id='growth'></div>"
          "<img id='growing-image' src='bres://dict-a/growth.png'>"
          "<iframe id='growing-frame' src='about:blank' style='height:10px'></iframe><script>"
          "history.scrollRestoration='manual';window.__fixtureRestores=0;window.__restoreLayoutFrames=0;"
          "const originalScroll=window.scrollTo.bind(window);window.scrollTo=(x,y)=>{"
          "if(y===1650)++window.__fixtureRestores;originalScroll(x,y)};"
          "addEventListener('gd-article-ready',()=>requestAnimationFrame(function grow(){"
          "if(window.__gdViewRestoreGeneration&&!window.__fixtureRestores)++window.__restoreLayoutFrames;"
          "window.__growthFrames=(window.__growthFrames||0)+1;"
          "document.getElementById('growth').style.height=(5000+window.__growthFrames*20)+'px';"
          "document.getElementById('growing-image').height=240+window.__growthFrames;"
          "document.getElementById('growing-frame').style.height=(320+window.__growthFrames)+'px';"
          "if(window.__growthFrames<60)requestAnimationFrame(grow);}));</script>");
        f.b->setArticleBody("<p>Second dictionary</p>");
        count = pages.size(); QElapsedTimer elapsed; elapsed.start();
        view->back(); WAIT_FOR(loaded(*view, pages, count, "A"));
        WAIT_FOR(evaluate(web->page(), "window.__fixtureRestores").toInt() == 1);
        QCOMPARE(evaluate(web->page(), "scrollY").toInt(), 1650);
        QVERIFY(evaluate(web->page(), "window.__restoreLayoutFrames").toInt() <= 12);
        QVERIFY(elapsed.elapsed() < 2500); // Includes loading and the bounded settle.
        evaluate(web->page(), "scrollTo(0,120);document.getElementById('growth').style.height='6000px';gdOnBridgeReady();"
                             "setTimeout(()=>window.__restoreCutoffObserved=true,600)");
        // Observe the later wall-cutoff turn: it must not apply a second time.
        WAIT_FOR(evaluate(web->page(), "window.__restoreCutoffObserved===true").toBool());
        QCOMPARE(evaluate(web->page(), "window.__fixtureRestores").toInt(), 1);
        auto captured = std::make_shared<std::optional<ArticleState>>();
        view->captureState([captured](ArticleState const &state) { captured->emplace(state); });
        WAIT_FOR(captured->has_value()); QCOMPARE(captured->value().sy, 120.0);

        count = pages.size(); view->showDefinition("C", 17); WAIT_FOR(loaded(*view, pages, count, "C"));
        f.a->setArticleBody("<div style='height:5000px'>fixture-headword</div><script>"
          "history.scrollRestoration='manual';window.__hiddenRestoreCalls=0;"
          "const nativeTimeout=window.setTimeout.bind(window);window.setTimeout=(callback,ms,...args)=>"
          "nativeTimeout(callback,ms===500?5000:ms,...args);"
          "const originalScroll=window.scrollTo.bind(window);window.scrollTo=(x,y)=>{"
          "++window.__hiddenRestoreCalls;originalScroll(x,y)};"
          "requestAnimationFrame(()=>window.__hiddenFrameRan=true);</script>");
        view->hide(); WAIT_FOR(!web->page()->isVisible());
        count = pages.size(); view->back(); WAIT_FOR(loaded(*view, pages, count, "A"));
        elapsed.restart();
        QVERIFY2(QTest::qWaitFor([&] {
            return evaluate(web->page(), "window.__hiddenRestoreCalls").toInt() == 1;
        }, int(qMin<qint64>(1000, qMax<qint64>(0, deadline.remainingTime())))),
                 "a hidden page's JavaScript timer must not control the wall cutoff");
        QVERIFY(elapsed.elapsed() < 1000);
        QVERIFY(!evaluate(web->page(), "window.__hiddenFrameRan===true").toBool());
        QCOMPARE(evaluate(web->page(), "scrollY").toInt(), 120);
    }

    // Break: a real user click misses the retained handler, the new-tab intent
    // is raised twice or lost, or Alt stops locking the jump.
    void rendererClicksDispatchExactlyOnceAndAltLocksTheJump()
    {
        QDeadlineTimer deadline(16000);
        Fixture f;
        f.a->setArticleBody("<p id='fixture-headword'>fixture-headword</p>"
                            "<a id='path-link' href='gdlookup://localhost/alpha'>alpha</a>"
                            "<a id='word-link' href='bword:beta'>beta</a>");
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        auto *page = qobject_cast<ArticleWebPage*>(web->page());
        QVERIFY(page);
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy actions(page, &ArticleWebPage::userLink);
        QSignalSpy newTabs(view.get(), &ArticleView::openLinkInNewTab);
        QSignalSpy words(view.get(), &ArticleView::sendWordToHistory);
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
        QCOMPARE(words.size(), 1);
        int const index = web->history()->currentItemIndex();
        int const generation = view->findChild<ArticleBridge*>()->generation();
        int const requests = f.a->articleRequestCount();
        QWidget *target = web->focusProxy() ? web->focusProxy() : static_cast<QWidget *>(web);
        QVERIFY(target);
        auto linkPoint = [&](QString const &id) {
            auto box = evaluate(web->page(), "(() => {const r=document.getElementById('" + id +
                "').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2};})()").toMap();
            return target->mapFrom(web, QPoint(qRound(box.value("x").toDouble()), qRound(box.value("y").toDouble())));
        };
        QVERIFY(evaluate(web->page(), "(() => !!document.getElementById('path-link') && "
                                   "!!document.getElementById('word-link'))()").toBool());
        // Control-click raises one new-tab intent and performs nothing here.
        auto controlPoint = linkPoint("path-link");
        QTest::mouseClick(target, Qt::LeftButton, Qt::ControlModifier, controlPoint);
        WAIT_FOR(actions.size() == 1);
        QCOMPARE(actions.at(0).at(0).toUrl(), QUrl("gdlookup://localhost/alpha"));
        QCOMPARE(actions.at(0).at(1).toBool(), true);
        WAIT_FOR(newTabs.size() == 1);
        QCOMPARE(newTabs.at(0).at(0).toUrl(), QUrl("gdlookup://localhost/alpha"));
        QCOMPARE(newTabs.at(0).at(2).toString(), QStringLiteral("gdfrom-dict-a"));
        QCOMPARE(pages.size(), 1);
        QCOMPARE(web->history()->currentItemIndex(), index);
        QCOMPARE(view->findChild<ArticleBridge*>()->generation(), generation);
        QCOMPARE(words.size(), 1);
        QCOMPARE(f.a->articleRequestCount(), requests);
        // The middle button raises exactly one further new-tab intent.
        auto wordPoint = linkPoint("word-link");
        QTest::mouseClick(target, Qt::MiddleButton, Qt::NoModifier, wordPoint);
        WAIT_FOR(actions.size() == 2);
        QCOMPARE(actions.at(1).at(0).toUrl(), QUrl("bword:beta"));
        QCOMPARE(actions.at(1).at(1).toBool(), true);
        WAIT_FOR(newTabs.size() == 2);
        QCOMPARE(newTabs.at(1).at(0).toUrl(), QUrl("bword:beta"));
        QCOMPARE(pages.size(), 1);
        // Alt locks the jump: the following Control-click must be the third action.
        QTest::keyPress(target, Qt::Key_Alt);
        QTest::mouseClick(target, Qt::LeftButton, Qt::AltModifier, wordPoint);
        QTest::keyRelease(target, Qt::Key_Alt);
        QTest::mouseClick(target, Qt::LeftButton, Qt::ControlModifier, wordPoint);
        WAIT_FOR(actions.size() == 3);
        QCOMPARE(actions.at(2).at(0).toUrl(), QUrl("bword:beta"));
        QCOMPARE(actions.at(2).at(1).toBool(), true);
        QCOMPARE(newTabs.size(), 3);
        QCOMPARE(pages.size(), 1);
        QCOMPARE(f.a->articleRequestCount(), requests);
        // A plain click dispatches the inherited reference group exactly once.
        QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, controlPoint);
        WAIT_FOR(actions.size() == 4);
        QCOMPARE(actions.at(3).at(0).toUrl(), QUrl("gdlookup://localhost/alpha"));
        QCOMPARE(actions.at(3).at(1).toBool(), false);
        QCOMPARE(newTabs.size(), 3);
        WAIT_FOR(loaded(*view, pages, 1, "alpha"));
        QCOMPARE(view->getViewGroup(), 17U);
        QCOMPARE(QUrlQuery(web->url()).queryItemValue("group"), QStringLiteral("17"));
        QCOMPARE(gd::toQString(f.a->lastLookup().word), QStringLiteral("alpha"));
        QCOMPARE(words.size(), 2);
        QCOMPARE(f.a->articleRequestCount(), requests + 1);
    }

    // Break: a clicked lookup loses the target dictionary, the inherited group or
    // scroll target, the anchor override, or turns a fragment into a new document.
    void clickedLookupsKeepDictionaryTargetGroupAndAnchorPrecedence()
    {
        QDeadlineTimer deadline(20000);
        Fixture f;
        f.a->setArticleBody("<p id='fixture-headword'>fixture-headword</p>"
                            "<a id='first' name='first'>first anchor</a>"
                            "<div style='height:1800px'></div>"
                            "<a id='second' name='second'>second anchor</a>"
                            "<div style='height:1800px'></div>");
        f.b->setArticleBody("<p>Second dictionary</p><div style='height:1800px'></div>"
                            "<a id='second' name='second'>second anchor</a><div style='height:1800px'></div>");
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy errors(view.get(), &ArticleView::statusBarMessage);
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
        QVERIFY2(reported(errors).isEmpty(), qPrintable(reported(errors)));
        // A display-name target selects that dictionary's article, and a clicked
        // lookup stops playback exactly as the retained showDefinition path did.
        auto *player = static_cast<Fixture::Player*>(f.player.data());
        int stops = player->stops;
        int count = pages.size();
        view->openLink(QUrl("gdlookup://localhost/cat?dict=Fixture%20Dictionary%20B"), web->url());
        QCOMPARE(player->stops, stops + 1);
        WAIT_FOR(loaded(*view, pages, count, "cat"));
        WAIT_FOR(view->getCurrentArticle() == "gdfrom-dict-b");
        QCOMPARE(ArticleUrls::parse(web->url(), ArticleUrls::Use::DocumentLoad).lookup.scrollTo,
                 QStringLiteral("gdfrom-dict-b"));
        QCOMPARE(QUrlQuery(web->url()).queryItemValue("group"), QStringLiteral("17"));
        // An unknown display name leaves the inherited scroll target in place.
        count = pages.size();
        view->openLink(QUrl("gdlookup://localhost/dog?dict=Missing%20Dictionary"), web->url(), "gdfrom-dict-a");
        WAIT_FOR(loaded(*view, pages, count, "dog"));
        WAIT_FOR(view->getCurrentArticle() == "gdfrom-dict-a");
        QCOMPARE(ArticleUrls::parse(web->url(), ArticleUrls::Use::DocumentLoad).lookup.scrollTo,
                 QStringLiteral("gdfrom-dict-a"));
        // An explicit valid target group overrides the inherited reference group,
        // including the valid zero group.
        count = pages.size();
        view->openLink(QUrl("gdlookup://localhost/puppy?group=0"), web->url());
        WAIT_FOR(loaded(*view, pages, count, "puppy"));
        QCOMPARE(QUrlQuery(web->url()).queryItemValue("group"), QStringLiteral("0"));
        QCOMPARE(ArticleUrls::parse(web->url(), ArticleUrls::Use::DocumentLoad).lookup.group, 0U);
        // The encoded headword survives and an explicit anchor beats the inherited one.
        count = pages.size();
        view->openLink(QUrl("gdlookup://localhost/red%20fox?gdanchor=second"), web->url(), {},
                       {{QStringLiteral("gdanchor"), QStringLiteral("first")},
                        {QStringLiteral("dict-b"), QStringLiteral("click-context")}});
        WAIT_FOR(loaded(*view, pages, count, QString::fromUtf8("red fox")));
        auto anchor = ArticleUrls::parse(web->url(), ArticleUrls::Use::DocumentLoad).lookup;
        QCOMPARE(anchor.anchor, QStringLiteral("second"));
        QCOMPARE(anchor.contexts.value("dict-b"), QStringLiteral("click-context"));
        QVERIFY(!anchor.contexts.contains("gdanchor"));
        WAIT_FOR(evaluate(web->page(), "scrollY").toInt() > 1000);
        // A fragment-only action scrolls this document without a loader/history item.
        count = pages.size();
        stops = player->stops;
        int const index = web->history()->currentItemIndex();
        int const generation = view->findChild<ArticleBridge*>()->generation();
        auto fragment = web->url(); fragment.setFragment("first");
        view->openLink(fragment, web->url());
        QCOMPARE(player->stops, stops);
        WAIT_FOR(evaluate(web->page(), "scrollY").toInt() < 300);
        QCOMPARE(pages.size(), count);
        QCOMPARE(web->history()->currentItemIndex(), index);
        QCOMPARE(view->findChild<ArticleBridge*>()->generation(), generation);
        // A reference FTS subset wins over the group: only that dictionary is asked.
        count = pages.size();
        view->showDefinition("filtered", QStringList{"dict-b"}, FtsHighlightSpec{}, 17, false);
        WAIT_FOR(loaded(*view, pages, count, "filtered"));
        int askedA = f.a->articleRequestCount(), askedB = f.b->articleRequestCount();
        count = pages.size(); errors.clear();
        view->openLink(QUrl("gdlookup://localhost/kitten?group=17"), web->url());
        WAIT_FOR(loaded(*view, pages, count, "kitten"));
        QCOMPARE(ArticleUrls::parse(web->url(), ArticleUrls::Use::DocumentLoad).lookup.dictionaries,
                 QStringList{"dict-b"});
        QCOMPARE(f.a->articleRequestCount(), askedA);
        QCOMPARE(f.b->articleRequestCount(), askedB + 1);
        QCOMPARE(gd::toQString(f.b->lastLookup().word), QStringLiteral("kitten"));
        QVERIFY2(reported(errors).isEmpty(), qPrintable(reported(errors)));
        // A malformed action is diagnosed and changes no history or document.
        count = pages.size();
        int const diagnosedIndex = web->history()->currentItemIndex();
        int const diagnosedGeneration = view->findChild<ArticleBridge*>()->generation();
        view->openLink(QUrl("gdlookup://localhost/cat?contexts=broken"), web->url());
        WAIT_FOR(!reported(errors).isEmpty());
        QCOMPARE(pages.size(), count);
        QCOMPARE(web->history()->currentItemIndex(), diagnosedIndex);
        QCOMPARE(view->findChild<ArticleBridge*>()->generation(), diagnosedGeneration);
    }

    // Break: a click on a resource, picture or external link is swallowed or
    // re-routed into a document load instead of its retained handler.
    void clickedResourcesPicturesAndExternalLinksRetainTheirHandlers()
    {
        QDeadlineTimer deadline(16000);
        Fixture f;
        QByteArray const pixel = QByteArray::fromBase64(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII=");
        f.a->setResources({{"pixel.png?size=2", pixel}, {"sound.wav", "fixture-audio"},
                           {"clip.mp4", "fixture-video"}});
        f.a->setArticleBody("<p>fixture-headword</p>");
        DesktopTarget desktop;
        QDesktopServices::setUrlHandler("https", &desktop, "handle");
        QDesktopServices::setUrlHandler("file", &desktop, "handle");
        {
            auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
            QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
            QSignalSpy errors(view.get(), &ArticleView::statusBarMessage);
            view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
            int count = pages.size();
            // External links reach desktop services with query and fragment intact.
            view->openLink(QUrl("https://example.invalid/path?q=1#frag"), web->url());
            QCOMPARE(desktop.opened.size(), 1);
            QCOMPARE(desktop.opened.constFirst(), QUrl("https://example.invalid/path?q=1#frag"));
            QCOMPARE(desktop.contents.size(), 0);
            QCOMPARE(pages.size(), count);
            // An audio action plays the resource bytes.
            view->openLink(QUrl("gdau://dict-a/sound.wav"), web->url());
            WAIT_FOR(static_cast<Fixture::Player*>(f.player.data())->played == "fixture-audio");
            QCOMPARE(pages.size(), count);
            // A video action hands the downloaded bytes to the desktop.
            view->openLink(QUrl("gdvideo://dict-a/clip.mp4"), web->url());
            WAIT_FOR(desktop.contents.size() == 1);
            QCOMPARE(desktop.opened.constLast().toLocalFile().isEmpty(), false);
            QCOMPARE(desktop.contents.constFirst(), QByteArray("fixture-video"));
            QCOMPARE(pages.size(), count);
            // A save action writes the same bytes and reports completion once.
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            auto saved = directory.filePath("saved.wav");
            auto handler = view->saveResource(QUrl("gdau://dict-a/sound.wav"), saved);
            QVERIFY(handler);
            WAIT_FOR(QFileInfo(saved).size() == QByteArray("fixture-audio").size());
            QFile file(saved);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray("fixture-audio"));
            // A picture action keeps its full encoded URL and loads its image.
            count = pages.size();
            view->openLink(QUrl("gdpicture://dict-a/pixel.png?size=2"), web->url());
            WAIT_FOR(pages.size() == count + 1 && web->url().scheme() == "gdpicture");
            QCOMPARE(web->url().query(), QStringLiteral("size=2"));
            WAIT_FOR(evaluate(web->page(), "document.querySelector('img').naturalWidth").toInt() > 0);
            QVERIFY(f.a->requestedResources().contains("pixel.png?size=2"));
            QVERIFY2(reported(errors).isEmpty(), qPrintable(reported(errors)));
        }
        QDesktopServices::unsetUrlHandler("https");
        QDesktopServices::unsetUrlHandler("file");
    }

    // Break: a script-initiated navigation is dispatched as a user click, a
    // redirect raises a second action, or a website frame loses its real origin.
    void navigationKindsStayDistinctAcrossClicksScriptsAndRedirects()
    {
        QDeadlineTimer deadline(16000);
        Fixture f;
        f.a->setArticleBody("<p id='fixture-headword'>fixture-headword</p>");
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        auto *page = qobject_cast<ArticleWebPage*>(web->page());
        QVERIFY(page);
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy actions(page, &ArticleWebPage::userLink);
        QSignalSpy requested(web, &ArticleWebView::navigationRequested);
        QSignalSpy words(view.get(), &ArticleView::sendWordToHistory);
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
        QCOMPARE(words.size(), 1);
        // A programmatic document change is a navigation, never a user click.
        int count = pages.size();
        int const index = web->history()->currentItemIndex();
        page->load(QUrl("gdlookup://localhost/?word=scripted&group=17"));
        WAIT_FOR(loaded(*view, pages, count, "scripted"));
        QCOMPARE(actions.size(), 0);
        QCOMPARE(requested.size(), 1);
        QCOMPARE(qvariant_cast<NavigationKind>(requested.at(0).at(1)), NavigationKind::NewLookup);
        QCOMPARE(web->history()->currentItemIndex(), index + 1);
        QCOMPARE(words.size(), 1);
        // A redirect inside the issued navigation commits once without a dispatch.
        TestSupport::HttpFixture server;
        QVERIFY2(server.start(QHostAddress("127.0.0.1")), qPrintable(server.errorString()));
        count = pages.size();
        int const generations = view->findChild<ArticleBridge*>()->generation();
        page->load(server.url("/redirect"));
        WAIT_FOR(pages.size() == count + 1);
        QVERIFY(web->url().path().endsWith("/dir/first.html"));
        QCOMPARE(server.requestCount("/redirect"), 1);
        QCOMPARE(view->findChild<ArticleBridge*>()->generation(), generations + 1);
        QCOMPARE(requested.size(), 2);
        QCOMPARE(qvariant_cast<NavigationKind>(requested.at(1).at(1)), NavigationKind::NewLookup);
        QCOMPARE(words.size(), 1);
        QCOMPARE(actions.size(), 0);
    }

    // Break: a double click translates an image, keeps translating with the
    // preference off, or stops translating the clicked word.
    void doubleClickRulesRetainImageAndPreferenceOutcomes()
    {
        QDeadlineTimer deadline(16000);
        Fixture f;
        QByteArray const pixel = QByteArray::fromBase64(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII=");
        f.a->setResources({{"pixel.png", pixel}});
        f.a->setArticleBody("<p id='fixture-headword'>fixture-headword</p>"
                            "<img id='pixel' src='bres://dict-a/pixel.png' style='width:60px;height:60px'>"
                            "<p><span id='dbl-word'>dblclicked</span></p>");
        DesktopTarget desktop;
        QDesktopServices::setUrlHandler("file", &desktop, "handle");
        {
            auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
            auto *page = qobject_cast<ArticleWebPage*>(web->page());
            QVERIFY(page);
            QVERIFY(view->findChild<ArticleBridge*>());
            QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
            QSignalSpy actions(page, &ArticleWebPage::userLink);
            QSignalSpy words(view.get(), &ArticleView::sendWordToHistory);
            view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
            WAIT_FOR(evaluate(web->page(), "document.getElementById('pixel').naturalWidth").toInt() > 0);
            QWidget *target = web->focusProxy() ? web->focusProxy() : static_cast<QWidget*>(web);
            QVERIFY(target);
            auto pointAt = [&](QString const &id) {
                auto box = evaluate(web->page(), "(() => {const r=document.getElementById('" + id +
                    "').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2};})()").toMap();
                return target->mapFrom(web, QPoint(qRound(box.value("x").toDouble()), qRound(box.value("y").toDouble())));
            };
            // With the preference off a text double click performs no action. The
            // following image double click is the ordering barrier: its observed
            // file cannot arrive before any action the text click had queued.
            f.config.preferences.doubleClickTranslates = false;
            auto text = pointAt("dbl-word");
            QTest::mouseDClick(target, Qt::LeftButton, Qt::NoModifier, text);
            // The image wins over translation: its bytes reach the desktop.
            auto image = pointAt("pixel");
            QTest::mouseDClick(target, Qt::LeftButton, Qt::NoModifier, image);
            WAIT_FOR(desktop.contents.size() == 1);
            QCOMPARE(desktop.contents.constFirst(), pixel);
            QCOMPARE(words.size(), 1);
            QCOMPARE(actions.size(), 0);
            QCOMPARE(pages.size(), 1);
            // With the preference on a text double click looks its word up once.
            f.config.preferences.doubleClickTranslates = true;
            QTest::mouseDClick(target, Qt::LeftButton, Qt::NoModifier, text);
            WAIT_FOR(words.size() == 2);
            QCOMPARE(words.at(1).at(0).toString(), QStringLiteral("dblclicked"));
            WAIT_FOR(loaded(*view, pages, 1, "dblclicked"));
            QCOMPARE(actions.size(), 0);
        }
        QDesktopServices::unsetUrlHandler("file");
    }

    // Break: uncoordinated link auto-load, stale action/capture reenters on the
    // outgoing document or the click is dispatched twice.
    void actualMainFrameClickRoutesExactlyOnce()
    {
        QDeadlineTimer deadline(10000);
        Fixture f;
        f.a->setArticleBody("<p>fixture-headword</p><a id='lookup' href='bword:clicked'>go</a>");
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        QSignalSpy links(web, &ArticleWebView::userLink);
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
        int count = pages.size(), index = web->history()->currentItemIndex(), calls = captureCalls;
        auto point = evaluate(web->page(), "(() => {const r=document.getElementById('lookup').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2};})()").toMap();
        auto input = web->focusProxy(); QVERIFY(input);
        QTest::mouseClick(input, Qt::LeftButton, Qt::NoModifier,
                          input->mapFrom(web, QPoint(point["x"].toInt(), point["y"].toInt())));
        WAIT_FOR(loaded(*view, pages, count, "clicked"));
        QCOMPARE(links.size(), 1); QCOMPARE(captureCalls, calls + 1);
        QCOMPARE(web->history()->currentItemIndex(), index + 1);
        QCOMPARE(view->getViewGroup(), 17U);
    }

    // Break: invalid live offsets accepted, stale reading event can repopulate
    // new-document caches, or asynchronous HTML touches a destroyed owner.
    void invalidAndStaleReadingResultsCannotReplaceCaches()
    {
        QDeadlineTimer deadline(12000);
        Fixture f; auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        auto bridge = view->findChild<ArticleBridge*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("A", 17); WAIT_FOR(loaded(*view, pages, 0, "A"));
        evaluate(web->page(), "window.scrollTo(0,200)");
        auto reading = std::make_shared<std::optional<ArticleState>>();
        view->captureState([reading](ArticleState const &state) { reading->emplace(state); });
        WAIT_FOR(reading->has_value());
        holdCapture = true; reading->reset();
        view->captureState([reading](ArticleState const &state) { reading->emplace(state); });
        WAIT_FOR(!captures.isEmpty());
        auto invalid = captures.takeFirst();
        auto map = invalid.value.toMap(); map["sy"] = std::numeric_limits<double>::infinity();
        invalid.callback(map);
        WAIT_FOR(reading->has_value()); QVERIFY(std::isfinite(reading->value().sy));
        int retired = bridge->generation(), count = pages.size();
        holdCapture = false;
        view->showDefinition("B", QStringList{"dict-a"}, FtsHighlightSpec{}, 17, false);
        WAIT_FOR(loaded(*view, pages, count, "B"));
        bridge->onJsReadingState({{"kind", "state"}, {"articles", QStringList{"dict-b"}}, {"currentArticle", "gdfrom-dict-b"},
                                 {"hasAudio", true}, {"sx", 0}, {"sy", 0}}, retired);
        QCOMPARE(view->getArticlesList(), QStringList{"dict-a"}); QVERIFY(!view->hasSound());
        auto deliveries = std::make_shared<int>(0);
        view->toHtml([deliveries](QString const &) { ++*deliveries; });
        view.reset();
        // A fresh renderer callback provides the event barrier after destruction.
        auto barrier = f.view(); auto barrierWeb = barrier->findChild<ArticleWebView*>();
        evaluate(barrierWeb->page(), "1");
        QCOMPARE(*deliveries, 0);
    }

    // Break: emitted file URLs are neither resolved on GUI nor retried under
    // their page's replacement immutable policy.
    void emittedFileMappingRetriesInOwningDocument()
    {
        QDeadlineTimer deadline(10000);
        Fixture f;
        QTemporaryFile local(QDir(Config::getConfigDir()).filePath("t22-file-XXXXXX.png"));
        QVERIFY(local.open());
        QCOMPARE(local.write(TestSupport::HttpFixture::pixelPng()), qint64(TestSupport::HttpFixture::pixelPng().size()));
        local.flush();
        auto emitted = QUrl::fromLocalFile('/' + QFileInfo(local.fileName()).fileName());
        f.a->setArticleBody("<p>fixture-headword</p><img id='local-file' src='" + emitted.toEncoded() + "'>");
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("files", 17); WAIT_FOR(loaded(*view, pages, 0, "files"));
        WAIT_FOR(evaluate(web->page(), "document.getElementById('local-file').naturalWidth").toInt() == 1);
        int count = pages.size();
        f.a->setArticleBody("<p>fixture-headword</p>");
        view->showDefinition("next", 17); WAIT_FOR(loaded(*view, pages, count, "next"));
        QCOMPARE(view->getCurrentArticle(), QStringLiteral("gdfrom-dict-a"));
    }

    // Break: policy globally unions configured dictionary hosts rather than page subset.
    void contributingDictionaryPolicyIsPublishedBeforeLoad()
    {
        QDeadlineTimer deadline(10000);
        Fixture f;
        TestSupport::HttpFixture a, b;
        QVERIFY2(a.start(QHostAddress("127.0.0.2")), qPrintable(a.errorString()));
        QVERIFY2(b.start(QHostAddress("127.0.0.3")), qPrintable(b.errorString()));
        f.config.webSites.append(Config::WebSite("dict-a", "A", a.url("/").toString(), true, {}, true));
        f.config.webSites.append(Config::WebSite("dict-b", "B", b.url("/").toString(), true, {}, true));
        f.config.preferences.disallowContentFromOtherSites = true;
        f.a->setArticleBody("<p>fixture-headword</p><img id='allowed' src='" + a.url("/a.png").toEncoded() +
            "'><img id='denied' src='" + b.url("/b.png").toEncoded() + "'>");
        auto view = f.view(); auto web = view->findChild<ArticleWebView*>();
        auto child = web->page()->findChild<QObject*>("gdArticleRequestInterceptor", Qt::FindDirectChildrenOnly);
        QVERIFY(dynamic_cast<ArticleRequestInterceptor*>(child));
        QSignalSpy pages(view.get(), &ArticleView::pageLoaded);
        view->showDefinition("policy", QStringList{"dict-a"}, FtsHighlightSpec{}, 17, false);
        WAIT_FOR(loaded(*view, pages, 0, "policy"));
        WAIT_FOR(evaluate(web->page(), "document.getElementById('allowed').naturalWidth").toInt() == 1);
        QCOMPARE(a.requestCount("/a.png"), 1);
        QCOMPARE(b.requestCount("/b.png"), 0);
        // A complete load plus the successful sibling request is the barrier.
        QCOMPARE(evaluate(web->page(), "document.getElementById('denied').naturalWidth").toInt(), 0);
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
    ArticleLoadTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_article_load.moc"
