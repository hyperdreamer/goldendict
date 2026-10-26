/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Contract suite for article URL dispatch, production resource readers and the
 * buffered WebEngine scheme coordinator. The custom main registers schemes
 * before QApplication; browser fixtures use real QWebEngineUrlRequestJobs.
 */

#include <QtTest>

#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QNetworkProxy>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebEngineHttpRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QDataStream>
#include <QMap>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "articleurls.hh"
#include "articleschemes.hh"
#include "article_netmgr.hh"
#include "wstring_qt.hh"
#include "support/fixtures.hh"

namespace {

using ArticleUrls::Kind;
using ArticleUrls::Parsed;
using ArticleUrls::Use;

// ---------------------------------------------------------------------------
// Compile-time contract pins
// ---------------------------------------------------------------------------

// Scheme registration has to happen before the application object exists. Only
// a free function with no arguments can be called at that point, so the shape
// of the declaration is part of the contract, not a comment.
static_assert( std::is_same< decltype( &registerArticleSchemes ), void ( * )() >::value,
               "registerArticleSchemes must be a free function taking no arguments" );

// The reader interface is defined once, in articleschemes.hh, and every
// signature below is what the scheme handler, the streaming device and the
// dictionary engines agree on.
static_assert( std::is_same< decltype( &DataRequestReader::fullData ),
                             QByteArray ( DataRequestReader::* )( Dictionary::DataRequest & ) const >::value,
               "DataRequestReader::fullData must return QByteArray by value" );
static_assert( std::is_same< decltype( &DataRequestReader::dataSlice ),
                             void ( DataRequestReader::* )( Dictionary::DataRequest &, size_t, size_t,
                                                            void * ) const >::value,
               "DataRequestReader::dataSlice must forward offset/size into a destination buffer" );
static_assert( std::is_same< decltype( &DataRequestReader::copyStreamBytes ),
                             void ( DataRequestReader::* )( char *, char const *, qint64 ) const >::value,
               "DataRequestReader::copyStreamBytes must be a producer-free span copy" );
static_assert( std::is_same< decltype( &makeDataRequestReader ),
                             std::shared_ptr< DataRequestReader const > ( * )() >::value,
               "makeDataRequestReader must be the production reader factory entry point" );
static_assert( std::is_same< DataRequestReaderFactory,
                             std::function< std::shared_ptr< DataRequestReader const >(
                               sptr< Dictionary::DataRequest > const & ) > >::value,
               "DataRequestReaderFactory must build one reader per job" );
static_assert( std::has_virtual_destructor< DataRequestReader >::value,
               "DataRequestReader must be destructible through the interface" );
static_assert( noexcept( std::declval< DataRequestReader & >().~DataRequestReader() ),
               "DataRequestReader's destructor must not throw" );

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString contextText( QMap< QString, QString > const & contexts )
{
    QStringList parts;
    for( QMap< QString, QString >::const_iterator it = contexts.cbegin(); it != contexts.cend(); ++it )
        parts.append( it.key() + QLatin1Char( '=' ) + it.value() );
    return parts.join( QLatin1Char( ';' ) );
}

QByteArray serializePayload( std::function< void( QDataStream & ) > const & write )
{
    QByteArray payload;
    QBuffer buffer( &payload );
    buffer.open( QIODevice::WriteOnly );
    QDataStream stream( &buffer );
    stream.setVersion( QDataStream::Qt_5_0 );
    write( stream );
    buffer.close();
    return payload;
}

QByteArray contextsPayload( QMap< QString, QString > const & contexts )
{
    return serializePayload( [ & ]( QDataStream & stream ) { stream << contexts; } );
}

QMap< QString, QString > fixedContextMap()
{
    QMap< QString, QString > contexts;
    contexts.insert( QStringLiteral( "gdanchor" ), QStringLiteral( "part" ) );
    contexts.insert( QStringLiteral( "k" ), QString::fromUtf8( "\xe4\xb8\xad+x%y" ) );
    return contexts;
}

/// The Base64 wire form of fixedContextMap() written with QDataStream Qt_5_0.
/// Pinned byte for byte: the payload version is part of the URL contract.
QString const fixedContextsBase64 =
  QStringLiteral( "AAAAAgAAABAAZwBkAGEAbgBjAGgAbwByAAAACABwAGEAcgB0AAAAAgBrAAAACk4tACsAeAAlAHk=" );

/// Builds the bres counterpart of a picture document the way the handler maps
/// it: the dictionary id is the host and the decoded resource name (including a
/// query suffix) is the path, exactly one percent-encoding pass later. A bare
/// `?` in the name is a present but empty query suffix and survives.
QUrl bresUrl( QString const & dictionaryId, QString const & resourceName )
{
    QUrl url;
    url.setScheme( QStringLiteral( "bres" ) );
    url.setHost( dictionaryId );

    QString path = resourceName;
    int const queryStart = path.indexOf( QLatin1Char( '?' ) );
    bool const hasQuery = queryStart >= 0;
    QString query;
    if( hasQuery )
    {
        query = path.mid( queryStart + 1 );
        path.truncate( queryStart );
    }

    url.setPath( QLatin1Char( '/' ) + path, QUrl::DecodedMode );
    if( hasQuery )
        url.setQuery( query, QUrl::DecodedMode );
    return url;
}

int kindOf( QUrl const & url, Use use )
{
    return static_cast< int >( ArticleUrls::parse( url, use ).kind );
}

using KindInteger = int;

bool readerDestructionObserved = false;

/// A reader implementation that only has to be implementable with an incomplete
/// producer type: the copy path is the one that must never touch a producer.
class RecordingReader final : public DataRequestReader
{
public:
    QByteArray fullData( Dictionary::DataRequest & ) const override { return QByteArrayLiteral( "payload" ); }

    void dataSlice( Dictionary::DataRequest &, size_t, size_t size, void * destination ) const override
    {
        std::memset( destination, 's', size );
    }

    void copyStreamBytes( char * destination, char const * source, qint64 count ) const override
    {
        ++copyCalls;
        for( qint64 i = 0; i < count; ++i )
            destination[ i ] = source[ i ];
    }

    ~RecordingReader() override { readerDestructionObserved = true; }

    mutable int copyCalls = 0;
};

/// State captured from main() before the application object existed: the same
/// window in which the scheme registration entry point has to run.
struct PreApplicationState
{
    bool ran = false;
    bool applicationAbsent = false;
    Kind kind = Kind::Invalid;
    QString headword;
    QList< QWebEngineUrlScheme > schemes;
    QStringList registrationWarnings;
};

PreApplicationState preApplication;

// These fixtures use the real nonvirtual DataRequest getters. No throwing
// reader/failure switches live here: those belong to Task 15's shared support.
sptr< Dictionary::DataRequest > instant( QByteArray const & bytes )
{
    auto result = sptr< Dictionary::DataRequestInstant >( new Dictionary::DataRequestInstant( true ) );
    result->getData().assign( bytes.begin(), bytes.end() );
    return result;
}

class PendingRequest final : public Dictionary::DataRequest
{
public:
    int cancellations = 0;
    void complete( QByteArray const & bytes )
    {
        {
            Mutex::Lock lock( dataMutex );
            hasAnyData = true;
            data.assign( bytes.begin(), bytes.end() );
        }
        finish();
    }
    void append( QByteArray const & bytes )
    {
        {
            Mutex::Lock lock( dataMutex );
            hasAnyData = true;
            data.insert( data.end(), bytes.begin(), bytes.end() );
        }
        update();
    }
    void repeatNotifications() { emit updated(); emit finished(); }
    void cancel() override { ++cancellations; finish(); }
};

class FailedRequest final : public Dictionary::DataRequest
{
public:
    FailedRequest()
    {
        hasAnyData = true;
        data.assign( { 'b', 'a', 'd' } );
        setErrorString( QStringLiteral( "producer IO failed" ) );
        finish();
    }
    void cancel() override {}
};

class ResourceDictionary final : public Dictionary::Class
{
public:
    ResourceDictionary(): Dictionary::Class( "fixture", {} )
    {
        QPixmap image( 16, 16 );
        image.fill( Qt::green );
        dictionaryIcon = QIcon( image );
        dictionaryIconLoaded = true;
    }
    QMap< QString, sptr< Dictionary::DataRequest > > resources;
    QStringList requestedNames;
    gd::wstring articleWord, articleContext;
    bool articleIgnoreDiacritics = false;
    std::vector< gd::wstring > articleAlternates;
    QByteArray articleBody = "<p id='fixture-article'>dictionary body</p>";
    std::string getName() noexcept override { return "Fixture & dictionary"; }
    std::map< Dictionary::Property, std::string > getProperties() noexcept override { return {}; }
    unsigned long getArticleCount() noexcept override { return 1; }
    unsigned long getWordCount() noexcept override { return 1; }
    sptr< Dictionary::WordSearchRequest > prefixMatch( gd::wstring const &, unsigned long ) override
    { return new Dictionary::WordSearchRequestInstant; }
    sptr< Dictionary::DataRequest > getArticle( gd::wstring const & word,
        std::vector< gd::wstring > const & alternates, gd::wstring const & context, bool ignore ) override
    {
        articleWord = word;
        articleAlternates = alternates;
        articleContext = context;
        articleIgnoreDiacritics = ignore;
        return instant( articleBody );
    }
    sptr< Dictionary::DataRequest > getResource( std::string const & name ) override
    {
        QString const decoded = QString::fromUtf8( name.data(), name.size() );
        requestedNames.append( decoded );
        if( decoded == QLatin1String( "throws.html" ) )
            throw std::runtime_error( "dictionary resource failed" );
        return resources.value( decoded, new Dictionary::DataRequestInstant( false ) );
    }
};

class NativeReplyProbe final : public ArticleResourceReply
{
public:
    using ArticleResourceReply::ArticleResourceReply;
    using ArticleResourceReply::readData;
};

class ObservedHandler final : public ArticleSchemeHandler
{
public:
    using ArticleSchemeHandler::ArticleSchemeHandler;
    QList< QPointer< QWebEngineUrlRequestJob > > jobs;
    QList< QByteArray > methods;
    QList< qint64 > bufferSizes, bufferPositions;
    QList< bool > buffersOpenAndJobOwned;
    QList< int > buffersAtDestruction;
    std::function< void() > validateSnapshot;
    void requestStarted( QWebEngineUrlRequestJob * job ) override
    {
        jobs.append( job );
        methods.append( job->requestMethod() );
        QPointer< QWebEngineUrlRequestJob > guardedJob( job );
        connect( job, &QObject::destroyed, this, [this]( QObject * object ) {
            buffersAtDestruction.append( object->findChildren< QBuffer * >().size() );
        } );
        ArticleSchemeHandler::requestStarted( job );
        if( !guardedJob ) return;
        // Buffered handoff characterization; no ArticleStreamDevice is used
        // until T16. The loader has not processed its posted reply yet.
        for( auto buffer : job->findChildren< QBuffer * >() )
        {
            bufferSizes.append( buffer->size() );
            bufferPositions.append( buffer->pos() );
            buffersOpenAndJobOwned.append( buffer->isOpen() && buffer->isReadable() && buffer->parent() == job );
        }
        if( validateSnapshot ) validateSnapshot();
    }
};

struct BrowserFixture
{
    sptr< ResourceDictionary > dictionary{ new ResourceDictionary };
    std::vector< sptr< Dictionary::Class > > dictionaries{ dictionary };
    std::vector< Instances::Group > groups;
    ArticleMaker maker{ dictionaries, groups, {}, {} };
    bool blockExternal = true, hideHeader = false;
    ArticleNetworkAccessManager resources{ nullptr, dictionaries, maker, blockExternal, hideHeader };
    ObservedHandler handler;
    QWebEngineProfile profile;
    std::unique_ptr< QWebEnginePage > page;

    explicit BrowserFixture( DataRequestReaderFactory factory =
        []( sptr< Dictionary::DataRequest > const & ) { return makeDataRequestReader(); } ):
        handler( resources, std::move( factory ) ), page( new QWebEnginePage( &profile ) )
    {
        for( QByteArray const scheme : { "gdlookup", "gdpicture", "bres", "gdau", "gdvideo", "gico", "qrcx" } )
            profile.installUrlSchemeHandler( scheme, &handler );
        page->settings()->setAttribute( QWebEngineSettings::LocalContentCanAccessRemoteUrls, true );
        page->settings()->setAttribute( QWebEngineSettings::LocalContentCanAccessFileUrls, true );
        page->settings()->setAttribute( QWebEngineSettings::BackForwardCacheEnabled, false );
    }
};

struct LoadResult { bool completed = false, ok = false; };
LoadResult load( QWebEnginePage & page, QWebEngineHttpRequest const & request )
{
    QSignalSpy finished( &page, &QWebEnginePage::loadFinished );
    page.load( request );
    bool const completed = QTest::qWaitFor( [&] { return !finished.isEmpty(); }, 5000 );
    if( !completed )
    {
        qWarning() << "Timed out loading" << request.url() << "page" << page.url() << page.title();
        auto handler = dynamic_cast< ObservedHandler const * >( page.profile()->urlSchemeHandler( request.url().scheme().toLatin1() ) );
        if( handler )
            for( auto const & job : handler->jobs )
            {
                qWarning() << "job" << job.data();
                if( job )
                    for( auto buffer : job->findChildren< QBuffer * >() )
                        qWarning() << "buffer" << buffer->size() << buffer->pos() << buffer->isOpen() << buffer->bytesAvailable();
            }
    }
    return { completed, completed && finished.last().at( 0 ).toBool() };
}

QVariant javascript( QWebEnginePage & page, QString const & script )
{
    struct Result { bool done = false; QVariant value; };
    auto result = std::make_shared< Result >();
    page.runJavaScript( script, [result]( QVariant const & value ) {
        result->value = value;
        result->done = true;
    } );
    if( !QTest::qWaitFor( [result] { return result->done; }, 5000 ) )
        return {};
    return result->value;
}

} // namespace

class TstArticleSchemes : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void preApplicationContractHolds();
    void navigationKindVocabularyIsShared();

    void documentLoadRequiresCompleteLookup();
    void blankDocumentForm();
    void explicitDictionariesAndScalarFields();
    void userActionLookupForms();
    void userActionIgnoresFragmentAsHeadword();
    void duplicateScalarKeysRejected();
    void unknownQueryKeysIgnored();
    void invalidAuthorityRejected();
    void resourceKindsAndExternalUrls_data();
    void resourceKindsAndExternalUrls();
    void invalidResourceUrls_data();
    void invalidResourceUrls();
    void resourcePathDecodesExactlyOneLevel();
    void gdLookupHostRules();

    void lookupDocumentRoundTrip();
    void lookupDocumentOmitsActionOnlyDict();
    void producedDocumentUrlTextEncoding();
    void lookupDocumentEncodesDecodedValuesOnce();
    void pictureDocumentComponents();
    void pictureDocumentPreservesResourceIdentity();

    void contextsWireFormatIsPinned();
    void contextsMalformedPayloadsRejected_data();
    void contextsMalformedPayloadsRejected();
    void contextsClearOutputOnFailure();
    void contextsRejectsNonCanonicalBase64();

    void highlightSpecCompilesWildcards();
    void highlightSpecFindsWildcardSpans_data();
    void highlightSpecFindsWildcardSpans();
    void highlightSpecRejectsInvalidUserPattern();

    void readerContractIsImplementable();
    void schemesRegisteredBeforeApplication();
    void productionReaderForwardsAndOwnsBytes();
    void managerRejectsInvalidDocumentsAndAuthorities();
    void managerDispatchPreservesLookupArguments();
    void managerServesBlankPictureQrcAndMime();
    void bufferedResourceRoundTrip();
    void emptyResourceSucceeds();
    void terminalErrorWinsOverPartialBytes();
    void factoryRunsBeforeImmediateStateInspection();
    void nullReaderFailsOnce();
    void jobDestructionCancelsOnce();
    void handlerDestructionCancelsOnce();
    void unsupportedMethodIsDenied();
    void missingResourceFails();
    void realDocumentExecutesLegacyJavascript();
    void explicitHttpRetainsRefererAndFrameHeaders();
    void emittedFileUrlsStayWithSnapshot();
    void bufferedProducerWaitsAndTerminalSignalsLatch();
    void rangeReplies_data();
    void rangeReplies();
    void rangeWaitsForCompletion();
    void factoryReaderSuppliesBufferedBytes();
    void resourceConstructionExceptionFailsOnce();
    void emptyMakerRendersBlankAndSuggestionLinks();
    void makerPreservesRtlEscapingAndFileSnapshots();
    void nativeReplyWaitsAndFinishesOnce();
    void explicitHttpPreservesPathAndUserAgent();
};

