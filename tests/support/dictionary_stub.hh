/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Test-only dictionary fixtures: a deterministic Dictionary::Class, a
 * producer a test can drive step by step, and the reader factory that lets a
 * real article handler/device read it.
 *
 * Nothing here is part of the application. The header is consumed read-only by
 * every suite that needs a dictionary double; the *production* reader stays the
 * real `makeDataRequestReader()` from articleschemes.cc, so the seam a suite
 * exercises is the same object the application installs.
 *
 * Build note: dictionary_stub.cc calls into the production reader, so a suite
 * that links it must also link the article reader closure:
 *
 *     SOURCES += $$GD_TESTS_ROOT/support/dictionary_stub.cc \
 *                $$GD_REPO_ROOT/articleschemes.cc \
 *                $$GD_REPO_ROOT/dictionary.cc \
 *                $$GD_REPO_ROOT/mutex.cc
 *
 * `tests/article-test.pri` does that for the Browser/Article targets;
 * `tests/tst_utf8_wstring/tst_utf8_wstring.pro` does it for the self-tests.
 * The suite file, not this translation unit, decides: tests/testsupport.pri
 * deliberately keeps this one file out of its automatic support glob so a pure
 * suite never links Chromium.
 */

#ifndef GOLDENDICT_TESTS_SUPPORT_DICTIONARY_STUB_HH_INCLUDED
#define GOLDENDICT_TESTS_SUPPORT_DICTIONARY_STUB_HH_INCLUDED

#include <QByteArray>
#include <QByteArrayView>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "articleschemes.hh"
#include "dictionary.hh"
#include "sptr.hh"

namespace TestSupport {

/// The three production reader operations a test can arm and observe.
enum class ReadOperation
{
    FullData,
    Slice,
    StreamCopy
};

/// One operation's observations since its most recent setter (or since the
/// request was created). `callingThreads` records the identity of every caller
/// with QThread::currentThreadId(), so a test can prove which thread the real
/// boundary ran the operation on.
struct ReadProbeSnapshot
{
    int entered = 0;
    int thrown = 0;
    int returned = 0;
    QSet< Qt::HANDLE > callingThreads;

    bool untouched() const
    {
        return entered == 0 && thrown == 0 && returned == 0 && callingThreads.isEmpty();
    }
};

/// The mutex-protected plain state a ControlledDataRequest shares with the
/// reader that reads it. It holds no producer pointer, no sptr and no QObject,
/// so it stays valid (and usable) after the producer has been destroyed.
struct ReadProbeState
{
    struct Operation
    {
        int entered = 0;
        int thrown = 0;
        int returned = 0;
        QSet< Qt::HANDLE > callingThreads;
        /// Non-empty while the next matching operation must throw this message.
        QString armedMessage;
    };

    static constexpr int OperationCount = 3;

    std::mutex mutex;
    Operation operations[ OperationCount ];

    Operation & operation( ReadOperation which )
    {
        return operations[ static_cast< int >( which ) ];
    }

    Operation const & operation( ReadOperation which ) const
    {
        return operations[ static_cast< int >( which ) ];
    }
};

/// A Dictionary::DataRequest a test drives explicitly.
///
/// The producer side (`append`, `finishSuccess`, `finishNotFound`,
/// `finishError`, `cancel`) follows the same dataMutex/hasAnyData discipline as
/// a real backend: `dataSize()` stays -1 until the first byte or a successful
/// (possibly empty) completion, and every terminal transition emits the
/// production `finished()` signal exactly once. The base class's nonvirtual
/// getters are used unchanged; this class neither hides nor overrides them.
///
/// The probe side (`setFullDataException` and friends, `readProbe`) is consumed
/// only by ThrowingDataRequestReader: a setter arms one operation once, resets
/// that operation's probe, and touches nothing else (no producer byte, no
/// getter call).
class ControlledDataRequest final : public Dictionary::DataRequest
{
public:
    enum class Completion
    {
        Pending,
        Success,
        NotFound,
        Error
    };

    /// A pending request: dataSize() == -1 until the test produces data.
    ControlledDataRequest();

    /// Instant success with `bytes`: the request is already finished when the
    /// constructor returns, exactly like Dictionary::DataRequestInstant.
    explicit ControlledDataRequest( QByteArray const & bytes );

    ~ControlledDataRequest() override;

    /// Producer operations. Every one of them is a no-op after a terminal
    /// transition, so a test cannot resurrect a finished request.
    void append( QByteArray const & bytes );
    void finishSuccess();
    void finishNotFound();
    void finishError( QString const & message );
    void cancel() override;

    /// Arms the next matching reader operation to throw `message` (UTF-8 in the
    /// std::runtime_error) and resets that operation's probe.
    void setFullDataException( QString const & message );
    void setSliceException( QString const & message );
    void setStreamCopyException( QString const & message );

    /// Observations of one reader operation since its most recent setter.
    ReadProbeSnapshot readProbe( ReadOperation operation ) const;

    /// The state the reader shares with this request. The factory reads it once,
    /// on the GUI thread, when it builds a reader; it never retains this request.
    std::shared_ptr< ReadProbeState > probeState() const;