void TstArticleSchemes::initTestCase()
{
    QStringList reasons;
    QVERIFY2( TestSupport::requiredEnvironment( &reasons ), qPrintable( reasons.join( '\n' ) ) );
    for( QString const & token : { QStringLiteral( "display" ), QStringLiteral( "webengine" ) } )
    {
        QString reason;
        QVERIFY2( TestSupport::prerequisiteAvailable( token, &reason ), qPrintable( reason ) );
    }
    TestSupport::installWatchdog( TestSupport::BrowserWatchdogSeconds );
}

void TstArticleSchemes::cleanupTestCase()
{
    TestSupport::clearWatchdog();
}

void TstArticleSchemes::preApplicationContractHolds()
{
    // main() ran the URL contract before constructing the application object.
    QVERIFY2( preApplication.ran, "the pre-application probe did not run" );
    QVERIFY2( preApplication.applicationAbsent,
              "the pre-application probe must observe no QCoreApplication instance" );
    QCOMPARE( preApplication.kind, Kind::LookupAction );
    QCOMPARE( preApplication.headword, QStringLiteral( "cat" ) );

    // And the application object exists by now, so the probe really bracketed
    // the ordering boundary instead of running in a world without one.
    QVERIFY( QCoreApplication::instance() );
}

void TstArticleSchemes::navigationKindVocabularyIsShared()
{
    // ArticleWebPage and ArticleHistory share this vocabulary; the order is the
    // declaration order and every kind is distinct.
    const QList< NavigationKind > kinds = {
        NavigationKind::NewLookup, NavigationKind::LinkClick, NavigationKind::Back,
        NavigationKind::Forward, NavigationKind::Reload, NavigationKind::Redirect
    };

    QCOMPARE( kinds.size(), 6 );
    for( int i = 0; i < kinds.size(); ++i )
        QCOMPARE( static_cast< int >( kinds[ i ] ), i );
}

void TstArticleSchemes::documentLoadRequiresCompleteLookup()
{
    // A path-only gdlookup URL is an action form, never a document request: the
    // document parser must not invent a group for it.
    QCOMPARE( ArticleUrls::parse( QUrl( "gdlookup://localhost/cat" ), Use::UserAction ).kind,
              Kind::LookupAction );

    const Parsed pathOnly = ArticleUrls::parse( QUrl( "gdlookup://localhost/cat" ), Use::DocumentLoad );
    QCOMPARE( pathOnly.kind, Kind::Invalid );
    QVERIFY2( pathOnly.error.contains( QStringLiteral( "group required for DocumentLoad" ) ),
              qPrintable( pathOnly.error ) );

    // Zero is a valid, specified group.
    const Parsed zero = ArticleUrls::parse( QUrl( "gdlookup://localhost/?word=cat&group=0" ), Use::DocumentLoad );
    QCOMPARE( zero.kind, Kind::LookupDocument );
    QCOMPARE( zero.lookup.phrase.phrase, QStringLiteral( "cat" ) );
    QCOMPARE( zero.lookup.group, 0u );
    QVERIFY( zero.lookup.groupSpecified );

    // The document word is trimmed, the punctuation suffix is retained.
    const Parsed trimmed = ArticleUrls::parse(
      QUrl( "gdlookup://localhost/?word=%20cat%20&group=3&punctuation_suffix=%2C" ), Use::DocumentLoad );
    QCOMPARE( trimmed.kind, Kind::LookupDocument );
    QCOMPARE( trimmed.lookup.phrase.phrase, QStringLiteral( "cat" ) );
    QCOMPARE( trimmed.lookup.phrase.punctuationSuffix, QStringLiteral( "," ) );
    QCOMPARE( trimmed.lookup.group, 3u );

    // A word is required.
    const Parsed noWord = ArticleUrls::parse( QUrl( "gdlookup://localhost/?group=1" ), Use::DocumentLoad );
    QCOMPARE( noWord.kind, Kind::Invalid );
    QVERIFY2( noWord.error.contains( QStringLiteral( "word required for DocumentLoad" ) ),
              qPrintable( noWord.error ) );

    // The group is a checked unsigned decimal.
    for( QString const & value : { QString(), QStringLiteral( "abc" ), QStringLiteral( "-1" ),
                                   QStringLiteral( "1.0" ), QStringLiteral( "4294967296" ),
                                   QStringLiteral( "0x10" ), QStringLiteral( " " ) } )
    {
        const QUrl url( QStringLiteral( "gdlookup://localhost/?word=cat&group=" ) + value );
        const Parsed parsed = ArticleUrls::parse( url, Use::DocumentLoad );
        QVERIFY2( parsed.kind == Kind::Invalid,
                  qPrintable( QStringLiteral( "accepted group=%1" ).arg( value ) ) );
        QVERIFY2( parsed.error.contains( QStringLiteral( "invalid group" ) ), qPrintable( parsed.error ) );
    }

    // A document load with a fragment keeps it; the fragment never becomes a
    // headword.
    const Parsed withFragment = ArticleUrls::parse(
      QUrl( "gdlookup://localhost/?word=cat&group=1#part" ), Use::DocumentLoad );
    QCOMPARE( withFragment.kind, Kind::LookupDocument );
    QCOMPARE( withFragment.lookup.phrase.phrase, QStringLiteral( "cat" ) );
    QVERIFY( withFragment.url.hasFragment() );
    QCOMPARE( withFragment.url.fragment(), QStringLiteral( "part" ) );
}

void TstArticleSchemes::blankDocumentForm()
{
    const Parsed blank = ArticleUrls::parse( QUrl( "gdlookup://localhost?blank=1" ), Use::DocumentLoad );
    QCOMPARE( blank.kind, Kind::LookupDocument );
    QVERIFY( blank.lookup.blank );
    QVERIFY( !blank.lookup.phrase.isValid() );
    QVERIFY( !blank.lookup.groupSpecified );

    // blank bypasses the word/group requirements.
    const Parsed blankWithFields = ArticleUrls::parse(
      QUrl( "gdlookup://localhost?blank=1&word=cat&group=2" ), Use::DocumentLoad );
    QCOMPARE( blankWithFields.kind, Kind::LookupDocument );
    QVERIFY( blankWithFields.lookup.blank );

    // Anything other than exactly "1" is not a blank request.
    QCOMPARE( ArticleUrls::parse( QUrl( "gdlookup://localhost?blank=0" ), Use::DocumentLoad ).kind,
              Kind::Invalid );
    QCOMPARE( ArticleUrls::parse( QUrl( "gdlookup://localhost?blank=yes" ), Use::DocumentLoad ).kind,
              Kind::Invalid );

    // The blank page is a document form, not a lookup action.
    const Parsed asAction = ArticleUrls::parse( QUrl( "gdlookup://localhost?blank=1" ), Use::UserAction );
    QCOMPARE( asAction.kind, Kind::Invalid );
    QVERIFY2( asAction.error.contains( QStringLiteral( "blank" ) ), qPrintable( asAction.error ) );

    // The blank exemption runs before group validation: the dispatch branch
    // never reads the group (spec §4.4, and the baseline returns makeEmptyPage
    // before inspecting it), so an empty, malformed or overflowing group must
    // not turn an empty-page URL into an error.
    for( QString const & group : { QString(), QStringLiteral( "abc" ), QStringLiteral( "4294967296" ) } )
    {
        const QUrl url( QStringLiteral( "gdlookup://localhost?blank=1&group=" ) + group );
        const Parsed parsed = ArticleUrls::parse( url, Use::DocumentLoad );
        QVERIFY2( parsed.kind == Kind::LookupDocument,
                  qPrintable( QStringLiteral( "blank with group=%1: %2" ).arg( group, parsed.error ) ) );
        QVERIFY2( parsed.lookup.blank, qPrintable( group ) );
        QVERIFY2( parsed.error.isEmpty(), qPrintable( parsed.error ) );
    }

    // Ordinary lookups still reject the same group values, so the exemption is
    // scoped to the blank form and not a general relaxation.
    for( QString const & group : { QString(), QStringLiteral( "abc" ), QStringLiteral( "4294967296" ) } )
    {
        const QUrl url( QStringLiteral( "gdlookup://localhost/?word=cat&group=" ) + group );
        const Parsed parsed = ArticleUrls::parse( url, Use::DocumentLoad );
        QVERIFY2( parsed.kind == Kind::Invalid,
                  qPrintable( QStringLiteral( "ordinary lookup accepted group=%1" ).arg( group ) ) );
        QVERIFY2( parsed.error.contains( QStringLiteral( "invalid group" ) ), qPrintable( parsed.error ) );
    }

    // The exemption is an ordering change only: duplicate scalar keys and the
    // contexts payload stay validated on the blank form.
    const Parsed duplicateGroup = ArticleUrls::parse(
      QUrl( "gdlookup://localhost?blank=1&group=1&group=1" ), Use::DocumentLoad );
    QCOMPARE( duplicateGroup.kind, Kind::Invalid );
    QVERIFY2( duplicateGroup.error.contains( QStringLiteral( "duplicate query parameter" ) ),
              qPrintable( duplicateGroup.error ) );

    const Parsed badContexts = ArticleUrls::parse(
      QUrl( "gdlookup://localhost?blank=1&contexts=%21%21%21" ), Use::DocumentLoad );
    QCOMPARE( badContexts.kind, Kind::Invalid );
    QVERIFY2( badContexts.error.contains( QStringLiteral( "contexts" ) ), qPrintable( badContexts.error ) );
}

void TstArticleSchemes::explicitDictionariesAndScalarFields()
{
    const QMap< QString, QString > contexts = fixedContextMap();
    QUrl url( QStringLiteral( "gdlookup://localhost/?word=cat&group=7&dictionaries=d1,d2"
                              "&muted=d3,,d4&ignore_diacritics=1&scrollto=gdfrom-abc"
                              "&gdanchor=part&regexp=a.*b&matchcase=1&wildcards=1"
                              "&dict=Known%20Name&punctuation_suffix=%2C&contexts=" )
              + fixedContextsBase64 );

    const Parsed parsed = ArticleUrls::parse( url, Use::DocumentLoad );
    QCOMPARE( parsed.kind, Kind::LookupDocument );
    QCOMPARE( parsed.lookup.phrase.phrase, QStringLiteral( "cat" ) );
    QCOMPARE( parsed.lookup.phrase.punctuationSuffix, QStringLiteral( "," ) );
    QCOMPARE( parsed.lookup.group, 7u );
    QVERIFY( parsed.lookup.groupSpecified );
    QCOMPARE( parsed.lookup.dictionaries, QStringList( { QStringLiteral( "d1" ), QStringLiteral( "d2" ) } ) );
    QCOMPARE( parsed.lookup.muted, QStringList( { QStringLiteral( "d3" ), QStringLiteral( "d4" ) } ) );
    QVERIFY( parsed.lookup.ignoreDiacritics );
    QCOMPARE( parsed.lookup.scrollTo, QStringLiteral( "gdfrom-abc" ) );
    QCOMPARE( parsed.lookup.anchor, QStringLiteral( "part" ) );
    QCOMPARE( parsed.lookup.dictionaryName, QStringLiteral( "Known Name" ) );
    QCOMPARE( contextText( parsed.lookup.contexts ), contextText( contexts ) );
    QCOMPARE( parsed.lookup.highlight.sourcePattern, QStringLiteral( "a.*b" ) );
    QVERIFY( parsed.lookup.highlight.matchCase );
    QVERIFY( parsed.lookup.highlight.wildcards );
    QVERIFY( parsed.lookup.highlight.expression.isValid() );

    // The explicit-dictionary branch requires a word but not a valid group; the
    // group is simply not specified then.
    const Parsed explicitNoGroup = ArticleUrls::parse(
      QUrl( "gdlookup://localhost/?word=cat&dictionaries=d1" ), Use::DocumentLoad );
    QCOMPARE( explicitNoGroup.kind, Kind::LookupDocument );
    QCOMPARE( explicitNoGroup.lookup.group, 0u );
    QVERIFY( !explicitNoGroup.lookup.groupSpecified );

    const Parsed explicitBadGroup = ArticleUrls::parse(
      QUrl( "gdlookup://localhost/?word=cat&dictionaries=d1&group=abc" ), Use::DocumentLoad );
    QCOMPARE( explicitBadGroup.kind, Kind::LookupDocument );
    QCOMPARE( explicitBadGroup.lookup.group, 0u );
    QVERIFY( !explicitBadGroup.lookup.groupSpecified );

    // The explicit-dictionary branch still needs a word.
    QCOMPARE( ArticleUrls::parse( QUrl( "gdlookup://localhost/?dictionaries=d1" ), Use::DocumentLoad ).kind,
              Kind::Invalid );

    // "exactly 1" switches, defaults are false.
    const Parsed defaults = ArticleUrls::parse(
      QUrl( "gdlookup://localhost/?word=cat&group=1&ignore_diacritics=0&matchcase=0&wildcards=0" ),
      Use::DocumentLoad );
    QCOMPARE( defaults.kind, Kind::LookupDocument );
    QVERIFY( !defaults.lookup.ignoreDiacritics );
    QVERIFY( !defaults.lookup.highlight.matchCase );
    QVERIFY( !defaults.lookup.highlight.wildcards );
    QVERIFY( defaults.lookup.contexts.isEmpty() );
    QVERIFY( defaults.lookup.highlight.sourcePattern.isEmpty() );
    QVERIFY( defaults.lookup.highlight.expression.pattern().isEmpty() );
}

void TstArticleSchemes::userActionLookupForms()
{
    const Parsed pathLink = ArticleUrls::parse( QUrl( "gdlookup://localhost/cat" ), Use::UserAction );
    QCOMPARE( pathLink.kind, Kind::LookupAction );
    QCOMPARE( pathLink.lookup.phrase.phrase, QStringLiteral( "cat" ) );
    QVERIFY( !pathLink.lookup.groupSpecified );

    // The decoded path is the headword, `dict` and `gdanchor` are retained
    // action fields, and a URL fragment is not folded into either of them.
    const Parsed action = ArticleUrls::parse(
      QUrl( "bword:cat?dict=Known%20Name&gdanchor=part" ), Use::UserAction );
    QCOMPARE( action.kind, Kind::LookupAction );
    QCOMPARE( action.lookup.phrase.phrase, QStringLiteral( "cat" ) );
    QCOMPARE( action.lookup.dictionaryName, QStringLiteral( "Known Name" ) );
    QCOMPARE( action.lookup.anchor, QStringLiteral( "part" ) );

    // Every bword authority/opaque form normalizes to the same headword.
    for( QString const & form : { QStringLiteral( "bword:cat" ), QStringLiteral( "bword:/cat" ),
                                  QStringLiteral( "bword://cat" ), QStringLiteral( "bword:///cat" ) } )
    {
        const Parsed parsed = ArticleUrls::parse( QUrl( form ), Use::UserAction );
        QVERIFY2( parsed.kind == Kind::LookupAction, qPrintable( form ) );
        QVERIFY2( parsed.lookup.phrase.phrase == QStringLiteral( "cat" ), qPrintable( form ) );
    }

    // bword is an action form only.
    QCOMPARE( ArticleUrls::parse( QUrl( "bword:cat" ), Use::DocumentLoad ).kind, Kind::Invalid );

    // The query word is a fallback when the action has no path headword, and it
    // never overrides one.
    QCOMPARE( ArticleUrls::parse( QUrl( "gdlookup://localhost/?word=dog" ), Use::UserAction )
                .lookup.phrase.phrase,
              QStringLiteral( "dog" ) );
    QCOMPARE( ArticleUrls::parse( QUrl( "gdlookup://localhost/cat?word=dog" ), Use::UserAction )
                .lookup.phrase.phrase,
              QStringLiteral( "cat" ) );

    // A target group overrides the inherited reference group when it is valid.
    const Parsed grouped = ArticleUrls::parse( QUrl( "bword:cat?group=2" ), Use::UserAction );
    QCOMPARE( grouped.kind, Kind::LookupAction );
    QCOMPARE( grouped.lookup.group, 2u );
    QVERIFY( grouped.lookup.groupSpecified );

    const Parsed badGroup = ArticleUrls::parse( QUrl( "bword:cat?group=x" ), Use::UserAction );
    QCOMPARE( badGroup.kind, Kind::Invalid );
    QVERIFY2( badGroup.error.contains( QStringLiteral( "invalid group" ) ), qPrintable( badGroup.error ) );

    // Picture documents are documents for both uses.
    QCOMPARE( ArticleUrls::parse( QUrl( "gdpicture://d/a%20b.png" ), Use::UserAction ).kind,
              Kind::PictureDocument );
    QCOMPARE( ArticleUrls::parse( QUrl( "gdpicture://d/a%20b.png" ), Use::DocumentLoad ).kind,
              Kind::PictureDocument );
}

void TstArticleSchemes::userActionIgnoresFragmentAsHeadword()
{
    const Parsed parsed = ArticleUrls::parse( QUrl( "gdlookup://localhost/cat#frag" ), Use::UserAction );
    QCOMPARE( parsed.kind, Kind::LookupAction );
    QCOMPARE( parsed.lookup.phrase.phrase, QStringLiteral( "cat" ) );
    QVERIFY( parsed.url.hasFragment() );
    QCOMPARE( parsed.url.fragment(), QStringLiteral( "frag" ) );
    QVERIFY( parsed.lookup.anchor.isEmpty() );

    // A bword form with a fragment keeps the same rules.
    const Parsed bword = ArticleUrls::parse( QUrl( "bword:cat#frag" ), Use::UserAction );
    QCOMPARE( bword.kind, Kind::LookupAction );
    QCOMPARE( bword.lookup.phrase.phrase, QStringLiteral( "cat" ) );
    QCOMPARE( bword.url.fragment(), QStringLiteral( "frag" ) );
}

void TstArticleSchemes::duplicateScalarKeysRejected()
{
    struct KnownKey
    {
        char const * key;
        char const * value;
    };

    KnownKey const knownKeys[] = {
        { "word", "cat" },
        { "punctuation_suffix", "." },
        { "blank", "1" },
        { "group", "1" },
        { "dictionaries", "d1" },
        { "muted", "d2" },
        { "contexts", "AAAAAA==" },
        { "ignore_diacritics", "1" },
        { "scrollto", "gdfrom-a" },
        { "dict", "Known" },
        { "gdanchor", "part" },
        { "regexp", "a.*b" },
        { "matchcase", "1" },
        { "wildcards", "1" },
    };

    QString const documentBase = QStringLiteral( "gdlookup://localhost/?word=cat&group=1" );
    QString const actionBase = QStringLiteral( "bword:cat" );

    for( KnownKey const & known : knownKeys )
    {
        QString const repeated = QString::fromLatin1( known.key ) + QLatin1Char( '=' )
                               + QString::fromLatin1( known.value ) + QLatin1Char( '&' )
                               + QString::fromLatin1( known.key ) + QLatin1Char( '=' )
                               + QString::fromLatin1( known.value );

        const Parsed document = ArticleUrls::parse( QUrl( documentBase + QLatin1Char( '&' ) + repeated ),
                                                    Use::DocumentLoad );
        QVERIFY2( document.kind == Kind::Invalid,
                  qPrintable( QStringLiteral( "DocumentLoad accepted duplicate %1" )
                                .arg( QString::fromLatin1( known.key ) ) ) );
        QVERIFY2( document.error.contains( QStringLiteral( "duplicate query parameter" ) ),
                  qPrintable( document.error ) );

        const Parsed action = ArticleUrls::parse( QUrl( actionBase + QLatin1Char( '?' ) + repeated ),
                                                  Use::UserAction );
        QVERIFY2( action.kind == Kind::Invalid,
                  qPrintable( QStringLiteral( "UserAction accepted duplicate %1" )
                                .arg( QString::fromLatin1( known.key ) ) ) );
        QVERIFY2( action.error.contains( QStringLiteral( "duplicate query parameter" ) ),
                  qPrintable( action.error ) );
    }
}

void TstArticleSchemes::unknownQueryKeysIgnored()
{
    const Parsed parsed = ArticleUrls::parse(
      QUrl( "gdlookup://localhost/?word=cat&group=1&foo=bar&foo=baz&x=1" ), Use::DocumentLoad );
    QCOMPARE( parsed.kind, Kind::LookupDocument );
    QCOMPARE( parsed.lookup.phrase.phrase, QStringLiteral( "cat" ) );
    QCOMPARE( parsed.lookup.group, 1u );
}

void TstArticleSchemes::invalidAuthorityRejected()
{
    struct Row
    {
        char const * url;
        Use use;
    };

    // Credentials and explicit ports are rejected on every application scheme.
    Row const rejected[] = {
        { "gdlookup://user@localhost/?word=cat&group=1", Use::DocumentLoad },
        { "gdlookup://user:pw@localhost/?word=cat&group=1", Use::DocumentLoad },
        { "gdlookup://localhost:8080/?word=cat&group=1", Use::DocumentLoad },
        { "gdlookup://localhost:0/?word=cat&group=1", Use::DocumentLoad },
        { "bword://u:p@cat", Use::UserAction },
        { "gdpicture://d:0/a.png", Use::DocumentLoad },
        { "bres://u:p@d/a.png", Use::DocumentLoad },
        { "gdau://d:9/a.mp3", Use::UserAction },
        { "qrcx://localhost:1/icons/a.png", Use::DocumentLoad },
    };

    for( Row const & row : rejected )
    {
        const Parsed parsed = ArticleUrls::parse( QUrl( QString::fromLatin1( row.url ) ), row.use );
        QVERIFY2( parsed.kind == Kind::Invalid,
                  qPrintable( QStringLiteral( "accepted %1" ).arg( QString::fromLatin1( row.url ) ) ) );
        QVERIFY2( parsed.error.contains( QStringLiteral( "credentials or port not allowed" ) ),
                  qPrintable( parsed.error ) );
    }

    // External URLs keep their own authority rules: they are not ours to police.
    for( QString const & external : { QStringLiteral( "https://user:pw@example.com:8443/x" ),
                                      QStringLiteral( "http://example.com:80/x" ) } )
    {
        QCOMPARE( ArticleUrls::parse( QUrl( external ), Use::UserAction ).kind, Kind::ExternalAction );
        QCOMPARE( ArticleUrls::parse( QUrl( external ), Use::DocumentLoad ).kind, Kind::ExternalAction );
    }

    // An empty authority and a missing resource name are both invalid.
    QCOMPARE( ArticleUrls::parse( QUrl( "bres:///a.png" ), Use::DocumentLoad ).kind, Kind::Invalid );
    QCOMPARE( ArticleUrls::parse( QUrl( "gdpicture:///a.png" ), Use::DocumentLoad ).kind, Kind::Invalid );
    QVERIFY( !ArticleUrls::parse( QUrl( "bres:///a.png" ), Use::DocumentLoad ).error.isEmpty() );
}

void TstArticleSchemes::resourceKindsAndExternalUrls_data()
{
    QTest::addColumn< QString >( "url" );
    QTest::addColumn< KindInteger >( "documentKind" );
    QTest::addColumn< KindInteger >( "actionKind" );

    auto const document = static_cast< KindInteger >( Kind::LookupDocument );
    auto const lookupAction = static_cast< KindInteger >( Kind::LookupAction );
    auto const picture = static_cast< KindInteger >( Kind::PictureDocument );
    auto const resource = static_cast< KindInteger >( Kind::Resource );
    auto const audio = static_cast< KindInteger >( Kind::AudioAction );
    auto const video = static_cast< KindInteger >( Kind::VideoAction );
    auto const external = static_cast< KindInteger >( Kind::ExternalAction );

    QTest::newRow( "gdlookup-document" )
      << QStringLiteral( "gdlookup://localhost/?word=cat&group=1" ) << document << lookupAction;
    QTest::newRow( "gdlookup-empty-legacy-host" )
      << QStringLiteral( "gdlookup:?word=cat&group=1" ) << document << lookupAction;
    QTest::newRow( "gdpicture" )
      << QStringLiteral( "gdpicture://d/folder/a%20b.png?q=1" ) << picture << picture;
    QTest::newRow( "bres" ) << QStringLiteral( "bres://d/a.png" ) << resource << resource;
    QTest::newRow( "bres-search-host" ) << QStringLiteral( "bres://search/a.png" ) << resource << resource;
    QTest::newRow( "gdau" ) << QStringLiteral( "gdau://d/a.mp3" ) << audio << audio;
    QTest::newRow( "gdvideo" ) << QStringLiteral( "gdvideo://d/a.webm" ) << video << video;
    QTest::newRow( "gico" ) << QStringLiteral( "gico://d/icon.png" ) << resource << resource;
    QTest::newRow( "qrcx" ) << QStringLiteral( "qrcx://localhost/icons/a.png" ) << resource << resource;
    QTest::newRow( "qrcx-hostless" ) << QStringLiteral( "qrcx:///icons/a.png" ) << resource << resource;
    QTest::newRow( "https" ) << QStringLiteral( "https://example.com/a" ) << external << external;
    QTest::newRow( "file" ) << QStringLiteral( "file:///tmp/a.html" ) << external << external;
    QTest::newRow( "mailto" ) << QStringLiteral( "mailto:someone@example.com" ) << external << external;
    QTest::newRow( "data" ) << QStringLiteral( "data:text/plain;base64,YQ==" ) << external << external;
}

void TstArticleSchemes::resourceKindsAndExternalUrls()
{
    QFETCH( QString, url );
    QFETCH( KindInteger, documentKind );
    QFETCH( KindInteger, actionKind );

    QCOMPARE( kindOf( QUrl( url ), Use::DocumentLoad ), documentKind );
    QCOMPARE( kindOf( QUrl( url ), Use::UserAction ), actionKind );
}

void TstArticleSchemes::invalidResourceUrls_data()
{
    QTest::addColumn< QString >( "url" );

    QTest::newRow( "bres-no-name" ) << QStringLiteral( "bres://d" );
    QTest::newRow( "bres-empty-name" ) << QStringLiteral( "bres://d/" );
    QTest::newRow( "gdau-no-name" ) << QStringLiteral( "gdau://d" );
    QTest::newRow( "gdvideo-empty-name" ) << QStringLiteral( "gdvideo://d/" );
    QTest::newRow( "gico-no-name" ) << QStringLiteral( "gico://d" );
    QTest::newRow( "qrcx-no-name" ) << QStringLiteral( "qrcx:" );
    QTest::newRow( "gdpicture-no-name" ) << QStringLiteral( "gdpicture://d" );
    QTest::newRow( "gdpicture-query-only" ) << QStringLiteral( "gdpicture://d/?q=1" );
}

void TstArticleSchemes::invalidResourceUrls()
{
    QFETCH( QString, url );

    const Parsed document = ArticleUrls::parse( QUrl( url ), Use::DocumentLoad );
    QCOMPARE( document.kind, Kind::Invalid );
    QVERIFY2( !document.error.isEmpty(), qPrintable( url ) );

    const Parsed action = ArticleUrls::parse( QUrl( url ), Use::UserAction );
    QCOMPARE( action.kind, Kind::Invalid );
}

void TstArticleSchemes::resourcePathDecodesExactlyOneLevel()
{
    // Path decoded, query suffix decoded, `+` stays a literal plus, and no
    // leading slash is part of the resource name.
    QCOMPARE( ArticleUrls::resourcePath( QUrl( "bres://d/a%20b.png?q=x+y" ) ),
              QStringLiteral( "a b.png?q=x+y" ) );

    // Exactly one structural leading slash is stripped.
    QCOMPARE( ArticleUrls::resourcePath( QUrl( "bres://d/a.png" ) ), QStringLiteral( "a.png" ) );
    QCOMPARE( ArticleUrls::resourcePath( QUrl( "bres://d//a.png" ) ), QStringLiteral( "/a.png" ) );

    // No query, empty query and fragment handling.
    QCOMPARE( ArticleUrls::resourcePath( QUrl( "bres://d/a.png" ) ), QStringLiteral( "a.png" ) );
    QCOMPARE( ArticleUrls::resourcePath( QUrl( "bres://d/a.png?" ) ), QStringLiteral( "a.png?" ) );
    QCOMPARE( ArticleUrls::resourcePath( QUrl( "bres://d/a.png#frag" ) ), QStringLiteral( "a.png" ) );

    // UTF-8 percent-encoded components survive one decode.
    QCOMPARE( ArticleUrls::resourcePath( QUrl( "bres://d/%E4%B8%AD/x%20y.png?q=%E4%B8%AD" ) ),
              QString::fromUtf8( "\xe4\xb8\xad/x y.png?q=\xe4\xb8\xad" ) );

    // The resource name of a parsed resource URL is the same value.
    const Parsed parsed = ArticleUrls::parse( QUrl( "gdvideo://d/folder/a%20b.webm?dl=1#t=1" ),
                                              Use::UserAction );
    QCOMPARE( parsed.kind, Kind::VideoAction );
    QCOMPARE( parsed.dictionaryId, QStringLiteral( "d" ) );
    QCOMPARE( parsed.resourceName, QStringLiteral( "folder/a b.webm?dl=1" ) );
}

void TstArticleSchemes::gdLookupHostRules()
{
    // An empty legacy host and localhost are both accepted; a foreign host is
    // not.
    QCOMPARE( kindOf( QUrl( "gdlookup://localhost/?word=cat&group=1" ), Use::DocumentLoad ),
              static_cast< int >( Kind::LookupDocument ) );
    QCOMPARE( kindOf( QUrl( "gdlookup:?word=cat&group=1" ), Use::DocumentLoad ),
              static_cast< int >( Kind::LookupDocument ) );

    const Parsed foreign = ArticleUrls::parse( QUrl( "gdlookup://example.com/?word=cat&group=1" ),
                                               Use::DocumentLoad );
    QCOMPARE( foreign.kind, Kind::Invalid );
    QVERIFY2( foreign.error.contains( QStringLiteral( "host" ) ), qPrintable( foreign.error ) );

    // A percent-encoded dictionary host is canonicalized by QUrl, not split by
    // hand.
    QCOMPARE( ArticleUrls::parse( QUrl( "bres://d%2Dx/a.png" ), Use::DocumentLoad ).dictionaryId,
              QStringLiteral( "d-x" ) );
}