    /// Weak token owned by this request: a test destroys the request, keeps the
    /// reader alive and asserts that the token expired, which proves the reader
    /// does not retain its producer.
    std::weak_ptr< void const > lifetimeToken() const;

    /// Producer observations.
    QByteArray producedBytes();
    int terminalEmissions() const;
    int cancelCount() const;
    Completion completion() const;
    QString errorMessage() const;
    bool isTerminal();

private:
    void terminate( Completion completion, QString const & message );

    std::shared_ptr< ReadProbeState > probe;
    std::shared_ptr< void const > lifetime;
    mutable std::mutex producerMutex;
    Completion state = Completion::Pending;
    QString error;
    int terminals = 0;
    int cancels = 0;
};

/// The reader the fixture factory returns for a ControlledDataRequest.
///
/// It retains exactly two things: the shared, mutex-protected probe state and
/// the production reader it delegates to. It never retains the producer, so it
/// stays usable after the request is gone (which is what the streaming device's
/// producer-free copy path relies on).
class ThrowingDataRequestReader final : public DataRequestReader
{
public:
    ThrowingDataRequestReader( std::shared_ptr< ReadProbeState > state,
                               std::shared_ptr< DataRequestReader const > delegate );

    QByteArray fullData( Dictionary::DataRequest & request ) const override;
    void dataSlice( Dictionary::DataRequest & request, size_t offset, size_t size,
                    void * destination ) const override;
    void copyStreamBytes( char * destination, char const * source, qint64 count ) const override;

    std::shared_ptr< ReadProbeState > probeState() const;

private:
    /// Records entry (and the calling thread) for `operation`; returns the armed
    /// message when this call must throw.
    std::optional< QString > enter( ReadOperation operation ) const;
    void recordThrow( ReadOperation operation ) const;
    void recordReturn( ReadOperation operation ) const;

    std::shared_ptr< ReadProbeState > state;
    std::shared_ptr< DataRequestReader const > delegate;
};

/// The factory a suite installs instead of the default one. It returns a
/// ThrowingDataRequestReader for the fixture's ControlledDataRequest producers
/// and the untouched production reader for every other request.
DataRequestReaderFactory controlledReaderFactory();

/// A deterministic Dictionary::Class: fixed id, no files, no real dictionary.
/// Articles and resources are produced from fixed in-memory bytes (or left
/// pending for the test to drive), and every lookup argument is recorded.
class FixtureDictionary final : public Dictionary::Class
{
public:
    struct Lookup
    {
        gd::wstring word;
        std::vector< gd::wstring > alternates;
        gd::wstring context;
        bool ignoreDiacritics = false;
    };

    explicit FixtureDictionary( std::string const & id = "dict-a",
                                QString const & displayName = QStringLiteral( "Fixture Dictionary A" ) );
    ~FixtureDictionary() override;

    /// Fixed article body this dictionary returns. The default is the fixture
    /// article file content when a suite sets one, otherwise a short marker.
    void setArticleBody( QByteArray const & body );
    void setResources( QMap< QString, QByteArray > const & resources );

    /// When true, getArticle()/getResource() return a pending request the test
    /// drives through lastArticleRequest()/lastResourceRequest() instead of an
    /// instantly completed one.
    void setArticlePending( bool pending );
    void setResourcePending( bool pending );

    std::string getName() noexcept override;
    std::map< Dictionary::Property, std::string > getProperties() noexcept override;
    unsigned long getArticleCount() noexcept override;
    unsigned long getWordCount() noexcept override;
    sptr< Dictionary::WordSearchRequest > prefixMatch( gd::wstring const & word,
                                                       unsigned long maxResults ) override;
    sptr< Dictionary::DataRequest > getArticle( gd::wstring const & word,
                                               std::vector< gd::wstring > const & alternates,
                                               gd::wstring const & context,
                                               bool ignoreDiacritics ) override;
    sptr< Dictionary::DataRequest > getResource( std::string const & name ) override;
    sptr< Dictionary::DataRequest > getSearchResults( QString const & searchString,
                                                      int searchMode,
                                                      bool matchCase,
                                                      int distanceBetweenWords,
                                                      int maxArticlesPerDictionary,
                                                      bool ignoreWordsOrder,
                                                      bool ignoreDiacritics,
                                                      QThreadPool * ftsThreadPoolPtr ) override;

    /// Observations.
    int articleRequestCount() const;
    Lookup lastLookup() const;
    QStringList requestedResources() const;
    int searchRequestCount() const;
    sptr< ControlledDataRequest > lastArticleRequest() const;
    sptr< ControlledDataRequest > lastResourceRequest() const;
    void clearObservations();

private:
    std::string name;
    QByteArray articleBody;
    QMap< QString, QByteArray > resources;
    bool articlePending = false;
    bool resourcePending = false;

    mutable std::mutex observationMutex;
    int articles = 0;
    int searches = 0;
    Lookup lookup;
    QStringList resourceNames;
    sptr< ControlledDataRequest > articleRequest;
    sptr< ControlledDataRequest > resourceRequest;
};

} // namespace TestSupport

#endif // GOLDENDICT_TESTS_SUPPORT_DICTIONARY_STUB_HH_INCLUDED