void TstArticleSchemes::lookupDocumentRoundTrip()
{
    ArticleUrls::Lookup lookup;
    lookup.phrase = Config::InputPhrase( QString::fromUtf8( "a+b c%d \xe4\xb8\xad" ), QStringLiteral( "," ) );
    lookup.group = 4;
    lookup.groupSpecified = true;
    lookup.dictionaries = QStringList( { QStringLiteral( "d1" ), QStringLiteral( "d2" ) } );
    lookup.muted = QStringList( { QStringLiteral( "d3" ) } );
    lookup.contexts = fixedContextMap();
    lookup.contexts.remove( QStringLiteral( "gdanchor" ) );
    lookup.contexts.insert( QStringLiteral( "ctx" ), QStringLiteral( "value" ) );
    lookup.ignoreDiacritics = true;
    lookup.scrollTo = QStringLiteral( "gdfrom-abc" );
    lookup.anchor = QStringLiteral( "part" );
    lookup.highlight.sourcePattern = QStringLiteral( "a.*b" );
    lookup.highlight.wildcards = true;
    lookup.highlight.matchCase = true;
    lookup.highlight.expression = QRegularExpression( QStringLiteral( "a.*b" ) );

    const QUrl produced = ArticleUrls::lookupDocument( lookup );
    QCOMPARE( produced.scheme(), QStringLiteral( "gdlookup" ) );
    QCOMPARE( produced.host(), QStringLiteral( "localhost" ) );

    const Parsed back = ArticleUrls::parse( produced, Use::DocumentLoad );
    QCOMPARE( back.kind, Kind::LookupDocument );
    QCOMPARE( back.lookup.phrase.phrase, lookup.phrase.phrase );
    QCOMPARE( back.lookup.phrase.punctuationSuffix, lookup.phrase.punctuationSuffix );
    QCOMPARE( back.lookup.group, lookup.group );
    QCOMPARE( back.lookup.groupSpecified, lookup.groupSpecified );
    QCOMPARE( back.lookup.dictionaries, lookup.dictionaries );
    QCOMPARE( back.lookup.muted, lookup.muted );
    QCOMPARE( contextText( back.lookup.contexts ), contextText( lookup.contexts ) );
    QCOMPARE( back.lookup.ignoreDiacritics, lookup.ignoreDiacritics );
    QCOMPARE( back.lookup.scrollTo, lookup.scrollTo );
    QCOMPARE( back.lookup.anchor, lookup.anchor );
    QCOMPARE( back.lookup.highlight.sourcePattern, lookup.highlight.sourcePattern );
    QCOMPARE( back.lookup.highlight.wildcards, lookup.highlight.wildcards );
    QCOMPARE( back.lookup.highlight.matchCase, lookup.highlight.matchCase );

    // An ordinary lookup without a specified group still produces a valid
    // document URL: the producer supplies the group the document form needs.
    ArticleUrls::Lookup ungrouped;
    ungrouped.phrase = Config::InputPhrase::fromPhrase( QStringLiteral( "cat" ) );
    const Parsed ungroupedBack = ArticleUrls::parse( ArticleUrls::lookupDocument( ungrouped ),
                                                     Use::DocumentLoad );
    QCOMPARE( ungroupedBack.kind, Kind::LookupDocument );
    QCOMPARE( ungroupedBack.lookup.group, 0u );
    QVERIFY( ungroupedBack.lookup.groupSpecified );

    // The explicit-dictionary form keeps group omission, so it re-parses as the
    // explicit-dictionary branch it came from.
    ArticleUrls::Lookup explicitDictionaries;
    explicitDictionaries.phrase = Config::InputPhrase::fromPhrase( QStringLiteral( "cat" ) );
    explicitDictionaries.dictionaries = QStringList( { QStringLiteral( "d1" ) } );
    const QUrl explicitUrl = ArticleUrls::lookupDocument( explicitDictionaries );
    QVERIFY( !QUrlQuery( explicitUrl ).hasQueryItem( QStringLiteral( "group" ) ) );
    const Parsed explicitBack = ArticleUrls::parse( explicitUrl, Use::DocumentLoad );
    QCOMPARE( explicitBack.kind, Kind::LookupDocument );
    QVERIFY( !explicitBack.lookup.groupSpecified );

    // The blank page round-trips as itself and carries nothing else.
    ArticleUrls::Lookup blank;
    blank.blank = true;
    const QUrl blankUrl = ArticleUrls::lookupDocument( blank );
    QCOMPARE( blankUrl.toEncoded(), QByteArray( "gdlookup://localhost?blank=1" ) );
    const Parsed blankBack = ArticleUrls::parse( blankUrl, Use::DocumentLoad );
    QCOMPARE( blankBack.kind, Kind::LookupDocument );
    QVERIFY( blankBack.lookup.blank );
}

void TstArticleSchemes::lookupDocumentOmitsActionOnlyDict()
{
    // `dict` is consumed by the action handler before a document URL is built:
    // it must not leak into the document request.
    ArticleUrls::Lookup lookup;
    lookup.phrase = Config::InputPhrase::fromPhrase( QStringLiteral( "cat" ) );
    lookup.group = 1;
    lookup.groupSpecified = true;
    lookup.dictionaryName = QStringLiteral( "Known Name" );

    const QUrl produced = ArticleUrls::lookupDocument( lookup );
    QVERIFY( !QUrlQuery( produced ).hasQueryItem( QStringLiteral( "dict" ) ) );

    const Parsed back = ArticleUrls::parse( produced, Use::DocumentLoad );
    QCOMPARE( back.kind, Kind::LookupDocument );
    QVERIFY( back.lookup.dictionaryName.isEmpty() );
}

void TstArticleSchemes::producedDocumentUrlTextEncoding()
{
    ArticleUrls::Lookup lookup;
    lookup.phrase = Config::InputPhrase::fromPhrase( QString::fromUtf8( "a+b c%d&e=f \xe4\xb8\xad" ) );
    lookup.group = 1;
    lookup.groupSpecified = true;

    const QUrl produced = ArticleUrls::lookupDocument( lookup );
    const QByteArray encoded = produced.toEncoded();

    QVERIFY2( !encoded.contains( ' ' ), encoded.constData() );
    // A literal plus stays a literal plus, it is never form-encoded into a
    // space, and the reserved delimiters are percent-encoded exactly once.
    QVERIFY2( encoded.contains( "a%2Bb%20c%25d%26e%3Df" ), encoded.constData() );
    QVERIFY2( encoded.contains( "%E4%B8%AD" ), encoded.constData() );

    const Parsed back = ArticleUrls::parse( produced, Use::DocumentLoad );
    QCOMPARE( back.kind, Kind::LookupDocument );
    QCOMPARE( back.lookup.phrase.phrase, lookup.phrase.phrase );
}

void TstArticleSchemes::lookupDocumentEncodesDecodedValuesOnce()
{
    // A decoded value may itself contain a valid %HH sequence. It has to be
    // encoded exactly once on the wire: QUrlQuery keeps a valid escape it is
    // handed because such a value looks already encoded, so an unencoded value
    // silently loses one level and parses back as a different string.
    ArticleUrls::Lookup lookup;
    lookup.phrase = Config::InputPhrase( QStringLiteral( "a%20b" ), QStringLiteral( "%2F" ) );
    lookup.group = 3;
    lookup.groupSpecified = true;
    lookup.dictionaries = QStringList( { QStringLiteral( "d%2C1" ) } );
    lookup.muted = QStringList( { QStringLiteral( "m%25" ) } );
    lookup.scrollTo = QStringLiteral( "gdfrom-a%20b" );
    lookup.anchor = QStringLiteral( "part%26x" );
    lookup.highlight.sourcePattern = QStringLiteral( "a%20b" );
    lookup.highlight.wildcards = true;
    lookup.highlight.matchCase = true;
    lookup.highlight.expression = QRegularExpression( QStringLiteral( "a%20b" ) );

    const QUrl produced = ArticleUrls::lookupDocument( lookup );

    // Literal expected URL text: every decoded value gained exactly one
    // percent-encoding pass, so each original escape became a double escape.
    QCOMPARE( produced.toEncoded(),
              QByteArray( "gdlookup://localhost?word=a%2520b"
                          "&punctuation_suffix=%252F&group=3"
                          "&dictionaries=d%252C1&muted=m%2525"
                          "&scrollto=gdfrom-a%2520b"
                          "&regexp=a%2520b&matchcase=1&wildcards=1"
                          "&gdanchor=part%2526x" ) );

    // And every decoded field comes back unchanged.
    const Parsed back = ArticleUrls::parse( produced, Use::DocumentLoad );
    QCOMPARE( back.kind, Kind::LookupDocument );
    QCOMPARE( back.lookup.phrase.phrase, lookup.phrase.phrase );
    QCOMPARE( back.lookup.phrase.punctuationSuffix, lookup.phrase.punctuationSuffix );
    QCOMPARE( back.lookup.group, lookup.group );
    QCOMPARE( back.lookup.dictionaries, lookup.dictionaries );
    QCOMPARE( back.lookup.muted, lookup.muted );
    QCOMPARE( back.lookup.scrollTo, lookup.scrollTo );
    QCOMPARE( back.lookup.anchor, lookup.anchor );
    QCOMPARE( back.lookup.highlight.sourcePattern, lookup.highlight.sourcePattern );
    QCOMPARE( back.lookup.highlight.wildcards, lookup.highlight.wildcards );
    QCOMPARE( back.lookup.highlight.matchCase, lookup.highlight.matchCase );

    // Each escape the finding named keeps its own level instead of losing one.
    struct EscapeRow
    {
        char const * decoded;
        char const * encoded;
    };

    EscapeRow const rows[] = {
        { "%25", "%2525" },
        { "%2F", "%252F" },
        { "%26", "%2526" },
        { "%2520", "%252520" },
    };

    for( EscapeRow const & row : rows )
    {
        ArticleUrls::Lookup value;
        value.phrase = Config::InputPhrase::fromPhrase( QString::fromLatin1( row.decoded ) );
        value.group = 1;
        value.groupSpecified = true;

        const QUrl url = ArticleUrls::lookupDocument( value );
        QVERIFY2( url.toEncoded().contains( QByteArray( "word=" ) + QByteArray( row.encoded ) + '&' ),
                  url.toEncoded().constData() );

        const Parsed valueBack = ArticleUrls::parse( url, Use::DocumentLoad );
        QCOMPARE( valueBack.kind, Kind::LookupDocument );
        QCOMPARE( valueBack.lookup.phrase.phrase, QString::fromLatin1( row.decoded ) );
    }
}

void TstArticleSchemes::pictureDocumentComponents()
{
    // Full picture -> bres component round-trip: what the document URL names is
    // exactly the resource the handler has to fetch.
    const QUrl picture = ArticleUrls::pictureDocument(
      QStringLiteral( "d" ), QString::fromUtf8( "folder/\xe4\xb8\xad a.png?q=x+y" ) );
    QCOMPARE( picture.scheme(), QStringLiteral( "gdpicture" ) );
    QCOMPARE( picture.host(), QStringLiteral( "d" ) );

    const QByteArray encoded = picture.toEncoded();
    QVERIFY2( encoded.contains( "folder/%E4%B8%AD%20a.png" ), encoded.constData() );
    QVERIFY2( encoded.contains( "?q=x+y" ), encoded.constData() );

    const Parsed parsed = ArticleUrls::parse( picture, Use::DocumentLoad );
    QCOMPARE( parsed.kind, Kind::PictureDocument );
    QCOMPARE( parsed.dictionaryId, QStringLiteral( "d" ) );
    QCOMPARE( parsed.resourceName, QString::fromUtf8( "folder/\xe4\xb8\xad a.png?q=x+y" ) );

    const QUrl resource = bresUrl( parsed.dictionaryId, parsed.resourceName );
    QCOMPARE( resource.scheme(), QStringLiteral( "bres" ) );
    QVERIFY2( resource.toEncoded().contains( "folder/%E4%B8%AD%20a.png" ),
              resource.toEncoded().constData() );
    QCOMPARE( ArticleUrls::resourcePath( resource ), parsed.resourceName );

    // A leading slash in the decoded resource name is resource data, not the
    // structural URL slash: the producer adds exactly one structural slash.
    const QUrl nested = ArticleUrls::pictureDocument( QStringLiteral( "d" ),
                                                      QStringLiteral( "/sub/a.png" ) );
    QCOMPARE( nested.toEncoded(), QByteArray( "gdpicture://d//sub/a.png" ) );
    const Parsed nestedParsed = ArticleUrls::parse( nested, Use::DocumentLoad );
    QCOMPARE( nestedParsed.kind, Kind::PictureDocument );
    QCOMPARE( nestedParsed.resourceName, QStringLiteral( "/sub/a.png" ) );
}

void TstArticleSchemes::pictureDocumentPreservesResourceIdentity()
{
    // bres -> picture -> bres must be lossless: `parse` hands out an already
    // decoded resource name, so a leading slash in it is data and a bare `?` is
    // a present but empty query suffix.
    struct Row
    {
        char const * bres;
        char const * resourceName;
        char const * picture;
    };

    Row const rows[] = {
        { "bres://d//a.png?q=x+y", "/a.png?q=x+y", "gdpicture://d//a.png?q=x+y" },
        { "bres://d/a.png?", "a.png?", "gdpicture://d/a.png?" },
        { "bres://d//a.png?", "/a.png?", "gdpicture://d//a.png?" },
        { "bres://d/a%20b.png?q=x+y", "a b.png?q=x+y", "gdpicture://d/a%20b.png?q=x+y" },
        { "bres://d/a%3Fb.png", "a?b.png", "gdpicture://d/a?b.png" },
    };

    for( Row const & row : rows )
    {
        const Parsed resource =
          ArticleUrls::parse( QUrl( QString::fromLatin1( row.bres ) ), Use::DocumentLoad );
        QVERIFY2( resource.kind == Kind::Resource, qPrintable( QString::fromLatin1( row.bres ) ) );
        QCOMPARE( resource.dictionaryId, QStringLiteral( "d" ) );
        QCOMPARE( resource.resourceName, QString::fromLatin1( row.resourceName ) );

        const QUrl picture = ArticleUrls::pictureDocument( resource.dictionaryId, resource.resourceName );
        QCOMPARE( picture.scheme(), QStringLiteral( "gdpicture" ) );
        QCOMPARE( picture.host(), QStringLiteral( "d" ) );
        QCOMPARE( picture.hasQuery(), QString::fromLatin1( row.resourceName ).contains( QLatin1Char( '?' ) ) );
        QCOMPARE( picture.toEncoded(), QByteArray( row.picture ) );

        const Parsed back = ArticleUrls::parse( picture, Use::DocumentLoad );
        QCOMPARE( back.kind, Kind::PictureDocument );
        QCOMPARE( back.dictionaryId, QStringLiteral( "d" ) );
        QCOMPARE( back.resourceName, QString::fromLatin1( row.resourceName ) );

        // The same decoded name maps back onto the same resource URL.
        QCOMPARE( ArticleUrls::resourcePath( bresUrl( back.dictionaryId, back.resourceName ) ),
                  QString::fromLatin1( row.resourceName ) );
    }
}

void TstArticleSchemes::contextsWireFormatIsPinned()
{
    const QMap< QString, QString > contexts = fixedContextMap();
    QCOMPARE( QString::fromLatin1( ArticleUrls::encodeContexts( contexts ) ), fixedContextsBase64 );
    QCOMPARE( QString::fromLatin1( ArticleUrls::encodeContexts( QMap< QString, QString >() ) ),
              QStringLiteral( "AAAAAA==" ) );

    QMap< QString, QString > decoded;
    QString error;
    QVERIFY2( ArticleUrls::decodeContexts( fixedContextsBase64.toLatin1(), &decoded, &error ),
              qPrintable( error ) );
    QVERIFY( error.isEmpty() );
    QCOMPARE( contextText( decoded ), contextText( contexts ) );

    // The wire form survives the URL query encoding: the Base64 of a payload
    // that contains '+' and '=' comes back unchanged.
    QMap< QString, QString > plusContexts;
    plusContexts.insert( QStringLiteral( "k" ), QString::fromUtf8( "\xc3\xb8" ) ); // U+00F8
    QCOMPARE( QString::fromLatin1( ArticleUrls::encodeContexts( plusContexts ) ),
              QStringLiteral( "AAAAAQAAAAIAawAAAAIA+A==" ) );

    ArticleUrls::Lookup lookup;
    lookup.phrase = Config::InputPhrase::fromPhrase( QStringLiteral( "cat" ) );
    lookup.group = 1;
    lookup.groupSpecified = true;
    lookup.contexts = plusContexts;
    const QUrl produced = ArticleUrls::lookupDocument( lookup );
    const Parsed back = ArticleUrls::parse( produced, Use::DocumentLoad );
    QCOMPARE( back.kind, Kind::LookupDocument );
    QCOMPARE( contextText( back.lookup.contexts ), contextText( plusContexts ) );

    // The producer extracts the anchor from the contexts, exactly like the
    // baseline showDefinition path did.
    QMap< QString, QString > withAnchor = contexts;
    QCOMPARE( withAnchor.contains( QStringLiteral( "gdanchor" ) ), true );
    ArticleUrls::Lookup anchored;
    anchored.phrase = Config::InputPhrase::fromPhrase( QStringLiteral( "cat" ) );
    anchored.group = 1;
    anchored.groupSpecified = true;
    anchored.contexts = withAnchor;
    const Parsed anchoredBack = ArticleUrls::parse( ArticleUrls::lookupDocument( anchored ), Use::DocumentLoad );
    QCOMPARE( anchoredBack.kind, Kind::LookupDocument );
    QCOMPARE( anchoredBack.lookup.anchor, QStringLiteral( "part" ) );
    QVERIFY( !anchoredBack.lookup.contexts.contains( QStringLiteral( "gdanchor" ) ) );
    QCOMPARE( anchoredBack.lookup.contexts.value( QStringLiteral( "k" ) ),
              QString( QString::fromUtf8( "\xe4\xb8\xad+x%y" ) ) );
}

void TstArticleSchemes::contextsMalformedPayloadsRejected_data()
{
    QTest::addColumn< QByteArray >( "payload" );

    QMap< QString, QString > const map = fixedContextMap();
    const QByteArray valid = contextsPayload( map );

    QTest::newRow( "empty" ) << QByteArray();
    QTest::newRow( "invalid-base64" ) << QByteArrayLiteral( "!!!" );
    QTest::newRow( "bad-padding" ) << QByteArrayLiteral( "AAAAAA=" );
    QTest::newRow( "missing-padding" ) << QByteArrayLiteral( "AAAAAA" );
    QTest::newRow( "nonzero-unused-bits" ) << QByteArrayLiteral( "AAAAAB==" );
    QTest::newRow( "truncated" ) << valid.left( valid.size() - 1 ).toBase64();
    QTest::newRow( "trailing-byte" ) << ( valid + QByteArray( 1, '\0' ) ).toBase64();
    QTest::newRow( "short-payload" ) << QByteArrayLiteral( "AAAA" );
    QTest::newRow( "empty-map-only" ) << QByteArrayLiteral( "AAAAAA==" );
    QTest::newRow( "not-a-map" )
      << serializePayload( []( QDataStream & stream ) { stream << QStringLiteral( "abcd" ); } ).toBase64();
    QTest::newRow( "oversized-count" )
      << serializePayload( []( QDataStream & stream ) {
             stream << quint32( 0x7fffffff );
         } ).toBase64();
}

void TstArticleSchemes::contextsMalformedPayloadsRejected()
{
    QFETCH( QByteArray, payload );

    // "empty-map-only" is the one well-formed payload: it decodes to an empty
    // map. Everything else must be rejected.
    QMap< QString, QString > decoded;
    QString error;
    const bool decodedOk = ArticleUrls::decodeContexts( payload, &decoded, &error );

    if( payload == QByteArrayLiteral( "AAAAAA==" ) )
    {
        QVERIFY2( decodedOk, qPrintable( error ) );
        QVERIFY( decoded.isEmpty() );
        return;
    }

    QVERIFY2( !decodedOk, qPrintable( QString::fromLatin1( payload ) ) );
    QVERIFY2( !error.isEmpty(), "a rejected payload must explain itself" );
    QVERIFY( decoded.isEmpty() );
}

void TstArticleSchemes::contextsClearOutputOnFailure()
{
    QMap< QString, QString > decoded;
    decoded.insert( QStringLiteral( "stale" ), QStringLiteral( "value" ) );

    QString error;
    QVERIFY( !ArticleUrls::decodeContexts( QByteArrayLiteral( "!!!" ), &decoded, &error ) );
    QVERIFY2( decoded.isEmpty(), "a failed decode must not leave a partial map" );
    QVERIFY2( !error.isEmpty(), "a failed decode must explain itself" );

    // Null outputs are tolerated.
    QVERIFY( !ArticleUrls::decodeContexts( QByteArrayLiteral( "!!!" ), nullptr, nullptr ) );
    QVERIFY( !ArticleUrls::decodeContexts( QByteArray(), nullptr, nullptr ) );

    // A malformed contexts query item invalidates the whole URL.
    const Parsed parsed = ArticleUrls::parse(
      QUrl( QStringLiteral( "gdlookup://localhost/?word=cat&group=1&contexts=!!!" ) ), Use::DocumentLoad );
    QCOMPARE( parsed.kind, Kind::Invalid );
    QVERIFY2( parsed.error.contains( QStringLiteral( "contexts" ) ), qPrintable( parsed.error ) );
}

void TstArticleSchemes::contextsRejectsNonCanonicalBase64()
{
    // AbortOnBase64DecodingErrors stops at invalid characters only: a missing
    // pad and a non-zero unused-bits tail still decode. The wire form is the
    // producer's canonical padded Base64, so both payloads must be rejected
    // directly and through the public parser.
    for( QByteArray const & payload : { QByteArrayLiteral( "AAAAAA" ),
                                        QByteArrayLiteral( "AAAAAB==" ) } )
    {
        QMap< QString, QString > decoded;
        decoded.insert( QStringLiteral( "stale" ), QStringLiteral( "value" ) );
        QString error;
        QVERIFY2( !ArticleUrls::decodeContexts( payload, &decoded, &error ), payload.constData() );
        QVERIFY2( !error.isEmpty(), payload.constData() );
        QVERIFY2( decoded.isEmpty(), payload.constData() );

        const QUrl url( QStringLiteral( "gdlookup://localhost/?word=cat&group=1&contexts=" )
                        + QString::fromLatin1( payload ) );
        const Parsed parsed = ArticleUrls::parse( url, Use::DocumentLoad );
        QVERIFY2( parsed.kind == Kind::Invalid, payload.constData() );
        QVERIFY2( parsed.error.contains( QStringLiteral( "contexts" ) ), qPrintable( parsed.error ) );
        QVERIFY2( parsed.error.contains( QStringLiteral( "base64" ) ), qPrintable( parsed.error ) );
    }

    // The canonical spelling of the same bytes stays accepted, so the check is
    // about grammar and not about the decoded map.
    QMap< QString, QString > canonical;
    QString canonicalError;
    QVERIFY2( ArticleUrls::decodeContexts( QByteArrayLiteral( "AAAAAA==" ), &canonical, &canonicalError ),
              qPrintable( canonicalError ) );
    QVERIFY( canonical.isEmpty() );
}

void TstArticleSchemes::highlightSpecCompilesWildcards()
{
    const Parsed wildcard = ArticleUrls::parse(
      QUrl( QStringLiteral( "gdlookup://localhost/?word=cat&group=1&regexp=a*.txt&wildcards=1" ) ),
      Use::DocumentLoad );
    QCOMPARE( wildcard.kind, Kind::LookupDocument );

    // The source pattern is retained separately from the compiled expression.
    QCOMPARE( wildcard.lookup.highlight.sourcePattern, QStringLiteral( "a*.txt" ) );
    QVERIFY( wildcard.lookup.highlight.wildcards );
    QVERIFY( !wildcard.lookup.highlight.matchCase );
    QVERIFY( wildcard.lookup.highlight.expression.isValid() );

    // Highlight inside ordinary text, allowing '*' to cross a slash. Literal
    // offsets catch whole-subject anchoring without repeating the converter.
    const auto embedded = wildcard.lookup.highlight.expression.match( QStringLiteral( "see a/b.txt today" ) );
    QVERIFY( embedded.hasMatch() );
    QCOMPARE( embedded.capturedStart(), 4 );
    QCOMPARE( embedded.capturedLength(), 7 );
    QVERIFY( wildcard.lookup.highlight.expression.match( QStringLiteral( "ab.txt" ) ).hasMatch() );
    QVERIFY( !wildcard.lookup.highlight.expression.match( QStringLiteral( "abXtxt" ) ).hasMatch() );
    // matchCase defaults to false, so the compiled expression is case
    // insensitive.
    QVERIFY( wildcard.lookup.highlight.expression.match( QStringLiteral( "AB.TXT" ) ).hasMatch() );

    const Parsed caseSensitive = ArticleUrls::parse(
      QUrl( QStringLiteral( "gdlookup://localhost/?word=cat&group=1&regexp=a*.txt&wildcards=1"
                            "&matchcase=1" ) ),
      Use::DocumentLoad );
    QCOMPARE( caseSensitive.kind, Kind::LookupDocument );
    QVERIFY( caseSensitive.lookup.highlight.matchCase );
    QVERIFY( caseSensitive.lookup.highlight.expression.match( QStringLiteral( "ab.txt" ) ).hasMatch() );
    QVERIFY( !caseSensitive.lookup.highlight.expression.match( QStringLiteral( "AB.TXT" ) ).hasMatch() );

    // Without the wildcard flag the source pattern is already a regular
    // expression.
    const Parsed regexp = ArticleUrls::parse(
      QUrl( QStringLiteral( "gdlookup://localhost/?word=cat&group=1&regexp=%5Ba-c%5D%2B" ) ),
      Use::DocumentLoad );
    QCOMPARE( regexp.kind, Kind::LookupDocument );
    QCOMPARE( regexp.lookup.highlight.sourcePattern, QStringLiteral( "[a-c]+" ) );
    QVERIFY( !regexp.lookup.highlight.wildcards );
    QVERIFY( regexp.lookup.highlight.expression.match( QStringLiteral( "abc" ) ).hasMatch() );
    QVERIFY( !regexp.lookup.highlight.expression.match( QStringLiteral( "xyz" ) ).hasMatch() );
}

void TstArticleSchemes::highlightSpecFindsWildcardSpans_data()
{
    QTest::addColumn< QString >( "sourcePattern" );
    QTest::addColumn< bool >( "matchCase" );
    QTest::addColumn< QString >( "subject" );
    QTest::addColumn< QList< qsizetype > >( "expectedStarts" );
    QTest::addColumn< QList< qsizetype > >( "expectedLengths" );

    // Whole-subject anchoring loses every embedded match in these fixtures.
    // Expectations are literal positions in text, independent of conversion.
    QTest::newRow( "embedded-repeated-literal" )
      << QStringLiteral( "cat" ) << false << QStringLiteral( "one cat and another cat" )
      << QList< qsizetype >{ 4, 20 } << QList< qsizetype >{ 3, 3 };
    QTest::newRow( "embedded-case-insensitive" )
      << QStringLiteral( "cat" ) << false << QStringLiteral( "one CAT and another cat" )
      << QList< qsizetype >{ 4, 20 } << QList< qsizetype >{ 3, 3 };
    QTest::newRow( "embedded-case-sensitive" )
      << QStringLiteral( "cat" ) << true << QStringLiteral( "one CAT and another cat" )
      << QList< qsizetype >{ 20 } << QList< qsizetype >{ 3 };
    QTest::newRow( "embedded-star-crosses-slash" )
      << QStringLiteral( "a*.txt" ) << false << QStringLiteral( "see a/b.txt then a/c.txt now" )
      << QList< qsizetype >{ 4, 17 } << QList< qsizetype >{ 7, 7 };
    QTest::newRow( "embedded-question-crosses-slash" )
      << QStringLiteral( "a?b" ) << false << QStringLiteral( "see a/b and a-b now" )
      << QList< qsizetype >{ 4, 12 } << QList< qsizetype >{ 3, 3 };
}

void TstArticleSchemes::highlightSpecFindsWildcardSpans()
{
    QFETCH( QString, sourcePattern );
    QFETCH( bool, matchCase );
    QFETCH( QString, subject );
    QFETCH( QList< qsizetype >, expectedStarts );
    QFETCH( QList< qsizetype >, expectedLengths );

    QUrl url( QStringLiteral( "gdlookup://localhost/?word=cat&group=1&wildcards=1" ) );
    QUrlQuery query( url );
    query.addQueryItem( QStringLiteral( "regexp" ), sourcePattern );
    query.addQueryItem( QStringLiteral( "matchcase" ), matchCase ? QStringLiteral( "1" ) : QStringLiteral( "0" ) );
    url.setQuery( query );

    const Parsed parsed = ArticleUrls::parse( url, Use::DocumentLoad );
    QCOMPARE( parsed.kind, Kind::LookupDocument );
    const FtsHighlightSpec & highlight = parsed.lookup.highlight;
    QCOMPARE( highlight.sourcePattern, sourcePattern );
    QVERIFY( highlight.wildcards );
    QCOMPARE( highlight.matchCase, matchCase );
    QVERIFY2( highlight.expression.isValid(), qPrintable( highlight.expression.errorString() ) );

    QList< qsizetype > starts, lengths;
    auto matches = highlight.expression.globalMatch( subject );
    while( matches.hasNext() )
    {
        const auto match = matches.next();
        starts.append( match.capturedStart() );
        lengths.append( match.capturedLength() );
    }
    QCOMPARE( starts, expectedStarts );
    QCOMPARE( lengths, expectedLengths );
}

void TstArticleSchemes::highlightSpecRejectsInvalidUserPattern()
{
    // A malformed user pattern is not a URL syntax error: the parse succeeds and
    // the consumer reports the highlight as unusable.
    const Parsed parsed = ArticleUrls::parse(
      QUrl( QStringLiteral( "gdlookup://localhost/?word=cat&group=1&regexp=%5B" ) ), Use::DocumentLoad );
    QCOMPARE( parsed.kind, Kind::LookupDocument );
    QCOMPARE( parsed.lookup.highlight.sourcePattern, QStringLiteral( "[" ) );
    QVERIFY( !parsed.lookup.highlight.expression.isValid() );

    // An empty pattern disables the highlight entirely: no flag may claim one.
    const Parsed empty = ArticleUrls::parse(
      QUrl( QStringLiteral( "gdlookup://localhost/?word=cat&group=1&regexp=&wildcards=1&matchcase=1" ) ),
      Use::DocumentLoad );
    QCOMPARE( empty.kind, Kind::LookupDocument );
    QVERIFY( empty.lookup.highlight.sourcePattern.isEmpty() );
    QVERIFY( empty.lookup.highlight.expression.pattern().isEmpty() );
    QVERIFY( !empty.lookup.highlight.wildcards );
    QVERIFY( !empty.lookup.highlight.matchCase );
}

void TstArticleSchemes::readerContractIsImplementable()
{
    std::shared_ptr< DataRequestReader const > reader = std::make_shared< RecordingReader >();
    QVERIFY( reader );

    // The producer-free copy path is the one that may run off the GUI thread.
    // dataSlice()/fullData() take the producer and are therefore GUI-only; this
    // suite deliberately has no complete Dictionary::DataRequest definition, so
    // their exact signatures are pinned by the static assertions above instead.
    const QByteArray source = QByteArrayLiteral( "abcdef" );
    char destination[ 8 ] = {};
    reader->copyStreamBytes( destination, source.constData(), source.size() );
    QCOMPARE( QByteArray( destination, source.size() ), source );
    QCOMPARE( static_cast< RecordingReader const * >( reader.get() )->copyCalls, 1 );

    // Destruction runs through the interface and does not throw.
    QVERIFY( !readerDestructionObserved );
    reader.reset();
    QVERIFY( readerDestructionObserved );
}

void TstArticleSchemes::schemesRegisteredBeforeApplication()
{
    using Scheme = QWebEngineUrlScheme;
    Scheme::Flags const flags = Scheme::SecureScheme | Scheme::LocalScheme | Scheme::LocalAccessAllowed
                               | Scheme::CorsEnabled;
    QVERIFY( preApplication.registrationWarnings.isEmpty() );
    QCOMPARE( preApplication.schemes.size(), 7 );
    QCOMPARE( Scheme::schemeByName( "gdicon" ).name(), QByteArray() );
    int index = 0;
    for( QByteArray const name : { "gdlookup", "gdpicture", "bres", "gdau", "gdvideo", "gico", "qrcx" } )
    {
        Scheme const before = preApplication.schemes.at( index++ );
        QCOMPARE( before.name(), name );
        QCOMPARE( before.syntax(), Scheme::Syntax::Host );
        QCOMPARE( before.defaultPort(), Scheme::PortUnspecified );
        QCOMPARE( before.flags(), flags );
        Scheme const scheme = Scheme::schemeByName( name );
        QCOMPARE( scheme.name(), name );
        QCOMPARE( scheme.syntax(), Scheme::Syntax::Host );
        QCOMPARE( scheme.defaultPort(), Scheme::PortUnspecified );
        QCOMPARE( scheme.flags(), flags );
    }
}

void TstArticleSchemes::productionReaderForwardsAndOwnsBytes()
{
    auto request = instant( QByteArray( "ab\0cdef", 7 ) );
    auto reader = makeDataRequestReader();
    QVERIFY( reader );
    QByteArray const owned = reader->fullData( *request );
    QCOMPARE( owned, QByteArray( "ab\0cdef", 7 ) );
    char slice[ 4 ] = {};
    reader->dataSlice( *request, 2, 4, slice );
    QCOMPARE( QByteArray( slice, 4 ), QByteArray( "\0cde", 4 ) );
    QVERIFY_EXCEPTION_THROWN( reader->dataSlice( *request, 6, 2, slice ), Dictionary::exSliceOutOfRange );
    request = instant( "changed" );
    QCOMPARE( owned, QByteArray( "ab\0cdef", 7 ) );
    char copy[ 7 ] = {};
    reader->copyStreamBytes( copy, owned.constData(), 7 );
    QCOMPARE( QByteArray( copy, 7 ), owned );
    reader->copyStreamBytes( nullptr, nullptr, 0 );
    QCOMPARE( reader->fullData( *instant( {} ) ), QByteArray() );
    PendingRequest pending;
    QVERIFY_EXCEPTION_THROWN( reader->fullData( pending ), Dictionary::exRequestUnfinished );
}

void TstArticleSchemes::managerRejectsInvalidDocumentsAndAuthorities()
{
    BrowserFixture fixture;
    for( char const * text : { "gdlookup://localhost/cat", "gdlookup://localhost/?word=cat",
        "gdlookup://foreign/?word=cat&group=1", "gdlookup://u:p@localhost/?blank=1",
        "gdlookup://localhost:1/?blank=1", "bres://u@fixture/a.txt", "bres://fixture:0/a.txt",
        "qrcx://u@localhost/icons/blank.png", "qrcx://localhost:9/icons/blank.png",
        "gico://unknown/dicticon.png", "gico://fixture/unknown.png" } )
    {
        QString mime;
        auto request = fixture.resources.getResource( QUrl( QString::fromLatin1( text ) ), mime );
        QVERIFY2( !request || ( request->isFinished() && request->dataSize() == -1 ), text );
    }
    QVERIFY( fixture.dictionary->requestedNames.isEmpty() );
}

void TstArticleSchemes::managerDispatchPreservesLookupArguments()
{
    BrowserFixture fixture;
    QMap< QString, QString > contexts;
    contexts.insert( "fixture", "context + %20 中" );
    QUrl url( "gdlookup://localhost/?word=a%2Bb%2520c&punctuation_suffix=%2C&group=0&ignore_diacritics=1" );
    QUrlQuery query( url );
    query.addQueryItem( "contexts", QString::fromLatin1( ArticleUrls::encodeContexts( contexts ) ) );
    url.setQuery( query );
    QString mime;
    auto result = fixture.resources.getResource( url, mime );
    QVERIFY( result );
    QTRY_VERIFY_WITH_TIMEOUT( result->isFinished(), 3000 );
    QCOMPARE( mime, QStringLiteral( "text/html" ) );
    QCOMPARE( gd::toQString( fixture.dictionary->articleWord ), QStringLiteral( "a+b%20c" ) );
    QVERIFY( std::find( fixture.dictionary->articleAlternates.begin(), fixture.dictionary->articleAlternates.end(),
                      gd::toWString( QStringLiteral( "a+b%20c," ) ) ) != fixture.dictionary->articleAlternates.end() );
    QCOMPARE( gd::toQString( fixture.dictionary->articleContext ), QStringLiteral( "context + %20 中" ) );
    QVERIFY( fixture.dictionary->articleIgnoreDiacritics );

    // The explicit FTS branch ignores contexts, muted dictionaries and diacritics.
    result = fixture.resources.getResource( QUrl( url.toString() + "&dictionaries=fixture&muted=fixture" ), mime );
    QVERIFY( result );
    QTRY_VERIFY_WITH_TIMEOUT( result->isFinished(), 3000 );
    QVERIFY( fixture.dictionary->articleContext.empty() );
    QVERIFY( !fixture.dictionary->articleIgnoreDiacritics );

    fixture.dictionary->articleWord.clear();
    result = fixture.resources.getResource( QUrl( url.toString() + "&muted=,fixture,," ), mime );
    QVERIFY( result );
    QTRY_VERIFY_WITH_TIMEOUT( result->isFinished(), 3000 );
    QVERIFY( fixture.dictionary->articleWord.empty() );

    Instances::Group group( QStringLiteral( "selected empty group" ) );
    group.id = 7;
    fixture.groups.push_back( group );
    QUrl grouped = url;
    QUrlQuery groupedQuery( grouped );
    groupedQuery.removeAllQueryItems( QStringLiteral( "group" ) );
    groupedQuery.addQueryItem( QStringLiteral( "group" ), QStringLiteral( "7" ) );
    grouped.setQuery( groupedQuery );
    result = fixture.resources.getResource( grouped, mime );
    QVERIFY( result );
    QTRY_VERIFY_WITH_TIMEOUT( result->isFinished(), 3000 );
    QVERIFY( fixture.dictionary->articleWord.empty() );
    QVERIFY( makeDataRequestReader()->fullData( *result ).contains( "selected empty group" ) );
}

void TstArticleSchemes::managerServesBlankPictureQrcAndMime()
{
    BrowserFixture fixture;
    QString mime;
    auto reader = makeDataRequestReader();
    auto blank = fixture.resources.getResource( QUrl( "gdlookup://localhost?blank=1&group=abc" ), mime );
    QVERIFY( blank && blank->isFinished() );
    QCOMPARE( mime, QStringLiteral( "text/html" ) );
    QVERIFY( reader->fullData( *blank ).contains( "<title>(untitled)</title>" ) );
    auto picture = fixture.resources.getResource( QUrl( "gdpicture://fixture/folder/a%20b.png?q=x+y#frag" ), mime );
    QVERIFY( picture );
    QCOMPARE( mime, QStringLiteral( "text/html" ) );
    QVERIFY( reader->fullData( *picture ).contains( "<img src=\"bres://fixture/folder/a%20b.png?q=x+y#frag\"" ) );

    QFile png( ":/icons/blank.png" );
    QVERIFY( png.open( QIODevice::ReadOnly ) );
    QByteArray const pngBytes = png.readAll();
    for( char const * authority : { "localhost", "" } )
    {
        auto resource = fixture.resources.getResource( QUrl( QStringLiteral( "qrcx://%1/icons/blank.png" ).arg( authority ) ), mime );
        QVERIFY( resource );
        QCOMPARE( mime, QStringLiteral( "image/png" ) );
        QCOMPARE( reader->fullData( *resource ), pngBytes );
    }
    struct Row { char const * scheme; char const * name; char const * mime; };
    for( Row const & row : { Row{ "bres", "a.css", "text/css" },
        Row{ "gdau", "a.mp3", "audio/mpeg" }, Row{ "gdvideo", "a.webm", "video/webm" },
        Row{ "bres", "a.unknownextension", "application/octet-stream" } } )
    {
        fixture.dictionary->resources.insert( row.name, instant( "bytes" ) );
        auto resource = fixture.resources.getResource( QUrl( QString::fromLatin1( row.scheme ) + "://fixture/" + row.name ), mime );
        QVERIFY( resource );
        QCOMPARE( mime, QString::fromLatin1( row.mime ) );
        QCOMPARE( reader->fullData( *resource ), QByteArray( "bytes" ) );
    }
    fixture.dictionary->resources.insert( "/中 a%20.png?q=x+y", instant( "encoded" ) );
    auto encoded = fixture.resources.getResource( QUrl( "bres://fixture//%E4%B8%AD%20a%2520.png?q=x+y" ), mime );
    QVERIFY( encoded );
    QCOMPARE( reader->fullData( *encoded ), QByteArray( "encoded" ) );
    QCOMPARE( fixture.dictionary->requestedNames.last(), QStringLiteral( "/中 a%20.png?q=x+y" ) );
    auto icon = fixture.resources.getResource( QUrl( "gico://fixture/dicticon.png" ), mime );
    QVERIFY( icon );
    QCOMPARE( mime, QStringLiteral( "image/png" ) );
    QVERIFY( reader->fullData( *icon ).startsWith( QByteArray::fromHex( "89504e470d0a1a0a" ) ) );
}

void TstArticleSchemes::bufferedResourceRoundTrip()
{
    BrowserFixture fixture;
    fixture.dictionary->resources.insert( "instant.html", instant( "<html><title>instant bytes</title><body>real bytes<script>window.answer=42;</script></body></html>" ) );
    auto result = load( *fixture.page, QWebEngineHttpRequest( QUrl( "bres://fixture/instant.html" ) ) );
    QVERIFY( result.completed );
    QVERIFY( result.ok );
    QCOMPARE( fixture.page->title(), QStringLiteral( "instant bytes" ) );
    QCOMPARE( javascript( *fixture.page, "window.answer" ).toInt(), 42 );
    QCOMPARE( fixture.dictionary->requestedNames, QStringList{ "instant.html" } );
}

void TstArticleSchemes::emptyResourceSucceeds()
{
    BrowserFixture fixture;
    fixture.dictionary->resources.insert( "empty.html", instant( {} ) );
    auto result = load( *fixture.page, QWebEngineHttpRequest( QUrl( "bres://fixture/empty.html" ) ) );
    QVERIFY( result.completed );
    QVERIFY( result.ok );
    QCOMPARE( javascript( *fixture.page, "document.body.textContent" ).toString(), QString() );
}

void TstArticleSchemes::terminalErrorWinsOverPartialBytes()
{
    BrowserFixture fixture;
    sptr< FailedRequest > request( new FailedRequest );
    fixture.dictionary->resources.insert( "error.html", request );
    QSignalSpy failed( &fixture.handler, &ArticleSchemeHandler::producerFailed );
    connect( &fixture.handler, &ArticleSchemeHandler::producerFailed, &fixture.handler, [&] {
        if( failed.size() == 1 )
        {
            emit request->updated();
            emit request->finished();
        }
    } );
    auto result = load( *fixture.page, QWebEngineHttpRequest( QUrl( "bres://fixture/error.html" ) ) );
    QVERIFY( result.completed );
    QVERIFY( !result.ok );
    QCOMPARE( failed.size(), 1 );
    QCOMPARE( failed.front().at( 0 ).toUrl(), QUrl( "bres://fixture/error.html" ) );
    QCOMPARE( failed.front().at( 1 ).toString(), QStringLiteral( "producer IO failed" ) );
}

void TstArticleSchemes::factoryRunsBeforeImmediateStateInspection()
{
    auto pending = sptr< PendingRequest >( new PendingRequest );
    int calls = 0;
    BrowserFixture fixture( [&]( sptr< Dictionary::DataRequest > const & request ) {
        ++calls;
        if( request.get() == pending.get() )
            pending->complete( "<title>finished in factory</title>" );
        return makeDataRequestReader();
    } );
    fixture.dictionary->resources.insert( "factory.html", pending );
    auto result = load( *fixture.page, QWebEngineHttpRequest( QUrl( "bres://fixture/factory.html" ) ) );
    QVERIFY( result.completed );
    QVERIFY( result.ok );
    QCOMPARE( calls, 1 );
    QCOMPARE( fixture.page->title(), QStringLiteral( "finished in factory" ) );
}

void TstArticleSchemes::nullReaderFailsOnce()
{
    int calls = 0;
    BrowserFixture fixture( [&]( sptr< Dictionary::DataRequest > const & ) {
        ++calls;
        return std::shared_ptr< DataRequestReader const >();
    } );
    QSignalSpy failed( &fixture.handler, &ArticleSchemeHandler::producerFailed );
    auto result = load( *fixture.page, QWebEngineHttpRequest( QUrl( "gdlookup://localhost?blank=1" ) ) );
    QVERIFY( result.completed );
    QVERIFY( !result.ok );
    QCOMPARE( calls, 1 );
    QCOMPARE( failed.size(), 1 );
    QVERIFY( !failed.front().at( 1 ).toString().isEmpty() );
}

void TstArticleSchemes::jobDestructionCancelsOnce()
{
    BrowserFixture fixture;
    auto pending = sptr< PendingRequest >( new PendingRequest );
    fixture.dictionary->resources.insert( "pending.html", pending );
    fixture.page->load( QUrl( "bres://fixture/pending.html" ) );
    QTRY_COMPARE_WITH_TIMEOUT( fixture.handler.jobs.size(), 1, 5000 );
    QVERIFY( fixture.handler.jobs.front() );
    QCOMPARE( pending->cancellations, 0 );
    fixture.page.reset();
    QTRY_VERIFY_WITH_TIMEOUT( fixture.handler.jobs.front().isNull(), 5000 );
    QCOMPARE( pending->cancellations, 1 );
    // Duplicate already-queued terminal events must not cancel a second time.
    pending->complete( "late bytes" );
    QCoreApplication::sendPostedEvents();
    QCOMPARE( pending->cancellations, 1 );
}

void TstArticleSchemes::handlerDestructionCancelsOnce()
{
    BrowserFixture fixture;
    fixture.profile.removeAllUrlSchemeHandlers();
    auto handler = std::make_unique< ObservedHandler >( fixture.resources );
    fixture.profile.installUrlSchemeHandler( "bres", handler.get() );
    auto pending = sptr< PendingRequest >( new PendingRequest );
    fixture.dictionary->resources.insert( "pending.html", pending );
    fixture.page->load( QUrl( "bres://fixture/pending.html" ) );
    QTRY_COMPARE_WITH_TIMEOUT( handler->jobs.size(), 1, 5000 );
    fixture.profile.removeUrlSchemeHandler( handler.get() );
    handler.reset();
    QCOMPARE( pending->cancellations, 1 );
    fixture.page.reset();
    QCoreApplication::sendPostedEvents();
    QCOMPARE( pending->cancellations, 1 );
}

void TstArticleSchemes::unsupportedMethodIsDenied()
{
    if( !qEnvironmentVariableIsSet( "GD_ARTICLE_METHOD_PROBE" ) )
    {
        // Production flags make Chromium reject POST before the handler runs.
        // A separate process enables POST on bres solely to exercise the real
        // handler's method guard, without altering the production flag check.
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert( QStringLiteral( "GD_ARTICLE_METHOD_PROBE" ), QStringLiteral( "1" ) );
        child.setProcessEnvironment( environment );
        child.setProcessChannelMode( QProcess::MergedChannels );
        child.start( QCoreApplication::applicationFilePath(), { QStringLiteral( "unsupportedMethodIsDenied" ) } );
        QTRY_VERIFY_WITH_TIMEOUT( child.state() == QProcess::NotRunning, 30000 );
        QByteArray const output = child.readAll();
        QVERIFY2( child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, output.constData() );
        QVERIFY( output.contains( "PASS   : TstArticleSchemes::unsupportedMethodIsDenied()" ) );
        return;
    }

    QVERIFY( QWebEngineUrlScheme::schemeByName( "bres" ).flags().testFlag( QWebEngineUrlScheme::FetchApiAllowed ) );
    BrowserFixture fixture;
    fixture.dictionary->resources.insert( "start.html", instant( "<html><body>start</body></html>" ) );
    fixture.dictionary->resources.insert( "post.html", instant( "<html><body>POST must not reach the producer</body></html>" ) );
    auto result = load( *fixture.page, QWebEngineHttpRequest( QUrl( "bres://fixture/start.html" ) ) );
    QVERIFY( result.completed );
    QVERIFY( result.ok );
    fixture.page->runJavaScript( "fetch('bres://fixture/post.html',{method:'POST'}).catch(function(){});" );
    QTRY_VERIFY_WITH_TIMEOUT( fixture.handler.methods.contains( "POST" ), 5000 );
    QCOMPARE( fixture.dictionary->requestedNames, QStringList{ QStringLiteral( "start.html" ) } );
}

void TstArticleSchemes::missingResourceFails()
{
    BrowserFixture fixture;
    QSignalSpy failed( &fixture.handler, &ArticleSchemeHandler::producerFailed );
    for( char const * url : { "bres://fixture/missing.html", "qrcx://localhost/does-not-exist.png",
                            "gico://unknown/dicticon.png", "gdlookup://localhost/cat" } )
    {
        auto result = load( *fixture.page, QWebEngineHttpRequest( QUrl( url ) ) );
        QVERIFY2( result.completed, url );
        QVERIFY2( !result.ok, url );
    }
    QCOMPARE( failed.size(), 0 );
}

void TstArticleSchemes::realDocumentExecutesLegacyJavascript()
{
    BrowserFixture fixture;
    QWebEngineScript facade;
    facade.setInjectionPoint( QWebEngineScript::DocumentCreation );
    facade.setWorldId( QWebEngineScript::MainWorld );
    facade.setSourceCode( "window.calls=[];window.articleview={onJsActiveArticleChanged:function(id){calls.push([id,arguments.length]);}};" );
    fixture.page->scripts().insert( facade );
    auto result = load( *fixture.page, QWebEngineHttpRequest( QUrl( "gdlookup://localhost/?word=cat&group=0" ) ) );
    QVERIFY( result.completed );
    QVERIFY( result.ok );
    QCOMPARE( fixture.page->title(), QStringLiteral( "cat" ) );
    QCOMPARE( javascript( *fixture.page, "document.getElementById('fixture-article').textContent" ).toString(), QStringLiteral( "dictionary body" ) );
    QCOMPARE( javascript( *fixture.page, "JSON.stringify(calls)" ).toString(), QStringLiteral( "[[\"gdfrom-fixture\",1]]" ) );
    QCOMPARE( javascript( *fixture.page,
        "var other=document.createElement('div');other.id='gdfrom-other';document.body.appendChild(other);"
        "gdMakeArticleActive('other');JSON.stringify(calls)" ).toString(),
        QStringLiteral( "[[\"gdfrom-fixture\",1],[\"gdfrom-other\",1]]" ) );
}

void TstArticleSchemes::explicitHttpRetainsRefererAndFrameHeaders()
{
    BrowserFixture fixture;
    QTcpServer server;
    QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );
    QByteArray received;
    connect( &server, &QTcpServer::newConnection, &server, [&] {
        auto socket = server.nextPendingConnection();
        connect( socket, &QTcpSocket::readyRead, socket, [&, socket] {
            received += socket->readAll();
            if( !received.contains( "\r\n\r\n" ) ) return;
            socket->write( "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Frame-Options: DENY\r\nConnection: close\r\n\r\nbytes" );
            socket->disconnectFromHost();
        } );
        connect( socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater );
    } );
    QNetworkRequest request( QUrl( QStringLiteral( "http://127.0.0.1:%1/resource" ).arg( server.serverPort() ) ) );
    request.setRawHeader( "Referer", "https://unrelated.example/article" );
    auto reply = fixture.resources.get( request );
    QSignalSpy finished( reply, &QNetworkReply::finished );
    QVERIFY( QTest::qWaitFor( [&] { return !finished.isEmpty(); }, 3000 ) );
    QCOMPARE( reply->error(), QNetworkReply::NoError );
    QCOMPARE( reply->rawHeader( "X-Frame-Options" ), QByteArray( "DENY" ) );
    QCOMPARE( reply->readAll(), QByteArray( "bytes" ) );
    QVERIFY( received.contains( "Referer: https://unrelated.example/article\r\n" ) );
    delete reply;
}

void TstArticleSchemes::emittedFileUrlsStayWithSnapshot()
{
    auto const urls = ArticleMaker::emittedFileUrls(
        "<img src=\"file:///missing/a%20b.png\"><style>.x{background:url('file:///tmp/c.png')}</style>"
        "<a href=\"https://example.org/file:///ignored\">link</a>"
        "<img src=\"file:///missing/a%20b.png\">" );
    QCOMPARE( urls, ( QList< QUrl >{ QUrl( "file:///missing/a%20b.png" ), QUrl( "file:///tmp/c.png" ) } ) );
    QVERIFY( ArticleMaker::emittedFileUrls( "<p>next snapshot</p>" ).isEmpty() );
}

void TstArticleSchemes::bufferedProducerWaitsAndTerminalSignalsLatch()
{
    BrowserFixture fixture;
    sptr< PendingRequest > pending( new PendingRequest );
    fixture.dictionary->resources.insert( QStringLiteral( "waiting.html" ), pending );
    ObservedHandler handler( fixture.resources );
    QWebEngineProfile profile;
    profile.installUrlSchemeHandler( "bres", &handler );
    QWebEnginePage page( &profile );
    QSignalSpy finished( &page, &QWebEnginePage::loadFinished );
    page.load( QUrl( "bres://fixture/waiting.html" ) );
    QTRY_COMPARE_WITH_TIMEOUT( handler.jobs.size(), 1, 5000 );
    QTest::qWait( 50 );
    QVERIFY( finished.isEmpty() );
    QVERIFY( handler.jobs.front() );
    QVERIFY( handler.jobs.front()->findChildren< QIODevice * >().isEmpty() );
    pending->append( "<html><title>still-waiting</title>" );
    QTest::qWait( 50 );
    QVERIFY( finished.isEmpty() );
    QVERIFY( handler.jobs.front()->findChildren< QIODevice * >().isEmpty() );
    pending->complete( "<html><title>still-waiting</title><p id='done'>complete</p></html>" );
    // Queue duplicates before Qt can consume the reply or destroy the job.
    pending->repeatNotifications();
    pending->repeatNotifications();
    QTRY_COMPARE_WITH_TIMEOUT( finished.size(), 1, 5000 );
    QVERIFY( finished.front().front().toBool() );
    QCOMPARE( page.title(), QStringLiteral( "still-waiting" ) );
    bool done = false;
    page.runJavaScript( QStringLiteral( "document.getElementById('done').textContent" ), [&]( QVariant value ) {
        QCOMPARE( value.toString(), QStringLiteral( "complete" ) );
        done = true;
    } );
    QTRY_VERIFY_WITH_TIMEOUT( done, 5000 );
    pending->repeatNotifications();
    pending->repeatNotifications();
    QTest::qWait( 50 );
    QCOMPARE( finished.size(), 1 );
    QTRY_VERIFY_WITH_TIMEOUT( !handler.buffersAtDestruction.isEmpty(), 5000 );
    QCOMPARE( handler.buffersAtDestruction.front(), 1 );
}

void TstArticleSchemes::rangeReplies_data()
{
    QTest::addColumn< QByteArray >( "header" );
    QTest::addColumn< QByteArray >( "body" );
    QTest::addColumn< bool >( "success" );
    QTest::addColumn< qint64 >( "position" );
    QTest::addColumn< QString >( "text" );
    QTest::newRow( "case-insensitive-unit" ) << QByteArray( "BYTES=3-5" ) << QByteArray( "0123456789" ) << true << qint64( 3 ) << QStringLiteral( "345" );
    QTest::newRow( "linear-whitespace" ) << QByteArray( " bytes \t=\t3 - 5 " ) << QByteArray( "0123456789" ) << true << qint64( 3 ) << QStringLiteral( "345" );
    // Spec §4.5: fields are checked nonnegative decimal positions. Signed
    // fields, empty comma tokens and the "N--1" spelling are malformed, so they
    // are not a Range selection and the response stays a full 200.
    // Every row here is a spelling Chromium's loader accepts, so the handler has
    // to position the buffer exactly where the loader will interpret the range:
    // Qt supplies the Content-Range from its own parse, and a handler that
    // answered "no selection" with position 0 would deliver prefix bytes labelled
    // as the representation's suffix.
    QTest::newRow( "signed-integers" ) << QByteArray( "bytes=+3-+5" ) << QByteArray( "0123456789" ) << true << qint64( 3 ) << QStringLiteral( "345" );
    QTest::newRow( "empty-comma-tokens" ) << QByteArray( "bytes=,3-5," ) << QByteArray( "0123456789" ) << true << qint64( 3 ) << QStringLiteral( "345" );
    QTest::newRow( "negative-last-is-open" ) << QByteArray( "bytes=3--1" ) << QByteArray( "0123456789" ) << true << qint64( 3 ) << QStringLiteral( "3456789" );
    // The suffix spellings are the ones where a stricter parser would be actively
    // wrong: Qt resolves the suffix itself and would label prefix bytes as the
    // representation's suffix, so the handler must position to 8 like the loader.
    QTest::newRow( "signed-suffix" ) << QByteArray( "bytes=-+2" ) << QByteArray( "0123456789" ) << true << qint64( 8 ) << QStringLiteral( "89" );
    QTest::newRow( "empty-comma-suffix" ) << QByteArray( "bytes=,-2," ) << QByteArray( "0123456789" ) << true << qint64( 8 ) << QStringLiteral( "89" );
    QTest::newRow( "zero-suffix-ignored" ) << QByteArray( "bytes=-0" ) << QByteArray( "0123456789" ) << true << qint64( 0 ) << QStringLiteral( "0123456789" );
    QTest::newRow( "reverse-ignored" ) << QByteArray( "bytes=5-3" ) << QByteArray( "0123456789" ) << true << qint64( 0 ) << QStringLiteral( "0123456789" );
    QTest::newRow( "whole" ) << QByteArray() << QByteArray( "0123456789" ) << true << qint64( 0 ) << QStringLiteral( "0123456789" );
    QTest::newRow( "zero" ) << QByteArray( "bytes=0-" ) << QByteArray( "0123456789" ) << true << qint64( 0 ) << QStringLiteral( "0123456789" );
    QTest::newRow( "positive-open" ) << QByteArray( "bytes=3-" ) << QByteArray( "0123456789" ) << true << qint64( 3 ) << QStringLiteral( "3456789" );
    QTest::newRow( "bounded" ) << QByteArray( "bytes=3-5" ) << QByteArray( "0123456789" ) << true << qint64( 3 ) << QStringLiteral( "345" );
    QTest::newRow( "suffix" ) << QByteArray( "bytes=-2" ) << QByteArray( "0123456789" ) << true << qint64( 8 ) << QStringLiteral( "89" );
    QTest::newRow( "long-suffix" ) << QByteArray( "bytes=-20" ) << QByteArray( "0123456789" ) << true << qint64( 0 ) << QStringLiteral( "0123456789" );
    QTest::newRow( "clamped-end" ) << QByteArray( "bytes=3-99" ) << QByteArray( "0123456789" ) << true << qint64( 3 ) << QStringLiteral( "3456789" );
    QTest::newRow( "past-end" ) << QByteArray( "bytes=10-" ) << QByteArray( "0123456789" ) << false << qint64( -1 ) << QString();
    QTest::newRow( "empty-whole" ) << QByteArray() << QByteArray( "" ) << true << qint64( 0 ) << QString();
    // A parsed range against S == 0 is unsatisfiable: fail once, never a
    // truncated 200 and never a buffer exposed for the selection.
    QTest::newRow( "empty-suffix-unsatisfiable" ) << QByteArray( "bytes=-2" ) << QByteArray( "" ) << false << qint64( -1 ) << QString();
    QTest::newRow( "empty-range" ) << QByteArray( "bytes=0-" ) << QByteArray( "" ) << false << qint64( -1 ) << QString();
    QTest::newRow( "malformed-ignored" ) << QByteArray( "bytes=nope" ) << QByteArray( "0123456789" ) << true << qint64( 0 ) << QStringLiteral( "0123456789" );
    QTest::newRow( "multiple-ignored" ) << QByteArray( "bytes=0-1,5-6" ) << QByteArray( "0123456789" ) << true << qint64( 0 ) << QStringLiteral( "0123456789" );
    QTest::newRow( "overflow-ignored" ) << QByteArray( "bytes=9223372036854775808-" ) << QByteArray( "0123456789" ) << true << qint64( 0 ) << QStringLiteral( "0123456789" );
}

void TstArticleSchemes::rangeReplies()
{
    QFETCH( QByteArray, header );
    QFETCH( QByteArray, body );
    QFETCH( bool, success );
    QFETCH( qint64, position );
    QFETCH( QString, text );
    BrowserFixture fixture;
    fixture.dictionary->resources.insert( QStringLiteral( "range.html" ), instant( body ) );
    ObservedHandler handler( fixture.resources );
    // Assert on the GUI handoff, before Chromium consumes a malformed mutant
    // device and obscures the violated contract with an IO-thread failure.
    handler.validateSnapshot = [&] {
        if( !success )
        {
            QVERIFY( handler.bufferSizes.isEmpty() );
            return;
        }
        QCOMPARE( handler.bufferSizes, QList< qint64 >{ body.size() } );
        QCOMPARE( handler.bufferPositions, QList< qint64 >{ position } );
        QCOMPARE( handler.buffersOpenAndJobOwned, QList< bool >{ true } );
    };
    QWebEngineProfile profile;
    profile.installUrlSchemeHandler( "bres", &handler );
    QWebEnginePage page( &profile );
    QSignalSpy finished( &page, &QWebEnginePage::loadFinished );
    QWebEngineHttpRequest request( QUrl( "bres://fixture/range.html" ) );
    if( !header.isEmpty() ) request.setHeader( "Range", header );
    page.load( request );
    QTRY_COMPARE_WITH_TIMEOUT( finished.size(), 1, 5000 );
    QCOMPARE( finished.front().front().toBool(), success );
    if( !success )
    {
        QVERIFY( handler.bufferSizes.isEmpty() );
        return;
    }
    QCOMPARE( handler.bufferSizes, QList< qint64 >{ body.size() } );
    QCOMPARE( handler.bufferPositions, QList< qint64 >{ position } );
    QCOMPARE( handler.buffersOpenAndJobOwned, QList< bool >{ true } );
    bool done = false;
    page.runJavaScript( QStringLiteral( "document.body.textContent" ), [&]( QVariant value ) {
        QCOMPARE( value.toString(), text );
        done = true;
    } );
    QTRY_VERIFY_WITH_TIMEOUT( done, 5000 );
}

void TstArticleSchemes::rangeWaitsForCompletion()
{
    BrowserFixture fixture;
    sptr< PendingRequest > pending( new PendingRequest );
    fixture.dictionary->resources.insert( QStringLiteral( "range.html" ), pending );
    ObservedHandler handler( fixture.resources );
    QWebEngineProfile profile;
    profile.installUrlSchemeHandler( "bres", &handler );
    QWebEnginePage page( &profile );
    QSignalSpy finished( &page, &QWebEnginePage::loadFinished );
    QWebEngineHttpRequest request( QUrl( "bres://fixture/range.html" ) );
    request.setHeader( "Range", "bytes=-2" );
    page.load( request );
    QTRY_COMPARE_WITH_TIMEOUT( handler.jobs.size(), 1, 5000 );
    QVERIFY( handler.jobs.front() );
    QVERIFY( finished.isEmpty() );
    pending->append( "01234" );
    QTest::qWait( 50 );
    QVERIFY( finished.isEmpty() );
    QVERIFY( handler.jobs.front() );
    QVERIFY( handler.jobs.front()->findChildren< QIODevice * >().isEmpty() );
    pending->complete( "0123456789" );
    QTRY_COMPARE_WITH_TIMEOUT( finished.size(), 1, 5000 );
    QVERIFY( finished.front().front().toBool() );
    bool done = false;
    page.runJavaScript( QStringLiteral( "document.body.textContent" ), [&]( QVariant value ) {
        QCOMPARE( value.toString(), QStringLiteral( "89" ) );
        done = true;
    } );
    QTRY_VERIFY_WITH_TIMEOUT( done, 5000 );
}

void TstArticleSchemes::factoryReaderSuppliesBufferedBytes()
{
    BrowserFixture fixture;
    fixture.dictionary->resources.insert( QStringLiteral( "reader.html" ), instant( "wrong producer bytes" ) );
    ArticleSchemeHandler handler( fixture.resources, []( sptr< Dictionary::DataRequest > ) {
        return std::make_shared< RecordingReader >();
    } );
    QWebEngineProfile profile;
    profile.installUrlSchemeHandler( "bres", &handler );
    QWebEnginePage page( &profile );
    QSignalSpy finished( &page, &QWebEnginePage::loadFinished );
    page.load( QUrl( "bres://fixture/reader.html" ) );
    QTRY_COMPARE_WITH_TIMEOUT( finished.size(), 1, 5000 );
    QVERIFY( finished.front().front().toBool() );
    bool done = false;
    page.runJavaScript( QStringLiteral( "document.body.textContent" ), [&]( QVariant value ) {
        QCOMPARE( value.toString(), QStringLiteral( "payload" ) );
        done = true;
    } );
    QTRY_VERIFY_WITH_TIMEOUT( done, 5000 );
}

void TstArticleSchemes::resourceConstructionExceptionFailsOnce()
{
    BrowserFixture fixture;
    ArticleSchemeHandler handler( fixture.resources );
    QSignalSpy failures( &handler, &ArticleSchemeHandler::producerFailed );
    QWebEngineProfile profile;
    profile.installUrlSchemeHandler( "bres", &handler );
    QWebEnginePage page( &profile );
    QSignalSpy finished( &page, &QWebEnginePage::loadFinished );
    page.load( QUrl( "bres://fixture/throws.html" ) );
    QTRY_COMPARE_WITH_TIMEOUT( finished.size(), 1, 5000 );
    QVERIFY( !finished.front().front().toBool() );
    QCOMPARE( failures.size(), 1 );
    QCOMPARE( failures.front().front().toUrl(), QUrl( "bres://fixture/throws.html" ) );
    QCOMPARE( failures.front().at( 1 ).toString(), QStringLiteral( "dictionary resource failed" ) );
}

void TstArticleSchemes::emptyMakerRendersBlankAndSuggestionLinks()
{
    BrowserFixture fixture;
    fixture.dictionaries.clear();
    ArticleSchemeHandler handler( fixture.resources );
    QWebEngineProfile profile;
    profile.installUrlSchemeHandler( "gdlookup", &handler );
    QWebEnginePage page( &profile );
    QSignalSpy finished( &page, &QWebEnginePage::loadFinished );
    page.load( QUrl( "gdlookup://localhost/?blank=1" ) );
    QTRY_COMPARE_WITH_TIMEOUT( finished.size(), 1, 5000 );
    QVERIFY( finished.front().front().toBool() );
    bool done = false;
    page.runJavaScript( QStringLiteral( "typeof gdSelectArticle === 'function' && document.body.children.length === 0" ), [&]( QVariant value ) {
        QVERIFY( value.toBool() ); done = true;
    } );
    QTRY_VERIFY_WITH_TIMEOUT( done, 5000 );
    Config::InputPhrase phrase;
    phrase.phrase = QStringLiteral( "red fox" );
    auto request = fixture.maker.makeDefinitionFor( phrase, 0, {}, {} );
    QTRY_VERIFY_WITH_TIMEOUT( request->isFinished(), 5000 );
    auto const & bytes = request->getFullData();
    QByteArray html( bytes.data(), bytes.size() );
    QVERIFY( html.contains( "gdlookup://localhost/red" ) );
    QVERIFY( html.contains( "gdlookup://localhost/fox" ) );
    QVERIFY( html.contains( "<title>red fox</title>" ) );
}

void TstArticleSchemes::makerPreservesRtlEscapingAndFileSnapshots()
{
    BrowserFixture fixture;
    QString const word = QString::fromUtf8( "עברית<&" );
    auto notFoundRequest = fixture.maker.makeNotFoundTextFor( word, QStringLiteral( "group<&" ) );
    auto const & notFoundData = notFoundRequest->getFullData();
    QByteArray notFound( notFoundData.data(), notFoundData.size() );
    QVERIFY( notFound.contains( QString( QChar( char16_t( 0x202e ) ) ).toUtf8() ) );
    QVERIFY( notFound.contains( QString( QChar( char16_t( 0x202c ) ) ).toUtf8() ) );
    QVERIFY( notFound.contains( "&lt;&amp;" ) );
    QVERIFY( notFound.contains( "group&lt;&amp;" ) );
    fixture.dictionary->articleBody = "<img src='file:///missing/a%20b.png'><a href='file:///tmp/c.png'>c</a>";
    Config::InputPhrase phrase;
    phrase.phrase = QStringLiteral( "files" );
    auto request = fixture.maker.makeDefinitionFor( phrase, 0, {}, {} );
    QTRY_VERIFY_WITH_TIMEOUT( request->isFinished(), 5000 );
    auto const & bytes = request->getFullData();
    auto * article = dynamic_cast< ArticleRequest * >( request.get() );
    QVERIFY( article );
    auto files = article->emittedFileUrls();
    QCOMPARE( files, ArticleMaker::emittedFileUrls( QByteArray( bytes.data(), bytes.size() ) ) );
    QCOMPARE( files, ( QList< QUrl >{ QUrl( "file:///missing/a%20b.png" ), QUrl( "file:///tmp/c.png" ) } ) );
}

void TstArticleSchemes::nativeReplyWaitsAndFinishesOnce()
{
    BrowserFixture fixture;
    sptr< PendingRequest > pending( new PendingRequest );
    fixture.dictionary->resources.insert( QStringLiteral( "native.html" ), pending );
    QScopedPointer< QNetworkReply > reply( fixture.resources.get( QNetworkRequest( QUrl( "bres://fixture/native.html" ) ) ) );
    QSignalSpy finished( reply.get(), &QNetworkReply::finished );
    char buffer[8] = {};
    QCOMPARE( reply->read( buffer, 1 ), qint64( 0 ) );
    QCOMPARE( reply->read( buffer, 0 ), qint64( 0 ) );
    QVERIFY( !reply->isFinished() );
    pending->append( "ab" );
    QTRY_VERIFY( reply->bytesAvailable() >= 2 );
    QCOMPARE( reply->read( 2 ), QByteArray( "ab" ) );
    QCOMPARE( reply->read( buffer, 1 ), qint64( 0 ) );
    pending->complete( "abcd" );
    QTRY_COMPARE( finished.size(), 1 );
    QCOMPARE( reply->readAll(), QByteArray( "cd" ) );
    QCOMPARE( reply->read( buffer, 1 ), qint64( -1 ) );
    pending->repeatNotifications();
    QTest::qWait( 50 );
    QCOMPARE( finished.size(), 1 );
    pending.reset();

    sptr< PendingRequest > aborting( new PendingRequest );
    fixture.dictionary->resources.insert( QStringLiteral( "abort.html" ), aborting );
    QScopedPointer< QNetworkReply > aborted( fixture.resources.get( QNetworkRequest( QUrl( "bres://fixture/abort.html" ) ) ) );
    QSignalSpy abortedFinished( aborted.get(), &QNetworkReply::finished );
    aborted->abort();
    aborted->abort();
    QCOMPARE( aborting->cancellations, 1 );
    QCOMPARE( aborted->error(), QNetworkReply::OperationCanceledError );
    QVERIFY( aborted->isFinished() );
    QCOMPARE( abortedFinished.size(), 1 );

    NativeReplyProbe empty( nullptr, QNetworkRequest( QUrl( "bres://fixture/empty.html" ) ), instant( {} ), "text/html" );
    QCOMPARE( empty.readData( buffer, 0 ), qint64( 0 ) );
    QCOMPARE( empty.readData( buffer, 1 ), qint64( -1 ) );
    sptr< PendingRequest > prefilled( new PendingRequest );
    prefilled->append( "xy" );
    NativeReplyProbe available( nullptr, QNetworkRequest( QUrl( "bres://fixture/prefilled.html" ) ), prefilled, "text/html" );
    QSignalSpy ready( &available, &QNetworkReply::readyRead );
    QTRY_COMPARE( ready.size(), 1 );
    QCOMPARE( available.readData( buffer, 2 ), qint64( 2 ) );
    QCOMPARE( QByteArray( buffer, 2 ), QByteArray( "xy" ) );
}

void TstArticleSchemes::explicitHttpPreservesPathAndUserAgent()
{
    BrowserFixture fixture;
    fixture.hideHeader = true;
    QTcpServer server;
    QVERIFY( server.listen( QHostAddress::LocalHost ) );
    QByteArray received;
    connect( &server, &QTcpServer::newConnection, &server, [&] {
        QTcpSocket * socket = server.nextPendingConnection();
        connect( socket, &QTcpSocket::readyRead, socket, [&, socket] {
            received += socket->readAll();
            if( received.contains( "\r\n\r\n" ) )
            {
                socket->write( "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok" );
                socket->disconnectFromHost();
            }
        } );
    } );
    QString const applicationName = QCoreApplication::applicationName();
    QCoreApplication::setApplicationName( QStringLiteral( "GoldenDict" ) );
    QNetworkRequest request( QUrl( QStringLiteral( "http://127.0.0.1:%1/w/image.php" ).arg( server.serverPort() ) ) );
    request.setRawHeader( "User-Agent", "prefix GoldenDict/1 suffix" );
    QScopedPointer< QNetworkReply > reply( fixture.resources.get( request ) );
    QSignalSpy finished( reply.get(), &QNetworkReply::finished );
    QTRY_COMPARE_WITH_TIMEOUT( finished.size(), 1, 5000 );
    QCoreApplication::setApplicationName( applicationName );
    QVERIFY( received.startsWith( "GET /w/image.php HTTP/1.1\r\n" ) );
    QVERIFY( received.contains( "User-Agent: prefix /1 suffix\r\n" ) );
    QCOMPARE( reply->readAll(), QByteArray( "ok" ) );

    // Preserve offline Wikimedia URL upgrades while keeping every connection
    // inside the loopback fixture. No request reaches the public service.
    QTcpServer proxy;
    QVERIFY( proxy.listen( QHostAddress::LocalHost, 0 ) );
    fixture.resources.setProxy( QNetworkProxy( QNetworkProxy::HttpProxy, QStringLiteral( "127.0.0.1" ), proxy.serverPort() ) );
    for( QString const & scheme : { QStringLiteral( "http" ), QStringLiteral( "gdlookup" ) } )
    {
        QUrl source( scheme + QStringLiteral( "://upload.wikimedia.org/w/image.png?q=a%2Bb#part" ) );
        QScopedPointer< QNetworkReply > upgraded( fixture.resources.get( QNetworkRequest( source ) ) );
        source.setScheme( QStringLiteral( "https" ) );
        QCOMPARE( upgraded->url(), source );
        upgraded->abort();
    }
}

int main( int argc, char * argv[] )
{
    if( TestSupport::isWatchdogProbeProcess() )
        TestSupport::runWatchdogProbe();

    // The ordering boundary: everything below runs before the application
    // object exists, which is where scheme registration has to happen. The pure
    // URL contract is exercised here to prove it has no application dependency.
    preApplication.ran = true;
    preApplication.applicationAbsent = QCoreApplication::instance() == nullptr;
    const ArticleUrls::Parsed preApplicationParsed =
      ArticleUrls::parse( QUrl( QStringLiteral( "bword:cat" ) ), ArticleUrls::Use::UserAction );
    preApplication.kind = preApplicationParsed.kind;
    preApplication.headword = preApplicationParsed.lookup.phrase.phrase;

    auto const previousHandler = qInstallMessageHandler( []( QtMsgType type, QMessageLogContext const &, QString const & text ) {
        if( type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg )
            preApplication.registrationWarnings.append( text );
    } );
    if( qEnvironmentVariableIsSet( "GD_ARTICLE_METHOD_PROBE" ) )
    {
        QWebEngineUrlScheme probe( "bres" );
        probe.setSyntax( QWebEngineUrlScheme::Syntax::Host );
        probe.setFlags( QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalScheme
                        | QWebEngineUrlScheme::LocalAccessAllowed | QWebEngineUrlScheme::CorsEnabled
                        | QWebEngineUrlScheme::FetchApiAllowed );
        QWebEngineUrlScheme::registerScheme( probe );
    }
    registerArticleSchemes();
    registerArticleSchemes();
    for( QByteArray const name : { "gdlookup", "gdpicture", "bres", "gdau", "gdvideo", "gico", "qrcx" } )
        preApplication.schemes.append( QWebEngineUrlScheme::schemeByName( name ) );
    qInstallMessageHandler( previousHandler );
    QApplication app( argc, argv );
    TstArticleSchemes testCase;
    return QTest::qExec( &testCase, argc, argv );
}

#include "tst_article_schemes.moc"
